/*
Copyright (C) 2003-2006 Andrey Nazarov

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
 * gl_surf.c -- surface post-processing code
 *
 */
#include "gl.h"
#include "common/mdfour.h"

lightmap_builder_t lm;
static byte lm_buffer[0x4000000];

/*
=============================================================================

LIGHTMAP COLOR ADJUSTING

=============================================================================
*/

static inline void
adjust_color_f(vec_t *out, const vec_t *in, float add, float modulate, float scale)
{
    float r, g, b, y, max;

    // add & modulate
    r = (in[0] + add) * modulate;
    g = (in[1] + add) * modulate;
    b = (in[2] + add) * modulate;

    // catch negative lights
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;

    // determine the brightest of the three color components
    max = g;
    if (r > max)
        max = r;
    if (b > max)
        max = b;

    // rescale all the color components if the intensity of the greatest
    // channel exceeds 1.0
    if (max > 255) {
        y = 255.0f / max;
        r *= y;
        g *= y;
        b *= y;
    }

    // transform to grayscale by replacing color components with
    // overall pixel luminance computed from weighted color sum
    if (scale != 1) {
        y = LUMINANCE(r, g, b);
        r = y + (r - y) * scale;
        g = y + (g - y) * scale;
        b = y + (b - y) * scale;
    }

    out[0] = r;
    out[1] = g;
    out[2] = b;
}

void GL_AdjustColor(vec3_t color)
{
    adjust_color_f(color, color, lm.add, gl_static.entity_modulate, lm.scale);
    VectorScale(color, (1.0f / 255), color);
}

/*
=============================================================================

DYNAMIC BLOCKLIGHTS

=============================================================================
*/

#define MAX_LIGHTMAP_EXTENTS    513
#define MAX_BLOCKLIGHTS         (MAX_LIGHTMAP_EXTENTS * MAX_LIGHTMAP_EXTENTS)

#define LM_PIXELS(map, s, t)    ((map)->buffer + ((t) << lm.block_shift) + ((s) << 2))

static float blocklights[MAX_BLOCKLIGHTS * 3];

static void put_blocklights(const mface_t *surf)
{
    float add, modulate, scale = lm.scale;
    int i, j, smax, tmax, stride = 1 << lm.block_shift;
    const float *bl;
    byte *out;

    if (gl_static.use_shaders) {
        add = 0;
        modulate = 1;
    } else {
        add = lm.add;
        modulate = lm.modulate;
    }

    smax = surf->lm_width;
    tmax = surf->lm_height;

    out = LM_PIXELS(surf->light_m, surf->light_s, surf->light_t);

    for (i = 0, bl = blocklights; i < tmax; i++, out += stride) {
        byte *dst;
        for (j = 0, dst = out; j < smax; j++, bl += 3, dst += 4) {
            vec3_t tmp;
            adjust_color_f(tmp, bl, add, modulate, scale);
            dst[0] = (byte)tmp[0];
            dst[1] = (byte)tmp[1];
            dst[2] = (byte)tmp[2];
            dst[3] = 255;
        }
    }
}

static void add_dynamic_lights(const mface_t *surf)
{
    const dlight_t  *light;
    vec3_t          point;
    vec2_t          local;
    vec_t           s_scale, t_scale, sd, td;
    vec_t           dist, rad, minlight, scale, frac;
    float           *bl;
    int             i, smax, tmax, s, t;

    smax = surf->lm_width;
    tmax = surf->lm_height;
    s_scale = surf->lm_scale[0];
    t_scale = surf->lm_scale[1];

    for (i = 0; i < glr.fd.num_dlights; i++) {
        if (!(surf->dlightbits & BIT_ULL(i)))
            continue;

        light = &glr.fd.dlights[i];
        dist = PlaneDiffFast(light->transformed, surf->plane);
        rad = light->intensity - fabsf(dist);
        if (rad < DLIGHT_CUTOFF)
            continue;

        if (gl_dlight_falloff->integer) {
            minlight = rad - DLIGHT_CUTOFF * 0.8f;
            scale = rad / minlight; // fall off from rad to 0
        } else {
            minlight = rad - DLIGHT_CUTOFF;
            scale = 1;              // fall off from rad to minlight
        }

        VectorMA(light->transformed, -dist, surf->plane->normal, point);

        local[0] = DotProduct(point, surf->lm_axis[0]) + surf->lm_offset[0];
        local[1] = DotProduct(point, surf->lm_axis[1]) + surf->lm_offset[1];

        bl = blocklights;
        for (t = 0; t < tmax; t++) {
            td = fabsf(local[1] - t) * t_scale;
            for (s = 0; s < smax; s++) {
                sd = fabsf(local[0] - s) * s_scale;
                if (sd > td)
                    dist = sd + td * 0.5f;
                else
                    dist = td + sd * 0.5f;
                if (dist < minlight) {
                    frac = rad - dist * scale;
                    VectorMA(bl, frac, light->color, bl);
                }
                bl += 3;
            }
        }
    }
}

// Where this surface's light actually comes from. The stained copy holds
// the same bytes at the same offsets, so one subtraction finds it.
static const byte *surf_lightmap(const mface_t *surf)
{
    if (!lm.stainmap)
        return surf->lightmap;
    return lm.stainmap + (surf->lightmap - gl_static.world.cache->lightmap);
}

static void add_light_styles(mface_t *surf)
{
    const lightstyle_t *style;
    const byte *src;
    float *bl;
    int i, j, size = surf->lm_width * surf->lm_height;

    if (!surf->numstyles) {
        // should this ever happen?
        memset(blocklights, 0, sizeof(blocklights[0]) * size * 3);
        return;
    }

    // init primary lightmap
    style = LIGHT_STYLE(surf->styles[0]);

    src = surf_lightmap(surf);
    bl = blocklights;
    if (style->white == 1) {
        for (j = 0; j < size; j++, bl += 3, src += 3)
            VectorCopy(src, bl);
    } else {
        for (j = 0; j < size; j++, bl += 3, src += 3)
            VectorScale(src, style->white, bl);
    }

    surf->stylecache[0] = style->white;

    // add remaining lightmaps
    for (i = 1; i < surf->numstyles; i++) {
        style = LIGHT_STYLE(surf->styles[i]);

        bl = blocklights;
        for (j = 0; j < size; j++, bl += 3, src += 3)
            VectorMA(bl, style->white, src, bl);

        surf->stylecache[i] = style->white;
    }
}

