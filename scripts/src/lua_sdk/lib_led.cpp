/*
  badge.led - the two WS2812B RGB LEDs.

  Writes are buffered; led.show() latches them. set()/all() do not push on their
  own so an app can stage both LEDs and update them in one go, which matters
  because each show() holds interrupts off for the duration of the bit-bang.
*/
#include <Arduino.h>

#include "../hal/leds.h"
#include "../ui/theme.h"
#include "lua_bindings.h"

extern "C" {
#include "../lua/lauxlib.h"
#include "../lua/lua.h"
}

namespace bindings {
namespace {

inline uint8_t channel(lua_State *L, int index) {
  return (uint8_t)constrain((int)luaL_checkinteger(L, index), 0, 255);
}

// led.set(index, r, g, b) - index is 0 or 1.
int l_set(lua_State *L) {
  const int index = (int)luaL_checkinteger(L, 1);
  if (index < 0 || index >= RGB_LED_COUNT) {
    return luaL_error(L, "led index %d out of range (0..%d)", index, RGB_LED_COUNT - 1);
  }
  leds::set((uint8_t)index, channel(L, 2), channel(L, 3), channel(L, 4));
  return 0;
}

int l_all(lua_State *L) {
  leds::setAll(channel(L, 1), channel(L, 2), channel(L, 3));
  return 0;
}

// led.gradient(index, t) - a point on the Solana ramp, 0.0 purple to 1.0 green.
int l_gradient(lua_State *L) {
  const int index = (int)luaL_checkinteger(L, 1);
  if (index < 0 || index >= RGB_LED_COUNT) {
    return luaL_error(L, "led index %d out of range (0..%d)", index, RGB_LED_COUNT - 1);
  }
  uint8_t r, g, b;
  theme::gradient((float)luaL_checknumber(L, 2), r, g, b);
  const float intensity = (float)luaL_optnumber(L, 3, 1.0);
  leds::set((uint8_t)index, (uint8_t)(r * intensity), (uint8_t)(g * intensity),
            (uint8_t)(b * intensity));
  return 0;
}

int l_show(lua_State *L) {
  (void)L;
  leds::show();
  return 0;
}

int l_off(lua_State *L) {
  (void)L;
  leds::off();
  return 0;
}

// A self-decaying flash. Runs from the OS's animation tick, so it keeps going
// without the app doing anything per-frame.
int l_pulse(lua_State *L) {
  leds::pulse(channel(L, 1), channel(L, 2), channel(L, 3),
              (uint16_t)luaL_optinteger(L, 4, 400));
  return 0;
}

int l_brightness(lua_State *L) {
  if (lua_gettop(L) >= 1) leds::setBrightness(channel(L, 1));
  lua_pushinteger(L, leds::brightness());
  return 1;
}

// Cancels any OS animation (boot sequence, idle breathing, a pulse) so the app
// has the LEDs to itself.
int l_take(lua_State *L) {
  (void)L;
  leds::stopAnimation();
  return 0;
}

const luaL_Reg FUNCTIONS[] = {
    {"set", l_set},       {"all", l_all},   {"gradient", l_gradient},
    {"show", l_show},     {"off", l_off},   {"pulse", l_pulse},
    {"brightness", l_brightness}, {"take", l_take}, {nullptr, nullptr},
};

const Field CONSTANTS[] = {
    {"COUNT", RGB_LED_COUNT},
    {nullptr, 0},
};

}  // namespace

void openLed(lua_State *L) { setTable(L, "led", FUNCTIONS, CONSTANTS); }

}  // namespace bindings
