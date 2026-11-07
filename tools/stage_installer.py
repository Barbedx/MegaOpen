#!/usr/bin/env python3
"""Stage the web installer (GitHub Pages) from the published releases.

    python tools/stage_installer.py [out_dir]      (default: site/)

Copies installer/ and, for the Latest release and the newest pre-release
when it is newer than Latest, downloads every megaopen-<env>-X.Y.Z-factory.bin
plus SHA256SUMS, checks the sums and writes an ESP Web Tools manifest per env:

    site/fw/<tag>/<env>/manifest.json + the factory.bin next to it
    site/channels.json   what the page offers (channel -> tag, version, envs)

The bins are copied into the site because GitHub's release downloads carry no
CORS header, so a browser on github.io cannot fetch them from Releases.
Needs the gh CLI (GH_TOKEN in CI).
"""
import hashlib
import json
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "installer"

# env -> (chip family for ESP Web Tools, board name shown on the page)
ENVS = {
    "t2can": ("ESP32-S3", "LilyGO T-2CAN"),
    "t2can_usbhid": ("ESP32-S3", "LilyGO T-2CAN, USB keyboard"),
    "direct": ("ESP32", "ESP32 DevKit (no panel)"),
}
FACTORY = re.compile(r"^megaopen-(?P<env>[a-z0-9_]+)-(?P<ver>\d+\.\d+\.\d+)-factory\.bin$")


def gh(*args):
    return subprocess.run(["gh", *args], check=True, capture_output=True, text=True).stdout


def releases():
    out = gh("release", "list", "-L", "40", "--json", "tagName,isLatest,isPrerelease,isDraft,publishedAt")
    return [r for r in json.loads(out) if not r["isDraft"]]


def vkey(tag):
    m = re.match(r"^v(\d+)\.(\d+)\.(\d+)(?:-rc(\d+))?$", tag)
    if not m:
        return None
    a, b, c, rc = m.groups()
    # a release sorts after all its rcs
    return (int(a), int(b), int(c), int(rc) if rc else 1 << 30)


def pick(rels):
    latest = next((r for r in rels if r["isLatest"]), None)
    if latest is None:
        sys.exit("no Latest release")
    chans = [("stable", latest["tagName"])]
    pre = [r for r in rels if r["isPrerelease"] and vkey(r["tagName"])]
    if pre:
        newest = max(pre, key=lambda r: vkey(r["tagName"]))
        if vkey(newest["tagName"]) > vkey(latest["tagName"]):
            chans.append(("rc", newest["tagName"]))
    return chans


PT_OFFSET = 0x8000  # where the bootloader looks for the partition table


def partitions(img):
    """(type, subtype, offset, size, label) of every entry in the image's table."""
    out = []
    for i in range(PT_OFFSET, PT_OFFSET + 0xC00, 32):
        e = img[i:i + 32]
        if e[:2] != b"\xaa\x50":
            break
        out.append((e[2], e[3], int.from_bytes(e[4:8], "little"), int.from_bytes(e[8:12], "little"),
                    e[12:28].split(b"\0")[0].decode()))
    return out


def split_around_nvs(img, name, d):
    """Write the factory image as two parts that leave the nvs partition alone.

    The merged image pads the gap between the partition table and otadata with
    0xFF, i.e. over nvs: flashed whole at 0x0 it wipes the WiFi and every
    setting even with "Erase" unticked. The two parts are the image's own
    bytes, before and after nvs.
    """
    nvs = [p for p in partitions(img) if p[0] == 1 and p[1] == 2]
    if len(nvs) != 1:
        sys.exit(f"{name}: expected one nvs partition in the table, found {len(nvs)}")
    _, _, off, size, _ = nvs[0]
    end = off + size
    if not PT_OFFSET < off < end < len(img):
        sys.exit(f"{name}: nvs {off:#x}+{size:#x} is not inside the image")
    stem = name[:-len(".bin")]
    (d / f"{stem}.boot.bin").write_bytes(img[:off])
    (d / f"{stem}.app.bin").write_bytes(img[end:])
    return [{"path": f"{stem}.boot.bin", "offset": 0},
            {"path": f"{stem}.app.bin", "offset": end}]


def stage_tag(tag, out):
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        gh("release", "download", tag, "-D", str(tmp), "-p", "*-factory.bin", "-p", "SHA256SUMS")
        sums = {}
        for line in (tmp / "SHA256SUMS").read_text().splitlines():
            h, _, name = line.strip().partition("  ")
            sums[name.lstrip("*")] = h
        envs = {}
        for f in sorted(tmp.glob("*-factory.bin")):
            m = FACTORY.match(f.name)
            if not m or m["env"] not in ENVS:
                print(f"  skip {f.name}")
                continue
            h = hashlib.sha256(f.read_bytes()).hexdigest()
            if sums.get(f.name) != h:
                sys.exit(f"{tag}/{f.name}: sha256 {h} does not match SHA256SUMS")
            env, ver = m["env"], m["ver"]
            chip, board = ENVS[env]
            d = out / "fw" / tag / env
            d.mkdir(parents=True, exist_ok=True)
            parts = split_around_nvs(f.read_bytes(), f.name, d)
            manifest = {
                "name": f"MegaOpen ({board})",
                "version": tag.lstrip("v"),
                "new_install_prompt_erase": True,
                "builds": [{"chipFamily": chip, "parts": parts}],
            }
            (d / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
            envs[env] = {"board": board, "chip": chip, "file": f.name, "sha256": h,
                         "size": f.stat().st_size, "manifest": f"fw/{tag}/{env}/manifest.json"}
            print(f"  {tag} {env}: {f.name} ({f.stat().st_size} bytes)")
        if not envs:
            sys.exit(f"{tag}: no factory images")
        return envs


def main():
    out = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / "site").resolve()
    if out.exists():
        shutil.rmtree(out)
    shutil.copytree(SRC, out)
    rels = releases()
    channels = []
    for chan, tag in pick(rels):
        print(f"{chan}: {tag}")
        channels.append({"channel": chan, "tag": tag, "version": tag.lstrip("v"),
                         "url": f"https://github.com/Barbedx/MegaOpen/releases/tag/{tag}",
                         "envs": stage_tag(tag, out)})
    (out / "channels.json").write_text(json.dumps({"channels": channels}, indent=2) + "\n")
    (out / ".nojekyll").write_text("")
    print(f"staged {out}")


if __name__ == "__main__":
    main()