// The block this surface sits in has changed, so the region it occupies
// wants uploading again before the next draw.
static void mark_lightmap_dirty(const mface_t *surf)
{
    lightmap_t *m = surf->light_m;
    int s0 = surf->light_s;
    int t0 = surf->light_t;
    int s1 = s0 + surf->lm_width;
    int t1 = t0 + surf->lm_height;

    m->mins[0] = min(m->mins[0], s0);
    m->mins[1] = min(m->mins[1], t0);

    m->maxs[0] = max(m->maxs[0], s1);
    m->maxs[1] = max(m->maxs[1], t1);
}

static void update_dynamic_lightmap(mface_t *surf)
{
    // add all the lightmaps
    add_light_styles(surf);

    // add all the dynamic lights
    if (surf->dlightframe == glr.dlightframe)
        add_dynamic_lights(surf);
    else
        surf->dlightframe = 0;

    // put into texture format
    put_blocklights(surf);

    mark_lightmap_dirty(surf);
}

// updates lightmaps in RAM
void GL_PushLights(mface_t *surf)
{
    const lightstyle_t *style;
    int i;

    if (!surf->light_m)
        return;

    // dynamic this frame or dynamic previously
    if (surf->dlightframe) {
        update_dynamic_lightmap(surf);
        return;
    }

    // check for light style updates
    if (GL_EffectiveLightstyles()) {
        for (i = 0; i < surf->numstyles; i++) {
            style = LIGHT_STYLE(surf->styles[i]);
            if (style->white != surf->stylecache[i]) {
                update_dynamic_lightmap(surf);
                return;
            }
        }
    }
}

static void clear_dirty_region(lightmap_t *m)
{
    m->mins[0] = lm.block_size;
    m->mins[1] = lm.block_size;
    m->maxs[0] = 0;
    m->maxs[1] = 0;
}

// uploads dirty lightmap regions to GL
void GL_UploadLightmaps(void)
{
    lightmap_t *m;
    bool set = false;
    int i;

    for (i = 0, m = lm.lightmaps; i < lm.nummaps; i++, m++) {
        int x, y, w, h;

        if (m->mins[0] >= m->maxs[0] || m->mins[1] >= m->maxs[1])
            continue;

        x = m->mins[0];
        y = m->mins[1];
        w = m->maxs[0] - x;
        h = m->maxs[1] - y;

        if (!(gl_config.caps & QGL_CAP_UNPACK_SUBIMAGE)) {
            x = 0;
            w = lm.block_size;
        } else if (!set) {
            qglPixelStorei(GL_UNPACK_ROW_LENGTH, lm.block_size);
            set = true;
        }

        // upload lightmap subimage
        GL_ForceTexture(TMU_LIGHTMAP, lm.texnums[i]);
        qglTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h,
                         GL_RGBA, GL_UNSIGNED_BYTE, LM_PIXELS(m, x, y));
        clear_dirty_region(m);
        c.texUploads++;
        c.lightTexels += w * h;
    }

    if (set)
        qglPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    lm.stains_dirty = false;
}

/*
=============================================================================

LIGHTMAPS BUILDING

=============================================================================
*/

#define LM_AllocBlock(w, h, s, t) \
    GL_AllocBlock(lm.block_size, lm.block_size, lm.inuse, w, h, s, t)

static void LM_InitBlock(void)
{
    memset(lm.inuse, 0, sizeof(lm.inuse));
    memset(lm.lightmaps[lm.nummaps].buffer, 0, lm.block_bytes);
}

static void LM_UploadBlock(void)
{
    if (!lm.dirty)
        return;

    Q_assert(lm.nummaps < lm.maxmaps);

    lightmap_t *m = &lm.lightmaps[lm.nummaps];
    clear_dirty_region(m);

    GL_ForceTexture(TMU_LIGHTMAP, lm.texnums[lm.nummaps]);
    qglTexImage2D(GL_TEXTURE_2D, 0, lm.comp,
                  lm.block_size, lm.block_size, 0,
                  GL_RGBA, GL_UNSIGNED_BYTE, m->buffer);
    qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    lm.nummaps++;
    lm.dirty = false;
}

int GL_EffectiveLightstyles(void)
{
    if (!gl_dynamic_lightstyles || !gl_dynamic)
        return 1;
    return gl_dynamic_lightstyles->integer >= 0
        ? gl_dynamic_lightstyles->integer : gl_dynamic->integer;
}

bool GL_EffectiveMuzzleflash(void)
{
    if (!gl_dynamic_muzzleflash || !gl_dynamic)
        return true;
    return gl_dynamic_muzzleflash->integer >= 0
        ? gl_dynamic_muzzleflash->integer : (gl_dynamic->integer == 1);
}

bool GL_AnyDynamic(void)
{
    // Stains ride in on this too: it is what gates both GL_PushLights and
    // GL_UploadLightmaps for the frame, so without it a stain on a still
    // map would never reach the texture.
    return lm.stains_dirty || GL_EffectiveLightstyles() || GL_EffectiveMuzzleflash();
}

static void build_style_map(int dynamic)
{
    int i;

    if (!dynamic) {
        // make all styles fullbright
        memset(gl_static.lightstylemap, 0, sizeof(gl_static.lightstylemap));
        return;
    }

    for (i = 0; i < MAX_LIGHTSTYLES; i++)
        gl_static.lightstylemap[i] = i;

    if (dynamic != 1) {
        // make dynamic styles fullbright
        for (i = 1; i < 32; i++)
            gl_static.lightstylemap[i] = 0;
    }
}

static bool no_lightmaps(void)
{
    return gl_fullbright->integer || gl_vertexlight->integer;
}

