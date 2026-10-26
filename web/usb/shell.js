// The shell around the board's own page (docs/design/usb-link.md, "The WebUSB
// page"): it holds the device and the link, fetches "/" over it, and draws that
// page in a frame with window.MOLink, which web/js/10-api.js uses instead of
// fetch, as in the app's WebView (android/.../ui/BoardScreen.kt).
import { Board, LinkError, OTA_MS, REQ_MS, findVendor } from './link.js';

const WORDS = {
  uk: {
    h1: 'MegaOpen через USB',
    lead: 'Налаштування плати без WiFi: під\'єднай плату USB-кабелем до цього комп\'ютера чи телефона й натисни «Підключити».',
    go: 'Підключити плату',
    n1: 'Потрібні Chrome або Edge (комп\'ютер чи Android) і прошивка 1.6 або новіша.',
    n2: 'Плату одночасно тримає одна програма: якщо відкрито застосунок MegaOpen Компаньйон чи tools/usb_link.py, закрий їх.',
    n3: 'Після першого відкриття сторінка працює й без інтернету.',
    noUsb: 'Цей браузер не вміє WebUSB. Відкрий сторінку в Chrome або Edge на комп\'ютері чи Android.',
    idle: 'не підключено',
    opening: 'підключаюсь…',
    loading: 'завантажую сторінку плати…',
    lost: 'Зв\'язок з платою втрачено. Чекаю на неї…',
    off: 'Відключити',
    e_busy: 'Плату тримає інша програма чи вкладка (MegaOpen Компаньйон, tools/usb_link.py). Закрий її й спробуй ще.',
    e_access: 'Немає доступу до плати. У Windows так буває, коли її відкрила інша програма або коли до неї не прив\'язано WinUSB (прошивка t2can_usbhid: там WebUSB вимкнено).',
    e_no_itf: 'Ця прошивка не має USB-лінку: потрібна 1.6 або новіша (env t2can).',
    e_no_hello: 'Плата не відповіла на привітання. Перепідключи кабель і спробуй ще.',
    e_page: 'Плата не віддала свою сторінку: {0}',
    e_other: 'Не вдалося: {0}',
    dl: 'Завантажую {0}…',
    saved: 'Збережено: {0}',
    dlFail: 'Не вдалося зберегти: {0}',
  },
  en: {
    h1: 'MegaOpen over USB',
    lead: 'The board\'s settings without WiFi: plug the board into this computer or phone with a USB cable and press Connect.',
    go: 'Connect the board',
    n1: 'Needs Chrome or Edge (computer or Android) and firmware 1.6 or newer.',
    n2: 'One program holds the board at a time: close the MegaOpen Companion app or tools/usb_link.py if they are open.',
    n3: 'After the first visit the page works without internet too.',
    noUsb: 'This browser has no WebUSB. Open the page in Chrome or Edge on a computer or Android.',
    idle: 'not connected',
    opening: 'connecting…',
    loading: 'loading the board\'s page…',
    lost: 'Lost the board. Waiting for it…',
    off: 'Disconnect',
    e_busy: 'Another program or tab holds the board (MegaOpen Companion, tools/usb_link.py). Close it and try again.',
    e_access: 'No access to the board. On Windows that happens when another program opened it, or when WinUSB is not bound to it (the t2can_usbhid firmware, which has WebUSB off).',
    e_no_itf: 'This firmware has no USB link: it needs 1.6 or newer (env t2can).',
    e_no_hello: 'The board did not answer the hello. Replug the cable and try again.',
    e_page: 'The board did not give its page: {0}',
    e_other: 'Failed: {0}',
    dl: 'Downloading {0}…',
    saved: 'Saved: {0}',
    dlFail: 'Could not save: {0}',
  },
};

const VID = 0x303A;
const $ = id => document.getElementById(id);
// The board's page keeps its language under the same key, on this same origin.
let lang = 'uk';
const readLang = () => { try { lang = localStorage.getItem('mo-lang') === 'en' ? 'en' : 'uk'; } catch (e) {} };
readLang();
const t =(k, ...a) => (WORDS[lang][k] ?? WORDS.uk[k] ?? k).replace(/\{(\d)\}/g, (m, i) => a[i] ?? '');

const b64enc = u8 => { let s = ''; for (let i = 0; i < u8.length; i += 0x8000) s += String.fromCharCode.apply(null, u8.subarray(i, i + 0x8000)); return btoa(s); };
const b64dec = b => { const s = atob(b), u = new Uint8Array(s.length); for (let i = 0; i < s.length; i++) u[i] = s.charCodeAt(i); return u; };

