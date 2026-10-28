/* ---------- Авто ---------- */
/* швидкі мітки запису: слова сторінки (ca.quick, через «|»), не довідник прошивки */
const quickMarks=()=>t('ca.quick').split('|');
const fileRow=f=>`<div class="row" data-file="${f.f}"><div class="l"><b class="mono">${f.f}</b>${f.current?' '+chipH('rec',t('ca.writing')):''}<small>${esc(t('ca.fileSub',f.session,f.part,fmtB(f.size),fmtWall(f.wall)))}${f.protected?esc(t('ca.keep')):''}</small></div><div class="c"><button class="btn sm" type="button" data-dl>${t('bd.download')}</button><button class="btn sm${f.protected?' pri':''}" type="button" data-pr title="${esc(f.protected?t('ca.unlockT'):t('ca.lockT'))}">${f.protected?'🔒':'🔓'}</button><button class="btn sm dng" type="button" data-rm${f.current?' disabled':''}>✕</button></div></div>`;
/* поля Raise — з каталогу (CAT.raise); /api/map дає прив'язку і значення за id */
const raiseRows=mp=>CAT.raise.map(x=>({...x,...(mp.rows.find(r=>r.id===x.id)||{})}));
/* рядок /api/can/ids: id — число, data — суцільний hex, changed — маска байтів */
const idRow=x=>{const bytes=(x.data.match(/../g)||[]);return {id:'0x'+Number(x.id).toString(16).toUpperCase().padStart(3,'0'),count:x.count,bytes,ch:bytes.map((b,i)=>x.changed>>i&1)}};
/* обмін з ГУ: напрямок (ca.hu.<R|J|T|V>), відомі команди від ГУ (ca.hucmd.<cmd>), поле тексту 0x99 (ca.hufld.<n>) */
const huW=(p,k)=>tHas(p+k)?t(p+k):'';
const huRow=x=>`<tr class="${x.dir==='R'?'':'muted'}"><td class="num">${(x.ageMs/1000).toFixed(1)}</td><td>${huW('ca.hu.',x.dir)||x.dir}</td><td class="mono">${x.cmd}${huW('ca.hucmd.',x.cmd)?' · '+huW('ca.hucmd.',x.cmd):''}</td><td>${x.text!==undefined?'<b>'+esc(huW('ca.hufld.',x.field)||t('ca.field',x.field))+':</b> '+esc(x.text):`<span class="mono">${esc((x.data.match(/../g)||[]).join(' '))}</span>`}</td></tr>`;
const fmtV=r=>r.v==null?DASH:r.kind==='b'?(r.v?t('ca.yes'):t('ca.no')):(+r.v).toFixed(r.dec)+(r.unit&&!/–/.test(r.unit)?' '+r.unit:'');
/* правило поля ГУ (/api/hu/fields): інший сигнал, константа чи вимк. — вище за власний сигнал поля */
const ruleLbl=ru=>ru.from==='sig'?t('ca.fromSigL',ru.src)+(ru.mul!==1||ru.div!==1||ru.add?` ×${ru.mul}/${ru.div}${ru.add?(ru.add>0?'+':'')+ru.add:''}`:''):ru.from==='k'?t('ca.fromKL',ru.add):t('ca.fromOffL');
const srcBtn=r=>r.rule?`<button class="srcb k" type="button" data-ed>${esc(ruleLbl(r.rule))}</button>`:r.src==='can'?`<button class="srcb can" type="button" data-ed>${esc(t('ca.byte',r.can.id,r.can.byte))}${r.can.len>1?'–'+(r.can.byte+r.can.len-1):''}</button>`:r.src==='const'?`<button class="srcb k" type="button" data-ed>${esc(t('ca.const',r.k))}</button>`:r.calc&&r.src==='off'?`<button class="srcb k" type="button" data-ed title="${esc(t('ca.calcT'))}">${t('ca.calc')}</button>`:`<button class="srcb" type="button" data-ed>${t('ca.bind')}</button>`;
const vCell=r=>(r.rule?(r.sent==null?DASH:String(r.sent)):fmtV(r))+(r.how==='test'?t('ca.testLeft',r.left):'');
const mapRow=r=>`<tr data-id="${r.id}" data-g="${esc(r.group)}" class="${r.fw?'':'nofw'}${r.how==='test'?' testing':''}"><td>${esc(r.name)}<small>${r.fw?r.cmd:esc(r.why||'')}</small></td><td class="num" data-v>${esc(vCell(r))}</td><td>${srcBtn(r)}</td>
 <td>${r.fw?'':`<button class="btn sm" type="button" data-exp title="${esc(t('ca.expT'))}">${t('ca.exp')}</button>`}${!r.fw?'':`<div class="tcell">${r.kind==='b'?`<select class="in" data-t aria-label="${esc(t('ca.testVal'))}"><option value="1">${t('ca.yes')}</option><option value="0">${t('ca.no')}</option></select>`:`<input class="in" type="number" data-t step="${r.dec?Math.pow(10,-r.dec):1}" min="${r.min}" max="${r.max}" placeholder="${r.min}…${r.max}" aria-label="${esc(t('ca.testVal'))}">`}<button class="btn sm" type="button" data-send title="${esc(t('ca.sendT'))}">▶</button></div>`}</td></tr>`;
