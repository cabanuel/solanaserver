/*
  badge.battery, badge.mic, badge.se050 - the on-board sensors.

  The microphones are off until an app asks for them: the I2S peripheral runs a
  DMA ring whether or not anyone reads it, and most apps never touch audio. The
  runtime turns them back off when the app stops.
*/
#include <Arduino.h>

#include "../hal/mic.h"
#include "../hal/power.h"
#include "../hal/se050.h"
#include "../hal/se050_t1.h"
#include "lua_bindings.h"
#include "lua_runtime.h"

extern "C" {
#include "../lua/lauxlib.h"
#include "../lua/lua.h"
}

namespace bindings {
namespace {

// -- battery -----------------------------------------------------------------

int l_volts(lua_State *L) {
  lua_pushnumber(L, power::volts());
  return 1;
}

int l_percent(lua_State *L) {
  lua_pushnumber(L, power::percent());
  return 1;
}

int l_charging(lua_State *L) {
  lua_pushboolean(L, power::charging());
  return 1;
}

const luaL_Reg BATTERY_FUNCTIONS[] = {
    {"volts", l_volts}, {"percent", l_percent}, {"charging", l_charging}, {nullptr, nullptr},
};

// -- mic ---------------------------------------------------------------------

int l_mic_enable(lua_State *L) {
  // mic.enable(false) turns it off; mic.enable() and mic.enable(true) turn it on.
  const bool on = lua_isnoneornil(L, 1) ? true : lua_toboolean(L, 1);
  if (on) {
    lua_pushboolean(L, mic::enable());
  } else {
    mic::disable();
    lua_pushboolean(L, true);
  }
  return 1;
}

int l_mic_enabled(lua_State *L) {
  lua_pushboolean(L, mic::enabled());
  return 1;
}

// mic.level() -> left, right (0..100, smoothed - meant for meters)
int l_mic_level(lua_State *L) {
  lua_pushnumber(L, mic::levelLeft());
  lua_pushnumber(L, mic::levelRight());
  return 2;
}

// mic.db() -> left, right (dBFS, roughly -58..0, unsmoothed)
int l_mic_db(lua_State *L) {
  lua_pushnumber(L, mic::dbLeft());
  lua_pushnumber(L, mic::dbRight());
  return 2;
}

// mic.read(n) -> table of interleaved L/R 16-bit samples. Capped so a big
// request cannot blow the app's Lua heap in one call.
int l_mic_read(lua_State *L) {
  const int requested = (int)luaL_optinteger(L, 1, 256);
  const int count = constrain(requested, 1, 1024);

  // Fixed stack buffer (at most 1024 * 2 = 2 KB), not malloc: lua_createtable
  // and lua_rawseti below can raise a memory error and longjmp, which would
  // skip a free() of any heap buffer and leak it. With the samples on the C
  // stack there is nothing to leak.
  int16_t buffer[1024];
  const size_t got = mic::readSamples(buffer, (size_t)count);
  lua_createtable(L, (int)got, 0);
  for (size_t i = 0; i < got; ++i) {
    lua_pushinteger(L, buffer[i]);
    lua_rawseti(L, -2, (lua_Integer)i + 1);
  }
  return 1;
}

const luaL_Reg MIC_FUNCTIONS[] = {
    {"enable", l_mic_enable}, {"enabled", l_mic_enabled}, {"level", l_mic_level},
    {"db", l_mic_db},         {"read", l_mic_read},       {nullptr, nullptr},
};

// -- se050 -------------------------------------------------------------------

int l_se_present(lua_State *L) {
  lua_pushboolean(L, se050::present());
  return 1;
}

int l_se_atr(lua_State *L) {
  const String hex = se050::atrHex();
  if (hex.length() == 0) {
    lua_pushnil(L);
  } else {
    lua_pushstring(L, hex.c_str());
  }
  return 1;
}

// Re-runs the link test. Takes ~20ms, so it is a deliberate call rather than
// something the OS does every frame.
int l_se_test(lua_State *L) {
  lua_pushboolean(L, se050::test());
  return 1;
}

// se050.random(n) -> string of n bytes, or nil + reason.
//
// The bytes come from the secure element's own RNG or the call fails; there is
// no fallback here and there must not be one, because "did the secure element
// answer?" is precisely the question a caller of this function is asking. An
// app that just wants entropy and does not care where from should use
// badge.system.random_bytes().
//
// Capped at 64 bytes, which is one T=1 exchange. Two exchanges would not fit
// inside a callback's 250 ms budget with any margin, and an app that wants more
// can call twice.
int l_se_random(lua_State *L) {
  const lua_Integer requested = luaL_optinteger(L, 1, 32);
  luaL_argcheck(L, requested >= 1 && requested <= 64, 1, "n must be 1..64");

  // This blocks on I2C for up to a transport budget, so tell the watchdog
  // before rather than being killed as a runaway loop halfway through.
  runtime::extendDeadline(se050_t1::BUDGET_MS + 60);

  uint8_t buffer[64];
  if (!se050::randomBytes(buffer, (size_t)requested)) {
    lua_pushnil(L);
    lua_pushstring(L, se050_t1::lastError());
    return 2;
  }

  lua_pushlstring(L, (const char *)buffer, (size_t)requested);
  return 1;
}

// se050.random_available() -> bool
//
// Deliberately an end-to-end probe: it asks the part for one real byte rather
// than reporting whether the I2C link is up. Those are different questions, and
// the difference is the whole uncertainty in this feature - the link layer is
// verified against known-good frames, the GetRandom encoding above it is not.
// A link check would happily say "yes" on a badge whose applet refuses the
// command, and the app would then show the wrong entropy source.
//
// Costs one full exchange, so probe once and remember the answer.
int l_se_random_available(lua_State *L) {
  runtime::extendDeadline(se050_t1::BUDGET_MS + 60);
  uint8_t probe = 0;
  lua_pushboolean(L, se050::randomBytes(&probe, 1));
  return 1;
}

const luaL_Reg SE050_FUNCTIONS[] = {
    {"present", l_se_present},
    {"atr", l_se_atr},
    {"test", l_se_test},
    {"random", l_se_random},
    {"random_available", l_se_random_available},
    {nullptr, nullptr},
};

}  // namespace

void openSensors(lua_State *L) {
  setTable(L, "battery", BATTERY_FUNCTIONS, nullptr);
  setTable(L, "mic", MIC_FUNCTIONS, nullptr);
  setTable(L, "se050", SE050_FUNCTIONS, nullptr);
}

}  // namespace bindings
