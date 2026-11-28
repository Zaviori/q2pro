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
#define WS_LOG_MAX      4096        // kill and death lines kept per map, for the rounds view
#define WS_LOG_LINE     200
#define WS_ROUNDS       256         // rounds a map keeps a log of (1..255)

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
	int      sent_sub;              // a substitute (matchmode's resp.subteam), as last sent
	int      tot[WS_NW][WS_NF];
	int      base[WS_NW][WS_NF];
	int      sent[WS_NW][WS_NF];
	unsigned changed;               // bit per weapon, since the last wlive
	int      ps[7];                 // pstats as last sent: score k d dmg ping min bot
	int      ping_at;               // realFramenum the ping was last sent
	int      alive;                 // as last sent in an alive line: 0, 1
	int      held;                  // the weapon in hand as last sent (curr_weap)
} ws_sid_t;

typedef struct {
	unsigned char p, r, w;
	int v[WS_NF];
} ws_hist_t;

// a round as it went, for the rounds view: kept for a late joiner
typedef struct {
	int      start, end;            // level.framenum at its start, its end (0 not over)
	int      winner;
	char     reason[12];
	int      ev_first, ev_n;        // its kill and death lines in ws.log
} ws_round_t;

typedef struct {
	qboolean begun;                 // in the game this map
	qboolean sub;                   // subscribed
	int      sid;                   // 1-based, 0 = none
	int      budget;                // history bytes it may still send now
	int      hist_next;             // backfill: next record, newest first; -1 the done line; -2 idle
	int      log_next;              // backfill: the round log to send next, newest first; 0 none
	int      req_p, req_r;          // a wstats_get being answered: p 0 = all, r 0 = all
	int      req_next;              // its next record; -1 none
	int      req_lines;
	qboolean deadview;              // may see who is alive: has the list
	qboolean events;                // takes the event lines too (userinfo aq2events)
} ws_client_t;

static struct {
	ws_sid_t     sids[WS_MAX_SIDS + 1];
	int          nsids;
	ws_client_t  cl[MAX_CLIENTS];
	ws_hist_t   *hist;
	int          nhist, hist_alloc;
	ws_round_t   rounds[WS_ROUNDS];
	char       (*log)[WS_LOG_LINE];
	int          nlog, log_alloc;
	int          round;             // rounds started this map
	qboolean     going;             // a round is being counted
	qboolean     map_ended;
	int          live_at;           // realFramenum of the last wlive
	int          team_sent[TEAM_TOP][4];    // wteam as last sent: score, name hash, ready, captain
	qboolean     paused;            // the match paused or in a timeout, as last told
	const char   *end_reason;       // why the round ending now ended: WS_RoundEndReason
	short        dmg_on[MAX_CLIENTS][MAX_CLIENTS];  // damage on a victim (row) by each attacker, this life
} ws;

// Non-NULL on an engine that keeps svc_userstatistic out of the MVD but
// for the dummy's copy and plays it only to AQtion clients. On an older
// engine every subscriber's copy would be recorded, and the demo would
// drop its older-protocol viewers: nothing is sent there.
void *engine_Stats_Channel;

static cvar_t *wstats;              // 0: count, send nothing
static cvar_t *wstats_mvd;          // 0: keep the stats out of the MVD
static cvar_t *wstats_live_ms;      // wlive interval, 0 = off
static cvar_t *wstats_events;       // 0: no event lines to anybody

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
#define WS_TO_EVENTS    4   // the event subscribers, the dummy included
#define WS_TO_EVENTS_WATCH 5    // those of them who watch: spectators and the dummy
#define WS_TO_EVENTS_PLAY  6    // those of them who play: not told the kills under limchasecam

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
			if ((ws_mode == WS_TO_EVENTS || ws_mode == WS_TO_EVENTS_WATCH) && !ws.cl[i].events)
				continue;
			if (ws_mode == WS_TO_EVENTS_WATCH && !ws_watcher(ent))
				continue;
			if (ws_mode == WS_TO_EVENTS_PLAY && (!ws.cl[i].events || ws_watcher(ent)))
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
	ws_line("sidmap p=%d c=%d name=\"%s\" team=%d%s", p, s->slot, name, s->sent_team,
		s->sent_sub ? " sub=1" : "");
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

static const char *const ws_pkey[7] = { "score", "k", "d", "dmg", "ping", "min", "bot" };

