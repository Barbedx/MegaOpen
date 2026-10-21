/* ---------- Плата ---------- */
/* пояснення ролей BLE: текст сторінки за id ролі; нової ролі тут нема — лише назва */
const BLEHINT={off:'BLE не запускається, +40 кБ пам’яті. Музика й час — від ГУ по UART.',
 ams:'AMS: назва треку і керування плеєром. ANCS: сповіщення. CTS: годинник. Лише iPhone, один.',
 keyboard:'Плата стає Bluetooth-пультом: play, next, prev, гучність. Для Android або ГУ без AMS. Тексту немає.',
 both:'Одночасно iPhone (AMS, ANCS, CTS) і пульт HID для другого пристрою.'};
const bars=r=>{const n=r>-55?4:r>-67?3:r>-78?2:1;return `<span class="sig">${[1,2,3,4].map(i=>`<i class="${i<=n?'on':''}" style="height:${i*4}px"></i>`).join('')}</span>`};
/* усі налаштування з реєстру прошивки (CAT.settings): ті самі, що в меню на щитку;
   ті, що вже є на сторінці (data-k в `have`), не дублюються; reboot -> смуга перезавантаження */
const allSettings=(se,have)=>{let h='';for(const f of CAT.settings||[]){
 if(f.type==='group'){h+=`<div class="row muted"><b>${esc(f.label)}</b></div>`;continue}
 if(f.type==='table')continue;/* таблиці малює своя сторінка (сповіщення — «Авто — наживо») */
 if(!(f.id in se)||have.includes(`|${f.id}"`))continue;
 const v=se[f.id],k=(f.reboot?'rb:':'')+'/api/settings|'+f.id,ex=f.reboot?rbA(k,v,f.label):'';let c;
 if(f.ro)c=`<span class="mono">${esc(f.enum?((f.enum.find(o=>o.v===v)||{}).name||v):v)}${f.unit?' '+esc(f.unit):''}</span>`;
 else if(f.type==='bool')c=sw(k,v,ex);
 else if(f.type==='enum')c=sel(k,f.enum.map(o=>[o.v,o.name]),v,'data-str'+ex);
 else c=num(k,v,`min="${f.min}" max="${f.max}"${f.step?` step="${f.step}"`:''}${ex}`);
 h+=row(esc(f.label)+(f.unit&&!f.ro?', '+esc(f.unit):''),c,[f.volatile?'до перезапуску':'',f.menu?'є і в меню на щитку':''].filter(Boolean).join('; '))}
 return h};
