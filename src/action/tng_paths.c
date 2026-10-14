/*
The jump mod's player pathing: the routes the players took at the starts
of the rounds of a demo, to study and to run beside.

"jmod paths" picks an .mvd2 out of demos/ (those of this map, newest
first), then a player, then one of their routes - the rounds they took
the same way from the same spawn folded into one row - or the overview:
every route drawn on the map by the client (jpath, src/client/jpath.c),
green at LIGHTS shading to red at ACTION!, with a numbered marker at its
end to click on. One click draws that route with the round's other
players around it for context, a double click sends the player's ghost
down it, the stored-jump way (Jmp_PlayTake). The demo is parsed on both
sides with the shared reader (shared/mvdpath.c), so the client is told
the trails by index and comes to the same ones.
*/

#include "g_local.h"
#include "shared/mvdpath.h"
#include "common/fuzzy.h"
#include <dirent.h>

#define JMP_PATH_MAX		48		// routes listed of a player
#define JMP_PATH_DEMOS		2048	// demos of a map looked at, the newest listed
#define JMP_PATH_FOUND		64		// demos a player is found in, the closest names first
#define JMP_PATH_HEAD		(72 << 10)	// read of a demo for its players: the first block is in there
#define JMP_PATH_ROWS		12		// a page of demos or players
#define JMP_PATH_ROUTE_ROWS	10		// of routes, under the overview row
#define JMP_PATH_FIRST		3
#define JMP_PATH_SECS		6		// shown past ACTION!, by default
#define JMP_PATH_CLICK_MS	300		// a second click within this is a double one
#define JMP_PATH_ICON_PX		18		// the shooter's weapon icon by his tag
#define JMP_PATH_ICON_LEFT		30		// units to the watcher's left of the tag
#define JMP_PATH_ICON_FRAMES	4		// frames it stays up from a shot: 0.4 s
#define JMP_PATH_GHOST_HOLD_MS	1000	// the ghosts stand at their ends this long, then set off again (JMP_PLAY_TAIL_MS: in step with a followed ghost)
#define JMP_PATH_GHOSTS		96		// ghosts at once: the routes' own, then the rest of their rounds
#define JMP_PATH_SKULL		"models/objects/gibs/skull/tris.md2"	// where a ghost's player died

typedef struct {
	char	name[MAX_OSPATH];
	time_t	mtime;
} jmp_path_demo_t;

// a demo a player was found in, and the name of his that matched
typedef struct {
	int		demo;					// index into jmp_path_demos
	int		score;
	char	name[MVP_NAME_MAX];
} jmp_path_found_t;

typedef struct {
	char	map[MAX_QPATH];			// the demo is of this map; another map drops it
	mvp_demo_t	*demo;
	char	demo_name[MAX_OSPATH];	// its file in demos/: a server demo's name is longer than MAX_QPATH
	int		player;					// index into demo->players, -1 none
	int		frames;					// samples shown: to ACTION! and jmod_path_secs past it
	int		run_frames;				// samples the ghosts and the lines run to: frames, or what alt+fire's question answered
	int		loop_frames;			// the loop's length: the longest trail among the ghosts, within run_frames
	int		ask_route;				// the route alt+fire asked the question for
	int		routes[JMP_PATH_MAX];	// the player's unique trails, and the rounds each folds
	int		dups[JMP_PATH_MAX];
	int		route_count;
	int		spawn[JMP_PATH_MAX];	// the spawnpoint each starts on, from 1; 0 none near
	int		vis[JMP_PATH_MAX];		// the routes shown, as indices into routes: all, or those from spawn_filter
	int		vis_count;
	int		spawn_filter;			// a spawnpoint's number, 0 all
	qboolean	spawn_mode;			// a player is picked: shift+fire on a spawn marker shows his routes from it
	edict_t	*ghosts[JMP_PATH_GHOSTS];	// a ghost running each route shown, and the rest of its round
	int		names[JMP_PATH_GHOSTS];		// each ghost's name over its head, a ghud label
	int		icons[JMP_PATH_GHOSTS];		// and his weapon's icon by it while he shoots
	qboolean ghost_dead[JMP_PATH_GHOSTS];	// the skull up, the ghost gone
	qboolean names_on;
	int		ghost_trail[JMP_PATH_GHOSTS];	// index into demo->trails
	edict_t	*skulls[JMP_PATH_GHOSTS];	// a skull where the ghost's player died, when he did inside the window
	int		ghost_count;
	float	ghost_t0;				// level.time they set off
	float	ghost_ms;				// where the run was last frame: back to 0 is a new run
	int		ghost_lca;				// LIGHTS, CAMERA, ACTION! said this run: 0 to 3
	int		solo;					// a route picked alone, as an index into routes; -1 none
	int		train;					// the route drilled: its ghost dim, off again at each spawn; -1 none
	int		train_others;			// the drill's company: 0 the route alone, 1 the round's other routes, 2 their ghosts too
	qboolean train_preset;			// the company was chosen with the click (shift+alt): the spawn menu does not ask
	vec3_t	back_origin;			// where he stood when he clicked a route's marker: Escape puts him back
	float	back_pitch, back_yaw;
	qboolean back_set;
	qboolean round_view;			// alt+fire on a route's marker: the whole round drawn and running, nobody followed

	int		top;					// the page of the step up
	int		pick;					// the route the route screen is of
	char	find[MVP_NAME_MAX];		// the player looked for over the map's demos
	int		found_count;			// what the search found
	jmp_path_found_t	found[JMP_PATH_FOUND];

	qboolean	over;				// the overview is up: lines on the client, markers here
	int		sel;					// the route drawn alone, -1 none
	int		aim;					// the marker aimed at, from 1; 0 none
	int		aim_sent;				// the trail the client was told is aimed at, -1 none
	qboolean	fire_held;
	int		click_route, click_ms;	// a click waiting to see if it is a double one
	int		ghud[JMP_PATH_MAX * 2];	// a ring and a number per route
	int		ghud_count;
	int		ghud_made;				// level.framenum they were made at
	int		ghud_key;
	int		ghud_size[JMP_PATH_MAX];
	int		tip[6];
	qboolean	tip_on;
} jmp_path_t;

static jmp_path_t jmp_paths[MAX_CLIENTS];


static jmp_path_demo_t jmp_path_demos[JMP_PATH_DEMOS];
static int jmp_path_demo_count;

static cvar_t *jmod_path_secs;
static cvar_t *jmod_path_lca;		// 0: the ghosts set off without "LIGHTS... CAMERA... ACTION!"
static cvar_t *jmod_paths_open;		// a demo's name the client set before the map (JmpPathAutoOpen)

// at the game's start, so the console knows them (its completion and
// its fuzzy search list the cvars registered) before anything uses them
void JmpPath_InitCvars(void)
{
	jmod_path_secs = gi.cvar("jmod_path_secs", va("%d", JMP_PATH_SECS), 0);
	jmod_path_lca = gi.cvar("jmod_path_lca", "1", 0);
	jmod_paths_open = gi.cvar("jmod_paths_open", "", 0);
}

static void JmpPathShow(edict_t *ent, int step, int cur);

static jmp_path_t *JmpPath(edict_t *ent)
{
	jmp_path_t *st = &jmp_paths[ent - g_edicts - 1];

	// a new map: the demo was of the old one
	if (strcmp(st->map, level.mapname)) {
		MvdPath_Free(st->demo);
		memset(st, 0, sizeof(*st));
		Q_strncpyz(st->map, level.mapname, sizeof(st->map));
		st->player = -1;
		st->sel = -1;
		st->solo = -1;
		st->train = -1;
	}
	return st;
}

// the whole round is parsed; jmod_path_secs is how much of it past
// ACTION! is shown and run, up to the whole of it (MVP_AFTER_FRAMES)
static const int jmp_path_secs_steps[] = { 3, 6, 10, 15, 20, 30, 45, 60, 90, 120, 180, 300 };
// alt+fire's question on a round: coarser, the round's end last
static const int jmp_path_round_steps[] = { 15, 30, 45, 60, MVP_AFTER_FRAMES / 10 };

static int JmpPathFrames(void)
{
	int secs;

	if (!jmod_path_secs)
		jmod_path_secs = gi.cvar("jmod_path_secs", va("%d", JMP_PATH_SECS), 0);
	secs = Q_clip((int)jmod_path_secs->value, 0, MVP_AFTER_FRAMES / 10);
	return MVP_ACTION_FRAMES + secs * 10;
}

// a demo's file name is what the server gave it: nothing a path or a
// console line could be made of gets through
static qboolean JmpPathSafeName(const char *name)
{
	size_t len = strlen(name);

	if (!len || len >= MAX_OSPATH - 32 || strstr(name, ".."))
		return false;
	for (; *name; name++)
		if (*name <= ' ' || *name == '/' || *name == '\\' || *name == '"' || *name == ';' || *name == '$')
			return false;
	return true;
}

static qboolean JmpPathHasMap(const char *name)
{
	char lower[MAX_OSPATH], map[MAX_QPATH];
	int i;

	for (i = 0; name[i] && i < MAX_OSPATH - 1; i++)
		lower[i] = Q_tolower(name[i]);
	lower[i] = 0;
	for (i = 0; level.mapname[i] && i < MAX_QPATH - 1; i++)
		map[i] = Q_tolower(level.mapname[i]);
	map[i] = 0;
	return strstr(lower, map) != NULL;
}

static int JmpPathDemoCmp(const void *a, const void *b)
{
	const jmp_path_demo_t *da = a, *db = b;

	if (da->mtime != db->mtime)
		return da->mtime < db->mtime ? 1 : -1;
	return strcmp(da->name, db->name);
}

// demos/ of the game: the .mvd2s with this map's name in theirs, newest first
static void JmpPathScan(void)
{
	char path[MAX_OSPATH];
	struct dirent *e;
	Q_STATBUF sb;
	DIR *dir;
	size_t len;

	jmp_path_demo_count = 0;
	Q_snprintf(path, sizeof(path), "%s/demos", Jmp_GameDir());
	dir = opendir(path);
	if (!dir)
		return;

	while (jmp_path_demo_count < JMP_PATH_DEMOS && (e = readdir(dir)) != NULL) {
		jmp_path_demo_t *d = &jmp_path_demos[jmp_path_demo_count];

		len = strlen(e->d_name);
		if (len < 6 || strcmp(e->d_name + len - 5, ".mvd2"))
			continue;
		if (!JmpPathSafeName(e->d_name) || !JmpPathHasMap(e->d_name))
			continue;
		Q_snprintf(path, sizeof(path), "%s/demos/%s", Jmp_GameDir(), e->d_name);
		if (os_stat(path, &sb))
			continue;
		Q_strncpyz(d->name, e->d_name, sizeof(d->name));
		d->mtime = sb.st_mtime;
		jmp_path_demo_count++;
	}
	closedir(dir);
	qsort(jmp_path_demos, jmp_path_demo_count, sizeof(jmp_path_demos[0]), JmpPathDemoCmp);
}

