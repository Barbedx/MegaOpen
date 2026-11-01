/* ---------- Кнопки ---------- */
const destName=d=>nameOf(CAT.dests,d);
/* чому призначення мертве: код від прошивки (WebUi::putDests), слова — kb.why.<code>; невідомий — як є */
const destWhy=c=>tHas('kb.why.'+c)?t('kb.why.'+c):c||'';
/* маска й живість кожного призначення: ланцюг keys з /api/health (WebUi::putDests);
   маски пишуться в реєстр налаштувань: uart -> routeUart, ams -> routeAms, ... */
const kRt=h=>({dest:((h.chains||[]).find(c=>c.id==='keys')||{}).dest||{}});
const routeId=d=>'route'+d[0].toUpperCase()+d.slice(1);
const actName=a=>nameOf(CAT.actions,a);
/* дії, що можуть бути клавішею макросу або його дією: усі, крім «нічого» */
const realActions=()=>CAT.actions.filter(a=>a.class!=null);
/* дія = зміст і клас маршруту; код лише підміняє байт для ГУ (порожньо = типовий код дії) */
const huOf=id=>(byId(CAT.actions,id)||{}).hu||'';
const actSel=(r)=>`<div class="ka" data-i="${r.index}">${sel('#action',CAT.actions.map(a=>[a.id,a.name]),r.action,`data-str aria-label="${esc(t('kb.action'))}"`)}<input class="in code kc" data-f="code" data-hex value="${r.code?h2(r.code):''}" placeholder="${esc(huOf(r.action))}" maxlength="2" aria-label="${esc(t('kb.code'))}" title="${esc(t('kb.code'))}"><button class="btn sm" type="button" data-test title="${esc(t('kb.testKey'))}">▶</button></div>`;
const kRow=(k,rows)=>{const c=rows.find(r=>r.key===k.id&&!r.hold),h=rows.find(r=>r.key===k.id&&r.hold);if(!c||!h)return '';
 return `<div class="krow" data-key="${esc(k.id)}"><div class="kh"><span class="lamp"></span><b>${esc(k.name)}</b><code class="muted">${esc(k.wire)}</code></div><div class="kgrid">${actSel(c)}${actSel(h)}</div></div>`};
const encLbl=a=>t({cw:'kb.cw',ccw:'kb.ccw',click:'kb.click',hold:'kb.hold'}[a]);
/* клацання для пошуку поганого контакту: руки зайняті дротами, очі не на екрані */
let AC=null;const tick=f=>{try{AC=AC||new AudioContext();const o=AC.createOscillator(),v=AC.createGain();o.frequency.value=f;v.gain.setValueAtTime(.2,AC.currentTime);v.gain.exponentialRampToValueAtTime(.001,AC.currentTime+.06);o.connect(v).connect(AC.destination);o.start();o.stop(AC.currentTime+.06)}catch(e){}};
const knobCard=(e,z,name,pre,zi)=>card(name+' <span id="kl_'+pre+'" class="chip off" style="margin-left:6px">…</span>',
 row(t('kb.enabled'),sw('@'+pre+'.enabled',e.enabled))+
 row(t('kb.test'),`<span class="ktest" id="kt_${pre}"><span class="kl" data-l="a">A</span><span class="kl" data-l="b">B</span><span class="kl" data-l="key">${t('kb.btn')}</span><b class="kdir">·</b><button class="btn sm" type="button" data-beep="${pre}" aria-pressed="false">${t('kb.sound')}</button></span>`,`<span id="ktn_${pre}">—</span>`,t('kb.testH'))+
 `<div class="enc4">${['cw','ccw','click','hold'].map(a=>`<label>${encLbl(a)}${sel('@'+pre+'.'+a+'.action',CAT.actions.map(x=>[x.id,x.name]),e[a].action,'data-str')}</label>`).join('')}</div>`+
 row(t('kb.ringLeds'),num('@led.zones.'+zi+'.count',z.count,'min="0" max="8"'))+
 row(t('kb.idleAct'),sel('@led.zones.'+zi+'.anim',enumOpts('ledIdle'),z.anim,'data-str')+sel('@led.zones.'+zi+'.react',enumOpts('ledReact'),z.react,'data-str'))+
 `<details class="sub"><summary>${t('kb.colours')}</summary><div class="fr" data-leds="${zi}">${z.leds.slice(0,z.count).map((c,j)=>ledPick(zi,j,c)).join('')}<button class="btn sm" type="button" data-same="${zi}">${t('kb.sameAll')}</button></div>
  ${row(t('kb.react'),`<input type="color" class="in" data-kf="led.zones.${zi}.lo" value="${z.lo}" aria-label="${esc(t('kb.from'))}"><input type="color" class="in" data-kf="led.zones.${zi}.hi" value="${z.hi}" aria-label="${esc(t('kb.to'))}">`)}
  ${row(t('kb.inverted'),sw('@'+pre+'.inverted',e.inverted))}${row(t('kb.ppd'),sel('@'+pre+'.ppd',[4,2,1],e.ppd))}${row(t('kb.holdMs'),num('@'+pre+'.holdMs',e.holdMs,'min="200" max="5000" step="50"'))}</details>`,'k_'+pre,'custom knob');