static void LM_BeginBuilding(void)
{
    const bsp_t *bsp = gl_static.world.cache;
    int size_shift, bits;

    // start up with fullbright styles
    build_style_map(0);

    // lightmap textures are not deleted from memory when changing maps,
    // they are merely reused
    lm.nummaps = lm.maxmaps = 0;
    lm.dirty = false;

    if (no_lightmaps())
        return;

    // use larger lightmaps for DECOUPLED_LM maps
    if (gl_lightmap_bits->integer)
        bits = Cvar_ClampInteger(gl_lightmap_bits, 7, 10);
    else
        bits = 8 + bsp->lm_decoupled * 2;

    // clamp to maximum texture size
    bits = min(bits, gl_config.max_texture_size_log2);

    lm.block_size = 1 << bits;
    lm.block_shift = bits + 2;

    size_shift = bits * 2 + 2;
    lm.block_bytes = 1 << size_shift;
    lm.maxmaps = min(sizeof(lm_buffer) >> size_shift, LM_MAX_LIGHTMAPS);

    for (int i = 0; i < lm.maxmaps; i++)
        lm.lightmaps[i].buffer = lm_buffer + (i << size_shift);

    Com_DPrintf("%s: %d lightmaps max, %d block size\n", __func__, lm.maxmaps, lm.block_size);

    LM_InitBlock();
}

static void LM_EndBuilding(void)
{
    // vertex lighting implies fullbright styles
    if (no_lightmaps())
        return;

    // upload the last lightmap
    LM_UploadBlock();

    // now build the real lightstyle map
    build_style_map(GL_EffectiveLightstyles());

    Com_DPrintf("%s: %d lightmaps built\n", __func__, lm.nummaps);
}

static void build_primary_lightmap(mface_t *surf)
{
    // add all the lightmaps
    add_light_styles(surf);

    surf->dlightframe = 0;

    // put into texture format
    put_blocklights(surf);
}

// defined below, and wanted by GL_StainReset above it
static void LM_RebuildSurfaces(void);

/*
=============================================================================

STAINMAPS

Blood, bullet scars and scorch, burned into the lightmap itself rather than
drawn as decals: the mark is lit like the wall it is on, costs no geometry
and no overdraw, and needs nothing of the renderer that the dynamic lights
did not already need. Ported from aprq2, whose version is Discoloda's.

Two things are ours rather than theirs. The stained bytes live in a copy of
the lump instead of in the BSP's own, because q2pro caches world models and
reuses them across map changes - staining the cached lump would leave last
night's blood on the map when it came round again, and its surfaces would
be pointing into a freed buffer if we repointed them. And the mark reaches
the screen through the existing dirty-region machinery: setting dlightframe
to a frame that is not this one makes GL_PushLights rebuild that surface
exactly once and clear the flag itself.

=============================================================================
*/

// Multipliers, so a stain darkens what is already there rather than
// painting over it - blood leaves red by taking green and blue away.
static bool stain_bloody;   // this stain is blood: tint red, do not darken

static const vec3_t stain_colors[4] = {
    { 1.00f, 0.80f, 0.80f },    // STAIN_BLOOD
    { 0.89f, 0.89f, 0.89f },    // STAIN_BULLET
    { 1.10f, 1.10f, 0.00f },    // STAIN_BLASTER
    { 0.80f, 0.80f, 0.80f },    // STAIN_SCORCH
};

// Throw the stains away: a fresh copy of the lump, or none at all when the
// cvar is off. Says nothing about the lightmaps already built from it.
static void stain_alloc(void)
{
    const bsp_t *bsp = gl_static.world.cache;

    Z_Free(lm.stainmap);
    lm.stainmap = NULL;

    if (bsp && bsp->lightmap && bsp->numlightmapbytes && gl_stainmaps->integer) {
        lm.stainmap = Z_Malloc(bsp->numlightmapbytes);
        memcpy(lm.stainmap, bsp->lightmap, bsp->numlightmapbytes);
    }
}

// ...and ask for the world to be lit from it again. The relighting cannot
// happen here: LM_RebuildSurfaces reads the light styles through
// LIGHT_STYLE, which is `glr.fd.lightstyles[...]`, and that pointer is only
// filled for the duration of a frame - it is NULL between them (see the end
// of GL_LoadWorld). Called from a cvar callback, which is to say from the
// console, it read straight through NULL and the client died with a
// segfault at 0 inside add_light_styles. So the work is left for the frame.
void GL_StainReset(void)
{
    stain_alloc();
    lm.stains_pending = true;
}

// At frame time, where the light styles exist. Cheap when there is nothing
// to do, which is every frame but the one after a reset.
void GL_StainApply(void)
{
    if (!lm.stains_pending)
        return;

    lm.stains_pending = false;

    if (gl_static.world.cache && lm.nummaps)
        LM_RebuildSurfaces();
}

static int stain_faces;     // touched by the stain being applied, for the log