/*
A demo's row: "hl_pickup_2024-12-23_204346_Team1_Team2_urban2_27930.mvd2"
is "2024-12-23 20:43 Team1_Team2" - the date, the time, what came after
them up to the map - else the name without its extension, cut to fit.
*/
static void JmpPathDemoLabel(const char *name, char *buf, size_t size)
{
	char plain[MAX_OSPATH];
	const char *p, *date = NULL, *rest, *map;
	size_t len = strlen(name), n;

	n = len > 5 ? len - 5 : len;
	if (n >= sizeof(plain))
		n = sizeof(plain) - 1;
	memcpy(plain, name, n);
	plain[n] = 0;

	// a date: dddd-dd-dd, with _dddddd after it
	for (p = plain; p[0]; p++) {
		if (strlen(p) >= 18 && Q_isdigit(p[0]) && Q_isdigit(p[1]) && Q_isdigit(p[2]) && Q_isdigit(p[3])
			&& p[4] == '-' && Q_isdigit(p[5]) && Q_isdigit(p[6]) && p[7] == '-' && Q_isdigit(p[8])
			&& Q_isdigit(p[9]) && p[10] == '_' && Q_isdigit(p[11]) && Q_isdigit(p[12]) && Q_isdigit(p[13])
			&& Q_isdigit(p[14])) {
			date = p;
			break;
		}
	}
	if (!date) {
		Q_strncpyz(buf, plain, size);
		return;
	}

	rest = date + 17;
	if (*rest == '_')
		rest++;
	// up to the map's name, when the name has it after the teams
	map = Q_stristr(rest, level.mapname);
	n = map && map > rest ? (size_t)(map - rest - 1) : strlen(rest);
	Q_snprintf(buf, size, "%.10s %.2s:%.2s %.*s", date, date + 11, date + 13, (int)n, rest);
}

#define JMP_PATH_LOC	128		// a location's description, as placedata_t has it

// the place's name; "" on a map without a location file - coordinates
// told nobody anything
static const char *JmpPathLoc(const vec3_t origin, char *buf)
{
	if (!GetLocationAt(origin, buf))
		buf[0] = 0;
	return buf;
}

// the spawnpoint a route starts on, from 1; 0 none near
static int JmpPathSpawn(const vec3_t origin)
{
	edict_t *spots[JMP_SPOTS_MAX];
	vec3_t d;
	int i, count = Jmp_Spots(spots, JMP_SPOTS_MAX);

	for (i = 0; i < count; i++) {
		VectorSubtract(spots[i]->s.origin, origin, d);
		if (VectorLength(d) < JMP_MARKER_NEAR)
			return i + 1;
	}
	return 0;
}

// where it goes: the location at the end, and at ACTION! when that is
// another - the names are phrases of their own ("outside red building")
static void JmpPathDesc(jmp_path_t *st, int k, char *buf, size_t size)
{
	const mvp_trail_t *t = &st->demo->trails[st->routes[k]];
	char from[JMP_PATH_LOC], via[JMP_PATH_LOC], to[JMP_PATH_LOC];
	vec3_t o;

	MvdPath_Origin(&st->demo->samples[t->first], o);
	JmpPathLoc(o, from);
	MvdPath_Origin(MvdPath_At(st->demo, t, MVP_ACTION_FRAMES), o);
	JmpPathLoc(o, via);
	MvdPath_Origin(MvdPath_At(st->demo, t, st->frames), o);
	JmpPathLoc(o, to);

	if (via[0] && strcmp(via, to) && strcmp(via, from))
		Q_snprintf(buf, size, "%s, by %s", to, via);
	else
		Q_strncpyz(buf, to, size);
}

static const char *JmpPathPlayerName(jmp_path_t *st)
{
	if (!st->demo || st->player < 0 || st->player >= st->demo->player_count)
		return "";
	return st->demo->players[st->player].name;
}

static int JmpPathSpawn(const vec3_t origin);
static void JmpPathMarkersClear(edict_t *ent, jmp_path_t *st);
static void JmpPathMarkers(edict_t *ent, jmp_path_t *st);
static void JmpPathGhostsSpawn(edict_t *ent, jmp_path_t *st, qboolean own);
static int JmpPathSkin(jmp_path_t *st, const mvp_trail_t *t, int which);

// the routes shown: all of them, or those from the spawnpoint picked
static void JmpPathFilter(jmp_path_t *st)
{
	int k;

	st->vis_count = 0;
	if (st->solo >= 0 && st->solo < st->route_count) {
		st->vis[st->vis_count++] = st->solo;
		return;
	}
	for (k = 0; k < st->route_count; k++)
		if (!st->spawn_filter || st->spawn[k] == st->spawn_filter)
			st->vis[st->vis_count++] = k;
}

// a route's number on the list and the markers, from 1; 0 if not shown
static int JmpPathVisNumber(jmp_path_t *st, int k)
{
	int i;

	for (i = 0; i < st->vis_count; i++)
		if (st->vis[i] == k)
			return i + 1;
	return 0;
}

static void JmpPathRoutes(jmp_path_t *st)
{
	vec3_t o;
	int k;

	st->route_count = 0;
	st->vis_count = 0;
	st->spawn_filter = 0;
	st->solo = -1;
	st->train = -1;
	st->sel = -1;
	if (!st->demo || st->player < 0)
		return;
	st->frames = JmpPathFrames();
	st->run_frames = st->frames;
	st->route_count = MvdPath_Unique(st->demo, JmpPathPlayerName(st), st->frames,
		st->routes, st->dups, JMP_PATH_MAX);
	for (k = 0; k < st->route_count; k++) {
		MvdPath_Origin(&st->demo->samples[st->demo->trails[st->routes[k]].first], o);
		st->spawn[k] = JmpPathSpawn(o);
	}
	JmpPathFilter(st);
}

static qboolean JmpPathLoad(edict_t *ent, jmp_path_t *st, const char *name)
{
	char path[MAX_OSPATH], err[128];
	FILE *f;
	byte *data;
	long len;
	mvp_demo_t *demo;

	if (!JmpPathSafeName(name)) {
		gi.cprintf(ent, PRINT_HIGH, "That is no demo name\n");
		return false;
	}
	Q_snprintf(path, sizeof(path), "%s/demos/%s", Jmp_GameDir(), name);
	f = fopen(path, "rb");
	if (!f) {
		gi.cprintf(ent, PRINT_HIGH, "No %s in %s/demos\n", name, Jmp_GameDir());
		return false;
	}
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	data = len > 0 ? malloc(len) : NULL;
	if (!data || fread(data, 1, len, f) != (size_t)len) {
		fclose(f);
		free(data);
		gi.cprintf(ent, PRINT_HIGH, "Could not read %s\n", name);
		return false;
	}
	fclose(f);

	demo = MvdPath_Parse(data, len, err, sizeof(err));
	free(data);
	if (!demo) {
		gi.cprintf(ent, PRINT_HIGH, "%s: %s\n", name, err);
		return false;
	}
	if (Q_stricmp(demo->map, level.mapname)) {
		gi.cprintf(ent, PRINT_HIGH, "%s is of %s, not %s\n", name, demo->map, level.mapname);
		MvdPath_Free(demo);
		return false;
	}

	MvdPath_Free(st->demo);
	st->demo = demo;
	Q_strncpyz(st->demo_name, name, sizeof(st->demo_name));
	st->player = -1;
	st->route_count = 0;
	st->sel = -1;
	gi.cprintf(ent, PRINT_HIGH, "%s: %d rounds, %d players\n", name, demo->round_count, demo->player_count);
	return true;
}

/*
==============================================================================
THE CLIENT'S LINES
==============================================================================
*/

static void JmpPathStuff(edict_t *ent, const char *cmd)
{
	char buf[MAX_STRING_CHARS];

	Q_snprintf(buf, sizeof(buf), "%s\n", cmd);
	stuffcmd(ent, buf);
}

// the overview: every listed route
static void JmpPathStuffShow(edict_t *ent, jmp_path_t *st)
{
	char cmd[MAX_STRING_CHARS], num[16];
	int k;

	Q_snprintf(cmd, sizeof(cmd), "jpath show \"%s\" %d ", st->demo_name, st->frames);
	for (k = 0; k < st->vis_count; k++) {
		Q_snprintf(num, sizeof(num), "%s%d", k ? "," : "", st->routes[st->vis[k]]);
		Q_strncatz(cmd, num, sizeof(cmd));
	}
	JmpPathStuff(ent, cmd);
}

// one route, the round's other players round it
// the route run on the client, with the round's others drawn dim or not
static void JmpPathStuffPlay(edict_t *ent, jmp_path_t *st, int k, qboolean others)
{
	const mvp_trail_t *t = &st->demo->trails[st->routes[k]];
	const mvp_round_t *r = &st->demo->rounds[t->round];
	char cmd[MAX_STRING_CHARS], num[16];
	int i, n = 0;

	// run as long as the longest trail drawn, within the span: a loop
	// that waited the whole span out stood still for minutes
	int frames = min(t->count - 1, st->run_frames);

	for (i = r->trail_first; others && i < r->trail_first + r->trail_count; i++)
		frames = max(frames, min(st->demo->trails[i].count - 1, st->run_frames));
	Q_snprintf(cmd, sizeof(cmd), "jpath play \"%s\" %d %d ", st->demo_name, max(frames, 1), st->routes[k]);
	for (i = r->trail_first; others && i < r->trail_first + r->trail_count; i++) {
		if (i == st->routes[k])
			continue;
		Q_snprintf(num, sizeof(num), "%s%d", n++ ? "," : "", i);
		Q_strncatz(cmd, num, sizeof(cmd));
	}
	JmpPathStuff(ent, cmd);
}

/*
==============================================================================
THE GHOST
==============================================================================
*/

