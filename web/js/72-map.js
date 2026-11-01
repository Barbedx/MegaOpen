/* ============ Мапа: сигнали, поля ГУ, формули, правила ============
   Одна сторінка для ланцюжка «CAN-рядок → сигнал → поле ГУ», чотири вкладки:
   #map — сигнали (Vehicle::State: значення, вік, звідки, чи перевірено в машині),
   #map:fields — що отримує ГУ (поле, «Звідки», тест), #map:formulas — формули,
   #map:rules — правила (src/vehicle/Rules.h: «коли → що зробити»).
   Вкладка — у хеші; перемикання — history.replaceState, без перемальовки сторінки.
   Опитування: одне на тік, лише видима вкладка — сигнали GET /api/map?live=1,
   поля GET /api/map + /api/hu/fields, раз на 2 с (вкладка браузера схована —
   99-main.js не тікає); опис сигналів — ?meta=1 раз. Без вебсокетів.
   Правила — GET/POST /api/rules (рядки як у файлі авто, CarFile::ruleRowTo), жива
   частина раз на 2 с, лише коли вкладка відкрита; «спробувати» — POST /api/rules/test. */
/* група сигналу (meta[4]) — lv.grp.<n>; джерело — lv.src.<літера> і колір чипа */
const LV_SRC={c:'ok',k:'warn',p:'warn',d:'ok',f:'ok',o:'off'};
/* клас джерела (docs/design/rules.md, крок 1): CAN, перевірений чи ні; плата; стала (або закріплено); нема */
const LV_CLS=['all','canOk','canHyp','board','user','const','none'];
/* формула: свій сигнал (група GRP_USER, meta[4] === 12) — «Свої», вбудований — «Плата» */
const lvCls=(src,ok,m)=>src==='c'?(ok?'canOk':'canHyp'):src==='f'?(m&&m[4]===12?'user':'board'):src==='d'?'board':src==='k'||src==='p'?'const':'none';
/* meta: [name, unit, decimals, yes/no, group, min, max, label]; sigs: [value, ageMs, src, verified, confirm]
   confirm: 1 — гіпотеза на CAN, можна «✓ підтвердити»; 2 — підтверджено тут, можна «↺ скасувати».
   POST /api/map {confirm:{sig,on}}; «Заводські прив'язки» повертають гіпотези. Назва — k.sig, k.raise */
const lvName=m=>tHas('k.sig.'+m[0])?t('k.sig.'+m[0]):((CAT.raise||[]).find(r=>r.id===m[0])||{}).name||m[7]||m[0];
const lvUnit=m=>m[1]&&!/^-?\d|^raw$/.test(m[1])?' '+unitT(m[1]):'';
const lvVal=(m,v)=>v==null?DASH:m[3]?(+v?t('lv.yes'):t('lv.no')):v+lvUnit(m);
const lvAge=a=>a==null?DASH:a<1000?t('lv.lt1s'):a<60000?t('lv.sec',Math.round(a/1000)):t('lv.min',Math.round(a/60000));
/* поля Raise — з каталогу (CAT.raise); /api/map дає прив'язку і значення за id */
const raiseRows=mp=>CAT.raise.map(x=>({...x,...(mp.rows.find(r=>r.id===x.id)||{})}));
/* правило поля ГУ (/api/hu/fields): своє (r.rule) або протоколу (r.def, напр. поле, що спершу вимкнене); над власним сигналом поля */
const ruleLbl=ru=>ru.from==='own'?t('ca.fromOwnL'):ru.from==='sig'?t('ca.fromSigL',ru.src)+(ru.mul!==1||ru.div!==1||ru.add?` ×${ru.mul}/${ru.div}${ru.add?(ru.add>0?'+':'')+ru.add:''}`:''):ru.from==='k'?t('ca.fromKL',ru.add):t('ca.fromOffL');
/* сигнал, який бере поле ГУ (рядок /api/hu/fields): власний (правила нема або «own») чи інший (sig); стала / вимк — жодного */
const fieldSig=f=>{const ru=f.rule||f.def;return !ru||ru.from==='own'?f.sig||f.id:ru.from==='sig'?ru.src:null};
const srcBtn=r=>r.rule?`<button class="srcb k" type="button" data-ed>${esc(ruleLbl(r.rule))}</button>`:r.def?`<button class="srcb" type="button" data-ed>${esc(t('ca.fromDefL',ruleLbl(r.def)))}</button>`:r.src==='can'?`<button class="srcb can" type="button" data-ed>${esc(t('ca.byte',r.can.id,r.can.byte))}${r.can.len>1?'–'+(r.can.byte+r.can.len-1):''}</button>`:r.src==='const'?`<button class="srcb k" type="button" data-ed>${esc(t('ca.const',r.k))}</button>`:r.calc&&r.src==='off'?`<button class="srcb k" type="button" data-ed title="${esc(t('ca.calcT'))}">${t('ca.calc')}</button>`:`<button class="srcb" type="button" data-ed>${t('ca.bind')}</button>`;
const vCell=r=>(r.rule||r.def?(r.sent==null?DASH:String(r.sent)):fmtV(r))+(r.how==='test'?t('ca.testLeft',r.left):'');
const mapRow=r=>`<tr data-id="${r.id}" data-g="${esc(r.group)}"${r.def?' data-def="1"':''} class="${r.fw?'':'nofw'}${r.how==='test'?' testing':''}"><td>${esc(r.name)}<small>${r.fw?r.cmd:esc(r.why||'')}</small></td><td class="num" data-v>${esc(vCell(r))}</td><td>${srcBtn(r)}</td>
 <td>${!r.fw?'':`<div class="tcell">${r.kind==='b'?`<select class="in" data-t aria-label="${esc(t('ca.testVal'))}"><option value="1">${t('ca.yes')}</option><option value="0">${t('ca.no')}</option></select>`:`<input class="in" type="number" data-t step="${r.dec?Math.pow(10,-r.dec):1}" min="${r.min}" max="${r.max}" placeholder="${r.min}…${r.max}" aria-label="${esc(t('ca.testVal'))}">`}<button class="btn sm" type="button" data-send title="${esc(t('ca.sendT'))}">▶</button></div>`}</td></tr>`;