static void stain_surface(mface_t *surf, const vec3_t org,
                          const vec3_t color, float size)
{
    vec_t   dist, rad, minlight, sd, td, floor;
    vec3_t  point;
    vec2_t  local;
    const byte *src;
    byte    *dst;
    bool    touched = false;
    int     s, t;

    // No lightmap to stain, or one we never built a block for
    if (!surf->lightmap || !surf->light_m)
        return;
    if (surf->drawflags & gl_static.nolm_mask)
        return;

    // A stain is not a light and must not borrow the light's threshold.
    // The dynamic-light code either side of this reads `rad < DLIGHT_CUTOFF`
    // as "too dim to bother with", and q2pro puts that constant at 64 -
    // while aprq2, where this routine comes from, defines it as 0, so there
    // it never rejects anything. Carried over unchanged it silently killed
    // the whole feature: a bullet stain has a radius of 5 and a blood spray
    // 18, so every one of them was under 64 and returned here. Measured on
    // a demo before the fix: 302 stains, 302 of them touching no surface at
    // all. What is left is the only test that means anything - the impact
    // has to be within `size` of the plane for any of its radius to survive.
    dist = PlaneDiffFast(org, surf->plane);
    rad = size - fabsf(dist);
    if (rad <= 0)
        return;
    minlight = rad;

    // Onto the plane, then into lightmap space - the same projection the
    // dynamic lights use, so odd lightmap scales come out right
    VectorMA(org, -dist, surf->plane->normal, point);
    local[0] = DotProduct(point, surf->lm_axis[0]) + surf->lm_offset[0];
    local[1] = DotProduct(point, surf->lm_axis[1]) + surf->lm_offset[1];

    // A plane is infinite and a surface is not. Everything above tests the
    // impact against the surface's *plane*, so a shot at one end of a long
    // floor passes for every face coplanar with it - 417 surfaces for a
    // radius-5 bullet, measured. Reject on the surface's own extent before
    // walking a luxel of it: `local` is already in luxels, so the radius
    // converts by the same scale.
    if (surf->lm_scale[0] > 0 && surf->lm_scale[1] > 0) {
        vec_t sr = rad / surf->lm_scale[0];
        vec_t tr = rad / surf->lm_scale[1];
        if (local[0] < -sr || local[0] > surf->lm_width - 1 + sr ||
            local[1] < -tr || local[1] > surf->lm_height - 1 + tr)
            return;
    }

    // Only the first style's block: the others are switchable lights, and
    // a stain belongs to the surface rather than to whether a lamp is lit
    dst = lm.stainmap + (surf->lightmap - gl_static.world.cache->lightmap);

    // The blend is multiplicative and there is no decay, so the same spot
    // shot twenty times would walk to zero and take the wall with it. The
    // floor is a fraction of what the luxel *started* at, read from the
    // BSP's own lump - an absolute floor would lift the dark corners of a
    // map instead of holding the bright parts down, which is backwards.
    // Repeated hits then converge on it rather than on black.
    floor = Cvar_ClampValue(gl_stain_floor, 0, 1);
    src = surf->lightmap;

    for (t = 0; t < surf->lm_height; t++) {
        td = fabsf(local[1] - t) * surf->lm_scale[1];
        for (s = 0; s < surf->lm_width; s++, dst += 3, src += 3) {
            sd = fabsf(local[0] - s) * surf->lm_scale[0];
            if (sd > td)
                dist = sd + td * 0.5f;
            else
                dist = td + sd * 0.5f;
            if (dist >= minlight)
                continue;
            if (stain_bloody) {
                // Blend toward a bright red instead of multiplying down, so
                // blood reads as a red splash on any surface - a dark floor
                // multiplied only ever goes blacker. Soft falloff to the
                // edge; red pushes up, green and blue pull down; repeated
                // hits converge on the red rather than on the wall's light.
                // The stainmap is the lightmap, so a luxel pushed to an
                // absolute red is lit like a lamp - on a dark map every
                // splash read as a red light source. Blood is a dark,
                // matte material: its red is the surface's own brightness
                // (from the pristine lump) times gl_stain_blood_bright,
                // never more, with gl_stain_blood as a ceiling on top.
                float lum = src[0] * 0.30f + src[1] * 0.59f + src[2] * 0.11f;
                int red = (int)(lum * Cvar_ClampValue(gl_stain_blood_bright, 0, 1));
                red = min(red, gl_stain_blood->integer);
                float a = 0.75f * (minlight - dist) / minlight;
                int tgt[3] = { red, red / 6, red / 6 };
                bool hit = false;
                for (int i = 0; i < 3; i++) {
                    int v = dst[i] + (int)((tgt[i] - dst[i]) * a);
                    v = min(max(v, 0), 255);
                    if (v != dst[i]) { dst[i] = v; hit = true; }
                }
                if (hit) touched = true;
                continue;
            }
            for (int i = 0; i < 3; i++) {
                int v = dst[i] * color[i];
                int lo = src[i] * floor;
                v = min(max(v, lo), 255);
                if (v != dst[i]) {
                    dst[i] = v;
                    touched = true;
                }
            }
        }
    }

    // Nothing actually changed - the reach fell between luxels, or they
    // were already at the floor. Rebuilding would cost the same as a real
    // stain and upload an identical block.
    if (!touched)
        return;

    // The bytes are in lm.stainmap now; what remains is to relight the
    // surface's texture block from them. Normally do it here rather than
    // flag it for the dynamic-light path: that path runs only when
    // GL_AnyDynamic() says so, and a map with no switchable lights and
    // nobody firing would never come back for a flagged surface. A stain
    // is a one-off edit, so it pays for itself once, on screen and off.
    //
    // But build_primary_lightmap reads the light styles through
    // glr.fd.lightstyles, and that pointer is only live for the duration
    // of a rendered frame - GL_LoadWorld leaves it NULL. A thrown-knife or
    // grenade impact parsed in the gap between a map loading and its first
    // frame (a clip seeking into a mid-fight citygate did exactly this)
    // would dereference NULL: segfault at 0 in add_light_styles, the whole
    // client gone, the reel black from that map on. When the styles are
    // not there, leave the relight to the frame - GL_StainApply does the
    // whole world once, which is heavy but only until the first frame.
    if (glr.fd.lightstyles) {
        build_primary_lightmap(surf);
        mark_lightmap_dirty(surf);
        lm.stains_dirty = true;     // ...and the frame uploads what changed
    } else {
        lm.stains_pending = true;   // relight everything next frame instead
    }
    stain_faces++;
}

static void stain_node(mnode_t *node, const vec3_t org,
                       const vec3_t color, float size)
{
    mface_t *face;
    vec_t   dot;
    int     i;

    while (node->plane) {
        dot = PlaneDiffFast(org, node->plane);
        if (dot > size) {
            node = node->children[0];
            continue;
        }
        if (dot < -size) {
            node = node->children[1];
            continue;
        }

        for (i = 0, face = node->firstface; i < node->numfaces; i++, face++)
            stain_surface(face, org, color, size);

        stain_node(node->children[0], org, color, size);
        node = node->children[1];
    }
}

void GL_StainWorld(const vec3_t org, const vec3_t color, float size)
{
    if (!lm.stainmap || !gl_static.world.cache)
        return;
    stain_faces = 0;
    stain_node(gl_static.world.cache->nodes, org, color, size);
}

// How many surfaces the last stain touched, for the `stain` console command
int GL_StainCount(void)
{
    return stain_faces;
}

bool R_StainmapsActive(void)
{
    return gl_stainmaps->integer && lm.stainmap != NULL;
}

