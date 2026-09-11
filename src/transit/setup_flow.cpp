// First-run on-device provisioning (see include/transit/setup_flow.h).
//
// Text entry happens off-device, in a captive-portal web page: the X4 boots
// a fixed-SSID Wi-Fi AP ("TransitBoard-Setup") alongside its STA interface
// (WIFI_AP_STA — the AP stays up for the whole flow), a DNSServer answers
// every DNS query with the AP's own IP so phones auto-prompt "Sign in to
// network," and a WebServer serves one inline-HTML/JS page plus JSON
// endpoints the page's JS calls. All on-device text (the phone can't see
// the panel) goes through RenderEngine::renderSetupPrompt (unit 5) — just
// the handful of top-level instructions/status a phone user can't see yet;
// list-picking (networks, stops) now happens in the browser, not on the
// e-ink screen, so renderSetupList is unused here.
//
// Each step writes to configStore_ the moment it's confirmed, and every
// step first checks whether configStore_ already has a value (i.e. this is
// a resume after an interrupted setup) before asking again — see the
// header comment on runFirstTimeSetup for why.
//
// No HTTPS/auth on the setup AP: it's a fresh AP the device itself just
// created, reachable only by someone physically close enough to join it —
// that proximity requirement is the access control, the same reasoning
// WiFiManager-style local provisioning portals rely on generally. Adding a
// self-signed TLS cert to a device serving its own first-boot setup page
// would not meaningfully improve on that.

#include "transit/setup_flow.h"

#include <ArduinoJson.h>
#include <Arduino.h>
#include <WiFi.h>

#include "transit/sta_models.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <string>
#include <vector>

