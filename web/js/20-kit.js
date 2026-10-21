/* ============ UI: будівельні блоки ============ */
/* стан маршрутизатора (веде 99-main.js); тут, бо екрани реєструються в SCR раніше */
const SCR={};let cur=null,curId='home',gen=0,leave=null;
let DEV=false;try{DEV=localStorage.getItem('mo-dev')==='1'}catch(e){}
const devUi=()=>{$('#tabSvc').hidden=!DEV};
/* каталог (CAT з /api/catalog): довідники прошивки, сторінка їх не дублює */
/* список із каталогу: CAT.enums[n] ({v,name}) або верхній список CAT[n] ({id,name}, напр. styles) */
const enumList=n=>((CAT.enums||{})[n]||CAT[n]||[]);
const enumOpts=n=>enumList(n).map(e=>[e.v!==undefined?e.v:e.id,e.name]);
const enumOf=(n,v)=>enumList(n).find(e=>String(e.v!==undefined?e.v:e.id)===String(v));
/* каталог словами сторінки (web/i18n "k.*"), один прохід одразу після /api/catalog: екрани беруть
   name/label/group/unit, як і раніше. Ключ — з ідентифікатора: k.set.<налаштування>[.<колонка>],
   рядок переліку k.<dom>.<v> (dom — від прошивки, спільний для всіх списків тих самих речей) або
   k.set.<налаштування>.<v>; сигнал k.sig.<v>, а без нього — його назва в списку Raise, k.raise.<v>.
   Прошивка шле назву лише там, де вона не слово (AF-RDS, 125k); нема ключа — та назва, а нема й її —
   сам ідентифікатор. tools/check_i18n.py рахує ключі тим самим способом */
/* одиниця прошивки (коротка англійська: km/h, ms) словами сторінки; невідома — як є */
const unitT=u=>tHas('k.unit.'+u)?t('k.unit.'+u):u;
function i18nCat(c){
 const tr=(k,o,f,id)=>{if(!o)return;if(tHas(k))o[f]=t(k);else if(o[f]==null&&id!==undefined)o[f]=String(id)};
 const rows=(list,dom,base)=>(list||[]).forEach(r=>tr(dom==='sig'&&!tHas('k.sig.'+r.v)?'k.raise.'+r.v:dom?'k.'+dom+'.'+r.v:base+'.'+r.v,r,'name',r.v));
 for(const s of c.settings||[]){tr('k.set.'+s.id,s,'label',s.id);rows(s.enum,s.dom,'k.set.'+s.id);
  for(const col of s.cols||[]){tr('k.set.'+s.id+'.'+col.id,col,'label',col.id);rows(col.enum,col.dom,'k.set.'+s.id+'.'+col.id)}}
 for(const[n,list]of Object.entries(c.enums||{}))rows(list,n);
 for(const l of c.layouts||[]){tr('k.layout.'+l.id,l,'name',l.id);for(const p of l.params||[]){tr('k.layout.'+l.id+'.'+p.n,p,'label',p.n);tr('k.layout.'+l.id+'.'+p.n+'.h',p,'hint')}}
 for(const[l,d]of[['styles','style'],['scenes','scene'],['bleModes','ble'],['panels','panel'],['dests','dest'],['actions','action'],['panelKeys','pkey']])
  for(const x of c[l]||[])tr('k.'+d+'.'+x.id,x,'name',x.id);
 for(const x of c.classes||[])tr('k.class.'+x.bit,x,'name',x.bit);
 for(const x of c.console||[]){tr('k.con.'+x.cmd,x,'name',x.cmd);for(const f of x.fields||[])tr('k.con.'+x.cmd+'.'+f.n,f,'label',f.n)}
 for(const x of c.raise||[]){tr('k.raise.'+x.id,x,'name',x.id);tr('k.raise.'+x.id+'.why',x,'why');tr('k.rgroup.'+x.group,x,'group');tr('k.unit.'+x.unit,x,'unit')}
 return c;
}
/* що є в цій збірці (CAT.features): екран питає лише маршрути, які прошивка зібрала,
   і не малює карток без них. getIf — запит, коли функція є, інакше null */
