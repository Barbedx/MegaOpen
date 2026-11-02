/* ---------- Пошук екрана навігації (docs/nav-text.md) ----------
   Картка на «Сервісі»: кожна гіпотеза — кнопка. ▶ шле один-два кадри на панель
   (/api/display/raw або /styled), далі вердикт ✓ ~ ✗ ⚡. Вердикт іде в лог плати
   (/api/display/sweep → [disp] sweep …, звідти в megaopen-logs) і в localStorage
   (mo-navsweep), щоб пережити перезавантаження сторінки. Байт формату — перебір
   за таймером: текст починається з F<hex>, тож фото каже, який це варіант. */
const NS_TEXT='>LITOMSKA, ULICA TEST 1234567890ABCDEFGHIJKLMNOP';
/* стрілка 48×48 з OEM-логу навігатора (navi start.csv, другий кадр 1F1), 288 Б */
const NS_ARROW='00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 0C 00 00 00 00 00 1C 00 00 00 00 00 78 00 00 00 00 01 E8 00 00 00 00 07 D0 00 00 00 00 1F 90 00 00 00 00 7F 20 00 00 00 01 FE 20 00 00 00 07 FC 40 00 00 00 1F F8 40 00 00 00 7F F0 80 00 00 01 FF E0 80 00 00 07 FF C1 00 00 00 3F FF 81 00 00 00 0F FF 02 00 00 00 03 FE 02 00 00 00 00 FC 04 00 00 00 00 38 04 00 00 00 00 10 08 00 00 00 00 08 08 00 00 00 00 04 10 00 00 00 00 04 10 00 00 00 00 02 20 00 00 00 00 02 20 00 00 00 00 01 40 00 00 00 00 01 40 00 00 00 00 00 80 00 00 00 00 00 80 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 03 3D E1 E6 33 80 04 A1 01 29 4A 40 04 B9 C1 C9 7A 40 04 A1 01 29 4A 40 03 21 01 26 4B 80 00 00 00 00 00 00';
const nsHex=b=>b.map(h2).join(' ');
const nsAscii=s=>[...s].map(c=>c.charCodeAt(0)&127);
/* кадр тексту (CarminatDisplay::setTextStyled): 10 <довжина> 77 icon bank2 src fmt 01 + комірки;
   бібліотека оголошує 0x0E = 6 байтів заголовка + 8 комірок, тут — 6 + усі комірки, якщо не задано decl */
const nsText=(cells,o={})=>{const c=nsAscii(cells);return nsHex([0x10,o.decl??(6+c.length),0x77,o.icon??0x55,o.bank2??0x55,o.src??0xFF,o.fmt??0x60,0x01,...c])};
/* кадр панелі навігації: заголовок 14 Б (OEM: 21 0B, дев'ять нулів, 12 30 30) + бітмап */
const NS_HDR=[0x21,0x0B,0,0,0,0,0,0,0,0,0,0x12,0x30,0x30];
const nsNav=(patch={})=>{const h=NS_HDR.slice();for(const k in patch)h[+k]=patch[k];return nsHex(h)+' '+NS_ARROW};
const T14=NS_TEXT.slice(0,14),T8=NS_TEXT.slice(0,8);
const ns=(id,g,sends)=>({id,g,sends});
/* sends: {id,data} — /api/display/raw (JSON-тіло: 302 Б не влазять у рядок запиту); {styled:{…}} — /api/display/styled */
const NS_STEPS=[
 ns('A1','A',[{id:'151',data:nsText(T14)}]),
 ns('A2','A',[{id:'151',data:nsText(NS_TEXT.slice(0,22))}]),
 ns('A3','A',[{id:'151',data:nsText(NS_TEXT.slice(0,30))}]),
 ns('A4','A',[{id:'151',data:nsText(NS_TEXT.slice(0,42))}]),
 ns('C1','C',[{styled:{text:T14,icon:0xD5}}]),
 ns('C2','C',[{styled:{text:T14,bank2:0xD5}}]),
 ns('C3','C',[{styled:{text:T14,icon:0xD5,bank2:0xD5}}]),
 ns('D1','D',[{id:'1F1',data:nsText(T14,{decl:0x0E})}]),
 ns('D2','D',[{id:'1F1',data:nsText(T14)}]),
 ns('E1','E',[{id:'1F1',data:nsNav()}]),
 ns('E2','E',[{id:'1F1',data:nsNav({3:0x25})}]),
 ns('E3','E',[{id:'1F1',data:nsNav({2:0x01})}]),
 ns('E4','E',[{id:'1F1',data:nsNav({4:0x01})}]),
 ns('F1','F',[{id:'1F1',data:nsNav()},{id:'151',data:nsText(T14)}]),
 ns('F2','F',[{id:'1F1',data:nsNav()},{id:'1F1',data:nsText(T14)}]),
];
const NS_GROUPS=['A','B','C','D','E','F'];
/* B: байт формату — 0x59..0x7F без звичного 0x60, тоді 0x19..0x3F */
const NS_FMTS=[];for(let v=0x59;v<=0x7F;v++)if(v!==0x60)NS_FMTS.push(v);for(let v=0x19;v<=0x3F;v++)NS_FMTS.push(v);
const NS_V=['yes','other','no','lost'],NS_ICON={yes:'✓',other:'~',no:'✗',lost:'⚡'};
const nsLoad=()=>{try{return JSON.parse(localStorage.getItem('mo-navsweep')||'{}')}catch(e){return {}}};
const nsSave=st=>{try{localStorage.setItem('mo-navsweep',JSON.stringify(st))}catch(e){}};
const nsVText=r=>r?NS_ICON[r.v]+' '+t('ns.v.'+r.v)+(r.note?' · '+r.note:''):'';
const nsRow=(s,st)=>`<div class="row" data-ns="${s.id}"><div class="l"><b>${s.id}</b><small>${esc(t('ns.s.'+s.id))}</small><small class="nsv" data-v>${esc(nsVText(st[s.id]))}</small></div><div class="c"><span class="fr"><button class="btn sm pri" type="button" data-send="${s.id}" aria-label="${esc(t('ns.send'))}">▶</button>${NS_V.map(v=>`<button class="btn sm" type="button" data-verdict="${v}" title="${esc(t('ns.v.'+v))}" aria-label="${esc(t('ns.v.'+v))}">${NS_ICON[v]}</button>`).join('')}</span></div></div>`;
const nsAutoHTML=st=>{const hits=Object.keys(st).filter(k=>k.startsWith('B:')).map(k=>k.slice(2)+' '+nsVText(st[k]));
 return `<div class="fr"><select class="in" id="nsBT" aria-label="${esc(t('ns.bEvery'))}">${[3,5,8].map(s=>`<option value="${s}"${s===5?' selected':''}>${t('ca.sec',s)}</option>`).join('')}</select><button class="btn pri" type="button" id="nsBGo">${t('ns.bStart')}</button><button class="btn" type="button" id="nsBHit">${t('ns.bHit')}</button></div>`+
  `<div class="pad"><b class="mono" id="nsBN">—</b> <span class="muted" id="nsBW"></span></div>`+(hits.length?row(t('ns.bHits'),'',hits.map(esc).join(' · ')):'')};
