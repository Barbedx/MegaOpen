// The shell's own files, kept for a visit without internet (the garage has
// none): the network first, with a short wait, then what the last visit saved.
// Only this folder's files; the board's page never passes through here, it
// comes over USB.
const CACHE = 'mo-usb-1';
const FILES = ['./', 'index.html', 'shell.js', 'link.js'];
const WAIT_MS = 3000;

self.addEventListener('install', e => {
  e.waitUntil(caches.open(CACHE).then(c => c.addAll(FILES)).then(() => self.skipWaiting()));
});

self.addEventListener('activate', e => {
  e.waitUntil(caches.keys()
    .then(ks => Promise.all(ks.filter(k => k.startsWith('mo-usb-') && k !== CACHE).map(k => caches.delete(k))))
    .then(() => self.clients.claim()));
});

self.addEventListener('fetch', e => {
  const r = e.request;
  if (r.method !== 'GET' || !r.url.startsWith(self.registration.scope)) return;
  e.respondWith((async () => {
    const c = await caches.open(CACHE);
    const net = fetch(r).then(res => {
      if (res.ok) c.put(r, res.clone()).catch(() => {});
      return res;
    });
    const late = new Promise(ok => setTimeout(ok, WAIT_MS));
    let res = null;
    try { res = await Promise.race([net, late]); } catch (x) { /* offline */ }
    if (res && res.ok) return res;
    // Offline, slow, or an error page from the host: the saved copy if any.
    const hit = await c.match(r, { ignoreSearch: true });
    if (hit) { net.catch(() => {}); return hit; }
    return res || net;
  })());
});