void R_AddStain(const vec3_t org, int color, float size)
{
    vec3_t c;
    float dark;

    if (!gl_stainmaps->integer || (unsigned)color >= q_countof(stain_colors))
        return;
    stain_bloody = (color == STAIN_BLOOD);

    // Every stain is darkened by gl_stain_darkness on top of its own colour,
    // so the marks can be deepened without touching the four base tints -
    // 0.15 takes 15% off each channel, blood included, which reads as more
    // of a mark on any surface. The floor (gl_stain_floor) still limits how
    // dark a much-hit luxel can finally go.
    dark = 1.0f - Cvar_ClampValue(gl_stain_darkness, 0, 0.9f);
    VectorScale(stain_colors[color], dark, c);
    GL_StainWorld(org, c, size * Cvar_ClampValue(gl_stain_scale, 0.1f, 10));
}

static void LM_BuildSurface(mface_t *surf)
{
    int smax, tmax, s, t;

    if (lm.nummaps >= lm.maxmaps)
        return;     // can't have any more

    smax = surf->lm_width;
    tmax = surf->lm_height;

    if (!LM_AllocBlock(smax, tmax, &s, &t)) {
        LM_UploadBlock();
        if (lm.nummaps >= lm.maxmaps) {
            Com_EPrintf("%s: too many lightmaps\n", __func__);
            return;
        }
        LM_InitBlock();
        if (!LM_AllocBlock(smax, tmax, &s, &t)) {
            Com_EPrintf("%s: LM_AllocBlock(%d, %d) failed\n",
                        __func__, smax, tmax);
            return;
        }
    }

    lm.dirty = true;

    // store the surface lightmap parameters
    surf->light_s = s;
    surf->light_t = t;
    surf->light_m = &lm.lightmaps[lm.nummaps];

    // build the primary lightmap
    build_primary_lightmap(surf);
}

static void LM_RebuildSurfaces(void)
{
    const bsp_t *bsp = gl_static.world.cache;
    mface_t *surf;
    lightmap_t *m;
    int i;

    build_style_map(GL_EffectiveLightstyles());

    if (!lm.nummaps)
        return;

    for (i = 0, surf = bsp->faces; i < bsp->numfaces; i++, surf++)
        if (surf->light_m)
            build_primary_lightmap(surf);

    // upload all lightmaps
    for (i = 0, m = lm.lightmaps; i < lm.nummaps; i++, m++) {
        GL_ForceTexture(TMU_LIGHTMAP, lm.texnums[i]);
        qglTexImage2D(GL_TEXTURE_2D, 0, lm.comp,
                      lm.block_size, lm.block_size, 0,
                      GL_RGBA, GL_UNSIGNED_BYTE, m->buffer);
        clear_dirty_region(m);
        c.texUploads++;
    }
}

/*
=============================================================================

POLYGONS BUILDING

=============================================================================
*/

#define DotProductDouble(x,y) \
    ((double)(x)[0]*(y)[0]+\
     (double)(x)[1]*(y)[1]+\
     (double)(x)[2]*(y)[2])

static uint32_t color_for_surface(const mface_t *surf)
{
    if (surf->drawflags & SURF_TRANS33)
        return gl_static.inverse_intensity_33;

    if (surf->drawflags & SURF_TRANS66)
        return gl_static.inverse_intensity_66;

    if (surf->drawflags & SURF_WARP)
        return gl_static.inverse_intensity_100;

    return U32_WHITE;
}

static bool enable_intensity_for_surface(const mface_t *surf)
{
    // enable for any surface with a lightmap in BSPX maps
    if (surf->lightmap && gl_static.world.cache->has_bspx)
        return true;

    // enable for non-transparent, non-warped surfaces
    if (!(surf->drawflags & SURF_COLOR_MASK))
        return true;

    // enable for non-transparent lava (hack)
    if (!(surf->drawflags & SURF_TRANS_MASK) && strstr(surf->texinfo->name, "lava"))
        return true;

    return false;
}

static glStateBits_t statebits_for_surface(const mface_t *surf)
{
    glStateBits_t statebits = GLS_DEFAULT;

    if (surf->drawflags & SURF_SKY) {
        if (surf->texinfo->image->flags & IF_CLASSIC_SKY)
            return GLS_TEXTURE_REPLACE | GLS_CLASSIC_SKY;
        else
            return GLS_TEXTURE_REPLACE | GLS_DEFAULT_SKY;
    }

    if (gl_static.use_shaders) {
        // no inverse intensity
        if (!(surf->drawflags & SURF_TRANS_MASK))
            statebits |= GLS_TEXTURE_REPLACE;
        if (enable_intensity_for_surface(surf))
            statebits |= GLS_INTENSITY_ENABLE;
    } else {
        if (!(surf->drawflags & SURF_COLOR_MASK))
            statebits |= GLS_TEXTURE_REPLACE;
    }

    if (surf->drawflags & SURF_WARP)
        statebits |= GLS_WARP_ENABLE;

    if (surf->drawflags & SURF_TRANS_MASK)
        statebits |= GLS_BLEND_BLEND | GLS_DEPTHMASK_FALSE;
    else if (surf->drawflags & SURF_ALPHATEST)
        statebits |= GLS_ALPHATEST_ENABLE;

    if (surf->drawflags & SURF_FLOWING) {
        statebits |= GLS_SCROLL_ENABLE;
        if (surf->drawflags & SURF_WARP)
            statebits |= GLS_SCROLL_SLOW;
    }

    if (surf->drawflags & SURF_N64_SCROLL_X)
        statebits |= GLS_SCROLL_ENABLE | GLS_SCROLL_X;

    if (surf->drawflags & SURF_N64_SCROLL_Y)
        statebits |= GLS_SCROLL_ENABLE | GLS_SCROLL_Y;

    if (surf->drawflags & SURF_N64_SCROLL_FLIP)
        statebits |= GLS_SCROLL_FLIP;

    return statebits;
}

