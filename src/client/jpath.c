/*
The players' round-start routes out of a demo, drawn as lines in the
world for the jump mod's player pathing (src/action/tng_paths.c).

The game picks the demo and the trails and says so over stuffcmd:

    jpath show <demo> <frames> <trail,...>          the routes, all at once
    jpath play <demo> <frames> <trail> [<trail,...>] one route, run from its
                                                     LIGHTS in real time, the
                                                     others of its round dim
    jpath off

The demo is demos/<demo> of the game path, parsed with the same reader
the game uses (shared/mvdpath.c), so a trail's index means the same
here. A route is green at LIGHTS, red by ACTION!, and darkens past it.
*/

#include "client.h"
#include "shared/mvdpath.h"

#define JPATH_HOLD_MS       1000    // a played route is held whole this long before it runs again (the ghosts' JMP_PATH_GHOST_HOLD_MS)
#define JPATH_MAX           64      // trails drawn at once

typedef struct {
    char        demo_name[MAX_QPATH];
    mvp_demo_t  *demo;
    int         frames;             // samples drawn of each trail
    int         trails[JPATH_MAX];  // drawn in full
    int         count;
    int         others[JPATH_MAX];  // drawn dim, for context
    int         other_count;
    bool        playing;            // trails[0] runs from its start
    unsigned    start;              // cls.realtime it last started
    int         hl;                 // the trail aimed at: lit, the rest dimmed; -1 none
} jpath_t;

static jpath_t jpath;

static cvar_t *jpath_depth;         // 1: the walls hide the lines, as anything else
static cvar_t *jpath_look;          // 1: his shots drawn as the bullets went - red to the wall, green to the player hit
static cvar_t *jpath_lift;          // units the lines float over the player's origin

static void JPath_Clear(void)
{
    MvdPath_Free(jpath.demo);
    memset(&jpath, 0, sizeof(jpath));
}

static bool JPath_Load(const char *name)
{
    char path[MAX_OSPATH], err[128];
    void *data;
    int len;

    if (jpath.demo && !strcmp(jpath.demo_name, name))
        return true;
    JPath_Clear();

    if (strstr(name, "..") || strchr(name, '/') || strchr(name, '\\')) {
        Com_Printf("jpath: no such demo name\n");
        return false;
    }
    Q_concat(path, sizeof(path), "demos/", name);
    len = FS_LoadFile(path, &data);
    if (!data) {
        Com_Printf("jpath: couldn't load %s: %s\n", path, Q_ErrorString(len));
        return false;
    }
    jpath.demo = MvdPath_Parse(data, len, err, sizeof(err));
    FS_FreeFile(data);
    if (!jpath.demo) {
        Com_Printf("jpath: %s: %s\n", name, err);
        return false;
    }
    Q_strlcpy(jpath.demo_name, name, sizeof(jpath.demo_name));
    return true;
}

// "3,7,12" into out, those that are trails of the demo
static int JPath_List(const char *s, int *out, int max)
{
    int n = 0, k;
    char *end;

    while (*s && n < max) {
        k = strtol(s, &end, 10);
        if (end == s)
            break;
        if (k >= 0 && k < jpath.demo->trail_count)
            out[n++] = k;
        s = end;
        if (*s == ',')
            s++;
    }
    return n;
}

static void JPath_Cmd_f(void)
{
    const char *sub = Cmd_Argv(1);

    if (!strcmp(sub, "off")) {
        jpath.count = jpath.other_count = 0;
        jpath.playing = false;
        jpath.hl = -1;
        return;
    }
    if (!strcmp(sub, "hl")) {
        jpath.hl = Cmd_Argc() > 2 ? atoi(Cmd_Argv(2)) : -1;
        return;
    }
    // the game's ghosts set off again: the line with them, so the two
    // clocks cannot drift apart
    if (!strcmp(sub, "restart")) {
        jpath.start = cls.realtime;
        return;
    }
    if (strcmp(sub, "show") && strcmp(sub, "play")) {
        Com_Printf("Usage: jpath show|play <demo> <frames> <trail,...> [<trail,...>], or jpath off\n");
        return;
    }
    if (Cmd_Argc() < 5 || !JPath_Load(Cmd_Argv(2)))
        return;

    jpath.frames = Q_clip(atoi(Cmd_Argv(3)), 1, MVP_ACTION_FRAMES + MVP_AFTER_FRAMES);
    jpath.count = JPath_List(Cmd_Argv(4), jpath.trails, JPATH_MAX);
    jpath.other_count = Cmd_Argc() > 5 ? JPath_List(Cmd_Argv(5), jpath.others, JPATH_MAX) : 0;
    jpath.playing = !strcmp(sub, "play") && jpath.count > 0;
    jpath.start = cls.realtime;
    jpath.hl = -1;
}

