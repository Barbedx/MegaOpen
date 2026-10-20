// MegaOpen cloud backend: online log storage (D1) + OTA firmware deployment (KV).
// Plain JS module worker, no build step. See ../README.md for the HTTP contract.

const MAX_DEVICE_BODY = 64 * 1024;          // ingest / coredump
const MAX_FW_BODY = 24 * 1024 * 1024;       // KV value limit is 25 MiB
const CORE_TTL_S = 30 * 24 * 3600;          // coredumps live 30 days
const LOG_RETENTION_MS = 30 * 24 * 3600 * 1000;
const MANIFEST_KEY = "manifest";
const VERSION_RE = /^[A-Za-z0-9._+-]{1,64}$/;
const DEV_RE = /^[A-Za-z0-9._:-]{1,64}$/;

// ---------- helpers ----------

const json = (obj, status = 200, headers = {}) =>
  new Response(JSON.stringify(obj), {
    status,
    headers: { "content-type": "application/json; charset=utf-8", ...headers },
  });
const err = (status, msg) => json({ ok: false, error: msg }, status);
const noContent = () => new Response(null, { status: 204 });

const enc = new TextEncoder();

// Constant-time token compare: hash both sides (fixed length), then XOR-accumulate.
async function safeEqual(a, b) {
  const [ha, hb] = await Promise.all([
    crypto.subtle.digest("SHA-256", enc.encode(a)),
    crypto.subtle.digest("SHA-256", enc.encode(b)),
  ]);
  const x = new Uint8Array(ha), y = new Uint8Array(hb);
  let diff = 0;
  for (let i = 0; i < x.length; i++) diff |= x[i] ^ y[i];
  return diff === 0;
}

async function authorized(request, secret) {
  if (!secret) return false; // secret not configured -> deny everything
  const h = request.headers.get("authorization") || "";
  const m = /^Bearer\s+(.+)$/i.exec(h.trim());
  if (!m) return false;
  return safeEqual(m[1].trim(), secret);
}

// Reads body with a hard size limit. Returns ArrayBuffer or null if too large.
async function readBody(request, limit) {
  const cl = request.headers.get("content-length");
  if (cl !== null && Number(cl) > limit) return null;
  const buf = await request.arrayBuffer();
  if (buf.byteLength > limit) return null;
  return buf;
}

const toInt = (v) => {
  const n = typeof v === "string" ? Number(v) : v;
  return typeof n === "number" && Number.isFinite(n) ? Math.trunc(n) : null;
};
const toStr = (v) => (v === undefined || v === null ? null : String(v));

const hex = (buf) =>
  [...new Uint8Array(buf)].map((b) => b.toString(16).padStart(2, "0")).join("");

async function getManifest(env) {
  return env.KV.get(MANIFEST_KEY, "json");
}

// Metadata of a stored firmware without downloading its value.
async function fwMeta(env, version) {
  const key = "fw:" + version;
  const res = await env.KV.list({ prefix: key });
  const k = res.keys.find((k) => k.name === key);
  return k ? k.metadata || {} : null;
}

async function listAll(env, prefix) {
  const out = [];
  let cursor;
  do {
    const r = await env.KV.list({ prefix, cursor });
    out.push(...r.keys);
    cursor = r.list_complete ? undefined : r.cursor;
  } while (cursor);
  return out;
}

// ---------- device endpoints ----------

