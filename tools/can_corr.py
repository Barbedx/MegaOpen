#!/usr/bin/env python3
"""can_corr.py - diagnostic replies as ground truth for the broadcast frames.

A capture taken while a diagnostic tool (DDT4all + ELM327) polls the car holds
both: the ECUs' broadcast frames and the request/response pairs on 7xx. This
tool reassembles the ISO-TP replies, decodes them with the DDT2000 definitions
(the DDT4all database, ../ddt-db/x84, outside git: Renault's data), and for
every decoded value looks for the broadcast byte, word or bit that moves with
it. Python stdlib only; the capture reader is can_dump.py's.

    can_corr.py ecus     [--ddt DIR]
    can_corr.py diag     FILE... [--ddt DIR] [--item RE] [-o OUT.csv]
    can_corr.py corr     FILE... [--ddt DIR] [--item RE] [--top N] [--min-n N]
    can_corr.py glossary [--fill] [--md OUT] [--ddt DIR]
    can_corr.py selftest

Record the session in FULL mode (/car: capture "усі кадри"). A change-only
capture drops a reply identical to the last frame on its id, and its keyframes
would replay old ISO-TP frames; this tool ignores KEYFRAME records on the
diagnostic ids, so change-only still works, with fewer samples.

The definitions follow DDT4all (core/ecu/ecu_data.py getHexValue): firstbyte
is 1-based in the reply (the 0x61 counts); the item's endian wins, else the
ECU file's; Big runs MSB-first from bitoffset, Little counts bitoffset from the
LSB inside one byte and swaps wider fields. value = (raw*step + offset)/divideby.

Glossary: docs/signal-glossary.json says which diag item stands for which
Vehicle::Sig. `corr` marks those items and prints a decode_defaults.inc row for
the best broadcast match; `glossary --fill` copies each item's encoding into the
JSON (so it stands without the database), `--md` renders docs/signal-glossary.md.
"""
import argparse
import bisect
import csv
import io
import json
import math
import os
import re
import shutil
import sys
import tempfile
from fractions import Fraction

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import can_dump as cd  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_DDT = os.path.join(os.path.dirname(REPO), "ddt-db", "x84")
GLOSSARY = os.path.join(REPO, "docs", "signal-glossary.json")
READ_SIDS = (0x21, 0x22)          # reads only: these are what gets correlated


# ---- definitions --------------------------------------------------------------

class Item:
    """One data item of one request, resolved to what decoding needs."""

    def __init__(self, name, it, d, ecu_endian):
        self.name = name
        self.firstbyte = it["firstbyte"]
        self.bitoffset = it.get("bitoffset", 0)
        if d.get("bytescount") and not d.get("bitscount"):
            self.bits = int(d["bytescount"] * 8)
        else:
            self.bits = d.get("bitscount", 8)
        self.little = (it.get("endian") or ecu_endian) == "Little"
        self.signed = bool(d.get("signed"))
        self.scaled = bool(d.get("scaled"))
        self.step = float(d.get("step", 1))
        self.offset = float(d.get("offset", 0))
        self.div = float(d.get("divideby", 1)) or 1.0
        self.unit = d.get("unit", "")
        self.lists = {int(k): v for k, v in d.get("lists", {}).items()} if d.get("lists") else {}
        self.ascii = bool(d.get("bytesascii"))

    @property
    def enum(self):
        return bool(self.lists) or self.bits <= 2

    def raw(self, p):
        """The raw field from a reply (bytes), or None when it is too short."""
        b0 = self.firstbyte - 1
        if self.little:
            if self.bitoffset + self.bits <= 8:
                if b0 >= len(p):
                    return None
                v = (p[b0] >> self.bitoffset) & ((1 << self.bits) - 1)
            else:
                nb = (self.bits + 7) // 8
                if b0 + nb > len(p):
                    return None
                v = int.from_bytes(p[b0:b0 + nb], "little")
        else:
            start = b0 * 8 + self.bitoffset
            nb = (self.bitoffset + self.bits + 7) // 8
            if b0 + nb > len(p):
                return None
            word = int.from_bytes(p[b0:b0 + nb], "big")
            v = (word >> (nb * 8 - self.bitoffset - self.bits)) & ((1 << self.bits) - 1)
        if self.signed and self.bits in (8, 16) and v >= 1 << (self.bits - 1):
            v -= 1 << self.bits
        return v

    def value(self, raw):
        if self.scaled:
            return (raw * self.step + self.offset) / self.div
        return float(raw)

    def label(self, raw):
        if self.lists and raw in self.lists:
            return self.lists[raw]
        v = self.value(raw)
        return ("%g %s" % (v, self.unit)).strip()

    def encoding(self):
        """The firmware-shaped encoding: byte, LSB shift, bits, LE, scale."""
        if self.little and self.bitoffset + self.bits <= 8:
            shift = self.bitoffset
        elif self.little:
            shift = 0
        else:
            nb = (self.bitoffset + self.bits + 7) // 8
            shift = nb * 8 - self.bitoffset - self.bits
        return {"byte": self.firstbyte, "shift": shift, "bits": self.bits, "le": self.little,
                "signed": self.signed, "step": self.step if self.scaled else 1,
                "offset": self.offset if self.scaled else 0, "div": self.div if self.scaled else 1,
                "unit": self.unit}