// green at LIGHTS shading to orange up to ACTION!, bright red from there;
// bright is the colour's share, under 1 for a route put in the shade
static uint32_t JPath_Color(int frame, int alpha, float bright)
{
    float f = (float)frame / MVP_ACTION_FRAMES;
    int r, g;

    if (f >= 1) {
        r = 255;
        g = 0;
    } else {
        r = 255 * f;
        g = 255 - 95 * f;
    }
    return MakeColor(r * bright, g * bright, 0, alpha);
}

#if USE_REF
/*
jpath_look: a shot drawn as the bullet went - from the eye the way he
looked, to the wall it hit, faint red, or to the player it hit, green.
The world is traced here; a hit is a player of the round within a
body's reach of the line at that frame. The demo has no trace of its
own, so a near miss on the line's edge may show either way.
*/
#define JPATH_SHOT_RANGE    8192
#define JPATH_HIT_RADIUS    20      // a body's: the bounding box is 16 wide, a little more for the frame's lag
#define JPATH_HIT_LOW       (-30)   // from the origin: the box is -24 to 32, and a little more
#define JPATH_HIT_HIGH      34

static void JPath_Shot(const mvp_trail_t *t, int i, const vec3_t at, int alpha, bool depth)
{
    const mvp_sample_t *m = &jpath.demo->samples[t->first + i];
    const mvp_round_t *r = &jpath.demo->rounds[t->round];
    vec3_t angles = { SHORT2ANGLE(m->pitch), SHORT2ANGLE(m->yaw), 0 }, fwd, eye, far, wall, hit;
    trace_t tr;
    float len, best = 0;
    bool got = false;
    int k;

    AngleVectors(angles, fwd, NULL, NULL);
    VectorCopy(at, eye);
    eye[2] += m->viewheight;
    VectorMA(eye, JPATH_SHOT_RANGE, fwd, far);
    CM_BoxTrace(&tr, eye, far, vec3_origin, vec3_origin, cl.bsp->nodes, MASK_SOLID, cl.csr.extended);
    VectorCopy(tr.endpos, wall);
    len = Distance(eye, wall);

    // the round's others where they stood that frame: the nearest on the line
    for (k = r->trail_first; k < r->trail_first + r->trail_count; k++) {
        const mvp_trail_t *o = &jpath.demo->trails[k];
        vec3_t c, d, p;
        float along, dz;

        if (o == t || i >= o->count)
            continue;
        MvdPath_Origin(&jpath.demo->samples[o->first + i], c);
        VectorSubtract(c, eye, d);
        along = DotProduct(d, fwd);
        if (along <= 0 || along > len || (got && along >= best))
            continue;
        VectorMA(eye, along, fwd, p);
        dz = c[2] - p[2];
        if (dz < JPATH_HIT_LOW || dz > JPATH_HIT_HIGH)
            continue;
        if ((c[0] - p[0]) * (c[0] - p[0]) + (c[1] - p[1]) * (c[1] - p[1]) > JPATH_HIT_RADIUS * JPATH_HIT_RADIUS)
            continue;
        got = true;
        best = along;
        VectorCopy(p, hit);
    }
    if (got) {
        R_AddDebugLine(eye, hit, MakeColor(60, 255, 80, alpha), 0, depth);
    } else {
        R_AddDebugLine(eye, wall, MakeColor(255, 60, 60, alpha * 0.35f), 0, depth);
    }
}

