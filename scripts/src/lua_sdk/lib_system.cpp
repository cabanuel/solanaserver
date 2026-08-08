/*
  badge.system - device info, timing, and app lifecycle.

  Also installs the trimmed `os` table. The stock Lua os library is not opened
  (see src/lua/linit.c) because it carries os.execute, os.remove, os.rename and
  os.exit, none of which mean anything sane here. os.time, os.clock and os.date
  do, and enough library code assumes they exist that leaving them out is worse
  than curating them.
*/
#include <Arduino.h>
#include <esp_random.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

#include "../apps/app_store.h"
#include "../hal/display.h"
#include "../settings.h"
#include "lua_bindings.h"
#include "lua_runtime.h"

extern "C" {
#include "../lua/lauxlib.h"
#include "../lua/lua.h"
}

namespace bindings {
namespace {

int l_millis(lua_State *L) {
  lua_pushinteger(L, (lua_Integer)millis());
  return 1;
}

int l_uptime(lua_State *L) {
  lua_pushnumber(L, millis() / 1000.0);
  return 1;
}

// Yields the CPU for `ms`. The runtime's deadline is pushed out by the same
// amount, so a deliberate wait is not mistaken for a hung app.
int l_sleep(lua_State *L) {
  const uint32_t ms = (uint32_t)luaL_checkinteger(L, 1);
  // Capped: a sleep longer than this starves the radios and the button poll,
  // and an app that wants a long delay should be counting frames in on_update.
  const uint32_t clamped = ms > 2000 ? 2000 : ms;
  runtime::extendDeadline(clamped + 50);
  delay(clamped);
  return 0;
}

int l_heap(lua_State *L) {
  lua_pushinteger(L, (lua_Integer)ESP.getFreeHeap());
  return 1;
}

int l_psram(lua_State *L) {
  lua_pushinteger(L, (lua_Integer)ESP.getFreePsram());
  return 1;
}

// How much of the app's own Lua heap cap is in use.
int l_lua_memory(lua_State *L) {
  lua_pushinteger(L, (lua_Integer)runtime::memoryUsed());
  lua_pushinteger(L, (lua_Integer)runtime::memoryLimit());
  return 2;
}

int l_chip(lua_State *L) {
  lua_createtable(L, 0, 5);
  lua_pushstring(L, ESP.getChipModel());
  lua_setfield(L, -2, "model");
  lua_pushinteger(L, ESP.getChipRevision());
  lua_setfield(L, -2, "revision");
  lua_pushinteger(L, ESP.getCpuFreqMHz());
  lua_setfield(L, -2, "mhz");
  lua_pushinteger(L, (lua_Integer)ESP.getFlashChipSize());
  lua_setfield(L, -2, "flash_bytes");
  lua_pushinteger(L, (lua_Integer)ESP.getPsramSize());
  lua_setfield(L, -2, "psram_bytes");
  return 1;
}

// Longest device name we accept. 23 fits the ESP-NOW beacon name buffer
// (char[24]) without truncation and stays within the BLE advertised name,
// SoftAP SSID (32) and mDNS hostname (63) limits. Local constant on purpose -
// config.h is owned by another agent.
static const size_t DEVICE_NAME_MAX = 23;

int l_name(lua_State *L) {
  if (lua_gettop(L) >= 1) {
    size_t len = 0;
    const char *name = luaL_checklstring(L, 1, &len);
    // Reject empty/oversized names, and anything that is not printable ASCII:
    // the name flows into BLE, mDNS, SoftAP and ESP-NOW, none of which want
    // control characters or arbitrary bytes. (No C++ String is live at these
    // raises, so the luaL_error longjmp is leak-free.)
    if (len == 0) return luaL_error(L, "system.name: name must not be empty");
    if (len > DEVICE_NAME_MAX)
      return luaL_error(L, "system.name: name too long (max %d chars)", (int)DEVICE_NAME_MAX);
    for (size_t i = 0; i < len; ++i) {
      const unsigned char ch = (unsigned char)name[i];
      if (ch < 0x20 || ch > 0x7E)
        return luaL_error(L, "system.name: only printable ASCII characters are allowed");
    }
    settings::setDeviceName(String(name));
  }
  lua_pushstring(L, settings::deviceName().c_str());
  return 1;
}

int l_reboot(lua_State *L) {
  (void)L;
  delay(50);
  ESP.restart();
  return 0;
}

// Ends the app and returns to the launcher. Deferred, so it is safe to call
// from anywhere in an app including deep inside on_draw().
int l_exit(lua_State *L) {
  (void)L;
  runtime::requestStop();
  return 0;
}

// Hands control to another installed app. Also deferred; the current app runs
// to the end of its callback first.
int l_launch(lua_State *L) {
  const char *id = luaL_checkstring(L, 1);
  if (!app_store::exists(String(id))) return luaL_error(L, "no such app: %s", id);
  runtime::requestLaunch(String(id));
  return 0;
}

// system.apps() -> array of { id, name, version, author, description, bytes }
int l_apps(lua_State *L) {
  lua_createtable(L, (int)app_store::count(), 0);
  app_store::Info info;
  for (size_t i = 0; i < app_store::count(); ++i) {
    if (!app_store::at(i, info)) continue;
    lua_createtable(L, 0, 6);
    lua_pushstring(L, info.id.c_str());
    lua_setfield(L, -2, "id");
    lua_pushstring(L, info.name.c_str());
    lua_setfield(L, -2, "name");
    lua_pushstring(L, info.version.c_str());
    lua_setfield(L, -2, "version");
    lua_pushstring(L, info.author.c_str());
    lua_setfield(L, -2, "author");
    lua_pushstring(L, info.description.c_str());
    lua_setfield(L, -2, "description");
    lua_pushinteger(L, (lua_Integer)info.sizeBytes);
    lua_setfield(L, -2, "bytes");
    lua_rawseti(L, -2, (lua_Integer)i + 1);
  }
  return 1;
}

int l_current_app(lua_State *L) {
  lua_pushstring(L, runtime::currentApp().c_str());
  return 1;
}

// system.random_bytes(n) -> string of n bytes from the ESP32-S3's hardware RNG.
//
// esp_random() is a genuine hardware TRNG, not a PRNG: it is fed by the SAR ADC
// and, once the RF subsystem is running, by the radios' own noise floor. Espressif
// only calls its output true random when Wi-Fi or BLE is enabled, so entropy is
// weakest on a badge with both radios off - which is still a hardware source,
// just a thinner one.
//
// This is deliberately NOT badge.se050.random(). Both are hardware, but only one
// of them is a certified secure element, and an app that wants that guarantee has
// to be able to ask for it by name and be told no. Folding a fallback in here
// would take that choice away from every caller at once.
int l_random_bytes(lua_State *L) {
  const lua_Integer requested = luaL_optinteger(L, 1, 32);
  luaL_argcheck(L, requested >= 1 && requested <= 256, 1, "n must be 1..256");

  uint8_t buffer[256];
  size_t filled = 0;
  while (filled < (size_t)requested) {
    const uint32_t word = esp_random();
    const size_t remaining = (size_t)requested - filled;
    const size_t take = remaining < 4 ? remaining : 4;
    memcpy(buffer + filled, &word, take);
    filled += take;
  }

  lua_pushlstring(L, (const char *)buffer, (size_t)requested);
  return 1;
}

const luaL_Reg FUNCTIONS[] = {
    {"millis", l_millis},   {"uptime", l_uptime},   {"sleep", l_sleep},
    {"heap", l_heap},       {"psram", l_psram},     {"lua_memory", l_lua_memory},
    {"chip", l_chip},       {"name", l_name},       {"reboot", l_reboot},
    {"exit", l_exit},       {"launch", l_launch},   {"apps", l_apps},
    {"current_app", l_current_app},
    {"random_bytes", l_random_bytes},
    {nullptr, nullptr},
};

// -- The curated `os` -------------------------------------------------------

int os_time(lua_State *L) {
  // No RTC and usually no NTP, so this is seconds since boot unless something
  // has set the system clock. Documented as such.
  lua_pushinteger(L, (lua_Integer)time(nullptr));
  return 1;
}

int os_clock(lua_State *L) {
  lua_pushnumber(L, (lua_Number)millis() / 1000.0);
  return 1;
}

// Conversion specifiers os.date will pass to strftime, mirroring stock Lua's
// LUA_STRFTIMEOPTIONS. '|' separates blocks by specifier length (1, then 2).
// Anything not listed is rejected rather than handed to the C library, where an
// unknown or malformed specifier is undefined behaviour.
#define BADGE_STRFTIMEOPTIONS \
  "aAbBcdHIjmMpSUwWxXyYzZ%||EcECExEXEyEY OdOeOHOIOmOMOSOuOUOVOwOWOy"
#define BADGE_SIZETIMEFMT 250

// Validates one specifier at `conv` and copies it (including any 'E'/'O'
// modifier) into `buff`, which must hold at least 3 chars plus a terminator.
// Returns the position just past the consumed specifier. Never returns on an
// invalid specifier (luaL_argerror longjmps).
const char *checkStrftimeOption(lua_State *L, const char *conv, ptrdiff_t convlen,
                                char *buff) {
  const char *option = BADGE_STRFTIMEOPTIONS;
  int oplen = 1;  // length of the specifiers currently being checked
  for (; *option != '\0' && oplen <= convlen; option += oplen) {
    if (*option == '|') {
      oplen++;  // move on to the longer specifiers
    } else if (memcmp(conv, option, (size_t)oplen) == 0) {
      memcpy(buff, conv, (size_t)oplen);
      buff[oplen] = '\0';
      return conv + oplen;
    }
  }
  luaL_argerror(L, 1,
                lua_pushfstring(L, "invalid conversion specifier '%%%s'", conv));
  return conv;  // unreachable; silences a warning
}

int os_date(lua_State *L) {
  size_t slen = 0;
  const char *format = luaL_optlstring(L, 1, "%c", &slen);
  time_t when = lua_isnoneornil(L, 2) ? time(nullptr) : (time_t)luaL_checkinteger(L, 2);
  const char *formatEnd = format + slen;

  const bool utc = (format[0] == '!');
  if (utc) ++format;

  struct tm parts;
  if (utc) {
    gmtime_r(&when, &parts);
  } else {
    localtime_r(&when, &parts);
  }

  if (strcmp(format, "*t") == 0) {
    lua_createtable(L, 0, 8);
    const struct {
      const char *key;
      int value;
    } fields[] = {{"year", parts.tm_year + 1900}, {"month", parts.tm_mon + 1},
                  {"day", parts.tm_mday},         {"hour", parts.tm_hour},
                  {"min", parts.tm_min},          {"sec", parts.tm_sec},
                  {"wday", parts.tm_wday + 1},    {"yday", parts.tm_yday + 1}};
    for (const auto &field : fields) {
      lua_pushinteger(L, field.value);
      lua_setfield(L, -2, field.key);
    }
    lua_pushboolean(L, parts.tm_isdst > 0);
    lua_setfield(L, -2, "isdst");
    return 1;
  }

  // Build the result one specifier at a time. Each is validated and formatted
  // into its own 250-byte scratch, so a long format never truncates to "" the
  // way a single strftime into a fixed 128-byte buffer could.
  char spec[4];
  spec[0] = '%';
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  while (format < formatEnd) {
    if (*format != '%') {
      luaL_addchar(&b, *format++);
    } else {
      char *out = luaL_prepbuffsize(&b, BADGE_SIZETIMEFMT);
      ++format;  // skip '%'
      format = checkStrftimeOption(L, format, formatEnd - format, spec + 1);
      const size_t reslen = strftime(out, BADGE_SIZETIMEFMT, spec, &parts);
      luaL_addsize(&b, reslen);
    }
  }
  luaL_pushresult(&b);
  return 1;
}

int os_difftime(lua_State *L) {
  lua_pushnumber(L, (lua_Number)(luaL_checknumber(L, 1) - luaL_optnumber(L, 2, 0)));
  return 1;
}

const luaL_Reg OS_FUNCTIONS[] = {
    {"time", os_time},         {"clock", os_clock}, {"date", os_date},
    {"difftime", os_difftime}, {nullptr, nullptr},
};

}  // namespace

void openSystem(lua_State *L) {
  setTable(L, "system", FUNCTIONS, nullptr);

  // `os` is a global, not a field of `badge` - code that expects it expects it
  // by that name.
  lua_newtable(L);
  luaL_setfuncs(L, OS_FUNCTIONS, 0);
  lua_setglobal(L, "os");
}

}  // namespace bindings
