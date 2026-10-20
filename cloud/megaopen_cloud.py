#!/usr/bin/env python3
"""MegaOpen cloud admin CLI (stdlib only).

Config: env MEGAOPEN_CLOUD_URL and MEGAOPEN_CLOUD_ADMIN, or cloud/.env (gitignored):
    MEGAOPEN_CLOUD_URL=https://megaopen-cloud.<you>.workers.dev
    MEGAOPEN_CLOUD_ADMIN=<ADMIN_TOKEN>

Commands:
    push <firmware.bin> <version> [--devices a,b] [--no-activate]
    manifest | pause | rollback <version> | versions
    devices
    logs [--dev X] [--since 1h|30m|2d|<epoch ms>] [--limit N] [--follow]
    coredumps [--dev X]
    coredump <key> -o file
"""
import argparse
import hashlib
import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
UA = "megaopen-cloud-cli/1.0"  # Cloudflare may reject the default Python-urllib UA


def load_config():
    env_file = os.path.join(HERE, ".env")
    file_vals = {}
    if os.path.exists(env_file):
        with open(env_file, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                file_vals[k.strip()] = v.strip().strip('"').strip("'")
    url = os.environ.get("MEGAOPEN_CLOUD_URL") or file_vals.get("MEGAOPEN_CLOUD_URL")
    tok = os.environ.get("MEGAOPEN_CLOUD_ADMIN") or file_vals.get("MEGAOPEN_CLOUD_ADMIN")
    if not url or not tok:
        sys.exit("error: set MEGAOPEN_CLOUD_URL and MEGAOPEN_CLOUD_ADMIN (env or cloud/.env)")
    return url.rstrip("/"), tok


class Api:
    def __init__(self):
        self.base, self.token = load_config()

    def call(self, method, path, params=None, body=None, ctype=None, raw=False):
        url = self.base + path
        if params:
            q = {k: v for k, v in params.items() if v is not None}
            if q:
                url += "?" + urllib.parse.urlencode(q)
        headers = {"Authorization": "Bearer " + self.token, "User-Agent": UA}
        data = None
        if body is not None:
            if isinstance(body, (bytes, bytearray)):
                data = bytes(body)
                headers["Content-Type"] = ctype or "application/octet-stream"
            else:
                data = json.dumps(body).encode()
                headers["Content-Type"] = "application/json"
        req = urllib.request.Request(url, data=data, method=method, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=120) as r:
                payload = r.read()
                status = r.status
        except urllib.error.HTTPError as e:
            msg = e.read().decode("utf-8", "replace")
            sys.exit(f"HTTP {e.code} {method} {path}: {msg}")
        except urllib.error.URLError as e:
            raise ConnectionError(str(e.reason))
        if raw:
            return status, payload
        if status == 204 or not payload:
            return status, None
        return status, json.loads(payload)


def pj(obj):
    print(json.dumps(obj, indent=2, ensure_ascii=False))


def fmt_ts(ms):
    if not ms:
        return "-"
    return datetime.fromtimestamp(ms / 1000, tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"


def parse_since(s):
    if s is None:
        return None
    if s.isdigit():
        return int(s)  # absolute epoch ms
    m = re.fullmatch(r"(\d+)\s*([smhd])", s)
    if not m:
        sys.exit("error: --since must be like 30s, 30m, 1h, 2d or epoch ms")
    mult = {"s": 1, "m": 60, "h": 3600, "d": 86400}[m.group(2)]
    return int(time.time() * 1000) - int(m.group(1)) * mult * 1000


# ---------- commands ----------

def cmd_push(api, a):
    with open(a.file, "rb") as f:
        data = f.read()
    md5 = hashlib.md5(data).hexdigest()
    print(f"uploading {a.file} ({len(data)} bytes, md5 {md5}) as {a.version} ...")
    params = {"devices": a.devices, "activate": "0" if a.no_activate else None}
    _, res = api.call("PUT", "/admin/fw/" + urllib.parse.quote(a.version), params, body=data)
    if res["stored"]["md5"] != md5 or res["stored"]["size"] != len(data):
        sys.exit(f"error: server md5/size mismatch: {res['stored']}")
    pj(res)
    print("activated (devices will pick it up)" if res["activated"] else "stored only, NOT activated")


def cmd_manifest(api, a):
    st, res = api.call("GET", "/admin/manifest")
    print("no active manifest (rollout paused)" if st == 204 else json.dumps(res, indent=2))


def cmd_pause(api, a):
    pj(api.call("DELETE", "/admin/manifest")[1])


def cmd_rollback(api, a):
    body = {"version": a.version}
    if a.devices:
        body["devices"] = [d.strip() for d in a.devices.split(",") if d.strip()]
    pj(api.call("POST", "/admin/manifest", body=body)[1])


def cmd_versions(api, a):
    _, res = api.call("GET", "/admin/fw")
    for v in res["versions"]:
        mark = "*" if v["version"] == res["active"] else " "
        print(f"{mark} {v['version']:<24} {v.get('size', '?'):>9}  {v.get('md5', '')}  {fmt_ts(v.get('created'))}")


def cmd_devices(api, a):
    _, rows = api.call("GET", "/admin/devices")
    if a.json:
        return pj(rows)
    for r in rows:
        print(f"{r['dev']}: fw={r.get('fw')} boot={r.get('boot')} up={r.get('up')} "
              f"last={fmt_ts(r.get('last_ts'))} check={fmt_ts(r.get('last_check'))}")
        if r.get("state"):
            print("    state:", json.dumps(r["state"], ensure_ascii=False))


def print_batch(b, show_dev):
    prefix = f"{fmt_ts(b['ts'])} {b['dev'] + ' ' if show_dev else ''}{b.get('fw') or '-'} up={b.get('up') if b.get('up') is not None else '-'}"
    for line in b.get("lines") or []:
        if line != "":
            print(prefix, line, flush=True)


def cmd_logs(api, a):
    since = parse_since(a.since)
    show_dev = not a.dev
    if not a.follow:
        _, rows = api.call("GET", "/admin/logs", {"dev": a.dev, "since": since, "limit": a.limit})
        for b in rows:
            print_batch(b, show_dev)
        return
    seen = set()
    last_ts = since
    while True:
        try:
            _, rows = api.call("GET", "/admin/logs", {"dev": a.dev, "since": last_ts, "limit": a.limit})
        except ConnectionError as e:
            print(f"# connection error: {e}; retrying", file=sys.stderr)
            rows = []
        for b in rows:
            if b["id"] in seen:
                continue
            seen.add(b["id"])
            print_batch(b, show_dev)
            last_ts = max(last_ts or 0, b["ts"])
        # keep only ids at the current boundary ts (since is inclusive)
        seen = {b["id"] for b in rows if b["ts"] >= (last_ts or 0)}
        try:
            time.sleep(5)
        except KeyboardInterrupt:
            return


def cmd_coredumps(api, a):
    _, rows = api.call("GET", "/admin/coredumps", {"dev": a.dev})
    for r in rows:
        print(f"{r['key']}  fw={r.get('fw')} size={r.get('size')} at={fmt_ts(r.get('ts'))}")
    if not rows:
        print("no coredumps")


def cmd_coredump(api, a):
    _, data = api.call("GET", "/admin/coredump/" + urllib.parse.quote(a.key, safe=":"), raw=True)
    with open(a.output, "wb") as f:
        f.write(data)
    print(f"saved {len(data)} bytes to {a.output}")


def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    p = argparse.ArgumentParser(description="MegaOpen cloud admin CLI")
    sp = p.add_subparsers(dest="cmd", required=True)

    s = sp.add_parser("push", help="upload firmware (and activate by default)")
    s.add_argument("file")
    s.add_argument("version")
    s.add_argument("--devices", help="comma-separated device ids to target (default: all)")
    s.add_argument("--no-activate", action="store_true", help="store only, don't change manifest")
    s.set_defaults(fn=cmd_push)

    sp.add_parser("manifest", help="show active manifest").set_defaults(fn=cmd_manifest)
    sp.add_parser("pause", help="delete manifest (pause rollout)").set_defaults(fn=cmd_pause)
    s = sp.add_parser("rollback", help="point manifest to an already stored version")
    s.add_argument("version")
    s.add_argument("--devices")
    s.set_defaults(fn=cmd_rollback)
    sp.add_parser("versions", help="list stored firmware versions").set_defaults(fn=cmd_versions)

    s = sp.add_parser("devices", help="list devices")
    s.add_argument("--json", action="store_true")
    s.set_defaults(fn=cmd_devices)

    s = sp.add_parser("logs", help="show logs (newest last)")
    s.add_argument("--dev")
    s.add_argument("--since", help="30m, 1h, 2d or epoch ms")
    s.add_argument("--limit", type=int, default=200, help="max batches (default 200)")
    s.add_argument("--follow", "-f", action="store_true", help="poll every 5 s for new lines")
    s.set_defaults(fn=cmd_logs)

    s = sp.add_parser("coredumps", help="list stored coredumps")
    s.add_argument("--dev")
    s.set_defaults(fn=cmd_coredumps)
    s = sp.add_parser("coredump", help="download a coredump")
    s.add_argument("key")
    s.add_argument("-o", "--output", required=True)
    s.set_defaults(fn=cmd_coredump)

    a = p.parse_args()
    try:
        a.fn(Api(), a)
    except ConnectionError as e:
        sys.exit(f"connection error: {e}")
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
