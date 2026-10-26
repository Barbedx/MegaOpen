// web/usb/link.js under Node: the frames against LinkFrame.cpp's rules, the
// HTTP framing against HttpWire.kt's, and the Board against a fake board on a fake
// USBDevice that speaks the link as src/link/UsbLink.cpp does (two channels,
// FIN when the server closes, 91 on a failure, PONG echoes the PING).
//
//   node tools/test_webusb.mjs          (exit 1 on a failure; Node 18+)
import { readFileSync } from 'node:fs';
import { gzipSync } from 'node:zlib';

const src = readFileSync(new URL('../web/usb/link.js', import.meta.url), 'utf8');
const L = await import('data:text/javascript;base64,' + Buffer.from(src).toString('base64'));
const { T, FIN, encode, Parser, crc16, httpRequest, HttpReader, Board } = L;

const te = new TextEncoder(), td = new TextDecoder();
let fails = 0, passes = 0;
const ok = (c, what) => { if (c) ++passes; else { ++fails; console.log('FAIL ' + what); } };
const eq = (a, b, what) => ok(a === b, `${what}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`);
const sleep = ms => new Promise(r => setTimeout(r, ms));
const waitFor = async (f, ms = 500) => { for (const end = Date.now() + ms; Date.now() < end; await sleep(5)) if (f()) return true; return f(); };
const frames = u8 => { const out = []; new Parser().feed(u8, f => out.push(f)); return out; };

// ---- frames ------------------------------------------------------------------
eq(crc16(te.encode('123456789')), 0x29B1, 'crc16 check value');
{
  const f = encode(T.HTTP_REQ, FIN, 1, te.encode('abc'));
  eq(Array.from(f.subarray(0, 8)).join(','), '77,79,16,1,1,0,3,0', 'header bytes');
  const got = frames(f);
  ok(got.length === 1 && got[0].type === T.HTTP_REQ && got[0].chan === 1 && td.decode(got[0].payload) === 'abc', 'round trip');
  const bad = f.slice(); bad[9] ^= 1;
  const p = new Parser(), out = [];
  p.feed(new Uint8Array([0x4D, 1, 2, 0x4F]), x => out.push(x));
  p.feed(bad, x => out.push(x));
  for (const b of encode(T.PONG, FIN, 0, te.encode('x'))) p.feed(new Uint8Array([b]), x => out.push(x));
  ok(out.length === 1 && out[0].type === T.PONG, 'garbage and a bad CRC are dropped, a byte-at-a-time frame is read');
  ok(p.bad > 0, 'bad counted');
  let threw = false;
  try { encode(T.HTTP_REQ, 0, 0, new Uint8Array(1025)); } catch (e) { threw = true; }
  ok(threw, 'payload over 1024 refused');
  // A frame the board cut: the next whole frame ends the wait for its length.
  const big = encode(T.HTTP_RESP, 0, 0, new Uint8Array(1024).fill(0x41)), q = new Parser(), seen = [];
  q.feed(L.concat(big.subarray(0, 300), encode(T.PONG, FIN, 0)), x => seen.push(x));
  ok(seen.length === 1 && seen[0].type === T.PONG && q.bad === 300, 'a cut frame does not swallow the next one');
  const r = new Parser(), got2 = [];
  r.feed(big.subarray(0, 600), x => got2.push(x));
  r.feed(big.subarray(600), x => got2.push(x));
  ok(got2.length === 1 && got2[0].payload.length === 1024 && r.bad === 0, 'a frame in two pieces is still one frame');
}

// ---- HTTP framing --------------------------------------------------------------
eq(td.decode(httpRequest('GET', '/api/status', { Host: 'x', Connection: 'close', Accept: '*/*' })),
  'GET /api/status HTTP/1.1\r\nHost: 127.0.0.1\r\nX-MegaOpen: 1\r\nAccept: */*\r\n\r\n', 'GET as HttpWire.kt');
eq(td.decode(httpRequest('POST', '/api/x', { 'Content-Type': 'a/b', 'Content-Length': '99' }, te.encode('{}'))),
  'POST /api/x HTTP/1.1\r\nHost: 127.0.0.1\r\nX-MegaOpen: 1\r\nContent-Type: a/b\r\nContent-Length: 2\r\n\r\n{}', 'POST as HttpWire.kt');
