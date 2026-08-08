/*
  badge.gfx - drawing.

  Everything targets the shared 320x240 PSRAM framebuffer from display.h. The
  runtime pushes it to the panel after on_draw() returns, so an app never has to
  think about tearing and never sees a half-drawn frame reach the glass.

  Colours are RGB565 integers. gfx.color(r, g, b) builds one from 8-bit
  components; the named constants cover the Solana palette and the usual
  primaries.
*/
#include <Arduino.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>

#include "../hal/display.h"
#include "../ui/theme.h"
#include "lua_bindings.h"
#include "lua_runtime.h"

extern "C" {
#include "../lua/lauxlib.h"
#include "../lua/lua.h"
}

namespace bindings {
namespace {

inline LGFX_Sprite &c() { return display::canvas(); }

// Colour arguments are optional almost everywhere; when omitted, white.
inline uint16_t optColor(lua_State *L, int index, uint16_t fallback = theme::WHITE) {
  return (uint16_t)luaL_optinteger(L, index, fallback);
}

// A radius/corner far larger than the canvas just burns time in the draw loop
// (a huge circle can spin for many seconds inside C, past the watchdog's reach
// since the hook cannot fire mid-C-call). Clamp to the canvas so a bogus value
// cannot hang the badge. Negative radii collapse to 0.
inline int32_t clampRadius(int32_t r) {
  const int32_t maxRadius = display::width() + display::height();
  if (r < 0) return 0;
  if (r > maxRadius) return maxRadius;
  return r;
}

int l_width(lua_State *L) {
  lua_pushinteger(L, display::width());
  return 1;
}

int l_height(lua_State *L) {
  lua_pushinteger(L, display::height());
  return 1;
}

int l_clear(lua_State *L) {
  c().fillScreen(optColor(L, 1, theme::BG));
  display::touch();
  return 0;
}

int l_pixel(lua_State *L) {
  c().drawPixel((int32_t)luaL_checkinteger(L, 1), (int32_t)luaL_checkinteger(L, 2),
                optColor(L, 3));
  display::touch();
  return 0;
}

int l_line(lua_State *L) {
  c().drawLine((int32_t)luaL_checkinteger(L, 1), (int32_t)luaL_checkinteger(L, 2),
               (int32_t)luaL_checkinteger(L, 3), (int32_t)luaL_checkinteger(L, 4),
               optColor(L, 5));
  display::touch();
  return 0;
}

int l_rect(lua_State *L) {
  c().drawRect((int32_t)luaL_checkinteger(L, 1), (int32_t)luaL_checkinteger(L, 2),
               (int32_t)luaL_checkinteger(L, 3), (int32_t)luaL_checkinteger(L, 4),
               optColor(L, 5));
  display::touch();
  return 0;
}

int l_fill_rect(lua_State *L) {
  c().fillRect((int32_t)luaL_checkinteger(L, 1), (int32_t)luaL_checkinteger(L, 2),
               (int32_t)luaL_checkinteger(L, 3), (int32_t)luaL_checkinteger(L, 4),
               optColor(L, 5));
  display::touch();
  return 0;
}

int l_round_rect(lua_State *L) {
  c().drawRoundRect((int32_t)luaL_checkinteger(L, 1), (int32_t)luaL_checkinteger(L, 2),
                    (int32_t)luaL_checkinteger(L, 3), (int32_t)luaL_checkinteger(L, 4),
                    clampRadius((int32_t)luaL_optinteger(L, 5, 4)), optColor(L, 6));
  display::touch();
  return 0;
}

int l_fill_round_rect(lua_State *L) {
  c().fillRoundRect((int32_t)luaL_checkinteger(L, 1), (int32_t)luaL_checkinteger(L, 2),
                    (int32_t)luaL_checkinteger(L, 3), (int32_t)luaL_checkinteger(L, 4),
                    clampRadius((int32_t)luaL_optinteger(L, 5, 4)), optColor(L, 6));
  display::touch();
  return 0;
}

int l_circle(lua_State *L) {
  c().drawCircle((int32_t)luaL_checkinteger(L, 1), (int32_t)luaL_checkinteger(L, 2),
                 clampRadius((int32_t)luaL_checkinteger(L, 3)), optColor(L, 4));
  display::touch();
  return 0;
}

int l_fill_circle(lua_State *L) {
  c().fillCircle((int32_t)luaL_checkinteger(L, 1), (int32_t)luaL_checkinteger(L, 2),
                 clampRadius((int32_t)luaL_checkinteger(L, 3)), optColor(L, 4));
  display::touch();
  return 0;
}

int l_triangle(lua_State *L) {
  c().drawTriangle((int32_t)luaL_checkinteger(L, 1), (int32_t)luaL_checkinteger(L, 2),
                   (int32_t)luaL_checkinteger(L, 3), (int32_t)luaL_checkinteger(L, 4),
                   (int32_t)luaL_checkinteger(L, 5), (int32_t)luaL_checkinteger(L, 6),
                   optColor(L, 7));
  display::touch();
  return 0;
}

int l_fill_triangle(lua_State *L) {
  c().fillTriangle((int32_t)luaL_checkinteger(L, 1), (int32_t)luaL_checkinteger(L, 2),
                   (int32_t)luaL_checkinteger(L, 3), (int32_t)luaL_checkinteger(L, 4),
                   (int32_t)luaL_checkinteger(L, 5), (int32_t)luaL_checkinteger(L, 6),
                   optColor(L, 7));
  display::touch();
  return 0;
}

// Shared by text/text_center/text_right - only the datum differs.
int drawTextWithDatum(lua_State *L, textdatum_t datum) {
  const char *string = luaL_checkstring(L, 1);
  const int32_t x = (int32_t)luaL_checkinteger(L, 2);
  const int32_t y = (int32_t)luaL_checkinteger(L, 3);
  const uint16_t color = optColor(L, 4);
  const uint8_t size = (uint8_t)luaL_optinteger(L, 5, 1);

  c().setTextSize(size);
  c().setTextColor(color);
  c().setTextDatum(datum);
  c().drawString(string, x, y);
  c().setTextDatum(textdatum_t::top_left);
  display::touch();
  return 0;
}

int l_text(lua_State *L) { return drawTextWithDatum(L, textdatum_t::top_left); }
int l_text_center(lua_State *L) { return drawTextWithDatum(L, textdatum_t::top_center); }
int l_text_right(lua_State *L) { return drawTextWithDatum(L, textdatum_t::top_right); }

int l_text_width(lua_State *L) {
  const char *string = luaL_checkstring(L, 1);
  c().setTextSize((uint8_t)luaL_optinteger(L, 2, 1));
  lua_pushinteger(L, c().textWidth(string));
  return 1;
}

int l_text_height(lua_State *L) {
  c().setTextSize((uint8_t)luaL_optinteger(L, 1, 1));
  lua_pushinteger(L, c().fontHeight());
  return 1;
}

int l_color(lua_State *L) {
  const int r = (int)luaL_checkinteger(L, 1);
  const int g = (int)luaL_checkinteger(L, 2);
  const int b = (int)luaL_checkinteger(L, 3);
  lua_pushinteger(L, rgb565((uint8_t)constrain(r, 0, 255), (uint8_t)constrain(g, 0, 255),
                            (uint8_t)constrain(b, 0, 255)));
  return 1;
}

// h in degrees, s and v in 0..1. Handy for anything that cycles colour.
int l_hsv(lua_State *L) {
  float h = (float)luaL_checknumber(L, 1);
  const float s = (float)luaL_optnumber(L, 2, 1.0);
  const float v = (float)luaL_optnumber(L, 3, 1.0);

  h = fmodf(h, 360.0f);
  if (h < 0) h += 360.0f;

  const float chroma = v * s;
  const float x = chroma * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
  const float m = v - chroma;

  float r = 0, g = 0, b = 0;
  if (h < 60) { r = chroma; g = x; }
  else if (h < 120) { r = x; g = chroma; }
  else if (h < 180) { g = chroma; b = x; }
  else if (h < 240) { g = x; b = chroma; }
  else if (h < 300) { r = x; b = chroma; }
  else { r = chroma; b = x; }

  lua_pushinteger(L, rgb565((uint8_t)((r + m) * 255), (uint8_t)((g + m) * 255),
                            (uint8_t)((b + m) * 255)));
  return 1;
}

// The Solana ramp: 0.0 is brand purple, 1.0 is brand green.
int l_gradient(lua_State *L) {
  lua_pushinteger(L, theme::gradient565((float)luaL_checknumber(L, 1)));
  return 1;
}

// Every failure here returns `false, reason` rather than raising.
//
// The alternative - raising for a missing/empty/oversized file and returning
// false only for a decode failure - meant an app could not simply check the
// return value, and every call site needed a pcall. The case that actually
// bites is a zero-byte file, which is exactly what an interrupted push leaves
// behind: an app should be able to draw a "broken image" card, not die.
int imageFailure(lua_State *L, const char *reason) {
  lua_pushboolean(L, false);
  lua_pushstring(L, reason);
  return 2;
}

constexpr size_t IMAGE_MAX_BYTES = 256 * 1024;

// Reads the whole file into a fresh buffer. Caller frees. Returns nullptr and
// sets `reason` on any failure.
uint8_t *loadImageFile(const String &path, size_t &sizeOut, const char *&reason) {
  reason = nullptr;
  File file = LittleFS.open(path, "r");
  if (!file) {
    reason = "cannot open";
    return nullptr;
  }
  const size_t size = file.size();
  if (size == 0) {
    file.close();
    reason = "file is empty (interrupted push?)";
    return nullptr;
  }
  if (size > IMAGE_MAX_BYTES) {
    file.close();
    reason = "larger than the 256 KB limit";
    return nullptr;
  }
  // Prefer PSRAM: this buffer can be up to 256 KB, and taking that from the
  // ~512 KB of internal SRAM would starve the Wi-Fi/BLE stacks. Fall back to
  // internal RAM only if PSRAM is absent or exhausted. free() handles either.
  uint8_t *buffer = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (buffer == nullptr) buffer = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_8BIT);
  if (buffer == nullptr) {
    file.close();
    reason = "out of memory";
    return nullptr;
  }
  const size_t read = file.read(buffer, size);
  file.close();
  if (read != size) {
    free(buffer);
    reason = "short read";
    return nullptr;
  }
  sizeOut = size;
  return buffer;
}

// gfx.image(path, x, y [, scale]) -> ok [, reason]
// PNG or JPEG from the running app's directory. Decoded from RAM rather than
// streamed, so the same path works for both formats and for a sprite target.
int l_image(lua_State *L) {
  const char *relative = luaL_checkstring(L, 1);
  // checknumber, not checkinteger: `(width - w * scale) / 2` is the natural way
  // to centre an image and it produces a float. Making every caller remember
  // math.floor for that one argument, when the rest of gfx accepts whatever
  // Lua's coercion allows, is a trap rather than a discipline.
  const int32_t x = (int32_t)lroundf((float)luaL_checknumber(L, 2));
  const int32_t y = (int32_t)lroundf((float)luaL_checknumber(L, 3));
  const float scale = (float)luaL_optnumber(L, 4, 1.0);

  const String path = runtime::resolveAppPath(String(relative));
  if (path.length() == 0) return imageFailure(L, "path escapes the app directory");

  size_t size = 0;
  const char *reason = nullptr;
  uint8_t *buffer = loadImageFile(path, size, reason);
  if (buffer == nullptr) return imageFailure(L, reason);

  const String lower = String(relative);
  const bool jpeg = lower.endsWith(".jpg") || lower.endsWith(".jpeg");
  const bool ok = jpeg ? c().drawJpg(buffer, size, x, y, 0, 0, 0, 0, scale, scale)
                       : c().drawPng(buffer, size, x, y, 0, 0, 0, 0, scale, scale);
  free(buffer);

  display::touch();
  if (!ok) return imageFailure(L, "decode failed");
  lua_pushboolean(L, true);
  return 1;
}

// gfx.image_size(path) -> w, h  (nil, reason on failure)
//
// Reads only the header, so measuring a picture costs a few dozen bytes rather
// than a full decode. Without this an app that wants to centre or fit an image
// has to re-implement PNG IHDR and JPEG SOF parsing in Lua - which the first
// app to need it duly did, in about fifty lines.
int l_image_size(lua_State *L) {
  const char *relative = luaL_checkstring(L, 1);
  const String path = runtime::resolveAppPath(String(relative));
  if (path.length() == 0) {
    lua_pushnil(L);
    lua_pushstring(L, "path escapes the app directory");
    return 2;
  }

  File file = LittleFS.open(path, "r");
  if (!file) {
    lua_pushnil(L);
    lua_pushstring(L, "cannot open");
    return 2;
  }

  // Enough for a PNG IHDR outright, and for the JPEG marker walk to reach SOF
  // on any normally-encoded file.
  uint8_t head[1024];
  const size_t got = file.read(head, sizeof(head));
  file.close();

  uint32_t width = 0, height = 0;

  static const uint8_t PNG_SIGNATURE[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (got >= 24 && memcmp(head, PNG_SIGNATURE, 8) == 0 && memcmp(head + 12, "IHDR", 4) == 0) {
    // IHDR is always the first chunk; width and height are big-endian at 16/20.
    width = ((uint32_t)head[16] << 24) | ((uint32_t)head[17] << 16) |
            ((uint32_t)head[18] << 8) | head[19];
    height = ((uint32_t)head[20] << 24) | ((uint32_t)head[21] << 16) |
             ((uint32_t)head[22] << 8) | head[23];
  } else if (got >= 4 && head[0] == 0xFF && head[1] == 0xD8) {
    // Walk the marker chain to the start-of-frame, whose payload carries the
    // dimensions. Skip standalone markers, which have no length field.
    size_t at = 2;
    while (at + 9 < got) {
      if (head[at] != 0xFF) {
        ++at;
        continue;
      }
      const uint8_t marker = head[at + 1];
      if (marker == 0xFF || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD9)) {
        at += 2;
        continue;
      }
      const uint16_t length = ((uint16_t)head[at + 2] << 8) | head[at + 3];
      const bool isSof = (marker >= 0xC0 && marker <= 0xCF) && marker != 0xC4 &&
                         marker != 0xC8 && marker != 0xCC;
      if (isSof) {
        height = ((uint32_t)head[at + 5] << 8) | head[at + 6];
        width = ((uint32_t)head[at + 7] << 8) | head[at + 8];
        break;
      }
      if (length < 2) break;  // malformed; refuse to loop
      at += 2 + length;
    }
  }

  if (width == 0 || height == 0) {
    lua_pushnil(L);
    lua_pushstring(L, "not a PNG or JPEG, or header is truncated");
    return 2;
  }
  lua_pushinteger(L, (lua_Integer)width);
  lua_pushinteger(L, (lua_Integer)height);
  return 2;
}

// Screen backlight, 0..255. Reads when called with no argument.
int l_brightness(lua_State *L) {
  if (lua_gettop(L) >= 1) {
    display::setBrightness((uint8_t)constrain((int)luaL_checkinteger(L, 1), 0, 255));
  }
  lua_pushinteger(L, display::brightness());
  return 1;
}

// Pushes the framebuffer immediately. The runtime already does this after
// on_draw(); apps only need it if they draw from somewhere else, like a long
// loading step inside on_start().
int l_flush(lua_State *L) {
  (void)L;
  display::invalidate();
  display::flush();
  return 0;
}

const luaL_Reg FUNCTIONS[] = {
    {"width", l_width},
    {"height", l_height},
    {"clear", l_clear},
    {"pixel", l_pixel},
    {"line", l_line},
    {"rect", l_rect},
    {"fill_rect", l_fill_rect},
    {"round_rect", l_round_rect},
    {"fill_round_rect", l_fill_round_rect},
    {"circle", l_circle},
    {"fill_circle", l_fill_circle},
    {"triangle", l_triangle},
    {"fill_triangle", l_fill_triangle},
    {"text", l_text},
    {"text_center", l_text_center},
    {"text_right", l_text_right},
    {"text_width", l_text_width},
    {"text_height", l_text_height},
    {"color", l_color},
    {"hsv", l_hsv},
    {"gradient", l_gradient},
    {"image", l_image},
    {"image_size", l_image_size},
    {"brightness", l_brightness},
    {"flush", l_flush},
    {nullptr, nullptr},
};

const Field CONSTANTS[] = {
    {"SOLANA_PURPLE", theme::PURPLE},
    {"SOLANA_GREEN", theme::GREEN},
    {"SOLANA_TEAL", theme::TEAL},
    {"SOLANA_MAGENTA", theme::MAGENTA},
    {"BLACK", theme::BLACK},
    {"WHITE", theme::WHITE},
    {"BG", theme::BG},
    {"PANEL", theme::PANEL},
    {"BORDER", theme::BORDER},
    {"MUTED", theme::MUTED},
    {"RED", rgb565(0xFF, 0x45, 0x45)},
    {"ORANGE", rgb565(0xFF, 0xB0, 0x20)},
    {"YELLOW", rgb565(0xFF, 0xE0, 0x40)},
    {"GREEN", theme::GREEN},
    {"CYAN", rgb565(0x00, 0xE5, 0xFF)},
    {"BLUE", rgb565(0x40, 0x80, 0xFF)},
    {nullptr, 0},
};

}  // namespace

void openGfx(lua_State *L) { setTable(L, "gfx", FUNCTIONS, CONSTANTS); }

}  // namespace bindings