const mapEd=(r,sigs)=>{const c=r.can||{id:'',byte:0,len:1,shift:0,mask:'FF',mul:1,div:1,add:0,le:false,signed:false},ru=r.rule||{from:'auto',src:'',mul:1,div:1,add:0};return `<tr class="mapEd" data-for="${r.id}"><td colspan="4"><div class="mg">
 <label>${t('ca.from')}${sel('#from',[['auto',t('ca.fromOwn')],['sig',t('ca.fromSig')],['k',t('ca.fromK')],['off',t('ca.fromOff')]],ru.from,'data-str')}</label>
 <label data-fr="sig">${t('ca.fromSigPick')}${sel('#fsrc',(sigs||[]).map(m=>[m[0],lvName(m)]),ru.src,'data-str')}</label>
 <label data-fr="sig">×${num('#fmul',ru.mul,'step="1"')}</label><label data-fr="sig">÷${num('#fdiv',ru.div,'step="1" min="1"')}</label><label data-fr="sig">+${num('#fadd',ru.from==='sig'?ru.add:0,'step="1"')}</label>
 <label data-fr="k">${t('ca.value')}${num('#fk',ru.from==='k'?ru.add:0,'step="1"')}</label>
 <label data-fr="auto">${t('ca.src')}${sel('#src',enumOpts('mapSrc').map(o=>r.calc&&o[0]==='off'?['off',t('ca.calc')]:o),r.src,'data-str')}</label>
 <label data-s="can">ID<input class="in" data-f="id" list="idl" value="${esc(c.id)}" placeholder="0x354" autocomplete="off"></label>
 <label data-s="can">${t('ca.byteL')}${num('#byte',c.byte,'min="0" max="7"')}</label><label data-s="can">${t('ca.len')}${num('#len',c.len,'min="1" max="4"')}</label>
 <label data-s="can">${t('ca.shift')}${num('#shift',c.shift,'min="0" max="31"')}</label><label data-s="can">${t('ca.mask')}${txt('#mask',c.mask,'FF','in')}</label>
 <label data-s="can">×${num('#mul',c.mul,'step="any"')}</label><label data-s="can">÷${num('#div',c.div,'step="any"')}</label><label data-s="can">+${num('#add',c.add,'step="any"')}</label>
 <label data-s="can" class="ck"><input type="checkbox" class="cb" data-f="le"${c.le?' checked':''}> ${t('ca.le')}</label><label data-s="can" class="ck"><input type="checkbox" class="cb" data-f="signed"${c.signed?' checked':''}> ${t('ca.signed')}</label>
 <label data-s="const">${t('ca.value')}${num('#k',r.k,'step="any"')}</label></div>
 <div class="mf" style="margin-top:8px"><span class="mw" data-raw style="color:var(--mute)"></span>${has('vehicle')?`<button class="btn sm" type="button" data-learn>${t('ca.learn')}</button>`:''}<button class="btn sm pri" type="button" data-save>${t('ca.save')}</button><button class="btn sm" type="button" data-close>✕</button></div></td></tr>`};
