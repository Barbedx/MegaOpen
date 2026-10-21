// The web app at phone width, every tab, both themes: fails on what a phone
// in a car shows badly. Runs the simulator (tools/build_sim.py) in headless
// Chrome through the DevTools protocol, because Chrome's own --window-size
// cannot go below ~500 px.
//
//   node tools/ui_audit.mjs build/ui-sim.html [outdir]
//   CHROME=/path/to/chrome node tools/ui_audit.mjs ...
//
// Fails (exit 1) on, per page:
//   * the page wider than the screen (outside a scrolling container);
//   * a text under 4.5:1 contrast on its own background (WCAG AA, sunlight);
//   * a control whose hit area is under 40 px (its ::before halo and a
//     wrapping label count), unless ALLOW says why;
//   * any console error or uncaught exception (the mock logs a change sent
//     without X-MegaOpen as one).
// Screenshots of failing pages go to outdir. Node 22+ (WebSocket, fetch).
import { spawn } from 'node:child_process';
import { writeFileSync, mkdirSync, rmSync } from 'node:fs';
import { pathToFileURL } from 'node:url';
import { join, resolve } from 'node:path';
import { tmpdir } from 'node:os';

const [, , htmlArg, outArg] = process.argv;
if (!htmlArg) { console.error('usage: node tools/ui_audit.mjs <ui-sim.html> [outdir]'); process.exit(2); }
const html = resolve(htmlArg);
const out = resolve(outArg || 'build/ui-audit');
mkdirSync(out, { recursive: true });

// Where each build env is looked at. Every tab on the car build; the direct
// build's own screens (matrix, KEY1/KEY2) on Кнопки and Плата.
const PAGES = [
  ...['home', 'screen', 'keys', 'car', 'live', 'board', 'service'].map(t => ['t2can', t]),
  ['direct', 'keys'], ['direct', 'board'],
];
const SIZES = [[390, 844], [360, 740]];
const THEMES = ['light', 'dark'];

// Known small targets, each with its reason. A selector matches the control
// itself (element.matches).
const ALLOW = [
  // The 48x48 picture editor's pixels: a grid, not buttons to hit one by one.
  ['#edGrid *', 'pixel grid of the custom picture editor'],
];

const CHROME = process.env.CHROME || (process.platform === 'win32'
  ? 'C:/Program Files/Google/Chrome/Application/chrome.exe'
  : process.platform === 'darwin' ? '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'
  : 'google-chrome');
const port = 9300 + Math.floor(Math.random() * 600);
const profile = join(tmpdir(), 'mo-ui-audit-' + port);
const chrome = spawn(CHROME, ['--headless=new', '--disable-gpu', '--no-sandbox', `--remote-debugging-port=${port}`,
  '--user-data-dir=' + profile, 'about:blank'], { stdio: 'ignore' });
const sleep = ms => new Promise(r => setTimeout(r, ms));

let ws, nextId = 0;
const pending = new Map(), listeners = [];
async function connect() {
  for (let i = 0; i < 100; i++) {
    try {
      const t = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
      const pg = t.find(x => x.type === 'page');
      if (pg) { ws = new WebSocket(pg.webSocketDebuggerUrl); break; }
    } catch (e) {}
    await sleep(100);
  }
  if (!ws) throw new Error('Chrome did not start: ' + CHROME);
  await new Promise(r => ws.onopen = r);
  ws.onmessage = m => {
    const d = JSON.parse(m.data);
    if (d.id && pending.has(d.id)) { pending.get(d.id)(d); pending.delete(d.id); }
    else if (d.method) listeners.forEach(f => f(d));
  };
}
const cmd = (method, params = {}) => new Promise(r => { const i = ++nextId; pending.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });

