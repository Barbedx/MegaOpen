"""Reassemble the AFFA panel-bus messages in a CAN capture CSV and print them with their ASCII.

    python tools/affa_capture.py "<capture.csv>" [id,id,...]

The CSV is the analyser's export (`Time Stamp,ID,Extended,Dir,Bus,LEN,D1..D8`), as the
OEM captures in docs/nav-text.md. Frames are ISO-TP style: `0N` a single frame of N bytes,
`1N LL` a first frame with a 12-bit length, `2N` a consecutive frame, `3x` flow control
(not printed). The optional id list (hex, no 0x) keeps the output to those ids; the first
line always says how many frames each id had.
"""
import collections
import csv
import sys


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    only = {int(x, 16) for x in sys.argv[2].split(",")} if len(sys.argv) > 2 else None
    rows = list(csv.reader(open(path, encoding="utf-8", errors="replace")))
    out = []
    pend = {}   # id -> [declared length, bytes so far]
    counts = collections.Counter()
    for r in rows[1:]:
        if len(r) < 7 or not r[1].strip():
            continue
        try:
            ts = int(r[0]); cid = int(r[1], 16); n = int(r[5])
        except ValueError:
            continue
        data = [int(x, 16) for x in r[6:6 + n] if x.strip()]
        counts[cid] += 1
        if (only and cid not in only) or not data:
            continue
        pci = data[0] >> 4
        if pci == 0:
            ln = data[0] & 0x0F
            out.append((ts, cid, "S", data[1:1 + ln] if ln else data[1:]))
        elif pci == 1:
            ln = ((data[0] & 0x0F) << 8) | data[1]
            pend[cid] = [ln, list(data[2:])]
        elif pci == 2 and cid in pend:
            pend[cid][1].extend(data[1:])
            if len(pend[cid][1]) >= pend[cid][0]:
                ln, b = pend.pop(cid)
                out.append((ts, cid, "M", b[:ln]))
        elif pci == 3:
            pass
        else:
            out.append((ts, cid, "?", data))
    print("frames per id:", ", ".join("%03X:%d" % kv for kv in sorted(counts.items())))
    for ts, cid, kind, b in out:
        print("%9d %03X %s len %3d | %s" % (ts, cid, kind, len(b), " ".join("%02X" % x for x in b)))
        if any(32 < x < 127 for x in b):
            print("%27s| %s" % ("", "".join(chr(x) if 32 <= x < 127 else "." for x in b)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
