#include "g_local.h"

//cvar_t *jump;

char *jump_statusbar =
    "yb -24 "
    "if 9 "
    "xr 0 "
    "yb 0 "
    "xv 0 "
    "yv 0 "
    "pic 9 "
    "endif "
    //  Current Speed
    "xr -105 "
    "yt 2 "
    "string \"Current Speed\" "
    "xr -82 "
    "yt 12 "
    "num 6 1 "

    //  High Speed
    "xr -81 "
    "yt 34 "
    "string \"High Speed\" "
    "xr -82 "
    "yt 44 "
    "num 6 2 "

    //  last fall damage
    "xr -129 "
    "yt 66 "
    "string \"Last Fall Damage\" "
    "xr -82 "
    "yt 76 "
    "num 10 3 "
;

void Jmp_SetStats(edict_t *ent)
{
	vec3_t	velocity;
	vec_t	speed;

	// calculate speed
	VectorClear(velocity);
	VectorCopy(ent->velocity, velocity);
	speed = VectorNormalize(velocity);

	if(speed > ent->client->resp.jmp_highspeed)
		ent->client->resp.jmp_highspeed = speed;

	ent->client->ps.stats[STAT_SPEEDX] = speed;
	ent->client->ps.stats[STAT_HIGHSPEED] = ent->client->resp.jmp_highspeed;
	ent->client->ps.stats[STAT_FALLDMGLAST] = ent->client->resp.jmp_falldmglast;

	//
	// layouts
	//
	ent->client->ps.stats[STAT_LAYOUTS] = 0;

	if (ent->health <= 0 || level.intermission_framenum || ent->client->layout)
		ent->client->ps.stats[STAT_LAYOUTS] |= 1;
	if (ent->client->showinventory && ent->health > 0)
		ent->client->ps.stats[STAT_LAYOUTS] |= 2;

	SetIDView (ent);
}

void Jmp_EquipClient(edict_t *ent)
{
	memset(ent->client->inventory, 0, sizeof(ent->client->inventory));
	ent->client->weapon = 0;

	// make client non-solid
	ent->solid = SOLID_TRIGGER;
	AddToTransparentList(ent);
}

void Cmd_Jmod_f (edict_t *ent)
{
	char *cmd = NULL;

	if( ! jump->value ) {
		gi.cprintf(ent, PRINT_HIGH, "The server does not have JumpMod enabled.\n");
		return;
	}

	if(gi.argc() < 2) {
		gi.cprintf(ent, PRINT_HIGH, "AQ2:TNG Jump mode commands:\n");
		gi.cprintf(ent, PRINT_HIGH, " jmod store - save your current point\n");
		gi.cprintf(ent, PRINT_HIGH, " jmod recall - teleport back to saved point\n");
		gi.cprintf(ent, PRINT_HIGH, " jmod reset - remove saved point\n");
		gi.cprintf(ent, PRINT_HIGH, " jmod clear - reset stats\n");
		gi.cprintf(ent, PRINT_HIGH, " jmod noclip - toggle noclip\n");
		gi.cprintf(ent, PRINT_HIGH, " jmod spawnp [#] [delay] [repeat] - teleport to spawnpoint #, 0 or none random\n");
		gi.cprintf(ent, PRINT_HIGH, " jmod spawnc [delay] [repeat] - teleport to the closest spawnpoint\n");
		gi.cprintf(ent, PRINT_HIGH, " jmod delay [secs] - default wait before spawnp/spawnc\n");
		gi.cprintf(ent, PRINT_HIGH, " jmod repeat [secs] - repeat the last spawn every secs, 0 stops\n");
		gi.cprintf(ent, PRINT_HIGH, " jmod respawn - the last spawn again, restarting its delay and repeat\n");
		gi.cprintf(ent, PRINT_HIGH, " jmod stop - stop a repeating or pending spawn\n");
		gi.cprintf(ent, PRINT_HIGH, " jmod goto <#> <#> <#> - teleport to x y z coordinates\n");
		gi.cprintf(ent, PRINT_HIGH, " jmod lca - start Lights Camera Action\n");
		gi.cprintf(ent, PRINT_HIGH, " jmod laser - toggle lasersight\n");
		gi.cprintf(ent, PRINT_HIGH, " jmod slippers - toggle stealth slippers\n");
		gi.cprintf(ent, PRINT_HIGH, " jmod menu - open the jump menu\n");
		gi.cprintf(ent, PRINT_HIGH, " jmod spawns - pick a spawnpoint, then its delay and repeat, from a menu\n");

		return;
	}

	cmd = gi.argv(1);

	if(Q_stricmp(cmd, "store") == 0)
	{
		Cmd_Store_f(ent);
		return;
	}
	else if(Q_stricmp(cmd, "recall") == 0)
	{
		Cmd_Recall_f(ent);
		return;
	}
	else if(Q_stricmp(cmd, "reset") == 0)
	{
		Cmd_Reset_f(ent);
		return;
	}
	else if(Q_stricmp(cmd, "clear") == 0 || Q_stricmp(cmd, "rhs") == 0)
	{
		Cmd_Clear_f(ent);
		return;
	}
	else if(Q_stricmp(cmd, "goto") == 0)
	{
		Cmd_Goto_f(ent);
		return;
	}
	else if(Q_stricmp(cmd, "spawnp") == 0)
	{
		Cmd_GotoP_f(ent);
		return;
	}
	else if(Q_stricmp(cmd, "spawnc") == 0)
	{
		Cmd_GotoPC_f(ent);
		return;
	}
	else if(Q_stricmp(cmd, "delay") == 0)
	{
		Cmd_SpawnDelay_f(ent);
		return;
	}
	else if(Q_stricmp(cmd, "respawn") == 0)
	{
		Cmd_Respawn_f(ent);
		return;
	}
	else if(Q_stricmp(cmd, "repeat") == 0)
	{
		Cmd_SpawnRepeat_f(ent);
		return;
	}
	else if(Q_stricmp(cmd, "cancel") == 0 || Q_stricmp(cmd, "stop") == 0)
	{
		Cmd_SpawnCancel_f(ent);
		return;
	}
	else if(Q_stricmp(cmd, "lca") == 0)
	{
		Cmd_PMLCA_f(ent);
		return;
	}
	else if(Q_stricmp(cmd, "noclip") == 0)
	{
		Cmd_Noclip_f(ent);
		return;
	}
	// the menu names "laser" and "slippers"; "lasersight" is the old name
	else if(Q_stricmp(cmd, "laser") == 0 || Q_stricmp(cmd, "lasersight") == 0)
	{
		Cmd_Toggle_f(ent, "laser");
		return;
	}
	else if(Q_stricmp(cmd, "slippers") == 0)
	{
		Cmd_Toggle_f(ent, "slippers");
		return;
	}
	else if(Q_stricmp(cmd, "menu") == 0)
	{
		OpenPMItemMenu(ent);
		return;
	}
	else if(Q_stricmp(cmd, "spawns") == 0)
	{
		Jmp_OpenSpawnMenu(ent, NULL);
		return;
	}

	gi.cprintf(ent, PRINT_HIGH, "Unknown jmod command\n");
}