const ledPick=(i,j,c)=>`<input type="color" class="in" data-kf="led.zones.${i}.leds.${j}" value="${esc(c)}" aria-label="${esc(t('kb.led',j+1))}">`;
SCR.keys={title:'nav.keys',async render(m){
 const D=has('display'),K=has('knobs'),MX=has('matrix'),RS=has('res');
 const[am,rt,se,kn,km,rs]=await Promise.all([getIf(D,'/api/affamap'),API.get('/api/health').then(kRt),API.get('/api/settings'),getIf(K,'/api/knobs'),getIf(MX,'/api/keymap'),getIf(RS,'/api/res')]);
 this.kn=kn;this.seq=((am||{}).last||{}).seq;
 const DS=CAT.dests,off=d=>!d.built||!rt.dest[d.id];
 m.innerHTML=`<div class="jump">${[...(D?[['kWheel',t('kb.wheel')]]:[]),...(MX?[['kMat',t('kb.matrix')]]:[]),['kRoute',t('kb.where')],...(RS?[['kRes','KEY1/KEY2']]:[]),...(K?[['k_enc2',t('kb.knobs')]]:[]),['kGest',t('kb.gestures')]].map(([id,lb])=>`<button class="btn sm" type="button" data-jump="${id}">${lb}</button>`).join('')}</div>`+
 (D?card(t('kb.wheelCard'),
  `<div class="row"><div class="l"><b>${t('kb.last')}</b>${hintBtn(t('kb.lastH'))}<small id="kLastW">${t('kb.pressWheel')}</small><small class="hint" hidden>${esc(t('kb.lastH'))}</small></div><div class="c"><span id="kLast" class="chip off">—</span></div></div>`+
  `<div class="krow" id="kHead" style="padding-top:4px;padding-bottom:4px"><div class="kgrid muted" style="font-size:12px;font-weight:800;text-transform:uppercase"><span>${t('kb.colClick')}</span><span>${t('kb.colHold')}</span></div></div>`+
  `<div id="affa">${CAT.panelKeys.filter(k=>k.wire).map(k=>kRow(k,am.rows)).join('')}</div><div class="fr"><button class="btn dng" type="button" id="affaDef">${t('kb.reset')}</button></div>`,'kWheel','custom full kwide'):MX?'':notBuilt(t('kb.wheelCard'),t('kb.noWheel')))+
 (MX?kmCard(km):'')+
 card(t('kb.routes'),`<div class="tw"><table class="grid"><tr><th></th>${DS.map(d=>`<th>${esc(d.name)}<br><span class="chip off" data-live="${esc(d.id)}">…</span></th>`).join('')}</tr>
  ${CAT.classes.map(c=>`<tr><td>${esc(c.name)}</td>${DS.map(d=>`<td><label class="cbh"><input type="checkbox" class="cb" data-f="${esc(d.id)}:${c.bit}"${rt.dest[d.id]&&rt.dest[d.id].mask&(1<<c.bit)?' checked':''}${off(d)||!(d.classes>>c.bit&1)?' disabled':''} aria-label="${esc(d.name)} ${esc(c.name)}"></label></td>`).join('')}</tr>`).join('')}</table></div><div id="rtWhy"></div>`+
  `<div id="rtSet">${row(t('kb.hu'),sw('/api/settings|uartEnabled',se.uartEnabled))}${row(t('kb.pulse'),num('/api/settings|pulseMs',se.pulseMs,'min="40" max="500" step="10"'))}${row(t('kb.popup'),num('/api/settings|keyPopupMs',se.keyPopup?se.keyPopupMs:0,'min="0" max="10000" step="100"'),t('kb.popup0'))}</div>`,'kRoute','custom')+
 (RS?resCard(rs):'')+
 (K?knobCard(kn.enc2,kn.led.zones[0],t('kb.knobL'),'enc2',0)+knobCard(kn.enc,kn.led.zones[1],t('kb.knobR'),'enc',1)+
 card(t('kb.lighting'),row(t('kb.enabled'),sw('@led.enabled',kn.led.enabled))+row(t('kb.brightness'),`<input type="range" min="0" max="255" data-kf="led.brightness" value="${kn.led.brightness}" aria-label="${esc(t('kb.brightness'))}">`)+
  row(t('kb.current'),'<span id="kMa" class="chip off">…</span>'+num('@led.maxMilliamps',kn.led.maxMilliamps,'min="0" max="20000" step="50"'))+
  `<details class="sub"><summary>${t('kb.fine')}</summary>${row(t('kb.order'),sel('@led.order',enumOpts('ledOrder'),kn.led.order,'data-str'))}${row(t('kb.rightFirst'),sw('@led.rightFirst',kn.led.rightFirst))}${row(t('kb.ringRev'),sw('@led.ringReverse',!!kn.led.ringReverse),'',t('kb.ringRevH'))}${row(t('kb.holdReact'),num('@led.holdMs',kn.led.holdMs,'min="0" max="5000" step="50"'),t('kb.hold0'))}${row(t('kb.levelSteps'),num('@led.levelSteps',kn.led.levelSteps,'min="4" max="64"'))}${row(t('kb.animPeriod'),num('@led.animPeriod',kn.led.animPeriod,'min="10" max="255"'))}</details>`+
  `<details class="sub"><summary>${t('kb.feet')}</summary>${row(t('kb.leds'),num('@led.feet.count',kn.led.feet.count,`min="0" max="${kn.led.feet.max||144}"`))}${row(t('kb.colour'),`<input type="color" class="in" data-kf="led.feet.color" value="${kn.led.feet.color}" aria-label="${esc(t('kb.colour'))}">`)}${row(t('kb.idle'),sel('@led.feet.anim',enumOpts('ledIdle'),kn.led.feet.anim,'data-str'))}${row(t('kb.brightness'),`<input type="range" min="0" max="255" data-kf="led.feet.brightness" value="${kn.led.feet.brightness}" aria-label="${esc(t('kb.feetBright'))}">`)}</details>`,'kLed','custom knob'):'')+
 card(t('kb.gestures'),row(t('kb.gestWhere'),`<a class="btn" href="#map:rules">${t('kb.gestGo')}</a>`,'',t('kb.gestH')),'kGest');
 $$('[data-jump]',m).forEach(b=>b.onclick=()=>$('#'+b.dataset.jump).scrollIntoView({behavior:'smooth',block:'start'}));
 // кермо
 if(D){const affa=$('#affa');
 const collect=()=>$$('.ka',affa).map(a=>({index:+a.dataset.i,action:$('[data-f="action"]',a).value,code:val($('[data-f="code"]',a)),asLong:(am.rows.find(r=>r.index===+a.dataset.i)||{}).asLong||false}));
 saver(affa,()=>API.post('/api/affamap',{rows:collect()}),'[data-f]');
 affa.addEventListener('change',e=>{if(e.target.dataset.f==='action')$('[data-f="code"]',e.target.closest('.ka')).placeholder=huOf(e.target.value)});
 affa.addEventListener('click',e=>{const b=e.target.closest('[data-test]');if(!b)return;const r=am.rows.find(x=>x.index===+b.closest('.ka').dataset.i);run(b,()=>API.post('/api/display/key?code=0x'+r.wire.toString(16)+'&hold='+(r.hold?1:0)),nameOf(CAT.panelKeys,r.key)+(r.hold?' '+t('kb.holdShort'):''))});
 $('#affaDef').onclick=e=>ask(e.target,t('kb.resetAsk',am.rows.length),()=>run(e.target,()=>API.post('/api/affamap/defaults')).then(()=>route()))}
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
 m.addEventListener('click',e=>{const bt=e.target.closest('[data-beep]');if(bt){bt.setAttribute('aria-pressed',bt.getAttribute('aria-pressed')==='true'?'false':'true');bt.classList.toggle('on');tick(900)}});
 m.addEventListener('click',e=>{const b=e.target.closest('[data-same]');if(!b)return;const ps=$$('input[type=color]',b.parentElement);if(!ps.length)return;ps.forEach(p=>p.value=ps[0].value);ps[0].dispatchEvent(new Event('input',{bubbles:true}))});
 /* підсвітка натискань: окреме швидке опитування, лише поки відкрита вкладка */
 /* лише коли сторінку видно і картка на екрані: інакше ~9 запитів/с без потреби */
 if(D||K||RS){const fast=setInterval(()=>{if(document.hidden)return;
   if(D&&onScreen($('#kWheel')))this.live().catch(()=>{});
   if(K&&($$('.knob',m).some(onScreen)))API.get('/api/knobs/live').then(kl=>this.knobTest(kl)).catch(()=>{});
   if(RS&&onScreen($('#kRes')))resLive().catch(()=>{})},K?250:300);leave=()=>clearInterval(fast)}
 this.n=0;await this.tick();
},async live(){const r=await API.get('/api/affamap?live=1');const L=r.last;if(!L||L.seq===this.seq||!$('#affa'))return;this.seq=L.seq;
 $$('#affa .krow').forEach(x=>x.classList.remove('hitC','hitH'));$$('#affa .ka').forEach(x=>x.classList.remove('hit'));
 const kr=L.key?$(`#affa .krow[data-key="${L.key}"]`):null;
 if(kr){kr.classList.add(L.hold?'hitH':'hitC');$$('.ka',kr)[L.hold?1:0].classList.add('hit');clearTimeout(this.ht);this.ht=setTimeout(()=>{kr.classList.remove('hitC','hitH');$$('.ka',kr).forEach(x=>x.classList.remove('hit'))},1500)}
 const none=L.action===noneAction(),wire='0x'+Number(L.wire).toString(16).padStart(4,'0');
 setChip('#kLast',L.key?(L.dests.length?'ok':'warn'):'bad',(L.key?nameOf(CAT.panelKeys,L.key):t('kb.unknown'))+(L.hold?t('kb.lastHold'):t('kb.lastClick')));
 $('#kLastW').textContent=wire+(L.key?' → '+(none?t('kb.nothing'):actName(L.action)+(L.dests.length?t('kb.route',L.dests.map(destName).join(', ')):t('kb.noRoute'))):t('kb.noCode'))},
knobTest(kl){this.kt=this.kt||{};for(const pre of['enc','enc2']){const el=$('#kt_'+pre),l=kl[pre];if(!el||!l)continue;
 const p=this.kt[pre],g=l.edges||[0,0,0],moved=p?g.map((n,i)=>n!==p.g[i]):[0,0,0];
 $$('.kl',el).forEach((x,i)=>x.classList.toggle('on',!!l[x.dataset.l]||!!moved[i]));
 const d=$('.kdir',el);if(p&&l.pos!==p.pos){d.textContent=l.pos>p.pos?'⟳':'⟲';clearTimeout(p.t);p.t=setTimeout(()=>d.textContent='·',700)}
 if(p&&moved.some(Boolean)&&$('[data-beep="'+pre+'"]',el).getAttribute('aria-pressed')==='true')tick(pre==='enc'?1200:700);
 this.kt[pre]={pos:l.pos,g,t:p&&p.t};$('#ktn_'+pre).textContent=t('kb.knobLine',g[0],g[1],g[2],l.detents,(l.pos>0?'+':'')+l.pos,l.clicks)+(l.errors?t('kb.errors',l.errors):'')}},
why(rt){const w=Object.entries(rt.dest).filter(([d,x])=>!x.live&&(byId(CAT.dests,d)||{}).built).map(([d,x])=>`<div class="row"><div class="l"><b>${esc(destName(d))}: ${esc(destWhy(x.code))}</b></div>${d==='uart'?'':`<div class="c"><a href="#board">${t('kb.board')}</a></div>`}</div>`).join('')+((rt.dest.ams||{}).volFallback?row(t('kb.vol'),`<span class="chip warn">${t('kb.fallback')}</span>`,t('kb.fallbackH')):'');const el=$('#rtWhy');if(el)el.innerHTML=w;
 CAT.dests.forEach(d=>{const c=$(`[data-live="${d.id}"]`),x=rt.dest[d.id];if(c)setChip(c,x&&x.live?'ok':'off',!d.built||!x?t('kb.none'):x.live?t('kb.live'):t('kb.dead'))})},
async tick(){
 this.n++;const kl=await getIf(has('knobs'),'/api/knobs/live');if(!$('#kRoute'))return;
 if(kl&&$('#kMa')){const kc=(id,k,l)=>setChip(id,!k.fitted?'off':k.enabled?'ok':'off',!k.fitted?t('kb.notFitted'):k.enabled?t('kb.clicks',l.detents)+(l.btn?t('kb.pressed'):''):t('kb.off'));
 kc('#kl_enc',this.kn.enc,kl.enc||{});kc('#kl_enc2',this.kn.enc2,kl.enc2||{});
 const led=kl.led||{};setChip('#kMa',led.mA>this.kn.led.maxMilliamps*.9?'warn':led.ok?'ok':'bad',led.ok?t('kb.mA',led.mA):t('kb.stripDead'))}
 if(this.n%3===0)this.why(kRt(await API.get('/api/health')));
}};
