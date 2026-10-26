// The USB link (docs/design/usb-link.md) from a browser: WebUSB on the board's
// vendor interface, the frames of src/link/LinkFrame.*, and HTTP to the
// board's own web server piped through them, as the app does
// (android/.../UsbBoard.kt, Http.kt). No DOM here: tools/test_webusb.mjs runs
// it under Node against a fake device.

export const T = {
  HELLO: 0x01, PING: 0x02, HTTP_REQ: 0x10, HTTP_CLOSE: 0x11,
  HELLO_ACK: 0x81, PONG: 0x82, HTTP_RESP: 0x90, HTTP_FAIL: 0x91, EVENT: 0xA0,
};
export const FIN = 0x01, HEAD = 8, MAX_PAYLOAD = 1024;

// How long a request may take (an update gets the app's two minutes), how
// often the board hears from us (alive means a frame within 3 s), and when a
// quiet board counts as gone.
export const REQ_MS = 20000, OTA_MS = 120000, PING_MS = 1000, SILENT_MS = 15000;
// The board closes a channel idle 15 s with `91 timeout`, which could cross a
// new request on it; we close our own idle ones first.
const IDLE_CLOSE_MS = 10000;
// A server that closes after an error answer sends its FIN just after it: the
// channel waits for it this long before the next request.
const CLOSE_GRACE_MS = 300;

export class LinkError extends Error {
  constructor(code) { super(code); this.code = code; }
}

const te = new TextEncoder(), td = new TextDecoder();

export function concat(...parts) {
  let n = 0;
  for (const p of parts) n += p.length;
  const out = new Uint8Array(n);
  let at = 0;
  for (const p of parts) { out.set(p, at); at += p.length; }
  return out;
}

const latin1 = s => { const u = new Uint8Array(s.length); for (let i = 0; i < s.length; i++) u[i] = s.charCodeAt(i) & 0xFF; return u; };
const unlatin1 = u => { let s = ''; for (let i = 0; i < u.length; i += 0x8000) s += String.fromCharCode.apply(null, u.subarray(i, i + 0x8000)); return s; };

// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF), as LinkFrame.cpp.
export function crc16(d, crc = 0xFFFF) {
  for (let i = 0; i < d.length; i++) {
    crc ^= d[i] << 8;
    for (let b = 0; b < 8; b++) crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xFFFF : (crc << 1) & 0xFFFF;
  }
  return crc;
}

export function encode(type, flags, chan, payload = new Uint8Array(0)) {
  const n = payload.length;
  if (n > MAX_PAYLOAD) throw new RangeError('payload ' + n);
  const f = new Uint8Array(HEAD + n + 2);
  f[0] = 0x4D; f[1] = 0x4F; f[2] = type; f[3] = flags;
  f[4] = chan & 0xFF; f[5] = chan >> 8; f[6] = n & 0xFF; f[7] = n >> 8;
  f.set(payload, HEAD);
  const c = crc16(f.subarray(2, HEAD + n));
  f[HEAD + n] = c & 0xFF; f[HEAD + n + 1] = c >> 8;
  return f;
}

// Bytes in, frames out. A bad magic, length or CRC drops one byte and looks
// for the magic again, as the board's parser does.
//
// One more than the board's: a frame the board cut (it gives up writing after
// 500 ms) says a length it never delivers, and waiting for that many bytes
// would swallow the frames after it, the PONGs too, until the link looks dead.
// So a whole frame with a good CRC inside the span of an unfinished one ends
// the wait: the unfinished one was cut.
export class Parser {
  constructor() { this.rest = new Uint8Array(0); this.bad = 0; }
  feed(d, onFrame) {
    const b = this.rest.length ? concat(this.rest, d) : d;
    let at = 0;
    for (;;) {
      while (at < b.length && b[at] !== 0x4D) { ++at; ++this.bad; }
      if (b.length - at < 2) break;
      if (b[at + 1] !== 0x4F) { ++at; ++this.bad; continue; }
      if (b.length - at < HEAD) break;
      const len = b[at + 6] | b[at + 7] << 8;
      if (len > MAX_PAYLOAD) { ++at; ++this.bad; continue; }
      if (b.length - at < HEAD + len + 2) {
        const next = wholeFrameAfter(b, at + 1);
        if (next < 0) break;
        this.bad += next - at;
        at = next;
        continue;
      }
      if (!crcOk(b, at, len)) { ++at; ++this.bad; continue; }
      onFrame({ type: b[at + 2], flags: b[at + 3], chan: b[at + 4] | b[at + 5] << 8, payload: b.slice(at + HEAD, at + HEAD + len) });
      at += HEAD + len + 2;
    }
    this.rest = b.slice(at);
  }
}