eq(td.decode(httpRequest('POST', '/a', {})), 'POST /a HTTP/1.1\r\nHost: 127.0.0.1\r\nX-MegaOpen: 1\r\nContent-Length: 0\r\n\r\n', 'empty POST');
ok(!td.decode(httpRequest('GET', '/a', { 'X-A': 'b\r\nEvil: 1' })).includes('\r\nEvil'), 'no header injection');
{
  const r = new HttpReader();
  r.push(te.encode('HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhe'));
  ok(r.done() === null, 'short body waits');
  r.push(te.encode('llo'));
  const x = r.done();
  ok(x && x.status === 200 && td.decode(x.body) === 'hello', 'content-length body');
  const c = new HttpReader(), parts = 'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nwiki\r\n5\r\npedia\r\n0\r\n\r\n';
  let y = null;
  for (const ch of parts) { ok(y === null, 'chunked not early'); c.push(te.encode(ch)); y = c.done(); }
  ok(y && td.decode(y.body) === 'wikipedia', 'chunked body, byte at a time');
  const n = new HttpReader();
  n.push(te.encode('HTTP/1.1 204 No Content\r\n\r\n'));
  ok(n.done() && n.done().body.length === 0, 'no length: no body');
  const e = new HttpReader();
  e.push(te.encode('HTTP/1.1 200 OK\r\n\r\nrest'));
  eq(td.decode(e.done(true).body), 'rest', 'no length, closed: the rest');
}

// ---- a fake board ----------------------------------------------------------------
// handler(req, ctl) answers one parsed request; ctl.reply(bytes, {fin}),
// ctl.fail(why), or nothing (it never answers).
class FakeDev {
  constructor(handler, chans = 2) {
    this.handler = handler;
    this.chansN = chans;
    this.opened = false;
    this.configuration = null;
    this.configurations = [{ interfaces: [
      { interfaceNumber: 0, alternates: [{ alternateSetting: 0, interfaceClass: 0xFF, endpoints: [
        { endpointNumber: 1, direction: 'out', type: 'bulk' }, { endpointNumber: 1, direction: 'in', type: 'bulk' }] }] },
      { interfaceNumber: 1, alternates: [{ alternateSetting: 0, interfaceClass: 2, endpoints: [] }] }] }];
    this.inq = [];
    this.waiters = [];
    this.parser = new Parser();
    this.sent = [];          // every frame the host wrote
    this.sock = [];          // per channel: bytes of a request in progress, or null when closed
    this.reqs = 0;
    this.gen = [];
    this.silent = false;
  }
  async open() { this.opened = true; }
  async selectConfiguration() { this.configuration = this.configurations[0]; }
  async claimInterface(n) { if (n !== 0) throw new Error('claim ' + n); this.claimed = true; }
  async releaseInterface() { this.claimed = false; }
  async close() { this.opened = false; this.unplug(); }
  unplug() { this.gone = true; for (const w of this.waiters.splice(0)) w.no(new Error('disconnected')); }
  push(u8) {
    if (this.silent) return;
    this.inq.push(u8);
    const w = this.waiters.shift();
    if (w) w.ok(this.take());
  }
  take() { const d = L.concat(...this.inq.splice(0)); return { status: 'ok', data: new DataView(d.buffer, d.byteOffset, d.byteLength) }; }
  transferIn() {
    if (this.gone) return Promise.reject(new Error('disconnected'));
    if (this.inq.length) return Promise.resolve(this.take());
    return new Promise((ok, no) => this.waiters.push({ ok, no }));
  }
  async transferOut(ep, data) {
    if (this.gone) throw new Error('disconnected');
    await sleep(0);
    this.parser.feed(data, f => { this.sent.push(f); this.onFrame(f); });
    return { status: 'ok', bytesWritten: data.length };
  }
  send(type, flags, chan, payload) { this.push(encode(type, flags, chan, payload)); }
  onFrame(f) {
    if (f.type === T.HELLO) {
      this.hello = JSON.parse(td.decode(f.payload));
      this.sock = Array(this.chansN).fill(null);
      this.send(T.HELLO_ACK, FIN, 0, te.encode(JSON.stringify({ proto: 1, fw: '1.6.0', build: 'abc', name: 'MegaOpen-TEST', chans: this.chansN })));
    } else if (f.type === T.PING) {
      this.send(T.PONG, FIN, f.chan, f.payload);
    } else if (f.type === T.HTTP_CLOSE) {
      this.shut(f.chan);
    } else if (f.type === T.HTTP_REQ) {
      const c = f.chan;
      if (c >= this.chansN) { this.send(T.HTTP_FAIL, FIN, c, te.encode('busy')); return; }
      if (this.sock[c] == null) this.sock[c] = new Uint8Array(0);
      this.sock[c] = L.concat(this.sock[c], f.payload);
      this.inflight = (this.inflight || 0);
      this.parse(c);
    }
  }
  // A socket closed is gone: what its handler sends later goes nowhere.
  shut(c) { this.sock[c] = null; this.gen[c] = (this.gen[c] || 0) + 1; }
  parse(c) {
    const b = this.sock[c];
    const s = td.decode(b), he = s.indexOf('\r\n\r\n');
    if (he < 0) return;
    const head = s.slice(0, he).split('\r\n'), h = {};
    for (const l of head.slice(1)) { const i = l.indexOf(':'); h[l.slice(0, i).toLowerCase()] = l.slice(i + 1).trim(); }
    const hb = te.encode(s.slice(0, he + 4)).length, len = +(h['content-length'] || 0);
    if (b.length < hb + len) return;
    this.sock[c] = b.slice(hb + len);
    const [method, path] = head[0].split(' ');
    const req = { method, path, headers: h, body: b.slice(hb, hb + len), chan: c, n: ++this.reqs };
    const g = this.gen[c] || 0, live = () => (this.gen[c] || 0) === g;
    const ctl = {
      reply: (bytes, o = {}) => {
        if (!live()) return;
        for (let at = 0; at < bytes.length; at += 1024) this.send(T.HTTP_RESP, 0, c, bytes.subarray(at, at + 1024));
        if (o.fin) { this.send(T.HTTP_RESP, FIN, c); this.shut(c); }
      },
      fin: () => { if (live()) { this.send(T.HTTP_RESP, FIN, c); this.shut(c); } },
      fail: why => { if (live()) { this.send(T.HTTP_FAIL, FIN, c, te.encode(why)); this.shut(c); } },
      // UsbLink's send() giving up: this frame only partly written, the
      // channel closed with no word.
      cutReply: (bytes, frame) => {
        for (let at = 0, i = 0; at < bytes.length; at += 1024, i++) {
          const f = encode(T.HTTP_RESP, 0, c, bytes.subarray(at, at + 1024));
          if (i === frame) { this.push(f.subarray(0, f.length >> 1)); this.shut(c); return; }
          this.push(f);
        }
      },
    };
    this.handler(req, ctl, this);
  }
}

