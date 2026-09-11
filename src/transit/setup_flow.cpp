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
// kSettings (runSettingsPortal()). Four steps: orientation -> STA -> preset
// "Home"/"Work" trip chains -> done. Each step's own Save/Skip button
// advances to the *next* step's <section> client-side; only reaching
// step-done calls /settingsdone (see settingsFinished_'s comment in
// setup_flow.h on why that, not "any setting was saved," is what actually
// tells the C++ loop to close the portal).
const char kSettingsPageHtml[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Transit Board Settings</title>
<style>
body{font-family:system-ui,sans-serif;max-width:480px;margin:1.5em auto;padding:0 1em;color:#222}
h1{font-size:1.2em}
h2{font-size:1em;margin-top:1.2em}
section{display:none;margin-bottom:1.5em}
section.active{display:block}
label{display:block;margin:.8em 0 .2em}
input,select{width:100%;box-sizing:border-box;padding:.5em;font-size:1em}
button{margin-top:1em;padding:.6em 1.2em;font-size:1em;margin-right:.5em}
button.small{padding:.3em .7em;font-size:.9em;margin-top:.4em}
ul{list-style:none;padding:0}
li{padding:.5em;border:1px solid #ccc;border-radius:4px;margin:.3em 0;cursor:pointer}
li:hover{background:#f0f0f0}
.leg{border:1px solid #ddd;border-radius:6px;padding:.6em;margin:.6em 0}
.leg-summary{font-size:.85em;color:#555;margin:.3em 0}
.picker{border:1px dashed #999;border-radius:6px;padding:.6em;margin:.6em 0}
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
<button onclick="showStep('step-presets')">Skip</button>
<div id="sta-msg" class="msg"></div>
</section>

<section id="step-presets">
<p>Optional &mdash; a fixed route chain (e.g. bus 31, transfer to 32) to a
saved "Home"/"Work" destination, so the board can tell you when to leave and
when to transfer. Enter each leg in order; up to 3 legs per destination.</p>

<h2>Home</h2>
<p id="home-summary" class="leg-summary"></p>
<div id="home-legs"></div>
<button class="small" onclick="addLeg('home')">+ Add leg to Home</button>
<label>Walk time to first stop (minutes)</label>
<input id="home-walk" type="number" min="0" value="0">

<h2>Work</h2>
<p id="work-summary" class="leg-summary"></p>
<div id="work-legs"></div>
<button class="small" onclick="addLeg('work')">+ Add leg to Work</button>
<label>Walk time to first stop (minutes)</label>
<input id="work-walk" type="number" min="0" value="0">

<label>Transfer buffer, both destinations (minutes)</label>
<input id="xfer-buf" type="number" min="0" value="3">

<div id="picker-host"></div>

<button onclick="savePresets()">Save</button>
<button onclick="finishSettings()">Skip</button>
<div id="presets-msg" class="msg"></div>
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
      if(res.ok){ loadPresets(); showStep('step-presets'); }
      else { setMsg('sta-msg',res.message||'Could not save.','err'); }
    }).catch(function(){ setMsg('sta-msg','Could not reach the board. Try again.','err'); });
}

// --- Preset "Home"/"Work" leg chains ---------------------------------------
//
// legs are assembled entirely client-side (routeId/stopId resolved live via
// /legdirections and /stopsearch below) and only sent to the board in one
// shot on Save -- see savePresets(). Editing previously-saved legs isn't
// supported: on load this only shows a plain leg count, not a rebuilt
// editable list (that would need a stop-name-from-id reverse lookup this
// page doesn't have) -- re-entering a whole preset's chain is the way to
// change it, acceptable at up to 3 legs per destination.
// legsTouched: true once the user has actually added/removed a leg this
// session. loadPresets() never rebuilds an editable leg list from the
// server (see the comment above) -- presets[x].legs starts empty every
// time the portal opens, indistinguishable by content alone from "the user
// deliberately cleared every leg." Without this flag, saving after only
// changing e.g. the walk-time field (never touching legs at all) would
// send an empty legs array and silently wipe out a previously-configured
// chain -- see savePresets() below, which omits the "legs" key entirely
// for an untouched preset instead of sending an empty array.
var presets = {home:{legs:[],legsTouched:false}, work:{legs:[],legsTouched:false}};
var picker = null; // {preset, legIndex, field:'board'|'alight'} while open

function loadPresets(){
  fetch('/getpresets').then(function(r){return r.json();}).then(function(s){
    el('home-summary').textContent = (s.home&&s.home.legCount>0) ? (s.home.legCount+' leg(s) currently configured.') : 'Not configured.';
    el('work-summary').textContent = (s.work&&s.work.legCount>0) ? (s.work.legCount+' leg(s) currently configured.') : 'Not configured.';
    if(s.home) el('home-walk').value = s.home.walkMin||0;
    if(s.work) el('work-walk').value = s.work.walkMin||0;
    el('xfer-buf').value = s.transferBufferMin||3;
  }).catch(function(){});
}

function emptyLeg(){
  return {routeQuery:'',routeId:'',routeShortName:'',boardStopId:'',boardStopName:'',
          alightStopId:'',alightStopName:'',directionId:-1,directions:null};
}

function addLeg(preset){
  if(presets[preset].legs.length>=3) return;
  presets[preset].legs.push(emptyLeg());
  presets[preset].legsTouched = true;
  renderLegs(preset);
}
function removeLeg(preset,idx){
  presets[preset].legs.splice(idx,1);
  presets[preset].legsTouched = true;
  renderLegs(preset);
}

function legComplete(leg){
  return !!(leg.routeId && leg.boardStopId && leg.alightStopId);
}

function renderLegs(preset){
  var container = el(preset+'-legs');
  container.innerHTML = '';
  presets[preset].legs.forEach(function(leg,idx){
    var div = document.createElement('div');
    div.className = 'leg';

    var routeLabel = document.createElement('label');
    routeLabel.textContent = 'Leg '+(idx+1)+' route number';
    div.appendChild(routeLabel);
    var routeInput = document.createElement('input');
    routeInput.value = leg.routeQuery;
    routeInput.placeholder = 'e.g. 31';
    routeInput.oninput = function(){ leg.routeQuery=routeInput.value.trim(); leg.routeId=''; leg.directions=null; leg.directionId=-1; };
    div.appendChild(routeInput);

    var boardBtn = document.createElement('button');
    boardBtn.className = 'small';
    boardBtn.textContent = leg.boardStopName ? ('Boarding: '+leg.boardStopName) : 'Pick boarding stop';
    boardBtn.onclick = function(){ openPicker(preset, idx, 'board'); };
    div.appendChild(boardBtn);

    var dirBtn = document.createElement('button');
    dirBtn.className = 'small';
    dirBtn.disabled = !(leg.routeQuery && leg.boardStopId);
    dirBtn.textContent = leg.directions ? 'Directions checked' : 'Check directions';
    dirBtn.onclick = function(){ checkDirections(preset, idx); };
    div.appendChild(dirBtn);

    if(leg.directions && leg.directions.length>1){
      var dirWrap = document.createElement('div');
      leg.directions.forEach(function(d){
        var label = document.createElement('label');
        var radio = document.createElement('input');
        radio.type='radio'; radio.name='dir-'+preset+'-'+idx; radio.checked = leg.directionId===d.directionId;
        radio.onclick = function(){ leg.directionId=d.directionId; };
        label.appendChild(radio);
        label.appendChild(document.createTextNode(' '+(d.headsign||('Direction '+d.directionId))));
        dirWrap.appendChild(label);
      });
      div.appendChild(dirWrap);
    }

    var alightBtn = document.createElement('button');
    alightBtn.className = 'small';
    alightBtn.textContent = leg.alightStopName ? ('Transfer at: '+leg.alightStopName) : 'Pick transfer stop';
    alightBtn.onclick = function(){ openPicker(preset, idx, 'alight'); };
    div.appendChild(alightBtn);

    var removeBtn = document.createElement('button');
    removeBtn.className = 'small';
    removeBtn.textContent = 'Remove leg';
    removeBtn.onclick = function(){ removeLeg(preset, idx); };
    div.appendChild(removeBtn);

    container.appendChild(div);
  });
}

function openPicker(preset, legIndex, field){
  picker = {preset:preset, legIndex:legIndex, field:field};
  var host = el('picker-host');
  host.innerHTML = '';
  var box = document.createElement('div');
  box.className = 'picker';
  box.innerHTML =
    '<label>Approximate latitude</label><input id="picker-lat" placeholder="e.g. 45.5017">'+
    '<label>Approximate longitude</label><input id="picker-lon" placeholder="e.g. -73.5673">'+
    '<label>Stop name (part of it)</label><input id="picker-query" placeholder="e.g. Main St">'+
    '<button class="small" onclick="searchPickerStops()">Search</button>'+
    '<div id="picker-msg" class="msg"></div><ul id="picker-list"></ul>';
  host.appendChild(box);
}

function searchPickerStops(){
  var lat=el('picker-lat').value.trim(), lon=el('picker-lon').value.trim(), query=el('picker-query').value.trim();
  setMsg('picker-msg','Searching...','');
  el('picker-list').innerHTML='';
  var body='lat='+encodeURIComponent(lat)+'&lon='+encodeURIComponent(lon)+'&query='+encodeURIComponent(query);
  fetch('/stopsearch',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body})
    .then(function(r){return r.json();}).then(function(res){
      if(!res.ok){ setMsg('picker-msg',res.message||'No stops found.','err'); return; }
      setMsg('picker-msg','Pick a stop:','');
      var ul=el('picker-list');
      res.results.forEach(function(item){
        var li=document.createElement('li');
        li.textContent=item.name+(item.distanceMeters>0?(' ('+Math.round(item.distanceMeters)+'m)'):'');
        li.onclick=function(){ applyPickedStop(item); };
        ul.appendChild(li);
      });
    }).catch(function(){ setMsg('picker-msg','Could not reach the board. Try again.','err'); });
}

function applyPickedStop(item){
  if(!picker) return;
  var presetKey = picker.preset;
  var leg = presets[presetKey].legs[picker.legIndex];
  if(picker.field==='board'){
    leg.boardStopId = item.stopId;
    leg.boardStopName = item.name;
    // Boarding stop changed -- any previously-resolved route/direction was
    // checked against the OLD boarding stop and no longer applies; clear
    // routeId too (not just directions), otherwise legComplete() would
    // still pass with a routeId resolved for a different stop than the one
    // now saved.
    leg.routeId = '';
    leg.directions = null;
    leg.directionId = -1;
  } else {
    leg.alightStopId = item.stopId;
    leg.alightStopName = item.name;
  }
  el('picker-host').innerHTML = '';
  picker = null;
  renderLegs(presetKey);
}

function checkDirections(preset, idx){
  var leg = presets[preset].legs[idx];
  setMsg('presets-msg','Checking directions for route '+leg.routeQuery+'...','');
  var body='routeQuery='+encodeURIComponent(leg.routeQuery)+'&stopId='+encodeURIComponent(leg.boardStopId);
  fetch('/legdirections',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body})
    .then(function(r){return r.json();}).then(function(res){
      if(!res.ok){ setMsg('presets-msg',res.message||'Route not found at that stop.','err'); return; }
      leg.routeId = res.routeId;
      leg.routeShortName = res.routeShortName;
      leg.directions = res.directions;
      leg.directionId = res.directions.length===1 ? res.directions[0].directionId : -1;
      setMsg('presets-msg','','');
      renderLegs(preset);
    }).catch(function(){ setMsg('presets-msg','Could not reach the board. Try again.','err'); });
}

function savePresets(){
  var payload = {
    home: {walkMin: parseInt(el('home-walk').value,10)||0},
    work: {walkMin: parseInt(el('work-walk').value,10)||0},
    transferBufferMin: parseInt(el('xfer-buf').value,10)||3
  };
  // Only send "legs" for a preset the user actually touched this session
  // (added/removed a leg) -- see presets' legsTouched comment above. An
  // untouched preset's server-side legs are left exactly as they were.
  if(presets.home.legsTouched) payload.home.legs = presets.home.legs.filter(legComplete);
  if(presets.work.legsTouched) payload.work.legs = presets.work.legs.filter(legComplete);
  setMsg('presets-msg','Saving...','');
  fetch('/setpresets',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)})
    .then(function(r){return r.json();}).then(function(res){
      if(res.ok){ finishSettings(); }
      else { setMsg('presets-msg',res.message||'Could not save.','err'); }
    }).catch(function(){ setMsg('presets-msg','Could not reach the board. Try again.','err'); });
}