function crcOk(b, at, len) {
  return crc16(b.subarray(at + 2, at + HEAD + len)) === (b[at + HEAD + len] | b[at + HEAD + len + 1] << 8);
}

// Where the first complete, CRC-good frame at or after `from` starts; -1 if none.
function wholeFrameAfter(b, from) {
  for (let i = from; i + HEAD + 2 <= b.length; i++) {
    if (b[i] !== 0x4D || b[i + 1] !== 0x4F) continue;
    const len = b[i + 6] | b[i + 7] << 8;
    if (len <= MAX_PAYLOAD && i + HEAD + len + 2 <= b.length && crcOk(b, i, len)) return i;
  }
  return -1;
}

// The request as Http.kt writes it: an IP literal passes the board's Host
// check (src/net/RequestGate.h), X-MegaOpen lets a change through.
export function httpRequest(method, path, headers = {}, body = null) {
  const clean = s => String(s).replace(/[\r\n]/g, '');
  let h = clean(method) + ' ' + clean(path) + ' HTTP/1.1\r\nHost: 127.0.0.1\r\nX-MegaOpen: 1\r\n';
  for (const [k, v] of Object.entries(headers)) {
    const lk = k.toLowerCase();
    if (lk === 'host' || lk === 'content-length' || lk === 'x-megaopen' || lk === 'connection') continue;
    h += clean(k) + ': ' + clean(v) + '\r\n';
  }
  if (body != null || method === 'POST') h += 'Content-Length: ' + (body ? body.length : 0) + '\r\n';
  const head = latin1(h + '\r\n');
  return body && body.length ? concat(head, body) : head;
}

// One response off a channel. esp_http_server keeps the connection open, so
// it ends where its framing says (Content-Length, or chunked's last chunk),
// or where the server closed.
export class HttpReader {
  constructor() { this.b = new Uint8Array(4096); this.n = 0; this.he = -1; this.scan = 0; }
  push(d) {
    if (this.n + d.length > this.b.length) {
      let cap = this.b.length * 2;
      while (cap < this.n + d.length) cap *= 2;
      const nb = new Uint8Array(cap);
      nb.set(this.b.subarray(0, this.n));
      this.b = nb;
    }
    this.b.set(d, this.n);
    this.n += d.length;
  }
  // {status, headers, body} once all of it is here; null before. eof: the
  // server closed, so a response with no length ends here.
  done(eof = false) {
    const b = this.b.subarray(0, this.n);
    if (this.he < 0) {
      for (let i = this.scan; i + 3 < b.length; i++) {
        if (b[i] === 13 && b[i + 1] === 10 && b[i + 2] === 13 && b[i + 3] === 10) { this.he = i; break; }
      }
      if (this.he < 0) { this.scan = Math.max(0, b.length - 3); return null; }
      const lines = unlatin1(b.subarray(0, this.he)).split('\r\n');
      this.status = parseInt((lines[0] || '').split(' ')[1], 10) || 0;
      this.headers = {};
      for (const l of lines.slice(1)) {
        const i = l.indexOf(':');
        if (i > 0) this.headers[l.slice(0, i).trim().toLowerCase()] = l.slice(i + 1).trim();
      }
      this.at = this.he + 4;
      this.parts = [];
    }
    let body;
    if ((this.headers['transfer-encoding'] || '').toLowerCase() === 'chunked') {
      for (;;) {       // this.at: the next chunk's size line; the chunks before it are in this.parts
        let eol = -1;
        for (let i = this.at; i + 1 < b.length; i++) if (b[i] === 13 && b[i + 1] === 10) { eol = i; break; }
        if (eol < 0) return null;
        const size = parseInt(unlatin1(b.subarray(this.at, eol)).split(';')[0].trim(), 16);
        if (!(size >= 0)) return null;
        if (size === 0) {
          if (b.length - (eol + 2) < 2) return null;
          break;
        }
        if (b.length - (eol + 2) < size + 2) return null;
        this.parts.push([eol + 2, size]);
        this.at = eol + 2 + size + 2;
      }
      body = concat(...this.parts.map(([o, s]) => b.subarray(o, o + s)));
    } else if ('content-length' in this.headers) {
      const len = parseInt(this.headers['content-length'], 10) || 0;
      if (b.length - this.at < len) return null;
      body = b.slice(this.at, this.at + len);
    } else {
      // Neither: Http.kt reads no body; only a close can say there was one.
      body = eof ? b.slice(this.at) : new Uint8Array(0);
    }
    return { status: this.status, headers: this.headers, body };
  }
}