// the route as a take: the demo's ten samples a second, the keys read
// off the movement - the demo does not have them
static void JmpPathTake(jmp_path_t *st, int k, jmp_take_t *take)
{
	const mvp_trail_t *t = &st->demo->trails[st->routes[k]];
	const mvp_round_t *r = &st->demo->rounds[t->round];
	int i, n = min(t->count, st->run_frames + 1);

	memset(take, 0, sizeof(*take));
	Q_snprintf(take->name, sizeof(take->name), "round %d", r->number);
	Q_strncpyz(take->author, t->name, sizeof(take->author));
	JmpPathDesc(st, k, take->desc, sizeof(take->desc));
	take->fps = 10;
	take->mvd = true;
	take->count = n;
	take->ms = (n - 1) * 100;
	take->skinnum = JmpPathSkin(st, t, 0);
	take->skinset = take->skinnum >= 0;
	take->samples = gi.TagMalloc(n * sizeof(jmp_sample_t), TAG_GAME);

	for (i = 0; i < n; i++) {
		const mvp_sample_t *m = &st->demo->samples[t->first + i];
		jmp_sample_t *s = &take->samples[i];
		vec3_t next, v, forward, right;
		float f, r2, yaw;

		s->ms = i * 100;
		MvdPath_Origin(m, s->origin);
		// the LIGHTS frame has the view's angles zeroed by the teleport
		if (i == 0)
			m = &st->demo->samples[t->first + min(2, n - 1)];
		s->pitch = SHORT2ANGLE(m->pitch);
		s->yaw = SHORT2ANGLE(m->yaw);
		s->frame = m->anim;
		s->viewheight = m->viewheight;
		s->weapon = m->weapon;
		s->keys = 0;
		s->speed = 0;
		if (i + 1 >= n)
			continue;

		MvdPath_Origin(&st->demo->samples[t->first + i + 1], next);
		VectorSubtract(next, s->origin, v);
		VectorScale(v, 10, v);
		s->speed = sqrtf(v[0] * v[0] + v[1] * v[1]);
		yaw = DEG2RAD(s->yaw);
		VectorSet(forward, cosf(yaw), sinf(yaw), 0);
		VectorSet(right, sinf(yaw), -cosf(yaw), 0);
		f = DotProduct(v, forward);
		r2 = DotProduct(v, right);
		if (f > 40)
			s[1].keys |= JMP_KEY_FWD;
		else if (f < -40)
			s[1].keys |= JMP_KEY_BACK;
		if (r2 > 40)
			s[1].keys |= JMP_KEY_RIGHT;
		else if (r2 < -40)
			s[1].keys |= JMP_KEY_LEFT;
		if (v[2] > 150)
			s[1].keys |= JMP_KEY_JUMP;
		if (st->demo->samples[t->first + i + 1].viewheight < 16)
			s[1].keys |= JMP_KEY_DUCK;
	}
}

/*
A route alone: its line with the others of its round round it, the
round's others running, and the player's ghost followed - from his
own eyes, fire for third person and back (Jmp_PlayThink) - in step with
them. Escape ends the following (Jmp_PlayEscape) and leaves the route
up; Escape again brings the set it was picked from back.
*/
static void JmpPathSolo(edict_t *ent, jmp_path_t *st, int k, qboolean pov)
{
	jmp_take_t take;

	if (k < 0 || k >= st->route_count)
		return;
	if (ent->client->layout == LAYOUT_MENU)
		PMenu_Close(ent);
	st->solo = k;
	st->train = -1;
	st->round_view = false;
	st->run_frames = st->frames;
	JmpPathFilter(st);
	st->over = true;
	st->sel = k;
	st->click_ms = 0;
	gi.cprintf(ent, PRINT_HIGH, "Route %d of %s: fire for third person and back, Escape brings the others back\n",
		st->demo->rounds[st->demo->trails[st->routes[k]].round].number, JmpPathPlayerName(st));
	JmpPathStuffPlay(ent, st, k, true);
	JmpPathMarkersClear(ent, st);
	JmpPathMarkers(ent, st);
	JmpPathGhostsSpawn(ent, st, false);
	JmpPathTake(st, k, &take);
	// where he is now, to come back to when he lets go
	VectorCopy(ent->s.origin, st->back_origin);
	st->back_pitch = ent->client->v_angle[PITCH];
	st->back_yaw = ent->client->v_angle[YAW];
	st->back_set = true;
	Jmp_PlayTake(ent, &take, pov);
}

static void JmpPathGhost(edict_t *ent, jmp_path_t *st, int k, qboolean pov)
{
	JmpPathSolo(ent, st, k, pov);
}

// alt+fire on a route's marker: the round as it went - the route and
// the others' drawn, every ghost running - watched from wherever he
// stands, for as long as the question before it answered. Escape
// brings the set back, as from a follow.
static void JmpPathRound(edict_t *ent, jmp_path_t *st, int k, int secs)
{
	const mvp_trail_t *t;

	if (k < 0 || k >= st->route_count)
		return;
	if (ent->client->layout == LAYOUT_MENU)
		PMenu_Close(ent);
	st->run_frames = MVP_ACTION_FRAMES + Q_clip(secs, 0, MVP_AFTER_FRAMES / 10) * 10;
	Jmp_PlayEscape(ent);
	t = &st->demo->trails[st->routes[k]];
	st->solo = k;
	st->train = -1;
	st->round_view = true;
	JmpPathFilter(st);
	st->over = true;
	st->sel = k;
	st->click_ms = 0;
	gi.cprintf(ent, PRINT_HIGH, "Round %d of %s, the whole round running. Escape brings the others back\n",
		st->demo->rounds[t->round].number, JmpPathPlayerName(st));
	JmpPathStuffPlay(ent, st, k, true);
	JmpPathMarkersClear(ent, st);
	JmpPathMarkers(ent, st);
	JmpPathGhostsSpawn(ent, st, true);
}

/*
A route to drill (shift+fire on its number, or jmod path <#> train):
the route drawn, its ghost dim beside you and off again every time you
spawn, and the spawn menu asking how long until you spawn at its start
and how often again; none of the round's others until asked. Escape ends the drill.
*/
static void JmpPathTrain(edict_t *ent, jmp_path_t *st, int k)
{
	const mvp_trail_t *t;
	const mvp_sample_t *m;
	vec3_t origin;
	int n;

	if (k < 0 || k >= st->route_count)
		return;
	if (ent->client->layout == LAYOUT_MENU)
		PMenu_Close(ent);
	Jmp_PlayEscape(ent);		// a ghost being followed lets go
	st->solo = k;
	st->train = k;
	st->round_view = false;
	st->run_frames = st->frames;
	st->train_others = 0;
	st->train_preset = false;
	JmpPathFilter(st);
	st->over = true;
	st->sel = k;
	st->click_ms = 0;
	// the route and its ghost alone: no markers, none of the round's
	// others until the spawn menu's last step asks for them
	JmpPathStuffPlay(ent, st, k, false);
	JmpPathMarkersClear(ent, st);
	JmpPathGhostsSpawn(ent, st, true);

	t = &st->demo->trails[st->routes[k]];
	n = min(t->count, st->run_frames + 1);
	MvdPath_Origin(&st->demo->samples[t->first], origin);
	m = &st->demo->samples[t->first + min(2, n - 1)];	// the LIGHTS frame has the angles zeroed
	gi.cprintf(ent, PRINT_HIGH, "Drilling route %d of %s: the ghost sets off whenever you spawn. Escape ends it\n",
		st->demo->rounds[t->round].number, JmpPathPlayerName(st));
	Jmp_AskDrill(ent, origin, SHORT2ANGLE(m->pitch), SHORT2ANGLE(m->yaw));
}

// the spawn menu's last step for a drill: the round's others drawn or
// not. 0 when it is not a drill's spawn, else the mode so far plus one.
int JmpPath_AskOthers(edict_t *ent)
{
	jmp_path_t *st = JmpPath(ent);

	if (st->train < 0 || !st->over || st->train_preset)
		return 0;
	return st->train_others + 1;
}

// the answer: the round's other routes drawn dim (1), their ghosts too (2)
void JmpPath_DrillOthers(edict_t *ent, int mode)
{
	jmp_path_t *st = JmpPath(ent);

	if (st->train < 0 || !st->over)
		return;
	st->train_others = Q_clip(mode, 0, 2);
	JmpPathStuffPlay(ent, st, st->train, st->train_others >= 1);
	JmpPathGhostsSpawn(ent, st, true);
}

// the status lines at the bottom while the pathing has something up:
// true with the rows filled. spawn is the pending spawn's line, or "".
qboolean JmpPath_Status(edict_t *ent, char rows[][64], const char *spawn)
{
	jmp_path_t *st = JmpPath(ent);
	const char *name = JmpPathPlayerName(st);
	size_t size = sizeof(rows[0]);

	if (!st->demo)
		return false;
	if (st->train >= 0 && st->over) {
		const mvp_trail_t *t = &st->demo->trails[st->routes[st->train]];

		Q_snprintf(rows[0], size, "Drilling round %d of %.16s%s", st->demo->rounds[t->round].number, name,
			st->train_others == 2 ? ", the round with it" : st->train_others == 1 ? ", the round's routes dim" : "");
		if (spawn[0])
			Q_strncpyz(rows[1], spawn, size);
		Q_strncpyz(rows[2], "Esc: end the drill", size);
		return true;
	}
	if (st->round_view && st->solo >= 0 && st->over) {
		const mvp_trail_t *t = &st->demo->trails[st->routes[st->solo]];

		Q_snprintf(rows[0], size, "Round %d of %.16s, the whole round running", st->demo->rounds[t->round].number, name);
		Q_strncpyz(rows[1], "Esc: back to the routes", size);
		return true;
	}
	if (st->over && st->vis_count) {
		if (st->spawn_filter)
			Q_snprintf(rows[0], size, "%.16s's %d route%s from spawn %d, ghosts running", name,
				st->vis_count, st->vis_count == 1 ? "" : "s", st->spawn_filter);
		else
			Q_snprintf(rows[0], size, "All %d of %.16s's routes on the map", st->vis_count, name);
		Q_strncpyz(rows[1], "Click at a number: follow the route", size);
		Q_strncpyz(rows[2], "Shift + click on a marker: drill it", size);
		Q_strncpyz(rows[3], "Esc: remove markers and stop", size);
		return true;
	}
	if (st->spawn_mode && ent->client->resp.jmp_markers && ent->client->layout != LAYOUT_MENU) {
		Q_snprintf(rows[0], size, "%.16s's routes: pick a spawn", name);
		Q_strncpyz(rows[1], "Shift + click on a marker: show routes", size);
		Q_strncpyz(rows[2], "Esc: remove markers", size);
		return true;
	}
	return false;
}

// a jmod spawn landed: a drill's ghost sets off with it
void JmpPath_Spawned(edict_t *ent)
{
	jmp_path_t *st = JmpPath(ent);

	if (st->train < 0 || !st->ghost_count)
		return;
	st->ghost_t0 = level.time;
	st->ghost_ms = -1;
	st->ghost_lca = 0;
	JmpPathStuff(ent, "jpath restart");
}

/*
==============================================================================
THE GHOSTS OF A SPAWN

Shift+fire on a spawn marker: a ghost per route from it, all setting off
together from LIGHTS and running their routes in a loop, the watcher
free to spawn there and run beside them. Unlike a stored jump's playback
(Jmp_PlayTake) they take no camera and no control.
==============================================================================
*/

