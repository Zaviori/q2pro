/*
The players' round-start movement, read out of an .mvd2 demo: see
shared/mvdpath.h.

The wire, from q2pro's src/server/mvd and the laptop's mvdlog.py, which
reads the same demos in python:

  - "MVD2", then blocks of uint16 length + data; one mvd_frame per block,
    so the block index is the frame number (the first block is frame 1).
  - A block is written reliable messages first - prints, configstrings,
    unicasts, the centerprints among them - then the frame, then the
    unreliable datagram. So a LIGHTS seen in a block is the state the
    block's frame carries: the players are already on their spawns.
  - LIGHTS and ACTION! are svc_centerprint inside unicasts, one per
    player; the first in a block counts.
  - A player state carries the origin (eighths), the view's angles and
    offset, and the pmove type; the player's entity (number slot + 1)
    carries the model's animation frame. A delta: an axis that did not
    change is not sent and is carried.
*/

#include "shared/shared.h"
#include "shared/mvdpath.h"
#include <stdlib.h>
#ifdef MVP_DEBUG
#include <stdio.h>
#endif

#define MVD_MAGIC_STR       "MVD2"
#define MVD_PROTOCOL        37
#define MVD_VERSION_FLAGS   2012        // from this version the flags are a word of their own
#define MVF_EXTLIMITS_BIT   1

enum {
    mvd_nop = 1,
    mvd_serverdata = 4,
    mvd_configstring,
    mvd_frame,
    mvd_frame_nodelta,
    mvd_unicast,
    mvd_unicast_r,
    mvd_multicast_all,
    mvd_multicast_phs,
    mvd_multicast_pvs,
    mvd_multicast_all_r,
    mvd_multicast_phs_r,
    mvd_multicast_pvs_r,
    mvd_sound,
    mvd_print
};

enum {
    svc_layout = 4,
    svc_muzzleflash = 1,
    svc_sound = 9,
    svc_print,
    svc_stufftext,
    svc_configstring = 13,
    svc_centerprint = 15
};

#define SND_VOLUME          1
#define SND_ATTENUATION     2
#define SND_OFFSET          16

#define CS_MAXCLIENTS_IDX   30
#define CS_MODELS_IDX       32
#define CS_PLAYERSKINS_IDX  1312
#define CS_END_IDX          2080

#define PPS_M_TYPE          (1 << 0)
#define PPS_M_ORIGIN        (1 << 1)
#define PPS_M_ORIGIN2       (1 << 2)
#define PPS_VIEWOFFSET      (1 << 3)
#define PPS_VIEWANGLES      (1 << 4)
#define PPS_VIEWANGLE2      (1 << 5)
#define PPS_KICKANGLES      (1 << 6)
#define PPS_BLEND           (1 << 7)
#define PPS_FOV             (1 << 8)
#define PPS_WEAPONINDEX     (1 << 9)
#define PPS_WEAPONFRAME     (1 << 10)
#define PPS_GUNOFFSET       (1 << 11)
#define PPS_GUNANGLES       (1 << 12)
#define PPS_RDFLAGS         (1 << 13)
#define PPS_STATS           (1 << 14)
#define PPS_REMOVE          (1 << 15)   // MOREBITS, which means REMOVE in the old protocol

#define U_ORIGIN1           (1u << 0)
#define U_ORIGIN2           (1u << 1)
#define U_ANGLE2            (1u << 2)
#define U_ANGLE3            (1u << 3)
#define U_FRAME8            (1u << 4)
#define U_EVENT             (1u << 5)
#define U_REMOVE            (1u << 6)
#define U_MOREBITS1         (1u << 7)
#define U_NUMBER16          (1u << 8)
#define U_ORIGIN3           (1u << 9)
#define U_ANGLE1            (1u << 10)
#define U_MODEL             (1u << 11)
#define U_RENDERFX8         (1u << 12)
#define U_EFFECTS8          (1u << 14)
#define U_MOREBITS2         (1u << 15)
#define U_SKIN8             (1u << 16)
#define U_FRAME16           (1u << 17)
#define U_RENDERFX16        (1u << 18)
#define U_EFFECTS16         (1u << 19)
#define U_MODEL2            (1u << 20)
#define U_MODEL3            (1u << 21)
#define U_MODEL4            (1u << 22)
#define U_MOREBITS3         (1u << 23)
#define U_OLDORIGIN         (1u << 24)
#define U_SKIN16            (1u << 25)
#define U_SOUND             (1u << 26)
#define U_SOLID             (1u << 27)