const has=f=>!!(CAT&&CAT.features&&CAT.features[f]);
const getIf=(ok,p)=>ok?API.get(p):Promise.resolve(null);
/* картка на екрані (хоч рядок видно): швидке опитування лише тоді — плата одна, а в машині ще й Bluetooth на тому ж радіо */
const onScreen=el=>{if(!el)return false;const r=el.getBoundingClientRect();return r.bottom>0&&r.top<innerHeight&&r.height>0};
const notBuilt=(title,what)=>card(title,`<div class="row muted">${esc(t('c.notBuilt',what))}</div>`,'','custom');
const noneAction=()=>(CAT.actions.find(a=>a.class==null)||{}).id;
const byId=(list,id)=>(list||[]).find(x=>x.id===id);
const nameOf=(list,id)=>{const x=byId(list,id);return x?x.name:id};
const at=k=>k[0]==='@'?`data-kf="${esc(k.slice(1))}"`:k[0]==='#'?`data-f="${esc(k.slice(1))}"`:`data-k="${esc(k)}"`;
const rbA=(k,v,label,always)=>k.startsWith('rb:')?` data-label="${esc(label)}" data-orig="${esc(String(v))}"${always?' data-always':''}`:'';
const sw=(k,on,ex='')=>`<input type="checkbox" class="sw" ${at(k)}${on?' checked':''} ${ex}>`;
const num=(k,v,ex='')=>`<input class="in" type="number" inputmode="numeric" ${at(k)} value="${esc(v)}" ${ex}>`;
const txt=(k,v,ph='',cls='in',type='text',ex='')=>`<input class="${cls}" type="${type}" ${at(k)} value="${esc(v)}" placeholder="${esc(ph)}" autocomplete="off" ${ex}>`;
const opt=o=>Array.isArray(o)?o:[o,o];
const sel=(k,opts,v,ex='')=>`<select class="in" ${at(k)} ${ex}>${opts.map(o=>{const[a,b]=opt(o);return `<option value="${esc(a)}"${String(a)===String(v)?' selected':''}>${esc(b)}</option>`}).join('')}</select>`;
const seg=(k,opts,v,cls='')=>`<div class="seg ${cls}" ${at(k)} data-v="${esc(v)}">${opts.map(o=>{const[a,b]=opt(o);return `<button type="button" data-val="${esc(a)}" class="${String(a)===String(v)?'on':''}">${esc(b)}</button>`}).join('')}</div>`;
const row=(label,ctrl='',sub='',hint='',id='')=>`<div class="row"${id?` id="${id}"`:''}><div class="l"><b>${label}</b>${hint?`<button class="i" type="button" aria-label="${t('c.hint')}">?</button>`:''}${sub?`<small>${sub}</small>`:''}${hint?`<small class="hint" hidden>${esc(hint)}</small>`:''}</div>${ctrl?`<div class="c">${ctrl}</div>`:''}</div>`;
const card=(title,body,id='',cls='')=>`<section class="card ${cls}"${id?` id="${id}"`:''}><h2><span>${title}</span><span class="sv" aria-live="polite"></span></h2>${body}</section>`;
const fold=(title,body,id='')=>`<details class="card"${id?` id="${id}"`:''}><summary><h2><span>${title}</span><span class="sv" aria-live="polite"></span></h2></summary>${body}</details>`;
const chipH=(st,t)=>`<span class="chip ${st}">${esc(t)}</span>`;
const hintBtn=h=>`<button class="i" type="button" aria-label="${t('c.hint')}" title="${esc(h)}">?</button>`;
function setChip(el,st,t){if(typeof el==='string')el=$(el);if(!el)return;el.className='chip '+st;el.textContent=t}
const CH={ok:'ok',idle:'warn',off:'off',broken:'bad'};
const svOf=el=>{const c=el&&el.closest&&el.closest('.card');return c?$('.sv',c):null};
function sv(el,st,msg){if(!el)return;clearTimeout(el._t);el.className='sv '+st;el.textContent=st==='busy'?'…':(st==='ok'?'✓ ':'✗ ')+(msg||(st==='ok'?t('c.saved'):t('c.error')));if(st==='ok')el._t=setTimeout(()=>{el.textContent='✓'},2600)}
/* помилка прошивки — коди, не текст: рядок на відмову, "[ключ: ]код[ аргументи]"
   ("pulseMs: number", "param exp", "brake: between 0 100"). Слова — e.<код> (web/i18n);
   код, якого словник не знає, і "HTTP 404" лишаються як є */
const errLine=l=>{const m=/^(?:(.*): )?([a-z][a-z0-9_]*)((?: \S+)*)$/.exec(String(l).trim());if(!m||!tHas('e.'+m[2]))return l;
 const a=m[3].trim()?m[3].trim().split(' '):[];return (m[1]?m[1]+': ':'')+t('e.'+m[2],...a).replace(/[\s:]+$/,'')};
