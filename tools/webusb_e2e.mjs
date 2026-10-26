// web/usb/ in headless Chrome, end to end, with no board: a fake
// navigator.usb whose device speaks the link (src/link/UsbLink.cpp's side),
// the board's real page out of src/net/gen/PageGz.cpp (gzipped, as the board
// serves it), and every other request answered by the simulator's mock board
// (tools/sim_http.mjs). It checks what only a browser shows: the page in its
// frame with MOLink, "#board" links in the same document, the language switch
// reloading through the shell, a POST and a download over the link, an unplug
// and a replug, and the shell opening from the service worker offline.
//
//   python tools/gen_pages.py && python tools/build_sim.py   (after pio test -e native)
//   node tools/webusb_e2e.mjs                                 (exit 1 on a failure)
//
// CHROME= picks the browser (sim_http.mjs's default otherwise). Node 22+.
import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { readFileSync, rmSync, existsSync } from 'node:fs';
import { join, resolve, dirname, extname } from 'node:path';
import { tmpdir } from 'node:os';

const ROOT = resolve(dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1')), '..');
const CHROME = process.env.CHROME || (process.platform === 'win32'
  ? 'C:/Program Files/Google/Chrome/Application/chrome.exe'
  : process.platform === 'darwin' ? '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'
  : 'google-chrome');
const sleep = ms => new Promise(r => setTimeout(r, ms));
let fails = 0, passes = 0;
const ok = (c, what) => { if (c) { ++passes; console.log('ok   ' + what); } else { ++fails; console.log('FAIL ' + what); } };

// ---- the board's bytes ---------------------------------------------------------
const gen = join(ROOT, 'src', 'net', 'gen', 'PageGz.cpp'), sim = join(ROOT, 'build', 'ui-sim.html');
for (const f of [gen, sim]) if (!existsSync(f)) { console.error('webusb_e2e: missing ' + f + ' (build first, see the header)'); process.exit(2); }
const cpp = readFileSync(gen, 'utf8');
const arr = name => {
  const m = new RegExp(name + '\\[\\][^{]*\\{([^}]*)\\}').exec(cpp);
  return Buffer.from(m[1].match(/0x[0-9a-f]{2}/g).map(x => parseInt(x, 16)));
};
const PAGE = arr('PAGE_APP_GZ'), EN = arr('PAGE_I18N_EN_GZ');

// ---- the mock behind the board -------------------------------------------------
const SIM_PORT = 18000 + (process.pid % 1000), PORT = SIM_PORT + 1000;
const simProc = spawn(process.execPath, [join(ROOT, 'tools', 'sim_http.mjs'), sim, String(SIM_PORT)], { stdio: ['ignore', 'pipe', 'inherit'] });
await new Promise((ok, no) => { simProc.stdout.on('data', d => { if (String(d).includes('on http')) ok(); }); simProc.on('exit', c => no(new Error('sim_http exit ' + c))); });

// ---- the site and the board's end of the link ----------------------------------
const seen = [];          // every request the board got: "METHOD path", and its gate headers
let offline = false;
const MIME = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8' };
function rawResponse(status, type, body, gz) {
  const h = `HTTP/1.1 ${status} X\r\nContent-Type: ${type}\r\n${gz ? 'Content-Encoding: gzip\r\n' : ''}Content-Length: ${body.length}\r\n\r\n`;
  return Buffer.concat([Buffer.from(h, 'latin1'), body]);
}
async function board(raw) {
  const he = raw.indexOf('\r\n\r\n');
  const lines = raw.subarray(0, he).toString('latin1').split('\r\n');
  const [method, path] = lines[0].split(' ');
  const h = {};
  for (const l of lines.slice(1)) { const i = l.indexOf(':'); h[l.slice(0, i).toLowerCase()] = l.slice(i + 1).trim(); }
  const body = raw.subarray(he + 4);
  seen.push({ req: method + ' ' + path, host: h.host, gate: h['x-megaopen'], len: body.length, type: h['content-type'] || '' });
  if (method === 'GET' && path === '/') return rawResponse(200, 'text/html', PAGE, true);
  if (method === 'GET' && path === '/i18n/en.json') return rawResponse(200, 'application/json', EN, true);
  const r = await fetch(`http://127.0.0.1:${SIM_PORT}${path}`, {
    method, body: method === 'GET' ? undefined : body,
    headers: { 'content-type': h['content-type'] || 'text/plain', 'x-megaopen': h['x-megaopen'] || '' },
  });
  return rawResponse(r.status, r.headers.get('content-type') || 'text/plain', Buffer.from(await r.arrayBuffer()), false);
}
const server = createServer((req, res) => {
  const chunks = [];
  req.on('data', c => chunks.push(c));
  req.on('end', async () => {
    if (req.url === '/__board') {
      try { const out = await board(Buffer.concat(chunks)); res.writeHead(200, { 'Content-Type': 'application/octet-stream' }); res.end(out); }
      catch (e) { res.writeHead(500); res.end(String(e)); }
      return;
    }
    const m = /^\/MegaOpen\/usb\/([a-z.]*)(\?.*)?$/.exec(req.url);
    if (!m) { res.writeHead(404); res.end('no'); return; }
    if (offline) { req.socket.destroy(); return; }
    const f = join(ROOT, 'web', 'usb', m[1] || 'index.html');
    if (!existsSync(f)) { res.writeHead(404); res.end('no'); return; }
    res.writeHead(200, { 'Content-Type': MIME[extname(f)] || 'application/octet-stream', 'Cache-Control': 'no-store' });
    res.end(readFileSync(f));
  });
}).listen(PORT, '127.0.0.1');

// ---- the fake USB device, in the page before its scripts -----------------------
const FAKE = `(() => {
  if (window !== top || !location.pathname.startsWith('/MegaOpen/usb/')) return;
  const L = import('/MegaOpen/usb/link.js');
  const te = new TextEncoder(), lat = u => { let s = ''; for (const b of u) s += String.fromCharCode(b); return s; };
  // The browser keeps a device allowed across a reload; so does this.
  const F = window.__fake = { opens: 0, hello: null, filters: null, plugged: true, granted: sessionStorage.getItem('fake-granted') === '1' };
  const on = { connect: [], disconnect: [] };
  function make() {
    const st = { inq: [], waiters: [], sock: [], parser: null, gone: false };
    const take = () => { const n = st.inq.reduce((a, x) => a + x.length, 0), d = new Uint8Array(n); let at = 0; for (const x of st.inq.splice(0)) { d.set(x, at); at += x.length; } return { status: 'ok', data: new DataView(d.buffer) }; };
    const push = u => { st.inq.push(u); const w = st.waiters.shift(); if (w) w.ok(take()); };
    const gone = () => new DOMException('The device was disconnected.', 'NotFoundError');
    async function frame(m, f) {
      const T = m.T, send = (t, c, p) => push(m.encode(t, 1, c, p)), part = (t, c, p) => push(m.encode(t, 0, c, p));
      if (f.type === T.HELLO) {
        F.hello = JSON.parse(new TextDecoder().decode(f.payload)); st.sock = [null, null];
        send(T.HELLO_ACK, 0, te.encode(JSON.stringify({ proto: 1, fw: '1.6.0', build: 'e2e', name: 'MegaOpen-E2E', chans: 2 })));
      } else if (f.type === T.PING) send(T.PONG, f.chan, f.payload);
      else if (f.type === T.HTTP_CLOSE) st.sock[f.chan] = null;
      else if (f.type === T.HTTP_REQ) {
        const c = f.chan;
        if (c > 1) { send(T.HTTP_FAIL, c, te.encode('busy')); return; }
        st.sock[c] = m.concat(st.sock[c] || new Uint8Array(0), f.payload);
        const b = st.sock[c], s = lat(b), he = s.indexOf('\\r\\n\\r\\n');
        if (he < 0) return;
        const cl = /\\r\\ncontent-length:\\s*(\\d+)/i.exec(s.slice(0, he)), len = cl ? +cl[1] : 0;
        if (b.length < he + 4 + len) return;
        const raw = b.slice(0, he + 4 + len);
        st.sock[c] = b.slice(he + 4 + len);
        const out = new Uint8Array(await (await fetch('/__board', { method: 'POST', body: raw })).arrayBuffer());
        if (st.gone) return;
        for (let at = 0; at < out.length; at += 1024) part(T.HTTP_RESP, c, out.subarray(at, at + 1024));
      }
    }
    return {
      vendorId: 0x303A, productId: 0x80C3, productName: 'MegaOpen', opened: false, configuration: null,
      configurations: [{ configurationValue: 1, interfaces: [
        { interfaceNumber: 0, alternates: [{ alternateSetting: 0, interfaceClass: 255, endpoints: [
          { endpointNumber: 1, direction: 'out', type: 'bulk' }, { endpointNumber: 1, direction: 'in', type: 'bulk' }] }] },
        { interfaceNumber: 1, alternates: [{ alternateSetting: 0, interfaceClass: 2, endpoints: [] }] }] }],
      async open() { if (st.gone) throw gone(); this.opened = true; F.opens++; },
      async selectConfiguration() { this.configuration = this.configurations[0]; },
      async claimInterface(n) { if (n !== 0) throw new DOMException('Unable to claim interface.', 'NetworkError'); },
      async releaseInterface() {}, async close() { this.opened = false; }, async clearHalt() {},
      transferIn() { if (st.gone) return Promise.reject(gone()); if (st.inq.length) return Promise.resolve(take()); return new Promise((ok, no) => st.waiters.push({ ok, no })); },
      async transferOut(ep, data) {
        if (st.gone) throw gone();
        const m = await L; if (!st.parser) st.parser = new m.Parser();
        st.parser.feed(new Uint8Array(data), f => { frame(m, f); });
        return { status: 'ok', bytesWritten: data.byteLength };
      },
      unplug() { st.gone = true; for (const w of st.waiters.splice(0)) w.no(gone()); },
    };
  }
  let dev = make();
  const usb = {
    async getDevices() { return F.granted && F.plugged ? [dev] : []; },
    async requestDevice(o) { F.filters = o.filters; F.granted = true; sessionStorage.setItem('fake-granted', '1'); return dev; },
    addEventListener(t, f) { on[t].push(f); }, removeEventListener() {},
  };
  Object.defineProperty(Navigator.prototype, 'usb', { get() { return usb; }, configurable: true });
  F.unplug = () => { F.plugged = false; const d = dev; d.unplug(); on.disconnect.forEach(f => f({ device: d })); };
  F.plug = () => { dev = make(); F.plugged = true; on.connect.forEach(f => f({ device: dev })); };
})();`;

// ---- Chrome ----------------------------------------------------------------------
const profile = join(tmpdir(), 'mo-webusb-e2e-' + process.pid + '-' + Date.now());
const chrome = spawn(CHROME, ['--headless=new', '--disable-gpu', '--no-sandbox', '--disable-dev-shm-usage', '--remote-debugging-port=0',
  '--user-data-dir=' + profile, 'about:blank'], { stdio: 'ignore' });
const stop = code => {
  try { chrome.kill(); } catch (e) {}
  try { simProc.kill(); } catch (e) {}
  server.close();
  setTimeout(() => { try { rmSync(profile, { recursive: true, force: true }); } catch (e) {} process.exit(code); }, 300);
};
let ws, nextId = 0;
const pending = new Map(), errors = [];
for (let i = 0; i < 300 && !ws; i++) {
  try {
    const p = readFileSync(join(profile, 'DevToolsActivePort'), 'utf8').split('\n')[0].trim();
    const pg = (await (await fetch(`http://127.0.0.1:${p}/json/list`)).json()).find(x => x.type === 'page');
    if (pg) ws = new WebSocket(pg.webSocketDebuggerUrl);
  } catch (e) {}
  if (!ws) await sleep(100);
}
if (!ws) { console.error('webusb_e2e: Chrome did not start: ' + CHROME); stop(2); }
await new Promise(r => ws.onopen = r);
ws.onmessage = m => {
  const d = JSON.parse(m.data);
  if (d.id && pending.has(d.id)) { pending.get(d.id)(d); pending.delete(d.id); }
  else if (d.method === 'Runtime.exceptionThrown') errors.push(d.params.exceptionDetails.exception?.description || d.params.exceptionDetails.text);
};
const cmd = (method, params = {}) => new Promise(r => { const i = ++nextId; pending.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
const ev = async expr => {
  const r = await cmd('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true });
  if (r.result.exceptionDetails) return { __err: r.result.exceptionDetails.exception?.description || r.result.exceptionDetails.text };
  return r.result.result.value;
};
const until = async (expr, ms = 8000) => {
  for (const end = Date.now() + ms; Date.now() < end; await sleep(100)) {
    const v = await ev(expr);
    if (v && !v.__err) return v;
  }
  return false;
};
// The board's page in the current frame, and its own realm (its const globals).
const FR = `document.querySelector('#box iframe')`;
const inFrame = js => `(() => { const f = ${FR}; return f && f.contentWindow && f.contentWindow.eval(${JSON.stringify(js)}); })()`;
const loaded = inFrame(`typeof CAT !== 'undefined' && !!CAT && !!document.getElementById('app')`);

try {
  await cmd('Runtime.enable');
  await cmd('Page.enable');
  await cmd('Page.addScriptToEvaluateOnNewDocument', { source: FAKE });
  const URL0 = `http://127.0.0.1:${PORT}/MegaOpen/usb/`;
  await cmd('Page.navigate', { url: URL0 });
  ok(await until(`!!window.__moShell && document.getElementById('st').textContent === 'не підключено'`), 'shell up, idle');
  ok(await ev(`!document.getElementById('intro').hidden && document.getElementById('box').hidden`), 'intro shown first');

  await ev(`document.getElementById('go').click()`);
  ok(await until(loaded, 15000), 'board page drawn in the frame, catalog loaded over the link');
  ok(await ev(`JSON.stringify(__fake.filters) === '[{"vendorId":12346,"classCode":255}]'`), 'chooser filter: Espressif VID, vendor class');
  ok(await ev(`__fake.hello.app === 'web-usb' && !('net' in __fake.hello)`), 'HELLO without net');
  ok(await ev(`document.getElementById('st').textContent.includes('MegaOpen-E2E') && document.getElementById('dot').className.includes('on')`), 'bar shows the board');
  ok(seen.length > 3 && seen.every(s => s.host === '127.0.0.1' && s.gate === '1'), 'every request: Host 127.0.0.1, X-MegaOpen 1 (' + seen.length + ')');
  ok(seen.some(s => s.req.startsWith('GET /api/catalog')), 'the page asked /api/catalog over USB');
  ok(await ev(inFrame(`typeof LINK === 'object' && LINK !== null`)), 'the page took window.MOLink');

  await ev(inFrame(`window.__mark = 1; document.querySelector('a[href="#board"]:not([hidden])').click()`));
  await sleep(400);
  ok(await ev(inFrame(`location.hash === '#board' && window.__mark === 1 && !!document.getElementById('app')`)), '"#board" stays in the same document');
  ok(await ev(`location.href === ${JSON.stringify(URL0)}`), 'the shell did not move');

  const post = await ev(inFrame(`API.post('/api/settings', {logLevel: 'DBG'}).then(r => JSON.stringify(r))`));
  const sp = seen.filter(s => s.req === 'POST /api/settings').pop();
  ok(typeof post === 'string' && sp && sp.type.includes('json') && sp.len > 10, 'POST /api/settings over the link: ' + post);

  await ev(inFrame(`download('/api/logs/file?n=0', 'boot-0.log')`));
  ok(await until(`/^Збережено: boot-0\.log/.test(document.getElementById('toast').textContent)`), 'download over the link: ' + await ev(`document.getElementById('toast').textContent`));
  ok(seen.some(s => s.req === 'GET /api/logs/file?n=0'), 'download asked the board');

  await ev(inFrame(`window.__old = 1; document.getElementById('langBtn').click()`));
  ok(await until(inFrame(`!window.__old && document.documentElement.lang === 'en' && typeof CAT !== 'undefined' && !!CAT`), 15000), 'language switch: reloaded through the shell, English');
  ok(seen.some(s => s.req === 'GET /i18n/en.json'), 'en.json fetched over the link (gzipped)');
  ok(await ev(`document.querySelectorAll('#bar').length === 1 && document.querySelectorAll('#box iframe').length === 1 && !${FR}.contentDocument.getElementById('intro')`), 'no shell inside the shell');
  ok(await ev(`document.getElementById('lang').textContent === 'UK'`), 'the shell follows the language');

  const opens = await ev(`__fake.opens`);
  await ev(`__fake.unplug()`);
  ok(await until(`!document.getElementById('lost').hidden && document.getElementById('dot').className.includes('bad')`), 'unplug: the page veiled');
  await ev(`__fake.plug()`);
  ok(await until(`document.getElementById('lost').hidden && __fake.opens === ${opens + 1}`, 10000), 'replug: connected again');
  ok(await until(loaded, 15000), 'replug: the page drawn again');

  await ev(`document.getElementById('off').click()`);
  ok(await until(`!document.getElementById('intro').hidden && document.getElementById('box').hidden`), 'disconnect: intro back');

  ok(await until(`navigator.serviceWorker.ready.then(() => caches.open('mo-usb-1')).then(c => c.keys()).then(k => k.length === 4)`, 10000), 'service worker cached the shell');
  offline = true;
  await cmd('Page.reload');     // not ignoreCache: that one skips the service worker
  await sleep(500);
  ok(await until(`!!document.getElementById('go') && document.title === 'MegaOpen USB' && !!window.__moShell`, 10000), 'offline: the shell opens from the cache');
  ok(await until(loaded, 15000), 'offline: and the board page still comes over USB');
  offline = false;

  ok(errors.length === 0, 'no uncaught exceptions' + (errors.length ? ': ' + errors.join(' | ').slice(0, 600) : ''));
} catch (e) {
  ok(false, 'webusb_e2e threw: ' + (e.stack || e));
}
console.log(`${passes} passed, ${fails} failed`);
stop(fails ? 1 : 0);
