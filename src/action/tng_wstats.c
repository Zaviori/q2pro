/*
 * Per-weapon statistics channel - doc/weapon-stats.md.
 *
 * Every counter is kept per stats id (p) and weapon (w) for the map:
 * tot is the map total, base the total at the round's start (round
 * values are tot - base), sent the total last put in a wlive line.
 * Lines go out as svc_userstatistic (a string and a long), only to
 * subscribed AQtion-protocol clients (userinfo aq2feed) and the MVD
 * dummy.
 */

#include "g_local.h"

#define WS_PROTOCOL     38          // PROTOCOL_VERSION_AQTION
#define WS_MAX_SIDS     128
#define WS_NW           14          // weapon numbers 1..13
#define WS_NF           12
#define WS_CHUNK        1200        // bytes per unicast: under the MVD's 2048
#define WS_RATE         4000        // history bytes a second per client
#define WS_HIST_MAX     16384       // history records kept per map

enum {
	F_SHOTS, F_HITS, F_HS, F_K, F_DMG,
	F_HEAD, F_CHEST, F_STOMACH, F_LEGS, F_HELMET, F_VEST, F_NOLOC
};

static const char *const ws_fkey[WS_NF] = {
	"shots", "hits", "hs", "k", "dmg",
	"head", "chest", "stomach", "legs", "helmet", "vest", "noloc"
};

typedef struct {
	qboolean used;
	int      slot;                  // client index, -1 when gone
	char     name[16];
	char     steamid[24];
	char     ip[64];
	char     token[20];
	char     sent_name[16];         // as last put in a sidmap line
	int      sent_team;
	int      tot[WS_NW][WS_NF];
	int      base[WS_NW][WS_NF];
	int      sent[WS_NW][WS_NF];
	unsigned changed;               // bit per weapon, since the last wlive
	int      ps[5];                 // pstats as last sent: score k d dmg ping
	int      ping_at;               // realFramenum the ping was last sent
	int      alive;                 // as last sent in an alive line: 0, 1
} ws_sid_t;

typedef struct {
	unsigned char p, r, w;
	int v[WS_NF];
} ws_hist_t;

typedef struct {
	qboolean begun;                 // in the game this map
	qboolean sub;                   // subscribed
	int      sid;                   // 1-based, 0 = none
	int      budget;                // history bytes it may still send now
	int      hist_next;             // backfill: next record, newest first; -1 the done line; -2 idle
	int      req_p, req_r;          // a wstats_get being answered: p 0 = all, r 0 = all
	int      req_next;              // its next record; -1 none
	int      req_lines;
	qboolean deadview;              // may see who is alive: has the list
} ws_client_t;

static struct {
	ws_sid_t     sids[WS_MAX_SIDS + 1];
	int          nsids;
	ws_client_t  cl[MAX_CLIENTS];
	ws_hist_t   *hist;
	int          nhist, hist_alloc;
	int          round;             // rounds started this map
	qboolean     going;             // a round is being counted
	qboolean     map_ended;
	int          live_at;           // realFramenum of the last wlive
	int          team_sent[TEAM_TOP][2];    // wteam as last sent: score, name hash
} ws;

// Non-NULL on an engine that keeps svc_userstatistic out of the MVD but
// for the dummy's copy and plays it only to AQtion clients. On an older
// engine every subscriber's copy would be recorded, and the demo would
// drop its older-protocol viewers: nothing is sent there.
void *engine_Stats_Channel;

static cvar_t *wstats;              // 0: count, send nothing
static cvar_t *wstats_mvd;          // 0: keep the stats out of the MVD
static cvar_t *wstats_live_ms;      // wlive interval, 0 = off

/* ------------------------------------------------------------------ */

static int ws_weapon(int mod)
{
	switch (mod) {
	case MOD_MK23:          return MK23_NUM;
	case MOD_MP5:           return MP5_NUM;
	case MOD_M4:            return M4_NUM;
	case MOD_M3:            return M3_NUM;
	case MOD_HC:            return HC_NUM;
	case MOD_SNIPER:        return SNIPER_NUM;
	case MOD_DUAL:          return DUAL_NUM;
	case MOD_KNIFE:         return KNIFE_NUM;
	case MOD_HANDGRENADE:
	case MOD_HG_SPLASH:
	case MOD_HELD_GRENADE:  return GRENADE_NUM;
	case MOD_KNIFE_THROWN:  return 10;
	case MOD_KICK:          return 11;
	case MOD_PUNCH:         return 12;
	case MOD_GRENADE_IMPACT: return 13;
	default:                return 0;
	}
}