const errLines=m=>String(m||'').split('\n').filter(Boolean).map(errLine);
const errMsgT=m=>errLines(m).join('; ');
const errT=e=>(e.status?e.status+' · ':'')+(errMsgT(e.message)||t('c.error'));
async function run(btn,fn,okText){const s=svOf(btn);if(btn)btn.disabled=true;sv(s,'busy');try{const r=await fn();sv(s,'ok',okText||(r&&r.note)||t('c.done'));return r}catch(e){sv(s,'bad',errT(e))}finally{if(btn)btn.disabled=false}}
function val(el){if(!el)return;if(el.dataset.hex!==undefined){const n=parseInt(el.value,16);return isNaN(n)?0:n}if(el.classList.contains('seg'))return el.dataset.v;if(el.classList.contains('multi'))return el.dataset.v.split(',').filter(Boolean);
 if(el.type==='checkbox')return el.checked;if(el.type==='number'||el.type==='range'){const n=parseFloat(el.value);return isNaN(n)?undefined:n}
 if(el.tagName==='SELECT'&&el.value!==''&&/^-?\d+$/.test(el.value)&&el.dataset.str===undefined)return +el.value;return el.value}
function put(el,v){if(!el||v===undefined||el.dataset.dirty||el===document.activeElement||el.contains(document.activeElement))return;
 if(el.classList.contains('seg')){el.dataset.v=v;$$('button',el).forEach(b=>b.classList.toggle('on',b.dataset.val===String(v)));return}
 if(el.type==='checkbox'){el.checked=!!v;return}if(el.dataset.hex!==undefined)v=v?h2(v):'';if(String(el.value)!==String(v))el.value=v}
function shown(el){if(el.tagName==='SELECT')return el.options[el.selectedIndex].text;if(el.type==='checkbox')return el.checked?t('c.on'):t('c.off');if(el.type==='password')return '••••';return el.value}
/* автозбереження: дебаунс 400 мс, чип секції … / ✓ / ✗ */
function saver(scope,fn,selr='[data-k],[data-f],[data-kf]'){
 const s=svOf(scope);let t=null;const ks=new Set(),els=new Set();
 const h=e=>{const el=e.target.closest(selr);if(!el||!scope.contains(el)||el.hasAttribute('data-nosave'))return;
  if(e.type==='input'&&(el.tagName==='SELECT'||el.type==='checkbox'))return;
  if(e.type==='change'&&el.tagName==='INPUT'&&['text','number','password'].includes(el.type))return;
  ks.add(el.dataset.k||el.dataset.f||el.dataset.kf);els.add(el);el.dataset.dirty=1;sv(s,'busy');clearTimeout(t);
  t=setTimeout(async()=>{const k=[...ks];ks.clear();const es=[...els];els.clear();
   try{const r=await fn(k,scope);sv(s,'ok',r&&r.note)}catch(err){sv(s,'bad',errT(err));if(err.after)err.after()}es.forEach(x=>delete x.dataset.dirty)},400)};
 scope.addEventListener('input',h);scope.addEventListener('change',h);
}
async function genericSave(ks,scope){
 const groups={};let rb=false;
 for(const k of ks){const el=scope.querySelector(`[data-k="${k}"]`);if(!el)continue;const[path,field]=k.split('|');
  if(el.minLength>0&&el.value&&el.value.length<el.minLength)throw Object.assign(new Error(t('c.minChars',el.minLength)),{status:400});
  if(path.startsWith('rb:')){pendFrom(path.slice(3));rb=true;continue}
  const v=val(el);if(v===undefined)continue;(groups[path]=groups[path]||{})[field]=v}
 let last;for(const[p,params]of Object.entries(groups))last=await API.post(p,params);
 return rb&&!last?{note:t('c.afterReboot')}:last;
}
/* смуга «Потребує перезавантаження» */
const PEND={};
function pendFrom(path){
 const els=$$(`[data-k^="rb:${path}|"]`,$('#main'));const ch=els.filter(el=>{const v=val(el);if(el.type==='password')return !!v;return String(v)!==el.dataset.orig});
 const key=curId+path;if(!ch.length){delete PEND[key];rbar();return}
 const p={},labels=[];els.forEach(el=>{if(!ch.includes(el)&&el.dataset.always===undefined)return;const f=el.dataset.k.split('|')[1];setPath(p,f,val(el));if(ch.includes(el))labels.push(el.dataset.label+': '+shown(el))});
 PEND[key]={path,params:p,labels};rbar();
}
function rbar(){const L=Object.values(PEND).flatMap(x=>x.labels);$('#rbar').hidden=!L.length;$('#rbN').textContent=t('c.nChanges',L.length);$('#rbList').innerHTML=L.map(l=>`<li>${esc(l)}</li>`).join('');if(!L.length)$('#rbAsk').hidden=true}
$('#rbGo').onclick=()=>{$('#rbAsk').hidden=!$('#rbAsk').hidden;$('#rbMsg').textContent=''};
$('#rbUndo').onclick=()=>{for(const k in PEND)delete PEND[k];rbar();route()};
$('#rbYes').onclick=async()=>{const b=$('#rbYes');b.disabled=true;$('#rbMsg').textContent='…';
 try{for(const x of Object.values(PEND))await API.post(x.path,x.params);for(const k in PEND)delete PEND[k];rbar();await reboot()}
 catch(e){$('#rbMsg').textContent='✗ '+errT(e)}finally{b.disabled=false}};