function finishSettings(){
  fetch('/settingsdone',{method:'POST'}).catch(function(){});
  showStep('step-done');
}

loadPresets();
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
  server_.on("/legdirections", HTTP_POST, [this]() { handleLegDirections(); });
  server_.on("/getpresets", HTTP_GET, [this]() { handleGetPresets(); });
  server_.on("/setpresets", HTTP_POST, [this]() { handleSetPresets(); });
  server_.on("/settingsdone", HTTP_POST, [this]() { handleSettingsDone(); });

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

// Resolves a route short name (typed free-text, e.g. "31") at a given
// boarding stop into a real globalRouteId plus that route's available
// directions -- stop_departures alone can't say which direction continues
// toward a transfer stop (see trip_planner.h's TripLegConfig::directionId
// comment), so the settings page calls this live rather than guessing.
// Takes the FIRST Route entry whose routeShortName matches exactly
// (case-sensitive; the caller trims -- see kSettingsPageHtml's route input
// handling) -- two different agencies serving the same stop with the
// same short number is a real but rare GTFS edge case this doesn't attempt
// to disambiguate further; see docs/TRIP_PLANNER.md.
void SetupFlow::handleLegDirections() {
  touchActivity();
  if (!requireSettingsMode()) return;
  std::string routeQuery = server_.hasArg("routeQuery") ? server_.arg("routeQuery").c_str() : "";
  std::string stopId = server_.hasArg("stopId") ? server_.arg("stopId").c_str() : "";
  if (routeQuery.empty() || stopId.empty()) {
    server_.send(200, "application/json", "{\"ok\":false,\"message\":\"Route and boarding stop required.\"}");
    return;
  }

  StopDeparturesParams params;
  params.maxNumDepartures = 1;  // only the route/direction/headsign metadata is needed here
  StopDeparturesResponse response;
  bool ok = apiClient_.stopDepartures({stopId}, params, response);
  if (!ok) {
    server_.send(200, "application/json", "{\"ok\":false,\"message\":\"Could not look up that stop right now.\"}");
    return;
  }

  const Route* matched = nullptr;
  for (const auto& route : response.routeDepartures) {
    if (route.routeShortName == routeQuery) {
      matched = &route;
      break;
    }
  }
  if (matched == nullptr) {
    server_.send(200, "application/json", "{\"ok\":false,\"message\":\"Route not found at that stop.\"}");
    return;
  }

  JsonDocument doc;
  doc["ok"] = true;
  doc["routeId"] = matched->globalRouteId;
  doc["routeShortName"] = matched->routeShortName;
  JsonArray directions = doc["directions"].to<JsonArray>();
  for (const auto& mi : matched->mergedItineraries) {
    JsonObject dir = directions.add<JsonObject>();
    dir["directionId"] = mi.directionId;
    std::string headsign;
    if (!mi.itineraries.empty()) {
      const Itinerary& itin = mi.itineraries.front();
      headsign = !itin.mergedHeadsign.empty() ? itin.mergedHeadsign : itin.headsign;
    }
    dir["headsign"] = headsign;
  }
  std::string body;
  serializeJson(doc, body);
  server_.send(200, "application/json", body.c_str());
}

