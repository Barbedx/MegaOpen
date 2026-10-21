/* ============ Авто — наживо ============
   Усі сигнали Vehicle::State: значення, вік, звідки, чи перевірено в машині.
   GET /api/map?live=1 раз на 2 с (одне опитування на тік; вкладка схована —
   99-main.js не тікає), опис сигналів — ?meta=1 раз.
   Без вебсокетів. Правила сповіщень — таблиця реєстру налаштувань `alerts`:
   GET/POST /api/settings, рядок = [сигнал, умова, поріг, пауза, увімк],
   уся таблиця з 16 рядків; колонки й умови — з CAT.settings. */
const LV_GRP=['Рух','Кузов','Світло','Поїздка й пальне','Клімат','Радар','Шини','Звук','Налаштування','Двигун','Безпека','Динаміка'];
const LV_SRC={c:['CAN','ok'],k:['стала','warn'],p:['закріплено','warn'],d:['розрах.','ok'],o:['нема','off']};
/* meta: [name, unit, decimals, yes/no, group, min, max, label]; sigs: [value, ageMs, src, verified, confirm]
   confirm: 1 — гіпотеза на CAN, можна «✓ підтвердити»; 2 — підтверджено тут, можна «↺ скасувати».
   POST /api/map {confirm:{sig,on}}; «Заводські прив'язки» повертають гіпотези. */
const LV_CONF=[['Підтвердити: сигнал перевірено в машині, «?» зникне всюди (меню, екран авто, G-метр). «Заводські прив\'язки» повернуть його в гіпотези.',true],
 ['Скасувати підтвердження: сигнал знову гіпотеза, з «?».',false]];
const lvName=m=>m[7]||((CAT.raise||[]).find(t=>t.id===m[0])||{}).name||m[0];
const lvUnit=m=>m[1]&&!/^-?\d|^raw$/.test(m[1])?' '+m[1]:'';
const lvVal=(m,v)=>v==null?DASH:m[3]?(+v?'так':'ні'):v+lvUnit(m);
const lvAge=a=>a==null?DASH:a<1000?'<1 с':a<60000?Math.round(a/1000)+' с':Math.round(a/60000)+' хв';
/* Запис для перевірки в машині: те, що сторінка й так бере раз на 2 с (жодного
   зайвого запиту), у памʼять цієї вкладки рядком JSON на зріз (так і рахує
   розмір), до REC_MAX зрізів (2 год). Стоп — пауза: «Записати» дописує далі,
   час у CSV показує прогалини. Позначки — рядки з текстом між зрізами. Ніде не зберігається: перезавантаження
   сторінки його губить, тому до того — «Завантажити CSV»; поки є незбережене,
   beforeunload питає (лише тоді: зайвий слухач вішав навігацію headless-Chrome у ui_audit). */
const REC_MAX=3600,REC_MARKS=['лівий поворот','правий поворот','гальмо','ручник','розгін'];
const REC={on:false,rows:[],n:0,bytes:0,M:null,saved:true};
const recUnload=e=>{e.preventDefault();e.returnValue=''};
const recSaved=v=>{REC.saved=v;window[(v?'remove':'add')+'EventListener']('beforeunload',recUnload)};
const recAdd=(M,r)=>{if(!REC.on||!r||!r.sigs)return;REC.M=REC.M||M;
 const s=JSON.stringify(r.sigs);REC.rows.push({t:Date.now(),s});REC.n++;REC.bytes+=s.length+16;recSaved(false);if(REC.n>=REC_MAX)REC.on=false};