async function ingest(request, env) {
  const buf = await readBody(request, MAX_DEVICE_BODY);
  if (!buf) return err(413, "body too large (max 64 KB)");
  let b;
  try {
    b = JSON.parse(new TextDecoder().decode(buf));
  } catch {
    return err(400, "invalid json");
  }
  if (!b || typeof b !== "object" || Array.isArray(b)) return err(400, "expected object");
  const dev = toStr(b.dev);
  if (!dev || !DEV_RE.test(dev)) return err(400, "bad dev");

  const now = Date.now();
  const fw = toStr(b.fw);
  const boot = toInt(b.boot), up = toInt(b.up), heap = toInt(b.heap), rssi = toInt(b.rssi);
  const reset = toStr(b.reset);
  const state = b.state === undefined || b.state === null ? null : JSON.stringify(b.state);
  const lines = Array.isArray(b.lines) ? b.lines.map((l) => String(l)).join("\n") : "";

  await env.DB.batch([
    env.DB.prepare(
      "INSERT INTO batches (ts, dev, fw, boot, up, reset, heap, rssi, state, lines) VALUES (?,?,?,?,?,?,?,?,?,?)"
    ).bind(now, dev, fw, boot, up, reset, heap, rssi, state, lines),
    env.DB.prepare(
      `INSERT INTO devices (dev, last_ts, fw, boot, up, state) VALUES (?,?,?,?,?,?)
       ON CONFLICT(dev) DO UPDATE SET last_ts=excluded.last_ts,
         fw=COALESCE(excluded.fw, devices.fw), boot=COALESCE(excluded.boot, devices.boot),
         up=COALESCE(excluded.up, devices.up), state=COALESCE(excluded.state, devices.state)`
    ).bind(dev, now, fw, boot, up, state),
  ]);
  return json({ ok: true, t: now });
}

async function coredump(request, env, url) {
  const dev = url.searchParams.get("dev") || "";
  const fw = url.searchParams.get("fw");
  if (!DEV_RE.test(dev)) return err(400, "bad dev");
  const buf = await readBody(request, MAX_DEVICE_BODY);
  if (!buf) return err(413, "body too large (max 64 KB)");
  if (buf.byteLength === 0) return err(400, "empty body");
  const now = Date.now();
  const key = `core:${dev}:${now}`;
  await env.KV.put(key, buf, {
    expirationTtl: CORE_TTL_S,
    metadata: { dev, fw, size: buf.byteLength, ts: now },
  });
  await env.DB.prepare("INSERT INTO batches (ts, dev, fw, lines) VALUES (?,?,?,?)")
    .bind(now, dev, fw, `COREDUMP stored ${key}`)
    .run();
  return json({ ok: true, t: now, key });
}

async function manifestCheck(env, url) {
  const dev = url.searchParams.get("dev") || "";
  const fw = url.searchParams.get("fw") || "";
  const now = Date.now();
  if (DEV_RE.test(dev)) {
    await env.DB.prepare(
      `INSERT INTO devices (dev, last_check) VALUES (?, ?)
       ON CONFLICT(dev) DO UPDATE SET last_check=excluded.last_check`
    ).bind(dev, now).run();
  }
  const m = await getManifest(env);
  if (!m || !m.version) return noContent();
  if (m.version === fw) return noContent();
  if (Array.isArray(m.devices) && !m.devices.includes(dev)) return noContent();
  return json({ version: m.version, size: m.size, md5: m.md5, path: `/v1/fw/${m.version}` });
}

async function fwDownload(request, env, version) {
  if (!VERSION_RE.test(version)) return err(404, "not found");
  const { value, metadata } = await env.KV.getWithMetadata("fw:" + version, "arrayBuffer");
  if (!value) return err(404, "not found");
  const size = value.byteLength;
  const md5 = (metadata && metadata.md5) || "";
  const headers = {
    "content-type": "application/octet-stream",
    "x-md5": md5,
    "accept-ranges": "bytes",
    "cache-control": "no-store",
  };
  // Optional single Range support (resume on flaky links). Plain GET is the contract.
  const range = request.headers.get("range");
  const rm = range && /^bytes=(\d*)-(\d*)$/.exec(range.trim());
  if (rm && (rm[1] !== "" || rm[2] !== "")) {
    let start, end;
    if (rm[1] === "") { start = Math.max(0, size - Number(rm[2])); end = size - 1; }
    else { start = Number(rm[1]); end = rm[2] === "" ? size - 1 : Math.min(Number(rm[2]), size - 1); }
    if (start > end || start >= size) {
      return new Response(null, { status: 416, headers: { "content-range": `bytes */${size}` } });
    }
    const part = value.slice(start, end + 1);
    return new Response(request.method === "HEAD" ? null : part, {
      status: 206,
      headers: { ...headers, "content-length": String(part.byteLength), "content-range": `bytes ${start}-${end}/${size}` },
    });
  }
  return new Response(request.method === "HEAD" ? null : value, {
    status: 200,
    headers: { ...headers, "content-length": String(size) },
  });
}

