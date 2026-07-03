#!/usr/bin/env python3
"""
gen_font.py — rasterize a TTF into the AkiraOS bitmap-font C array format.

Output matches src/drivers/display/font_data.c: `const uint16_t NAME[chars][H]`,
each row an MSB-first bitmask (bit 15 = leftmost pixel), width <= 16.

Usage:
  python3 tools/gen_font.py --ttf FONT.ttf --name font_hero --w 16 --h 28 \
      --size 26 --first 32 --last 126 > font_hero.inc

For the rounded Playdate look, drop in a rounded TTF (Nunito / Quicksand /
Comfortaa / Baloo) and re-run — the array format is identical, so only
font_data.c changes. DejaVuSans-Bold is the offline fallback.
"""
import argparse
from PIL import Image, ImageDraw, ImageFont


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ttf", required=True)
    ap.add_argument("--name", required=True)
    ap.add_argument("--w", type=int, required=True)   # cell width (<=16)
    ap.add_argument("--h", type=int, required=True)   # cell height
    ap.add_argument("--size", type=int, required=True)  # font px size
    ap.add_argument("--first", type=int, default=32)
    ap.add_argument("--last", type=int, default=126)
    ap.add_argument("--thresh", type=int, default=128)
    ap.add_argument("--yoff", type=int, default=0)
    a = ap.parse_args()
    assert a.w <= 16, "row is uint16 → width must be <= 16"

    font = ImageFont.truetype(a.ttf, a.size)
    n = a.last - a.first + 1
    rows_out = []
    for code in range(a.first, a.last + 1):
        ch = chr(code)
        img = Image.new("L", (a.w, a.h), 0)
        d = ImageDraw.Draw(img)
        # centre the glyph horizontally in the cell
        try:
            bb = d.textbbox((0, 0), ch, font=font)
            gw = bb[2] - bb[0]
            gx = (a.w - gw) // 2 - bb[0]
        except Exception:
            gx = 0
        d.text((gx, a.yoff), ch, fill=255, font=font)
        px = img.load()
        rows = []
        for y in range(a.h):
            bits = 0
            for x in range(a.w):
                if px[x, y] >= a.thresh:
                    bits |= (0x8000 >> x)
            rows.append(bits)
        rows_out.append((ch, rows))

    W = a.w
    print(f"/* {a.name}: {a.w}x{a.h}, chars {a.first}..{a.last}, from {a.ttf} */")
    print(f"const uint16_t {a.name}[{n}][{a.h}] = {{")
    for ch, rows in rows_out:
        safe = ch if 32 < ord(ch) < 127 and ch not in "\\'\"" else f"0x{ord(ch):02X}"
        body = ", ".join(f"0x{r:04X}" for r in rows)
        print(f"    {{ {body} }}, /* '{safe}' */")
    print("};")
    _ = W


if __name__ == "__main__":
    main()
