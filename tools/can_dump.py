#!/usr/bin/env python3
"""can_dump.py - read MegaOpen CAN captures (/cap/NNNN.bin) on the PC.

The format is src/capture/CapFormat.h; this file is its second reader and is
checked against the same layout by `selftest`. Python stdlib only. It reads
both versions: v1 (MOCAP1, 18-byte records, the files of 1.11 and before) and
v2 (MOCAP2, self-contained segments of variable-length records: what 1.12
keeps in RAM, pushes as cap/<session>/rNNNN.bin and spills to flash).

    can_dump.py info     FILE...
    can_dump.py convert  FILE... [--to gvret|candump] [-o OUT]
    can_dump.py ids      FILE... [--bus N]
    can_dump.py between  A B FILE... [--bus N]
    can_dump.py slow     FILE... [--bus N] [--min-span S]
    can_dump.py track    ID BYTE FILE... [--bus N] [-o OUT.csv]
    can_dump.py hu       FILE...            the head unit's UART (bus 2), frame by frame
    can_dump.py pack     FILE... -o OUT     v1 (or v2) to v2 segments; prints bytes/record
    can_dump.py selftest

FILE... are the files of one session in order (a session that crossed the
file size is NNNN.bin, NNNN+1.bin ...); they are read as one stream.

CHANGE-ONLY captures (the default on the board) hold a frame only when its
data changed, plus a keyframe per id every 5 s and a STATS record per id every
second with the real frame count. `convert` writes the frames that are in the
file (so a change-only capture converts to a sparse log; replaying it in
SavvyCAN shows every value change but not the repeat rate); `ids` takes rates
from STATS.

Markers (A, B for `between`) are given by number ("3") or by a piece of their
text ("fuel 3/4").
"""
import argparse
import csv
import io
import os
import shutil
import struct
import sys
import tempfile

MAGIC = b"MOCAP1"
MAGIC2 = b"MOCAP2"
HEADER_SIZE = 64
HEADER2_SIZE = 80
REC_SIZE = 18
SEG_MAX = 64 * 1024
SEG_MS = 60000
SEG_IDS = 255
IDX_ESC = 0xFF
F_FULL = 0x08
DT_ESC = 15
FRAME, KEYFRAME, MARKER, STATS, GAP, BITRATE, MODE = range(7)
KIND_NAMES = {FRAME: "frame", KEYFRAME: "key", MARKER: "marker", STATS: "stats",
              GAP: "gap", BITRATE: "bitrate", MODE: "mode"}
ID_MASK = 0x1FFFFFFF
EXT_BIT = 1 << 31

HDR = struct.Struct("<6sHIHBBIIIIIHH24s")
HDR2 = struct.Struct("<IIII")              # t0, segLen, seq, records (v2, at 64)
REC = struct.Struct("<IIBB8s")
assert HDR.size == HEADER_SIZE and REC.size == REC_SIZE and HEADER_SIZE + HDR2.size == HEADER2_SIZE


def id_word(cid, ext, bus):
    return (cid & ID_MASK) | ((bus & 3) << 29) | (EXT_BIT if ext else 0)


def split_word(w):
    return w & ID_MASK, (w >> 29) & 3, bool(w & EXT_BIT)


# ---- reading ------------------------------------------------------------------

class Header:
    def __init__(self, raw):
        if len(raw) < HEADER_SIZE:
            raise ValueError("short header (%d bytes)" % len(raw))
        (magic, hlen, self.session, self.part, self.mode, recsize, br0, br1, self.boot,
         self.wall_sec, self.wall_at_ms, self.keyframe_ms, self.stats_ms, build) = HDR.unpack(raw[:HEADER_SIZE])
        if magic == MAGIC:
            self.version = 1
            if recsize != REC_SIZE:
                raise ValueError("record size %d, this tool reads %d" % (recsize, REC_SIZE))
            self.t0 = self.seg_len = self.seq = self.records = 0
        elif magic == MAGIC2:
            self.version = 2
            if recsize != 0 or hlen < HEADER2_SIZE or len(raw) < HEADER2_SIZE:
                raise ValueError("not a v2 segment header")
            self.t0, self.seg_len, self.seq, self.records = HDR2.unpack_from(raw, HEADER_SIZE)
        else:
            raise ValueError("not a MegaOpen capture (magic %r)" % magic)
        self.header_len = hlen
        self.bitrate = [br0, br1]
        self.build = build.split(b"\0", 1)[0].decode("ascii", "replace")

    def wall(self, t_ms):
        """Unix seconds for an uptime, or None when the board had no clock."""
        if not self.wall_sec:
            return None
        return self.wall_sec + (t_ms - self.wall_at_ms) / 1000.0


class Rec:
    __slots__ = ("t", "w", "len", "kind", "data", "file", "header")

    def __init__(self, t, w, ln, kind, data, file=None, header=None):
        self.t, self.w, self.len, self.kind, self.data = t, w, ln, kind, data
        self.file, self.header = file, header

    @property
    def id(self):
        return self.w & ID_MASK

    @property
    def bus(self):
        return (self.w >> 29) & 3

    @property
    def ext(self):
        return bool(self.w & EXT_BIT)

    def payload(self):
        return self.data[:min(self.len, 8)]


class Marker:
    def __init__(self, no, text, t, file):
        self.no, self.text, self.t, self.file = no, text, t, file

    def __repr__(self):
        return "#%d %r at %.3f s" % (self.no, self.text, self.t / 1000.0)


# ---- v2 -------------------------------------------------------------------------

class _Short(Exception):
    """A record runs past the end: the segment was cut there."""


def _get_var(b, q, end):
    v, s = 0, 0
    while True:
        if q >= end or s >= 35:
            raise _Short()
        x = b[q]
        q += 1
        v |= (x & 0x7F) << s
        if not x & 0x80:
            return v, q
        s += 7


