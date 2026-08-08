#include "push_server.h"

#include <ESPmDNS.h>
#include <WebServer.h>
#include <mbedtls/base64.h>

#include "../apps/app_store.h"
#include "../badge_log.h"
#include "../config.h"
#include "../hal/leds.h"
#include "../hal/power.h"
#include "../identity/identity.h"
#include "../lua_sdk/lua_runtime.h"
#include "../settings.h"
#include "broker_client.h"
#include "cert_store.h"
#include "espnow_mgr.h"
#include "wifi_mgr.h"

namespace push_server {
namespace {

WebServer sServer(PUSH_SERVER_PORT);
bool sRunning = false;
bool sMdnsUp = false;
String sIndexOverride;

// ---------------------------------------------------------------------------
// The browser UI. Deliberately one self-contained page with no external assets:
// the badge is often its own access point with no route to the internet, so
// anything it cannot serve itself would simply not load.
// ---------------------------------------------------------------------------
const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Solana Badge</title><style>
:root{--pu:#9945FF;--gr:#14F195;--bg:#0b0b12;--pa:#16161f;--bo:#2c2c3a;--tx:#e8e8f0;--mu:#9393a8}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--tx);
font:15px/1.5 ui-sans-serif,system-ui,-apple-system,Segoe UI,Roboto,sans-serif;padding:20px}
.wrap{max-width:760px;margin:0 auto}
h1{font-size:22px;margin:0 0 4px;background:linear-gradient(90deg,var(--pu),var(--gr));
-webkit-background-clip:text;background-clip:text;color:transparent;display:inline-block}
.sub{color:var(--mu);font-size:13px;margin-bottom:20px}
.card{background:var(--pa);border:1px solid var(--bo);border-radius:10px;padding:16px;margin-bottom:14px}
h2{font-size:13px;text-transform:uppercase;letter-spacing:.08em;color:var(--mu);margin:0 0 12px}
label{display:block;font-size:12px;color:var(--mu);margin:10px 0 4px}
input,select,textarea{width:100%;background:#0f0f18;border:1px solid var(--bo);color:var(--tx);
border-radius:7px;padding:9px 11px;font:inherit}
textarea{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:13px;min-height:190px}
button{background:linear-gradient(90deg,var(--pu),var(--gr));color:#0b0b12;border:0;border-radius:7px;
padding:9px 16px;font-weight:650;cursor:pointer;font-size:14px}
button.ghost{background:transparent;color:var(--tx);border:1px solid var(--bo);font-weight:500}
button.danger{background:transparent;color:#ff6b6b;border:1px solid #52323c;font-weight:500}
.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}
.app{display:flex;justify-content:space-between;align-items:center;gap:10px;padding:10px 0;
border-bottom:1px solid var(--bo)}.app:last-child{border-bottom:0}
.app b{font-weight:600}.app small{color:var(--mu);display:block;font-size:12px}
pre{background:#0f0f18;border:1px solid var(--bo);border-radius:7px;padding:11px;overflow:auto;
font-size:12px;max-height:230px;margin:0;white-space:pre-wrap}
.ok{color:var(--gr)}.err{color:#ff6b6b}
</style></head><body><div class="wrap">
<h1>Solana Badge</h1><div class="sub" id="sub">connecting...</div>

<div class="card"><h2>Pairing</h2>
<label>Six-digit code from Settings &rarr; Push</label>
<input id="tok" inputmode="numeric" maxlength="6" placeholder="000000">
<div class="sub" style="margin:8px 0 0">Stored in this browser only.</div></div>

<div class="card"><h2>Wi-Fi</h2>
<div class="sub" id="wifi" style="margin:0 0 8px">...</div>
<div class="row"><div style="flex:1;min-width:150px"><label>Network</label>
<input id="ssid" list="nets" placeholder="SSID"><datalist id="nets"></datalist></div>
<div style="flex:1;min-width:150px"><label>Security</label>
<select id="sec" onchange="secChanged()">
<option value="psk">Personal (WPA2-PSK)</option>
<option value="eap">Enterprise (WPA2-EAP)</option></select></div></div>

<div id="pskbox"><label>Password</label>
<input id="pass" type="password" placeholder="leave blank if open"></div>

<div id="eapbox" style="display:none">
<div class="row"><div style="flex:1;min-width:130px"><label>EAP method</label>
<select id="eapm"><option value="peap">PEAP</option><option value="ttls">TTLS</option></select></div>
<div style="flex:1;min-width:130px"><label>Inner auth (TTLS only)</label>
<select id="ph2"><option value="mschapv2">MSCHAPv2</option><option value="pap">PAP</option>
<option value="chap">CHAP</option><option value="mschap">MSCHAP</option></select></div></div>
<div class="row"><div style="flex:1;min-width:130px"><label>Username</label>
<input id="euser" placeholder="from wifireg.defcon.org"></div>
<div style="flex:1;min-width:130px"><label>Password</label>
<input id="epass" type="password"></div></div>
<div class="row"><div style="flex:1;min-width:130px"><label>Outer identity (optional)</label>
<input id="eident" placeholder="defaults to username"></div>
<div style="flex:1;min-width:130px"><label>Expected server domain</label>
<input id="edom" placeholder="wifireg.defcon.org"></div></div>
<label>CA certificate</label>
<div class="row"><select id="eca" style="flex:1"><option value="">none — server NOT validated</option></select>
<label style="margin:0"><input type="file" id="cafile" style="width:auto" onchange="uploadCert()"></label></div>
<div class="sub" style="margin:8px 0 0">PEM only. Convert a <code>.crt</code> in DER form
with <code>openssl x509 -inform der -in x.crt -out x.pem</code>.</div>
</div>

<div class="row" style="margin-top:12px">
<button onclick="joinWifi()">Join</button>
<button class="ghost" onclick="scanWifi()">Scan</button>
<button class="ghost" onclick="defconPreset()">DEF CON preset</button></div>
<div class="sub" style="margin:10px 0 0">Joining moves the badge onto the new
network, so this page will stop responding at its current address.</div></div>

<div class="card"><h2>App store</h2>
<div class="sub" style="margin:0 0 8px">This badge's ID is what someone types into
the App Store page to send it scripts. Nothing installs until the wearer presses A.</div>
<div class="row"><div style="flex:1;min-width:150px"><label>Badge ID</label>
<input id="bid" readonly></div>
<div style="flex:1;min-width:150px"><label>Key lives in</label>
<input id="bsrc" readonly></div></div>
<label>Public key</label><input id="bpk" readonly>
<label>Broker address</label>
<input id="burl" placeholder="http://192.168.1.20:8787">
<div class="row" style="margin-top:12px">
<button onclick="saveBroker(true)">Save &amp; enable</button>
<button class="ghost" onclick="saveBroker(false)">Disable</button></div>
<div class="sub" id="bstat" style="margin:10px 0 0">...</div></div>

<div class="card"><h2>Installed apps</h2><div id="apps">loading...</div>
<div class="row" style="margin-top:12px">
<button class="ghost" onclick="refresh()">Refresh</button>
<button class="ghost" onclick="post('/api/stop')">Stop running app</button></div></div>

<div class="card"><h2>Push an app</h2>
<div class="row"><div style="flex:1;min-width:150px"><label>App id</label>
<input id="id" placeholder="my-app" value="my-app"></div>
<div style="flex:1;min-width:150px"><label>File</label>
<input id="path" value="main.lua"></div></div>
<label>Source</label>
<textarea id="src">function on_start()
  badge.log("hello from " .. badge.device_name)
end

function on_draw()
  local g = badge.gfx
  g.clear(g.color(11, 11, 18))
  g.text_center("Hello, Solana", g.width() // 2, 100, g.SOLANA_GREEN, 2)
end

function on_button(key, pressed)
  if key == "b" and pressed then badge.system.exit() end
end</textarea>
<div class="row" style="margin-top:12px">
<button onclick="push(false)">Upload</button>
<button onclick="push(true)">Upload &amp; run</button>
<label style="margin:0"><input type="file" id="file" style="width:auto" onchange="pickFile()"></label>
</div></div>

<div class="card"><h2>Log</h2><pre id="log">...</pre>
<div class="row" style="margin-top:12px"><button class="ghost" onclick="loadLog()">Refresh</button>
<button class="danger" onclick="post('/api/reboot')">Reboot badge</button></div></div>
</div><script>
const $=s=>document.getElementById(s);
$('tok').value=localStorage.getItem('badgeToken')||'';
$('tok').oninput=e=>localStorage.setItem('badgeToken',e.target.value);
const hdr=()=>({'X-Badge-Token':$('tok').value});
async function j(u,o){const r=await fetch(u,{...o,headers:{...hdr(),...(o?.headers||{})}});
  if(!r.ok)throw new Error(await r.text());return r.json().catch(()=>({}))}
async function post(u,body,ct){try{await j(u,{method:'POST',body,
  headers:ct?{'Content-Type':ct}:{}});await refresh();await loadLog()}
  catch(e){alert(e.message)}}
async function refresh(){
  try{const s=await j('/api/status');
    $('sub').innerHTML=`${s.name} v${s.version} &middot; ${s.ip} &middot; battery ${s.battery}% &middot; `+
      (s.running?`running <span class="ok">${s.running}</span>`:'launcher');
    $('wifi').textContent=s.ssid
      ?`${s.status}: ${s.ssid}${s.enterprise?' [enterprise]':''} `+
       `(${s.rssi} dBm, channel ${s.channel})`
      :`wi-fi ${s.status}`;
    const a=await j('/api/apps');
    $('apps').innerHTML=a.apps.length?a.apps.map(x=>
      `<div class="app"><div><b>${x.name}</b><small>${x.id} &middot; ${x.bytes} B`+
      `${x.version?' &middot; v'+x.version:''}</small></div><div class="row">`+
      `<button class="ghost" onclick="post('/api/run?id=${x.id}')">Run</button>`+
      `<button class="danger" onclick="del('${x.id}')">Delete</button></div></div>`).join('')
      :'<div class="sub">Nothing installed yet.</div>';
  }catch(e){$('sub').innerHTML='<span class="err">'+e.message+'</span>'}
  loadBroker()}
async function loadBroker(){
  try{const i=await j('/api/identity');
    $('bid').value=i.badge_id||'(not ready)';$('bsrc').value=i.source;$('bpk').value=i.pubkey;
    const b=await j('/api/broker');
    // Never overwrite what is being typed.
    if(document.activeElement!==$('burl'))$('burl').value=b.url;
    $('bstat').innerHTML=`${b.enabled?'enabled':'disabled'} &middot; ${b.state}`+
      (b.registered?' &middot; <span class="ok">registered</span>':'')+
      (b.error?' &middot; <span class="err">'+b.error+'</span>':'');
  }catch(e){$('bstat').innerHTML='<span class="err">'+e.message+'</span>'}}
async function saveBroker(on){
  try{await j('/api/broker',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({url:$('burl').value.trim(),enabled:on})});await loadBroker()}
  catch(e){alert(e.message)}}
async function del(id){
  if(!confirm('Delete '+id+'?'))return;
  try{await j('/api/app?id='+encodeURIComponent(id),{method:'DELETE'});await refresh()}
  catch(e){alert(e.message)}}
function pickFile(){const f=$('file').files[0];if(!f)return;$('path').value=f.name;
  f.text().then(t=>$('src').value=t)}
async function scanWifi(){
  $('wifi').textContent='scanning...';
  try{await j('/api/wifi/scan',{method:'POST'});
    for(let i=0;i<12;i++){await new Promise(r=>setTimeout(r,900));
      const s=await j('/api/wifi');
      if(!s.scanning){$('nets').innerHTML=s.networks.map(n=>
        `<option value="${n.ssid}">${n.rssi} dBm${n.encrypted?' (secured)':''}</option>`).join('');
        $('wifi').textContent=`${s.networks.length} network(s) found`;return}}
    $('wifi').textContent='scan timed out'}
  catch(e){$('wifi').textContent=e.message}}
function secChanged(){const e=$('sec').value==='eap';
  $('eapbox').style.display=e?'':'none';$('pskbox').style.display=e?'none':'';
  if(e)loadCerts()}
function defconPreset(){
  $('ssid').value='DefCon';$('sec').value='eap';secChanged();
  $('eapm').value='peap';$('ph2').value='mschapv2';$('edom').value='wifireg.defcon.org';
  $('wifi').textContent='DEF CON preset: PEAP/MSCHAPv2. Add your wifireg credentials '+
    'and upload this year’s CA certificate.'}
async function loadCerts(){
  try{const c=await j('/api/certs');const cur=$('eca').value;
    $('eca').innerHTML='<option value="">none — server NOT validated</option>'+
      c.certs.map(x=>`<option value="${x.name}">${x.name} (${x.bytes} B)</option>`).join('');
    $('eca').value=cur}catch(e){}}
async function uploadCert(){
  const f=$('cafile').files[0];if(!f)return;
  const name=f.name.toLowerCase().replace(/[^a-z0-9._-]/g,'-');
  try{const text=await f.text();
    await j('/api/certs?name='+encodeURIComponent(name),
      {method:'POST',body:text,headers:{'Content-Type':'text/plain'}});
    await loadCerts();$('eca').value=name;
    $('wifi').textContent='certificate '+name+' stored'}
  catch(e){alert(e.message)}}
async function joinWifi(){
  const ssid=$('ssid').value.trim();if(!ssid)return alert('pick a network');
  let body;
  if($('sec').value==='eap'){
    if(!$('euser').value)return alert('enterprise needs a username');
    if(!$('eca').value&&!confirm(
      'No CA certificate selected.\n\nThe badge cannot verify it is talking to the real '+
      'network, and your credentials go to whoever answers. Continue anyway?'))return;
    body={ssid,security:'eap',eap_method:$('eapm').value,phase2:$('ph2').value,
      username:$('euser').value,password:$('epass').value,
      identity:$('eident').value,domain:$('edom').value,ca:$('eca').value};
  } else {
    body={ssid,password:$('pass').value};
  }
  try{await j('/api/wifi',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify(body)});
    $('wifi').textContent='joining '+ssid+'...'}
  catch(e){alert(e.message)}}
async function push(run){
  const id=$('id').value.trim(),p=$('path').value.trim();
  if(!id||!p)return alert('id and file are required');
  try{await j(`/api/app?id=${id}&path=${encodeURIComponent(p)}`,
      {method:'POST',body:$('src').value,headers:{'Content-Type':'text/plain'}});
    if(run)await j('/api/run?id='+id,{method:'POST'});
    await refresh();await loadLog()}catch(e){alert(e.message)}}
async function loadLog(){try{const l=await j('/api/logs');$('log').textContent=l.lines.join('\n')}
  catch(e){$('log').textContent=e.message}}
refresh();loadLog();setInterval(refresh,5000);
</script></body></html>)HTML";

// ---------------------------------------------------------------------------

String jsonEscape(const String &input) {
  String out;
  out.reserve(input.length() + 8);
  for (size_t i = 0; i < input.length(); ++i) {
    const char c = input[i];
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if ((unsigned char)c < 0x20) {
          char escape[7];
          snprintf(escape, sizeof(escape), "\\u%04x", c);
          out += escape;
        } else {
          out += c;
        }
    }
  }
  return out;
}