static void build_surface_poly(mface_t *surf, vec_t *vbo)
{
    const bsp_t *bsp = gl_static.world.cache;
    const msurfedge_t *src_surfedge;
    const mvertex_t *src_vert;
    const medge_t *src_edge;
    const mtexinfo_t *texinfo = surf->texinfo;
    const uint32_t color = color_for_surface(surf);
    vec2_t scale, tc, mins, maxs;
    int i, bmins[2], bmaxs[2];

    // convert surface flags to state bits
    surf->statebits = statebits_for_surface(surf);

    // normalize texture coordinates
    scale[0] = 1.0f / texinfo->image->width;
    scale[1] = 1.0f / texinfo->image->height;

    if (surf->drawflags & SURF_N64_UV) {
        scale[0] *= 0.5f;
        scale[1] *= 0.5f;
    }

    mins[0] = mins[1] = 99999;
    maxs[0] = maxs[1] = -99999;

    src_surfedge = surf->firstsurfedge;
    for (i = 0; i < surf->numsurfedges; i++) {
        src_edge = bsp->edges + src_surfedge->edge;
        src_vert = bsp->vertices + src_edge->v[src_surfedge->vert];
        src_surfedge++;

        // vertex coordinates
        VectorCopy(src_vert->point, vbo);

        // vertex color
        WN32(vbo + 3, color);

        // texture0 coordinates
        tc[0] = DotProductDouble(vbo, texinfo->axis[0]) + texinfo->offset[0];
        tc[1] = DotProductDouble(vbo, texinfo->axis[1]) + texinfo->offset[1];

        vbo[4] = tc[0] * scale[0];
        vbo[5] = tc[1] * scale[1];

        // texture1 coordinates
        if (bsp->lm_decoupled) {
            vbo[6] = DotProduct(vbo, surf->lm_axis[0]) + surf->lm_offset[0];
            vbo[7] = DotProduct(vbo, surf->lm_axis[1]) + surf->lm_offset[1];
        } else {
            if (mins[0] > tc[0]) mins[0] = tc[0];
            if (maxs[0] < tc[0]) maxs[0] = tc[0];

            if (mins[1] > tc[1]) mins[1] = tc[1];
            if (maxs[1] < tc[1]) maxs[1] = tc[1];

            vbo[6] = tc[0] / 16;
            vbo[7] = tc[1] / 16;
        }

        vbo += VERTEX_SIZE;
    }

    if (bsp->lm_decoupled) {
        surf->lm_scale[0] = 1.0f / VectorLength(surf->lm_axis[0]);
        surf->lm_scale[1] = 1.0f / VectorLength(surf->lm_axis[1]);
        return;
    }

    // calculate surface extents
    bmins[0] = floor(mins[0] / 16);
    bmins[1] = floor(mins[1] / 16);
    bmaxs[0] = ceil(maxs[0] / 16);
    bmaxs[1] = ceil(maxs[1] / 16);

    VectorScale(texinfo->axis[0], 1.0f / 16, surf->lm_axis[0]);
    VectorScale(texinfo->axis[1], 1.0f / 16, surf->lm_axis[1]);
    surf->lm_offset[0] = texinfo->offset[0] / 16 - bmins[0];
    surf->lm_offset[1] = texinfo->offset[1] / 16 - bmins[1];
    surf->lm_width  = bmaxs[0] - bmins[0] + 1;
    surf->lm_height = bmaxs[1] - bmins[1] + 1;
    surf->lm_scale[0] = 16;
    surf->lm_scale[1] = 16;

    for (i = 0; i < surf->numsurfedges; i++) {
        vbo -= VERTEX_SIZE;
        vbo[6] -= bmins[0];
        vbo[7] -= bmins[1];
    }
}

// vertex lighting approximation
static void sample_surface_verts(mface_t *surf, vec_t *vbo)
{
    int     i;
    vec3_t  color;
    byte    *dst;

    if (surf->drawflags & SURF_COLOR_MASK)
        return;

    glr.lightpoint.surf = surf;

    for (i = 0; i < surf->numsurfedges; i++) {
        glr.lightpoint.s = (int)vbo[6];
        glr.lightpoint.t = (int)vbo[7];

        GL_SampleLightPoint(color);
        adjust_color_f(color, color, lm.add, lm.modulate, lm.scale);

        dst = (byte *)(vbo + 3);
        dst[0] = (byte)color[0];
        dst[1] = (byte)color[1];
        dst[2] = (byte)color[2];
        dst[3] = 255;

        vbo += VERTEX_SIZE;
    }

    surf->statebits &= ~GLS_TEXTURE_REPLACE;
    surf->statebits |= GLS_SHADE_SMOOTH;
}

// normalizes and stores lightmap texture coordinates in vertices
static void normalize_surface_lmtc(const mface_t *surf, vec_t *vbo)
{
    float s, t, scale = 1.0f / lm.block_size;
    int i;

    s = surf->light_s + 0.5f;
    t = surf->light_t + 0.5f;

    for (i = 0; i < surf->numsurfedges; i++) {
        vbo[6] += s;
        vbo[7] += t;
        vbo[6] *= scale;
        vbo[7] *= scale;

        vbo += VERTEX_SIZE;
    }
}

// validates and processes surface lightmap
static void build_surface_light(mface_t *surf, vec_t *vbo)
{
    const bsp_t *bsp = gl_static.world.cache;
    int smax, tmax, size, ofs;

    if (gl_fullbright->integer)
        return;

    if (!surf->lightmap)
        return;

    if (surf->drawflags & gl_static.nolm_mask)
        return;

    smax = surf->lm_width;
    tmax = surf->lm_height;

    // validate lightmap extents
    if (smax < 1 || tmax < 1 || smax > MAX_LIGHTMAP_EXTENTS || tmax > MAX_LIGHTMAP_EXTENTS) {
        Com_WPrintf("Bad lightmap extents: %d x %d\n", smax, tmax);
        surf->lightmap = NULL;  // don't use this lightmap
        return;
    }

    // validate lightmap bounds
    size = smax * tmax;
    ofs = surf->lightmap - bsp->lightmap;
    if (surf->numstyles * size * 3 > bsp->numlightmapbytes - ofs) {
        Com_WPrintf("Bad surface lightmap\n");
        surf->lightmap = NULL;  // don't use this lightmap
        return;
    }

    if (gl_vertexlight->integer) {
        sample_surface_verts(surf, vbo);
    } else {
        LM_BuildSurface(surf);
        normalize_surface_lmtc(surf, vbo);
    }
}

static void calc_surface_hash(mface_t *surf)
{
    uint32_t args[] = {
        surf->texinfo->image - r_images,
        surf->light_m ? surf->light_m - lm.lightmaps : 0,
        surf->statebits
    };
    struct mdfour md;
    uint8_t out[16];

    mdfour_begin(&md);
    mdfour_update(&md, (uint8_t *)args, sizeof(args));
    mdfour_result(&md, out);

    surf->hash = 0;
    for (int i = 0; i < 16; i++)
        surf->hash ^= out[i];
}

