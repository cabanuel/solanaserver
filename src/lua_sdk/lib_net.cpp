/*
  badge.wifi and badge.http.

  wifi.connect() returns immediately - joining takes seconds and blocking the
  main loop for that long would stall the display, the buttons and every other
  radio. Poll wifi.connected() from on_update() instead.

  http.get/post DO block, because there is no sane non-blocking shape for them
  in a callback-driven script. They extend the runtime's deadline by their own
  timeout so a legitimately slow request is not killed as a runaway loop, and
  the timeout is capped at 10s.
*/
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>

#include "../net/push_server.h"
#include "../net/wifi_mgr.h"
#include "../settings.h"
#include "lua_bindings.h"
#include "lua_runtime.h"

extern "C" {
#include "../lua/lauxlib.h"
#include "../lua/lua.h"
}

namespace bindings {
namespace {

// -- wifi --------------------------------------------------------------------

int l_connect(lua_State *L) {
  const char *ssid = luaL_checkstring(L, 1);
  const char *password = luaL_optstring(L, 2, "");
  // Apps do not get to overwrite the user's saved network.
  lua_pushboolean(L, wifi_mgr::connect(String(ssid), String(password), false));
  return 1;
}

// wifi.connect_enterprise{ ssid=, username=, password=, ca=, domain=, method=,
//                          identity=, phase2= }
//
// A table rather than eight positional arguments, because at a call site
// `wifi.connect_enterprise{ssid="DefCon", username=u, password=p, ca="defcon.pem",
// domain="wifireg.defcon.org"}` is readable and the positional form is not.
int l_connect_enterprise(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);

  auto field = [&](const char *key, const char *fallback = "") -> String {
    lua_getfield(L, 1, key);
    const char *value = lua_isstring(L, -1) ? lua_tostring(L, -1) : fallback;
    const String out(value);
    lua_pop(L, 1);
    return out;
  };

  // luaL_error longjmps and skips C++ destructors, so no heap-owning String
  // (ssid, or the Enterprise config's Strings) may be live across it. Compute
  // everything inside a scope, capture the outcome as plain scalars, let the
  // Strings destruct, and only then raise or push results.
  const char *errmsg = nullptr;
  bool ok = false;
  bool caPresent = false;
  {
    const String ssid = field("ssid");

    wifi_mgr::Enterprise config;
    config.method = wifi_mgr::eapFromString(field("method", "peap"));
    config.identity = field("identity");
    config.username = field("username");
    config.password = field("password");
    config.caCertName = field("ca");
    config.domain = field("domain");
    config.ttlsPhase2 = field("phase2", "mschapv2");

    if (ssid.length() == 0) {
      errmsg = "connect_enterprise: ssid is required";
    } else if (!config.valid()) {
      errmsg = "connect_enterprise: username is required for PEAP and TTLS";
    } else {
      // As with wifi.connect(), an app does not get to overwrite the user's
      // saved network - it can join one for its own purposes, not repoint the
      // badge.
      ok = wifi_mgr::connectEnterprise(ssid, config, false);
      caPresent = config.caCertName.length() > 0;
    }
  }  // ssid and config are destroyed here, before any longjmp below.

  if (errmsg != nullptr) return luaL_error(L, "%s", errmsg);