SCR.car={title:'nav.car',async render(m){
 const C2=has('can2'),CP=has('capture'),V=has('vehicle');
 const DG=has('diag');
 const[c2,cp,mp,cb,dg,lp]=await Promise.all([getIf(C2,'/api/can2'),getIf(CP,'/api/cap'),API.get('/api/map'),getIf(V,'/api/can'),getIf(DG,'/api/diag'),getIf(has('logpush'),'/api/logpush')]);
 this.n=0;this.filesSig='';this.grp='all';this.learn={};this.mp=mp;
 /* фільтр за групою: значення — 'all' або назва групи, як у data-g рядка */
 const groups=[['all',t('ca.all')],...[...new Set(CAT.raise.map(x=>x.group))].map(g=>[g,g])];
 m.innerHTML=
 card(t('ca.live'),row(t('ca.liveRow'),`<a class="btn" href="#live">${t('ca.open')}</a>`,t('ca.liveSub')))+
 /* передачі (src/vehicle/Derived.h): наживо, як плата рахує; «Це N-та» пише поточне ratio в таблицю */
 (C2?card(t('ca.gears'),`<div class="kv"><div><small>${t('ca.gSpeed')}</small><b id="gV"></b></div><div><small>${t('ca.gRpm')}</small><b id="gR"></b></div><div><small>ratio</small><b id="gK"></b></div><div><small>${t('ca.gGear')}</small><b id="gG"></b></div><div><small>${t('ca.gClutch')}</small><b id="gC"></b></div></div>`+
  `<div class="tw"><table><thead><tr><th>#</th><th>${t('ca.gTable')}</th><th>${t('ca.gLearned')}</th><th></th></tr></thead><tbody id="gT">${[1,2,3,4,5,6].map(i=>`<tr data-g="${i}"><td>${i}</td><td>${num('/api/settings|gear'+i,'','min="0" max="999" style="max-width:6em"')}</td><td data-l class="mono"></td><td><button class="btn sm" type="button" data-set="${i}">${t('ca.gThis',i)}</button></td></tr>`).join('')}</tbody></table></div>`+
  row('','',t('ca.gearsH'))+`<div class="mono" id="gP"></div>`,'cGears'):'')+
 (C2?card('CAN A',row(t('ca.bus'),'<span id="cSt" class="chip off">…</span>','<span id="cStW"></span>')+
  `<div class="kv"><div><small>${t('ca.fps')}</small><b id="cFps"></b></div><div><small>${t('ca.total')}</small><b id="cFr"></b></div><div><small>rec / tec</small><b id="cErr"></b></div><div><small>${t('ca.overflows')}</small><b id="cOv"></b></div></div>`+
  row(t('ca.speed'),sel('/api/can2|bitrate',enumOpts('canRates'),c2.setting||0),'',t('ca.speedH'))):notBuilt('CAN A','CAN A'))+
 /* діагностика клімату (CarDiag): 21 C1 на CAN A, лише за вмикачем; проба — один запит */
 (dg?card(t('ca.diag'),row(t('ca.diagOn'),sw('/api/diag|on',dg.on),t('ca.diagOnSub'),t('ca.diagH'))+row(t('ca.diagIv'),num('/api/diag|interval',dg.interval,'min="2" max="60"'))+(dg.session!==undefined?row(t('ca.diagSes'),sw('/api/diag|session',dg.session),'',t('ca.diagSesH')):'')+
  row(t('ca.diagSt'),'<span id="dSt" class="chip off">…</span>','<span id="dW"></span>')+`<div class="fr"><button class="btn" type="button" id="dProbe">${t('ca.diagProbe')}</button><button class="btn" type="button" id="dSweep">${t('ca.diagSweep')}</button></div>`+row('','',t('ca.diagSweepH')),'cDiag'):'')+
 (CP?card(t('ca.rec'),`<div class="fr"><button class="btn rec big" id="cRec" type="button" style="flex:1">…</button></div>`+
  `<div class="fr"><input class="in" id="mkT" placeholder="${esc(t('ca.markPh'))}" maxlength="40" data-nosave><button class="btn pri" type="button" id="mkGo">${t('ca.mark')}</button></div>`+
  `<div class="pad"><div class="seg wrap" id="quick">${quickMarks().map(q=>`<button type="button" data-q="${esc(q)}">${esc(q)}</button>`).join('')}</div></div>`+
  `<div class="kv"><div><small>${t('ca.recorded')}</small><b id="pFr"></b></div><div><small>${t('ca.lost')}</small><b id="pDr"></b></div><div><small>${t('ca.marks')}</small><b id="pMk"></b></div><div><small>ID</small><b id="pId"></b></div><div><small>${t('ca.file')}</small><b id="pFile"></b></div><div><small>${t('ca.storage')}</small><b id="pSt"></b></div></div>`+
  '<div class="pad"><div class="bar big"><i id="pBar"></i></div></div>'+
  row(t('k.set.cap.autostart'),sw('/api/settings|cap.autostart',cp.settings.autostart),'',t('ca.autostartH'))+
  (lp?row(t('bd.push'),`<a class="btn" href="#board">${t('ca.pushWhere')}</a>`,'',t('ca.pushMoved')):'')+
  row(t('k.set.cap.mode'),seg('/api/settings|cap.mode',enumOpts('capModes'),cp.settings.mode)))+
 card(t('ca.files'),row('','',t('ca.filesH'))+`<div id="files"></div><div class="fr"><button class="btn dng" type="button" id="fUnp">${t('ca.delUnp')}</button></div>`,'cFiles','custom'):C2?notBuilt(t('ca.rec'),t('ca.noRec')):'')+
 card(t('ca.rav4'),
  row(t('ca.send'),sw('/api/map|send',mp.send),t('ca.kept'),t('ca.sendH'))+
  row(t('ca.units'),sel('#fuel',enumOpts('fuelUnits'),mp.units.fuel,`aria-label="${esc(t('ca.fuel'))}"`)+sel('#trip',enumOpts('tripUnits'),mp.units.trip,`aria-label="${esc(t('ca.trip'))}"`),'','','mUnits')+
  row(t('ca.exps'),'<span class="fr" id="mExp">'+[1,2,4,8].map(b=>'<label class="ck"><input type="checkbox" class="cb" data-b="'+b+'"'+((mp.exp|0)&b?' checked':'')+'> '+t('ca.exp.'+b)+'</label>').join(' ')+'</span>',t('ca.offDefault'),t('ca.expsH'),'mExpRow')+
  row(t('ca.batt'),'<span class="fr" id="mBatt"><label class="ck"><input type="checkbox" class="cb" id="mBattOn"'+((mp.exp|0)&16?' checked':'')+'> '+t('ca.on')+'</label><select class="in" id="mBattAt" aria-label="'+esc(t('ca.huField'))+'">'+[0,1,2].map(v=>'<option value="'+v+'"'+((((mp.exp|0)>>5)&3)===v?' selected':'')+'>'+t('ca.battAt.'+v)+'</option>').join('')+'</select></span>',t('ca.offDefault1'),t('ca.battH'),'mBattRow')+
  row(t('ca.sweep'),`<span class="fr"><select class="in" id="mSwT" aria-label="${esc(t('ca.step'))}">${[3,5,8].map(s=>`<option value="${s}"${s===5?' selected':''}>${t('ca.sec',s)}</option>`).join('')}</select><button class="btn" type="button" id="mSwGo">${t('ca.sweepGo')}</button><b class="mono" id="mSwV">—</b></span>`,'0x7D 06, D2 = 0…15',t('ca.sweepH'),'mSwRow')+
  `<div class="pad"><div class="seg wrap" id="mGrp" data-v="all">${groups.map(([v,n])=>`<button type="button" data-val="${esc(v)}" class="${v==='all'?'on':''}">${esc(n)}</button>`).join('')}</div></div>`+
  `<div class="fr"><button class="btn" type="button" id="mReset">${t('ca.factory')}</button></div>`+
  `<div class="tw"><table class="mapt"><thead><tr><th>${t('ca.thField')}</th><th>${t('ca.thNow')}</th><th>${t('ca.thFrom')}</th><th>${t('ca.thTest')}</th></tr></thead><tbody id="map">${raiseRows(mp).map(mapRow).join('')}</tbody></table></div><datalist id="idl"></datalist>`,'cMap','custom full')+
 fold(t('ca.hu'),row(t('ca.show'),seg('#huIn',[['1',t('ca.fromHu')],['0',t('ca.everything')]],'1'),'',t('ca.huH'))+`<div class="tw"><table><thead><tr><th>${t('ca.ago')}</th><th>${t('ca.who')}</th><th>${t('ca.cmd')}</th><th>${t('ca.data')}</th></tr></thead><tbody id="huLog"></tbody></table></div>`,'cHu')+
 (V?fold(t('ca.ids'),row(t('ca.idTable'),sw('/api/can/sniff|on',!!cb.sniff),'',t('ca.idTableH'))+`<div class="fr"><button class="btn" type="button" id="idBase">${t('ca.baseline')}</button><button class="btn" type="button" id="idClr">${t('ca.clear')}</button></div><div class="tw"><table><thead><tr><th>ID</th><th>${t('ca.count')}</th><th>${t('ca.bytes')}</th></tr></thead><tbody id="ids"></tbody></table></div>`,'cIds'):'');
 $$('.card:not(.custom)',m).forEach(c=>saver(c,genericSave,'[data-k]'));
 const gt=$('#gT');if(gt)gt.onclick=e=>{const b=e.target.closest('[data-set]');if(!b)return;const i=+b.dataset.set;
  if(!this.gK){sv(svOf(b),'bad',t('ca.gNoRatio'));return}
  run(b,()=>API.post('/api/settings',{['gear'+i]:this.gK}),t('ca.gSaved',i,this.gK)).then(()=>this.gears())};
 if(dg)$('#dSweep').onclick=e=>ask(e.target,t('ca.diagSweepAsk'),()=>run(e.target,async()=>{await API.post('/api/diag',{sweep:true});setTimeout(()=>this.diag(),2000)},t('ca.diagSent')));
 if(dg)$('#dProbe').onclick=e=>run(e.target,async()=>{await API.post('/api/diag',{probe:true});setTimeout(()=>this.diag(),1500)},t('ca.diagSent'));
 if(CP){$('#cRec').onclick=e=>run(e.target,()=>API.post(e.target.classList.contains('on')?'/api/cap/stop':'/api/cap/start')).then(()=>this.tick(true));
 const mk=(tx,b)=>run(b,()=>API.post('/api/cap/mark?t='+encodeURIComponent(tx)),t('ca.marked',tx));
 $('#mkGo').onclick=e=>{const tx=$('#mkT').value.trim()||t('ca.markDefault');mk(tx,e.target).then(()=>$('#mkT').value='')};
 $('#mkT').onkeydown=e=>{if(e.key==='Enter')$('#mkGo').click()};
 $('#quick').onclick=e=>{const b=e.target.closest('[data-q]');if(b)mk(b.dataset.q,b)};
 $('#files').onclick=e=>{const b=e.target.closest('button');if(!b)return;const r=b.closest('[data-file]');if(!r)return;const f=r.dataset.file;
  if(b.dataset.dl!==undefined)run(b,()=>download('/api/cap/get?f='+f,f+'.bin'));
  else if(b.dataset.pr!==undefined)run(b,()=>API.post('/api/cap/protect?f='+f+'&on='+(b.classList.contains('pri')?0:1))).then(()=>this.files(true));
  else if(b.dataset.rm!==undefined)ask(b,t('ca.delAsk',f,b.parentElement.querySelector('.pri')?t('ca.withLock'):''),()=>run(b,()=>API.post('/api/cap/delete?f='+f),t('ca.deleted')).then(()=>this.files(true)))};
 $('#fUnp').onclick=e=>ask(e.target,t('ca.delUnpAsk'),()=>run(e.target,()=>API.post('/api/cap/delete_unprotected').then(r=>({note:t('ca.deletedN',r.deleted)}))).then(()=>this.files(true)))}
 if(V){$('#idBase').onclick=e=>run(e.target,()=>API.post('/api/can/baseline'),t('ca.baseSet'));
 $('#idClr').onclick=e=>run(e.target,()=>API.post('/api/can/reset'),t('ca.cleared'))}
 // RAV4
 saver($('#cMap .row'),()=>API.post('/api/map',{send:$('[data-k="/api/map|send"]').checked}),'[data-k]');
 saver($('#mUnits'),()=>API.post('/api/map',{units:{fuel:val($('[data-f="fuel"]')),trip:val($('[data-f="trip"]'))}}),'[data-f]');
$('#mSwGo').onclick=async e=>{const b=e.target,v=$('#mSwV'),live=tok=>this.sw===tok&&document.contains(b);if(this.sw){this.sw=null;b.textContent=t('ca.sweepGo');return}
  if(!$('#mExp [data-b="8"]').checked){sv(svOf(b),'bad',t('ca.needSel'));return}
  const tok=this.sw={};b.textContent=t('ca.sweepStop');
  for(let n=0;n<16&&live(tok);n++){v.textContent='D2 = '+n;
   try{await API.post('/api/map/test',{id:'prnd',value:n})}catch(err){sv(svOf(b),'bad',errT(err));break}
   for(let w=+$('#mSwT').value*10;w>0&&live(tok);w--)await sleep(100)}
  if(this.sw===tok){this.sw=null;b.textContent=t('ca.sweepGo');sv(svOf(b),'ok',t('ca.sweepDone'))}};
/* експерименти ГУ: біти huExp; АКБ (16) з полем у бітах 5-6 займає поле іншого експерименту (Rav4Emitter::experimentsWhy) */
 const expSync=()=>{const on=$('#mBattOn').checked,at=+$('#mBattAt').value,rival=on?(at===1?2:4):0;
  $$('#mExp [data-b]').forEach(c=>{const b=+c.dataset.b,off=!!(rival&b);if(off)c.checked=false;c.disabled=off});$('#mBattAt').disabled=!on};
 expSync();
 const expSave=e=>{expSync();const x=$$('#mExp [data-b]').reduce((a,c)=>a|(c.checked?+c.dataset.b:0),0)|($('#mBattOn').checked?16|(+$('#mBattAt').value<<5):0);run(e.target,()=>API.post('/api/map',{exp:x}),t('ca.expsSent',x))};
 $('#mExp').onchange=expSave;$('#mBatt').onchange=expSave;
 $('#mReset').onclick=e=>ask(e.target,t('ca.resetAsk'),()=>run(e.target,()=>API.post('/api/map',{reset:true}),t('ca.factoryDone')).then(()=>this.map()));
 $('#mGrp').addEventListener('change',e=>{this.grp=e.currentTarget.dataset.v;$$('#map tr[data-id]').forEach(tr=>{const hide=this.grp!=='all'&&tr.dataset.g!==this.grp;tr.hidden=hide;const ed=$(`#map tr[data-for="${tr.dataset.id}"]`);if(ed)ed.hidden=hide})});
 const mapT=$('#map');
 mapT.addEventListener('click',async e=>{const b=e.target.closest('button');if(!b)return;
  const tr=b.closest('tr');
  if(b.dataset.exp!==undefined){const x=$('#mBattRow');x.scrollIntoView({behavior:'smooth',block:'center'});x.classList.add('hl');setTimeout(()=>x.classList.remove('hl'),2000);return}
  if(b.dataset.send!==undefined){const id=tr.dataset.id,v=$('[data-t]',tr).value;if(v===''){sv(svOf(b),'bad',t('ca.enterVal'));return}run(b,()=>API.post('/api/map/test',{id,value:+v})).then(()=>this.map());return}
  if(b.dataset.ed!==undefined){const id=tr.dataset.id,open=$(`#map tr[data-for="${id}"]`);if(open){open.remove();return}if(!this.sigs)this.sigs=(await API.get('/api/map?meta=1&live=1')).meta||[];const r=this.withRules(raiseRows(this.mp)).find(x=>x.id===id);tr.insertAdjacentHTML('afterend',mapEd(r,this.sigs));this.edVis($(`#map tr[data-for="${id}"]`));if(has('vehicle'))this.fillIds();return}
  const ed=b.closest('tr.mapEd');if(!ed)return;const id=ed.dataset.for;
  if(b.dataset.close!==undefined){ed.remove();return}
  if(b.dataset.save!==undefined){const g=f=>val($(`[data-f="${f}"]`,ed));const from=g('from')||'auto';
   if(from!=='auto'){const rule={id,from,src:g('fsrc'),mul:+g('fmul')||1,div:+g('fdiv')||1,add:from==='k'?(+g('fk')||0):(+g('fadd')||0)};
    run(b,()=>API.post('/api/hu/fields',{rows:[rule]}),t('ca.saved')).then(r=>{if(r){ed.remove();this.map(true)}});return}
   if(this.fl&&this.fl[id]&&this.fl[id].rule)await API.post('/api/hu/fields',{rows:[{id,from:'auto'}]});
   const src=g('src');
   const row={id,src,k:g('k')||0,can:src==='can'?{id:(g('id')||'').trim(),byte:g('byte'),len:g('len'),shift:g('shift')||0,mask:(g('mask')||'').replace(/^0x/i,''),mul:g('mul'),div:g('div'),add:g('add')||0,le:g('le'),signed:g('signed')}:null};
   run(b,()=>API.post('/api/map',{rows:[row]}),t('ca.saved')).then(r=>{if(r){ed.remove();this.map(true)}});return}
  if(b.dataset.learn!==undefined){const cid=val($('[data-f="id"]',ed)).trim().toUpperCase().replace('0X','0x');
   if(!this.learn[id]){await run(b,()=>API.post('/api/can/baseline'),t('ca.baseOk'));this.learn[id]=true;b.textContent=t('ca.showChanges');$('[data-raw]',ed).textContent=t('ca.doIt');return}
   const r=await API.get('/api/can/ids');const ch=r.ids.filter(x=>x.changed).map(idRow);const mine=ch.find(x=>x.id===cid);
   $('[data-raw]',ed).innerHTML=(mine?esc(t('ca.changedBytes',cid,mine.bytes.map((b,i)=>mine.ch[i]?i:null).filter(x=>x!==null).join(', '))):cid?esc(t('ca.notChanged',cid)):'')+esc(t('ca.changed'))+(ch.map(x=>`<a href="#" data-pick="${x.id}">${x.id}</a>`).join(' ')||esc(t('ca.nothing')));
   this.learn[id]=false;b.textContent=t('ca.learn')}});
 mapT.addEventListener('click',e=>{const a=e.target.closest('[data-pick]');if(!a)return;e.preventDefault();const ed=a.closest('tr.mapEd');$('[data-f="id"]',ed).value=a.dataset.pick});
 mapT.addEventListener('change',e=>{const ed=e.target.closest('tr.mapEd');if(ed&&(e.target.dataset.f==='src'||e.target.dataset.f==='from'))this.edVis(ed)});
 if(CP)await this.files(true);await this.tick(true);
},edVis(ed){const fr=val($('[data-f="from"]',ed))||'auto',s=val($('[data-f="src"]',ed));
 $$('[data-fr]',ed).forEach(l=>l.hidden=l.dataset.fr!==fr);$$('[data-s]',ed).forEach(l=>l.hidden=fr!=='auto'||l.dataset.s!==s)},
/* правила полів ГУ з /api/hu/fields на рядки мапи: r.rule, r.sent (те, що йде в ГУ) */
withRules(rows){const fl=this.fl||{};return rows.map(r=>{const f=fl[r.id];return f?{...r,rule:f.rule,sent:f.v}:r})},
async fillIds(){const r=await API.get('/api/can/ids');const d=$('#idl');if(d)d.innerHTML=r.ids.map(idRow).map(x=>`<option value="${x.id}">${x.bytes.join(' ')}</option>`).join('')},
async map(){const[mp,hf]=await Promise.all([API.get('/api/map'),API.get('/api/hu/fields').catch(()=>null)]);this.mp=mp;if(!$('#map'))return;
 this.fl=Object.fromEntries(((hf&&hf.rows)||[]).map(x=>[x.id,x]));
 this.withRules(raiseRows(mp)).forEach(r=>{const tr=$(`#map tr[data-id="${r.id}"]`);if(!tr)return;$('[data-v]',tr).textContent=vCell(r);tr.classList.toggle('testing',r.how==='test');
  const sb=tr.children[2];const h=srcBtn(r);if(sb.innerHTML!==h)sb.innerHTML=h;
  const ed=$(`#map tr[data-for="${r.id}"] [data-raw]`);if(ed&&!this.learn[r.id]&&!ed.querySelector('a'))ed.textContent=r.raw?r.raw+' → '+fmtV(r):''});
 put($('[data-k="/api/map|send"]'),mp.send)},
async files(force){const l=(await API.get('/api/cap/list')).files||[];const sig=JSON.stringify(l.map(f=>[f.f,f.protected,f.current,Math.round(f.size/65536)]));const el=$('#files');if(!el)return;
 if(!force&&(sig===this.filesSig||el.parentElement.querySelector('.ask')))return;this.filesSig=sig;el.innerHTML=l.length?l.slice().reverse().map(fileRow).join(''):`<div class="row muted">${esc(t('ca.noFiles'))}</div>`},
/* CarDiag: стан (чип), остання відповідь і салон */
async diag(){if(!has('diag')||!$('#dSt'))return;const v=await API.get('/api/diag');
 const ok=v.last==='ok',bad=v.state==='fault'||v.state==='stopped'||v.last==='negative';
 setChip('#dSt',bad?'bad':ok?'ok':v.state==='off'?'off':'warn',t('ca.diag.'+v.state));
 $('#dW').textContent=(v.cabin!==undefined?t('ca.diagCabin',v.cabin.toFixed(1),v.sun):'')+(v.last?' · '+v.last+(v.nrc?' 7F 21 '+v.nrc.toString(16).toUpperCase().padStart(2,'0'):''):'')+(v.reply?' · '+v.reply:'')},
/* картка передач: значення наживо, таблиця з налаштувань, піки навчання */
async gears(){if(!$('#gT'))return;const g=await API.get('/api/gears');const nv=v=>v===null||v===undefined?DASH:v;
 $('#gV').textContent=g.speed===null?DASH:(g.speed/100).toFixed(1);$('#gR').textContent=nv(g.rpm);$('#gK').textContent=nv(g.ratio);
 $('#gG').textContent=g.gear===null?DASH:g.gear<0?'R':g.gear===0?'N':g.gear;$('#gC').textContent=(g.clutch?t('ca.gPressed'):'—')+(g.neutral?' · N':'');
 this.gK=g.ratio;
 $$('#gT tr').forEach((tr,i)=>{const inp=$('input',tr);if(inp&&document.activeElement!==inp&&inp.value!==String(g.table[i]))inp.value=g.table[i];
  const p=(g.peaks||[]).map(x=>x[0]).reduce((b,x)=>b===null||Math.abs(x-g.table[i])<Math.abs(b-g.table[i])?x:b,null);
  $('[data-l]',tr).textContent=p!==null&&g.table[i]&&Math.abs(p-g.table[i])*100<=g.table[i]*20?p:DASH});
 $('#gP').textContent=(g.peaks||[]).length?t('ca.gPeaks',g.peaks.map(x=>x[0]+' ('+x[1]+' s)').join(', ')):t('ca.gNoPeaks')},
async tick(force){this.n++;await this.gears();const[c2,cp]=await Promise.all([getIf(has('can2'),'/api/can2'),getIf(has('capture'),'/api/cap')]);if(!$('#cMap'))return;
 if(c2){canChip($('#cSt'),c2);
 $('#cStW').textContent=c2.autobaud!=='manual'?t('ca.autobaud'):t('ca.manual',c2.setting/1000)+(c2.fps?'':t('ca.checkRate'));
 $('#cFps').textContent=fmtN(c2.fps);$('#cFr').textContent=fmtN(c2.frames);$('#cErr').textContent=c2.rec+' / '+c2.tec;$('#cOv').textContent=c2.hwOverflows;
 put($('[data-k="/api/can2|bitrate"]'),c2.setting||0)}
 if(cp){recBtn($('#cRec'),cp);
 $('#pFr').textContent=fmtN(cp.frames);$('#pDr').textContent=cp.dropped;$('#pMk').textContent=cp.markers;$('#pId').textContent=cp.ids;$('#pFile').textContent=cp.file?String(cp.file).padStart(4,'0'):DASH;
 $('#pSt').textContent=fmtB(cp.capBytes)+' / '+fmtB(cp.budget);const pb=$('#pBar');pb.style.width=(cp.capBytes/cp.budget*100)+'%';pb.style.background=cp.capBytes/cp.budget>cp.settings.fill/100*.95?'var(--warn)':'var(--acc)';
 put($('[data-k="/api/settings|cap.autostart"]'),cp.settings.autostart);put($('[data-k="/api/settings|cap.mode"]'),cp.settings.mode)}
 await this.map();if(cp&&(this.n%10===0||force))await this.files();
 await this.diag();
 const hl=$('#cHu');if(hl&&hl.open){const r=await API.get('/api/canbox/last?in='+$('#huIn').dataset.v);const tb=$('#huLog');if(tb)tb.innerHTML=(r.frames||[]).map(huRow).join('')||`<tr><td colspan="4" class="muted">${esc(t('ca.empty'))}</td></tr>`}
 const ids=$('#cIds');if(ids&&ids.open&&$('[data-k="/api/can/sniff|on"]').checked){const r=await API.get('/api/can/ids');const tb=$('#ids');if(tb)tb.innerHTML=r.ids.map(idRow).map(x=>`<tr><td class="mono">${x.id}</td><td class="num">${fmtN(x.count)}</td><td class="mono">${x.bytes.map((b,i)=>x.ch[i]?`<span class="ch">${b}</span>`:b).join(' ')}</td></tr>`).join('')||`<tr><td colspan="3" class="muted">${esc(t('ca.empty'))}</td></tr>`}
}};
