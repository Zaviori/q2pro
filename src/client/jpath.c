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

#define JPATH_HOLD_MS       1500    // a played route is held whole this long before it runs again
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

// green at LIGHTS shading to orange up to ACTION!, bright red from there
static uint32_t JPath_Color(int frame, int alpha)
{
    float f = (float)frame / MVP_ACTION_FRAMES;

    if (f >= 1)
        return MakeColor(255, 0, 0, alpha);
    return MakeColor(255 * f, 255 - 95 * f, 0, alpha);
}

#if USE_REF
static void JPath_Lines(const mvp_trail_t *t, float upto, int alpha, uint32_t dim, bool depth, float lift)
{
    const mvp_sample_t *s = &jpath.demo->samples[t->first];
    vec3_t a, b, head;
    int i, n = min(t->count, jpath.frames + 1), whole = (int)upto;

    MvdPath_Origin(&s[0], a);
    a[2] += lift;
    R_AddDebugPoint(a, 12, dim ? dim : JPath_Color(0, alpha), 0, depth);
    for (i = 0; i + 1 < n && i < whole; i++) {
        MvdPath_Origin(&s[i + 1], b);
        b[2] += lift;
        R_AddDebugLine(a, b, dim ? dim : JPath_Color(i, alpha), 0, depth);
        // where ACTION! came: a white cross on the line
        if (i + 1 == MVP_ACTION_FRAMES && !dim)
            R_AddDebugPoint(b, 20, MakeColor(255, 255, 255, alpha), 0, depth);
        VectorCopy(b, a);
    }
    // the stretch being run, and its head
    if (i + 1 < n && upto > whole) {
        MvdPath_Origin(&s[i + 1], b);
        b[2] += lift;
        LerpVector(a, b, upto - whole, head);
        R_AddDebugLine(a, head, dim ? dim : JPath_Color(i, alpha), 0, depth);
        R_AddDebugPoint(head, 16, dim ? dim : MakeColor(255, 255, 255, 255), 0, depth);
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

        JPath_Lines(o, upto, 110, dim, depth, jpath_lift->value);
    }
    // the one aimed at lit and drawn twice, a little higher, for a thick
    // line; the rest dimmed while it is
    for (i = 0; i < jpath.count; i++) {
        bool lit = jpath.hl >= 0 && jpath.trails[i] == jpath.hl;
        int alpha = jpath.hl >= 0 && !lit ? 90 : 255;

        JPath_Lines(&jpath.demo->trails[jpath.trails[i]], upto, alpha, 0, depth, jpath_lift->value);
        if (lit)
            JPath_Lines(&jpath.demo->trails[jpath.trails[i]], upto, 255, 0, depth, jpath_lift->value + 2);
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
        Prompt_AddMatch(ctx, "off");
    }
}

static const cmdreg_t c_jpath[] = {
    { "jpath", JPath_Cmd_f, JPath_Cmd_c },
    { NULL }
};

void JPath_Init(void)
{
    jpath_depth = Cvar_Get("jpath_depth", "1", 0);
    jpath_lift = Cvar_Get("jpath_lift", "0", 0);
    Cmd_Register(c_jpath);
}