namespace {
void presetToJson(const std::vector<TripLegConfig>& legs, int walkMin, JsonObject& out) {
  out["legCount"] = static_cast<int>(legs.size());
  out["walkMin"] = walkMin;
}
}  // namespace

void SetupFlow::handleGetPresets() {
  touchActivity();
  if (!requireSettingsMode()) return;
  JsonDocument doc;
  JsonObject home = doc["home"].to<JsonObject>();
  presetToJson(configStore_.presetLegs(ConfigStore::PresetId::kHome),
              configStore_.presetWalkToFirstStopMin(ConfigStore::PresetId::kHome), home);
  JsonObject work = doc["work"].to<JsonObject>();
  presetToJson(configStore_.presetLegs(ConfigStore::PresetId::kWork),
              configStore_.presetWalkToFirstStopMin(ConfigStore::PresetId::kWork), work);
  doc["transferBufferMin"] = configStore_.transferBufferMin();
  std::string body;
  serializeJson(doc, body);
  server_.send(200, "application/json", body.c_str());
}

namespace {
// Parses one preset's "legs" JSON array (see kSettingsPageHtml's
// savePresets() for the request shape) into TripLegConfig entries. Returns
// false (leaving out unspecified) on any leg missing a required field --
// the caller is trusted to have resolved routeId/stopIds live via
// /legdirections and /stopsearch before assembling this payload, so this is
// a shape check, not a re-verification against a live API call.
bool parseLegsJson(JsonArrayConst legsJson, std::vector<TripLegConfig>& out) {
  if (legsJson.size() > 3) return false;
  out.clear();
  for (JsonObjectConst legJson : legsJson) {
    TripLegConfig leg;
    leg.routeId = legJson["routeId"] | "";
    leg.boardStopId = legJson["boardStopId"] | "";
    leg.alightStopId = legJson["alightStopId"] | "";
    leg.directionId = legJson["directionId"] | -1;
    if (leg.routeId.empty() || leg.boardStopId.empty() || leg.alightStopId.empty()) return false;
    out.push_back(leg);
  }
  return true;
}
}  // namespace