def _put_var(v):
    out = bytearray()
    while v >= 0x80:
        out.append((v & 0x7F) | 0x80)
        v >>= 7
    out.append(v)
    return bytes(out)


def _zig(v):
    v = ((v + 0x80000000) & 0xFFFFFFFF) - 0x80000000     # as int32
    return ((v << 1) ^ (v >> 31)) & 0xFFFFFFFF


def _unzig(z):
    return (z >> 1) ^ -(z & 1)


def read_segment(blob, off=0, path=None):
    """One v2 segment at blob[off:]: (header, [Rec], whole). A segment cut
    short gives the records up to its last whole one and whole=False."""
    h = Header(blob[off:off + HEADER2_SIZE])
    if h.version != 2:
        raise ValueError("not a v2 segment")
    avail = len(blob) - off
    end = off + (h.seg_len if h.seg_len and h.seg_len <= avail else avail)
    whole = bool(h.seg_len) and h.seg_len <= avail
    q = off + h.header_len
    recs = []
    if q >= end:
        return h, recs, False
    n = blob[q]
    if q + 1 + 4 * n > end:
        return h, recs, False
    q += 1
    ids = [[w, 0, bytes(8)] for w in struct.unpack_from("<%dI" % n, blob, q)]
    q += 4 * n
    t, stats_ms = h.t0, 0
    try:
        while q < end:

            b0 = blob[q]
            q += 1
            kind, full, z = b0 & 7, bool(b0 & F_FULL), b0 >> 4
            if z == DT_ESC:
                z, q = _get_var(blob, q, end)
            rt = (t + _unzig(z)) & 0xFFFFFFFF
            if kind in (FRAME, KEYFRAME, STATS):
                if q >= end:
                    raise _Short()
                if blob[q] == IDX_ESC:
                    if q + 5 > end:
                        raise _Short()
                    w = struct.unpack_from("<I", blob, q + 1)[0]
                    q += 5
                    idx = next((i for i, e in enumerate(ids) if e[0] == w), None)
                    esc = idx is None
                else:
                    idx = blob[q]
                    q += 1
                    if idx >= len(ids):
                        raise _Short()
                    w, esc = ids[idx][0], False
                if kind == STATS:
                    cnt, q = _get_var(blob, q, end)
                    ch, q = _get_var(blob, q, end)
                    ms = stats_ms
                    if full:
                        ms, q = _get_var(blob, q, end)
                    if esc and len(ids) < SEG_IDS:
                        ids.append([w, 0, bytes(8)])
                    stats_ms = ms
                    recs.append(Rec(rt, w, 8, STATS, struct.pack("<IHH", cnt & 0xFFFFFFFF, ms & 0xFFFF, ch & 0xFFFF),
                                    path, h))
                else:
                    if q >= end:
                        raise _Short()
                    if full:
                        ln = blob[q]
                        q += 1
                        if ln > 8 or q + ln > end:
                            raise _Short()
                        data = blob[q:q + ln].ljust(8, b"\0")
                        q += ln
                    else:
                        mask = blob[q]
                        q += 1
                        c = bin(mask).count("1")
                        if q + c > end:
                            raise _Short()
                        changed = blob[q:q + c]
                        q += c
                        base = ids[idx] if idx is not None else [w, 0, bytes(8)]
                        ln, data, k = base[1], bytearray(base[2]), 0
                        for i in range(8):
                            if mask >> i & 1:
                                data[i] = changed[k]
                                k += 1
                        data = bytes(data)
                    if esc and len(ids) < SEG_IDS:
                        ids.append([w, 0, bytes(8)])
                        idx = len(ids) - 1
                    if idx is not None:
                        ids[idx][1], ids[idx][2] = ln, data
                    recs.append(Rec(rt, w, ln, kind, data, path, h))
            elif kind == MARKER:
                no, q = _get_var(blob, q, end)
                if q + 3 > end:
                    raise _Short()
                total, piece, ln = blob[q], blob[q + 1], blob[q + 2]
                q += 3
                if ln > 8 or q + ln > end:
                    raise _Short()
                recs.append(Rec(rt, (no & 0xFFFF) | total << 16 | piece << 24, ln, MARKER,
                                blob[q:q + ln].ljust(8, b"\0"), path, h))
                q += ln
            elif kind in (GAP, BITRATE):
                bus = 0
                if kind == BITRATE:
                    if q >= end:
                        raise _Short()
                    bus = blob[q]
                    q += 1
                v, q = _get_var(blob, q, end)
                w = (1 if full else 0) if kind == GAP else (bus & 3) << 29
                recs.append(Rec(rt, w, 4, kind, struct.pack("<I", v & 0xFFFFFFFF) + bytes(4), path, h))
            elif kind == MODE:
                if q >= end:
                    raise _Short()
                recs.append(Rec(rt, 0, 1, MODE, bytes([blob[q]]) + bytes(7), path, h))
                q += 1
            else:
                raise _Short()        # not a kind of this version
            t = rt
    except _Short:
        whole = False
    return h, recs, whole


def read_segments(blob, path=None):
    """Segments back to back: ([header], [Rec]). Stops at one cut short."""
    headers, recs, off = [], [], 0
    while off + HEADER2_SIZE <= len(blob):
        h, r, whole = read_segment(blob, off, path)
        headers.append(h)
        recs.extend(r)
        if not whole:
            break
        off += h.seg_len
    return headers, recs


