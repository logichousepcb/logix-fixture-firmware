#pragma once
#include <Arduino.h>

// Single-page web UI served at "/". Talks only to the JSON API in web.cpp.
static const char WEB_UI[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>LoGiX Fixture</title>
<style>
:root{--bg:#0f1115;--card:#181b22;--line:#2a2f3a;--tx:#e8eaf0;--mu:#8b93a7;--ac:#4f8cff;--ok:#2ecc71;--bad:#ff6b6b;--warn:#f5a623}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--tx);font:15px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
main{max-width:560px;margin:0 auto;padding:16px}
header{display:flex;align-items:center;justify-content:space-between;gap:12px;margin:4px 0 14px}
h1{font-size:20px;margin:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
h2{font-size:15px;margin:0 0 8px}
.pill{flex:none;font-size:12px;padding:3px 10px;border-radius:99px;background:var(--line);color:var(--mu)}
.pill.ok{background:#173b27;color:var(--ok)}.pill.warn{background:#3d2e12;color:var(--warn)}
.pills{flex:none;display:flex;gap:6px;align-items:center}
a.pill.upd{background:#3d2e12;color:var(--warn);text-decoration:none;font-weight:600}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:16px;margin-bottom:14px}
.row{display:flex;align-items:center;gap:12px;margin:12px 0}
.row>label{flex:0 0 96px;color:var(--mu);font-size:13px}
.row>input:not([type=color]),.row>select{flex:1;min-width:0}
input,select,button{font:inherit;color:var(--tx)}
input[type=text],input[type=password],input[type=number],select{background:#10131a;border:1px solid var(--line);border-radius:9px;padding:9px 10px;width:100%}
input[type=range]{width:100%;accent-color:var(--ac)}
input[type=color]{width:52px;height:38px;border:1px solid var(--line);border-radius:9px;background:none;padding:2px}
button{background:var(--ac);border:0;border-radius:9px;padding:10px 14px;cursor:pointer;font-weight:600}
button.sec{background:var(--line)}button.danger{background:#5a1f1f;color:#ffc2c2}
button:disabled{opacity:.5;cursor:default}
.top{display:flex;align-items:center;gap:16px}
.power{flex:none;width:64px;height:64px;border-radius:50%;font-size:26px;background:var(--line);color:var(--mu)}
.power.on{background:var(--ac);color:#fff;box-shadow:0 0 24px #4f8cff55}
.grow{flex:1;min-width:0}
.sw{display:flex;flex-wrap:wrap;gap:8px}
.sw b{width:30px;height:30px;border-radius:50%;cursor:pointer;border:2px solid #ffffff22}
.tabs{display:flex;gap:4px;overflow-x:auto;margin:-4px 0 12px}
.tabs button{background:none;color:var(--mu);padding:7px 11px;border-radius:8px;font-weight:500}
.tabs button.act{background:var(--line);color:var(--tx)}
.tab{display:none}.tab.act{display:block}
.nets{max-height:230px;overflow:auto;border:1px solid var(--line);border-radius:9px;margin:10px 0}
.net{display:flex;justify-content:space-between;gap:8px;padding:9px 11px;cursor:pointer;border-bottom:1px solid var(--line)}
.net:last-child{border-bottom:0}.net:hover{background:#ffffff08}.net span{color:var(--mu);font-size:13px;flex:none}
.hint{color:var(--mu);font-size:12.5px;margin:6px 0}
.msg{font-size:13.5px;margin-top:10px;min-height:18px}.msg.ok{color:var(--ok)}.msg.bad{color:var(--bad)}
.banner{background:#3d2e12;color:#ffd58a;border-radius:12px;padding:12px 14px;margin-bottom:14px;font-size:14px}
progress{width:100%;height:10px;margin-top:10px}
dl{display:grid;grid-template-columns:auto 1fr;gap:5px 14px;font-size:13px;margin:0}
dt{color:var(--mu)}dd{margin:0;word-break:break-all}
.actions{display:flex;gap:8px;flex-wrap:wrap;margin-top:14px}
a{color:var(--ac)}
code{background:#10131a;padding:1px 5px;border-radius:5px;font-size:12.5px}
.ttl{display:flex;align-items:center;gap:10px;min-width:0}
#hicon{flex:none;height:34px;max-width:96px;object-fit:contain}
#micon{display:block;max-height:110px;max-width:100%;margin:4px auto 2px}
</style></head><body><main>
<header><span class="ttl"><img id="hicon" alt="" hidden><h1 id="title">LoGiX Fixture</h1></span><span class="pills"><a id="upd" class="pill upd" href="#" hidden></a><span id="pill" class="pill">…</span></span></header>
<div id="banner" class="banner" hidden>Setup mode: choose your Wi&#8209;Fi network below to put this fixture on your network.</div>

<section class="card" id="ctl">
  <div class="top">
    <button id="pw" class="power" title="Power" aria-label="Power">&#x23FB;</button>
    <div class="grow"><div class="hint">Brightness <span id="briv"></span></div><input id="bri" type="range" min="1" max="255"></div>
  </div>
  <div class="row"><label>Color</label><input id="col" type="color"><div class="sw grow" id="sw"></div></div>
  <div class="row"><label>Effect</label><select id="fx"></select></div>
  <div class="row"><label>Speed</label><input id="spd" type="range" min="0" max="255"></div>
  <div class="hint" id="ma"></div>
</section>

<section class="card">
  <nav class="tabs" id="tabs">
    <button data-t="wifi">Wi&#8209;Fi</button><button data-t="dev">Device</button><button data-t="hw">Hardware</button><button data-t="fw">Firmware</button><button data-t="info">Info</button>
  </nav>

  <div class="tab" id="t-wifi">
    <div class="hint" id="wcur"></div>
    <button class="sec" id="scan">Scan for networks</button>
    <div class="nets" id="nets" hidden></div>
    <div class="row"><label>Network</label><input id="ssid" type="text" autocomplete="off" placeholder="SSID"></div>
    <div class="row"><label>Password</label><input id="pass" type="password" placeholder="leave blank for open network"></div>
    <div class="actions"><button id="wsave">Connect</button><button class="danger" id="wforget">Forget network</button></div>
    <div class="msg" id="wmsg"></div>
  </div>

  <div class="tab" id="t-dev">
    <div class="row"><label>Name</label><input id="name" type="text" maxlength="32"></div>
    <div class="hint">Hostname: <code id="hostprev"></code>. The setup hotspot is called "<span id="appprev"></span>".</div>
    <div class="row"><label>Setup AP password</label><input id="appass" type="password" placeholder="blank = open hotspot (8+ chars)"></div>
    <div class="actions"><button id="dsave">Save</button><button class="sec" id="ident">Identify (blink)</button></div>
    <div class="msg" id="dmsg"></div>
    <h2 style="margin-top:22px">API token</h2>
    <div class="hint" id="tokstate"></div>
    <div id="tokbox" hidden><div class="row"><code id="tokval" style="flex:1;word-break:break-all;padding:8px"></code></div><div id="tokqr" style="background:#fff;padding:10px;border-radius:10px;width:max-content"></div></div>
    <div class="actions"><button class="sec" id="tokgen">Generate new token</button><button class="danger" id="tokoff">Turn off</button></div>
    <div class="msg" id="tmsg"></div>
  </div>

  <div class="tab" id="t-hw">
    <div class="row"><label>Model</label><select id="model"></select></div>
    <img id="micon" alt="" hidden>
    <div class="hint" id="mhint" hidden></div>
    <div class="row"><label>LED data GPIO</label><select id="ledpin"></select></div>
    <div class="row"><label>LED count</label><input id="ledcount" type="number" min="1" max="800"></div>
    <div class="row"><label>Color order</label><select id="order"><option>GRB</option><option>RGB</option><option>BRG</option><option>RBG</option><option>GBR</option><option>BGR</option></select></div>
    <div class="row"><label>IR receiver GPIO</label><select id="irpin"></select></div>
    <div class="row"><label>Power limit (mA)</label><input id="maxma" type="number" min="0" max="20000" step="50"></div>
    <div class="hint">Power limit is the total current budget for the board plus LEDs (0 = no limit). A USB 2.0 port gives about 500&nbsp;mA, a USB charger usually 1000&ndash;2400&nbsp;mA.</div>
    <div class="actions"><button id="hsave">Save &amp; reboot</button></div>
    <div class="msg" id="hmsg"></div>
  </div>

  <div class="tab" id="t-fw">
    <div class="hint">Installed: <b id="fwver"></b> <span id="fwbuild"></span></div>
    <div class="hint" id="fwlatest" hidden></div>
    <div class="row"><input id="fwfile" type="file" accept=".bin,.gz"></div>
    <div class="actions"><button id="fwup">Upload &amp; install</button></div>
    <progress id="fwprog" value="0" max="100" hidden></progress>
    <div class="msg" id="fwmsg"></div>
    <div class="hint">Use a <code>-4mb.bin</code> release file (<code>-1mb.bin</code> for 1&nbsp;MB modules) or <code>.pio/build/usb/firmware.bin</code> from a PlatformIO build. Settings are kept.</div>
  </div>

  <div class="tab" id="t-info">
    <dl id="dl"></dl>
    <div class="hint">API: <a href="/api/info">/api/info</a> · <a href="/api/state">/api/state</a> · <a href="/api/effects">/api/effects</a> · <a href="/api/config">/api/config</a></div>
    <div class="actions"><button class="sec" id="reboot">Reboot</button><button class="danger" id="reset">Factory reset</button></div>
    <div class="msg" id="imsg"></div>
  </div>
</section>
</main>
<script>
const $=id=>document.getElementById(id);
let S={},I={},C={},pending={},timer=null,lastSend=0;
const SW=['#ff0000','#ff6a00','#ffc400','#00ff40','#00d0ff','#0040ff','#a000ff','#ff00a0','#ffffff','#ff9329'];
const LEDP=[[2,'GPIO2 – LED headers (board default)'],[4,'GPIO4 – IR receiver on this board'],[5,'GPIO5'],[12,'GPIO12'],[13,'GPIO13'],[14,'GPIO14'],[0,'GPIO0 – must be HIGH at boot'],[15,'GPIO15 – must be LOW at boot'],[3,'GPIO3/RX – disables serial'],[1,'GPIO1/TX – disables serial']];
// Fixture models: id, label, preset LED count (0 = keep whatever is set), icon.
const MODELS=[['custom','Custom / other',0,''],['string','String Lights',0,'/icon/string.webp'],['snowflake91','91 Pixel Snowflake',91,'/icon/snowflake91.webp']];
const model=id=>MODELS.find(m=>m[0]===id)||MODELS[0];
function showIcon(el,id){const m=model(id);el.hidden=!m[3];if(m[3])el.src=m[3];else el.removeAttribute('src')}
const IRP=[[-1,'Disabled'],[4,'GPIO4 – TSOP38238 (board default)'],[5,'GPIO5'],[12,'GPIO12'],[13,'GPIO13'],[14,'GPIO14'],[0,'GPIO0'],[2,'GPIO2'],[3,'GPIO3/RX – disables serial'],[1,'GPIO1/TX – disables serial']];

let TOK='';try{TOK=localStorage.getItem('logixToken')||''}catch(e){}
function setTok(t){TOK=t||'';try{TOK?localStorage.setItem('logixToken',TOK):localStorage.removeItem('logixToken')}catch(e){}}
let asking=null;
async function api(p,o){o=o||{};o.headers=Object.assign({},o.headers||{},TOK?{Authorization:'Bearer '+TOK}:{});
  const r=await fetch(p,o);let j={};try{j=await r.json()}catch(e){}
  if(r.status===401){ // token required: ask once, then retry
    if(!asking)asking=new Promise(res=>{const t=prompt('This fixture requires an API token. Enter it (Device tab shows it once generated):',TOK);res(t)});
    const t=await asking;asking=null;if(t&&t!==TOK){setTok(t.trim());return api(p,o)}
    throw new Error('API token required')}
  if(!r.ok)throw new Error(j.error||('HTTP '+r.status));return j}
const post=(p,b)=>api(p,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b||{})});
function msg(id,t,cls){const e=$(id);e.textContent=t;e.className='msg '+(cls||'')}
function opts(sel,list){sel.innerHTML=list.map(([v,t])=>`<option value="${v}">${t}</option>`).join('')}
const host=n=>{let h=n.toLowerCase().replace(/[^a-z0-9]+/g,'-').replace(/^-+|-+$/g,'').slice(0,31).replace(/-+$/,'');return h||'logix-fixture'};

// ---- control
function send(o){Object.assign(pending,o);Object.assign(S,o);paint(true);if(timer)return;
  const wait=Math.max(0,120-(Date.now()-lastSend));
  timer=setTimeout(async()=>{const b=pending;pending={};timer=null;lastSend=Date.now();
    try{S=await post('/api/state',b);paint()}catch(e){}},wait)}
function paint(local){
  $('pw').classList.toggle('on',!!S.on);
  const a=document.activeElement;
  if(a!==$('bri'))$('bri').value=S.bri;
  $('briv').textContent=Math.round(S.bri/2.55)+'%';
  if(!local&&S.hex&&a!==$('col'))$('col').value=S.hex;
  if(a!==$('fx'))$('fx').value=S.effect;
  if(a!==$('spd'))$('spd').value=S.speed;
}
$('pw').onclick=()=>send({on:!S.on});
$('bri').oninput=e=>send({on:true,bri:+e.target.value});
$('col').oninput=e=>send({on:true,color:e.target.value});
$('fx').onchange=e=>send({on:true,effect:+e.target.value});
$('spd').oninput=e=>send({speed:+e.target.value});
$('sw').innerHTML=SW.map(c=>`<b style="background:${c}" data-c="${c}"></b>`).join('');
$('sw').onclick=e=>{const c=e.target.dataset.c;if(c){$('col').value=c;send({on:true,color:c,effect:0})}};

// ---- tabs
function tab(t){document.querySelectorAll('#tabs button').forEach(b=>b.classList.toggle('act',b.dataset.t===t));
  document.querySelectorAll('.tab').forEach(d=>d.classList.toggle('act',d.id==='t-'+t));
  if(t==='info')renderInfo()}
$('tabs').onclick=e=>{if(e.target.dataset.t)tab(e.target.dataset.t)};

// ---- info / header
function renderHeader(){
  $('title').textContent=I.name;document.title=I.name;showIcon($('hicon'),I.fixture);
  const w=I.wifi,p=$('pill');
  if(w.connected){p.textContent=w.ssid+' · '+w.ip;p.className='pill ok'}
  else if(w.apActive){p.textContent='Setup hotspot';p.className='pill warn'}
  else{p.textContent='Wi‑Fi: '+w.status;p.className='pill warn'}
  $('banner').hidden=!(w.apActive&&!w.connected);
  $('wcur').textContent=w.ssid?`Saved network: ${w.ssid} (${w.status}${w.connected?', '+w.rssi+' dBm':''})`:'No network saved yet.';
  const l=I.leds;$('ma').textContent=`${l.count} LEDs on GPIO${l.pin} · ~${l.estimatedMa} mA`+(l.maxMa?` of ${l.maxMa} mA limit`:'')+(l.powerLimited?' · dimmed by power limit':'');
  $('fwver').textContent=I.version;$('fwbuild').textContent='('+I.build+')';
}
function renderInfo(){if(!I.wifi)return;const w=I.wifi,s=I.system;
  const rows=[['Name',I.name],['Hostname',I.hostname+'.local'],['Firmware',I.firmware+' '+I.version],['MAC',I.mac],['Chip ID',I.chipId],
   ['Wi‑Fi',w.ssid?w.ssid+' – '+w.status:'not configured'],['IP',w.ip||'–'],['Signal',w.rssi?w.rssi+' dBm':'–'],
   ['Setup AP',w.apActive?w.apSsid+' ('+w.apIp+')':'off'],['LEDs',`${I.leds.count} × ${I.leds.colorOrder} on GPIO${I.leds.pin}`],
   ['IR',I.ir.enabled?`GPIO${I.ir.pin}`+(I.ir.lastCode?` · last code ${I.ir.lastCode}`:''):'disabled'],
   ['Uptime',fmtUp(s.uptimeS)],['Free heap',s.freeHeap+' B'],['Sketch',Math.round(s.sketchSize/1024)+' KB ('+Math.round(s.freeSketchSpace/1024)+' KB free)'],['Last reset',s.resetReason]];
  $('dl').innerHTML=rows.map(([k,v])=>`<dt>${k}</dt><dd>${esc(v)}</dd>`).join('')}
const esc=v=>String(v).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const fmtUp=s=>{const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return(d?d+'d ':'')+h+'h '+m+'m'};

async function refresh(){try{I=await api('/api/info');renderHeader();if($('t-info').classList.contains('act'))renderInfo()}catch(e){$('pill').textContent='offline';$('pill').className='pill'}
  try{if(!timer){S=await api('/api/state');paint()}}catch(e){}}

// ---- update check: the browser asks GitHub for the latest release (needs internet, so it stays
// quiet on the setup hotspot). Cached for 6 h to stay well inside GitHub's rate limit.
const REPO='logichousepcb/logix-fixture-firmware';let REL=null;
const vnum=v=>String(v).replace(/^v/,'').split('.').map(n=>parseInt(n,10)||0);
const newer=(a,b)=>{a=vnum(a);b=vnum(b);for(let i=0;i<3;i++){const x=a[i]||0,y=b[i]||0;if(x!==y)return x>y}return false};
async function checkUpdate(){let r=null;
  try{const c=JSON.parse(localStorage.getItem('logixRel')||'null');if(c&&Date.now()-c.t<6*3600e3)r=c.r}catch(e){}
  if(!r){try{const x=await fetch(`https://api.github.com/repos/${REPO}/releases/latest`,{headers:{Accept:'application/vnd.github+json'}});
      if(!x.ok)return;const j=await x.json();
      r={tag:j.tag_name,url:j.html_url,assets:(j.assets||[]).map(a=>({name:a.name,url:a.browser_download_url}))};
      try{localStorage.setItem('logixRel',JSON.stringify({t:Date.now(),r}))}catch(e){}}catch(e){return}}
  REL=r;showUpdate()}
function showUpdate(){if(!REL||!I||!I.version)return;const v=REL.tag.replace(/^v/,''),up=newer(v,I.version),u=$('upd'),f=$('fwlatest');
  u.hidden=!up;u.textContent='Update '+v;u.title=`Firmware ${v} is available (installed ${I.version})`;
  const big=!I.system||I.system.flashSize>1048576,a=REL.assets.find(x=>x.name.endsWith(big?'-4mb.bin':'-1mb.bin'));
  f.innerHTML=up?`Update available: <b>${esc(v)}</b>. <a href="${esc(a?a.url:REL.url)}">Download ${esc(a?a.name:'from GitHub')}</a>, then upload it below. <a href="${esc(REL.url)}" target="_blank" rel="noopener">Release notes</a>`
    :`Up to date (latest release ${esc(v)}).`;
  f.hidden=false}
$('upd').onclick=e=>{e.preventDefault();tab('fw')};

// ---- wifi
$('scan').onclick=async()=>{const b=$('scan');b.disabled=true;b.textContent='Scanning…';
  try{for(let i=0;i<20;i++){const r=await api('/api/wifi/scan');if(!r.scanning){
    const n=$('nets');n.hidden=false;n.innerHTML=r.networks.length?r.networks.map(x=>`<div class="net" data-s="${esc(x.ssid)}">${esc(x.ssid)}<span>${x.secure?'🔒 ':''}${x.rssi} dBm</span></div>`).join(''):'<div class="net">No networks found</div>';break}
    await new Promise(r=>setTimeout(r,1200))}}catch(e){msg('wmsg',e.message,'bad')}
  b.disabled=false;b.textContent='Scan again'};
$('nets').onclick=e=>{const d=e.target.closest('.net');if(d&&d.dataset.s){$('ssid').value=d.dataset.s;$('pass').focus()}};
$('wsave').onclick=async()=>{const ssid=$('ssid').value.trim();if(!ssid)return msg('wmsg','Enter a network name.','bad');
  try{await post('/api/wifi',{ssid,pass:$('pass').value});}catch(e){return msg('wmsg',e.message,'bad')}
  msg('wmsg','Connecting to '+ssid+'…');
  for(let i=0;i<15;i++){await new Promise(r=>setTimeout(r,2000));
    try{I=await api('/api/info');renderHeader();const w=I.wifi;
      if(w.connected){return msg('wmsg',`Connected! IP ${w.ip}. Switch your phone/PC back to "${ssid}" and open http://${I.hostname}.local or http://${w.ip}. The setup hotspot turns off in about a minute.`,'ok')}
      if(i>3&&(w.status==='wrong password'||w.status==='network not found'))return msg('wmsg','Could not connect: '+w.status+'.','bad')}
    catch(e){}}
  msg('wmsg','No connection yet ('+(I.wifi?I.wifi.status:'unknown')+'). Check the password and try again.','bad')};
$('wforget').onclick=async()=>{if(!confirm('Forget the saved Wi‑Fi network? The fixture will reboot into setup-hotspot mode.'))return;
  try{await post('/api/wifi',{ssid:''});msg('wmsg','Forgotten. Rebooting into setup mode…','ok')}catch(e){msg('wmsg',e.message,'bad')}};

// ---- device
$('name').oninput=()=>{const n=$('name').value;$('hostprev').textContent=host(n)+'.local';$('appprev').textContent=C.setupSsid||''};
$('ident').onclick=async()=>{try{await post('/api/identify');msg('dmsg','Blinking…','ok')}catch(e){msg('dmsg',e.message,'bad')}};
function showTok(t){$('tokstate').textContent=t?'On. Apps and scripts must send this token (header "Authorization: Bearer …"). Scan the code with the app to pair.':'Off. Anyone on your network can control this fixture.';
  $('tokbox').hidden=!t;$('tokoff').hidden=!t;$('tokval').textContent=t||'';$('tokqr').innerHTML='';
  if(!t)return;const pair=JSON.stringify({logix:1,id:I.id||C.id,host:I.hostname||C.hostname,token:t});
  const draw=()=>{try{new QRCode($('tokqr'),{text:pair,width:168,height:168})}catch(e){$('tokqr').hidden=true}};
  if(window.QRCode)draw();else{const s=document.createElement('script');s.src='https://cdnjs.cloudflare.com/ajax/libs/qrcodejs/1.0.0/qrcode.min.js';s.onload=draw;s.onerror=()=>{$('tokqr').hidden=true};document.head.appendChild(s)}}
async function setToken(v){try{const r=await post('/api/config',{apiToken:v});setTok(r.apiToken||'');C.apiToken=r.apiToken||'';showTok(C.apiToken);
    msg('tmsg',r.reboot?'Saved. Rebooting to update the network announcement…':'Saved. Old token no longer works.','ok');if(r.reboot)setTimeout(()=>location.reload(),9000)}catch(e){msg('tmsg',e.message,'bad')}}
$('tokgen').onclick=()=>{if(confirm('Generate a new API token? Apps using the old token will need to pair again.'))setToken('generate')};
$('tokoff').onclick=()=>{if(confirm('Turn off the API token? Anyone on your network will be able to control this fixture.'))setToken('')};
$('dsave').onclick=async()=>{const body={name:$('name').value.trim()};if($('appass').value)body.apPass=$('appass').value;
  try{const r=await post('/api/config',body);
    if(r.reboot){msg('dmsg',`Saved. Rebooting… the new address is http://${host(body.name)}.local`,'ok');setTimeout(()=>location.href='http://'+host(body.name)+'.local/',9000)}
    else msg('dmsg','Saved.','ok')}catch(e){msg('dmsg',e.message,'bad')}};

// ---- hardware
opts($('ledpin'),LEDP);opts($('irpin'),IRP);opts($('model'),MODELS.map(m=>[m[0],m[1]]));
function modelUI(){const m=model($('model').value);showIcon($('micon'),m[0]);
  $('ledcount').readOnly=!!m[2];if(m[2])$('ledcount').value=m[2];
  $('mhint').hidden=!m[2];$('mhint').textContent=m[2]?`${m[1]}: LED count is fixed at ${m[2]}.`:''}
$('model').onchange=modelUI;
$('hsave').onclick=async()=>{const body={fixture:$('model').value,ledPin:+$('ledpin').value,ledCount:+$('ledcount').value,colorOrder:$('order').value,irPin:+$('irpin').value,maxMa:+$('maxma').value};
  try{const r=await post('/api/config',body);msg('hmsg',r.reboot?'Saved. Rebooting…':'Saved.','ok');if(r.reboot)setTimeout(()=>location.reload(),8000)}catch(e){msg('hmsg',e.message,'bad')}};

// ---- firmware
$('fwup').onclick=()=>{const f=$('fwfile').files[0];if(!f)return msg('fwmsg','Choose a firmware .bin file first.','bad');
  const fd=new FormData();fd.append('firmware',f,f.name);const x=new XMLHttpRequest(),p=$('fwprog');
  const send0=x.send.bind(x);x.send=b=>{if(TOK)x.setRequestHeader('Authorization','Bearer '+TOK);send0(b)};
  p.hidden=false;p.value=0;$('fwup').disabled=true;msg('fwmsg','Uploading…');
  x.upload.onprogress=e=>{if(e.lengthComputable)p.value=e.loaded/e.total*100};
  x.onload=()=>{let j={};try{j=JSON.parse(x.responseText)}catch(e){}
    if(x.status===200){msg('fwmsg','Installed. Rebooting… this page reloads in 15 s.','ok');setTimeout(()=>location.reload(),15000)}
    else{msg('fwmsg','Update failed: '+(j.error||x.status),'bad');$('fwup').disabled=false}};
  x.onerror=()=>{msg('fwmsg','Upload failed (connection lost).','bad');$('fwup').disabled=false};
  x.open('POST','/api/update');x.send(fd)};

// ---- system
$('reboot').onclick=async()=>{try{await post('/api/reboot');msg('imsg','Rebooting…','ok');setTimeout(()=>location.reload(),8000)}catch(e){msg('imsg',e.message,'bad')}};
$('reset').onclick=async()=>{if(!confirm('Erase all settings (Wi‑Fi, name, GPIO) and reboot into setup mode?'))return;
  try{await post('/api/reset');msg('imsg','Settings erased. Rebooting into setup mode…','ok')}catch(e){msg('imsg',e.message,'bad')}};

// ---- boot
(async()=>{
  try{const fx=await api('/api/effects');$('fx').innerHTML=fx.map(e=>`<option value="${e.id}">${e.name}</option>`).join('')}catch(e){}
  try{C=await api('/api/config');$('name').value=C.name;$('name').oninput();$('ssid').value=C.ssid;
    $('ledpin').value=C.ledPin;$('ledcount').value=C.ledCount;$('order').value=C.colorOrder;$('irpin').value=C.irPin;$('maxma').value=C.maxMa;
    $('model').value=C.fixture||'custom';modelUI();
    if(C.hasApPass)$('appass').placeholder='(unchanged)';}catch(e){}
  await refresh();
  showTok(C.apiToken||'');
  tab(I.wifi&&I.wifi.apActive&&!I.wifi.connected?'wifi':'dev');
  setInterval(()=>{if(!document.hidden)refresh()},4000);
  checkUpdate();
})();
</script></body></html>)HTML";