const resp = (status, body, h = {}) => {
  const b = typeof body === 'string' ? te.encode(body) : body;
  let s = `HTTP/1.1 ${status} X\r\nContent-Length: ${b.length}\r\n`;
  for (const [k, v] of Object.entries(h)) s += `${k}: ${v}\r\n`;
  return L.concat(te.encode(s + '\r\n'), b);
};

const FAST = { ping: 50, silent: 600, idle: 200, grace: 100 };
async function withBoard(handler, fn, ms = FAST) {
  const dev = new FakeDev(handler);
  const b = new Board(dev, 'test', ms);
  let closedWhy = null;
  b.onclose = w => { closedWhy = w; };
  const hello = await b.open();
  try { await fn(b, dev, hello, () => closedWhy); } finally { b.close('done'); }
}

// Open: HELLO without "net", the channels from the answer.
await withBoard((req, ctl) => ctl.reply(resp(200, 'x')), async (b, dev, hello) => {
  eq(dev.hello.app, 'test', 'HELLO app');
  ok(!('net' in dev.hello), 'HELLO has no net');
  eq(hello.name, 'MegaOpen-TEST', 'hello answer');
  eq(b.chans.length, 2, 'two channels');
});

// Three at once over two channels; gzip undone; headers through.
await withBoard((req, ctl) => {
  if (req.path === '/') { ctl.reply(resp(200, gzipSync(Buffer.from('<html>page</html>')), { 'Content-Encoding': 'gzip', 'Content-Type': 'text/html' })); return; }
  setTimeout(() => ctl.reply(resp(200, '{"p":"' + req.path + '","ch":' + req.chan + '}', { 'Content-Type': 'application/json' })), 30);
}, async (b, dev) => {
  const rs = await Promise.all(['/a', '/b', '/c'].map(p => b.request('GET', p)));
  ok(rs.every((r, i) => JSON.parse(td.decode(r.body)).p === ['/a', '/b', '/c'][i]), 'three answers, each its own');
  ok(dev.sent.filter(f => f.type === T.HTTP_REQ).every(f => f.chan < 2), 'never a third channel');
  const g = await b.request('GET', '/');
  eq(td.decode(g.body), '<html>page</html>', 'gzip undone');
  ok(!('content-encoding' in g.headers) && g.headers['content-type'] === 'text/html', 'encoding header gone, type kept');
  eq(dev.sent.filter(f => f.type === T.HTTP_REQ && f.chan === 0).length + dev.sent.filter(f => f.type === T.HTTP_REQ && f.chan === 1).length, 4, 'four requests, one frame each');
});

