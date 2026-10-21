/* ---------- Огляд ---------- */
/* слово для стану ланцюга з /api/health */
const STATE_T={ok:'працює',idle:'чекає',off:'вимкнено',broken:'не працює',muted:'приглушено'};
/* пояснення за стабільним code ланцюга; нового коду тут нема — показуємо англійське why прошивки */
const WHY_T={panel_off:'панелі нема в цій збірці',panel_unregistered:'реєстрацію втрачено: панель не завершує рукостискання',
 media_off:'екран музики вимкнено',media_no_panel:'панель не зареєстрована, нема де малювати',media_no_phone:'телефон не підключено',media_no_track:'телефон підключено, трека ще нема',
 clock_off:'автоматичний час вимкнено',clock_no_phone:'нема телефона, а час іде лише з нього (CTS)',clock_waiting:'телефон підключено, часу ще не надіслав',
 keys_none:'жодного натискання з запуску',ancs_off:'сповіщень нема в цій збірці',ancs_no_phone:'телефон не підключено',ancs_waiting:'підключено, сповіщень ще не було',
 hu_off:'UART до ГУ вимкнено',hu_silent:'від ГУ жодного кадру з запуску',hu_quiet:'ГУ замовкло',hu_seen:'від ГУ були кадри, але ця збірка не знає, як давно',
 phone_ble_off:'Bluetooth вимкнено',phone_advertising:'видно, але не підключено',
 knobs_no_expander:'PCF8574 не знайдено: кабель QWIIC 2, живлення перехідника',lights_off:'вимкнено: Кнопки → Підсвітка',lights_fail:'драйвер стрічки не запустився'};
