/*
  The Lua VM that runs badge apps.

  One app at a time, one lua_State per app. Starting an app builds a fresh
  state, so nothing an app leaves behind can reach the next one; stopping it
  tears the state down and releases every radio handler the app installed.

  Three things keep a bad app from taking the badge with it:

    - a PSRAM allocator with a hard byte cap, so runaway allocation surfaces as
      a normal Lua "not enough memory" error
    - an instruction-count hook checked against a wall-clock deadline, so an
      infinite loop errors out instead of hanging the main loop
    - launch/stop are *requests* applied between frames, never mid-callback, so
      an app cannot destroy the state it is currently executing in

  App entry points are globals in the app's own script:

    on_start()                     once, after the script is loaded
    on_update(dt)                  every frame; dt in seconds
    on_draw()                      every frame, after on_update
    on_button(key, pressed)        key is "up"/"down"/"left"/"right"/"a"/"b"
    on_espnow(mac, data, rssi)
    on_ble(line)
    on_stop()                      before the state is torn down

  All are optional. An app that defines none of them still runs its top level
  once, which is enough for a script that just prints something.
*/
#pragma once

#include <Arduino.h>

struct lua_State;

namespace runtime {

bool begin();

// -- Lifecycle ---------------------------------------------------------------
// Immediate. Only safe from the main loop with no Lua frame on the stack;
// everything else should use the request* variants.
bool launch(const String &appId);
void stop();

// Deferred to the next processRequests(). Safe from inside a Lua callback, an
// HTTP handler or the BLE task.
void requestLaunch(const String &appId);
void requestStop();
// True if this call started or stopped an app. The caller needs that to notice
// a launch that failed: such an app never appears in running(), so a "was it up
// at the top of the tick?" flag alone would miss it and its error would never
// reach the screen.
bool processRequests();

bool running();
const String &currentApp();

// The app that was running most recently. Unlike currentApp() this survives
// stop(), which is what the error screen's "retry" needs.
const String &lastApp();

// Empty unless the last app died. Cleared by clearError() or a new launch.
const String &lastError();
void clearError();

// -- Per-frame ---------------------------------------------------------------
// Runs on_update(dt) then on_draw(). Returns false if the app errored and was
// stopped, which is the shell's cue to show the error screen.
bool update();

// -- Event dispatch ----------------------------------------------------------
void dispatchButton(uint8_t key, bool pressed);
void dispatchEspnow(const uint8_t *mac, const uint8_t *data, size_t length, int8_t rssi);
void dispatchBle(const String &line);

// -- Introspection -----------------------------------------------------------
lua_State *state();
size_t memoryUsed();
size_t memoryLimit();

// -- For bindings ------------------------------------------------------------
// Sets the wall-clock budget for the callback that is about to run. The
// instruction hook errors the app out once it is exceeded.
void armDeadline(uint32_t budgetMs);

// Bindings that block (badge.system.sleep, a blocking HTTP call) push the
// deadline out by the time they intend to spend, so a legitimate wait is not
// mistaken for a runaway loop.
//
// Granted up to LUA_CALLBACK_EXTENSION_CAP_MS in total per callback. Without a
// ceiling a loop around any blocking binding buys time faster than it spends it
// and the deadline never arrives - which turns "the app gets an error" into
// "the badge needs a reset".
void extendDeadline(uint32_t extraMs);

// Absolute path on the filesystem for a path relative to the running app's
// directory, or an empty string if the path escapes it or no app is running.
String resolveAppPath(const String &relativePath);

}  // namespace runtime
