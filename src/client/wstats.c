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
    { "w_sniper" }, { "w_akimbo" },
    { "w_knife_slash", "w_knife" },     // ours: w_knife nearly level, as held to stab
    { "a_m61frag" },
    { "w_knife_throw", "w_knife" },     // ours: w_knife steep, as held to throw
    { "hud_boot", "slippers" }, { "hud_fist" }, { "a_m61frag" }
};

typedef struct {
    bool    used;
    int     slot;               // client number, -1 when gone
    char    name[32];
    int     team;
    bool    sub;                // a substitute for the team (matchmode)
    bool    have_ps;
    int     score, kills, deaths, dmg, ping;
    int     minutes;            // on the server, for the list page
    bool    bot;                // the server's list shows "BOT" for the ping
    bool    alive;
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
    bool            alive_shown;    // the server sent who is alive: we may know
    bool            hist_loading;   // the round history is still coming
    int             page;       // 1 the team board, 2 the list (the server's second page)
    wsc_player_t    p[WSC_MAX_P];
    char            team_name[WSC_MAX_TEAMS][32];
    char            team_icon[WSC_MAX_TEAMS][MAX_QPATH];
    int             team_score[WSC_MAX_TEAMS];
    int             team_ready[WSC_MAX_TEAMS];  // -1 not sent (no matchmode), 0, 1
    int             team_captain[WSC_MAX_TEAMS];    // the captain's stats id, 0 none
    bool            team_seen[WSC_MAX_TEAMS];
    wsc_hist_t      *hist;
    int             nhist, hist_alloc;

    // the board
    bool            open;
    int             cursor;     // stats id under the cursor
    bool            cursor_shown;   // the arrows moved it since the board opened
    int             expanded;   // stats id whose weapons are shown, 0 none
    int             scope;      // 0 the map, else a round
} wsc;

static cvar_t *cl_statsboard;
static cvar_t *scr_statslist_weapons;   // the list page's kills per weapon, icons over them
static cvar_t *scr_statsboard_x;    // the board's middle, a fraction of the width
static cvar_t *scr_statsboard_y;    // its upper edge, a fraction of the height
static cvar_t *scr_statsboard_style;    // 0 the dark board, 1 the classic two columns,
                                        // 2 the server's own board replicated, in conchars
static cvar_t *scr_statsboard_alpha;    // the backgrounds' opacity, 0..1
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
    for (int t = 0; t < WSC_MAX_TEAMS; t++)
        wsc.team_ready[t] = -1;     // until a wteam says
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
            int cursor = wsc.cursor, expanded = wsc.expanded, page = wsc.page;
            bool shown = wsc.cursor_shown;
            CL_WS_Clear();
            wsc.active = true;
            wsc.open = open;
            wsc.page = page;
            wsc.cursor_shown = shown;
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

    // the backfill of past rounds: started, and done (an older server
    // sends no start, the demo neither: no spinner then)
    if (!strcmp(l.event, "wstats_hist")) {
        const char *state = wsc_get(&l, "state");
        if (state)
            wsc.hist_loading = !strcmp(state, "start");
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
        p->sub = wsc_int(&l, "sub", 0) != 0;
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
        p->minutes = wsc_int(&l, "min", p->minutes);
        p->bot = wsc_int(&l, "bot", p->bot) != 0;
        return;
    }

    if (!strcmp(l.event, "alive")) {
        const char *state = wsc_get(&l, "state");
        if (state) {
            wsc.alive_shown = !strcmp(state, "list");
            return;
        }
        if (!(p = wsc_player(&l)))
            return;
        p->alive = wsc_int(&l, "a", 0) != 0;
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
        wsc.team_ready[t] = wsc_int(&l, "ready", -1);
        wsc.team_captain[t] = wsc_int(&l, "captain", 0);
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

// shots and hits over every weapon (the map): the list's accuracy
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

    if (pa->sub != pb->sub)
        return pa->sub ? 1 : -1;            // the subs after the players
    if ((pa->slot < 0) != (pb->slot < 0))
        return pa->slot < 0 ? 1 : -1;       // who left goes last
    // then as the server's own board (G_PlayerCmp): score, fewer deaths,
    // more damage, the client slot
    if (pa->score != pb->score)
        return pb->score - pa->score;
    if (pa->deaths != pb->deaths)
        return pa->deaths - pb->deaths;
    if (pa->dmg != pb->dmg)
        return pb->dmg - pa->dmg;
    if (pa->slot != pb->slot)
        return pa->slot - pb->slot;
    return *(const int *)a - *(const int *)b;
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
// The list page's order, as the server's (G_PlayerCmp): everyone
// connected, spectators too - score, fewer deaths, more damage, the slot
static int wsc_cmp_list(const void *a, const void *b)
{
    const wsc_player_t *pa = &wsc.p[*(const int *)a];
    const wsc_player_t *pb = &wsc.p[*(const int *)b];

    if (pa->score != pb->score)
        return pb->score - pa->score;
    if (pa->deaths != pb->deaths)
        return pa->deaths - pb->deaths;
    if (pa->dmg != pb->dmg)
        return pb->dmg - pa->dmg;
    return pa->slot - pb->slot;
}

static int wsc_list_page(int *out)
{
    int n = 0;

    for (int i = 1; i < WSC_MAX_P; i++)
        if (wsc.p[i].used && wsc.p[i].slot >= 0)
            out[n++] = i;
    qsort(out, n, sizeof(int), wsc_cmp_list);
    return n;
}

static int wsc_board_list(int *out)
{
    int n = 0;

    if (wsc.page == 2)
        return wsc_list_page(out);

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
        // as the server's board: no bar until the arrows, no weapon table
        // (and with it no key hints) until Enter
        wsc.cursor_shown = false;
        wsc.expanded = 0;
        wsc.scope = 0;
        // every opening starts on you, wherever the arrows left it -
        // alive or dead (a spectator, not on the board, starts on its first)
        if (wsc.me)
            wsc.cursor = wsc.me;
        wsc_fix_cursor();
    }
}

// "score": the server's own cycle - its team board, its second page (the
// list: frags, time, ping...), closed - with ours as the first page where
// the server sends the channel
static void CL_WS_Score_f(void)
{
    if (!CL_WS_Active()) {
        CL_ClientCommand("score");
        return;
    }
    int layouts = cl.frame.ps.stats[STAT_LAYOUTS];
    // intermission: the server's board, which it keeps up itself
    if (layouts & LAYOUTS_INTERMISSION) {
        CL_ClientCommand("score");
        return;
    }
    // a layout of the server's up: put away, and the cycle goes on as
    // the server's own would from it - its team board (the big scores'
    // num widgets, or the new board's "Frg Tim Png") to the list page,
    // its list (the "Player" caption) closed, a menu to the team board
    if (layouts & LAYOUTS_LAYOUT) {
        int next = strstr(cl.layout, " num ") || strstr(cl.layout, "Frg Tim Png") ? 2 :
                   strstr(cl.layout, "Player") ? 0 : 1;
        // the list page is the server's own layout's (2) only: the dark
        // and classic boards have the frags and the weapons in their rows
        if (next == 2 && scr_statsboard_style->integer != 2)
            next = 0;
        CL_ClientCommand("putaway");
        if (next) {
            wsc.page = next;
            wsc_open(true);
        } else {
            wsc.open = false;
        }
        return;
    }
    // the server's cycle, both pages ours: the team board, the list,
    // closed - on the server's own layout (2); the dark and classic
    // boards open and close, their rows having what the list adds
    if (wsc.open && wsc.page == 1 && scr_statsboard_style->integer == 2) {
        wsc.page = 2;
        wsc_open(true);
        return;
    }
    if (wsc.open) {
        wsc.open = false;
        return;
    }
    wsc.page = 1;
    wsc_open(true);
}

