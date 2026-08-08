/*
  badge.input - the six buttons.

  Keys are named, not numbered: "up", "down", "left", "right", "a", "b". The
  numeric indices are also accepted for anyone doing arithmetic on them, but the
  names are what on_button() delivers and what apps should prefer.

  Polling and events both work. on_button(key, pressed) is the event path;
  input.down("a") is the polling path, which is usually what a game wants.
*/
#include <Arduino.h>

#include "../hal/buttons.h"
#include "lua_bindings.h"

extern "C" {
#include "../lua/lauxlib.h"
#include "../lua/lua.h"
}

namespace bindings {
namespace {

// Accepts "a" / "A" / 4. Raises rather than silently reading key 0, which would
// turn a typo into a mysterious behaviour bug.
uint8_t checkKey(lua_State *L, int index) {
  if (lua_type(L, index) == LUA_TNUMBER) {
    const int value = (int)luaL_checkinteger(L, index);
    if (value < 0 || value >= BUTTON_COUNT) {
      luaL_error(L, "button index %d out of range (0..%d)", value, BUTTON_COUNT - 1);
    }
    return (uint8_t)value;
  }

  const char *name = luaL_checkstring(L, index);
  for (uint8_t i = 0; i < BUTTON_COUNT; ++i) {
    if (strcasecmp(name, buttons::shortName(i)) == 0) return i;
  }
  luaL_error(L, "unknown button '%s' (up/down/left/right/a/b)", name);
  return 0;  // unreachable; luaL_error does not return
}

int l_down(lua_State *L) {
  lua_pushboolean(L, buttons::down(checkKey(L, 1)));
  return 1;
}

int l_pressed(lua_State *L) {
  lua_pushboolean(L, buttons::pressed(checkKey(L, 1)));
  return 1;
}

int l_released(lua_State *L) {
  lua_pushboolean(L, buttons::released(checkKey(L, 1)));
  return 1;
}

int l_repeated(lua_State *L) {
  lua_pushboolean(L, buttons::repeated(checkKey(L, 1)));
  return 1;
}

int l_held_ms(lua_State *L) {
  lua_pushinteger(L, buttons::heldMs(checkKey(L, 1)));
  return 1;
}

// input.any() - true if anything is held. Cheap "press any key" check.
int l_any(lua_State *L) {
  lua_pushboolean(L, buttons::downMask() != 0);
  return 1;
}

// input.keys() -> { "up", "down", "left", "right", "a", "b" }
int l_keys(lua_State *L) {
  lua_createtable(L, BUTTON_COUNT, 0);
  for (uint8_t i = 0; i < BUTTON_COUNT; ++i) {
    lua_pushstring(L, buttons::shortName(i));
    lua_rawseti(L, -2, i + 1);
  }
  return 1;
}

// input.label(key) -> "SELECT", for on-screen key hints. Deliberately the
// silkscreen word rather than the Lua id: "press a" names a key nobody can find
// on the board.
int l_label(lua_State *L) {
  lua_pushstring(L, buttons::name(checkKey(L, 1)));
  return 1;
}

int l_present(lua_State *L) {
  lua_pushboolean(L, buttons::present());
  return 1;
}

const luaL_Reg FUNCTIONS[] = {
    {"down", l_down},       {"pressed", l_pressed}, {"released", l_released},
    {"repeated", l_repeated}, {"held_ms", l_held_ms}, {"any", l_any},
    {"keys", l_keys},       {"label", l_label},     {"present", l_present},
    {nullptr, nullptr},
};

const Field CONSTANTS[] = {
    {"UP", BTN_UP},       {"DOWN", BTN_DOWN}, {"LEFT", BTN_LEFT},
    {"RIGHT", BTN_RIGHT}, {"A", BTN_A},       {"B", BTN_B},
    {"COUNT", BUTTON_COUNT}, {nullptr, 0},
};

}  // namespace

void openInput(lua_State *L) { setTable(L, "input", FUNCTIONS, CONSTANTS); }

}  // namespace bindings