/* редактор «Звідки» поля ГУ; посилання [data-goto] — до сигналу на вкладці «Сигнали», з відкритою прив'язкою */
const mapEd=(r,sigs)=>{const ru=r.rule||{from:'auto',src:'',mul:1,div:1,add:0},cs=ru.src||((sigs||[])[0]||[])[0]||'';return `<tr class="mapEd" data-for="${r.id}"><td colspan="4"><div class="mg">
 <label>${t('ca.from')}${sel('#from',[['auto',r.def?t('ca.fromDefL',ruleLbl(r.def)):t('ca.fromOwn')],...(r.def?[['own',t('ca.fromOwnL')]]:[]),['sig',t('ca.fromSig')],['k',t('ca.fromK')],['off',t('ca.fromOff')]],ru.from,'data-str')}</label>
 <label data-fr="sig">${t('ca.fromSigPick')}${sel('#fsrc',(sigs||[]).map(m=>[m[0],lvName(m)]),ru.src,'data-str')}<a href="#" data-goto="${esc(cs)}" data-gosrc>${esc(lvName((sigs||[]).find(m=>m[0]===cs)||[cs]))}</a></label>
 <label data-fr="sig">×${num('#fmul',ru.mul,'step="1"')}</label><label data-fr="sig">÷${num('#fdiv',ru.div,'step="1" min="1"')}</label><label data-fr="sig">+${num('#fadd',ru.from==='sig'?ru.add:0,'step="1"')}</label>
 <label data-fr="k">${t('ca.value')}${num('#fk',ru.from==='k'?ru.add:0,'step="1"')}</label>
 <span data-fr="auto" class="mw">${esc(t('ca.ownIs',bindLbl(r)))} <a href="#" data-goto="${r.id}">${t('ca.ownEdit')}</a></span></div>
 <div class="mf" style="margin-top:8px"><button class="btn sm pri" type="button" data-save>${t('ca.save')}</button><button class="btn sm" type="button" data-close>✕</button></div></td></tr>`};
/* рядок на 2 с підсвічено (.hl), прокручено до середини */
const hlRow=tr=>{if(!tr)return;tr.scrollIntoView({block:'center'});tr.classList.add('hl');setTimeout(()=>tr.classList.remove('hl'),2000)};
/* вкладка -> її картка; картки прямо в #main (широкий екран: .full на всю ширину, не в колонки) */
/* правило на сторінці: картка (.rule); жест — поле .multi з клавішами, як було в макросів */
const ruFld=fx=>(enumOf('ruleEffects',fx)||{}).field||'';
const ruEffects=()=>enumOpts('ruleEffects').filter(o=>o[0]!=='none');
const ruLive=l=>!l?'':l.on===false?t('ru.off'):[l.holds?t('ru.holds'):'',l.fires?t('ru.fires',l.fires)+(l.agoMs!=null?' · '+t('ru.ago',fmtUp(l.agoMs)):''):''].filter(Boolean).join(' · ')||t('ru.idle');
const actHTML=(a,k)=>{const fld=ruFld(a.effect);return `<div class="act" data-a="${k}"><div class="mg">
 <label>${t('ru.effect')}${sel('#effect',ruEffects(),a.effect,'data-str')}</label>
 <label data-show="action">${t('ru.action')}${sel('#action',CAT.actions.map(x=>[x.id,x.name]),a.action||'None','data-str')}</label>
 <label data-show="code raw">${t('ru.codeHex')}${txt('#codeHex',a.codeHex||'','07','in code')}</label>
 <label data-show="raw">${t('ru.dataHex')}${txt('#data',a.data||'','01 00','in')}</label>
 <label data-show="text">${t('ru.text')}${txt('#arg',fld==='text'?(a.arg||''):'',t('ru.textPh'),'in','text','maxlength="39"')}</label>
 <label data-show="scene">${t('ru.scene')}${sel('#argScene',CAT.scenes.map(s=>[s.id,s.name]),fld==='scene'?a.arg:'','data-str')}</label></div>
 <button class="btn sm" type="button" data-delact title="${esc(t('ru.delAct'))}">✕</button></div>`};