class SegWriter:
    """Encodes v1 records into v2 segments exactly as the board does
    (src/capture/CapFormat.cpp SegEncoder): same bytes for the same records
    (selftest checks GOLDEN_V2). Ids keep their index from one segment to the
    next; each segment starts with the whole table."""

    def __init__(self, session=1, boot=0, bitrate=(0, 0), mode=0, build="pack",
                 wall_sec=0, wall_at_ms=0, seg_max=SEG_MAX, seg_ms=SEG_MS):
        self.session, self.boot, self.bitrate, self.mode, self.build = session, boot, bitrate, mode, build
        self.wall_sec, self.wall_at_ms, self.seg_max, self.seg_ms = wall_sec, wall_at_ms, seg_max, seg_ms
        self.ids = []          # [w, len, data(8), seen]
        self.index = {}
        self.buf = None
        self.segments = []     # sealed segment bytes
        self.part = 0
        self.seq = 0

    def open(self, t0, seq=None, part=None):
        self.seq = self.seq + 1 if seq is None else seq
        self.part_now = self.part if part is None else part
        self.part += 1
        self.t0 = self.last = t0 & 0xFFFFFFFF
        self.recs = 0
        self.stats_known = False
        self.stats_ms = 0
        for e in self.ids:
            e[3] = False
        self.buf = bytearray(HEADER2_SIZE)
        self.buf += bytes([len(self.ids)]) + b"".join(struct.pack("<I", e[0]) for e in self.ids)
        self._patch()

    def _patch(self):
        hdr = HDR.pack(MAGIC2, HEADER2_SIZE, self.session, self.part_now & 0xFFFF, self.mode, 0,
                       self.bitrate[0], self.bitrate[1], self.boot, self.wall_sec, self.wall_at_ms,
                       5000, 1000, self.build.encode("ascii")[:23].ljust(24, b"\0"))
        self.buf[0:HEADER2_SIZE] = hdr + HDR2.pack(self.t0, len(self.buf), self.seq, self.recs)

    def seal(self):
        if self.buf is not None:
            self.segments.append(bytes(self.buf))
            self.buf = None

    def add(self, r):
        """Appends one record; seals and opens the next segment when this one
        is full (seg_max) or old (seg_ms)."""
        if self.buf is not None and ((r.t - self.t0) & 0xFFFFFFFF) >= self.seg_ms:
            self.seal()
        if self.buf is None:
            self.open(r.t)
        enc = self._encode(r)
        if len(self.buf) + 24 > self.seg_max:
            self.seal()
            self.open(r.t)
            enc = self._encode(r)
        self._commit(enc)

    def _encode(self, r):
        """(bytes, state change) of r against the current segment."""
        z = _zig(r.t - self.last)
        b0 = r.kind & 7
        out = bytearray()
        if z < DT_ESC:
            b0 |= z << 4
        else:
            b0 |= DT_ESC << 4
            out += _put_var(z)
        st = None
        if r.kind in (FRAME, KEYFRAME, STATS):
            idx = self.index.get(r.w)
            new = idx is None
            if new:
                out += bytes([IDX_ESC]) + struct.pack("<I", r.w)
                idx = len(self.ids) if len(self.ids) < SEG_IDS else None
            else:
                out.append(idx)
            if r.kind == STATS:
                cnt, ms, ch = struct.unpack("<IHH", bytes(r.data[:8]).ljust(8, b"\0"))
                out += _put_var(cnt) + _put_var(ch)
                if not self.stats_known or ms != self.stats_ms:
                    b0 |= F_FULL
                    out += _put_var(ms)
                st = ("stats", new, idx, r.w, ms)
            else:
                ln = min(r.len, 8)
                data = bytes(r.data[:ln])
                e = self.ids[idx] if idx is not None and not new else None
                if e is None or not e[3] or e[1] != ln:
                    b0 |= F_FULL
                    out += bytes([ln]) + data
                else:
                    mask, ch = 0, bytearray()
                    for i in range(ln):
                        if data[i] != e[2][i]:
                            mask |= 1 << i
                            ch.append(data[i])
                    out += bytes([mask]) + ch
                st = ("frame", new, idx, r.w, ln, data.ljust(8, b"\0"))
        elif r.kind == MARKER:
            ln = min(r.len, 8)
            out += _put_var(r.w & 0xFFFF) + bytes([(r.w >> 16) & 0xFF, (r.w >> 24) & 0xFF, ln]) + bytes(r.data[:ln])
        elif r.kind == GAP:
            if r.w == 1:
                b0 |= F_FULL
            out += _put_var(struct.unpack("<I", bytes(r.data[:4]))[0])
        elif r.kind == BITRATE:
            out += bytes([(r.w >> 29) & 3]) + _put_var(struct.unpack("<I", bytes(r.data[:4]))[0])
        elif r.kind == MODE:
            out.append(r.data[0])
        else:
            return None
        return bytes([b0]) + bytes(out), st, r.t

    def _commit(self, enc):
        if enc is None:
            return
        b, st, t = enc
        if st:
            if st[1] and st[2] is not None:
                self.index[st[3]] = st[2]
                self.ids.append([st[3], 0, bytes(8), False])
            if st[0] == "stats":
                self.stats_known, self.stats_ms = True, st[4]
            elif st[2] is not None:
                e = self.ids[st[2]]
                e[1], e[2], e[3] = st[4], st[5], True
        self.buf += b
        self.last = t & 0xFFFFFFFF
        self.recs += 1
        self._patch()


def read_file(path):
    """(header, [Rec]) for one file: v1 records, or v2 segments back to back
    (the header is the first segment's; each Rec carries its own). A torn last
    record is ignored."""
    with open(path, "rb") as f:
        blob = f.read()
    h = Header(blob)
    if h.version == 2:
        headers, recs = read_segments(blob, path)
        h.segments = len(headers)
        return h, recs
    recs = []
    off = h.header_len
    while off + REC_SIZE <= len(blob):
        t, w, ln, kind, data = REC.unpack_from(blob, off)
        recs.append(Rec(t, w, ln, kind, data, path, h))
        off += REC_SIZE
    return h, recs


