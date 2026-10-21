/* ---------- Плата ---------- */
/* пояснення ролі BLE — bd.ble.<id>; нової ролі в словнику нема — порожньо */
const bleHint=id=>tHas('bd.ble.'+id)?t('bd.ble.'+id):'';
const bars=r=>{const n=r>-55?4:r>-67?3:r>-78?2:1;return `<span class="sig">${[1,2,3,4].map(i=>`<i class="${i<=n?'on':''}" style="height:${i*4}px"></i>`).join('')}</span>`};
/* усі налаштування з реєстру прошивки (CAT.settings): ті самі, що в меню на щитку;
   ті, що вже є на сторінці (data-k в `have`), не дублюються; reboot -> смуга перезавантаження.
   Одиниця — уже в назві («Брязкіт, мс»), тож поруч не дописується; лише значення «лише читати» несе її */
const allSettings=(se,have)=>{let h='';for(const f of CAT.settings||[]){
 if(f.type==='group'){h+=`<div class="row muted"><b>${esc(f.label)}</b></div>`;continue}
 if(f.type==='table')continue;/* таблиці малює своя сторінка (сповіщення — «Авто — наживо») */
 if(!(f.id in se)||have.includes(`|${f.id}"`))continue;
 const v=se[f.id],k=(f.reboot?'rb:':'')+'/api/settings|'+f.id,ex=f.reboot?rbA(k,v,f.label):'';let c;
 if(f.ro)c=`<span class="mono">${esc(f.enum?((f.enum.find(o=>o.v===v)||{}).name||v):v)}${f.unit?' '+esc(unitT(f.unit)):''}</span>`;
 else if(f.type==='bool')c=sw(k,v,ex);
 else if(f.type==='enum')c=sel(k,f.enum.map(o=>[o.v,o.name]),v,'data-str'+ex);
 else if(f.type==='str')c=txt(k,v,'','in','text',`maxlength="${f.max}"${ex}`);
 else c=num(k,v,`min="${f.min}" max="${f.max}"${f.step?` step="${f.step}"`:''}${ex}`);
 h+=row(esc(f.label),c,f.volatile?t('bd.volatile'):'')}
 return h};