static bool create_surface_vbo(size_t size)
{
    GLuint buf = 0;

    if (!qglGenBuffers)
        return false;

#if USE_GLES
    if (size > 65536 * VERTEX_SIZE * sizeof(vec_t))
        return false;
#endif

#if USE_DEBUG
    if (gl_novbo->integer)
        return false;
#endif

    GL_ClearErrors();

    qglGenBuffers(1, &buf);
    GL_BindBuffer(GL_ARRAY_BUFFER, buf);
    qglBufferData(GL_ARRAY_BUFFER, size, NULL, GL_STATIC_DRAW);

    if (GL_ShowErrors("Failed to create world model VBO")) {
        GL_BindBuffer(GL_ARRAY_BUFFER, 0);
        qglDeleteBuffers(1, &buf);
        return false;
    }

    gl_static.world.vertices = NULL;
    gl_static.world.buffer = buf;
    return true;
}

static void upload_surface_vbo(int lastvert)
{
    size_t offset = lastvert * VERTEX_SIZE * sizeof(vec_t);
    size_t size = tess.numverts * VERTEX_SIZE * sizeof(vec_t);

    Com_DDPrintf("%s: %zu bytes at %zu\n", __func__, size, offset);

    qglBufferSubData(GL_ARRAY_BUFFER, offset, size, tess.vertices);
    tess.numverts = 0;
}

static void check_multitexture(void)
{
    if (gl_vertexlight->integer)
        return;
    if (qglActiveTexture && (qglClientActiveTexture || gl_static.use_shaders))
        return;
    Com_WPrintf("OpenGL doesn't support multitexturing, forcing vertex lighting.\n");
    Cvar_Set("gl_vertexlight", "1");
}

static void upload_world_surfaces(void)
{
    const bsp_t *bsp = gl_static.world.cache;
    size_t size = gl_static.world.buffer_size;
    vec_t *vbo;
    mface_t *surf;
    int i, currvert, lastvert;

    // force vertex lighting if multitexture is not supported
    check_multitexture();

    // begin building lightmaps
    LM_BeginBuilding();

    if (!gl_static.world.vertices)
        GL_BindBuffer(GL_ARRAY_BUFFER, gl_static.world.buffer);

    currvert = 0;
    lastvert = 0;
    for (i = 0, surf = bsp->faces; i < bsp->numfaces; i++, surf++) {
        if (surf->drawflags & SURF_NODRAW)
            continue;

        Q_assert(surf->numsurfedges >= 3 && surf->numsurfedges <= TESS_MAX_VERTICES);
        Q_assert(size >= surf->numsurfedges * VERTEX_SIZE * sizeof(vbo[0]));
        size -= surf->numsurfedges * VERTEX_SIZE * sizeof(vbo[0]);

        if (gl_static.world.vertices) {
            vbo = gl_static.world.vertices + currvert * VERTEX_SIZE;
        } else {
            // upload VBO chunk if needed
            if (tess.numverts + surf->numsurfedges > TESS_MAX_VERTICES) {
                upload_surface_vbo(lastvert);
                lastvert = currvert;
            }

            vbo = tess.vertices + tess.numverts * VERTEX_SIZE;
            tess.numverts += surf->numsurfedges;
        }

        surf->light_m = NULL;   // start with no lightmap
        surf->firstvert = currvert;
        build_surface_poly(surf, vbo);
        build_surface_light(surf, vbo);

        calc_surface_hash(surf);

        currvert += surf->numsurfedges;
    }

    // upload the last VBO chunk
    if (!gl_static.world.vertices)
        upload_surface_vbo(lastvert);

    // end building lightmaps
    LM_EndBuilding();

    gl_fullbright->modified = false;
    gl_vertexlight->modified = false;
    gl_lightmap_bits->modified = false;
}

static void set_world_size(const mnode_t *node)
{
    vec_t size;
    int i;

    for (i = 0, size = 0; i < 3; i++)
        size = max(size, node->maxs[i] - node->mins[i]);

    if (size > 4096)
        gl_static.world.size = 8192;
    else if (size > 2048)
        gl_static.world.size = 4096;
    else
        gl_static.world.size = 2048;
}

// called from the main loop whenever lighting parameters change
void GL_RebuildLighting(void)
{
    if (!gl_static.world.cache)
        return;

    // rebuild all surfaces if toggling lightmaps off/on
    if (gl_fullbright->modified || gl_vertexlight->modified) {
        upload_world_surfaces();
        return;
    }

    if (gl_fullbright->integer)
        return;

    // rebuild all surfaces if doing vertex lighting (and not fullbright)
    if (gl_vertexlight->integer || gl_lightmap_bits->modified) {
        upload_world_surfaces();
        return;
    }

    // rebuild all lightmaps
    LM_RebuildSurfaces();
}

void GL_FreeWorld(void)
{
    if (!gl_static.world.cache)
        return;

    BSP_Free(gl_static.world.cache);
    Z_Free(gl_static.world.vertices);
    Z_Free(lm.stainmap);
    lm.stainmap = NULL;
    GL_DeleteBuffer(gl_static.world.buffer);

    if (gls.currentva == VA_3D)
        gls.currentva = VA_NONE;

    memset(&gl_static.world, 0, sizeof(gl_static.world));
}

static const mnode_t *find_face_node(const bsp_t *bsp, const mface_t *face)
{
    const mnode_t *node;
    int i, left, right;

    left = 0;
    right = bsp->numnodes - 1;
    while (left <= right) {
        i = (left + right) / 2;
        node = &bsp->nodes[i];
        if (node->firstface + node->numfaces <= face)
            left = i + 1;
        else if (node->firstface > face)
            right = i - 1;
        else
            return node;
    }

    return NULL;
}