  lua_pushboolean(L, ok);
  if (!ok) {
    // The usual cause is a CA name that is not installed, and returning a
    // reason saves the app author a trip to the console.
    lua_pushstring(L, caPresent ? "association refused (is the CA certificate installed?)"
                                : "association refused");
    return 2;
  }
  return 1;
}

int l_enterprise(lua_State *L) {
  lua_pushboolean(L, wifi_mgr::usingEnterprise());
  return 1;
}

int l_disconnect(lua_State *L) {
  (void)L;
  wifi_mgr::disconnect();
  return 0;
}

int l_connected(lua_State *L) {
  lua_pushboolean(L, wifi_mgr::connected());
  return 1;
}

int l_status(lua_State *L) {
  lua_pushstring(L, wifi_mgr::statusText());
  return 1;
}

int l_ip(lua_State *L) {
  lua_pushstring(L, wifi_mgr::ip().toString().c_str());
  return 1;
}

int l_ssid(lua_State *L) {
  lua_pushstring(L, wifi_mgr::ssid().c_str());
  return 1;
}

int l_rssi(lua_State *L) {
  lua_pushinteger(L, wifi_mgr::rssi());
  return 1;
}

int l_mac(lua_State *L) {
  // Lowercased to match badge.espnow: peers()[i].mac and the mac handed to
  // on_espnow() both come from espnow_mgr::macToString(), which formats with
  // %02x, while Arduino's WiFi.macAddress() returns uppercase. An app comparing
  // its own address against an ESP-NOW sender - the obvious way to filter out
  // your own broadcasts - would silently never match. One convention, and it is
  // the one apps actually compare against.
  String mac = wifi_mgr::macAddress();
  mac.toLowerCase();
  lua_pushstring(L, mac.c_str());
  return 1;
}

int l_channel(lua_State *L) {
  lua_pushinteger(L, wifi_mgr::channel());
  return 1;
}

// wifi.scan() kicks off an async scan; wifi.scanning() reports progress and
// wifi.networks() returns the results once it is done.
int l_scan(lua_State *L) {
  lua_pushboolean(L, wifi_mgr::startScan());
  return 1;
}

int l_scanning(lua_State *L) {
  lua_pushboolean(L, wifi_mgr::scanning());
  return 1;
}

int l_networks(lua_State *L) {
  const int count = wifi_mgr::scanResultCount();
  lua_createtable(L, count, 0);
  for (int i = 0; i < count; ++i) {
    lua_createtable(L, 0, 3);
    lua_pushstring(L, wifi_mgr::scanSsid(i).c_str());
    lua_setfield(L, -2, "ssid");
    lua_pushinteger(L, wifi_mgr::scanRssi(i));
    lua_setfield(L, -2, "rssi");
    lua_pushboolean(L, wifi_mgr::scanEncrypted(i));
    lua_setfield(L, -2, "encrypted");
    lua_rawseti(L, -2, i + 1);
  }
  return 1;
}

int l_hotspot(lua_State *L) {
  const char *password = luaL_optstring(L, 1, "");
  lua_pushboolean(L, wifi_mgr::startAccessPoint(String(password)));
  return 1;
}

// wifi.serve_page(html) swaps in the app's own "/" and "/index.html" response
// on the push server that the main loop already brings up on port 80 whenever
// Wi-Fi (station or hotspot) is connected - see push_server::setIndexOverride.
// It does not start Wi-Fi itself; call wifi.hotspot() (or connect()) first.
int l_serve_page(lua_State *L) {
  const char *html = luaL_checkstring(L, 1);
  push_server::setIndexOverride(String(html));
  return 0;
}

// Restores the normal push-server UI. wifi.disconnect() and app teardown do
// not call this implicitly, so an app that overrides the page should clear it
// from on_stop.
int l_clear_page(lua_State *L) {
  (void)L;
  push_server::clearIndexOverride();
  return 0;
}

const luaL_Reg WIFI_FUNCTIONS[] = {
    {"connect", l_connect},
    {"connect_enterprise", l_connect_enterprise},
    {"enterprise", l_enterprise},
    {"disconnect", l_disconnect}, {"connected", l_connected},
    {"status", l_status},     {"ip", l_ip},                 {"ssid", l_ssid},
    {"rssi", l_rssi},         {"mac", l_mac},               {"channel", l_channel},
    {"scan", l_scan},         {"scanning", l_scanning},     {"networks", l_networks},
    {"hotspot", l_hotspot},   {"serve_page", l_serve_page}, {"clear_page", l_clear_page},
    {nullptr, nullptr},
};

// -- http --------------------------------------------------------------------

// Shared by get and post. Returns status, body on success; nil, message on a
// transport failure.
int request(lua_State *L, const char *method, bool hasBody) {
  const char *url = luaL_checkstring(L, 1);

  int bodyArg = 2;
  size_t bodyLength = 0;
  const char *body = nullptr;
  if (hasBody) {
    body = luaL_optlstring(L, 2, "", &bodyLength);
    bodyArg = 3;
  }
  const char *contentType = luaL_optstring(L, bodyArg, "text/plain");
  const uint32_t timeoutMs =
      (uint32_t)constrain((int)luaL_optinteger(L, bodyArg + 1, 5000), 100, 10000);

  if (!wifi_mgr::connected()) {
    lua_pushnil(L);
    lua_pushstring(L, "wifi not connected");
    return 2;
  }

  // The call below blocks for up to timeoutMs; without this the instruction
  // hook would fire mid-request and kill the app for waiting on the network.
  runtime::extendDeadline(timeoutMs + 500);

  HTTPClient http;
  http.setTimeout(timeoutMs);
  http.setConnectTimeout(timeoutMs);
  if (!http.begin(String(url))) {
    lua_pushnil(L);
    lua_pushstring(L, "bad url");
    return 2;
  }
  if (hasBody) http.addHeader("Content-Type", contentType);

  const int status = hasBody ? http.POST((uint8_t *)body, bodyLength) : http.GET();
  if (status <= 0) {
    const String error = HTTPClient::errorToString(status);
    http.end();
    lua_pushnil(L);
    lua_pushstring(L, error.c_str());
    return 2;
  }

  const String payload = http.getString();
  http.end();

  lua_pushinteger(L, status);
  lua_pushlstring(L, payload.c_str(), payload.length());
  return 2;
}

int l_http_get(lua_State *L) { return request(L, "GET", false); }
int l_http_post(lua_State *L) { return request(L, "POST", true); }

const luaL_Reg HTTP_FUNCTIONS[] = {
    {"get", l_http_get}, {"post", l_http_post}, {nullptr, nullptr},
};

}  // namespace

void openNet(lua_State *L) {
  setTable(L, "wifi", WIFI_FUNCTIONS, nullptr);
  setTable(L, "http", HTTP_FUNCTIONS, nullptr);
}

}  // namespace bindings
