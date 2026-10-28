/* ============ Авто — наживо ============
   Усі сигнали Vehicle::State: значення, вік, звідки, чи перевірено в машині.
   GET /api/map?live=1 раз на 2 с (одне опитування на тік; вкладка схована —
   99-main.js не тікає), опис сигналів — ?meta=1 раз.
   Без вебсокетів. Правила сповіщень — таблиця реєстру налаштувань `alerts`:
   GET/POST /api/settings, рядок = [сигнал, умова, поріг, пауза, увімк],
   уся таблиця з 16 рядків; колонки й умови — з CAT.settings. */
/* група сигналу (meta[4]) — lv.grp.<n>; джерело — lv.src.<літера> і колір чипа */
const LV_SRC={c:'ok',k:'warn',p:'warn',d:'ok',o:'off'};
/* meta: [name, unit, decimals, yes/no, group, min, max, label]; sigs: [value, ageMs, src, verified, confirm]
   confirm: 1 — гіпотеза на CAN, можна «✓ підтвердити»; 2 — підтверджено тут, можна «↺ скасувати».
   POST /api/map {confirm:{sig,on}}; «Заводські прив'язки» повертають гіпотези. Назва — k.sig, k.raise */
const lvName=m=>tHas('k.sig.'+m[0])?t('k.sig.'+m[0]):((CAT.raise||[]).find(r=>r.id===m[0])||{}).name||m[7]||m[0];
const lvUnit=m=>m[1]&&!/^-?\d|^raw$/.test(m[1])?' '+unitT(m[1]):'';
const lvVal=(m,v)=>v==null?DASH:m[3]?(+v?t('lv.yes'):t('lv.no')):v+lvUnit(m);
/* сигнал, чию прив'язку відкрити, коли таблиця намалюється (посилання «змінити» з мапи ГУ) */
let LIVE_OPEN=null;
const lvAge=a=>a==null?DASH:a<1000?t('lv.lt1s'):a<60000?t('lv.sec',Math.round(a/1000)):t('lv.min',Math.round(a/60000));
SCR.live={title:'lv.title',hidden:true,async render(m){
 const[r,se]=await Promise.all([API.get('/api/map?meta=1&live=1'),API.get('/api/settings')]);
 const al=(CAT.settings||[]).find(f=>f.id==='alerts')||{cols:[]},cond=al.cols.find(c=>c.id==='cond')||{enum:[]};
 this.M=r.meta;this.C=cond.enum.map(o=>[o.v,o.name]);this.A=se.alerts||[];this.n=0;
 m.innerHTML=card(t('lv.signals'),row(t('lv.auto'),sw('#lvAuto',true,'data-nosave'),t('lv.autoSub'))+
  row(t('lv.only'),sw('#lvOnly',true,'data-nosave'))+row(t('lv.alerts'),`<span id="lvAl" class="muted">${DASH}</span>`,t('lv.alertsSub'))+
  `<datalist id="idl"></datalist><div class="tw"><table><thead><tr><th>${t('lv.thSig')}</th><th>${t('lv.thNow')}</th><th>${t('lv.thAge')}</th><th>${t('lv.thSrc')}</th><th title="${esc(t('lv.checkedT'))}">✓</th><th></th></tr></thead><tbody id="lvT"></tbody></table></div>`,'lvCard','custom full')+
 card(t('lv.alCard'),row('','',t('lv.alH'))+
  `<div class="tw"><table><thead><tr><th>${t('lv.thSig')}</th><th>${t('lv.thCond')}</th><th>${t('lv.thThr')}</th><th>${t('lv.thPause')}</th><th>${t('lv.thOn')}</th></tr></thead><tbody id="alT"></tbody></table></div>`+
  `<div class="fr"><button class="btn pri" type="button" id="alSave">${t('lv.alSave')}</button><button class="btn" type="button" id="alFac">${t('lv.alFactory')}</button></div>`,'alCard','custom');
 this.draw(r);this.drawAlerts();BIND.wire($('#lvT'));
 if(LIVE_OPEN){const id=LIVE_OPEN;LIVE_OPEN=null;const i=this.M.findIndex(x=>x[0]===id);
  if(i>=0){$('[data-f="lvOnly"]').checked=false;this.draw(this.last);await this.bind(i,id);const ed=$('#lvT tr.bindEd');if(ed)ed.scrollIntoView({block:'center'})}}
 $('[data-f="lvOnly"]').onchange=()=>this.draw(this.last);
 /* заводські правила (src/vehicle/AlertRules.cpp factory()) у порожні рядки; свої лишаються */
 $('#alFac').onclick=e=>{this.A=this.readAlerts();let add=0,full=false;
  for(const f of [['fuel_low','rises',0,300,true],['drive_no_lights','rises',0,60,true],['drive_handbrake','rises',0,30,true]]){
   const have=this.A.find(a=>a[0]===f[0]);if(have){have[4]=true;continue}
   const i=this.A.findIndex(a=>!a[0]);if(i<0){full=true;break}this.A[i]=f.slice();add++}
  this.drawAlerts();if(full){sv(svOf(e.target),'bad',t('lv.all16'));return}
  run(e.target,()=>API.post('/api/settings',{alerts:this.A.map(a=>[a[0],a[1],+a[2]||0,parseInt(a[3])||0,!!a[4]])}),t('lv.alFactoryDone',add))};
 $('#alSave').onclick=e=>{this.A=this.readAlerts();run(e.target,()=>API.post('/api/settings',{alerts:this.A.map(a=>[a[0],a[1],+a[2]||0,parseInt(a[3])||0,!!a[4]])}),t('lv.saved'))};
 $('#lvT').onclick=e=>{const c=e.target.closest('[data-conf]');
  if(c){const mm=this.M[+c.dataset.conf],on=c.dataset.on==='1';
   ask(c,lvName(mm)+'. '+(on?t('lv.confOn'):t('lv.confOff')),()=>run(c,()=>API.post('/api/map',{confirm:{sig:mm[0],on}}),on?t('lv.confirmed'):t('lv.hypothesis')).then(async()=>this.draw(await API.get('/api/map?live=1'))));return}
  const bb=e.target.closest('button');const be=bb&&bb.closest('tr.bindEd');
  if(be){BIND.click(bb,be,async()=>this.draw(await API.get('/api/map?live=1')));return}
  const bd=e.target.closest('[data-bind]');if(bd){this.bind(+bd.dataset.bind,this.M[+bd.dataset.bind][0]);return}
  const b=e.target.closest('[data-add]');if(!b)return;this.A=this.readAlerts();
  const i=this.A.findIndex(a=>!a[0]);if(i<0){sv(svOf($('#alSave')),'bad',t('lv.all16'));return}
  const mm=this.M[+b.dataset.add];this.A[i]=[mm[0],mm[3]?'rises':'above',0,60,true];this.drawAlerts();$('#alCard').scrollIntoView({behavior:'smooth'})};
},
draw(r){this.last=r;const tb=$('#lvT');if(!tb||!r||!r.sigs||$('.ask',tb)||$('.bindEd',tb))return;const only=$('[data-f="lvOnly"]').checked;let h='',g=-1;
 const idx=this.M.map((m,i)=>i).filter(i=>!only||r.sigs[i][0]!=null).sort((a,b)=>this.M[a][4]-this.M[b][4]||a-b);
 for(const i of idx){const m=this.M[i],[v,age,src,ok,cf]=r.sigs[i];
  const vh=(ok?'✓':`<span class="muted" title="${esc(src==='d'?t('lv.computedT'):t('lv.uncheckedT'))}">?</span>`)+
   (cf===1?` <button class="btn sm" type="button" data-conf="${i}" data-on="1" title="${esc(t('lv.confirmT'))}">${t('lv.confirm')}</button>`:
    cf===2?` <button class="btn sm" type="button" data-conf="${i}" data-on="0" title="${esc(t('lv.undoT'))}">${t('lv.undo')}</button>`:'');
  if(m[4]!==g){g=m[4];h+=`<tr><th colspan="6">${esc(tHas('lv.grp.'+g)?t('lv.grp.'+g):'')}</th></tr>`}
  const stale=src==='c'&&age!=null&&age>3000,sk=LV_SRC[src]?src:'o';
  h+=`<tr${stale?' class="muted"':''}><td>${esc(lvName(m))} <small class="mono muted">${esc(m[0])}</small></td><td class="num">${esc(lvVal(m,v))}</td><td class="num">${lvAge(age)}</td><td><button class="srcb" type="button" data-bind="${i}" title="${esc(t('lv.bindT'))}">${chipH(LV_SRC[sk],t('lv.src.'+sk))}</button></td><td style="white-space:nowrap">${vh}</td><td><button class="btn sm" type="button" data-add="${i}" title="${esc(t('lv.alertT'))}">🔔</button></td></tr>`}
 tb.innerHTML=h||`<tr><td colspan="6" class="muted">${esc(t('lv.noValues'))}</td></tr>`;
 if(r.alertStats)$('#lvAl').textContent=r.alertStats.fired+' / '+r.alertStats.shown+' / '+r.alertStats.dropped},
drawAlerts(){const so=[['','—'],...this.M.map(m=>[m[0],lvName(m)])],co=this.C.map(c=>[c[0],c[1]]);
 $('#alT').innerHTML=this.A.map((a,i)=>`<tr data-r="${i}"><td>${sel('#sig',so,a[0],'data-str')}</td><td>${sel('#cond',co,a[1],'data-str')}</td><td>${num('#thr',a[2],'step="any" style="width:6em"')}</td><td>${num('#cd',a[3],'min="5" max="65535" style="width:5em"')}</td><td>${sw('#on',a[4])}</td></tr>`).join('')},
/* прив'язка одного сигналу: рядок під ним, з GET /api/map?sig= */
async bind(i,id){const open=$(`#lvT tr.bindEd[data-for="${id}"]`);if(open){open.remove();return}
 const r=((await API.get('/api/map?sig='+encodeURIComponent(id))).rows||[])[0];
 /* після запиту: таблиця могла перемалюватися (тік кожні 2 с) — рядок шукаємо знову */
 const b=$(`#lvT [data-bind="${i}"]`),tr=b&&b.closest('tr');if(!r||!tr||$('#lvT tr.bindEd'))return;
 tr.insertAdjacentHTML('afterend',bindEd(r,6));BIND.vis($(`#lvT tr.bindEd[data-for="${id}"]`));if(has('vehicle'))BIND.ids()},
readAlerts(){return $$('#alT tr').map(tr=>{const g=f=>val($(`[data-f="${f}"]`,tr));return [g('sig')||'',g('cond')||'rises',g('thr')??0,g('cd')??0,!!g('on')]})},
async tick(){const a=$('[data-f="lvAuto"]');if(!a||!a.checked||this.n++%2)return;const r=await API.get('/api/map?live=1');this.draw(r)}};