function nsCard(){const st=nsLoad();
 return card(t('ns.card'),row('','',t('ns.h'))+
  row(t('ns.link'),`<span class="chip off" id="nsLink">…</span>`,t('ns.linkSub'))+
  `<div class="fr"><button class="btn" type="button" id="nsResync">${t('ns.resync')}</button><button class="btn" type="button" id="nsClear">${t('ns.clear')}</button><input class="in grow" id="nsNote" placeholder="${esc(t('ns.notePh'))}" maxlength="60" aria-label="${esc(t('ns.notePh'))}"></div>`+
  NS_GROUPS.map(g=>`<details class="sub"${g==='A'?' open':''}><summary>${esc(t('ns.g.'+g))}</summary>${row('','',t('ns.gh.'+g))}${g==='B'?nsAutoHTML(st):NS_STEPS.filter(s=>s.g===g).map(s=>nsRow(s,st)).join('')}</details>`).join(''),
  'nsCard','custom full')}
function nsBind(m){const st=nsLoad();let bt=null,bi=-1;
 const link=async()=>{try{const d=await API.get('/api/display');setChip('#nsLink',d.registered?'ok':d.synced?'warn':'bad',d.registered?t('ns.ok'):d.synced?t('ns.sync'):t('ns.down'));return d.registered}catch(e){setChip('#nsLink','bad',t('ns.down'));return false}};
 const sendOne=x=>x.styled?API.post('/api/display/styled?'+new URLSearchParams({text:x.styled.text,icon:x.styled.icon??0x55,src:x.styled.src??0xFF,fmt:x.styled.fmt??0x60,bank2:x.styled.bank2??0x55}))
  :API.post('/api/display/raw',{id:x.id,data:x.data});
 const send=async s=>{for(const x of s.sends)await sendOne(x);await sleep(1500);const ok=await link();return {note:ok?t('ns.sent',s.id):t('ns.lost')}};
 const verdict=(id,v,btn)=>{const note=$('#nsNote').value.trim();st[id]={v,note,t:Date.now()};nsSave(st);
  const r=$(`[data-ns="${id}"] [data-v]`,m);if(r)r.textContent=nsVText(st[id]);
  run(btn,()=>API.post('/api/display/sweep?'+new URLSearchParams({step:id,verdict:v,note})),t('ns.saved'))};
 m.addEventListener('click',e=>{const b=e.target.closest('[data-send]');if(b){const s=NS_STEPS.find(x=>x.id===b.dataset.send);if(s)run(b,()=>send(s));return}
  const v=e.target.closest('[data-verdict]');if(v){const row=v.closest('[data-ns]');if(row)verdict(row.dataset.ns,v.dataset.verdict,v)}});
 $('#nsResync').onclick=e=>run(e.target,()=>API.post('/api/display/resync'));
 $('#nsClear').onclick=e=>run(e.target,()=>API.post('/api/display/clear'),t('sv.back'));
 const bStop=()=>{clearInterval(bt);bt=null;const b=$('#nsBGo');if(b)b.textContent=t('ns.bStart')};
 $('#nsBGo').onclick=async e=>{if(bt){bStop();return}bi=-1;e.target.textContent=t('ns.bStop');
  const tick=async()=>{bi++;if(bi>=NS_FMTS.length){bStop();$('#nsBN').textContent=t('ns.bDone');return}const f=NS_FMTS[bi];
   $('#nsBN').textContent=(bi+1)+'/'+NS_FMTS.length+' · fmt '+h2(f);$('#nsBW').textContent='';
   try{await sendOne({styled:{text:'F'+h2(f)+' LITOMSKA ULICA',fmt:f}})}catch(x){$('#nsBW').textContent=errT(x)}};
  await tick();bt=setInterval(tick,+$('#nsBT').value*1000)};
 $('#nsBHit').onclick=e=>{if(bi<0)return;const f=NS_FMTS[bi];bStop();verdict('B:'+h2(f),'yes',e.target)};
 link();const lt=setInterval(link,2000);const prev=leave;leave=()=>{if(prev)prev();clearInterval(lt);bStop()}}
