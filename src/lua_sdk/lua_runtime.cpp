#include "lua_runtime.h"

#include <esp_heap_caps.h>

#include "../apps/app_store.h"
#include "../badge_log.h"
#include "../config.h"
#include "../hal/buttons.h"
#include "../hal/display.h"
#include "../hal/leds.h"
#include "../hal/mic.h"
#include "../net/ble_mgr.h"
#include "../net/espnow_mgr.h"
#include "lua_bindings.h"

extern "C" {
#include "../lua/lauxlib.h"
#include "../lua/lua.h"
#include "../lua/lualib.h"
}

// ---------------------------------------------------------------------------
// Lua's own output hooks. luaconf.h routes print() and panic text here, which
// is why these live in this file rather than in badge_log.
// ---------------------------------------------------------------------------
extern "C" void badge_lua_write(const char *text, size_t len) {
  badge_log::write(text, len);
}

extern "C" void badge_lua_error_printf(const char *format, const char *argument) {
  badge_log::printf(format, argument);
}

namespace runtime {
namespace {

lua_State *sState = nullptr;
String sCurrentApp;
String sLastApp;  // survives stop(), for the error screen's retry
String sLastError;

size_t sAllocated = 0;
uint32_t sDeadline = 0;
bool sDeadlineArmed = false;
// Extension granted to the callback currently running, against the cap in
// config.h. Reset by armDeadline(), so the allowance is per callback.
uint32_t sExtensionGranted = 0;

// Deferred lifecycle requests.
bool sLaunchPending = false;
String sPendingApp;
bool sStopPending = false;
// True only while on_stop() runs inside stop(). Launch/stop requests made
// during teardown are ignored so an app cannot relaunch itself (or anything
// else) from on_stop and strand the user out of the launcher.
bool sTeardownActive = false;

uint32_t sLastFrameAt = 0;

// -- Allocator ---------------------------------------------------------------
// Lua's heap comes from PSRAM: the 512 KB of internal SRAM is wanted by the
// Wi-Fi and BLE stacks, and a Lua app that eats it starves the radios. The byte
// cap is what turns "the badge ran out of memory" into "this app ran out of
// memory", which is a far better failure.
void *luaAllocator(void *ud, void *ptr, size_t osize, size_t nsize) {
  (void)ud;
  if (nsize == 0) {
    if (ptr) {
      sAllocated -= osize;
      heap_caps_free(ptr);
    }
    return nullptr;
  }

  const size_t currentlyUsed = ptr ? sAllocated - osize : sAllocated;
  if (currentlyUsed + nsize > LUA_HEAP_LIMIT_BYTES) return nullptr;

  void *next = heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (next == nullptr) {
    // No PSRAM left (or no PSRAM at all): fall back to internal RAM rather than
    // failing outright, so a badge with a dead PSRAM chip still runs small apps.
    next = heap_caps_realloc(ptr, nsize, MALLOC_CAP_8BIT);
    if (next == nullptr) return nullptr;
  }
  sAllocated = currentlyUsed + nsize;
  return next;
}

// -- Watchdog ----------------------------------------------------------------
void deadlineHook(lua_State *L, lua_Debug *ar) {
  (void)ar;
  if (!sDeadlineArmed) return;
  if ((int32_t)(millis() - sDeadline) < 0) return;
  // Latch: stay armed with the deadline in the past. If we disarmed here, a
  // pcall() or coroutine.resume() that catches this error would leave the hook
  // permanently silent, so a loop *after* a caught timeout would run unbounded.
  // Keeping the latch set means every instruction past the deadline re-raises
  // until callGlobal() disarms on return or armDeadline() grants a fresh budget.
  luaL_error(L, "app exceeded its time budget (stuck in a loop?)");
}

// Prepends a traceback to the error message so a failure names the line it
// happened on rather than just the message.
int messageHandler(lua_State *L) {
  const char *message = lua_tostring(L, 1);
  if (message == nullptr) {
    // A non-string error: use __tostring if it has one.
    if (luaL_callmeta(L, 1, "__tostring") && lua_type(L, -1) == LUA_TSTRING) return 1;
    message = lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, 1));
  }
  luaL_traceback(L, L, message, 1);
  return 1;
}

void reportError(const char *context) {
  const char *message = lua_tostring(sState, -1);
  sLastError = String(context) + ": " + (message ? message : "unknown error");
  lua_pop(sState, 1);
  badge_log::tagf("lua", "%s", sLastError.c_str());
}