/*
A ghost wears the demo player's name and team skin of that round: a
player model takes its skin from a player skin configstring, so each
ghost gets a slot of its own above the real clients. Slot 0 is for the
followed ghost (Jmp_PlayTake), the rest for the spawn's.
*/
#define JMP_PATH_SKIN_SLOTS	(JMP_PATH_GHOSTS + 1)

// the slot set for the trail, -1 when there is none to spare
static int JmpPathSkin(jmp_path_t *st, const mvp_trail_t *t, int which)
{
	const mvp_round_t *r = &st->demo->rounds[t->round];
	int slot = game.maxclients + which;

	if (which < 0 || which >= JMP_PATH_SKIN_SLOTS || slot >= MAX_CLIENTS || !r->skin[t->team][0])
		return -1;
	gi.configstring(game.csr.playerskins + slot, va("%s\\%s", t->name, r->skin[t->team]));
	return slot;
}

static void JmpPathGhostsClear(jmp_path_t *st)
{
	int i;

	// the names: the state's client is its slot's
	for (i = 0; st->names_on && i < st->ghost_count; i++) {
		if (st->names[i] >= 0)
			Ghud_RemoveElement(g_edicts + 1 + (st - jmp_paths), st->names[i]);
		if (st->icons[i] >= 0)
			Ghud_RemoveElement(g_edicts + 1 + (st - jmp_paths), st->icons[i]);
	}
	st->names_on = false;
	for (i = 0; i < st->ghost_count; i++) {
		if (st->ghosts[i] && st->ghosts[i]->inuse && !strcmp(st->ghosts[i]->classname, "jmp_path_ghost"))
			G_FreeEdict(st->ghosts[i]);
		if (st->skulls[i] && st->skulls[i]->inuse && !strcmp(st->skulls[i]->classname, "jmp_path_skull"))
			G_FreeEdict(st->skulls[i]);
		st->skulls[i] = NULL;
	}
	st->ghost_count = 0;
}

// a ghost down trail k, at its start; a skull where he died, hidden
// until the ghost gets there. Dim: one to run beside, not to watch
static void JmpPathGhostAdd(edict_t *ent, jmp_path_t *st, int k, qboolean dim, qboolean named)
{
	const mvp_trail_t *t = &st->demo->trails[k];
	edict_t *ghost, *skull;
	int i = st->ghost_count, slot;

	if (i >= JMP_PATH_GHOSTS)
		return;
	ghost = G_Spawn();
	ghost->classname = "jmp_path_ghost";
	ghost->owner = ent;
	ghost->movetype = MOVETYPE_NONE;
	ghost->solid = SOLID_NOT;
	ghost->s.modelindex = 255;
	slot = JmpPathSkin(st, t, 1 + i);
	ghost->s.skinnum = slot >= 0 ? slot : ent - g_edicts - 1;
	// through the walls, as the view weapon is: a route runs out of sight
	ghost->s.renderfx = RF_TRANSLUCENT | RF_DEPTHHACK;
	// and sent whatever the viewer's PVS: a noclip out of the world
	// empties it, and the ghosts went with it
	ghost->svflags |= SVF_NOCULL;
	if (dim)
		ghost->x.alpha = 0.3f;
	MvdPath_Origin(&st->demo->samples[t->first], ghost->s.origin);
	VectorCopy(ghost->s.origin, ghost->s.old_origin);
	gi.linkentity(ghost);
	st->ghosts[i] = ghost;
	st->ghost_trail[i] = k;
	st->skulls[i] = NULL;

	// his name over his head, small and plain, as the stream names the
	// players over their arrows (GHUD_TEXT_SIZE/PLAIN, the client's) -
	// where there are others to tell him from
	st->names[i] = st->icons[i] = -1;
	st->ghost_dead[i] = false;
	if (!named)
		goto skull;
	st->names[i] = Ghud_NewElement(ent, GHT_TEXT);
	Ghud_SetFlags(ent, st->names[i], GHF_3DPOS);
	Ghud_SetPosition3D(ent, st->names[i], ghost->s.origin[0], ghost->s.origin[1], ghost->s.origin[2] + 40);
	Ghud_SetTextFlags(ent, st->names[i], UI_CENTER);
	Ghud_SetInt(ent, st->names[i], (-8 & 0xff) | 0x200 | 0x400);	// small, plain, never faded
	Ghud_SetColor(ent, st->names[i], 255, 255, 255, dim ? 120 : 220);
	Ghud_SetText(ent, st->names[i], (char *)t->name);
	// his weapon's icon by the name, up while he shoots
	st->icons[i] = Ghud_NewElement(ent, GHT_IMG);
	Ghud_SetFlags(ent, st->icons[i], GHF_3DPOS | GHF_HIDE);
	Ghud_SetPosition3D(ent, st->icons[i], ghost->s.origin[0], ghost->s.origin[1], ghost->s.origin[2] + 50);
	Ghud_SetSize(ent, st->icons[i], JMP_PATH_ICON_PX, JMP_PATH_ICON_PX);
	st->names_on = true;

skull:

	if (t->died && t->count <= st->run_frames + 1) {
		skull = G_Spawn();
		skull->classname = "jmp_path_skull";
		skull->owner = ent;
		skull->movetype = MOVETYPE_NONE;
		skull->solid = SOLID_NOT;
		skull->s.modelindex = gi.modelindex(JMP_PATH_SKULL);
		skull->s.renderfx = RF_DEPTHHACK;
		skull->svflags |= SVF_NOCLIENT | SVF_NOCULL;
		MvdPath_Origin(&st->demo->samples[t->first + t->count - 1], skull->s.origin);
		skull->s.origin[2] += 4;
		VectorCopy(skull->s.origin, skull->s.old_origin);
		gi.linkentity(skull);
		st->skulls[i] = skull;
	}
	st->ghost_count++;
}

// one per route shown - unless the one route shown is followed as the
// watcher's own ghost - and with a route alone, the rest of its round
static void JmpPathGhostsSpawn(edict_t *ent, jmp_path_t *st, qboolean own)
{
	int i, k;
	// names only where the round's others run too, to tell them apart
	qboolean named = st->solo >= 0 && (st->train < 0 || st->train_others >= 2);

	JmpPathGhostsClear(st);
	for (i = 0; own && i < st->vis_count; i++)
		JmpPathGhostAdd(ent, st, st->routes[st->vis[i]], st->train >= 0, named);
	// a route followed runs with the whole round, teammates and
	// opponents - the view Escape leaves it in; a drill the same when
	// its spawn menu asked for them
	for (i = 0; st->solo >= 0 && i < st->vis_count; i++) {
		const mvp_trail_t *t = &st->demo->trails[st->routes[st->vis[i]]];
		const mvp_round_t *r = &st->demo->rounds[t->round];

		if (st->train >= 0 && st->train_others < 2)
			continue;
		for (k = r->trail_first; k < r->trail_first + r->trail_count; k++)
			if (k != st->routes[st->vis[i]])
				JmpPathGhostAdd(ent, st, k, false, true);
	}
	// the loop as long as the longest trail among them, within the span
	st->loop_frames = 1;
	for (i = 0; i < st->ghost_count; i++)
		st->loop_frames = max(st->loop_frames, min(st->demo->trails[st->ghost_trail[i]].count - 1, st->run_frames));
	st->ghost_t0 = level.time;
	st->ghost_ms = -1;
	st->ghost_lca = 0;
}

// the round's start as the ghosts run it: LIGHTS as they set off, CAMERA
// two seconds in, ACTION! where the demo had it
static void JmpPathGhostsLca(edict_t *ent, jmp_path_t *st, float ms)
{
	static const struct {
		const char *text, *sound;
		float ms;
	} lca[3] = {
		{ "LIGHTS...", "atl/lights.wav", 0 },
		{ "CAMERA...", "atl/camera.wav", 2000 },
		{ "ACTION!", "atl/action.wav", MVP_ACTION_FRAMES * 100 },
	};

	if (!jmod_path_lca)
		jmod_path_lca = gi.cvar("jmod_path_lca", "1", 0);
	while (st->ghost_lca < 3 && ms >= lca[st->ghost_lca].ms) {
		if (jmod_path_lca->value) {
			gi.centerprintf(ent, "%s\n", lca[st->ghost_lca].text);
			unicastSound(ent, gi.soundindex((char *)lca[st->ghost_lca].sound), 1.0f);
		}
		st->ghost_lca++;
	}
}

// the shooter's weapon by his name tag: its HUD icon, up from the shot
// for JMP_PATH_ICON_FRAMES, at the tag's upper left as the watcher sees it
static void JmpPathShotIcon(edict_t *ent, jmp_path_t *st, int i, edict_t *ghost, const mvp_trail_t *t, int k, qboolean dead)
{
	static int icon_index[256];
	const mvp_sample_t *s = &st->demo->samples[t->first];
	vec3_t right, pos;
	int j, weapon = 0, lo = max(k - JMP_PATH_ICON_FRAMES + 1, 0);

	for (j = k; j >= lo; j--)
		if (s[j].flags & MVP_SHOT) {
			weapon = s[j].weapon;
			break;
		}
	if (dead || !weapon) {
		Ghud_SetFlags(ent, st->icons[i], GHF_3DPOS | GHF_HIDE);
		return;
	}
	if (!icon_index[weapon]) {
		gitem_t *item = FindItemByNum(weapon);

		icon_index[weapon] = item && item->icon ? gi.imageindex(item->icon) : -1;
	}
	if (icon_index[weapon] < 0) {
		Ghud_SetFlags(ent, st->icons[i], GHF_3DPOS | GHF_HIDE);
		return;
	}
	AngleVectors(ent->client->v_angle, NULL, right, NULL);
	VectorMA(ghost->s.origin, -JMP_PATH_ICON_LEFT, right, pos);
	Ghud_SetInt(ent, st->icons[i], icon_index[weapon]);
	Ghud_SetPosition3D(ent, st->icons[i], pos[0], pos[1], pos[2] + 50);
	Ghud_SetFlags(ent, st->icons[i], GHF_3DPOS);
}