// min (minutes on the server) and bot (the server's list shows "BOT" for
// the ping) are the list page's, at the end of the line
static void ws_line_pstats(int p)
{
	const int *v = ws.sids[p].ps;

	ws_line("pstats p=%d %s=%d %s=%d %s=%d %s=%d %s=%d %s=%d %s=%d", p,
		ws_pkey[0], v[0], ws_pkey[1], v[1], ws_pkey[2], v[2], ws_pkey[3], v[3], ws_pkey[4], v[4],
		ws_pkey[5], v[5], ws_pkey[6], v[6]);
}

// Refresh a connected player's scoreboard numbers; true when they changed
// (the ping only counts every few seconds, or it would be every line).
static qboolean ws_pstats_update(int p, qboolean force)
{
	ws_sid_t *s = &ws.sids[p];
	edict_t *ent;
	int v[7];
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
	v[5] = (level.framenum - ent->client->resp.enterframe) / (60 * HZ);
	v[6] = 0;
#ifndef NO_BOTS
	// as the server's own list (A_ScoreboardMessage, 'P'): "BOT", or the
	// bot's made-up ping when it reports as a client
	if (IS_BOT(ent)) {
		if (bot_reportasclient->value && bot_reportpings->value)
			v[4] = ent->bot.bot_ping;
		else
			v[6] = 1;
	}
#endif
	changed = memcmp(v, s->ps, 4 * sizeof(int)) != 0 ||
		v[5] != s->ps[5] || v[6] != s->ps[6];
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

// the stats id of a team's captain, 0 for none
static int ws_team_captain(int t)
{
	edict_t *c = teams[t].captain;
	int slot;

	if (!c || !c->inuse || !c->client)
		return 0;
	slot = c - g_edicts - 1;
	return slot >= 0 && slot < game.maxclients ? ws.cl[slot].sid : 0;
}

static void ws_line_wteam(int t)
{
	char name[56];

	ws_quote(name, sizeof(name), teams[t].name);
	// ready only in matchmode, where the server's own board shows it
	ws_line("wteam t=%d name=\"%s\" score=%d icon=%s captain=%d%s", t, name, teams[t].score,
		teams[t].skin_index[0] ? teams[t].skin_index : "-", ws_team_captain(t),
		matchmode->value ? (teams[t].ready ? " ready=1" : " ready=0") : "");
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
// The dummy's history is in the demo as it happened, unless the stream
// started after it (backfill: WS_MvdStreamStarted).
static void ws_snapshot(edict_t *ent, qboolean backfill)
{
	int slot = ws_slot(ent), p, w;
	char fields[256];
	int v[WS_NF];

	ws_begin(WS_TO_ONE, ent);
	ws_line("wscope map=%s r=%d going=%d hz=%d", level.mapname, ws.round, ws.going, HZ);
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

	ws.cl[slot].hist_next = backfill ? ws.nhist - 1 : -2;
	ws.cl[slot].log_next = backfill ? min(ws.going ? ws.round - 1 : ws.round, WS_ROUNDS - 1) : 0;
	ws.cl[slot].budget = 0;

	// the backfill's start, for the client's "loading" until its done line
	if (backfill) {
		ws_begin(WS_TO_ONE, ent);
		ws_line("wstats_hist state=start r=1-%d", ws.round);
		ws_flush();
	}
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
		ws_snapshot(ent, !ws_is_dummy(ent));
	} else if (!wants) {
		ws.cl[slot].sub = false;
	}
	// the events, a subscription of their own (aq2events): the demo has
	// them where the server allows them at all
	ws.cl[slot].events = ws.cl[slot].sub && wstats_events->value &&
		(ws_is_dummy(ent) || atoi(Info_ValueForKey(ent->client->pers.userinfo, "aq2events")) >= 1);
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
		c->log_next = 0;
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
			s->sent_sub = ent->client->resp.subteam != 0;
			c->sid = p;
			if (ws_on()) {
				ws_begin(WS_TO_ALL, NULL);
				ws_line_sidmap(p);
				ws_flush();
				ws_begin(WS_TO_EVENTS, NULL);
				ws_line("player p=%d state=join c=%d f=%d", p, slot, level.framenum);
				ws_flush();
			}
		}
	}

	ws_subscribe(ent);
}