void sendJson(int code, const String &body) { sServer.send(code, "application/json", body); }
void sendError(int code, const char *message) {
  sendJson(code, String("{\"error\":\"") + message + "\"}");
}

// Brute-force throttling for the six-digit pairing code. Local constants
// because config.h is owned by another module; a follow-up could move these
// there. A 6-digit code is only ~20 bits, so an unthrottled attacker guesses it
// in seconds - the lockout turns that into hours.
constexpr int AUTH_FAIL_THRESHOLD = 5;      // wrong codes before lockout
constexpr uint32_t AUTH_LOCK_MS = 30000;    // base lockout window
constexpr uint32_t AUTH_LOCK_MAX_MS = 300000;  // ceiling on the growing backoff
int sAuthFailCount = 0;
uint32_t sAuthLockUntil = 0;

// Constant-time string comparison so the pairing code cannot be recovered a
// character at a time from response timing. Length is allowed to leak (the code
// is always six digits), the contents are not.
bool constantTimeEquals(const String &a, const String &b) {
  if (a.length() != b.length()) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < a.length(); ++i) {
    diff |= (uint8_t)a[i] ^ (uint8_t)b[i];
  }
  return diff == 0;
}

bool authLockedOut() { return (int32_t)(millis() - sAuthLockUntil) < 0; }