SCR.board={title:'nav.board',async render(m){
 const B=has('ble');
 const[st,se,lg]=await Promise.all([API.get('/api/status'),API.get('/api/settings'),getIf(has('logfs'),'/api/logs')]);
 const ssid2=se.ssid2||'',un=t('bd.unchanged');
 m.innerHTML=
 card(t('bd.wifi'),row(t('bd.now'),'<span id="wNow" class="chip off">…</span>','<span id="wNowW"></span>')+
  `<div class="fr"><button class="btn pri" type="button" id="wScan">${t('bd.scan')}</button></div><div id="wList"></div>`+
  `<div id="wForm">${row(t('bd.net'),txt('rb:/api/wifi|ssid',se.ssid,'SSID','in','text',rbA('rb:',se.ssid,'WiFi',true)))}${row(t('bd.pass'),txt('rb:/api/wifi|pass','',un,'in','password',rbA('rb:','',t('bd.passLbl'))))}
  ${row(t('bd.net2'),txt('rb:/api/wifi|ssid2',ssid2,t('bd.net2Ph'),'in','text',rbA('rb:',ssid2,t('bd.net2'),true)))}${row(t('bd.pass2'),txt('rb:/api/wifi|pass2','',un,'in','password',rbA('rb:','',t('bd.pass2Lbl'))))}</div>`+
  row(t('bd.noNet'),`<span class="mono">${esc(st.apName||'')}</span>`,t('bd.apAt'),t('bd.apH'))+
  row(t('bd.apPass'),txt('rb:/api/settings|apPass','',un,'in','password',rbA('rb:','',t('bd.apPassLbl'))+' minlength="8"'),t('bd.min8')),'cWifi')+
 (B?card('Bluetooth',row(t('bd.role'),sel('rb:/api/settings|bleMode',CAT.bleModes.map(b=>[b.id,b.name]),se.bleMode,'data-str'+rbA('rb:',se.bleMode,t('bd.roleLbl'))),`<span id="bHint">${esc(bleHint(se.bleMode))}</span>`)+
  `<div id="bLive"${se.bleMode==='off'?' hidden':''}>${row(t('bd.state'),'<span id="bSt" class="chip off">…</span>','<span id="bName"></span>')}<div class="fr"><button class="btn dng" type="button" id="bForget">${t('bd.forget')}</button></div></div>`):'')+
 card(t('bd.update'),`<div class="kv"><div><small>${t('bd.fw')}</small><b>${esc(se.fw)}</b></div><div><small>${t('bd.build')}</small><b>${esc(se.build)}</b></div></div>`+
  (has('ghota')?row(t('bd.gh'),'<span id="ghSt" class="chip off">…</span>','<span id="ghW"></span>',t('bd.ghH'))+
   `<div class="fr"><button class="btn" type="button" id="ghChk">${t('bd.check')}</button><button class="btn pri" type="button" id="ghGo" hidden>${t('bd.updateBtn')}</button><button class="btn pri" type="button" id="ghBoot" hidden>${t('bd.restart')}</button></div><div class="pad" id="ghP" hidden><div class="bar big"><i id="ghBar"></i></div></div>`+row('','',t('bd.orFile')):'')+
  `<div class="fr"><input type="file" id="otaF" accept=".bin" class="in" style="flex:1"><button class="btn pri" type="button" id="otaGo">${t('bd.updateBtn')}</button></div><div class="pad" id="otaP" hidden><div class="bar big"><i id="otaBar"></i></div></div>`,'cOta','custom')+
 (has('logfs')?card(t('bd.log'),row('','',t('bd.logH'))+`<div id="logs">${(lg.files||[]).map(f=>row(f.n?t('bd.bootN',f.n):t('bd.bootThis'),`<button class="btn sm" type="button" data-n="${f.n}">${t('bd.download')}</button>`,fmtB(f.size))).join('')}</div>`,'','custom'):'')+
 '<div id="allSlot"></div>'+
 card(t('bd.toFile'),row('','',t('bd.toFileH'))+
  `<div class="fr"><button class="btn" type="button" id="cfgSave">${t('bd.saveFile')}</button><input type="file" id="cfgF" accept=".json,application/json" class="in" style="flex:1" aria-label="${esc(t('bd.fileLbl'))}"></div><div id="cfgPrev"></div>`,'cCfg','custom')+
 card(t('bd.ui'),row(t('bd.dev'),`<input type="checkbox" class="sw" id="devSw" data-nosave${DEV?' checked':''}>`,t('bd.devH')),'','custom')+
 card(t('bd.danger'),`<div class="fr"><button class="btn" type="button" id="rbt">${t('bd.restart')}</button><button class="btn dng" type="button" id="fac">${t('bd.factory')}</button></div>`,'','custom');
 $('#allSlot').outerHTML=fold(t('bd.all'),row('','',t('bd.allH'))+allSettings(se,m.innerHTML),'cAll');
 saver($('#cWifi'),genericSave,'[data-k]');
 saver($('#cAll'),genericSave,'[data-k]');
 if(B){saver($('[data-k="rb:/api/settings|bleMode"]').closest('.card'),genericSave,'[data-k]');
 $('[data-k="rb:/api/settings|bleMode"]').addEventListener('change',e=>{$('#bHint').textContent=bleHint(e.target.value)});
 $('#bForget').onclick=e=>ask(e.target,t('bd.forgetAsk'),()=>run(e.target,()=>API.post('/api/ble/unbond')).then(()=>this.tick()))}
 $('#devSw').onchange=e=>{DEV=e.target.checked;try{localStorage.setItem('mo-dev',DEV?'1':'0')}catch(x){}devUi();API.post('/api/settings',{devMode:DEV}).catch(()=>{})};
 $('#wScan').onclick=e=>run(e.target,async()=>{$('#wList').innerHTML=`<div class="row muted">${t('bd.scanning')}</div>`;const r=await API.post('/api/wifi/scan');
  $('#wList').innerHTML=r.networks.sort((a,b)=>b.rssi-a.rssi).map(n=>`<button class="net" type="button" data-ssid="${esc(n.ssid)}">${bars(n.rssi)}<b>${esc(n.ssid)}</b><span class="muted">${n.secure?t('bd.secure'):t('bd.open')} · ${n.rssi}</span></button>`).join('');return {note:t('bd.nNets',r.count)}});
 $('#wList').onclick=e=>{const b=e.target.closest('[data-ssid]');if(!b)return;const i=$('[data-k="rb:/api/wifi|ssid"]');i.value=b.dataset.ssid;i.dispatchEvent(new Event('input',{bubbles:true}));$('[data-k="rb:/api/wifi|pass"]').focus()};
 $('#otaGo').onclick=async e=>{const f=$('#otaF').files[0];const b=e.target;if(!f){sv(svOf(b),'bad',t('bd.pickBin'));return}b.disabled=true;$('#otaP').hidden=false;const s=svOf(b);
  const onP=p=>{$('#otaBar').style.width=p+'%';sv(s,'busy');s.textContent=p+'%'};
  try{await reboot(()=>upload(f,onP),undefined,t('bd.newFw',f.name))}catch(err){sv(s,'bad',errT(err))}b.disabled=false};
 if(has('ghota')){$('#ghChk').onclick=e=>run(e.target,async()=>{await API.post('/api/fw/check');await this.gh()},t('bd.checking'));
  $('#ghGo').onclick=e=>ask(e.target,t('bd.ghAsk',this.ghTag),()=>run(e.target,async()=>{await API.post('/api/fw/install');await this.gh()},t('bd.downloading')));
  $('#ghBoot').onclick=e=>ask(e.target,t('bd.bootAsk',this.ghTag),()=>run(e.target,async()=>{await API.post('/api/fw/reboot');await this.gh()},t('bd.restarting')))}
 if(has('logfs'))$('#logs').onclick=e=>{const b=e.target.closest('[data-n]');if(b)run(b,()=>download('/api/logs/file?n='+b.dataset.n,'boot-'+b.dataset.n+'.log'))};
 this.fw=se.fw;this.cfgBind();
 $('#rbt').onclick=e=>ask(e.target,t('bd.rebootAsk'),()=>reboot().catch(err=>sv(svOf(e.target),'bad',errT(err))));
 $('#fac').onclick=e=>ask(e.target,t('bd.factoryAsk'),()=>reboot('/api/factory?confirm=factory',undefined,t('bd.factoryWhy')).catch(err=>sv(svOf(e.target),'bad',errT(err))),'factory');
 this.bleOff=!B||se.bleMode==='off';await this.tick();
},
/* файл налаштувань: GET /api/config у файл; з файлу — /api/config?dry=1 (що зміниться, або всі помилки), потім POST */
cfgBind(){
 $('#cfgSave').onclick=e=>run(e.target,async()=>{const c=await API.get('/api/config');const d=new Date().toISOString().slice(0,10);
  return {note:saveText('megaopen-settings-'+d+'.json',JSON.stringify(c,null,1))}});
 const prev=$('#cfgPrev'),lbl=id=>{const f=(CAT.settings||[]).find(x=>x.id===id);return f&&f.label?f.label:id};
 $('#cfgF').onchange=async e=>{const f=e.target.files[0];prev.innerHTML='';if(!f)return;let doc;
  try{doc=JSON.parse(await f.text())}catch(x){sv(svOf(e.target),'bad',t('bd.notJson'));return}
  sv(svOf(e.target),'busy');let p;
  try{p=await API.post('/api/config?dry=1',doc)}catch(x){sv(svOf(e.target),'bad',t('bd.fileBad'));
   prev.innerHTML=row(t('bd.nothingChanged'),'',errLines(x.message).map(esc).join('<br>'));return}
  sv(svOf(e.target),'ok',t('bd.checked'));
  const ch=p.changes||[],fw=p.fileFw&&p.fileFw!==this.fw?row(t('bd.otherFw'),chipH('warn',p.fileFw),t('bd.otherFwH')):'';
  prev.innerHTML=fw+row(ch.length?t('bd.willChange',ch.length):t('bd.noChange'),p.reboot?chipH('warn',t('bd.needRestart')):'',ch.map(lbl).map(esc).join(' · '))+
   (p.knobs?row(t('bd.knobs'),'',t('bd.knobsFile')+((p.clamped||[]).length?t('bd.clamped',p.clamped.map(esc).join(', ')):'')):'')+
   ((p.skipped||[]).length?row(t('bd.skipped'),chipH('off',String(p.skipped.length)),p.skipped.map(lbl).map(esc).join(' · ')):'')+
   (p.unknown?row(t('bd.unknown'),chipH('off',String(p.unknown)),t('bd.unknownH')):'')+
   `<div class="fr"><button class="btn pri" type="button" id="cfgGo">${t('bd.apply')}</button><button class="btn" type="button" id="cfgNo">${t('bd.cancel')}</button></div>`;
  $('#cfgNo').onclick=()=>{prev.innerHTML='';e.target.value=''};
  $('#cfgGo').onclick=b=>run(b.target,async()=>{const r=await API.post('/api/config',doc);prev.innerHTML='';e.target.value='';
   const part=(r&&r.partial)||[];
   if(part.length)prev.innerHTML=row(t('bd.partial'),chipH('warn',String(part.length)),part.map(errLine).map(esc).join('<br>'));
   if(r&&r.pendingReboot){prev.innerHTML+=row(t('bd.applied'),`<button class="btn" type="button" id="cfgRb">${t('bd.restart')}</button>`,t('bd.appliedH'));$('#cfgRb').onclick=x=>reboot().catch(err=>sv(svOf(x.target),'bad',errT(err)))}
   return {note:part.length?t('bd.notePartial'):t('bd.noteApplied')}})}},
/* GhOta: стан з /api/fw; phase -> чип, підпис, кнопка, смуга. rebooting -> та сама заставка, що й після файлу */
async gh(){const v=await API.get('/api/fw');if(!$('#ghSt'))return;this.ghTag=v.latest;
 const busy=v.phase==='checking'||v.phase==='downloading',pct=v.size?Math.floor(v.got/v.size*100):0;
 const[st,tx,w]={idle:['off',t('bd.gh.idle'),t('bd.gh.this',v.fw)],checking:['warn',t('bd.gh.checking'),''],current:['ok',t('bd.gh.current'),t('bd.gh.latestThis',v.latest,v.fw)],
  available:['warn',t('bd.gh.available',v.latest),t('bd.gh.this',v.fw)+' · '+fmtB(v.size)],downloading:['warn',t('bd.gh.dl',pct),t('bd.gh.of',fmtB(v.got),fmtB(v.size))],
  written:['warn',t('bd.gh.written',v.latest),t('bd.gh.writtenW')],
  rebooting:['ok',t('bd.gh.rebooting'),t('bd.gh.rebootingW',v.latest)],failed:['bad',t('bd.gh.failed'),v.err||'']}[v.phase]||['off',v.phase,''];
 setChip('#ghSt',st,tx);$('#ghW').textContent=w;$('#ghChk').disabled=busy||v.phase==='written'||v.phase==='rebooting';
 $('#ghBoot').hidden=v.phase!=='written';
 const go=$('#ghGo');go.hidden=v.phase!=='available';go.textContent=t('bd.gh.updateTo',v.latest);
 $('#ghP').hidden=v.phase!=='downloading';$('#ghBar').style.width=pct+'%';
 if(v.phase==='rebooting'&&!REBOOTING)reboot(async()=>{},undefined,t('bd.gh.from',v.latest))},
async tick(){const[st,ble]=await Promise.all([API.get('/api/status'),getIf(has('display')&&has('ble')&&!this.bleOff,'/api/ble')]);if(!$('#wNow'))return;
 if(has('ghota'))await this.gh();
 wifiChip($('#wNow'),$('#wNowW'),st,t('bd.noNets'));
 if(this.bleOff||!ble)return;
 setChip('#bSt',ble.connected?'ok':ble.bonded?'warn':'off',ble.connected?t('bd.bt.connected'):ble.bonded?t('bd.bt.bonded'):t('bd.bt.none'));
 $('#bName').textContent=ble.connected?ble.address||'':t('bd.bt.visible');$('#bForget').disabled=!ble.bonded}};