let REBOOTING=false;
/* path: маршрут, що перезавантажує, або функція (оновлення прошивки) */
async function reboot(path='/api/reboot',params,why=''){
 await (typeof path==='function'?path():API.post(path,params));REBOOTING=true;$('#boot').hidden=false;$('#bootWhy').textContent=why;
 for(let i=4;i>0;i--){$('#bootN').textContent=i;await sleep(1000)}$('#boot').hidden=true;REBOOTING=false;route();
}
/* підтвердження в рядку (без confirm()) */
function ask(btn,text,onYes,word){
 $$('.ask').forEach(a=>a.remove());const d=document.createElement('div');d.className='ask';
 d.innerHTML=`<span>${esc(text)}</span>${word?`<input class="in" autocomplete="off" autocapitalize="off" placeholder="${esc(t('c.typeWord',word))}">`:''}<button class="btn dng" type="button" data-y${word?' disabled':''}>${t('c.yes')}</button><button class="btn" type="button" data-n>${t('c.no')}</button>`;
 (btn.closest('.row,.fr,.mac,.krow')||btn).after(d);const inp=$('input',d),y=$('[data-y]',d);
 if(inp){inp.oninput=()=>y.disabled=inp.value.trim()!==word;inp.focus()}
 $('[data-n]',d).onclick=()=>d.remove();y.onclick=()=>{d.remove();onYes()};
}
/* одне поле з каталогу (параметр розкладки, поле консолі): enum -> список,
   num -> число (список, коли варіантів 8 або менше), інакше текст до max */
function fieldCtrl(f,k,v){
 if(f.enum){const o=enumOpts(f.enum);return sel(k,o,v===undefined&&o[0]?o[0][0]:v,o.every(x=>typeof x[0]==='number')?'':'data-str')}
 if(f.num){if(f.max-f.min<=8){const o=[];for(let i=f.min;i<=f.max;i++)o.push(i);return sel(k,o,v===undefined?f.min:v)}
  return num(k,v===undefined?'':v,`min="${f.min}" max="${f.max}"${f.step?` step="${f.step}"`:''}`)}
 return txt(k,v===undefined?'':v,'','in','text','maxlength="'+f.max+'"');
}
const fieldLabel=f=>esc(f.label)+(f.max&&!f.num&&!f.enum?' · '+f.max:'');
/* спільні чипи: WiFi (Огляд, Плата), CAN A і кнопка запису (Огляд, Авто) */
function wifiChip(chip,sub,st,lost){if(st.mode==='ap'){setChip(chip,'warn',t('c.ap'));sub.textContent=(st.apName||st.ssid)+' · '+st.ip+' · '+lost}else{setChip(chip,'ok',st.ssid);sub.textContent=st.ip+' · '+st.rssi+' dBm'}}
function canChip(chip,c2){if(c2.fps>0)setChip(chip,'ok',(c2.detected||c2.bitrate)/1000+'k · '+t('c.perS',fmtN(c2.fps)));else if(c2.autobaud==='searching')setChip(chip,'warn',t('c.canSearch'));else setChip(chip,'bad',t('c.canSilent'))}
function recBtn(b,cp){b.classList.toggle('on',cp.recording);b.textContent=cp.recording?t('c.recStop'):t('c.recStart')}
/* глобальні кліки: сегменти, мультивибір, підказки */
document.addEventListener('click',e=>{
 const sb=e.target.closest('.seg button');if(sb&&sb.dataset.val!==undefined){const sg=sb.parentElement;sg.dataset.v=sb.dataset.val;$$('button',sg).forEach(b=>b.classList.toggle('on',b===sb));sg.dispatchEvent(new Event('change',{bubbles:true}));return}
 const mb=e.target.closest('.multi button');if(mb){mb.classList.toggle('on');const m=mb.parentElement;m.dataset.v=$$('button.on',m).map(b=>b.dataset.key).join(',');m.dispatchEvent(new Event('change',{bubbles:true}));return}
 const ib=e.target.closest('.i');if(ib){const h=(ib.closest('.l')||ib.parentElement).querySelector('.hint');if(h)h.hidden=!h.hidden}
});