/*
jmod's spawnpoints: the deathmatch ones, then the teams' - where rounds
start on most maps - skipping any that sit on one already listed (maps
often put a deathmatch spot on each team spot). Numbered from 1 in this
order; spawnp, the menu and the markers all count the same way.
*/
static char *const jmp_spot_classes[] = {
	"info_player_deathmatch", "info_player_team1", "info_player_team2", "info_player_team3", NULL
};

int Jmp_Spots(edict_t **spots, int max)
{
	edict_t *spot;
	vec3_t d;
	int c, i, count = 0;

	for (c = 0; jmp_spot_classes[c]; c++) {
		for (spot = NULL; count < max && (spot = G_Find(spot, FOFS(classname), jmp_spot_classes[c])) != NULL; ) {
			for (i = 0; i < count; i++) {
				VectorSubtract(spot->s.origin, spots[i]->s.origin, d);
				if (VectorLength(d) < 32)
					break;
			}
			if (i == count)
				spots[count++] = spot;
		}
	}
	return count;
}

// the number of the spawnpoint nearest to ent, 0 if none
static int JmpClosestNumber(edict_t *ent)
{
	edict_t *spots[JMP_SPOTS_MAX];
	vec3_t d;
	float dist, best = -1;
	int i, count, number = 0;

	count = Jmp_Spots(spots, JMP_SPOTS_MAX);
	for (i = 0; i < count; i++) {
		VectorSubtract(spots[i]->s.origin, ent->s.origin, d);
		dist = VectorLength(d);
		if (best < 0 || dist < best) {
			best = dist;
			number = i + 1;
		}
	}
	return number;
}

void Cmd_PMLCA_f(edict_t *ent)
{
	if (ent->client->pers.spectator)
	{
		gi.cprintf(ent,PRINT_HIGH,"This command cannot be used by spectators\n");
		ent->client->resp.toggle_lca = 0;
		return;
	}

	// Set this to 1 to prevent damage during LCA
	lights_camera_action = 1;
	if (!ent->client->resp.toggle_lca)
	{
		gi.centerprintf (ent,"LIGHTS...\n");
		gi.sound(ent, CHAN_VOICE, gi.soundindex("atl/lights.wav"), 1, ATTN_STATIC, 0);
		ent->client->resp.toggle_lca = 43;
	}
	else if (ent->client->resp.toggle_lca == 23)
	{
		gi.centerprintf (ent,"CAMERA...\n");
		gi.sound(ent, CHAN_VOICE, gi.soundindex("atl/camera.wav"), 1, ATTN_STATIC, 0);
	}
    else if (ent->client->resp.toggle_lca == 3)
	{
		gi.centerprintf (ent,"ACTION!\n");
		gi.sound(ent, CHAN_VOICE, gi.soundindex("atl/action.wav"), 1, ATTN_STATIC, 0);
	}
	ent->client->resp.toggle_lca--;
	// Set it back to 0 for normal damage to occur
	lights_camera_action = 0;
}

// spawnpoint number (from 1, clamped), 0 a random one
edict_t *PMSelectSpawnPoint (int number)
{
	edict_t *spots[JMP_SPOTS_MAX];
	int count = Jmp_Spots(spots, JMP_SPOTS_MAX);

	if (!count)
		return NULL;
	if (number <= 0)
		return spots[rand() % count];
	return spots[min(number, count) - 1];
}

void jmodTeleport (edict_t *ent, edict_t *spot)
{
	vec3_t		teleport_goto, angles;
	int			i;

	VectorCopy (spot->s.origin, teleport_goto);
	teleport_goto[2] += 9;
	VectorCopy (spot->s.angles, angles);

	ent->client->jumping = 0;
	ent->movetype = MOVETYPE_NOCLIP;
	gi.unlinkentity (ent);

	VectorCopy (teleport_goto, ent->s.origin);
	VectorCopy (teleport_goto, ent->s.old_origin);

	// clear the velocity and hold them in place briefly
	VectorClear (ent->velocity);

	ent->client->ps.pmove.pm_time = 160>>3;		// hold time

	// draw the teleport splash on the player
	ent->s.event = EV_PLAYER_TELEPORT;

	VectorClear (ent->s.angles);
	VectorClear (ent->client->ps.viewangles);
	VectorClear (ent->client->v_angle);

	VectorCopy(angles,ent->s.angles);
	VectorCopy(ent->s.angles,ent->client->v_angle);

	for (i=0;i<2;i++)
		ent->client->ps.pmove.delta_angles[i] = ANGLE2SHORT(ent->client->v_angle[i] - ent->client->resp.cmd_angles[i]);
	if (ent->client->pers.spectator)
		ent->solid = SOLID_BBOX;
	else
		ent->solid = SOLID_TRIGGER;

	ent->deadflag = DEAD_NO;

	gi.linkentity (ent);

	ent->movetype = MOVETYPE_WALK;

	// Run LCA right after spawn
	Cmd_PMLCA_f(ent);
}

