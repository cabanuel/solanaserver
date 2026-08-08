/*
  badge.ble - the Nordic UART link, from an app's point of view.

  Calling ble.listen() claims the link: incoming lines start arriving as
  on_ble(line) instead of being interpreted as app-push commands. The runtime
  releases the claim when the app stops, which is what puts the badge back in a
  state where a phone can push a new app to it.

  Traffic is line-oriented text. Binary belongs in base64.
*/
#include <Arduino.h>

#include "../net/ble_mgr.h"
#include "../net/push_protocol.h"
#include "../settings.h"
#include "lua_bindings.h"
#include "lua_runtime.h"

extern "C" {
#include "../lua/lauxlib.h"
#include "../lua/lua.h"
}

namespace bindings {
namespace {

int l_enable(lua_State *L) {
  const bool on = lua_isnoneornil(L, 1) ? true : lua_toboolean(L, 1);
  if (on) {
    // settings::deviceName() returns a String by value; hold it in a named
    // local so its buffer outlives the luaL_optstring default pointer. Passing
    // .c_str() of the temporary directly leaves `name` dangling.
    const String fallback = settings::deviceName();
    const char *name = luaL_optstring(L, 2, fallback.c_str());
    lua_pushboolean(L, ble_mgr::begin(String(name)));
  } else {
    ble_mgr::end();
    lua_pushboolean(L, true);
  }
  return 1;
}

int l_enabled(lua_State *L) {
  lua_pushboolean(L, ble_mgr::enabled());
  return 1;
}

int l_connected(lua_State *L) {
  lua_pushboolean(L, ble_mgr::connected());
  return 1;
}

int l_send(lua_State *L) {
  size_t length = 0;
  const char *text = luaL_checklstring(L, 1, &length);
  lua_pushboolean(L, ble_mgr::sendLine(String(text)));
  return 1;
}

// ble.listen(true) routes incoming lines to on_ble(); ble.listen(false) hands
// them back to the app-push protocol.
int l_listen(lua_State *L) {
  const bool on = lua_isnoneornil(L, 1) ? true : lua_toboolean(L, 1);
  if (on) {
    ble_mgr::onLine([](const String &line) { runtime::dispatchBle(line); });
  } else {
    ble_mgr::clearLineHandler();
    push_protocol::reset();
  }
  lua_pushboolean(L, on);
  return 1;
}

int l_listening(lua_State *L) {
  lua_pushboolean(L, ble_mgr::hasLineHandler());
  return 1;
}

int l_address(lua_State *L) {
  const String address = ble_mgr::address();
  if (address.length() == 0) {
    lua_pushnil(L);
  } else {
    lua_pushstring(L, address.c_str());
  }
  return 1;
}

const luaL_Reg FUNCTIONS[] = {
    {"enable", l_enable},   {"enabled", l_enabled},     {"connected", l_connected},
    {"send", l_send},       {"listen", l_listen},       {"listening", l_listening},
    {"address", l_address}, {nullptr, nullptr},
};

}  // namespace

void openBle(lua_State *L) { setTable(L, "ble", FUNCTIONS, nullptr); }

}  // namespace bindings