#define PM_NORMAL_TYPE      0

#define WINDOW_FRAMES       (1 + MVP_ACTION_FRAMES + MVP_AFTER_FRAMES)
#define SAME_SPAWN          64      // units: two routes from one spawn start this close
#define SAME_ROUTE          80      // units: mean distance between two takes of one route
#define SAME_STEP           5       // samples between the points compared

typedef struct {
    const byte  *d;
    size_t      p, end;
    bool        bad;
} rd_t;

static int rd_byte(rd_t *r)
{
    if (r->p >= r->end) {
        r->bad = true;
        return 0;
    }
    return r->d[r->p++];
}

static int rd_char(rd_t *r)
{
    return (signed char)rd_byte(r);
}

static int rd_short(rd_t *r)
{
    int lo = rd_byte(r), hi = rd_byte(r);
    return (short)(lo | (hi << 8));
}

static int rd_word(rd_t *r)
{
    return rd_short(r) & 0xffff;
}

static int rd_long(rd_t *r)
{
    int lo = rd_word(r), hi = rd_word(r);
    return lo | (hi << 16);
}

static void rd_skip(rd_t *r, size_t n)
{
    if (r->end - r->p < n) {
        r->p = r->end;
        r->bad = true;
        return;
    }
    r->p += n;
}

// into buf (cut to fit), the whole string consumed
static void rd_string(rd_t *r, char *buf, size_t size)
{
    size_t n = 0;

    while (r->p < r->end) {
        int c = r->d[r->p++];
        if (!c) {
            if (buf)
                buf[n] = 0;
            return;
        }
        if (buf && n + 1 < size)
            buf[n++] = c;
    }
    if (buf)
        buf[n] = 0;
    r->bad = true;
}

typedef struct {
    bool        inuse;
    int         pm_type;
    short       origin[3];
    short       angles[2];
    signed char viewheight;
} ps_t;

typedef struct {
    bool        inuse;
    short       frame;
    int         skinnum;    // the weapon held is its high byte (the mod's vwep)
    byte        model2;     // 255 while a weapon is held
    bool        shot;       // a muzzle flash of his in this block
} es_t;

typedef struct {
    mvp_demo_t  *demo;
    int         frame;              // the block being read
    int         maxclients;

    ps_t        ps[MAX_CLIENTS];
    es_t        es[MAX_CLIENTS + 1];
    char        names[MAX_CLIENTS][MVP_NAME_MAX];
    char        skins[MAX_CLIENTS][MAX_QPATH];

    int         open;               // the round the window is on, -1 none
    int         end_f;              // its window's last frame
    bool        lights;             // a LIGHTS in this block: the round opens on its frame
    bool        action;             // an ACTION! in this block
    // the open round's players, by slot, their samples held back until
    // the round is committed whole
    int         team[MAX_CLIENTS];  // 0 or 1; -1 not in play at its LIGHTS
    bool        alive[MAX_CLIENTS];
    mvp_sample_t *buf;              // MAX_CLIENTS * WINDOW_FRAMES
    int         bufcount[MAX_CLIENTS];

    size_t      round_cap, trail_cap, sample_cap;
} parser_t;

static bool grow(void **p, size_t *cap, size_t need, size_t size)
{
    size_t n = *cap ? *cap : 64;
    void *q;

    if (need <= *cap)
        return true;
    while (n < need)
        n *= 2;
    q = realloc(*p, n * size);
    if (!q)
        return false;
    *p = q;
    *cap = n;
    return true;
}

