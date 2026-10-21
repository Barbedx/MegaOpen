/* ---------- Авто ---------- */
/* швидкі мітки запису: текст сторінки, не довідник прошивки */
const QUICK=['двері відч.','двері зачин.','фари','габарити','поворот Л','поворот П','задній хід','гальмо','кондиціонер','паливо'];
const fileRow=f=>`<div class="row" data-file="${f.f}"><div class="l"><b class="mono">${f.f}</b>${f.current?' '+chipH('rec','пишеться'):''}<small>сесія ${f.session}·${f.part} · ${fmtB(f.size)} · ${esc(fmtWall(f.wall))}${f.protected?' · не видалиться сам':''}</small></div><div class="c"><button class="btn sm" type="button" data-dl>Скачати</button><button class="btn sm${f.protected?' pri':''}" type="button" data-pr title="${f.protected?'Зараз: не видаляється при нестачі місця. Натисни, щоб дозволити.':'Не видаляти при нестачі місця'}">${f.protected?'🔒':'🔓'}</button><button class="btn sm dng" type="button" data-rm${f.current?' disabled':''}>✕</button></div></div>`;
/* поля Raise — з каталогу (CAT.raise); /api/map дає прив'язку і значення за id */
const raiseRows=mp=>CAT.raise.map(t=>({...t,...(mp.rows.find(r=>r.id===t.id)||{})}));
/* рядок /api/can/ids: id — число, data — суцільний hex, changed — маска байтів */
const idRow=x=>{const bytes=(x.data.match(/../g)||[]);return {id:'0x'+Number(x.id).toString(16).toUpperCase().padStart(3,'0'),count:x.count,bytes,ch:bytes.map((b,i)=>x.changed>>i&1)}};
/* обмін з ГУ: напрямок, відомі команди від ГУ, поле тексту 0x99 */
const HU_DIR={R:'← ГУ',J:'← сміття',T:'→ кнопка',V:'→ авто'};
const HU_CMD={'99':'медіа-текст','9D':'годинник','90':'запит','C7':'команда UI'};
const HU_FLD={2:'назва',3:'виконавець',4:'альбом'};
const huRow=x=>`<tr class="${x.dir==='R'?'':'muted'}"><td class="num">${(x.ageMs/1000).toFixed(1)}</td><td>${HU_DIR[x.dir]||x.dir}</td><td class="mono">${x.cmd}${HU_CMD[x.cmd]?' · '+HU_CMD[x.cmd]:''}</td><td>${x.text!==undefined?'<b>'+esc(HU_FLD[x.field]||'поле '+x.field)+':</b> '+esc(x.text):`<span class="mono">${esc((x.data.match(/../g)||[]).join(' '))}</span>`}</td></tr>`;
const fmtV=r=>r.v==null?DASH:r.kind==='b'?(r.v?'так':'ні'):(+r.v).toFixed(r.dec)+(r.unit&&!/–/.test(r.unit)?' '+r.unit:'');
const srcBtn=r=>!r.fw?'<span class="srcb no" title="Android-апка це читає, прошивка ще не шле">нема в прошивці</span>':r.src==='can'?`<button class="srcb can" type="button" data-ed>${r.can.id} · байт ${r.can.byte}${r.can.len>1?'–'+(r.can.byte+r.can.len-1):''}</button>`:r.src==='const'?`<button class="srcb k" type="button" data-ed>стала ${r.k}</button>`:'<button class="srcb" type="button" data-ed>+ прив’язати</button>';
const vCell=r=>fmtV(r)+(r.how==='test'?' · тест '+r.left+' с':'');
const mapRow=r=>`<tr data-id="${r.id}" data-g="${esc(r.group)}" class="${r.fw?'':'nofw'}${r.how==='test'?' testing':''}"><td>${esc(r.name)}<small>${r.cmd}</small></td><td class="num" data-v>${esc(vCell(r))}</td><td>${srcBtn(r)}</td>
 <td><div class="tcell">${r.kind==='b'?`<select class="in" data-t aria-label="Тестове значення"><option value="1">так</option><option value="0">ні</option></select>`:`<input class="in" type="number" data-t step="${r.dec?Math.pow(10,-r.dec):1}" min="${r.min}" max="${r.max}" placeholder="${r.min}…${r.max}" aria-label="Тестове значення">`}<button class="btn sm" type="button" data-send${r.fw?'':' disabled'} title="Надіслати в ГУ зараз і тримати 30 с">▶</button></div></td></tr>`;