void noteAuthFailure() {
  if (++sAuthFailCount < AUTH_FAIL_THRESHOLD) return;
  // Grow the lockout with the number of failures past the threshold, capped.
  uint32_t window = AUTH_LOCK_MS * (uint32_t)(sAuthFailCount - AUTH_FAIL_THRESHOLD + 1);
  if (window > AUTH_LOCK_MAX_MS) window = AUTH_LOCK_MAX_MS;
  sAuthLockUntil = millis() + window;
  badge_log::tagf("push", "pairing lockout: %d failures, blocked for %us", sAuthFailCount,
                  (unsigned)(window / 1000));
}

// Checks the presented token without touching the throttle counters. Sets
// `provided` true only when a NON-EMPTY token was sent, so the browser UI's
// standing X-Badge-Token header (empty until the wearer types a code, then
// possibly a partial code) never registers as a brute-force attempt.
// `allowQueryToken` gates the ?token= query parameter: accepted on read-only
// GET routes for convenience, never on writes (a query token rides along on a
// cross-site link, and text/plain writes are otherwise CORS-simple).
bool tokenPresented(bool allowQueryToken, bool &provided) {
  provided = false;
  const String expected = settings::pairingCode();
  if (sServer.hasHeader("X-Badge-Token")) {
    const String t = sServer.header("X-Badge-Token");
    if (t.length()) {
      provided = true;
      if (constantTimeEquals(t, expected)) return true;
    }
  }
  if (allowQueryToken && sServer.hasArg("token")) {
    const String t = sServer.arg("token");
    if (t.length()) {
      provided = true;
      if (constantTimeEquals(t, expected)) return true;
    }
  }
  return false;
}

