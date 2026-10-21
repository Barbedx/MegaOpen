/* ---------- Екран ---------- */
/* пояснення до типу панелі — sc.panelHint.<id>; нема — лише назва */
const panelHint=id=>tHas('sc.panelHint.'+id)?': '+t('sc.panelHint.'+id):'';
const clkSrcT=s=>tHas('sc.clk.'+s)?t('sc.clk.'+s):s;
const mk=f=>$(`[data-k="/api/media|${f}"]`);
/* параметри розкладки — поля з каталогу (layouts[].params), той самий рендер, що й консоль */
function lpHTML(me){return (layoutOf(me.layout).params||[]).map(f=>row(fieldLabel(f),fieldCtrl(f,'/api/media|'+f.n,me[f.n]),f.hint?esc(f.hint):'')).join('')}
/* /api/media плоско, як ключі полів: pane — 'auto' або сцена */
const mediaFlat=me=>({...me,pane:me.pane?(me.pane.auto?'auto':me.pane.scene):undefined,paneSpeed:me.pane&&me.pane.speed,paneCycleS:me.pane&&me.pane.cycleS});
SCR.screen={title:'nav.screen',async render(m){
 if(!has('display')){m.innerHTML=notBuilt(t('nav.screen'),t('sc.noPanel'));return}
 const[me,se,di]=await Promise.all([API.get('/api/media'),API.get('/api/settings'),API.get('/api/display')]);
 const ams=has('ble')&&!!(byId(CAT.bleModes,se.bleMode)||{}).ams;this.ams=ams;
 const panel=byId(CAT.panels,di.type)||{id:di.type,name:di.type,caps:[]},lays=CAT.layouts.filter(l=>(l.panels||[]).includes(di.type)),hasPane=(panel.caps||[]).includes('pane');
 const srcs=enumList('mediaSrc').filter(e=>e.pick&&(ams||!e.ams)).map(e=>[e.v,e.name]),pv=mediaFlat(me).pane;
 m.innerHTML=
 card(t('sc.panel'),
  row(t('sc.showMusic'),sw('/api/media|auto',me.auto))+
  `<div class="glasswrap"><canvas id="glass" class="glassC" role="img" aria-label="${esc(t('sc.preview'))}"></canvas><small class="muted" id="glassW"></small></div>`+
  row(t('sc.layout'),sel('/api/media|layout',lays.map(l=>[l.id,l.name]),me.layout,'data-str'),lays.length<CAT.layouts.length?esc(t('sc.onlyLayouts',panel.name,lays.map(l=>l.name.split(' · ')[0].toLowerCase()).join(', '))):'')+
  '<div id="lp">'+lpHTML(me)+'</div>'+
  row(t('sc.textSrc'),seg('/api/settings|mediaSource',srcs,srcs.some(s=>s[0]===se.mediaSource)?se.mediaSource:srcs[0][0]),'<span id="srcNow"></span>',
   ams?t('sc.srcAuto'):t('sc.srcNoBle')),'cGlass','full')+
 card(t('sc.car'),
  row(t('sc.carShow'),sw('/api/settings|carScreen',se.carScreen),'',t('sc.carShowH'))+
  row(t('sc.carWhat'),seg('/api/settings|carPreset',regOpts('carPreset'),se.carPreset),'',t('sc.carWhatH'))+
  `<div class="row muted" id="carHint"${se.carPreset==='trip'?'':' hidden'}>${esc(t('sc.carHint'))}</div>`+[1,2,3].map(i=>row(t('sc.row',i),sel('/api/settings|carSig'+i,regOpts('carSig'+i),se['carSig'+i],'data-str'))).join(''))+
 (hasPane?card(t('sc.pane'),
  row(t('sc.state'),'<span id="paneSt" class="chip off">…</span>')+
  `<div class="pad">${seg('/api/media|pane',[['auto',t('k.scene.auto')],...CAT.scenes.map(s=>[s.id,s.name])],pv,'wrap')}</div>`+
  row(t('sc.animSpeed'),num('/api/media|paneSpeed',(me.pane||{}).speed,'min="1" max="6"'))+
  row(t('sc.cycle'),num('/api/media|paneCycleS',(me.pane||{}).cycleS,'min="5" max="600"'))+
  `<details class="sub" id="drawD"><summary>${t('sc.draw')}</summary><div class="drawbox">
   <canvas class="ed" id="ed" width="48" height="48" aria-label="${esc(t('sc.editor'))}"></canvas>
   <div class="seg" id="edTool" data-v="pen"><button type="button" data-val="pen" class="on">${t('sc.pen')}</button><button type="button" data-val="erase">${t('sc.erase')}</button></div>
   <div class="fr" style="padding:0;border:0"><button class="btn sm" type="button" id="edClr">${t('sc.clear')}</button><button class="btn sm" type="button" id="edInv">${t('sc.invert')}</button><button class="btn sm" type="button" id="edGrab">${t('sc.grab')}</button></div>
   <button class="btn pri" type="button" id="edGo">${t('sc.showOnPanel')}</button></div></details>`):'')+
 card(t('sc.clock'),
  row(t('sc.autoTime'),sw('/api/media|autoTime',me.autoTime),'',t('sc.autoTimeH',ams?t('sc.autoTimeCts'):''))+
  row(t('sc.clockNow'),'<span id="clkSrc" class="chip off">…</span>')+
  row(t('sc.setManual'),`<button class="btn" id="clkNow" type="button">${t('sc.likeThis')}</button>`))+
 (ams&&has('ancs')?card(t('sc.ntf'),row(t('sc.ntfShow'),sw('/api/settings|ancsPopup',se.ancsPopup))+'<div id="ntf"></div>'):'')+
 card(t('sc.panelType'),
  row(t('sc.type'),sel('rb:/api/display|type',CAT.panels.map(p=>[p.id,p.name+panelHint(p.id)]),di.type,'data-str'+rbA('rb:',di.type,t('sc.typeLbl'),true)))+
  row(t('sc.funcReg'),sw('rb:/api/display|funcReg',di.funcReg,rbA('rb:',di.funcReg,t('sc.funcRegLbl'),true)),'',t('sc.funcRegH')));
 $$('.card',m).forEach(c=>saver(c,genericSave));
 $('[data-k="/api/settings|carPreset"]').addEventListener('change',e=>$('#carHint').hidden=e.currentTarget.dataset.v!=='trip');
 mk('layout').addEventListener('change',e=>{const cur={...this.me,layout:e.target.value};$('#lp').innerHTML=lpHTML(cur);this.me=cur;this.fill(cur)});
 $('#clkNow').onclick=e=>{const hm=clockStr().replace(':','');run(e.target,()=>API.post('/api/display/clock?time='+hm),t('sc.timeSet',clockStr()))};
 if(hasPane)this.editor(me);
 this.me=me;this.fill(me);if(ams&&has('ancs'))this.ntf();
},editor(me){const ed=$('#ed'),x=ed.getContext('2d');let px=me.pane&&me.pane.scene==='custom'?hex2px(me.pane.bits):new Uint8Array(2304),down=false;
 const draw=()=>{const cs=getComputedStyle(document.documentElement);x.fillStyle=cs.getPropertyValue('--glass');x.fillRect(0,0,48,48);x.fillStyle=cs.getPropertyValue('--glassT');for(let i=0;i<2304;i++)if(px[i])x.fillRect(i%48,Math.floor(i/48),1,1)};
 const at=e=>{const r=ed.getBoundingClientRect(),cx=Math.floor((e.clientX-r.left)/r.width*48),cy=Math.floor((e.clientY-r.top)/r.height*48);if(cx<0||cy<0||cx>47||cy>47)return;px[cy*48+cx]=$('#edTool').dataset.v==='pen'?1:0;draw()};
 ed.addEventListener('pointerdown',e=>{down=true;ed.setPointerCapture(e.pointerId);at(e)});ed.addEventListener('pointermove',e=>{if(down)at(e)});ed.addEventListener('pointerup',()=>down=false);
 $('#edClr').onclick=()=>{px.fill(0);draw()};$('#edInv').onclick=()=>{px=px.map(b=>b^1);draw()};
 $('#edGrab').onclick=async()=>{const r=await API.get('/api/media');if(r.pane&&r.pane.bits)px=hex2px(r.pane.bits);draw()};
 /* bits= зберігає малюнок і вмикає сцену custom; tick() одразу показує її вибраною */
 $('#edGo').onclick=e=>run(e.target,async()=>{const r=await API.post('/api/media',{bits:px2hex(px)});await this.tick();return r});draw()},
fill(me){glassDraw(me);const ps=$('#paneSt'),pn=me.pane;
 if(ps&&pn){if(pn.hiddenBy)setChip(ps,'off',t('sc.hidden',nameOf(CAT.layouts,pn.hiddenBy)));else setChip(ps,pn.on===false?'off':'ok',(pn.auto?t('k.scene.auto')+' · ':'')+nameOf(CAT.scenes,pn.scene))}
 const sn=$('#srcNow'),src=enumOf('mediaSrc',me.sourceActive);if(sn)sn.textContent=t('sc.now',src?src.name:me.sourceActive||DASH);
 /* clockSrc/clockAt: хто востаннє виставив час панелі і на скільки */
 const cs=$('#clkSrc');if(cs)setChip(cs,me.clockSrc?'ok':!me.autoTime?'off':'warn',me.clockSrc?clkSrcT(me.clockSrc)+' · '+me.clockAt:!me.autoTime?t('sc.autoOff'):t('sc.waitTime'));
 /* усі поля екрана з ключем /api/media|… — без окремого списку полів */
 const f=mediaFlat(me);$$('[data-k^="/api/media|"]').forEach(el=>put(el,f[el.dataset.k.split('|')[1]]))},
async ntf(){const nt=await API.get('/api/notifications');const el=$('#ntf');if(el)el.innerHTML=(nt.items||[]).map(n=>`<div class="nt"><b>${esc(n.app)}</b> · ${esc(n.title)}<small>${esc(n.message)}</small></div>`).join('')||`<div class="nt muted">${esc(t('sc.ntfNone'))}</div>`},
/* поки розкладку не збережено (dirty), тримаємо свою: інакше блок параметрів перемалюється з-під поля, яке редагують */
async tick(){if(!has('display'))return;let me=await API.get('/api/media');const lc=mk('layout');if(this.me&&lc&&lc.dataset.dirty)me={...me,layout:this.me.layout};
 if(this.me&&this.me.layout!==me.layout&&$('#lp')){$('#lp').innerHTML=lpHTML(me)}this.me=me;this.fill(me);this.n=(this.n||0)+1;if(this.ams&&has('ancs')&&this.n%5===0)this.ntf()}};