void SetupFlow::handleSetPresets() {
  touchActivity();
  if (!requireSettingsMode()) return;
  // JSON body, not form fields (unlike this file's other POST handlers):
  // a nested per-leg structure doesn't fit application/x-www-form-urlencoded
  // cleanly. WebServer surfaces an unrecognized Content-Type's raw body as
  // the "plain" arg -- the standard ESP32 WebServer idiom for a JSON POST.
  std::string rawBody = server_.hasArg("plain") ? server_.arg("plain").c_str() : "";
  JsonDocument doc;
  if (deserializeJson(doc, rawBody) != DeserializationError::Ok) {
    server_.send(200, "application/json", "{\"ok\":false,\"message\":\"Malformed request.\"}");
    return;
  }

  // "legs" is only present when the settings page's JS actually touched
  // that preset's leg list this session (added/removed a leg) -- see
  // kSettingsPageHtml's legsTouched comment. Its absence means "leave this
  // preset's saved legs exactly as they are," distinct from a present-but-
  // empty array (which means "the user cleared every leg, disable this
  // preset"). Skipping setPresetLegs() entirely in the absent case is what
  // prevents saving an unrelated field (walk time, transfer buffer,
  // orientation) from silently wiping out a previously-configured chain.
  const bool homeLegsPresent = doc["home"]["legs"].is<JsonArrayConst>();
  const bool workLegsPresent = doc["work"]["legs"].is<JsonArrayConst>();
  std::vector<TripLegConfig> homeLegs;
  std::vector<TripLegConfig> workLegs;
  if ((homeLegsPresent && !parseLegsJson(doc["home"]["legs"].as<JsonArrayConst>(), homeLegs)) ||
      (workLegsPresent && !parseLegsJson(doc["work"]["legs"].as<JsonArrayConst>(), workLegs))) {
    server_.send(200, "application/json",
                 "{\"ok\":false,\"message\":\"Each destination allows at most 3 legs, and every leg needs "
                 "a route, boarding stop, and transfer stop.\"}");
    return;
  }

  if (homeLegsPresent) configStore_.setPresetLegs(ConfigStore::PresetId::kHome, homeLegs);
  if (workLegsPresent) configStore_.setPresetLegs(ConfigStore::PresetId::kWork, workLegs);
  configStore_.setPresetWalkToFirstStopMin(ConfigStore::PresetId::kHome, doc["home"]["walkMin"] | 0);
  configStore_.setPresetWalkToFirstStopMin(ConfigStore::PresetId::kWork, doc["work"]["walkMin"] | 0);
  configStore_.setTransferBufferMin(doc["transferBufferMin"] | 3);
  settingsSaved_ = true;
  server_.send(200, "application/json", "{\"ok\":true}");
}