// Read-path auth. Deliberately does NOT enforce or advance the lockout: reads
// are harmless and, crucially, the UI polls /api/status every few seconds, so
// counting those would lock the wearer out mid-typing. Throttling lives on the
// write path, where it belongs.
bool authorised(bool allowQueryToken) {
  if (!settings::pushRequiresPairing()) return true;
  bool provided;
  return tokenPresented(allowQueryToken, provided);
}

// Write-path auth: header-only token, plus brute-force throttling. This is the
// only place that advances the lockout, and it is reached only from writeGuard()
// (i.e. after the Host/Origin checks), so a state-changing request is what an
// attacker must send to burn an attempt.
bool authorisedWrite() {
  if (!settings::pushRequiresPairing()) return true;
  if (authLockedOut()) return false;
  bool provided;
  if (tokenPresented(/*allowQueryToken=*/false, provided)) {
    sAuthFailCount = 0;
    return true;
  }
  if (provided) noteAuthFailure();
  return false;
}

bool requireAuth(bool allowQueryToken = true) {
  if (authorised(allowQueryToken)) return true;
  sendError(401, "pairing code required");
  return false;
}

// Lowercases and strips a scheme prefix and any ":port" so a Host or Origin
// header can be compared against the badge's own names.
String hostPart(const String &value) {
  String out = value;
  const int scheme = out.indexOf("://");
  if (scheme >= 0) out = out.substring(scheme + 3);
  const int slash = out.indexOf('/');
  if (slash >= 0) out = out.substring(0, slash);
  const int colon = out.indexOf(':');
  if (colon >= 0) out = out.substring(0, colon);
  out.toLowerCase();
  return out;
}

// True when `host` names this badge. Used to defeat DNS rebinding: an attacker
// page that rebinds its own domain to the badge's IP still sends the attacker's
// Host, which is not in this set.
bool isOwnHost(const String &host) {
  if (host.length() == 0) return true;  // HTTP/1.0 with no Host - not a browser rebind
  if (host == "localhost" || host == "127.0.0.1") return true;
  if (host == wifi_mgr::ip().toString()) return true;
  String name = settings::deviceName();
  name.toLowerCase();
  if (host == name || host == name + ".local") return true;
  if (host == String(DEFAULT_HOSTNAME) || host == String(DEFAULT_HOSTNAME) + ".local") return true;
  return false;
}

// Guard for every state-changing route. Order matters: the Host/Origin checks
// run before auth so a rebinding page is rejected even when pairing is off.
//
//   1. Host allowlist  - defeats DNS rebinding (the classic "install code from a
//      web page the user merely visited" attack).
//   2. Same-origin     - a cross-origin browser page may not drive writes.
//   3. Custom header    - requiring X-Badge-Token makes the request non-simple,
//      so a cross-origin browser must preflight, which we no longer wave through.
//   4. Header-only auth - the pairing code, never from the query string.
bool writeGuard() {
  if (!isOwnHost(hostPart(sServer.hostHeader()))) {
    sendError(403, "bad host");
    return false;
  }
  if (sServer.hasHeader("Origin") && !isOwnHost(hostPart(sServer.header("Origin")))) {
    sendError(403, "cross-origin request refused");
    return false;
  }
  if (!sServer.hasHeader("X-Badge-Token")) {
    sendError(403, "X-Badge-Token header required");
    return false;
  }
  if (authorisedWrite()) return true;
  if (authLockedOut()) {
    sendError(429, "too many attempts - locked out");
  } else {
    sendError(401, "pairing code required");
  }
  return false;
}