void Cmd_Goto_f (edict_t *ent)
{
	int 		i;
	vec3_t		teleport_goto;

	if (!ent->deadflag && !ent->client->pers.spectator)
	{
		// 5 = jmod goto x y z
		if (gi.argc() == 5)
		{
			// Verifying input
			if (Q_stricmp(gi.argv(0), "jmod") == 0 && Q_stricmp(gi.argv(1), "goto") == 0){
				// Hacky shit, set gi.argv(2) as teleport_goto[0], etc..:
				for (i = 0; i < 3; i++){
					teleport_goto[i] = atoi(gi.argv(i+2));
				}
			}
			teleport_goto[2] -= ent->viewheight;

			ent->client->jumping = 0;
			ent->movetype = MOVETYPE_NOCLIP;
			gi.unlinkentity (ent);

			VectorCopy (teleport_goto, ent->s.origin);
			VectorCopy (teleport_goto, ent->s.old_origin);

			// clear the velocity and hold them in place briefly
			VectorClear (ent->velocity);

			ent->client->ps.pmove.pm_time = 160>>3;		// hold time

			// draw the teleport splash on the player
			ent->s.event = EV_PLAYER_TELEPORT;

			VectorClear (ent->s.angles);
			VectorClear (ent->client->ps.viewangles);
			VectorClear (ent->client->v_angle);

			for (i=0;i<2;i++)
				ent->client->ps.pmove.delta_angles[i] = ANGLE2SHORT(ent->client->v_angle[i] - ent->client->resp.cmd_angles[i]);
			if (ent->client->pers.spectator)
				ent->solid = SOLID_BBOX;
			else
				ent->solid = SOLID_TRIGGER;

			ent->deadflag = DEAD_NO;

			gi.linkentity (ent);
			
			ent->movetype = MOVETYPE_WALK;
		}
		else
			gi.cprintf(ent,PRINT_HIGH,"Wrong syntax: goto <#> <#> <#>\n");
	}
	else
		gi.cprintf(ent,PRINT_HIGH,"This command cannot be used by spectators\n");
	
}

void Cmd_GotoP_f_compat (edict_t *ent, pmenu_t *p)
{
	Cmd_GotoP_f(ent);
}

void Cmd_Respawn_f_compat (edict_t *ent, pmenu_t *p)
{
	PMenu_Close(ent);
	Cmd_Respawn_f(ent);
}

void Cmd_GotoPC_f_compat (edict_t *ent, pmenu_t *p)
{
	Cmd_GotoPC_f(ent);
}

#define JMP_DELAY_MAX	30
#define JMP_REPEAT_MIN	1
#define JMP_REPEAT_MAX	60
#define JMP_COUNT_SECS	3	// the countdown shows the last seconds only

// a seconds argument, if given
static qboolean JmpSecsArg(int arg, float *secs)
{
	if (gi.argc() <= arg || Q_stricmp(gi.argv(0), "jmod") != 0)
		return false;

	*secs = atof(gi.argv(arg));
	return true;
}

static float JmpRepeatClip(float secs)
{
	return secs > 0 ? Q_clipf(secs, JMP_REPEAT_MIN, JMP_REPEAT_MAX) : 0;
}

static void JmpSpawnAt(edict_t *ent, float delay)
{
	gclient_t *client = ent->client;

	client->resp.jmp_spawn_frame = level.framenum + max((int)(delay * HZ + 0.5f), 1);
}

static void JmpSpawnNow(edict_t *ent)
{
	gclient_t *client = ent->client;
	edict_t *spot;

	if (client->resp.jmp_spawn_spot < 0)
		spot = PMSelectSpawnPoint(max(JmpClosestNumber(ent), 1));
	else
		spot = PMSelectSpawnPoint(client->resp.jmp_spawn_spot);

	if (!spot) {
		gi.cprintf(ent, PRINT_HIGH, "This map has no spawnpoints\n");
		client->resp.jmp_spawn_frame = 0;
		client->resp.jmp_spawn_repeat = 0;
		return;
	}
	jmodTeleport(ent, spot);

	// a drill: the same spawn again in repeat seconds
	if (client->resp.jmp_spawn_repeat > 0)
		JmpSpawnAt(ent, client->resp.jmp_spawn_repeat);
}

// teleport to a spawnpoint now or after delay seconds (Jmp_RunSpawn),
// then every repeat seconds until cancelled
static void JmpSpawn(edict_t *ent, int number, float delay, float repeat)
{
	gclient_t *client = ent->client;

	if (ent->deadflag || client->pers.spectator) {
		gi.cprintf(ent, PRINT_HIGH, "This command cannot be used by spectators\n");
		return;
	}

	delay = Q_clipf(delay, 0, JMP_DELAY_MAX);
	repeat = JmpRepeatClip(repeat);
	if (repeat > 0 && repeat != client->resp.jmp_spawn_repeat)
		gi.cprintf(ent, PRINT_HIGH, "Repeating every %g seconds, \"jmod stop\" to stop\n", repeat);

	client->resp.jmp_spawn_spot = number;
	client->resp.jmp_spawn_repeat = repeat;
	client->resp.jmp_spawn_last_delay = delay;
	client->resp.jmp_spawn_frame = 0;

	if (delay <= 0) {
		JmpSpawnNow(ent);
		return;
	}

	JmpSpawnAt(ent, delay);
	gi.centerprintf(ent, "Spawning in %g\n", delay);
}

