/* ---------- Сервіс ---------- */
/* поле консолі з каталогу — той самий рендер, що й параметри розкладки (fieldCtrl) */
const cField=f=>`<label>${fieldLabel(f)}${fieldCtrl(f,'#'+f.n)}</label>`;
/* межі числа з реєстру налаштувань (CAT.settings): ті самі, що перевіряє прошивка */
const regOf=id=>(CAT.settings||[]).find(x=>x.id===id)||{};
const regRange=id=>{const f=regOf(id);return f.type==='int'?`min="${f.min}" max="${f.max}"${f.step?` step="${f.step}"`:''}`:''};
const regOpts=id=>(regOf(id).enum||[]).map(o=>[o.v,o.name]);
SCR.service={title:'nav.service',async render(m){
 const D=has('display'),V=has('vehicle'),CP=has('capture');
 const[me,msgs,can,se]=await Promise.all([getIf(D,'/api/media'),API.get('/api/messages'),getIf(V,'/api/can'),API.get('/api/settings')]);
 /* режим розробника живе в браузері; плата знає його лише до перезапуску (devMode) */
 if(DEV&&!se.devMode)API.post('/api/settings',{devMode:true}).catch(()=>{});
 const MS=msgs.messages||[];this.n=0;this.paused=false;this.lines=[];this.tagF='all';this.since=0;this.busy=false;this.boot=undefined;this.drop=undefined;
 const PK=CAT.panelKeys.filter(k=>k.src==='wheel'),con=()=>CAT.console.find(c=>c.cmd===$('#cK').value)||{name:'',fields:[]};
 m.innerHTML=
 card(t('sv.log'),`<div class="pad"><div class="seg wrap" id="lgTag" data-v="all">${['all',...CAT.logTags].map(g=>`<button type="button" class="${g==='all'?'on':''}" data-val="${esc(g)}">${g==='all'?t('sv.all'):esc(g)}</button>`).join('')}</div></div>`+
  row(t('sv.level'),sel('/api/settings|logLevel',regOpts('logLevel'),se.logLevel,'data-str'),t('bd.volatile'))+
  `<div class="fr"><button class="btn" type="button" id="lgP">${t('sv.pause')}</button><button class="btn" type="button" id="lgC">${t('sv.clear')}</button><button class="btn" type="button" id="lgD">${t('bd.download')}</button></div><div class="log" id="lg"></div>`,'','full')+
 (D?card(t('sv.console'),row(t('sv.power'),`<button class="btn sm" type="button" data-pw="1">${t('sv.on')}</button><button class="btn sm" type="button" data-pw="0">${t('sv.off')}</button>`)+
  '<div class="fr">'+`<select class="in" id="cK" style="flex:1">${CAT.console.map(c=>`<option value="${esc(c.cmd)}">${esc(c.name)}</option>`).join('')}</select>`+`</div><div class="pad"><div class="mg" id="cF"></div></div><div class="fr"><button class="btn pri" type="button" id="cGo">${t('sv.send')}</button></div>`+
  '<div class="kv" id="caps"></div>','cCon','custom')+
 card(t('sv.sweep'),row('','',t('sv.sweepH'))+`<div class="fr"><button class="btn pri" type="button" id="swGo">${t('sv.start')}</button><button class="btn" type="button" id="swEnd">${t('sv.reset')}</button></div><div class="pad"><b class="mono" id="swN">—</b> <span id="swW" class="muted"></span></div>`,'','custom')+
 card(t('sv.wire'),row('','',t('sv.wireH'))+
  `<div class="fr">${sel('wK',PK.map(a=>[a.wire,a.name]),PK.length?PK[0].wire:'','data-nosave data-str id="wK"')}<button class="btn" type="button" data-wire="0">${t('sv.click')}</button><button class="btn" type="button" data-wire="1">${t('sv.hold')}</button></div>`,'','custom')+
 card(t('sv.test'),row('','',t('sv.testH'))+`<div class="fr"><input class="in" id="iT" placeholder="${esc(t('sv.title'))}" value="Test"><input class="in" id="iA" placeholder="${esc(t('sv.artist'))}" value="MegaOpen"></div><div class="fr"><input class="in" type="number" id="iE" value="30" style="width:80px" aria-label="${esc(t('sv.elapsed'))}"><input class="in" type="number" id="iD" value="200" style="width:80px" aria-label="${esc(t('sv.duration'))}"><button class="btn pri" type="button" id="iGo">${t('sv.show')}</button><button class="btn" type="button" id="spGo">${t('sv.splash')}</button></div>`,'','custom')+
 card(t('sv.hu'),row('','',t('sv.huH'))+`<div class="kv"><div><small>${t('sv.huT')}</small><b id="huT"></b></div><div><small>${t('sv.huA')}</small><b id="huA"></b></div><div><small>${t('sv.huN')}</small><b id="huN"></b></div><div><small>${t('sv.huS')}</small><b id="huS"></b></div></div>`+
  row(t('sv.pinT'),txt('/api/settings|huTitleCmd',se.huTitleCmd?h2(se.huTitleCmd):'',t('sv.auto'),'in code','text','data-hex'))+row(t('sv.pinA'),txt('/api/settings|huArtistCmd',se.huArtistCmd?h2(se.huArtistCmd):'',t('sv.auto'),'in code','text','data-hex'))+row(t('sv.titleFirst'),sw('/api/settings|huTitleFirst',se.huTitleFirst))+
  `<div class="fr"><button class="btn dng" type="button" id="huF">${t('sv.forget')}</button></div>`):'')+
 card(t('k.set.raise'),`<div id="tun">${row(t('k.set.raise.doorsLen'),sel('#doorsLen',[[1,t('sv.doors1')],[2,'2']],se['raise.doorsLen']))}${row(t('k.set.raise.cmd28'),sel('#cmd28',regOpts('raise.cmd28'),se['raise.cmd28'],'data-str'))}${row(t('k.set.raise.version'),`<input class="in" data-f="version" value="${esc(se['raise.version']||'')}" maxlength="16">`)}</div>
  <details class="sub"><summary>${t('sv.msgs',MS.length)}</summary><div class="tw"><table><thead><tr><th>${t('sv.thName')}</th><th>cmd</th><th>${t('sv.thMs')}</th><th>${t('sv.thOn')}</th><th></th></tr></thead><tbody id="msgs">${MS.map(x=>`<tr data-id="${x.id}"><td>${esc(x.name)}</td><td class="mono">${h2(x.cmd)}</td><td class="num">${x.interval}</td><td><label class="cbh"><input type="checkbox" class="cb" data-en${x.enabled?' checked':''} aria-label="${esc(t('sv.enabled'))}"></label></td><td><button class="btn sm" type="button" data-send${x.ready?'':' disabled'}>▶</button></td></tr>`).join('')}</tbody></table></div></details>
  <details class="sub"><summary>${t('sv.key')}</summary><div class="fr"><input class="in code" id="kc" placeholder="16" maxlength="2">${sel('ks',[[1,t('sv.press')],[2,t('sv.long')]],1,'data-nosave id="ks"')}<button class="btn pri" type="button" id="kGo">${t('sv.send')}</button></div><div class="pad"><div class="seg wrap" id="cat">${CAT.huKeys.map(k=>`<button type="button" data-kc="${esc(k.code)}">${esc(k.code)} ${esc(k.name)}</button>`).join('')}</div></div></details>
  <details class="sub"><summary>${t('sv.raw')}</summary><div class="fr"><input class="in code" id="rC" placeholder="cmd" maxlength="2"><input class="in" id="rD" placeholder="${esc(t('sv.rawPh'))}"><button class="btn pri" type="button" id="rGo">${t('sv.send')}</button></div></details>
  <details class="sub" id="ringD"><summary>${t('sv.ring')}</summary><pre class="json" id="ring"></pre></details>`,'','custom')+
 (V?card(t('sv.canb'),'<div class="kv" id="cb"></div>'+
  row(t('sv.ecc'),`<button class="btn sm" type="button" id="eccGo">${t('sv.eccGo')}</button>`,'',t('sv.eccH'))+'<pre class="json" id="ecc" hidden></pre>'):'')+
 (CP?card(t('sv.cap'),row(t('sv.capRing'),sw('/api/settings|cap.ring',se['cap.ring']),t('sv.capRingH'))+row(t('k.set.cap.fill'),num('/api/settings|cap.fill',se['cap.fill'],regRange('cap.fill')))+row(t('k.set.cap.fileMB'),num('/api/settings|cap.fileMB',se['cap.fileMB'],regRange('cap.fileMB')))+row(t('k.set.cap.autoProtect'),sw('/api/settings|cap.autoProtect',se['cap.autoProtect']))+
  `<div class="fr"><button class="btn dng" type="button" id="capFmt">${t('sv.capFmt')}</button></div>`):'');
 $$('.card:not(.custom)',m).forEach(c=>saver(c,genericSave,'[data-k]'));
 // лог: опитування /api/logs?since= раз на секунду замість WebSocket; more — плата
 // віддала не все (до 200 рядків за раз), lost — кільце витіснило рядки раніше, ніж сторінка їх забрала
 const lg=$('#lg'),lnTxt=l=>l.lost?t('sv.lost',l.lost):l.drop?t('sv.drop',l.drop):`${(l.t/1000).toFixed(1)} ${l.l} [${l.tag}] ${l.msg}`;
 const draw=()=>{lg.innerHTML=this.lines.filter(l=>l.lost||l.drop||this.tagF==='all'||l.tag===this.tagF).slice(-400).map(l=>`<span class="${l.lost||l.drop?'W':l.l[0]}">${esc(lnTxt(l))}</span>`).join('\n');lg.scrollTop=lg.scrollHeight};
 const poll=async()=>{if(this.busy)return;this.busy=true;try{let got=0;for(let i=0;i<10;i++){const r=await API.get('/api/logs?since='+this.since);
   /* інший boot — плата перезапустилась: спочатку, з since=0, без позначки пропуску */
   if(this.boot!==undefined&&r.boot!==this.boot){this.boot=r.boot;this.since=0;this.drop=r.dropped;continue}
   this.boot=r.boot;if(this.drop!==undefined&&r.dropped>this.drop){this.lines.push({drop:r.dropped-this.drop});got++}this.drop=r.dropped;
   /* перше опитування починає з найстаршого рядка в кільці: що було до нього — не пропуск */
   if(r.lost&&this.since)this.lines.push({lost:r.lost});this.since=r.next;this.lines.push(...r.lines);got+=r.lines.length+(r.lost?1:0);if(!r.more)break}
  if(got){if(this.lines.length>2000)this.lines.splice(0,this.lines.length-2000);if(!this.paused)draw()}}catch(e){}finally{this.busy=false}};
 await poll();const lt=setInterval(poll,1000);leave=()=>clearInterval(lt);
 $('#lgTag').addEventListener('change',()=>{this.tagF=$('#lgTag').dataset.v;draw()});
 $('#lgP').onclick=e=>{this.paused=!this.paused;e.target.textContent=this.paused?t('sv.resume'):t('sv.pause');if(!this.paused)draw()};
 $('#lgC').onclick=()=>{this.lines=[];draw()};
 $('#lgD').onclick=e=>sv(svOf(e.target),'ok',saveText('log.txt',this.lines.map(lnTxt).join('\n'))+t('sv.lines',this.lines.length));
 // консоль
 if(D){const cF=()=>{$('#cF').innerHTML=con().fields.map(cField).join('')||`<span class="muted">${esc(t('sv.noParams'))}</span>`};cF();$('#cK').onchange=cF;
 $('#cGo').onclick=e=>{const c=con(),q=new URLSearchParams();c.fields.forEach(({n})=>{const v=val($(`#cF [data-f="${n}"]`));if(v!==undefined&&v!=='')q.set(n,v)});run(e.target,()=>API.post('/api/display/'+c.cmd+'?'+q),t('sv.queued',c.name))};
 /* заміри скла: лінійки по одній на крок, docs/glass-sweeps.md */
 const RD='123456789A123456789B123456789C',RW='WWWWWWWWW1WWWWWWWWW2WWWWWWWWW3',RL='abcdefghi1abcdefghi2abcdefghi3',SW=[
  ['fullscreen',{l1:RD},t('sv.sw.1')],
  ['fullscreen',{l1:RW},t('sv.sw.2')],
  ['fullscreen',{l1:RL},t('sv.sw.3')],
  ['fullscreen',{l1:RD.slice(0,22),l2:RD.slice(0,22),l3:RD.slice(0,22)},t('sv.sw.4')],
  ['menu',{header:RD,row0:RD,row1:RW},t('sv.sw.5')],
  ['info',{l1:'12345678',l2:'WWWWWWWW',l3:'abcdefgh'},t('sv.sw.6')],
 ];
 let swI=-1;const swStep=async b=>{swI++;if(swI>=SW.length){swI=-1;$('#swN').textContent=t('sv.done');$('#swW').textContent='';b.textContent=t('sv.start');await API.post('/api/display/clear');return {note:t('sv.end')}}
  const[c,q,w]=SW[swI];$('#swN').textContent=(swI+1)+'/'+SW.length;$('#swW').textContent=w;b.textContent=t('sv.next');await API.post('/api/display/'+c+'?'+new URLSearchParams(q));return {note:t('sv.step',swI+1)}};
 $('#swGo').onclick=e=>run(e.target,()=>swStep(e.target));
 $('#swEnd').onclick=e=>{swI=-1;$('#swN').textContent='—';$('#swW').textContent='';$('#swGo').textContent=t('sv.start');run(e.target,()=>API.post('/api/display/clear'),t('sv.back'))};
 $$('[data-pw]',m).forEach(b=>b.onclick=()=>run(b,()=>API.post('/api/display/power?on='+b.dataset.pw)));
 $$('[data-wire]',m).forEach(b=>b.onclick=()=>run(b,()=>API.post('/api/display/key?wire=1&code='+$('#wK').value+'&hold='+b.dataset.wire)));
 /* утримання коліщатка не має форми на шині: каталог каже wireHold:false */
 const wH=()=>{$('[data-wire="1"]',m).disabled=(PK.find(k=>k.wire===$('#wK').value)||{}).wireHold===false};$('#wK').addEventListener('change',wH);wH();
 $('#iGo').onclick=e=>run(e.target,()=>API.post('/api/media',{title:$('#iT').value,artist:$('#iA').value,elapsed:+$('#iE').value,duration:+$('#iD').value}));
 $('#spGo').onclick=e=>run(e.target,()=>API.post('/api/media',{splashNow:true}));
 $('#huF').onclick=e=>ask(e.target,t('sv.forgetAsk'),()=>run(e.target,()=>API.post('/api/settings',{huForget:true}),t('sv.forgotten')))}
 /* налаштування Raise — записи реєстру raise.* (зберігаються в NVS) */
 saver($('#tun'),()=>{const o={};$$('#tun [data-f]').forEach(el=>{const v=val(el);if(v!==undefined)o['raise.'+el.dataset.f]=v});return API.post('/api/settings',o)},'[data-f]');
 /* таблиця реєстру huMessages: рядок i = повідомлення i, [увімк]; лише в RAM, тож шлемо всі рядки */
 $('#msgs').addEventListener('change',e=>{if(e.target.dataset.en===undefined)return;
  run(e.target,()=>API.post('/api/settings',{huMessages:$$('#msgs tr').map(tr=>[$('[data-en]',tr).checked])}))});
 $('#msgs').addEventListener('click',e=>{const b=e.target.closest('[data-send]');if(!b)return;const id=b.closest('tr').dataset.id;run(b,()=>API.post('/api/messages/send?id='+id),t('sv.sent'))});
 $('#cat').onclick=e=>{const b=e.target.closest('[data-kc]');if(b)$('#kc').value=b.dataset.kc};
 $('#kGo').onclick=e=>run(e.target,()=>API.post('/api/canbox/key?code='+$('#kc').value+'&state='+$('#ks').value));
 $('#rGo').onclick=e=>run(e.target,()=>API.post('/api/canbox/raw?cmd='+$('#rC').value+'&data='+encodeURIComponent($('#rD').value)).then(r=>({note:r.bytes})));
 if(V){$('#eccGo').onclick=e=>run(e.target,async()=>{const r=await API.get('/api/can/ecc');const p=$('#ecc');p.hidden=false;p.textContent=(r.seen||[]).map(x=>t('sv.eccRow',x.count,x.type,x.dir,x.where,h2(x.raw))).join('\n')||t('sv.eccNone');return {note:t('sv.samples')}});}
 if(CP)$('#capFmt').onclick=e=>ask(e.target,t('sv.capFmtAsk'),()=>run(e.target,()=>API.post('/api/cap/format?confirm=cap'),t('sv.erased')),'cap');
 await this.tick();
},async tick(){this.n++;const D=has('display');const[di,me,can]=await Promise.all([getIf(D,'/api/display'),getIf(D,'/api/media'),getIf(has('vehicle'),'/api/can')]);if(!$('#lg'))return;
 if(di){$('#caps').innerHTML=[[t('sv.phase'),di.phase],[t('sv.lostN'),di.sessionsLost],[t('sv.lastLoss'),di.lastLoss],[t('sv.supports'),Object.keys(di.supports||{}).filter(k=>di.supports[k]).join(' ')]].map(([a,b])=>`<div><small>${a}</small><b>${esc(b)}</b></div>`).join('');
 const hx=n=>n?'0x'+h2(n):DASH,src=enumOf('mediaSrc',me.sourceActive);$('#huT').textContent=hx(me.huTitleCmd);$('#huA').textContent=hx(me.huArtistCmd);$('#huN').textContent=fmtN(me.huFrames||0);$('#huS').textContent=src?src.name:me.sourceActive||DASH}
 if(can){$('#cb').innerHTML=[[t('sv.canState'),can.state],[t('sv.canErr'),can.rxErr+' / '+can.txErr],[t('sv.canBus'),can.busErr],[t('sv.canRx'),fmtN(can.twaiRx)],[t('sv.canTx'),fmtN(can.twaiTxOk)+' / '+can.twaiTxFail],[t('sv.canPins'),can.rxPin+' / '+can.txPin]].map(([a,b])=>`<div><small>${a}</small><b>${esc(b)}</b></div>`).join('');}
 if($('#ringD').open&&this.n%2===0){const r=await API.get('/api/canbox/last');$('#ring').textContent=(r.frames||[]).map(x=>t('sv.ageS',(x.ageMs/1000).toFixed(1))+'  '+x.dir+' '+x.cmd+' '+x.data+(x.ascii?'  '+x.ascii:'')).join('\n')||t('sv.empty')}
}};
