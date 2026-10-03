/*
Copyright (C) 2026

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

/*
TrueType text, beside the 8x8 conchars rather than instead of them.

A font is a .ttf under fonts/ in the game path (r_font, r_font_bold).
Glyphs are rasterised by stb_truetype at the size they land on screen -
the virtual size divided by the 2D scale - so text is sharp at any HUD
scale instead of a magnified 8x8 tile. Each (font, pixel size) pair gets
its own atlas: the printable ASCII range, every glyph twice, once as is
and once dilated into an outline, baked when the size is first asked for.
A handful of sizes are kept; the least recently drawn one is dropped
when a new size needs the slot.

Strings are Quake strings: a byte with the high bit set is the alternate
colour, drawn as its low-bit ASCII in the caller's alt colour.
*/

#include "gl.h"

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_malloc(x, u)  ((void)(u), Z_Malloc(x))
#define STBTT_free(x, u)    ((void)(u), Z_Free(x))
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#include "stb/stb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define TEXT_FIRST      32
#define TEXT_LAST       126
#define TEXT_GLYPHS     (TEXT_LAST - TEXT_FIRST + 1)
#define TEXT_SIZES      12
#define TEXT_ATLAS_W    512
#define TEXT_MIN_PX     6
#define TEXT_MAX_PX     160

typedef struct {
    short   x, y, w, h;     // fill rect in the atlas; the outline sits at x + ox
    short   ox;             // ...offset to the outline copy, same row
    short   xoff, yoff;     // from the pen at the baseline to the rect's corner
    float   advance;
} glyph_t;

typedef struct {
    char            name[MAX_QPATH];
    byte            *data;
    stbtt_fontinfo  info;
    bool            tried;
    bool            ok;
} font_t;

typedef struct {
    int         font;       // index into fonts[], -1 when the slot is empty
    int         px;
    GLuint      texnum;
    int         atlas_h;
    float       ascent;     // pixels above the baseline
    float       line;       // ascent + descent + gap, pixels
    float       scale;      // stbtt scale for px
    unsigned    used;
    glyph_t     glyphs[TEXT_GLYPHS];
} textsize_t;

static cvar_t   *r_font;
static cvar_t   *r_font_bold;

static font_t       fonts[2];
static textsize_t   sizes[TEXT_SIZES];
static unsigned     text_clock;

static void text_free_sizes(void)
{
    for (int i = 0; i < TEXT_SIZES; i++) {
        if (sizes[i].texnum)
            qglDeleteTextures(1, &sizes[i].texnum);
        memset(&sizes[i], 0, sizeof(sizes[i]));
        sizes[i].font = -1;
    }
}

static void text_free_fonts(void)
{
    for (int i = 0; i < 2; i++) {
        Z_Free(fonts[i].data);
        memset(&fonts[i], 0, sizeof(fonts[i]));
    }
}

static void r_font_changed(cvar_t *self)
{
    GL_Flush2D();
    text_free_sizes();
    text_free_fonts();
}

static font_t *text_font(int index)
{
    font_t *f = &fonts[index];
    cvar_t *var = index ? r_font_bold : r_font;
    char path[MAX_QPATH];
    void *buf;
    int len;

    if (f->tried)
        return f->ok ? f : NULL;
    f->tried = true;

    Q_snprintf(path, sizeof(path), "fonts/%s.ttf", var->string);
    len = FS_LoadFile(path, &buf);
    if (len <= 0) {
        Com_WPrintf("Couldn't load %s\n", path);
        return NULL;
    }

    // stb keeps pointers into the file for the font's whole life
    f->data = Z_Malloc(len);
    memcpy(f->data, buf, len);
    FS_FreeFile(buf);

    if (!stbtt_InitFont(&f->info, f->data, stbtt_GetFontOffsetForIndex(f->data, 0))) {
        Com_WPrintf("%s is not a font stb_truetype can read\n", path);
        Z_Free(f->data);
        f->data = NULL;
        return NULL;
    }

    Q_strlcpy(f->name, var->string, sizeof(f->name));
    f->ok = true;
    return f;
}

// Grow `src` (w x h coverage) by r pixels: each output pixel takes the
// strongest coverage within a disc of radius r. Slow and simple, and run
// once per glyph per size.
static void text_dilate(const byte *src, byte *dst, int w, int h, int r)
{
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int best = 0;
            for (int dy = -r; dy <= r && best < 255; dy++) {
                int sy = y + dy;
                if (sy < 0 || sy >= h)
                    continue;
                for (int dx = -r; dx <= r; dx++) {
                    int sx = x + dx;
                    if (sx < 0 || sx >= w || dx * dx + dy * dy > r * r + r)
                        continue;
                    best = max(best, src[sy * w + sx]);
                }
            }
            dst[y * w + x] = best;
        }
    }
}