static void JmpMarkersUpdate(edict_t *ent);

// called every frame for each client: counts a pending spawn down, and
// keeps the spawnpoint labels
void Jmp_RunSpawn(edict_t *ent)
{
	gclient_t *client = ent->client;
	int left;

	JmpMarkersUpdate(ent);

	if (!client->resp.jmp_spawn_frame)
		return;

	if (ent->deadflag || client->pers.spectator) {
		client->resp.jmp_spawn_frame = 0;
		client->resp.jmp_spawn_repeat = 0;
		return;
	}

	left = client->resp.jmp_spawn_frame - level.framenum;
	if (left <= 0) {
		client->resp.jmp_spawn_frame = 0;
		JmpSpawnNow(ent);
		return;
	}

	// on each of the last whole seconds, clear of the LCA's prints
	if (left % HZ == 0 && left / HZ <= JMP_COUNT_SECS)
		gi.centerprintf(ent, "Spawning in %d\n", left / HZ);
}

// jmod delay [secs]
void Cmd_SpawnDelay_f(edict_t *ent)
{
	if (gi.argc() >= 3)
		ent->client->resp.jmp_spawn_delay = Q_clipf(atof(gi.argv(2)), 0, JMP_DELAY_MAX);

	gi.cprintf(ent, PRINT_HIGH, "Spawn delay: %g seconds\n", ent->client->resp.jmp_spawn_delay);
}

// jmod repeat [secs] - repeat the last spawn every secs, 0 stops
void Cmd_SpawnRepeat_f(edict_t *ent)
{
	gclient_t *client = ent->client;
	float repeat;

	if (gi.argc() < 3) {
		if (client->resp.jmp_spawn_repeat > 0)
			gi.cprintf(ent, PRINT_HIGH, "Repeating every %g seconds\n", client->resp.jmp_spawn_repeat);
		else
			gi.cprintf(ent, PRINT_HIGH, "Not repeating. Usage: jmod repeat <secs>\n");
		return;
	}

	repeat = JmpRepeatClip(atof(gi.argv(2)));
	if (!repeat) {
		client->resp.jmp_spawn_repeat = 0;
		client->resp.jmp_spawn_frame = 0;
		gi.cprintf(ent, PRINT_HIGH, "Repeat stopped\n");
		return;
	}

	// the spawn the last spawnp/spawnc used, starting now
	JmpSpawn(ent, client->resp.jmp_spawn_spot, 0, repeat);
}

// jmod respawn - the last spawn again, with its delay; a repeat counts
// on from this one
void Cmd_Respawn_f(edict_t *ent)
{
	gclient_t *client = ent->client;

	JmpSpawn(ent, client->resp.jmp_spawn_spot, client->resp.jmp_spawn_last_delay,
		client->resp.jmp_spawn_repeat);
}

void Cmd_SpawnCancel_f(edict_t *ent)
{
	gclient_t *client = ent->client;

	if (!client->resp.jmp_spawn_frame)
		return;

	client->resp.jmp_spawn_frame = 0;
	client->resp.jmp_spawn_repeat = 0;
	gi.centerprintf(ent, " ");
	gi.cprintf(ent, PRINT_HIGH, "Spawn stopped\n");
}

// jmod spawnp [#] [delay] [repeat] - from the menu, random with the default delay
void Cmd_GotoP_f (edict_t *ent)
{
	float delay = ent->client->resp.jmp_spawn_delay, repeat = 0;
	int number = 0;

	if (gi.argc() >= 3 && Q_stricmp(gi.argv(0), "jmod") == 0)
		number = max(atoi(gi.argv(2)), 0);
	JmpSecsArg(3, &delay);
	JmpSecsArg(4, &repeat);

	JmpSpawn(ent, number, delay, repeat);
}

// jmod spawnc [delay] [repeat]
void Cmd_GotoPC_f (edict_t *ent)
{
	float delay = ent->client->resp.jmp_spawn_delay, repeat = 0;

	JmpSecsArg(2, &delay);
	JmpSecsArg(3, &repeat);

	JmpSpawn(ent, -1, delay, repeat);
}

/*
The spawnpoint menu, three steps: a spawnpoint, then the delay before
it, then how often to repeat it - the last choice spawns. Each step's
cursor starts on what was used last, so Enter, Enter repeats it. Its
rows live in the client, as the values shown are the player's own.
*/
#define JMP_MENU_SPOTS	8	// spawnpoints on a page
#define JMP_MENU_FIRST	8	// the row of the first one
#define JMP_MENU_CHOICE	4	// the row of a step's first value

static const float jmp_delays[] = { 0, 1, 2, 3, 5, 10 };
static const float jmp_repeats[] = { 0, 3, 5, 8, 10, 15, 20, 30 };

#define JMP_COUNT(a)	(int)(sizeof(a) / sizeof((a)[0]))

static const char jmp_menu_line[] = "\x9D\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9F";

static void JmpMenuShow(edict_t *ent, int step, int cur);

static void JmpMenuRow_Set(edict_t *ent, int row, int align, intptr_t arg,
	void (*func)(edict_t *, pmenu_t *), const char *fmt, ...)
{
	gclient_t *client = ent->client;
	pmenu_t *p = &client->jmp_menu[row];
	va_list argptr;

	p->text = NULL;
	if (fmt) {
		va_start(argptr, fmt);
		Q_vsnprintf(client->jmp_menu_text[row], sizeof(client->jmp_menu_text[row]), fmt, argptr);
		va_end(argptr);
		p->text = client->jmp_menu_text[row];
	}
	p->align = align;
	p->arg = (void *)arg;
	p->SelectFunc = func;
}

