# MegaOpen cloud

Cloudflare Worker для ESP32 MegaOpen: онлайн-логи (D1) + OTA-прошивки (KV). Лише free tier, без R2.
Cloudflare Worker for the MegaOpen ESP32: online logs (D1) + OTA firmware (KV). Free tier only, no R2.

Files: `src/index.js` (worker), `wrangler.toml`, `schema.sql` (D1 migration), `megaopen_cloud.py` (admin CLI, stdlib only).

## Деплой / Deploy

```sh
cd cloud
npm install
npx wrangler login

# 1. D1 database -> copy "database_id" into wrangler.toml [[d1_databases]]
npx wrangler d1 create megaopen
# 2. KV namespace -> copy "id" into wrangler.toml [[kv_namespaces]]
npx wrangler kv namespace create KV
# 3. Schema (idempotent)
npx wrangler d1 execute megaopen --remote --file schema.sql
# 4. Secrets (generate e.g. with: python -c "import secrets;print(secrets.token_urlsafe(32))")
npx wrangler secret put DEVICE_TOKEN
npx wrangler secret put ADMIN_TOKEN
# 5. Deploy -> prints https://megaopen-cloud.<account>.workers.dev
npx wrangler deploy
```

Місця для id позначені `PLACEHOLDER` у `wrangler.toml`. / The id placeholders are marked `PLACEHOLDER` in `wrangler.toml`.

CLI config: `cloud/.env` (gitignored) або змінні оточення / or env vars:
```
MEGAOPEN_CLOUD_URL=https://megaopen-cloud.<account>.workers.dev
MEGAOPEN_CLOUD_ADMIN=<ADMIN_TOKEN>
```
```sh
python megaopen_cloud.py push .pio/build/<env>/firmware.bin 1.4.2 [--devices car1,car2] [--no-activate]
python megaopen_cloud.py manifest | pause | rollback 1.4.1 | versions
python megaopen_cloud.py devices
python megaopen_cloud.py logs [--dev car1] [--since 1h|30m|2d|<epoch ms>] [--follow]
python megaopen_cloud.py coredumps
python megaopen_cloud.py coredump core:car1:1790000000000 -o core.bin
```

Локальний тест / local test:
```sh
cp .dev.vars.example .dev.vars
npx wrangler d1 execute megaopen --local --file schema.sql
npx wrangler dev --local            # http://127.0.0.1:8787
curl "http://127.0.0.1:8787/__scheduled?cron=17+3+*+*+*"   # with --test-scheduled
```

## HTTP contract

Auth on every endpoint: `Authorization: Bearer <token>` (constant-time compare).
Device endpoints need `DEVICE_TOKEN`, admin endpoints need `ADMIN_TOKEN` (the other token gives 401).
Missing/wrong token -> `401 {"ok":false,"error":"unauthorized"}`. Unknown path -> `404`. Wrong method on a known path -> `405`.
Errors are JSON `{"ok":false,"error":"..."}`.

### Device (DEVICE_TOKEN)

**`POST /v1/ingest`** — JSON body, max 64 KB (larger -> 413, bad JSON -> 400, missing/invalid `dev` -> 400):
```json
{"dev":"car1","fw":"1.4.2","boot":17,"up":123456,"reset":"POWERON","heap":145000,"rssi":-71,
 "state":{"...":"any"},"lines":["log line 1","log line 2"]}
```
Stores one row in `batches` (`ts` = server epoch ms, `state` as JSON text, `lines` newline-joined) and
upserts `devices` (last_ts, fw, boot, up, state; fields absent from the body keep their previous value).
Response `200 {"ok":true,"t":<server epoch ms>}` — the device may use `t` as a clock.
`dev` must match `[A-Za-z0-9._:-]{1,64}`. Numeric fields may be omitted (stored NULL).

**`POST /v1/coredump?dev=<dev>&fw=<fw>`** — raw binary body, 1 B..64 KB. Stored in KV as `core:<dev>:<ts>`
(TTL 30 days) and a `batches` row with line `COREDUMP stored core:<dev>:<ts>` is added.
Response `200 {"ok":true,"t":<ms>,"key":"core:<dev>:<ts>"}`.

**`GET /v1/manifest?dev=<dev>&fw=<current fw>`** — records `devices.last_check`. Returns
`200 {"version":"1.4.2","size":1468000,"md5":"<32 hex>","path":"/v1/fw/1.4.2"}` if a manifest is active
AND `version != fw` AND (manifest has no `devices` list OR it contains `dev`); otherwise `204` (empty body).

**`GET /v1/fw/<version>`** — firmware binary. `Content-Type: application/octet-stream`, `Content-Length`,
`x-md5: <32 hex lowercase>`. 404 if unknown. Extra (optional): `HEAD`, and a single `Range: bytes=a-b` -> 206.

### Admin (ADMIN_TOKEN)

| Method & path | Description |
|---|---|
| `PUT /admin/fw/<version>[?devices=a,b][&activate=0]` | raw binary body (max 24 MB). Stores `fw:<version>`, md5 computed server-side. Unless `activate=0`, sets manifest `{version,size,md5,created[,devices]}`. Response `{"ok":true,"stored":{...},"activated":bool,"manifest":{...}\|null}`. Version: `[A-Za-z0-9._+-]{1,64}`. |
| `GET /admin/manifest` | active manifest JSON, or 204 if none |
| `DELETE /admin/manifest` | pause rollout (devices get 204) |
| `POST /admin/manifest` `{"version":"1.4.1"[,"devices":["car1"]]}` | point manifest at an already stored version (rollback); 404 if not stored |
| `GET /admin/fw` | (extra) list stored versions + active one |
| `GET /admin/devices` | all `devices` rows (state parsed as JSON) |
| `GET /admin/logs?dev=&since=<ms>&limit=<n, default 200>&format=text\|json` | newest batches up to `limit`, returned newest-last. `json` (default): array of batch rows with `lines` as array. `text`: one line per log line: `<ISO time> [<dev> ]<fw> up=<up> <line>` (dev shown only when not filtered) |
| `GET /admin/coredumps[?dev=]` | list `[{key,expiration,dev,fw,size,ts}]` |
| `GET /admin/coredump/<key>` | raw coredump bytes |

### Scheduled
Daily cron (`17 3 * * *` UTC): deletes `batches` older than 30 days. Coredumps expire via KV TTL.

## Free-tier notes
- KV free: 1 000 writes/day, 100 000 reads/day, 1 GB storage — firmware uploads/coredumps are rare, fine. Each fw download is 1 KV read.
- D1 free: 100 000 rows written/day, 5 GB. Each ingest = 2 row writes -> keep ingest interval >= ~2 s per device.
- KV is eventually consistent (up to ~60 s across locations): after `push`/`pause`/`rollback`, devices may see the old manifest for up to a minute.
