'use strict';
/* ============ утиліти ============
   web/js/*.js склеюються в один <script> за іменем файлу (tools/gen_pages.py,
   tools/build_sim.py): без модулів і збирача, тож порядок файлів = порядок коду. */
const $=(s,r=document)=>r.querySelector(s), $$=(s,r=document)=>[...r.querySelectorAll(s)];
/* ============ мова ============
   Слова сторінки — у web/i18n/<мова>.json (ключ -> текст). Українська вбудована в
   сторінку (I18N.uk, tools/gen_pages.py), англійську сторінка бере з /i18n/en.json,
   лише коли її вибрано. Вибір живе в цьому браузері (mo-lang); ?lang=en в адресі важить більше.
   t(ключ, ...аргументи): "{0}" -> перший аргумент. Нема в мові — українською; нема
   й там — сам ключ, тож ще не перекладений текст, переданий як ключ, лишається як є.
   Ніколи не під час завантаження скрипта: англійська приходить пізніше (i18nLoad) */
let LANG='uk';try{if((new URLSearchParams(location.search).get('lang')||localStorage.getItem('mo-lang'))==='en')LANG='en'}catch(e){}
const tHas=k=>k in(I18N[LANG]||I18N.uk)||k in I18N.uk;
const t=(k,...a)=>{const d=I18N[LANG]||{},s=k in d?d[k]:k in I18N.uk?I18N.uk[k]:k;return a.length?s.replace(/\{(\d)\}/g,(m,i)=>a[i]==null?'':a[i]):s};
const LOCALE=()=>LANG==='en'?'en-GB':'uk-UA';
async function i18nLoad(){
 if(!I18N[LANG]){try{const r=await fetch('/i18n/'+LANG+'.json');if(r.ok)I18N[LANG]=await r.json()}catch(e){}}
 document.documentElement.lang=LANG;
 $$('[data-i18n]').forEach(e=>e.textContent=t(e.dataset.i18n));
 $$('[data-i18n-aria]').forEach(e=>e.setAttribute('aria-label',t(e.dataset.i18nAria)));
}
const esc=s=>String(s==null?'':s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const sleep=ms=>new Promise(r=>setTimeout(r,ms));
const now=()=>Date.now();
/* порожнє значення в таблицях і чипах (не дія: «нічого» в діях — CAT.actions) */
const DASH='—';
const B=v=>v===true||v===1||v==='1'||v==='true'||v==='on';
const fmtB=n=>n>=1048576?t('c.mb',(n/1048576).toFixed(1)):n>=1024?t('c.kb',Math.round(n/1024)):t('c.b',n);
const fmtN=n=>Math.round(n).toLocaleString(LOCALE());
const fmtT=s=>{s=Math.max(0,Math.floor(s));return Math.floor(s/60)+':'+String(s%60).padStart(2,'0')};
const fmtUp=ms=>{const s=Math.max(0,Math.floor(ms/1000)),h=Math.floor(s/3600),m=Math.floor(s%3600/60);return h?t('c.upH',h,m):t('c.upM',m,s%60)};
const deep=o=>JSON.parse(JSON.stringify(o));
function setPath(o,p,v){const k=p.split('.');let x=o;for(let i=0;i<k.length-1;i++){x=x[k[i]]=x[k[i]]==null?{}:x[k[i]]}x[k[k.length-1]]=v}
const clockStr=()=>{const d=new Date();return String(d.getHours()).padStart(2,'0')+':'+String(d.getMinutes()).padStart(2,'0')};
/* картинка 48×48: 288 байт, 6 на рядок, MSB зліва (MediaPane.h) */
const px2hex=p=>{let s='';for(let i=0;i<2304;i+=8){let b=0;for(let j=0;j<8;j++)b=b<<1|p[i+j];s+=b.toString(16).padStart(2,'0')}return s};
const hex2px=h=>{const p=new Uint8Array(2304);for(let i=0;i<288;i++){const b=parseInt(h.substr(i*2,2),16);for(let j=0;j<8;j++)p[i*8+j]=b>>(7-j)&1}return p};
const h2=n=>(n&255).toString(16).toUpperCase().padStart(2,'0');
/* unix-секунди файла запису; 0 = годинник був невідомий */
const fmtWall=s=>{if(!s)return t('c.timeUnknown');const d=new Date(s*1000),p=n=>String(n).padStart(2,'0');return d.getFullYear()+'-'+p(d.getMonth()+1)+'-'+p(d.getDate())+' '+p(d.getHours())+':'+p(d.getMinutes())};