// the spawnpoint's name: its number and location, or closest/random
static void JmpSpotName(int number, char *buf, size_t size, qboolean shortname)
{
	edict_t *spot;
	char loc[128];

	if (number < 0) {
		Q_strncpyz(buf, "Closest spawnpoint", size);
		return;
	}
	if (number == 0) {
		Q_strncpyz(buf, "Random spawnpoint", size);
		return;
	}

	spot = PMSelectSpawnPoint(number);
	if (shortname || !spot || !GetPlayerLocation(spot, loc)) {
		Q_snprintf(buf, size, "Spawnpoint %d", number);
		return;
	}
	Q_snprintf(buf, size, "%d %s", number, loc);
}

static void JmpSecsName(float secs, char *buf, size_t size)
{
	if (secs == 1)
		Q_strncpyz(buf, "1 second", size);
	else
		Q_snprintf(buf, size, "%g seconds", secs);
}

// step 0: a spawnpoint
static void JmpMenuPickSpot(edict_t *ent, pmenu_t *p)
{
	ent->client->jmp_menu_pick = (int)(intptr_t)p->arg;
	JmpMenuShow(ent, 1, -1);
}

static void JmpMenuPage(edict_t *ent, pmenu_t *p)
{
	gclient_t *client = ent->client;

	client->jmp_menu_top = max(client->jmp_menu_top + (int)(intptr_t)p->arg, 0);
	JmpMenuShow(ent, 0, p - client->jmp_menu);
}

static void JmpMenuStop(edict_t *ent, pmenu_t *p)
{
	Cmd_SpawnCancel_f(ent);
	JmpMenuShow(ent, 0, -1);
}

static void JmpMenuItems(edict_t *ent, pmenu_t *p)
{
	PMenu_Close(ent);
	OpenPMItemMenu(ent);
}

// step 1: the delay, in tenths
static void JmpMenuPickDelay(edict_t *ent, pmenu_t *p)
{
	ent->client->resp.jmp_spawn_delay = (int)(intptr_t)p->arg / 10.0f;
	JmpMenuShow(ent, 2, -1);
}

// step 2: the repeat, in tenths - and spawn
static void JmpMenuPickRepeat(edict_t *ent, pmenu_t *p)
{
	gclient_t *client = ent->client;

	client->resp.jmp_menu_repeat = (int)(intptr_t)p->arg / 10.0f;
	PMenu_Close(ent);
	JmpSpawn(ent, client->jmp_menu_pick, client->resp.jmp_spawn_delay, client->resp.jmp_menu_repeat);
}

static void JmpMenuBack(edict_t *ent, pmenu_t *p)
{
	JmpMenuShow(ent, ent->client->jmp_menu_step - 1, -1);
}

static void JmpMenuSpots(edict_t *ent, int *cur)
{
	gclient_t *client = ent->client;
	edict_t *spots[JMP_SPOTS_MAX];
	char name[32];
	int i, count = Jmp_Spots(spots, JMP_SPOTS_MAX), row;

	if (client->jmp_menu_top >= count)
		client->jmp_menu_top = max(count - 1, 0) / JMP_MENU_SPOTS * JMP_MENU_SPOTS;

	JmpMenuRow_Set(ent, 0, PMENU_ALIGN_CENTER, 0, NULL, "*Pick a spawnpoint (%d)", count);
	if (client->resp.jmp_spawn_frame)
		JmpMenuRow_Set(ent, 3, PMENU_ALIGN_LEFT, 0, JmpMenuStop,
			client->resp.jmp_spawn_repeat > 0 ? "Stop repeating" : "Stop the pending spawn");
	JmpMenuRow_Set(ent, 5, PMENU_ALIGN_LEFT, -1, JmpMenuPickSpot, "Closest spawnpoint");
	JmpMenuRow_Set(ent, 6, PMENU_ALIGN_LEFT, 0, JmpMenuPickSpot, "Random spawnpoint");
	if (client->jmp_menu_pick < 1 && *cur < 0)
		*cur = client->jmp_menu_pick < 0 ? 5 : 6;

	// the page's spawnpoints, numbered as spawnp takes them
	row = JMP_MENU_FIRST;
	for (i = client->jmp_menu_top; i < count && row < JMP_MENU_FIRST + JMP_MENU_SPOTS; i++, row++) {
		// "12 Courtyard", or the bare number on a map without locations
		JmpSpotName(i + 1, name, sizeof(name), false);
		JmpMenuRow_Set(ent, row, PMENU_ALIGN_LEFT, i + 1, JmpMenuPickSpot, "%2d %s", i + 1,
			Q_isdigit(name[0]) ? strchr(name, ' ') + 1 : "");
		if (*cur < 0 && i + 1 == client->jmp_menu_pick)
			*cur = row;
	}

	row = JMP_MENU_FIRST + JMP_MENU_SPOTS;
	if (client->jmp_menu_top > 0)
		JmpMenuRow_Set(ent, row, PMENU_ALIGN_LEFT, -JMP_MENU_SPOTS, JmpMenuPage, "Previous page");
	if (client->jmp_menu_top + JMP_MENU_SPOTS < count)
		JmpMenuRow_Set(ent, row + 1, PMENU_ALIGN_LEFT, JMP_MENU_SPOTS, JmpMenuPage, "Next page");

	JmpMenuRow_Set(ent, JMP_MENU_ROWS - 1, PMENU_ALIGN_LEFT, 0, JmpMenuItems, "Back");
	if (*cur < 0)
		*cur = JMP_MENU_FIRST;
}

// the delay or the repeat: a list of values, the cursor on the current one
static void JmpMenuValues(edict_t *ent, int *cur, const float *values, int count, float now,
	void (*func)(edict_t *, pmenu_t *), const char *zero)
{
	char secs[32];
	int i, row = JMP_MENU_CHOICE;

	for (i = 0; i < count; i++, row++) {
		JmpSecsName(values[i], secs, sizeof(secs));
		if (values[i] <= 0)
			JmpMenuRow_Set(ent, row, PMENU_ALIGN_LEFT, 0, func, "%s", zero);
		else
			JmpMenuRow_Set(ent, row, PMENU_ALIGN_LEFT, (intptr_t)(values[i] * 10 + 0.5f), func,
				"%s%s", func == JmpMenuPickRepeat ? "every " : "", secs);
		if (fabsf(values[i] - now) < 0.05f)
			*cur = row;
	}

	JmpMenuRow_Set(ent, JMP_MENU_ROWS - 1, PMENU_ALIGN_LEFT, 0, JmpMenuBack, "Back");
	if (*cur < 0)
		*cur = JMP_MENU_CHOICE;
}