// each frame: along their routes, ten samples a second, held at the
// end a moment and off again together
static void JmpPathGhostsFrame(edict_t *ent, jmp_path_t *st)
{
	float ms, period, frac, pitch, yaw;
	int i, k, n, whole;
	qboolean restarted;

	if (!st->ghost_count || !st->demo)
		return;
	period = max(st->loop_frames, 1) * 100 + JMP_PATH_GHOST_HOLD_MS;
	ms = fmodf((level.time - st->ghost_t0) * 1000, period);
	whole = (int)(ms / 100);
	frac = ms / 100 - whole;
	// off again: the clock went back round (a frame may land past the
	// first tenth, which a test on it missed, and the run went silent)
	if (ms < st->ghost_ms)
		st->ghost_lca = 0;
	restarted = ms < st->ghost_ms;
	st->ghost_ms = ms;
	// the client's line runs on its own clock: set it off with the ghosts
	if (restarted && st->over)
		JmpPathStuff(ent, "jpath restart");
	JmpPathGhostsLca(ent, st, ms);

	for (i = 0; i < st->ghost_count; i++) {
		const mvp_trail_t *t = &st->demo->trails[st->ghost_trail[i]];
		const mvp_sample_t *a, *b;
		edict_t *ghost = st->ghosts[i], *skull = st->skulls[i];
		vec3_t oa, ob;
		qboolean dead;

		if (!ghost || !ghost->inuse)
			continue;
		n = min(t->count, st->run_frames + 1);
		k = min(whole, n - 1);
		// at his death the ghost goes and the skull stays, until the next
		// run; at the restart every ghost is gone for the one frame, to
		// come back at its start as new - no glide back, and none of the
		// teleport event's burst and sound
		dead = skull && whole >= n - 1;
		if ((dead || restarted) != ((ghost->svflags & SVF_NOCLIENT) != 0))
			ghost->svflags ^= SVF_NOCLIENT;
		if (dead != st->ghost_dead[i]) {
			st->ghost_dead[i] = dead;
			if (skull && skull->inuse) {
				skull->svflags = dead ? (skull->svflags & ~SVF_NOCLIENT) : (skull->svflags | SVF_NOCLIENT);
				gi.linkentity(skull);
			}
			if (st->names_on && st->names[i] >= 0)
				Ghud_SetFlags(ent, st->names[i], dead ? GHF_3DPOS | GHF_HIDE : GHF_3DPOS);
		}
		a = &st->demo->samples[t->first + k];
		b = &st->demo->samples[t->first + min(k + 1, n - 1)];
		MvdPath_Origin(a, oa);
		MvdPath_Origin(b, ob);
		LerpVector(oa, ob, k < n - 1 ? frac : 0, ghost->s.origin);
		// the LIGHTS frame has the view's angles zeroed by the teleport
		if (k == 0)
			a = &st->demo->samples[t->first + min(2, n - 1)];
		pitch = SHORT2ANGLE(a->pitch);
		yaw = SHORT2ANGLE(a->yaw);
		VectorSet(ghost->s.angles, pitch / 3, yaw, 0);
		ghost->s.frame = a->anim;
		ghost->s.skinnum = (ghost->s.skinnum & 255) | (a->weapon << 8);
		ghost->s.modelindex2 = a->weapon ? 255 : 0;
		gi.linkentity(ghost);
		if (st->names_on && st->names[i] >= 0)
			Ghud_SetPosition3D(ent, st->names[i], ghost->s.origin[0], ghost->s.origin[1], ghost->s.origin[2] + 40);
		if (st->names_on && st->icons[i] >= 0)
			JmpPathShotIcon(ent, st, i, ghost, t, k, dead);
	}
}

/*
==============================================================================
THE OVERVIEW: MARKERS AND CLICKS
==============================================================================
*/

static void JmpPathMarkersClear(edict_t *ent, jmp_path_t *st)
{
	int i;

	for (i = 0; i < st->ghud_count; i++)
		Ghud_RemoveElement(ent, st->ghud[i]);
	st->ghud_count = 0;
}

// the route markers' tooltip: the spawn markers' four rows (Jmp_TipRows)
static void JmpPathTip(edict_t *ent, jmp_path_t *st, qboolean on)
{
	static const char *const rows[6] = {
		"Click: follow this route from the player's view",
		"Fire while following: third person and back",
		"Alt + click: show what the other players did that round",
		"Shift + click: drill it beside its ghost",
		"Shift + alt + click: drill it with the other players' ghosts",
		"Esc: back to the routes",
	};

	if (on == st->tip_on)
		return;
	st->tip_on = on;
	Jmp_TipRows(ent, st->tip, on);
	if (on)
		Jmp_TipText(ent, st->tip, rows);
}

static void JmpPathEnd(jmp_path_t *st, int k, vec3_t out)
{
	const mvp_trail_t *t = &st->demo->trails[st->routes[k]];

	MvdPath_Origin(MvdPath_At(st->demo, t, st->frames), out);
}

static void JmpPathMarkers(edict_t *ent, jmp_path_t *st)
{
	vec3_t origins[JMP_PATH_MAX];
	char text[32];
	int i, count = st->vis_count, hl, key, ring;

	// a new map has cleared every slot, ours went with them
	if (st->ghud_count && st->ghud_made > level.framenum)
		st->ghud_count = 0;

	if (!st->over || !count || st->train >= 0) {	// a drill has no markers
		st->aim = 0;
		st->aim_sent = -1;
		JmpPathTip(ent, st, false);
		JmpPathMarkersClear(ent, st);
		return;
	}

	for (i = 0; i < count; i++)
		JmpPathEnd(st, st->vis[i], origins[i]);
	// no aiming out of a ghost's eyes
	st->aim = (ent->svflags & SVF_NOCLIENT) ? 0 : Jmp_AimAt(ent, origins, count);
	JmpPathTip(ent, st, st->aim != 0);
	// the client lights the route aimed at
	i = st->aim ? st->routes[st->vis[st->aim - 1]] : -1;
	if (i != st->aim_sent) {
		char cmd[64];

		st->aim_sent = i;
		Q_snprintf(cmd, sizeof(cmd), "jpath hl %d", i);
		JmpPathStuff(ent, cmd);
	}
	hl = st->aim ? st->aim : st->sel >= 0 ? JmpPathVisNumber(st, st->sel) : 0;

	// sized by distance, as the spawn markers are: the client scales a
	// 3D image by 300 / distance
	for (i = 0; i < count && 2 * i < st->ghud_count; i++) {
		vec3_t d;
		int want, *had = &st->ghud_size[i];

		VectorSubtract(origins[i], ent->s.origin, d);
		want = (i + 1 == hl ? JMP_MARKER_HL : JMP_MARKER_PX) * max(VectorLength(d), 150) / 300;
		want = min(want, 30000);
		if (abs(want - *had) * 10 > *had) {
			*had = want;
			Ghud_SetSize(ent, st->ghud[2 * i], want, want);
		}
	}

	key = (hl << 16) | (st->spawn_filter << 8) | count;
	if (st->ghud_count && key == st->ghud_key)
		return;
	st->ghud_key = key;
	memset(st->ghud_size, 0, sizeof(st->ghud_size));

	if (!st->ghud_count) {
		ring = gi.imageindex((char *)Jmp_MarkerPic());
		st->ghud_made = level.framenum;
		for (i = 0; i < count; i++) {
			int el = Ghud_NewElement(ent, GHT_IMG);

			Ghud_SetFlags(ent, el, GHF_3DPOS);
			Ghud_SetPosition3D(ent, el, origins[i][0], origins[i][1], origins[i][2] + 8);
			Ghud_SetInt(ent, el, ring);
			Ghud_SetSize(ent, el, JMP_MARKER_PX, JMP_MARKER_PX);
			st->ghud[st->ghud_count++] = el;

			el = Ghud_NewElement(ent, GHT_TEXT);
			Ghud_SetFlags(ent, el, GHF_3DPOS);
			Ghud_SetPosition3D(ent, el, origins[i][0], origins[i][1], origins[i][2] + 72);
			Ghud_SetTextFlags(ent, el, UI_CENTER);
			st->ghud[st->ghud_count++] = el;
		}
	}

	for (i = 0; i < count && 2 * i + 1 < st->ghud_count; i++) {
		int el_ring = st->ghud[2 * i], el = st->ghud[2 * i + 1];
		const mvp_trail_t *t = &st->demo->trails[st->routes[st->vis[i]]];

		if (i + 1 == hl) {
			Q_snprintf(text, sizeof(text), "> %d: round %d <", i + 1, st->demo->rounds[t->round].number);
			Ghud_SetColor(ent, el, 255, 220, 0, 255);
			Ghud_SetColor(ent, el_ring, 255, 220, 0, 255);
		} else {
			Q_snprintf(text, sizeof(text), "%d", i + 1);
			Ghud_SetColor(ent, el, 255, 255, 255, 255);
			Ghud_SetColor(ent, el_ring, 255, 150, 60, 255);
		}
		Ghud_SetText(ent, el, text);
	}
}

static void JmpPathOver(edict_t *ent, jmp_path_t *st, qboolean on)
{
	if (on && !st->vis_count) {
		if (st->route_count)
			gi.cprintf(ent, PRINT_HIGH, "No route of %s from spawn %d\n", JmpPathPlayerName(st), st->spawn_filter);
		else
			gi.cprintf(ent, PRINT_HIGH, "No routes listed: jmod paths picks a demo and a player\n");
		return;
	}
	if (ent->client->layout == LAYOUT_MENU)
		PMenu_Close(ent);
	st->over = on;
	st->sel = -1;
	st->click_ms = 0;
	if (!on) {
		JmpPathGhostsClear(st);
		st->train = -1;
	}
	if (on) {
		JmpPathStuffShow(ent, st);
		if (st->spawn_filter)
			gi.cprintf(ent, PRINT_HIGH, "%d route%s of %s from spawn %d: fire on a number for one, twice for its ghost. Escape puts them away\n",
				st->vis_count, st->vis_count == 1 ? "" : "s", JmpPathPlayerName(st), st->spawn_filter);
		else
			gi.cprintf(ent, PRINT_HIGH, "%d routes of %s on the map: fire on a number for one, twice for its ghost. Escape puts them away\n",
				st->vis_count, JmpPathPlayerName(st));
	} else {
		JmpPathStuff(ent, "jpath off");
	}
	JmpPathMarkers(ent, st);
}

// a click on a route's number: the route alone, followed from the player's view
static void JmpPathOne(edict_t *ent, jmp_path_t *st, int k)
{
	JmpPathSolo(ent, st, k, true);
}