const whyT=c=>WHY_T[c.code]||c.why||'';
SCR.home={title:'Огляд',async render(m){
 const D=has('display');
 m.innerHTML=card('Стан',
  (D?`<div class="row"><div class="l"><b>Панель</b>${hintBtn('Деталі реєстрації')}<small id="hPanelW"></small><small class="hint" id="hPanelD" hidden></small></div><div class="c"><span id="hPanel" class="chip off">…</span></div></div>`+
  '<div class="fr" id="hResyncRow" hidden><button class="btn pri wide" id="hResync" type="button">Перереєструвати панель</button></div>':'')+
  row('ГУ по UART','<span id="hHu" class="chip off">…</span>','<span id="hHuW"></span>')+
  (has('can2')?row('CAN A (авто)','<span id="hCan" class="chip off">…</span>','<span id="hCanW"></span>'):'')+
  (has('knobs')?row('Ручки','<span id="hKnob" class="chip off">…</span>','<span id="hKnobW"></span>')+
  row('Підсвітка','<span id="hLed" class="chip off">…</span>','<span id="hLedW"></span>'):'')+
  (D?row('Музика','<span id="hSrc" class="chip off">…</span>','<span id="hTrack"></span><span class="bar" style="display:block"><i id="hProg"></i></span>')+
  row('Годинник','<span id="hClk" class="chip off">…</span>','<span id="hClkW"></span>'):'')+
  (has('ble')?`<div id="hPhoneRow">${row('Телефон','<span id="hPhone" class="chip off">…</span>','<span id="hPhoneW"></span>')}</div>`:'')+
  row('Кнопки → виходи','<span id="hKeys" class="chips"></span>')+
  (has('capture')?row('Запис CAN A','<button class="btn rec" id="hRec" type="button">…</button>','<span id="hRecW"></span>'):'')+
  row('WiFi','<span id="hWifi" class="chip off">…</span>','<span id="hWifiW"></span>'),'','full');
 if(D)$('#hResync').onclick=e=>run(e.target,()=>API.post('/api/display/resync'));
 if(has('capture'))$('#hRec').onclick=e=>run(e.target,()=>API.post(e.target.classList.contains('on')?'/api/cap/stop':'/api/cap/start'),'').then(()=>this.tick());
 await this.tick();
},async tick(){
 /* раз на 2 с: шість запитів щосекунди — забагато для плати, на якій ще й Bluetooth */
 this.n=(this.n||0)+1;if(this.n%2===0)return;
 const D=has('display');
 const[h,st,me,c2,cp,di]=await Promise.all([API.get('/api/health'),API.get('/api/status'),getIf(D,'/api/media'),getIf(has('can2'),'/api/can2'),getIf(has('capture'),'/api/cap'),getIf(D,'/api/display')]);
 if(!$('#hHu'))return;
 /* ланцюга може не бути (функцію не зібрано): тоді рядок каже «нема даних» */
 const ch=id=>(h.chains||[]).find(c=>c.id===id)||{state:'off',why:'нема даних'};
 const chip=(el,c,t)=>setChip(el,CH[c.state]||'off',t||STATE_T[c.state]||c.state);
 if(D){const p=ch('panel');chip('#hPanel',p,di.registered?'зареєстрована':'');$('#hResyncRow').hidden=p.state==='ok';
  $('#hPanelW').textContent=p.state==='ok'?nameOf(CAT.panels,di.type):whyT(p);
  const dt=`Тип: ${di.type} · реєстрація функцій: ${di.funcReg?'плата':'штатне радіо'} · фаза: ${di.phase||DASH} · втрат сесії: ${di.sessionsLost||0} · остання: ${di.lastLoss||DASH}`;
  $('#hPanelD').textContent=dt;$('#hPanel').title=dt;
  const md=ch('media'),src=enumOf('mediaSrc',me.sourceActive);chip('#hSrc',md,src&&me.sourceActive!=='none'?src.name:'');
  const tt=me.srcTitle||me.title;$('#hTrack').textContent=tt?[tt,me.srcArtist||me.artist,me.srcAlbum].filter(Boolean).join(' — ')+(me.hasTransport?' · '+fmtT(me.elapsed)+' / '+fmtT(me.duration):''):whyT(md)||'нічого не грає';
  $('#hProg').parentElement.hidden=!me.hasTransport;$('#hProg').style.width=me.duration?(me.elapsed/me.duration*100)+'%':'0';
  const ck=ch('clock');chip('#hClk',ck,ck.state==='ok'?ck.hhmm:'');$('#hClkW').textContent=whyT(ck)}
 /* UART: що саме приходить каже, де шукати — дріт, швидкість чи нічого */
 const hu=ch('hu'),hn=hu.rxFrames||0,hj=hu.junk||0;
 if(hu.state!=='off'&&!hn&&hj){setChip('#hHu','bad','сміття');$('#hHuW').textContent=`байти є (${fmtN(hj)}), кадрів нема: дроти живі, не та швидкість чи протокол ГУ`}
 else if(hu.state!=='off'&&!hn){chip('#hHu',hu,'тиша');$('#hHuW').textContent='від ГУ нічого: дріт C8 (жовтий, RX), маса; або поміняй C7/C8 · відправлено '+fmtN(st.framesSent||0)}
 else{chip('#hHu',hu);$('#hHuW').textContent=[whyT(hu),'кадрів від ГУ '+fmtN(hn),hu.badCs?'збій суми '+fmtN(hu.badCs):'','відправлено '+fmtN(st.framesSent||0)].filter(Boolean).join(' · ')}
 if(has('knobs')){const k=ch('knobs'),l=ch('lights');
  chip('#hKnob',k,k.state==='ok'?'0x'+(k.addr||0).toString(16).toUpperCase():'');
  $('#hKnobW').textContent=k.state==='ok'?`ліва ${k.left} · права ${k.right} · клацань ${k.clicks}`+(k.i2cErrors?` · збоїв I2C ${fmtN(k.i2cErrors)}`:'')+' — крути, числа мають мінятись':whyT(k);
  chip('#hLed',l,l.state==='ok'?l.leds+' LED':'');$('#hLedW').textContent=l.state==='ok'?'кільця мають світитись і реагувати на ручки':whyT(l)}
 if(has('ble')){const ph=ch('phone');$('#hPhoneRow').hidden=h.bleMode==='off';chip('#hPhone',ph);$('#hPhoneW').textContent=[whyT(ph),nameOf(CAT.bleModes,h.bleMode)].filter(Boolean).join(' · ')}
 const kd=ch('keys').dest||{};$('#hKeys').innerHTML=CAT.dests.filter(d=>d.built&&kd[d.id]).map(d=>chipH(kd[d.id].live?'ok':'off',d.name)).join('');
 if(c2){canChip($('#hCan'),c2);$('#hCanW').textContent=c2.fps?'':c2.autobaud!=='manual'?'кадрів нема: шина спить або не підключена':'перевір швидкість'}
 if(cp){recBtn($('#hRec'),cp);$('#hRecW').textContent=cp.recording?`${fmtN(cp.frames)} кадрів · міток ${cp.markers}`:cp.error||''}
 wifiChip($('#hWifi'),$('#hWifiW'),st,'жодної мережі не знайдено');
}};