// build a step and show it, the cursor on row cur or the step's default
static void JmpMenuShow(edict_t *ent, int step, int cur)
{
	gclient_t *client = ent->client;
	char name[32], secs[32];
	int i;

	for (i = 0; i < JMP_MENU_ROWS; i++)
		JmpMenuRow_Set(ent, i, PMENU_ALIGN_LEFT, 0, NULL, NULL);
	JmpMenuRow_Set(ent, 1, PMENU_ALIGN_CENTER, 0, NULL, "%s", jmp_menu_line);

	client->jmp_menu_step = step;
	if (step == 0) {
		JmpMenuSpots(ent, &cur);
	} else if (step == 1) {
		JmpSpotName(client->jmp_menu_pick, name, sizeof(name), true);
		JmpMenuRow_Set(ent, 0, PMENU_ALIGN_CENTER, 0, NULL, "*%s", name);
		JmpMenuRow_Set(ent, 2, PMENU_ALIGN_CENTER, 0, NULL, "Wait before spawning");
		JmpMenuValues(ent, &cur, jmp_delays, JMP_COUNT(jmp_delays),
			client->resp.jmp_spawn_delay, JmpMenuPickDelay, "no wait");
	} else {
		JmpSpotName(client->jmp_menu_pick, name, sizeof(name), true);
		JmpMenuRow_Set(ent, 0, PMENU_ALIGN_CENTER, 0, NULL, "*%s", name);
		if (client->resp.jmp_spawn_delay > 0) {
			JmpSecsName(client->resp.jmp_spawn_delay, secs, sizeof(secs));
			JmpMenuRow_Set(ent, 2, PMENU_ALIGN_CENTER, 0, NULL, "after %s, repeat", secs);
		} else {
			JmpMenuRow_Set(ent, 2, PMENU_ALIGN_CENTER, 0, NULL, "now, repeat");
		}
		JmpMenuValues(ent, &cur, jmp_repeats, JMP_COUNT(jmp_repeats),
			client->resp.jmp_menu_repeat, JmpMenuPickRepeat, "once, no repeat");
	}

	if (client->layout == LAYOUT_MENU)
		PMenu_Close(ent);
	PMenu_Open(ent, client->jmp_menu, cur, JMP_MENU_ROWS);
}

/*
While the spawnpoint menu is up, every spawnpoint is marked in the world
(ghud 3D, through walls): a ring sized by distance and its number over
it, the one under the cursor - or picked - in yellow with its name. Only
the pick shows past the first step.
*/
static void JmpMarkersClear(edict_t *ent)
{
	gclient_t *client = ent->client;
	int i;

	for (i = 0; i < client->jmp_ghud_count; i++)
		Ghud_RemoveElement(ent, client->jmp_ghud[i]);
	client->jmp_ghud_count = 0;
}

/*
The marker: jmod's own pic (action/pics/jmod_spawn.png, white, the ghud
tints it) where the server has it - a local game's client then has it
too, and a remote one can download it - else a crosshair ring from the
pak, which every install has.
*/
const char *Jmp_MarkerPic(void)
{
	cvar_t *game_cvar = gi.cvar("game", "action", 0);
	char path[MAX_QPATH];
	FILE *f;

	Q_snprintf(path, sizeof(path), "%s/pics/jmod_spawn.png",
		*game_cvar->string ? game_cvar->string : GAMEVERSION);
	f = fopen(path, "rb");
	if (!f)
		return "ch14";
	fclose(f);
	return "jmod_spawn";
}

#define JMP_MARKER_NEAR	64	// no marker on a spot this close

static qboolean JmpNear(edict_t *ent, edict_t *spot)
{
	vec3_t d;

	VectorSubtract(spot->s.origin, ent->s.origin, d);
	return VectorLength(d) < JMP_MARKER_NEAR;
}

