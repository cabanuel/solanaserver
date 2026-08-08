/*
  HTTP app-push API, plus a small browser UI for it.

  Runs whenever Wi-Fi is up (joined to a network or serving its own hotspot) and
  is reachable at http://<ip>/ or http://solana-badge.local/.

  Auth: every /api route except /api/status wants the six-digit pairing code
  shown in Settings -> Push. Read-only GET routes accept it as an X-Badge-Token
  header or a ?token= query parameter; state-changing routes (POST/PUT/DELETE)
  require the header and reject the query parameter, so a cross-site link cannot
  carry the token and browsers must send a CORS preflight. Turn pairing off in
  Settings if it gets in the way.

  /api/status is reachable without a code but reveals only generic hardware
  status unless authenticated; the network identity and running app are gated.

  State-changing routes additionally require the request's Host to name this
  badge and any Origin to be same-origin, which defeats DNS-rebinding attacks
  from a page the wearer merely visits. CORS is same-origin only (no wildcard).

    GET    /                          browser UI
    GET    /api/status                device, radios, filesystem
    GET    /api/apps                  installed apps
    POST   /api/app?id=&path=&enc=    write one file; body is the content
    DELETE /api/app?id=               uninstall
    POST   /api/run?id=               launch
    POST   /api/stop                  back to the launcher
    GET    /api/wifi                  status + last scan results
    POST   /api/wifi/scan             start an async scan
    POST   /api/wifi                  {"ssid":"...","password":"..."} - join
    GET    /api/logs                  recent log lines
    POST   /api/reboot
    GET    /api/identity              badge id, public key, where the key lives
    GET    /api/broker                app-store client status
    POST   /api/broker                {"url":"...","enabled":true} - configure it

  `enc=base64` decodes the body first, which is how binary assets get through.

  /api/identity and /api/broker are also served under /api/v1/ so that a page
  written against broker/PROTOCOL.md's versioned paths finds them where it
  expects. The badge's own API is otherwise unversioned.
*/
#pragma once

#include <Arduino.h>

namespace push_server {

void begin();
void stop();
bool running();
void update();

// Lets an app replace the "/" and "/index.html" response with its own HTML
// while it runs (badge.wifi.serve_page in the Lua SDK) - a page hosted from a
// hotspot without touching the push protocol underneath it. An empty string
// (the default, and what clearIndexOverride() restores) falls back to the
// normal browser UI.
void setIndexOverride(const String &html);
void clearIndexOverride();

}  // namespace push_server
