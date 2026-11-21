/*
 * The per-weapon stats channel on the client (doc/weapon-stats.md): its
 * lines kept as state, and the scoreboard drawn from them in place of the
 * server's own - only while the server is sending the channel.
 *
 * Totals: a player's map line holds the rounds that are over; a round
 * being played comes as wlive (to a spectator) and is added on top until
 * the round's end brings new map lines.
 */

#include "client.h"

#define WSC_MAX_P       129     // stats ids 1..128
#define WSC_NW          14      // weapons 1..13
#define WSC_NF          12
#define WSC_MAX_TEAMS   4
#define WSC_HIST_MAX    16384

enum {
    F_SHOTS, F_HITS, F_HS, F_K, F_DMG,
    F_HEAD, F_CHEST, F_STOMACH, F_LEGS, F_HELMET, F_VEST, F_NOLOC
};

static const char *const wsc_fkey[WSC_NF] = {
    "shots", "hits", "hs", "k", "dmg",
    "head", "chest", "stomach", "legs", "helmet", "vest", "noloc"
};

static const char *const wsc_wname[WSC_NW] = {
    "", "MK23", "MP5", "M4", "M3", "Handcannon", "Sniper", "Dual MK23",
    "Knife", "Grenade", "Knife throw", "Kick", "Punch", "Nade impact"
};

// The kill feed's icons (the game's item pics) per weapon, as on the
// stream branch; kick and punch have pics of their own in action/pics,
// the game's slippers standing in for the boot where they are missing
static const char *const wsc_wpic[WSC_NW][2] = {
    { NULL }, { "w_mk23" }, { "w_mp5" }, { "w_m4" }, { "w_super90" }, { "w_cannon" },
    { "w_sniper" }, { "w_akimbo" }, { "w_knife" }, { "a_m61frag" }, { "w_knife" },
    { "hud_boot", "slippers" }, { "hud_fist" }, { "a_m61frag" }
};

typedef struct {
    bool    used;
    int     slot;               // client number, -1 when gone
    char    name[32];
    int     team;
    bool    have_ps;
    int     score, kills, deaths, dmg, ping;
    int     map[WSC_NW][WSC_NF];    // rounds that are over
    int     live[WSC_NW][WSC_NF];   // the round being played, from wlive
    int     live_r;
} wsc_player_t;

typedef struct {
    unsigned char p, r, w;
    int v[WSC_NF];
} wsc_hist_t;

static struct {
    bool            active;     // the server sends the channel: a snapshot came
    char            mapname[MAX_QPATH];
    int             round;
    bool            going;
    int             me;
    wsc_player_t    p[WSC_MAX_P];
    char            team_name[WSC_MAX_TEAMS][32];
    char            team_icon[WSC_MAX_TEAMS][MAX_QPATH];
    int             team_score[WSC_MAX_TEAMS];
    bool            team_seen[WSC_MAX_TEAMS];
    wsc_hist_t      *hist;
    int             nhist, hist_alloc;

    // the board
    bool            open;
    int             cursor;     // stats id under the cursor
    int             expanded;   // stats id whose weapons are shown, 0 none
    int             scope;      // 0 the map, else a round
} wsc;

static cvar_t *cl_statsboard;
static cvar_t *scr_statsboard_x;    // the board's middle, a fraction of the width
static cvar_t *scr_statsboard_y;    // its upper edge, a fraction of the height
#ifdef TEXT_BOLD                // the refresh has TrueType text (the extras branch)
static cvar_t *scr_statsboard_font;
static cvar_t *scr_statsboard_size;
#endif

/* ------------------------------------------------------------------ */
/* state                                                              */

static void wsc_clear_counts(void)
{
    for (int i = 0; i < WSC_MAX_P; i++) {
        memset(wsc.p[i].map, 0, sizeof(wsc.p[i].map));
        memset(wsc.p[i].live, 0, sizeof(wsc.p[i].live));
        wsc.p[i].live_r = -1;
    }
    wsc.nhist = 0;
}

void CL_WS_Clear(void)
{
    wsc_hist_t *hist = wsc.hist;
    int alloc = wsc.hist_alloc;

    memset(&wsc, 0, sizeof(wsc));
    wsc.hist = hist;
    wsc.hist_alloc = alloc;
    wsc_clear_counts();
}

bool CL_WS_Active(void)
{
    return wsc.active && cl_statsboard && cl_statsboard->integer;
}

static void wsc_hist_set(int p, int r, int w, const int *v)
{
    for (int i = 0; i < wsc.nhist; i++) {
        wsc_hist_t *h = &wsc.hist[i];
        if (h->p == p && h->r == r && h->w == w) {
            memcpy(h->v, v, sizeof(h->v));
            return;
        }
    }
    if (wsc.nhist >= WSC_HIST_MAX)
        return;
    if (wsc.nhist == wsc.hist_alloc) {
        int n = wsc.hist_alloc ? wsc.hist_alloc * 2 : 256;
        wsc_hist_t *nh = Z_Malloc(n * sizeof(*nh));
        if (wsc.hist) {
            memcpy(nh, wsc.hist, wsc.nhist * sizeof(*nh));
            Z_Free(wsc.hist);
        }
        wsc.hist = nh;
        wsc.hist_alloc = n;
    }
    wsc_hist_t *h = &wsc.hist[wsc.nhist++];
    h->p = p;
    h->r = r;
    h->w = w;
    memcpy(h->v, v, sizeof(h->v));
}