static void JmpMarkersUpdate(edict_t *ent)
{
	gclient_t *client = ent->client;
	edict_t *spots[JMP_SPOTS_MAX];
	pmenu_t *p;
	char text[64], loc[128];
	int i, count, hl = 0, key, ring;

	// a new map has cleared every slot, ours went with them
	if (client->jmp_ghud_count && client->jmp_ghud_made > level.framenum)
		client->jmp_ghud_count = 0;

	if (client->layout != LAYOUT_MENU || client->menu.entries != client->jmp_menu) {
		JmpMarkersClear(ent);
		return;
	}

	// which one to light: the cursor's, or the pick
	if (client->jmp_menu_step == 0) {
		if (client->menu.cur >= 0) {
			p = &client->jmp_menu[client->menu.cur];
			if (p->SelectFunc == JmpMenuPickSpot)
				hl = (int)(intptr_t)p->arg;
		}
	} else {
		hl = client->jmp_menu_pick;
	}
	if (hl < 0)
		hl = JmpClosestNumber(ent);

	// each spot: a ring at chest height and its number over it
	count = Jmp_Spots(spots, JMP_SPOTS_MAX);

	// the client scales a 3D image by 300 / distance, unclamped, so a
	// far ring shrinks to a dot: size it by distance here to keep it
	// about the same on screen, updated when it drifts past 10%
	for (i = 0; i < count && 2 * i < client->jmp_ghud_count; i++) {
		vec3_t d;
		int want, *had = &client->jmp_ghud_size[i];

		VectorSubtract(spots[i]->s.origin, ent->s.origin, d);
		want = (i + 1 == hl ? JMP_MARKER_HL : JMP_MARKER_PX) * max(VectorLength(d), 150) / 300;
		want = min(want, 30000);
		if (abs(want - *had) * 10 > *had) {
			*had = want;
			Ghud_SetSize(ent, client->jmp_ghud[2 * i], want, want);
		}
	}

	// redo them when the light or the spot you stand on changes
	key = (client->jmp_menu_step << 24) | ((hl & 0xff) << 16);
	for (i = 0; i < count; i++)
		if (JmpNear(ent, spots[i]))
			key |= (i + 1) & 0xffff;
	if (client->jmp_ghud_count && key == client->jmp_ghud_key)
		return;
	client->jmp_ghud_key = key;
	memset(client->jmp_ghud_size, 0, sizeof(client->jmp_ghud_size));	// resize on the next frame

	if (!client->jmp_ghud_count) {
		ring = gi.imageindex((char *)Jmp_MarkerPic());
		client->jmp_ghud_made = level.framenum;
		for (i = 0; i < count; i++) {
			vec_t *o = spots[i]->s.origin;
			int el = Ghud_NewElement(ent, GHT_IMG);

			Ghud_SetFlags(ent, el, GHF_3DPOS);
			Ghud_SetPosition3D(ent, el, o[0], o[1], o[2] + 8);
			Ghud_SetInt(ent, el, ring);
			Ghud_SetSize(ent, el, JMP_MARKER_PX, JMP_MARKER_PX);
			client->jmp_ghud[client->jmp_ghud_count++] = el;

			el = Ghud_NewElement(ent, GHT_TEXT);
			Ghud_SetFlags(ent, el, GHF_3DPOS);
			Ghud_SetPosition3D(ent, el, o[0], o[1], o[2] + 72);
			Ghud_SetTextFlags(ent, el, UI_CENTER | UI_DROPSHADOW);
			client->jmp_ghud[client->jmp_ghud_count++] = el;
		}
	}

	for (i = 0; i < count && 2 * i + 1 < client->jmp_ghud_count; i++) {
		int el_ring = client->jmp_ghud[2 * i], el = client->jmp_ghud[2 * i + 1];
		int flags = GHF_3DPOS;

		// none on the spot you stand on: it would hang right over your eye
		if (JmpNear(ent, spots[i]))
			flags |= GHF_HIDE;

		if (i + 1 == hl) {
			if (GetPlayerLocation(spots[i], loc))
				Q_snprintf(text, sizeof(text), "> %d %.40s <", i + 1, loc);
			else
				Q_snprintf(text, sizeof(text), "> %d <", i + 1);
			Ghud_SetColor(ent, el, 255, 220, 0, 255);
			Ghud_SetColor(ent, el_ring, 255, 220, 0, 255);
		} else {
			Q_snprintf(text, sizeof(text), "%d", i + 1);
			Ghud_SetColor(ent, el, 255, 255, 255, 255);
			Ghud_SetColor(ent, el_ring, 120, 255, 120, 255);
			if (client->jmp_menu_step)
				flags |= GHF_HIDE;
		}
		Ghud_SetText(ent, el, text);
		Ghud_SetFlags(ent, el, flags);
		Ghud_SetFlags(ent, el_ring, flags);
	}
}

// from the jmod item menu, and "jmod spawns"
void Jmp_OpenSpawnMenu(edict_t *ent, pmenu_t *p)
{
	JmpMenuShow(ent, 0, -1);
}

void Cmd_Clear_f(edict_t *ent)
{
	ent->client->resp.jmp_highspeed = 0;
	ent->client->resp.jmp_falldmglast = 0;
	gi.cprintf(ent, PRINT_HIGH, "Statistics cleared\n");
}

void Cmd_Reset_f (edict_t *ent)
{
	VectorClear(ent->client->resp.jmp_teleport_origin);
	VectorClear(ent->client->resp.jmp_teleport_v_angle);
	gi.cprintf(ent, PRINT_HIGH, "Teleport location removed\n");
}

void Cmd_Store_f (edict_t *ent)
{
	if (ent->client->pers.spectator)
	{
		gi.cprintf(ent, PRINT_HIGH, "This command cannot be used by spectators\n");
		return;
	}

	VectorCopy (ent->s.origin, ent->client->resp.jmp_teleport_origin);
	VectorCopy(ent->client->v_angle, ent->client->resp.jmp_teleport_v_angle);

	if (ent->client->ps.pmove.pm_flags & PMF_DUCKED)
	{
		ent->client->resp.jmp_teleport_ducked = true;
	}

	gi.cprintf(ent, PRINT_HIGH, "Location stored\n");
}

