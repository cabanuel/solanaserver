/*
  Output hooks for the vendored Lua core.

  luaconf.h is patched to route lua_writestring / lua_writeline /
  lua_writestringerror through these two functions, so an app's print() and
  any panic text land in the badge logger (UART0 + USB CDC + the on-screen
  console) instead of vanishing into a stdout nothing is attached to.

  Kept deliberately tiny: luaconf.h is pulled in very early by every Lua
  translation unit, so this header must not drag in anything heavy.

  Definitions live in ../lua_sdk/lua_runtime.cpp.
*/
#ifndef BADGE_LUA_IO_H
#define BADGE_LUA_IO_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Raw write of `len` bytes - Lua does not NUL-terminate what it hands us. */
void badge_lua_write(const char *text, size_t len);

/* Always called as (format, string); no other argument type is ever passed. */
void badge_lua_error_printf(const char *format, const char *argument);

#ifdef __cplusplus
}
#endif

#endif /* BADGE_LUA_IO_H */