def read_files(paths):
    """Headers, all records in order, and the markers reassembled."""
    headers, recs = [], []
    for p in paths:
        h, r = read_file(p)
        headers.append(h)
        recs.extend(r)
    markers = []
    i = 0
    while i < len(recs):
        r = recs[i]
        if r.kind != MARKER:
            i += 1
            continue
        no, total, idx = r.w & 0xFFFF, (r.w >> 16) & 0xFF, (r.w >> 24) & 0xFF
        pieces = 1 if total == 0 else (total + 7) // 8
        text = b""
        ok = idx == 0
        for k in range(pieces):
            if i + k >= len(recs):
                ok = False
                break
            q = recs[i + k]
            if q.kind != MARKER or (q.w & 0xFFFF) != no or ((q.w >> 24) & 0xFF) != k:
                ok = False
                break
            text += q.payload()
        if ok:
            markers.append(Marker(no, text.decode("ascii", "replace"), r.t, r.file))
            i += pieces
        else:
            i += 1
    return headers, recs, markers


def frames(recs, bus=None):
    for r in recs:
        if r.kind in (FRAME, KEYFRAME) and (bus is None or r.bus == bus):
            yield r


# ---- convert ------------------------------------------------------------------

def write_gvret(recs, out):
    """SavvyCAN's GVRET CSV: microseconds, hex id, Extended, Dir, Bus, LEN, D1..D8."""
    w = csv.writer(out, lineterminator="\n")
    w.writerow(["Time Stamp", "ID", "Extended", "Dir", "Bus", "LEN",
                "D1", "D2", "D3", "D4", "D5", "D6", "D7", "D8"])
    n = 0
    for r in frames(recs):
        d = ["%02X" % b for b in r.payload()]
        d += [""] * (8 - len(d))
        w.writerow([r.t * 1000, "%08X" % r.id, "true" if r.ext else "false", "Rx",
                    r.bus, r.len] + d)
        n += 1
    return n


def write_candump(recs, out):
    """candump -l: "(sec.usec) canN ID#DATA"; bus 0 (car) is can0, panel can1.
    The board's wall clock when it had one, else uptime."""
    n = 0
    for r in frames(recs):
        wall = r.header.wall(r.t) if r.header else None
        ts = wall if wall is not None else r.t / 1000.0
        cid = ("%08X" if r.ext else "%03X") % r.id
        out.write("(%.6f) can%d %s#%s\n" % (ts, r.bus, cid, r.payload().hex().upper()))
        n += 1
    return n


# ---- analysis -----------------------------------------------------------------

def id_table(recs, bus=None):
    """Per id word: records, lengths, per-byte change counts, frames counted by
    STATS and their span."""
    t = {}
    for r in recs:
        if bus is not None and r.bus != bus:
            continue
        if r.kind in (FRAME, KEYFRAME):
            e = t.setdefault(r.w, {"recs": 0, "lens": set(), "chg": [0] * 8, "last": None,
                                   "stat_frames": 0, "stat_ms": 0, "first": r.t, "lastT": r.t})
            e["recs"] += 1
            e["lens"].add(r.len)
            e["lastT"] = r.t
            p = r.payload()
            if e["last"] is not None:
                for i in range(min(len(p), len(e["last"]))):
                    if p[i] != e["last"][i]:
                        e["chg"][i] += 1
            e["last"] = p
        elif r.kind == STATS:
            e = t.setdefault(r.w, {"recs": 0, "lens": set(), "chg": [0] * 8, "last": None,
                                   "stat_frames": 0, "stat_ms": 0, "first": r.t, "lastT": r.t})
            cnt, ms, _ch = struct.unpack("<IHH", r.data)
            e["stat_frames"] += cnt
            e["stat_ms"] += ms
    return t


def cmd_ids(a):
    _, recs, _ = read_files(a.files)
    t = id_table(recs, a.bus)
    out = sys.stdout
    out.write("bus  id        rate/s  len   recs   changes per byte (b0..b7)\n")
    for w in sorted(t, key=lambda w: (split_word(w)[1], split_word(w)[0])):
        e = t[w]
        cid, bus, ext = split_word(w)
        if e["stat_ms"]:
            rate = e["stat_frames"] * 1000.0 / e["stat_ms"]
        else:
            span = (e["lastT"] - e["first"]) / 1000.0
            rate = (e["recs"] - 1) / span if span > 0 else 0.0
        lens = "/".join(str(x) for x in sorted(e["lens"])) or "-"
        out.write("%-4d %-9s %7.1f  %-5s %6d   %s\n" % (
            bus, ("%08X" if ext else "%03X") % cid, rate, lens, e["recs"],
            " ".join("%4d" % c for c in e["chg"])))
    return 0


def find_marker(markers, key):
    if key.lstrip("#").isdigit():
        no = int(key.lstrip("#"))
        for m in markers:
            if m.no == no:
                return m
    for m in markers:
        if key.lower() in m.text.lower():
            return m
    raise SystemExit("no marker %r (have: %s)" % (key, ", ".join(repr(m) for m in markers) or "none"))


def state_at(recs, t, bus=None):
    """Each id's last written data at uptime t (the stream is in time order)."""
    s = {}
    for r in frames(recs, bus):
        if r.t > t:
            break
        s[r.w] = r.payload()
    return s


