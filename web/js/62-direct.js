/* ---------- Кнопки, збірка direct: матриця 3×3 (/api/keymap) і драбина KEY1/KEY2 (/api/res*) ----------
   Лише там, де прошивка їх зібрала (CAT.features.matrix / .res); на T-2CAN цих карток нема.
   Екран «Кнопки» (60-keys.js) вставляє картки й викликає kmBind/resBind/resLive. */

/* фаза енкодера — позиція матриці, не вибір: у каталозі її нема, тож показуємо й відсилаємо як є */
const isPhase=a=>/^EncPhase/.test(a);
const kmCell=s=>`<div class="kmc" data-pos="${s.pos}"><small class="muted">позиція ${s.pos+1}</small>${isPhase(s.action)
 ?`<span class="chip off" data-ph="${esc(s.action)}">колесо · фаза ${esc(s.action.slice(-1))}</span>`
 :sel('#action',CAT.actions.map(a=>[a.id,a.name]),s.action,'data-str aria-label="Дія"')+`<input class="in code kc" data-f="code" data-hex value="${s.code?h2(s.code):''}" placeholder="${esc(huOf(s.action))}" maxlength="2" aria-label="Код для ГУ, порожньо — типовий" title="Код для ГУ; порожньо — типовий">`}</div>`;
const kmCard=k=>card('Матриця 3×3',
 `<div class="kmg" id="kmGrid">${k.slots.map(kmCell).join('')}</div>`+
 row('Колесо: коди ↻ / ↺',`<input class="in code" data-f="encCw" data-hex value="${h2(k.encCw)}" maxlength="2" aria-label="Код за годинниковою"><input class="in code" data-f="encCcw" data-hex value="${h2(k.encCcw)}" maxlength="2" aria-label="Код проти годинникової">`,'','Байти, які ГУ отримує на клац колеса. Типові — RAV4 TUNE+ / TUNE−.')+
 row('Колесо навпаки',sw('#encInverted',k.encInverted))+
 `<div class="fr"><button class="btn dng" type="button" id="kmDef">Скинути до Megane</button></div>`,'kMat','custom');

function kmBind(){const g=$('#kMat');if(!g)return;
 const collect=()=>({slots:$$('.kmc',g).map(c=>{const ph=$('[data-ph]',c),pos=+c.dataset.pos;
   return ph?{pos,action:ph.dataset.ph,code:0}:{pos,action:$('[data-f="action"]',c).value,code:val($('[data-f="code"]',c))||0}}),
  encCw:val($('[data-f="encCw"]',g))||0,encCcw:val($('[data-f="encCcw"]',g))||0,encInverted:val($('[data-f="encInverted"]',g))});
 saver(g,()=>API.post('/api/keymap',collect()),'[data-f]');
 g.addEventListener('change',e=>{if(e.target.dataset.f==='action')$('[data-f="code"]',e.target.closest('.kmc')).placeholder=huOf(e.target.value)});
 $('#kmDef').onclick=e=>ask(e.target,'Скинути матрицю до розкладки Megane?',()=>run(e.target,()=>API.post('/api/keymap/defaults')).then(()=>route()))}

/* ---- драбина: щабель = маска ніг (паралельно) + послідовний; OPEN — ноги не впаяні ---- */
const RES_OPEN=65535;
const ohmT=o=>o===RES_OPEN?'—':o>=1000?(o/1000).toFixed(o%1000?1:0)+' кОм':o+' Ом';
const rungOpts=(c,none)=>[[0,none],...c.rungs.map(r=>[r.mask,ohmT(r.ohms)])];
const RES_T=[['pulseMs','Натиск, мс',50,1000],['gapMs','Пауза між натисками, мс',20,1000],['maxHoldMs','Найдовше утримання, мс',500,20000],
 ['holdAfter','Утримувати після клаців',1,20],['repeatStartMs','Повтор після, мс',100,3000],['repeatMs','Крок повтору, мс',50,2000]];
const resRow=(a,r)=>{const c=r.ch[a.ch]||r.ch[0];return `<tr data-a="${esc(a.name)}"><td>${esc(actName(a.name))}</td>
 <td>${sel('#ch',r.ch.map((_,i)=>[i,'KEY'+(i+1)]),a.ch,'aria-label="Лінія"')}</td><td>${sel('#mask',rungOpts(c,'—'),a.mask,'aria-label="Щабель кліку"')}</td>
 <td>${a.enc?'<span class="muted">—</span>':sel('#longMask',rungOpts(c,'як клік'),a.longMask,'aria-label="Щабель утримання"')}</td>
 <td><button class="btn sm" type="button" data-learn title="Утримати щабель 4 с, щоб ГУ його вивчила">▶</button></td></tr>`};
