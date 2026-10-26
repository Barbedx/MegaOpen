#pragma once
#include <pgmspace.h>

// The page "/" serves in a build without the settings page (FEATURE_WEB_PAGE=0):
// the settings live in MegaOpen Companion on the head unit and on the WebUSB page
// for a PC; this keeps the two things nobody may be locked out of, a firmware
// update from a file and the WiFi network. Every image keeps OTA (CLAUDE.md).
// Every change carries X-MegaOpen, as the full page's (src/net/RequestGate.h).
static const char RESCUE_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="uk"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>MegaOpen</title>
<style>
body{font:16px system-ui,sans-serif;background:#111418;color:#e3e6ea;margin:0;padding:16px;max-width:640px}
h1{font-size:22px}h2{font-size:17px;margin-top:28px}p,small{color:#b4bac2}
input,button{font:inherit;padding:10px;border-radius:8px;border:1px solid #39404a;background:#181c21;color:#e3e6ea;box-sizing:border-box}
input{width:100%;margin:4px 0 10px}button{background:#1e88e5;border:0;color:#fff;padding:10px 18px}
#st{min-height:1.4em}
</style></head><body>
<h1>MegaOpen</h1>
<p>Налаштування — у застосунку <b>MegaOpen Компаньйон</b> на магнітолі (плата по USB або WiFi), або в Chrome на комп'ютері: <a style="color:#64b5f6" href="https://barbedx.github.io/MegaOpen/usb/">barbedx.github.io/MegaOpen/usb</a>.<br>
<small>The settings are in the MegaOpen Companion app on the head unit, or in Chrome on a PC (link above). This page keeps a firmware update and the WiFi network.</small></p>
<h2>Прошивка · Firmware</h2>
<input type="file" id="f" accept=".bin"><button id="up">Оновити · Update</button>
<h2>WiFi</h2>
<input id="ss" placeholder="SSID" maxlength="32"><input id="pw" type="password" placeholder="пароль · password" maxlength="64">
<button id="wf">Зберегти й перезапустити · Save and restart</button>
<p id="st"></p>
<script>
const G={'X-MegaOpen':'1'},st=t=>document.getElementById('st').textContent=t;
document.getElementById('up').onclick=async()=>{const f=document.getElementById('f').files[0];if(!f)return st('.bin?');
 try{const r=await fetch('/ota/start?mode=fr',{headers:G});if(!r.ok)throw r.status;}catch(e){return st('start: '+e)}
 const x=new XMLHttpRequest();x.open('POST','/ota/upload');x.setRequestHeader('X-MegaOpen','1');
 x.upload.onprogress=e=>e.lengthComputable&&st(Math.round(e.loaded/e.total*100)+'%');
 x.onload=()=>st(x.status<300?'OK — restarting…':'HTTP '+x.status+' '+x.responseText);x.onerror=()=>st('no board');
 const d=new FormData();d.append('file',f,f.name);x.send(d)};
document.getElementById('wf').onclick=async()=>{const b={ssid:document.getElementById('ss').value,pass:document.getElementById('pw').value};
 try{const r=await fetch('/api/wifi',{method:'POST',headers:{...G,'Content-Type':'application/json'},body:JSON.stringify(b)});
  if(!r.ok)return st('HTTP '+r.status+' '+await r.text());await fetch('/api/reboot',{method:'POST',headers:G});st('OK — restarting…')}catch(e){st(''+e)}};
</script></body></html>)HTML";