// -- Handlers ----------------------------------------------------------------

void handleIndex() {
  if (sIndexOverride.length() > 0) {
    sServer.send(200, "text/html", sIndexOverride);
    return;
  }
  sServer.send_P(200, "text/html", INDEX_HTML);
}

void handleStatus() {
  // /api/status is intentionally reachable without pairing so the UI can show
  // "connecting..." before a code is entered. But it must not double as an
  // oracle: the `auth` field is gone (it told an unauthenticated caller whether
  // a code even mattered), and the fields that identify the network and the
  // wearer's activity (device name, SSID, IP, running app) are emitted only to
  // an authenticated caller. Everything left is generic hardware status.
  const bool authed = authorised(/*allowQueryToken=*/true);

  String json = "{";
  json += "\"name\":\"" SOLANA_OS_NAME "\",";
  json += "\"version\":\"" SOLANA_OS_VERSION "\",";
  json += "\"api\":" + String(SOLANA_OS_API_VERSION) + ",";
  json += "\"espnow\":" + String(espnow_mgr::enabled() ? "true" : "false") + ",";
  json += "\"peers\":" + String((unsigned)espnow_mgr::peerCount()) + ",";
  json += "\"battery\":" + String((int)power::percent()) + ",";
  json += "\"volts\":" + String(power::volts(), 2) + ",";
  json += "\"heap\":" + String((unsigned)ESP.getFreeHeap()) + ",";
  json += "\"psram\":" + String((unsigned)ESP.getFreePsram()) + ",";
  json += "\"fs_total\":" + String((unsigned)app_store::totalBytes()) + ",";
  json += "\"fs_used\":" + String((unsigned)app_store::usedBytes()) + ",";
  json += "\"channel\":" + String(wifi_mgr::channel()) + ",";
  json += "\"status\":\"" + String(wifi_mgr::statusText()) + "\",";
  json += "\"uptime\":" + String((unsigned)(millis() / 1000));
  if (authed) {
    json += ",\"device\":\"" + jsonEscape(settings::deviceName()) + "\",";
    json += "\"ip\":\"" + wifi_mgr::ip().toString() + "\",";
    json += "\"ssid\":\"" + jsonEscape(wifi_mgr::ssid()) + "\",";
    json += "\"rssi\":" + String(wifi_mgr::rssi()) + ",";
    json += "\"running\":\"" + jsonEscape(runtime::currentApp()) + "\"";
  }
  json += "}";
  sendJson(200, json);
}

void handleListApps() {
  if (!requireAuth()) return;
  String json = "{\"apps\":[";
  app_store::Info info;
  for (size_t i = 0; i < app_store::count(); ++i) {
    if (!app_store::at(i, info)) continue;
    if (i) json += ",";
    json += "{\"id\":\"" + jsonEscape(info.id) + "\",";
    json += "\"name\":\"" + jsonEscape(info.name) + "\",";
    json += "\"version\":\"" + jsonEscape(info.version) + "\",";
    json += "\"author\":\"" + jsonEscape(info.author) + "\",";
    json += "\"description\":\"" + jsonEscape(info.description) + "\",";
    json += "\"entry\":\"" + jsonEscape(info.entry) + "\",";
    json += "\"bytes\":" + String((unsigned)info.sizeBytes) + "}";
  }
  json += "]}";
  sendJson(200, json);
}

void handleWriteApp() {
  if (!writeGuard()) return;

  const String id = sServer.arg("id");
  String path = sServer.arg("path");
  if (path.length() == 0) path = APP_ENTRY;
  const bool append = sServer.arg("append") == "1";
  const bool isBase64 = sServer.arg("enc") == "base64";

  if (!app_store::isValidId(id)) {
    sendError(400, "id must be [a-z0-9._-], 1-32 chars");
    return;
  }

  // NOTE: WebServer has already buffered the whole request body into the "plain"
  // arg by the time this handler runs (a second copy on top of the socket
  // buffer), so the earliest we can reject an oversized upload without editing
  // the library is here. Content-Length is checked too, so a request that
  // *claims* to be huge is refused before we even look at the buffered body.
  if (sServer.clientContentLength() > (int)PUSH_MAX_FILE_BYTES) {
    sendError(413, "file too large");
    return;
  }

  const String body = sServer.arg("plain");
  if (body.length() > PUSH_MAX_FILE_BYTES) {
    sendError(413, "file too large");
    return;
  }

  bool ok;
  size_t written;
  if (isBase64) {
    const size_t capacity = (body.length() / 4 + 1) * 3 + 4;
    uint8_t *buffer = (uint8_t *)malloc(capacity);
    if (buffer == nullptr) {
      sendError(507, "out of memory");
      return;
    }
    size_t decoded = 0;
    const int result = mbedtls_base64_decode(buffer, capacity, &decoded,
                                             (const unsigned char *)body.c_str(), body.length());
    if (result != 0) {
      free(buffer);
      sendError(400, "invalid base64");
      return;
    }
    if (decoded > PUSH_MAX_FILE_BYTES) {
      free(buffer);
      sendError(413, "file too large");
      return;
    }
    ok = app_store::writeFile(id, path, buffer, decoded, append);
    written = decoded;
    free(buffer);  // intermediate buffer freed promptly, before responding
  } else {
    ok = app_store::writeFile(id, path, (const uint8_t *)body.c_str(), body.length(), append);
    written = body.length();
  }

  if (!ok) {
    sendError(500, "write failed");
    return;
  }

  app_store::refresh();
  leds::pulse(0x14, 0xF1, 0x95, 600);
  badge_log::tagf("push", "http wrote %s/%s (%u bytes)", id.c_str(), path.c_str(),
                  (unsigned)written);
  sendJson(200, "{\"ok\":true,\"bytes\":" + String((unsigned)written) + "}");
}

