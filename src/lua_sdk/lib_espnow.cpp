/*
  badge.espnow - badge-to-badge messaging with no access point in between.

  Incoming application messages arrive as on_espnow(mac, data, rssi). The
  presence beacons that populate espnow.peers() are handled by the OS and never
  surface as app messages, so an app that only wants to know who is nearby can
  poll peers() and never define the callback at all.

  Range/signal: every peer entry carries the RSSI of the last frame heard from
  it, which is what Settings -> ESP-NOW draws its bars from.
*/
#include <Arduino.h>

#include "../net/espnow_mgr.h"
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
    const uint8_t channel = (uint8_t)luaL_optinteger(L, 2, settings::espnowChannel());
    lua_pushboolean(L, espnow_mgr::begin(channel));
  } else {
    espnow_mgr::end();
    lua_pushboolean(L, true);
  }
  return 1;
}

int l_enabled(lua_State *L) {
  lua_pushboolean(L, espnow_mgr::enabled());
  return 1;
}

int l_channel(lua_State *L) {
  lua_pushinteger(L, espnow_mgr::channel());
  return 1;
}

int l_broadcast(lua_State *L) {
  size_t length = 0;
  const char *data = luaL_checklstring(L, 1, &length);
  if (length > ESPNOW_MAX_PAYLOAD) {
    return luaL_error(L, "espnow: payload is %d bytes, limit is %d", (int)length,
                      (int)ESPNOW_MAX_PAYLOAD);
  }
  lua_pushboolean(L, espnow_mgr::broadcast((const uint8_t *)data, length));
  return 1;
}

int l_send(lua_State *L) {
  const char *macText = luaL_checkstring(L, 1);
  size_t length = 0;
  const char *data = luaL_checklstring(L, 2, &length);

  uint8_t mac[6];
  bool macValid;
  {
    // Scope the String so it is destroyed before any luaL_error longjmp below;
    // luaL_error skips C++ destructors, so nothing heap-owning may be live.
    const String macStr(macText);
    macValid = espnow_mgr::macFromString(macStr, mac);
  }
  if (!macValid) {
    return luaL_error(L, "espnow: '%s' is not a MAC address (aa:bb:cc:dd:ee:ff)", macText);
  }
  if (length > ESPNOW_MAX_PAYLOAD) {
    return luaL_error(L, "espnow: payload is %d bytes, limit is %d", (int)length,
                      (int)ESPNOW_MAX_PAYLOAD);
  }
  lua_pushboolean(L, espnow_mgr::send(mac, (const uint8_t *)data, length));
  return 1;
}

// espnow.peers() -> array of { mac, name, rssi, age_ms, packets }, strongest
// signal first so a "who is closest" app needs no sorting of its own.
int l_peers(lua_State *L) {
  const size_t count = espnow_mgr::peerCount();

  // Index order by RSSI, descending. n is at most ESPNOW_MAX_PEERS (20), so an
  // insertion sort is the right tool.
  uint8_t order[ESPNOW_MAX_PEERS];
  for (size_t i = 0; i < count; ++i) order[i] = (uint8_t)i;
  for (size_t i = 1; i < count; ++i) {
    const uint8_t key = order[i];
    const int8_t keyRssi = espnow_mgr::peerAt(key)->rssi;
    size_t j = i;
    while (j > 0 && espnow_mgr::peerAt(order[j - 1])->rssi < keyRssi) {
      order[j] = order[j - 1];
      --j;
    }
    order[j] = key;
  }

  const uint32_t now = millis();
  lua_createtable(L, (int)count, 0);
  for (size_t i = 0; i < count; ++i) {
    const espnow_mgr::Peer *peer = espnow_mgr::peerAt(order[i]);
    if (peer == nullptr) continue;
    lua_createtable(L, 0, 5);
    lua_pushstring(L, espnow_mgr::macToString(peer->mac).c_str());
    lua_setfield(L, -2, "mac");
    lua_pushstring(L, peer->name);
    lua_setfield(L, -2, "name");
    lua_pushinteger(L, peer->rssi);
    lua_setfield(L, -2, "rssi");
    lua_pushinteger(L, (lua_Integer)(now - peer->lastSeenMs));
    lua_setfield(L, -2, "age_ms");
    lua_pushinteger(L, (lua_Integer)peer->packets);
    lua_setfield(L, -2, "packets");
    lua_rawseti(L, -2, (lua_Integer)i + 1);
  }
  return 1;
}

// espnow.signal(mac) -> rssi, age_ms. nil if that peer has not been heard.
int l_signal(lua_State *L) {
  const char *macText = luaL_checkstring(L, 1);
  uint8_t mac[6];
  bool macValid;
  {
    const String macStr(macText);
    macValid = espnow_mgr::macFromString(macStr, mac);
  }
  if (!macValid) {
    return luaL_error(L, "espnow: '%s' is not a MAC address", macText);
  }
  const espnow_mgr::Peer *peer = espnow_mgr::peerByMac(mac);
  if (peer == nullptr) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushinteger(L, peer->rssi);
  lua_pushinteger(L, (lua_Integer)(millis() - peer->lastSeenMs));
  return 2;
}

// Whether this badge announces itself. Turning it off makes the badge invisible
// to other badges' radars while still able to see them.
int l_beacon(lua_State *L) {
  if (lua_gettop(L) >= 1) espnow_mgr::setBeaconEnabled(lua_toboolean(L, 1));
  lua_pushboolean(L, espnow_mgr::beaconEnabled());
  return 1;
}

int l_clear(lua_State *L) {
  (void)L;
  espnow_mgr::clearPeers();
  return 0;
}

const luaL_Reg FUNCTIONS[] = {
    {"enable", l_enable},       {"enabled", l_enabled}, {"channel", l_channel},
    {"broadcast", l_broadcast}, {"send", l_send},       {"peers", l_peers},
    {"signal", l_signal},       {"beacon", l_beacon},   {"clear", l_clear},
    {nullptr, nullptr},
};

const Field CONSTANTS[] = {
    {"MAX_PAYLOAD", (int)ESPNOW_MAX_PAYLOAD},
    {"MAX_PEERS", (int)ESPNOW_MAX_PEERS},
    {nullptr, 0},
};

}  // namespace

void openEspnow(lua_State *L) { setTable(L, "espnow", FUNCTIONS, CONSTANTS); }

}  // namespace bindings
