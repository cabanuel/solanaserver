#include "lua_bindings.h"

#include <Arduino.h>

#include "../badge_log.h"
#include "../config.h"
#include "../settings.h"

extern "C" {
#include "../lua/lauxlib.h"
#include "../lua/lua.h"
}

namespace bindings {
namespace {

// badge.log(...) - same argument handling as print(), but every line is tagged
// with the app so the console shows where it came from.
int l_log(lua_State *L) {
  String line;
  const int argc = lua_gettop(L);
  for (int i = 1; i <= argc; ++i) {
    if (i > 1) line += "\t";
    size_t length = 0;
    // luaL_tolstring honours __tostring, so tables with a metamethod print
    // usefully instead of as an address.
    const char *text = luaL_tolstring(L, i, &length);
    line += String(text);
    lua_pop(L, 1);
  }
  badge_log::tagf("app", "%s", line.c_str());
  return 0;
}

const luaL_Reg BADGE_FUNCTIONS[] = {
    {"log", l_log},
    {nullptr, nullptr},
};

}  // namespace

void setTable(lua_State *L, const char *name, const luaL_Reg *functions, const Field *constants) {
  lua_newtable(L);
  if (functions) luaL_setfuncs(L, functions, 0);
  if (constants) {
    for (const Field *field = constants; field->name != nullptr; ++field) {
      lua_pushinteger(L, field->value);
      lua_setfield(L, -2, field->name);
    }
  }
  lua_setfield(L, -2, name);
}

void openBadge(lua_State *L) {
  lua_newtable(L);
  luaL_setfuncs(L, BADGE_FUNCTIONS, 0);

  lua_pushstring(L, SOLANA_OS_VERSION);
  lua_setfield(L, -2, "version");
  lua_pushinteger(L, SOLANA_OS_API_VERSION);
  lua_setfield(L, -2, "api_version");
  lua_pushstring(L, settings::deviceName().c_str());
  lua_setfield(L, -2, "device_name");

  openGfx(L);
  openInput(L);
  openLed(L);
  openSystem(L);
  openStorage(L);
  openSensors(L);
  openNet(L);
  openEspnow(L);
  openBle(L);

  // A copy at the top level as well, so `badge.millis()` and `badge.gfx` sit
  // side by side - the shortcuts people reach for constantly should not need a
  // sub-table.
  lua_getfield(L, -1, "system");
  lua_getfield(L, -1, "millis");
  lua_setfield(L, -3, "millis");
  lua_getfield(L, -1, "sleep");
  lua_setfield(L, -3, "sleep");
  lua_pop(L, 1);

  lua_setglobal(L, "badge");
}

}  // namespace bindings