// The outline's thickness for a size: a pixel at small sizes, more as the
// glyphs grow, so it reads as the same weight at any scale.
static int text_outline_px(int px)
{
    return max(1, (px + 8) / 14);
}

static textsize_t *text_bake(int index, int px)
{
    font_t *f = text_font(index);
    textsize_t *ts = NULL;
    int ascent, descent, gap, pad, x, y, row_h;
    byte *cov, *out, *rgba;

    if (!f)
        return NULL;

    // a free slot, or the one drawn longest ago
    for (int i = 0; i < TEXT_SIZES; i++) {
        if (sizes[i].font < 0) {
            ts = &sizes[i];
            break;
        }
        if (!ts || sizes[i].used < ts->used)
            ts = &sizes[i];
    }

    GL_Flush2D();
    if (ts->texnum)
        qglDeleteTextures(1, &ts->texnum);
    memset(ts, 0, sizeof(*ts));
    ts->font = index;
    ts->px = px;
    ts->scale = stbtt_ScaleForPixelHeight(&f->info, px);

    stbtt_GetFontVMetrics(&f->info, &ascent, &descent, &gap);
    ts->ascent = ascent * ts->scale;
    ts->line = (ascent - descent + gap) * ts->scale;

    pad = text_outline_px(px) + 1;

    // Lay the glyphs out on shelves first, to know how tall the atlas is
    x = y = row_h = 0;
    for (int c = 0; c < TEXT_GLYPHS; c++) {
        glyph_t *g = &ts->glyphs[c];
        int x0, y0, x1, y1, adv, lsb;

        stbtt_GetCodepointHMetrics(&f->info, TEXT_FIRST + c, &adv, &lsb);
        stbtt_GetCodepointBitmapBox(&f->info, TEXT_FIRST + c, ts->scale, ts->scale,
                                    &x0, &y0, &x1, &y1);
        g->advance = adv * ts->scale;
        g->w = (x1 - x0) + pad * 2;
        g->h = (y1 - y0) + pad * 2;
        g->xoff = x0 - pad;
        g->yoff = y0 - pad;
        g->ox = g->w;

        if (x + g->w * 2 > TEXT_ATLAS_W) {
            x = 0;
            y += row_h;
            row_h = 0;
        }
        g->x = x;
        g->y = y;
        x += g->w * 2;
        row_h = max(row_h, g->h);
    }
    ts->atlas_h = 1;
    while (ts->atlas_h < y + row_h)
        ts->atlas_h <<= 1;

    rgba = Z_Mallocz(TEXT_ATLAS_W * ts->atlas_h * 4);
    cov = Z_Malloc((TEXT_MAX_PX * 2) * (TEXT_MAX_PX * 2));
    out = Z_Malloc((TEXT_MAX_PX * 2) * (TEXT_MAX_PX * 2));

    // White everywhere, the glyph in the alpha: tinted by the vertex colour
    for (int i = 0; i < TEXT_ATLAS_W * ts->atlas_h; i++) {
        rgba[i * 4 + 0] = 255;
        rgba[i * 4 + 1] = 255;
        rgba[i * 4 + 2] = 255;
    }

    for (int c = 0; c < TEXT_GLYPHS; c++) {
        glyph_t *g = &ts->glyphs[c];
        int gw = g->w, gh = g->h;

        if (gw <= pad * 2 || gh <= pad * 2)
            continue;   // a space
        if (gw > TEXT_MAX_PX * 2 || gh > TEXT_MAX_PX * 2)
            continue;

        memset(cov, 0, gw * gh);
        stbtt_MakeCodepointBitmap(&f->info, cov + pad * gw + pad,
                                  gw - pad * 2, gh - pad * 2, gw,
                                  ts->scale, ts->scale, TEXT_FIRST + c);
        text_dilate(cov, out, gw, gh, text_outline_px(px));

        for (int j = 0; j < gh; j++) {
            byte *row = rgba + ((g->y + j) * TEXT_ATLAS_W + g->x) * 4;
            for (int i = 0; i < gw; i++) {
                row[i * 4 + 3] = cov[j * gw + i];
                row[(g->ox + i) * 4 + 3] = out[j * gw + i];
            }
        }
    }

    qglGenTextures(1, &ts->texnum);
    GL_ForceTexture(TMU_TEXTURE, ts->texnum);
    qglTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TEXT_ATLAS_W, ts->atlas_h, 0,
                  GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    // Glyphs are drawn one texel to one pixel; linear only matters for the
    // sub-pixel offsets of a fractional HUD scale
    qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    Z_Free(rgba);
    Z_Free(cov);
    Z_Free(out);
    return ts;
}