void Cmd_Recall_f (edict_t *ent)
{
	int i;

	if (ent->deadflag || ent->client->pers.spectator)
	{
		gi.cprintf(ent, PRINT_HIGH, "This command cannot be used by spectators or dead players\n");
		return;
	}

	if(VectorLength(ent->client->resp.jmp_teleport_origin) == 0)
	{
		gi.cprintf(ent, PRINT_HIGH, "You must first \"store\" a location to teleport to\n");
		return;
	}

	ent->client->jumping = 0;

	ent->movetype = MOVETYPE_NOCLIP;

	/* teleport effect */
	gi.WriteByte (svc_temp_entity);
	gi.WriteByte (TE_TELEPORT_EFFECT);
	gi.WritePosition (ent->s.origin);
	gi.multicast (ent->s.origin, MULTICAST_PVS);

	gi.unlinkentity (ent);

	VectorCopy (ent->client->resp.jmp_teleport_origin, ent->s.origin);
	VectorCopy (ent->client->resp.jmp_teleport_origin, ent->s.old_origin);

	VectorClear (ent->velocity);

	ent->client->ps.pmove.pm_time = 160>>3;

	ent->s.event = EV_PLAYER_TELEPORT;

	VectorClear(ent->s.angles);
	VectorClear(ent->client->ps.viewangles);
	VectorClear(ent->client->ps.kick_angles);
	VectorClear(ent->client->v_angle);
	VectorClear(ent->client->ps.pmove.delta_angles);
	VectorClear(ent->client->kick_angles);
	ent->client->fall_time = 0;
	ent->client->fall_value = 0;

	VectorCopy(ent->client->resp.jmp_teleport_v_angle, ent->client->v_angle);
	VectorCopy(ent->client->v_angle, ent->client->ps.viewangles);

	for (i=0;i<2;i++)
		ent->client->ps.pmove.delta_angles[i] = ANGLE2SHORT(ent->client->v_angle[i] - ent->client->resp.cmd_angles[i]);

	if (ent->client->resp.jmp_teleport_ducked)
		ent->client->ps.pmove.pm_flags = PMF_DUCKED;

	gi.linkentity (ent);

	/* teleport effect */
	gi.WriteByte (svc_temp_entity);
	gi.WriteByte (TE_TELEPORT_EFFECT);
	gi.WritePosition (ent->s.origin);
	gi.multicast (ent->s.origin, MULTICAST_PVS);

	ent->movetype = MOVETYPE_WALK;
}

void PMLaserSight(edict_t *self)
{
	vec3_t  start,forward,right,end;
	edict_t *lasersight = self->client->lasersight;

	if (lasersight) {  // laser is on
		G_FreeEdict(lasersight);
		self->client->lasersight = NULL;
		return;
	} else {
		AngleVectors (self->client->v_angle, forward, right, NULL);

		VectorSet(end,100 , 0, 0);
		G_ProjectSource (self->s.origin, end, forward, right, start);

		lasersight = G_Spawn();
		self->client->lasersight = lasersight;
		lasersight->owner = self;
		lasersight->movetype = MOVETYPE_NOCLIP;
		lasersight->solid = SOLID_NOT;
		lasersight->classname = "lasersight";
		lasersight->s.modelindex = level.model_lsight;
		lasersight->s.renderfx = RF_TRANSLUCENT;
		lasersight->ideal_yaw = self->viewheight;
		lasersight->count = 0;
		lasersight->think = LaserSightThink;
		lasersight->nextthink = level.framenum + 1;
		LaserSightThink( lasersight );
		VectorCopy( lasersight->s.origin, lasersight->s.old_origin );
		VectorCopy( lasersight->s.origin, lasersight->old_origin );
	}
}

void PMStealthSlippers(edict_t *self)
{
	// Removes stealth slippers
	if (self->client->inventory[ITEM_INDEX(GET_ITEM(SLIP_NUM))]) {
		self->client->inventory[ITEM_INDEX(GET_ITEM(SLIP_NUM))]--;
	} else {
	// Adds stealth slippers
		AddItem(self, GET_ITEM(SLIP_NUM));
	}
}

//PaTMaN - New Toggle Command
void Cmd_Toggle_f(edict_t *ent, char *toggle)
{
	//char	*s;
	char	ACT  [2][12] = { "Deactivated\0","Activated\0" };
	//char	ENA  [2][9]  = { "Disabled\0","Enabled\0" };
	int		spec, val=0;

	//s = strtok(gi.args()," ");

	//if ((gi.argc() == 1) && (s == NULL))
	// if (toggle)
	// {
	// 		gi.cprintf(ent,PRINT_HIGH,"Options to toggle: laser, vest, slippers, silencer, helmet, ir\n");
	// 		return;
	// }

	// if (!Q_stricmp(s,"togglecode"))
	// 	s = strtok(NULL," ");
	// else
	// 	s = strtok(gi.args()," ");

	spec = ent->client->pers.spectator;

	if ( Q_stricmp(toggle, "laser") == 0 )
	{
		if (spec) goto spec;
		if (ent->client->resp.toggles & TG_LASER) {
			ent->client->resp.toggles -= TG_LASER;
			PMLaserSight(ent); 
		} else { 
			ent->client->resp.toggles += TG_LASER;
			val=1;
			PMLaserSight(ent); 
		}
		gi.cprintf(ent, PRINT_HIGH, "Laser %s\n",ACT[val]);
	}
	else if ( Q_stricmp(toggle, "slippers") == 0 )
	{
		if (spec) goto spec;
		if (ent->client->resp.toggles & TG_SLIPPERS) {
			ent->client->resp.toggles -= TG_SLIPPERS;
			PMStealthSlippers(ent);
			//ent->client->inventory[ITEM_INDEX(GET_ITEM(SLIP_NUM))]--;
		}
		else { 
			ent->client->resp.toggles += TG_SLIPPERS;
			val=1;
			PMStealthSlippers(ent);
			//AddItem(ent, GET_ITEM(SLIP_NUM));
		}
		gi.cprintf(ent, PRINT_HIGH, "Slippers %s\n",ACT[val]);
	}
	// else if ( Q_stricmp(s, "kickable") == 0 )
	// {
	// 	if (spec) goto spec;
	// 	if (ent->client->resp.toggles & TG_KICKABLE)
	// 	{
	// 		ent->client->resp.toggles -= TG_KICKABLE;
	// 		gi.cprintf(ent,PRINT_HIGH, "You are now NOT KICKABLE\n");
	// 	}
	// 	else
	// 	{
	// 		ent->client->resp.toggles += TG_KICKABLE;
	// 		gi.cprintf(ent,PRINT_HIGH, "You are now KICKABLE\n");
	// 	}
	// }
	else
		gi.cprintf(ent, PRINT_HIGH, "\"%s\" isn't a valid toggle option\n",toggle);
	return;
spec:
	gi.cprintf(ent,PRINT_HIGH,"This command cannot be used by spectators\n");
}