// with the overview up the markers are buttons: a click draws that
// route, a second one in time sends its ghost. Called with every command.
qboolean JmpPath_Fire(edict_t *ent, usercmd_t *ucmd)
{
	jmp_path_t *st = JmpPath(ent);
	gclient_t *client = ent->client;
	qboolean fire = (ucmd->buttons & BUTTON_ATTACK) != 0;
	qboolean pressed = fire && !st->fire_held;
	int k = st->aim > 0 && st->aim <= st->vis_count ? st->vis[st->aim - 1] : -1;

	st->fire_held = fire;
	if (!st->over || client->layout == LAYOUT_MENU || (ent->svflags & SVF_NOCLIENT))
		return false;
	// shift on a route's number drills it - with alt as well, the
	// round's others run with it; on a spawn marker it is the routes
	// from that spawn, which is Jmp_MenuFire's
	if (ucmd->impulse == IMPULSE_SHIFT || client->jmp_shift) {
		if (pressed && k >= 0) {
			client->latched_buttons &= ~BUTTON_ATTACK;
			JmpPathTrain(ent, st, k);
			if (client->jmp_alt) {
				JmpPath_DrillOthers(ent, 2);
				st->train_preset = true;
			}
			return true;
		}
		return false;
	}
	// alt on a route's number shows its round; on a spawn marker it asks
	// for another player, which is Jmp_MenuFire's
	if (client->jmp_alt) {
		if (pressed && k >= 0) {
			client->latched_buttons &= ~BUTTON_ATTACK;
			// how much of the round to run comes first
			st->ask_route = k;
			JmpPathShow(ent, JMP_MENU_PATH_SECS, -1);
			return true;
		}
		return false;
	}

	if (st->click_ms > 0) {
		st->click_ms -= ucmd->msec;
		if (pressed && k == st->click_route) {
			st->click_ms = 0;
			client->latched_buttons &= ~BUTTON_ATTACK;
			JmpPathGhost(ent, st, k, false);
			return true;
		} else if (pressed) {
			st->click_ms = 0;
		} else if (st->click_ms <= 0) {
			st->click_ms = 0;
			JmpPathOne(ent, st, st->click_route);
			return true;
		}
	}

	// off the route markers the click is the spawn markers' (a plain fire
	// on one spawns there, to run beside the ghosts)
	if (!pressed || k < 0)
		return false;
	client->latched_buttons &= ~BUTTON_ATTACK;
	st->click_route = k;
	st->click_ms = JMP_PATH_CLICK_MS;
	return true;
}

qboolean JmpPath_Escape(edict_t *ent)
{
	jmp_path_t *st = JmpPath(ent);

	if (!st->over)
		return false;
	if (st->train >= 0) {
		st->train = -1;
		Cmd_SpawnCancel_f(ent);
	}
	if (st->solo >= 0) {
		// back to the set the route was picked from, the follow let go
		// and the round's ghosts with it - one Escape, not two - and he
		// is put back where he clicked the marker
		if (!st->back_set || !Jmp_PlayStopTo(ent, st->back_origin, st->back_pitch, st->back_yaw))
			Jmp_PlayEscape(ent);
		st->back_set = false;
		st->round_view = false;
		st->solo = -1;
		JmpPathFilter(st);
		JmpPathOver(ent, st, false);
		JmpPathOver(ent, st, true);
		if (st->over && st->spawn_filter)
			JmpPathGhostsSpawn(ent, st, true);
		return true;
	}
	JmpPathOver(ent, st, false);
	return true;
}

// jmod_paths_open: a demo's name the client set before the map, for the
// pathing menu to open on as soon as he is in - the demo browser's way
// into a map's routes. Read once, then cleared.
static void JmpPathAutoOpen(edict_t *ent)
{
	jmp_path_t *st = JmpPath(ent);
	char name[MAX_OSPATH];

	if (!jmod_paths_open)
		jmod_paths_open = gi.cvar("jmod_paths_open", "", 0);
	if (!jmod_paths_open->string[0] || level.framenum < 10)
		return;
	if (ent->deadflag || ent->client->pers.spectator || ent->client->layout == LAYOUT_MENU)
		return;
	Q_strncpyz(name, jmod_paths_open->string, sizeof(name));
	gi.cvar_set("jmod_paths_open", "");
	if (!JmpPathLoad(ent, st, name))
		return;
	st->top = 0;
	JmpPathShow(ent, JMP_MENU_PATH_PLAYERS, -1);
}

void JmpPath_Frame(edict_t *ent)
{
	JmpPathAutoOpen(ent);
	JmpPathMarkers(ent, JmpPath(ent));
	JmpPathGhostsFrame(ent, JmpPath(ent));
}

void JmpPath_HudCleared(edict_t *ent)
{
	jmp_path_t *st = &jmp_paths[ent - g_edicts - 1];

	st->ghud_count = 0;
	st->tip_on = false;
	st->names_on = false;	// the names went with the rest; made again with the next ghosts
}

void JmpPath_ClientDisconnect(edict_t *ent)
{
	jmp_path_t *st = &jmp_paths[ent - g_edicts - 1];

	JmpPathGhostsClear(st);
	MvdPath_Free(st->demo);
	memset(st, 0, sizeof(*st));
}

/*
==============================================================================
THE MENUS
==============================================================================
*/

static void JmpPathPickDemo(edict_t *ent, pmenu_t *p)
{
	jmp_path_t *st = JmpPath(ent);
	int i = (int)(intptr_t)p->arg;

	if (i < 0) {
		// the one loaded
		JmpPathShow(ent, JMP_MENU_PATH_PLAYERS, -1);
		return;
	}
	if (i >= jmp_path_demo_count)
		return;
	if (!JmpPathLoad(ent, st, jmp_path_demos[i].name)) {
		JmpPathShow(ent, JMP_MENU_PATHS, Jmp_MenuRowOf(ent, p));
		return;
	}
	st->top = 0;
	JmpPathShow(ent, JMP_MENU_PATH_PLAYERS, -1);
}

/*
A player picked: the spawn markers come up, and shift+fire on one draws
the routes he took from that spawn (JmpPath_SpawnPick, from
Jmp_MenuFire); a plain fire spawns there as ever, to run beside them.
*/
static void JmpPathSpawnMode(edict_t *ent, jmp_path_t *st)
{
	int k, spawns = 0, seen[JMP_SPOTS_MAX + 1] = { 0 };

	if (ent->client->layout == LAYOUT_MENU)
		PMenu_Close(ent);
	if (st->over)
		JmpPathOver(ent, st, false);
	st->spawn_mode = true;
	ent->client->resp.jmp_markers = true;
	for (k = 0; k < st->route_count; k++)
		if (st->spawn[k] > 0 && st->spawn[k] <= JMP_SPOTS_MAX && !seen[st->spawn[k]]++)
			spawns++;
	gi.cprintf(ent, PRINT_HIGH, "%s: %d route%s from %d spawn%s. Shift+fire on a spawn marker draws the ones from it; jmod pathmap draws them all\n",
		JmpPathPlayerName(st), st->route_count, st->route_count == 1 ? "" : "s", spawns, spawns == 1 ? "" : "s");
}

// shift+fire on spawn marker spot while a player is picked: his routes
// from there. False when it is not the pathing's click to take.
qboolean JmpPath_SpawnPick(edict_t *ent, int spot)
{
	jmp_path_t *st = JmpPath(ent);

	if (!st->spawn_mode || !st->route_count || spot <= 0 || spot >= JMP_SPOT_JUMPS)
		return false;
	st->spawn_filter = spot;
	st->solo = -1;
	st->train = -1;
	JmpPathFilter(st);
	if (st->over)
		JmpPathOver(ent, st, false);
	JmpPathOver(ent, st, true);
	if (st->over)
		JmpPathGhostsSpawn(ent, st, true);
	return true;
}

// alt+fire on a spawn marker while a player is picked: the players
// menu, to pick another. False when it is not the pathing's click to take.
qboolean JmpPath_SpawnAlt(edict_t *ent)
{
	jmp_path_t *st = JmpPath(ent);

	if (!st->spawn_mode || !st->demo)
		return false;
	if (st->over)
		JmpPathOver(ent, st, false);
	st->top = 0;
	JmpPathShow(ent, JMP_MENU_PATH_PLAYERS, -1);
	return true;
}

// the spawn markers' tooltip while a player is picked
qboolean JmpPath_SpawnTip(edict_t *ent, char *shift, size_t size)
{
	jmp_path_t *st = JmpPath(ent);

	if (!st->spawn_mode || !st->route_count)
		return false;
	Q_snprintf(shift, size, "Shift + click: %.16s's routes from here", JmpPathPlayerName(st));
	return true;
}

// the client's aq2world list, on this map: a demo got there is listed here
static void JmpPathMenuAq2world(edict_t *ent, pmenu_t *p)
{
	char cmd[64];

	PMenu_Close(ent);
	Q_snprintf(cmd, sizeof(cmd), "aq2world map %s", level.mapname);	// that map exactly, three months of it
	JmpPathStuff(ent, cmd);
}

/*
A player found over the map's demos: each demo's first block names its
players, read off its head alone, and the name typed is matched fuzzily
against them, as the browsers filter. The demos he is in come up a list,
the closest names first, and one picked opens on his routes.
*/
static void JmpPathMenuFind(edict_t *ent, pmenu_t *p)
{
	JmpPathScan();
	if (!jmp_path_demo_count) {
		gi.cprintf(ent, PRINT_HIGH, "No demos of %s in demos/ to search: get one from aq2world first\n", level.mapname);
		gi.centerprintf(ent, "No demos of %s to search\n", level.mapname);
		return;
	}
	Jmp_AskPathPlayer(ent);
}

void JmpPath_FindAnswer(edict_t *ent, const char *text)
{
	jmp_path_t *st = JmpPath(ent);
	char names[MAX_CLIENTS][MVP_NAME_MAX], path[MAX_OSPATH];
	static byte head[JMP_PATH_HEAD];
	FILE *f;
	size_t len, n = 0;
	int i, k, count, score, best, bestk;

	for (; text && *text && n + 1 < sizeof(st->find); text++)
		if (*text > 32 && *text < 127 && *text != '"')
			st->find[n++] = *text;
	st->find[n] = 0;
	if (!n) {
		JmpPathShow(ent, JMP_MENU_PATHS, -1);
		return;
	}

	JmpPathScan();
	st->found_count = 0;
	for (i = 0; i < jmp_path_demo_count && st->found_count < JMP_PATH_FOUND; i++) {
		Q_snprintf(path, sizeof(path), "%s/demos/%s", Jmp_GameDir(), jmp_path_demos[i].name);
		f = fopen(path, "rb");
		if (!f)
			continue;
		len = fread(head, 1, sizeof(head), f);
		fclose(f);
		count = MvdPath_Names(head, len, names, MAX_CLIENTS);
		best = -1;
		bestk = -1;
		for (k = 0; k < count; k++) {
			score = Fuzzy_Score(st->find, names[k]);
			if (score > best) {
				best = score;
				bestk = k;
			}
		}
		if (best < 0)
			continue;
		st->found[st->found_count].demo = i;
		st->found[st->found_count].score = best;
		Q_strncpyz(st->found[st->found_count].name, names[bestk], MVP_NAME_MAX);
		st->found_count++;
	}
	// the closest names first; among equals the newest, as scanned
	for (i = 1; i < st->found_count; i++) {
		int j = i;

		while (j > 0 && st->found[j - 1].score < st->found[j].score) {
			jmp_path_found_t t = st->found[j - 1];
			st->found[j - 1] = st->found[j];
			st->found[j] = t;
			j--;
		}
	}
	st->top = 0;
	JmpPathShow(ent, JMP_MENU_PATH_FOUND, -1);
}