static void set_playerskin(parser_t *ps, int slot, const char *s)
{
    const char *bs = strchr(s, '\\');
    size_t n;

    if (slot < 0 || slot >= MAX_CLIENTS)
        return;
    n = bs ? (size_t)(bs - s) : strlen(s);
    if (n >= MVP_NAME_MAX)
        n = MVP_NAME_MAX - 1;
    memcpy(ps->names[slot], s, n);
    ps->names[slot][n] = 0;
    Q_strlcpy(ps->skins[slot], bs ? bs + 1 : "", sizeof(ps->skins[slot]));
}

static void configstring(parser_t *ps, int index, const char *s)
{
    if (index == CS_MAXCLIENTS_IDX) {
        ps->maxclients = atoi(s);
    } else if (index == CS_MODELS_IDX + 1) {
        // "maps/urban2.bsp"
        const char *p = strrchr(s, '/'), *e;
        size_t n;

        p = p ? p + 1 : s;
        e = strrchr(p, '.');
        n = e ? (size_t)(e - p) : strlen(p);
        if (n >= sizeof(ps->demo->map))
            n = sizeof(ps->demo->map) - 1;
        memcpy(ps->demo->map, p, n);
        ps->demo->map[n] = 0;
    } else if (index >= CS_PLAYERSKINS_IDX && index < CS_PLAYERSKINS_IDX + MAX_CLIENTS) {
        set_playerskin(ps, index - CS_PLAYERSKINS_IDX, s);
    }
}

static void parse_playerstate(parser_t *ps, rd_t *r)
{
    int num, bits, i;
    ps_t *p;

    while (!r->bad) {
        num = rd_byte(r);
        if (num == 255)
            return;
        if (num >= MAX_CLIENTS) {
            r->bad = true;
            return;
        }
        p = &ps->ps[num];
        bits = rd_word(r);

        if (bits & PPS_M_TYPE)
            p->pm_type = rd_byte(r);
        if (bits & PPS_M_ORIGIN) {
            p->origin[0] = rd_short(r);
            p->origin[1] = rd_short(r);
        }
        if (bits & PPS_M_ORIGIN2)
            p->origin[2] = rd_short(r);
        if (bits & PPS_VIEWOFFSET) {
            rd_char(r);
            rd_char(r);
            p->viewheight = rd_char(r) / 4;
        }
        if (bits & PPS_VIEWANGLES) {
            p->angles[0] = rd_short(r);
            p->angles[1] = rd_short(r);
        }
        if (bits & PPS_VIEWANGLE2)
            rd_short(r);
        if (bits & PPS_KICKANGLES)
            rd_skip(r, 3);
        if (bits & PPS_WEAPONINDEX)
            rd_byte(r);
        if (bits & PPS_WEAPONFRAME)
            rd_byte(r);
        if (bits & PPS_GUNOFFSET)
            rd_skip(r, 3);
        if (bits & PPS_GUNANGLES)
            rd_skip(r, 3);
        if (bits & PPS_BLEND)
            rd_skip(r, 4);
        if (bits & PPS_FOV)
            rd_byte(r);
        if (bits & PPS_RDFLAGS)
            rd_byte(r);
        if (bits & PPS_STATS) {
            unsigned mask = rd_long(r);
            for (i = 0; i < 32; i++)
                if (mask & (1u << i))
                    rd_short(r);
        }

        p->inuse = !(bits & PPS_REMOVE);
    }
}