static void remove_fake_sky_faces(const bsp_t *bsp)
{
    const mleaf_t *leaf;
    const mnode_t *node;
    int i, j, k, count = 0;
    mface_t *face;

    // find CONTENTS_MIST leafs
    for (i = 1, leaf = bsp->leafs + i; i < bsp->numleafs; i++, leaf++) {
        if (!(leaf->contents[0] & CONTENTS_MIST))
            continue;

        // remove sky faces in this leaf
        for (j = 0; j < leaf->numleaffaces; j++) {
            face = leaf->firstleafface[j];
            if (!(face->drawflags & SURF_SKY))
                continue;

            face->drawflags = SURF_NODRAW;
            count++;

            // find node this face is on
            node = find_face_node(bsp, face);
            if (!node) {
                Com_DPrintf("Sky face node not found\n");
                continue;
            }

            // remove other sky faces on this node
            for (k = 0, face = node->firstface; k < node->numfaces; k++, face++) {
                if (face->drawflags & SURF_SKY) {
                    face->drawflags = SURF_NODRAW;
                    count++;
                }
            }
        }
    }

    if (count)
        Com_DPrintf("Removed %d fake sky faces\n", count);
}

void GL_LoadWorld(const char *name)
{
    char buffer[MAX_QPATH];
    size_t size;
    bsp_t *bsp;
    mtexinfo_t *info;
    mface_t *surf;
    int i, n64surfs, ret;

    if (!name || !*name)
        return;

    Q_concat(buffer, sizeof(buffer), "maps/", name, ".bsp");
    ret = BSP_Load(buffer, &bsp);
    if (!bsp)
        Com_Error(ERR_DROP, "%s: couldn't load %s: %s",
                  __func__, buffer, BSP_ErrorString(ret));

    // check if the required world model was already loaded
    if (gl_static.world.cache == bsp) {
        for (i = 0; i < bsp->numtexinfo; i++)
            bsp->texinfo[i].image->registration_sequence = r_registration_sequence;

        for (i = 0; i < bsp->numnodes; i++)
            bsp->nodes[i].visframe = 0;

        for (i = 0; i < bsp->numleafs; i++)
            bsp->leafs[i].visframe = 0;

        Com_DPrintf("%s: reused old world model\n", __func__);
        bsp->refcount--;
        GL_StainReset();    // a reused map starts clean, like a fresh one
        return;
    }

    // free previous model, if any
    GL_FreeWorld();

    // delete occlusion queries
    GL_DeleteQueries();
    GL_InitQueries();

    gl_static.world.cache = bsp;

    // before the lightmaps are built, so they are built from it
    stain_alloc();

    // calculate world size for far clip plane and sky box
    set_world_size(bsp->nodes);

    // register all texinfo
    for (i = 0, info = bsp->texinfo; i < bsp->numtexinfo; i++, info++) {
        if (info->c.flags & SURF_SKY) {
            if (!gl_static.use_cubemaps) {
                info->image = R_NOTEXTURE;
            } else if (Q_stristr(info->name, "env/sky")) {
                Q_concat(buffer, sizeof(buffer), "textures/", info->name, ".tga");
                info->image = IMG_Find(buffer, IT_SKY, IF_REPEAT | IF_CLASSIC_SKY);
            } else if (Q_stricmpn(info->name, CONST_STR_LEN("sky/")) == 0) {
                Q_concat(buffer, sizeof(buffer), info->name, ".tga");
                info->image = IMG_Find(buffer, IT_SKY, IF_CUBEMAP);
            } else {
                info->image = R_SKYTEXTURE;
            }
        } else if (info->c.flags & SURF_NODRAW && bsp->has_bspx) {
            info->image = R_NOTEXTURE;
        } else {
            imageflags_t flags = (info->c.flags & SURF_WARP) ? IF_TURBULENT : IF_NONE;
            Q_concat(buffer, sizeof(buffer), "textures/", info->name, ".wal");
            info->image = IMG_Find(buffer, IT_WALL, flags);
        }
    }

    // setup drawflags, etc
    for (i = n64surfs = 0, surf = bsp->faces; i < bsp->numfaces; i++, surf++) {
        // hack surface flags into drawflags for faster access
        surf->drawflags |= surf->texinfo->c.flags & ~DSURF_PLANEBACK;

        // clear statebits from previous load
        surf->statebits = GLS_DEFAULT;

        // don't count sky surfaces unless using cubemaps
        if (surf->drawflags & SURF_SKY) {
            if (!gl_static.use_cubemaps) {
                surf->drawflags |= SURF_NODRAW; // simplify other code
                continue;
            }
            surf->drawflags &= ~SURF_NODRAW;
        }

        // ignore NODRAW bit in vanilla maps for compatibility
        if (surf->drawflags & SURF_NODRAW) {
            if (bsp->has_bspx)
                continue;
            surf->drawflags &= ~SURF_NODRAW;
        }

        if (surf->drawflags & (SURF_N64_UV | SURF_N64_SCROLL_X | SURF_N64_SCROLL_Y))
            n64surfs++;
    }

    // remove fake sky faces in vanilla maps
    if (!bsp->has_bspx && gl_static.use_cubemaps)
        remove_fake_sky_faces(bsp);

    // calculate vertex buffer size in bytes
    for (i = size = 0, surf = bsp->faces; i < bsp->numfaces; i++, surf++)
        if (!(surf->drawflags & SURF_NODRAW))
            size += surf->numsurfedges * VERTEX_SIZE * sizeof(vec_t);

    // try VBO first, then allocate on heap
    if (create_surface_vbo(size)) {
        Com_DPrintf("%s: %zu bytes of vertex data as VBO\n", __func__, size);
    } else {
        gl_static.world.vertices = R_Malloc(size);
        Com_DPrintf("%s: %zu bytes of vertex data on heap\n", __func__, size);
    }
    gl_static.world.buffer_size = size;

    gl_static.nolm_mask = SURF_NOLM_MASK_DEFAULT;
    gl_static.use_bmodel_skies = gl_static.use_cubemaps && bsp->has_bspx;

    // only supported in BSPX and N64 maps because vanilla maps have broken
    // lightofs for liquids/alphas. legacy renderer doesn't support lightmapped
    // liquids too.
    if ((bsp->has_bspx || n64surfs > 100) && gl_static.use_shaders)
        gl_static.nolm_mask = SURF_NOLM_MASK_REMASTER;

    glr.fd.lightstyles = &(lightstyle_t){ 1 };

    // post process all surfaces
    upload_world_surfaces();

    glr.fd.lightstyles = NULL;

    GL_ShowErrors(__func__);
}