// See settingsFinished_'s comment in setup_flow.h for why the portal's
// close decision is driven by this explicit signal rather than
// settingsSaved_.
void SetupFlow::handleSettingsDone() {
  touchActivity();
  if (!requireSettingsMode()) return;
  settingsFinished_ = true;
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

// Unlike this file's other handlers, reachable from BOTH portal modes: the
// first-run wizard's own stop-pick step (unchanged, still resolves via the
// index-based /stopselect below) and the settings portal's preset leg-stop
// picker (setup_flow.cpp's kSettingsPageHtml openPicker()/applyPickedStop(),
// which reads globalStopId directly out of this response instead of a
// second select round-trip -- see the "results" loop below). Safe to widen
// unlike requireFirstRunMode()'s other guarded handlers: this makes no
// config writes and no credential/PII exposure, just proxies a Transit API
// search call the caller already has network access to trigger from either
// mode's open AP anyway (same physical-proximity access control either way,
// see this file's header comment) -- so there's nothing new being protected
// by restricting it to one mode.
void SetupFlow::handleStopSearch() {
  touchActivity();
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
    // Not a secret -- exposed so the settings portal's preset leg-stop
    // picker can resolve a pick entirely client-side (see the file comment
    // above) instead of needing its own index-based /select endpoint.
    item["stopId"] = lastStopResults_[i].globalStopId;
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
  settingsFinished_ = false;
  uint32_t finishedAtMs = 0;
  while (true) {
    dnsServer_.processNextRequest();
    server_.handleClient();

    // settingsFinished_ (set only by handleSettingsDone(), called once the
    // page's JS reaches step-done), not settingsSaved_: the settings page
    // is a multi-step flow now (orientation -> STA -> presets -> done), and
    // closing the portal right after the *first* individual save would tear
    // it down while the user is still on a later step -- see
    // settingsFinished_'s comment in setup_flow.h.
    if (settingsFinished_) {
      if (finishedAtMs == 0) {
        finishedAtMs = millis();
        renderEngine_.renderSetupPrompt("Settings saved", "Applying your changes...");
      } else if (millis() - finishedAtMs > kProvisionedLingerMs) {
        break;
      }
    } else if (millis() - lastActivityMs_ > kSetupIdleTimeoutMs) {
      renderEngine_.renderSetupPrompt(
          "Settings paused",
          settingsSaved_ ? "No activity on the settings page.\nSome changes were already saved."
                        : "No activity on the settings page.\nNo changes were made.");
      break;
    }

    delay(10);
  }

  stopPortal();
  portalMode_ = PortalMode::kFirstRun;
  return settingsSaved_;
}

}  // namespace transit