static void parse_entities(parser_t *ps, rd_t *r)
{
    unsigned bits;
    int num, frame;
    es_t *e;

    while (!r->bad) {
        bits = rd_byte(r);
        if (bits & U_MOREBITS1)
            bits |= rd_byte(r) << 8;
        if (bits & U_MOREBITS2)
            bits |= rd_byte(r) << 16;
        if (bits & U_MOREBITS3)
            bits |= (unsigned)rd_byte(r) << 24;
        num = (bits & U_NUMBER16) ? rd_word(r) : rd_byte(r);
        if (!num)
            return;
        e = num <= MAX_CLIENTS ? &ps->es[num] : NULL;
        frame = e ? e->frame : 0;

        if (bits & U_MODEL)
            rd_byte(r);
        if (bits & U_MODEL2) {
            int m2 = rd_byte(r);
            if (e)
                e->model2 = m2;
        }
        if (bits & U_MODEL3)
            rd_byte(r);
        if (bits & U_MODEL4)
            rd_byte(r);
        if (bits & U_FRAME8)
            frame = rd_byte(r);
        if (bits & U_FRAME16)
            frame = rd_word(r);
        if (bits & (U_SKIN8 | U_SKIN16)) {
            int skin;
            if ((bits & (U_SKIN8 | U_SKIN16)) == (U_SKIN8 | U_SKIN16))
                skin = rd_long(r);
            else if (bits & U_SKIN8)
                skin = rd_byte(r);
            else
                skin = rd_word(r);
            if (e)
                e->skinnum = skin;
        }
        if ((bits & (U_EFFECTS8 | U_EFFECTS16)) == (U_EFFECTS8 | U_EFFECTS16))
            rd_long(r);
        else if (bits & U_EFFECTS8)
            rd_byte(r);
        else if (bits & U_EFFECTS16)
            rd_word(r);
        if ((bits & (U_RENDERFX8 | U_RENDERFX16)) == (U_RENDERFX8 | U_RENDERFX16))
            rd_long(r);
        else if (bits & U_RENDERFX8)
            rd_byte(r);
        else if (bits & U_RENDERFX16)
            rd_word(r);
        if (bits & U_ORIGIN1)
            rd_short(r);
        if (bits & U_ORIGIN2)
            rd_short(r);
        if (bits & U_ORIGIN3)
            rd_short(r);
        if (bits & U_ANGLE1)
            rd_byte(r);
        if (bits & U_ANGLE2)
            rd_byte(r);
        if (bits & U_ANGLE3)
            rd_byte(r);
        if (bits & U_OLDORIGIN)
            rd_skip(r, 6);
        if (bits & U_SOUND)
            rd_byte(r);
        if (bits & U_EVENT)
            rd_byte(r);
        if (bits & U_SOLID)
            rd_short(r);

        if (e) {
            e->frame = frame;
            e->inuse = !(bits & U_REMOVE);
        }
    }
}

static bool in_play(const parser_t *ps, int slot)
{
    return ps->ps[slot].inuse && ps->ps[slot].pm_type == PM_NORMAL_TYPE && ps->es[slot + 1].inuse;
}

static void sample(const parser_t *ps, int slot, mvp_sample_t *s)
{
    const ps_t *p = &ps->ps[slot];

    VectorCopy(p->origin, s->origin);
    s->pitch = p->angles[0];
    s->yaw = p->angles[1];
    s->anim = ps->es[slot + 1].frame;
    s->viewheight = p->viewheight;
    s->weapon = ps->es[slot + 1].model2 == 255 ? (ps->es[slot + 1].skinnum >> 8) & 255 : 0;
    s->flags = ps->es[slot + 1].shot ? MVP_SHOT : 0;
}

// the open round's trails into the demo, the short ones dropped
static bool close_round(parser_t *ps)
{
    mvp_demo_t *demo = ps->demo;
    mvp_round_t *rnd;
    int slot, k;

    if (ps->open < 0)
        return true;
    rnd = &demo->rounds[ps->open];
    rnd->trail_first = demo->trail_count;
    for (slot = 0; slot < MAX_CLIENTS; slot++) {
        mvp_trail_t *t;

        k = ps->team[slot];
        if (k < 0)
            continue;
        if (ps->bufcount[slot] < MVP_MIN_SAMPLES)
            continue;
        if (!grow((void **)&demo->trails, &ps->trail_cap, demo->trail_count + 1, sizeof(*demo->trails)))
            return false;
        if (!grow((void **)&demo->samples, &ps->sample_cap, demo->sample_count + ps->bufcount[slot],
                  sizeof(*demo->samples)))
            return false;
        t = &demo->trails[demo->trail_count++];
        t->round = ps->open;
        t->slot = slot;
        t->team = k;
        Q_strlcpy(t->name, ps->names[slot], sizeof(t->name));
        t->first = demo->sample_count;
        t->count = ps->bufcount[slot];
        t->died = !ps->alive[slot];
        memcpy(&demo->samples[demo->sample_count], &ps->buf[slot * WINDOW_FRAMES],
               t->count * sizeof(*demo->samples));
        demo->sample_count += t->count;
    }
    rnd->trail_count = demo->trail_count - rnd->trail_first;
    ps->open = -1;
    return true;
}

