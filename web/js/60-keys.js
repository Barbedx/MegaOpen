/* ---------- Кнопки ---------- */
const destName=d=>nameOf(CAT.dests,d);
/* маска й живість кожного призначення: ланцюг keys з /api/health (WebUi::putDests);
   маски пишуться в реєстр налаштувань: uart -> routeUart, ams -> routeAms, ... */
const kRt=h=>({dest:((h.chains||[]).find(c=>c.id==='keys')||{}).dest||{}});
const routeId=d=>'route'+d[0].toUpperCase()+d.slice(1);
const actName=a=>nameOf(CAT.actions,a);
/* дії, що можуть бути клавішею макросу або його дією: усі, крім «нічого» */
const realActions=()=>CAT.actions.filter(a=>a.class!=null);
/* дія = зміст і клас маршруту; код лише підміняє байт для ГУ (порожньо = типовий код дії) */
const huOf=id=>(byId(CAT.actions,id)||{}).hu||'';
const actSel=(r)=>`<div class="ka" data-i="${r.index}">${sel('#action',CAT.actions.map(a=>[a.id,a.name]),r.action,'data-str aria-label="Дія"')}<input class="in code kc" data-f="code" data-hex value="${r.code?h2(r.code):''}" placeholder="${esc(huOf(r.action))}" maxlength="2" aria-label="Код для ГУ, порожньо — типовий" title="Код для ГУ; порожньо — типовий"><button class="btn sm" type="button" data-test title="Як натиснути на кермі: піде тими ж маршрутами">▶</button></div>`;
const kRow=(k,rows)=>{const c=rows.find(r=>r.key===k.id&&!r.hold),h=rows.find(r=>r.key===k.id&&r.hold);if(!c||!h)return '';
 return `<div class="krow" data-key="${esc(k.id)}"><div class="kh"><span class="lamp"></span><b>${esc(k.name)}</b><code class="muted">${esc(k.wire)}</code></div><div class="kgrid">${actSel(c)}${actSel(h)}</div></div>`};
const LBL={cw:'↻ за год.',ccw:'↺ проти',click:'натиск',hold:'утримання'};
/* клацання для пошуку поганого контакту: руки зайняті дротами, очі не на екрані */
let AC=null;const tick=f=>{try{AC=AC||new AudioContext();const o=AC.createOscillator(),v=AC.createGain();o.frequency.value=f;v.gain.setValueAtTime(.2,AC.currentTime);v.gain.exponentialRampToValueAtTime(.001,AC.currentTime+.06);o.connect(v).connect(AC.destination);o.start();o.stop(AC.currentTime+.06)}catch(e){}};
const knobCard=(e,z,name,pre,zi)=>card(name+' <span id="kl_'+pre+'" class="chip off" style="margin-left:6px">…</span>',
 row('Увімкнено',sw('@'+pre+'.enabled',e.enabled))+
 row('Тест',`<span class="ktest" id="kt_${pre}"><span class="kl" data-l="a">A</span><span class="kl" data-l="b">B</span><span class="kl" data-l="key">кн</span><b class="kdir">·</b><button class="btn sm" type="button" data-beep="${pre}" aria-pressed="false">звук</button></span>`,`<span id="ktn_${pre}">—</span>`,'Лампа спалахує на кожну зміну лінії, навіть коротку. Цифри — скільки разів лінія перемикалась: на клац A і B додають по 2. Ворушиш дріт без обертання і цифра росте — там поганий контакт. Росте лише одна з A/B — обрив другої. Не росте нічого — немає землі або «+». «звук» клацає на кожну зміну.')+
 `<div class="enc4">${['cw','ccw','click','hold'].map(a=>`<label>${LBL[a]}${sel('@'+pre+'.'+a+'.action',CAT.actions.map(x=>[x.id,x.name]),e[a].action,'data-str')}</label>`).join('')}</div>`+
 row('Кільце: діодів',num('@led.zones.'+zi+'.count',z.count,'min="0" max="8"'))+
 row('У спокої / на дію',sel('@led.zones.'+zi+'.anim',enumOpts('ledIdle'),z.anim,'data-str')+sel('@led.zones.'+zi+'.react',enumOpts('ledReact'),z.react,'data-str'))+
 `<details class="sub"><summary>Кольори і тонко</summary><div class="fr" data-leds="${zi}">${z.leds.slice(0,z.count).map((c,j)=>ledPick(zi,j,c)).join('')}<button class="btn sm" type="button" data-same="${zi}">усі як перший</button></div>
  ${row('Реакція: від → до',`<input type="color" class="in" data-kf="led.zones.${zi}.lo" value="${z.lo}" aria-label="Від"><input type="color" class="in" data-kf="led.zones.${zi}.hi" value="${z.hi}" aria-label="До">`)}
  ${row('Навпаки',sw('@'+pre+'.inverted',e.inverted))}${row('Імпульсів на клац',sel('@'+pre+'.ppd',[4,2,1],e.ppd))}${row('Утримання від, мс',num('@'+pre+'.holdMs',e.holdMs,'min="200" max="5000" step="50"'))}</details>`,'k_'+pre,'custom knob');
