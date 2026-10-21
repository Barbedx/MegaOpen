// The simulator's mock board (web/sim/mock.js) over HTTP, for tools that speak
// curl: tools/rc_smoke.sh against the mock catches a reply the page and the
// smoke test no longer agree on (a code renamed, a field dropped) without a
// board.
//
//   node tools/sim_http.mjs build/ui-sim.html [port] [env]     (8077, t2can)
//   tools/rc_smoke.sh http://127.0.0.1:8077
//
// The page is opened once in headless Chrome; every request becomes
// SIM.call(method, path, params, headers) in it, the same call the page makes,
// and its answer (or its error status and body) goes back as JSON or
// text/plain. GET /i18n/en.json is the file the board serves. What only the
// firmware does is not here: the Host check, CORS, /update, the log ring's
// size, the gzip encoding. Ctrl+C stops it. Node 22+ (WebSocket, fetch).
import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { readFileSync, rmSync } from 'node:fs';
import { pathToFileURL } from 'node:url';
import { join, resolve, dirname } from 'node:path';
import { tmpdir } from 'node:os';

const [, , htmlArg, portArg, envArg] = process.argv;
if (!htmlArg) { console.error('usage: node tools/sim_http.mjs <ui-sim.html> [port] [env]'); process.exit(2); }
const html = resolve(htmlArg), port = +(portArg || 8077), env = envArg || 't2can';
const ROOT = resolve(dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1')), '..');

const CHROME = process.env.CHROME || (process.platform === 'win32'
  ? 'C:/Program Files/Google/Chrome/Application/chrome.exe'
  : process.platform === 'darwin' ? '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'
  : 'google-chrome');
const profile = join(tmpdir(), 'mo-sim-http-' + process.pid + '-' + Date.now());
const chrome = spawn(CHROME, ['--headless=new', '--disable-gpu', '--no-sandbox', '--disable-dev-shm-usage', '--remote-debugging-port=0',
  '--user-data-dir=' + profile, 'about:blank'], { stdio: ['ignore', 'ignore', 'ignore'] });
let chromeExit = null;
chrome.on('exit', c => { chromeExit = c; });
const sleep = ms => new Promise(r => setTimeout(r, ms));
const stop = () => { try { chrome.kill(); } catch (e) {} try { rmSync(profile, { recursive: true, force: true }); } catch (e) {} process.exit(0); };
process.on('SIGINT', stop); process.on('SIGTERM', stop);

let ws, nextId = 0;
const pending = new Map();
for (let i = 0; i < 300 && chromeExit === null && !ws; i++) {
  try {
    const p = readFileSync(join(profile, 'DevToolsActivePort'), 'utf8').split('\n')[0].trim();
    const pg = (await (await fetch(`http://127.0.0.1:${p}/json/list`)).json()).find(x => x.type === 'page');
    if (pg) ws = new WebSocket(pg.webSocketDebuggerUrl);
  } catch (e) {}
  if (!ws) await sleep(100);
}
if (!ws) { console.error('sim_http: Chrome did not start: ' + CHROME); stop(); }
await new Promise(r => ws.onopen = r);
ws.onmessage = m => { const d = JSON.parse(m.data); if (d.id && pending.has(d.id)) { pending.get(d.id)(d); pending.delete(d.id); } };
const cmd = (method, params = {}) => new Promise(r => { const i = ++nextId; pending.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });

await cmd('Runtime.enable');
await cmd('Page.navigate', { url: pathToFileURL(html).href + '?env=' + env + '#home' });
for (let i = 0; i < 100; i++) {
  const r = await cmd('Runtime.evaluate', { expression: '!!(window.SIM && window.CAT)', returnByValue: true });
  if (r.result && r.result.result && r.result.result.value) break;
  await sleep(100);
}

// The request's parameters as the firmware reads them: the query string, then
// a JSON body or a form body on top.
function paramsOf(req, body) {
  const ct = req.headers['content-type'] || '';
  if (!body.length) return null;
  if (ct.includes('json')) { try { return JSON.parse(body); } catch (e) { return { __bad: body }; } }
  return Object.fromEntries(new URLSearchParams(body));
}

createServer((req, res) => {
  const chunks = [];
  req.on('data', c => chunks.push(c));
  req.on('end', async () => {
    const body = Buffer.concat(chunks).toString('utf8');
    const send = (st, b) => {
      const json = typeof b !== 'string';
      res.writeHead(st, { 'Content-Type': json ? 'application/json' : 'text/plain' });
      res.end(json ? JSON.stringify(b === undefined ? { ok: true } : b) : b);
    };
    if (req.method === 'GET' && req.url === '/i18n/en.json')
      return send(200, JSON.parse(readFileSync(join(ROOT, 'web', 'i18n', 'en.json'), 'utf8')));
    const params = paramsOf(req, body);
    if (params && params.__bad !== undefined) return send(400, 'bad_json');
    const hdr = req.headers['x-megaopen'] ? { 'X-MegaOpen': req.headers['x-megaopen'] } : {};
    const expr = `SIM.call(${JSON.stringify(req.method)}, ${JSON.stringify(req.url)}, ${JSON.stringify(params)}, ${JSON.stringify(hdr)})` +
      `.then(b => ({st: 200, b}), e => ({st: e.status || 500, b: e.body !== undefined ? e.body : e.message}))`;
    const r = await cmd('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true });
    const v = r.result && r.result.result && r.result.result.value;
    if (!v) return send(500, 'sim_http: ' + JSON.stringify(r.result && (r.result.exceptionDetails || r.result)).slice(0, 300));
    send(v.st, v.b);
  });
}).listen(port, '127.0.0.1', () => console.log(`sim_http: ${html} (${env}) on http://127.0.0.1:${port}`));