bool CL_WS_BoardOpen(void)
{
    // The server opening something of its own - a menu (TAB, inven), the
    // inventory, its scoreboard at intermission - puts the board away, as
    // its layout replaced its own scoreboard before: drawn instead of the
    // layout, the board would hide the menu. Only on the moment it opens,
    // so a menu already up when the board was opened leaves it be.
    static int last_layouts;
    int layouts = cl.frame.ps.stats[STAT_LAYOUTS] & (LAYOUTS_LAYOUT | LAYOUTS_INVENTORY);

    if (wsc.open && (layouts & ~last_layouts))
        wsc.open = false;
    last_layouts = layouts;

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
    wsc.cursor_shown = true;
    // the weapons open follow the cursor, in the scope they were in
    if (wsc.expanded)
        wsc.expanded = wsc.cursor;
}

// Left/Right: the next team's column, at the same place in its list (or
// its last), the bar shown; an open weapon table follows
static void wsc_team_step(int dir)
{
    int list[WSC_MAX_P], n, i, t, nteams = min(wsc_num_teams(), WSC_MAX_TEAMS - 1);

    wsc_fix_cursor();
    if (!wsc.cursor)
        return;
    t = wsc.p[wsc.cursor].team;
    if (t < 1 || t > nteams)
        return;
    n = wsc_team_list(t, list);
    for (i = 0; i < n && list[i] != wsc.cursor; i++)
        ;
    for (int k = 1; k < nteams; k++) {
        int nt = (t - 1 + dir * k + nteams * k) % nteams + 1;
        int m = wsc_team_list(nt, list);
        if (m) {
            wsc.cursor = list[min(i, m - 1)];
            wsc.cursor_shown = true;
            if (wsc.expanded)
                wsc.expanded = wsc.cursor;
            return;
        }
    }
    wsc.cursor_shown = true;    // no other team with players: just the bar
}

// PgUp goes back in time: map, the newest round, ... the first; right back
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
    // the arrows only once Enter has shown the bar; before that they go
    // through, to whatever they are bound
    case K_UPARROW:
    case K_KP_UPARROW:
        if (!wsc.cursor_shown)
            return false;
        wsc_move(-1);
        return true;
    case K_DOWNARROW:
    case K_KP_DOWNARROW:
        if (!wsc.cursor_shown)
            return false;
        wsc_move(1);
        return true;
    case K_ENTER:
    case K_KP_ENTER:
        // the first press only shows the bar - on you, where the cursor
        // starts - so the next one opens whose it marks
        if (!wsc.cursor_shown) {
            wsc_fix_cursor();
            wsc.cursor_shown = true;
            return true;
        }
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
        if (!wsc.cursor_shown || wsc.page == 2)
            return false;
        wsc_team_step(-1);
        return true;
    case K_RIGHTARROW:
    case K_KP_RIGHTARROW:
        if (!wsc.cursor_shown || wsc.page == 2)
            return false;
        wsc_team_step(1);
        return true;
    // the open table's map total and rounds: up goes back in time
    case K_PGUP:
    case K_KP_PGUP:
        if (!wsc.expanded)
            return false;
        wsc_step_scope(1);
        return true;
    case K_PGDN:
    case K_KP_PGDN:
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
    scr_statslist_weapons = Cvar_Get("scr_statslist_weapons", "1", CVAR_ARCHIVE);
    scr_statsboard_x = Cvar_Get("scr_statsboard_x", "0.5", CVAR_ARCHIVE);
    scr_statsboard_y = Cvar_Get("scr_statsboard_y", "0.1", CVAR_ARCHIVE);
    scr_statsboard_style = Cvar_Get("scr_statsboard_style", "0", CVAR_ARCHIVE);
    scr_statsboard_alpha = Cvar_Get("scr_statsboard_alpha", "0.85", CVAR_ARCHIVE);
#ifdef TEXT_BOLD
    scr_statsboard_font = Cvar_Get("scr_statsboard_font", "1", CVAR_ARCHIVE);
    // 0 picks the layout's own: 16 on the classic board - about the
    // conchars' size, the letters a little taller and the words narrower,
    // being proportional - and 10 on the dark one, which has twice the
    // rows to fit. Any other value sets both.
    scr_statsboard_size = Cvar_Get("scr_statsboard_size", "0", CVAR_ARCHIVE);
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

#define WSC_DIM     MakeColor(150, 150, 150, 255)
#define WSC_STRIPE  MakeColor(0, 0, 0, 70)
#define WSC_CURSOR  MakeColor(255, 255, 255, 56)

static qhandle_t wsc_font;
static float wsc_hud_scale;
static bool wsc_ttf;
static float wsc_size;
static int wsc_rh;              // a row
static float wsc_bg_k;          // scr_statsboard_alpha against the dark board's own 215

// A background: its alpha scaled by scr_statsboard_alpha, so 0.85 is the
// boards as they were made (the dark board's 215), 1 opaque and 0 only the
// text and the accents (the team rules, your own marker) left
static void wsc_fill(int x, int y, int w, int h, uint32_t c)
{
    color_t col = { .u32 = c };
    int a = (int)(col.u8[3] * wsc_bg_k + 0.5f);

    if (a <= 0)
        return;
    col.u8[3] = min(a, 255);
    R_DrawFill32(x, y, w, h, col.u32);
}
static bool wsc_alt_bold;       // bold in conchars is the alt charset: the dark board's
static bool wsc_clear;          // the classic board see-through: light letters, dark shadow

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

// The weapon table: the name, then these columns in this order, each a
// cell apart and right-aligned. The optional ones (a helmet, and the
// shotgun's and handcannon's no-location hits) only when the table has
// a count in them.
#define WPN_NAME_X  3
#define WPN_NAME_W  11
#define WC_ACC      -1
static const struct {
    const char  *caption;
    int         width;
    int         field;          // a counter, or WC_ACC
    bool        optional;
} wpn_table[] = {
    // a cell wider than their captions, HS and K aside: at the font's
    // larger sizes the captions of neighbours ran into each other
    { "SHOTS", 6, F_SHOTS }, { "HITS", 5, F_HITS }, { "ACC", 5, WC_ACC },
    { "HS", 3, F_HS }, { "K", 3, F_K }, { "DMG", 6, F_DMG },
    { "HEAD", 5, F_HEAD }, { "HELM", 5, F_HELMET, true }, { "CHEST", 6, F_CHEST },
    { "VEST", 5, F_VEST }, { "STOM", 5, F_STOMACH }, { "LEGS", 5, F_LEGS },
    { "NOLOC", 6, F_NOLOC, true },
};
#define BOARD_CHARS 92  // the weapon table with every column

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
        if (wsc_clear)
            flags |= TEXT_SHADOW;
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
    R_DrawString(x, y + (wsc_rh - CH) / 2, bold && wsc_alt_bold ? UI_ALTCOLOR : 0, len, s, wsc_font);
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