static const wsc_hist_t *wsc_hist_get(int p, int r, int w)
{
    for (int i = 0; i < wsc.nhist; i++) {
        const wsc_hist_t *h = &wsc.hist[i];
        if (h->p == p && h->r == r && h->w == w)
            return h;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* parsing: "<event> key=value key=\"quoted value\" ..."                */

#define WSC_MAX_KV  24

typedef struct {
    char    event[24];
    int     n;
    char    key[WSC_MAX_KV][16];
    char    val[WSC_MAX_KV][64];
} wsc_line_t;

static bool wsc_tokenize(const char *s, wsc_line_t *l)
{
    size_t n = strcspn(s, " ");

    memset(l, 0, sizeof(*l));
    if (!n || n >= sizeof(l->event))
        return false;
    memcpy(l->event, s, n);
    s += n;

    while (*s && l->n < WSC_MAX_KV) {
        while (*s == ' ')
            s++;
        if (!*s || *s == ';')
            break;      // the rest is a comment
        n = strcspn(s, "= ");
        if (s[n] != '=' || !n || n >= sizeof(l->key[0]))
            return false;
        memcpy(l->key[l->n], s, n);
        s += n + 1;

        char *v = l->val[l->n];
        size_t len = 0;
        if (*s == '"') {
            s++;
            while (*s && *s != '"') {
                if (*s == '\\' && s[1])
                    s++;
                if (len + 1 < sizeof(l->val[0]))
                    v[len++] = *s;
                s++;
            }
            if (*s == '"')
                s++;
        } else {
            while (*s && *s != ' ') {
                if (len + 1 < sizeof(l->val[0]))
                    v[len++] = *s;
                s++;
            }
        }
        v[len] = 0;
        l->n++;
    }
    return true;
}

static const char *wsc_get(const wsc_line_t *l, const char *key)
{
    for (int i = 0; i < l->n; i++)
        if (!strcmp(l->key[i], key))
            return l->val[i];
    return NULL;
}

static int wsc_int(const wsc_line_t *l, const char *key, int def)
{
    const char *v = wsc_get(l, key);
    return v ? atoi(v) : def;
}

static wsc_player_t *wsc_player(const wsc_line_t *l)
{
    int p = wsc_int(l, "p", 0);

    if (p < 1 || p >= WSC_MAX_P)
        return NULL;
    wsc.p[p].used = true;
    return &wsc.p[p];
}

static int wsc_weapon(const wsc_line_t *l)
{
    int w = wsc_int(l, "w", 0);
    return w > 0 && w < WSC_NW ? w : 0;
}

// counters of a line; a counter not on it is zero, or - for a delta - left as is
static void wsc_fields(const wsc_line_t *l, int *v, bool delta)
{
    for (int f = 0; f < WSC_NF; f++) {
        const char *s = wsc_get(l, wsc_fkey[f]);
        if (s)
            v[f] = atoi(s);
        else if (!delta)
            v[f] = 0;
    }
}

static void wsc_take_token(const char *token)
{
    if (token && *token && strlen(token) < 32)
        Cvar_Set("aq2sid", token);
}

void CL_WS_ParseLine(const char *line)
{
    wsc_line_t l;
    wsc_player_t *p;
    int w;

    if (!wsc_tokenize(line, &l))
        return;

    if (!strcmp(l.event, "wscope")) {
        const char *state = wsc_get(&l, "state");
        if (!state) {
            // a snapshot: everything comes again
            bool open = wsc.open;
            int cursor = wsc.cursor, expanded = wsc.expanded;
            CL_WS_Clear();
            wsc.active = true;
            wsc.open = open;
            wsc.cursor = cursor;
            wsc.expanded = expanded;
        } else if (!strcmp(state, "reset")) {
            wsc_clear_counts();
            wsc.scope = 0;
        }
        Q_strlcpy(wsc.mapname, wsc_get(&l, "map") ? wsc_get(&l, "map") : "", sizeof(wsc.mapname));
        wsc.round = wsc_int(&l, "r", wsc.round);
        if (state && !strcmp(state, "end"))
            wsc.going = false;
        else
            wsc.going = wsc_int(&l, "going", wsc.going) != 0;
        return;
    }

    if (!strcmp(l.event, "sid")) {
        wsc.me = wsc_int(&l, "p", 0);
        wsc_take_token(wsc_get(&l, "token"));
        return;
    }

    if (!strcmp(l.event, "sidmap")) {
        if (!(p = wsc_player(&l)))
            return;
        p->slot = wsc_int(&l, "c", -1);
        if (wsc_get(&l, "name"))
            Q_strlcpy(p->name, wsc_get(&l, "name"), sizeof(p->name));
        p->team = wsc_int(&l, "team", p->team);
        return;
    }

    if (!strcmp(l.event, "pstats")) {
        if (!(p = wsc_player(&l)))
            return;
        p->have_ps = true;
        p->score = wsc_int(&l, "score", p->score);
        p->kills = wsc_int(&l, "k", p->kills);
        p->deaths = wsc_int(&l, "d", p->deaths);
        p->dmg = wsc_int(&l, "dmg", p->dmg);
        p->ping = wsc_int(&l, "ping", p->ping);
        return;
    }

    if (!strcmp(l.event, "wteam")) {
        int t = wsc_int(&l, "t", 0);
        if (t < 1 || t >= WSC_MAX_TEAMS)
            return;
        wsc.team_seen[t] = true;
        if (wsc_get(&l, "name"))
            Q_strlcpy(wsc.team_name[t], wsc_get(&l, "name"), sizeof(wsc.team_name[t]));
        wsc.team_score[t] = wsc_int(&l, "score", wsc.team_score[t]);
        if (wsc_get(&l, "icon"))
            Q_strlcpy(wsc.team_icon[t], strcmp(wsc_get(&l, "icon"), "-") ? wsc_get(&l, "icon") : "",
                      sizeof(wsc.team_icon[t]));
        return;
    }

    if (!strcmp(l.event, "wstats")) {
        int v[WSC_NF];
        if (!(p = wsc_player(&l)) || !(w = wsc_weapon(&l)))
            return;
        wsc_fields(&l, v, false);
        const char *scope = wsc_get(&l, "scope");
        if (scope && !strcmp(scope, "map")) {
            memcpy(p->map[w], v, sizeof(v));
        } else {
            int r = wsc_int(&l, "r", 0);
            if (r > 0 && r < 256)
                wsc_hist_set(p - wsc.p, r, w, v);
        }
        return;
    }

    if (!strcmp(l.event, "wlive")) {
        if (!(p = wsc_player(&l)) || !(w = wsc_weapon(&l)))
            return;
        int r = wsc_int(&l, "r", 0);
        if (p->live_r != r) {
            memset(p->live, 0, sizeof(p->live));
            p->live_r = r;
        }
        wsc_fields(&l, p->live[w], true);
        return;
    }

    if (!strcmp(l.event, "wround")) {
        const char *state = wsc_get(&l, "state");
        wsc.round = wsc_int(&l, "r", wsc.round);
        if (state && !strcmp(state, "start")) {
            wsc.going = true;
        } else if (state && !strcmp(state, "end")) {
            // the round's map lines came just before: live is in them now
            wsc.going = false;
            for (int i = 0; i < WSC_MAX_P; i++) {
                memset(wsc.p[i].live, 0, sizeof(wsc.p[i].live));
                wsc.p[i].live_r = -1;
            }
        }
        return;
    }
}

/* ------------------------------------------------------------------ */
/* totals                                                             */

static bool wsc_live_counts(const wsc_player_t *p)
{
    return wsc.going && p->live_r == wsc.round;
}

// a player's counters for one weapon in the board's scope
static bool wsc_weapon_values(int pi, int w, int scope, int *v)
{
    const wsc_player_t *p = &wsc.p[pi];

    if (!scope) {
        for (int f = 0; f < WSC_NF; f++)
            v[f] = p->map[w][f] + (wsc_live_counts(p) ? p->live[w][f] : 0);
    } else if (scope == wsc.round && wsc_live_counts(p)) {
        memcpy(v, p->live[w], sizeof(int) * WSC_NF);
    } else {
        const wsc_hist_t *h = wsc_hist_get(pi, scope, w);
        if (!h)
            return false;
        memcpy(v, h->v, sizeof(int) * WSC_NF);
    }
    for (int f = 0; f < WSC_NF; f++)
        if (v[f])
            return true;
    return false;
}

// shots and hits over every weapon (the map): the board's accuracy
static void wsc_accuracy(int pi, int *shots, int *hits)
{
    int v[WSC_NF];

    *shots = *hits = 0;
    for (int w = 1; w < WSC_NW; w++) {
        // kick, punch and the slash are no aim, and a grenade counts
        // every body it hurts: none of them is accuracy
        if (w == 8 || w == 9 || w == 11 || w == 12 || w == 13)
            continue;
        if (wsc_weapon_values(pi, w, 0, v)) {
            *shots += v[F_SHOTS];
            *hits += v[F_HITS];
        }
    }
}

// the rounds the player has a line for, and the one being played
static int wsc_rounds(int pi, int *out, int max)
{
    int n = 0;

    for (int r = 255; r > 0 && n < max; r--) {
        bool any = r == wsc.round && wsc_live_counts(&wsc.p[pi]);
        for (int w = 1; w < WSC_NW && !any; w++)
            any = wsc_hist_get(pi, r, w) != NULL;
        if (any)
            out[n++] = r;
    }
    return n;      // newest first
}

/* ------------------------------------------------------------------ */
/* the board's player list: teams in order, best score first          */

static int wsc_sort_team;

static int wsc_cmp(const void *a, const void *b)
{
    const wsc_player_t *pa = &wsc.p[*(const int *)a];
    const wsc_player_t *pb = &wsc.p[*(const int *)b];

    if ((pa->slot < 0) != (pb->slot < 0))
        return pa->slot < 0 ? 1 : -1;       // who left goes last
    if (pa->score != pb->score)
        return pb->score - pa->score;
    if (pa->kills != pb->kills)
        return pb->kills - pa->kills;
    return pa->deaths - pb->deaths;
}

static bool wsc_has_stats(const wsc_player_t *p)
{
    if (p->have_ps && (p->score || p->kills || p->deaths || p->dmg))
        return true;
    for (int w = 1; w < WSC_NW; w++)
        for (int f = 0; f < WSC_NF; f++)
            if (p->map[w][f] || p->live[w][f])
                return true;
    return false;
}

static int wsc_team_list(int team, int *out)
{
    int n = 0;

    for (int i = 1; i < WSC_MAX_P; i++) {
        const wsc_player_t *p = &wsc.p[i];
        if (!p->used || p->team != team)
            continue;
        if (p->slot < 0 && !wsc_has_stats(p))
            continue;
        out[n++] = i;
    }
    wsc_sort_team = team;
    qsort(out, n, sizeof(int), wsc_cmp);
    return n;
}

static int wsc_num_teams(void)
{
    int n = 2;
    if (wsc.team_seen[3])
        n = 3;
    for (int i = 1; i < WSC_MAX_P; i++)
        if (wsc.p[i].used && wsc.p[i].team > n && wsc.p[i].team < WSC_MAX_TEAMS)
            n = wsc.p[i].team;
    return n;
}

// every player on a team, in the board's order: the cursor walks this
static int wsc_board_list(int *out)
{
    int n = 0;

    for (int t = 1; t <= wsc_num_teams(); t++)
        n += wsc_team_list(t, out + n);
    return n;
}

/* ------------------------------------------------------------------ */
/* commands and keys                                                  */

static void wsc_fix_cursor(void)
{
    int list[WSC_MAX_P];
    int n = wsc_board_list(list);

    for (int i = 0; i < n; i++)
        if (list[i] == wsc.cursor)
            return;
    wsc.cursor = n ? list[0] : 0;
}

static void wsc_open(bool open)
{
    wsc.open = open;
    if (open) {
        if (!wsc.cursor && wsc.me)
            wsc.cursor = wsc.me;
        wsc_fix_cursor();
    }
}

// "score": the new board where the server sends the channel, else as ever
static void CL_WS_Score_f(void)
{
    if (!CL_WS_Active()) {
        CL_ClientCommand("score");
        return;
    }
    wsc_open(!wsc.open);
}

bool CL_WS_BoardOpen(void)
{
    if (wsc.open && !CL_WS_Active())
        wsc.open = false;
    return wsc.open;
}

static void wsc_move(int step)
{
    int list[WSC_MAX_P];
    int n = wsc_board_list(list), i;

    if (!n)
        return;
    for (i = 0; i < n && list[i] != wsc.cursor; i++)
        ;
    if (i == n)
        i = 0;
    else
        i = (i + step + n) % n;
    wsc.cursor = list[i];
}

// Left goes back in time: map, the newest round, ... the first; right back
static void wsc_step_scope(int dir)
{
    int rounds[256], n, i;

    if (!wsc.expanded)
        return;
    n = wsc_rounds(wsc.expanded, rounds, 256);
    if (!n) {
        wsc.scope = 0;
        return;
    }
    // positions: -1 the map, 0..n-1 the rounds newest first
    for (i = 0; i < n && rounds[i] != wsc.scope; i++)
        ;
    int pos = wsc.scope && i < n ? i : -1;
    pos += dir;
    if (pos < -1)
        pos = n - 1;
    if (pos >= n)
        pos = -1;
    wsc.scope = pos < 0 ? 0 : rounds[pos];
}

// the keys the board takes while it is up; false lets the key through
bool CL_WS_KeyEvent(int key)
{
    if (!CL_WS_BoardOpen())
        return false;

    switch (key) {
    case K_ESCAPE:
        wsc.open = false;
        return true;
    case K_UPARROW:
    case K_KP_UPARROW:
        wsc_move(-1);
        return true;
    case K_DOWNARROW:
    case K_KP_DOWNARROW:
        wsc_move(1);
        return true;
    case K_ENTER:
    case K_KP_ENTER:
        wsc_fix_cursor();
        if (wsc.expanded == wsc.cursor) {
            wsc.expanded = 0;
        } else {
            wsc.expanded = wsc.cursor;
            wsc.scope = 0;
        }
        return true;
    case K_LEFTARROW:
    case K_KP_LEFTARROW:
        if (!wsc.expanded)
            return false;
        wsc_step_scope(1);
        return true;
    case K_RIGHTARROW:
    case K_KP_RIGHTARROW:
        if (!wsc.expanded)
            return false;
        wsc_step_scope(-1);
        return true;
    }
    return false;
}

void CL_WS_Init(void)
{
    cl_statsboard = Cvar_Get("cl_statsboard", "1", 0);
    scr_statsboard_x = Cvar_Get("scr_statsboard_x", "0.5", CVAR_ARCHIVE);
    scr_statsboard_y = Cvar_Get("scr_statsboard_y", "0.25", CVAR_ARCHIVE);
#ifdef TEXT_BOLD
    scr_statsboard_font = Cvar_Get("scr_statsboard_font", "1", CVAR_ARCHIVE);
    scr_statsboard_size = Cvar_Get("scr_statsboard_size", "10", CVAR_ARCHIVE);
#endif
    Cmd_AddCommand("score", CL_WS_Score_f);
    CL_WS_Clear();
}

/* ------------------------------------------------------------------ */
/* drawing                                                            */
/*                                                                    */
/* The board is laid out in conchar cells, as columns. Each field is    */
/* drawn whole at its column - left-aligned at its start, numbers right */
/* aligned at its end - so it reads the same in conchars and, where the */
/* refresh has TrueType text (scr_statsboard_font), in the font.        */

#define CW  CONCHAR_WIDTH
#define CH  CONCHAR_HEIGHT

static qhandle_t wsc_font;
static float wsc_hud_scale;
static bool wsc_ttf;
static float wsc_size;
static int wsc_rh;              // a row

enum { AL_LEFT, AL_RIGHT, AL_CENTER };

typedef struct {
    int start, width, align;    // in cells
} wsc_col_t;

// " %-2s %-16s %5s %5s %6s %4s %6s %5s %4s"
#define ROW_FMT     " %-2s %-*.*s %5s %5s %6s %4s %6s %5s %4s"
#define NAME_W      16
static const wsc_col_t row_cols[] = {
    { 1, 2, AL_LEFT }, { 4, NAME_W, AL_LEFT }, { 21, 5, AL_RIGHT }, { 27, 5, AL_RIGHT },
    { 33, 6, AL_RIGHT }, { 40, 4, AL_RIGHT }, { 45, 6, AL_RIGHT }, { 52, 5, AL_RIGHT },
    { 58, 4, AL_RIGHT },
};
#define ROW_NAME_COL 1

// "   %-11s %5s %4s %4s %3s %3s %5s %4s %5s %4s %4s %4s %4s %5s"
#define WPN_FMT     "   %-11s %5s %4s %4s %3s %3s %5s %4s %5s %4s %4s %4s %4s %5s"
static const wsc_col_t wpn_cols[] = {
    { 3, 11, AL_LEFT }, { 15, 5, AL_RIGHT }, { 21, 4, AL_RIGHT }, { 26, 4, AL_RIGHT },
    { 31, 3, AL_RIGHT }, { 35, 3, AL_RIGHT }, { 39, 5, AL_RIGHT }, { 45, 4, AL_RIGHT },
    { 50, 5, AL_RIGHT }, { 56, 4, AL_RIGHT }, { 61, 4, AL_RIGHT }, { 66, 4, AL_RIGHT },
    { 71, 4, AL_RIGHT }, { 76, 5, AL_RIGHT },
};
#define BOARD_CHARS 81

static int wsc_measure(int bold, const char *s, size_t len)
{
#ifdef TEXT_BOLD
    if (wsc_ttf)
        return R_MeasureText(bold ? TEXT_BOLD : 0, wsc_size, s, len);
#endif
    (void)bold;
    return (int)len * CW;
}

// one string at x, vertically centred in a row starting at y
static void wsc_str(int x, int y, int align, bool bold, uint32_t color, const char *s, size_t len)
{
    if (!len)
        return;
#ifdef TEXT_BOLD
    if (wsc_ttf) {
        int flags = (bold ? TEXT_BOLD : 0) | (align == AL_RIGHT ? TEXT_RIGHT :
                                              align == AL_CENTER ? TEXT_CENTER : 0);
        uint32_t c = color ? color : U32_WHITE;
        int lh = R_TextLineHeight(flags, wsc_size);
        R_DrawText(x, y + (wsc_rh - lh) / 2, flags, wsc_size, c, c, s, len);
        return;
    }
    R_TextConchars(true);       // real conchars here, whatever r_ttf says
#endif
    if (align == AL_RIGHT)
        x -= (int)len * CW;
    else if (align == AL_CENTER)
        x -= (int)len * CW / 2;
    if (color)
        R_SetColor(color);
    R_DrawString(x, y + (wsc_rh - CH) / 2, bold ? UI_ALTCOLOR : 0, len, s, wsc_font);
    if (color)
        R_ClearColor();
#ifdef TEXT_BOLD
    R_TextConchars(false);
#endif
}

static void wsc_text(int x, int y, int align, bool bold, uint32_t color, const char *s)
{
    wsc_str(x, y, align, bold, color, s, strlen(s));
}

// A line printf'ed in cells, drawn field by field at its columns. The
// name column is cut to its width by measure.
static void wsc_grid(int x, int y, bool bold, uint32_t color, const char *line,
                     const wsc_col_t *cols, int ncols, int name_col)
{
    int linelen = (int)strlen(line);

    for (int i = 0; i < ncols; i++) {
        const wsc_col_t *c = &cols[i];
        if (c->start >= linelen)
            break;
        const char *f = line + c->start;
        int len = min(c->width, linelen - c->start);
        while (len && *f == ' ') {
            f++;
            len--;
        }
        while (len && f[len - 1] == ' ')
            len--;
        if (i == name_col) {
            // a whole name, as long as it fits its column
            const char *full = f;
            int room = c->width * CW;
            len = (int)strlen(full);
            while (len > 1 && wsc_measure(bold, full, len) > room)
                len--;
            wsc_str(x + c->start * CW, y, AL_LEFT, bold, color, full, len);
            continue;
        }
        if (c->align == AL_RIGHT)
            wsc_str(x + (c->start + c->width) * CW, y, AL_RIGHT, bold, color, f, len);
        else
            wsc_str(x + c->start * CW, y, AL_LEFT, bold, color, f, len);
    }
}

static uint32_t wsc_team_color(int team, int alpha)
{
    switch (team) {
    case 1:  return MakeColor(190,  55,  55, alpha);
    case 2:  return MakeColor( 55,  90, 190, alpha);
    case 3:  return MakeColor( 55, 180,  70, alpha);
    default: return MakeColor(120, 120, 120, alpha);
    }
}

// double size, for the team names
static void wsc_big_text(int cx, int y, uint32_t color, const char *s)
{
#ifdef TEXT_BOLD
    if (wsc_ttf) {
        R_DrawText(cx, y, TEXT_BOLD | TEXT_OUTLINE | TEXT_CENTER, wsc_size * 2, color, color,
                   s, strlen(s));
        return;
    }
    R_TextConchars(true);
#endif
    // half the scale draws twice the size: the coordinates halve with it
    int x = cx - (int)strlen(s) * CW;
    R_SetScale(wsc_hud_scale * 0.5f);
    R_SetColor(color);
    R_DrawString(x / 2, y / 2, 0, strlen(s), s, wsc_font);
    R_ClearColor();
    R_SetScale(wsc_hud_scale);
#ifdef TEXT_BOLD
    R_TextConchars(false);
#endif
}

// the HUD's big digits, 16x24
static int wsc_draw_number(int x, int y, int value)
{
    char buf[16];
    int n = Q_snprintf(buf, sizeof(buf), "%d", value);

    for (int i = 0; i < n; i++) {
        qhandle_t pic = buf[i] == '-' ? R_RegisterPic("num_minus") :
                        R_RegisterPic(va("num_%c", buf[i]));
        if (pic)
            R_DrawPic(x + i * 16, y, pic);
    }
    return n * 16;
}

static void wsc_fmt_acc(char *out, size_t size, int hits, int shots)
{
    if (shots > 0)
        Q_snprintf(out, size, "%d%%", hits * 100 / shots);
    else
        Q_strlcpy(out, "-", size);
}

static int wsc_draw_header(int px, int pw, int y)
{
    int mid = px + pw / 2;

    for (int t = 1; t <= 2; t++) {
        const char *name = wsc.team_name[t][0] ? wsc.team_name[t] : va("Team %d", t);
        char cut[17];
        Q_strlcpy(cut, name, sizeof(cut));
        wsc_big_text(t == 1 ? mid - pw / 4 : mid + pw / 4, y, wsc_team_color(t, 255), cut);
    }
    y += CH * 2 + 4;

    // team icons beside the big scores, the scores hugging the middle
    for (int t = 1; t <= 2; t++) {
        char buf[16];
        int sw = Q_snprintf(buf, sizeof(buf), "%d", wsc.team_score[t]) * 16;
        int sx = t == 1 ? mid - CW * 2 - sw : mid + CW * 2;
        wsc_draw_number(sx, y, wsc.team_score[t]);

        qhandle_t icon = wsc.team_icon[t][0] ? R_RegisterPic(wsc.team_icon[t]) : 0;
        if (icon) {
            int iw, ih;
            R_GetPicSize(&iw, &ih, icon);
            // the server's own board draws them 24 high, as the digits
            int dw = ih > 0 ? iw * 24 / ih : 24;
            int ix = t == 1 ? sx - CW - dw : sx + sw + CW;
            R_DrawStretchPic(ix, y, dw, 24, icon);
        }
    }
    wsc_text(mid, y + (24 - wsc_rh) / 2, AL_CENTER, false, MakeColor(200, 200, 200, 255), "-");
    y += 24 + 4;

    char info[64];
    if (wsc.round)
        Q_snprintf(info, sizeof(info), "%s - round %d%s", wsc.mapname, wsc.round,
                   wsc.going ? "" : " over");
    else
        Q_strlcpy(info, wsc.mapname, sizeof(info));
    wsc_text(mid, y, AL_CENTER, false, MakeColor(170, 170, 170, 255), info);
    return y + wsc_rh + 4;
}

// a weapon's icon, a row tall at its own aspect, at most two cells wide
static void wsc_draw_icon(int x, int y, int wi)
{
    qhandle_t pic = 0;
    int iw, ih, h = wsc_rh - 2;

    for (int i = 0; i < 2 && !pic && wsc_wpic[wi][i]; i++)
        pic = R_RegisterPic(wsc_wpic[wi][i]);
    if (!pic)
        return;
    R_GetPicSize(&iw, &ih, pic);
    if (iw <= 0 || ih <= 0)
        return;
    int dw = iw * h / ih;
    if (dw > CW * 2) {
        h = h * CW * 2 / dw;
        dw = CW * 2;
    }
    R_DrawStretchPic(x + (CW * 2 - dw) / 2, y + (wsc_rh - h) / 2, dw, h, pic);
}

static int wsc_draw_weapons(int x, int y, int w, int bottom, int pi)
{
    char line[160], scope[48];
    int rounds[256];
    int nr = wsc_rounds(pi, rounds, 256);
    uint32_t cap = MakeColor(185, 185, 185, 235);

    R_DrawFill32(x, y, w, wsc_rh * 2 + 2, MakeColor(20, 20, 30, 200));

    if (!wsc.scope)
        Q_strlcpy(scope, "MAP TOTAL", sizeof(scope));
    else
        Q_snprintf(scope, sizeof(scope), "ROUND %d%s", wsc.scope,
                   wsc.scope == wsc.round && wsc.going ? " (live)" : "");
    Q_snprintf(line, sizeof(line), "%s%s%s", nr ? "< " : "", scope, nr ? " >" : "");
    wsc_text(x + 3 * CW, y + 1, AL_LEFT, true, MakeColor(230, 230, 120, 255), line);
    y += wsc_rh;

    Q_snprintf(line, sizeof(line), WPN_FMT, "WEAPON", "SHOTS", "HITS", "ACC", "HS", "K",
               "DMG", "HEAD", "CHEST", "STOM", "LEGS", "HELM", "VEST", "NOLOC");
    wsc_grid(x, y + 1, true, cap, line, wpn_cols, q_countof(wpn_cols), -1);
    y += wsc_rh;

    int rows = 0, v[WSC_NF];
    for (int wi = 1; wi < WSC_NW && y + wsc_rh <= bottom; wi++) {
        if (!wsc_weapon_values(pi, wi, wsc.scope, v))
            continue;
        char f[WSC_NF][12], acc[8];
        for (int k = 0; k < WSC_NF; k++)
            Q_snprintf(f[k], sizeof(f[k]), "%d", v[k]);
        // kick, punch and the slash are no aim: no accuracy
        if (wi == 8 || wi == 11 || wi == 12)
            Q_strlcpy(acc, "-", sizeof(acc));
        else
            wsc_fmt_acc(acc, sizeof(acc), v[F_HITS], v[F_SHOTS]);
        Q_snprintf(line, sizeof(line), WPN_FMT, wsc_wname[wi], f[F_SHOTS], f[F_HITS], acc,
                   f[F_HS], f[F_K], f[F_DMG], f[F_HEAD], f[F_CHEST], f[F_STOMACH],
                   f[F_LEGS], f[F_HELMET], f[F_VEST], f[F_NOLOC]);
        R_DrawFill32(x, y, w, wsc_rh, MakeColor(20, 20, 30, rows & 1 ? 200 : 170));
        wsc_draw_icon(x + CW / 2, y, wi);
        wsc_grid(x, y, false, 0, line, wpn_cols, q_countof(wpn_cols), -1);
        y += wsc_rh;
        rows++;
    }
    if (!rows && y + wsc_rh <= bottom) {
        R_DrawFill32(x, y, w, wsc_rh, MakeColor(20, 20, 30, 170));
        wsc_text(x + 3 * CW, y, AL_LEFT, false, MakeColor(150, 150, 150, 255), "no shots");
        y += wsc_rh;
    }
    return y + 3;
}

static int wsc_draw_row(int x, int y, int w, int rank, int pi, bool alt)
{
    const wsc_player_t *p = &wsc.p[pi];
    bool selected = pi == wsc.cursor;
    bool gone = p->slot < 0;
    char line[160], num[8][12], acc[8], kd[12];
    int shots, hits;

    uint32_t bg = selected ? MakeColor(60, 60, 140, 200) :
                  wsc_team_color(p->team, alt ? 60 : 40);
    R_DrawFill32(x, y, w, wsc_rh, bg);
    if (pi == wsc.me && !selected)
        R_DrawFill32(x, y, 2, wsc_rh, MakeColor(230, 230, 120, 255));

    wsc_accuracy(pi, &shots, &hits);
    wsc_fmt_acc(acc, sizeof(acc), hits, shots);
    float ratio = p->deaths > 0 ? (float)p->kills / p->deaths : (float)p->kills;
    Q_snprintf(kd, sizeof(kd), ratio >= 100.0f ? "%.0f" : "%.2f", ratio);
    Q_snprintf(num[0], sizeof(num[0]), "%d", rank);
    Q_snprintf(num[1], sizeof(num[1]), "%d", p->score);
    Q_snprintf(num[2], sizeof(num[2]), "%d", p->kills);
    Q_snprintf(num[3], sizeof(num[3]), "%d", p->deaths);
    Q_snprintf(num[4], sizeof(num[4]), "%d", p->dmg);
    if (gone)
        Q_strlcpy(num[5], "left", sizeof(num[5]));
    else
        Q_snprintf(num[5], sizeof(num[5]), "%d", p->ping);

    // the name goes in whole: the grid cuts it to its column by measure
    Q_snprintf(line, sizeof(line), ROW_FMT, num[0], NAME_W, NAME_W, "",
               num[1], num[2], num[3], acc, num[4], kd, num[5]);
    uint32_t color = gone && !selected ? MakeColor(150, 150, 150, 255) : 0;
    wsc_grid(x, y, false, color, line, row_cols, q_countof(row_cols), -1);
    {
        const char *name = p->name[0] ? p->name : "(unnamed)";
        int len = (int)strlen(name), room = NAME_W * CW;
        while (len > 1 && wsc_measure(false, name, len) > room)
            len--;
        wsc_str(x + row_cols[ROW_NAME_COL].start * CW, y, AL_LEFT, false, color, name, len);
    }

    // the expand mark at the row's end
    wsc_text(x + w - CW, y, AL_RIGHT, false, MakeColor(170, 170, 170, 255),
             wsc.expanded == pi ? "-" : "+");
    return y + wsc_rh;
}

void CL_WS_DrawBoard(int hud_x, int hud_y, int hud_w, int hud_h, float hud_scale, qhandle_t font)
{
    int list[WSC_MAX_P];
    char line[160];

    if (!CL_WS_BoardOpen())
        return;
    wsc_font = font;
    wsc_hud_scale = hud_scale;
    wsc_ttf = false;
    wsc_size = CH;
    wsc_rh = CH + 2;
#ifdef TEXT_BOLD
    if (scr_statsboard_font && scr_statsboard_font->integer && R_TextAvailable()) {
        wsc_ttf = true;
        wsc_size = Cvar_ClampValue(scr_statsboard_size, 6, 24);
        wsc_rh = max(CH + 2, R_TextLineHeight(TEXT_BOLD, wsc_size) + 1);
    }
#endif
    wsc_fix_cursor();

    // height: the header, each team's band, caption and rows, the
    // expanded weapons, spectators and the footer
    int nteams = wsc_num_teams();
    int h = CH * 2 + 4 + 24 + 4 + wsc_rh + 4;
    for (int t = 1; t <= nteams; t++) {
        int n = wsc_team_list(t, list);
        h += wsc_rh + 3 + wsc_rh + n * wsc_rh + CH / 2;
        for (int i = 0; i < n; i++) {
            if (list[i] == wsc.expanded) {
                int v[WSC_NF], rows = 0;
                for (int w = 1; w < WSC_NW; w++)
                    rows += wsc_weapon_values(list[i], w, wsc.scope, v);
                h += wsc_rh * (2 + max(rows, 1)) + 3;
            }
        }
    }
    h += wsc_rh * 3;

    int pw = min(BOARD_CHARS * CW + CW * 2, (int)(hud_w * 0.96f));
    int ph = min(h + CH, (int)(hud_h * 0.94f));
    // where scr_statsboard_x/_y put it, moved back on screen if it would
    // leave it: a tall board climbs rather than being cut at the bottom
    int px = hud_x + (int)(hud_w * Cvar_ClampValue(scr_statsboard_x, 0, 1)) - pw / 2;
    int py = hud_y + (int)(hud_h * Cvar_ClampValue(scr_statsboard_y, 0, 1));
    px = Q_clip(px, hud_x, hud_x + hud_w - pw);
    py = Q_clip(py, hud_y, hud_y + hud_h - ph);
    int bottom = py + ph - wsc_rh * 2;

    R_DrawFill32(px, py, pw, ph, MakeColor(0, 0, 0, 215));

    int x = px + CW, w = pw - CW * 2;
    int y = wsc_draw_header(px, pw, py + CH / 2);
    uint32_t cap = MakeColor(185, 185, 185, 235);

    for (int t = 1; t <= nteams && y + wsc_rh * 3 <= bottom; t++) {
        int n = wsc_team_list(t, list);
        const char *tname = wsc.team_name[t][0] ? wsc.team_name[t] : va("Team %d", t);

        R_DrawFill32(x, y, w, wsc_rh + 2, wsc_team_color(t, 150));
        wsc_text(x + CW, y + 1, AL_LEFT, true, U32_WHITE, tname);
        y += wsc_rh + 3;

        Q_snprintf(line, sizeof(line), ROW_FMT, "#", NAME_W, NAME_W, "PLAYER",
                   "SCORE", "KILLS", "DEATHS", "ACC", "DMG", "K/D", "PING");
        wsc_grid(x, y, true, cap, line, row_cols, q_countof(row_cols), -1);
        y += wsc_rh;

        for (int i = 0; i < n && y + wsc_rh <= bottom; i++) {
            y = wsc_draw_row(x, y, w, i + 1, list[i], i & 1);
            if (list[i] == wsc.expanded)
                y = wsc_draw_weapons(x, y, w, bottom, list[i]);
        }
        y += CH / 2;
    }

    // spectators on one line, cut to the board
    size_t len = Q_strlcpy(line, "Spectators:", sizeof(line));
    bool any = false;
    for (int i = 1; i < WSC_MAX_P && len < sizeof(line); i++) {
        const wsc_player_t *p = &wsc.p[i];
        if (p->used && p->slot >= 0 && p->team == 0) {
            len += Q_snprintf(line + len, sizeof(line) - len, " %s", p->name);
            any = true;
        }
    }
    if (any && y + wsc_rh <= bottom) {
        len = min(strlen(line), sizeof(line) - 1);
        while (len > 1 && wsc_measure(false, line, len) > w)
            len--;
        wsc_str(x, y, AL_LEFT, false, MakeColor(150, 150, 150, 220), line, len);
    }

    wsc_text(x, py + ph - wsc_rh - CH / 2, AL_LEFT, false, MakeColor(150, 150, 150, 220),
             wsc.expanded ? "[UP/DOWN] Player   [ENTER] Close stats   [LEFT/RIGHT] Map/rounds   [ESC] Close"
                          : "[UP/DOWN] Player   [ENTER] Weapon stats   [ESC] Close");
}