const ledPick=(i,j,c)=>`<input type="color" class="in" data-kf="led.zones.${i}.leds.${j}" value="${esc(c)}" aria-label="Діод ${j+1}">`;
/* макрос: тригер і ефект з каталогу; яке поле потрібне ефекту, каже macroEffects[].field */
const macHTML=(x,i)=>{const keys=x.keys.filter(k=>realActions().some(a=>a.id===k)),fx=enumOf('macroEffects',x.effect)||{},fld=fx.field||'';return `<div class="mac${x.valid===false&&x.enabled?' bad':''}" data-i="${i}">
 <div class="mh">${sw('#enabled',x.enabled,'aria-label="Увімкнено"')}<input class="in grow" data-f="name" value="${esc(x.name)}" maxlength="19" aria-label="Назва"><span class="chip off" data-fires>${x.fires||0}×</span></div>
 <div class="multi" data-f="keys" data-v="${esc(keys.join(','))}">${realActions().map(a=>`<button type="button" data-key="${esc(a.id)}" class="${keys.includes(a.id)?'on':''}">${esc(a.name)}</button>`).join('')}</div>
 <div class="mg"><label>Тригер${sel('#trigger',enumOpts('macroTriggers'),x.trigger,'data-str')}</label>
 <label>Разів${num('#taps',x.taps,'min="2" max="10"')}</label><label>Вікно, мс${num('#windowMs',x.windowMs,'min="50" max="2000" step="50"')}</label>
 <label>Ефект${sel('#effect',enumOpts('macroEffects'),x.effect,'data-str')}</label>
 <label data-show="action">Дія${sel('#action',CAT.actions.map(a=>[a.id,a.name]),x.action,'data-str')}</label>
 <label data-show="code raw">Код, hex${txt('#codeHex',fld==='code'||fld==='raw'?x.codeHex:'','07','in code')}</label>
 <label data-show="raw">Дані, hex${txt('#data',x.data||'','01 00','in')}</label>
 <label data-show="text">Текст${txt('#arg',fld==='text'?x.arg:'','MEGANE','in','text','maxlength="39"')}</label>
 <label data-show="scene">Сцена${sel('#argScene',CAT.scenes.map(s=>[s.id,s.name]),fld==='scene'?x.arg:'','data-str')}</label>
 <label class="ck"><input type="checkbox" class="cb" data-f="swallow"${x.swallow?' checked':''}> проковтнути</label></div>
 <div class="mf"><span class="mw"></span><button class="btn sm" type="button" data-test>▶ спробувати</button><button class="btn sm dng" type="button" data-del>Видалити</button></div></div>`};