def cmd_between(a):
    _, recs, markers = read_files(a.files)
    ma, mb = find_marker(markers, a.A), find_marker(markers, a.B)
    if mb.t < ma.t:
        ma, mb = mb, ma
    sa, sb = state_at(recs, ma.t, a.bus), state_at(recs, mb.t, a.bus)
    # How often each byte moved in between: a byte that moved once is a
    # better candidate than one that moves all the time.
    moves = {}
    last = dict(sa)
    for r in frames(recs, a.bus):
        if r.t <= ma.t:
            continue
        if r.t > mb.t:
            break
        p = r.payload()
        prev = last.get(r.w)
        if prev is not None:
            m = moves.setdefault(r.w, [0] * 8)
            for i in range(min(len(p), len(prev))):
                if p[i] != prev[i]:
                    m[i] += 1
        last[r.w] = p
    out = sys.stdout
    out.write("between %r and %r (%.1f s)\n" % (ma, mb, (mb.t - ma.t) / 1000.0))
    out.write("bus  id        byte  at A  at B  moves\n")
    n = 0
    for w in sorted(set(sa) | set(sb), key=lambda w: (split_word(w)[1], split_word(w)[0])):
        pa, pb = sa.get(w), sb.get(w)
        cid, bus, ext = split_word(w)
        name = ("%08X" if ext else "%03X") % cid
        if pa is None or pb is None:
            out.write("%-4d %-9s  (only %s)\n" % (bus, name, "after A" if pa is None else "before B"))
            continue
        for i in range(min(len(pa), len(pb))):
            if pa[i] != pb[i]:
                out.write("%-4d %-9s %4d   %02X    %02X  %5d\n" % (bus, name, i, pa[i], pb[i],
                                                                 moves.get(w, [0] * 8)[i]))
                n += 1
    out.write("%d byte(s) changed\n" % n)
    return 0


def byte_series(recs, bus=None):
    """{(w, i): [(t, value), ...]} of every byte's value at each change."""
    series = {}
    last = {}
    for r in frames(recs, bus):
        p = r.payload()
        prev = last.get(r.w)
        for i, v in enumerate(p):
            if prev is None or i >= len(prev) or prev[i] != v:
                series.setdefault((r.w, i), []).append((r.t, v))
        last[r.w] = p
    return series


def slow_candidates(recs, bus=None, min_span_s=30.0):
    """Bytes that change seldom and in one direction: fuel, odometer, coolant.
    Returns [(score, w, i, n_changes, first, last, span_s, direction)], best first."""
    out = []
    for (w, i), s in byte_series(recs, bus).items():
        if len(s) < 3:
            continue
        span = (s[-1][0] - s[0][0]) / 1000.0
        if span < min_span_s:
            continue
        deltas = [b[1] - a[1] for a, b in zip(s, s[1:])]
        # One wrap of an 8-bit counter (odometer low byte) still counts as one way.
        deltas = [d - 256 if d > 128 else d + 256 if d < -128 else d for d in deltas]
        up = sum(1 for d in deltas if d > 0)
        down = sum(1 for d in deltas if d < 0)
        mono = max(up, down) / float(len(deltas))
        small = sum(1 for d in deltas if abs(d) <= 4) / float(len(deltas))
        per_min = len(s) / (span / 60.0)
        if mono < 0.8 or small < 0.8 or per_min > 30:
            continue
        score = mono * small * min(1.0, len(s) / 5.0)
        out.append((score, w, i, len(s), s[0][1], s[-1][1], span, "up" if up >= down else "down"))
    out.sort(key=lambda x: -x[0])
    return out


def cmd_slow(a):
    _, recs, _ = read_files(a.files)
    c = slow_candidates(recs, a.bus, a.min_span)
    sys.stdout.write("bus  id        byte  changes  first  last  span_s  dir   score\n")
    for score, w, i, n, v0, v1, span, d in c:
        cid, bus, ext = split_word(w)
        sys.stdout.write("%-4d %-9s %4d  %7d   %02X     %02X  %6.0f  %-4s  %.2f\n" % (
            bus, ("%08X" if ext else "%03X") % cid, i, n, v0, v1, span, d, score))
    sys.stdout.write("%d candidate(s)\n" % len(c))
    return 0


def track(recs, cid, byte, bus=None):
    """[(t_ms, value)] of one byte, every record that carries it (keyframes too)."""
    out = []
    for r in frames(recs, bus):
        if r.id == cid and byte < r.len:
            out.append((r.t, r.data[byte]))
    return out


# Bus 2 is the head unit's UART (src/main.cpp huRecord): a frame of up to 64
# bytes in records of 8, id = cmd | dir << 8 | piece << 11 | len << 15, dir 0 T
# (to the head unit), 1 V (car data to it), 2 R (from it), 3 J (bytes outside
# a frame). Change-only works per piece, so a frame is the newest of its pieces.
HU_DIR = "TVRJ"


