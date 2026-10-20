/* ---------- Сервіс ---------- */
/* поле консолі з каталогу — той самий рендер, що й параметри розкладки (fieldCtrl) */
const cField=f=>`<label>${fieldLabel(f)}${fieldCtrl(f,'#'+f.n)}</label>`;
/* межі числа з реєстру налаштувань (CAT.settings): ті самі, що перевіряє прошивка */
const regOf=id=>(CAT.settings||[]).find(x=>x.id===id)||{};
const regRange=id=>{const f=regOf(id);return f.type==='int'?`min="${f.min}" max="${f.max}"${f.step?` step="${f.step}"`:''}`:''};
const regOpts=id=>(regOf(id).enum||[]).map(o=>[o.v,o.name]);
SCR.service={title:'Сервіс',async render(m){
 const D=has('display'),V=has('vehicle'),CP=has('capture');
 const[me,msgs,can,se]=await Promise.all([getIf(D,'/api/media'),API.get('/api/messages'),getIf(V,'/api/can'),API.get('/api/settings')]);
 const MS=msgs.messages||[];this.n=0;this.paused=false;this.lines=[];this.tagF='all';this.since=0;this.busy=false;this.boot=undefined;this.drop=undefined;
 const PK=CAT.panelKeys.filter(k=>k.src==='wheel'),con=()=>CAT.console.find(c=>c.cmd===$('#cK').value)||{name:'',fields:[]};
 m.innerHTML=
 card('Лог',`<div class="pad"><div class="seg wrap" id="lgTag" data-v="all">${['all',...CAT.logTags].map(t=>`<button type="button" class="${t==='all'?'on':''}" data-val="${esc(t)}">${t==='all'?'усе':esc(t)}</button>`).join('')}</div></div>`+
  row('Рівень',sel('/api/settings|logLevel',regOpts('logLevel'),se.logLevel,'data-str'),'до перезапуску; є і в меню на щитку')+
  '<div class="fr"><button class="btn" type="button" id="lgP">Пауза</button><button class="btn" type="button" id="lgC">Очистити</button><button class="btn" type="button" id="lgD">Скачати</button></div><div class="log" id="lg"></div>','','full')+
 (D?card('Панель: консоль',row('Живлення','<button class="btn sm" type="button" data-pw="1">Увімк</button><button class="btn sm" type="button" data-pw="0">Вимк</button>')+
  '<div class="fr">'+`<select class="in" id="cK" style="flex:1">${CAT.console.map(c=>`<option value="${esc(c.cmd)}">${esc(c.name)}</option>`).join('')}</select>`+'</div><div class="pad"><div class="mg" id="cF"></div></div><div class="fr"><button class="btn pri" type="button" id="cGo">Надіслати</button></div>'+
  '<div class="kv" id="caps"></div>','cCon','custom')+
 card('Клавіша в CAN B',row('','','Кадр клавіші йде в шину панелі, ніби від джойстика: щоб перевірити штатне радіо. Плата сама його не обробляє.')+
  `<div class="fr">${sel('wK',PK.map(a=>[a.wire,a.name]),PK.length?PK[0].wire:'','data-nosave data-str id="wK"')}<button class="btn" type="button" data-wire="0">клік</button><button class="btn" type="button" data-wire="1">утрим.</button></div>`,'','custom')+
 card('Медіа: тестовий трек',row('','','Підставляє вигаданий трек, щоб глянути розкладку без телефона. Тримається, доки джерело не оновить екран.')+`<div class="fr"><input class="in" id="iT" placeholder="Назва" value="Тест"><input class="in" id="iA" placeholder="Виконавець" value="MegaOpen"></div><div class="fr"><input class="in" type="number" id="iE" value="30" style="width:80px" aria-label="Минуло, с"><input class="in" type="number" id="iD" value="200" style="width:80px" aria-label="Тривалість, с"><button class="btn pri" type="button" id="iGo">Показати</button><button class="btn" type="button" id="spGo">Заставка зараз</button></div>`,'','custom')+
 card('ГУ: текст',row('','','Коли ГУ шле текст без маркера, плата вчиться: перша команда з текстом — назва, друга — виконавець. Можна закріпити вручну.')+'<div class="kv"><div><small>Назва: cmd</small><b id="huT"></b></div><div><small>Виконавець: cmd</small><b id="huA"></b></div><div><small>Кадрів від ГУ</small><b id="huN"></b></div><div><small>Джерело зараз</small><b id="huS"></b></div></div>'+
  row('Закріпити назву',txt('/api/settings|huTitleCmd',se.huTitleCmd?h2(se.huTitleCmd):'','авто','in code','text','data-hex'))+row('Закріпити виконавця',txt('/api/settings|huArtistCmd',se.huArtistCmd?h2(se.huArtistCmd):'','авто','in code','text','data-hex'))+row('Назва першою в «А - Б»',sw('/api/settings|huTitleFirst',se.huTitleFirst))+
  '<div class="fr"><button class="btn dng" type="button" id="huF">Забути вивчене</button></div>'):'')+
 card('ГУ: протокол Raise',`<div id="tun">${row('Двері: довжина кадру',sel('#doorsLen',[[1,'1 (апка бачить відчинений капот)'],[2,'2']],se['raise.doorsLen']))}${row('cmd 0x28 несе',sel('#cmd28',regOpts('raise.cmd28'),se['raise.cmd28'],'data-str'))}${row('Версія коробки',`<input class="in" data-f="version" value="${esc(se['raise.version']||'')}" maxlength="16">`)}</div>
  <details class="sub"><summary>Повідомлення · ${MS.length}</summary><div class="tw"><table><thead><tr><th>Назва</th><th>cmd</th><th>мс</th><th>вкл</th><th></th></tr></thead><tbody id="msgs">${MS.map(x=>`<tr data-id="${x.id}"><td>${esc(x.name)}</td><td class="mono">${h2(x.cmd)}</td><td class="num">${x.interval}</td><td><input type="checkbox" class="cb" data-en${x.enabled?' checked':''}></td><td><button class="btn sm" type="button" data-send${x.ready?'':' disabled'}>▶</button></td></tr>`).join('')}</tbody></table></div></details>
  <details class="sub"><summary>Код клавіші 0x20</summary><div class="fr"><input class="in code" id="kc" placeholder="16" maxlength="2">${sel('ks',[[1,'натиск'],[2,'довге']],1,'data-nosave id="ks"')}<button class="btn pri" type="button" id="kGo">Надіслати</button></div><div class="pad"><div class="seg wrap" id="cat">${CAT.huKeys.map(k=>`<button type="button" data-kc="${esc(k.code)}">${esc(k.code)} ${esc(k.name)}</button>`).join('')}</div></div></details>
  <details class="sub"><summary>Сирий кадр</summary><div class="fr"><input class="in code" id="rC" placeholder="cmd" maxlength="2"><input class="in" id="rD" placeholder="дані hex: 01 00"><button class="btn pri" type="button" id="rGo">Надіслати</button></div></details>
  <details class="sub" id="ringD"><summary>Останні кадри UART</summary><pre class="json" id="ring"></pre></details>`,'','custom')+
 (V?card('Шина панелі (CAN B)','<div class="kv" id="cb"></div>'+
  row('Помилки шини за типом','<button class="btn sm" type="button" id="eccGo">Зняти</button>','','Регістр ECC контролера TWAI: 3000 вибірок, які помилки (bit, form, stuff) і в якому місці кадру. Допомагає знайти погану пайку чи термінатор.')+'<pre class="json" id="ecc" hidden></pre>'):'')+
 (CP?card('Запис: розширене',row('Коли місце скінчилось, стирати найстаріші',sw('/api/settings|cap.ring',se['cap.ring']),'вимк: запис зупиниться')+row('Заповнювати сховище до, %',num('/api/settings|cap.fill',se['cap.fill'],regRange('cap.fill')))+row('Розмір файлу, МБ',num('/api/settings|cap.fileMB',se['cap.fileMB'],regRange('cap.fileMB')))+row('Мітка ставить 🔒',sw('/api/settings|cap.autoProtect',se['cap.autoProtect']))+
  '<div class="fr"><button class="btn dng" type="button" id="capFmt">Стерти всі записи</button></div>'):'');
 $$('.card:not(.custom)',m).forEach(c=>saver(c,genericSave,'[data-k]'));
 // лог: опитування /api/logs?since= раз на секунду замість WebSocket; more — плата
 // віддала не все (до 200 рядків за раз), lost — кільце витіснило рядки раніше, ніж сторінка їх забрала
 const lg=$('#lg'),lnTxt=l=>l.lost?`— пропущено рядків: ${l.lost} —`:l.drop?`— плата відкинула рядків: ${l.drop} (кільце повне) —`:`${(l.t/1000).toFixed(1)} ${l.l} [${l.tag}] ${l.msg}`;
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
 $('#lgP').onclick=e=>{this.paused=!this.paused;e.target.textContent=this.paused?'Далі':'Пауза';if(!this.paused)draw()};
 $('#lgC').onclick=()=>{this.lines=[];draw()};
 $('#lgD').onclick=e=>sv(svOf(e.target),'ok',saveText('log.txt',this.lines.map(lnTxt).join('\n'))+', рядків '+this.lines.length);
 // консоль
 if(D){const cF=()=>{$('#cF').innerHTML=con().fields.map(cField).join('')||'<span class="muted">без параметрів</span>'};cF();$('#cK').onchange=cF;
 $('#cGo').onclick=e=>{const c=con(),q=new URLSearchParams();c.fields.forEach(({n})=>{const v=val($(`#cF [data-f="${n}"]`));if(v!==undefined&&v!=='')q.set(n,v)});run(e.target,()=>API.post('/api/display/'+c.cmd+'?'+q),c.name+': у черзі')};
 $$('[data-pw]',m).forEach(b=>b.onclick=()=>run(b,()=>API.post('/api/display/power?on='+b.dataset.pw)));
 $$('[data-wire]',m).forEach(b=>b.onclick=()=>run(b,()=>API.post('/api/display/key?wire=1&code='+$('#wK').value+'&hold='+b.dataset.wire)));
 /* утримання коліщатка не має форми на шині: каталог каже wireHold:false */
 const wH=()=>{$('[data-wire="1"]',m).disabled=(PK.find(k=>k.wire===$('#wK').value)||{}).wireHold===false};$('#wK').addEventListener('change',wH);wH();
 $('#iGo').onclick=e=>run(e.target,()=>API.post('/api/media',{title:$('#iT').value,artist:$('#iA').value,elapsed:+$('#iE').value,duration:+$('#iD').value}));
 $('#spGo').onclick=e=>run(e.target,()=>API.post('/api/media',{splashNow:true}));
 $('#huF').onclick=e=>ask(e.target,'Забути вивчені команди тексту ГУ?',()=>run(e.target,()=>API.post('/api/settings',{huForget:true}),'забуто'))}
 /* налаштування Raise — записи реєстру raise.* (зберігаються в NVS) */
 saver($('#tun'),()=>{const t={};$$('#tun [data-f]').forEach(el=>{const v=val(el);if(v!==undefined)t['raise.'+el.dataset.f]=v});return API.post('/api/settings',t)},'[data-f]');
 /* таблиця реєстру huMessages: рядок i = повідомлення i, [увімк]; лише в RAM, тож шлемо всі рядки */
 $('#msgs').addEventListener('change',e=>{if(e.target.dataset.en===undefined)return;
  run(e.target,()=>API.post('/api/settings',{huMessages:$$('#msgs tr').map(tr=>[$('[data-en]',tr).checked])}))});
 $('#msgs').addEventListener('click',e=>{const b=e.target.closest('[data-send]');if(!b)return;const id=b.closest('tr').dataset.id;run(b,()=>API.post('/api/messages/send?id='+id),'надіслано')});
 $('#cat').onclick=e=>{const b=e.target.closest('[data-kc]');if(b)$('#kc').value=b.dataset.kc};
 $('#kGo').onclick=e=>run(e.target,()=>API.post('/api/canbox/key?code='+$('#kc').value+'&state='+$('#ks').value));
 $('#rGo').onclick=e=>run(e.target,()=>API.post('/api/canbox/raw?cmd='+$('#rC').value+'&data='+encodeURIComponent($('#rD').value)).then(r=>({note:r.bytes})));
 if(V){$('#eccGo').onclick=e=>run(e.target,async()=>{const r=await API.get('/api/can/ecc');const p=$('#ecc');p.hidden=false;p.textContent=(r.seen||[]).map(x=>`${x.count}× ${x.type} ${x.dir} у ${x.where} (0x${h2(x.raw)})`).join('\n')||'помилок нема';return {note:'3000 вибірок'}});}
 if(CP)$('#capFmt').onclick=e=>ask(e.target,'Стерти ВСІ записи, разом із 🔒.',()=>run(e.target,()=>API.post('/api/cap/format?confirm=cap'),'стерто'),'cap');
 await this.tick();
},async tick(){this.n++;const D=has('display');const[di,me,can]=await Promise.all([getIf(D,'/api/display'),getIf(D,'/api/media'),getIf(has('vehicle'),'/api/can')]);if(!$('#lg'))return;
 if(di){$('#caps').innerHTML=[['Фаза',di.phase],['Втрат сесії',di.sessionsLost],['Остання втрата',di.lastLoss],['Уміє',Object.keys(di.supports||{}).filter(k=>di.supports[k]).join(' ')]].map(([a,b])=>`<div><small>${a}</small><b>${esc(b)}</b></div>`).join('');
 const hx=n=>n?'0x'+h2(n):DASH,src=enumOf('mediaSrc',me.sourceActive);$('#huT').textContent=hx(me.huTitleCmd);$('#huA').textContent=hx(me.huArtistCmd);$('#huN').textContent=fmtN(me.huFrames||0);$('#huS').textContent=src?src.name:me.sourceActive||DASH}
 if(can){$('#cb').innerHTML=[['Стан',can.state],['Помилки rx / tx',can.rxErr+' / '+can.txErr],['Помилки шини',can.busErr],['Прийнято',fmtN(can.twaiRx)],['Надіслано ok / ні',fmtN(can.twaiTxOk)+' / '+can.twaiTxFail],['Піни CRX/CTX',can.rxPin+' / '+can.txPin]].map(([a,b])=>`<div><small>${a}</small><b>${esc(b)}</b></div>`).join('');}
 if($('#ringD').open&&this.n%2===0){const r=await API.get('/api/canbox/last');$('#ring').textContent=(r.frames||[]).map(x=>(x.ageMs/1000).toFixed(1)+' с  '+x.dir+' '+x.cmd+' '+x.data+(x.ascii?'  '+x.ascii:'')).join('\n')||'порожньо'}
}};
