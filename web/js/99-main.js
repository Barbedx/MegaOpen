/* ============ каталог, маршрутизація, опитування, тема ============ */
/* CAT: довідники прошивки з GET /api/catalog, раз на запуск сторінки. Екрани
   будують списки лише з нього, тому новий рядок у прошивці не потребує правки тут */
let CAT=null;
let routing=Promise.resolve();
function route(){routing=routing.then(doRoute,doRoute);return routing}
async function doRoute(){
 const id=(location.hash||'#home').slice(1);curId=SCR[id]?id:'home';cur=SCR[curId];const my=++gen;
 if(leave){leave();leave=null}
 $('#title').textContent=t(cur.title);$$('#tabs a').forEach(a=>a.classList.toggle('on',a.dataset.t===curId));$('#back').hidden=!cur.hidden;
 if(curId==='service')$('#tabSvc').hidden=false;
 const m=$('#main');m.innerHTML='<div class="row muted">…</div>';window.scrollTo(0,0);
 try{await cur.render(m)}catch(e){if(my===gen)m.innerHTML=card(t('c.errTitle'),row(esc(errT(e)),`<button class="btn" type="button" onclick="route()">${t('c.retry')}</button>`))}
}
let busy=false,tickN=0;
async function foot(){const st=await API.get('/api/status');$('#foot').textContent=[st.fw,st.build.split(' ')[0],t('m.up',fmtUp(st.uptimeMs)),t('m.mem',Math.round(st.heap/1024)),st.ip].join(' · ')}
setInterval(async()=>{const L=$('#link');if(REBOOTING){setChip(L,'warn',t('m.restarting'));L.hidden=false;return}if(!cur||busy||document.hidden)return;busy=true;const my=gen;
 try{if(cur.tick&&my===gen)await cur.tick(true);if(tickN++%5===0)await foot();L.hidden=true}catch(e){if(!e.status){setChip(L,'bad',t('c.noLink'));L.hidden=false}}busy=false},1000);
window.addEventListener('hashchange',()=>{if(CAT)route()});
/* тема */
const THEMES=['auto','light','dark'];let th='auto';try{th=localStorage.getItem('mo-theme')||'auto'}catch(e){}
const applyTh=()=>{if(th==='auto')document.documentElement.removeAttribute('data-theme');else document.documentElement.dataset.theme=th;$('#themeBtn').textContent=th==='auto'?'◐':th==='light'?'☀':'☾';if(cur&&cur.me)glassDraw(cur.me)};
/* мова: перемикач у шапці показує ту, на яку перейде; перехід — перезавантаження сторінки */
$('#langBtn').textContent=LANG==='uk'?'EN':'UA';
$('#langBtn').onclick=()=>{try{localStorage.setItem('mo-lang',LANG==='uk'?'en':'uk')}catch(e){}location.reload()};
applyTh();$('#themeBtn').onclick=()=>{th=THEMES[(THEMES.indexOf(th)+1)%3];try{localStorage.setItem('mo-theme',th)}catch(e){}applyTh()};
/* старт: після розбору всієї сторінки (у симуляторі після sim-скриптів), спершу каталог */
async function start(){await i18nLoad();devUi();
 try{CAT=await API.get('/api/catalog')}catch(e){$('#main').innerHTML=card(t('c.errTitle'),row(esc(t('m.catalog',errT(e))),`<button class="btn" type="button" onclick="start()">${t('c.retry')}</button>`));return}
 $('[data-t="screen"]').hidden=!has('display');
 route();foot().catch(()=>{});
}
if(document.readyState==='loading')document.addEventListener('DOMContentLoaded',start);else start();
