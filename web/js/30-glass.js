/* ============ щиток: попередній перегляд ============
   Геометрія з каталогу (layouts: cols, rows, pane, clock), рядки і картинка —
   з GET /api/media (preview, pane.bits): текст складає прошивка, не сторінка. */
const layoutOf=id=>byId(CAT.layouts,id)||{id,cols:8,rows:3,params:[],panels:[]};
function glassDraw(me){const c=$('#glass');if(!c)return;const L=layoutOf(me.layout),cols=L.cols,rows=L.rows;
 const cw=15,chh=26,pad=12,pane=L.pane?96:0,clk=L.clock?6*cw:0;
 const W=pad*2+cols*cw+(pane?pane+pad:0)+clk,H=Math.max(pad*2+rows*chh,pane?pane+pad*2:0);
 const dpr=window.devicePixelRatio||1;if(c.width!==W*dpr){c.width=W*dpr;c.height=H*dpr;c.style.maxWidth=W*1.4+'px'}
 const x=c.getContext('2d');x.setTransform(dpr,0,0,dpr,0,0);const cs=getComputedStyle(document.documentElement),v=n=>cs.getPropertyValue(n).trim();
 x.fillStyle=v('--glass');x.fillRect(0,0,W,H);x.textBaseline='middle';x.textAlign='center';x.font='600 19px '+v('--mono');
 const y0=(H-rows*chh)/2,pv=me.preview||[];
 for(let r=0;r<rows;r++){const s=(pv[r]||'').padEnd(cols);for(let i=0;i<cols;i++){const px=pad+i*cw,py=y0+r*chh;
  x.globalAlpha=.35;x.fillStyle=v('--glassD');x.fillRect(px+1,py+2,cw-2,chh-4);x.globalAlpha=1;x.fillStyle=v('--glassT');x.fillText(s[i],px+cw/2,py+chh/2+1)}}
 /* стрілки: поле розкладки зі списком arrows; що малювати, кажуть up/down значення */
 const af=(L.params||[]).find(f=>f.enum==='arrows'),ar=af&&enumOf('arrows',me[af.n]);
 if(ar){x.fillStyle=v('--glassT');x.font='14px sans-serif';if(ar.up)x.fillText('▲',W-pad/2-2,y0+chh*1.5);if(ar.down)x.fillText('▼',W-pad/2-2,y0+chh*2.5)}
 if(clk){x.fillStyle=v('--glassT');x.font='600 19px '+v('--mono');x.fillText(me.clock||'--:--',W-pad-clk/2+cw/2,H/2+1)}
 if(pane&&me.pane&&me.pane.bits&&me.pane.on!==false){const px=hex2px(me.pane.bits),ox=W-pad-pane,oy=(H-pane)/2,s=pane/48;x.fillStyle=v('--glassD');x.globalAlpha=.25;x.fillRect(ox,oy,pane,pane);x.globalAlpha=1;x.fillStyle=v('--glassT');
  for(let i=0;i<2304;i++)if(px[i])x.fillRect(ox+(i%48)*s,oy+Math.floor(i/48)*s,s,s)}
 $('#glassW').textContent=`${cols}×${rows}`+(pane?' + картинка 48×48':'')+(clk?' + годинник панелі':'')+' · так щиток покаже зараз';
}