// ---------- admin endpoints ----------

async function adminPutFw(request, env, url, version) {
  if (!VERSION_RE.test(version)) return err(400, "bad version (allowed: A-Z a-z 0-9 . _ + -, max 64)");
  const buf = await readBody(request, MAX_FW_BODY);
  if (!buf) return err(413, "firmware too large (max 24 MB)");
  if (buf.byteLength === 0) return err(400, "empty body");
  const md5 = hex(await crypto.subtle.digest("MD5", buf));
  const created = Date.now();
  const size = buf.byteLength;
  await env.KV.put("fw:" + version, buf, { metadata: { version, size, md5, created } });

  const activate = url.searchParams.get("activate") !== "0";
  let manifest = null;
  if (activate) {
    manifest = { version, size, md5, created };
    const devs = parseDevices(url.searchParams.get("devices"));
    if (devs) manifest.devices = devs;
    await env.KV.put(MANIFEST_KEY, JSON.stringify(manifest));
  }
  return json({ ok: true, stored: { version, size, md5, created }, activated: activate, manifest });
}

function parseDevices(s) {
  if (s === null || s === undefined) return null;
  const list = (Array.isArray(s) ? s : String(s).split(","))
    .map((d) => String(d).trim())
    .filter(Boolean);
  return list.length ? list : null;
}

async function adminSetManifest(request, env) {
  let b;
  try {
    b = await request.json();
  } catch {
    return err(400, "invalid json");
  }
  const version = b && toStr(b.version);
  if (!version || !VERSION_RE.test(version)) return err(400, "bad version");
  const meta = await fwMeta(env, version);
  if (!meta) return err(404, `firmware ${version} not stored`);
  const manifest = { version, size: meta.size, md5: meta.md5, created: meta.created, activated: Date.now() };
  const devs = parseDevices(b.devices);
  if (devs) manifest.devices = devs;
  await env.KV.put(MANIFEST_KEY, JSON.stringify(manifest));
  return json({ ok: true, manifest });
}

async function adminListFw(env) {
  const keys = await listAll(env, "fw:");
  const m = await getManifest(env);
  return json({
    active: m ? m.version : null,
    versions: keys.map((k) => ({ version: k.name.slice(3), ...(k.metadata || {}) }))
      .sort((a, b) => (a.created || 0) - (b.created || 0)),
  });
}

async function adminDevices(env) {
  const { results } = await env.DB.prepare("SELECT * FROM devices ORDER BY last_ts DESC").all();
  for (const r of results) {
    try { r.state = r.state ? JSON.parse(r.state) : null; } catch { /* keep raw */ }
  }
  return json(results);
}

async function adminLogs(env, url) {
  const dev = url.searchParams.get("dev");
  const since = toInt(url.searchParams.get("since"));
  let limit = toInt(url.searchParams.get("limit")) ?? 200;
  limit = Math.min(Math.max(limit, 1), 5000);
  const format = url.searchParams.get("format") || "json";

  const where = [], args = [];
  if (dev) { where.push("dev = ?"); args.push(dev); }
  if (since !== null) { where.push("ts >= ?"); args.push(since); }
  const sql =
    "SELECT * FROM batches" + (where.length ? " WHERE " + where.join(" AND ") : "") +
    " ORDER BY id DESC LIMIT ?";
  args.push(limit);
  const { results } = await env.DB.prepare(sql).bind(...args).all();
  results.reverse(); // newest-last

  if (format === "text") {
    const out = [];
    for (const r of results) {
      const prefix = `${new Date(r.ts).toISOString()} ${dev ? "" : r.dev + " "}${r.fw ?? "-"} up=${r.up ?? "-"}`;
      for (const line of (r.lines || "").split("\n")) {
        if (line !== "") out.push(`${prefix} ${line}`);
      }
    }
    return new Response(out.join("\n") + (out.length ? "\n" : ""), {
      headers: { "content-type": "text/plain; charset=utf-8" },
    });
  }
  for (const r of results) {
    try { r.state = r.state ? JSON.parse(r.state) : null; } catch { /* keep raw */ }
    r.lines = r.lines ? r.lines.split("\n") : [];
  }
  return json(results);
}

