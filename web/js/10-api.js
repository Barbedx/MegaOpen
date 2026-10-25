/* ============ API: єдина точка ============
   На платі — fetch. У симуляторі web/sim/mock.js ставить window.SIM, і ті самі
   виклики йдуть у мок; це єдине місце, де сторінка знає про симулятор
   і про застосунок (window.MOLink, нижче). */
/* ці маршрути приймають JSON; решта — форма (urlencoded) */
const JSON_PATHS=new Set(['/api/settings','/api/display','/api/media','/api/wifi','/api/affamap','/api/knobs','/api/macros','/api/map','/api/map/test','/api/config',
 '/api/keymap','/api/res','/api/res/map','/api/res/auto','/api/res/learn','/api/logpush','/api/fw/channel','/api/diag','/api/alerts/test']);
/* форма: так/ні йде як 1/0 — прошивка порівнює з "1" (paramStr(r,"on")=="1") */
const formBody=params=>new URLSearchParams(Object.entries(params).map(([k,v])=>[k,v===true?'1':v===false?'0':String(v)]));
/* текст помилки: прошивка відповідає text/plain, рідше JSON з error */
const errMsg=(body,status)=>typeof body==='string'&&body.trim()?body.trim():body&&body.error?body.error:'HTTP '+status;
/* кожен запит несе X-MegaOpen: інша сторінка не може його надіслати, тож не змінить плату (src/net/RequestGate.h) */
const GATE={'X-MegaOpen':'1'};
/* У застосунку MegaOpen Голос (android/, BoardActivity) сторінка йде по USB:
   window.MOLink несе запит до плати, відповідь приходить у __moDone.
   docs/design/usb-link.md */
const LINK=window.MOLink,LINKQ={};let LINKN=0;
window.__moDone=(id,status,ct,b64)=>{const q=LINKQ[id];delete LINKQ[id];if(q)q(status,ct,b64)};
const b64enc=u8=>{let s='';for(let i=0;i<u8.length;i+=0x8000)s+=String.fromCharCode.apply(null,u8.subarray(i,i+0x8000));return btoa(s)};
const b64dec=b=>{const s=atob(b),u=new Uint8Array(s.length);for(let i=0;i<s.length;i++)u[i]=s.charCodeAt(i);return u};
/* {status, ct, text}; status -1: немає зв'язку з платою */
function linkFetch(m,path,headers,body){
 return new Promise((ok,no)=>{const id=++LINKN;
  LINKQ[id]=(status,ct,b64)=>status<0?no(new Error(t('c.noBoard'))):ok({status,ct,text:new TextDecoder().decode(b64dec(b64))});
  LINK.request(id,m,path,JSON.stringify(headers||{}),body==null?'':b64enc(typeof body==='string'?new TextEncoder().encode(body):body))});
}
async function call(m,path,params){
 const [p]=path.split('?'),json=JSON_PATHS.has(p);
 /* симулятор отримує параметри такими, якими їх побачить прошивка */
 if(window.SIM)return SIM.call(m,path,m==='POST'&&params?(json?JSON.parse(JSON.stringify(params)):Object.fromEntries(formBody(params))):params,GATE);
 const o={method:m,headers:{...GATE}};
 if(m==='POST'&&params){if(json){o.headers['Content-Type']='application/json';o.body=JSON.stringify(params)}else o.body=formBody(params)}
 if(LINK){
  if(o.body instanceof URLSearchParams){o.headers['Content-Type']='application/x-www-form-urlencoded';o.body=o.body.toString()}
  const r=await linkFetch(m,path,o.headers,o.body);
  const body=r.ct.includes('json')?JSON.parse(r.text||'null'):r.text;
  if(r.status>=400)throw Object.assign(new Error(errMsg(body,r.status)),{status:r.status,body});
  return body;
 }
 const r=await fetch(path,o);const ct=r.headers.get('content-type')||'';
 const body=ct.includes('json')?await r.json():await r.text();
 if(!r.ok)throw Object.assign(new Error(errMsg(body,r.status)),{status:r.status,body});
 return body;
}
const API={get:p=>call('GET',p),post:(p,params)=>call('POST',p,params)};
/* файл з плати: браузер сам качає за посиланням. Повертає {note} для run() */
async function download(path,name){
 if(window.SIM){await SIM.call('GET',path);return {note:t('c.simDl',name)}}
 if(LINK){LINK.download(path,name);return {note:name}}   /* застосунок кладе в Завантаження */
 const a=document.createElement('a');a.href=path;a.download=name;document.body.append(a);a.click();a.remove();return {note:name};
}
/* текст зі сторінки у файл (лог, налаштування). Повертає текст для чипа */
function saveText(name,text){
 if(window.SIM)return t('c.simDl',name);
 if(LINK){LINK.saveText(name,text);return name}
 const u=URL.createObjectURL(new Blob([text],{type:'text/plain;charset=utf-8'})),a=document.createElement('a');
 a.href=u;a.download=name;document.body.append(a);a.click();a.remove();setTimeout(()=>URL.revokeObjectURL(u),1000);return name;
}
/* прошивка: ElegantOTA хоче /ota/start перед /ota/upload; onP(відсотки) */
async function upload(file,onP){
 if(window.SIM)return SIM.upload(file,onP);
 await call('GET','/ota/start?mode=fr');
 if(LINK){
  /* те саме тіло multipart, що надіслав би XHR; поступ — лише кінець */
  const fd=new FormData();fd.append('file',file,file.name);
  const rq=new Request('/ota/upload',{method:'POST',body:fd});
  const r=await linkFetch('POST','/ota/upload',{...GATE,'Content-Type':rq.headers.get('content-type')},new Uint8Array(await rq.arrayBuffer()));
  if(r.status>=300)throw Object.assign(new Error(errMsg(r.text,r.status)),{status:r.status});
  onP(100);return {ok:true};
 }
 return new Promise((ok,no)=>{const x=new XMLHttpRequest();x.open('POST','/ota/upload');x.setRequestHeader('X-MegaOpen','1');
  x.upload.onprogress=e=>{if(e.lengthComputable)onP(Math.round(e.loaded/e.total*100))};
  x.onload=()=>x.status<300?ok({ok:true}):no(Object.assign(new Error(errMsg(x.responseText,x.status)),{status:x.status}));
  x.onerror=()=>no(new Error(t('c.noBoard')));
  const fd=new FormData();fd.append('file',file,file.name);x.send(fd)});
}