void handleDeleteApp() {
  if (!writeGuard()) return;
  const String id = sServer.arg("id");
  if (!app_store::exists(id)) {
    sendError(404, "no such app");
    return;
  }
  // Immediate, not deferred: removeApp() runs on the next line and must not
  // delete files out from under a live VM. Handlers run from the main loop with
  // no Lua frame on the stack, so tearing the state down here is safe.
  if (runtime::currentApp() == id) runtime::stop();
  sendJson(200, app_store::removeApp(id) ? "{\"ok\":true}" : "{\"error\":\"delete failed\"}");
}

void handleRun() {
  if (!writeGuard()) return;
  const String id = sServer.arg("id");
  if (!app_store::exists(id)) {
    sendError(404, "no such app");
    return;
  }
  runtime::requestLaunch(id);
  sendJson(200, "{\"ok\":true}");
}

void handleStop() {
  if (!writeGuard()) return;
  runtime::requestStop();
  sendJson(200, "{\"ok\":true}");
}

void handleLogs() {
  if (!requireAuth()) return;
  String json = "{\"lines\":[";
  const size_t total = badge_log::lineCount();
  for (size_t i = 0; i < total; ++i) {
    if (i) json += ",";
    json += "\"" + jsonEscape(badge_log::line(i)) + "\"";
  }
  json += "]}";
  sendJson(200, json);
}

// The badge has no keyboard, so a WPA passphrase cannot be entered on the
// device. Starting the hotspot and using this endpoint from a phone is the
// supported way to put the badge on a secured network.
void handleWifiStatus() {
  if (!requireAuth()) return;
  String json = "{";
  json += "\"status\":\"" + String(wifi_mgr::statusText()) + "\",";
  json += "\"connected\":" + String(wifi_mgr::connected() ? "true" : "false") + ",";
  json += "\"ssid\":\"" + jsonEscape(wifi_mgr::ssid()) + "\",";
  json += "\"ip\":\"" + wifi_mgr::ip().toString() + "\",";
  json += "\"enterprise\":" + String(wifi_mgr::usingEnterprise() ? "true" : "false") + ",";
  json += "\"saved_ssid\":\"" + jsonEscape(settings::wifiSsid()) + "\",";
  json += "\"scanning\":" + String(wifi_mgr::scanning() ? "true" : "false") + ",";
  json += "\"networks\":[";
  const int count = wifi_mgr::scanResultCount();
  for (int i = 0; i < count; ++i) {
    if (i) json += ",";
    json += "{\"ssid\":\"" + jsonEscape(wifi_mgr::scanSsid(i)) + "\",";
    json += "\"rssi\":" + String(wifi_mgr::scanRssi(i)) + ",";
    json += "\"encrypted\":" + String(wifi_mgr::scanEncrypted(i) ? "true" : "false") + "}";
  }
  json += "]}";
  sendJson(200, json);
}

void handleWifiScan() {
  if (!writeGuard()) return;
  sendJson(200, wifi_mgr::startScan() ? "{\"ok\":true}" : "{\"error\":\"scan failed\"}");
}

// Pulls one string field out of a flat JSON object. Not a parser: this endpoint
// takes a handful of string fields and nothing nested, so a real JSON
// dependency would not earn its place.
// Finds the offset of a key's `"..."` token, but only where it is actually a
// key: the quoted string must be immediately followed (modulo whitespace) by a
// ':'. Without this a *value* that happens to contain the key text - e.g.
// {"password":"\"ssid\":\"evil\""} - could shadow a real field. Returns the
// index of the value's opening context (the ':'), or -1.
int findKeyColon(const String &body, const char *key) {
  const String needle = String("\"") + key + "\"";
  int from = 0;
  while (true) {
    const int at = body.indexOf(needle, from);
    if (at < 0) return -1;
    int i = at + (int)needle.length();
    while (i < (int)body.length() &&
           (body[i] == ' ' || body[i] == '\t' || body[i] == '\n' || body[i] == '\r')) {
      ++i;
    }
    if (i < (int)body.length() && body[i] == ':') return i;
    from = at + 1;  // this occurrence was a value, keep looking for the key
  }
}

String jsonField(const String &body, const char *key) {
  const int colon = findKeyColon(body, key);
  if (colon < 0) return String();
  const int open = body.indexOf('"', colon);
  if (open < 0) return String();

  String out;
  for (int i = open + 1; i < (int)body.length(); ++i) {
    const char c = body[i];
    if (c == '\\' && i + 1 < (int)body.length()) {
      out += body[++i];
      continue;
    }
    if (c == '"') break;
    out += c;
  }
  return out;
}