// Calls the global `name` with `argCount` values already pushed. Returns false
// if it errored; the app is torn down by the caller.
bool callGlobal(const char *name, int argCount, uint32_t budgetMs) {
  // Callers push arguments only after checking sState, so reaching here with a
  // dead state means nothing was pushed and there is nothing to clean up.
  if (sState == nullptr) return true;

  // Insert the message handler below the arguments so lua_pcall can find it.
  const int handlerIndex = lua_gettop(sState) - argCount + 1;
  lua_pushcfunction(sState, messageHandler);
  lua_insert(sState, handlerIndex);

  if (lua_getglobal(sState, name) != LUA_TFUNCTION) {
    // Not defined - drop the function slot, the arguments and the handler.
    lua_pop(sState, 1 + argCount);
    lua_remove(sState, handlerIndex);
    return true;
  }
  // The function is on top but the arguments are below it; move it under them.
  lua_insert(sState, handlerIndex + 1);

  armDeadline(budgetMs);
  const int result = lua_pcall(sState, argCount, 0, handlerIndex);
  sDeadlineArmed = false;
  lua_remove(sState, handlerIndex);

  if (result != LUA_OK) {
    reportError(name);
    return false;
  }
  return true;
}

// A failed callback stops the app. Anything else means an app that errors every
// frame spams the log and never lets the user back to the launcher.
void failApp() {
  badge_log::tagf("lua", "stopping '%s' after error", sCurrentApp.c_str());
  stop();
}

void setPackagePath(lua_State *L, const String &appId) {
  const String appDir = String(FS_ROOT) + app_store::directory(appId);
  const String path = appDir + "/?.lua;" + appDir + "/?/init.lua;" + FS_ROOT LIB_DIR "/?.lua";

  lua_getglobal(L, "package");
  if (lua_istable(L, -1)) {
    lua_pushstring(L, path.c_str());
    lua_setfield(L, -2, "path");
    // No dynamic C modules exist on the badge; an empty cpath makes require()
    // say "module not found" instead of "cannot load .so".
    lua_pushstring(L, "");
    lua_setfield(L, -2, "cpath");
  }
  lua_pop(L, 1);
}

}  // namespace

// ---------------------------------------------------------------------------

bool begin() {
  badge_log::tagf("lua", "%s, heap cap %u KB from PSRAM", LUA_RELEASE,
                  (unsigned)(LUA_HEAP_LIMIT_BYTES / 1024));
  return true;
}

void armDeadline(uint32_t budgetMs) {
  sDeadline = millis() + budgetMs;
  sDeadlineArmed = true;
  sExtensionGranted = 0;
}

// Bounded on purpose - see LUA_CALLBACK_EXTENSION_CAP_MS. A binding asking for
// more than the callback has left gets what is left and no more, which keeps a
// loop of blocking calls from outrunning its own deadline forever.
void extendDeadline(uint32_t extraMs) {
  if (!sDeadlineArmed) return;
  if (sExtensionGranted >= LUA_CALLBACK_EXTENSION_CAP_MS) return;

  const uint32_t room = LUA_CALLBACK_EXTENSION_CAP_MS - sExtensionGranted;
  const uint32_t granted = extraMs > room ? room : extraMs;
  sExtensionGranted += granted;
  sDeadline += granted;
}

bool launch(const String &appId) {
  if (!app_store::exists(appId)) {
    sLastError = "no such app: " + appId;
    badge_log::tagf("lua", "%s", sLastError.c_str());
    return false;
  }

  stop();
  sLastError = "";
  sAllocated = 0;

  sState = lua_newstate(luaAllocator, nullptr);
  if (sState == nullptr) {
    sLastError = "could not create Lua state (out of memory)";
    badge_log::tagf("lua", "%s", sLastError.c_str());
    return false;
  }

  luaL_openlibs(sState);
  setPackagePath(sState, appId);
  bindings::openBadge(sState);

  lua_sethook(sState, deadlineHook, LUA_MASKCOUNT, LUA_HOOK_INSTRUCTIONS);

  // sCurrentApp must be set before the script runs: bindings that resolve paths
  // relative to the app directory are reachable from its top level.
  sCurrentApp = appId;
  sLastApp = appId;

  const String entry = app_store::entryPath(appId);
  badge_log::tagf("lua", "launching '%s' (%s)", appId.c_str(), entry.c_str());

  lua_pushcfunction(sState, messageHandler);
  const int handlerIndex = lua_gettop(sState);

  // Text-only ("t"): never load precompiled bytecode. Untrusted apps ship as
  // source; a binary chunk would be an arbitrary-memory-access sandbox escape.
  if (luaL_loadfilex(sState, entry.c_str(), "t") != LUA_OK) {
    reportError("load");
    stop();
    return false;
  }

  armDeadline(LUA_START_BUDGET_MS);
  const int result = lua_pcall(sState, 0, 0, handlerIndex);
  sDeadlineArmed = false;
  lua_remove(sState, handlerIndex);

  if (result != LUA_OK) {
    reportError("run");
    stop();
    return false;
  }

  if (!callGlobal("on_start", 0, LUA_START_BUDGET_MS)) {
    failApp();
    return false;
  }

  sLastFrameAt = millis();
  badge_log::tagf("lua", "'%s' started, %u KB Lua heap in use", appId.c_str(),
                  (unsigned)(sAllocated / 1024));
  return true;
}