// a demo found: loaded, and his routes
static void JmpPathPickFound(edict_t *ent, pmenu_t *p)
{
	jmp_path_t *st = JmpPath(ent);
	int i = (int)(intptr_t)p->arg, k;

	if (i < 0 || i >= st->found_count || st->found[i].demo >= jmp_path_demo_count)
		return;
	if (!JmpPathLoad(ent, st, jmp_path_demos[st->found[i].demo].name)) {
		JmpPathShow(ent, JMP_MENU_PATH_FOUND, Jmp_MenuRowOf(ent, p));
		return;
	}
	for (k = 0; k < st->demo->player_count; k++)
		if (!strcmp(st->demo->players[k].name, st->found[i].name))
			break;
	if (k == st->demo->player_count) {
		// in at the start, out before a whole round: the players menu then
		st->top = 0;
		JmpPathShow(ent, JMP_MENU_PATH_PLAYERS, -1);
		return;
	}
	st->player = k;
	JmpPathRoutes(st);
	st->top = 0;
	JmpPathSpawnMode(ent, st);
}

static void JmpPathPickPlayer(edict_t *ent, pmenu_t *p)
{
	jmp_path_t *st = JmpPath(ent);
	int i = (int)(intptr_t)p->arg;

	if (!st->demo || i < 0 || i >= st->demo->player_count)
		return;
	st->player = i;
	JmpPathRoutes(st);
	st->top = 0;
	JmpPathSpawnMode(ent, st);
}

static void JmpPathPickRoute(edict_t *ent, pmenu_t *p)
{
	jmp_path_t *st = JmpPath(ent);

	st->pick = (int)(intptr_t)p->arg;
	JmpPathShow(ent, JMP_MENU_PATH_ROUTE, -1);
}

static void JmpPathMenuOver(edict_t *ent, pmenu_t *p)
{
	JmpPathOver(ent, JmpPath(ent), true);
}

// alt+fire's question answered: the round run that far
static void JmpPathPickSecs(edict_t *ent, pmenu_t *p)
{
	jmp_path_t *st = JmpPath(ent);

	JmpPathRound(ent, st, st->ask_route, (int)(intptr_t)p->arg);
}

// the row on how far past ACTION! the routes go: Enter steps it up,
// round to the shortest; the routes are folded afresh over the new span
static void JmpPathMenuSecs(edict_t *ent, pmenu_t *p)
{
	jmp_path_t *st = JmpPath(ent);
	int i, n = sizeof(jmp_path_secs_steps) / sizeof(jmp_path_secs_steps[0]);
	int now = (st->frames - MVP_ACTION_FRAMES) / 10, next = jmp_path_secs_steps[0];

	for (i = 0; i < n; i++)
		if (jmp_path_secs_steps[i] > now) {
			next = jmp_path_secs_steps[i];
			break;
		}
	gi.cvar_set("jmod_path_secs", va("%d", next));
	if (st->over)
		JmpPathOver(ent, st, false);
	JmpPathRoutes(st);
	JmpPathShow(ent, JMP_MENU_PATH_ROUTES, Jmp_MenuRowOf(ent, p));
}

static void JmpPathMenuAllSpawns(edict_t *ent, pmenu_t *p)
{
	jmp_path_t *st = JmpPath(ent);

	st->spawn_filter = 0;
	st->solo = -1;
	JmpPathFilter(st);
	st->top = 0;
	JmpPathShow(ent, JMP_MENU_PATH_ROUTES, -1);
}

static void JmpPathMenuOne(edict_t *ent, pmenu_t *p)
{
	jmp_path_t *st = JmpPath(ent);

	JmpPathOne(ent, st, st->pick);
}

static void JmpPathMenuGhost(edict_t *ent, pmenu_t *p)
{
	jmp_path_t *st = JmpPath(ent);

	if (ent->client->layout == LAYOUT_MENU)
		PMenu_Close(ent);
	JmpPathGhost(ent, st, st->pick, (int)(intptr_t)p->arg != 0);
}

static void JmpPathPage(edict_t *ent, pmenu_t *p)
{
	jmp_path_t *st = JmpPath(ent);

	st->top = max(st->top + (int)(intptr_t)p->arg, 0);
	JmpPathShow(ent, ent->client->jmp_menu_step, Jmp_MenuRowOf(ent, p));
}

static void JmpPathBack(edict_t *ent, pmenu_t *p)
{
	int step = ent->client->jmp_menu_step;

	JmpPath(ent)->top = 0;
	if (step == JMP_MENU_PATHS)
		Jmp_MenuItems(ent, p);
	else if (step == JMP_MENU_PATH_FOUND)
		JmpPathShow(ent, JMP_MENU_PATHS, -1);
	else if (step == JMP_MENU_PATH_SECS)
		PMenu_Close(ent);	// back to the markers
	else
		JmpPathShow(ent, step - 1, -1);
}

// "Previous page" and "Next page" under a list of n, a page of size at a time
static void JmpPathPaging(edict_t *ent, jmp_path_t *st, int row, int n, int size)
{
	if (st->top > 0)
		Jmp_MenuRow(ent, row, PMENU_ALIGN_LEFT, -size, JmpPathPage, "Previous page");
	if (st->top + size < n)
		Jmp_MenuRow(ent, row + 1, PMENU_ALIGN_LEFT, size, JmpPathPage, "Next page");
}

