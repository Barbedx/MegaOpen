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
const lvAge=a=>a==null?DASH:a<1000?t('lv.lt1s'):a<60000?t('lv.sec',Math.round(a/1000)):t('lv.min',Math.round(a/60000));
/* Запис для перевірки в машині: те, що сторінка й так бере раз на 2 с (жодного
   зайвого запиту), у памʼять цієї вкладки рядком JSON на зріз (так і рахує
   розмір), до REC_MAX зрізів (2 год). Стоп — пауза: «Записати» дописує далі,
   час у CSV показує прогалини. Позначки — рядки з текстом між зрізами. Ніде не зберігається: перезавантаження
   сторінки його губить, тому до того — «Завантажити CSV»; поки є незбережене,
   beforeunload питає (лише тоді: зайвий слухач вішав навігацію headless-Chrome у ui_audit). */
const REC_MAX=3600,recMarks=()=>t('lv.marks').split('|');
const REC={on:false,rows:[],n:0,bytes:0,M:null,saved:true};
const recUnload=e=>{e.preventDefault();e.returnValue=''};
const recSaved=v=>{REC.saved=v;window[(v?'remove':'add')+'EventListener']('beforeunload',recUnload)};
const recAdd=(M,r)=>{if(!REC.on||!r||!r.sigs)return;REC.M=REC.M||M;
 const s=JSON.stringify(r.sigs);REC.rows.push({t:Date.now(),s});REC.n++;REC.bytes+=s.length+16;recSaved(false);if(REC.n>=REC_MAX)REC.on=false};
const recMark=tx=>{tx=(tx||'').trim();if(!tx||!REC.M)return false;REC.rows.push({t:Date.now(),mark:tx});REC.bytes+=tx.length+16;recSaved(false);return true};
const recStat=()=>{if(!REC.rows.length)return REC.on?t('lv.waitFirst'):t('lv.empty');const a=REC.rows[0].t,b=REC.rows[REC.rows.length-1].t;
 return (REC.on?'● ':REC.n>=REC_MAX?t('lv.full'):'')+t('lv.recStat',fmtUp(b-a),REC.n,REC.rows.length-REC.n,fmtB(REC.bytes))};