SCR.board={title:'Плата',async render(m){
 const B=has('ble');
 const[st,se,lg]=await Promise.all([API.get('/api/status'),API.get('/api/settings'),getIf(has('logfs'),'/api/logs')]);
 const ssid2=se.ssid2||'';
 m.innerHTML=
 card('WiFi',row('Зараз','<span id="wNow" class="chip off">…</span>','<span id="wNowW"></span>')+
  '<div class="fr"><button class="btn pri" type="button" id="wScan">Знайти мережі</button></div><div id="wList"></div>'+
  `<div id="wForm">${row('Мережа',txt('rb:/api/wifi|ssid',se.ssid,'SSID','in','text',rbA('rb:',se.ssid,'WiFi',true)))}${row('Пароль',txt('rb:/api/wifi|pass','','без змін','in','password',rbA('rb:','','Пароль WiFi')))}
  ${row('Запасна мережа',txt('rb:/api/wifi|ssid2',ssid2,'напр. хотспот телефона','in','text',rbA('rb:',ssid2,'Запасна мережа',true)))}${row('Її пароль',txt('rb:/api/wifi|pass2','','без змін','in','password',rbA('rb:','','Пароль запасної')))}</div>`+
  row('Якщо жодної нема',`<span class="mono">${esc(st.apName||'')}</span>`,'точка 192.168.4.1','Піднімається сама, якщо за 15 с не вдалося підключитись або зв’язок зник на 30 с. Кожні 60 с пробує мережі знову, поки до точки ніхто не підключений.')+
  row('Пароль точки доступу',txt('rb:/api/settings|apPass','','без змін','in','password',rbA('rb:','','Пароль точки/OTA')+' minlength="8"'),'мін. 8 символів'),'cWifi')+
 (B?card('Bluetooth',row('Роль',sel('rb:/api/settings|bleMode',CAT.bleModes.map(b=>[b.id,b.name]),se.bleMode,'data-str'+rbA('rb:',se.bleMode,'Роль Bluetooth')),`<span id="bHint">${esc(BLEHINT[se.bleMode]||'')}</span>`)+
  `<div id="bLive"${se.bleMode==='off'?' hidden':''}>${row('Стан','<span id="bSt" class="chip off">…</span>','<span id="bName"></span>')}<div class="fr"><button class="btn dng" type="button" id="bForget">Забути телефон</button></div></div>`):'')+
 card('Оновлення прошивки','<div class="kv"><div><small>Прошивка</small><b>'+esc(se.fw)+'</b></div><div><small>Збірка</small><b>'+esc(se.build)+'</b></div></div>'+
  (has('ghota')?row('З GitHub','<span id="ghSt" class="chip off">…</span>','<span id="ghW"></span>','Остання стабільна версія з github.com/Barbedx/MegaOpen. Лише коли плата в мережі з інтернетом, лише з цієї кнопки й не під час руху. Образ перевіряється за SHA-256; якщо нова прошивка не стане здоровою за 3 хв, плата повернеться до попередньої.')+
   '<div class="fr"><button class="btn" type="button" id="ghChk">Перевірити</button><button class="btn pri" type="button" id="ghGo" hidden>Оновити</button><button class="btn pri" type="button" id="ghBoot" hidden>Перезавантажити</button></div><div class="pad" id="ghP" hidden><div class="bar big"><i id="ghBar"></i></div></div>'+row('','','або файлом .bin:'):'')+
  '<div class="fr"><input type="file" id="otaF" accept=".bin" class="in" style="flex:1"><button class="btn pri" type="button" id="otaGo">Оновити</button></div><div class="pad" id="otaP" hidden><div class="bar big"><i id="otaBar"></i></div></div>','cOta','custom')+
 (has('logfs')?card('Журнал запусків',row('','','Ті самі рядки, що йдуть у серійний порт, але на флеші: файл на кожен запуск, 8 останніх. Живий потік — Сервіс › Лог. Після зависання чи зникнення живлення останні ~16 кБ можуть не потрапити.')+`<div id="logs">${(lg.files||[]).map(f=>row(f.n?'Запуск −'+f.n:'Цей запуск',`<button class="btn sm" type="button" data-n="${f.n}">Скачати</button>`,fmtB(f.size))).join('')}</div>`,'','custom'):'')+
 '<div id="allSlot"></div>'+
 card('Інтерфейс',row('Режим розробника',`<input type="checkbox" class="sw" id="devSw" data-nosave${DEV?' checked':''}>`,'показує вкладку Сервіс'),'','custom')+
 card('Небезпечна зона','<div class="fr"><button class="btn" type="button" id="rbt">Перезавантажити</button><button class="btn dng" type="button" id="fac">Скинути до заводських</button></div>','','custom');
 $('#allSlot').outerHTML=fold('Усі налаштування',row('','','Ті самі значення, що в меню на щитку (утримати правий енкодер).')+allSettings(se,m.innerHTML),'cAll');
 saver($('#cWifi'),genericSave,'[data-k]');
 saver($('#cAll'),genericSave,'[data-k]');
 if(B){saver($('[data-k="rb:/api/settings|bleMode"]').closest('.card'),genericSave,'[data-k]');
 $('[data-k="rb:/api/settings|bleMode"]').addEventListener('change',e=>{$('#bHint').textContent=BLEHINT[e.target.value]||''});
 $('#bForget').onclick=e=>ask(e.target,'Забути всі пари? На iPhone теж: Bluetooth → MEGA → Забути.',()=>run(e.target,()=>API.post('/api/ble/unbond')).then(()=>this.tick()))}
 $('#devSw').onchange=e=>{DEV=e.target.checked;try{localStorage.setItem('mo-dev',DEV?'1':'0')}catch(x){}devUi()};
 $('#wScan').onclick=e=>run(e.target,async()=>{$('#wList').innerHTML='<div class="row muted">шукаю ~2 с…</div>';const r=await API.get('/api/wifi/scan');
  $('#wList').innerHTML=r.networks.sort((a,b)=>b.rssi-a.rssi).map(n=>`<button class="net" type="button" data-ssid="${esc(n.ssid)}">${bars(n.rssi)}<b>${esc(n.ssid)}</b><span class="muted">${n.secure?'пароль':'відкрита'} · ${n.rssi}</span></button>`).join('');return {note:r.count+' мереж'}});
 $('#wList').onclick=e=>{const b=e.target.closest('[data-ssid]');if(!b)return;const i=$('[data-k="rb:/api/wifi|ssid"]');i.value=b.dataset.ssid;i.dispatchEvent(new Event('input',{bubbles:true}));$('[data-k="rb:/api/wifi|pass"]').focus()};
 $('#otaGo').onclick=async e=>{const f=$('#otaF').files[0];const b=e.target;if(!f){sv(svOf(b),'bad','вибери .bin');return}b.disabled=true;$('#otaP').hidden=false;const s=svOf(b);
  const onP=p=>{$('#otaBar').style.width=p+'%';sv(s,'busy');s.textContent=p+'%'};
  try{await reboot(()=>upload(f,onP),undefined,'нова прошивка '+f.name)}catch(err){sv(s,'bad',errT(err))}b.disabled=false};
 if(has('ghota')){$('#ghChk').onclick=e=>run(e.target,async()=>{await API.post('/api/fw/check');await this.gh()},'перевіряю');
  $('#ghGo').onclick=e=>ask(e.target,'Завантажити '+this.ghTag+' з GitHub? Перезавантаження — потім, окремо.',()=>run(e.target,async()=>{await API.post('/api/fw/install');await this.gh()},'завантажую'));
  $('#ghBoot').onclick=e=>ask(e.target,'Перезавантажити в '+this.ghTag+' зараз?',()=>run(e.target,async()=>{await API.post('/api/fw/reboot');await this.gh()},'перезавантажую'))}
 if(has('logfs'))$('#logs').onclick=e=>{const b=e.target.closest('[data-n]');if(b)run(b,()=>download('/api/logs/file?n='+b.dataset.n,'boot-'+b.dataset.n+'.log'))};
 $('#rbt').onclick=e=>ask(e.target,'Перезавантажити плату?',()=>reboot().catch(err=>sv(svOf(e.target),'bad',errT(err))));
 $('#fac').onclick=e=>ask(e.target,'Стерти все, включно з WiFi. Плата підніме точку 192.168.4.1.',()=>reboot('/api/factory?confirm=factory',undefined,'заводські налаштування').catch(err=>sv(svOf(e.target),'bad',errT(err))),'factory');
 this.bleOff=!B||se.bleMode==='off';await this.tick();
},
/* GhOta: стан з /api/fw; phase -> чип, підпис, кнопка, смуга. rebooting -> та сама заставка, що й після файлу */
async gh(){const v=await API.get('/api/fw');if(!$('#ghSt'))return;this.ghTag=v.latest;
 const busy=v.phase==='checking'||v.phase==='downloading',pct=v.size?Math.floor(v.got/v.size*100):0;
 const[st,t,w]={idle:['off','не перевірено','ця '+v.fw],checking:['warn','перевіряю…',''],current:['ok','актуальна','остання '+v.latest+' · ця '+v.fw],
  available:['warn','є '+v.latest,'ця '+v.fw+' · '+fmtB(v.size)],downloading:['warn','завантажую '+pct+'%',fmtB(v.got)+' з '+fmtB(v.size)],
  written:['warn','записано '+v.latest,'SHA-256 збігся · перезавантажиться сама, коли авто стоїть, або кнопкою'],
  rebooting:['ok','перезавантаження',v.latest+' записано, SHA-256 збігся'],failed:['bad','помилка',v.err||'']}[v.phase]||['off',v.phase,''];
 setChip('#ghSt',st,t);$('#ghW').textContent=w;$('#ghChk').disabled=busy||v.phase==='written'||v.phase==='rebooting';
 $('#ghBoot').hidden=v.phase!=='written';
 const go=$('#ghGo');go.hidden=v.phase!=='available';go.textContent='Оновити до '+v.latest;
 $('#ghP').hidden=v.phase!=='downloading';$('#ghBar').style.width=pct+'%';
 if(v.phase==='rebooting'&&!REBOOTING)reboot(async()=>{},undefined,'оновлення '+v.latest+' з GitHub')},
async tick(){const[st,ble]=await Promise.all([API.get('/api/status'),getIf(has('display')&&has('ble')&&!this.bleOff,'/api/ble')]);if(!$('#wNow'))return;
 if(has('ghota'))await this.gh();
 wifiChip($('#wNow'),$('#wNowW'),st,'мереж не знайдено');
 if(this.bleOff||!ble)return;
 setChip('#bSt',ble.connected?'ok':ble.bonded?'warn':'off',ble.connected?'на зв’язку':ble.bonded?'спарено, не на зв’язку':'немає пари');
 $('#bName').textContent=ble.connected?ble.address||'':'видно як MEGA: iPhone → Параметри → Bluetooth';$('#bForget').disabled=!ble.bonded}};