const ruleHTML=(x,i)=>{const g=x.gesture,isG=!!g,keys=isG?(g.keys||[]).filter(k=>realActions().some(a=>a.id===k)):[];
 return `<div class="rule mac" data-r="${i}" data-kind="${isG?'g':'s'}">
 <div class="mh">${sw('#on',x.on!==false,`aria-label="${esc(t('ru.on'))}"`)}<input class="in grow" data-f="name" value="${esc(x.name||'')}" maxlength="31" aria-label="${esc(t('ru.name'))}">${chipH(isG?'warn':'ok',isG?t('ru.kindGesture'):t('ru.kindSig'))}<span class="chip off" data-live>${esc(ruLive(x.live))}</span></div>
 ${isG?`<div class="multi" data-f="keys" data-v="${esc(keys.join(','))}">${realActions().map(a=>`<button type="button" data-key="${esc(a.id)}" class="${keys.includes(a.id)?'on':''}">${esc(a.name)}</button>`).join('')}</div>
 <div class="mg"><label>${t('ru.trigger')}${sel('#trigger',enumOpts('ruleTriggers').filter(o=>o[0]!=='off'),g.trigger,'data-str')}</label>
 <label data-taps>${t('ru.taps')}${num('#taps',g.taps||3,'min="2" max="10"')}</label><label data-taps>${t('ru.window')}${num('#windowMs',g.windowMs||600,'min="50" max="2000" step="50"')}</label>
 <label class="ck"><input type="checkbox" class="cb" data-f="swallow"${g.swallow!==false?' checked':''}> ${t('ru.swallow')}</label></div>`:''}
 <div class="mg"><label class="wide">${isG?t('ru.onlyIf'):t('ru.when')}${txt('#when',x.when||'',isG?'':t('ru.whenPh'),'in code','text','maxlength="255" autocapitalize="off" spellcheck="false" list="fmNames"')}<small class="muted" data-chk></small></label>
 <label>${t('ru.pause')}${num('#pause',x.pause??(isG?0:5),`min="${isG?0:5}" max="65535"`)}</label></div>
 <div class="acts">${(x.do||[]).map((a,k)=>actHTML(a,k)).join('')}</div>
 <div class="mf"><span class="mw"></span><button class="btn sm" type="button" data-addact>${t('ru.addAct')}</button><button class="btn sm" type="button" data-test>${t('ru.try')}</button><button class="btn sm dng" type="button" data-del>${t('ru.del')}</button></div></div>`};
const ruVis=c=>{$$('.act',c).forEach(a=>{const fld=ruFld($('[data-f="effect"]',a).value);$$('[data-show]',a).forEach(l=>l.hidden=!l.dataset.show.split(' ').includes(fld||'-'))});
 const tg=$('[data-f="trigger"]',c);if(tg)$$('[data-taps]',c).forEach(l=>l.hidden=tg.value!=='sequence')};
