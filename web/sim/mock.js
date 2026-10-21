'use strict';
/* ============ СИМУЛЯТОР: плата в браузері ============
   Лише для tools/build_sim.py (артефакт), на плату не потрапляє ніколи.
   Іде ПЕРЕД скриптами сторінки: ставить window.SIM, і 10-api.js шле call() сюди.
   Усе інше будується при першому виклику (simBoot), коли утиліти сторінки
   ($, esc, now, px2hex…) уже є; усередині замикання, щоб імена не зіткнулись.

   ПРОШИВКА — СПЕЦИФІКАЦІЯ (docs/design/web-ui.md): незмінені маршрути відповідають
   рівно так, як src/net/WebUi*.cpp сьогодні — ті самі поля, типи, англійські
   `why`, помилки text/plain. Нове з плану (catalog, logs?since, reboot, map,
   config, affamap.last, key?wire, media bits, status fw/build/apName, health hu,
   macros "<index>: <why>") позначено НОВЕ. */
window.SIM={nextErr:0,offline:false,
 call(m,p,q,h){if(!SIM._call)simBoot();return SIM._call(m,p,q,h)},
 /* прошивка: показ поступу, потім ті самі /ota/start і /ota/upload, що й на платі */
 async upload(file,onP){const h={'X-MegaOpen':'1'};await SIM.call('GET','/ota/start?mode=fr',null,h);for(let p=0;p<=100;p+=4){onP(p);await sleep(80)}return SIM.call('POST','/ota/upload',{name:file.name,size:file.size},h)}};
