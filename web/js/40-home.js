/* ---------- Огляд ---------- */
/* стан ланцюга з /api/health — s.<state>; пояснення за стабільним code — w.<code> (web/i18n).
   Прошивка шле лише code; коду, якого словник ще не знає, показуємо сам код. Код «працює»
   без слів (panel_ok…) — порожньо */
const stateT=s=>tHas('s.'+s)?t('s.'+s):s;
const whyT=c=>tHas('w.'+c.code)?t('w.'+c.code):c.state==='ok'?'':c.code||'';
SCR.home={title:'nav.home',async render(m){
 const D=has('display');
 m.innerHTML=card(t('h.status'),
  (D?`<div class="row"><div class="l"><b>${t('h.panel')}</b>${hintBtn(t('h.panelDet'))}<small id="hPanelW"></small><small class="hint" id="hPanelD" hidden></small></div><div class="c"><span id="hPanel" class="chip off">…</span></div></div>`+
  `<div class="fr" id="hResyncRow" hidden><button class="btn pri wide" id="hResync" type="button">${t('h.resync')}</button></div>`:'')+
  row(t('h.hu'),'<span id="hHu" class="chip off">…</span>','<span id="hHuW"></span>')+
  (has('can2')?row(t('h.can'),'<span id="hCan" class="chip off">…</span>','<span id="hCanW"></span>'):'')+
  (has('knobs')?row(t('h.knobs'),'<span id="hKnob" class="chip off">…</span>','<span id="hKnobW"></span>')+
  row(t('h.lights'),'<span id="hLed" class="chip off">…</span>','<span id="hLedW"></span>'):'')+
  (D?row(t('h.music'),'<span id="hSrc" class="chip off">…</span>','<span id="hTrack"></span><span class="bar" style="display:block"><i id="hProg"></i></span>')+
  row(t('h.clock'),'<span id="hClk" class="chip off">…</span>','<span id="hClkW"></span>'):'')+
  (has('ble')?`<div id="hPhoneRow">${row(t('h.phone'),'<span id="hPhone" class="chip off">…</span>','<span id="hPhoneW"></span>')}</div>`:'')+
  row(t('h.keys'),'<span id="hKeys" class="chips"></span>')+
  (has('capture')?row(t('h.rec'),'<button class="btn rec" id="hRec" type="button">…</button>','<span id="hRecW"></span>'):'')+
  row(t('h.wifi'),'<span id="hWifi" class="chip off">…</span>','<span id="hWifiW"></span>'),'','full');
 if(D)$('#hResync').onclick=e=>run(e.target,()=>API.post('/api/display/resync'));
 if(has('capture'))$('#hRec').onclick=e=>run(e.target,()=>API.post(e.target.classList.contains('on')?'/api/cap/stop':'/api/cap/start'),'').then(()=>this.tick());
 await this.tick();
},async tick(fromLoop){
 /* з циклу (99-main) — раз на 2 с: шість запитів щосекунди забагато для плати, на якій ще й Bluetooth.
    Оновлення одразу після дії (запис, мітка) не пропускається ніколи */
 if(fromLoop){this.n=(this.n||0)+1;if(this.n%2===0)return}
 const D=has('display');
 const[h,st,me,c2,cp,di]=await Promise.all([API.get('/api/health'),API.get('/api/status'),getIf(D,'/api/media'),getIf(has('can2'),'/api/can2'),getIf(has('capture'),'/api/cap'),getIf(D,'/api/display')]);
 if(!$('#hHu'))return;
 /* ланцюга може не бути (функцію не зібрано): тоді рядок каже «нема даних» */
 const ch=id=>(h.chains||[]).find(c=>c.id===id)||{state:'off',code:'no_data'};
 const chip=(el,c,txt)=>setChip(el,CH[c.state]||'off',txt||stateT(c.state));
 if(D){const p=ch('panel');chip('#hPanel',p,di.registered?t('h.registered'):'');$('#hResyncRow').hidden=p.state==='ok';
  $('#hPanelW').textContent=p.state==='ok'?nameOf(CAT.panels,di.type):whyT(p);
  const dt=t('h.panelDt',di.type,di.funcReg?t('h.byBoard'):t('h.byRadio'),di.phase||DASH,di.sessionsLost||0,di.lastLoss||DASH);
  $('#hPanelD').textContent=dt;$('#hPanel').title=dt;
  const md=ch('media'),src=enumOf('mediaSrc',me.sourceActive);chip('#hSrc',md,src&&me.sourceActive!=='none'?src.name:'');
  const tt=me.srcTitle||me.title;$('#hTrack').textContent=tt?[tt,me.srcArtist||me.artist,me.srcAlbum].filter(Boolean).join(' — ')+(me.hasTransport?' · '+fmtT(me.elapsed)+' / '+fmtT(me.duration):''):whyT(md)||t('h.nothing');
  $('#hProg').parentElement.hidden=!me.hasTransport;$('#hProg').style.width=me.duration?(me.elapsed/me.duration*100)+'%':'0';
  const ck=ch('clock');chip('#hClk',ck,ck.state==='ok'?ck.hhmm:'');$('#hClkW').textContent=whyT(ck)}
 /* UART: що саме приходить каже, де шукати — дріт, швидкість чи нічого */
 const hu=ch('hu'),hn=hu.rxFrames||0,hj=hu.junk||0;
 if(hu.state!=='off'&&!hn&&hj){setChip('#hHu','bad',t('h.junk'));$('#hHuW').textContent=t('h.junkW',fmtN(hj))}
 else if(hu.state!=='off'&&!hn){chip('#hHu',hu,t('h.silent'));$('#hHuW').textContent=t('h.silentW',fmtN(st.framesSent||0))}
 else{chip('#hHu',hu,hu.code==='hu_dialect_vw'?t('h.vw'):'');$('#hHuW').textContent=[whyT(hu),t('h.huRx',fmtN(hn)),hu.notSupported?t('h.notSup',fmtN(hu.notSupported)):'',hu.badCs?t('h.badCs',fmtN(hu.badCs)):'',t('h.sent',fmtN(st.framesSent||0))].filter(Boolean).join(' · ')}
 if(has('knobs')){const k=ch('knobs'),l=ch('lights');
  chip('#hKnob',k,k.state==='ok'?'0x'+(k.addr||0).toString(16).toUpperCase():'');
  $('#hKnobW').textContent=k.state==='ok'?[t('h.knobsW',k.left,k.right,k.clicks),k.i2cErrors?t('h.i2c',fmtN(k.i2cErrors)):''].filter(Boolean).join(' · '):whyT(k);
  chip('#hLed',l,l.state==='ok'?t('h.leds',l.leds):'');$('#hLedW').textContent=whyT(l)}
 if(has('ble')){const ph=ch('phone');$('#hPhoneRow').hidden=h.bleMode==='off';chip('#hPhone',ph);$('#hPhoneW').textContent=[whyT(ph),nameOf(CAT.bleModes,h.bleMode)].filter(Boolean).join(' · ')}
 const kd=ch('keys').dest||{};$('#hKeys').innerHTML=CAT.dests.filter(d=>d.built&&kd[d.id]).map(d=>chipH(kd[d.id].live?'ok':'off',d.name)).join('');
 if(c2){canChip($('#hCan'),c2);$('#hCanW').textContent=c2.fps?'':c2.autobaud!=='manual'?t('h.canNone'):t('h.canRate')}
 if(cp){recBtn($('#hRec'),cp);$('#hRecW').textContent=cp.recording?t('h.recW',fmtN(cp.frames),cp.markers):cp.error?errLine(cp.error):''}
 wifiChip($('#hWifi'),$('#hWifiW'),st,t('h.noWifi'));
}};