const MAP_TABS={sig:'lvCard',fields:'cMap',formulas:'fmCard',rules:'ruCard'};
SCR.map={title:'map.title',hidden:true,async render(m){
 const[r,se,mp,hf,fm,ru]=await Promise.all([API.get('/api/map?meta=1&live=1'),API.get('/api/settings'),API.get('/api/map'),API.get('/api/hu/fields').catch(()=>null),API.get('/api/formulas').catch(()=>null),API.get('/api/rules').catch(()=>null)]);
 this.F=(fm&&fm.rows)||[];this.R=(ru&&ru.rows)||[];
 this.M=r.meta;this.n=0;this.grp='all';this.mp=mp;this.sw=null;this.last=null;this.setFl(hf);
 const tab=location.hash.split(':')[1]||'';this.tb=MAP_TABS[tab]?tab:'sig';
 /* фільтр за групою: значення — 'all' або назва групи, як у data-g рядка */
 const groups=[['all',t('ca.all')],...[...new Set(CAT.raise.map(x=>x.group))].map(g=>[g,g])];
 m.innerHTML=`<div class="pad full">${seg('#mTab',[['sig',t('map.tSig')],['fields',t('map.tFields')],['formulas',t('map.tFormulas')],['rules',t('map.tRules')]],this.tb)}</div>`+
 card(t('fm.card'),row('','',t('fm.h'))+
  `<div class="tw"><table class="fmt"><thead><tr><th>${t('fm.thName')}</th><th>${t('fm.thF')}</th><th>${t('fm.thUnit')}</th><th>${t('fm.thDec')}</th><th>${t('fm.thBool')}</th><th>${t('fm.thOn')}</th><th>${t('fm.thNow')}</th><th></th></tr></thead><tbody id="fmT"></tbody></table></div>`+
  `<div class="fr"><button class="btn" type="button" id="fmAdd">${t('fm.add')}</button><button class="btn pri" type="button" id="fmSave">${t('fm.save')}</button><button class="btn" type="button" id="fmFac">${t('fm.factory')}</button></div>`,'fmCard','custom full')+
 card(t('lv.signals'),row(t('lv.auto'),sw('#lvAuto',true,'data-nosave'),t('lv.autoSub'))+
  row(t('lv.only'),sw('#lvOnly',true,'data-nosave'))+
  `<div class="pad">${seg('#lvCls',LV_CLS.map(c=>[c,t('lv.cls.'+c)]),'all','wrap')}</div>`+row(t('lv.alerts'),`<span id="lvAl" class="muted">${DASH}</span>`,t('lv.alertsSub'))+
  `<div class="fr"><button class="btn" type="button" id="lvReset">${t('ca.factory')}</button></div><datalist id="idl"></datalist><div class="tw"><table><thead><tr><th>${t('lv.thSig')}</th><th>${t('lv.thNow')}</th><th>${t('lv.thAge')}</th><th>${t('lv.thSrc')}</th><th title="${esc(t('lv.checkedT'))}">✓</th><th></th></tr></thead><tbody id="lvT"></tbody></table></div>`,'lvCard','custom full')+
 card(t('ca.rav4'),
  row(t('ca.proto'),`<b>${esc(tHas('k.proto.'+CAT.proto)?t('k.proto.'+CAT.proto):CAT.proto||'')}</b>`,t('ca.protoSub'))+
  row(t('ca.send'),sw('/api/map|send',mp.send),t('ca.kept'),t('ca.sendH'))+
  `<div id="cProto">${protoSettings(se)}</div>`+
  `<div class="pad"><div class="seg wrap" id="mGrp" data-v="all">${groups.map(([v,n])=>`<button type="button" data-val="${esc(v)}" class="${v==='all'?'on':''}">${esc(n)}</button>`).join('')}</div></div>`+
  row(t('ca.sweep'),`<span class="fr"><select class="in" id="mSwT" aria-label="${esc(t('ca.step'))}">${[3,5,8].map(s=>`<option value="${s}"${s===5?' selected':''}>${t('ca.sec',s)}</option>`).join('')}</select><button class="btn" type="button" id="mSwGo">${t('ca.sweepGo')}</button><b class="mono" id="mSwV">—</b></span>`,'0x7D 06, D2 = 0…15',t('ca.sweepH'),'mSwRow')+
  `<div class="tw"><table class="mapt"><thead><tr><th>${t('ca.thField')}</th><th>${t('ca.thNow')}</th><th>${t('ca.thFrom')}</th><th>${t('ca.thTest')}</th></tr></thead><tbody id="map">${this.withRules(raiseRows(mp)).map(mapRow).join('')}</tbody></table></div>`,'cMap','custom full')+
 card(t('ru.card'),row('','',t('ru.h'))+`<div id="ruT"></div>`+
  `<div class="fr"><button class="btn" type="button" id="ruAddS">${t('ru.addSig')}</button><button class="btn" type="button" id="ruAddG">${t('ru.addGesture')}</button><button class="btn pri" type="button" id="ruSave">${t('ru.save')}</button><button class="btn" type="button" id="ruFac">${t('ru.factory')}</button></div>`,'ruCard','custom full');
 this.go(this.tb);this.draw(r);this.drawFormulas();this.drawRules();BIND.wire($('#lvT'));
 // Формули
 $('#fmAdd').onclick=()=>{this.F=this.readFormulas();this.F.push({name:'',f:'',unit:'',dec:0,bool:false,on:true,v:null});this.drawFormulas();const n=$$('#fmT [data-f="name"]').pop();if(n)n.focus()};
 $('#fmT').addEventListener('click',e=>{const d=e.target.closest('[data-del]');if(!d)return;this.F=this.readFormulas();this.F.splice(+d.dataset.del,1);this.drawFormulas()});
 /* жива перевірка формули: /api/expr рахує її на даних авто зараз; нове ім'я з цієї таблиці відоме після «Зберегти» */
 $('#fmT').addEventListener('input',e=>{const f=e.target.closest('[data-f="f"]');if(!f)return;clearTimeout(f._t);f._t=setTimeout(()=>this.checkFormula(f),400)});
 $('#fmSave').onclick=e=>run(e.target,async()=>{await API.post('/api/formulas',{rows:this.readFormulas().map(({v,...x})=>x)});await this.reloadFormulas()},t('fm.saved'));
 $('#fmFac').onclick=e=>ask(e.target,t('fm.factoryAsk'),()=>run(e.target,async()=>{await API.post('/api/formulas',{reset:true});await this.reloadFormulas()},t('fm.factoryDone')));
 $('[data-f="mTab"]').addEventListener('change',e=>this.go(e.currentTarget.dataset.v));
 // Сигнали
 /* заводські прив'язки сигналів (DecodeTable::factory): свої CAN-рядки й константи зникають */
 $('#lvReset').onclick=e=>ask(e.target,t('ca.resetAsk'),()=>run(e.target,()=>API.post('/api/map',{reset:true}),t('ca.factoryDone')).then(async()=>this.draw(await API.get('/api/map?live=1'))));
 $('[data-f="lvOnly"]').onchange=()=>this.draw(this.last);
 $('[data-f="lvCls"]').addEventListener('change',()=>this.draw(this.last));
 $('#lvT').onclick=e=>{const c=e.target.closest('[data-conf]');
  if(c){const mm=this.M[+c.dataset.conf],on=c.dataset.on==='1';
   ask(c,lvName(mm)+'. '+(on?t('lv.confOn'):t('lv.confOff')),()=>run(c,()=>API.post('/api/map',{confirm:{sig:mm[0],on}}),on?t('lv.confirmed'):t('lv.hypothesis')).then(async()=>this.draw(await API.get('/api/map?live=1'))));return}
  /* «→ поле ГУ»: те поле на вкладці «Поля ГУ» */
  const fk=e.target.closest('[data-fld]');if(fk){e.preventDefault();this.goField(fk.dataset.fld);return}
  const bb=e.target.closest('button');const be=bb&&bb.closest('tr.bindEd');
  if(be){BIND.click(bb,be,async()=>this.draw(await API.get('/api/map?live=1')));return}
  const bd=e.target.closest('[data-bind]');if(bd){this.bind(+bd.dataset.bind,this.M[+bd.dataset.bind][0]);return}
  /* 🔔: правило на цей сигнал — так/ні на появу, число на перевищення теперішнього значення */
  const b=e.target.closest('[data-add]');if(!b)return;const mm=this.M[+b.dataset.add],cur=(((this.last||{}).sigs||[])[+b.dataset.add]||[])[0];
  this.R=this.readRules();this.R.push({name:mm[0],on:true,when:mm[3]?`rises(${mm[0]})`:`${mm[0]} > ${cur==null?0:cur}`,pause:60,do:[{effect:'popup',arg:''}]});
  this.drawRules();this.go('rules');hlRow($$('#ruT .rule').pop())};
 // Поля ГУ
 saver($('#cMap .row'),()=>API.post('/api/map',{send:$('[data-k="/api/map|send"]').checked}),'[data-k]');
 saver($('#cProto'),genericSave,'[data-k]');
 $('#mSwGo').onclick=async e=>{const b=e.target,v=$('#mSwV'),live=tok=>this.sw===tok&&document.contains(b);if(this.sw){this.sw=null;b.textContent=t('ca.sweepGo');return}
  const pr=(this.fl||{}).prnd,on=pr&&((pr.rule||pr.def||{}).from!=='off');if(!on){sv(svOf(b),'bad',t('ca.needSel'));return}
  const tok=this.sw={};b.textContent=t('ca.sweepStop');
  for(let n=0;n<16&&live(tok);n++){v.textContent='D2 = '+n;
   try{await API.post('/api/map/test',{id:'prnd',value:n})}catch(err){sv(svOf(b),'bad',errT(err));break}
   for(let w=+$('#mSwT').value*10;w>0&&live(tok);w--)await sleep(100)}
  if(this.sw===tok){this.sw=null;b.textContent=t('ca.sweepGo');sv(svOf(b),'ok',t('ca.sweepDone'))}};
 $('#mGrp').addEventListener('change',e=>this.group(e.currentTarget.dataset.v));
 const mapT=$('#map');
 mapT.addEventListener('click',async e=>{
  /* «змінити в таблиці сигналів» / назва іншого сигналу: його прив'язка на вкладці «Сигнали» */
  const a=e.target.closest('[data-goto]');if(a){e.preventDefault();if(a.dataset.goto)this.goSig(a.dataset.goto);return}
  const b=e.target.closest('button');if(!b)return;
  const tr=b.closest('tr');
  if(b.dataset.send!==undefined){const id=tr.dataset.id,v=$('[data-t]',tr).value;if(v===''){sv(svOf(b),'bad',t('ca.enterVal'));return}run(b,()=>API.post('/api/map/test',{id,value:+v})).then(()=>this.map());return}
  if(b.dataset.ed!==undefined){const id=tr.dataset.id,open=$(`#map tr[data-for="${id}"]`);if(open){open.remove();return}const r=this.withRules(raiseRows(this.mp)).find(x=>x.id===id);tr.insertAdjacentHTML('afterend',mapEd(r,this.M));this.edVis($(`#map tr[data-for="${id}"]`));return}
  const ed=b.closest('tr.mapEd');if(!ed)return;const id=ed.dataset.for;
  if(b.dataset.close!==undefined){ed.remove();return}
  if(b.dataset.save!==undefined){const g=f=>val($(`[data-f="${f}"]`,ed));const from=g('from')||'auto';
   const rule=from==='auto'?{id,from}:{id,from,src:g('fsrc'),mul:+g('fmul')||1,div:+g('fdiv')||1,add:from==='k'?(+g('fk')||0):(+g('fadd')||0)};
   run(b,()=>API.post('/api/hu/fields',{rows:[rule]}),t('ca.saved')).then(r=>{if(r){ed.remove();this.map()}})}});
 mapT.addEventListener('change',e=>{const ed=e.target.closest('tr.mapEd');if(!ed)return;if(e.target.dataset.f==='from')this.edVis(ed);
  /* обрано інший сигнал: посилання веде до нього */
  if(e.target.dataset.f==='fsrc'){const l=$('[data-gosrc]',ed),s=val(e.target);l.dataset.goto=s;l.textContent=lvName(this.M.find(x=>x[0]===s)||[s])}});
 // Правила
 const ruT=$('#ruT');
 ruT.addEventListener('change',e=>{const c=e.target.closest('.rule');if(c&&/effect|trigger/.test(e.target.dataset.f||''))ruVis(c)});
 /* жива перевірка формули, як у формул: /api/expr на даних авто зараз */
 ruT.addEventListener('input',e=>{const f=e.target.closest('[data-f="when"]');if(!f)return;clearTimeout(f._t);f._t=setTimeout(()=>this.checkFormula(f),400)});
 ruT.addEventListener('click',e=>{const b=e.target.closest('button');if(!b||b.dataset.key!==undefined)return;const c=b.closest('.rule');if(!c)return;const i=+c.dataset.r;
  if(b.dataset.addact!==undefined){this.R=this.readRules();if(this.R[i].do.length>=3)return;this.R[i].do.push({effect:'popup',arg:''});this.drawRules();return}
  if(b.dataset.delact!==undefined){this.R=this.readRules();this.R[i].do.splice(+b.closest('.act').dataset.a,1);this.drawRules();return}
  if(b.dataset.test!==undefined){run(b,()=>API.post('/api/rules/test?id='+i),t('ru.queued'));return}
  if(b.dataset.del!==undefined)ask(b,t('ru.delAsk',$('[data-f="name"]',c).value),()=>{this.R=this.readRules();this.R.splice(i,1);this.drawRules()})});
 $('#ruAddS').onclick=()=>{this.R=this.readRules();this.R.push({name:t('ru.newName'),on:true,when:'',pause:5,do:[{effect:'popup',arg:''}]});this.drawRules();const n=$$('#ruT [data-f="when"]').pop();if(n)n.focus()};
 $('#ruAddG').onclick=()=>{this.R=this.readRules();this.R.push({name:t('ru.newName'),on:true,gesture:{trigger:'combo',keys:[],taps:3,windowMs:600,swallow:true},when:'',pause:0,do:[{effect:'key',codeHex:'07'}]});this.drawRules();hlRow($$('#ruT .rule').pop())};
 /* помилка «<назва>: <код>»: картка з цією назвою позначається, слова — e.<код> */
 const ruSave=async()=>{$$('#ruT .rule').forEach(c=>{c.classList.remove('bad');$('.mw',c).textContent=''});
  try{await API.post('/api/rules',{rows:this.readRules().map(({live,...x})=>x)});await this.reloadRules()}
  catch(e){const mm=/^(.*?): (.*)$/.exec(e.message||'');const c=mm&&$$('#ruT .rule').find(x=>$('[data-f="name"]',x).value===mm[1]);if(c){c.classList.add('bad');$('.mw',c).textContent=errLine(mm[2])}throw e}};
 $('#ruSave').onclick=e=>run(e.target,ruSave,t('ru.saved'));
 $('#ruFac').onclick=e=>ask(e.target,t('ru.factoryAsk'),()=>run(e.target,async()=>{await API.post('/api/rules',{reset:true});await this.reloadRules()},t('ru.factoryDone')));
},
/* вкладка: панелі показати/сховати, хеш без hashchange; на «Сигнали» — свіжі позначки «→ поле ГУ» */
go(tb){this.tb=tb;put($('[data-f="mTab"]'),tb);for(const[k,id]of Object.entries(MAP_TABS))$('#'+id).hidden=k!==tb;
 history.replaceState(null,'',tb==='sig'?'#map':'#map:'+tb);if(tb==='sig'&&this.last)this.draw(this.last)},
/* поле ГУ: вкладка «Поля ГУ»; фільтр групи знято, якщо ховає рядок */
goField(id){this.go('fields');const tr=$(`#map tr[data-id="${id}"]`);if(!tr)return;if(tr.hidden){put($('#mGrp'),'all');this.group('all')}hlRow(tr)},
/* сигнал: вкладка «Сигнали», «Лише з даними» вимкнено, його прив'язка відкрита */
async goSig(id){const i=this.M.findIndex(x=>x[0]===id);if(i<0)return;this.go('sig');$('[data-f="lvOnly"]').checked=false;
 $$('#lvT tr.bindEd,#lvT .ask').forEach(x=>x.remove());this.draw(this.last);await this.bind(i,id);const ed=$('#lvT tr.bindEd');if(ed)ed.scrollIntoView({block:'center'})},
group(g){this.grp=g;$$('#map tr[data-id]').forEach(tr=>{const hide=g!=='all'&&tr.dataset.g!==g;tr.hidden=hide;const ed=$(`#map tr[data-for="${tr.dataset.id}"]`);if(ed)ed.hidden=hide})},
/* /api/hu/fields: правила полів (this.fl за id) і поля на кожен сигнал (this.fs: сигнал → [поле]) */
setFl(hf){const rows=(hf&&hf.rows)||[];this.fl=Object.fromEntries(rows.map(x=>[x.id,x]));this.fs={};
 for(const f of rows){const s=fieldSig(f);if(s)(this.fs[s]=this.fs[s]||[]).push(f.id)}},
draw(r){if(r)this.last=r;const tb=$('#lvT');if(!tb||!r||!r.sigs||$('.ask',tb)||$('.bindEd',tb))return;const only=$('[data-f="lvOnly"]').checked;let h='',g=-1;
 const cs=$('[data-f="lvCls"]'),cl=cs?cs.dataset.v:'all',n={};
 /* вільний слот свого сигналу (ім'я "") не існує: його не видно й не лічать */
 const live=this.M.map((m,i)=>i).filter(i=>this.M[i][0]);
 for(const i of live){const x=r.sigs[i],c=lvCls(x[2],x[3],this.M[i]);n[c]=(n[c]||0)+1}
 if(cs)$$('button',cs).forEach(b=>{const c=b.dataset.val;b.textContent=t('lv.cls.'+c)+' · '+(c==='all'?live.length:(n[c]||0))});
 const idx=live.filter(i=>(!only||r.sigs[i][0]!=null)&&(cl==='all'||lvCls(r.sigs[i][2],r.sigs[i][3],this.M[i])===cl)).sort((a,b)=>this.M[a][4]-this.M[b][4]||a-b);
 for(const i of idx){const m=this.M[i],[v,age,src,ok,cf]=r.sigs[i],fs=this.fs[m[0]];
  const vh=(ok?'✓':`<span class="muted" title="${esc(src==='d'?t('lv.computedT'):t('lv.uncheckedT'))}">?</span>`)+
   (cf===1?` <button class="btn sm" type="button" data-conf="${i}" data-on="1" title="${esc(t('lv.confirmT'))}">${t('lv.confirm')}</button>`:
    cf===2?` <button class="btn sm" type="button" data-conf="${i}" data-on="0" title="${esc(t('lv.undoT'))}">${t('lv.undo')}</button>`:'');
  if(m[4]!==g){g=m[4];h+=`<tr><th colspan="6">${esc(tHas('lv.grp.'+g)?t('lv.grp.'+g):'')}</th></tr>`}
  const stale=src==='c'&&age!=null&&age>3000,sk=LV_SRC[src]?src:'o',hyp=src==='c'&&!ok;
  h+=`<tr${stale?' class="muted"':''}><td>${esc(lvName(m))} <small class="mono muted">${esc(m[0])}</small>${fs?`<small class="muted fmk">${t('map.toFld')} ${fs.map(f=>`<a href="#" data-fld="${f}">${esc(nameOf(CAT.raise,f))}</a>`).join(', ')}</small>`:''}</td><td class="num">${esc(lvVal(m,v))}</td><td class="num">${lvAge(age)}</td><td><button class="srcb" type="button" data-bind="${i}" title="${esc(t('lv.bindT'))}">${chipH(hyp?'warn':LV_SRC[sk],hyp?t('lv.cls.canHyp'):t('lv.src.'+sk))}</button></td><td style="white-space:nowrap">${vh}</td><td><button class="btn sm" type="button" data-add="${i}" title="${esc(t('lv.alertT'))}">🔔</button></td></tr>`}
 tb.innerHTML=h||`<tr><td colspan="6" class="muted">${esc(t('lv.noValues'))}</td></tr>`;
 if(r.ruleStats)$('#lvAl').textContent=r.ruleStats.fired+' / '+r.ruleStats.shown+' / '+r.ruleStats.dropped},
/* правила: картки з this.R (рядки /api/rules); поля читаються назад так, як прошивка їх приймає */
drawRules(){const tb=$('#ruT');if(!tb)return;tb.innerHTML=this.R.length?this.R.map(ruleHTML).join(''):`<div class="row muted">${esc(t('ru.none'))}</div>`;$$('.rule',tb).forEach(ruVis)},
readRules(){return $$('#ruT .rule').map(c=>{const g=f=>val($(`[data-f="${f}"]`,c)),isG=c.dataset.kind==='g',was=this.R[+c.dataset.r]||{};
 const acts=$$('.act',c).map(a=>{const h=f=>val($(`[data-f="${f}"]`,a)),fx=h('effect'),fld=ruFld(fx),o={effect:fx};
  if(fld==='action')o.action=h('action');if(fld==='code'||fld==='raw')o.codeHex=(h('codeHex')||'').trim();if(fld==='raw')o.data=(h('data')||'').trim();
  if(fld==='text')o.arg=h('arg')||'';if(fld==='scene')o.arg=h('argScene')||'';return o});
 const r={name:(g('name')||'').trim(),on:!!g('on'),when:(g('when')||'').trim(),pause:+g('pause')||0,do:acts,live:was.live};
 if(isG)r.gesture={trigger:g('trigger'),keys:g('keys')||[],taps:+g('taps')||3,windowMs:+g('windowMs')||600,swallow:!!g('swallow')};
 return r})},
async reloadRules(){const ru=await API.get('/api/rules');this.R=ru.rows||[];this.drawRules()},
/* лише живі чипи: правки в полях не зачіпаються */
async liveRules(){const ru=await API.get('/api/rules');(ru.rows||[]).forEach((x,i)=>{const c=$(`#ruT .rule[data-r="${i}"] [data-live]`);if(c)c.textContent=ruLive(x.live)})},
/* формули: рядок = [ім'я, формула, одиниця, знаки, так/ні, увімк., значення зараз] */
drawFormulas(){const tb=$('#fmT');if(!tb)return;
 tb.innerHTML=this.F.length?this.F.map((x,i)=>`<tr data-r="${i}"><td>${txt('#name',x.name,'my_signal','in code','text','maxlength="23" autocapitalize="off" spellcheck="false"')}</td>`+
  `<td>${txt('#f',x.f,'fuel_level * 100 / 60','in code','text','maxlength="255" autocapitalize="off" spellcheck="false" list="fmNames"')}<small class="muted" data-chk></small></td>`+
  `<td>${txt('#unit',x.unit,'','in','text','maxlength="11" style="width:5em"')}</td><td>${sel('#dec',[0,1,2,3],x.dec||0)}</td><td>${sw('#bool',!!x.bool)}</td><td>${sw('#on',x.on!==false)}</td>`+
  `<td class="num">${x.v==null?DASH:esc(String(x.v))}</td><td><button class="btn sm" type="button" data-del="${i}" title="${esc(t('fm.del'))}">✕</button></td></tr>`).join('')
  :`<tr><td colspan="8" class="muted">${esc(t('fm.none'))}</td></tr>`;
 if(!$('#fmNames'))tb.insertAdjacentHTML('afterend',`<datalist id="fmNames">${this.M.filter(m=>m[0]).map(m=>`<option value="${esc(m[0])}">`).join('')}</datalist>`)},
readFormulas(){return $$('#fmT tr[data-r]').map(tr=>{const g=f=>val($(`[data-f="${f}"]`,tr));const was=this.F[+tr.dataset.r]||{};
 return {name:(g('name')||'').trim(),f:(g('f')||'').trim(),unit:(g('unit')||'').trim(),dec:+g('dec')||0,bool:!!g('bool'),on:!!g('on'),v:was.v??null}})},
async reloadFormulas(){const fm=await API.get('/api/formulas');this.F=fm.rows||[];this.drawFormulas()},
async checkFormula(el){const out=$('[data-chk]',el.closest('td'));if(!out)return;const f=el.value.trim();if(!f){out.textContent='';return}
 try{const r=await API.post('/api/expr',{f});if(el.value.trim()!==f)return;
  out.textContent=r.ok?'= '+(r.v==null?t('fm.noValue'):r.v):'✗ '+t('e.'+r.code)+' · '+t('fm.at',r.pos+1)}
 catch(e){out.textContent='✗ '+errT(e)}},
/* прив'язка одного сигналу: рядок під ним, з GET /api/map?sig= */
async bind(i,id){const open=$(`#lvT tr.bindEd[data-for="${id}"]`);if(open){open.remove();return}
 const r=((await API.get('/api/map?sig='+encodeURIComponent(id))).rows||[])[0];
 /* після запиту: таблиця могла перемалюватися (тік кожні 2 с) — рядок шукаємо знову */
 const b=$(`#lvT [data-bind="${i}"]`),tr=b&&b.closest('tr');if(!r||!tr)return;
 $$('#lvT tr.bindEd').forEach(x=>x.remove());delete BIND.learn[id];
 tr.insertAdjacentHTML('afterend',bindEd(r,6));BIND.vis($(`#lvT tr.bindEd[data-for="${id}"]`));if(has('vehicle'))BIND.ids()},
edVis(ed){const fr=val($('[data-f="from"]',ed))||'auto';
 const own=fr==='own'||fr==='auto'&&!$(`#map tr[data-id="${ed.dataset.for}"]`).dataset.def;
 $$('[data-fr]',ed).forEach(l=>l.hidden=l.dataset.fr==='auto'?!own:l.dataset.fr!==fr)},
/* правила полів ГУ з /api/hu/fields на рядки мапи: r.rule, r.sent (те, що йде в ГУ) */
withRules(rows){const fl=this.fl||{};return rows.map(r=>{const f=fl[r.id];return f?{...r,rule:f.rule,def:f.def,sent:f.v}:r})},
/* поля ГУ: значення, «Звідки» і правила (this.fl, позначки «→ поле ГУ») */
async map(){const[mp,hf]=await Promise.all([API.get('/api/map'),API.get('/api/hu/fields').catch(()=>null)]);if(!$('#map'))return;this.mp=mp;
 this.setFl(hf);
 this.withRules(raiseRows(mp)).forEach(r=>{const tr=$(`#map tr[data-id="${r.id}"]`);if(!tr)return;$('[data-v]',tr).textContent=vCell(r);tr.classList.toggle('testing',r.how==='test');
  const sb=tr.children[2];const h=srcBtn(r);if(sb.innerHTML!==h)sb.innerHTML=h;if(r.def)tr.dataset.def='1';else delete tr.dataset.def});
 put($('[data-k="/api/map|send"]'),mp.send)},
async tick(){if(this.n++%2)return;
 if(this.tb==='sig'){const a=$('[data-f="lvAuto"]');if(a&&a.checked)this.draw(await API.get('/api/map?live=1'))}
 else if(this.tb==='fields')await this.map()
 else if(this.tb==='rules')await this.liveRules()}};