def hu_frames(recs):
    pieces, seen = {}, {}
    for r in recs:
        if r.kind not in (FRAME, KEYFRAME) or r.bus != 2:
            continue
        cmd, d, k, n = r.id & 0xFF, (r.id >> 8) & 3, (r.id >> 11) & 0xF, (r.id >> 15) & 0x7F
        key = (d, cmd, n)
        cur = pieces.setdefault(key, bytearray(n))
        cur[k * 8:k * 8 + r.len] = r.payload()
        got = seen.setdefault(key, set())
        got.add(k)
        # Once every piece has been seen, any piece that changes is a new frame.
        if len(got) >= max(1, (n + 7) // 8):
            yield r.t, HU_DIR[d], cmd, bytes(cur)


def cmd_hu(a):
    _, recs, _ = read_files(a.files)
    for t, d, cmd, data in hu_frames(recs):
        if d == "J":
            print("%10.3f J  %s" % (t / 1000.0, data.hex(" ").upper()))
        else:
            print("%10.3f %s  %02X %s" % (t / 1000.0, d, cmd, data.hex(" ").upper()))
    return 0


def cmd_track(a):
    _, recs, markers = read_files(a.files)
    cid = int(a.ID, 16)
    rows = track(recs, cid, a.BYTE, a.bus)
    out = open(a.o, "w", newline="") if a.o else sys.stdout
    try:
        w = csv.writer(out, lineterminator="\n")
        w.writerow(["t_s", "value", "marker"])
        mi = 0
        ms = sorted(markers, key=lambda m: m.t)
        for t, v in rows:
            label = []
            while mi < len(ms) and ms[mi].t <= t:
                label.append(ms[mi].text)
                mi += 1
            w.writerow(["%.3f" % (t / 1000.0), v, " | ".join(label)])
    finally:
        if a.o:
            out.close()
    if a.o:
        sys.stderr.write("%d row(s) -> %s\n" % (len(rows), a.o))
    return 0


def cmd_info(a):
    for p in a.files:
        h, recs = read_file(p)
        kinds = {}
        for r in recs:
            kinds[KIND_NAMES.get(r.kind, str(r.kind))] = kinds.get(KIND_NAMES.get(r.kind, str(r.kind)), 0) + 1
        span = (recs[-1].t - recs[0].t) / 1000.0 if recs else 0
        size = os.path.getsize(p)
        sys.stdout.write("%s: v%d%s session %04d part %d boot %d mode %s build %s; %d bytes, %.2f B/rec\n" % (
            os.path.basename(p), h.version, " (%d segment(s))" % h.segments if h.version == 2 else "",
            h.session, h.part, h.boot, "full" if h.mode else "change-only", h.build, size,
            size / float(len(recs)) if recs else 0.0))
        sys.stdout.write("  bitrate car %d panel %d; wall %s; %d records over %.1f s: %s\n" % (
            h.bitrate[0], h.bitrate[1], h.wall_sec or "unknown", len(recs), span,
            ", ".join("%s=%d" % kv for kv in sorted(kinds.items()))))
    _, _, markers = read_files(a.files)
    for m in markers:
        sys.stdout.write("  marker %r\n" % m)
    return 0


def cmd_convert(a):
    _, recs, _ = read_files(a.files)
    out = open(a.o, "w", newline="") if a.o else sys.stdout
    try:
        n = (write_gvret if a.to == "gvret" else write_candump)(recs, out)
    finally:
        if a.o:
            out.close()
    if a.o:
        sys.stderr.write("%d frame(s) -> %s\n" % (n, a.o))
    return 0


def pack(paths):
    """The records of FILE... (v1 or v2) as v2 segments: (bytes, records)."""
    headers, recs, _ = read_files(paths)
    h = headers[0]
    w = SegWriter(session=h.session, boot=h.boot, bitrate=tuple(h.bitrate), mode=h.mode,
                  build=h.build, wall_sec=h.wall_sec, wall_at_ms=h.wall_at_ms)
    for r in recs:
        w.add(r)
    w.seal()
    return b"".join(w.segments), recs


def cmd_pack(a):
    blob, recs = pack(a.files)
    before = sum(os.path.getsize(p) for p in a.files)
    with open(a.o, "wb") as f:
        f.write(blob)
    _, back = read_segments(blob)
    assert len(back) == len(recs), (len(back), len(recs))
    n = max(1, len(recs))
    sys.stdout.write("%d records: %d bytes (%.2f B/rec) -> %s %d bytes (%.2f B/rec), %.1fx smaller\n" % (
        len(recs), before, before / float(n), a.o, len(blob), len(blob) / float(n),
        before / float(max(1, len(blob)))))
    return 0


# ---- writing (selftest, and anyone who wants to fake a capture) -------------------

class Writer:
    """Writes a capture the way the board does, change-only logic included
    (src/capture/CapFormat.cpp: ChangeFilter)."""

    def __init__(self, f, session=1, part=0, mode=0, bitrate=(500000, 0), boot=1,
                 wall_sec=0, wall_at_ms=0, build="selftest", keyframe_ms=5000, stats_ms=1000):
        self.f, self.mode, self.keyframe_ms, self.stats_ms = f, mode, keyframe_ms, stats_ms
        f.write(HDR.pack(MAGIC, HEADER_SIZE, session, part, mode, REC_SIZE, bitrate[0], bitrate[1],
                         boot, wall_sec, wall_at_ms, keyframe_ms, stats_ms,
                         build.encode("ascii")[:23].ljust(24, b"\0")))
        self.table = {}
        self.stats_at = None
        self.marker_no = 0

    def rec(self, t, w, kind, data=b""):
        self.f.write(REC.pack(t & 0xFFFFFFFF, w, len(data), kind, bytes(data).ljust(8, b"\0")))

    def frame(self, t, cid, data, bus=0, ext=False):
        w = id_word(cid, ext, bus)
        data = bytes(data)
        if self.mode == 1:
            self.rec(t, w, FRAME, data)
            return
        if self.stats_at is None:
            self.stats_at = t
        e = self.table.get(w)
        if e is None:
            e = self.table[w] = {"data": None, "at": 0, "count": 0, "chg": 0}
        changed = e["data"] != data
        e["count"] += 1
        if changed:
            e["chg"] += 1
        kind = FRAME if changed else KEYFRAME if t - e["at"] >= self.keyframe_ms else None
        if kind is not None:
            e["data"], e["at"] = data, t
            self.rec(t, w, kind, data)
        if t - self.stats_at >= self.stats_ms:
            self.stats(t)

    def stats(self, t):
        ms = min(0xFFFF, t - self.stats_at)
        self.stats_at = t
        for w, e in self.table.items():
            if e["count"]:
                self.rec(t, w, STATS, struct.pack("<IHH", e["count"], ms, min(e["chg"], 0xFFFF)))
                e["count"] = e["chg"] = 0

    def mark(self, t, text):
        self.marker_no += 1
        b = text.encode("ascii")[:120]
        pieces = max(1, (len(b) + 7) // 8)
        for i in range(pieces):
            self.f.write(REC.pack(t, self.marker_no | (len(b) << 16) | (i << 24),
                                  len(b[i * 8:i * 8 + 8]), MARKER, b[i * 8:i * 8 + 8].ljust(8, b"\0")))
        return self.marker_no


# src/capture/CapFormat.cpp encodes these records to these bytes
# (test/test_capture golden_v2.inc, test_v2_bytes_are_the_golden_ones).
GOLDEN_V2 = bytes.fromhex(
    "4D4F434150325000050000000700000020A10700000000000300000000000000"
    "000000008813E803676F6C64656E000000000000000000000000000000000000"
    "E80300009D000000070000000D0000000008FF2301000003010203A0000209F1"
    "904E00000B001401E80798FF10F1DAB801AA2800020109220109000868692074"
    "6865726502010901012124030CB009250190A10F0601F3D20F00AC0200")


def golden_recs():
    w123 = id_word(0x123, False, 0)
    st = lambda t, c, ms, ch: Rec(t, w123, 8, STATS, struct.pack("<IHH", c, ms, ch))
    fr = lambda t, w, d, k=FRAME: Rec(t, w, len(d), k, bytes(d).ljust(8, b"\0"))
    text = b"hi there!"
    mk = [Rec(6002, 1 | len(text) << 16 | i << 24, len(text[i * 8:i * 8 + 8]), MARKER,
              text[i * 8:i * 8 + 8].ljust(8, b"\0")) for i in range(2)]
    u32 = lambda t, k, w, v, n=4: Rec(t, w, n, k, struct.pack("<I", v) + bytes(4))
    return [fr(1000, w123, [1, 2, 3]), fr(1005, w123, [1, 9, 3]), fr(6005, w123, [1, 9, 3], KEYFRAME),
            st(6005, 20, 1000, 1), fr(6000, id_word(0x18DAF110, True, 1), [0xAA]), fr(6001, w123, [1, 9]),
            mk[0], mk[1], u32(6003, GAP, 0, 3), u32(6003, GAP, 1, 1200),
            u32(6004, BITRATE, 1 << 29, 250000), u32(6004, MODE, 0, 1, 1), st(7005, 300, 1000, 0)]


def same(a, b):
    return (a.t, a.w, a.kind, a.len, bytes(a.data[:min(a.len, 8)])) == \
           (b.t, b.w, b.kind, b.len, bytes(b.data[:min(b.len, 8)]))


def selftest_v2(tmp):
    # The board's bytes, byte for byte.
    w = SegWriter(session=5, boot=3, bitrate=(500000, 0), build="golden")
    w.open(1000, seq=7, part=7)
    recs = golden_recs()
    for r in recs:
        w._commit(w._encode(r))
    w.seal()
    assert w.segments[0] == GOLDEN_V2, w.segments[0].hex().upper()
    h, back, whole = read_segment(GOLDEN_V2)
    assert whole and h.seq == 7 and h.records == len(recs) and h.seg_len == len(GOLDEN_V2)
    assert all(same(a, b) for a, b in zip(recs, back)) and len(back) == len(recs)
    # Cut anywhere: a prefix, never a wrong record.
    last = 0
    for cut in range(HEADER2_SIZE, len(GOLDEN_V2) + 1):
        _, got, _ = read_segment(GOLDEN_V2[:cut])
        assert len(got) >= last and all(same(a, b) for a, b in zip(recs, got)), cut
        last = len(got)
    assert last == len(recs)


def cmd_selftest(_a):
    tmp = tempfile.mkdtemp(prefix="can_dump_")
    selftest_v2(tmp)
    p1 = os.path.join(tmp, "0001.bin")
    p2 = os.path.join(tmp, "0002.bin")
    # Two files of one session: a 10-minute drive at 20 Hz per id.
    with open(p1, "wb") as f1, open(p2, "wb") as f2:
        w1 = Writer(f1, session=1, part=0, wall_sec=1760000000, wall_at_ms=0)
        w2 = Writer(f2, session=1, part=1, wall_sec=1760000000, wall_at_ms=0)
        fuel = 200
        odo = 0
        for t in range(0, 600000, 50):
            w = w1 if t < 300000 else w2
            if t % 20000 == 0 and t:
                fuel -= 1                         # slowly down
            if t % 7000 == 0 and t:
                odo = (odo + 1) & 0xFF            # slowly up
            w.frame(t, 0x5DE, [0x10, fuel, 0, 0, 0, 0, 0, odo])
            w.frame(t, 0x181, [(t // 50) & 0xFF, 0x22])            # a counter: fast
            w.frame(t, 0x18DAF110, [0x02, 0x01, 0x0C], bus=0, ext=True)
            w.frame(t, 0x3B7, [0x01, 0x00], bus=1)                  # panel bus, constant
            # Marker numbers run on across files, as on the board.
            w.marker_no = max(w1.marker_no, w2.marker_no)
            if t == 100000:
                w.mark(t, "fuel 3/4")
            if t == 400000:
                w.mark(t, "after a long stretch of road")
        w2.rec(600000, 0, GAP, struct.pack("<I", 3))

    files = [p1, p2]
    h, recs, markers = read_files(files)
    assert [m.text for m in markers] == ["fuel 3/4", "after a long stretch of road"], markers
    assert [m.no for m in markers] == [1, 2], markers
    assert h[1].part == 1 and h[0].bitrate[0] == 500000

    # ids: rates come from STATS (20/s), though most repeats were not written.
    t = id_table(recs)
    e = t[id_word(0x181, False, 0)]
    rate = e["stat_frames"] * 1000.0 / e["stat_ms"]
    assert 19.5 < rate < 20.5, rate
    e = t[id_word(0x5DE, False, 0)]
    assert 19.5 < e["stat_frames"] * 1000.0 / e["stat_ms"] < 20.5, e
    e = t[id_word(0x3B7, False, 1)]
    assert e["recs"] < 200 and e["stat_frames"] > 10000, e   # change-only + keyframes

    # convert, both ways.
    g = io.StringIO()
    n = write_gvret(recs, g)
    rows = list(csv.reader(io.StringIO(g.getvalue())))
    assert rows[0][0] == "Time Stamp" and len(rows) == n + 1
    assert any(r[1] == "18DAF110" and r[2] == "true" for r in rows[1:])
    c = io.StringIO()
    write_candump(recs, c)
    first = c.getvalue().splitlines()[0]
    assert first.startswith("(1760000000.000000) can0 "), first

    # between: fuel byte 1 and the odometer byte 7 of 0x5DE moved.
    sa = state_at(recs, markers[0].t)
    sb = state_at(recs, markers[1].t)
    w5 = id_word(0x5DE, False, 0)
    assert sa[w5][1] != sb[w5][1] and sa[w5][7] != sb[w5][7]
    assert sa[id_word(0x3B7, False, 1)] == sb[id_word(0x3B7, False, 1)]

    # slow: fuel (down) and odometer (up) found; the counter is not.
    c = slow_candidates(recs)
    found = {(x[1], x[2]): x[7] for x in c}
    assert found.get((w5, 1)) == "down", c[:5]
    assert found.get((w5, 7)) == "up", c[:5]
    assert (id_word(0x181, False, 0), 0) not in found

    # track: one value per written record, falling.
    tr = track(recs, 0x5DE, 1)
    assert tr[0][1] == 200 and tr[-1][1] < 200

    # Every command through the CLI as well, so the argument wiring is covered.
    old = sys.stdout
    try:
        for argv in (["info"] + files,
                     ["convert"] + files + ["--to", "gvret", "-o", os.path.join(tmp, "o.csv")],
                     ["convert"] + files + ["--to", "candump", "-o", os.path.join(tmp, "o.log")],
                     ["ids"] + files,
                     ["between", "1", "long stretch"] + files,
                     ["slow"] + files,
                     ["track", "5DE", "1"] + files + ["-o", os.path.join(tmp, "t.csv")]):
            sys.stdout = io.StringIO()
            rc = main(argv)
            assert rc == 0, argv
    finally:
        sys.stdout = old
    with open(os.path.join(tmp, "t.csv")) as f:
        assert f.readline().strip() == "t_s,value,marker"

    # v1 -> v2: the same records, read back by every command; segments of
    # 64 kB / 60 s, each one readable alone.
    p3 = os.path.join(tmp, "packed.bin")
    sys.stdout = io.StringIO()
    try:
        assert main(["pack"] + files + ["-o", p3]) == 0
        line = sys.stdout.getvalue()
    finally:
        sys.stdout = old
    h3, recs3, markers3 = read_files([p3])
    assert h3[0].version == 2 and h3[0].segments > 1, h3[0].__dict__
    assert len(recs3) == len(recs) and all(same(a, b) for a, b in zip(recs, recs3))
    assert [m.text for m in markers3] == [m.text for m in markers]
    with open(p3, "rb") as f:
        blob = f.read()
    hs, _ = read_segments(blob)
    second = blob[hs[0].seg_len:hs[0].seg_len + hs[1].seg_len]
    _, alone, whole = read_segment(second)
    assert whole and alone and all(same(a, b) for a, b in zip(recs[hs[0].records:], alone))
    try:
        for argv in (["info", p3], ["ids", p3], ["slow", p3], ["between", "1", "long stretch", p3]):
            sys.stdout = io.StringIO()
            assert main(argv) == 0, argv
    finally:
        sys.stdout = old
    shutil.rmtree(tmp, ignore_errors=True)
    print("selftest OK (%d records, %d markers; v2 golden, %s)" % (len(recs), len(markers), line.strip()))
    return 0


# ---- CLI ----------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("info", help="headers, record counts, markers")
    s.add_argument("files", nargs="+")
    s.set_defaults(fn=cmd_info)

    s = sub.add_parser("convert", help="to SavvyCAN GVRET CSV or candump -l text")
    s.add_argument("files", nargs="+")
    s.add_argument("--to", choices=("gvret", "candump"), default="gvret")
    s.add_argument("-o", help="output file (default stdout)")
    s.set_defaults(fn=cmd_convert)

    s = sub.add_parser("ids", help="per-id rate, length, byte-change counts")
    s.add_argument("files", nargs="+")
    s.add_argument("--bus", type=int)
    s.set_defaults(fn=cmd_ids)

    s = sub.add_parser("between", help="bytes that changed between two markers")
    s.add_argument("A")
    s.add_argument("B")
    s.add_argument("files", nargs="+")
    s.add_argument("--bus", type=int)
    s.set_defaults(fn=cmd_between)

    s = sub.add_parser("slow", help="slowly monotonic bytes (fuel, odometer)")
    s.add_argument("files", nargs="+")
    s.add_argument("--bus", type=int)
    s.add_argument("--min-span", type=float, default=30.0, help="seconds a byte must be watched")
    s.set_defaults(fn=cmd_slow)

    s = sub.add_parser("track", help="one byte over time, as CSV")
    s.add_argument("ID", help="hex id, e.g. 5DE")
    s.add_argument("BYTE", type=int, help="byte index 0..7")
    s.add_argument("files", nargs="+")
    s.add_argument("--bus", type=int)
    s.add_argument("-o", help="output CSV (default stdout)")
    s.set_defaults(fn=cmd_track)

    s = sub.add_parser("hu", help="the head unit's UART frames (bus 2)")
    s.add_argument("files", nargs="+")
    s.set_defaults(fn=cmd_hu)

    s = sub.add_parser("pack", help="re-encode FILE... (v1 or v2) as v2 segments; bytes per record")
    s.add_argument("files", nargs="+")
    s.add_argument("-o", required=True, help="output v2 file")
    s.set_defaults(fn=cmd_pack)

    s = sub.add_parser("selftest", help="write a synthetic capture and run every command on it")
    s.set_defaults(fn=cmd_selftest)

    a = ap.parse_args(argv)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
