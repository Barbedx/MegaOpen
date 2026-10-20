#pragma once

// /wiggle: contact hunt. CTX is pulsed; the page shows how many of 400 edges
// came back on CRX, ~10x/s, large, with a tone when good.
// While it runs the controller is off the pins 40 ms in every 100, so the panel
// will not register until it is closed.
// Its own page, not a screen of the app: the background is the verdict.
static const char PAGE_WIGGLE[] PROGMEM = R"PAGE(
<!doctype html><html lang="uk"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>MegaOpen — wiggle</title><style>
body{margin:0;font:16px system-ui,sans-serif;color:#fff;background:#b00;transition:background .1s;min-height:100vh;display:flex;flex-direction:column}
main{flex:1;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:8px}
#n{font:bold 30vw/1 ui-monospace,monospace}#s{font-size:5vw;opacity:.85}
canvas{width:100%;height:60px;display:block;background:rgba(0,0,0,.25)}
footer{padding:10px 14px;font-size:13px;opacity:.9}button{font:inherit;padding:8px 16px;border-radius:8px;border:0}
footer a{color:#fff}
</style></head><body><main><div id="n">–</div><div id="s">натисни «старт»</div>
<div><button id="go">старт</button> <button id="stop">стоп</button> <label><input type="checkbox" id="snd" checked> звук</label></div></main>
<canvas id="h" width="600" height="60"></canvas>
<footer>CTX смикається імпульсами, число = скільки з 400 повернулось на CRX. <b>≥350 зелене</b> — контакт є. Поки ця сторінка працює, панель НЕ реєструється — закрий її після пошуку.
<div style="margin-top:8px"><a href="/">← MegaOpen</a></div></footer>
<script>
let run=false,ac=null,hist=[];const $=i=>document.getElementById(i);
function beep(f){if(!$('snd').checked||!ac)return;const o=ac.createOscillator(),g=ac.createGain();o.frequency.value=f;g.gain.value=.15;o.connect(g).connect(ac.destination);o.start();o.stop(ac.currentTime+.06);}
function show(f,c){$('n').textContent=f;const ok=f>=c*0.85,mid=f>20;document.body.style.background=ok?'#0a7a2a':mid?'#a06000':'#b00000';$('s').textContent=ok?'КОНТАКТ Є':mid?'частково':'нема';if(ok)beep(880);
hist.push(f);if(hist.length>150)hist.shift();const cv=$('h'),g=cv.getContext('2d');g.clearRect(0,0,cv.width,cv.height);hist.forEach((v,i)=>{g.fillStyle=v>=c*0.85?'#7dffb0':v>20?'#ffd27d':'#ff8080';const hh=v/c*cv.height;g.fillRect(i*4,cv.height-hh,3,hh);});}
async function tick(){if(!run)return;try{const r=await fetch('/api/can/loop',{method:'POST'});const d=await r.json();show(d.followed|0,d.cycles||400);}catch(e){$('s').textContent='нема зв’язку з платою';}setTimeout(tick,60);}
$('go').onclick=()=>{if(!ac)ac=new (window.AudioContext||window.webkitAudioContext)();run=true;tick();};
$('stop').onclick=()=>{run=false;$('s').textContent='зупинено';};
</script></body></html>
)PAGE";