/*
A row of up to 40 conchars is drawn whole (PMenu_Update shifts a long
one left), and the TrueType layout font fits a third more in the same
span: the demo rows run to 52, which the font shows whole and conchars
cut at the screen's edge; the buffer (jmp_menu_text) cuts the rest.
*/
static void JmpPathShow(edict_t *ent, int step, int cur)
{
	jmp_path_t *st = JmpPath(ent);
	gclient_t *client = ent->client;
	char label[64], desc[JMP_PATH_LOC * 2], loc[JMP_PATH_LOC];
	int i, row, n;

	Jmp_MenuFrame(ent);

	// a step needs what the one before it picked
	if (step >= JMP_MENU_PATH_PLAYERS && step != JMP_MENU_PATH_FOUND && !st->demo)
		step = JMP_MENU_PATHS;
	if (step >= JMP_MENU_PATH_ROUTES && step != JMP_MENU_PATH_FOUND && st->player < 0)
		step = JMP_MENU_PATH_PLAYERS;
	if (step == JMP_MENU_PATH_SECS && !st->route_count)
		step = JMP_MENU_PATH_ROUTES;
	if (step == JMP_MENU_PATH_ROUTE && (st->pick < 0 || st->pick >= st->route_count))
		step = JMP_MENU_PATH_ROUTES;
	if (step != client->jmp_menu_step)
		cur = -1;
	client->jmp_menu_step = step;

	if (step == JMP_MENU_PATHS) {
		JmpPathScan();
		n = jmp_path_demo_count;
		if (st->top >= n)
			st->top = max(n - 1, 0) / JMP_PATH_ROWS * JMP_PATH_ROWS;
		Jmp_MenuRow(ent, 0, PMENU_ALIGN_CENTER, 0, NULL, "*Player pathing: a demo of %.13s", level.mapname);
		Jmp_MenuRow(ent, 2, PMENU_ALIGN_CENTER, 0, NULL, n ? "%d .mvd2 of it in demos/" : "none in demos/ yet", n);
		Jmp_MenuRow(ent, 3, PMENU_ALIGN_LEFT, 0, JmpPathMenuAq2world, "Download a recent pickup game of this map...");
		Jmp_MenuRow(ent, 4, PMENU_ALIGN_LEFT, 0, JmpPathMenuFind, "Search for player in local demos...");
		row = JMP_PATH_FIRST + 3;
		if (st->demo) {
			JmpPathDemoLabel(st->demo_name, label, sizeof(label));
			Jmp_MenuRow(ent, row, PMENU_ALIGN_LEFT, -1, JmpPathPickDemo, "Loaded: %.44s", label);
			if (cur < 0)
				cur = row;
			row += 2;
		}
		for (i = st->top; i < n && i < st->top + JMP_PATH_ROWS; i++, row++) {
			JmpPathDemoLabel(jmp_path_demos[i].name, label, sizeof(label));
			Jmp_MenuRow(ent, row, PMENU_ALIGN_LEFT, i, JmpPathPickDemo, "%.52s", label);
			if (cur < 0)
				cur = row;
		}
		if (!n)
			Jmp_MenuRow(ent, row, PMENU_ALIGN_LEFT, 0, NULL, "No .mvd2 of %.16s in demos/", level.mapname);
		JmpPathPaging(ent, st, JMP_PATH_FIRST + 3 + JMP_PATH_ROWS + 2, n, JMP_PATH_ROWS);
	} else if (step == JMP_MENU_PATH_FOUND) {
		n = st->found_count;
		if (st->top >= n)
			st->top = max(n - 1, 0) / JMP_PATH_ROWS * JMP_PATH_ROWS;
		Jmp_MenuRow(ent, 0, PMENU_ALIGN_CENTER, 0, NULL, "*%.16s: %d demo%s of %.12s", st->find, n,
			n == 1 ? "" : "s", level.mapname);
		row = JMP_PATH_FIRST;
		for (i = st->top; i < n && i < st->top + JMP_PATH_ROWS; i++, row++) {
			JmpPathDemoLabel(jmp_path_demos[st->found[i].demo].name, label, sizeof(label));
			Jmp_MenuRow(ent, row, PMENU_ALIGN_LEFT, i, JmpPathPickFound, "%-30.30s %.16s", label, st->found[i].name);
			if (cur < 0)
				cur = row;
		}
		if (!n)
			Jmp_MenuRow(ent, row, PMENU_ALIGN_LEFT, 0, NULL, "Nobody like that in the demos here");
		JmpPathPaging(ent, st, JMP_PATH_FIRST + JMP_PATH_ROWS + 1, n, JMP_PATH_ROWS);
	} else if (step == JMP_MENU_PATH_PLAYERS) {
		n = st->demo->player_count;
		if (st->top >= n)
			st->top = max(n - 1, 0) / JMP_PATH_ROWS * JMP_PATH_ROWS;
		JmpPathDemoLabel(st->demo_name, label, sizeof(label));
		Jmp_MenuRow(ent, 0, PMENU_ALIGN_CENTER, 0, NULL, "*%.50s", label);
		Jmp_MenuRow(ent, 2, PMENU_ALIGN_CENTER, 0, NULL, "%d rounds. Whose routes?", st->demo->round_count);
		row = JMP_PATH_FIRST + 1;
		for (i = st->top; i < n && i < st->top + JMP_PATH_ROWS; i++, row++) {
			const mvp_player_t *pl = &st->demo->players[i];

			Jmp_MenuRow(ent, row, PMENU_ALIGN_LEFT, i, JmpPathPickPlayer, "%-24.24s %2d round%s",
				pl->name, pl->trails, pl->trails == 1 ? "" : "s");
			if (cur < 0 && (i == st->player || (st->player < 0 && i == st->top)))
				cur = row;
		}
		if (!n)
			Jmp_MenuRow(ent, row, PMENU_ALIGN_LEFT, 0, NULL, "Nobody played a round in it");
		JmpPathPaging(ent, st, JMP_PATH_FIRST + 1 + JMP_PATH_ROWS + 1, n, JMP_PATH_ROWS);
	} else if (step == JMP_MENU_PATH_ROUTES) {
		n = st->vis_count;
		if (st->top >= n)
			st->top = max(n - 1, 0) / JMP_PATH_ROUTE_ROWS * JMP_PATH_ROUTE_ROWS;
		if (st->spawn_filter)
			Jmp_MenuRow(ent, 0, PMENU_ALIGN_CENTER, 0, NULL, "*%.20s: %d from spawn %d", JmpPathPlayerName(st),
				n, st->spawn_filter);
		else
			Jmp_MenuRow(ent, 0, PMENU_ALIGN_CENTER, 0, NULL, "*%.24s: %d route%s", JmpPathPlayerName(st),
				n, n == 1 ? "" : "s");
		Jmp_MenuRow(ent, 2, PMENU_ALIGN_CENTER, 0, NULL, "of %d rounds", st->demo->players[st->player].trails);
		Jmp_MenuRow(ent, JMP_PATH_FIRST, PMENU_ALIGN_LEFT, 0, JmpPathMenuSecs,
			(st->frames - MVP_ACTION_FRAMES) / 10 >= MVP_AFTER_FRAMES / 10 ? "Shown to the round's end"
			: "Shown to %d s past ACTION!", (st->frames - MVP_ACTION_FRAMES) / 10);
		Jmp_MenuRow(ent, JMP_PATH_FIRST + 1, PMENU_ALIGN_LEFT, 0, JmpPathMenuOver, "All of them on the map");
		if (st->spawn_filter)
			Jmp_MenuRow(ent, JMP_PATH_FIRST + 2, PMENU_ALIGN_LEFT, 0, JmpPathMenuAllSpawns, "From every spawn");
		row = JMP_PATH_FIRST + 3;
		for (i = st->top; i < n && i < st->top + JMP_PATH_ROUTE_ROWS; i++, row++) {
			int k = st->vis[i];
			const mvp_trail_t *t = &st->demo->trails[st->routes[k]];

			JmpPathDesc(st, k, desc, sizeof(desc));
			if (st->dups[k])
				Q_snprintf(label, sizeof(label), "R%d x%d", st->demo->rounds[t->round].number, st->dups[k] + 1);
			else
				Q_snprintf(label, sizeof(label), "R%d", st->demo->rounds[t->round].number);
			Jmp_MenuRow(ent, row, PMENU_ALIGN_LEFT, k, JmpPathPickRoute, "%2d %-6.6s %.30s", i + 1, label, desc);
			if (cur < 0 && k == st->pick)
				cur = row;
		}
		if (!n)
			Jmp_MenuRow(ent, row, PMENU_ALIGN_LEFT, 0, NULL, st->spawn_filter ? "None from this spawn" : "No whole round of theirs");
		JmpPathPaging(ent, st, JMP_PATH_FIRST + 3 + JMP_PATH_ROUTE_ROWS + 1, n, JMP_PATH_ROUTE_ROWS);
		if (cur < 0)
			cur = JMP_PATH_FIRST + 1;
	} else if (step == JMP_MENU_PATH_SECS) {
		// alt+fire on a route: how much of the round to run
		const mvp_trail_t *t = &st->demo->trails[st->routes[Q_clip(st->ask_route, 0, max(st->route_count - 1, 0))]];
		int now = (st->run_frames - MVP_ACTION_FRAMES) / 10;
		int count = sizeof(jmp_path_round_steps) / sizeof(jmp_path_round_steps[0]);

		Jmp_MenuRow(ent, 0, PMENU_ALIGN_CENTER, 0, NULL, "*Round %d, the whole round", st->demo->rounds[t->round].number);
		Jmp_MenuRow(ent, 2, PMENU_ALIGN_CENTER, 0, NULL, "How much of it?");
		for (i = 0; i < count; i++) {
			int secs = jmp_path_round_steps[i];

			Jmp_MenuRow(ent, JMP_PATH_FIRST + i, PMENU_ALIGN_LEFT, secs, JmpPathPickSecs,
				secs >= MVP_AFTER_FRAMES / 10 ? "To the round's end" : "To %d s past ACTION!", secs);
			if (cur < 0 && secs >= now)
				cur = JMP_PATH_FIRST + i;
		}
		if (cur < 0)
			cur = JMP_PATH_FIRST;
	} else {
		const mvp_trail_t *t = &st->demo->trails[st->routes[st->pick]];
		const mvp_round_t *r = &st->demo->rounds[t->round];
		vec3_t o;
		int spawn, ms = (min(t->count, st->frames + 1) - 1) * 100;

		Jmp_MenuRow(ent, 0, PMENU_ALIGN_CENTER, 0, NULL, "*%d: round %d, %.16s", JmpPathVisNumber(st, st->pick), r->number, t->name);
		MvdPath_Origin(&st->demo->samples[t->first], o);
		spawn = JmpPathSpawn(o);
		// the places, where the map has a location file to name them
		if (spawn)
			Jmp_MenuRow(ent, 3, PMENU_ALIGN_LEFT, 0, NULL, "spawn %d%s%.30s", spawn, *JmpPathLoc(o, loc) ? ": " : "", loc);
		else if (*JmpPathLoc(o, loc))
			Jmp_MenuRow(ent, 3, PMENU_ALIGN_LEFT, 0, NULL, "LIGHTS: %.32s", loc);
		MvdPath_Origin(MvdPath_At(st->demo, t, MVP_ACTION_FRAMES), o);
		if (*JmpPathLoc(o, loc))
			Jmp_MenuRow(ent, 4, PMENU_ALIGN_LEFT, 0, NULL, "ACTION!: %.31s", loc);
		MvdPath_Origin(MvdPath_At(st->demo, t, st->frames), o);
		if (*JmpPathLoc(o, loc) || (t->died && t->count <= st->frames))
			Jmp_MenuRow(ent, 5, PMENU_ALIGN_LEFT, 0, NULL, "%s%s%.34s", t->died && t->count <= st->frames ? "died" : "then",
				loc[0] ? ": " : "", loc);
		Jmp_MenuRow(ent, 2, PMENU_ALIGN_CENTER, 0, NULL, "%d.%d seconds%s", ms / 1000, ms % 1000 / 100,
			t->died && t->count <= st->frames ? ", cut by the death" : "");
		if (st->dups[st->pick])
			Jmp_MenuRow(ent, 6, PMENU_ALIGN_LEFT, 0, NULL, "taken the same way %d times", st->dups[st->pick] + 1);
		Jmp_MenuRow(ent, 8, PMENU_ALIGN_LEFT, 0, JmpPathMenuOne, "Draw it on the map");
		Jmp_MenuRow(ent, 9, PMENU_ALIGN_LEFT, 0, JmpPathMenuGhost, "Watch its ghost, third person");
		Jmp_MenuRow(ent, 10, PMENU_ALIGN_LEFT, 1, JmpPathMenuGhost, "Watch from the player's view");
		Jmp_MenuRow(ent, 12, PMENU_ALIGN_LEFT, 0, JmpPathMenuOver, "All the routes on the map");
		if (cur < 0)
			cur = 8;
	}

	Jmp_MenuRow(ent, JMP_MENU_ROWS - 1, PMENU_ALIGN_LEFT, 0, JmpPathBack, "Back");
	if (cur < 0)
		cur = JMP_PATH_FIRST;

	if (client->layout == LAYOUT_MENU)
		PMenu_Close(ent);
	PMenu_Open(ent, client->jmp_menu, cur < 0 ? cur : Jmp_MenuRowAt(cur), JMP_MENU_FRAME);
}

// from the jmod item menu, and "jmod paths"
void Jmp_OpenPathMenu(edict_t *ent, pmenu_t *p)
{
	if (Jmp_OpenTakeMenu(ent))
		return;
	JmpPath(ent)->top = 0;
	JmpPathShow(ent, JMP_MENU_PATHS, -1);
}

// jmod paths [demo] | jmod path <#> [ghost|pov] | jmod pathmap
void JmpPath_Cmd(edict_t *ent, const char *cmd)
{
	jmp_path_t *st = JmpPath(ent);
	const char *arg = gi.argc() > 2 ? gi.argv(2) : "";
	int k;

	if (!Q_stricmp(cmd, "pathmap")) {
		st->solo = -1;
		JmpPathFilter(st);
		JmpPathOver(ent, st, !st->over);
		return;
	}
	if (!Q_stricmp(cmd, "pathspawn")) {
		k = atoi(arg);
		if (!st->route_count) {
			gi.cprintf(ent, PRINT_HIGH, "No routes listed: jmod paths picks a demo and a player\n");
			return;
		}
		if (!arg[0] || k < 0) {
			gi.cprintf(ent, PRINT_HIGH, "Usage: jmod pathspawn <spawnpoint>: %s's routes from it; 0 from every spawn\n",
				JmpPathPlayerName(st));
			return;
		}
		st->spawn_mode = true;
		st->spawn_filter = k;
		st->solo = -1;
		JmpPathFilter(st);
		if (st->over)
			JmpPathOver(ent, st, false);
		JmpPathOver(ent, st, true);
		if (st->over && k)
			JmpPathGhostsSpawn(ent, st, true);
		return;
	}
	if (!Q_stricmp(cmd, "path")) {
		k = atoi(arg) - 1;
		if (!arg[0] || k < 0 || k >= st->vis_count) {
			gi.cprintf(ent, PRINT_HIGH, "Usage: jmod path <1-%d> [ghost|pov]: a route of %s\n",
				st->vis_count, st->route_count ? JmpPathPlayerName(st) : "the player jmod paths picks");
			return;
		}
		k = st->vis[k];
		arg = gi.argc() > 3 ? gi.argv(3) : "";
		if (!Q_stricmp(arg, "train"))
			JmpPathTrain(ent, st, k);
		else if (!Q_stricmp(arg, "ghost"))
			JmpPathGhost(ent, st, k, false);
		else if (!Q_stricmp(arg, "pov"))
			JmpPathGhost(ent, st, k, true);
		else
			JmpPathOne(ent, st, k);
		return;
	}

	if (!arg[0]) {
		Jmp_OpenPathMenu(ent, NULL);
		return;
	}
	if (!JmpPathLoad(ent, st, arg))
		return;
	st->top = 0;
	JmpPathShow(ent, JMP_MENU_PATH_PLAYERS, -1);
}