export async function gunzip(u8) {
  const s = new Blob([u8]).stream().pipeThrough(new DecompressionStream('gzip'));
  return new Uint8Array(await new Response(s).arrayBuffer());
}

// The vendor interface: class FF with a bulk endpoint each way.
export function findVendor(cfg) {
  for (const itf of (cfg && cfg.interfaces) || []) {
    for (const alt of itf.alternates) {
      if (alt.interfaceClass !== 0xFF) continue;
      const ep = dir => alt.endpoints.find(e => e.type === 'bulk' && e.direction === dir);
      const i = ep('in'), o = ep('out');
      if (i && o) return { itf: itf.interfaceNumber, alt: alt.alternateSetting, epIn: i.endpointNumber, epOut: o.endpointNumber };
    }
  }
  return null;
}

const json = u8 => { try { return JSON.parse(td.decode(u8)); } catch (e) { return null; } };

// One board on one USBDevice (WebUSB, or anything with the same calls).
// request() queues over the channels HELLO gave; onclose(why) once when the
// link is gone, whoever closed it. ms: the timings above, for the tests.
export class Board {
  constructor(dev, app = 'web', ms = {}) {
    this.dev = dev;
    this.app = app;
    this.ms = { ping: PING_MS, silent: SILENT_MS, idle: IDLE_CLOSE_MS, grace: CLOSE_GRACE_MS, ...ms };
    this.hello = null;
    this.closed = false;
    this.onclose = null;
    this.chans = [];
    this.queue = [];
    this.parser = new Parser();
    this.wq = Promise.resolve();
    this.lastRx = Date.now();
    this.pingN = 0;
    this.barriers = new Map();
  }

  async open() {
    const d = this.dev;
    try {
      await d.open();
      if (!d.configuration) await d.selectConfiguration(1);
      const v = findVendor(d.configuration);
      if (!v) throw new LinkError('no_itf');
      await d.claimInterface(v.itf);
      if (v.alt) await d.selectAlternateInterface(v.itf, v.alt);
      this.itf = v.itf; this.epIn = v.epIn; this.epOut = v.epOut;
      this.readLoop();
      let timer;
      const ack = new Promise((ok, no) => {
        this.helloWait = ok;
        this.helloFail = no;
        timer = setTimeout(() => no(new LinkError('no_hello')), 3000);
      });
      // No "net": the board's own GitHub requests stay on its WiFi.
      await this.send(T.HELLO, FIN, 0, te.encode(JSON.stringify({ app: this.app })));
      try { this.hello = await ack; } finally { clearTimeout(timer); }
    } catch (e) {
      this.close();
      throw e;
    }
    const n = Math.min(8, Math.max(1, this.hello.chans | 0 || 1));
    this.chans = Array.from({ length: n }, (_, i) => ({ n: i, job: null, open: false, last: 0, hold: null }));
    this.lastRx = Date.now();
    this.ticker = setInterval(() => this.tick(), this.ms.ping);
    return this.hello;
  }

  close(why = 'closed') {
    if (this.closed) return;
    this.closed = true;
    clearInterval(this.ticker);
    const err = new LinkError(why);
    for (const ch of this.chans) {
      clearTimeout(ch.hold);
      if (ch.job) { clearTimeout(ch.job.timer); ch.job.no(err); ch.job = null; }
    }
    for (const j of this.queue.splice(0)) j.no(err);
    if (this.helloFail) this.helloFail(err);
    this.helloWait = this.helloFail = null;
    const d = this.dev;
    Promise.resolve()
      .then(() => this.itf != null && d.opened && d.releaseInterface(this.itf)).catch(() => {})
      .then(() => d.opened && d.close()).catch(() => {});
    if (this.onclose) this.onclose(why);
  }