const macVis=c=>{const fx=enumOf('macroEffects',$('[data-f="effect"]',c).value)||{},tg=enumOf('macroTriggers',$('[data-f="trigger"]',c).value)||{};
 $$('[data-show]',c).forEach(l=>l.hidden=!l.dataset.show.split(' ').includes(fx.field||'-'));$('[data-f="taps"]',c).closest('label').hidden=!tg.taps;$('[data-f="windowMs"]',c).closest('label').hidden=!tg.taps};
/* порожній рядок таблиці прошивки: тригер вимк, без назви, без ефекту */
const macEmpty=x=>!x.name&&!x.enabled&&!(enumOf('macroEffects',x.effect)||{}).field;
SCR.keys={title:'Кнопки',async render(m){
 const D=has('display'),K=has('knobs'),MX=has('matrix'),RS=has('res');
 const[am,rt,se,kn,mc,km,rs]=await Promise.all([getIf(D,'/api/affamap'),API.get('/api/health').then(kRt),API.get('/api/settings'),getIf(K,'/api/knobs'),API.get('/api/macros'),getIf(MX,'/api/keymap'),getIf(RS,'/api/res')]);
 this.kn=kn;this.seq=((am||{}).last||{}).seq;this.macCap=mc.macros.length;
 const DS=CAT.dests,off=d=>!d.built||!rt.dest[d.id];
 const shown=mc.macros.map((x,i)=>({...x,i})).filter(x=>!macEmpty(x));
 m.innerHTML=`<div class="jump">${[...(D?[['kWheel','Кермо']]:[]),...(MX?[['kMat','Матриця']]:[]),['kRoute','Куди'],...(RS?[['kRes','KEY1/KEY2']]:[]),...(K?[['k_enc2','Ручки']]:[]),['kMac','Макроси']].map(([i,t])=>`<button class="btn sm" type="button" data-jump="${i}">${t}</button>`).join('')}</div>`+
 (D?card('Кермо → дія',
  `<div class="row"><div class="l"><b>Остання</b>${hintBtn('Натисни на кермі: рядок засвітиться (синій клік, жовтий утримання). Засвітився не той рядок — переплутано дроти джойстика. ▶ робить те саме, що натискання.')}<small id="kLastW">натисни кнопку на кермі</small><small class="hint" hidden>Натисни на кермі: рядок засвітиться (синій — клік, жовтий — утримання). Засвітився не той рядок — переплутано дроти джойстика. ▶ = те саме, що натискання: дія піде маршрутами нижче.</small></div><div class="c"><span id="kLast" class="chip off">—</span></div></div>`+
  `<div class="krow" id="kHead" style="padding-top:4px;padding-bottom:4px"><div class="kgrid muted" style="font-size:12px;font-weight:800;text-transform:uppercase"><span>клік</span><span>утримання</span></div></div>`+
  `<div id="affa">${CAT.panelKeys.filter(k=>k.wire).map(k=>kRow(k,am.rows)).join('')}</div><div class="fr"><button class="btn dng" type="button" id="affaDef">Скинути до типових</button></div>`,'kWheel','custom full kwide'):MX?'':notBuilt('Кермо → дія','панелі (кермо по CAN)'))+
 (MX?kmCard(km):'')+
 card('Куди йдуть натискання',`<div class="tw"><table class="grid"><tr><th></th>${DS.map(d=>`<th>${esc(d.name)}<br><span class="chip off" data-live="${esc(d.id)}">…</span></th>`).join('')}</tr>
  ${CAT.classes.map(c=>`<tr><td>${esc(c.name)}</td>${DS.map(d=>`<td><input type="checkbox" class="cb" data-f="${esc(d.id)}:${c.bit}"${rt.dest[d.id]&&rt.dest[d.id].mask&(1<<c.bit)?' checked':''}${off(d)||!(d.classes>>c.bit&1)?' disabled':''} aria-label="${esc(d.name)} ${esc(c.name)}"></td>`).join('')}</tr>`).join('')}</table></div><div id="rtWhy"></div>`+
  `<div id="rtSet">${row('ГУ по UART',sw('/api/settings|uartEnabled',se.uartEnabled))}${row('Довжина натиску в ГУ, мс',num('/api/settings|pulseMs',se.pulseMs,'min="40" max="500" step="10"'))}${row('Показувати натискання на щитку, мс',num('/api/settings|keyPopupMs',se.keyPopup?se.keyPopupMs:0,'min="0" max="10000" step="100"'),'0 = не показувати')}</div>`,'kRoute','custom')+
 (RS?resCard(rs):'')+
 (K?knobCard(kn.enc2,kn.led.zones[0],'Ручка ліва','enc2',0)+knobCard(kn.enc,kn.led.zones[1],'Ручка права','enc',1)+
 card('Підсвітка',row('Увімкнено',sw('@led.enabled',kn.led.enabled))+row('Яскравість',`<input type="range" min="0" max="255" data-kf="led.brightness" value="${kn.led.brightness}" aria-label="Яскравість">`)+
  row('Струм зараз / межа, мА','<span id="kMa" class="chip off">…</span>'+num('@led.maxMilliamps',kn.led.maxMilliamps,'min="0" max="20000" step="50"'))+
  `<details class="sub"><summary>Тонко</summary>${row('Порядок кольорів',sel('@led.order',enumOpts('ledOrder'),kn.led.order,'data-str'))}${row('Правий перший',sw('@led.rightFirst',kn.led.rightFirst))}${row('Кільця проти годинникової',sw('@led.ringReverse',!!kn.led.ringReverse),'','Ефект біжить не в той бік, що ручка: діоди в кільці розпаяні проти годинникової. Перемикач дзеркалить усі кільця.')}${row('Тримати реакцію, мс',num('@led.holdMs',kn.led.holdMs,'min="0" max="5000" step="50"'),'0 = тримати')}${row('Кроків рівня',num('@led.levelSteps',kn.led.levelSteps,'min="4" max="64"'))}${row('Цикл анімації, 0,1 с',num('@led.animPeriod',kn.led.animPeriod,'min="10" max="255"'))}</details>`+
  `<details class="sub"><summary>Ноги · пізніше</summary>${row('Діодів',num('@led.feet.count',kn.led.feet.count,`min="0" max="${kn.led.feet.max||144}"`))}${row('Колір',`<input type="color" class="in" data-kf="led.feet.color" value="${kn.led.feet.color}" aria-label="Колір">`)}${row('У спокої',sel('@led.feet.anim',enumOpts('ledIdle'),kn.led.feet.anim,'data-str'))}${row('Яскравість',`<input type="range" min="0" max="255" data-kf="led.feet.brightness" value="${kn.led.feet.brightness}" aria-label="Яскравість ніг">`)}</details>`,'kLed','custom knob'):'')+
 card('Макроси',row('Зараз натиснуто','<span id="mDown" class="chips"><span class="muted">—</span></span>','<span id="mCnt"></span>')+`<div id="macs">${shown.map(x=>macHTML(x,x.i)).join('')}</div><div class="fr"><button class="btn" type="button" id="mAdd">+ Макрос</button><button class="btn dng" type="button" id="mDef">Скинути до прикладів</button></div>`,'kMac','custom full');
 $$('[data-jump]',m).forEach(b=>b.onclick=()=>$('#'+b.dataset.jump).scrollIntoView({behavior:'smooth',block:'start'}));
 // кермо
 if(D){const affa=$('#affa');
 const collect=()=>$$('.ka',affa).map(a=>({index:+a.dataset.i,action:$('[data-f="action"]',a).value,code:val($('[data-f="code"]',a)),asLong:(am.rows.find(r=>r.index===+a.dataset.i)||{}).asLong||false}));
 saver(affa,()=>API.post('/api/affamap',{rows:collect()}),'[data-f]');
 affa.addEventListener('change',e=>{if(e.target.dataset.f==='action')$('[data-f="code"]',e.target.closest('.ka')).placeholder=huOf(e.target.value)});
 affa.addEventListener('click',e=>{const b=e.target.closest('[data-test]');if(!b)return;const r=am.rows.find(x=>x.index===+b.closest('.ka').dataset.i);run(b,()=>API.post('/api/display/key?code=0x'+r.wire.toString(16)+'&hold='+(r.hold?1:0)),nameOf(CAT.panelKeys,r.key)+(r.hold?' утрим.':''))});
 $('#affaDef').onclick=e=>ask(e.target,'Скинути всі '+am.rows.length+' призначень до типових?',()=>run(e.target,()=>API.post('/api/affamap/defaults')).then(()=>route()))}
 if(MX)kmBind();
 if(RS)resBind(rs);
 // маршрути
 saver($('#kRoute .tw'),()=>{const p={};DS.forEach(d=>p[routeId(d.id)]=0);$$('#kRoute [data-f]').forEach(c=>{const[d,b]=c.dataset.f.split(':');if(c.checked)p[routeId(d)]|=1<<+b});return API.post('/api/settings',p)},'[data-f]');
 saver($('#rtSet'),async ks=>{const v=val($('[data-k="/api/settings|keyPopupMs"]'));const q={};for(const k of ks){const f=k.split('|')[1];if(f==='keyPopupMs'){q.keyPopup=v>0;if(v>0)q.keyPopupMs=v}else q[f]=val($(`[data-k="${k}"]`))}return API.post('/api/settings',q)},'[data-k]');
 this.why(rt);
 // ручки + підсвітка
 const knSave=()=>{const cfg={enc:deep(this.kn.enc),enc2:deep(this.kn.enc2),led:deep(this.kn.led)};$$('[data-kf]',m).forEach(el=>{const v=val(el);if(v!==undefined)setPath(cfg,el.dataset.kf,v)});Object.assign(this.kn,cfg);return API.post('/api/knobs',cfg)};
 $$('.knob',m).forEach(c=>saver(c,knSave,'[data-kf]'));
 m.addEventListener('input',e=>{const mm=/^led\.zones\.(\d)\.count$/.exec(e.target.dataset.kf||'');if(!mm)return;const i=+mm[1],box=$(`[data-leds="${i}"]`),n=Math.max(0,Math.min(8,Math.round(+e.target.value||0)));
  const cols=this.kn.led.zones[i].leds.slice();$$('input[type=color]',box).forEach((c,j)=>{cols[j]=c.value;c.remove()});box.insertAdjacentHTML('afterbegin',cols.slice(0,n).map((c,j)=>ledPick(i,j,c)).join(''))});
 m.addEventListener('click',e=>{const t=e.target.closest('[data-beep]');if(t){t.setAttribute('aria-pressed',t.getAttribute('aria-pressed')==='true'?'false':'true');t.classList.toggle('on');tick(900)}});
 m.addEventListener('click',e=>{const b=e.target.closest('[data-same]');if(!b)return;const ps=$$('input[type=color]',b.parentElement);if(!ps.length)return;ps.forEach(p=>p.value=ps[0].value);ps[0].dispatchEvent(new Event('input',{bubbles:true}))});
 // макроси: таблиця прошивки — суцільний список; рядки, яких не надіслано, стираються
 const macs=$('#macs');$$('.mac',macs).forEach(macVis);
 const macCollect=()=>$$('.mac',macs).map(c=>{const g=f=>val($(`[data-f="${f}"]`,c));const fld=(enumOf('macroEffects',g('effect'))||{}).field;
  return {enabled:g('enabled'),name:g('name'),trigger:g('trigger'),keys:g('keys'),taps:g('taps')||2,windowMs:g('windowMs')||600,swallow:g('swallow'),effect:g('effect'),
   action:fld==='action'?g('action'):noneAction(),codeHex:fld==='code'||fld==='raw'?g('codeHex')||'00':'00',data:fld==='raw'?g('data'):'',arg:fld==='scene'?g('argScene'):fld==='text'?g('arg'):''}});
 const mAddOk=()=>{$('#mAdd').disabled=$$('.mac',macs).length>=this.macCap};
 const macSave=async()=>{$$('.mac',macs).forEach(c=>{c.classList.remove('bad');$('.mw',c).textContent=''});
  try{const r=await API.post('/api/macros',{macros:macCollect()});$$('.mac',macs).forEach((c,i)=>c.dataset.i=i);mAddOk();return r}
  /* помилка «<номер>: <чому>»: позначаємо рядок із цим номером */
  catch(e){const mm=/^(\d+):\s*(.*)$/.exec(e.message||'');if(mm){const c=$$('.mac',macs)[+mm[1]];if(c){c.classList.add('bad');$('.mw',c).textContent=mm[2]}}throw e}};
 saver(macs,macSave,'[data-f]');
 macs.addEventListener('change',e=>{const c=e.target.closest('.mac');if(c&&/effect|trigger/.test(e.target.dataset.f||''))macVis(c)});
 macs.addEventListener('click',e=>{const b=e.target.closest('[data-test],[data-del]');if(!b)return;const c=b.closest('.mac');
  if(b.dataset.test!==undefined)run(b,()=>API.post('/api/macros/test?id='+c.dataset.i),'запущено');
  else ask(b,'Видалити «'+$('[data-f="name"]',c).value+'»?',()=>{c.remove();run($('#mAdd'),macSave,'видалено')})});
 $('#mAdd').onclick=()=>{const tg=enumList('macroTriggers').find(t=>t.taps)||{},fx=enumList('macroEffects').find(f=>f.field==='action')||{},d=document.createElement('div');
  d.innerHTML=macHTML({enabled:true,name:'Новий',trigger:tg.v,keys:[],taps:2,windowMs:400,effect:fx.v,action:(realActions()[0]||{}).id,codeHex:'',data:'',arg:'',swallow:true,fires:0},$$('.mac',macs).length);
  const c=d.firstElementChild;macs.append(c);macVis(c);mAddOk();$('[data-f="name"]',c).focus();sv(svOf(macs),'bad','вибери клавішу')};
 mAddOk();
 $('#mDef').onclick=e=>ask(e.target,'Замінити всі макроси прикладами?',()=>run(e.target,()=>API.post('/api/macros/defaults')).then(()=>route()));
 /* підсвітка натискань: окреме швидке опитування, лише поки відкрита вкладка */
 if(D||K||RS){const fast=setInterval(()=>{if(D)this.live().catch(()=>{});if(K)API.get('/api/knobs/live').then(kl=>this.knobTest(kl)).catch(()=>{});if(RS)resLive().catch(()=>{})},K?250:300);leave=()=>clearInterval(fast)}
 this.n=0;await this.tick();
},async live(){const r=await API.get('/api/affamap?live=1');const L=r.last;if(!L||L.seq===this.seq||!$('#affa'))return;this.seq=L.seq;
 $$('#affa .krow').forEach(x=>x.classList.remove('hitC','hitH'));$$('#affa .ka').forEach(x=>x.classList.remove('hit'));
 const kr=L.key?$(`#affa .krow[data-key="${L.key}"]`):null;
 if(kr){kr.classList.add(L.hold?'hitH':'hitC');$$('.ka',kr)[L.hold?1:0].classList.add('hit');clearTimeout(this.ht);this.ht=setTimeout(()=>{kr.classList.remove('hitC','hitH');$$('.ka',kr).forEach(x=>x.classList.remove('hit'))},1500)}
 const none=L.action===noneAction(),wire='0x'+Number(L.wire).toString(16).padStart(4,'0');
 setChip('#kLast',L.key?(L.dests.length?'ok':'warn'):'bad',(L.key?nameOf(CAT.panelKeys,L.key):'невідома')+(L.hold?' · утрим.':' · клік'));
 $('#kLastW').textContent=wire+(L.key?' → '+(none?'нічого не призначено':actName(L.action)+(L.dests.length?' · маршрут: '+L.dests.map(destName).join(', '):' · маршруту нема')):' — такого коду в таблиці нема')},
knobTest(kl){this.kt=this.kt||{};for(const pre of['enc','enc2']){const el=$('#kt_'+pre),l=kl[pre];if(!el||!l)continue;
 const p=this.kt[pre],g=l.edges||[0,0,0],moved=p?g.map((n,i)=>n!==p.g[i]):[0,0,0];
 $$('.kl',el).forEach((x,i)=>x.classList.toggle('on',!!l[x.dataset.l]||!!moved[i]));
 const d=$('.kdir',el);if(p&&l.pos!==p.pos){d.textContent=l.pos>p.pos?'⟳':'⟲';clearTimeout(p.t);p.t=setTimeout(()=>d.textContent='·',700)}
 if(p&&moved.some(Boolean)&&$('[data-beep="'+pre+'"]',el).getAttribute('aria-pressed')==='true')tick(pre==='enc'?1200:700);
 this.kt[pre]={pos:l.pos,g,t:p&&p.t};$('#ktn_'+pre).textContent=`A ${g[0]} · B ${g[1]} · кн ${g[2]} · ${l.detents} клац (${l.pos>0?'+':''}${l.pos}) · ${l.clicks} натиск${l.errors?' · '+l.errors+' помилок':''}`}},
why(rt){const w=Object.entries(rt.dest).filter(([d,x])=>!x.live&&(byId(CAT.dests,d)||{}).built).map(([d,x])=>`<div class="row"><div class="l"><b>${esc(destName(d))}: ${esc(x.why||'')}</b></div>${d==='uart'?'':`<div class="c"><a href="#board">Плата →</a></div>`}</div>`).join('')+((rt.dest.ams||{}).volFallback?row('Гучність → iPhone','<span class="chip warn">запасний шлях</span>','ГУ мовчить, тож гучність іде в телефон'):'');const el=$('#rtWhy');if(el)el.innerHTML=w;
 CAT.dests.forEach(d=>{const c=$(`[data-live="${d.id}"]`),x=rt.dest[d.id];if(c)setChip(c,x&&x.live?'ok':'off',!d.built||!x?'нема':x.live?'живий':'мертвий')})},
async tick(){
 this.n++;const[kl,ml]=await Promise.all([getIf(has('knobs'),'/api/knobs/live'),API.get('/api/macros/live')]);if(!$('#mDown'))return;
 if(kl&&$('#kMa')){const kc=(id,k,l)=>setChip(id,!k.fitted?'off':k.enabled?'ok':'off',!k.fitted?'не встановлена':k.enabled?`${l.detents} клац${l.btn?' · натиснута':''}`:'вимк');
 kc('#kl_enc',this.kn.enc,kl.enc||{});kc('#kl_enc2',this.kn.enc2,kl.enc2||{});
 const led=kl.led||{};setChip('#kMa',led.mA>this.kn.led.maxMilliamps*.9?'warn':led.ok?'ok':'bad',led.ok?led.mA+' мА':'стрічка не відповідає')}
 $('#mDown').innerHTML=(ml.down||[]).length?ml.down.map(k=>chipH('ok',actName(k))).join(''):'<span class="muted">—</span>';
 $('#mCnt').textContent=`спрацювань ${ml.fired} · проковтнуто ${ml.swallowed} · повторено ${ml.replayed}`;
 $$('#macs .mac').forEach(c=>{const f=$('[data-fires]',c),n=(ml.fires||[])[+c.dataset.i];if(f&&n!=null)f.textContent=n+'×'});
 if(this.n%3===0)this.why(kRt(await API.get('/api/health')));
}};