// An MVD stream (a recording, a GTV client, a new map's gamestate) has
// just started: what the dummy was sent before it is in no stream - the
// engine records nothing while inactive, and the dummy begins before the
// gamestate is written. Send its snapshot again, with the rounds already
// played, so a demo knows who each p is from its first frame.
void WS_MvdStreamStarted(void)
{
	int i;

	if (!ws_on())
		return;
	for (i = 0; i < game.maxclients; i++) {
		edict_t *ent = g_edicts + 1 + i;

		if (!ent->inuse || !ws_is_dummy(ent) || !ws.cl[i].sub)
			continue;
		ws_snapshot(ent, true);
	}
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
			c->sub = c->events = false;     // not to the one leaving
			ws_begin(WS_TO_ALL, NULL);
			ws_line_sidmap(c->sid);
			ws_flush();
			ws_begin(WS_TO_EVENTS, NULL);
			ws_line("player p=%d state=leave f=%d", c->sid, level.framenum);
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
	// a knife or a grenade let go of: an event, for those watching
	if ((mod == MOD_KNIFE_THROWN || mod == MOD_HG_SPLASH) && ws_on() && ws_sid_of(ent)) {
		ws_begin(WS_TO_EVENTS_WATCH, NULL);
		ws_line("throw p=%d kind=%s f=%d", (int)(ws_sid_of(ent) - ws.sids),
			mod == MOD_KNIFE_THROWN ? "knife" : "grenade", level.framenum);
		ws_flush();
	}
}

void WS_Hit(edict_t *ent, int mod, int loc)
{
	ws_add(ent, mod, F_HITS, 1);
	if (loc == LOC_HDAM || loc == LOC_KVLR_HELMET)
		ws_add(ent, mod, F_HS, 1);
	if (loc >= LOC_HDAM && loc <= LOC_NO)
		ws_add(ent, mod, F_HEAD + (loc - LOC_HDAM), 1);
}

void WS_Damage(edict_t *ent, edict_t *targ, int mod, int damage)
{
	int a = ws_slot(ent), v = targ ? ws_slot(targ) : -1;

	ws_add(ent, mod, F_DMG, damage);
	// what this attacker has done to this victim, for the kill line
	if (a >= 0 && v >= 0 && ws.dmg_on[v][a] < 30000)
		ws.dmg_on[v][a] += damage;
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
	wstats_events = gi.cvar("wstats_events", "1", 0);
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
	ws.nlog = 0;
	memset(ws.rounds, 0, sizeof(ws.rounds));
}