class Request:
    def __init__(self, r, data, ecu_endian):
        self.name = r["name"]
        self.sent = bytes.fromhex(r["sentbytes"])
        self.minbytes = r.get("minbytes", 0)
        self.items = []
        for n, it in (r.get("receivebyte_dataitems") or {}).items():
            d = data.get(n, {})
            if d.get("bytesascii"):
                continue
            item = Item(n, it, d, ecu_endian)
            if item.bits <= 32:
                self.items.append(item)


class Ecu:
    def __init__(self, name, j):
        o = j.get("obd") or {}
        self.name = name
        self.tx = int(o["send_id"], 16) if o.get("send_id") else None
        self.rx = int(o["recv_id"], 16) if o.get("recv_id") else None
        endian = j.get("endian", "Big")
        data = j.get("data") or {}
        self.reqs = [Request(r, data, endian) for r in j.get("requests") or []
                     if r.get("sentbytes") and int(r["sentbytes"][:2], 16) in READ_SIDS]

    def match(self, payload, last_req=None):
        """The request a positive reply answers: the read whose identifier the
        reply echoes, the longest one winning."""
        if not payload or payload[0] - 0x40 not in READ_SIDS:
            return None
        sid = payload[0] - 0x40
        best = None
        for r in self.reqs:
            if r.sent[0] != sid or len(payload) < len(r.sent):
                continue
            if payload[1:len(r.sent)] == r.sent[1:]:
                if best is None or len(r.sent) > len(best.sent):
                    best = r
        if best is None and last_req is not None:
            for r in self.reqs:
                if r.sent == last_req:
                    return r
        return best


def load_ecus(ddt, names=None):
    if not os.path.isdir(ddt):
        raise SystemExit("no DDT definitions at %s (see --ddt; the database lives outside git)" % ddt)
    if names is None:
        names = list(load_glossary()["ecus"])
    out = []
    for n in names:
        p = os.path.join(ddt, n if n.endswith(".json") else n + ".json")
        with open(p, encoding="utf-8") as f:
            e = Ecu(os.path.basename(p)[:-5], json.load(f))
        if e.tx is None:
            print("note: %s has no CAN ids (K-line?), skipped" % e.name, file=sys.stderr)
            continue
        out.append(e)
    return out


def load_glossary(path=GLOSSARY):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


# ---- ISO-TP ---------------------------------------------------------------------

def is_diag_frame(r):
    return r.kind == cd.FRAME or r.kind == cd.KEYFRAME


def isotp(recs, ecus, bus=0):
    """Reassembled diagnostic messages: (t, ecu, 'tx'|'rx', payload bytes).
    KEYFRAME records on these ids are ignored: they repeat an old frame."""
    by_id = {}
    for e in ecus:
        by_id[e.tx] = (e, "tx")
        by_id[e.rx] = (e, "rx")
    part = {}
    out = []
    for r in recs:
        if r.kind != cd.FRAME or r.bus != bus or r.ext or r.id not in by_id:
            continue
        e, d = by_id[r.id]
        p = r.payload()
        if not p:
            continue
        pci = p[0] >> 4
        if pci == 0:
            n = p[0] & 0xF
            if 0 < n <= len(p) - 1:
                out.append((r.t, e, d, bytes(p[1:1 + n])))
            part.pop(r.id, None)
        elif pci == 1 and len(p) >= 2:
            n = ((p[0] & 0xF) << 8) | p[1]
            part[r.id] = [r.t, n, bytearray(p[2:]), 1]
        elif pci == 2:
            s = part.get(r.id)
            if s is None:
                continue
            if (p[0] & 0xF) != (s[3] & 0xF):
                del part[r.id]                    # a lost frame: drop the message
                continue
            s[2] += p[1:]
            s[3] += 1
            if len(s[2]) >= s[1]:
                out.append((r.t, e, d, bytes(s[2][:s[1]])))
                del part[r.id]
        # pci 3 (flow control) carries no data
    return out