const recMark=t=>{t=(t||'').trim();if(!t||!REC.M)return false;REC.rows.push({t:Date.now(),mark:t});REC.bytes+=t.length+16;recSaved(false);return true};
const recSize=b=>b<1048576?Math.max(1,Math.round(b/1024))+' кБ':(b/1048576).toFixed(1)+' МБ';
const recStat=()=>{if(!REC.rows.length)return REC.on?'● чекаю на перше оновлення':'порожньо';const a=REC.rows[0].t,b=REC.rows[REC.rows.length-1].t;
 return (REC.on?'● ':REC.n>=REC_MAX?'повно (2 год) · ':'')+fmtUp(b-a)+' · '+REC.n+' зрізів · '+(REC.rows.length-REC.n)+' позн. · '+recSize(REC.bytes)};
const csvQ=x=>{const s=x==null?'':String(x);return /[",\n\r]/.test(s)?'"'+s.replace(/"/g,'""')+'"':s};
const recTime=t=>{const d=new Date(t),p=(n,w=2)=>String(n).padStart(w,'0');return d.getFullYear()+'-'+p(d.getMonth()+1)+'-'+p(d.getDate())+' '+p(d.getHours())+':'+p(d.getMinutes())+':'+p(d.getSeconds())+'.'+p(d.getMilliseconds(),3)};
/* CSV: час, секунд від першого рядка, позначка; далі на кожен сигнал
   значення, перевірено (1/0) і вік, мс. BOM — щоб Excel узяв UTF-8. */
function recCsv(){const M=REC.M||[],t0=REC.rows.length?REC.rows[0].t:0,L=[];
 L.push(['time','s','mark',...M.flatMap(m=>[m[0],m[0]+'.ok',m[0]+'.age_ms'])].map(csvQ).join(','));
 for(const r of REC.rows){const h=[recTime(r.t),((r.t-t0)/1000).toFixed(1),r.mark||''];
  if(r.s){const S=JSON.parse(r.s);M.forEach((m,i)=>{const s=S[i]||[];h.push(s[0],s[3]?1:0,s[1])})}else M.forEach(()=>h.push('','',''));
  L.push(h.map(csvQ).join(','))}
 return '﻿'+L.join('\r\n')+'\r\n'}
function recSave(){const a=document.createElement('a'),t=recTime(REC.rows[0].t).slice(0,16).replace(/[-: ]/g,'');
 a.href=URL.createObjectURL(new Blob([recCsv()],{type:'text/csv;charset=utf-8'}));a.download='megaopen-live-'+t+'.csv';
 document.body.appendChild(a);a.click();a.remove();setTimeout(()=>URL.revokeObjectURL(a.href),60000);recSaved(true)}
SCR.live={title:'Авто — наживо',hidden:true,async render(m){
 const[r,se]=await Promise.all([API.get('/api/map?meta=1&live=1'),API.get('/api/settings')]);
 const al=(CAT.settings||[]).find(f=>f.id==='alerts')||{cols:[]},cond=al.cols.find(c=>c.id==='cond')||{enum:[]};
 this.M=r.meta;this.C=cond.enum.map(o=>[o.v,o.name]);this.A=se.alerts||[];this.n=0;
 m.innerHTML=card('Запис для перевірки',row('Запис','<button class="btn pri" type="button" id="rcGo"></button>',`<span id="rcSt" class="mono"></span>`)+
  `<div class="pad"><div class="seg wrap" id="rcMk">${REC_MARKS.map(t=>`<button type="button" data-mk="${esc(t)}">${esc(t)}</button>`).join('')}</div></div>`+
  `<div class="fr">${txt('#rcTxt','','своя позначка','in','text','data-nosave maxlength="60" aria-label="Своя позначка"')}<button class="btn" type="button" id="rcAdd">Позначка</button></div>`+
  '<div class="fr"><button class="btn" type="button" id="rcCsv">Завантажити CSV</button><button class="btn dng" type="button" id="rcClr">Очистити</button></div>'+
  row('','','Пише те, що сторінка й так оновлює раз на 2 с: усі сигнали, вік і ✓, у памʼять телефона, до 2 год. Екран має світитись, а сторінка — бути відкритою: схована вкладка не опитує, і в CSV буде прогалина. Позначка — рядок із текстом у ту мить. Стоп — пауза, «Записати» пише далі. Ніде не зберігається: завантаж CSV, перш ніж закрити чи оновити сторінку.'),'rcCard','custom')+
  card('Сигнали',row('Оновлювати',sw('#lvAuto',true,'data-nosave'),'раз на 2 с, поки сторінка відкрита')+
  row('Лише з даними',sw('#lvOnly',true,'data-nosave'))+row('Сповіщень',`<span id="lvAl" class="muted">${DASH}</span>`,'спрацювало / показано / зняли до показу')+
  `<div class="tw"><table><thead><tr><th>Сигнал</th><th>Зараз</th><th>Вік</th><th>Звідки</th><th title="перевірено в машині">✓</th><th></th></tr></thead><tbody id="lvT"></tbody></table></div>`,'lvCard','custom full')+
 card('Сповіщення на дисплей',row('','','Умова справдилась — на дисплеї спливає назва й значення, але не поверх меню: чекає, поки воно закриється, і зникає, якщо умова вже минула. «зʼявилось» для так/ні — момент увімкнення. Пауза — не частіше, ніж раз на стільки секунд. 🔔 у таблиці вище додає сигнал сюди.')+
  `<div class="tw"><table><thead><tr><th>Сигнал</th><th>Умова</th><th>Поріг</th><th>Пауза, с</th><th>Увімк</th></tr></thead><tbody id="alT"></tbody></table></div>`+
  '<div class="fr"><button class="btn pri" type="button" id="alSave">Зберегти правила</button></div>','alCard','custom');
 this.draw(r);this.drawAlerts();recAdd(this.M,r);this.drawRec();
 $('[data-f="lvOnly"]').onchange=()=>this.draw(this.last);
 $('#rcGo').onclick=()=>{REC.on=!REC.on&&REC.n<REC_MAX;if(REC.on){REC.M=REC.M||this.M;const a=$('[data-f="lvAuto"]');if(a)a.checked=true}this.drawRec()};
 const mark=(t,el)=>{if(recMark(t)){sv(svOf(el),'ok',t);this.drawRec();return true}sv(svOf(el),'bad',REC.M?'порожня':'спершу «Записати»');return false};
 $('#rcMk').onclick=e=>{const b=e.target.closest('[data-mk]');if(b)mark(b.dataset.mk,b)};
 $('#rcAdd').onclick=e=>{const i=$('[data-f="rcTxt"]');if(mark(i.value,e.target))i.value=''};
 $('#rcCsv').onclick=e=>{if(!REC.rows.length){sv(svOf(e.target),'bad','порожньо');return}recSave();sv(svOf(e.target),'ok','CSV');this.drawRec()};
 $('#rcClr').onclick=e=>{const b=e.target;if(!REC.saved&&!b.dataset.sure){b.dataset.sure=1;b.textContent='Не збережено. Ще раз — очистити';return}
  Object.assign(REC,{on:false,rows:[],n:0,bytes:0,M:null});recSaved(true);delete b.dataset.sure;b.textContent='Очистити';this.drawRec()};
 $('#alSave').onclick=e=>{this.A=this.readAlerts();run(e.target,()=>API.post('/api/settings',{alerts:this.A.map(a=>[a[0],a[1],+a[2]||0,parseInt(a[3])||0,!!a[4]])}),'збережено')};
 $('#lvT').onclick=e=>{const c=e.target.closest('[data-conf]');
  if(c){const mm=this.M[+c.dataset.conf],[t,on]=LV_CONF[c.dataset.on==='1'?0:1];
   ask(c,lvName(mm)+'. '+t,()=>run(c,()=>API.post('/api/map',{confirm:{sig:mm[0],on}}),on?'підтверджено':'знову гіпотеза').then(async()=>this.draw(await API.get('/api/map?live=1'))));return}
  const b=e.target.closest('[data-add]');if(!b)return;this.A=this.readAlerts();
  const i=this.A.findIndex(a=>!a[0]);if(i<0){sv(svOf($('#alSave')),'bad','усі 16 зайняті');return}
  const mm=this.M[+b.dataset.add];this.A[i]=[mm[0],mm[3]?'rises':'above',0,60,true];this.drawAlerts();$('#alCard').scrollIntoView({behavior:'smooth'})};
},
draw(r){this.last=r;const tb=$('#lvT');if(!tb||!r||!r.sigs||$('.ask',tb))return;const only=$('[data-f="lvOnly"]').checked;let h='',g=-1;
 const idx=this.M.map((m,i)=>i).filter(i=>!only||r.sigs[i][0]!=null).sort((a,b)=>this.M[a][4]-this.M[b][4]||a-b);
 for(const i of idx){const m=this.M[i],[v,age,src,ok,cf]=r.sigs[i];
  const vh=(ok?'✓':`<span class="muted" title="${src==='d'?'розрахунок: підтвердіть його вхідні сигнали':'ще не перевірено в машині'}">?</span>`)+
   (cf===1?` <button class="btn sm" type="button" data-conf="${i}" data-on="1" title="перевірено в машині">✓ підтвердити</button>`:
    cf===2?` <button class="btn sm" type="button" data-conf="${i}" data-on="0" title="підтверджено тут; знову гіпотеза">↺ скасувати</button>`:'');
  if(m[4]!==g){g=m[4];h+=`<tr><th colspan="6">${esc(LV_GRP[g]||'')}</th></tr>`}
  const stale=src==='c'&&age!=null&&age>3000,s=LV_SRC[src]||LV_SRC.o;
  h+=`<tr${stale?' class="muted"':''}><td>${esc(lvName(m))} <small class="mono muted">${esc(m[0])}</small></td><td class="num">${esc(lvVal(m,v))}</td><td class="num">${lvAge(age)}</td><td>${chipH(s[1],s[0])}</td><td style="white-space:nowrap">${vh}</td><td><button class="btn sm" type="button" data-add="${i}" title="сповіщення на дисплей">🔔</button></td></tr>`}
 tb.innerHTML=h||'<tr><td colspan="6" class="muted">жодного значення: CAN A мовчить?</td></tr>';
 if(r.alertStats)$('#lvAl').textContent=r.alertStats.fired+' / '+r.alertStats.shown+' / '+r.alertStats.dropped},
drawAlerts(){const so=[['','—'],...this.M.map(m=>[m[0],lvName(m)])],co=this.C.map(c=>[c[0],c[1]]);
 $('#alT').innerHTML=this.A.map((a,i)=>`<tr data-r="${i}"><td>${sel('#sig',so,a[0],'data-str')}</td><td>${sel('#cond',co,a[1],'data-str')}</td><td>${num('#thr',a[2],'step="any" style="width:6em"')}</td><td>${num('#cd',a[3],'min="5" max="65535" style="width:5em"')}</td><td>${sw('#on',a[4])}</td></tr>`).join('')},
readAlerts(){return $$('#alT tr').map(tr=>{const g=f=>val($(`[data-f="${f}"]`,tr));return [g('sig')||'',g('cond')||'rises',g('thr')??0,g('cd')??0,!!g('on')]})},
drawRec(){const g=$('#rcGo'),s=$('#rcSt');if(!g)return;g.textContent=REC.on?'■ Стоп':'● Записати';g.classList.toggle('dng',REC.on);g.classList.toggle('pri',!REC.on);
 g.disabled=!REC.on&&REC.n>=REC_MAX;s.textContent=recStat();$('#rcCsv').disabled=!REC.rows.length},
async tick(){const a=$('[data-f="lvAuto"]');if(!a||!a.checked||this.n++%2)return;const r=await API.get('/api/map?live=1');this.draw(r);recAdd(this.M,r);this.drawRec()}};