// A big body: frames of at most 1024, FIN on the last only, every byte there.
await withBoard((req, ctl) => ctl.reply(resp(200, String(req.body.length) + ':' + req.body.reduce((a, x) => (a + x) % 65536, 0))), async (b, dev) => {
  const body = new Uint8Array(40000).map((_, i) => i * 7);
  const r = await b.request('POST', '/ota/upload', { 'Content-Type': 'application/octet-stream' }, body);
  eq(td.decode(r.body), '40000:' + body.reduce((a, x) => (a + x) % 65536, 0), 'body intact');
  const fr = dev.sent.filter(f => f.type === T.HTTP_REQ);
  ok(fr.every(f => f.payload.length <= 1024), 'frames <= 1024');
  ok(fr.slice(0, -1).every(f => !(f.flags & FIN)) && (fr[fr.length - 1].flags & FIN), 'FIN on the last only');
  await sleep(120);
  ok(dev.sent.some(f => f.type === T.PING), 'pings go out');
});

// A chunked answer, and a big one in many frames.
await withBoard((req, ctl) => {
  if (req.path === '/big') { ctl.reply(resp(200, new Uint8Array(300000).fill(65))); return; }
  ctl.reply(te.encode('HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n'));
}, async b => {
  eq(td.decode((await b.request('GET', '/log')).body), 'abcde', 'chunked over the link');
  eq((await b.request('GET', '/big')).body.length, 300000, '300 kB answer');
});

// The board drops a socket under a GET (91, or FIN with nothing): sent again
// once. A POST is not.
await withBoard((req, ctl, dev) => {
  if (req.path === '/flaky' && req.n === 1) { ctl.fail('timeout'); return; }
  if (req.path === '/closer' && req.n === 3) { ctl.fin(); return; }
  if (req.path === '/post') { ctl.fail('send'); return; }
  if (req.path === '/always') { ctl.fail('connect'); return; }
  ctl.reply(resp(200, 'ok' + req.n));
}, async b => {
  eq(td.decode((await b.request('GET', '/flaky')).body), 'ok2', 'GET after 91: retried');
  eq(td.decode((await b.request('GET', '/closer')).body), 'ok4', 'GET after FIN with nothing: retried');
  let e1 = null; try { await b.request('POST', '/post', {}, te.encode('x')); } catch (e) { e1 = e; }
  eq(e1 && e1.code, 'send', 'POST after 91: failed');
  let e2 = null; try { await b.request('GET', '/always'); } catch (e) { e2 = e; }
  eq(e2 && e2.code, 'connect', 'a second failure is final');
});

// A frame cut mid-answer: a GET is sent again at once (not after the 20 s
// timeout), a POST fails.
await withBoard((req, ctl) => {
  const body = new Uint8Array(5000).map((_, i) => 48 + i % 10);
  if (req.n === 1 || req.method === 'POST') { ctl.cutReply(resp(200, body), 2); return; }
  ctl.reply(resp(200, body));
}, async (b, dev) => {
  const t0 = Date.now();
  const r = await b.request('GET', '/api/catalog');
  ok(r.body.length === 5000 && r.body[4999] === 48 + 4999 % 10, 'cut GET: whole answer the second time');
  ok(Date.now() - t0 < 1000, 'cut GET: retried at once (' + (Date.now() - t0) + ' ms)');
  ok(await waitFor(() => dev.sent.some(f => f.type === T.HTTP_CLOSE)), 'cut: the channel closed');
  let e = null; try { await b.request('POST', '/api/x', {}, te.encode('{}')); } catch (x) { e = x; }
  eq(e && e.code, 'cut', 'cut POST fails');
});

