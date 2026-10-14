#!/usr/bin/env python3
"""The kill report's skull, byte 6 of AQtion's conchars, with the skull
centred on its disc: in the pak's cell the white skull sits a pixel high
(its weight at row 14.9 of 16, and the disc's lower-right shadow pulls
the eye the other way). This takes the cell from a conchars.png, moves
every non-disc pixel down one, fills the row it leaves with the disc,
and writes it at 4x (128x128, Lanczos) to action/pics/glyph/6.png - big
enough to stay out of the scrap and carry mip levels. The client draws
pics/glyph/<byte>.png in place of an icon's cell (text.c, text_glyph).

    python3 tools/glyph-gen.py <conchars.png | pak with pics/conchars.png> [outdir]
"""
import io, os, sys, zipfile
from PIL import Image

CELL_BYTE = 6
SHIFT = 1

def load_conchars(path):
    if path.lower().endswith(('.pkz', '.zip', '.pak')):
        with zipfile.ZipFile(path) as z:
            return Image.open(io.BytesIO(z.read('pics/conchars.png'))).convert('RGBA')
    return Image.open(path).convert('RGBA')

def is_disc(p):
    r, g, b, a = p
    return a > 128 and r > 150 and g < 140 and b < 80     # the orange, lit or in shadow

def main():
    src = sys.argv[1]
    outdir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), '..', 'action', 'pics', 'glyph')
    im = load_conchars(src)
    cell = im.width // 16
    x0, y0 = (CELL_BYTE % 16) * cell, (CELL_BYTE // 16) * cell
    c = im.crop((x0, y0, x0 + cell, y0 + cell))
    out = c.copy()
    px, po = c.load(), out.load()
    # the skull: opaque, not the orange, inside the disc (its edge's
    # half-transparent pixels and the corners stay put)
    skull = {(x, y) for y in range(cell) for x in range(cell)
             if px[x, y][3] == 255 and not is_disc(px[x, y]) and 3 <= x < cell - 3 and 3 <= y < cell - 3}
    for x, y in skull:
        if y + SHIFT < cell:
            po[x, y + SHIFT] = px[x, y]
    for x, y in skull:
        if (x, y - SHIFT) not in skull:      # vacated: the disc above it
            po[x, y] = px[x, y - SHIFT]
    # transparent pixels in the disc's colour, so the mip levels' average
    # does not darken its edge
    disc = next(px[x, y] for y in range(cell) for x in range(cell) if is_disc(px[x, y]))
    for y in range(cell):
        for x in range(cell):
            if po[x, y][3] == 0:
                po[x, y] = disc[:3] + (0,)
    os.makedirs(outdir, exist_ok=True)
    big = out.resize((cell * 4, cell * 4), Image.LANCZOS)
    big.save(os.path.join(outdir, f'{CELL_BYTE}.png'), optimize=True)
    print(f'{CELL_BYTE}.png: {cell}x{cell} cell shifted {SHIFT} down, written {big.size} to {outdir}')

if __name__ == '__main__':
    main()