void WS_MapStart(void)
{
	if (ws.hist)
		gi.TagFree(ws.hist);
	if (ws.log)
		gi.TagFree(ws.log);
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
		ws.cl[i].log_next = 0;
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
	if (ws.round < WS_ROUNDS) {
		ws_round_t *rd = &ws.rounds[ws.round];
		memset(rd, 0, sizeof(*rd));
		rd->start = level.framenum;
		rd->ev_first = ws.nlog;
	}
	for (p = 1; p <= ws.nsids; p++) {
		memcpy(ws.sids[p].base, ws.sids[p].tot, sizeof(ws.sids[p].base));
		memcpy(ws.sids[p].sent, ws.sids[p].tot, sizeof(ws.sids[p].sent));
		ws.sids[p].changed = 0;
	}
	if (ws_on()) {
		ws_begin(WS_TO_ALL, NULL);
		ws_line("wround r=%d state=start f=%d", ws.round, level.framenum);
		ws_flush();
		if (ws.round == 1) {
			ws_begin(WS_TO_EVENTS, NULL);
			ws_line("match state=start f=%d", level.framenum);
			ws_flush();
		}
	}
	memset(ws.dmg_on, 0, sizeof(ws.dmg_on));
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

// the round ending next ended for this (the round time limit says so
// before it calls WonGame); an elimination otherwise
void WS_RoundEndReason(const char *reason)
{
	ws.end_reason = reason;
}

void WS_RoundEnd(int winner)
{
	int p, w, first = ws.nhist, i;
	int v[WS_NF];
	const char *reason = ws.end_reason ? ws.end_reason : "eliminated";

	ws.end_reason = NULL;
	if (!teamplay->value || !ws.going || !ws.round)
		return;
	ws.going = false;
	if (ws.round < WS_ROUNDS) {
		ws_round_t *rd = &ws.rounds[ws.round];
		rd->end = level.framenum;
		rd->winner = winner > NOTEAM && winner < TEAM_TOP ? winner : 0;
		Q_strncpyz(rd->reason, reason, sizeof(rd->reason));
	}

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
	// the winner (0 a tie or none) and why
	ws_line("wround r=%d state=end f=%d winner=%d reason=%s", ws.round, level.framenum,
		winner > NOTEAM && winner < TEAM_TOP ? winner : 0, reason);
	ws_flush();

	// Under limchasecam the players were not told the kills as they
	// happened: the round is over now, and its log goes to them
	if (limchasecam->value != 0 && ws.round < WS_ROUNDS) {
		const ws_round_t *rd = &ws.rounds[ws.round];
		ws_begin(WS_TO_EVENTS_PLAY, NULL);
		for (i = 0; i < rd->ev_n; i++)
			ws_line("%s", ws.log[rd->ev_first + i]);
		ws_flush();
	}
}

void WS_MapEnd(void)
{
	int p, w;
	char fields[256];

	if (ws.map_ended)
		return;
	WS_RoundEndReason("mapend");
	WS_RoundEnd(0);
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
	ws_begin(WS_TO_EVENTS, NULL);
	ws_line("match state=over r=%d f=%d", ws.round, level.framenum);
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

// player events found by the tick, sent after its own lines: to the
// event subscribers, a batch of their own
static char ws_ev[32][96];
static int ws_nev;

static void ws_ev_add(const char *fmt, ...)
{
	va_list ap;

	if (ws_nev >= (int)q_countof(ws_ev))
		return;
	va_start(ap, fmt);
	Q_vsnprintf(ws_ev[ws_nev++], sizeof(ws_ev[0]), fmt, ap);
	va_end(ap);
}

static void ws_sidmap_changes(void)
{
	int p;

	ws_nev = 0;
	ws_begin(WS_TO_ALL, NULL);
	for (p = 1; p <= ws.nsids; p++) {
		ws_sid_t *s = &ws.sids[p];
		edict_t *ent;

		if (s->slot < 0)
			continue;
		ent = g_edicts + 1 + s->slot;
		if (!ent->inuse || !ent->client)
			continue;
		if (!strcmp(s->sent_name, ent->client->pers.netname) && s->sent_team == ent->client->resp.team &&
		    s->sent_sub == (ent->client->resp.subteam != 0))
			continue;
		if (s->sent_team != ent->client->resp.team)
			ws_ev_add("player p=%d state=team team=%d f=%d", p, ent->client->resp.team, level.framenum);
		if (s->sent_sub != (ent->client->resp.subteam != 0))
			ws_ev_add("player p=%d state=%s team=%d f=%d", p,
				ent->client->resp.subteam ? "sub" : "unsub", ent->client->resp.team, level.framenum);
		Q_strncpyz(s->name, ent->client->pers.netname, sizeof(s->name));
		Q_strncpyz(s->sent_name, s->name, sizeof(s->sent_name));
		s->sent_team = ent->client->resp.team;
		s->sent_sub = ent->client->resp.subteam != 0;
		ws_line_sidmap(p);
	}
	for (p = 1; p <= ws.nsids; p++) {
		if (ws.sids[p].slot >= 0 && ws_pstats_update(p, false))
			ws_line_pstats(p);
	}
	for (p = 1; p <= ws_teams(); p++) {
		int sc = teams[p].score, nh = (int)(ws_hash(teams[p].name) ^ ws_hash(teams[p].skin_index) * 31);

		int rd = matchmode->value ? (teams[p].ready ? 1 : 0) : -1;

		int cp = ws_team_captain(p);

		if (ws.team_sent[p][0] == sc && ws.team_sent[p][1] == nh && ws.team_sent[p][2] == rd &&
		    ws.team_sent[p][3] == cp)
			continue;
		if (cp && ws.team_sent[p][3] != cp)
			ws_ev_add("player p=%d state=captain team=%d f=%d", cp, p, level.framenum);
		if (rd >= 0 && ws.team_sent[p][2] != rd && ws.team_sent[p][2] >= 0)
			ws_ev_add("team t=%d state=%s f=%d", p, rd ? "ready" : "unready", level.framenum);
		ws.team_sent[p][0] = sc;
		ws.team_sent[p][1] = nh;
		ws.team_sent[p][2] = rd;
		ws.team_sent[p][3] = cp;
		ws_line_wteam(p);
	}
	ws_flush();

	if (ws_nev) {
		ws_begin(WS_TO_EVENTS, NULL);
		for (p = 0; p < ws_nev; p++)
			ws_line("%s", ws_ev[p]);
		ws_flush();
	}
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
		if (c->hist_next == -1 && c->log_next > 0) {
			// then each past round as it went: its result, and its kills
			// and deaths to who takes the events
			const ws_round_t *rd = &ws.rounds[c->log_next--];
			int i;
			if (!rd->end)
				continue;
			ws_line("wround_past r=%d f=%d winner=%d reason=%s start=%d",
				c->log_next + 1, rd->end, rd->winner, rd->reason, rd->start);
			c->budget -= 80;
			for (i = 0; c->events && i < rd->ev_n; i++) {
				ws_line("%s", ws.log[rd->ev_first + i]);
				c->budget -= strlen(ws.log[rd->ev_first + i]) + 6;
			}
			continue;
		}
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

/*
 * Events (aq2events): what happened, as it happened. Kills, deaths and
 * the match's state to every event subscriber; spawns, weapons in hand
 * and throws, which would give a position or a loadout away, only to the
 * watchers - spectators and the demo; chat to whoever the server prints
 * it to. Under limchasecam the kills and deaths are the watchers' alone:
 * the server hides who is alive from the players then, and every kill
 * line carries the teams' alive counts.
 */

static const char *ws_loc_name(int loc)
{
	switch (loc) {
	case LOC_HDAM:        return "head";
	case LOC_CDAM:        return "chest";
	case LOC_SDAM:        return "stomach";
	case LOC_LDAM:        return "legs";
	case LOC_KVLR_HELMET: return "helmet";
	case LOC_KVLR_VEST:   return "vest";
	default:              return "none";
	}
}

// the alive counts of the teams, "a1=4 a2=3", the dying one not among them
static void ws_alive_counts(const edict_t *dying, char *out, size_t size)
{
	int alive[TEAM_TOP] = { 0 }, t, i;
	size_t len = 0;

	for (i = 0; i < game.maxclients; i++) {
		const edict_t *e = g_edicts + 1 + i;
		if (e == dying || !e->inuse || !e->client || !IS_ALIVE(e))
			continue;
		t = e->client->resp.team;
		if (t > NOTEAM && t < TEAM_TOP)
			alive[t]++;
	}
	out[0] = 0;
	for (t = TEAM1; t <= ws_teams() && len < size; t++)
		len += Q_snprintf(out + len, size - len, " a%d=%d", t, alive[t]);
}

// a kill or death of a round being played, kept for the rounds view
static void ws_log_add(const char *line)
{
	ws_round_t *rd;

	if (!teamplay->value || !ws.going || ws.round < 1 || ws.round >= WS_ROUNDS || ws.nlog >= WS_LOG_MAX)
		return;
	if (ws.nlog == ws.log_alloc) {
		int n = ws.log_alloc ? ws.log_alloc * 2 : 128;
		char (*nl)[WS_LOG_LINE] = gi.TagMalloc(n * sizeof(*nl), TAG_GAME);

		if (ws.log) {
			memcpy(nl, ws.log, ws.nlog * sizeof(*nl));
			gi.TagFree(ws.log);
		}
		ws.log = nl;
		ws.log_alloc = n;
	}
	Q_strncpyz(ws.log[ws.nlog], line, WS_LOG_LINE);
	rd = &ws.rounds[ws.round];
	if (!rd->ev_n)
		rd->ev_first = ws.nlog;
	rd->ev_n++;
	ws.nlog++;
}

// player_die, after the obituary: meansOfDeath and locOfDeath are this death's
void WS_Death(edict_t *self, edict_t *attacker)
{
	ws_sid_t *v = ws_sid_of(self);
	int mod = meansOfDeath & ~MOD_FRIENDLY_FIRE, vs = ws_slot(self);
	char alive[48], line[WS_LOG_LINE];

	if (!ws_on() || !v)
		return;
	ws_alive_counts(self, alive, sizeof(alive));
	if (attacker && attacker != self && attacker->client && ws_sid_of(attacker)) {
		int as = ws_slot(attacker);
		Q_snprintf(line, sizeof(line), "kill a=%d v=%d w=%d loc=%s hs=%d dist=%d dmg=%d%s f=%d%s r=%d",
			(int)(ws_sid_of(attacker) - ws.sids), (int)(v - ws.sids), ws_weapon(mod),
			ws_loc_name(locOfDeath), locOfDeath == LOC_HDAM || locOfDeath == LOC_KVLR_HELMET,
			(int)Distance(attacker->s.origin, self->s.origin),
			as >= 0 && vs >= 0 ? ws.dmg_on[vs][as] : 0,
			(meansOfDeath & MOD_FRIENDLY_FIRE) ? " ff=1" : "", level.framenum, alive, ws.round);
	} else {
		const char *why =
			mod == MOD_FALLING ? "fall" : mod == MOD_WATER ? "drown" :
			mod == MOD_SLIME || mod == MOD_LAVA ? "lava" : mod == MOD_CRUSH ? "crush" :
			mod == MOD_TELEFRAG ? "telefrag" : attacker == self ? "suicide" : "world";
		Q_snprintf(line, sizeof(line), "death v=%d reason=%s f=%d%s r=%d", (int)(v - ws.sids), why,
			level.framenum, alive, ws.round);
	}
	ws_begin(limchasecam->value != 0 ? WS_TO_EVENTS_WATCH : WS_TO_EVENTS, NULL);
	ws_line("%s", line);
	ws_flush();
	ws_log_add(line);
	if (vs >= 0)
		memset(ws.dmg_on[vs], 0, sizeof(ws.dmg_on[vs]));
}

// StartLCA, the players just spawned: where each stands at LIGHTS
void WS_RoundSpawns(void)
{
	int i;

	if (!ws_on())
		return;
	ws_begin(WS_TO_EVENTS_WATCH, NULL);
	for (i = 0; i < game.maxclients; i++) {
		edict_t *e = g_edicts + 1 + i;
		ws_sid_t *s = ws_sid_of(e);
		if (!s || !e->inuse || !e->client || e->client->resp.team == NOTEAM || !IS_ALIVE(e))
			continue;
		ws_line("spawn p=%d r=%d x=%d y=%d z=%d f=%d", (int)(s - ws.sids), ws.round + 1,
			(int)e->s.origin[0], (int)e->s.origin[1], (int)e->s.origin[2], level.framenum);
	}
	ws_flush();
}

// Cmd_Say_f, as the server prints the message to `to`: the same line to
// the same recipient, if it takes the events
void WS_ChatTo(edict_t *to, edict_t *from, qboolean team, const char *text)
{
	int slot = ws_slot(to);
	ws_sid_t *s = ws_sid_of(from);
	char quoted[300];
	size_t len;

	if (!ws_on() || slot < 0 || !ws.cl[slot].events || !s)
		return;
	ws_quote(quoted, sizeof(quoted), text);
	len = strlen(quoted);
	while (len && (quoted[len - 1] == '\n' || quoted[len - 1] == '\r'))
		quoted[--len] = 0;
	ws_begin(WS_TO_ONE, to);
	ws_line("chat p=%d team=%d dead=%d text=\"%s\" f=%d", (int)(s - ws.sids), team ? 1 : 0,
		teamplay->value && !IS_ALIVE(from), quoted, level.framenum);
	ws_flush();
}

// every frame: weapons taken in hand, and the match paused or going on
static void ws_events_frame(void)
{
	qboolean paused = level.pauseFrames > 0 || level.timeoutFrames > 0;
	int p;

	if (paused != ws.paused) {
		ws.paused = paused;
		ws_begin(WS_TO_EVENTS, NULL);
		ws_line("match state=%s f=%d", paused ? "pause" : "resume", level.framenum);
		ws_flush();
	}

	ws_begin(WS_TO_EVENTS_WATCH, NULL);
	for (p = 1; p <= ws.nsids; p++) {
		ws_sid_t *s = &ws.sids[p];
		edict_t *e;
		if (s->slot < 0)
			continue;
		e = g_edicts + 1 + s->slot;
		if (!e->inuse || !e->client || e->client->resp.team == NOTEAM || !IS_ALIVE(e))
			continue;
		if (e->client->curr_weap == s->held)
			continue;
		s->held = e->client->curr_weap;
		ws_line("hold p=%d w=%d f=%d", p, s->held, level.framenum);
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
	ws_events_frame();

	if (ms > 0 && ws.going && (level.realFramenum - ws.live_at) * 1000 >= ms * (HZ > 0 ? HZ : 10)) {
		ws.live_at = level.realFramenum;
		ws_live();
	}

	for (i = 0; i < game.maxclients; i++) {
		ws_client_t *c = &ws.cl[i];
		edict_t *ent = g_edicts + 1 + i;

		// the dummy only after WS_MvdStreamStarted: its history is idle otherwise
		if (!c->sub || !ent->inuse)
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