// the height of the double size text
static int wsc_big_h(void)
{
#ifdef TEXT_BOLD
    if (wsc_ttf)
        return max(CH * 2, R_TextLineHeight(TEXT_BOLD | TEXT_OUTLINE, wsc_size * 2));
#endif
    return CH * 2;
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
    y += wsc_big_h() + 4;

    // matchmode: a team that is not ready says so under its name
    if (wsc.team_ready[1] == 0 || wsc.team_ready[2] == 0) {
        for (int t = 1; t <= 2; t++)
            if (wsc.team_ready[t] == 0)
                wsc_text(t == 1 ? mid - pw / 4 : mid + pw / 4, y, AL_CENTER, false,
                         MakeColor(255, 170, 60, 255), "NOT READY");
        y += wsc_rh + 2;
    }

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

// The icons are 96x96 art drawn a row tall: as plain pics - no mip chain,
// and nearest filtering by default (gl_bilerp_pics 0) - that skips pixels
// and looks blocky. They are loaded mipmapped and trilinear instead, as a
// sprite: the type keeps this copy apart from the HUD's own of the same
// pic, which the image cache would otherwise hand back unchanged. The png
// first, the pcx where there is none.
static qhandle_t wsc_icon_pic(const char *name)
{
    qhandle_t pic = R_RegisterImage(va("pics/%s.png", name), IT_SPRITE,
                                    IF_PERMANENT | IF_OPTIONAL | IF_MIPMAP);
    if (!pic)
        pic = R_RegisterImage(va("pics/%s.pcx", name), IT_SPRITE,
                              IF_PERMANENT | IF_OPTIONAL | IF_MIPMAP);
    return pic;
}

// a weapon's icon, a row tall at its own aspect, at most two cells wide
static void wsc_draw_icon(int x, int y, int wi)
{
    qhandle_t pic = 0;
    int iw, ih, h = wsc_rh - 2;

    for (int i = 0; i < 2 && !pic && wsc_wpic[wi][i]; i++)
        pic = wsc_icon_pic(wsc_wpic[wi][i]);
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

// the colours of the weapon table, the one part both boards draw
typedef struct {
    uint32_t    text, caption, dim, scope, alive, ready, unready;
    uint32_t    head_bg, row_bg[2];
} wsc_theme_t;

static const wsc_theme_t wsc_dark = {
    .text = U32_WHITE,
    .caption = MakeColor(185, 185, 185, 235),
    .dim = MakeColor(150, 150, 150, 255),
    .scope = MakeColor(230, 230, 120, 255),
    .alive = MakeColor(120, 230, 120, 255),
    .ready = MakeColor(120, 230, 120, 255),
    .unready = MakeColor(255, 170, 60, 255),
    .head_bg = MakeColor(20, 20, 30, 200),
    .row_bg = { MakeColor(20, 20, 30, 170), MakeColor(20, 20, 30, 200) },
};

// `who` (the classic board, where the table is not under the player's row)
// names whose weapons these are at the head of the table
static int wsc_draw_weapons(int x, int y, int w, int bottom, int pi, const wsc_theme_t *th,
                            const char *who, uint32_t who_color)
{
    char line[160], scope[48];
    int rounds[256];
    int nr = wsc_rounds(pi, rounds, 256);
    uint32_t cap = th->caption;

    wsc_fill(x, y, w, wsc_rh * 2 + 2, th->head_bg);

    if (!wsc.scope)
        Q_strlcpy(scope, "MAP TOTAL", sizeof(scope));
    else
        Q_snprintf(scope, sizeof(scope), "ROUND %d%s", wsc.scope,
                   wsc.scope == wsc.round && wsc.going ? " (live)" : "");
    Q_snprintf(line, sizeof(line), "%s%s%s", nr ? "< " : "", scope, nr ? " >" : "");
    int lx = x + 3 * CW;
    if (who) {
        wsc_text(lx, y + 1, AL_LEFT, true, who_color, who);
        lx += wsc_measure(true, who, strlen(who)) + CW * 3;
    }
    wsc_text(lx, y + 1, AL_LEFT, true, th->scope, line);
    // the rounds still arriving: PgUp/PgDn reach only as far back as came
    if (wsc.hist_loading) {
        static const char spin[] = "|/-\\";
        char busy[32];
        Q_snprintf(busy, sizeof(busy), "loading rounds %c", spin[(cls.realtime / 120) % 4]);
        wsc_text(lx + wsc_measure(true, line, strlen(line)) + CW * 3, y + 1, AL_LEFT, false,
                 th->dim, busy);
    }
    y += wsc_rh;

    // which columns: the optional ones where some weapon has a count
    int v[WSC_NF], ends[q_countof(wpn_table)];
    bool show[q_countof(wpn_table)];
    for (int c = 0; c < (int)q_countof(wpn_table); c++)
        show[c] = !wpn_table[c].optional;
    for (int wi = 1; wi < WSC_NW; wi++) {
        if (!wsc_weapon_values(pi, wi, wsc.scope, v))
            continue;
        for (int c = 0; c < (int)q_countof(wpn_table); c++)
            if (wpn_table[c].field >= 0 && v[wpn_table[c].field])
                show[c] = true;
    }
    int col = WPN_NAME_X + WPN_NAME_W;
    for (int c = 0; c < (int)q_countof(wpn_table); c++) {
        if (show[c])
            col += 1 + wpn_table[c].width;
        ends[c] = col;
    }

    wsc_text(x + WPN_NAME_X * CW, y + 1, AL_LEFT, true, cap, "WEAPON");
    for (int c = 0; c < (int)q_countof(wpn_table); c++)
        if (show[c])
            wsc_text(x + ends[c] * CW, y + 1, AL_RIGHT, true, cap, wpn_table[c].caption);
    y += wsc_rh;

    int rows = 0;
    for (int wi = 1; wi < WSC_NW && y + wsc_rh <= bottom; wi++) {
        if (!wsc_weapon_values(pi, wi, wsc.scope, v))
            continue;
        wsc_fill(x, y, w, wsc_rh, th->row_bg[rows & 1]);
        wsc_draw_icon(x + CW / 2, y, wi);
        wsc_text(x + WPN_NAME_X * CW, y, AL_LEFT, false, th->text, wsc_wname[wi]);
        for (int c = 0; c < (int)q_countof(wpn_table); c++) {
            char f[12];
            if (!show[c])
                continue;
            if (wpn_table[c].field != WC_ACC)
                Q_snprintf(f, sizeof(f), "%d", v[wpn_table[c].field]);
            else if (wi == 8 || wi == 11 || wi == 12)
                Q_strlcpy(f, "-", sizeof(f));   // kick, punch and the slash are no aim
            else
                wsc_fmt_acc(f, sizeof(f), v[F_HITS], v[F_SHOTS]);
            wsc_text(x + ends[c] * CW, y, AL_RIGHT, false, th->text, f);
        }
        y += wsc_rh;
        rows++;
    }
    if (!rows && y + wsc_rh <= bottom) {
        wsc_fill(x, y, w, wsc_rh, th->row_bg[0]);
        wsc_text(x + 3 * CW, y, AL_LEFT, false, th->dim, "no shots");
        y += wsc_rh;
    }
    return y + 3;
}

// The weapons a player got their kills with, most first (ties by weapon
// number), at most `max`: the map total and the round being played
static int wsc_top_weapons(int pi, int *ids, int *kills, int max)
{
    int n = 0, v[WSC_NF];

    for (int w = 1; w < WSC_NW; w++) {
        if (!wsc_weapon_values(pi, w, 0, v) || v[F_K] <= 0)
            continue;
        int i = n < max ? n++ : max;
        if (i == max) {
            if (v[F_K] <= kills[max - 1])
                continue;
            i = max - 1;
        }
        while (i > 0 && kills[i - 1] < v[F_K]) {
            ids[i] = ids[i - 1];
            kills[i] = kills[i - 1];
            i--;
        }
        ids[i] = w;
        kills[i] = v[F_K];
    }
    return n;
}

#define WPN_COL_END 2   // cells from the row's end to the weapons' end: the expand mark

// "count icon" per weapon, best first, the group right-aligned against
// the end of the row, as on the stream branch's score view
static void wsc_draw_row_weapons(int x, int y, int w, int pi, uint32_t color)
{
    int ids[3], kills[3], widths[3], total = 0;
    char cnt[3][8];
    int n = wsc_top_weapons(pi, ids, kills, 3);
    const int gap = 2, after = CW;

    for (int i = 0; i < n; i++) {
        Q_snprintf(cnt[i], sizeof(cnt[i]), "%d", kills[i]);
        widths[i] = wsc_measure(false, cnt[i], strlen(cnt[i])) + gap + CW * 2;
        total += widths[i] + (i ? after : 0);
    }
    int cx = x + w - WPN_COL_END * CW - total;
    for (int i = 0; i < n; i++) {
        int cw = widths[i] - gap - CW * 2;
        wsc_str(cx, y, AL_LEFT, false, color, cnt[i], strlen(cnt[i]));
        wsc_draw_icon(cx + cw + gap, y, ids[i]);
        cx += widths[i] + after;
    }
}

static int wsc_draw_row(int x, int y, int w, int rank, int pi, bool alt)
{
    const wsc_player_t *p = &wsc.p[pi];
    bool selected = pi == wsc.cursor && wsc.cursor_shown;
    bool gone = p->slot < 0;
    char line[160], num[8][12], acc[8], kd[12];
    int shots, hits;

    uint32_t bg = selected ? MakeColor(60, 60, 140, 200) :
                  p->sub ? MakeColor(110, 110, 110, alt ? 40 : 26) :    // greyed: not in play
                  wsc_team_color(p->team, alt ? 60 : 40);
    wsc_fill(x, y, w, wsc_rh, bg);
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
    // who is alive in the green the server's own scoreboard has for them,
    // where the server says we may know (dead, spectating, between rounds)
    uint32_t color = (gone || p->sub) && !selected ? MakeColor(150, 150, 150, 255) :
                     wsc.alive_shown && p->alive ? MakeColor(120, 230, 120, 255) : 0;
    wsc_grid(x, y, false, color, line, row_cols, q_countof(row_cols), -1);
    {
        // a team's captain wears an @ before the name, as on the server's board
        bool captain = p->team > 0 && p->team < WSC_MAX_TEAMS && wsc.team_captain[p->team] == pi;
        char name[40];
        Q_snprintf(name, sizeof(name), "%s%s", captain ? "@" : "", p->name[0] ? p->name : "(unnamed)");
        int len = (int)strlen(name), room = NAME_W * CW;
        while (len > 1 && wsc_measure(false, name, len) > room)
            len--;
        wsc_str(x + row_cols[ROW_NAME_COL].start * CW, y, AL_LEFT, false, color, name, len);
    }

    wsc_draw_row_weapons(x, y, w, pi, color);

    // the expand mark at the row's end
    wsc_text(x + w - CW, y, AL_RIGHT, false, MakeColor(170, 170, 170, 255),
             wsc.expanded == pi ? "-" : "+");
    return y + wsc_rh;
}

static const char *wsc_hints(void)
{
    return wsc.page == 2 ? "[UP/DOWN] Player   [PGUP/PGDN] Map/rounds   [ENTER] Close stats   [ESC] Close"
                         : "[ARROWS] Player/team   [PGUP/PGDN] Map/rounds   [ENTER] Close stats   [ESC] Close";
}

// spectators on one line, cut to the board
static void wsc_draw_spectators(int x, int y, int w, int bottom, uint32_t color)
{
    char line[160];
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
        wsc_str(x, y, AL_LEFT, false, color, line, len);
    }
}

static void wsc_draw_dark(int hud_x, int hud_y, int hud_w, int hud_h)
{
    int list[WSC_MAX_P];
    char line[160];

    // height: the header, each team's band, caption and rows, the
    // expanded weapons, spectators and the footer
    int nteams = wsc_num_teams();
    int h = wsc_big_h() + 4 + 24 + 4 + wsc_rh + 4;
    if (wsc.team_ready[1] == 0 || wsc.team_ready[2] == 0)
        h += wsc_rh + 2;        // NOT READY under the names
    for (int t = 1; t <= nteams; t++) {
        int n = wsc_team_list(t, list);
        h += wsc_rh + 3 + wsc_rh + n * wsc_rh + CH / 2;
        for (int i = 0; i < n; i++)
            if (wsc.p[list[i]].sub) {
                h += wsc_rh + 2;        // the SUBS heading
                break;
            }
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
    // The upper edge stays where scr_statsboard_y puts it, and the board
    // grows down from there: opening a player never moves it. What does
    // not fit above the screen's bottom is cut (the rows stop at
    // `bottom`), the footer kept.
    int px = hud_x + (int)(hud_w * Cvar_ClampValue(scr_statsboard_x, 0, 1)) - pw / 2;
    int py = hud_y + (int)(hud_h * Cvar_ClampValue(scr_statsboard_y, 0, 0.9f));
    px = Q_clip(px, hud_x, hud_x + hud_w - pw);
    int ph = min(h + CH, hud_y + hud_h - py);
    int bottom = py + ph - wsc_rh * 2;

    wsc_fill(px, py, pw, ph, MakeColor(0, 0, 0, 215));

    int x = px + CW, w = pw - CW * 2;
    int y = wsc_draw_header(px, pw, py + CH / 2);
    uint32_t cap = MakeColor(185, 185, 185, 235);

    for (int t = 1; t <= nteams && y + wsc_rh * 3 <= bottom; t++) {
        int n = wsc_team_list(t, list);
        const char *tname = wsc.team_name[t][0] ? wsc.team_name[t] : va("Team %d", t);

        wsc_fill(x, y, w, wsc_rh + 2, wsc_team_color(t, 150));
        wsc_text(x + CW, y + 1, AL_LEFT, true, U32_WHITE, tname);
        y += wsc_rh + 3;

        Q_snprintf(line, sizeof(line), ROW_FMT, "#", NAME_W, NAME_W, "PLAYER",
                   "SCORE", "KILLS", "DEATHS", "ACC", "DMG", "K/D", "PING");
        wsc_grid(x, y, true, cap, line, row_cols, q_countof(row_cols), -1);
        wsc_text(x + w - WPN_COL_END * CW, y, AL_RIGHT, true, cap, "WEAPONS");
        y += wsc_rh;

        for (int i = 0; i < n && y + wsc_rh <= bottom; i++) {
            // the subs lined off from the players in play, under a heading
            if (wsc.p[list[i]].sub && (!i || !wsc.p[list[i - 1]].sub)) {
                if (y + wsc_rh * 2 + 2 > bottom)
                    break;
                wsc_fill(x, y, w, 1, MakeColor(120, 120, 120, 160));
                y += 2;
                wsc_text(x + CW, y, AL_LEFT, false, MakeColor(150, 150, 150, 220), "SUBS");
                y += wsc_rh;
            }
            y = wsc_draw_row(x, y, w, i + 1, list[i], i & 1);
            if (list[i] == wsc.expanded)
                y = wsc_draw_weapons(x, y, w, bottom, list[i], &wsc_dark, NULL, 0);
        }
        y += CH / 2;
    }

    wsc_draw_spectators(x, y, w, bottom, MakeColor(150, 150, 150, 220));

    if (wsc.expanded)
        wsc_text(x, py + ph - wsc_rh - CH / 2, AL_LEFT, false, MakeColor(150, 150, 150, 220), wsc_hints());
}

/* ------------------------------------------------------------------ */
/* the classic board (scr_statsboard_style 1): the teams side by side, */
/* as the server's old scoreboard has them - name, icon and big score  */
/* over each team's players - light, with the weapon table of the      */
/* player opened under the columns, across the board                   */

static const wsc_theme_t wsc_light = {
    .text = MakeColor(28, 28, 34, 255),
    .caption = MakeColor(105, 105, 115, 255),
    .dim = MakeColor(145, 145, 152, 255),
    .scope = MakeColor(150, 95, 0, 255),
    .alive = MakeColor(20, 135, 45, 255),
    .ready = MakeColor(20, 135, 45, 255),
    .unready = MakeColor(205, 105, 0, 255),
    .head_bg = MakeColor(212, 212, 218, 240),
    .row_bg = { MakeColor(255, 255, 255, 140), MakeColor(226, 226, 232, 160) },
};

// See-through, the light panel no longer carries dark letters: the old
// scoreboard's own scheme instead, light letters with a dark shadow,
// which read on any wall
static const wsc_theme_t wsc_light_clear = {
    .text = U32_WHITE,
    .caption = MakeColor(205, 205, 212, 255),
    .dim = MakeColor(165, 165, 172, 255),
    .scope = MakeColor(240, 220, 110, 255),
    .alive = MakeColor(120, 230, 120, 255),
    .ready = MakeColor(120, 230, 120, 255),
    .unready = MakeColor(255, 170, 60, 255),
    .head_bg = MakeColor(212, 212, 218, 240),
    .row_bg = { MakeColor(255, 255, 255, 140), MakeColor(226, 226, 232, 160) },
};

#define CL_BG       MakeColor(238, 238, 241, 238)
#define CL_EDGE     MakeColor(165, 165, 172, 255)
#define CL_CURSOR   MakeColor(176, 198, 238, 240)

// a team's numbers, each right-aligned at its end and a cell apart
static const struct {
    const char  *caption;
    int         width;
} cl_cols[] = {
    { "SCR", 4 }, { "K", 3 }, { "D", 3 }, { "ACC", 4 }, { "DMG", 5 }, { "PNG", 4 },
};
#define CL_NCOLS    ((int)q_countof(cl_cols))
#define CL_NAME_W   15
#define CL_STATS_W  (4 + 3 + 3 + 4 + 5 + 4 + CL_NCOLS)
#define CL_COL_W    (1 + CL_NAME_W + CL_STATS_W + 1)    // in cells, a cell either side
#define CL_GAP      2
#define CL_ICON     32                                  // the team icon, as the old board's
#define CL_HEAD_H   (wsc_rh + 4 + CL_ICON + 4 + wsc_rh + 2)

// the numbers' ends in a column `colw` cells wide; returns where the name may end
// a dividing line: drawn in the board's dim text colour, so it shows
// however see-through the panel is
static void wsc_rule(int x, int y, int w, const wsc_theme_t *th)
{
    color_t c = { .u32 = th->dim };
    c.u8[3] = 190;
    R_DrawFill32(x, y, w, 1, c.u32);
}

static int wsc_classic_cols(int colw, int *ends)
{
    int e = colw - 1;

    for (int c = CL_NCOLS - 1; c >= 0; c--) {
        ends[c] = e;
        e -= cl_cols[c].width + 1;
    }
    return e;
}

static const char *wsc_team_title(int t)
{
    return wsc.team_name[t][0] ? wsc.team_name[t] : va("Team %d", t);
}

static int wsc_classic_header(int x, int colw, int y, int t, const int *ends,
                              const int *list, int n, const wsc_theme_t *th)
{
    int w = colw * CW;
    char buf[32];

    // the name, centred over the team, and a rule in its colour
    wsc_text(x + w / 2, y, AL_CENTER, true, th->text, wsc_team_title(t));
    y += wsc_rh + 1;
    R_DrawFill32(x + CW, y, w - CW * 2, 2, wsc_team_color(t, 255));
    y += 3;

    // the icon on the left, the big score on the right
    int tx = x + CW;
    qhandle_t icon = wsc.team_icon[t][0] ? R_RegisterPic(wsc.team_icon[t]) : 0;
    if (icon) {
        int iw, ih;
        R_GetPicSize(&iw, &ih, icon);
        int dw = ih > 0 ? iw * CL_ICON / ih : CL_ICON;
        R_DrawStretchPic(tx, y, dw, CL_ICON, icon);
        tx += dw + CW;
    }
    // the HUD's silver digits are faint on the light board: where there is
    // the font, the score is its bold in the board's text colour
    int len = Q_snprintf(buf, sizeof(buf), "%d", wsc.team_score[t]);
#ifdef TEXT_BOLD
    if (wsc_ttf) {
        float size = max(30.0f, wsc_size * 2);     // grows with the text
        int lh = R_TextLineHeight(TEXT_BOLD, size);
        R_DrawText(x + w - CW, y + (CL_ICON - lh) / 2,
                   TEXT_BOLD | TEXT_RIGHT | (wsc_clear ? TEXT_SHADOW : 0), size,
                   th->text, th->text, buf, len);
    } else
#endif
    wsc_draw_number(x + w - CW - len * 16, y + (CL_ICON - 24) / 2, wsc.team_score[t]);

    // beside the icon, as the old board: the players' score / players (/ subs)
    int score = 0, players = 0, subs = 0;
    for (int i = 0; i < n; i++) {
        const wsc_player_t *p = &wsc.p[list[i]];
        if (p->sub) {
            subs++;
        } else if (p->slot >= 0) {
            score += p->score;
            players++;
        }
    }
    if (wsc.team_ready[t] >= 0)
        Q_snprintf(buf, sizeof(buf), "%d/%d/%d", score, players, subs);
    else
        Q_snprintf(buf, sizeof(buf), "%d/%d", score, players);
    wsc_text(tx, y + CL_ICON - wsc_rh, AL_LEFT, false, th->caption, buf);
    y += CL_ICON + 4;

    // the captions
    wsc_text(x + CW, y, AL_LEFT, true, th->caption, "Name");
    for (int c = 0; c < CL_NCOLS; c++)
        wsc_text(x + ends[c] * CW, y, AL_RIGHT, true, th->caption, cl_cols[c].caption);
    y += wsc_rh;
    wsc_rule(x + CW / 2, y, w - CW, th);
    return y + 2;
}

static void wsc_classic_row(int x, int colw, int y, const int *ends, int name_end,
                            int pi, bool alt, const wsc_theme_t *th)
{
    const wsc_player_t *p = &wsc.p[pi];
    bool selected = pi == wsc.cursor && wsc.cursor_shown;
    bool gone = p->slot < 0;
    int w = colw * CW;
    char f[CL_NCOLS][12], name[40];
    int shots, hits;

    if (selected) {
        // the cursor keeps some of its fill however see-through the board
        color_t c = { .u32 = CL_CURSOR };
        c.u8[3] = max((int)(c.u8[3] * wsc_bg_k), 110);
        R_DrawFill32(x + CW / 2, y, w - CW, wsc_rh, c.u32);
    } else if (alt)
        wsc_fill(x + CW / 2, y, w - CW, wsc_rh, th->row_bg[1]);
    if (pi == wsc.me)
        R_DrawFill32(x + CW / 2, y, 2, wsc_rh, wsc_team_color(p->team, 255));

    // who is alive in green, where the server says we may know
    uint32_t color = gone || p->sub ? th->dim :
                     wsc.alive_shown && p->alive ? th->alive : th->text;

    bool captain = p->team > 0 && p->team < WSC_MAX_TEAMS && wsc.team_captain[p->team] == pi;
    Q_snprintf(name, sizeof(name), "%s%s", captain ? "@" : "", p->name[0] ? p->name : "(unnamed)");
    int len = (int)strlen(name), room = (name_end - 1) * CW;
    bool bold = wsc.expanded == pi;     // whose weapons are open under the columns
    while (len > 1 && wsc_measure(bold, name, len) > room)
        len--;
    wsc_str(x + CW, y, AL_LEFT, bold, color, name, len);

    wsc_accuracy(pi, &shots, &hits);
    Q_snprintf(f[0], sizeof(f[0]), "%d", p->score);
    Q_snprintf(f[1], sizeof(f[1]), "%d", p->kills);
    Q_snprintf(f[2], sizeof(f[2]), "%d", p->deaths);
    wsc_fmt_acc(f[3], sizeof(f[3]), hits, shots);
    Q_snprintf(f[4], sizeof(f[4]), "%d", p->dmg);
    if (gone)
        Q_strlcpy(f[5], "left", sizeof(f[5]));
    else
        Q_snprintf(f[5], sizeof(f[5]), "%d", min(p->ping, 999));
    for (int c = 0; c < CL_NCOLS; c++)
        wsc_text(x + ends[c] * CW, y, AL_RIGHT, false, color, f[c]);
}

static void wsc_draw_classic(int hud_x, int hud_y, int hud_w, int hud_h)
{
    const wsc_theme_t *th = wsc_clear ? &wsc_light_clear : &wsc_light;
    static int lists[WSC_MAX_TEAMS][WSC_MAX_P];
    int counts[WSC_MAX_TEAMS], ends[CL_NCOLS];
    int nteams = min(wsc_num_teams(), WSC_MAX_TEAMS - 1);

    // the columns as wide as the old board's, the names giving way where
    // the teams do not fit side by side; the weapon table wants the room
    // of the dark board
    int maxw = (int)(hud_w * 0.96f);
    int colw = CL_COL_W;
    while (colw > CL_COL_W - 8 && (nteams * colw + (nteams - 1) * CL_GAP + 2) * CW > maxw)
        colw--;
    int pw = (nteams * colw + (nteams - 1) * CL_GAP + 2) * CW;
    pw = min(max(pw, BOARD_CHARS * CW + CW * 2), maxw);
    int name_end = wsc_classic_cols(colw, ends);

    // height: the team heads, the longest team, the ready line, the
    // weapons opened, spectators, the map line and the hints
    int rows = 0, expanded_rows = 0, subs_px = 0;
    bool ready_line = false;
    for (int t = 1; t <= nteams; t++) {
        int n = counts[t] = wsc_team_list(t, lists[t]), r = n;
        for (int i = 0; i < n; i++)
            if (wsc.p[lists[t][i]].sub) {
                r++;            // the Subs heading, and its line
                subs_px = 3;
                break;
            }
        rows = max(rows, r);
        if (wsc.team_ready[t] >= 0)
            ready_line = true;
        for (int i = 0; i < n; i++)
            if (lists[t][i] == wsc.expanded) {
                int v[WSC_NF], wr = 0;
                for (int w = 1; w < WSC_NW; w++)
                    wr += wsc_weapon_values(lists[t][i], w, wsc.scope, v);
                expanded_rows = 2 + max(wr, 1);
            }
    }
    int h = CH / 2 + CL_HEAD_H + rows * wsc_rh + subs_px + CH / 2;
    if (ready_line)
        h += wsc_rh + 2;
    if (expanded_rows)
        h += wsc_rh * expanded_rows + 3 + CH / 2;
    h += wsc_rh * 3 + CH / 2;

    int px = hud_x + (int)(hud_w * Cvar_ClampValue(scr_statsboard_x, 0, 1)) - pw / 2;
    int py = hud_y + (int)(hud_h * Cvar_ClampValue(scr_statsboard_y, 0, 0.9f));
    px = Q_clip(px, hud_x, hud_x + hud_w - pw);
    int ph = min(h, hud_y + hud_h - py);
    int bottom = py + ph - wsc_rh * 2 - CH / 2;

    wsc_fill(px, py, pw, ph, CL_BG);
    wsc_fill(px, py, pw, 1, CL_EDGE);
    wsc_fill(px, py + ph - 1, pw, 1, CL_EDGE);
    wsc_fill(px, py, 1, ph, CL_EDGE);
    wsc_fill(px + pw - 1, py, 1, ph, CL_EDGE);

    // the columns, centred in the board
    int colpx = colw * CW, gappx = CL_GAP * CW;
    int x0 = px + (pw - (nteams * colpx + (nteams - 1) * gappx)) / 2;
    int top = py + CH / 2, maxy = top;

    for (int t = 1; t <= nteams; t++) {
        int x = x0 + (t - 1) * (colpx + gappx);
        int *list = lists[t], n = counts[t];
        int y = wsc_classic_header(x, colw, top, t, ends, list, n, th);

        for (int i = 0; i < n && y + wsc_rh <= bottom; i++) {
            if (wsc.p[list[i]].sub && (!i || !wsc.p[list[i - 1]].sub)) {
                if (y + wsc_rh * 2 + 3 > bottom)
                    break;
                // a line between who plays and who waits
                wsc_rule(x + CW / 2, y + 1, colpx - CW, th);
                y += 3;
                wsc_text(x + CW, y, AL_LEFT, true, th->caption, "Subs");
                y += wsc_rh;
            }
            wsc_classic_row(x, colw, y, ends, name_end, list[i], i & 1, th);
            y += wsc_rh;
        }
        maxy = max(maxy, y);
    }
    int y = maxy + CH / 2;

    // matchmode: Ready / Not Ready under each team, as the old board's footer
    if (ready_line && y + wsc_rh <= bottom) {
        for (int t = 1; t <= nteams; t++) {
            if (wsc.team_ready[t] < 0)
                continue;
            int x = x0 + (t - 1) * (colpx + gappx);
            wsc_text(x + colpx / 2, y, AL_CENTER, true, wsc.team_ready[t] ? th->ready : th->unready,
                     wsc.team_ready[t] ? "Ready" : "Not Ready");
        }
        y += wsc_rh + 2;
    }

    // the opened player's weapons, across the board, headed by their name
    if (expanded_rows && y + wsc_rh * 3 <= bottom) {
        const wsc_player_t *p = &wsc.p[wsc.expanded];
        // the team's colour, lightened for the see-through board's light letters
        uint32_t nc = wsc_team_color(p->team, 255);
        if (wsc_clear) {
            color_t c = { .u32 = nc };
            for (int i = 0; i < 3; i++)
                c.u8[i] += (255 - c.u8[i]) / 2;
            nc = c.u32;
        }
        y = wsc_draw_weapons(px + CW, y, pw - CW * 2, bottom, wsc.expanded, th,
                             p->name[0] ? p->name : "(unnamed)", nc);
        y += CH / 2;
    }

    wsc_draw_spectators(px + CW, y, pw - CW * 2, bottom, th->dim);

    // the map and round centred, as the old board's time, and the keys
    char info[64];
    if (wsc.round)
        Q_snprintf(info, sizeof(info), "%s - round %d%s", wsc.mapname, wsc.round,
                   wsc.going ? "" : " over");
    else
        Q_strlcpy(info, wsc.mapname, sizeof(info));
    int fy = py + ph - wsc_rh * 2 - CH / 2;
    wsc_text(px + pw / 2, fy, AL_CENTER, false, th->caption, info);
    if (wsc.expanded)
        wsc_text(px + CW, fy + wsc_rh, AL_LEFT, false, th->dim, wsc_hints());
}

/* ------------------------------------------------------------------ */
/* the replica: the server's own matchmode scoreboard (A_Scoreboard-    */
/* Message, use_newscore 0) as it draws it - conchars on the layout's   */
/* 320x240 grid, no panel - with the cursor, and the weapon table under */
/* it                                                                   */

static const wsc_theme_t wsc_replica = {
    // white and the alt charset's green, as the layout's string/string2
    .text = U32_WHITE,
    .caption = U32_WHITE,
    .dim = MakeColor(150, 150, 150, 255),
    .scope = U32_WHITE,
    .alive = U32_WHITE,
    .ready = U32_WHITE,
    .unready = U32_WHITE,
    .head_bg = 0,
    .row_bg = { 0, MakeColor(0, 0, 0, 70) },
};

#define RP_ROW_W        160     // TEAM_ROW_WIDTH: twenty cells a team
#define RP_MAX_ROWS     9       // MAX_SCORES_PER_TEAM

// one of the layout's strings at its xv/yv
static void wsc_rp_string(int ox, int oy, int xv, int yv, bool alt, uint32_t color,
                          const char *s)
{
    wsc_str(ox + xv, oy + yv, AL_LEFT, alt, color, s, strlen(s));
}

// a player's row, the cursor's bar under it
static void wsc_rp_row(int ox, int oy, int xv, int yv, int pi, bool alt)
{
    const wsc_player_t *p = &wsc.p[pi];
    char name[20];

    if (pi == wsc.cursor && wsc.cursor_shown)
        R_DrawFill32(ox + xv - 2, oy + yv - 1, 15 * CW + 4, CH + 2, WSC_CURSOR);
    bool captain = p->team > 0 && p->team < WSC_MAX_TEAMS && wsc.team_captain[p->team] == pi;
    // playername[16] on the server: the '@' and fifteen letters at most
    Q_snprintf(name, 16, "%s%s", captain ? "@" : "", p->name[0] ? p->name : "(unnamed)");
    // who left keeps their line here, greyed, as they keep their stats
    wsc_rp_string(ox, oy, xv, yv, alt, p->slot < 0 ? WSC_DIM : 0, name);
}


static void wsc_draw_replica(int hud_x, int hud_y, int hud_w, int hud_h)
{
    static int lists[WSC_MAX_TEAMS][WSC_MAX_P];
    int nteams, ox, oy, base_x;
    bool matchmode = false, deadview;
    qhandle_t tag;
    char buf[64];

    nteams = min(wsc_num_teams(), WSC_MAX_TEAMS - 1);
    ox = hud_x + hud_w / 2 - 160;
    oy = hud_y + hud_h / 2 - 120;
    base_x = 160 - (RP_ROW_W * nteams) / 2;
    deadview = wsc.alive_shown;     // never under limchasecam: the server says
    tag = R_RegisterPic("tag3");

    for (int t = 1; t <= nteams; t++)
        if (wsc.team_ready[t] >= 0)
            matchmode = true;

    for (int t = 1, lx = base_x; t <= nteams; t++, lx += RP_ROW_W) {
        int *list = lists[t], n = wsc_team_list(t, list);
        int total = 0, subs = 0, score = 0, alive = 0;

        for (int i = 0; i < n; i++) {
            const wsc_player_t *p = &wsc.p[list[i]];
            if (p->sub) {
                subs++;
            } else {
                total++;
                if (p->slot >= 0)
                    score += p->score;
                if (p->alive)
                    alive++;
            }
        }

        // the header: the team's skin and tag pics, the totals, the
        // big score, the name centred over twenty cells
        qhandle_t icon = wsc.team_icon[t][0] ? R_RegisterPic(wsc.team_icon[t]) : 0;
        if (icon)
            R_DrawPic(ox + lx, oy + 8, icon);
        if (tag)
            R_DrawPic(ox + lx + 32, oy + 8, tag);
        if (matchmode)
            Q_snprintf(buf, sizeof(buf), "%4i/%2i/%-2d", score, total, subs);
        else
            Q_snprintf(buf, sizeof(buf), "%4i/%2i", score, total);
        wsc_rp_string(ox, oy, lx + 32, 28, false, 0, buf);
        // the num widget: three digits, right-aligned, two pixels in
        int digits = Q_snprintf(buf, sizeof(buf), "%d", wsc.team_score[t]);
        wsc_draw_number(ox + lx + 96 + 2 + 16 * max(3 - digits, 0), oy + 12, wsc.team_score[t]);
        const char *tn = wsc_team_title(t);
        int name_pos = max((20 - (int)strlen(tn)) / 2, 0) * 8;
        wsc_rp_string(ox, oy, lx + name_pos, 0, false, 0, tn);

        // the players from yv 42, alive ones in green when we may know;
        // the subs under their heading at yv 96, as the server fits them
        int max_rows = RP_MAX_ROWS, sub_lines = 0;
        if (matchmode) {
            sub_lines = 1 + min(subs, 2);
            if (max_rows - sub_lines < 4)
                sub_lines = 1;
            max_rows -= sub_lines;
        }
        int shown = total > max_rows ? max_rows - 1 : total;
        int y = 42, printed = 0, alive_printed = 0;
        for (int i = 0; i < n && printed < shown; i++) {
            const wsc_player_t *p = &wsc.p[list[i]];
            if (p->sub)
                continue;
            wsc_rp_row(ox, oy, lx, y, list[i], deadview && p->alive);
            alive_printed += p->alive;
            y += 8;
            printed++;
        }
        if (printed < total) {
            if (!deadview)
                Q_snprintf(buf, sizeof(buf), "..and %i more", total - printed);
            else
                Q_snprintf(buf, sizeof(buf), "..and %i/%i more", alive - alive_printed,
                           total - printed);
            wsc_rp_string(ox, oy, lx, y, deadview && alive > alive_printed, 0, buf);
        }

        if (sub_lines == 1 && subs) {
            Q_snprintf(buf, sizeof(buf), "%d Subs", subs);
            wsc_rp_string(ox, oy, lx, 96, true, 0, buf);
        } else if (sub_lines > 0) {
            wsc_rp_string(ox, oy, lx, 96, true, 0, "Subs");
            int sub_max = subs > sub_lines - 1 ? sub_lines - 2 : subs;
            y = 104;
            printed = 0;
            for (int i = 0; i < n && printed < sub_max; i++) {
                if (!wsc.p[list[i]].sub)
                    continue;
                wsc_rp_row(ox, oy, lx, y, list[i], false);
                y += 8;
                printed++;
            }
            if (printed < subs) {
                Q_snprintf(buf, sizeof(buf), " + %i more", subs - printed);
                wsc_rp_string(ox, oy, lx, y, false, 0, buf);
            }
        }

        if (wsc.team_ready[t] >= 0)
            wsc_rp_string(ox, oy, base_x + 39 + (t - 1) * RP_ROW_W, 128, true, 0,
                          wsc.team_ready[t] ? "Ready" : "Not Ready");
    }


    // below: the opened player's weapons, a row two pixels taller for the
    // icons, then the keys
    int bottom = hud_y + hud_h - CH * 2;
    int y = oy + 160;
    int tw = min(BOARD_CHARS * CW, hud_w - CW * 2);
    int tx = hud_x + (hud_w - tw) / 2;
    wsc_rh = CH + 2;
    if (wsc.expanded && wsc.p[wsc.expanded].used)
        y = wsc_draw_weapons(tx, y, tw, bottom, wsc.expanded, &wsc_replica,
                             wsc.p[wsc.expanded].name[0] ? wsc.p[wsc.expanded].name : "(unnamed)",
                             U32_WHITE) + CH / 2;
    // the keys only with a player's weapons open: the board alone is the
    // server's, as it looks
    if (wsc.expanded && wsc.p[wsc.expanded].used)
        wsc_text(tx + CW, min(y, bottom), AL_LEFT, false, WSC_DIM, wsc_hints());
}

/* ------------------------------------------------------------------ */
/* the list page: the server's second scoreboard (LAYOUT_SCORES2, its    */
/* default "FNMPIT" columns) at its places, with Acc after Damage and    */
/* the kills per weapon after Team - a column for each weapon somebody   */
/* on the list has killed with, its kill-feed icon over it               */

#define LS_CHARS    47      // "Frags Player... Time Ping Damage Acc Team"

static void wsc_draw_list(int hud_x, int hud_y, int hud_w, int hud_h)
{
    int list[WSC_MAX_P], n = wsc_list_page(list);
    int wk[WSC_NW], nw = 0, v[WSC_NF];
    bool matchmode = false;
    char line[256];

    for (int t = 1; t < WSC_MAX_TEAMS; t++)
        if (wsc.team_ready[t] >= 0)
            matchmode = true;
    // the weapons somebody on the list killed with, most kills in all on
    // the left (a tie keeps the weapons' own order)
    int wtot[WSC_NW] = { 0 };
    for (int w = 1; w < WSC_NW && scr_statslist_weapons->integer; w++) {
        for (int i = 0; i < n; i++)
            if (wsc_weapon_values(list[i], w, 0, v))
                wtot[w] += v[F_K];
        if (wtot[w] > 0)
            wk[nw++] = w;
    }
    for (int a = 1; a < nw; a++)        // a handful: insertion sort, stable
        for (int b = a; b > 0 && wtot[wk[b]] > wtot[wk[b - 1]]; b--) {
            int t = wk[b];
            wk[b] = wk[b - 1];
            wk[b - 1] = t;
        }

    int chars = LS_CHARS + nw * 4;
    int ox = hud_x + hud_w / 2 - 160, oy = hud_y + hud_h / 2 - 120;
    int lx = ox + 160 - chars * 4;

    // the captions and their bars, string2
    size_t len = Q_strlcpy(line, "Frags Player          Time Ping Damage Acc Team", sizeof(line));
    for (int k = 0; k < nw && len + 4 < sizeof(line); k++)
        len += Q_strlcpy(line + len, "    ", sizeof(line) - len);
    wsc_str(lx, oy + 32, AL_LEFT, true, 0, line, strlen(line));
    len = Q_strlcpy(line, "\x9D\x9E\x9E\x9E\x9F \x9D\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9F "
                    "\x9D\x9E\x9E\x9F \x9D\x9E\x9E\x9F \x9D\x9E\x9E\x9E\x9E\x9F \x9D\x9E\x9F \x9D\x9E\x9E\x9F",
                    sizeof(line));
    for (int k = 0; k < nw && len + 4 < sizeof(line); k++)
        len += Q_strlcpy(line + len, " \x9D\x9E\x9F", sizeof(line) - len);
    wsc_str(lx, oy + 40, AL_LEFT, true, 0, line, strlen(line));
    // the weapons' icons over their columns: the column's three cells wide
    // at most, standing on its bar
    for (int k = 0; k < nw; k++) {
        qhandle_t pic = 0;
        int iw, ih, bw = CW * 3, bh = 14;
        for (int j = 0; j < 2 && !pic && wsc_wpic[wk[k]][j]; j++)
            pic = wsc_icon_pic(wsc_wpic[wk[k]][j]);
        if (!pic)
            continue;
        R_GetPicSize(&iw, &ih, pic);
        if (iw <= 0 || ih <= 0)
            continue;
        int dw = iw * bh / ih, dh = bh;
        if (dw > bw) {
            dh = dh * bw / dw;
            dw = bw;
        }
        R_DrawStretchPic(lx + (LS_CHARS + k * 4 + 1) * CW + (bw - dw) / 2, oy + 40 - 1 - dh, dw, dh, pic);
    }

    int y = oy + 48, bottom = hud_y + hud_h - CH * 2;
    for (int i = 0; i < n; i++) {
        const wsc_player_t *p = &wsc.p[list[i]];
        char tm[8], pg[8], tc[8];
        int shots, hits;

        if (y + CH > bottom - CH) {
            Q_snprintf(line, sizeof(line), "..and %d more", n - i);
            wsc_str(lx, y, AL_LEFT, false, 0, line, strlen(line));
            y += 8;
            break;
        }
        if (list[i] == wsc.cursor && wsc.cursor_shown)
            R_DrawFill32(lx - 2, y - 1, chars * CW + 4, CH + 2, WSC_CURSOR);

        if (p->minutes < 60)
            Q_snprintf(tm, sizeof(tm), "%4d", p->minutes);
        else if (p->minutes < 600)
            Q_snprintf(tm, sizeof(tm), "%1d:%02d", p->minutes / 60, p->minutes % 60);
        else
            Q_snprintf(tm, sizeof(tm), "%3dh", min(999, p->minutes / 60));
        if (p->bot)
            Q_strlcpy(pg, " BOT", sizeof(pg));
        else
            Q_snprintf(pg, sizeof(pg), "%4d", min(9999, p->ping));
        char team = p->team > 0 && p->team < 10 ? '0' + p->team : ' ';
        if (matchmode) {
            bool captain = p->team > 0 && p->team < WSC_MAX_TEAMS && wsc.team_captain[p->team] == list[i];
            Q_snprintf(tc, sizeof(tc), "  %c%c", team, captain ? 'C' : p->sub ? 'S' : ' ');
        } else {
            Q_snprintf(tc, sizeof(tc), "   %c", team);
        }
        wsc_accuracy(list[i], &shots, &hits);
        len = Q_snprintf(line, sizeof(line), "%5d %-15.15s %s %s %6d %3d %s",
                         min(99999, p->score), p->name[0] ? p->name : "(unnamed)", tm, pg,
                         min(999999, p->dmg), shots ? hits * 100 / shots : 0, tc);
        for (int k = 0; k < nw && len + 4 < sizeof(line); k++) {
            int kills = wsc_weapon_values(list[i], wk[k], 0, v) ? v[F_K] : 0;
            len += Q_snprintf(line + len, sizeof(line) - len, " %3d", kills);
        }
        wsc_str(lx, y, AL_LEFT, false, 0, line, strlen(line));
        y += 8;
    }

    // below: the opened player's weapons, then the keys, as on the board
    int tw = min(BOARD_CHARS * CW, hud_w - CW * 2);
    int tx = hud_x + (hud_w - tw) / 2;
    y += CH;
    wsc_rh = CH + 2;
    if (wsc.expanded && wsc.p[wsc.expanded].used) {
        y = wsc_draw_weapons(tx, y, tw, bottom, wsc.expanded, &wsc_replica,
                             wsc.p[wsc.expanded].name[0] ? wsc.p[wsc.expanded].name : "(unnamed)",
                             U32_WHITE) + CH / 2;
        wsc_text(tx + CW, min(y, bottom), AL_LEFT, false, WSC_DIM, wsc_hints());
    }
}

void CL_WS_DrawBoard(int hud_x, int hud_y, int hud_w, int hud_h, float hud_scale, qhandle_t font)
{
    if (!CL_WS_BoardOpen())
        return;
    wsc_font = font;
    wsc_hud_scale = hud_scale;
    wsc_ttf = false;
    wsc_size = CH;
    wsc_rh = CH + 2;
    wsc_bg_k = Cvar_ClampValue(scr_statsboard_alpha, 0, 1) / (215.0f / 255.0f);
    wsc_fix_cursor();

    // the list page and the replica are the server's own boards: its
    // conchars, rows of eight, whatever the style's text
    if (wsc.page == 2 || scr_statsboard_style->integer == 2) {
        wsc_rh = CH;
        wsc_alt_bold = true;
        wsc_clear = false;
        if (wsc.page == 2)
            wsc_draw_list(hud_x, hud_y, hud_w, hud_h);
        else
            wsc_draw_replica(hud_x, hud_y, hud_w, hud_h);
        return;
    }

#ifdef TEXT_BOLD
    if (scr_statsboard_font && scr_statsboard_font->integer && R_TextAvailable()) {
        wsc_ttf = true;
        wsc_size = scr_statsboard_size->value ? Cvar_ClampValue(scr_statsboard_size, 6, 24) :
                   scr_statsboard_style->integer == 1 ? 16 : 10;
        wsc_rh = max(CH + 2, R_TextLineHeight(TEXT_BOLD, wsc_size) + 1);
    }
#endif
    // the classic board's dark text on light: the alt charset's green would
    // stand out of it, so its bold is the plain letters in their colour
    wsc_alt_bold = scr_statsboard_style->integer != 1;
    // The old scoreboard reads on anything as light letters with a dark
    // edge (the conchars' own, or the font's shadow); the classic board's
    // dark letters need the light panel behind them, so below half its
    // opacity it takes the old scheme
    wsc_clear = !wsc_alt_bold && Cvar_ClampValue(scr_statsboard_alpha, 0, 1) < 0.5f;
    if (!wsc_alt_bold)
        wsc_draw_classic(hud_x, hud_y, hud_w, hud_h);
    else
        wsc_draw_dark(hud_x, hud_y, hud_w, hud_h);
}
