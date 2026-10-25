/* ============ API: єдина точка ============
   На платі — fetch. У симуляторі web/sim/mock.js ставить window.SIM, і ті самі
   виклики йдуть у мок; це єдине місце, де сторінка знає про симулятор. */
/* ці маршрути приймають JSON; решта — форма (urlencoded) */
const JSON_PATHS=new Set(['/api/settings','/api/display','/api/media','/api/wifi','/api/affamap','/api/knobs','/api/macros','/api/map','/api/map/test','/api/config',
 '/api/keymap','/api/res','/api/res/map','/api/res/auto','/api/res/learn','/api/logpush','/api/fw/channel','/api/diag','/api/alerts/test']);
/* форма: так/ні йде як 1/0 — прошивка порівнює з "1" (paramStr(r,"on")=="1") */
const formBody=params=>new URLSearchParams(Object.entries(params).map(([k,v])=>[k,v===true?'1':v===false?'0':String(v)]));
/* текст помилки: прошивка відповідає text/plain, рідше JSON з error */
const errMsg=(body,status)=>typeof body==='string'&&body.trim()?body.trim():body&&body.error?body.error:'HTTP '+status;
/* кожен запит несе X-MegaOpen: інша сторінка не може його надіслати, тож не змінить плату (src/net/RequestGate.h) */
const GATE={'X-MegaOpen':'1'};
async function call(m,path,params){
 const [p]=path.split('?'),json=JSON_PATHS.has(p);
 /* симулятор отримує параметри такими, якими їх побачить прошивка */
 if(window.SIM)return SIM.call(m,path,m==='POST'&&params?(json?JSON.parse(JSON.stringify(params)):Object.fromEntries(formBody(params))):params,GATE);
 const o={method:m,headers:{...GATE}};
 if(m==='POST'&&params){if(json){o.headers['Content-Type']='application/json';o.body=JSON.stringify(params)}else o.body=formBody(params)}
 const r=await fetch(path,o);const ct=r.headers.get('content-type')||'';
 const body=ct.includes('json')?await r.json():await r.text();
 if(!r.ok)throw Object.assign(new Error(errMsg(body,r.status)),{status:r.status,body});
 return body;
}
const API={get:p=>call('GET',p),post:(p,params)=>call('POST',p,params)};
/* файл з плати: браузер сам качає за посиланням. Повертає {note} для run() */
async function download(path,name){
 if(window.SIM){await SIM.call('GET',path);return {note:t('c.simDl',name)}}
 const a=document.createElement('a');a.href=path;a.download=name;document.body.append(a);a.click();a.remove();return {note:name};
}
/* текст зі сторінки у файл (лог, налаштування). Повертає текст для чипа */
function saveText(name,text){
 if(window.SIM)return t('c.simDl',name);
 const u=URL.createObjectURL(new Blob([text],{type:'text/plain;charset=utf-8'})),a=document.createElement('a');
 a.href=u;a.download=name;document.body.append(a);a.click();a.remove();setTimeout(()=>URL.revokeObjectURL(u),1000);return name;
}
/* прошивка: ElegantOTA хоче /ota/start перед /ota/upload; onP(відсотки) */
async function upload(file,onP){
 if(window.SIM)return SIM.upload(file,onP);
 await call('GET','/ota/start?mode=fr');
 return new Promise((ok,no)=>{const x=new XMLHttpRequest();x.open('POST','/ota/upload');x.setRequestHeader('X-MegaOpen','1');
  x.upload.onprogress=e=>{if(e.lengthComputable)onP(Math.round(e.loaded/e.total*100))};
  x.onload=()=>x.status<300?ok({ok:true}):no(Object.assign(new Error(errMsg(x.responseText,x.status)),{status:x.status}));
  x.onerror=()=>no(new Error(t('c.noBoard')));
  const fd=new FormData();fd.append('file',file,file.name);x.send(fd)});
}