let board = null;        // the open link
let dev = null;          // its device
let want = false;        // a link was asked for: come back to it after a replug or a reboot
let opening = false;
let hello = null;

function start() {
  // redraw: the page reloaded itself, mostly for its language switch.
  window.__moShell = { adopt, redraw: () => { readLang(); words(); setTimeout(draw, 0); } };
  words();
  $('lang').onclick = () => {
    lang = lang === 'uk' ? 'en' : 'uk';
    try { localStorage.setItem('mo-lang', lang); } catch (e) {}
    words();
    if (board) draw();
  };
  $('off').onclick = () => {
    want = false;
    if (board) board.close('user');
    page(null);
  };
  if (!navigator.usb) {
    $('go').disabled = true;
    $('err').textContent = t('noUsb');
    status('bad', 'idle');
    return;
  }
  if (navigator.serviceWorker) navigator.serviceWorker.register('sw.js').catch(() => {});
  $('go').onclick = async () => {
    if (board) { draw(); return; }     // linked, but its page did not come: try that again
    let d;
    try {
      d = await navigator.usb.requestDevice({ filters: [{ vendorId: VID, classCode: 0xFF }] });
    } catch (e) {
      if (e.name !== 'NotFoundError') showErr(e);    // NotFoundError: the chooser was closed
      return;
    }
    want = true;
    connect(d);
  };
  navigator.usb.addEventListener('disconnect', e => { if (board && e.device === dev) board.close('unplugged'); });
  navigator.usb.addEventListener('connect', e => { if (want && !board && ours(e.device)) connect(e.device); });
  // Asked for once, the device stays allowed: no click the next time.
  navigator.usb.getDevices().then(ds => {
    const d = ds.find(ours);
    if (d) { want = true; connect(d); }
  }).catch(() => {});
  setInterval(() => { if (want && !board && !opening) again(); }, 3000);
  status('', 'idle');
}

function ours(d) {
  return d.vendorId === VID && (d.configurations || []).some(c => findVendor(c));
}

function again() {
  navigator.usb.getDevices().then(ds => { const d = ds.find(ours); if (d && want && !board) connect(d); }).catch(() => {});
}

function words() {
  document.documentElement.lang = lang;
  for (const k of ['h1', 'lead', 'go', 'n1', 'n2', 'n3']) $(k).textContent = t(k);
  $('lang').textContent = lang === 'uk' ? 'EN' : 'UK';
  $('off').textContent = t('off');
  if (!navigator.usb) $('err').textContent = t('noUsb');
  status(...bar);
}

function label() {
  return hello ? 'USB · ' + [hello.fw, hello.name].filter(Boolean).join(' · ') : '';
}

// The bar: the dot's kind and a word's key, or '' for the board's own label.
let bar = ['', 'idle'];
function status(kind, key) {
  bar = [kind, key];
  $('dot').className = 'dot' + (kind ? ' ' + kind : '');
  $('st').textContent = key ? t(key) : label();
}

function errText(e) {
  if (e instanceof LinkError && WORDS.uk['e_' + e.code]) return t('e_' + e.code);
  if (e && e.name === 'SecurityError') return t('e_access');
  if (e && e.name === 'NetworkError' && /claim|busy/i.test(e.message)) return t('e_busy');
  return t('e_other', (e && e.message) || String(e));
}

function showErr(e) { $('err').textContent = e ? errText(e) : ''; }

let toastT = 0;
function toast(s) {
  const el = $('toast');
  el.textContent = s;
  el.classList.add('on');
  clearTimeout(toastT);
  toastT = setTimeout(() => el.classList.remove('on'), 3500);
}

async function connect(d) {
  if (board || opening) return;
  opening = true;
  showErr(null);
  status('wait', 'opening');
  $('go').disabled = true;
  const b = new Board(d, 'web-usb');
  b.onclose = why => lost(b, why);
  try {
    hello = await b.open();
    board = b;
    dev = d;
    $('off').hidden = false;
    status('on', '');
  } catch (e) {
    status('bad', 'idle');
    if (!$('box').hidden) $('lost').hidden = false;
    else showErr(e);
    return;
  } finally {
    opening = false;
    $('go').disabled = false;
  }
  draw();
}

function lost(b, why) {
  if (b !== board) return;
  board = null;
  $('off').hidden = true;
  status(why === 'user' ? '' : 'bad', 'idle');
  if (why !== 'user' && !$('box').hidden) {
    $('lost').textContent = t('lost');
    $('lost').hidden = false;
  }
}

