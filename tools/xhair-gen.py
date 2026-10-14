#!/usr/bin/env python3
"""Sharp crosshairs: the twenty stock AQ2 crosshairs (pics/ch1-20, 32x32
pixel art) redrawn as vector shapes and rendered at 8x (256x256) into
action/pics/xhair/chN.png. The client draws these, mipmapped, where it
drew the pcx, so they come out identical at 1:1 and clean at any other
size; a straight edge stays straight and the rings and diamonds are
round.

Shapes are in the art's 32-unit grid, a pixel (x, y) covering
[x, x+1). An outlined crosshair is its white fill dilated by `outline`
units in black, as the art has it. Transparent pixels carry the fill's
colour (or the outline's) so the box-filtered mip levels do not darken
the edges.

    python3 tools/xhair-gen.py [outdir]
"""
import os, sys
from PIL import Image, ImageDraw, ImageFilter

S = 8           # output scale: 32 -> 256
SS = 4          # supersampling for the anti-aliased edges

WHITE = (255, 255, 255)
BLACK = (0, 0, 0)
RED = (255, 0, 0)
BROWN = (43, 11, 0)
LIGHT = (235, 235, 235)

def rect(x, y, w, h, c=WHITE): return ('rect', x, y, w, h, c)
def ring(cx, cy, r, t, c=WHITE): return ('ring', cx, cy, r, t, c)   # centre, outer radius, thickness
def diamond(cx, cy, r, c=WHITE): return ('diamond', cx, cy, r, c)    # |dx|+|dy| <= r

def vbar(x, y0, y1, w=1, c=WHITE): return rect(x, y0, w, y1 - y0 + 1, c)
def hbar(y, x0, x1, h=1, c=WHITE): return rect(x0, y, x1 - x0 + 1, h, c)

# fill shapes, outline width in units (0 none)
XHAIRS = {
    1:  ([vbar(16, 12, 20), hbar(16, 12, 20)], 1),
    2:  ([vbar(16, 12, 14), vbar(16, 18, 20), hbar(16, 12, 14), hbar(16, 18, 20)], 0),
    3:  ([vbar(16, 11, 12), vbar(16, 20, 21), hbar(16, 11, 12), hbar(16, 20, 21)], 1),
    4:  ([rect(15, 10, 3, 2), rect(15, 21, 3, 2), rect(10, 15, 2, 3), rect(21, 15, 2, 3)], 0),
    5:  ([vbar(16, 4, 11), vbar(16, 21, 28), hbar(16, 4, 11), hbar(16, 21, 28)], 1),
    6:  ([vbar(16, 7, 12), vbar(16, 20, 25), hbar(16, 7, 12), rect(16, 16, 1, 1), hbar(16, 20, 24)], 1),
    7:  ([vbar(16, 8, 24), hbar(16, 8, 24), diamond(16.5, 16.5, 2.5)], 1),
    8:  ([diamond(16.5, 16.5, 2.5)], 1),
    9:  ([rect(15, 15, 3, 3)], 1),
    10: ([ring(16.5, 16.5, 3.5, 1)], 0),
    11: ([vbar(15, 7, 12, 3), vbar(15, 20, 25, 3), hbar(15, 7, 12, 3), hbar(15, 20, 25, 3)], 0),
    12: ([vbar(15, 5, 8, 2), vbar(15, 23, 26, 2), hbar(15, 5, 8, 2), hbar(15, 23, 26, 2)], 2),
    13: ([rect(13, 3, 6, 4), rect(13, 25, 6, 4), rect(3, 13, 4, 6), rect(25, 13, 4, 6)], 0),
    14: ([ring(16.5, 16.5, 5.5, 1)], 0),
    15: ([diamond(16, 16, 5)], 2),
    16: ([rect(13, 13, 7, 7)], 1),
    17: ([vbar(16, 8, 11, 1, BROWN), vbar(16, 12, 14, 1, LIGHT), rect(16, 15, 1, 1, RED),
          rect(16, 16, 1, 1, BROWN), rect(16, 17, 1, 1, RED), vbar(16, 18, 20, 1, LIGHT), vbar(16, 21, 24, 1, BROWN),
          hbar(16, 8, 11, 1, BROWN), hbar(16, 12, 14, 1, LIGHT), rect(15, 16, 1, 1, RED),
          rect(17, 16, 1, 1, RED), hbar(16, 18, 20, 1, LIGHT), hbar(16, 21, 24, 1, BROWN)], 0),
    18: ([vbar(15, 8, 12, 3), vbar(15, 20, 24, 3), hbar(15, 8, 12, 3), hbar(15, 20, 24, 3)], 1),
    19: ([vbar(15, 8, 12, 2), vbar(15, 19, 23, 2), hbar(15, 8, 12, 2), hbar(15, 19, 23, 2)], 1),
    20: ([vbar(16, 14, 15), vbar(16, 17, 18), hbar(16, 14, 15), hbar(16, 17, 18)], 0),
}

def draw_shape(d, sh, grow, colour, k):
    """One shape onto ImageDraw d at k units per art unit, grown by `grow` units."""
    if sh[0] == 'rect':
        _, x, y, w, h, c = sh
        d.rectangle([(x - grow) * k, (y - grow) * k, (x + w + grow) * k - 1, (y + h + grow) * k - 1], fill=colour)
    elif sh[0] == 'ring':
        _, cx, cy, r, t, c = sh
        ro, ri = r + grow, r - t - grow
        d.ellipse([(cx - ro) * k, (cy - ro) * k, (cx + ro) * k, (cy + ro) * k], fill=colour)
        if ri > 0:
            d.ellipse([(cx - ri) * k, (cy - ri) * k, (cx + ri) * k, (cy + ri) * k], fill=(0, 0, 0, 0))
    elif sh[0] == 'diamond':
        _, cx, cy, r, c = sh
        r += grow
        d.polygon([((cx) * k, (cy - r) * k), ((cx + r) * k, cy * k), (cx * k, (cy + r) * k), ((cx - r) * k, cy * k)], fill=colour)

def render(n, shapes, outline):
    k = S * SS
    size = 32 * k
    # transparent pixels in the colour they will be averaged with
    base = BLACK if outline else shapes[0][-1]
    img = Image.new('RGBA', (size, size), base + (0,))
    d = ImageDraw.Draw(img)
    if outline:
        for sh in shapes:
            draw_shape(d, sh, outline, BLACK + (255,), k)
    for sh in shapes:
        c = sh[-1]
        draw_shape(d, sh, 0, c + (255,), k)
    # the rings' inner hole was cut to transparent: give it the base colour again
    px = img.load()
    for y in range(size):
        for x in range(size):
            if px[x, y][3] == 0:
                px[x, y] = base + (0,)
    out = img.resize((32 * S, 32 * S), Image.BOX)
    return out

def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..', 'action', 'pics', 'xhair')
    os.makedirs(outdir, exist_ok=True)
    for n, (shapes, outline) in XHAIRS.items():
        im = render(n, shapes, outline)
        im.save(os.path.join(outdir, f'ch{n}.png'), optimize=True)
    print(f'{len(XHAIRS)} crosshairs in {outdir}')

if __name__ == '__main__':
    main()