function simBoot(){
const rnd=(a,b)=>a+Math.random()*(b-a);
const h2=n=>(n&255).toString(16).toUpperCase().padStart(2,'0');
const unixNow=()=>Math.floor(Date.now()/1000);

/* ============ довідники: GET /api/catalog (НОВЕ) ============
   Не тут: build_sim.py вкладає build/catalog.json, який пише прошивка
   (pio test -e native -f test_catalog), у window.SIM_CATALOG. */
/* Поля RAV4 і заводська мапа — з прошивки: catalog.raise (RaiseTargets) і build/decode.json
   (test_decode: заводська таблиця у формі \`can\` з /api/map і імена сигналів) у window.SIM_DECODE. */
/* ?env=direct (t2can, t2can_usbhid): ця збірка — features з каталогу і лише її маршрути.
   Правда про env — з самої збірки плати: build_sim.py кладе window.SIM_ENVS
   (build/features/<env>.json від gen_pages.py). Без env — t2can, коли він є. */
const ENVS=window.SIM_ENVS||{},ENV=(()=>{let e=null;try{e=new URLSearchParams(location.search).get('env')}catch(x){}return ENVS[e]?e:ENVS.t2can?'t2can':Object.keys(ENVS)[0]||''})();
const CATALOG=envCatalog(window.SIM_CATALOG,ENVS[ENV]);
/* каталог, який віддала б ця збірка (Catalog.cpp): без панелі нема розкладок, стилів,
   сцен, типів панелі, консолі, клавіш керма і байтів панелі; dests.built — з features */
function envCatalog(c0,F){const c=JSON.parse(JSON.stringify(c0));c.features=F?{...F}:Object.fromEntries(Object.keys(c.features).map(k=>[k,true]));F=c.features;
 if(!F.display){c.layouts=[];c.styles=[];c.scenes=[];c.panels=[];c.console=[];c.panelKeys=c.panelKeys.filter(k=>k.src!=='wheel');['arrows','icons','src','fmt'].forEach(k=>delete c.enums[k])}
 const built={uart:F.uart,ams:F.ble,hid:F.ble,res:F.res,usb:F.usb};c.dests=c.dests.filter(d=>d.id!=='usb').map(d=>({...d,built:!!built[d.id]}));
 if(F.usb)c.dests.push({id:'usb',name:'USB',built:true});return c}
/* маршрути, що є лише з функцією (як #if у WebUi*.cpp); решта — в кожній збірці */
const GATES=[[/^\/api\/(display|affamap|media)(\/|$)/,F=>F.display],[/^\/api\/ble(\/|$)/,F=>F.display&&F.ble],[/^\/api\/notifications$/,F=>F.ancs],
 [/^\/api\/can2$/,F=>F.can2],[/^\/api\/can(\/|$)/,F=>F.vehicle],[/^\/api\/cap(\/|$)/,F=>F.capture],[/^\/api\/knobs(\/|$)/,F=>F.knobs],
 [/^\/api\/keymap(\/|$)/,F=>F.matrix],[/^\/api\/res(\/|$)/,F=>F.res],[/^\/api\/logs\/(file|format)$/,F=>F.logfs],[/^\/api\/fw(\/|$)/,F=>F.ghota]];
const routeBuilt=p=>{const g=GATES.find(([re])=>re.test(p));return !g||!!g[1](CATALOG.features)};
const C=CATALOG,LVL=C.enums.logLevels.map(e=>e.name),DESTS=C.dests.map(d=>d.id),DNAME=Object.fromEntries(C.dests.map(d=>[d.id,d.name]));
const lay=id=>C.layouts.find(l=>l.id===id)||C.layouts.find(l=>l.id==='fullscreen');
const actClass=id=>{const a=C.actions.find(x=>x.id===id);return a?a.class:null};
const WHEEL=C.panelKeys.filter(k=>k.src==='wheel');
const CAROUSEL=C.scenes.map(s=>s.id).filter(s=>s!=='off'&&s!=='custom');
/* Catalog::arrowsKnown: POST /api/media і /api/display/menu приймають лише значення arrows */
const ARROWS=(C.enums.arrows||[]).map(e=>e.v),SCROLL_WHY='scroll: 0, 7, 11 or 3';
/* App::Ui::clockFrom: хто востаннє виставив час панелі (hu, cts, manual) і на скільки */
const clockFrom=(src,at)=>{S.clk={src,at}};
let ctsMin=-1,huMin=-1;   /* servicePhoneClock: s_lastMin, лише нова хвилина */

/* ============ МОК: стан плати ============ */
/* AffaMap::loadDefaults */
const AFFA_DEF=[['Load','Load'],['SrcNext','Mute'],['SrcPrev','SrcPrev'],['VolUp','VolUp'],['VolDown','VolDown'],['Ok','Ok'],['EncCw','None'],['EncCcw','None']];
const defAffa=()=>WHEEL.flatMap((k,i)=>[0,1].map(h=>({index:i*2+h,key:k.id,wire:parseInt(k.wire,16),hold:!!h,action:AFFA_DEF[i][h],code:0,asLong:!!h&&AFFA_DEF[i][h]!=='None'&&i!==1})));
/* MacroTable::loadDefaults: два приклади, обидва вимкнені; решта — порожні */
const emptyMacro=()=>({enabled:false,name:'',trigger:'off',keys:['None','None','None'],taps:3,windowMs:600,swallow:true,effect:'none',action:'None',codeHex:'00',data:'',arg:''});
const defMacros=()=>{const t=Array.from({length:8},emptyMacro);
 Object.assign(t[0],{name:'VOL+ & VOL-',trigger:'combo',keys:['VolUp','VolDown','None'],effect:'key',codeHex:'07'});
 Object.assign(t[1],{name:'MUTE x3',trigger:'sequence',keys:['Mute','None','None'],taps:3,windowMs:500,effect:'popup',arg:'MACRO'});return t};
const bind=a=>({action:a,code:0});
const encDef=(on,a)=>({enabled:on,inverted:false,ppd:4,holdMs:600,cw:bind(a[0]),ccw:bind(a[1]),click:bind(a[2]),hold:bind(a[3])});
const zone=(react)=>({count:5,react,anim:'breathe',leds:Array(8).fill('#00a0ff'),lo:'#00a0ff',hi:'#ff3000'});
/* KnobConfig.h: типові значення */
const defKnobs=()=>({enc:encDef(true,['EncCw','EncCcw','Ok','SrcNext']),enc2:encDef(false,['VolUp','VolDown','Mute','None']),
 led:{enabled:false,order:'grb',brightness:128,holdMs:1500,levelSteps:30,animPeriod:60,rightFirst:false,ringReverse:false,zoneLeds:8,zones:[zone('level'),zone('flash')],
  feet:{count:0,max:144,anim:'static',brightness:96,color:'#ffffff'},maxMilliamps:400}});
const defSet=()=>({pulseMs:120,uartEnabled:true,uartBaud:'38400',ssid:'HomeWiFi',bleMode:'ams',autoTime:true,ancsPopup:true,mdnsOn:true,mediaLayout:'info',mediaSource:'auto',
 huTitleCmd:0,huArtistCmd:0,huTitleFirst:true,keyPopup:true,keyPopupMs:1200});
/* Canbox::Tuning */
const defTun=()=>({cmd28:0,doorsLen:2,steerInvert:0,steerDiv:10,steerClamp:540,fuelUnit:2,tripUnit:2,tripFields:1,histLen:13,radarInvert:0,version:'raise-rav4-1.2   '});
/* DecodeTable::factory у формі /api/map: рядок за id поля (= ім'я сигналу) */
const pow10=d=>Math.pow(10,d);
/* RaiseTargets::toInt: lround (половина — від нуля), не Math.round */
const toInt=(v,d)=>{const x=v*pow10(d);return Math.sign(x)*Math.round(Math.abs(x))};
const defMap=()=>{const M={};C.raise.forEach(t=>M[t.id]={src:'off',can:null,k:0});
 for(const r of SIM_DECODE.rows)if(M[r.sig]&&M[r.sig].src==='off')M[r.sig]={src:'can',can:{...r.can},k:0};
 return M};
const day=(d,h,m)=>Math.floor(new Date(2026,9,d,h,m).getTime()/1000);
const S={
 bootAt:now()-(3*3600+12*60)*1000,fw:'0.9.3',build:'acc5ace 2026-10-15',heap:80312,cfgVer:7,events:1342,logLevel:2,
 wifi:{mode:'sta',ssid:'HomeWiFi',ip:'192.168.1.50',rssi:-58,apName:'MegaOpen-1A2B',apPass:'megaopen'},
 wifi2:{ssid:'iPhone'},
 nets:[{ssid:'HomeWiFi',rssi:-58,secure:true},{ssid:'iPhone',rssi:-47,secure:true},{ssid:'KYIVSTAR_7F2A',rssi:-79,secure:true}],
 set:defSet(),staged:{},
 ble:{bonded:true,connected:true,address:'00:11:22:33:44:55'},
 disp:{type:'carminat',funcReg:true,registered:true,phase:'Ready',sessionsLost:1,lastLoss:'AckTimeout',resyncAt:0},
 clk:{src:'',at:''},
 med:{auto:true,style:'classic',scroll:0,scrollMsTitle:700,scrollMsArtist:1400,splashMs:4000,pane:'media',paneAuto:true,paneSpeed:2,paneCycleS:8,ti:0,elapsed:63,playing:true,inj:null,custom:null},
 tracks:[['Нічний Київ','Океан Ельзи',241],['Плине кача','ДахаБраха',214],['Обійми','Океан Ельзи',258]],
 huTrack:['Радіо Рокс','Kozak System — Горить'],
 hu:{connected:true,rx:0,bad:0,trunc:0,txKey:418,txVeh:0,acks:0,learnedTitle:0x13,learnedArtist:0x14,count:0},
 emitter:false,tun:defTun(),map:defMap(),tests:{},
 can2:{setting:0,state:'locked',bitrate:500000,detected:500000,saved:500000,fps:850,frames:9123456,rec:0,tec:0,hwOverflows:0,ringPeak:212,restarts:0,quiet:false},
 cap:{recording:true,mode:'change',frames:1834220,written:6123456,dropped:0,markers:3,session:12,error:'',settings:{ring:true,fill:85,fileMB:4,autoProtect:true,mode:'change',autostart:true}},
 budget:10380902,
 files:[{f:'0009',session:9,part:0,size:1048576,wall:day(9,18,2),protected:true},{f:'0010',session:10,part:0,size:1048576,wall:day(14,8,11),protected:false},{f:'0011',session:10,part:1,size:734003,wall:day(14,8,24),protected:false},{f:'0012',session:12,part:0,size:612345,wall:0,protected:false,current:true}],
 sniff:false,ids:{},
 can:{state:'running',txErr:0,rxErr:0,busErr:0,arbLost:0,twaiRx:4410233,twaiTxOk:982113,twaiTxFail:0,rxMissed:0},
 affa:defAffa(),unm:{count:0,last:0,hold:false,known:true},affaEvents:0,last:{seq:0},
 routing:{uart:0x0F,ams:0x01,hid:0x02,res:0,usb:0x03},
 kn:defKnobs(),knl:{enc:{detents:0,errors:0,btn:false,clicks:0,holds:0,a:false,b:false,key:false,edges:[0,0,0]},enc2:{detents:0,errors:0,btn:false,clicks:0,holds:0,a:false,b:false,key:false,edges:[0,0,0]},led:{ok:true,frames:0,mA:64,mAWanted:64,busy:0},dropped:0},
 mac:defMacros(),macFires:Array(8).fill(0),macLive:{fired:17,swallowed:21,replayed:4,down:[]},
 ntf:[{uid:1,category:'Social',app:'Telegram',appId:'ph.telegra.Telegraph',title:'Олег',message:'Буду о 19:30'},{uid:2,category:'Navigation',app:'Карти',appId:'com.apple.Maps',title:'Поворот',message:'Через 300 м праворуч'}],
 logs:{used:412000,total:2097152,files:[48211,120044,96500,22010,180330,7700,64000,51022].map((s,n)=>({n,size:s}))},
 msgs:[['doors',0x24,250,true,'high'],['speed',0x7D,500,true,'high'],['rpm',0x7D,333,true,'high'],['outside temp',0x28,5000,true,'medium'],['steering',0x29,200,false,'medium'],['rear radar',0x1E,100,false,'low']]
  .map((m,i)=>({id:i,name:m[0],cmd:m[1],interval:m[2],enabled:m[3],conf:m[4],note:'',ready:i<4,bytes:'2E '+h2(m[1])+' 02 01 00 '+h2(0xDA-i)})),
 uartRing:[]
};
const bleOn=()=>S.set.bleMode!=='off', amsOn=()=>!!(C.bleModes.find(b=>b.id===S.set.bleMode)||{}).ams, hidOn=()=>['keyboard','both'].includes(S.set.bleMode);
const phone=()=>amsOn()&&S.ble.connected;
function mediaSrc(){const hu=S.set.uartEnabled&&S.hu.connected,s=S.set.mediaSource;
 if(s==='hu')return hu?'hu':'none';if(s==='ams')return phone()?'ams':'none';return hu?'hu':phone()?'ams':'none'}
function curTrack(){const m=S.med;if(m.inj)return m.inj;const s=mediaSrc();if(s==='hu'){const[a,b]=S.huTrack;return {title:a,artist:b,elapsed:0,duration:0,state:1,transport:false}}
 if(s==='ams'){const t=S.tracks[m.ti];return {title:t[0],artist:t[1],elapsed:m.elapsed,duration:t[2],state:m.playing?1:0,transport:true}}return {title:'',artist:'',elapsed:0,duration:0,state:0,transport:false}}
const clockStrOrEmpty=()=>S.set.autoTime&&(S.hu.connected&&S.set.uartEnabled||phone())?clockStr():'';
/* ---- текст щитка: як MediaScreen::compose (на платі це робить прошивка) ---- */
function preview(){
 const m=S.med,tr=curTrack(),l=lay(S.set.mediaLayout),w=l.cols;
 if(!tr.title)return l.rows===1?['MEGANE','','']:['','MEGANE',''];
 const sc=(s,ms)=>{if(!ms||s.length<=w)return s.slice(0,w);const t=s+'   ',o=Math.floor(now()/ms)%t.length;return (t+t).slice(o,o+w)};
 if(l.rows===1)return [sc(tr.title+' - '+tr.artist,m.scrollMsTitle),'',''];
 const Ti=sc(tr.title,m.scrollMsTitle),A=sc(tr.artist,m.scrollMsArtist),pos=tr.duration?fmtT(tr.elapsed)+'/'+fmtT(tr.duration):'',ck=clockStr();
 const bar=n=>{const p=tr.duration?Math.round(tr.elapsed/tr.duration*n):0;return '#'.repeat(p)+'-'.repeat(n-p)};
 const tick=Math.floor(now()/300);
 const r={classic:[(w>8?ck+' > '+pos+' ['+bar(2)+']':ck+' >'),Ti,A],minimal:[ck,Ti,A],bar:[Ti,A,w>8?bar(w-12)+' '+pos:pos||bar(w)],
  bigtitle:[tr.title.slice(0,w),tr.title.slice(w,2*w)||' ',A],ticker:[0,1,2].map(i=>sc(tr.title+' — '+tr.artist,(m.scrollMsTitle||700)/(i+1))),
  split:[A,Ti,pos],pulse:[Ti,A,' '.repeat(tick%w)+'*'],scanner:[Ti,A,' '.repeat(Math.abs(tick%(2*w-2)-(w-1)))+'█'],clock:[ck.padStart(Math.floor((w+5)/2)),Ti,A],
  vu:[Ti,A,'▮'.repeat(1+Math.floor(Math.random()*(w-1)))],flow:[(w>8?ck+' > '+pos+' ['+bar(2)+']':ck),tr.title.slice(0,w),tr.title.slice(w,2*w)||A],
  plain:[Ti,A,tr.album||''],winamp:[ck+' '+Array.from({length:w},()=>' .:|'[Math.floor(Math.random()*4)]).join(''),sc(tr.artist+' - '+tr.title+' *** ',m.scrollMsTitle||300),tr.album||''],
  tape:(()=>{const c=w>=16?ck+' ':'',n=Math.max(0,w-c.length-6),sp=tr.transport!==false&&tr.state===1&&Math.floor(now()/250)%2?'x':'+',hd=tr.duration?Math.min(n-1,Math.floor(tr.elapsed/tr.duration*n)):-1;
   return [c+'('+sp+')'+Array.from({length:n},(_,i)=>i===hd?'o':'=').join('')+'('+sp+')',Ti,A]})(),
  typewriter:[A,Ti,(w>8?ck+' > '+pos+' ['+bar(2)+']':ck)]}[m.style]||[Ti,A,''];
 return r.map(x=>String(x).slice(0,w));
}
/* ---- картинка 48×48 сцени (MediaPane); на платі її малює прошивка ---- */
function sceneBits(id,t){const p=new Uint8Array(2304),set=(x,y)=>{x=Math.round(x);y=Math.round(y);if(x>=0&&x<48&&y>=0&&y<48)p[y*48+x]=1},circ=(cx,cy,r)=>{for(let a=0;a<64;a++)set(cx+r*Math.cos(a/64*6.283),cy+r*Math.sin(a/64*6.283))};
 const f=t/400;
 switch(id){
  case 'media':for(let y=8;y<34;y++){set(30,y);set(31,y)}for(let x=31;x<40;x++){set(x,8+(x-31)/2);set(x,9+(x-31)/2)}for(let a=0;a<6;a++)circ(26,34,a);break;
  case 'vu':for(let b=0;b<8;b++){const hh=4+Math.abs(Math.sin(f+b))*36;for(let y=46;y>46-hh;y--)for(let x=b*6;x<b*6+4;x++)set(x,y)}break;
  case 'wave':for(let x=0;x<48;x++){set(x,24+16*Math.sin(x/7+f));set(x,24+8*Math.sin(x/4-f))}break;
  case 'clock':{circ(24,24,22);const d=new Date(),hA=(d.getHours()%12+d.getMinutes()/60)/12*6.283-1.571,mA=d.getMinutes()/60*6.283-1.571;for(let r=0;r<18;r++){set(24+r*Math.cos(mA),24+r*Math.sin(mA))}for(let r=0;r<12;r++)set(24+r*Math.cos(hA),24+r*Math.sin(hA));break}
  case 'stars':for(let s=0;s<40;s++){const a=(s*97)%48,b=(s*57+Math.floor(f*3)*s)%48;set(a,b)}break;
  case 'bounce':{const x=Math.abs((f*9)%88-44)+2,y=46-Math.abs(Math.sin(f*1.3))*40;circ(x,y,3);circ(x,y,2);break}
  case 'rings':for(let r=2;r<24;r+=5)circ(24,24,(r+f*4)%24);break;
  case 'spiral':for(let a=0;a<220;a++){const r=a/10;set(24+r*Math.cos(a/12+f),24+r*Math.sin(a/12+f))}break;
  case 'scope':for(let x=0;x<48;x++)set(x,24+18*Math.sin(x/5+f*2)*Math.cos(x/17));for(let x=0;x<48;x+=4)set(x,24);break;
  case 'rain':for(let s=0;s<30;s++){const x=(s*37)%48,y=((s*13)+f*12)%48;set(x,y);set(x,y+1)}break;
  case 'radar':{circ(24,24,22);circ(24,24,12);const a=f*1.5;for(let r=0;r<22;r++)set(24+r*Math.cos(a),24+r*Math.sin(a));break}
  case 'life':for(let y=0;y<48;y+=2)for(let x=0;x<48;x+=2)if(((x*7+y*13+Math.floor(f))%11)<3){set(x,y);set(x+1,y);set(x,y+1);set(x+1,y+1)}break;
  case 'tape':{const tr=curTrack(),R=tr.duration?Math.min(7,Math.floor(tr.elapsed/tr.duration*7)):3,L=7-R,a=f*2;
   for(let x=0;x<48;x++){set(x,4);set(x,44);set(x,6)}for(let y=4;y<45;y++){set(0,y);set(47,y)}
   [[15,L],[32,R]].forEach(([cx,pk])=>{circ(cx,20,4);circ(cx,20,5+pk);for(let k=0;k<3;k++){const q=a+k*2.094;for(let r=1;r<5;r++)set(cx+r*Math.sin(q),20-r*Math.cos(q))}});
   for(let x=13;x<35;x++)set(x,36);for(let x=15;x<33;x++)set(x,38);break}
  case 'tunnel':{const vx=23+7*Math.sin(f*0.3),vy=23+7*Math.cos(f*0.5);for(let i=0;i<8;i++){const ph=(i*12+f*8)%96,h=Math.round(3*2**(ph/24)),n=Math.min(24,h)/24,x=vx+(23-vx)*n,y=vy+(23-vy)*n;
   for(let k=-h;k<=h;k++){set(x+k,y-h);set(x+k,y+h);set(x-h,y+k);set(x+h,y+k)}}break}
  case 'custom':if(S.med.custom)p.set(S.med.custom);break}
 return p}
const liveScene=()=>{const m=S.med;return m.paneAuto?CAROUSEL[Math.floor(now()/1000/m.paneCycleS)%CAROUSEL.length]:m.pane};
/* ---- лог плати: кільце з номерами рядків (/api/logs?since=, LogRing.h) ----
   t — мс від старту; tag — лише з каталогу (logTags), як у прошивці (Log::TAG_ROWS). */
const RING=[],SIM_BOOT=Math.floor(Math.random()*2**32);let logSeq=0;
function emit(l,tag,msg){if(l>S.logLevel)return;if(!C.logTags.includes(tag))console.warn('mock log: tag not in catalog',tag);
 RING.push({n:++logSeq,t:now()-S.bootAt,l:LVL[l],tag,msg});if(RING.length>300)RING.shift()}
/* як logLines() у WebUi.cpp: після since, до 200 рядків; lost — витіснені до того, як їх забрали */
function logsSince(since){if(since>logSeq)since=0;const old=RING.length?RING[0].n:logSeq+1,lost=Math.max(0,old-since-1),
 lines=RING.filter(l=>l.n>since).slice(0,200),next=lines.length?lines[lines.length-1].n:Math.max(since,old-1);return {lines,next,lost,more:next<logSeq,dropped:0,boot:SIM_BOOT}}
/* ---- маршрути мок-API ---- */
const R={},g=(p,f)=>R['GET '+p]=f,po=(p,f)=>R['POST '+p]=f;
/* помилки прошивки: text/plain, тіло — сам текст */
const fail=(st,text)=>({__status:st,body:String(text)});
const ok={ok:true},queued=()=>({__status:202,body:{queued:true,note:'queued for the display task; the result is reported in the log'}});
/* НОВЕ (зріз 5): рівно dest із каталогу; usb лише там, де він є. press: приймач бере лише Press (BLE, USB), не Long */
function destState(){const o={
 uart:{mask:S.routing.uart,live:S.set.uartEnabled&&(!C.features.display||S.hu.connected),why:S.set.uartEnabled?'head unit silent (link timeout)':'uartEnabled is off'},
 ams:{mask:S.routing.ams,live:amsOn(),why:'bleMode has no AMS role',press:true},
 hid:{mask:S.routing.hid,live:hidOn(),why:'bleMode has no keyboard role',press:true},
 res:{mask:0,live:false,why:'FEATURE_RESISTIVE_KEYS is off in this build'}};
 if(DESTS.includes('usb'))o.usb={mask:S.routing.usb,live:true,why:'',press:true};
 /* як Keys::volumeToAms: гучність, яку ніхто живий не бере, іде в iPhone */
 o.ams.volFallback=o.ams.live&&!(o.ams.mask&2)&&!Object.entries(o).some(([k,x])=>k!=='ams'&&x.live&&x.mask&2);return o}
/* як WebUi::putDests: /api/health keys.dest */
const strip=o=>{const x={...o};if(x.live)delete x.why;delete x.press;x.state=!o.live?'off':o.mask?'ok':'muted';return x};
/* НОВЕ (зріз 4): кожен ланцюг має стабільний code; W — ті самі пари [code, why], що в WebUi.cpp */
const W={panel:[['panel_unregistered','registration lost — panel is not completing the handshake'],['panel_ok','']],
 media:[['media_off','media screen is switched off (mediaScreen)'],['media_no_panel','panel is not registered — nothing can be drawn'],['media_no_phone','no phone connected'],['media_no_track','phone connected but has reported no track yet'],['media_ok','']],
 clock:[['clock_off','autoTime is off'],['clock_no_phone','no phone — CTS is the only time source'],['clock_waiting','phone connected, no time received yet'],['clock_ok','']],
 keys:[['keys_ok',''],['keys_none','no key event since boot']],ancs:[['ancs_ok',''],['ancs_no_phone','no phone connected'],['ancs_waiting','connected, nothing pushed yet']],
 hu:[['hu_off','uartEnabled is off'],['hu_silent','no frame from the head unit since boot'],['hu_quiet','head unit has gone quiet (link timeout)'],['hu_ok',''],['hu_seen','frames since boot; this build cannot tell how recent']],
 phone:[['phone_ok',''],['phone_ble_off','bleMode is off'],['phone_advertising','advertising — not connected']]};
function health(){
 const d=S.disp,ch=[],chain=(id,state,i,x)=>{const[code,why]=W[id][i],o={id,state,code,...(x||{})};if(why)o.why=why;ch.push(o);return o};
 const reg=d.registered,tr=curTrack(),ck=clockStrOrEmpty(),F=C.features;
 if(F.display){chain('panel',reg?'ok':'broken',reg?1:0,{rxFrames:S.can.twaiRx,rxPin:6,txPin:7,resyncs:d.sessionsLost,txGated:false});
 const mi=!S.med.auto?0:!reg?1:!phone()&&mediaSrc()!=='hu'?2:!tr.title?3:4;
 chain('media',!S.med.auto?'off':mi<4?'idle':'ok',mi,{title:tr.title,artist:tr.artist,clock:ck,delivered:1834,superseded:12,failed:0});
 chain('clock',!S.set.autoTime?'off':ck?'ok':'idle',!S.set.autoTime?0:!phone()?1:!ck?2:3,{hhmm:ck})}
 const k=chain('keys',S.affaEvents?'ok':'idle',S.affaEvents?0:1,{events:S.events,dest:{}});
 for(const[id,x]of Object.entries(destState()))k.dest[id]=strip(x);
 if(!F.ancs)ch.push({id:'ancs',state:'off',code:'ancs_off',why:'FEATURE_ANCS is off in this build'});
 else chain('ancs',S.ntf.length&&phone()?'ok':'idle',S.ntf.length&&phone()?0:!phone()?1:2,{count:S.ntf.length,popup:S.set.ancsPopup});
 const hi=!S.set.uartEnabled?0:!F.display?(S.hu.rx?4:1):!S.hu.connected?(S.hu.rx?2:1):3;
 chain('hu',hi===0?'off':hi===3?'ok':'idle',hi,{rxFrames:S.hu.rx,junk:0,badCs:0,dialect:S.hu.rx?'toyota':'unknown',notSupported:0});
 if(F.knobs){const kl=S.knl||{},e=kl.enc||{},e2=kl.enc2||{};
  ch.push({id:'knobs',state:'ok',code:'knobs_ok',addr:0x3F,i2cErrors:0,right:e.pos||0,left:e2.pos||0,clicks:(e.clicks||0)+(e2.clicks||0),lineErrors:0});
  const on=S.kn.led&&S.kn.led.enabled;ch.push(on?{id:'lights',state:'ok',code:'lights_ok',leds:10,frames:0}:{id:'lights',state:'off',code:'lights_off',why:'led.enabled is off'})}
 chain('phone',phone()?'ok':'idle',phone()?0:S.set.bleMode==='off'?1:2);
 return {fw:S.fw,build:S.build,chains:ch,bleMode:S.set.bleMode};
}
g('/api/catalog',()=>deep(CATALOG));
g('/api/status',()=>{const w=S.wifi;return {ip:w.ip,mode:w.mode,ssid:w.mode==='sta'?w.ssid:w.apName,rssi:w.mode==='sta'?w.rssi:0,uptimeMs:now()-S.bootAt,framesSent:S.hu.txKey,events:S.events,ghosts:0,encSteps:0,heap:S.heap,
 fw:S.fw,build:S.build,apName:w.apName}});   /* fw, build, apName — НОВЕ */
g('/api/health',health);
/* прошивка зберігає в cfg одразу, тож GET бачить збережене; діє воно після перезавантаження */
/* кожен id реєстру, якого нема в defSet, — з першого значення (enum) або min (int) */
/* str (raise.version) has no enum: '' until RAISE fills it */
const regDef=f=>f.type==='bool'?false:f.type==='int'?f.min:f.type==='str'?'':f.enum[0].v;
/* cap.* реєстру живуть у S.cap.settings, як у прошивці — у рекордера (свій простір NVS) */
const CAPK=['ring','fill','fileMB','autoProtect','mode','autostart'];
const mRouteId=d=>'route'+d[0].toUpperCase()+d.slice(1);
/* raise.* реєстру = S.tun (Canbox::Tuning): [ключ S.tun, у значення реєстру, назад] */
const FUELID={2:'l100km',1:'kml',0:'mpg'},TRIPID={2:'km',1:'miles',0:'hide'},byVal=(o,v)=>+Object.keys(o).find(k=>o[k]===v);
const RAISE={doorsLen:[x=>x,x=>x],cmd28:[x=>x?'ac':'temp',x=>x==='ac'?1:0],steerInvert:[x=>!!x,x=>x?1:0],steerDiv:[x=>x,x=>x],steerClamp:[x=>x,x=>x],
 fuelUnit:[x=>FUELID[x],x=>byVal(FUELID,x)],tripUnit:[x=>TRIPID[x],x=>byVal(TRIPID,x)],tripFields:[x=>!!x,x=>x?1:0],histLen:[x=>x,x=>x],radarInvert:[x=>!!x,x=>x?1:0],
 version:[x=>String(x).trimEnd(),x=>String(x).padEnd(16,' ').slice(0,16)]};
g('/api/settings',()=>{for(const f of C.settings||[])if(f.type!=='group'&&f.type!=='table'&&!(f.id in S.set))S.set[f.id]=regDef(f);
 const st=S.staged,o={...S.set,alerts:S.alerts.map(a=>a.slice()),fw:S.fw,build:S.build,cfgVer:S.cfgVer,cfgVerWas:0,wifiSleep:false,ssid2:S.wifi2.ssid};   /* ssid2 — НОВЕ */
 for(const k of CAPK)o['cap.'+k]=S.cap.settings[k];
 o.logLevel=LVL[S.logLevel];   /* logLevel: лише в RAM, як у прошивці */
 for(const d of DESTS)o[mRouteId(d)]=S.routing[d]|0;   /* route*: маски призначень (S.routing, як destState) */
 o.huMessages=S.msgs.map(m=>[!!m.enabled]);   /* таблиця huMessages: лише в RAM */
 for(const[k,[to]]of Object.entries(RAISE))o['raise.'+k]=to(S.tun[k]);
 if(st.bleMode)o.bleMode=st.bleMode;if(st.wifi)o.ssid=st.wifi.ssid;return o});
/* НОВЕ (план, зріз 4): перезапуск не побічний ефект, а pendingReboot; apPass */
po('/api/settings',q=>{const s=S.set,ap=q.apPass===undefined?'':String(q.apPass);
 if(ap&&(ap.length<8||ap.length>63||ap==='megaopen'))return fail(400,'apPass: 8 to 63 characters, and not the public default');
 let rb=typeof q.mdnsOn==='boolean'&&q.mdnsOn!==s.mdnsOn;
 /* таблиці реєстру (WebUi.cpp): рядок = клітинки в порядку колонок; рядок чи клітинка, яких не прислали
    (або null), лишаються як були; ті самі тексти помилок */
 let AL=null;if(q.alerts!==undefined){if(!Array.isArray(q.alerts)||q.alerts.length>16)return fail(400,'alerts: a list of 16 rows at most');AL=[];
  for(let i=0;i<q.alerts.length;i++){const a=q.alerts[i],id='alerts '+(i+1)+': ',old=S.alerts[i];if(!Array.isArray(a))return fail(400,id+'a list of cells');
   const[sig,cond,thr,cd,on]=old.map((x,k)=>a[k]==null?x:a[k]),m=META.find(x=>x[0]===sig);
   if(sig&&!m)return fail(400,id+'sig: unknown value');if(!COND.some(c=>c[0]===cond))return fail(400,id+'cond: unknown value');
   if(typeof thr!=='number')return fail(400,id+'thr: a number');if(m&&(toInt(thr,m[2])<m[5]||toInt(thr,m[2])>m[6]))return fail(400,id+'thr: out of range');
   if(!(isInt(cd)&&cd<=65535))return fail(400,id+'cooldownS: '+(typeof cd==='number'?'out of range':'a number'));
   if(typeof on!=='boolean')return fail(400,id+'on: true or false');AL.push([sig,cond,thr,Math.max(5,cd),!!on&&!!sig])}}
 let MG=null;if(q.huMessages!==undefined){if(!Array.isArray(q.huMessages)||q.huMessages.length>S.msgs.length)return fail(400,'huMessages: a list of '+S.msgs.length+' rows at most');
  MG=q.huMessages.map((a,i)=>Array.isArray(a)&&a[0]!=null?a[0]:S.msgs[i].enabled);
  const bad=MG.findIndex(x=>typeof x!=='boolean');if(bad>=0)return fail(400,'huMessages '+(bad+1)+': on: true or false')}
 /* реєстр (catalog.settings): невідоме значення enum -> 400, число в межі, як прошивка */
 for(const f of C.settings||[]){if(f.type==='group'||f.type==='table'||q[f.id]===undefined||['bleMode','mediaSource','huTitleCmd','huArtistCmd'].includes(f.id))continue;
  if(f.ro)return fail(400,f.id+': read-only');
  if(f.type==='enum'&&!f.enum.some(o=>o.v===q[f.id]))return fail(400,f.id+': unknown value');
  if(f.type==='str'&&!(typeof q[f.id]==='string'&&q[f.id].length<=f.max&&/^[ -~]*$/.test(q[f.id])))return fail(400,f.id+': printable ASCII text, too long, or not text');
  if(f.reboot&&q[f.id]!==s[f.id])rb=true;
  s[f.id]=f.type==='int'?Math.min(f.max,Math.max(f.min,+q[f.id])):q[f.id]}
 if(q.logLevel!==undefined){S.logLevel=LVL.indexOf(q.logLevel);delete s.logLevel}
 for(const[k,[,back]]of Object.entries(RAISE))if(q['raise.'+k]!==undefined){S.tun[k]=back(s['raise.'+k]);delete s['raise.'+k]}
 for(const d of DESTS){const k=mRouteId(d);if(q[k]!==undefined){S.routing[d]=s[k]&0x0F;delete s[k]}}
 for(const k of CAPK)if(q['cap.'+k]!==undefined){S.cap.settings[k]=s['cap.'+k];delete s['cap.'+k];if(k==='mode')S.cap.mode=S.cap.settings.mode}
 if(typeof q.mediaSource==='string'&&['ams','hu','auto'].includes(q.mediaSource))s.mediaSource=q.mediaSource;
 if(typeof q.huTitleCmd==='number'||typeof q.huArtistCmd==='number'){if(typeof q.huTitleCmd==='number')s.huTitleCmd=q.huTitleCmd;if(typeof q.huArtistCmd==='number')s.huArtistCmd=q.huArtistCmd}
 if(q.huForget){S.hu.learnedTitle=0;S.hu.learnedArtist=0}
 if(q.uartEnabled===false)S.emitter=false;
 if(typeof q.bleMode==='string'&&C.bleModes.some(b=>b.id===q.bleMode)){rb=rb||q.bleMode!==s.bleMode;S.staged.bleMode=q.bleMode}
 if(ap){rb=rb||ap!==(S.staged.apPass||S.wifi.apPass);S.staged.apPass=ap}
 if(AL){S.alerts=S.alerts.map((old,i)=>AL[i]||old);emit(2,'CAN','[alert] rules saved: '+S.alerts.filter(a=>a[4]).length+' on')}
 if(MG)MG.forEach((x,i)=>S.msgs[i].enabled=x);
 emit(2,'NET',rb?'settings saved, pending reboot':'settings saved');return rb?{ok:true,pendingReboot:true}:ok});
/* файл налаштувань (WebUi.cpp /api/config): налаштування без ro/volatile + ручки; POST — ті самі перевірки, ?dry=1 нічого не міняє */
const fileSettings=()=>{const all=R['GET /api/settings'](),o={};for(const f of C.settings||[])if(f.type!=='group'&&!f.ro&&!f.volatile&&f.id in all)o[f.id]=all[f.id];return o};
g('/api/config',()=>{const o={format:'megaopen-config',schema:1,fw:S.fw,build:S.build,settings:fileSettings()};if(C.features.knobs)o.knobs=R['GET /api/knobs']();return o});
po('/api/config',q=>{if(!q||q.format!=='megaopen-config'||typeof q.settings!=='object')return fail(400,'not a MegaOpen settings file');
 if(q.schema>1)return fail(400,'saved by a newer firmware (schema too new): update the board first');
 if(q.schema<1)return fail(400,'saved by an older firmware whose settings this one cannot read');
 const bad=['apPass','huForget'].filter(k=>k in q.settings);if(bad.length)return fail(400,bad.map(k=>k+': not allowed in a settings file').join('\n'));
 const set={};for(const k in q.settings)if(!(C.settings||[]).some(f=>f.id===k&&f.volatile)&&q.settings[k]!==null)set[k]=q.settings[k];
 const was=fileSettings(),unknown=Object.keys(q.settings).filter(k=>!(C.settings||[]).some(f=>f.id===k)).length;
 if(q.dry==='1'){const snap=JSON.parse(JSON.stringify(S));const r=R['POST /api/settings'](set);for(const k of Object.keys(S))delete S[k];Object.assign(S,snap);
  if(r&&r.__status)return r;
  const changes=Object.keys(set).filter(k=>k in was&&JSON.stringify(was[k])!==JSON.stringify(set[k]));
  return {ok:true,changes,reboot:changes.some(k=>((C.settings||[]).find(f=>f.id===k)||{}).reboot),unknown,skipped:[],knobs:!!(q.knobs&&C.features.knobs),clamped:[],fileFw:q.fw||''}}
 const r=R['POST /api/settings'](set);if(r&&r.__status)return r;if(q.knobs&&C.features.knobs)R['POST /api/knobs'](q.knobs);emit(2,'NET','settings set from a file');return r});
g('/api/display',()=>{const d=S.disp,car=d.type==='carminat';return {type:d.type,funcReg:d.funcReg,canRx:S.can.twaiRx,attached:true,sync:d.registered?15:1,synced:d.registered,registered:d.registered,passive:!d.funcReg,busy:false,queued:0,lastResult:'Ok',
 supports:{text:true,time:car,power:car,menu:car,popup:car,fullscreen:car,confirm:car,info:car},phase:d.phase,lastRendered:'text',lastRenderedMs:1200,glassHoldMs:10000,glassHeldMs:0,
 sessionsLost:d.sessionsLost,lastLoss:d.lastLoss,linkFlaps:0,linkRecoveries:0,linkDownMs:0,txGated:false,txGates:0,reinstalls:0,failStreak:0,rendersPosted:412,rendersRefused:0}});
po('/api/display',q=>{const st=S.staged,t0=st.dispType||S.disp.type,f0=st.funcReg!==undefined?st.funcReg:S.disp.funcReg;
 const t=q.type===undefined?t0:q.type,f=q.funcReg===undefined?f0:!!q.funcReg;
 if(!C.panels.some(p=>p.id===t))return fail(400,'type: unknown panel');
 st.dispType=t;st.funcReg=f;return t!==t0||f!==f0?{ok:true,pendingReboot:true}:ok});
for(const k of ['text','clock','power','menu','highlight','popup','confirm','msgbox','selectbutton','styled','info','fullscreen','abort'])po('/api/display/'+k,q=>{
 if(k==='text'&&q.text===undefined)return fail(400,'text?');if(k==='clock'&&q.time===undefined)return fail(400,'time? (HHMM)');if(k==='popup'&&q.text===undefined&&!B(q.hide))return fail(400,'text? or hide=1');
 if(k==='menu'&&q.scroll!==undefined&&!ARROWS.includes(+q.scroll))return fail(400,SCROLL_WHY);
 /* НОВЕ: справжній HHMM записує джерело manual (WebUi → App::Ui::noteManualClock) */
 if(k==='clock'&&/^\d{4}$/.test(q.time)&&+q.time.slice(0,2)<24&&+q.time.slice(2)<60)clockFrom('manual',q.time.slice(0,2)+':'+q.time.slice(2));
 emit(2,'PNL',k+' '+JSON.stringify(q));return queued()});
for(const k of ['release','notify','clear'])po('/api/display/'+k,()=>ok);
/* лише для розробника (WebUi.cpp, display/InfoRaw.h): 403 без devMode; len 0x0B..0x13 обрізається, offset/prefix поза межами — 400 */
po('/api/display/inforaw',q=>{if(!S.set.devMode)return fail(403,'developer mode is off (Плата › Інтерфейс)');const o=parseInt(q.offset??'0x41'),p=parseInt(q.prefix??'0x60');
 if(!(o>=0x40&&o<=0x4F))return fail(400,'offset: 0x40..0x4F');if(!(p>=0x40&&p<=0x7F))return fail(400,'prefix: 0x40..0x7F');emit(2,'PNL','inforaw '+JSON.stringify(q));return queued()});
/* НОВЕ: last у /api/affamap, як publishKeyLast у main.cpp: key — рядок таблиці (і порожній), action — id дії;
   рядок без дії (None) не відображено, свій код без дії теж; dests — маска і живий приймач, ГУ ще й має код */
function pressKey(wire,hold){const r=S.affa.find(x=>x.wire===wire&&x.hold===!!hold),key=r?r.key:'';
 if(!r||r.action==='None'){S.unm={count:S.unm.count+1,last:wire,hold:!!hold,known:!!r};S.last={seq:S.last.seq+1,key,wire,hold:!!hold,action:'None',dests:[]};
  emit(1,'KEY','[panelkey] '+(r?r.key+(hold?' H':'')+' > no action':'key '+wire.toString(16).padStart(4,'0').toUpperCase()+(hold?' H':'')+' > unknown'));return}
 S.events++;S.affaEvents++;
 const a=C.actions.find(x=>x.id===r.action)||{},cl=a.class,isLong=r.asLong&&!/^Enc/.test(r.action),ds=destState(),
  to=DESTS.filter(d=>{const x=ds[d];return x&&x.live&&cl!=null&&(x.mask&(1<<cl))&&!(isLong&&x.press)&&(d!=='uart'||r.code||a.hu)&&(d!=='usb'||cl<2)});
 S.last={seq:S.last.seq+1,key,wire,hold:!!hold,action:r.action,dests:to};S.macLive.down=[r.action];
 emit(2,'KEY','[panelkey] '+r.key+(hold?' H':'')+' > '+(to.includes('uart')?h2(r.code||parseInt(a.hu,16))+(isLong?' LONG':' PRESS'):'not routed'))}
po('/api/display/key',q=>{if(q.code===undefined)return fail(400,'code? (hex wire code)');const code=Number(q.code);
 /* НОВЕ: wire=1 — кадр у CAN B (KeySource::Wire); 409, поки зв'язку з панеллю нема (synced) */
 if(B(q.wire)&&B(q.hold)&&(WHEEL.find(k=>parseInt(k.wire,16)===code)||{}).wireHold===false)return fail(400,'a held detent has no wire form');
 if(B(q.wire)){if(!S.disp.registered)return fail(409,'panel link is not up');emit(2,'PNL','TX key frame 0x'+code.toString(16)+(B(q.hold)?' hold':'')+' (KeySource::Wire)');return queued()}
 pressKey(code,B(q.hold));return queued()});
po('/api/display/resync',()=>{S.disp.registered=false;S.disp.resyncAt=now()+3000;S.disp.phase='Handshake';emit(2,'PNL','resync requested');return {ok:true,note:'handshake restarting; watch registered'}});
g('/api/ble',()=>({mode:S.set.bleMode==='off'?'ams':S.set.bleMode,connected:S.ble.connected,status:S.ble.connected?'connected':'advertising',bonded:S.ble.bonded,address:S.ble.connected?S.ble.address:'',
 ams:{active:amsOn(),connected:S.ble.connected,secured:S.ble.connected,address:S.ble.connected?S.ble.address:'',svc:{ams:true,ancs:true,cts:true}},hid:{active:hidOn(),connected:false,address:''}}));
po('/api/ble/unbond',()=>{S.ble.bonded=false;S.ble.connected=false;emit(1,'BLE','all bonds cleared');return {ok:true,note:'forget it on the phone as well'}});
g('/api/notifications',()=>({popup:S.set.ancsPopup,items:phone()?S.ntf:[]}));
g('/api/media',()=>{const m=S.med,tr=curTrack(),l=lay(S.set.mediaLayout),sc=liveScene();
 return {auto:m.auto,title:tr.title,artist:tr.artist,clock:clockStrOrEmpty(),clockSrc:S.clk.src,clockAt:S.clk.at,state:tr.state,elapsed:tr.elapsed,duration:tr.duration,cols:l.cols,bar:10,scrollMs:m.scrollMsTitle,scrollMsTitle:m.scrollMsTitle,scrollMsArtist:m.scrollMsArtist,
  layout:S.set.mediaLayout,splashMs:m.splashMs,style:m.style,scroll:m.scroll,hasTransport:tr.transport,
  source:S.set.mediaSource,sourceActive:mediaSrc(),sourceLive:mediaSrc()!=='none',sourceStatus:'',
  huFrames:S.hu.rx,huTextFrames:Math.floor(S.hu.rx/3),huLearnedTitle:S.hu.learnedTitle,huLearnedArtist:S.hu.learnedArtist,huTitleCmd:S.set.huTitleCmd||S.hu.learnedTitle,huArtistCmd:S.set.huArtistCmd||S.hu.learnedArtist,huTitleFirst:S.set.huTitleFirst,
  huTitle:S.huTrack[0],huArtist:S.huTrack[1],phone:phone(),autoTime:S.set.autoTime,bleMode:S.set.bleMode,preview:preview(),
  pane:{scene:sc,on:sc!=='off',auto:m.paneAuto,speed:m.paneSpeed,cycleS:m.paneCycleS,visible:l.pane,...(l.pane||sc==='off'?{}:{hiddenBy:l.id}),bits:px2hex(sceneBits(sc,now()))}}});
po('/api/media',q=>{const m=S.med;
 if(Number.isInteger(q.scroll)&&!ARROWS.includes(q.scroll))return fail(400,SCROLL_WHY);
 if(typeof q.title==='string'){m.inj={title:q.title,artist:q.artist||'',state:1,elapsed:+q.elapsed||0,duration:+q.duration||0,transport:true};setTimeout(()=>m.inj=null,30000)}
 /* НОВЕ: bits= — свій малюнок 48×48 */
 if(q.bits!==undefined){if(!/^[0-9a-f]{576}$/i.test(q.bits))return fail(400,'bits: 576 hex characters');m.custom=hex2px(q.bits);m.pane='custom';m.paneAuto=false}
 for(const k of ['scrollMsTitle','scrollMsArtist'])if(typeof q[k]==='number')m[k]=q[k];if(Number.isInteger(q.scroll))m.scroll=q.scroll;
 if(typeof q.splashMs==='number')m.splashMs=q.splashMs<=0?0:Math.min(20000,Math.max(1000,q.splashMs));
 if(typeof q.paneCycleS==='number')m.paneCycleS=Math.min(600,Math.max(5,q.paneCycleS));
 if(typeof q.paneSpeed==='number')m.paneSpeed=Math.min(6,Math.max(1,q.paneSpeed));
 if(typeof q.layout==='string')S.set.mediaLayout=C.layouts.some(l=>l.id===q.layout)?q.layout:'fullscreen';
 if(typeof q.style==='string')m.style=C.styles.some(s=>s.id===q.style)?q.style:'classic';
 if(typeof q.auto==='boolean')m.auto=q.auto;
 if(typeof q.autoTime==='boolean')S.set.autoTime=q.autoTime;
 if(typeof q.pane==='string'){if(q.pane==='auto')m.paneAuto=true;else{if(q.pane==='custom'&&!m.custom)return fail(409,'draw the custom picture first');m.paneAuto=false;m.pane=C.scenes.some(s=>s.id===q.pane)?q.pane:'off'}}
 if(q.splashNow)emit(2,'PNL','splash');
 return ok});
g('/api/affamap',q=>{const b={unmapped:S.unm,events:S.affaEvents,last:S.last};if(B(q.live))return b;
 return {rows:S.affa.map(r=>{const eff=r.code||parseInt((C.actions.find(a=>a.id===r.action)||{hu:''}).hu,16)||0;return {...r,effective:eff,effectiveName:(C.huKeys.find(k=>parseInt(k.code,16)===eff)||{name:'?'}).name}}),...b}});
po('/api/affamap',q=>{let n=0;for(const r of q.rows||[]){const x=S.affa[r.index];if(!x)continue;x.action=C.actions.some(a=>a.id===r.action)?r.action:'None';x.code=(r.code|0)&255;x.asLong=!!r.asLong;n++}emit(2,'KEY','stalk mapping saved ('+n+' rows)');return {ok:true,applied:n}});
po('/api/affamap/defaults',()=>{S.affa=defAffa();return ok});
po('/api/wifi/scan',()=>({networks:S.nets.map(n=>({...n,rssi:Math.round(n.rssi+rnd(-3,3))})),count:S.nets.length,current:S.wifi.mode==='sta'?S.wifi.ssid:''}));
/* НОВЕ (зріз 4): ssid2/pass2; порожній пароль = той самий, поки назва та сама; pendingReboot, коли щось змінилось.
   pendingReboot лише за першу мережу; друга (WifiManager::setNet2) діє без перезавантаження, з наступної спроби з точки */
po('/api/wifi',q=>{const h1=typeof q.ssid==='string',h2=typeof q.ssid2==='string',bad=p=>p&&(p.length<8||p.length>64);
 if(!h1&&!h2)return fail(400,'ssid? or ssid2?');if((q.ssid||'').length>32||(q.ssid2||'').length>32)return fail(400,'ssid: 32 characters at most');
 if(bad(q.pass||'')||bad(q.pass2||''))return fail(400,'pass: 8 to 64 characters, or empty');
 const w1=S.staged.wifi||{ssid:S.set.ssid,pass:S.wifi.pass||''},ch=h1&&(q.ssid!==w1.ssid||!!q.pass&&q.pass!==w1.pass);
 if(h1)S.staged.wifi={ssid:q.ssid,pass:q.pass||(q.ssid===w1.ssid?w1.pass:'')};if(h2)S.wifi2.ssid=q.ssid2;
 emit(2,'NET',ch?'wifi saved, pending reboot':'wifi saved');return ch?{ok:true,pendingReboot:true}:ok});
function doBoot(){S.bootAt=now()+4000;S.heap=Math.round(rnd(79000,82000));const st=S.staged;
 if(st.wifi){S.set.ssid=st.wifi.ssid;S.wifi.pass=st.wifi.pass}
 if(st.bleMode)S.set.bleMode=st.bleMode;
 if(st.apPass)S.wifi.apPass=st.apPass;if(st.dispType)S.disp.type=st.dispType;if(st.funcReg!==undefined)S.disp.funcReg=st.funcReg;S.staged={};
 const n=S.nets.find(x=>x.ssid===S.set.ssid)||S.nets.find(x=>x.ssid===S.wifi2.ssid);
 if(n){S.wifi.mode='sta';S.wifi.ssid=n.ssid;S.wifi.ip=n.ssid==='HomeWiFi'?'192.168.1.50':'172.20.10.4';S.wifi.rssi=n.rssi}else{S.wifi.mode='ap';S.wifi.ip='192.168.4.1'}
 S.disp.registered=false;S.disp.resyncAt=now()+6000;S.cap.recording=S.cap.settings.autostart;S.emitter=false;S.tests={}}
po('/api/reboot',()=>{setTimeout(doBoot,200);return ok});   /* НОВЕ */
po('/api/factory',q=>{if(q.confirm!=='factory')return {__status:400,body:{ok:false,error:'factory reset WIPES NVS including WiFi credentials; the board returns in AP mode. Re-send with ?confirm=factory if that is what you want.'}};
 S.set=defSet();S.set.ssid='';S.affa=defAffa();S.mac=defMacros();S.kn=defKnobs();S.tun=defTun();S.map=defMap();S.routing={uart:0x0F,ams:0x05,hid:0x02,res:0,usb:0x03};
 S.wifi2={ssid:''};S.wifi.apPass='megaopen';S.ble={bonded:false,connected:false,address:''};S.staged={};setTimeout(doBoot,200);return {ok:true,note:'AP mode after reboot'}});
g('/api/can2',()=>{const c=S.can2;return {live:!c.quiet,listenOnly:true,autobaud:c.setting?'manual':c.quiet?'searching':'locked',bitrate:c.bitrate,detected:c.quiet||c.setting?0:c.detected,setting:c.setting,saved:c.saved,fps:c.quiet?0:c.fps,frames:c.frames,
 fdDropped:0,rtrDropped:0,intKicks:0,maskMaxUs:0,hwOverflows:c.hwOverflows,ringPeak:c.ringPeak,rec:c.rec,tec:c.tec,errFlags:0,bdiag1:0,restarts:c.restarts,rounds:c.quiet?7:1,rejected:0,beginErr:0}});
po('/api/can2',q=>{const v=String(q.bitrate==null?'':q.bitrate),bps=v==='auto'||!v?0:parseInt(v,10)||0;const c=S.can2;
 if(bps&&![125000,250000,500000,1000000].includes(bps))return fail(400,'bitrate: 0 (auto), 125000, 250000, 500000 or 1000000');
 c.setting=bps;if(bps){c.bitrate=bps;c.quiet=bps!==500000;c.rec=c.quiet?96:0}else{c.bitrate=500000;c.quiet=false;c.rec=0}return ok});
const capStatus=()=>{const c=S.cap,used=S.files.reduce((a,f)=>a+f.size,0),cur=S.files.find(f=>f.current);
 return {ready:true,recording:c.recording,mode:c.mode,frames:c.frames,written:c.written,dropped:c.dropped,markers:c.markers,ringUsed:3,ringCap:64,file:c.recording&&cur?+cur.f:0,fileBytes:cur?cur.size:0,session:c.session,
  capBytes:used,budget:S.budget,free:Math.max(0,S.budget-used),fsFree:S.budget-used,rateFps:S.can2.quiet?0:S.can2.fps,ids:41,filterOverflow:0,files:S.files.length,bitrate:[S.can2.bitrate,500000],error:c.error,settings:{...c.settings}}};
g('/api/cap',capStatus);
po('/api/cap/start',()=>{const c=S.cap;if(!c.recording){c.recording=true;c.session++;S.files.forEach(f=>f.current=false);
 S.files.push({f:String(Math.max(...S.files.map(f=>+f.f),0)+1).padStart(4,'0'),session:c.session,part:0,size:0,wall:unixNow(),protected:false,current:true});c.markers=0}return ok});
po('/api/cap/stop',()=>{S.cap.recording=false;S.files.forEach(f=>f.current=false);return ok});
po('/api/cap/mark',q=>{S.cap.markers++;if(S.cap.recording&&S.cap.settings.autoProtect){const f=S.files.find(x=>x.current);if(f)S.files.filter(x=>x.session===f.session).forEach(x=>x.protected=true)}emit(2,'CAP','marker "'+(q.t||'')+'"');return ok});
const fileOf=q=>S.files.find(x=>+x.f===parseInt(q.f,10));
g('/api/cap/list',()=>({files:S.files.map(f=>({...f,current:!!f.current}))}));
g('/api/cap/get',q=>{if(!parseInt(q.f,10))return fail(400,'f=NNNN');return fileOf(q)?{bytes:fileOf(q).size}:fail(404,'no such capture file')});
po('/api/cap/delete',q=>{if(!parseInt(q.f,10))return fail(400,'f=NNNN');const f=fileOf(q);if(!f)return fail(404,'no such capture file');if(f.current)return fail(409,'file is being written or downloaded');S.files=S.files.filter(x=>x!==f);return ok});
po('/api/cap/delete_unprotected',()=>{const n=S.files.length;S.files=S.files.filter(f=>f.protected||f.current);return {ok:true,deleted:n-S.files.length}});
po('/api/cap/protect',q=>{if(!parseInt(q.f,10))return fail(400,'f=NNNN&on=0|1');const f=fileOf(q);if(!f)return fail(404,'no such capture file');S.files.filter(x=>x.session===f.session).forEach(x=>x.protected=q.on===undefined||B(q.on));return ok});
po('/api/cap/format',q=>{if(q.confirm!=='cap')return fail(400,'deletes every capture, protected too: add confirm=cap');S.cap.recording=false;const n=S.files.length;S.files=[];return {ok:true,deleted:n}});
/* ---- НОВЕ: мапа RAV4 замість /api/signals, /api/signals/unpinall, /api/emitter і масштабів у /api/canbox/tuning ---- */
const LV={speed:47,rpm:1850,steering:-1.2,out_temp:9,door_fl:0,door_fr:0,light_pos:1,light_dip:1,fuel_inst:7.2,odometer:182345};
const CANB={};const canBytes=id=>CANB[id]||(CANB[id]=Array.from({length:8},()=>Math.floor(rnd(0,256))));
function mapVal(t){const r=S.map[t.id],ts=S.tests[t.id];if(ts&&ts.until>now())return {v:ts.v,how:'test',left:Math.ceil((ts.until-now())/1000)};
 if(!r||r.src==='off')return {v:null,how:'off'};if(r.src==='const')return {v:r.k,how:'const'};
 const v=t.id in LV?LV[t.id]:(t.kind==='b'?0:+(t.min+(t.max-t.min)*.3).toFixed(t.dec));return {v,how:'can',raw:r.can.id+': '+canBytes(r.can.id).map(h2).join(' ')}}
/* WebUiMap.cpp: назву, одиницю й межі поля сторінка бере з каталогу; тут — лише прив'язка і значення */
/* ?live=1 / ?meta=1 (WebUiMap.cpp putLive/putMeta): живі сигнали — вигадана поїздка; правила — AlertRules */
const META=SIM_DECODE.meta||[],DERIVED=new Set(SIM_DECODE.derived||[]),BOUND=new Set(SIM_DECODE.rows.map(r=>r.sig));
const decStr=(v,d)=>d?(v/pow10(d)).toFixed(d):String(v);
const COND=[['above','вище'],['below','нижче'],['rises','зʼявилось'],['falls','зникло'],['changes','змінилось']];
S.alerts=Array.from({length:16},(_,i)=>i?['','rises',0,5,false]:['fuel_low','rises',0,300,true]);
function liveVal(n,t){const s=t/1000,w=(p,a,b)=>a+(b-a)*(.5+.5*Math.sin(s/p));
 const v={speed:Math.round(w(9,0,9000)),rpm:Math.round(w(9,800,3200)),fuel_level:31,fuel_low:0,batt_v:Math.round(w(30,1380,1420)),eng_temp:89,out_temp:12,
  throttle:Math.round(w(5,0,400)),neutral:0,clutch:s%20<1?1:0,handbrake:0,door_fl:0,door_fr:0,door_rl:0,door_rr:0,boot:0,light_pos:1,light_dip:1,
  fuel_counter:Math.floor(s*7)%256,oil_level:9,service_km:12500,locked:0,belt_drv:1,belt_pass:0,odo_cluster:184321,fuel_level_apv:30};
 const R5=[75,136,200,270,352];v.gear_ratio=Math.round(v.speed*100/v.rpm);let gi=0;R5.forEach((r,i)=>{if(Math.abs(v.gear_ratio-r)<Math.abs(v.gear_ratio-R5[gi]))gi=i});v.gear=v.speed<300?0:gi+1;
 return v[n]}
const mapBase=()=>({send:S.emitter,uart:S.set.uartEnabled,units:{fuel:S.tun.fuelUnit,trip:S.tun.tripUnit},exp:S.huExp||0});
g('/api/map',q=>{const o=mapBase();o.rows=q.live?[]:C.raise.map(t=>({id:t.id,...S.map[t.id],...mapVal(t)}));
 if(q.live){const t=Date.now();o.sigs=META.map(m=>{const v=liveVal(m[0],t),src=DERIVED.has(m[0])?'d':BOUND.has(m[0])?'c':'o';
  return [v===undefined?null:decStr(v,m[2]),v===undefined?null:Math.floor(Math.random()*300),src,['door_fl','door_fr','door_rl','door_rr','boot','light_pos','light_dip'].includes(m[0])?1:0]});
  o.alertStats={fired:0,shown:0,dropped:0}}
 if(q.meta)o.meta=META.map(m=>[m[0],m[1],m[2],m[3],m[4],decStr(m[5],m[2]),decStr(m[6],m[2]),m[7]]);
 return o});
const isInt=x=>Number.isInteger(x),hexOk=x=>/^(0x)?[0-9a-f]{1,8}$/i.test(String(x==null?'':x)),hexV=x=>parseInt(String(x).replace(/^0x/i,''),16);
/* DecodeJson::canFrom: ті самі перевірки й тексти */
function canWhy(c){if(!hexOk(c.id)||hexV(c.id)>0x1FFFFFFF)return 'id: a CAN id, like 0x354';
 if(!isInt(c.byte)||c.byte<0||c.byte>7)return 'byte: 0..7';const len=c.len==null?1:c.len;if(!isInt(len)||len<1||len>4||c.byte+len>8)return 'len: 1..4 bytes, inside the 8';
 if(c.shift!=null&&(!isInt(c.shift)||c.shift<0||c.shift>31))return 'shift: 0..31';if(!hexOk(c.mask)||!hexV(c.mask))return 'mask: hex, not 0';
 const i16=x=>isInt(x)&&x&&x>=-32768&&x<=32767;if(c.mul!=null&&!i16(c.mul))return 'mul: a whole number, not 0, within 16 bits';if(c.div!=null&&!i16(c.div))return 'div: a whole number, not 0, within 16 bits';if(c.add!=null&&!isInt(c.add))return 'add: a whole number';return ''}
po('/api/map',q=>{if(q.send!==undefined&&q.send&&!S.set.uartEnabled)return fail(409,'head-unit UART is off');
 if(q.units){const f=q.units.fuel??S.tun.fuelUnit,t=q.units.trip??S.tun.tripUnit;if(!C.enums.fuelUnits.some(e=>e.v===f))return fail(400,"units.fuel: one of the catalog's fuelUnits");if(!C.enums.tripUnits.some(e=>e.v===t))return fail(400,"units.trip: one of the catalog's tripUnits")}
 const rows=q.rows||[];if(rows.length>16)return fail(400,'rows: 16 at most in one request');const E=[];
 for(const r of rows){const t=C.raise.find(x=>x.id===r.id);if(!t)return fail(404,'no such field');
  if(!SIM_DECODE.sigs.includes(t.id))return fail(409,r.id+': the firmware has no signal for it yet');
  if(E.some(([x])=>x.id===r.id))return fail(400,r.id+': twice in one request');
  if(!['can','const','off'].includes(r.src))return fail(400,r.id+': src: can, const or off');
  if(r.src==='can'){if(!r.can||typeof r.can!=='object')return fail(400,r.id+': can: {id, byte, len, ...}');const w=canWhy(r.can);if(w)return fail(400,r.id+': '+w)}
  if(r.src==='const'&&!(typeof r.k==='number'&&toInt(r.k,t.dec)>=toInt(t.min,t.dec)&&toInt(r.k,t.dec)<=toInt(t.max,t.dec)))return fail(400,r.id+': k: '+t.min+'..'+t.max);
  E.push([r,t])}

 if(q.exp!==undefined&&!(isInt(q.exp)&&q.exp>=0&&q.exp<=15))return fail(400,'exp: 0..15, the experiment bits');
 if(q.exp!==undefined){S.huExp=q.exp;emit(2,'HU','[map] head-unit experiments: '+q.exp)}
 if(q.send!==undefined){S.emitter=!!q.send;emit(2,'HU','[map] send to the head unit '+(S.emitter?'on':'off'))}
 if(q.units){if(q.units.fuel!=null)S.tun.fuelUnit=q.units.fuel;if(q.units.trip!=null)S.tun.tripUnit=q.units.trip}
 if(q.reset)S.map=defMap();
 for(const[r,t]of E){const c=r.can||{};S.map[r.id]={src:r.src,can:r.src==='can'?{id:'0x'+hexV(c.id).toString(16).toUpperCase().padStart(3,'0'),byte:c.byte,len:c.len??1,shift:c.shift??0,mask:hexV(c.mask).toString(16).toUpperCase(),mul:c.mul??1,div:c.div??1,add:c.add??0,le:!!c.le,signed:!!c.signed}:null,k:r.src==='const'?toInt(r.k,t.dec)/pow10(t.dec):0}}
 if(q.reset||E.length)emit(2,'CAN','[map] '+(q.reset?'back to factory':'saved')+': '+E.length+' field(s)');
 return ok});
const frameFor=(t,v)=>{const[c,sub0]=t.cmd.split(' '),sub=sub0&&sub0!=='A/C'?sub0:'',n=Math.round(+v*pow10(t.dec))&0xFFFF;return ('2E '+c+' 0'+(sub?3:2)+' '+(sub?sub+' ':'')+h2(n)+' '+h2(n>>8)+' CS').toUpperCase()};
/* WebUiMap.cpp: перед тестом — чи носить 0x28 цю форму; ворота повідомлень (одометр, TPMS…) мок не моделює */
po('/api/map/test',q=>{const t=C.raise.find(x=>x.id===q.id);if(!t)return fail(404,'no such field');if(!t.fw)return fail(409,t.id+': firmware does not send '+t.cmd+' yet');if(!S.set.uartEnabled)return fail(409,'head-unit UART is off');
 if(t.cmd==='28'&&S.tun.cmd28)return fail(409,t.id+': 0x28 carries the A/C form now: switch it in Service, Raise');
 if(t.id==='prnd'&&!((S.huExp|0)&8))return fail(409,'prnd: switch on the selector experiment first');
 if(t.cmd==='28 A/C'&&!S.tun.cmd28)return fail(409,t.id+': 0x28 carries the outside temperature now: switch it in Service, Raise');
 const v=q.value;if(typeof v!=='number'||!(v>=t.min&&v<=t.max)||(t.kind==='b'&&v!==0&&v!==1))return fail(400,t.id+': value: '+t.min+'..'+t.max);
 S.tests[t.id]={v,until:now()+30000};const b=frameFor(t,v);uart('T',b);S.hu.txVeh++;emit(2,'HU','[map] test '+t.id+' = '+v+' -> '+b);return {ok:true,bytes:b}});
g('/api/canbox/last',q=>({total:S.hu.count,baud:S.set.uartBaud,frames:S.uartRing.slice().reverse().filter(e=>!(q&&q.in==='1')||e.dir==='R'||e.dir==='J').slice(0,14).map(e=>({...e,ageMs:now()-e.at}))}));
po('/api/canbox/key',q=>{if(q.code===undefined)return fail(400,'code? (hex)');S.hu.txKey++;uart('T','2E 20 02 '+h2(parseInt(q.code,16)||0)+' 0'+(q.state||1)+' CS');return {__status:202,body:{ok:true,queued:true}}});
po('/api/canbox/raw',q=>{if(q.cmd===undefined)return fail(400,'cmd? (hex)');const cmd=parseInt(q.cmd,16);if(!(cmd>=0&&cmd<=255))return fail(400,'cmd out of range');
 const d=String(q.data||'').replace(/[\s,:-]/g,'');if(!/^([0-9a-f]{2})*$/i.test(d))return fail(400,'data must be hex bytes');
 const b='2E '+h2(cmd)+' '+h2(d.length/2)+(d?' '+d.match(/../g).join(' ').toUpperCase():'')+' CS';uart('T',b);return {__status:202,body:{ok:true,queued:true,bytes:b}}});
g('/api/messages',()=>({messages:S.msgs.map(m=>({...m}))}));
po('/api/messages/send',q=>{if(q.id===undefined)return fail(400,'id?');const m=S.msgs[+q.id];if(!m||!m.ready)return fail(409,'no data for this message — pin its signals');uart('T',m.bytes);return ok});
/* без since — як прошивка; since= — НОВЕ замість /ws */
g('/api/logs',q=>q.since!==undefined?logsSince(+q.since||0):{lines:logSeq,dropped:0,ringSize:300,mounted:true,mountError:'',buffered:812,used:S.logs.used,total:S.logs.total,files:S.logs.files});
g('/api/logs/file',q=>{const f=S.logs.files[+q.n||0];return f?{bytes:f.size}:fail(404,'no such log')});
g('/api/can',()=>({txEnabled:true,txGated:false,txGates:0,listenOnly:false,
 state:S.can.state,txErr:S.can.txErr,rxErr:S.can.rxErr,txFailed:0,rxMissed:0,busErr:S.can.busErr,arbLost:0,txQueued:0,twaiTxOk:S.can.twaiTxOk,twaiTxFail:S.can.twaiTxFail,twaiRx:S.can.twaiRx,twaiMode:'normal',
 rxFrames:S.can.twaiRx,ids:Object.keys(S.ids).length,frames:S.can.twaiRx,overflow:0,sniff:S.sniff,rxPin:6,txPin:7}));
g('/api/can/ecc',()=>({seen:[{raw:0x6a,count:2,type:'stuff',dir:'while RECEIVING',where:'data'},{raw:0x78,count:1,type:'form',dir:'while RECEIVING',where:'CRC delimiter'}]}));
po('/api/can/sniff',q=>{S.sniff=q.on==='1';emit(2,'CAN','can sniffer '+(S.sniff?'on':'off'));return {ok:true,sniff:S.sniff}});
const SNIFF_IDS=[0x0C2,0x181,0x354,0x551,0x5FD,0x60D,0x645,0x186,0x29A,0x352,0x3B7,0x4F8];
g('/api/can/ids',()=>{if(S.sniff&&!S.can2.quiet)SNIFF_IDS.forEach((id,i)=>{let x=S.ids[id];if(!x){const d=canBytes('0x'+id.toString(16).toUpperCase().padStart(3,'0'));x=S.ids[id]={id,data:d,base:[...d],count:0}}x.count+=Math.round(rnd(5,60));if(i%3===0)x.data[i%8]=Math.floor(rnd(0,256))});
 return {overflow:false,ids:Object.values(S.ids).sort((a,b)=>a.id-b.id).map(x=>({id:x.id,count:x.count,len:8,changed:x.data.reduce((m,b,i)=>b!==x.base[i]?m|1<<i:m,0),data:x.data.map(h2).join('')}))}});
po('/api/can/baseline',()=>{Object.values(S.ids).forEach(x=>x.base=[...x.data]);return ok});
po('/api/can/reset',()=>{S.ids={};return ok});
/* ---- direct: матриця 3×3 (/api/keymap, WebUi.cpp) і драбина KEY1/KEY2 (/api/res*, WebUiRes.cpp) ---- */
const KM_DEF=['VolDown','Mode','VolUp','SrcNext','Radio','Mute','EncPhaseA','EncPhaseB','EncPhaseC'];
const huHex=id=>parseInt((C.actions.find(a=>a.id===id)||{}).hu||'0',16)||0;
const km=()=>S.km||(S.km={slots:KM_DEF.map(a=>({action:a,code:0})),encCw:0,encCcw:0,encInverted:false});
g('/api/keymap',()=>{const k=km();return {slots:k.slots.map((x,pos)=>({pos,action:x.action,code:x.code,effective:x.code||huHex(x.action)})),
 encCw:k.encCw||huHex('EncCw'),encCcw:k.encCcw||huHex('EncCcw'),encInverted:k.encInverted}});
po('/api/keymap',q=>{const k=km();for(const o of q.slots||[])if(o.pos>=0&&o.pos<9)k.slots[o.pos]={action:o.action||'None',code:o.code|0};
 k.encCw=q.encCw|0;k.encCcw=q.encCcw|0;k.encInverted=!!q.encInverted;emit(2,'KEY','keymap saved');return ok});
po('/api/keymap/defaults',()=>{S.km=null;km();emit(2,'KEY','keymap reset to Megane defaults');return ok});
/* драбина: ноги паралельно, щабель = маска ніг, опір = паралель + послідовний */
const resSt=()=>S.res||(S.res={enabled:false,activeHigh:false,spin:2,timing:{pulseMs:60,gapMs:50,maxHoldMs:3000,holdAfter:3,repeatStartMs:500,repeatMs:120},
 ch:[{legs:3,series:0,leg:[1000,2200,4700,0,0]},{legs:2,series:220,leg:[3300,6800,0,0,0]}],slot:{},presses:0,detents:0,live:[0,0],learnUntil:[0,0]});
const rOhms=(c,m)=>{let g=0;for(let i=0;i<c.legs;i++)if(m>>i&1&&c.leg[i])g+=1/c.leg[i];return g?Math.round(1/g)+c.series:65535};
const rungs=c=>{const o=[];for(let m=1;m<(1<<c.legs);m++){const v=rOhms(c,m);if(v!==65535)o.push({mask:m,ohms:v})}return o};
const resWant=()=>{const w=['VolUp','VolDown'];for(const x of km().slots)if(!/^EncPhase/.test(x.action)&&x.action!=='None'&&!w.includes(x.action))w.push(x.action);
 if(km().slots.some(x=>/^EncPhase/.test(x.action)))w.push('EncCw','EncCcw');return w};
g('/api/res',()=>{const r=resSt(),w=resWant(),sl=a=>r.slot[a]||{ch:0,mask:0,longMask:0};
 const used=[0,1].map(c=>w.map(sl).filter(x=>x.ch===c).flatMap(x=>[x.mask,x.longMask]).filter(Boolean));
 return {enabled:r.enabled,activeHigh:r.activeHigh,spin:r.spin,classes:S.routing.res||0,timing:{...r.timing},
  ch:r.ch.map(c=>({legs:c.legs,series:c.series,inWindow:true,leg:[...c.leg],pins:c.legs===3?[25,26,27]:[32,33],rungs:rungs(c)})),
  actions:w.map(a=>{const x=sl(a);return {name:a,ch:x.ch,mask:x.mask,longMask:x.longMask,ohms:x.mask?rOhms(r.ch[x.ch],x.mask):65535,enc:a==='EncCw'||a==='EncCcw'}}),
  sep:used.some(u=>u.length>1)?18:255,
  free:[0,1].flatMap(c=>rungs(r.ch[c]).filter(g=>!used[c].includes(g.mask)).map(g=>({ch:c,...g})))}});
g('/api/res/live',()=>{const r=resSt(),t=now();return {ch:[0,1].map(c=>{const m=t<r.learnUntil[c]?r.live[c]:0;return {mask:m,ohms:m?rOhms(r.ch[c],m):65535,queue:0,learn:t<r.learnUntil[c]}}),
 presses:r.presses,detents:r.detents,dropped:0,preempts:0,longs:0,stuck:0}});
po('/api/res',q=>{const r=resSt();if(typeof q.activeHigh==='boolean')r.activeHigh=q.activeHigh;if(q.spin>=0&&q.spin<=2)r.spin=q.spin|0;
 [0,1].forEach(c=>{if(Array.isArray((q.legs||[])[c]))q.legs[c].forEach((v,i)=>{if(i<5&&v>=0)r.ch[c].leg[i]=v|0});if((q.series||[])[c]>=0)r.ch[c].series=q.series[c]|0});
 if(q.timing)for(const k in r.timing)if(q.timing[k]>=0)r.timing[k]=q.timing[k]|0;
 r.timing.pulseMs=Math.max(50,r.timing.pulseMs);r.timing.gapMs=Math.max(20,r.timing.gapMs);if(typeof q.enabled==='boolean')r.enabled=q.enabled;
 emit(2,'KEY','res: '+(r.enabled?'on':'off'));return ok});
po('/api/res/map',q=>{if(!C.actions.some(a=>a.id===q.action))return fail(400,'unknown action');const r=resSt(),x={...(r.slot[q.action]||{ch:0,mask:0,longMask:0})};
 for(const k of ['ch','mask','longMask'])if(q[k]>=0)x[k]=q[k]|0;r.slot[q.action]=x;return ok});
po('/api/res/auto',q=>{const r=resSt(),c=q.ch|0,g=rungs(r.ch[c]).sort((a,b)=>a.ohms-b.ohms),w=resWant();let n=0;
 w.forEach((a,i)=>{if(i<g.length){r.slot[a]={ch:c,mask:g[i].mask,longMask:0};n++}});emit(2,'KEY','res: auto-assign ch'+c+' placed '+n+' of '+w.length);return {ok:true,wanted:w.length,placed:n}});
po('/api/res/learn',q=>{const r=resSt();if(q.stop){r.learnUntil=[0,0];return ok}const ms=q.ms||4000;
 if(q.action){const x=r.slot[q.action];if(!x||!x.mask)return fail(409,'no rung, or its legs are not fitted');r.live[x.ch]=x.mask;r.learnUntil[x.ch]=now()+ms;return ok}
 if(!q.mask)return fail(400,'mask?');const c=q.ch|0;if(rOhms(r.ch[c],q.mask)===65535)return fail(409,'those legs are not fitted');r.live[c]=q.mask;r.learnUntil[c]=now()+ms;return ok});
g('/api/knobs',()=>({board:'T-2CAN',pins:{},pullup:{},maxLeds:160,...deep(S.kn),enc:{fitted:true,...deep(S.kn.enc)},enc2:{fitted:true,...deep(S.kn.enc2)},pot:{mode:'off',inverted:false,zones:8,min:0,max:4095,up:bind('None'),down:bind('None')}}));
g('/api/knobs/live',()=>deep(S.knl));
po('/api/knobs',q=>{for(const k of ['enc','enc2'])if(q[k])Object.assign(S.kn[k],deep(q[k]));if(q.led){const l=deep(q.led);Object.assign(S.kn.led,l)}return ok});
g('/api/macros',()=>({macros:S.mac.map((m,i)=>({...m,valid:macroWhy(m)==='',fires:S.macFires[i],progress:0}))}));
g('/api/macros/live',()=>({...S.macLive,progress:S.mac.map(()=>0),fires:S.macFires.slice()}));
/* Macro::valid() */
function macroWhy(m){const keys=(m.keys||[]).filter(k=>k&&k!=='None'),fx=C.enums.macroEffects.find(e=>e.v===m.effect);
 if(!fx||m.effect==='none')return 'no effect';if(fx.field==='action'&&m.action==='None')return 'no action';if((fx.field==='code'||fx.field==='raw')&&!parseInt(m.codeHex,16))return 'code 00';
 if((fx.field==='text'||fx.field==='scene')&&!m.arg)return 'empty text';const hold=k=>k!=='EncCw'&&k!=='EncCcw';
 if(m.trigger==='combo')return keys.length<2||keys.some(k=>!hold(k))||new Set(keys).size!==keys.length?'combo needs 2-3 different keys that can be held':'';
 if(m.trigger==='sequence')return !keys.length||!(m.taps>=2&&m.taps<=10)||m.windowMs<50?'sequence needs a key, 2..10 taps, window >= 50 ms':'';
 if(m.trigger==='hold')return !keys.length||!hold(keys[0])?'hold needs one key that can be held':'';return 'trigger is off'}
/* НОВЕ: помилка "<index>: <why>" */
po('/api/macros',q=>{const ms=q.macros;if(!Array.isArray(ms))return fail(400,'macros[]?');
 for(let i=0;i<ms.length&&i<8;i++){const m=ms[i];if(!/^[0-9a-f\s,:-]*$/i.test(m.data||''))return fail(400,i+': data must be hex bytes (max 8)');const w=macroWhy(m);if(m.enabled&&w)return fail(400,i+': '+w)}
 S.mac=Array.from({length:8},(_,i)=>{const m=ms[i];if(!m)return emptyMacro();const keys=(m.keys||[]).filter(k=>k&&k!=='None').slice(0,3);while(keys.length<3)keys.push('None');
  return {enabled:!!m.enabled,name:String(m.name||'').slice(0,19),trigger:m.trigger||'off',keys,taps:m.taps|0||3,windowMs:m.windowMs|0||600,swallow:m.swallow!==false,effect:m.effect||'none',action:m.action||'None',codeHex:h2(parseInt(m.codeHex,16)||0),data:m.data||'',arg:String(m.arg||'').slice(0,39)}});
 emit(2,'MAC','macros saved — '+S.mac.filter(m=>m.enabled).length+' active');return ok});
po('/api/macros/defaults',()=>{S.mac=defMacros();return ok});
po('/api/macros/test',q=>{if(q.id===undefined)return fail(400,'id?');const i=parseInt(q.id,10);if(!(i>=0&&i<8))return fail(400,'id out of range');if(S.mac[i].effect==='none')return fail(409,'this row has no effect');
 S.macFires[i]++;S.macLive.fired++;emit(2,'MAC','test "'+S.mac[i].name+'"');return {__status:202,body:{ok:true,queued:true}}});
g('/ota/start',()=>'OK');
/* GhOta (src/net/GhOta.cpp): check -> available (наступний патч), install -> поступ -> перезавантаження */
const ghNext=()=>{const p=S.fw.split('.').map(Number);p[2]++;return p.join('.')};
const ghView=()=>{const v=S.gh||(S.gh={phase:'idle',latest:'',asset:'',size:0,got:0,heapStart:0,heapMin:0,tlsInPsram:true});
 if(v.phase==='checking'&&now()-v.at>1200){v.phase='available';v.latest='v'+ghNext();v.asset='megaopen-t2can-'+ghNext()+'.bin';v.size=1572353}
 if(v.phase==='downloading'){v.got=Math.min(v.size,Math.round((now()-v.at)/6000*v.size));v.heapMin=Math.min(v.heapMin,v.heapStart-Math.round(rnd(3000,6000)));
  if(v.got>=v.size)v.phase='written'}
 if(v.phase==='reboot'){v.phase='rebooting';S.fw=v.latest.slice(1);S.build='github '+v.latest;setTimeout(()=>{S.gh=null;doBoot()},1500)}
 return {fw:S.fw,env:'t2can',...v}};
g('/api/fw',ghView);
po('/api/fw/check',()=>{const v=ghView();if(S.wifi.mode!=='sta')return fail(409,'the board is on its own access point: join it to a WiFi network with internet');
 if(v.phase==='written')return fail(409,'an update is written and waits for the reboot');if(v.phase==='checking'||v.phase==='downloading')return fail(409,'a check or an update is already running');Object.assign(S.gh,{phase:'checking',at:now(),err:undefined,heapStart:S.heap,heapMin:S.heap});return ok});
po('/api/fw/reboot',()=>{const v=ghView();if(v.phase!=='written')return fail(409,'nothing written to reboot into');S.gh.phase='reboot';return ok});
po('/api/fw/install',()=>{const v=ghView();if(v.phase!=='available')return fail(409,'no newer release: check first');Object.assign(S.gh,{phase:'downloading',at:now(),got:0});return ok});
po('/ota/upload',q=>{if(!q.size)return fail(400,'empty file');S.build='ota '+new Date().toISOString().slice(0,16);setTimeout(doBoot,200);return 'OK'});
function uart(dir,b){const p=b.split(' ');S.hu.count++;S.uartRing.push({dir,cmd:p[1]||'00',len:Math.max(0,p.length-4),total:p.length,at:now(),data:p.slice(3,-1).join(''),ascii:'',u16:false});if(S.uartRing.length>40)S.uartRing.shift()}

/* ---- живий двигун симуляції ---- */
setInterval(()=>{
 const c=S.can2;c.fps=c.quiet?0:Math.round(rnd(820,880));c.frames+=c.fps;
 const cp=S.cap;if(cp.recording&&!c.quiet){const n=Math.round(c.fps*rnd(.25,.35)),b=n*16;cp.frames+=n;cp.written+=b;const f=S.files.find(x=>x.current);
  if(f){f.size+=b;if(f.size>cp.settings.fileMB*1048576){f.current=false;S.files.push({f:String(+f.f+1).padStart(4,'0'),session:f.session,part:f.part+1,size:0,wall:unixNow(),protected:f.protected,current:true})}}
  while(cp.settings.ring&&S.files.reduce((a,x)=>a+x.size,0)>S.budget*cp.settings.fill/100){const o=S.files.find(x=>!x.protected&&!x.current);if(!o){cp.recording=false;cp.error='storage full';break}S.files.splice(S.files.indexOf(o),1)}}
 const m=S.med;if(phone()&&m.playing&&!m.inj){m.elapsed++;if(m.elapsed>=S.tracks[m.ti][2]){m.ti=(m.ti+1)%S.tracks.length;m.elapsed=0}}
 if(S.hu.connected&&S.set.uartEnabled){S.hu.rx+=3;S.hu.acks+=2;/* ГУ шле 0x9D раз на хвилину (raise-protocol.md:216), не на кожен кадр */
  if(new Date().getMinutes()!==huMin){huMin=new Date().getMinutes();uart('R','2E 9D 03 '+clockStr().replace(':',' ')+' 00 CS');if(S.set.autoTime)clockFrom('hu',clockStr())}}else huMin=-1;
 if(!phone())ctsMin=-1;else if(S.set.autoTime&&new Date().getMinutes()!==ctsMin){ctsMin=new Date().getMinutes();clockFrom('cts',clockStr())}
 if(S.emitter&&S.hu.connected)S.hu.txVeh+=4;
 if(S.disp.resyncAt&&now()>S.disp.resyncAt){S.disp.registered=true;S.disp.resyncAt=0;S.disp.phase='Ready';emit(2,'PNL','panel registered')}
 LV.speed=Math.max(0,Math.round(LV.speed+rnd(-3,3)));LV.rpm=Math.round(800+LV.speed*22+rnd(-40,40));LV.fuel_inst=+(5+rnd(0,4)).toFixed(1);LV.steering=+(LV.steering*.7+rnd(-20,20)).toFixed(1);
 S.macLive.down=[];
 if(WHEEL.length&&Math.random()<.08){const k=WHEEL[Math.floor(rnd(0,WHEEL.length))];pressKey(parseInt(k.wire,16),Math.random()<.25)}
 const e=S.knl.enc;if(Math.random()<.1){e.detents++;e.pos=(e.pos||0)+1;S.knl.led.mA=Math.round(rnd(60,140))}else S.knl.led.mA=Math.round(rnd(58,70));e.btn=e.key=Math.random()<.05;e.a=Math.random()<.3;e.b=Math.random()<.3;if(e.a)e.edges[0]++;if(e.b)e.edges[1]++;if(e.key)e.edges[2]++;S.knl.led.mAWanted=S.knl.led.mA;
 if(Math.random()<.35)emit(3,'CAN','[car] fps='+fmtN(c.fps)+' rec='+c.rec);
 if(cp.recording&&Math.random()<.2)emit(2,'CAP','[cap] frames '+cp.frames);
 if(Math.random()<.1)emit(2,'SYS','[hb] heap='+S.heap);
},1000);
for(let i=0;i<20;i++)emit(i%5?2:1,['KEY','BLE','PNL','CAN','CAP','HU','SYS'][i%7],['[panelkey] Pause > Ok → ГУ','[BLE] AMS: Нічний Київ','[disp] media: info, scene media','[can] A 500k, 851/s','[cap] frames 1834220, file 0012','[hu] text: cmd 0x13','[boot] MegaOpen'][i%7]);

/* ---- виклик: затримка як у мережі, помилки на замовлення, журнал API ---- */
const JR=[];
SIM._call=async function(m,path,params,hdr){
 const [p,qs]=path.split('?');const q=Object.assign(Object.fromEntries(new URLSearchParams(qs||'')),params||{});
 const t0=now();await sleep(p==='/api/wifi/scan'?1800:p==='/ota/upload'?400:60+Math.random()*160);
 if(SIM.offline){JR.unshift({m,p:path,st:0,ms:now()-t0});jrDraw();throw new Error('немає зв’язку з платою')}
 let st=200,body;const fn=R[m+' '+p];
 if(m==='POST'&&SIM.nextErr){st=SIM.nextErr;SIM.nextErr=0;simErrUi();body={409:'conflict',503:'busy, try again'}[st]}
 /* src/net/RequestGate.h: зміна без X-MegaOpen — 403, як на платі */
 else if((m!=='GET'||p==='/ota/start')&&!(hdr&&hdr['X-MegaOpen']==='1')){st=403;body="a change needs the header X-MegaOpen: 1 (the board's own page sends it)"}
 else if(!fn||!routeBuilt(p)){st=404;body='not found'}
 else{try{const r=fn(q);if(r&&r.__status){st=r.__status;body=r.body}else body=r}catch(e){st=500;body=e.message}}
 JR.unshift({m,p:path,st,ms:now()-t0});if(JR.length>60)JR.pop();jrDraw();
 if(st>=400)throw Object.assign(new Error(errMsg(body,st)),{status:st,body});
 return body===undefined?{ok:true}:body;
};

/* ============ панель симуляції: кнопка СИМ у шапці і бічна панель ============ */
document.head.insertAdjacentHTML('beforeend',`<style>
#side{position:fixed;top:0;right:0;bottom:0;width:380px;max-width:100%;z-index:60;overflow:auto;display:none;padding:10px 12px 20px;background:var(--bg);border-left:1px solid var(--line);box-shadow:-8px 0 24px var(--shd)}
#side.open{display:block}
#side .card{margin-top:0}
.jr{display:grid;grid-template-columns:44px minmax(0,1fr) 36px 46px;gap:4px;padding:3px 12px;font:12px/1.35 var(--mono);border-top:1px solid var(--line)}
.jr .s2{color:var(--ok)}.jr .s4,.jr .s5{color:var(--bad)}
.jr span:nth-child(2){overflow-wrap:anywhere}
#jr{max-height:40vh;overflow:auto}
</style>`);
$('#themeBtn').insertAdjacentHTML('afterend','<button class="hb" id="simBtn" type="button" title="Панель симуляції (лише в макеті)">СИМ</button>');
$('#app').insertAdjacentHTML('afterend',`<aside id="side">
 <section class="card"><h2><span>Симуляція (лише макет)</span><button class="hb" id="simClose" type="button">✕</button></h2>
  <div class="row"><div class="l"><b>Кермо</b><small>натиснути як із джойстика</small></div><div class="c"><select class="in" id="simKey"></select><button class="btn sm" id="simKc" type="button">клік</button><button class="btn sm" id="simKh" type="button">утрим.</button></div></div>
  <div class="row"><div class="l"><b>Збірка</b><small>що в ній є: features і маршрути</small></div><div class="c"><select class="in" id="simEnv"></select></div></div>
  <div class="row"><div class="l"><b>ГУ по UART</b><small>шле текст і час</small></div><div class="c"><input type="checkbox" class="sw" id="simHu" checked></div></div>
  <div class="row"><div class="l"><b>Телефон</b></div><div class="c"><div class="seg" id="simPh"><button type="button" data-p="c" class="on">на зв’язку</button><button type="button" data-p="b">далеко</button><button type="button" data-p="n">немає</button></div></div></div>
  <div class="row"><div class="l"><b>Панель</b></div><div class="c"><button class="btn sm" id="simLose" type="button">Втратити реєстрацію</button></div></div>
  <div class="row"><div class="l"><b>CAN A</b></div><div class="c"><button class="btn sm" id="simCan" type="button">Мовчить / живе</button></div></div>
  <div class="row"><div class="l"><b>WiFi</b></div><div class="c"><button class="btn sm" id="simAp" type="button">Мережу втрачено → AP</button></div></div>
  <div class="row"><div class="l"><b>Наступний POST</b></div><div class="c"><div class="seg" id="simErr"><button type="button" data-e="0" class="on">200</button><button type="button" data-e="409">409</button><button type="button" data-e="503">503</button></div></div></div>
  <div class="row"><div class="l"><b>Плата недоступна</b></div><div class="c"><input type="checkbox" class="sw" id="simOff"></div></div>
 </section>
 <section class="card"><h2><span>Журнал API</span><label class="ck" style="min-height:0;font-size:12px"><input type="checkbox" id="jrPost" checked> лише POST</label></h2><div id="jr"></div></section>
</aside>`);
function jrDraw(){const el=$('#jr');if(!el||jrDraw.q)return;jrDraw.q=requestAnimationFrame(()=>{jrDraw.q=0;const only=$('#jrPost').checked;el.innerHTML=JR.filter(j=>!only||j.m==='POST').slice(0,40).map(j=>`<div class="jr"><span>${j.m}</span><span>${esc(j.p)}</span><span class="s${String(j.st)[0]}">${j.st}</span><span>${j.ms}мс</span></div>`).join('')||'<div class="jr"><span></span><span class="muted">поки немає</span></div>'})}
$('#jrPost').onchange=jrDraw;
function simErrUi(){$$('#simErr button').forEach(b=>b.classList.toggle('on',+b.dataset.e===SIM.nextErr))}
$('#simErr').onclick=e=>{const b=e.target.closest('[data-e]');if(b){SIM.nextErr=+b.dataset.e;simErrUi()}};
$('#simKey').innerHTML=WHEEL.map(k=>`<option value="${k.wire}">${esc(k.name)}</option>`).join('')+'<option value="0x0113">невідома 0x0113</option>';
$('#simKc').onclick=()=>pressKey(parseInt($('#simKey').value,16),false);$('#simKh').onclick=()=>pressKey(parseInt($('#simKey').value,16),true);
$('#simEnv').innerHTML=(Object.keys(ENVS).length?Object.keys(ENVS):['усе']).map(e=>`<option${e===ENV?' selected':''}>${esc(e)}</option>`).join('');
$('#simEnv').onchange=e=>{const u=new URL(location.href);u.searchParams.set('env',e.target.value);location.href=u.href};
$('#simHu').onchange=e=>{S.hu.connected=e.target.checked};
$('#simPh').onclick=e=>{const b=e.target.closest('[data-p]');if(!b)return;const p=b.dataset.p;S.ble.connected=p==='c';S.ble.bonded=p!=='n';$$('#simPh button').forEach(x=>x.classList.toggle('on',x===b))};
$('#simLose').onclick=()=>{S.disp.registered=false;S.disp.resyncAt=0;S.disp.sessionsLost++;S.disp.lastLoss='AckTimeout';S.disp.phase='Lost';emit(0,'PNL','registration lost')};
$('#simCan').onclick=()=>{S.can2.quiet=!S.can2.quiet};
$('#simAp').onclick=()=>{const w=S.wifi;if(w.mode==='sta'){w.mode='ap';w.ip='192.168.4.1'}else{w.mode='sta';w.ssid='HomeWiFi';w.ip='192.168.1.50';w.rssi=-58}};
$('#simOff').onchange=e=>{SIM.offline=e.target.checked};
$('#simBtn').onclick=()=>$('#side').classList.toggle('open');$('#simClose').onclick=()=>$('#side').classList.remove('open');
jrDraw();
}