// a LIGHTS was in this block: everybody in play is on a spawn now
static bool open_round(parser_t *ps)
{
    mvp_demo_t *demo = ps->demo;
    mvp_round_t *rnd;
    const char *skins[2] = { NULL, NULL };
    int slot, k;

    if (!close_round(ps))
        return false;
    if (!grow((void **)&demo->rounds, &ps->round_cap, demo->round_count + 1, sizeof(*demo->rounds)))
        return false;
    rnd = &demo->rounds[demo->round_count];
    memset(rnd, 0, sizeof(*rnd));
    rnd->number = demo->round_count + 1;
    rnd->lights_f = ps->frame;
    ps->open = demo->round_count++;
    ps->end_f = ps->frame + WINDOW_FRAMES - 1;

    for (slot = 0; slot < MAX_CLIENTS; slot++) {
        ps->team[slot] = -1;
        ps->bufcount[slot] = 0;
        ps->alive[slot] = false;
        if (!in_play(ps, slot))
            continue;
        // the team by the skin: the first skin met is team 0, the other 1
        for (k = 0; k < 2; k++) {
            if (!skins[k]) {
                skins[k] = ps->skins[slot];
                break;
            }
            if (!strcmp(skins[k], ps->skins[slot]))
                break;
        }
        ps->team[slot] = k < 2 ? k : 1;
        ps->alive[slot] = true;
    }
    for (k = 0; k < 2; k++)
        Q_strlcpy(rnd->skin[k], skins[k] ? skins[k] : "", sizeof(rnd->skin[k]));
    return true;
}

// the block's frame is read: a sample for each trail still running
static void sample_frame(parser_t *ps)
{
    int slot;

    if (ps->open < 0)
        return;
    for (slot = 0; slot < MAX_CLIENTS; slot++) {
        if (ps->team[slot] < 0 || !ps->alive[slot])
            continue;
        if (!in_play(ps, slot) || ps->bufcount[slot] >= WINDOW_FRAMES) {
            ps->alive[slot] = false;
            continue;
        }
        sample(ps, slot, &ps->buf[slot * WINDOW_FRAMES + ps->bufcount[slot]++]);
    }
}

static void parse_frame(parser_t *ps, rd_t *r)
{
    rd_skip(r, rd_byte(r));             // portal bits
    parse_playerstate(ps, r);
    parse_entities(ps, r);
}

static void centerprint(parser_t *ps, const char *text)
{
    if (!strncmp(text, "LIGHTS", 6))
        ps->lights = true;
    else if (!strncmp(text, "ACTION", 6))
        ps->action = true;
}