void handleWifiJoin() {
  if (!writeGuard()) return;

  const String body = sServer.arg("plain");
  const String ssid = jsonField(body, "ssid");
  if (ssid.length() == 0) {
    sendError(400, "ssid required");
    return;
  }

  const String security = jsonField(body, "security");
  if (security == "eap" || security == "enterprise") {
    wifi_mgr::Enterprise config;
    config.method = wifi_mgr::eapFromString(jsonField(body, "eap_method"));
    config.identity = jsonField(body, "identity");
    config.username = jsonField(body, "username");
    config.password = jsonField(body, "password");
    config.caCertName = jsonField(body, "ca");
    config.domain = jsonField(body, "domain");
    const String phase2 = jsonField(body, "phase2");
    if (phase2.length()) config.ttlsPhase2 = phase2;
    // The web UI pops a confirmation dialog before sending a no-CA enterprise
    // join, so reaching here with an empty CA is a deliberate operator choice.
    // Opting in explicitly keeps wifi_mgr from failing the (validated-by-default)
    // path closed while still refusing silent downgrades from other callers.
    config.allowNoCa = (config.caCertName.length() == 0);

    if (!config.valid()) {
      sendError(400, "username required for PEAP/TTLS");
      return;
    }
    if (config.caCertName.length() && !cert_store::exists(config.caCertName)) {
      sendError(400, "no such certificate - upload it to /api/certs first");
      return;
    }

    // Answered before associating: joining tears down this TCP connection the
    // moment the badge leaves its current network, so a reply sent afterwards
    // never arrives.
    sendJson(200, "{\"ok\":true}");
    delay(120);
    wifi_mgr::connectEnterprise(ssid, config, true);
    return;
  }

  sendJson(200, "{\"ok\":true}");
  delay(120);
  wifi_mgr::connect(ssid, jsonField(body, "password"), true);
}

// -- Identity and the app-store broker ---------------------------------------
//
// Both are read-only-ish status plus, for the broker, the one field that cannot
// be typed on the badge. Same reasoning as the Wi-Fi endpoints above: no
// keyboard, so the broker address has to arrive from a phone.

// The unquoted sibling of jsonField(), for `"enabled": true`. Whitespace after
// the colon is legal JSON and JSON.stringify() does not emit it, but a curl by
// hand will.
bool jsonBool(const String &body, const char *key, bool fallback) {
  int at = findKeyColon(body, key);
  if (at < 0) return fallback;
  // Skip the colon and all JSON whitespace after it.
  ++at;
  while (at < (int)body.length() &&
         (body[at] == ' ' || body[at] == '\t' || body[at] == '\n' || body[at] == '\r')) {
    ++at;
  }
  // Match a real boolean token, not just a "true" prefix of some longer word.
  if (body.startsWith("true", at)) {
    const int after = at + 4;
    const char c = after < (int)body.length() ? body[after] : '\0';
    const bool wordChar = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '_';
    if (!wordChar) return true;
  }
  if (body.startsWith("false", at)) return false;
  return fallback;
}

void handleIdentity() {
  if (!requireAuth()) return;
  String json = "{";
  json += "\"ready\":" + String(identity::ready() ? "true" : "false") + ",";
  json += "\"badge_id\":\"" + jsonEscape(identity::badgeId()) + "\",";
  json += "\"pubkey\":\"" + jsonEscape(identity::publicKeyBase58()) + "\",";
  json += "\"source\":\"" + String(identity::sourceName()) + "\",";
  json += "\"status\":\"" + jsonEscape(identity::status()) + "\"";
  json += "}";
  sendJson(200, json);
}

void handleBrokerStatus() {
  if (!requireAuth()) return;
  String json = "{";
  json += "\"enabled\":" + String(broker::enabled() ? "true" : "false") + ",";
  json += "\"url\":\"" + jsonEscape(broker::url()) + "\",";
  json += "\"state\":\"" + String(broker::stateText()) + "\",";
  json += "\"registered\":" + String(broker::registered() ? "true" : "false") + ",";
  json += "\"error\":\"" + jsonEscape(broker::lastError()) + "\",";
  json += "\"offer\":" + String(broker::hasOffer() ? "true" : "false");
  json += "}";
  sendJson(200, json);
}

void handleBrokerConfigure() {
  if (!writeGuard()) return;

  const String body = sServer.arg("plain");
  // Absent means "leave it alone" for both fields, so a page can flip the
  // toggle without having to echo the address back.
  if (body.indexOf("\"url\"") >= 0) {
    const String url = jsonField(body, "url");
    if (url.length() && !url.startsWith("http://") && !url.startsWith("https://")) {
      sendError(400, "url must start with http:// or https://");
      return;
    }
    // Reject control characters and whitespace: the URL is later concatenated
    // into request lines by broker_client, so a CR/LF or space here would be a
    // header/request-injection vector.
    for (size_t i = 0; i < url.length(); ++i) {
      const unsigned char c = (unsigned char)url[i];
      if (c <= 0x20 || c == 0x7F) {
        sendError(400, "url contains illegal whitespace or control characters");
        return;
      }
    }
    // setUrl() drops the stored token when the address changes: a bearer token
    // only means anything to the broker that issued it.
    broker::setUrl(url);
  }
  if (body.indexOf("\"enabled\"") >= 0) {
    broker::setEnabled(jsonBool(body, "enabled", broker::enabled()));
  }

  badge_log::tagf("broker", "configured over http: %s, %s",
                  broker::url().length() ? broker::url().c_str() : "no address",
                  broker::enabled() ? "enabled" : "disabled");
  handleBrokerStatus();
}

// -- Certificates ------------------------------------------------------------

void handleListCerts() {
  if (!requireAuth()) return;
  String json = "{\"certs\":[";
  String name;
  for (size_t i = 0; i < cert_store::count(); ++i) {
    if (!cert_store::nameAt(i, name)) continue;
    if (i) json += ",";
    json += "{\"name\":\"" + jsonEscape(name) + "\",\"bytes\":" +
            String((unsigned)cert_store::sizeOf(name)) + "}";
  }
  json += "]}";
  sendJson(200, json);
}

