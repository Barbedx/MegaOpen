'use strict';
/* ============ утиліти ============
   web/js/*.js склеюються в один <script> за іменем файлу (tools/gen_pages.py,
   tools/build_sim.py): без модулів і збирача, тож порядок файлів = порядок коду. */
const $=(s,r=document)=>r.querySelector(s), $$=(s,r=document)=>[...r.querySelectorAll(s)];
const esc=s=>String(s==null?'':s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const sleep=ms=>new Promise(r=>setTimeout(r,ms));
const now=()=>Date.now();
/* порожнє значення в таблицях і чипах (не дія: «нічого» в діях — CAT.actions) */
const DASH='—';
const B=v=>v===true||v===1||v==='1'||v==='true'||v==='on';
const fmtB=n=>n>=1048576?(n/1048576).toFixed(1)+' МБ':n>=1024?Math.round(n/1024)+' кБ':n+' Б';
const fmtN=n=>Math.round(n).toLocaleString('uk-UA');
const fmtT=s=>{s=Math.max(0,Math.floor(s));return Math.floor(s/60)+':'+String(s%60).padStart(2,'0')};
const fmtUp=ms=>{const s=Math.max(0,Math.floor(ms/1000)),h=Math.floor(s/3600),m=Math.floor(s%3600/60);return h?h+' год '+m+' хв':m+' хв '+(s%60)+' с'};
const deep=o=>JSON.parse(JSON.stringify(o));
function setPath(o,p,v){const k=p.split('.');let x=o;for(let i=0;i<k.length-1;i++){x=x[k[i]]=x[k[i]]==null?{}:x[k[i]]}x[k[k.length-1]]=v}
const clockStr=()=>{const d=new Date();return String(d.getHours()).padStart(2,'0')+':'+String(d.getMinutes()).padStart(2,'0')};
/* картинка 48×48: 288 байт, 6 на рядок, MSB зліва (MediaPane.h) */
const px2hex=p=>{let s='';for(let i=0;i<2304;i+=8){let b=0;for(let j=0;j<8;j++)b=b<<1|p[i+j];s+=b.toString(16).padStart(2,'0')}return s};
const hex2px=h=>{const p=new Uint8Array(2304);for(let i=0;i<288;i++){const b=parseInt(h.substr(i*2,2),16);for(let j=0;j<8;j++)p[i*8+j]=b>>(7-j)&1}return p};
const h2=n=>(n&255).toString(16).toUpperCase().padStart(2,'0');
/* unix-секунди файла запису; 0 = годинник був невідомий */
const fmtWall=s=>{if(!s)return 'час невідомий';const d=new Date(s*1000),p=n=>String(n).padStart(2,'0');return d.getFullYear()+'-'+p(d.getMonth()+1)+'-'+p(d.getDate())+' '+p(d.getHours())+':'+p(d.getMinutes())};