static void parse_unicast(parser_t *ps, rd_t *r, int length)
{
    rd_t sub;
    char text[MAX_STRING_CHARS];
    int flags, index;

    rd_byte(r);                         // clientNum: not counted in the length
    if (r->end - r->p < (size_t)length) {
        r->bad = true;
        return;
    }
    sub.d = r->d;
    sub.p = r->p;
    sub.end = r->p + length;
    sub.bad = false;
    r->p += length;

    while (sub.p < sub.end && !sub.bad) {
        switch (rd_byte(&sub)) {
        case svc_centerprint:
            rd_string(&sub, text, sizeof(text));
            centerprint(ps, text);
            break;
        case svc_print:
            rd_byte(&sub);
            rd_string(&sub, NULL, 0);
            break;
        case svc_layout:
        case svc_stufftext:
            rd_string(&sub, NULL, 0);
            break;
        case svc_configstring:
            index = rd_word(&sub);
            rd_string(&sub, text, sizeof(text));
            configstring(ps, index, text);
            break;
        case svc_sound:
            flags = rd_byte(&sub);
            rd_byte(&sub);              // index
            if (flags & SND_VOLUME)
                rd_byte(&sub);
            if (flags & SND_ATTENUATION)
                rd_byte(&sub);
            if (flags & SND_OFFSET)
                rd_byte(&sub);
            if (flags & 8)              // SND_ENT
                rd_word(&sub);
            if (flags & 4)              // SND_POS
                rd_skip(&sub, 6);
            break;
        default:
            return;                     // the rest of this unicast is unknown, and skipped
        }
    }
}

// false on a demo that is no MVD at all; a bad block ends the reading
static bool parse_block(parser_t *ps, rd_t *r, char *err, size_t errlen)
{
    char text[MAX_STRING_CHARS];
    int cmd, op, extra, index, length, flags, version;
    bool framed = false;

    ps->lights = ps->action = false;
    for (index = 0; index <= MAX_CLIENTS; index++)
        ps->es[index].shot = false;     // the block's flashes come before its frame
    while (r->p < r->end && !r->bad) {
        cmd = rd_byte(r);
        op = cmd & 31;
        extra = cmd >> 5;

        switch (op) {
        case mvd_nop:
            break;
        case mvd_serverdata:
            if (rd_long(r) != MVD_PROTOCOL) {
                Q_strlcpy(err, "not an MVD stream", errlen);
                return false;
            }
            version = rd_word(r);
            flags = version >= MVD_VERSION_FLAGS ? rd_word(r) : extra;
            if (flags & MVF_EXTLIMITS_BIT) {
                Q_strlcpy(err, "an extended-limits demo, which is not supported", errlen);
                return false;
            }
            rd_long(r);                 // servercount
            rd_string(r, NULL, 0);      // gamedir
            rd_short(r);                // clientNum
            while (!r->bad) {
                index = rd_word(r);
                if (index == CS_END_IDX)
                    break;
                rd_string(r, text, sizeof(text));
                configstring(ps, index, text);
            }
            // the baseline frame follows, with no opcode of its own
            parse_frame(ps, r);
            framed = true;
            break;
        case mvd_configstring:
            index = rd_word(r);
            rd_string(r, text, sizeof(text));
            configstring(ps, index, text);
            break;
        case mvd_frame:
        case mvd_frame_nodelta:
            parse_frame(ps, r);
            framed = true;
            break;
        case mvd_unicast:
        case mvd_unicast_r:
            length = rd_byte(r) | (extra << 8);
            parse_unicast(ps, r, length);
            break;
        case mvd_multicast_all:
        case mvd_multicast_phs:
        case mvd_multicast_pvs:
        case mvd_multicast_all_r:
        case mvd_multicast_phs_r:
        case mvd_multicast_pvs_r:
            length = rd_byte(r) | (extra << 8);
            if ((op - mvd_multicast_all) % 3)
                rd_word(r);             // leafnum
            // a muzzle flash in it is a shot: the entity's, this frame
            // (the flash is sent on its own, a multicast to itself)
            if (length >= 4 && r->p + 4 <= r->end && r->d[r->p] == svc_muzzleflash) {
                int num = r->d[r->p + 1] | (r->d[r->p + 2] << 8);

                if (num >= 1 && num <= MAX_CLIENTS)
                    ps->es[num].shot = true;
            }
            rd_skip(r, length);
            break;
        case mvd_sound:
            flags = rd_byte(r);
            rd_byte(r);                 // index
            if (flags & SND_VOLUME)
                rd_byte(r);
            if (flags & SND_ATTENUATION)
                rd_byte(r);
            if (flags & SND_OFFSET)
                rd_byte(r);
            rd_word(r);                 // sendchan
            break;
        case mvd_print:
            rd_byte(r);
            rd_string(r, NULL, 0);
            break;
        default:
#ifdef MVP_DEBUG
            fprintf(stderr, "frame %d: unknown op %d at %zu of %zu\n", ps->frame, op, r->p, r->end);
#endif
            // past the frame it is the datagram: what is not known of
            // it is left, the frame stands
            if (framed) {
                r->p = r->end;
                break;
            }
            r->bad = true;
            break;
        }
#ifdef MVP_DEBUG
        if (r->bad)
            fprintf(stderr, "frame %d: bad after op %d at %zu of %zu\n", ps->frame, op, r->p, r->end);
#endif

        // the frame is followed by the datagram: the block's multicasts,
        // read for the muzzle flashes (the shots), which belong to it
    }

    if (r->bad)
        return true;                    // what came before this block stands

    if (ps->lights && !open_round(ps)) {
        Q_strlcpy(err, "out of memory", errlen);
        return false;
    }
    if (ps->action && ps->open >= 0 && !ps->demo->rounds[ps->open].action_f)
        ps->demo->rounds[ps->open].action_f = ps->frame;
    if (framed)
        sample_frame(ps);
    if (ps->open >= 0 && ps->frame >= ps->end_f && !close_round(ps)) {
        Q_strlcpy(err, "out of memory", errlen);
        return false;
    }
    return true;
}

