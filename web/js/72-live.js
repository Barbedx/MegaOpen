/* ============ Авто — наживо ============
   Усі сигнали Vehicle::State: значення, вік, звідки, чи перевірено в машині.
   GET /api/map?live=1 раз на 2 с (одне опитування на тік; вкладка схована —
   99-main.js не тікає), опис сигналів — ?meta=1 раз.
   Без вебсокетів. Правила сповіщень — таблиця реєстру налаштувань `alerts`:
   GET/POST /api/settings, рядок = [сигнал, умова, поріг, пауза, увімк],
   уся таблиця з 16 рядків; колонки й умови — з CAT.settings. */
const LV_GRP=['Рух','Кузов','Світло','Поїздка й пальне','Клімат','Радар','Шини','Звук','Налаштування','Двигун','Безпека','Динаміка'];
const LV_SRC={c:['CAN','ok'],k:['стала','warn'],p:['закріплено','warn'],d:['розрах.','ok'],o:['нема','off']};
/* meta: [name, unit, decimals, yes/no, group, min, max, label]; sigs: [value, ageMs, src, verified] */
const lvName=m=>m[7]||((CAT.raise||[]).find(t=>t.id===m[0])||{}).name||m[0];
const lvUnit=m=>m[1]&&!/^-?\d|^raw$/.test(m[1])?' '+m[1]:'';
const lvVal=(m,v)=>v==null?DASH:m[3]?(+v?'так':'ні'):v+lvUnit(m);
const lvAge=a=>a==null?DASH:a<1000?'<1 с':a<60000?Math.round(a/1000)+' с':Math.round(a/60000)+' хв';
SCR.live={title:'Авто — наживо',hidden:true,async render(m){
 const[r,se]=await Promise.all([API.get('/api/map?meta=1&live=1'),API.get('/api/settings')]);
 const al=(CAT.settings||[]).find(f=>f.id==='alerts')||{cols:[]},cond=al.cols.find(c=>c.id==='cond')||{enum:[]};
 this.M=r.meta;this.C=cond.enum.map(o=>[o.v,o.name]);this.A=se.alerts||[];this.n=0;
 m.innerHTML=card('Сигнали',row('Оновлювати',sw('#lvAuto',true,'data-nosave'),'раз на 2 с, поки сторінка відкрита')+
  row('Лише з даними',sw('#lvOnly',true,'data-nosave'))+row('Сповіщень',`<span id="lvAl" class="muted">${DASH}</span>`,'спрацювало / показано / зняли до показу')+
  `<div class="tw"><table><thead><tr><th>Сигнал</th><th>Зараз</th><th>Вік</th><th>Звідки</th><th title="перевірено в машині">✓</th><th></th></tr></thead><tbody id="lvT"></tbody></table></div>`,'lvCard','custom full')+
 card('Сповіщення на дисплей',row('','','Умова справдилась — на дисплеї спливає назва й значення, але не поверх меню: чекає, поки воно закриється, і зникає, якщо умова вже минула. «зʼявилось» для так/ні — момент увімкнення. Пауза — не частіше, ніж раз на стільки секунд. 🔔 у таблиці вище додає сигнал сюди.')+
  `<div class="tw"><table><thead><tr><th>Сигнал</th><th>Умова</th><th>Поріг</th><th>Пауза, с</th><th>Увімк</th></tr></thead><tbody id="alT"></tbody></table></div>`+
  '<div class="fr"><button class="btn pri" type="button" id="alSave">Зберегти правила</button></div>','alCard','custom');
 this.draw(r);this.drawAlerts();
 $('[data-f="lvOnly"]').onchange=()=>this.draw(this.last);
 $('#alSave').onclick=e=>{this.A=this.readAlerts();run(e.target,()=>API.post('/api/settings',{alerts:this.A.map(a=>[a[0],a[1],+a[2]||0,parseInt(a[3])||0,!!a[4]])}),'збережено')};
 $('#lvT').onclick=e=>{const b=e.target.closest('[data-add]');if(!b)return;this.A=this.readAlerts();
  const i=this.A.findIndex(a=>!a[0]);if(i<0){sv(svOf($('#alSave')),'bad','усі 16 зайняті');return}
  const mm=this.M[+b.dataset.add];this.A[i]=[mm[0],mm[3]?'rises':'above',0,60,true];this.drawAlerts();$('#alCard').scrollIntoView({behavior:'smooth'})};
},
draw(r){this.last=r;const tb=$('#lvT');if(!tb||!r||!r.sigs)return;const only=$('[data-f="lvOnly"]').checked;let h='',g=-1;
 const idx=this.M.map((m,i)=>i).filter(i=>!only||r.sigs[i][0]!=null).sort((a,b)=>this.M[a][4]-this.M[b][4]||a-b);
 for(const i of idx){const m=this.M[i],[v,age,src,ok]=r.sigs[i];
  if(m[4]!==g){g=m[4];h+=`<tr><th colspan="6">${esc(LV_GRP[g]||'')}</th></tr>`}
  const stale=src==='c'&&age!=null&&age>3000,s=LV_SRC[src]||LV_SRC.o;
  h+=`<tr${stale?' class="muted"':''}><td>${esc(lvName(m))} <small class="mono muted">${esc(m[0])}</small></td><td class="num">${esc(lvVal(m,v))}</td><td class="num">${lvAge(age)}</td><td>${chipH(s[1],s[0])}</td><td>${ok?'✓':'<span class="muted" title="ще не перевірено в машині">?</span>'}</td><td><button class="btn sm" type="button" data-add="${i}" title="сповіщення на дисплей">🔔</button></td></tr>`}
 tb.innerHTML=h||'<tr><td colspan="6" class="muted">жодного значення: CAN A мовчить?</td></tr>';
 if(r.alertStats)$('#lvAl').textContent=r.alertStats.fired+' / '+r.alertStats.shown+' / '+r.alertStats.dropped},
drawAlerts(){const so=[['','—'],...this.M.map(m=>[m[0],lvName(m)])],co=this.C.map(c=>[c[0],c[1]]);
 $('#alT').innerHTML=this.A.map((a,i)=>`<tr data-r="${i}"><td>${sel('#sig',so,a[0],'data-str')}</td><td>${sel('#cond',co,a[1],'data-str')}</td><td>${num('#thr',a[2],'step="any" style="width:6em"')}</td><td>${num('#cd',a[3],'min="0" max="65535" style="width:5em"')}</td><td>${sw('#on',a[4])}</td></tr>`).join('')},
readAlerts(){return $$('#alT tr').map(tr=>{const g=f=>val($(`[data-f="${f}"]`,tr));return [g('sig')||'',g('cond')||'rises',g('thr')??0,g('cd')??0,!!g('on')]})},
async tick(){const a=$('[data-f="lvAuto"]');if(!a||!a.checked||this.n++%2)return;this.draw(await API.get('/api/map?live=1'))}};