void stop() {
  if (sState == nullptr) {
    sCurrentApp = "";
    return;
  }

  // on_stop runs before anything is torn down so the app can still touch the
  // badge; a failure here is logged and then ignored, since we are shutting
  // down either way. Any launch/stop request it makes is dropped (see
  // sTeardownActive) so it cannot resurrect itself in this same tick.
  sTeardownActive = true;
  callGlobal("on_stop", 0, LUA_CALLBACK_BUDGET_MS);
  sTeardownActive = false;

  // Radio handlers hold std::function objects that capture the Lua state.
  // Clearing them before lua_close() is what keeps a late ESP-NOW packet from
  // reaching a freed VM.
  espnow_mgr::clearReceiveHandler();
  ble_mgr::clearLineHandler();

  // Peripherals an app may have claimed. This belongs here rather than in the
  // main loop's app-exited branch, because an app that hands over with
  // system.launch() never passes through that branch - and leaving the I2S DMA
  // ring running or the LEDs mid-animation would leak straight into the next
  // app.
  mic::disable();
  leds::stopAnimation();
  leds::off();

  lua_close(sState);
  sState = nullptr;
  sDeadlineArmed = false;

  badge_log::tagf("lua", "stopped '%s'", sCurrentApp.c_str());
  sCurrentApp = "";
  sAllocated = 0;
}

void requestLaunch(const String &appId) {
  // A launch queued from inside on_stop would be honored in the same tick and
  // could relaunch the very app being torn down, stranding the user with no way
  // back to the launcher. Drop requests made during teardown.
  if (sTeardownActive) return;
  sPendingApp = appId;
  sLaunchPending = true;
  sStopPending = false;
}

void requestStop() {
  if (sTeardownActive) return;
  sStopPending = true;
  sLaunchPending = false;
}

bool processRequests() {
  bool acted = false;
  if (sStopPending) {
    sStopPending = false;
    stop();
    acted = true;
  }
  if (sLaunchPending) {
    sLaunchPending = false;
    const String appId = sPendingApp;
    sPendingApp = "";
    launch(appId);
    acted = true;
  }
  return acted;
}

bool running() { return sState != nullptr; }
const String &currentApp() { return sCurrentApp; }
const String &lastApp() { return sLastApp; }
const String &lastError() { return sLastError; }
void clearError() { sLastError = ""; }

bool update() {
  if (sState == nullptr) return true;

  const uint32_t now = millis();
  const float dt = (now - sLastFrameAt) / 1000.0f;
  sLastFrameAt = now;

  lua_pushnumber(sState, dt);
  if (!callGlobal("on_update", 1, LUA_CALLBACK_BUDGET_MS)) {
    failApp();
    return false;
  }

  if (!callGlobal("on_draw", 0, LUA_CALLBACK_BUDGET_MS)) {
    failApp();
    return false;
  }

  return true;
}

void dispatchButton(uint8_t key, bool pressed) {
  if (sState == nullptr) return;
  lua_pushstring(sState, buttons::shortName(key));
  lua_pushboolean(sState, pressed);
  if (!callGlobal("on_button", 2, LUA_CALLBACK_BUDGET_MS)) failApp();
}

void dispatchEspnow(const uint8_t *mac, const uint8_t *data, size_t length, int8_t rssi) {
  if (sState == nullptr) return;
  lua_pushstring(sState, espnow_mgr::macToString(mac).c_str());
  lua_pushlstring(sState, (const char *)data, length);
  lua_pushinteger(sState, rssi);
  if (!callGlobal("on_espnow", 3, LUA_CALLBACK_BUDGET_MS)) failApp();
}

void dispatchBle(const String &line) {
  if (sState == nullptr) return;
  lua_pushlstring(sState, line.c_str(), line.length());
  if (!callGlobal("on_ble", 1, LUA_CALLBACK_BUDGET_MS)) failApp();
}

lua_State *state() { return sState; }
size_t memoryUsed() { return sAllocated; }
size_t memoryLimit() { return LUA_HEAP_LIMIT_BYTES; }

String resolveAppPath(const String &relativePath) {
  if (sCurrentApp.length() == 0) return String();
  if (relativePath.length() == 0 || relativePath.length() > 96) return String();
  if (relativePath.startsWith("/")) return String();
  if (relativePath.indexOf("..") >= 0) return String();
  return app_store::directory(sCurrentApp) + "/" + relativePath;
}

}  // namespace runtime