// The board's page in a fresh frame, or the intro back when html is null.
// Fresh every time: the old frame's timers (the page polls) die with it.
function page(html) {
  const box = $('box');
  for (const f of box.querySelectorAll('iframe')) f.remove();
  $('lost').hidden = true;
  box.hidden = html == null;
  $('intro').hidden = html != null;
  if (html == null) return;
  const f = document.createElement('iframe');
  f.title = 'MegaOpen';
  box.prepend(f);
  // MOLink must be on the frame's window before the page's own script reads
  // it: the first thing in <head>. document.open() gives the frame this
  // shell's URL, so "#board" links stay in the same document.
  const inject = '<script>parent.__moShell.adopt(window)</' + 'script>';
  const m = /<head[^>]*>/i.exec(html);
  const out = m ? html.slice(0, m.index + m[0].length) + inject + html.slice(m.index + m[0].length) : inject + html;
  const doc = f.contentDocument;
  doc.open();
  doc.write(out);
  doc.close();
}

async function draw() {
  const b = board;
  if (!b) return;
  status('wait', 'loading');
  let r;
  try {
    r = await b.request('GET', '/', {}, null, 30000);
    if (r.status !== 200) throw new Error('HTTP ' + r.status);
  } catch (e) {
    if (b !== board) return;
    status('on', '');
    page(null);
    $('err').textContent = t('e_page', errText(e));
    return;
  }
  if (b !== board) return;
  status('on', '');
  showErr(null);
  page(new TextDecoder().decode(r.body));
}

function save(blob, name) {
  const u = URL.createObjectURL(blob), a = document.createElement('a');
  a.href = u;
  a.download = name;
  document.body.append(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(u), 10000);
}

function done(w, id, status, ct, b64) {
  try { if (!w.closed && w.__moDone) w.__moDone(id, status, ct, b64); } catch (e) {}
}

// Called by the frame's first script: what the app's Bridge is there.
function adopt(w) {
  w.MOLink = {
    request(id, method, path, headersJson, bodyB64) {
      let h = {};
      try { h = JSON.parse(headersJson || '{}') || {}; } catch (e) {}
      if (!board) { done(w, id, -1, '', 'link'); return; }
      board.request(method, path, h, bodyB64 ? b64dec(bodyB64) : null, path.startsWith('/ota/') ? OTA_MS : REQ_MS)
        .then(r => done(w, id, r.status, r.headers['content-type'] || 'application/octet-stream', b64enc(r.body)),
              e => done(w, id, -1, '', String(e.message || e)));
    },
    download(path, name) {
      if (!board) { toast(t('dlFail', 'link')); return; }
      toast(t('dl', name));
      board.request('GET', path, {}, null, OTA_MS).then(r => {
        if (r.status >= 300) throw new Error('HTTP ' + r.status);
        save(new Blob([r.body], { type: r.headers['content-type'] || 'application/octet-stream' }), name);
        toast(t('saved', name));
      }).catch(e => toast(t('dlFail', e.message || e)));
    },
    saveText(name, text) {
      save(new Blob([text], { type: 'text/plain;charset=utf-8' }), name);
      toast(t('saved', name));
    },
  };
  // The page fetches a few files itself (/i18n/en.json): a path on the board
  // goes over the link, anything else to the network as before.
  const net = w.fetch;
  w.fetch = function (input, init) {
    if (typeof input !== 'string' || input[0] !== '/' || input[1] === '/') return net.call(w, input, init);
    const o = init || {}, h = {};
    if (o.headers) new w.Headers(o.headers).forEach((v, k) => { h[k] = v; });
    const body = o.body == null ? null : typeof o.body === 'string' ? new TextEncoder().encode(o.body)
      : ArrayBuffer.isView(o.body) ? new Uint8Array(o.body.buffer, o.body.byteOffset, o.body.byteLength)
      : o.body instanceof w.ArrayBuffer || o.body instanceof ArrayBuffer ? new Uint8Array(o.body) : null;
    if (!board) return Promise.reject(new w.TypeError('Failed to fetch (USB link down)'));
    return board.request((o.method || 'GET').toUpperCase(), input, h, body).then(r => {
      const s = r.status >= 200 && r.status <= 599 ? r.status : 502;
      return new w.Response([101, 204, 205, 304].includes(s) ? null : r.body, { status: s, headers: { 'content-type': r.headers['content-type'] || '' } });
    }, e => { throw new w.TypeError('Failed to fetch (USB: ' + (e.message || e) + ')'); });
  };
}

// Last, once every binding above exists.
if (!window.__moFramed) start();
