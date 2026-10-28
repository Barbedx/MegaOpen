"""The icons, from one picture: art/icon.webp (square, a rounded plate on black).

    python tools/gen_icons.py      # needs Pillow; run again after art/icon.webp changes

Writes, and the outputs are committed (the builds need no Pillow):
  web/icon.png                               the page's favicon, 48 px, 64 colours; tools/gen_pages.py
                                             puts it into web/index.html's <!--ICON--> as a data URI
  android/.../mipmap-<dpi>/ic_launcher.png   MegaOpen Companion, Android 7 (no adaptive icons):
                                             the plate with its corners cut round
  android/.../drawable-nodpi/ic_launcher_fg.png    Android 8+: the glowing drawing alone,
  android/.../drawable-nodpi/ic_launcher_mono.png  in colour and as a white mask (themed icons),
                                             on the plate's colour (values/colors.xml icon_bg)

The drawing is lifted off its dark plate by brightness: alpha from how far a
pixel is above the plate, its colour un-mixed from the plate. A launcher masks
an adaptive icon to its own shape (circle, squircle); the drawing is scaled so
its corner nodes stay inside the circle."""
import os
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "art", "icon.webp")
RES = os.path.join(ROOT, "android", "app", "src", "main", "res")

PLATE = (20, 10, 2)          # the plate's colour = icon_bg in values/colors.xml
EDGE = (24, 24, 1230, 1230)  # the plate's square in the source picture
RADIUS = 0.175               # its corner radius, of its side
DRAWING = (100, 100, 1154, 1100)   # the drawing and its glow
DPI = {"mdpi": 48, "hdpi": 72, "xhdpi": 96, "xxhdpi": 144, "xxxhdpi": 192}
FG = 432                     # 108 dp at xxxhdpi; the launcher scales it down
FIT = 0.58                   # the drawing's box, of the 108 dp layer: its corner nodes inside the 72 dp circle a launcher shows


def rounded(im, size):
    """The plate at `size`, its corners transparent."""
    im = im.crop(EDGE).resize((size * 4, size * 4), Image.LANCZOS)
    mask = Image.new("L", im.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, im.size[0] - 1, im.size[1] - 1), radius=int(RADIUS * im.size[0]), fill=255)
    im.putalpha(mask)
    return im.resize((size, size), Image.LANCZOS)


def lifted(im):
    """The drawing off the plate: RGBA, transparent where the plate shows."""
    out = Image.new("RGBA", im.size)
    src, dst = im.load(), out.load()
    for y in range(im.size[1]):
        for x in range(im.size[0]):
            p = src[x, y]
            a = max(0.0, min(1.0, (max(p) - max(PLATE)) / (255 - max(PLATE))))
            if a <= 0.02:
                dst[x, y] = (0, 0, 0, 0)
                continue
            c = tuple(max(0, min(255, round((p[i] - PLATE[i] * (1 - a)) / a))) for i in range(3))
            dst[x, y] = c + (round(a * 255),)
    return out


def layer(drawing):
    """The drawing centred on a transparent 108 dp layer."""
    w, h = drawing.size
    s = FIT * FG / max(w, h)
    d = drawing.resize((round(w * s), round(h * s)), Image.LANCZOS)
    out = Image.new("RGBA", (FG, FG), (0, 0, 0, 0))
    out.paste(d, ((FG - d.size[0]) // 2, (FG - d.size[1]) // 2), d)
    return out


def save(im, *path, colors=0):
    """colors: a palette PNG that small (the favicon is in the firmware's page, base64)."""
    p = os.path.join(*path)
    if colors:
        im = im.quantize(colors, method=Image.Quantize.FASTOCTREE)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    im.save(p, optimize=True)
    print("gen_icons: %s, %d B" % (os.path.relpath(p, ROOT), os.path.getsize(p)))


def main():
    im = Image.open(SRC).convert("RGB")
    save(rounded(im, 48), ROOT, "web", "icon.png", colors=64)
    for dpi, px in DPI.items():
        save(rounded(im, px), RES, "mipmap-" + dpi, "ic_launcher.png")
    fg = layer(lifted(im.crop(DRAWING)))
    save(fg, RES, "drawable-nodpi", "ic_launcher_fg.png")
    mono = Image.new("RGBA", fg.size, (255, 255, 255, 0))
    mono.putalpha(fg.getchannel("A"))
    save(mono, RES, "drawable-nodpi", "ic_launcher_mono.png")


if __name__ == "__main__":
    main()