// Runs in the page. ALLOW is passed in.
const AUDIT = allow => `(() => {
  const ALLOW = ${JSON.stringify(allow)};
  const W = innerWidth, res = {overflow: [], small: [], contrast: []};
  const vis = e => { const r = e.getBoundingClientRect(); const s = getComputedStyle(e); return r.width > 0 && r.height > 0 && s.visibility !== 'hidden' && s.display !== 'none'; };
  const scroller = e => { for (let p = e.parentElement; p; p = p.parentElement) { const o = getComputedStyle(p).overflowX; if (o === 'auto' || o === 'scroll') return true; } return false; };
  const name = e => e.tagName + (e.id ? '#' + e.id : '') + (typeof e.className === 'string' && e.className.trim() ? '.' + e.className.trim().split(/\\s+/).join('.') : '') +
    (e.dataset && (e.dataset.k || e.dataset.f || e.dataset.kf) ? '[' + (e.dataset.k || e.dataset.f || e.dataset.kf) + ']' : '') +
    ' "' + ((e.innerText || e.value || e.getAttribute('aria-label') || '') + '').trim().slice(0, 20) + '"';
  for (const e of document.querySelectorAll('#main *, header *, nav *')) {
    if (!vis(e)) continue;
    const r = e.getBoundingClientRect();
    if (r.right > W + 1 && !scroller(e)) res.overflow.push(name(e) + ' right=' + Math.round(r.right));
  }
  // The hit area: the box, grown by an absolutely placed ::before with
  // negative insets, or the label a checkbox sits in.
  const hit = e => {
    if ((e.type === 'checkbox' || e.type === 'radio') && e.closest('label')) e = e.closest('label');
    const r = e.getBoundingClientRect();
    let w = r.width, h = r.height;
    const b = getComputedStyle(e, '::before');
    if (b.content && b.content !== 'none' && b.position === 'absolute') {
      const n = v => parseFloat(v) || 0;
      w = Math.max(w, r.width - n(b.left) - n(b.right));
      h = Math.max(h, r.height - n(b.top) - n(b.bottom));
    }
    return [w, h];
  };
  for (const e of document.querySelectorAll('#main button, #main a[href], #main input:not([type=hidden]):not([type=range]), #main select, #main summary, #main textarea, header button, nav a')) {
    if (!vis(e) || e.disabled) continue;
    if (ALLOW.some(([sel]) => e.matches(sel))) continue;
    const [w, h] = hit(e);
    if (w < 40 || h < 40) res.small.push(name(e) + ' ' + Math.round(w) + 'x' + Math.round(h));
  }
  const lum = c => { const m = c.match(/[\\d.]+/g); if (!m) return null; const [r, g, b, a] = m.map(Number); if (a === 0) return null; const f = v => { v /= 255; return v <= 0.03928 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4; }; return 0.2126 * f(r) + 0.7152 * f(g) + 0.0722 * f(b); };
  const bgOf = e => { for (let p = e; p; p = p.parentElement) { const c = getComputedStyle(p).backgroundColor; const m = c.match(/[\\d.]+/g); if (m && (m.length < 4 || +m[3] > 0.5)) return c; } return getComputedStyle(document.body).backgroundColor; };
  const seen = new Set();
  for (const e of document.querySelectorAll('#main *, header *, nav *')) {
    if (!vis(e) || e.closest('[disabled]') || e.closest('canvas')) continue;
    const t = [...e.childNodes].filter(n => n.nodeType === 3).map(n => n.textContent.trim()).join('');
    if (!t) continue;
    const s = getComputedStyle(e), L1 = lum(s.color), bg = bgOf(e), L2 = lum(bg);
    if (L1 == null || L2 == null) continue;
    const cr = (Math.max(L1, L2) + 0.05) / (Math.min(L1, L2) + 0.05);
    const key = s.color + '/' + bg;
    if (cr < 4.5 && !seen.has(key)) { seen.add(key); res.contrast.push(cr.toFixed(2) + ' ' + name(e) + ' ' + s.color + ' on ' + bg); }
  }
  return JSON.stringify(res);
})()`;

const t0 = Date.now();
let failed = 0;
const report = {};
try {
  await connect();
  await cmd('Page.enable'); await cmd('Runtime.enable'); await cmd('Log.enable');
  let errors = [];
  listeners.push(d => {
    if (d.method === 'Runtime.exceptionThrown') errors.push('exception: ' + (d.params.exceptionDetails.exception?.description || d.params.exceptionDetails.text));
    else if (d.method === 'Runtime.consoleAPICalled' && d.params.type === 'error') errors.push('console.error: ' + d.params.args.map(a => a.value ?? a.description).join(' '));
    else if (d.method === 'Log.entryAdded' && d.params.entry.level === 'error') errors.push('log: ' + d.params.entry.text);
  });
  for (const theme of THEMES) {
    await cmd('Page.addScriptToEvaluateOnNewDocument', { source: `try{localStorage.setItem('mo-theme','${theme}');localStorage.setItem('mo-dev','1')}catch(e){}` });
    await cmd('Emulation.setEmulatedMedia', { features: [{ name: 'prefers-color-scheme', value: theme }] });
    for (const [w, h] of SIZES) {
      for (const [env, tab] of PAGES) {
        await cmd('Emulation.setDeviceMetricsOverride', { width: w, height: h, deviceScaleFactor: 1, mobile: true });
        await cmd('Emulation.setTouchEmulationEnabled', { enabled: true, maxTouchPoints: 5 });
        errors = [];
        await cmd('Page.navigate', { url: 'about:blank' });
        await cmd('Page.navigate', { url: pathToFileURL(html).href + '?env=' + env + '#' + tab });
        // Rendered: a card in #main (or an error card), then a moment to settle.
        for (let i = 0; i < 60; i++) {
          const r = await cmd('Runtime.evaluate', { expression: '!!document.querySelector("#main .card")', returnByValue: true });
          if (r.result?.result?.value) break;
          await sleep(100);
        }
        await sleep(700);
        const a = await cmd('Runtime.evaluate', { expression: AUDIT(ALLOW), returnByValue: true });
        const r = JSON.parse(a.result.result.value || '{}');
        r.errors = errors.slice(0, 10);
        const key = `${theme}-${w}-${env}-${tab}`;
        const bad = r.overflow.length + r.small.length + r.contrast.length + r.errors.length;
        report[key] = r;
        if (bad) {
          failed++;
          console.log(`FAIL ${key}`);
          for (const k of ['overflow', 'small', 'contrast', 'errors']) for (const x of r[k].slice(0, 8)) console.log(`  ${k}: ${x}`);
          const lm = await cmd('Page.getLayoutMetrics');
          await cmd('Emulation.setDeviceMetricsOverride', { width: w, height: Math.min(Math.ceil(lm.result.cssContentSize.height), 6000), deviceScaleFactor: 1, mobile: true });
          const shot = await cmd('Page.captureScreenshot', { format: 'png' });
          writeFileSync(join(out, key + '.png'), Buffer.from(shot.result.data, 'base64'));
        } else {
          console.log(`ok   ${key}`);
        }
      }
    }
  }
} finally {
  writeFileSync(join(out, 'report.json'), JSON.stringify(report, null, 1));
  try { ws?.close(); } catch (e) {}
  chrome.kill();
  try { rmSync(profile, { recursive: true, force: true }); } catch (e) {}
}
const n = Object.keys(report).length;
console.log(`ui_audit: ${n} pages, ${failed} failed, ${((Date.now() - t0) / 1000).toFixed(0)} s`);
process.exit(failed || n === 0 ? 1 : 0);