const resCard=r=>card('KEY1/KEY2 · резистори <span id="resLive" class="chip off" style="margin-left:6px">…</span>',
 `<div id="resTop">${row('Увімкнено',sw('#enabled',r.enabled))}${row('Керування',seg('#activeHigh',[['0','GPIO'],['1','MOSFET']],r.activeHigh?'1':'0'),'','GPIO тягне лінію сам; MOSFET — GPIO відкриває транзистор (HIGH = натиснуто).')}
 ${row('Колесо',sel('#spin',enumOpts('resSpin'),r.spin))}
 <details class="sub"><summary>Час</summary>${RES_T.map(([k,l,a,b])=>row(l,num('#t.'+k,r.timing[k],`min="${a}" max="${b}"`))).join('')}</details></div>`+
 r.ch.map((c,i)=>`<div class="sub resch" data-ch="${i}"><div class="row"><div class="l"><b>KEY${i+1}</b><small>піни ${esc(c.pins.join(', '))}</small></div><div class="c">${c.inWindow?'':chipH('bad','поза вікном ГУ')}<button class="btn sm" type="button" data-auto="${i}">Розкласти кнопки</button></div></div>
  <div class="fr">${Array.from({length:c.legs},(_,j)=>`<label>нога ${j+1}, Ом${num('#leg.'+j,c.leg[j],'min="0" max="65000"')}</label>`).join('')}<label>послідовно, Ом${num('#series',c.series,'min="0" max="65000"')}</label></div></div>`).join('')+
 `<div class="tw"><table class="grid" id="resMap"><tr><th>Кнопка</th><th>Лінія</th><th>Клік</th><th>Утримання</th><th></th></tr>${r.actions.map(a=>resRow(a,r)).join('')}</table></div>`+
 row('Найменший запас між щаблями',chipH(r.sep===255?'off':r.sep<15?'warn':'ok',r.sep===255?'—':r.sep+' %'),'','Наскільки близькі два найближчі щаблі, які ГУ має розрізнити. Менше ~15 % — ГУ може їх плутати.')+
 (r.free.length?`<details class="sub"><summary>Вільні щаблі (${r.free.length})</summary>${r.free.map(f=>row('KEY'+(f.ch+1)+' · '+ohmT(f.ohms),`<button class="btn sm" type="button" data-fire="${f.ch}:${f.mask}">▶ утримати</button>`)).join('')}</details>`:''),'kRes','custom');

function resBind(r){const c=$('#kRes');if(!c)return;
 const top=$('#resTop');
 saver(top,()=>{const t={};$$('[data-f^="t."]',top).forEach(e=>{const v=val(e);if(v!==undefined)t[e.dataset.f.slice(2)]=v});
  return API.post('/api/res',{enabled:val($('[data-f="enabled"]',top)),activeHigh:val($('[data-f="activeHigh"]',top))==='1',spin:val($('[data-f="spin"]',top)),timing:t})},'[data-f]');
 /* ноги й послідовний міняють щаблі: після збереження картка перемальовується */
 $$('.resch',c).forEach(b=>saver(b,async()=>{const i=+b.dataset.ch,legs=r.ch.map(()=>null),series=r.ch.map(()=>null);
  legs[i]=$$('[data-f^="leg."]',b).map(e=>val(e)||0);series[i]=val($('[data-f="series"]',b))||0;
  const x=await API.post('/api/res',{legs,series});setTimeout(()=>route(),0);return x},'[data-f]'));
 $('#resMap').addEventListener('change',e=>{const tr=e.target.closest('tr[data-a]');if(!tr)return;
  const q={action:tr.dataset.a,ch:val($('[data-f="ch"]',tr)),mask:val($('[data-f="mask"]',tr))};const l=$('[data-f="longMask"]',tr);if(l)q.longMask=val(l);
  /* інша лінія — інші щаблі: щабель скидається, картка перемальовується */
  if(e.target.dataset.f==='ch'){q.mask=0;q.longMask=0}
  run(e.target,()=>API.post('/api/res/map',q)).then(()=>{if(e.target.dataset.f==='ch')route()})});
 c.addEventListener('click',e=>{const b=e.target.closest('[data-learn],[data-fire],[data-auto]');if(!b)return;
  if(b.dataset.learn!==undefined)run(b,()=>API.post('/api/res/learn',{action:b.closest('tr').dataset.a}),'тримаю 4 с — ГУ вчиться');
  else if(b.dataset.fire){const[ch,mask]=b.dataset.fire.split(':').map(Number);run(b,()=>API.post('/api/res/learn',{ch,mask}),'тримаю 4 с')}
  else run(b,async()=>{const x=await API.post('/api/res/auto',{ch:+b.dataset.auto});setTimeout(()=>route(),0);return {note:'розкладено '+x.placed+' з '+x.wanted}})})}

/* жива лінія: що зараз на KEY1/KEY2 */
async function resLive(){const el=$('#resLive');if(!el)return;const l=await API.get('/api/res/live');
 const on=l.ch.map((c,i)=>c.mask?'KEY'+(i+1)+' '+ohmT(c.ohms)+(c.learn?' · вчу':''):'').filter(Boolean);
 setChip(el,on.length?(l.ch.some(c=>c.learn)?'warn':'ok'):l.stuck?'bad':'off',on.length?on.join(' · '):l.stuck?'застрягло '+l.stuck:'тихо · '+l.presses+' натиск.')}
