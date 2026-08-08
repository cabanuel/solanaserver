/*
  The shell: the launcher, the settings screens, and the error screen an app
  lands on when it dies.

  The shell only draws and handles input when no Lua app is running. Once an app
  starts, the main loop routes frames and buttons to the runtime instead, and
  the shell does nothing until the app exits.

  Navigation is the same everywhere: up/down move, A selects or toggles,
  left/right adjust a value, B goes back. Holding B for APP_ESCAPE_HOLD_MS
  inside a running app force-quits it, which is handled in the main loop rather
  than here so it works even for an app that traps every button.
*/
#pragma once

#include <Arduino.h>

namespace shell {

void begin();

// One tick of the shell. Reads the button edge masks computed this frame and
// redraws when something changed.
void update();

// Called when an app stops so the shell can rebuild the launcher and, if the
// app died with an error, show it.
void onAppStopped();

// Jumps straight to the error screen.
void showError(const String &message);

}  // namespace shell