  // A method, a path, headers as an object, a body as bytes or null; the
  // answer as {status, headers, body (bytes, gzip undone)}.
  request(method, path, headers = {}, body = null, timeoutMs = REQ_MS) {
    return new Promise((ok, no) => {
      if (this.closed || !this.hello) { no(new LinkError('closed')); return; }
      this.queue.push({ method, path, headers, body, timeoutMs, ok, no, tries: 0 });
      this.pump();
    });
  }

  // ---- inside ---------------------------------------------------------------

  write(u8) {
    const p = this.wq.then(() => {
      if (this.closed) throw new LinkError('closed');
      return this.dev.transferOut(this.epOut, u8);
    }).then(r => { if (r.status !== 'ok') throw new LinkError('write'); });
    this.wq = p.catch(() => {});
    p.catch(() => this.close('write'));
    return p;
  }

  send(type, flags, chan, payload) { return this.write(encode(type, flags, chan, payload)); }

  async readLoop() {
    while (!this.closed) {
      let r;
      try {
        r = await this.dev.transferIn(this.epIn, 16384);
      } catch (e) {
        this.close('read');
        return;
      }
      if (r.status === 'stall') {
        try { await this.dev.clearHalt('in', this.epIn); } catch (e) { this.close('read'); return; }
        continue;
      }
      if (!r.data || !r.data.byteLength) continue;
      this.lastRx = Date.now();
      const bad = this.parser.bad;
      this.parser.feed(new Uint8Array(r.data.buffer, r.data.byteOffset, r.data.byteLength), f => {
        try { this.onFrame(f); } catch (e) { /* one bad frame is not the link */ }
      });
      if (this.parser.bad !== bad && this.hello) this.cut();
    }
  }

  tick() {
    const now = Date.now();
    if (now - this.lastRx > this.ms.silent) { this.close('silent'); return; }
    this.send(T.PING, FIN, 0).catch(() => {});
    for (const ch of this.chans) {
      if (!ch.job && !ch.hold && ch.open && now - ch.last >= this.ms.idle) {
        ch.open = false;
        this.send(T.HTTP_CLOSE, FIN, ch.n).catch(() => {});
      }
    }
  }

  onFrame(f) {
    if (f.type === T.HELLO_ACK) {
      const w = this.helloWait;
      this.helloWait = this.helloFail = null;
      if (w) w(json(f.payload) || {});
      return;
    }
    if (!this.hello) return;   // what the board sent for an earlier app
    if (f.type === T.PONG) {
      if (f.payload.length === 4) {
        const k = f.payload[0] | f.payload[1] << 8 | f.payload[2] << 16 | f.payload[3] << 24;
        const b = this.barriers.get(k);
        if (b) { this.barriers.delete(k); b(); }
      }
      return;
    }
    const ch = this.chans[f.chan];
    if (!ch) return;
    if (f.type === T.HTTP_RESP) this.onResp(ch, f);
    else if (f.type === T.HTTP_FAIL) this.onFail(ch, td.decode(f.payload));
    // Anything else (EVENT, the NET types an app with "net" gets) is not ours.
  }

  pump() {
    while (this.queue.length && !this.closed) {
      const ch = this.chans.find(c => !c.job && !c.hold);
      if (!ch) return;
      this.start(ch, this.queue.shift());
    }
  }

  start(ch, job) {
    ch.job = job;
    ch.open = true;
    job.rd = new HttpReader();
    job.got = 0;
    job.timer = setTimeout(() => this.abort(ch, 'timeout'), job.timeoutMs);
    this.sendRequest(ch.n, httpRequest(job.method, job.path, job.headers, job.body)).catch(() => {});
  }

  // A frame at a time is a transfer at a time; batches of ~8 kB, each awaited
  // before the next, let the pings in between during an upload.
  async sendRequest(chan, raw) {
    let batch = [], size = 0;
    for (let at = 0; ;) {
      const n = Math.min(MAX_PAYLOAD, raw.length - at), last = at + n >= raw.length;
      const fr = encode(T.HTTP_REQ, last ? FIN : 0, chan, raw.subarray(at, at + n));
      batch.push(fr);
      size += fr.length;
      at += n;
      if (size >= 8192 || last) {
        await this.write(batch.length === 1 ? batch[0] : concat(...batch));
        batch = []; size = 0;
      }
      if (last) return;
    }
  }

