#include "WebDashboard.h"
#include "Config.h"
#include "IRManager.h"
#include "SensorManager.h"
#include "Indicator.h"
#include <WiFi.h>
#include <AsyncMqttClient.h>
#include <WebServer.h>
#include <Update.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "ScheduleManager.h"
#include <nvs_flash.h>

// --- Externs (Grabbing from main.cpp) ---
extern Preferences preferences;
extern bool pendingReboot;
extern unsigned long rebootTime;
extern AsyncMqttClient mqttClient;

namespace WebDashboard
{
  // --- Private Variables ---
  static WebServer server(80);

  const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>ClimateIQ Setup</title>
<style>
  :root{--bg:#0f1724;--card:#111827;--text:#f8fafc;--acc:#3b82f6;--ok:#10b981;--err:#ef4444;--wait:#eab308;}
  *{box-sizing:border-box;}
  body{font-family:system-ui,-apple-system,sans-serif;background:var(--bg);color:var(--text);margin:0;padding:0;}
  .hdr{background:var(--card);border-bottom:2px solid #1e3a5f;padding:14px 24px;display:flex;align-items:center;justify-content:space-between;position:sticky;top:0;z-index:10;}
  .hdr-title{font-size:1.15rem;font-weight:700;color:var(--acc);letter-spacing:1px;}
  .hdr-sub{font-size:0.75rem;color:#64748b;margin-top:2px;}
  .wrap{padding:20px;}
  .grid{display:grid;grid-template-columns:300px 1fr 280px;gap:18px;max-width:1200px;margin:0 auto;}
  .card{background:var(--card);border-radius:12px;padding:20px;box-shadow:0 4px 12px rgba(0,0,0,0.25);border:1px solid #1f2937;}
  .card h3{margin-top:0;border-bottom:1px solid #1f2937;padding-bottom:10px;color:#64748b;font-size:0.78rem;text-transform:uppercase;letter-spacing:0.8px;}
  .sp{background:#000;padding:14px;border-radius:8px;border:1px solid #334155;font-family:monospace;}
  .sr{margin-bottom:11px;border-bottom:1px dashed #1e293b;padding-bottom:8px;}
  .sr:last-child{border-bottom:none;margin-bottom:0;padding-bottom:0;}
  .sl{font-size:0.72rem;color:#475569;text-transform:uppercase;letter-spacing:0.5px;margin-bottom:4px;}
  .badge{background:#1f2937;padding:4px 10px;border-radius:6px;display:inline-block;font-size:0.82rem;margin:2px;color:white;}
  .badge.yellow{background:var(--wait);color:#000;font-weight:bold;}
  .badge.green{background:var(--ok);color:white;font-weight:bold;}
  .badge.red{background:var(--err);color:white;font-weight:bold;}
  #alog{color:var(--ok);margin-top:5px;font-size:0.88rem;min-height:1.2em;}
  button{width:100%;padding:11px;margin-bottom:8px;border:none;border-radius:8px;font-weight:600;font-size:0.9rem;cursor:pointer;transition:opacity .15s;color:white;}
  button:last-child{margin-bottom:0;}
  button:active{opacity:.72;}
  button:disabled{opacity:.4;cursor:not-allowed;}
  .btn-p{background:var(--acc);}
  .btn-v{background:#6366f1;}
  .btn-d{background:var(--err);}
  .btn-w{background:#d97706;}
  .btn-t{background:#0d9488;}
  .btn-g2{display:grid;grid-template-columns:1fr 1fr;gap:8px;}
  .btn-g2 button{margin-bottom:0;}
  select,input[type=text],input[type=password]{width:100%;padding:10px;margin-bottom:12px;border-radius:6px;border:1px solid #334155;background:#1f2937;color:white;font-size:0.9rem;outline:none;}
  select:focus,input:focus{border-color:var(--acc);}
  .sw{margin:8px 0;}
  .sl2{display:flex;justify-content:space-between;font-size:0.75rem;color:#94a3b8;margin-bottom:3px;}
  .stk{background:#1f2937;border-radius:4px;height:20px;overflow:hidden;}
  .sf{height:100%;border-radius:4px;transition:width .35s ease;width:0%;}
  .sf.mv{background:linear-gradient(90deg,#2563eb,#60a5fa);}
  .sf.sx{background:linear-gradient(90deg,#7c3aed,#a78bfa);}
  .mbg{display:none;position:fixed;inset:0;background:rgba(0,0,0,.85);z-index:200;align-items:center;justify-content:center;}
  .mbox{background:var(--card);border-radius:14px;padding:28px;width:320px;border:1px solid #334155;}
  .mbox h3{margin:0 0 12px;color:var(--acc);font-size:1.05rem;}
  .dpw{max-width:1200px;margin:18px auto 0;display:none;}
  .dhdr{display:flex;justify-content:space-between;align-items:center;margin-bottom:14px;padding:0 2px;}
  .dhdr h2{margin:0;color:var(--acc);font-size:1.05rem;font-weight:700;}
  .dclose{background:var(--err);border:none;color:white;padding:7px 18px;border-radius:7px;cursor:pointer;font-weight:600;width:auto;margin:0;font-size:0.85rem;}
  .dgrid{display:grid;grid-template-columns:1fr 1fr 1fr;gap:18px;}
  .pr{display:flex;justify-content:space-between;align-items:center;padding:9px 0;border-bottom:1px solid #1f2937;}
  .pr:last-child{border-bottom:none;}
  .pn{font-size:0.85rem;color:var(--text);font-weight:500;}
  .ps{font-size:0.7rem;color:#475569;margin-top:2px;}
  .pi{background:#1f2937;border:1px solid #334155;border-radius:6px;padding:6px 8px;color:white;width:72px;text-align:center;font-size:0.88rem;outline:none;}
  .pi:focus{border-color:var(--acc);}
  .pu{font-size:0.75rem;color:#64748b;margin-left:4px;}
  .dr{display:flex;justify-content:space-between;padding:7px 0;border-bottom:1px solid #1f2937;font-size:0.85rem;}
  .dr:last-child{border-bottom:none;}
  .dk{color:#64748b;}
  .dv{color:var(--text);font-family:monospace;font-size:0.82rem;}
  @media(max-width:900px){.grid,.dgrid{grid-template-columns:1fr;}}
</style>
<script>
function sui(t,c){const e=document.getElementById('sm');e.innerText=t;e.className='badge '+c;}
async function refresh(idle){
  if(idle!==false)sui('System Ready','green');
  try{
    const d=await(await fetch('/status')).json();
    document.getElementById('spro').innerText=d.protocol||'None Detected';
    if(document.getElementById('sms'))document.getElementById('sms').value=d.mode;
    const wb=document.getElementById('swf');
    if(d.has_credentials){wb.innerText='Saved in Memory';wb.className='badge green';}
    else{wb.innerText='Missing / Empty';wb.className='badge yellow';}
    const kd=document.getElementById('sk');
    kd.innerHTML=d.keys.length>0?d.keys.map(k=>'<span class="badge">'+k+'</span>').join(''):'<span style="color:#64748b;">No Buttons Saved</span>';
  }catch(e){sui('Disconnected','red');}
}
async function doAct(a,ep){
  const lg=document.getElementById('alog');
  document.querySelectorAll('button').forEach(b=>b.disabled=true);
  sui('Listening...','yellow');
  lg.innerText='> Point remote & press '+a;lg.style.color='var(--wait)';
  try{
    const res=await fetch(ep);const txt=await res.text();
    lg.innerText='> '+txt;
    if(res.ok){lg.style.color='var(--ok)';sui('Success','green');}
    else{lg.style.color='var(--err)';sui('Failed','red');}
  }catch(e){lg.innerText='> Network Error!';lg.style.color='var(--err)';sui('Error','red');}
  document.querySelectorAll('button').forEach(b=>b.disabled=false);refresh(false);
  setTimeout(()=>{sui('System Ready','green');lg.innerText='> Ready';lg.style.color='var(--ok)';},3000);
}
async function doReset(ep){
  const lg=document.getElementById('alog');
  document.querySelectorAll('button').forEach(b=>b.disabled=true);
  sui('Wiping...','yellow');
  lg.innerText='> Wiping IR memory...';
  lg.style.color='var(--wait)';
  try{
    const res=await fetch(ep);const txt=await res.text();
    lg.innerText='> '+txt;
    if(res.ok){lg.style.color='var(--ok)';sui('Success','green');}
    else{lg.style.color='var(--err)';sui('Failed','red');}
  }catch(e){lg.innerText='> Network Error!';lg.style.color='var(--err)';sui('Error','red');}
  document.querySelectorAll('button').forEach(b=>b.disabled=false);refresh(false);
  setTimeout(()=>{sui('System Ready','green');lg.innerText='> Ready';lg.style.color='var(--ok)';},3000);
}
async function saveSt(){
  const m=document.getElementById('sms').value;
  const s=document.getElementById('ns').value;
  const p=document.getElementById('np').value;
  if(m==='wifi'&&!s)return alert('SSID cannot be empty for WiFi mode');
  if(confirm('Save settings and reboot device?')){
    await fetch('/setwifi?mode='+m+'&ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p),{method:'POST'});
    alert('Saved! Device is rebooting.');
  }
}
async function doCal(ep){
  const lg=document.getElementById('alog');
  document.querySelectorAll('button').forEach(b=>b.disabled=true);
  sui('Calibrating...','yellow');
  lg.innerText=ep.includes('auto')?'> EMPTY the room — calibration takes up to 120s...':'> Sending factory reset to radar...';
  lg.style.color='var(--wait)';
  try{
    const res=await fetch(ep);const txt=await res.text();
    lg.innerText='> '+txt;
    if(res.ok){lg.style.color='var(--ok)';sui('Done','green');}
    else{lg.style.color='var(--err)';sui('Failed','red');}
  }catch(e){lg.innerText='> Network Error!';lg.style.color='var(--err)';sui('Error','red');}
  document.querySelectorAll('button').forEach(b=>b.disabled=false);
  setTimeout(()=>{sui('System Ready','green');lg.innerText='> Ready';lg.style.color='var(--ok)';},5000);
}
var dpt=null;
function openDM(){document.getElementById('dm').style.display='flex';setTimeout(()=>document.getElementById('dpi').focus(),50);document.getElementById('de').style.display='none';}
function closeDM(){document.getElementById('dm').style.display='none';document.getElementById('dpi').value='';}
function dpk(e){if(e.key==='Enter')chkDP();}
async function chkDP(){
  var p=document.getElementById('dpi').value;
  try{
    var d=await(await fetch('/devauth?pass='+encodeURIComponent(p))).json();
    if(d.ok){closeDM();openDP();}else{document.getElementById('de').style.display='block';}
  }catch(e){document.getElementById('de').style.display='block';}
}
function openDP(){
  document.getElementById('dpanel').style.display='block';
  pollD();dpt=setInterval(pollD,1000);
  setTimeout(()=>document.getElementById('dpanel').scrollIntoView({behavior:'smooth'}),100);
}
function closeDP(){
  document.getElementById('dpanel').style.display='none';
  if(dpt){clearInterval(dpt);dpt=null;}
}
function buildGates(cid,arr,isMv){
  var clr=isMv?'linear-gradient(90deg,#2563eb,#60a5fa)':'linear-gradient(90deg,#7c3aed,#a78bfa)';
  var h='';
  for(var i=0;i<arr.length;i++){
    var v=arr[i],p=Math.min(v,100);
    h+='<div style="display:flex;align-items:center;gap:5px;margin-bottom:3px;">'
      +'<span style="font-family:monospace;font-size:0.68rem;color:#64748b;width:16px;flex-shrink:0;">G'+i+'</span>'
      +'<div style="flex:1;background:#0f172a;border-radius:3px;height:13px;overflow:hidden;">'
        +'<div style="width:'+p+'%;height:100%;background:'+clr+';border-radius:3px;transition:width .35s;"></div>'
      +'</div>'
      +'<span style="font-family:monospace;font-size:0.7rem;color:var(--text);width:24px;text-align:right;">'+v+'</span>'
    +'</div>';
  }
  document.getElementById(cid).innerHTML=h;
}
async function pollD(){
  try{
    var d=await(await fetch('/devdata')).json();
    if(d.mv&&d.mv.length)buildGates('mv-gates',d.mv,true);
    if(d.sx&&d.sx.length)buildGates('st-gates',d.sx,false);
    var pr=document.getElementById('dpres');
    pr.innerText=d.presence?'DETECTED':'EMPTY';
    pr.className='badge '+(d.presence?'green':'red');
    var ids=['inp-nt','inp-et','inp-etime','inp-otime','inp-fdelay'];
    var vals=[d.normal_temp,d.eco_temp,d.eco_time_min,d.off_time_min, d.flap_delay_sec];
    for(var i=0;i<ids.length;i++){var el=document.getElementById(ids[i]);if(el&&document.activeElement!==el)el.value=vals[i];}
    var u=d.uptime_s,h=Math.floor(u/3600),m=Math.floor((u%3600)/60),s=u%60;
    document.getElementById('dvh').innerText=(d.free_heap/1024).toFixed(1)+' KB';
    document.getElementById('dvu').innerText=h+'h '+m+'m '+s+'s';
    document.getElementById('dvr').innerText=d.radar_ready?'OK':'ERROR';
    document.getElementById('dvhdc').innerText=d.hdc_ok?'OK':'FAULT';
    document.getElementById('dva').innerText=d.radar_auto?'Enabled':'Disabled';
    document.getElementById('dvnvs').innerText = d.nvs_used_entries + ' / ' + d.nvs_total_entries;
  }catch(e){}
}
async function saveDP(){
  var n=document.getElementById('inp-nt').value;
  var e=document.getElementById('inp-et').value;
  var et=document.getElementById('inp-etime').value;
  var ot=document.getElementById('inp-otime').value;
  var fd=document.getElementById('inp-fdelay').value;
  if(parseInt(ot)<=parseInt(et))return alert('Off Time must be greater than Eco Time!');
  var res=await fetch('/setparams?normal_temp='+n+'&eco_temp='+e+'&eco_time='+et+'&off_time='+ot+'&flap_delay='+fd,{method:'POST'});
  alert(await res.text());
}
async function fetchSched() {
  const box = document.getElementById('dsched');
  box.innerText = "Reading NVS flash memory...";
  box.style.color = "var(--wait)";
  try {
    const res = await fetch('/devschedule');
    const j = await res.json();
    box.innerText = JSON.stringify(j, null, 2);
    box.style.color = "#a5b4fc"; // Reset to standard purple/blue
  } catch(e) {
    box.innerText = "Error: Failed to read flash memory.";
    box.style.color = "var(--err)";
  }
}
window.onload=function(){refresh();};
</script>
</head>
<body>
<div class="hdr">
  <div>
    <div class="hdr-title">ClimateIQ Controller</div>
    <div class="hdr-sub">AP Configuration Mode</div>
  </div>
  <div id="sm" class="badge green">Loading...</div>
</div>
<div class="wrap">
<div class="grid">

  <div class="card">
    <h3>System Overview</h3>
    <div class="sp">
      <div class="sr"><div class="sl">Status</div><div id="alog">&gt; Ready</div></div>
      <div class="sr"><div class="sl">WiFi Credentials</div><div id="swf" class="badge">Checking...</div></div>
      <div class="sr"><div class="sl">AC Protocol</div><div id="spro" style="color:var(--acc);font-size:1.0rem;font-weight:bold;">Loading...</div></div>
      <div class="sr" style="border:none;"><div class="sl">Saved Buttons</div><div id="sk">Loading...</div></div>
    </div>
  </div>

  <div class="card">
    <h3>IR Learning Center</h3>
    <button class="btn-p" style="margin-bottom:16px;padding:14px;" onclick="doAct('ANY Button','/learn/protocol')">Detect AC Protocol</button>
    <div style="font-size:0.72rem;color:#64748b;margin-bottom:8px;text-transform:uppercase;letter-spacing:.5px;">Custom Buttons</div>
    <div class="btn-g2">
      <button class="btn-v" onclick="doAct('ON','/learn/on')">Learn ON</button>
      <button class="btn-v" onclick="doAct('OFF','/learn/off')">Learn OFF</button>
      <button class="btn-v" style="background:#7c3aed;" onclick="doAct('24C','/learn/24')">Learn 24&#176;C</button>
      <button class="btn-v" style="background:#7c3aed;" onclick="doAct('26C','/learn/26')">Learn 26&#176;C</button>
      <button class="btn-v" style="background:#5b21b6;" onclick="doAct('28C','/learn/28')">Learn 28&#176;C</button>
      <button class="btn-v" style="background:#5b21b6;" onclick="doAct('30C','/learn/30')">Learn 30&#176;C</button>
    </div>
  </div>

  <div class="card">
    <h3>Network Settings</h3>
    <div class="sl" style="margin-bottom:4px;">System Mode</div>
    <select id="sms"><option value="wifi">WiFi Mode</option><option value="gsm">GSM Mode</option></select>
    <div class="sl" style="margin-bottom:4px;">WiFi SSID</div>
    <input type="text" id="ns" placeholder="Enter Router Name">
    <div class="sl" style="margin-bottom:4px;">WiFi Password</div>
    <input type="password" id="np" placeholder="Enter Password" style="margin-bottom:0;">
    <button class="btn-w" style="margin-top:12px;" onclick="saveSt()">Save &amp; Reboot</button>
  </div>

  <div class="card">
    <h3>System Admin</h3>
    
    <!-- Danger Zone Group -->
    <div style="font-size:0.72rem; color:#64748b; margin-bottom:8px; text-transform:uppercase; letter-spacing:.5px;">Danger Zone</div>
    <button class="btn-d" onclick="if(confirm('Wipe all saved IR data?'))doReset('Reset','/reset')">Reset IR Memory</button>
    <button style="background:#450a0a; color:#f87171; border:1px solid #7f1d1d; margin-bottom:20px;" onclick="if(confirm('Wipe WiFi and Reboot?'))doAct('Wipe WiFi','/resetwifi')">Wipe WiFi Credentials</button>
    
    <!-- System Tools Group -->
    <div style="font-size:0.72rem; color:#64748b; margin-bottom:8px; text-transform:uppercase; letter-spacing:.5px;">System Tools</div>
    <button class="btn-v" onclick="window.location.href='/update'">OTA Firmware Update</button>
    <button style="background:#1e293b; color:#cbd5e1; border:1px solid #334155;" onclick="openDM()">&#128295; Developer Mode</button>
  </div>

  <div class="card">
    <h3>Radar Calibration</h3>
    <p style="font-size:0.82rem;color:#94a3b8;margin-top:0;line-height:1.6;">
      <b style="color:var(--text);">Auto-Calibrate:</b> Room must be completely empty. Sensor learns ambient noise floor and sets thresholds automatically (firmware &ge; 2.44). Up to 120 seconds.<br><br>
      <b style="color:var(--text);">Factory Reset:</b> Wipes learned thresholds back to HLK defaults. Use if auto-calibrate produces false triggers.
    </p>
    <button class="btn-p" onclick="if(confirm('EMPTY the room first. Continue?'))doCal('/calibrate/auto')">Auto-Calibrate (Recommended)</button>
    <button class="btn-w" onclick="if(confirm('Reset radar to factory defaults?'))doCal('/calibrate/reset')">Factory Reset Radar</button>
  </div>

</div>

<div class="dpw" id="dpanel">
  <div class="dhdr">
    <h2>&#128295; Developer Mode</h2>
    <button class="dclose" onclick="closeDP()">&#128274; Lock &amp; Close</button>
  </div>
  <div class="dgrid">
    <div class="card">
      <h3>Radar Live Feed</h3>
      <div style="margin-bottom:14px;">
        <div class="sl">Presence</div>
        <div id="dpres" class="badge red">EMPTY</div>
      </div>
      <div style="margin-top:8px;">
        <div class="sl" style="margin-bottom:5px;">Moving Energy (per gate)</div>
        <div id="mv-gates"><span style="color:#475569;font-size:0.8rem;">Waiting for data...</span></div>
      </div>
      <div style="margin-top:12px;">
        <div class="sl" style="margin-bottom:5px;">Stationary Energy (per gate)</div>
        <div id="st-gates"><span style="color:#475569;font-size:0.8rem;">Waiting for data...</span></div>
      </div>
    </div>
    <div class="card">
      <h3>Automation Parameters</h3>
      <div class="pr">
        <div><div class="pn">Normal Temp</div><div class="ps">AC ON set point</div></div>
        <div style="display:flex;align-items:center;"><input type="number" class="pi" id="inp-nt" min="16" max="32" value="24"><span class="pu">&#176;C</span></div>
      </div>
      <div class="pr">
        <div><div class="pn">Eco Temp</div><div class="ps">Raised when room is empty</div></div>
        <div style="display:flex;align-items:center;"><input type="number" class="pi" id="inp-et" min="16" max="32" value="26"><span class="pu">&#176;C</span></div>
      </div>
      <div class="pr">
        <div><div class="pn">Eco Delay</div><div class="ps">Minutes before eco mode</div></div>
        <div style="display:flex;align-items:center;"><input type="number" class="pi" id="inp-etime" min="1" max="120" value="2"><span class="pu">min</span></div>
      </div>
      <div class="pr" style="border:none;">
        <div><div class="pn">Off Delay</div><div class="ps">Minutes before AC turns off</div></div>
        <div style="display:flex;align-items:center;"><input type="number" class="pi" id="inp-otime" min="2" max="240" value="5"><span class="pu">min</span></div>
      </div>
      <div class="pr" style="border:none;">
        <div><div class="pn">Flap Delay</div><div class="ps">Ignore radar after OFF</div></div>
        <div style="display:flex;align-items:center;"><input type="number" class="pi" id="inp-fdelay" min="0" max="120" value="10"><span class="pu">sec</span></div>
      </div>
      <button class="btn-p" style="margin-top:14px;" onclick="saveDP()">Save Parameters</button>
    </div>
    <div class="card">
      <h3>System Diagnostics</h3>
      <div class="dr"><span class="dk">Free Heap</span><span class="dv" id="dvh">&#8212;</span></div>
      <div class="dr"><span class="dk">Uptime</span><span class="dv" id="dvu">&#8212;</span></div>
      <div class="dr"><span class="dk">Radar Sensor</span><span class="dv" id="dvr">&#8212;</span></div>
      <div class="dr"><span class="dk">HDC1080 Sensor</span><span class="dv" id="dvhdc">&#8212;</span></div>
      <div class="dr"><span class="dk">Radar Auto Mode</span><span class="dv" id="dva">&#8212;</span></div>
      <div class="dr"><span class="dk">NVS Flash Used</span><span class="dv" id="dvnvs">&#8212;</span></div>
    </div>
    <div class="card" style="grid-column: 1 / -1;">
      <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:10px;">
        <h3 style="border:none; margin:0; padding:0;">Flash Memory: Local Schedule Dump</h3>
        <button class="btn-v" style="width:auto; padding:5px 12px; margin:0;" onclick="fetchSched()">Load from Flash</button>
      </div>
      <pre id="dsched" style="background:#0f172a; padding:12px; border-radius:6px; font-size:0.75rem; color:#a5b4fc; max-height:250px; overflow-y:auto; margin:0; border:1px solid #1e293b;">Click 'Load from Flash' to view active memory...</pre>
    </div>
  </div>
</div>

</div>

<div class="mbg" id="dm">
  <div class="mbox">
    <h3>&#128295; Developer Access</h3>
    <div style="font-size:0.82rem;color:#94a3b8;margin-bottom:14px;">Enter the developer password to unlock advanced settings and live radar diagnostics.</div>
    <input type="password" id="dpi" placeholder="Developer password" onkeydown="dpk(event)" style="margin-bottom:6px;">
    <div id="de" style="display:none;color:var(--err);font-size:0.82rem;margin-bottom:8px;">&#10007; Incorrect password. Try again.</div>
    <button class="btn-p" onclick="chkDP()">Unlock</button>
    <button class="btn-d" onclick="closeDM()" style="margin-top:6px;">Cancel</button>
  </div>
</div>

</body>
</html>
)rawliteral";

  // --- Private Route Setup ---
  static void setupRoutes()
  {
    // Require Admin Login for Dashboard
    server.on("/", HTTP_GET, []()
              { 
            if (!server.authenticate(WWW_USERNAME, WWW_PASSWORD)) {
                return server.requestAuthentication();
            }
            server.send(200, "text/html; charset=utf-8", FPSTR(DASHBOARD_HTML)); });

    server.on("/setwifi", HTTP_POST, []()
              {
            if (!server.authenticate(WWW_USERNAME, WWW_PASSWORD)) return server.requestAuthentication();
            
            if (server.hasArg("mode")) {
                String modeStr = server.arg("mode");
                bool isWiFi = (modeStr == "wifi");
                String newSSID = server.hasArg("ssid") ? server.arg("ssid") : "";
                String newPass = server.hasArg("pass") ? server.arg("pass") : "";

                if (isWiFi && newSSID.length() == 0) {
                    server.send(400, "text/plain", "Error: No SSID provided for WiFi mode!");
                    return;
                }

                Serial.println("\n[SYSTEM] Saving New Settings:");
                Serial.println("Mode: " + modeStr);
                Serial.println("SSID: " + newSSID);
                
                preferences.putBool("use_wifi", isWiFi);
                preferences.putString("wifi_ssid", newSSID);
                preferences.putString("wifi_pass", newPass);
                
                server.send(200, "text/plain", "Settings Saved. Rebooting...");
                pendingReboot = true;
                rebootTime = millis() + 2000;
            } else {
                server.send(400, "text/plain", "Error: Incomplete request!");
            } });

    server.onNotFound([]()
                      { server.send(404, "text/plain", "Not Found"); });

    server.on("/status", HTTP_GET, []()
              {
            JsonDocument doc;
            doc["protocol"] = preferences.isKey("protocol_name") ? preferences.getString("protocol_name") : "None";
            
            JsonArray keys = doc["keys"].to<JsonArray>();
            auto addKey = [&](const char* keyId, const char* name) {
                if (preferences.getBool((String("has_") + keyId).c_str(), false)) keys.add(name);
            };
            
            addKey("ir_on", "ON"); addKey("ir_off", "OFF");
            addKey("ir_24", "24\xC2\xB0""C"); addKey("ir_26", "26\xC2\xB0""C");
            addKey("ir_28", "28\xC2\xB0""C"); addKey("ir_30", "30\xC2\xB0""C");

            doc["has_credentials"] = (preferences.getString("wifi_ssid", "").length() > 0);
            doc["mode"] = sysData.switch_gsm_wifi ? "wifi" : "gsm";
            
            String json;
            serializeJson(doc, json);
            server.send(200, "application/json; charset=utf-8", json); });

    // --- IR Learning Helpers ---
    auto handleLearn = [](const char *key, bool isProtocol)
    {
      int result = IRManager::learnCommand(key, isProtocol);
      if (result == 0)
      {
        Indicator::indicateSuccess();
        server.send(200, "text/plain", String(key) + " Saved Successfully!");
      }
      else if (result == 1)
      {
        Indicator::indicateError();
        server.send(408, "text/plain", "Timeout: No remote signal detected.");
      }
      else
      {
        Indicator::indicateError();
        server.send(400, "text/plain", "Error: Protocol not recognized.");
      }
    };

    server.on("/learn/protocol", HTTP_GET, [handleLearn]()
              { handleLearn("ir_protocol", true); });
    server.on("/learn/on", HTTP_GET, [handleLearn]()
              { handleLearn("ir_on", false); });
    server.on("/learn/off", HTTP_GET, [handleLearn]()
              { handleLearn("ir_off", false); });
    server.on("/learn/24", HTTP_GET, [handleLearn]()
              { handleLearn("ir_24", false); });
    server.on("/learn/26", HTTP_GET, [handleLearn]()
              { handleLearn("ir_26", false); });
    server.on("/learn/28", HTTP_GET, [handleLearn]()
              { handleLearn("ir_28", false); });
    server.on("/learn/30", HTTP_GET, [handleLearn]()
              { handleLearn("ir_30", false); });

    server.on("/reset", HTTP_GET, []()
              {
            IRManager::wipeMemory(); // Uses your nice new clean function!
            Indicator::indicateSuccess(); delay(100); Indicator::indicateSuccess();
            server.send(200, "text/plain", "IR Memory Wiped. WiFi & Settings Preserved."); });

    server.on("/resetwifi", HTTP_GET, []()
              {
            preferences.remove("wifi_ssid");
            preferences.remove("wifi_pass");
            Indicator::indicateSuccess(); delay(100); Indicator::indicateSuccess();
            server.send(200, "text/plain", "WiFi Credentials Wiped. Rebooting to AP mode...");
            pendingReboot = true;
            rebootTime = millis() + 2000; });

    // Sensor Endpoints
    server.on("/calibrate/auto", HTTP_GET, []()
              { SensorManager::calibrateRadarAuto(); });
    server.on("/calibrate/reset", HTTP_GET, []()
              { SensorManager::calibrateRadarReset(); });

    server.on("/devauth", HTTP_GET, []()
              {
            String pass = server.hasArg("pass") ? server.arg("pass") : "";
            JsonDocument doc;
            doc["ok"] = (pass == String(DEV_PASSWORD));
            String json;
            serializeJson(doc, json);
            server.send(200, "application/json; charset=utf-8", json); });

    server.on("/devdata", HTTP_GET, []()
              {
            JsonDocument doc;
            // --- ADD NVS HEALTH CHECK ---
        nvs_stats_t nvs_stats;
        nvs_get_stats(NULL, &nvs_stats);
        doc["nvs_used_entries"] = nvs_stats.used_entries;
        doc["nvs_free_entries"] = nvs_stats.free_entries;
        doc["nvs_total_entries"] = nvs_stats.total_entries;
        // ----------------------------
            if (sysData.sensorReady) {
                const MyLD2410::ValuesArray& mvSig = SensorManager::getMovingSignals(); 
                const MyLD2410::ValuesArray& stSig = SensorManager::getStationarySignals(); 
                JsonArray mvArr = doc["mv"].to<JsonArray>();
                JsonArray stArr = doc["sx"].to<JsonArray>();
                for (int i = 0; i <= mvSig.N; i++) mvArr.add((int)mvSig.values[i]);
                for (int i = 0; i <= stSig.N; i++) stArr.add((int)stSig.values[i]);
            } else {
                doc["mv"].to<JsonArray>();
                doc["sx"].to<JsonArray>();
            }
            doc["presence"]     = sysData.cachedPresence;
            doc["radar_ready"]  = sysData.sensorReady;
            doc["radar_auto"]   = sysData.radarAutoMode;
            doc["normal_temp"]  = sysData.currentNormalTemp;
            doc["eco_temp"]     = sysData.currentEcoTemp;
            doc["eco_time_min"] = (int)(sysData.TEcoTime / 60000);
            doc["off_time_min"] = (int)(sysData.TOffTime / 60000);
            doc["flap_delay_sec"] = sysData.flapDelaySec; // <--- ADD THIS
            doc["free_heap"]    = (int)ESP.getFreeHeap();
            doc["uptime_s"]     = (int)(millis() / 1000);
            doc["hdc_ok"]       = !sysData.hdcInitFailed;
            String json;
            serializeJson(doc, json);
            server.send(200, "application/json; charset=utf-8", json); });

    server.on("/setparams", HTTP_POST, []()
              {
            bool changed = false;
            if (server.hasArg("normal_temp")) {
                sysData.currentNormalTemp = constrain(server.arg("normal_temp").toInt(), 16, 32);
                preferences.putInt("normal_temp", sysData.currentNormalTemp);
                changed = true;
            }
            if (server.hasArg("eco_temp")) {
                sysData.currentEcoTemp = constrain(server.arg("eco_temp").toInt(), 16, 32);
                preferences.putInt("eco_temp", sysData.currentEcoTemp);
                changed = true;
            }
            if (server.hasArg("eco_time")) {
                sysData.TEcoTime = (unsigned long)constrain(server.arg("eco_time").toInt(), 1, 120) * 60000;
                preferences.putULong("eco_time", sysData.TEcoTime);
                changed = true;
            }
            if (server.hasArg("off_time")) {
                unsigned long newOff = (unsigned long)constrain(server.arg("off_time").toInt(), 2, 240) * 60000;
                if (newOff <= sysData.TEcoTime) newOff = sysData.TEcoTime + 60000;
                sysData.TOffTime = newOff;
                preferences.putULong("off_time", sysData.TOffTime);
                changed = true;
            }
            if (server.hasArg("flap_delay")) {
                sysData.flapDelaySec = (unsigned long)constrain(server.arg("flap_delay").toInt(), 0, 120);
                preferences.putULong("flap_delay", sysData.flapDelaySec);
                changed = true;
            }
            if (changed) {
                Serial.printf("[DEV] Params saved — Normal:%d°C Eco:%d°C TEco:%lums TOff:%lums\n",
                    sysData.currentNormalTemp, sysData.currentEcoTemp, sysData.TEcoTime, sysData.TOffTime);
                server.send(200, "text/plain", "Parameters saved successfully.");
            } else {
                server.send(400, "text/plain", "No valid parameters provided.");
            } });

    server.on("/devschedule", HTTP_GET, []()
              {
            JsonDocument doc; 
            const char* days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
            bool hasAnyData = false;

            for (int w = 0; w < 7; w++) {
                uint8_t count = ScheduleManager::loadSegmentCount(w);
                if (count > 0) {
                    hasAnyData = true;
                    JsonArray dayArr = doc[days[w]].to<JsonArray>();
                    for (int i = 0; i < count; i++) {
                        ScheduleSegment seg;
                        if (ScheduleManager::loadSegment(w, i, seg)) {
                            JsonObject s = dayArr.add<JsonObject>();
                            char startStr[6]; char endStr[6];
                            snprintf(startStr, sizeof(startStr), "%02d:%02d", seg.startMin / 60, seg.startMin % 60);
                            snprintf(endStr, sizeof(endStr), "%02d:%02d", seg.endMin / 60, seg.endMin % 60);
                            s["time"] = String(startStr) + " to " + String(endStr);
                            s["temp"] = seg.temp;
                            s["radar"] = (seg.radar == 1) ? "ON" : (seg.radar == 2) ? "OFF" : "Unset";
                        }
                    }
                }
            }
            if (!hasAnyData) doc["status"] = "No schedules currently saved in flash.";
            String json;
            serializeJson(doc, json);
            server.send(200, "application/json; charset=utf-8", json); });

    server.on("/update", HTTP_GET, []()
              { server.send(200, "text/html", "<form method='POST' action='/update' enctype='multipart/form-data'><input type='file' name='update'><input type='submit' value='Upload'></form>"); });
    server.on("/update", HTTP_POST, []()
              {
            if (!server.authenticate(WWW_USERNAME, WWW_PASSWORD)) return server.requestAuthentication();
            server.send(200, "text/plain", Update.hasError() ? "OTA FAILED" : "OTA SUCCESS - Rebooting");
            delay(1000); ESP.restart(); }, []()
              {
            HTTPUpload& upload = server.upload();
            if (upload.status == UPLOAD_FILE_START) { Update.begin(UPDATE_SIZE_UNKNOWN); } 
            else if (upload.status == UPLOAD_FILE_WRITE) { Update.write(upload.buf, upload.currentSize); } 
            else if (upload.status == UPLOAD_FILE_END) { Update.end(true); } });
  }

  // --- Public Functions ---
  void init()
  {
    setupRoutes();
  }

  void startAPMode()
  {
    if (sysData.isAPMode)
      return;
    sysData.currentState = SYS_AP_MODE;
    Serial.println("\n--- SWITCHING TO AP (DASHBOARD) MODE ---");

    if (sysData.switch_gsm_wifi)
    {
      mqttClient.disconnect();
      WiFi.disconnect(true);
    }

    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    Serial.println("✓ HOTSPOT ACTIVE: Connect to " + String(AP_SSID));

    server.begin();
    sysData.isAPMode = true;
    Indicator::indicateSuccess();
  }

  void stopAPMode()
  {
    if (!sysData.isAPMode)
      return;
    Serial.println("\n--- SWITCHING TO NORMAL MODE ---");

    server.stop();
    WiFi.softAPdisconnect(true);

    if (sysData.switch_gsm_wifi)
      WiFi.mode(WIFI_STA);
    else
      WiFi.mode(WIFI_OFF);

    sysData.isAPMode = false;
    Indicator::indicateSuccess();
    delay(100);
    Indicator::indicateSuccess();
  }

  // 1. Define the task privately inside the cpp file
  void TaskWeb(void *pvParameters)
  {
    for (;;)
    {
      if (sysData.isAPMode)
      {
        handleClient();
      }
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }

  void handleClient()
  {
    server.handleClient();
  }
}