const mapEd=r=>{const c=r.can||{id:'',byte:0,len:1,shift:0,mask:'FF',mul:1,div:1,add:0,le:false,signed:false};return `<tr class="mapEd" data-for="${r.id}"><td colspan="4"><div class="mg">
 <label>Джерело${sel('#src',enumOpts('mapSrc'),r.src,'data-str')}</label>
 <label data-s="can">ID<input class="in" data-f="id" list="idl" value="${esc(c.id)}" placeholder="0x354" autocomplete="off"></label>
 <label data-s="can">Байт${num('#byte',c.byte,'min="0" max="7"')}</label><label data-s="can">Довжина, байт${num('#len',c.len,'min="1" max="4"')}</label>
 <label data-s="can">Зсув, біт${num('#shift',c.shift,'min="0" max="31"')}</label><label data-s="can">Маска, hex${txt('#mask',c.mask,'FF','in')}</label>
 <label data-s="can">×${num('#mul',c.mul,'step="any"')}</label><label data-s="can">÷${num('#div',c.div,'step="any"')}</label><label data-s="can">+${num('#add',c.add,'step="any"')}</label>
 <label data-s="can" class="ck"><input type="checkbox" class="cb" data-f="le"${c.le?' checked':''}> молодший байт перший</label><label data-s="can" class="ck"><input type="checkbox" class="cb" data-f="signed"${c.signed?' checked':''}> зі знаком</label>
 <label data-s="const">Значення${num('#k',r.k,'step="any"')}</label></div>
 <div class="mf" style="margin-top:8px"><span class="mw" data-raw style="color:var(--mute)"></span>${has('vehicle')?'<button class="btn sm" type="button" data-learn>Що змінилось?</button>':''}<button class="btn sm pri" type="button" data-save>Зберегти</button><button class="btn sm" type="button" data-close>✕</button></div></td></tr>`};