const csvQ=x=>{const s=x==null?'':String(x);return /[",\n\r]/.test(s)?'"'+s.replace(/"/g,'""')+'"':s};
const recTime=ms=>{const d=new Date(ms),p=(n,w=2)=>String(n).padStart(w,'0');return d.getFullYear()+'-'+p(d.getMonth()+1)+'-'+p(d.getDate())+' '+p(d.getHours())+':'+p(d.getMinutes())+':'+p(d.getSeconds())+'.'+p(d.getMilliseconds(),3)};
/* CSV: час, секунд від першого рядка, позначка; далі на кожен сигнал
   значення, перевірено (1/0) і вік, мс. BOM — щоб Excel узяв UTF-8. */
function recCsv(){const M=REC.M||[],t0=REC.rows.length?REC.rows[0].t:0,L=[];
 L.push(['time','s','mark',...M.flatMap(m=>[m[0],m[0]+'.ok',m[0]+'.age_ms'])].map(csvQ).join(','));
 for(const r of REC.rows){const h=[recTime(r.t),((r.t-t0)/1000).toFixed(1),r.mark||''];
  if(r.s){const S=JSON.parse(r.s);M.forEach((m,i)=>{const s=S[i]||[];h.push(s[0],s[3]?1:0,s[1])})}else M.forEach(()=>h.push('','',''));
  L.push(h.map(csvQ).join(','))}
 return '﻿'+L.join('\r\n')+'\r\n'}
function recSave(){const a=document.createElement('a'),stamp=recTime(REC.rows[0].t).slice(0,16).replace(/[-: ]/g,'');
 a.href=URL.createObjectURL(new Blob([recCsv()],{type:'text/csv;charset=utf-8'}));a.download='megaopen-live-'+stamp+'.csv';
 document.body.appendChild(a);a.click();a.remove();setTimeout(()=>URL.revokeObjectURL(a.href),60000);recSaved(true)}
SCR.live={title:'lv.title',hidden:true,async render(m){
 const[r,se]=await Promise.all([API.get('/api/map?meta=1&live=1'),API.get('/api/settings')]);
 const al=(CAT.settings||[]).find(f=>f.id==='alerts')||{cols:[]},cond=al.cols.find(c=>c.id==='cond')||{enum:[]};
 this.M=r.meta;this.C=cond.enum.map(o=>[o.v,o.name]);this.A=se.alerts||[];this.n=0;
 m.innerHTML=card(t('lv.recCard'),row(t('lv.rec'),'<button class="btn pri" type="button" id="rcGo"></button>',`<span id="rcSt" class="mono"></span>`)+
  `<div class="pad"><div class="seg wrap" id="rcMk">${recMarks().map(k=>`<button type="button" data-mk="${esc(k)}">${esc(k)}</button>`).join('')}</div></div>`+
  `<div class="fr">${txt('#rcTxt','',t('lv.ownMark'),'in','text',`data-nosave maxlength="60" aria-label="${esc(t('lv.ownMark'))}"`)}<button class="btn" type="button" id="rcAdd">${t('lv.mark')}</button></div>`+
  `<div class="fr"><button class="btn" type="button" id="rcCsv">${t('lv.csv')}</button><button class="btn dng" type="button" id="rcClr">${t('lv.clear')}</button></div>`+
  row('','',t('lv.recH')),'rcCard','custom')+
  card(t('lv.signals'),row(t('lv.auto'),sw('#lvAuto',true,'data-nosave'),t('lv.autoSub'))+
  row(t('lv.only'),sw('#lvOnly',true,'data-nosave'))+row(t('lv.alerts'),`<span id="lvAl" class="muted">${DASH}</span>`,t('lv.alertsSub'))+
  `<div class="tw"><table><thead><tr><th>${t('lv.thSig')}</th><th>${t('lv.thNow')}</th><th>${t('lv.thAge')}</th><th>${t('lv.thSrc')}</th><th title="${esc(t('lv.checkedT'))}">✓</th><th></th></tr></thead><tbody id="lvT"></tbody></table></div>`,'lvCard','custom full')+
 card(t('lv.alCard'),row('','',t('lv.alH'))+
  `<div class="tw"><table><thead><tr><th>${t('lv.thSig')}</th><th>${t('lv.thCond')}</th><th>${t('lv.thThr')}</th><th>${t('lv.thPause')}</th><th>${t('lv.thOn')}</th></tr></thead><tbody id="alT"></tbody></table></div>`+
  `<div class="fr"><button class="btn pri" type="button" id="alSave">${t('lv.alSave')}</button></div>`,'alCard','custom');
 this.draw(r);this.drawAlerts();recAdd(this.M,r);this.drawRec();
 $('[data-f="lvOnly"]').onchange=()=>this.draw(this.last);
 $('#rcGo').onclick=()=>{REC.on=!REC.on&&REC.n<REC_MAX;if(REC.on){REC.M=REC.M||this.M;const a=$('[data-f="lvAuto"]');if(a)a.checked=true}this.drawRec()};
 const mark=(tx,el)=>{if(recMark(tx)){sv(svOf(el),'ok',tx);this.drawRec();return true}sv(svOf(el),'bad',REC.M?t('lv.emptyMark'):t('lv.recFirst'));return false};
 $('#rcMk').onclick=e=>{const b=e.target.closest('[data-mk]');if(b)mark(b.dataset.mk,b)};
 $('#rcAdd').onclick=e=>{const i=$('[data-f="rcTxt"]');if(mark(i.value,e.target))i.value=''};
 $('#rcCsv').onclick=e=>{if(!REC.rows.length){sv(svOf(e.target),'bad',t('lv.empty'));return}recSave();sv(svOf(e.target),'ok','CSV');this.drawRec()};
 $('#rcClr').onclick=e=>{const b=e.target;if(!REC.saved&&!b.dataset.sure){b.dataset.sure=1;b.textContent=t('lv.notSaved');return}
  Object.assign(REC,{on:false,rows:[],n:0,bytes:0,M:null});recSaved(true);delete b.dataset.sure;b.textContent=t('lv.clear');this.drawRec()};
 $('#alSave').onclick=e=>{this.A=this.readAlerts();run(e.target,()=>API.post('/api/settings',{alerts:this.A.map(a=>[a[0],a[1],+a[2]||0,parseInt(a[3])||0,!!a[4]])}),t('lv.saved'))};
 $('#lvT').onclick=e=>{const c=e.target.closest('[data-conf]');
  if(c){const mm=this.M[+c.dataset.conf],on=c.dataset.on==='1';
   ask(c,lvName(mm)+'. '+(on?t('lv.confOn'):t('lv.confOff')),()=>run(c,()=>API.post('/api/map',{confirm:{sig:mm[0],on}}),on?t('lv.confirmed'):t('lv.hypothesis')).then(async()=>this.draw(await API.get('/api/map?live=1'))));return}
  const b=e.target.closest('[data-add]');if(!b)return;this.A=this.readAlerts();
  const i=this.A.findIndex(a=>!a[0]);if(i<0){sv(svOf($('#alSave')),'bad',t('lv.all16'));return}
  const mm=this.M[+b.dataset.add];this.A[i]=[mm[0],mm[3]?'rises':'above',0,60,true];this.drawAlerts();$('#alCard').scrollIntoView({behavior:'smooth'})};
},
draw(r){this.last=r;const tb=$('#lvT');if(!tb||!r||!r.sigs||$('.ask',tb))return;const only=$('[data-f="lvOnly"]').checked;let h='',g=-1;
 const idx=this.M.map((m,i)=>i).filter(i=>!only||r.sigs[i][0]!=null).sort((a,b)=>this.M[a][4]-this.M[b][4]||a-b);
 for(const i of idx){const m=this.M[i],[v,age,src,ok,cf]=r.sigs[i];
  const vh=(ok?'✓':`<span class="muted" title="${esc(src==='d'?t('lv.computedT'):t('lv.uncheckedT'))}">?</span>`)+
   (cf===1?` <button class="btn sm" type="button" data-conf="${i}" data-on="1" title="${esc(t('lv.confirmT'))}">${t('lv.confirm')}</button>`:
    cf===2?` <button class="btn sm" type="button" data-conf="${i}" data-on="0" title="${esc(t('lv.undoT'))}">${t('lv.undo')}</button>`:'');
  if(m[4]!==g){g=m[4];h+=`<tr><th colspan="6">${esc(tHas('lv.grp.'+g)?t('lv.grp.'+g):'')}</th></tr>`}
  const stale=src==='c'&&age!=null&&age>3000,sk=LV_SRC[src]?src:'o';
  h+=`<tr${stale?' class="muted"':''}><td>${esc(lvName(m))} <small class="mono muted">${esc(m[0])}</small></td><td class="num">${esc(lvVal(m,v))}</td><td class="num">${lvAge(age)}</td><td>${chipH(LV_SRC[sk],t('lv.src.'+sk))}</td><td style="white-space:nowrap">${vh}</td><td><button class="btn sm" type="button" data-add="${i}" title="${esc(t('lv.alertT'))}">🔔</button></td></tr>`}
 tb.innerHTML=h||`<tr><td colspan="6" class="muted">${esc(t('lv.noValues'))}</td></tr>`;
 if(r.alertStats)$('#lvAl').textContent=r.alertStats.fired+' / '+r.alertStats.shown+' / '+r.alertStats.dropped},
drawAlerts(){const so=[['','—'],...this.M.map(m=>[m[0],lvName(m)])],co=this.C.map(c=>[c[0],c[1]]);
 $('#alT').innerHTML=this.A.map((a,i)=>`<tr data-r="${i}"><td>${sel('#sig',so,a[0],'data-str')}</td><td>${sel('#cond',co,a[1],'data-str')}</td><td>${num('#thr',a[2],'step="any" style="width:6em"')}</td><td>${num('#cd',a[3],'min="5" max="65535" style="width:5em"')}</td><td>${sw('#on',a[4])}</td></tr>`).join('')},
readAlerts(){return $$('#alT tr').map(tr=>{const g=f=>val($(`[data-f="${f}"]`,tr));return [g('sig')||'',g('cond')||'rises',g('thr')??0,g('cd')??0,!!g('on')]})},
drawRec(){const g=$('#rcGo'),s=$('#rcSt');if(!g)return;g.textContent=REC.on?t('lv.stop'):t('lv.record');g.classList.toggle('dng',REC.on);g.classList.toggle('pri',!REC.on);
 g.disabled=!REC.on&&REC.n>=REC_MAX;s.textContent=recStat();$('#rcCsv').disabled=!REC.rows.length},
async tick(){const a=$('[data-f="lvAuto"]');if(!a||!a.checked||this.n++%2)return;const r=await API.get('/api/map?live=1');this.draw(r);recAdd(this.M,r);this.drawRec()}};