async function adminCoredumps(env, url) {
  const dev = url.searchParams.get("dev");
  const keys = await listAll(env, dev ? `core:${dev}:` : "core:");
  return json(keys.map((k) => ({ key: k.name, expiration: k.expiration, ...(k.metadata || {}) })));
}

async function adminCoredump(env, key) {
  if (!key.startsWith("core:")) return err(404, "not found");
  const v = await env.KV.get(key, "arrayBuffer");
  if (!v) return err(404, "not found");
  return new Response(v, {
    headers: {
      "content-type": "application/octet-stream",
      "content-length": String(v.byteLength),
      "content-disposition": `attachment; filename="${key.replace(/[^A-Za-z0-9._-]/g, "_")}.bin"`,
    },
  });
}

// ---------- router ----------

async function route(request, env) {
  const url = new URL(request.url);
  const p = url.pathname;
  const m = request.method;
  const dev = () => authorized(request, env.DEVICE_TOKEN);
  const adm = () => authorized(request, env.ADMIN_TOKEN);
  const unauth = () => err(401, "unauthorized");
  const badMethod = () => err(405, "method not allowed");

  // Device API
  if (p === "/v1/ingest") {
    if (!(await dev())) return unauth();
    return m === "POST" ? ingest(request, env) : badMethod();
  }
  if (p === "/v1/coredump") {
    if (!(await dev())) return unauth();
    return m === "POST" ? coredump(request, env, url) : badMethod();
  }
  if (p === "/v1/manifest") {
    if (!(await dev())) return unauth();
    return m === "GET" ? manifestCheck(env, url) : badMethod();
  }
  if (p.startsWith("/v1/fw/")) {
    if (!(await dev())) return unauth();
    if (m !== "GET" && m !== "HEAD") return badMethod();
    return fwDownload(request, env, decodeURIComponent(p.slice("/v1/fw/".length)));
  }

  // Admin API
  if (p.startsWith("/admin/fw/")) {
    if (!(await adm())) return unauth();
    return m === "PUT"
      ? adminPutFw(request, env, url, decodeURIComponent(p.slice("/admin/fw/".length)))
      : badMethod();
  }
  if (p === "/admin/fw") {
    if (!(await adm())) return unauth();
    return m === "GET" ? adminListFw(env) : badMethod();
  }
  if (p === "/admin/manifest") {
    if (!(await adm())) return unauth();
    if (m === "GET") {
      const man = await getManifest(env);
      return man ? json(man) : noContent();
    }
    if (m === "DELETE") {
      await env.KV.delete(MANIFEST_KEY);
      return json({ ok: true, paused: true });
    }
    if (m === "POST") return adminSetManifest(request, env);
    return badMethod();
  }
  if (p === "/admin/devices") {
    if (!(await adm())) return unauth();
    return m === "GET" ? adminDevices(env) : badMethod();
  }
  if (p === "/admin/logs") {
    if (!(await adm())) return unauth();
    return m === "GET" ? adminLogs(env, url) : badMethod();
  }
  if (p === "/admin/coredumps") {
    if (!(await adm())) return unauth();
    return m === "GET" ? adminCoredumps(env, url) : badMethod();
  }
  if (p.startsWith("/admin/coredump/")) {
    if (!(await adm())) return unauth();
    return m === "GET"
      ? adminCoredump(env, decodeURIComponent(p.slice("/admin/coredump/".length)))
      : badMethod();
  }
  return err(404, "not found");
}

export default {
  async fetch(request, env) {
    try {
      return await route(request, env);
    } catch (e) {
      console.error("unhandled", e && e.stack ? e.stack : e);
      return err(500, "internal error");
    }
  },

  async scheduled(event, env, ctx) {
    const cutoff = Date.now() - LOG_RETENTION_MS;
    ctx.waitUntil(
      env.DB.prepare("DELETE FROM batches WHERE ts < ?").bind(cutoff).run()
        .then((r) => console.log("retention: deleted", r.meta && r.meta.changes, "batches"))
    );
  },
};