namespace transit {

namespace {

constexpr const char* kApSsid = "TransitBoard-Setup";
constexpr uint16_t kDnsPort = 53;
constexpr uint16_t kHttpPort = 80;

// Per-attempt Wi-Fi connect timeout (STA only — the AP interface never
// drops). Mirrors the previous BLE-flow's tryConnectWifi timeout.
constexpr uint32_t kWifiConnectTimeoutMs = 20000;

// Guards against a portal left unattended forever (e.g. the user closed the
// browser tab and walked away) draining the battery instead of resuming on
// the next wake. Reset by touchActivity() on every HTTP request handled.
constexpr uint32_t kSetupIdleTimeoutMs = 20UL * 60UL * 1000UL;

// Once configStore_.isProvisioned() goes true, keep the portal up briefly
// so the page's in-flight/next status poll can render the "done" screen
// before the AP disappears out from under it.
constexpr uint32_t kProvisionedLingerMs = 5000;

// Fixed placeholder coordinate purely to exercise the API for key
// validation — the device has no GPS. Same point root CLAUDE.md's own curl
// validation example uses (step 4), not a real location.
constexpr double kKeyCheckLat = 45.5017;
constexpr double kKeyCheckLon = -73.5673;

// No GPS on the X4 (docs/CONFIG_AND_STATE.md), so the user supplies an
// approximate search-area center themselves (looked up once off-device, in
// any maps app) rather than relying on a live radius search every poll —
// now typed into a web form field instead of typed via a paired keyboard.
bool parseDouble(const std::string& text, double& out) {
  if (text.empty()) return false;
  const char* start = text.c_str();
  char* end = nullptr;
  double value = strtod(start, &end);
  if (end == start || *end != '\0') return false;
  out = value;
  return true;
}

// The setup page, served as one inline document (no SPIFFS/filesystem
// pipeline needed for a simple form-based UI). Kept minimal/functional
// rather than fancy. JS drives four steps by showing/hiding <section>s and
// polling/posting the JSON endpoints registered in SetupFlow::startPortal.
const char kSetupPageHtml[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Transit Board Setup</title>
<style>
body{font-family:system-ui,sans-serif;max-width:480px;margin:1.5em auto;padding:0 1em;color:#222}
h1{font-size:1.2em}
section{display:none;margin-bottom:1.5em}
section.active{display:block}
label{display:block;margin:.8em 0 .2em}
input,select{width:100%;box-sizing:border-box;padding:.5em;font-size:1em}
button{margin-top:1em;padding:.6em 1.2em;font-size:1em}
ul{list-style:none;padding:0}
li{padding:.5em;border:1px solid #ccc;border-radius:4px;margin:.3em 0;cursor:pointer}
li:hover{background:#f0f0f0}
.msg{margin-top:.6em;font-size:.9em}
.msg.err{color:#b00}
.msg.ok{color:#0a0}
</style></head>
<body>
<h1>Transit Board Setup</h1>

<section id="step-wifi" class="active">
<p>Step 1 of 3 &mdash; Wi-Fi</p>
<label>Nearby networks</label>
<ul id="ssid-list"><li>Scanning&hellip;</li></ul>
<label>Network name (SSID)</label>
<input id="ssid" placeholder="Network name">
<label>Password</label>
<input id="password" type="password" placeholder="Leave blank if open">
<button id="connect-btn" onclick="connectWifi()">Connect</button>
<div id="wifi-msg" class="msg"></div>
</section>

<section id="step-apikey">
<p>Step 2 of 3 &mdash; Transit API key</p>
<label>API key</label>
<input id="apikey" type="password" placeholder="Paste your Transit API key">
<button onclick="submitApiKey()">Validate &amp; save</button>
<div id="apikey-msg" class="msg"></div>
</section>

<section id="step-stop">
<p>Step 3 of 3 &mdash; Find your stop</p>
<label>Approximate latitude</label>
<input id="lat" placeholder="e.g. 45.5017">
<label>Approximate longitude</label>
<input id="lon" placeholder="e.g. -73.5673">
<label>Stop name (part of it)</label>
<input id="query" placeholder="e.g. Main St">
<button onclick="searchStops()">Search</button>
<div id="stop-msg" class="msg"></div>
<ul id="stop-list"></ul>
</section>

<section id="step-done">
<h2>Setup complete</h2>
<p id="done-msg">Your board is ready. You can close this page.</p>
</section>

<script>
function el(id){return document.getElementById(id);}
function showStep(id){
  document.querySelectorAll('section').forEach(function(s){s.classList.remove('active');});
  el(id).classList.add('active');
}
function setMsg(id,text,cls){
  var m=el(id); m.textContent=text; m.className='msg'+(cls?(' '+cls):'');
}

function loadScan(){
  fetch('/scan').then(function(r){return r.json();}).then(function(list){
    var ul=el('ssid-list'); ul.innerHTML='';
    if(list.length===0){ ul.innerHTML='<li>No networks found &mdash; type one below.</li>'; return; }
    list.forEach(function(ssid){
      var li=document.createElement('li');
      li.textContent=ssid;
      li.onclick=function(){ el('ssid').value=ssid; };
      ul.appendChild(li);
    });
  }).catch(function(){ el('ssid-list').innerHTML='<li>Scan failed &mdash; type a network name below.</li>'; });
}

function connectWifi(){
  var ssid=el('ssid').value.trim();
  if(!ssid){ setMsg('wifi-msg','Enter or pick a network name first.','err'); return; }
  setMsg('wifi-msg','Connecting to "'+ssid+'"...','');
  var body='ssid='+encodeURIComponent(ssid)+'&password='+encodeURIComponent(el('password').value);
  fetch('/connect',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body})
    .then(function(r){return r.json().then(function(res){return {status:r.status,res:res};});})
    .then(function(result){
      if(result.status!==200 || !result.res.ok){ setMsg('wifi-msg',(result.res&&result.res.message)||'Could not connect. Try again.','err'); return; }
      pollStatus();
    })
    .catch(function(){ setMsg('wifi-msg','Could not reach the board. Try again.','err'); });
}

var statusTimer=null;
function pollStatus(){
  if(statusTimer) clearTimeout(statusTimer);
  fetch('/status').then(function(r){return r.json();}).then(function(s){
    if(s.wifiState==='connecting'){
      setMsg('wifi-msg','Connecting...','');
      statusTimer=setTimeout(pollStatus,1500);
    } else if(s.wifiState==='failed'){
      setMsg('wifi-msg','Could not connect. Check the password and try again.','err');
    } else if(s.wifiState==='connected' || s.apiKeySet || s.stopSet){
      if(s.wifiState==='connected'){ setMsg('wifi-msg','Connected.','ok'); }
      if(s.provisioned){
        showStep('step-done');
      } else if(s.apiKeySet){
        showStep('step-stop');
      } else {
        showStep('step-apikey');
      }
    }
  }).catch(function(){ statusTimer=setTimeout(pollStatus,2000); });
}

function submitApiKey(){
  var key=el('apikey').value.trim();
  if(!key){ setMsg('apikey-msg','Paste your API key first.','err'); return; }
  setMsg('apikey-msg','Checking key...','');
  fetch('/apikey',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'key='+encodeURIComponent(key)})
    .then(function(r){return r.json();}).then(function(res){
      if(res.ok){ setMsg('apikey-msg','Key accepted, ending in '+res.keySuffix+'.','ok'); showStep('step-stop'); }
      else { setMsg('apikey-msg',res.message||'Key rejected. Try again.','err'); }
    }).catch(function(){ setMsg('apikey-msg','Could not reach the board. Try again.','err'); });
}

function searchStops(){
  var lat=el('lat').value.trim(), lon=el('lon').value.trim(), query=el('query').value.trim();
  setMsg('stop-msg','Searching...','');
  el('stop-list').innerHTML='';
  var body='lat='+encodeURIComponent(lat)+'&lon='+encodeURIComponent(lon)+'&query='+encodeURIComponent(query);
  fetch('/stopsearch',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body})
    .then(function(r){return r.json();}).then(function(res){
      if(!res.ok){ setMsg('stop-msg',res.message||'No stops found.','err'); return; }
      setMsg('stop-msg','Pick your stop:','');
      var ul=el('stop-list');
      res.results.forEach(function(item){
        var li=document.createElement('li');
        li.textContent=item.name+(item.distanceMeters>0?(' ('+Math.round(item.distanceMeters)+'m)'):'');
        li.onclick=function(){ selectStop(item.index); };
        ul.appendChild(li);
      });
    }).catch(function(){ setMsg('stop-msg','Could not reach the board. Try again.','err'); });
}

function selectStop(index){
  setMsg('stop-msg','Saving...','');
  fetch('/stopselect',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'index='+encodeURIComponent(index)})
    .then(function(r){return r.json();}).then(function(res){
      if(res.ok){ el('done-msg').textContent='Stop saved: '+res.stopName+'. Your board is ready. You can close this page.'; showStep('step-done'); }
      else { setMsg('stop-msg',res.message||'Could not save that stop.','err'); }
    }).catch(function(){ setMsg('stop-msg','Could not reach the board. Try again.','err'); });
}

loadScan();
pollStatus();
</script>
</body></html>
)HTML";

// Settings-only page: served instead of kSetupPageHtml when portalMode_ ==
// kSettings (runSettingsPortal()). Deliberately much smaller than the
// first-run wizard above -- one setting today (display orientation), no
// Wi-Fi/API-key/stop steps to re-walk. Add future settings as additional
// <section>s + endpoints the same way, rather than growing this into a copy
// of the first-run wizard.
const char kSettingsPageHtml[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Transit Board Settings</title>
<style>
body{font-family:system-ui,sans-serif;max-width:480px;margin:1.5em auto;padding:0 1em;color:#222}
h1{font-size:1.2em}
section{display:none;margin-bottom:1.5em}
section.active{display:block}
label{display:block;margin:.8em 0 .2em}
select{width:100%;box-sizing:border-box;padding:.5em;font-size:1em}
button{margin-top:1em;padding:.6em 1.2em;font-size:1em}
.msg{margin-top:.6em;font-size:.9em}
.msg.err{color:#b00}
.msg.ok{color:#0a0}
</style></head>
<body>
<h1>Transit Board Settings</h1>

<section id="step-orientation" class="active">
<label>Display orientation</label>
<select id="orientation">
<option value="landscape">Horizontal (landscape)</option>
<option value="portrait">Vertical (portrait)</option>
</select>
<button onclick="saveOrientation()">Save</button>
<div id="orientation-msg" class="msg"></div>
</section>

<section id="step-sta">
<p>Optional &mdash; also show Spokane Transit Authority (STA) departures alongside Transit's.</p>
<label>STA stop number</label>
<input id="sta-stop" placeholder="e.g. 4377 &mdash; printed on the stop sign">
<button onclick="saveStaStop()">Save</button>
<button onclick="showStep('step-done')">Skip</button>
<div id="sta-msg" class="msg"></div>
</section>

<section id="step-done">
<h2>Settings saved</h2>
<p>Your board will redraw with the new settings. You can close this page.</p>
</section>

<script>
function el(id){return document.getElementById(id);}
function showStep(id){
  document.querySelectorAll('section').forEach(function(s){s.classList.remove('active');});
  el(id).classList.add('active');
}
function setMsg(id,text,cls){
  var m=el(id); m.textContent=text; m.className='msg'+(cls?(' '+cls):'');
}

fetch('/getorientation').then(function(r){return r.json();}).then(function(s){
  el('orientation').value = s.portrait ? 'portrait' : 'landscape';
}).catch(function(){});

function saveOrientation(){
  var portrait = el('orientation').value === 'portrait';
  setMsg('orientation-msg','Saving...','');
  fetch('/setorientation',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'portrait='+(portrait?'1':'0')})
    .then(function(r){return r.json();}).then(function(res){
      if(res.ok){
        fetch('/getstastop').then(function(r){return r.json();}).then(function(s){
          el('sta-stop').value = s.stopCode || '';
        }).catch(function(){});
        showStep('step-sta');
      }
      else { setMsg('orientation-msg',res.message||'Could not save.','err'); }
    }).catch(function(){ setMsg('orientation-msg','Could not reach the board. Try again.','err'); });
}

function saveStaStop(){
  var code = el('sta-stop').value.trim();
  setMsg('sta-msg','Saving...','');
  fetch('/setstastop',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'code='+encodeURIComponent(code)})
    .then(function(r){return r.json();}).then(function(res){
      if(res.ok){ showStep('step-done'); }
      else { setMsg('sta-msg',res.message||'Could not save.','err'); }
    }).catch(function(){ setMsg('sta-msg','Could not reach the board. Try again.','err'); });
}
</script>
</body></html>
)HTML";

}  // namespace

SetupFlow::SetupFlow(ConfigStore& configStore, TransitApiClient& apiClient, RenderEngine& renderEngine)
    : configStore_(configStore),
      apiClient_(apiClient),
      renderEngine_(renderEngine),
      server_(kHttpPort) {}

void SetupFlow::touchActivity() { lastActivityMs_ = millis(); }

void SetupFlow::startPortal() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(kApSsid);
  IPAddress apIp = WiFi.softAPIP();
  dnsServer_.start(kDnsPort, "*", apIp);

  server_.on("/", HTTP_GET, [this]() { handleRoot(); });
  server_.on("/scan", HTTP_GET, [this]() { handleScan(); });
  server_.on("/connect", HTTP_POST, [this]() { handleConnect(); });
  server_.on("/status", HTTP_GET, [this]() { handleStatus(); });
  server_.on("/apikey", HTTP_POST, [this]() { handleApiKey(); });
  server_.on("/stopsearch", HTTP_POST, [this]() { handleStopSearch(); });
  server_.on("/stopselect", HTTP_POST, [this]() { handleStopSelect(); });
  server_.on("/getorientation", HTTP_GET, [this]() { handleGetOrientation(); });
  server_.on("/setorientation", HTTP_POST, [this]() { handleSetOrientation(); });
  server_.on("/getstastop", HTTP_GET, [this]() { handleGetStaStop(); });
  server_.on("/setstastop", HTTP_POST, [this]() { handleSetStaStop(); });

  // Common captive-portal probe URLs (Android/Chrome, iOS/macOS, Windows) —
  // redirecting these to "/" is what makes phones auto-open the portal
  // instead of silently deciding the AP has no internet and moving on.
  const char* probeUrls[] = {
      "/generate_204",       "/gen_204",          "/hotspot-detect.html",
      "/library/test/success.html", "/ncsi.txt",   "/connecttest.txt",
      "/success.txt",        "/fwlink",
  };
  for (const char* url : probeUrls) {
    server_.on(url, HTTP_GET, [this]() { handleCaptiveRedirect(); });
  }
  server_.onNotFound([this]() { handleNotFound(); });

  server_.begin();
  touchActivity();
}

void SetupFlow::stopPortal() {
  server_.stop();
  dnsServer_.stop();
  // Drop the AP only (not softAPdisconnect(true), which powers the whole
  // Wi-Fi driver off) — main.cpp does its own WiFi.mode(WIFI_STA)/
  // WiFi.begin() right after runFirstTimeSetup() returns regardless, so
  // there's no reason to force a full radio teardown here.
  WiFi.softAPdisconnect(false);
  WiFi.mode(WIFI_STA);
}

void SetupFlow::pollWifiConnectState() {
  if (wifiConnectState_ != WifiConnectState::kConnecting) return;

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnectState_ = WifiConnectState::kConnected;
    configStore_.setWifiSsid(pendingSsid_);
    configStore_.setWifiPassword(pendingPassword_);
    return;
  }
  if (millis() - wifiConnectStartMs_ > kWifiConnectTimeoutMs) {
    wifiConnectState_ = WifiConnectState::kFailed;
  }
}

// --- Route handlers ----------------------------------------------------------

void SetupFlow::handleRoot() {
  touchActivity();
  if (portalMode_ == PortalMode::kSettings) {
    server_.send_P(200, "text/html", kSettingsPageHtml);
  } else {
    server_.send_P(200, "text/html", kSetupPageHtml);
  }
}

void SetupFlow::handleGetOrientation() {
  touchActivity();
  if (!requireSettingsMode()) return;
  JsonDocument doc;
  doc["portrait"] = configStore_.displayPortrait();
  std::string body;
  serializeJson(doc, body);
  server_.send(200, "application/json", body.c_str());
}

void SetupFlow::handleSetOrientation() {
  touchActivity();
  if (!requireSettingsMode()) return;
  std::string portraitArg = server_.hasArg("portrait") ? server_.arg("portrait").c_str() : "";
  bool portrait = portraitArg == "1" || portraitArg == "true";
  configStore_.setDisplayPortrait(portrait);
  settingsSaved_ = true;
  server_.send(200, "application/json", "{\"ok\":true}");
}

void SetupFlow::handleGetStaStop() {
  touchActivity();
  if (!requireSettingsMode()) return;
  JsonDocument doc;
  doc["stopCode"] = configStore_.staStopCode();
  std::string body;
  serializeJson(doc, body);
  server_.send(200, "application/json", body.c_str());
}

void SetupFlow::handleSetStaStop() {
  touchActivity();
  if (!requireSettingsMode()) return;
  std::string code = server_.hasArg("code") ? server_.arg("code").c_str() : "";

  // Validated synchronously against the baked-in sta_stop_table.h (a plain
  // table lookup, not a network call like handleApiKey()'s validation) via
  // the same sta::parseStaStopCode() helper StaClient::fetchDepartures()
  // resolves a saved code with, so a typo surfaces here immediately rather
  // than silently saving a code that will just never match anything at
  // fetch time -- and the two can't drift out of sync with each other.
  // Empty is always accepted -- it means "STA not configured," same as an
  // empty api_key/stop_id means "not provisioned" elsewhere in this flow.
  if (!code.empty() && sta::parseStaStopCode(code) == nullptr) {
    server_.send(200, "application/json",
                 "{\"ok\":false,\"message\":\"Stop number not found. Check the number on the "
                 "sign.\"}");
    return;
  }

  configStore_.setStaStopCode(code);
  settingsSaved_ = true;
  server_.send(200, "application/json", "{\"ok\":true}");
}

// The Wi-Fi/API-key/stop wizard's endpoints stay registered even while
// runSettingsPortal() is up (startPortal() is shared machinery — see its
// comment), so each of those handlers must refuse to act when portalMode_
// isn't kFirstRun. Without this, someone joined to the open setup AP (its
// only access control is physical proximity, per this file's header
// comment) could POST to e.g. /apikey or /connect during what the settings
// page presents as an orientation-only change and silently overwrite
// Wi-Fi credentials, the API key, or the stop pick.
bool SetupFlow::requireFirstRunMode() {
  if (portalMode_ == PortalMode::kFirstRun) return true;
  server_.send(403, "application/json", "{\"ok\":false,\"message\":\"Not available.\"}");
  return false;
}

// The mirror image of requireFirstRunMode(), for the settings-only
// handlers above: startPortal() also keeps /getorientation, /setorientation,
// /getstastop, and /setstastop registered while the open first-run AP is up
// (before the board is even provisioned), even though the first-run
// wizard's own page never presents those steps and never links to them.
// Lower stakes than the first-run-endpoints-during-settings case above
// (anyone on that AP already has full first-run wizard access anyway), but
// there's no reason to leave settings writable from a step of the flow
// that hasn't asked for them.
bool SetupFlow::requireSettingsMode() {
  if (portalMode_ == PortalMode::kSettings) return true;
  server_.send(403, "application/json", "{\"ok\":false,\"message\":\"Not available.\"}");
  return false;
}

void SetupFlow::handleScan() {
  touchActivity();
  if (!requireFirstRunMode()) return;
  // Skip the actual radio scan while a STA connect attempt (e.g. the
  // background resume-with-saved-credentials one runFirstTimeSetup() may
  // have kicked off) is in flight: scanning on the same radio mid-association
  // routinely aborts/restarts an ESP32 station connection. The page's own
  // manual-SSID field still works as a fallback for this brief window.
  int found = wifiConnectState_ == WifiConnectState::kConnecting ? 0 : WiFi.scanNetworks();
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  std::vector<std::string> seen;
  for (int i = 0; i < found; ++i) {
    std::string ssid(WiFi.SSID(i).c_str());
    if (ssid.empty()) continue;
    if (std::find(seen.begin(), seen.end(), ssid) != seen.end()) continue;  // dedupe APs
    seen.push_back(ssid);
    arr.add(ssid);
  }
  WiFi.scanDelete();
  std::string body;
  serializeJson(doc, body);
  server_.send(200, "application/json", body.c_str());
}

void SetupFlow::handleConnect() {
  touchActivity();
  if (!requireFirstRunMode()) return;
  std::string ssid = server_.hasArg("ssid") ? server_.arg("ssid").c_str() : "";
  if (ssid.empty()) {
    server_.send(400, "application/json", "{\"ok\":false,\"message\":\"SSID required\"}");
    return;
  }
  std::string password = server_.hasArg("password") ? server_.arg("password").c_str() : "";

  pendingSsid_ = ssid;
  pendingPassword_ = password;
  // STA-only connect attempt — the AP interface (WIFI_AP_STA) stays up the
  // whole time so the portal itself never drops.
  WiFi.begin(pendingSsid_.c_str(), pendingPassword_.empty() ? nullptr : pendingPassword_.c_str());
  wifiConnectState_ = WifiConnectState::kConnecting;
  wifiConnectStartMs_ = millis();

  server_.send(200, "application/json", "{\"ok\":true}");
}

void SetupFlow::handleStatus() {
  touchActivity();
  const char* wifiStateStr = "idle";
  switch (wifiConnectState_) {
    case WifiConnectState::kIdle: wifiStateStr = "idle"; break;
    case WifiConnectState::kConnecting: wifiStateStr = "connecting"; break;
    case WifiConnectState::kConnected: wifiStateStr = "connected"; break;
    case WifiConnectState::kFailed: wifiStateStr = "failed"; break;
  }

  JsonDocument doc;
  doc["wifiState"] = wifiStateStr;
  doc["apiKeySet"] = !configStore_.apiKey().empty();
  doc["stopSet"] = !configStore_.stopId().empty();
  doc["provisioned"] = configStore_.isProvisioned();
  std::string body;
  serializeJson(doc, body);
  server_.send(200, "application/json", body.c_str());
}

void SetupFlow::handleApiKey() {
  touchActivity();
  if (!requireFirstRunMode()) return;
  std::string key = server_.hasArg("key") ? server_.arg("key").c_str() : "";
  if (key.empty()) {
    server_.send(200, "application/json", "{\"ok\":false,\"message\":\"The key can't be empty.\"}");
    return;
  }

  apiClient_.setApiKey(key);
  NearbyStopsParams params;
  params.maxDistanceMeters = 500;
  NearbyStopsResponse response;
  bool ok = apiClient_.nearbyStops(kKeyCheckLat, kKeyCheckLon, params, response);

  JsonDocument doc;
  if (ok) {
    configStore_.setApiKey(key);
    // Never show the full key anywhere (root CLAUDE.md's "truncate to the
    // last few characters" guardrail) — a short key just shows fully
    // masked rather than falling back to printing it whole.
    std::string suffix = key.size() > 4 ? ("..." + key.substr(key.size() - 4)) : "(hidden)";
    doc["ok"] = true;
    doc["keySuffix"] = suffix;
  } else {
    doc["ok"] = false;
    doc["message"] = "Key rejected, or offline. A freshly issued key can take a moment to propagate.";
  }
  std::string body;
  serializeJson(doc, body);
  server_.send(200, "application/json", body.c_str());
}

void SetupFlow::handleStopSearch() {
  touchActivity();
  if (!requireFirstRunMode()) return;
  std::string latText = server_.hasArg("lat") ? server_.arg("lat").c_str() : "";
  std::string lonText = server_.hasArg("lon") ? server_.arg("lon").c_str() : "";
  std::string query = server_.hasArg("query") ? server_.arg("query").c_str() : "";

  double lat = 0.0;
  double lon = 0.0;
  bool validCoords = parseDouble(latText, lat) && parseDouble(lonText, lon) && lat >= -90.0 &&
                      lat <= 90.0 && lon >= -180.0 && lon <= 180.0;
  if (!validCoords) {
    server_.send(200, "application/json",
                 "{\"ok\":false,\"message\":\"Enter a valid latitude (-90 to 90) and longitude (-180 to "
                 "180).\"}");
    return;
  }

  SearchStopsParams params;
  params.maxNumResults = 20;
  SearchStopsResponse response;
  bool ok = apiClient_.searchStops(lat, lon, query, params, response);

  JsonDocument doc;
  if (!ok || response.results.empty()) {
    doc["ok"] = false;
    doc["message"] = "No matching stops near that area. Check the coordinates/spelling and try again.";
    std::string body;
    serializeJson(doc, body);
    server_.send(200, "application/json", body.c_str());
    return;
  }

  lastStopResults_ = response.results;
  doc["ok"] = true;
  JsonArray results = doc["results"].to<JsonArray>();
  for (size_t i = 0; i < lastStopResults_.size(); ++i) {
    JsonObject item = results.add<JsonObject>();
    item["index"] = static_cast<int>(i);
    item["name"] = lastStopResults_[i].stopName;
    item["distanceMeters"] = lastStopResults_[i].distanceMeters;
  }
  std::string body;
  serializeJson(doc, body);
  server_.send(200, "application/json", body.c_str());
}

void SetupFlow::handleStopSelect() {
  touchActivity();
  if (!requireFirstRunMode()) return;
  std::string indexText = server_.hasArg("index") ? server_.arg("index").c_str() : "";
  // strtol, not atoi: atoi silently returns 0 for non-numeric input, which
  // would look like a valid "first result" pick instead of a parse failure.
  int index = -1;
  if (!indexText.empty()) {
    const char* start = indexText.c_str();
    char* end = nullptr;
    long parsed = strtol(start, &end, 10);
    if (end != start && *end == '\0' && parsed >= 0 && parsed <= INT_MAX) {
      index = static_cast<int>(parsed);
    }
  }
  if (index < 0 || static_cast<size_t>(index) >= lastStopResults_.size()) {
    server_.send(200, "application/json", "{\"ok\":false,\"message\":\"Search again and pick a stop.\"}");
    return;
  }

  const SearchStopResult& chosen = lastStopResults_[static_cast<size_t>(index)];
  configStore_.setStopId(chosen.globalStopId);

  JsonDocument doc;
  doc["ok"] = true;
  doc["stopName"] = chosen.stopName;
  std::string body;
  serializeJson(doc, body);
  server_.send(200, "application/json", body.c_str());
}

void SetupFlow::handleCaptiveRedirect() {
  touchActivity();
  std::string location = "http://" + std::string(WiFi.softAPIP().toString().c_str()) + "/";
  server_.sendHeader("Location", location.c_str(), true);
  server_.send(302, "text/plain", "");
}

void SetupFlow::handleNotFound() {
  touchActivity();
  std::string location = "http://" + std::string(WiFi.softAPIP().toString().c_str()) + "/";
  server_.sendHeader("Location", location.c_str(), true);
  server_.send(302, "text/plain", "");
}

bool SetupFlow::runFirstTimeSetup() {
  if (configStore_.isProvisioned()) return true;

  renderEngine_.renderSetupPrompt(
      "Setup",
      std::string("Connect your phone to Wi-Fi\nnetwork \"") + kApSsid +
          "\",\nthen visit http://192.168.4.1");

  startPortal();

  // Resume support: reuse already-saved values without re-asking. Wi-Fi
  // credentials may already be stored (a resume after an interrupted setup,
  // or a later re-run — this device has no settings UI beyond first-run
  // setup) — try them in the background rather than making a returning user
  // re-enter a network that still works; the portal stays up regardless, so
  // falling back to the web step if this fails costs nothing.
  std::string savedSsid = configStore_.wifiSsid();
  if (!savedSsid.empty()) {
    pendingSsid_ = savedSsid;
    pendingPassword_ = configStore_.wifiPassword();
    WiFi.begin(pendingSsid_.c_str(), pendingPassword_.empty() ? nullptr : pendingPassword_.c_str());
    wifiConnectState_ = WifiConnectState::kConnecting;
    wifiConnectStartMs_ = millis();
  }
  if (!configStore_.apiKey().empty()) {
    // main.cpp constructed apiClient_ with whatever configStore_.apiKey()
    // held at boot, which is this same value — re-set defensively in case a
    // caller ever changes that.
    apiClient_.setApiKey(configStore_.apiKey());
  }

  uint32_t provisionedAtMs = 0;
  while (true) {
    dnsServer_.processNextRequest();
    server_.handleClient();
    pollWifiConnectState();

    if (configStore_.isProvisioned()) {
      if (provisionedAtMs == 0) {
        provisionedAtMs = millis();
        renderEngine_.renderSetupPrompt("Setup complete", "Your board is ready.");
      } else if (millis() - provisionedAtMs > kProvisionedLingerMs) {
        break;
      }
    } else if (millis() - lastActivityMs_ > kSetupIdleTimeoutMs) {
      renderEngine_.renderSetupPrompt("Setup paused",
                                       "No activity on the setup page.\nWill retry on next wake.");
      break;
    }

    delay(10);
  }

  stopPortal();
  return configStore_.isProvisioned();
}

bool SetupFlow::runSettingsPortal() {
  portalMode_ = PortalMode::kSettings;
  renderEngine_.renderSetupPrompt(
      "Settings",
      std::string("Connect your phone to Wi-Fi\nnetwork \"") + kApSsid +
          "\",\nthen visit http://192.168.4.1");
  startPortal();

  settingsSaved_ = false;
  uint32_t savedAtMs = 0;
  while (true) {
    dnsServer_.processNextRequest();
    server_.handleClient();

    if (settingsSaved_) {
      if (savedAtMs == 0) {
        savedAtMs = millis();
        renderEngine_.renderSetupPrompt("Settings saved", "Applying your changes...");
      } else if (millis() - savedAtMs > kProvisionedLingerMs) {
        break;
      }
    } else if (millis() - lastActivityMs_ > kSetupIdleTimeoutMs) {
      renderEngine_.renderSetupPrompt("Settings paused",
                                       "No activity on the settings page.\nNo changes were made.");
      break;
    }

    delay(10);
  }

  stopPortal();
  portalMode_ = PortalMode::kFirstRun;
  return settingsSaved_;
}

}  // namespace transit
