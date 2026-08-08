/*
  The `badge` global.

  openBadge() creates the table and hands it to each module opener in turn.
  Every opener is called with the `badge` table on top of the stack and is
  expected to leave it there - build a sub-table, populate it, then
  lua_setfield() it onto `badge`.

  The full API reference lives in firmware/solana-os/README.md; this header is
  only the wiring.
*/
#pragma once

// Pulled in rather than forward-declared: setTable() takes a luaL_Reg array,
// and a forward declaration inside `namespace bindings` would introduce a
// distinct bindings::luaL_Reg that never matches the real one.
extern "C" {
#include "../lua/lauxlib.h"
#include "../lua/lua.h"
}

namespace bindings {

// Builds `badge` and installs it as a global.
void openBadge(lua_State *L);

// -- Module openers, one per lib_*.cpp. `badge` is on top of the stack. -------
void openGfx(lua_State *L);
void openInput(lua_State *L);
void openLed(lua_State *L);
void openSystem(lua_State *L);
void openStorage(lua_State *L);
void openSensors(lua_State *L);
void openNet(lua_State *L);
void openEspnow(lua_State *L);
void openBle(lua_State *L);

// -- Shared helpers ----------------------------------------------------------
// An integer constant exposed on a module table. Terminate arrays with {nullptr, 0}.
struct Field {
  const char *name;
  int value;
};

// Builds a table from `functions` and `constants` and sets it as `badge.<name>`.
// Expects the `badge` table on top of the stack and leaves it there.
void setTable(lua_State *L, const char *name, const luaL_Reg *functions, const Field *constants);

}  // namespace bindings