// An error answer the server closes after: the channel waits for that FIN,
// so the next request does not meet it.
await withBoard((req, ctl) => {
  if (req.path === '/missing') { ctl.reply(resp(404, 'nope')); setTimeout(() => ctl.fin(), 20); return; }
  ctl.reply(resp(200, 'fine' + req.chan));
}, async (b, dev) => {
  const [m, f] = await Promise.all([b.request('GET', '/missing'), b.request('GET', '/x')]);
  eq(m.status, 404, '404 through');
  eq(td.decode(f.body), 'fine1', 'the other channel');
  ok(b.chans[0].hold, 'channel 0 waits for the close');
  await sleep(50);                     // the FIN at 20 ms, the grace at 100
  ok(!b.chans[0].hold, 'hold let go on the FIN');
  const g = await Promise.all([b.request('GET', '/y'), b.request('GET', '/z')]);
  ok(g.every(x => x.status === 200), 'both channels after the close');
});

// Our timeout: CLOSE, then the channel waits behind a PING; a late answer of
// the dead request never reaches the next one.
await withBoard((req, ctl, dev) => {
  if (req.path === '/slow') { setTimeout(() => ctl.reply(resp(200, 'LATE')), 150); return; }
  ctl.reply(resp(200, 'next'));
}, async (b, dev) => {
  let e = null;
  try { await b.request('GET', '/slow', {}, null, 80); } catch (x) { e = x; }
  eq(e && e.code, 'timeout', 'timed out');
  ok(await waitFor(() => dev.sent.some(f => f.type === T.HTTP_CLOSE && f.chan === 0)), 'CLOSE sent');
  const r = await Promise.all([b.request('GET', '/a'), b.request('GET', '/b')]);
  await sleep(200);
  ok(r.every(x => td.decode(x.body) === 'next'), 'next answers are their own: ' + r.map(x => td.decode(x.body)).join(','));
});

// An idle channel: closed by us before the board's 15 s.
await withBoard((req, ctl) => ctl.reply(resp(200, 'k')), async (b, dev) => {
  await b.request('GET', '/a');
  await sleep(350);
  ok(dev.sent.some(f => f.type === T.HTTP_CLOSE && f.chan === 0), 'idle channel closed');
  eq(b.chans[0].open, false, 'marked closed');
  eq(td.decode((await b.request('GET', '/b')).body), 'k', 'and used again');
});

// Frames that are not ours (an alert, the app's NET types) are ignored.
await withBoard((req, ctl, dev) => {
  dev.send(T.EVENT, FIN, 0, te.encode('{"t":"alert"}'));
  dev.send(0xC0, FIN, 1, te.encode('{}'));
  dev.send(0x7E, FIN, 0);
  ctl.reply(resp(200, 'z'));
}, async b => {
  eq(td.decode((await b.request('GET', '/a')).body), 'z', 'unknown types ignored');
});

// Unplugged: onclose once, the waiting requests fail, later ones too.
await withBoard(() => {}, async (b, dev, hello, why) => {
  const p = b.request('GET', '/never').catch(e => e.code);
  const q = b.request('GET', '/never2').catch(e => e.code);
  const r = b.request('GET', '/queued').catch(e => e.code);
  await sleep(20);
  dev.unplug();
  eq(await p, 'read', 'in flight fails');
  eq(await r, 'read', 'queued fails');
  await q;
  eq(why(), 'read', 'onclose(read)');
  let e = null; try { await b.request('GET', '/x'); } catch (x) { e = x; }
  eq(e && e.code, 'closed', 'after close');
});

// A board that stops talking: closed as silent.
await withBoard(() => {}, async (b, dev, hello, why) => {
  dev.silent = true;
  await sleep(900);
  eq(why(), 'silent', 'silent board closed');
}, { ...FAST, silent: 300 });

// No HELLO answer: open fails, the device is let go.
{
  const dev = new FakeDev(() => {});
  dev.onFrame = () => {};
  const b = new Board(dev, 'test', FAST);
  let e = null;
  try { await b.open(); } catch (x) { e = x; }
  eq(e && e.code, 'no_hello', 'no hello');
  await sleep(10);
  eq(dev.opened, false, 'device closed after a failed open');
}

// No vendor interface: a clear error.
{
  const dev = new FakeDev(() => {});
  dev.configurations = [{ interfaces: [{ interfaceNumber: 0, alternates: [{ alternateSetting: 0, interfaceClass: 3, endpoints: [] }] }] }];
  let e = null;
  try { await new Board(dev).open(); } catch (x) { e = x; }
  eq(e && e.code, 'no_itf', 'no vendor interface');
}

console.log(`${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