static void JPath_Lines(const mvp_trail_t *t, float upto, int alpha, float bright, uint32_t dim, bool depth, float lift)
{
    const mvp_sample_t *s = &jpath.demo->samples[t->first];
    vec3_t a, b, head;
    int i, n = min(t->count, jpath.frames + 1), whole = (int)upto;

    MvdPath_Origin(&s[0], a);
    a[2] += lift;
    R_AddDebugPoint(a, 12, dim ? dim : JPath_Color(0, alpha, bright), 0, depth);
    for (i = 0; i + 1 < n && i < whole; i++) {
        MvdPath_Origin(&s[i + 1], b);
        b[2] += lift;
        R_AddDebugLine(a, b, dim ? dim : JPath_Color(i, alpha, bright), 0, depth);
        // where ACTION! came: a white cross on the line
        if (i + 1 == MVP_ACTION_FRAMES && !dim)
            R_AddDebugPoint(b, 20, MakeColor(255 * bright, 255 * bright, 255 * bright, alpha), 0, depth);
        // his shots, as the bullets went
        if (jpath_look->integer && (s[i].flags & MVP_SHOT) && !dim)
            JPath_Shot(t, i, a, alpha, depth);
        VectorCopy(b, a);
    }
    // the stretch being run, and its head - where he looks right now
    if (i + 1 < n && upto > whole) {
        MvdPath_Origin(&s[i + 1], b);
        b[2] += lift;
        LerpVector(a, b, upto - whole, head);
        R_AddDebugLine(a, head, dim ? dim : JPath_Color(i, alpha, bright), 0, depth);
        R_AddDebugPoint(head, 16, dim ? dim : MakeColor(255, 255, 255, 255), 0, depth);
        if (jpath_look->integer && (s[i].flags & MVP_SHOT) && !dim)
            JPath_Shot(t, i, head, 255, depth);
    }
}

void JPath_AddToScene(void)
{
    float upto = 1e9f;
    bool depth;
    unsigned ms;
    int i;

    if (!jpath.demo || (!jpath.count && !jpath.other_count))
        return;
    if (cls.state != ca_active || Q_stricmp(jpath.demo->map, cl.mapname))
        return;
    depth = jpath_depth->integer != 0;

    if (jpath.playing) {
        // ten samples a second from the start, held whole, and again
        ms = cls.realtime - jpath.start;
        if (ms > (unsigned)jpath.frames * 100 + JPATH_HOLD_MS) {
            jpath.start = cls.realtime;
            ms = 0;
        }
        upto = ms * 0.01f;
    }

    for (i = 0; i < jpath.other_count; i++) {
        const mvp_trail_t *o = &jpath.demo->trails[jpath.others[i]];
        const mvp_trail_t *t = jpath.count ? &jpath.demo->trails[jpath.trails[0]] : NULL;
        uint32_t dim = t && o->team == t->team ? MakeColor(90, 170, 255, 110) : MakeColor(220, 220, 220, 90);

        JPath_Lines(o, upto, 110, 1, dim, depth, jpath_lift->value);
    }
    // the one aimed at lit and drawn twice, a little higher, for a thick
    // line; the rest dimmed while it is
    for (i = 0; i < jpath.count; i++) {
        bool lit = jpath.hl >= 0 && jpath.trails[i] == jpath.hl;
        bool shade = jpath.hl >= 0 && !lit;

        // the rest in the shade while one is lit: darker, and faint
        JPath_Lines(&jpath.demo->trails[jpath.trails[i]], upto, shade ? 70 : 255, shade ? 0.45f : 1,
                    0, depth, jpath_lift->value);
        if (lit)
            JPath_Lines(&jpath.demo->trails[jpath.trails[i]], upto, 255, 1, 0, depth, jpath_lift->value + 2);
    }
}
#else
void JPath_AddToScene(void)
{
}
#endif

static void JPath_Cmd_c(genctx_t *ctx, int argnum)
{
    if (argnum == 1) {
        Prompt_AddMatch(ctx, "show");
        Prompt_AddMatch(ctx, "play");
        Prompt_AddMatch(ctx, "restart");
        Prompt_AddMatch(ctx, "off");
    }
}

static const cmdreg_t c_jpath[] = {
    { "jpath", JPath_Cmd_f, JPath_Cmd_c },
    { NULL }
};

void JPath_Init(void)
{
    jpath_depth = Cvar_Get("jpath_depth", "0", 0);   // through the walls, as the ghosts are drawn
    jpath_look = Cvar_Get("jpath_look", "0", 0);
    jpath_lift = Cvar_Get("jpath_lift", "0", 0);
    Cmd_Register(c_jpath);
}