static int player_cmp(const void *a, const void *b)
{
    const mvp_player_t *pa = a, *pb = b;

    if (pa->trails != pb->trails)
        return pb->trails - pa->trails;
    return Q_stricmp(pa->name, pb->name);
}

static void count_players(mvp_demo_t *demo)
{
    int i, k;

    demo->player_count = 0;
    for (i = 0; i < demo->trail_count; i++) {
        const char *name = demo->trails[i].name;

        for (k = 0; k < demo->player_count; k++)
            if (!strcmp(demo->players[k].name, name))
                break;
        if (k == demo->player_count) {
            if (k >= MAX_CLIENTS)
                continue;
            Q_strlcpy(demo->players[k].name, name, sizeof(demo->players[k].name));
            demo->players[k].trails = 0;
            demo->player_count++;
        }
        demo->players[k].trails++;
    }
    qsort(demo->players, demo->player_count, sizeof(demo->players[0]), player_cmp);
}

void MvdPath_Free(mvp_demo_t *demo)
{
    if (!demo)
        return;
    free(demo->rounds);
    free(demo->trails);
    free(demo->samples);
    free(demo);
}

mvp_demo_t *MvdPath_Parse(const byte *data, size_t len, char *err, size_t errlen)
{
    parser_t *ps;
    mvp_demo_t *demo;
    rd_t r;
    size_t pos;
    int blocklen;
    bool ok = true;

    if (errlen)
        err[0] = 0;
    if (len < 6 || memcmp(data, MVD_MAGIC_STR, 4)) {
        Q_strlcpy(err, "not an MVD2 demo", errlen);
        return NULL;
    }

    ps = calloc(1, sizeof(*ps));
    demo = calloc(1, sizeof(*demo));
    if (ps)
        ps->buf = malloc(sizeof(*ps->buf) * MAX_CLIENTS * WINDOW_FRAMES);
    if (!ps || !demo || !ps->buf) {
        Q_strlcpy(err, "out of memory", errlen);
        goto fail;
    }
    ps->demo = demo;
    ps->open = -1;
    ps->maxclients = MAX_CLIENTS;

    pos = 4;
    while (pos + 2 <= len) {
        blocklen = data[pos] | (data[pos + 1] << 8);
        pos += 2;
        if (!blocklen)
            break;
        if (pos + blocklen > len)
            break;                      // cut short: the whole blocks before stand
        r.d = data;
        r.p = pos;
        r.end = pos + blocklen;
        r.bad = false;
        pos += blocklen;
        ps->frame++;
        demo->frames = ps->frame;

        if (!parse_block(ps, &r, err, errlen)) {
            ok = false;
            break;
        }
        if (r.bad)
            break;
    }

    if (ok && !close_round(ps)) {
        Q_strlcpy(err, "out of memory", errlen);
        ok = false;
    }
    if (!ok && !demo->round_count)
        goto fail;
    count_players(demo);
    free(ps->buf);
    free(ps);
    return demo;

fail:
    if (ps)
        free(ps->buf);
    free(ps);
    MvdPath_Free(demo);
    return NULL;
}