  onResp(ch, f) {
    const job = ch.job;
    const fin = (f.flags & FIN) !== 0;
    if (!job) {
      // The server closed a channel nobody waits on (the close after an
      // error answer, or an idle one): the next request opens a new socket.
      if (fin) { ch.open = false; this.release(ch); }
      return;
    }
    if (f.payload.length) { job.rd.push(f.payload); job.got += f.payload.length; }
    if (fin) ch.open = false;
    const r = job.rd.done(fin);
    if (r) { this.finish(ch, null, r); return; }
    if (!fin) return;
    // Closed with the request unread (the server dropped the socket under
    // it): a GET is safe to send again, once.
    if (job.got === 0) this.retry(ch, 'closed');
    else this.finish(ch, new LinkError('closed'));
  }

  onFail(ch, why) {
    ch.open = false;        // the board closed that socket
    if (!ch.job) return;    // an idle channel the board timed out
    if (why === 'busy') { this.requeue(ch, 100); return; }
    if (ch.job.got === 0) this.retry(ch, why);
    else this.finish(ch, new LinkError(why));
  }

  retry(ch, why) {
    const job = ch.job;
    if (job.method !== 'GET' || job.tries > 0) { this.finish(ch, new LinkError(why)); return; }
    job.tries++;
    this.requeue(ch, 50);
  }

  requeue(ch, ms) {
    const job = ch.job;
    clearTimeout(job.timer);
    ch.job = null;
    setTimeout(() => {
      if (this.closed) { job.no(new LinkError('closed')); return; }
      this.queue.unshift(job);
      this.pump();
    }, ms);
    this.pump();
  }

  // Our timeout: the board drops the socket on CLOSE; a PING after it comes
  // back behind whatever the board sent on that channel before, so the
  // channel is free again only then.
  abort(ch, why) {
    if (!ch.job) return;
    this.drop(ch);
    this.finish(ch, new LinkError(why));
  }

  drop(ch) {
    ch.open = false;
    this.send(T.HTTP_CLOSE, FIN, ch.n).catch(() => {});
    this.hold(ch, 3000);
    const k = (++this.pingN) >>> 0, p = new Uint8Array([k & 0xFF, k >> 8 & 0xFF, k >> 16 & 0xFF, k >>> 24]);
    this.barriers.set(k | 0, () => this.release(ch));
    this.send(T.PING, FIN, 0, p).catch(() => {});
  }

  // A frame cut on the way (the board gave up writing it: a host that did not
  // read for 500 ms) took bytes of some answer with it, and frames carry no
  // sequence number, so every answer in flight is suspect. A GET goes again
  // once, on a channel past its barrier; anything else fails.
  cut() {
    for (const ch of this.chans) {
      const job = ch.job;
      if (!job) continue;
      this.drop(ch);
      clearTimeout(job.timer);
      ch.job = null;
      if (job.method === 'GET' && job.tries === 0) { job.tries++; this.queue.unshift(job); }
      else job.no(new LinkError('cut'));
    }
    this.pump();
  }

  hold(ch, ms) {
    clearTimeout(ch.hold);
    ch.hold = setTimeout(() => this.release(ch), ms);
  }

  release(ch) {
    if (!ch.hold) return;
    clearTimeout(ch.hold);
    ch.hold = null;
    this.pump();
  }

  finish(ch, err, r) {
    const job = ch.job;
    if (!job) return;
    clearTimeout(job.timer);
    ch.job = null;
    ch.last = Date.now();
    if (r && ch.open && (r.status >= 400 || (r.headers.connection || '').toLowerCase() === 'close')) this.hold(ch, this.ms.grace);
    this.pump();
    if (err) { job.no(err); return; }
    if ((r.headers['content-encoding'] || '').toLowerCase() !== 'gzip') { job.ok(r); return; }
    gunzip(r.body).then(body => {
      const h = { ...r.headers };
      delete h['content-encoding'];
      h['content-length'] = String(body.length);
      job.ok({ status: r.status, headers: h, body });
    }, () => job.no(new LinkError('gzip')));
  }
}