// The atlas for this virtual size at the current 2D scale
static textsize_t *text_size(int flags, float size)
{
    int index = (flags & TEXT_BOLD) ? 1 : 0;
    int px = Q_rint(size / draw.scale);

    px = Q_clip(px, TEXT_MIN_PX, TEXT_MAX_PX);

    for (int i = 0; i < TEXT_SIZES; i++) {
        if (sizes[i].font == index && sizes[i].px == px) {
            sizes[i].used = ++text_clock;
            return &sizes[i];
        }
    }

    textsize_t *ts = text_bake(index, px);
    if (ts)
        ts->used = ++text_clock;
    return ts;
}

static inline int text_char(int c, bool *alt)
{
    *alt = c & 0x80;
    c = Q_charascii(c);
    if (c < TEXT_FIRST || c > TEXT_LAST)
        c = '?';
    return c;
}

// Width in pixels of the screen, not virtual units
static float text_width_px(textsize_t *ts, const char *s, size_t maxlen)
{
    font_t *f = &fonts[ts->font];
    float w = 0;
    int prev = 0;
    bool alt;

    while (maxlen-- && *s) {
        int c = text_char((byte)*s++, &alt);
        if (prev)
            w += stbtt_GetCodepointKernAdvance(&f->info, prev, c) * ts->scale;
        w += ts->glyphs[c - TEXT_FIRST].advance;
        prev = c;
    }
    return w;
}

bool R_TextAvailable(void)
{
    return text_font(0) != NULL;
}

int R_MeasureText(int flags, float size, const char *s, size_t maxlen)
{
    textsize_t *ts = text_size(flags, size);

    if (!ts)
        return 0;
    return Q_rint(text_width_px(ts, s, maxlen) * draw.scale);
}

// Height of one line of this size in virtual units: ascent, descent and
// the font's own line gap
int R_TextLineHeight(int flags, float size)
{
    textsize_t *ts = text_size(flags, size);

    if (!ts)
        return Q_rint(size);
    return Q_rint(ts->line * draw.scale);
}

static uint32_t text_scale_alpha(uint32_t color, int alpha)
{
    color_t c = { .u32 = color };
    c.u8[3] = c.u8[3] * alpha / 255;
    return c.u32;
}