SCR.car={title:'Авто',async render(m){
 const C2=has('can2'),CP=has('capture'),V=has('vehicle');
 const[c2,cp,mp,cb]=await Promise.all([getIf(C2,'/api/can2'),getIf(CP,'/api/cap'),API.get('/api/map'),getIf(V,'/api/can')]);
 this.n=0;this.filesSig='';this.grp='усі';this.learn={};this.mp=mp;
 const groups=['усі',...new Set(CAT.raise.map(t=>t.group))];
 m.innerHTML=
 card('Наживо',row('Усі сигнали машини','<a class="btn" href="#live">Відкрити</a>','значення, вік, чи перевірено; сповіщення на дисплей'))+
 (C2?card('CAN A',row('Шина','<span id="cSt" class="chip off">…</span>','<span id="cStW"></span>')+
  '<div class="kv"><div><small>Кадр/с</small><b id="cFps"></b></div><div><small>Усього</small><b id="cFr"></b></div><div><small>rec / tec</small><b id="cErr"></b></div><div><small>Переповнень</small><b id="cOv"></b></div></div>'+
  row('Швидкість',sel('/api/can2|bitrate',enumOpts('canRates'),c2.setting||0),'','Авто: спершу остання вдала, далі 500 → 250 → 125 → 1000k. Знайшла — тримає до перезапуску. Плата лише слухає.')):notBuilt('CAN A','CAN A'))+
 (CP?card('Запис',`<div class="fr"><button class="btn rec big" id="cRec" type="button" style="flex:1">…</button></div>`+
  `<div class="fr"><input class="in" id="mkT" placeholder="Що зараз роблю" maxlength="40" data-nosave><button class="btn pri" type="button" id="mkGo">Мітка</button></div>`+
  `<div class="pad"><div class="seg wrap" id="quick">${QUICK.map(q=>`<button type="button" data-q="${esc(q)}">${esc(q)}</button>`).join('')}</div></div>`+
  '<div class="kv"><div><small>Записано</small><b id="pFr"></b></div><div><small>Втрачено</small><b id="pDr"></b></div><div><small>Міток</small><b id="pMk"></b></div><div><small>ID</small><b id="pId"></b></div><div><small>Файл</small><b id="pFile"></b></div><div><small>Сховище</small><b id="pSt"></b></div></div>'+
  '<div class="pad"><div class="bar big"><i id="pBar"></i></div></div>'+
  row('Автостарт',sw('/api/settings|cap.autostart',cp.settings.autostart),'','Запис стартує, щойно після запуску з’являються кадри.')+
  row('Що писати',seg('/api/settings|cap.mode',enumOpts('capModes'),cp.settings.mode)))+
 card('Файли',row('','','Коли місце закінчується, найстаріші файли видаляються самі. 🔒 — не видаляти. Мітка ставить 🔒 на свою сесію сама.')+`<div id="files"></div><div class="fr"><button class="btn dng" type="button" id="fUnp">Видалити всі без 🔒</button></div>`,'cFiles','custom'):C2?notBuilt('Запис','запису'):'')+
 card('RAV4 → ГУ',
  row('Слати дані авто в ГУ',sw('/api/map|send',mp.send),'запам’ятовується','Ліворуч — що вміє протокол RAV4 (0x2E …). ▶ шле тестове значення в ГУ одразу і тримає 30 с: так видно, що й де покаже Android. Потім «прив’язати» поле до ID машини. Нема в прошивці — апка це читає, але плата ще не шле.')+
  row('Одиниці: пальне · пробіг',sel('#fuel',enumOpts('fuelUnits'),mp.units.fuel,'aria-label="Пальне"')+sel('#trip',enumOpts('tripUnits'),mp.units.trip,'aria-label="Пробіг"'),'','','mUnits')+
  row('Експерименти ГУ','<span class="fr" id="mExp">'+[[1,'t° ОР'],[2,'пальне в 7D04'],[4,'розрах. витрата'],[8,'селектор 7D06']].map(([b,t])=>'<label class="ck"><input type="checkbox" class="cb" data-b="'+b+'"'+((mp.exp|0)&b?' checked':'')+'> '+t+'</label>').join(' ')+'</span>','за замовчуванням вимкнені','Поля, які магнітола розбирає, але жоден запис їх не показав. t° ОР — 0x7D 09 з температурою охолодної рідини; пальне — літри ×10 у 0x7D 04 D4-D5 замість F2 08 (і одометр щитка, якщо 0x5FD мовчить); розрах. витрата — запас ходу й витрата з лічильника 0x551 у 0x21/0x22/0x23; селектор — 0x7D 06, лише зі значенням з прогону нижче. Вмикай по одному і дивись, що зміниться на планшеті.','mExpRow')+
  row('Напруга АКБ у полі ГУ','<span class="fr" id="mBatt"><label class="ck"><input type="checkbox" class="cb" id="mBattOn"'+((mp.exp|0)&16?' checked':'')+'> увімк.</label><select class="in" id="mBattAt" aria-label="Поле ГУ">'+[[0,'0x21 запас ходу'],[1,'0x7D 04 (÷10)'],[2,'0x23 витрата']].map(([v,t])=>'<option value="'+v+'"'+((((mp.exp|0)>>5)&3)===v?' selected':'')+'>'+t+'</option>').join('')+'</select></span>','за замовчуванням вимкнено','У профілі RAV4 немає поля для напруги, тож вона йде в чуже: вольти ×10 (12,6 В → 126). У 0x21 це «126 км»; 0x7D 04 і 0x23 ділять на 10, там «12.6». Поле зайняте розрах. витратою (0x21/0x23) або пальним у 7D04, тож їх разом не можна.')+
  row('Прогін селектора','<span class="fr"><select class="in" id="mSwT" aria-label="Крок"><option value="3">3 с</option><option value="5" selected>5 с</option><option value="8">8 с</option></select><button class="btn" type="button" id="mSwGo">Почати прогін</button><b class="mono" id="mSwV">—</b></span>','0x7D 06, D2 = 0…15','Шле в ГУ 0x7D 06 з D2 = 0, 1 … 15, одне значення на крок. Дивись на планшет: коли з’явиться P, R, N чи D, запиши число D2. Потрібен увімкнений «селектор 7D06» вище. Кожне значення тримається 30 с; після зупинки кадр зникає сам.','mSwRow')+
  `<div class="pad"><div class="seg wrap" id="mGrp" data-v="усі">${groups.map(g=>`<button type="button" data-val="${esc(g)}" class="${g==='усі'?'on':''}">${esc(g)}</button>`).join('')}</div></div>`+
  `<div class="fr"><button class="btn" type="button" id="mReset">Заводські прив’язки</button></div>`+
  `<div class="tw"><table class="mapt"><thead><tr><th>Поле</th><th>Зараз</th><th>Звідки</th><th>Тест</th></tr></thead><tbody id="map">${raiseRows(mp).map(mapRow).join('')}</tbody></table></div><datalist id="idl"></datalist>`,'cMap','custom full')+
 fold('Обмін з ГУ (UART)',row('Показувати',seg('#huIn',[['1','від ГУ'],['0','усе']],'1'),'','«від ГУ» — лише кадри магнітоли: текст треку, годинник, запити. «усе» — разом із нашими кадрами, які йдуть кожні 200 мс.')+'<div class="tw"><table><thead><tr><th>с тому</th><th>хто</th><th>команда</th><th>дані</th></tr></thead><tbody id="huLog"></tbody></table></div>','cHu')+
 (V?fold('ID на шині',row('Таблиця ID',sw('/api/can/sniff|on',!!cb.sniff),'','Рахує кадри за ID і підсвічує байти, що змінились від бази.')+'<div class="fr"><button class="btn" type="button" id="idBase">Зафіксувати базу</button><button class="btn" type="button" id="idClr">Очистити</button></div><div class="tw"><table><thead><tr><th>ID</th><th>к-сть</th><th>байти</th></tr></thead><tbody id="ids"></tbody></table></div>','cIds'):'');
 $$('.card:not(.custom)',m).forEach(c=>saver(c,genericSave,'[data-k]'));
 if(CP){$('#cRec').onclick=e=>run(e.target,()=>API.post(e.target.classList.contains('on')?'/api/cap/stop':'/api/cap/start')).then(()=>this.tick(true));
 const mk=(t,b)=>run(b,()=>API.post('/api/cap/mark?t='+encodeURIComponent(t)),'мітка «'+t+'»');
 $('#mkGo').onclick=e=>{const t=$('#mkT').value.trim()||'мітка';mk(t,e.target).then(()=>$('#mkT').value='')};
 $('#mkT').onkeydown=e=>{if(e.key==='Enter')$('#mkGo').click()};
 $('#quick').onclick=e=>{const b=e.target.closest('[data-q]');if(b)mk(b.dataset.q,b)};
 $('#files').onclick=e=>{const b=e.target.closest('button');if(!b)return;const r=b.closest('[data-file]');if(!r)return;const f=r.dataset.file;
  if(b.dataset.dl!==undefined)run(b,()=>download('/api/cap/get?f='+f,f+'.bin'));
  else if(b.dataset.pr!==undefined)run(b,()=>API.post('/api/cap/protect?f='+f+'&on='+(b.classList.contains('pri')?0:1))).then(()=>this.files(true));
  else if(b.dataset.rm!==undefined)ask(b,'Видалити '+f+'.bin'+(b.parentElement.querySelector('.pri')?' (з 🔒)':'')+'?',()=>run(b,()=>API.post('/api/cap/delete?f='+f),'видалено').then(()=>this.files(true)))};
 $('#fUnp').onclick=e=>ask(e.target,'Видалити всі файли без 🔒?',()=>run(e.target,()=>API.post('/api/cap/delete_unprotected').then(r=>({note:'видалено '+r.deleted}))).then(()=>this.files(true)))}
 if(V){$('#idBase').onclick=e=>run(e.target,()=>API.post('/api/can/baseline'),'база зафіксована');
 $('#idClr').onclick=e=>run(e.target,()=>API.post('/api/can/reset'),'очищено')}
 // RAV4
 saver($('#cMap .row'),()=>API.post('/api/map',{send:$('[data-k="/api/map|send"]').checked}),'[data-k]');
 saver($('#mUnits'),()=>API.post('/api/map',{units:{fuel:val($('[data-f="fuel"]')),trip:val($('[data-f="trip"]'))}}),'[data-f]');
$('#mSwGo').onclick=async e=>{const b=e.target,v=$('#mSwV'),live=t=>this.sw===t&&document.contains(b);if(this.sw){this.sw=null;b.textContent='Почати прогін';return}
  if(!$('#mExp [data-b="8"]').checked){sv(svOf(b),'bad','спершу увімкни «селектор 7D06»');return}
  const tok=this.sw={};b.textContent='Зупинити прогін';
  for(let n=0;n<16&&live(tok);n++){v.textContent='D2 = '+n;
   try{await API.post('/api/map/test',{id:'prnd',value:n})}catch(err){sv(svOf(b),'bad',errT(err));break}
   for(let w=+$('#mSwT').value*10;w>0&&live(tok);w--)await sleep(100)}
  if(this.sw===tok){this.sw=null;b.textContent='Почати прогін';sv(svOf(b),'ok','прогін закінчено')}};
/* експерименти ГУ: біти huExp; АКБ (16) з полем у бітах 5-6 займає поле іншого експерименту (Rav4Emitter::experimentsWhy) */
 const expSync=()=>{const on=$('#mBattOn').checked,at=+$('#mBattAt').value,rival=on?(at===1?2:4):0;
  $$('#mExp [data-b]').forEach(c=>{const b=+c.dataset.b,off=!!(rival&b);if(off)c.checked=false;c.disabled=off});$('#mBattAt').disabled=!on};
 expSync();
 const expSave=e=>{expSync();const m=$$('#mExp [data-b]').reduce((a,c)=>a|(c.checked?+c.dataset.b:0),0)|($('#mBattOn').checked?16|(+$('#mBattAt').value<<5):0);run(e.target,()=>API.post('/api/map',{exp:m}),'експерименти: '+m)};
 $('#mExp').onchange=expSave;$('#mBatt').onchange=expSave;
 $('#mReset').onclick=e=>ask(e.target,'Повернути заводські прив’язки? Твої зміни зникнуть.',()=>run(e.target,()=>API.post('/api/map',{reset:true}),'заводські').then(()=>this.map()));
 $('#mGrp').addEventListener('change',e=>{this.grp=e.currentTarget.dataset.v;$$('#map tr[data-id]').forEach(tr=>{const hide=this.grp!=='усі'&&tr.dataset.g!==this.grp;tr.hidden=hide;const ed=$(`#map tr[data-for="${tr.dataset.id}"]`);if(ed)ed.hidden=hide})});
 const mapT=$('#map');
 mapT.addEventListener('click',async e=>{const b=e.target.closest('button');if(!b)return;
  const tr=b.closest('tr');
  if(b.dataset.send!==undefined){const id=tr.dataset.id,v=$('[data-t]',tr).value;if(v===''){sv(svOf(b),'bad','введи значення');return}run(b,()=>API.post('/api/map/test',{id,value:+v})).then(()=>this.map());return}
  if(b.dataset.ed!==undefined){const id=tr.dataset.id,open=$(`#map tr[data-for="${id}"]`);if(open){open.remove();return}const r=raiseRows(this.mp).find(x=>x.id===id);tr.insertAdjacentHTML('afterend',mapEd(r));this.edVis($(`#map tr[data-for="${id}"]`));if(has('vehicle'))this.fillIds();return}
  const ed=b.closest('tr.mapEd');if(!ed)return;const id=ed.dataset.for;
  if(b.dataset.close!==undefined){ed.remove();return}
  if(b.dataset.save!==undefined){const g=f=>val($(`[data-f="${f}"]`,ed));const src=g('src');
   const row={id,src,k:g('k')||0,can:src==='can'?{id:(g('id')||'').trim(),byte:g('byte'),len:g('len'),shift:g('shift')||0,mask:(g('mask')||'').replace(/^0x/i,''),mul:g('mul'),div:g('div'),add:g('add')||0,le:g('le'),signed:g('signed')}:null};
   run(b,()=>API.post('/api/map',{rows:[row]}),'збережено').then(r=>{if(r){ed.remove();this.map(true)}});return}
  if(b.dataset.learn!==undefined){const cid=val($('[data-f="id"]',ed)).trim().toUpperCase().replace('0X','0x');
   if(!this.learn[id]){await run(b,()=>API.post('/api/can/baseline'),'база є');this.learn[id]=true;b.textContent='Показати зміни';$('[data-raw]',ed).textContent='Зроби дію в машині (відчини двері, ввімкни фари…), потім натисни ще раз.';return}
   const r=await API.get('/api/can/ids');const ch=r.ids.filter(x=>x.changed).map(idRow);const mine=ch.find(x=>x.id===cid);
   $('[data-raw]',ed).innerHTML=(mine?`${cid}: змінились байти ${mine.bytes.map((b,i)=>mine.ch[i]?i:null).filter(x=>x!==null).join(', ')}. `:cid?`${esc(cid)} не змінився. `:'')+'Змінились: '+(ch.map(x=>`<a href="#" data-pick="${x.id}">${x.id}</a>`).join(' ')||'нічого');
   this.learn[id]=false;b.textContent='Що змінилось?'}});
 mapT.addEventListener('click',e=>{const a=e.target.closest('[data-pick]');if(!a)return;e.preventDefault();const ed=a.closest('tr.mapEd');$('[data-f="id"]',ed).value=a.dataset.pick});
 mapT.addEventListener('change',e=>{const ed=e.target.closest('tr.mapEd');if(ed&&e.target.dataset.f==='src')this.edVis(ed)});
 if(CP)await this.files(true);await this.tick(true);
},edVis(ed){const s=val($('[data-f="src"]',ed));$$('[data-s]',ed).forEach(l=>l.hidden=l.dataset.s!==s)},
async fillIds(){const r=await API.get('/api/can/ids');const d=$('#idl');if(d)d.innerHTML=r.ids.map(idRow).map(x=>`<option value="${x.id}">${x.bytes.join(' ')}</option>`).join('')},
async map(){const mp=this.mp=await API.get('/api/map');if(!$('#map'))return;
 raiseRows(mp).forEach(r=>{const tr=$(`#map tr[data-id="${r.id}"]`);if(!tr)return;$('[data-v]',tr).textContent=vCell(r);tr.classList.toggle('testing',r.how==='test');
  const sb=tr.children[2];const h=srcBtn(r);if(sb.innerHTML!==h)sb.innerHTML=h;
  const ed=$(`#map tr[data-for="${r.id}"] [data-raw]`);if(ed&&!this.learn[r.id]&&!ed.querySelector('a'))ed.textContent=r.raw?r.raw+' → '+fmtV(r):''});
 put($('[data-k="/api/map|send"]'),mp.send)},
async files(force){const l=(await API.get('/api/cap/list')).files||[];const sig=JSON.stringify(l.map(f=>[f.f,f.protected,f.current,Math.round(f.size/65536)]));const el=$('#files');if(!el)return;
 if(!force&&(sig===this.filesSig||el.parentElement.querySelector('.ask')))return;this.filesSig=sig;el.innerHTML=l.length?l.slice().reverse().map(fileRow).join(''):'<div class="row muted">файлів немає</div>'},
async tick(force){this.n++;const[c2,cp]=await Promise.all([getIf(has('can2'),'/api/can2'),getIf(has('capture'),'/api/cap')]);if(!$('#cMap'))return;
 if(c2){canChip($('#cSt'),c2);
 $('#cStW').textContent=c2.autobaud!=='manual'?'автошвидкість':'вручну '+(c2.setting/1000)+'k'+(c2.fps?'':' · перевір швидкість');
 $('#cFps').textContent=fmtN(c2.fps);$('#cFr').textContent=fmtN(c2.frames);$('#cErr').textContent=c2.rec+' / '+c2.tec;$('#cOv').textContent=c2.hwOverflows;
 put($('[data-k="/api/can2|bitrate"]'),c2.setting||0)}
 if(cp){recBtn($('#cRec'),cp);
 $('#pFr').textContent=fmtN(cp.frames);$('#pDr').textContent=cp.dropped;$('#pMk').textContent=cp.markers;$('#pId').textContent=cp.ids;$('#pFile').textContent=cp.file?String(cp.file).padStart(4,'0'):DASH;
 $('#pSt').textContent=fmtB(cp.capBytes)+' / '+fmtB(cp.budget);const pb=$('#pBar');pb.style.width=(cp.capBytes/cp.budget*100)+'%';pb.style.background=cp.capBytes/cp.budget>cp.settings.fill/100*.95?'var(--warn)':'var(--acc)';
 put($('[data-k="/api/settings|cap.autostart"]'),cp.settings.autostart);put($('[data-k="/api/settings|cap.mode"]'),cp.settings.mode)}
 await this.map();if(cp&&(this.n%10===0||force))await this.files();
 const hl=$('#cHu');if(hl&&hl.open){const r=await API.get('/api/canbox/last?in='+$('#huIn').dataset.v);const tb=$('#huLog');if(tb)tb.innerHTML=(r.frames||[]).map(huRow).join('')||'<tr><td colspan="4" class="muted">порожньо</td></tr>'}
 const ids=$('#cIds');if(ids&&ids.open&&$('[data-k="/api/can/sniff|on"]').checked){const r=await API.get('/api/can/ids');const tb=$('#ids');if(tb)tb.innerHTML=r.ids.map(idRow).map(x=>`<tr><td class="mono">${x.id}</td><td class="num">${fmtN(x.count)}</td><td class="mono">${x.bytes.map((b,i)=>x.ch[i]?`<span class="ch">${b}</span>`:b).join(' ')}</td></tr>`).join('')||'<tr><td colspan="3" class="muted">порожньо</td></tr>'}
}};