def samples(recs, ecus, bus=0):
    """Decoded replies: {(ecu, request): [(t, payload)]}."""
    res = {}
    last = {}
    for t, e, d, p in isotp(recs, ecus, bus):
        if d == "tx":
            last[e.name] = p
            continue
        if p[0] == 0x7F:
            continue
        r = e.match(p, last.get(e.name))
        if r is None or len(p) < r.minbytes:
            continue
        res.setdefault((e.name, r.sent.hex().upper()), (e, r, []))[2].append((t, p))
    return res


# ---- broadcast fields -----------------------------------------------------------

class Field:
    __slots__ = ("w", "start", "n", "shift", "mask", "le", "signed", "kind")

    def __init__(self, w, start, n, shift, mask, le=False, signed=False, kind=""):
        self.w, self.start, self.n, self.shift, self.mask = w, start, n, shift, mask
        self.le, self.signed, self.kind = le, signed, kind

    def get(self, p):
        if self.start + self.n > len(p):
            return None
        b = p[self.start:self.start + self.n]
        v = (int.from_bytes(b, "little" if self.le else "big") >> self.shift) & self.mask
        if self.signed:
            top = (self.mask + 1) >> 1
            if v >= top:
                v -= self.mask + 1
        return v

    def name(self):
        cid, bus, ext = cd.split_word(self.w)
        idt = ("%08X" if ext else "0x%03X") % cid
        if self.mask == 1:
            return "%s b%d bit%d" % (idt, self.start + self.shift // 8, self.shift % 8) if self.n == 1 \
                else "%s %s@%d bit%d" % (idt, self.kind, self.start, self.shift)
        if self.kind == "u2":
            return "%s b%d bits%d-%d" % (idt, self.start, self.shift, self.shift + 1)
        return "%s %s@%d" % (idt, self.kind, self.start)


def fields_for(w, length, enum):
    out = []
    if enum:
        for i in range(length):
            for k in range(8):
                out.append(Field(w, i, 1, k, 1, kind="bit"))
            for k in range(0, 7):
                out.append(Field(w, i, 1, k, 3, kind="u2"))
            out.append(Field(w, i, 1, 0, 0xFF, kind="u8"))
        return out
    for i in range(length):
        out.append(Field(w, i, 1, 0, 0xFF, kind="u8"))
        out.append(Field(w, i, 1, 0, 0xFF, signed=True, kind="s8"))
        if i + 2 <= length:
            for le in (False, True):
                for s in (False, True):
                    out.append(Field(w, i, 2, 0, 0xFFFF, le=le, signed=s,
                                     kind=("s" if s else "u") + "16" + ("le" if le else "be")))
        if i + 3 <= length:
            out.append(Field(w, i, 3, 0, 0xFFFFFF, kind="u24be"))
    return out


class Broadcast:
    """Per id word: times and payloads, for 'the data at time t'."""

    def __init__(self, recs, skip_ids, bus=None):
        self.t, self.p, self.len = {}, {}, {}
        for r in cd.frames(recs, bus):
            if r.id in skip_ids and not r.ext:
                continue
            self.t.setdefault(r.w, []).append(r.t)
            pl = r.payload()
            self.p.setdefault(r.w, []).append(pl)
            self.len[r.w] = max(self.len.get(r.w, 0), len(pl))

    def at(self, w, t):
        ts = self.t[w]
        i = bisect.bisect_right(ts, t) - 1
        return self.p[w][i] if i >= 0 else None


# ---- scoring --------------------------------------------------------------------

def transitions(vals):
    return sum(1 for a, b in zip(vals, vals[1:]) if a != b)


def score_numeric(fv, dv):
    """r squared and the fit dv = a*fv + b over the pairs where fv exists."""
    pts = [(f, d) for f, d in zip(fv, dv) if f is not None]
    n = len(pts)
    if n < 3:
        return None
    sf = sum(f for f, _ in pts)
    sd = sum(d for _, d in pts)
    mf, md = sf / n, sd / n
    cff = sum((f - mf) ** 2 for f, _ in pts)
    cdd = sum((d - md) ** 2 for _, d in pts)
    if cff == 0 or cdd == 0:
        return None
    cfd = sum((f - mf) * (d - md) for f, d in pts)
    a = cfd / cff
    return cfd * cfd / (cff * cdd), a, md - a * mf, n


def score_enum(fv, dv):
    """How well each predicts the other (the weaker way), as a lift over
    always guessing the commonest diag value: 1 = a perfect 1:1 map."""
    pts = [(f, d) for f, d in zip(fv, dv) if f is not None]
    n = len(pts)
    if n < 3:
        return None
    by_f, by_d, cnt_d = {}, {}, {}
    for f, d in pts:
        by_f.setdefault(f, {}).setdefault(d, 0)
        by_f[f][d] += 1
        by_d.setdefault(d, {}).setdefault(f, 0)
        by_d[d][f] += 1
        cnt_d[d] = cnt_d.get(d, 0) + 1
    if len(by_f) < 2 or len(cnt_d) < 2:
        return None
    fwd = sum(max(c.values()) for c in by_f.values()) / n
    bwd = sum(max(c.values()) for c in by_d.values()) / n
    base = max(cnt_d.values()) / n
    acc = min(fwd, bwd)
    lift = (acc - base) / (1 - base) if base < 1 else 0.0
    mapping = {f: max(c, key=c.get) for f, c in by_f.items()}
    return lift, acc, mapping, n


def nice_ratio(x, maxden=1000):
    fr = Fraction(x).limit_denominator(maxden)
    return fr.numerator, fr.denominator


def sig_glossary_index(g):
    """(ecu, req, item) -> (sig, to_sig)."""
    out = {}
    for s in g.get("signals", []):
        for d in s.get("diag", []):
            out[(d["ecu"], d["req"].upper(), d["item"])] = (s["sig"], d.get("to_sig"))
    return out


def decode_row(field, sig, mul, div, add, flags="DF_HYPOTHESIS"):
    cid, _, ext = cd.split_word(field.w)
    fl = flags + (" | DF_LE" if field.le else "") + (" | DF_SIGNED" if field.signed else "") \
        + (" | DF_EXT" if ext else "")
    return "{ 0x%03X, %3d, %d, %2d, 0x%Xu, %d, %d, %d, %s, %s, 0 }," % (
        cid, field.start, field.n, field.shift, field.mask, mul, div, add, sig, fl)


def correlate(recs, ecus, item_re=None, min_n=10, top=3, bus=0, glossary=None):
    """Per decoded item: its samples and the best broadcast candidates."""
    skip = set()
    for e in ecus:
        skip.update((e.tx, e.rx))
    skip.update(range(0x7DF, 0x7E0))
    bc = Broadcast(recs, skip)
    gidx = sig_glossary_index(glossary or {"signals": []})
    rx = re.compile(item_re, re.I) if item_re else None
    results = []
    for (ename, rhex), (e, r, ss) in sorted(samples(recs, ecus, bus).items()):
        times = [t for t, _ in ss]
        # The broadcast state at each sample time, once per request.
        snap = {w: [bc.at(w, t) for t in times] for w in bc.t}
        fcache = {}
        for item in r.items:
            if rx and not rx.search(item.name):
                continue
            raws = [item.raw(p) for _, p in ss]
            keep = [i for i, v in enumerate(raws) if v is not None]
            if len(keep) < min_n:
                continue
            vals = [item.value(raws[i]) for i in keep]
            distinct = len(set(vals))
            res = {"ecu": ename, "req": rhex, "item": item, "n": len(keep), "distinct": distinct,
                   "trans": transitions(vals), "first": vals[0], "last": vals[-1],
                   "min": min(vals), "max": max(vals), "cands": []}
            res["sig"], res["to_sig"] = gidx.get((ename, rhex, item.name), (None, None))
            results.append(res)
            enum = item.enum or distinct <= 2
            if distinct < 2 or (not enum and distinct < 3):
                continue
            cands = []
            for w, pl in snap.items():
                key = (w, enum)
                if key not in fcache:
                    fcache[key] = [(f, [f.get(x) if x is not None else None for x in pl])
                                   for f in fields_for(w, bc.len[w], enum)]
                for f, series in fcache[key]:
                    fv = [series[i] for i in keep]
                    if enum:
                        s = score_enum(fv, vals)
                        if s and s[0] > 0.5:
                            cands.append((s[0], f, s))
                    else:
                        s = score_numeric(fv, vals)
                        if s and s[0] > 0.5:
                            cands.append((s[0], f, s))
            # Prefer, among equal scores, the narrower field (a bit over a byte).
            cands.sort(key=lambda c: (-round(c[0], 4), c[1].mask, c[1].n))
            res["cands"] = cands[:top]
    return results


def print_results(results, out=sys.stdout):
    for res in results:
        it = res["item"]
        tag = "  [%s]" % res["sig"] if res["sig"] else ""
        out.write("%s %s  %s%s\n" % (res["ecu"], res["req"], it.name, tag))
        out.write("    n=%d distinct=%d changes=%d range %g..%g %s\n" % (
            res["n"], res["distinct"], res["trans"], res["min"], res["max"], it.unit))
        if res["distinct"] < 2:
            out.write("    constant during the capture: nothing to correlate\n")
            continue
        if not res["cands"]:
            out.write("    no broadcast field follows it\n")
            continue
        weak = " (WEAK: under 3 changes)" if res["trans"] < 3 else ""
        for score, f, s in res["cands"]:
            if it.enum or res["distinct"] <= 2:
                mp = ", ".join("%s->%s" % (k, it.label(int(v)) if float(v).is_integer() else v)
                               for k, v in sorted(s[2].items(), key=lambda kv: str(kv[0])))
                out.write("    %-26s lift=%.3f acc=%.3f  {%s}%s\n" % (f.name(), score, s[1], mp[:80], weak))
            else:
                out.write("    %-26s r2=%.4f  value = %.6g * raw %+.6g%s\n" % (f.name(), score, s[1], s[2], weak))
        best = res["cands"][0]
        if res["sig"] and res["to_sig"] is not None:
            f, s = best[1], best[2]
            if it.enum or res["distinct"] <= 2:
                inv = s[2].get(0) not in (None, 0.0) and s[2].get(1) in (0.0,)
                mul, div, add = (-1, 1, 1) if inv else (1, 1, 0)
            else:
                k = res["to_sig"]
                mul, div = nice_ratio(s[1] * k)
                add = int(round(s[2] * k))
            out.write("    row: %s\n" % decode_row(f, res["sig"], mul, div, add))


# ---- commands -------------------------------------------------------------------

def cmd_ecus(a):
    for e in load_ecus(a.ddt, a.ecu or None):
        out = sys.stdout
        out.write("%-46s tx 0x%03X rx 0x%03X  %d reads\n" % (e.name, e.tx, e.rx, len(e.reqs)))
    return 0


def cmd_diag(a):
    ecus = load_ecus(a.ddt, a.ecu or None)
    _, recs, _ = cd.read_files(a.files)
    rx = re.compile(a.item, re.I) if a.item else None
    out = open(a.o, "w", newline="", encoding="utf-8") if a.o else sys.stdout
    w = csv.writer(out, lineterminator="\n")
    w.writerow(["t_ms", "ecu", "req", "item", "raw", "value", "label"])
    n = 0
    for (ename, rhex), (e, r, ss) in sorted(samples(recs, ecus, a.bus).items()):
        for t, p in ss:
            for it in r.items:
                if rx and not rx.search(it.name):
                    continue
                raw = it.raw(p)
                if raw is None:
                    continue
                w.writerow([t, ename, rhex, it.name, raw, "%g" % it.value(raw), it.label(raw)])
                n += 1
    if a.o:
        out.close()
        print("%d values -> %s" % (n, a.o))
    return 0


def cmd_corr(a):
    ecus = load_ecus(a.ddt, a.ecu or None)
    _, recs, _ = cd.read_files(a.files)
    g = load_glossary() if os.path.exists(GLOSSARY) else None
    res = correlate(recs, ecus, a.item, a.min_n, a.top, a.bus, g)
    if not res:
        print("no decoded diagnostic replies in the capture (was a tool polling? full mode?)")
        return 1
    print_results(res)
    return 0


def cmd_glossary(a):
    g = load_glossary()
    if a.fill:
        ecus = {}
        for name in g["ecus"]:
            p = os.path.join(a.ddt, name + ".json")
            with open(p, encoding="utf-8") as f:
                j = json.load(f)
            ecus[name] = j
            o = j.get("obd") or {}
            if o.get("send_id"):
                g["ecus"][name]["tx"], g["ecus"][name]["rx"] = o["send_id"], o["recv_id"]
        bad = 0
        for s in g["signals"]:
            for d in s.get("diag", []):
                j = ecus[d["ecu"]]
                r = next((r for r in j["requests"] if r["sentbytes"].upper() == d["req"].upper()), None)
                it = r and (r.get("receivebyte_dataitems") or {}).get(d["item"])
                if not it:
                    print("NOT FOUND: %s %s %r" % (d["ecu"], d["req"], d["item"]))
                    bad += 1
                    continue
                item = Item(d["item"], it, j["data"].get(d["item"], {}), j.get("endian", "Big"))
                d.update(item.encoding())
                if item.lists:
                    d["values"] = {str(k): v for k, v in sorted(item.lists.items())}
        if bad:
            return 1
        with open(GLOSSARY, "w", encoding="utf-8", newline="\n") as f:
            f.write(dump_glossary(g))
        print("filled %s" % os.path.relpath(GLOSSARY, REPO))
    if a.md:
        with open(a.md, "w", encoding="utf-8", newline="\n") as f:
            f.write(glossary_md(g))
        print("wrote %s" % a.md)
    if not a.fill and not a.md:
        sys.stdout.write(glossary_md(g))
    return 0


def dump_glossary(g):
    """One signal per block, one diag ref per line: diff-friendly."""
    o = io.StringIO()
    o.write("{\n  \"about\": %s,\n" % json.dumps(g["about"], ensure_ascii=False, indent=2).replace("\n", "\n  "))
    o.write("  \"ecus\": {\n")
    o.write(",\n".join("    %s: %s" % (json.dumps(k, ensure_ascii=False), json.dumps(v, ensure_ascii=False))
                       for k, v in g["ecus"].items()))
    o.write("\n  },\n  \"signals\": [\n")
    blocks = []
    for s in g["signals"]:
        head = {k: v for k, v in s.items() if k not in ("diag", "note", "broadcast")}
        lines = ["    {" + json.dumps(head, ensure_ascii=False)[1:-1]]
        if "broadcast" in s:
            lines.append("     \"broadcast\": " + json.dumps(s["broadcast"], ensure_ascii=False))
        if s.get("diag"):
            lines.append("     \"diag\": [\n" + ",\n".join("       " + json.dumps(d, ensure_ascii=False)
                                                       for d in s["diag"]) + "]")
        if "note" in s:
            lines.append("     \"note\": " + json.dumps(s["note"], ensure_ascii=False))
        blocks.append(",\n".join(lines) + "}")
    o.write(",\n".join(blocks))
    o.write("\n  ],\n  \"absent\": %s\n}\n" % json.dumps(g.get("absent", []), ensure_ascii=False))
    return o.getvalue()


def fmt_scale(d):
    if "step" not in d:
        return "(not filled: run glossary --fill)"
    s = "x"
    if d["step"] != 1:
        s = "x*%g" % d["step"]
    if d["offset"]:
        s += "%+g" % d["offset"]
    if d["div"] != 1:
        s = "(%s)/%g" % (s, d["div"])
    return (s + " " + d.get("unit", "")).strip()


def glossary_md(g):
    L = ["# Signal glossary: where each Vehicle::Sig comes from", "",
         "**Generated from `docs/signal-glossary.json` by `tools/can_corr.py glossary --md`. Edit the JSON, not this file.**",
         "What this is, the findings and the plan order: `docs/signal-glossary-README.md`.", "",
         "For OUR Mégane II (X84), whose ECUs the DDT4all scan of 2026-06-14 identified",
         "(its log is not in this repository). `pick` is the source to make the default:",
         "`broadcast` decodes a frame the car sends anyway (decode_defaults.inc, all still",
         "DF_HYPOTHESIS), `diag` polls a read on the car bus (MegaOpen does not do this yet),",
         "and `derived` is computed in MegaOpen. Confidence is a judgement until the",
         "correlator (`tools/can_corr.py corr`) has measured it against a capture.", "",
         "Diag columns: `req` is sent to `tx`, and the reply comes back on `rx`. `byte` is 1-based in the reply, and the 0x61",
         "counts. `shift`/`bits` are LSB-based inside the big-endian word that the field spans",
         "(LE: inside the byte, or byte-swapped), the same convention as decode_defaults.inc.", "",
         "## ECUs", "", "| ECU file | tx/rx | what |", "|---|---|---|"]
    for k, v in g["ecus"].items():
        L.append("| `%s` | %s/%s | %s |" % (k, v["tx"], v["rx"], v.get("what", "")))
    L += ["", "## Signals", "",
          "| signal | pick | conf. | broadcast (hypothesis) | diag: ecu req byte/shift/bits | scale → signal |",
          "|---|---|---|---|---|---|"]
    for s in g["signals"]:
        b = s.get("broadcast")
        bs = "`%s` %s<br>*%s*" % (b["id"], b["row"], b["status"]) if b else "—"
        ds, sc = [], []
        for d in s.get("diag", []):
            ecu = g["ecus"].get(d["ecu"], {})
            pos = "b%s>>%s/%s%s" % (d.get("byte", "?"), d.get("shift", "?"), d.get("bits", "?"),
                                    " LE" if d.get("le") else "")
            ds.append("%s `%s` %s *%s*" % (ecu.get("tx", "?"), d["req"], pos, d["item"]))
            k = d.get("to_sig")
            vals = ""
            if d.get("values"):
                vals = " {" + ", ".join("%s=%s" % kv for kv in list(d["values"].items())[:4]) + "}"
            sc.append(fmt_scale(d) + vals + (" ×%g" % k if k not in (None, 1) else " (derived)" if k is None else ""))
        L.append("| `%s` | %s | %s | %s | %s | %s |" % (
            s["sig"], s["pick"], s.get("confidence", ""), bs, "<br>".join(ds) or "—", "<br>".join(sc) or "—"))
    L += ["", "## Notes", ""]
    for s in g["signals"]:
        if s.get("note"):
            L.append("- **%s**: %s" % (s["sig"], s["note"]))
    L += ["", "## Not on this car", "", ", ".join("`%s`" % x for x in g.get("absent", [])), ""]
    return "\n".join(L)


# ---- selftest -------------------------------------------------------------------

FAKE_ECU = {
    "obd": {"protocol": "CAN", "send_id": "7E0", "recv_id": "7E8"},
    "endian": "Big",
    "data": {
        "rpm": {"bitscount": 16, "scaled": True, "unit": "rpm"},
        "temp": {"scaled": True, "step": 0.625, "offset": -40, "unit": "C"},
        "door": {"bitscount": 1, "lists": {"0": "closed", "1": "open"}},
    },
    "requests": [{"sentbytes": "21A0", "name": "general", "minbytes": 7, "receivebyte_dataitems": {
        "temp": {"firstbyte": 3}, "rpm": {"firstbyte": 4}, "door": {"firstbyte": 6, "bitoffset": 2}}}],
}


def cmd_selftest(_a):
    tmp = tempfile.mkdtemp(prefix="can_corr_")
    with open(os.path.join(tmp, "Fake.json"), "w", encoding="utf-8") as f:
        json.dump(FAKE_ECU, f)
    ok = True
    for mode in (1, 0):                    # full, then change-only
        path = os.path.join(tmp, "000%d.bin" % (mode + 1))
        with open(path, "wb") as f:
            w = cd.Writer(f, mode=mode)
            rpm, door, temp_raw = 800, 0, 150
            for t in range(0, 120000, 10):
                if t % 1000 == 0:
                    rpm = 800 + (t // 1000 * 37) % 2400          # a ramp that wraps
                if t % 7000 == 0:
                    door ^= 1
                if t % 9000 == 0:
                    temp_raw = 150 + (t // 9000) % 9
                if t % 20 == 0:                                 # 0x181: rpm*8 at 0-1, a counter at 2
                    v = rpm * 8
                    w.frame(t, 0x181, [v >> 8, v & 0xFF, (t // 20) & 0xFF, 0x10])
                if t % 100 == 0:                                # 0x60D: door at byte 2 bit 3
                    w.frame(t, 0x60D, [0x00, 0x04, door << 3, 0x00, 0x50, 0, 0, 0])
                    w.frame(t, 0x551, [temp_raw, 0x22])
                if t % 250 == 0:                                # the tool polls 21A0: SF out, FF+CF back
                    w.frame(t, 0x7E0, [0x02, 0x21, 0xA0, 0, 0, 0, 0, 0])
                    reply = bytes([0x61, 0xA0, temp_raw, rpm >> 8, rpm & 0xFF, (door << 5), 0x00])
                    w.frame(t + 2, 0x7E8, [0x10, len(reply)] + list(reply[:6]))
                    w.frame(t + 4, 0x7E0, [0x30, 0, 0, 0, 0, 0, 0, 0])
                    w.frame(t + 6, 0x7E8, [0x21] + list(reply[6:]) + [0xAA] * (7 - len(reply[6:])))
        _, recs, _ = cd.read_files([path])
        ecus = load_ecus(tmp, ["Fake"])
        ss = samples(recs, ecus)
        n = len(ss[("Fake", "21A0")][2])
        res = {r["item"].name: r for r in correlate(recs, ecus, min_n=10)}
        best = {k: v["cands"][0][1].name() if v["cands"] else None for k, v in res.items()}
        expect = {"rpm": "0x181 u16be@0", "door": "0x60D b2 bit3", "temp": "0x551 u8@0"}
        for k, e in expect.items():
            if best.get(k) != e:
                print("FAIL mode %d %s: best %r, want %r" % (mode, k, best.get(k), e))
                ok = False
        fit = res["rpm"]["cands"][0][2]
        if abs(fit[1] - 0.125) > 1e-6:
            print("FAIL mode %d rpm slope %r" % (mode, fit[1]))
            ok = False
        print("mode %d: %d replies; rpm %s, door %s, temp %s" % (mode, n, best["rpm"], best["door"], best["temp"]))
    # The decoder against DDT4all's own rule on a LITTLE-endian ECU file.
    it = Item("x", {"firstbyte": 2, "bitoffset": 3}, {"bitscount": 1}, "Little")
    if it.raw(bytes([0x00, 0x08])) != 1 or it.raw(bytes([0x00, 0x10])) != 0:
        print("FAIL little-endian bit")
        ok = False
    it = Item("x", {"firstbyte": 2, "bitoffset": 3, "endian": "Big"}, {"bitscount": 1}, "Little")
    if it.raw(bytes([0x00, 0x10])) != 1:
        print("FAIL big-endian bit")
        ok = False
    shutil.rmtree(tmp, ignore_errors=True)
    print("selftest OK" if ok else "selftest FAILED")
    return 0 if ok else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(s, files=True):
        if files:
            s.add_argument("files", nargs="+")
            s.add_argument("--bus", type=int, default=0, help="the car's bus (default 0)")
        s.add_argument("--ddt", default=DEFAULT_DDT, help="DDT2000 JSON folder (default ../ddt-db/x84)")
        s.add_argument("--ecu", action="append", help="ECU file name; default: the glossary's ECUs")

    s = sub.add_parser("ecus", help="the definitions in use and their ids")
    common(s, files=False)
    s.set_defaults(fn=cmd_ecus)
    s = sub.add_parser("diag", help="every decoded diag value, as CSV")
    common(s)
    s.add_argument("--item", help="regex on the item name")
    s.add_argument("-o", help="CSV file (default stdout)")
    s.set_defaults(fn=cmd_diag)
    s = sub.add_parser("corr", help="broadcast fields that follow each diag value")
    common(s)
    s.add_argument("--item", help="regex on the item name")
    s.add_argument("--top", type=int, default=3)
    s.add_argument("--min-n", type=int, default=10, help="fewest samples to try an item")
    s.set_defaults(fn=cmd_corr)
    s = sub.add_parser("glossary", help="fill the glossary's encodings, render its Markdown")
    s.add_argument("--fill", action="store_true", help="copy encodings from the DDT files into the JSON")
    s.add_argument("--md", help="write the Markdown here (default: print it)")
    s.add_argument("--ddt", default=DEFAULT_DDT)
    s.set_defaults(fn=cmd_glossary)
    s = sub.add_parser("selftest", help="a synthetic polled capture through every step")
    s.set_defaults(fn=cmd_selftest)
    a = ap.parse_args(argv)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