/*
Draw a Quake string with the top of its line at (x, y), in virtual units.
`size` is the font's pixel height in virtual units, so 8 is the height of
a conchar. `color` is the text, `alt` the colour of high-bit characters.
The current 2D alpha (R_SetAlpha) multiplies both. Returns the x the text
ends at; TEXT_RIGHT and TEXT_CENTER align it on x instead.
*/
int R_DrawText(int x, int y, int flags, float size, uint32_t color, uint32_t alt,
               const char *s, size_t maxlen)
{
    textsize_t *ts = text_size(flags, size);
    font_t *f;
    float sc = draw.scale;
    float pen, base, w;
    int alpha = draw.colors[0].u8[3];
    uint32_t edge = text_scale_alpha(U32_BLACK, alpha * 220 / 255);

    if (!ts)
        return x;
    f = &fonts[ts->font];

    color = text_scale_alpha(color, alpha);
    alt = text_scale_alpha(alt, alpha);

    // Positions are worked in screen pixels and snapped there, so every
    // glyph lands on whole pixels whatever the HUD scale is
    w = text_width_px(ts, s, maxlen);
    pen = x / sc;
    if (flags & TEXT_RIGHT)
        pen -= w;
    else if (flags & TEXT_CENTER)
        pen -= w * 0.5f;
    pen = floorf(pen + 0.5f);
    base = floorf(y / sc + ts->ascent + 0.5f);

    float tw = 1.0f / TEXT_ATLAS_W, th = 1.0f / ts->atlas_h;
    int shadow = max(1, ts->px / 14);

    // Edges first, all of them, so no letter's outline covers its
    // neighbour's fill
    for (int pass = (flags & (TEXT_OUTLINE | TEXT_SHADOW)) ? 0 : 1; pass < 2; pass++) {
        float p = pen;
        int prev = 0;
        bool is_alt;
        size_t n = maxlen;

        for (const char *t = s; n-- && *t; ) {
            int c = text_char((byte)*t++, &is_alt);
            const glyph_t *g = &ts->glyphs[c - TEXT_FIRST];

            if (prev)
                p += stbtt_GetCodepointKernAdvance(&f->info, prev, c) * ts->scale;
            prev = c;

            if (c != ' ' && g->w > 0) {
                float gx = floorf(p + 0.5f) + g->xoff;
                float gy = base + g->yoff;

                if (pass == 0 && (flags & TEXT_OUTLINE)) {
                    GL_TextQuad(gx * sc, gy * sc, g->w * sc, g->h * sc,
                                   (g->x + g->ox) * tw, g->y * th,
                                   (g->x + g->ox + g->w) * tw, (g->y + g->h) * th,
                                   edge, ts->texnum);
                } else if (pass == 0) {
                    GL_TextQuad((gx + shadow) * sc, (gy + shadow) * sc,
                                   g->w * sc, g->h * sc,
                                   g->x * tw, g->y * th,
                                   (g->x + g->w) * tw, (g->y + g->h) * th,
                                   edge, ts->texnum);
                } else {
                    GL_TextQuad(gx * sc, gy * sc, g->w * sc, g->h * sc,
                                   g->x * tw, g->y * th,
                                   (g->x + g->w) * tw, (g->y + g->h) * th,
                                   is_alt ? alt : color, ts->texnum);
                }
            }
            p += g->advance;
        }
    }

    if (flags & TEXT_RIGHT)
        return x;
    return Q_rint((pen + w) * sc);
}

// font_test [size]: a panel of sample text at a few sizes, beside the
// conchars it replaces, for judging a font by eye
static cvar_t *r_font_test;

void Text_DrawTest(void)
{
    static const char sample[] = "zaviori [RNG] killed xeqt with M4 - HS 1234567890";
    static const float test_sizes[] = { 8, 10, 12, 16, 24 };
    float scale_was = draw.scale;
    int y = 40;

    if (!r_font_test || !r_font_test->integer)
        return;

    R_SetScale(R_ClampScale(Cvar_FindVar("scr_scale")));
    int vw = Q_rint(r_config.width * draw.scale);

    R_DrawFill32(20, y - 6, vw - 40, 230, MakeColor(30, 40, 50, 200));
    qhandle_t conchars = R_RegisterFont("conchars");
    R_DrawString(30, y, 0, MAX_STRING_CHARS, "conchars (8):", conchars);
    y += 10;
    R_DrawString(30, y, UI_DROPSHADOW, MAX_STRING_CHARS, sample, conchars);
    y += 16;

    for (int i = 0; i < q_countof(test_sizes); i++) {
        float sz = test_sizes[i];
        int x = 30;
        char label[32];

        Q_snprintf(label, sizeof(label), "%g:", sz);
        x = R_DrawText(x, y, TEXT_SHADOW, 8, U32_WHITE, U32_WHITE, label, MAX_STRING_CHARS) + 4;
        x = R_DrawText(x, y, TEXT_SHADOW, sz, U32_WHITE, MakeColor(120, 230, 120, 255),
                       sample, MAX_STRING_CHARS) + 12;
        R_DrawText(x, y, TEXT_BOLD | TEXT_OUTLINE, sz, MakeColor(255, 210, 90, 255),
                   U32_WHITE, "Bold outlined", MAX_STRING_CHARS);
        y += R_TextLineHeight(0, sz) + 4;
    }

    R_SetScale(scale_was);
}

void Text_Init(void)
{
    // fonts/<name>.ttf, searched like any other game file
    r_font = Cvar_Get("r_font", "NotoSans-Regular", CVAR_ARCHIVE);
    r_font_bold = Cvar_Get("r_font_bold", "NotoSans-Bold", CVAR_ARCHIVE);
    r_font->changed = r_font_changed;
    r_font_bold->changed = r_font_changed;
    r_font_test = Cvar_Get("r_font_test", "0", 0);

    for (int i = 0; i < TEXT_SIZES; i++)
        sizes[i].font = -1;
}

// The GL context is going: the atlases go with it. The fonts stay loaded
// unless the whole renderer is.
void Text_Shutdown(bool total)
{
    text_free_sizes();
    if (total)
        text_free_fonts();
}