int MvdPath_Names(const byte *data, size_t len, char (*names)[MVP_NAME_MAX], int max)
{
    rd_t r;
    char text[MAX_STRING_CHARS], *bs;
    int blocklen, cmd, version, flags, index, n = 0;
    size_t cs;

    if (len < 6 || memcmp(data, MVD_MAGIC_STR, 4))
        return 0;
    blocklen = data[4] | (data[5] << 8);
    r.d = data;
    r.p = 6;
    r.end = len < 6 + (size_t)blocklen ? len : 6 + blocklen;
    r.bad = false;

    cmd = rd_byte(&r);
    if ((cmd & 31) != mvd_serverdata || rd_long(&r) != MVD_PROTOCOL)
        return 0;
    version = rd_word(&r);
    flags = version >= MVD_VERSION_FLAGS ? rd_word(&r) : cmd >> 5;
    if (flags & MVF_EXTLIMITS_BIT)
        return 0;
    rd_long(&r);
    rd_string(&r, NULL, 0);
    rd_short(&r);
    while (!r.bad && n < max) {
        index = rd_word(&r);
        if (index == CS_END_IDX)
            break;
        rd_string(&r, text, sizeof(text));
        if (index < CS_PLAYERSKINS_IDX || index >= CS_PLAYERSKINS_IDX + MAX_CLIENTS)
            continue;
        bs = strchr(text, '\\');
        cs = bs ? (size_t)(bs - text) : strlen(text);
        if (!cs)
            continue;
        if (cs >= MVP_NAME_MAX)
            cs = MVP_NAME_MAX - 1;
        memcpy(names[n], text, cs);
        names[n][cs] = 0;
        // the server's spectator slots, no game's players
        if (!Q_stricmp(names[n], "spec") || !Q_stricmp(names[n], "[MVDSPEC]"))
            continue;
        n++;
    }
    return n;
}

// the distance between two samples, in units
static float sample_dist(const mvp_sample_t *a, const mvp_sample_t *b)
{
    vec3_t va, vb, d;

    MvdPath_Origin(a, va);
    MvdPath_Origin(b, vb);
    VectorSubtract(va, vb, d);
    return VectorLength(d);
}

static bool same_route(const mvp_demo_t *demo, const mvp_trail_t *a, const mvp_trail_t *b, int frames)
{
    const mvp_sample_t *sa = &demo->samples[a->first], *sb = &demo->samples[b->first];
    int n = frames, i, k = 0;
    float sum = 0;

    if (sample_dist(sa, sb) > SAME_SPAWN)
        return false;
    if (n > a->count)
        n = a->count;
    if (n > b->count)
        n = b->count;
    for (i = 0; i < n; i += SAME_STEP, k++)
        sum += sample_dist(&sa[i], &sb[i]);
    if (!k)
        return true;
    return sum / k <= SAME_ROUTE;
}

int MvdPath_Unique(const mvp_demo_t *demo, const char *name, int frames, int *out, int *dups, int max)
{
    int i, k, n = 0;

    for (i = 0; i < demo->trail_count && n < max; i++) {
        const mvp_trail_t *t = &demo->trails[i];

        if (strcmp(t->name, name))
            continue;
        for (k = 0; k < n; k++)
            if (same_route(demo, &demo->trails[out[k]], t, frames))
                break;
        if (k < n) {
            dups[k]++;
            continue;
        }
        out[n] = i;
        dups[n] = 0;
        n++;
    }
    return n;
}