static int ws_slot(const edict_t *ent)
{
	int slot = ent - g_edicts - 1;

	if (!ent->client || slot < 0 || slot >= game.maxclients)
		return -1;
	return slot;
}

static ws_sid_t *ws_sid_of(const edict_t *ent)
{
	int slot = ws_slot(ent);

	if (slot < 0 || !ws.cl[slot].sid)
		return NULL;
	return &ws.sids[ws.cl[slot].sid];
}

static qboolean ws_is_dummy(const edict_t *ent)
{
	return ent->client && ent->client->pers.mvdspec;
}

// Whether this client asked for the channel and can take it: an older
// protocol drops on the unknown command, so it never gets one.
static qboolean ws_wants(edict_t *ent)
{
	if (!ent->inuse || !ent->client)
		return false;
#ifndef NO_BOTS
	if (ent->is_bot)
		return false;
#endif
	if (ws_is_dummy(ent))
		return wstats_mvd->value != 0;
	if (atoi(Info_ValueForKey(ent->client->pers.userinfo, "aq2feed")) < 1)
		return false;
	return Client_GetProtocol(ent) == WS_PROTOCOL;
}

static qboolean ws_on(void)
{
	return wstats->value && engine_Stats_Channel;
}

static void ws_token(char *out, size_t size, int sid)
{
	unsigned char b[8];
	FILE *f = fopen("/dev/urandom", "rb");
	size_t got = f ? fread(b, 1, sizeof(b), f) : 0;
	int i;

	if (f)
		fclose(f);
	if (got != sizeof(b)) {
		unsigned x = (unsigned)time(NULL) ^ (unsigned)(sid * 2654435761u) ^ (unsigned)level.realFramenum;
		for (i = 0; i < 8; i++) {
			x = x * 1103515245u + 12345u + (unsigned)rand();
			b[i] = (unsigned char)(x >> 16);
		}
	}
	Q_snprintf(out, size, "%02x%02x%02x%02x%02x%02x%02x%02x",
		b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
}

/* ------------------------------------------------------------------ */
/* line batching: lines are queued, then written to each target as     */
/* svc_userstatistic commands, WS_CHUNK bytes to a unicast              */

#define WS_TO_ALL       0   // every subscriber, the dummy included
#define WS_TO_WATCHERS  1   // spectators and the dummy: live data
#define WS_TO_ONE       2
#define WS_TO_DEADVIEW  3   // who may see who is alive, the dummy included

static char ws_buf[8192];
static int  ws_len;
static int  ws_mode;
static edict_t *ws_target;

static void ws_write(edict_t *ent)
{
	int pos = 0, chunk = 0;

	while (pos < ws_len) {
		const char *line = ws_buf + pos;
		int len = strlen(line);

		if (chunk && chunk + len + 6 > WS_CHUNK) {
			gi.unicast(ent, true);
			chunk = 0;
		}
		gi.WriteByte(svc_userstatistic);
		gi.WriteString(line);
		gi.WriteLong(0);
		chunk += len + 6;
		pos += len + 1;
	}
	if (chunk)
		gi.unicast(ent, true);
}

static qboolean ws_watcher(const edict_t *ent)
{
	return ws_is_dummy(ent) || ent->client->resp.team == NOTEAM;
}

static void ws_flush(void)
{
	int i;

	if (!ws_len)
		return;
	if (ws_mode == WS_TO_ONE) {
		if (ws_target && ws_target->inuse)
			ws_write(ws_target);
	} else {
		for (i = 0; i < game.maxclients; i++) {
			edict_t *ent = g_edicts + 1 + i;

			if (!ws.cl[i].sub || !ent->inuse)
				continue;
			if (ws_mode == WS_TO_WATCHERS && !ws_watcher(ent))
				continue;
			if (ws_mode == WS_TO_DEADVIEW && !ws.cl[i].deadview)
				continue;
			ws_write(ent);
		}
	}
	ws_len = 0;
}

static void ws_begin(int mode, edict_t *target)
{
	ws_len = 0;
	ws_mode = mode;
	ws_target = target;
}

static void ws_line(const char *fmt, ...)
{
	char line[512];
	va_list ap;
	int len;

	va_start(ap, fmt);
	len = Q_vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (len < 0)
		return;
	if (len >= (int)sizeof(line))
		len = sizeof(line) - 1;
	if (ws_len + len + 1 > (int)sizeof(ws_buf))
		ws_flush();
	memcpy(ws_buf + ws_len, line, len + 1);
	ws_len += len + 1;
}

// key=value pairs of the nonzero counters in v
static void ws_fields(char *out, size_t size, const int *v)
{
	size_t len = 0;
	int f;

	out[0] = 0;
	for (f = 0; f < WS_NF && len < size; f++) {
		if (v[f])
			len += Q_snprintf(out + len, size - len, " %s=%d", ws_fkey[f], v[f]);
	}
}

static void ws_quote(char *out, size_t size, const char *s)
{
	size_t n = 0;

	for (; *s && n + 3 < size; s++) {
		if (*s == '"' || *s == '\\')
			out[n++] = '\\';
		out[n++] = *s;
	}
	out[n] = 0;
}

/* ------------------------------------------------------------------ */
/* lines                                                              */

static void ws_line_sidmap(int p)
{
	ws_sid_t *s = &ws.sids[p];
	char name[40];

	if (s->slot < 0) {
		ws_line("sidmap p=%d c=-1", p);
		return;
	}
	ws_quote(name, sizeof(name), s->name);
	ws_line("sidmap p=%d c=%d name=\"%s\" team=%d", p, s->slot, name, s->sent_team);
}

static void ws_round_values(const ws_sid_t *s, int w, int *v)
{
	int f;

	for (f = 0; f < WS_NF; f++)
		v[f] = s->tot[w][f] - s->base[w][f];
}

static qboolean ws_any(const int *v)
{
	int f;

	for (f = 0; f < WS_NF; f++)
		if (v[f])
			return true;
	return false;
}

static void ws_line_hist(const ws_hist_t *h)
{
	char fields[256];

	ws_fields(fields, sizeof(fields), h->v);
	ws_line("wstats p=%d r=%d w=%d%s", h->p, h->r, h->w, fields);
}

/* ------------------------------------------------------------------ */
/* the scoreboard: pstats (per player) and wteam                        */

#define WS_PING_FRAMES  (5 * (HZ > 0 ? HZ : 10))

static const char *const ws_pkey[5] = { "score", "k", "d", "dmg", "ping" };

static void ws_line_pstats(int p)
{
	const int *v = ws.sids[p].ps;

	ws_line("pstats p=%d %s=%d %s=%d %s=%d %s=%d %s=%d", p,
		ws_pkey[0], v[0], ws_pkey[1], v[1], ws_pkey[2], v[2], ws_pkey[3], v[3], ws_pkey[4], v[4]);
}

// Refresh a connected player's scoreboard numbers; true when they changed
// (the ping only counts every few seconds, or it would be every line).
static qboolean ws_pstats_update(int p, qboolean force)
{
	ws_sid_t *s = &ws.sids[p];
	edict_t *ent;
	int v[5];
	qboolean changed;

	if (s->slot < 0)
		return force;
	ent = g_edicts + 1 + s->slot;
	if (!ent->inuse || !ent->client)
		return force;
	v[0] = ent->client->resp.score;
	v[1] = ent->client->resp.kills;
	v[2] = ent->client->resp.deaths;
	v[3] = ent->client->resp.damage_dealt;
	v[4] = ent->client->ping;
	changed = memcmp(v, s->ps, 4 * sizeof(int)) != 0;
	if (v[4] != s->ps[4] && level.realFramenum - s->ping_at >= WS_PING_FRAMES)
		changed = true;
	if (changed || force) {
		memcpy(s->ps, v, sizeof(v));
		s->ping_at = level.realFramenum;
	}
	return changed || force;
}

static unsigned ws_hash(const char *s)
{
	unsigned h = 5381;

	while (*s)
		h = h * 33 + (unsigned char)*s++;
	return h;
}

static void ws_line_wteam(int t)
{
	char name[56];

	ws_quote(name, sizeof(name), teams[t].name);
	ws_line("wteam t=%d name=\"%s\" score=%d icon=%s", t, name, teams[t].score,
		teams[t].skin_index[0] ? teams[t].skin_index : "-");
}

static int ws_teams(void)
{
	return teamplay->value ? (teamCount < TEAM_TOP - 1 ? teamCount : TEAM_TOP - 1) : 0;
}

/* ------------------------------------------------------------------ */
/* stats ids                                                          */

static int ws_find(const char *token, const char *steamid, const char *ip, const char *name)
{
	int p;

	if (token[0]) {
		for (p = 1; p <= ws.nsids; p++)
			if (ws.sids[p].slot < 0 && !strcmp(ws.sids[p].token, token))
				return p;
	}
	if (steamid[0] && strcmp(steamid, "0")) {
		for (p = 1; p <= ws.nsids; p++)
			if (ws.sids[p].slot < 0 && !strcmp(ws.sids[p].steamid, steamid))
				return p;
	}
	for (p = 1; p <= ws.nsids; p++)
		if (ws.sids[p].slot < 0 && !strcmp(ws.sids[p].ip, ip) && !strcmp(ws.sids[p].name, name))
			return p;
	return 0;
}

static void ws_send_token(edict_t *ent, int p)
{
	if (ws_is_dummy(ent) || !p)
		return;
	ws_begin(WS_TO_ONE, ent);
	ws_line("sid p=%d token=%s", p, ws.sids[p].token);
	ws_flush();
}

// What a client that has just subscribed needs: the scope, its token,
// who is who, the map's totals and the round so far; then the history.
static void ws_snapshot(edict_t *ent)
{
	int slot = ws_slot(ent), p, w;
	char fields[256];
	int v[WS_NF];

	ws_begin(WS_TO_ONE, ent);
	ws_line("wscope map=%s r=%d going=%d", level.mapname, ws.round, ws.going);
	ws_flush();
	ws_send_token(ent, ws.cl[slot].sid);

	ws_begin(WS_TO_ONE, ent);
	for (p = 1; p <= ws.nsids; p++)
		ws_line_sidmap(p);
	for (p = 1; p <= ws.nsids; p++) {
		ws_pstats_update(p, true);
		ws_line_pstats(p);
	}
	for (w = 1; w <= ws_teams(); w++)
		ws_line_wteam(w);
	// The map's totals of the rounds that are over: a round being played
	// comes as wlive on top (to a spectator), and goes into these at its end
	for (p = 1; p <= ws.nsids; p++) {
		for (w = 1; w < WS_NW; w++) {
			const int *m = ws.going ? ws.sids[p].base[w] : ws.sids[p].tot[w];

			if (!ws_any(m))
				continue;
			ws_fields(fields, sizeof(fields), m);
			ws_line("wstats p=%d scope=map w=%d%s", p, w, fields);
		}
	}
	if (ws.going && ws_watcher(ent)) {
		for (p = 1; p <= ws.nsids; p++) {
			for (w = 1; w < WS_NW; w++) {
				ws_round_values(&ws.sids[p], w, v);
				if (!ws_any(v))
					continue;
				ws_fields(fields, sizeof(fields), v);
				ws_line("wlive p=%d r=%d w=%d%s f=%d", p, ws.round, w, fields, level.framenum);
			}
		}
	}
	ws_flush();

	// who is alive: the next frame sends the list, if they may see it
	ws.cl[slot].deadview = false;

	// history is in the demo already, as it happened
	ws.cl[slot].hist_next = ws_is_dummy(ent) ? -2 : ws.nhist - 1;
	ws.cl[slot].budget = 0;
}

static void ws_subscribe(edict_t *ent)
{
	int slot = ws_slot(ent);
	qboolean wants;

	if (slot < 0 || !ws.cl[slot].begun)
		return;
	wants = ws_on() && ws_wants(ent);
	if (wants && !ws.cl[slot].sub) {
		ws.cl[slot].sub = true;
		ws.cl[slot].req_next = -1;
		ws_snapshot(ent);
	} else if (!wants) {
		ws.cl[slot].sub = false;
	}
}

void WS_ClientBegin(edict_t *ent)
{
	int slot = ws_slot(ent), p;
	ws_client_t *c;
	ws_sid_t *s;
	const char *token, *steamid;

	if (slot < 0)
		return;
	c = &ws.cl[slot];

	if (!c->begun) {
		c->begun = true;
		c->sub = false;
		c->hist_next = -2;
		c->req_next = -1;
	}

	if (!c->sid && !ws_is_dummy(ent)) {
		token = Info_ValueForKey(ent->client->pers.userinfo, "aq2sid");
		steamid = Info_ValueForKey(ent->client->pers.userinfo, "steamid");
		p = ws_find(token, steamid, ent->client->pers.ip, ent->client->pers.netname);
		if (!p && ws.nsids < WS_MAX_SIDS) {
			p = ++ws.nsids;
			s = &ws.sids[p];
			memset(s, 0, sizeof(*s));
			s->used = true;
			ws_token(s->token, sizeof(s->token), p);
		}
		if (p) {
			s = &ws.sids[p];
			s->slot = slot;
			Q_strncpyz(s->name, ent->client->pers.netname, sizeof(s->name));
			Q_strncpyz(s->steamid, steamid, sizeof(s->steamid));
			Q_strncpyz(s->ip, ent->client->pers.ip, sizeof(s->ip));
			Q_strncpyz(s->sent_name, s->name, sizeof(s->sent_name));
			s->sent_team = ent->client->resp.team;
			c->sid = p;
			if (ws_on()) {
				ws_begin(WS_TO_ALL, NULL);
				ws_line_sidmap(p);
				ws_flush();
			}
		}
	}

	ws_subscribe(ent);
}

void WS_ClientUserinfoChanged(edict_t *ent)
{
	ws_subscribe(ent);
}

void WS_ClientDisconnect(edict_t *ent)
{
	int slot = ws_slot(ent);
	ws_client_t *c;

	if (slot < 0)
		return;
	c = &ws.cl[slot];
	if (c->sid) {
		ws.sids[c->sid].slot = -1;
		if (ws_on()) {
			c->sub = false;     // not to the one leaving
			ws_begin(WS_TO_ALL, NULL);
			ws_line_sidmap(c->sid);
			ws_flush();
		}
	}
	memset(c, 0, sizeof(*c));
	c->hist_next = -2;
	c->req_next = -1;
}

/* ------------------------------------------------------------------ */
/* counting                                                           */

static void ws_add(edict_t *ent, int mod, int f, int n)
{
	ws_sid_t *s = ws_sid_of(ent);
	int w = ws_weapon(mod);

	if (!s || !w || !n)
		return;
	s->tot[w][f] += n;
	s->changed |= 1u << w;
}

void WS_Shot(edict_t *ent, int mod)
{
	ws_add(ent, mod, F_SHOTS, 1);
}

void WS_Hit(edict_t *ent, int mod, int loc)
{
	ws_add(ent, mod, F_HITS, 1);
	if (loc == LOC_HDAM || loc == LOC_KVLR_HELMET)
		ws_add(ent, mod, F_HS, 1);
	if (loc >= LOC_HDAM && loc <= LOC_NO)
		ws_add(ent, mod, F_HEAD + (loc - LOC_HDAM), 1);
}

void WS_Damage(edict_t *ent, int mod, int damage)
{
	ws_add(ent, mod, F_DMG, damage);
}

void WS_Kill(edict_t *ent, int mod)
{
	ws_add(ent, mod, F_K, 1);
}

/* ------------------------------------------------------------------ */
/* scope, rounds and the map                                          */

void WS_Init(void)
{
	wstats = gi.cvar("wstats", "1", 0);
	wstats_mvd = gi.cvar("wstats_mvd", "1", 0);
	wstats_live_ms = gi.cvar("wstats_live_ms", "1000", 0);
}

static void ws_clear_counts(void)
{
	int p;

	for (p = 1; p <= ws.nsids; p++) {
		memset(ws.sids[p].tot, 0, sizeof(ws.sids[p].tot));
		memset(ws.sids[p].base, 0, sizeof(ws.sids[p].base));
		memset(ws.sids[p].sent, 0, sizeof(ws.sids[p].sent));
		ws.sids[p].changed = 0;
	}
	ws.nhist = 0;
}

void WS_MapStart(void)
{
	if (ws.hist)
		gi.TagFree(ws.hist);
	memset(&ws, 0, sizeof(ws));
	// without rounds the whole map is one, numbered 0
	ws.going = !teamplay->value;
	for (int i = 0; i < MAX_CLIENTS; i++) {
		ws.cl[i].hist_next = -2;
		ws.cl[i].req_next = -1;
	}
}

// The match was reset (ResetScores): what was counted no longer counts.
void WS_ResetScope(void)
{
	int i;

	ws_clear_counts();
	ws.round = 0;
	ws.going = !teamplay->value;
	ws.map_ended = false;
	for (i = 0; i < game.maxclients; i++) {
		ws.cl[i].hist_next = -2;
		ws.cl[i].req_next = -1;
	}
	if (ws_on()) {
		ws_begin(WS_TO_ALL, NULL);
		ws_line("wscope map=%s r=0 going=%d state=reset", level.mapname, ws.going);
		ws_flush();
	}
}

void WS_RoundStart(void)
{
	int p;

	ws.round++;
	ws.going = true;
	ws.map_ended = false;
	for (p = 1; p <= ws.nsids; p++) {
		memcpy(ws.sids[p].base, ws.sids[p].tot, sizeof(ws.sids[p].base));
		memcpy(ws.sids[p].sent, ws.sids[p].tot, sizeof(ws.sids[p].sent));
		ws.sids[p].changed = 0;
	}
	if (ws_on()) {
		ws_begin(WS_TO_ALL, NULL);
		ws_line("wround r=%d state=start f=%d", ws.round, level.framenum);
		ws_flush();
	}
}

static void ws_hist_add(int p, int r, int w, const int *v)
{
	ws_hist_t *h;

	if (ws.nhist >= WS_HIST_MAX)
		return;
	if (ws.nhist == ws.hist_alloc) {
		int n = ws.hist_alloc ? ws.hist_alloc * 2 : 256;
		ws_hist_t *nh = gi.TagMalloc(n * sizeof(*nh), TAG_GAME);

		if (ws.hist) {
			memcpy(nh, ws.hist, ws.nhist * sizeof(*nh));
			gi.TagFree(ws.hist);
		}
		ws.hist = nh;
		ws.hist_alloc = n;
	}
	h = &ws.hist[ws.nhist++];
	h->p = p;
	h->r = r > 255 ? 255 : r;
	h->w = w;
	memcpy(h->v, v, sizeof(h->v));
}

void WS_RoundEnd(void)
{
	int p, w, first = ws.nhist, i;
	int v[WS_NF];

	if (!teamplay->value || !ws.going || !ws.round)
		return;
	ws.going = false;

	for (p = 1; p <= ws.nsids; p++) {
		for (w = 1; w < WS_NW; w++) {
			ws_round_values(&ws.sids[p], w, v);
			if (ws_any(v))
				ws_hist_add(p, ws.round, w, v);
		}
		ws.sids[p].changed = 0;
	}
	if (!ws_on())
		return;
	ws_begin(WS_TO_ALL, NULL);
	for (i = first; i < ws.nhist; i++)
		ws_line_hist(&ws.hist[i]);
	// and the map's totals of whoever played it, which now include it
	for (i = first; i < ws.nhist; i++) {
		char fields[256];
		const ws_hist_t *h = &ws.hist[i];

		ws_fields(fields, sizeof(fields), ws.sids[h->p].tot[h->w]);
		ws_line("wstats p=%d scope=map w=%d%s", h->p, h->w, fields);
	}
	ws_line("wround r=%d state=end f=%d", ws.round, level.framenum);
	ws_flush();
}

void WS_MapEnd(void)
{
	int p, w;
	char fields[256];

	if (ws.map_ended)
		return;
	WS_RoundEnd();
	ws.map_ended = true;
	if (!ws_on())
		return;
	ws_begin(WS_TO_ALL, NULL);
	for (p = 1; p <= ws.nsids; p++) {
		for (w = 1; w < WS_NW; w++) {
			if (!ws_any(ws.sids[p].tot[w]))
				continue;
			ws_fields(fields, sizeof(fields), ws.sids[p].tot[w]);
			ws_line("wstats p=%d scope=map w=%d%s", p, w, fields);
		}
	}
	ws_line("wscope map=%s r=%d state=end", level.mapname, ws.round);
	ws_flush();
}

/* ------------------------------------------------------------------ */
/* the frame: wlive, sidmap changes, paced history                     */

static void ws_live(void)
{
	int p, w, f;
	char fields[256];
	size_t len;

	ws_begin(WS_TO_WATCHERS, NULL);
	for (p = 1; p <= ws.nsids; p++) {
		ws_sid_t *s = &ws.sids[p];

		if (!s->changed)
			continue;
		for (w = 1; w < WS_NW; w++) {
			if (!(s->changed & (1u << w)))
				continue;
			fields[0] = 0;
			len = 0;
			for (f = 0; f < WS_NF; f++) {
				if (s->tot[w][f] != s->sent[w][f])
					len += Q_snprintf(fields + len, sizeof(fields) - len, " %s=%d",
						ws_fkey[f], s->tot[w][f] - s->base[w][f]);
			}
			memcpy(s->sent[w], s->tot[w], sizeof(s->sent[w]));
			if (len)
				ws_line("wlive p=%d r=%d w=%d%s f=%d", p, ws.round, w, fields, level.framenum);
		}
		s->changed = 0;
	}
	ws_flush();
}

static void ws_sidmap_changes(void)
{
	int p;

	ws_begin(WS_TO_ALL, NULL);
	for (p = 1; p <= ws.nsids; p++) {
		ws_sid_t *s = &ws.sids[p];
		edict_t *ent;

		if (s->slot < 0)
			continue;
		ent = g_edicts + 1 + s->slot;
		if (!ent->inuse || !ent->client)
			continue;
		if (!strcmp(s->sent_name, ent->client->pers.netname) && s->sent_team == ent->client->resp.team)
			continue;
		Q_strncpyz(s->name, ent->client->pers.netname, sizeof(s->name));
		Q_strncpyz(s->sent_name, s->name, sizeof(s->sent_name));
		s->sent_team = ent->client->resp.team;
		ws_line_sidmap(p);
	}
	for (p = 1; p <= ws.nsids; p++) {
		if (ws.sids[p].slot >= 0 && ws_pstats_update(p, false))
			ws_line_pstats(p);
	}
	for (p = 1; p <= ws_teams(); p++) {
		int sc = teams[p].score, nh = (int)(ws_hash(teams[p].name) ^ ws_hash(teams[p].skin_index) * 31);

		if (ws.team_sent[p][0] == sc && ws.team_sent[p][1] == nh)
			continue;
		ws.team_sent[p][0] = sc;
		ws.team_sent[p][1] = nh;
		ws_line_wteam(p);
	}
	ws_flush();
}

static qboolean ws_req_match(const ws_client_t *c, const ws_hist_t *h)
{
	return (!c->req_p || h->p == c->req_p) && (!c->req_r || h->r == c->req_r);
}

// History to one client, within its byte budget: a wstats_get first,
// then the backfill, newest round first.
static void ws_history(edict_t *ent, ws_client_t *c)
{
	c->budget += WS_RATE / (HZ > 0 ? HZ : 10);
	if (c->budget > WS_CHUNK * 2)
		c->budget = WS_CHUNK * 2;

	ws_begin(WS_TO_ONE, ent);
	while (c->req_next >= 0 && c->budget > 0) {
		if (c->req_next >= ws.nhist) {
			ws_line("wstats_get state=done n=%d", c->req_lines);
			c->req_next = -1;
			break;
		}
		if (ws_req_match(c, &ws.hist[c->req_next])) {
			ws_line_hist(&ws.hist[c->req_next]);
			c->req_lines++;
			c->budget -= 100;
		}
		c->req_next++;
	}
	while (c->req_next < 0 && c->hist_next >= -1 && c->budget > 0) {
		if (c->hist_next == -1) {
			ws_line("wstats_hist state=done r=1-%d", ws.round);
			c->hist_next = -2;
			break;
		}
		if (c->hist_next >= ws.nhist)
			c->hist_next = ws.nhist - 1;
		else {
			ws_line_hist(&ws.hist[c->hist_next--]);
			c->budget -= 100;
		}
	}
	ws_flush();
}

/*
 * Who is alive, as the server's own scoreboard shows it: only to one who
 * may know - dead, spectating, or between rounds (A_NewScoreboardMessage's
 * `dead`) - and the dummy. Who becomes one gets the whole list; who stops
 * being one (spawned at a round's start) is told to hide it.
 */
static qboolean ws_alive(const ws_sid_t *s)
{
	const edict_t *ent;

	if (s->slot < 0)
		return false;
	ent = g_edicts + 1 + s->slot;
	return ent->inuse && ent->client && ent->client->resp.team != NOTEAM && IS_ALIVE(ent);
}

static qboolean ws_deadview(const edict_t *ent)
{
	// the demo always: it shows every player's every move anyway
	if (ws_is_dummy(ent))
		return true;
	// anyone else as the server's scoreboards: nobody under limchasecam -
	// they hide who is alive "during matches" then (A_ScoreboardMessage) -
	// and otherwise one who is dead or spectating, or between rounds
	if (limchasecam->value != 0)
		return false;
	return !IS_ALIVE(ent) || !team_round_going;
}

static void ws_alive_frame(void)
{
	int i, p;

	if (!teamplay->value)
		return;

	for (i = 0; i < game.maxclients; i++) {
		ws_client_t *c = &ws.cl[i];
		edict_t *ent = g_edicts + 1 + i;
		qboolean dv;

		if (!c->sub || !ent->inuse || !ent->client)
			continue;
		dv = ws_deadview(ent);
		if (dv == c->deadview)
			continue;
		c->deadview = dv;
		ws_begin(WS_TO_ONE, ent);
		if (dv) {
			ws_line("alive state=list");
			for (p = 1; p <= ws.nsids; p++)
				ws_line("alive p=%d a=%d", p, ws_alive(&ws.sids[p]));
		} else {
			ws_line("alive state=hidden");
		}
		ws_flush();
	}

	ws_begin(WS_TO_DEADVIEW, NULL);
	for (p = 1; p <= ws.nsids; p++) {
		int a = ws_alive(&ws.sids[p]);
		if (a == ws.sids[p].alive)
			continue;
		ws.sids[p].alive = a;
		ws_line("alive p=%d a=%d", p, a);
	}
	ws_flush();
}

void WS_Frame(void)
{
	int i, ms = wstats_live_ms->value;

	if (!ws_on())
		return;

	if (!(level.realFramenum % (HZ > 0 ? HZ : 10)))
		ws_sidmap_changes();

	ws_alive_frame();

	if (ms > 0 && ws.going && (level.realFramenum - ws.live_at) * 1000 >= ms * (HZ > 0 ? HZ : 10)) {
		ws.live_at = level.realFramenum;
		ws_live();
	}

	for (i = 0; i < game.maxclients; i++) {
		ws_client_t *c = &ws.cl[i];
		edict_t *ent = g_edicts + 1 + i;

		if (!c->sub || !ent->inuse || ws_is_dummy(ent))
			continue;
		if (c->req_next >= 0 || c->hist_next >= -1)
			ws_history(ent, c);
	}
}

// wstats_get <p|*> <r|all>
void WS_Cmd_Get(edict_t *ent)
{
	int slot = ws_slot(ent);
	ws_client_t *c;
	const char *a1 = gi.argv(1), *a2 = gi.argv(2);

	if (slot < 0)
		return;
	c = &ws.cl[slot];
	if (!c->sub || ws_is_dummy(ent))
		return;     // a reply could drop an older client: only to subscribers
	if (gi.argc() < 3) {
		gi.cprintf(ent, PRINT_HIGH, "Usage: wstats_get <p|*> <round|all>\n");
		return;
	}
	c->req_p = !strcmp(a1, "*") ? 0 : atoi(a1);
	c->req_r = !Q_stricmp(a2, "all") ? 0 : atoi(a2);
	c->req_next = 0;
	c->req_lines = 0;
}