void handleWriteCert() {
  if (!writeGuard()) return;

  const String name = sServer.arg("name");
  if (!cert_store::isValidName(name)) {
    sendError(400, "name must be [a-z0-9._-], 1-40 chars");
    return;
  }

  const String body = sServer.arg("plain");
  if (body.length() == 0) {
    sendError(400, "empty body");
    return;
  }
  if (body.length() > cert_store::MAX_CERT_BYTES) {
    sendError(413, "certificate too large");
    return;
  }

  if (!cert_store::write(name, (const uint8_t *)body.c_str(), body.length())) {
    sendError(400, "not a PEM certificate (convert DER with `openssl x509 -inform der`)");
    return;
  }
  sendJson(200, "{\"ok\":true,\"bytes\":" + String((unsigned)body.length()) + "}");
}

void handleDeleteCert() {
  if (!writeGuard()) return;
  const String name = sServer.arg("name");
  if (!cert_store::exists(name)) {
    sendError(404, "no such certificate");
    return;
  }
  sendJson(200, cert_store::remove(name) ? "{\"ok\":true}" : "{\"error\":\"delete failed\"}");
}

void handleReboot() {
  if (!writeGuard()) return;
  sendJson(200, "{\"ok\":true}");
  delay(200);
  ESP.restart();
}

void handleNotFound() {
  // We do NOT emit permissive CORS headers here. Answering OPTIONS with a bare
  // 204 (and no Access-Control-Allow-* headers) means a cross-origin preflight
  // gets no grant, so the browser refuses the follow-up write - which is the
  // behaviour we want. Same-origin requests never preflight and are unaffected.
  if (sServer.method() == HTTP_OPTIONS) {
    if (!isOwnHost(hostPart(sServer.hostHeader()))) {
      sendError(403, "bad host");
      return;
    }
    sServer.send(204);
    return;
  }
  sendError(404, "not found");
}

}  // namespace

void begin() {
  if (sRunning) return;

  sServer.on("/", HTTP_GET, handleIndex);
  sServer.on("/index.html", HTTP_GET, handleIndex);
  sServer.on("/api/status", HTTP_GET, handleStatus);
  sServer.on("/api/apps", HTTP_GET, handleListApps);
  sServer.on("/api/app", HTTP_POST, handleWriteApp);
  sServer.on("/api/app", HTTP_PUT, handleWriteApp);
  sServer.on("/api/app", HTTP_DELETE, handleDeleteApp);
  sServer.on("/api/run", HTTP_POST, handleRun);
  sServer.on("/api/stop", HTTP_POST, handleStop);
  sServer.on("/api/wifi", HTTP_GET, handleWifiStatus);
  sServer.on("/api/wifi", HTTP_POST, handleWifiJoin);
  sServer.on("/api/wifi/scan", HTTP_POST, handleWifiScan);
  sServer.on("/api/certs", HTTP_GET, handleListCerts);
  sServer.on("/api/certs", HTTP_POST, handleWriteCert);
  sServer.on("/api/certs", HTTP_PUT, handleWriteCert);
  sServer.on("/api/certs", HTTP_DELETE, handleDeleteCert);
  // Registered twice: /api/... matches the rest of this API, /api/v1/... is
  // where a page written against broker/PROTOCOL.md's versioned paths looks.
  sServer.on("/api/identity", HTTP_GET, handleIdentity);
  sServer.on("/api/v1/identity", HTTP_GET, handleIdentity);
  sServer.on("/api/broker", HTTP_GET, handleBrokerStatus);
  sServer.on("/api/v1/broker", HTTP_GET, handleBrokerStatus);
  sServer.on("/api/broker", HTTP_POST, handleBrokerConfigure);
  sServer.on("/api/v1/broker", HTTP_POST, handleBrokerConfigure);
  sServer.on("/api/logs", HTTP_GET, handleLogs);
  sServer.on("/api/reboot", HTTP_POST, handleReboot);
  sServer.onNotFound(handleNotFound);

  // WebServer drops headers it was not told to keep. Origin feeds the
  // same-origin check in writeGuard(); the Host header and Content-Length have
  // dedicated accessors (hostHeader(), clientContentLength()) and need no
  // collection.
  const char *headers[] = {"X-Badge-Token", "Content-Type", "Origin"};
  sServer.collectHeaders(headers, 3);

  // Deliberately NOT enableCORS(true): that emits Access-Control-Allow-Origin:*,
  // which would let any web page on the internet read the badge's API and,
  // together with the token, drive it. The API is same-origin only; the browser
  // UI is served from the badge itself so it needs no cross-origin grant.
  sServer.begin();
  sRunning = true;

  const String host = settings::deviceName();
  sMdnsUp = MDNS.begin(host.c_str());
  if (sMdnsUp) MDNS.addService("http", "tcp", PUSH_SERVER_PORT);

  badge_log::tagf("push", "http up on port %u%s", (unsigned)PUSH_SERVER_PORT,
                  sMdnsUp ? " (mDNS ok)" : "");
}

void stop() {
  if (!sRunning) return;
  sServer.stop();
  if (sMdnsUp) {
    MDNS.end();
    sMdnsUp = false;
  }
  sRunning = false;
  badge_log::tagf("push", "http down");
}

bool running() { return sRunning; }

void update() {
  if (sRunning) sServer.handleClient();
}

void setIndexOverride(const String &html) { sIndexOverride = html; }
void clearIndexOverride() { sIndexOverride = ""; }

}  // namespace push_server
