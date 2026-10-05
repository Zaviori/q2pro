#include "g_local.h"
#include <dirent.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif
#include "common/fuzzy.h"

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

	/*
	"jmod markers": the markers are up with no menu. An empty layout is
	kept up with them - sent again after a menu or the scores have had
	the client's - for what the client only does under one: Escape sent
	on as "putaway", which puts the markers away, and shift told of.
	*/
	if (ent->client->layout != LAYOUT_NONE) {
		ent->client->jmp_blank = false;
	} else if (ent->client->resp.jmp_markers) {
		if (!ent->client->jmp_blank) {
			gi.WriteByte(svc_layout);
			gi.WriteString("");
			gi.unicast(ent, true);
			ent->client->jmp_blank = true;
		}
		ent->client->ps.stats[STAT_LAYOUTS] |= 1;
	}
}

void Jmp_EquipClient(edict_t *ent)
{
	memset(ent->client->inventory, 0, sizeof(ent->client->inventory));
	ent->client->weapon = 0;

	// make client non-solid
	ent->solid = SOLID_TRIGGER;
	AddToTransparentList(ent);
}

static qboolean Jmp_Stop(edict_t *ent);
static void Cmd_JumpRec_f(edict_t *ent);
static void Cmd_JumpSave_f(edict_t *ent);
static void Cmd_JumpPlay_f(edict_t *ent);
static void Cmd_JumpDelete_f(edict_t *ent);
static void Cmd_JumpMarkers_f(edict_t *ent);
static void Cmd_JumpStart_f(edict_t *ent);
static void JmpAnswerText(edict_t *ent, const char *text);

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
		gi.cprintf(ent, PRINT_HIGH, " jmod markers - the spawnpoint markers without the list, to click on; again or Escape puts them away\n");
		gi.cprintf(ent, PRINT_HIGH, " jmod rec - record a jump, again to end it; a teleport restarts the take\n");
		gi.cprintf(ent, PRINT_HIGH, " jmod save [[collection/]name] [description] - keep the last take for everybody on this map; asks for what is left out\n");
		gi.cprintf(ent, PRINT_HIGH, " jmod play [[collection/]name] [3rd] - watch a stored jump, or your last take, from the player's view or in third person\n");
		gi.cprintf(ent, PRINT_HIGH, " jmod start <[collection/]name> - spawn where a stored jump starts, facing its way\n");
		gi.cprintf(ent, PRINT_HIGH, " jmod delete <[collection/]name> - remove a stored jump of yours; \"collection/\" the collection itself\n");
		gi.cprintf(ent, PRINT_HIGH, " jmod jumps - the stored jumps of this map, from a menu\n");

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
		// a playback or a recording first, then the pending spawn
		if (!Jmp_Stop(ent))
			Cmd_SpawnCancel_f(ent);
		return;
	}
	else if(Q_stricmp(cmd, "rec") == 0 || Q_stricmp(cmd, "record") == 0)
	{
		Cmd_JumpRec_f(ent);
		return;
	}
	else if(Q_stricmp(cmd, "save") == 0)
	{
		Cmd_JumpSave_f(ent);
		return;
	}
	else if(Q_stricmp(cmd, "play") == 0)
	{
		Cmd_JumpPlay_f(ent);
		return;
	}
	else if(Q_stricmp(cmd, "start") == 0)
	{
		Cmd_JumpStart_f(ent);
		return;
	}
	else if(Q_stricmp(cmd, "delete") == 0)
	{
		Cmd_JumpDelete_f(ent);
		return;
	}
	else if(Q_stricmp(cmd, "answer") == 0)
	{
		// from the client's messageprompt, not for typing
		JmpAnswerText(ent, gi.argc() >= 3 ? gi.argv(2) : NULL);
		return;
	}
	else if(Q_stricmp(cmd, "jumps") == 0)
	{
		Jmp_OpenJumpMenu(ent, NULL);
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
	else if(Q_stricmp(cmd, "markers") == 0)
	{
		Cmd_JumpMarkers_f(ent);
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

	Jmp_RecordRestart(ent);

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

			Jmp_RecordRestart(ent);
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
#define JMP_SPOT_STORED	-2	// jmp_spawn_spot: the spot "jmod store" keeps

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

// the spot "jmod store" keeps, without a word
static void JmpStoreHere(edict_t *ent)
{
	gclient_t *client = ent->client;

	VectorCopy(ent->s.origin, client->resp.jmp_teleport_origin);
	VectorCopy(client->v_angle, client->resp.jmp_teleport_v_angle);
	client->resp.jmp_teleport_ducked = (client->ps.pmove.pm_flags & PMF_DUCKED) != 0;
}

static qboolean JmpIsRecording(edict_t *ent);

static void JmpSpawnNow(edict_t *ent)
{
	gclient_t *client = ent->client;
	edict_t *spot;

	// a stored jump's start: it becomes the stored spot, which is next
	if (client->resp.jmp_spawn_spot >= JMP_SPOT_JUMPS) {
		int k = client->resp.jmp_spawn_spot - JMP_SPOT_JUMPS;
		jmp_start_t *start = &client->resp.jmp_starts[k];

		if (k >= client->resp.jmp_starts_count) {
			gi.cprintf(ent, PRINT_HIGH, "That jump's start is not on the list any more\n");
			client->resp.jmp_spawn_frame = 0;
			client->resp.jmp_spawn_repeat = 0;
			return;
		}
		VectorCopy(start->origin, client->resp.jmp_teleport_origin);
		VectorSet(client->resp.jmp_teleport_v_angle, start->pitch, start->yaw, 0);
		client->resp.jmp_teleport_ducked = start->ducked;
	}

	// the stored spot: a recall. No lights, camera, action - that is
	// for a round's start at a spawnpoint, not for a place of one's own
	if (client->resp.jmp_spawn_spot == JMP_SPOT_STORED || client->resp.jmp_spawn_spot >= JMP_SPOT_JUMPS) {
		Cmd_Recall_f(ent);
		if (client->resp.jmp_spawn_repeat > 0)
			JmpSpawnAt(ent, client->resp.jmp_spawn_repeat);
		return;
	}

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

	// a take begins here: recall brings him back for another go
	if (JmpIsRecording(ent))
		JmpStoreHere(ent);

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
static void JmpJumpsFrame(edict_t *ent);

// called every frame for each client: counts a pending spawn down, and
// keeps the spawnpoint labels
void Jmp_RunSpawn(edict_t *ent)
{
	gclient_t *client = ent->client;
	int left;

	JmpMarkersUpdate(ent);
	JmpJumpsFrame(ent);

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
#define JMP_MENU_JUMPS	10	// the steps of the jumps menu, in the same rows: the list,
#define JMP_MENU_VIEW	11	// then how to watch the pick
#define JMP_MENU_TAKE	12	// and what to do with a take, running or not yet saved
#define JMP_MENU_PACKS	13	// the map's collections, before the list
#define JMP_MENU_SAVETO	14	// which collection a take is saved to
#define JMP_MENU_DELETE	15	// "are you sure"
#define JMP_MENU_PACKDEL	16	// the same of a whole collection,
#define JMP_MENU_PACKDEL2	17	// and again when it has jumps in it
#define JMP_REC_COUNTDOWN	3	// seconds before a recording's teleport to its spawnpoint

static const float jmp_delays[] = { 0, 1, 2, 3, 5, 10 };
static const float jmp_repeats[] = { 0, 3, 5, 8, 10, 15, 20, 30 };

#define JMP_COUNT(a)	(int)(sizeof(a) / sizeof((a)[0]))

static const char jmp_menu_line[] = "\x9D\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9E\x9F";

static void JmpMenuShow(edict_t *ent, int step, int cur);
static void JmpJumpsShow(edict_t *ent, int step, int cur);

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

static int JmpSpotPacks(char packs[][JMP_NAME_MAX], int *counts, int max);
static qboolean JmpStartsOn(edict_t *ent, const char *pack);
static void JmpStartsToggle(edict_t *ent, const char *pack);
static edict_t *jmp_name_ent;	// whose starts JmpSpotName names

static const char *JmpStartName(int number)
{
	int k = number - JMP_SPOT_JUMPS;

	if (!jmp_name_ent || k < 0 || k >= jmp_name_ent->client->resp.jmp_starts_count)
		return "A jump's start";
	return jmp_name_ent->client->resp.jmp_starts[k].name;
}

// the spawnpoint's name: its number and location, or closest/random
static void JmpSpotName(int number, char *buf, size_t size, qboolean shortname)
{
	edict_t *spot;
	char loc[128];

	if (number >= JMP_SPOT_JUMPS) {
		Q_strncpyz(buf, JmpStartName(number), size);
		return;
	}
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
	gclient_t *client = ent->client;

	client->jmp_menu_pick = (int)(intptr_t)p->arg;

	// recording from it: there after a countdown, the take starting on
	// arrival - this spawn takes the place of the one Cmd_JumpRec_f set up
	if (client->jmp_menu_rec) {
		PMenu_Close(ent);
		Cmd_JumpRec_f(ent);
		JmpSpawn(ent, client->jmp_menu_pick, JMP_REC_COUNTDOWN, 0);
		return;
	}
	JmpMenuShow(ent, 1, -1);
}

// recording from where he stands: Cmd_JumpRec_f does just that
static void JmpMenuRecHere(edict_t *ent, pmenu_t *p)
{
	PMenu_Close(ent);
	Cmd_JumpRec_f(ent);
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

// a collection's row: its jumps' starts on the list and marked, or off again
static void JmpMenuPickPack(edict_t *ent, pmenu_t *p)
{
	char packs[JMP_PACKS_MAX][JMP_NAME_MAX];
	int counts[JMP_PACKS_MAX];
	int i = (int)(intptr_t)p->arg, n = JmpSpotPacks(packs, counts, JMP_PACKS_MAX);

	if (i >= 0 && i < n)
		JmpStartsToggle(ent, packs[i]);
	JmpMenuShow(ent, 0, p - ent->client->jmp_menu);
}

/*
The list: the map's spawnpoints, then the map's collections of stored
jumps - a row each, which Enter turns on and off - and under one that
is on, the places its jumps start from, to spawn at as at a spawnpoint.
*/
static void JmpMenuSpots(edict_t *ent, int *cur)
{
	gclient_t *client = ent->client;
	edict_t *spots[JMP_SPOTS_MAX];
	char packs[JMP_PACKS_MAX][JMP_NAME_MAX], name[32];
	int counts[JMP_PACKS_MAX];
	int kind[JMP_SPOTS_MAX + JMP_PACKS_MAX + JMP_STARTS_MAX], arg[JMP_SPOTS_MAX + JMP_PACKS_MAX + JMP_STARTS_MAX];
	int i, k, n = 0, count = Jmp_Spots(spots, JMP_SPOTS_MAX), npacks, row;

	// what is listed: 0 a spawnpoint, 1 a collection, 2 a jump's start
	for (i = 0; i < count; i++, n++) {
		kind[n] = 0;
		arg[n] = i + 1;
	}
	npacks = JmpSpotPacks(packs, counts, JMP_PACKS_MAX);
	for (i = 0; i < npacks; i++) {
		kind[n] = 1;
		arg[n++] = i;
		if (!JmpStartsOn(ent, packs[i]))
			continue;
		for (k = 0; k < client->resp.jmp_starts_count; k++) {
			if (strcmp(client->resp.jmp_starts[k].pack, packs[i]))
				continue;
			kind[n] = 2;
			arg[n++] = JMP_SPOT_JUMPS + k;
		}
	}

	if (client->jmp_menu_top >= n)
		client->jmp_menu_top = max(n - 1, 0) / JMP_MENU_SPOTS * JMP_MENU_SPOTS;

	JmpMenuRow_Set(ent, 0, PMENU_ALIGN_CENTER, 0, NULL,
		client->jmp_menu_rec ? "*Record a jump from" : "*Pick a spawnpoint (%d)", count);
	if (client->jmp_menu_rec) {
		JmpMenuRow_Set(ent, 3, PMENU_ALIGN_LEFT, 0, JmpMenuRecHere, "Start here");
		if (*cur < 0)
			*cur = 3;
	} else if (client->resp.jmp_spawn_frame)
		JmpMenuRow_Set(ent, 3, PMENU_ALIGN_LEFT, 0, JmpMenuStop,
			client->resp.jmp_spawn_repeat > 0 ? "Stop repeating" : "Stop the pending spawn");
	// a jump is recorded from a spot picked, not from whichever
	if (!client->jmp_menu_rec) {
		JmpMenuRow_Set(ent, 5, PMENU_ALIGN_LEFT, -1, JmpMenuPickSpot, "Closest spawnpoint");
		JmpMenuRow_Set(ent, 6, PMENU_ALIGN_LEFT, 0, JmpMenuPickSpot, "Random spawnpoint");
		if (client->jmp_menu_pick < 1 && *cur < 0)
			*cur = client->jmp_menu_pick < 0 ? 5 : 6;
	}

	row = JMP_MENU_FIRST;
	for (i = client->jmp_menu_top; i < n && row < JMP_MENU_FIRST + JMP_MENU_SPOTS; i++, row++) {
		if (kind[i] == 0) {
			// "12 Courtyard", or the bare number on a map without locations
			JmpSpotName(arg[i], name, sizeof(name), false);
			JmpMenuRow_Set(ent, row, PMENU_ALIGN_LEFT, arg[i], JmpMenuPickSpot, "%2d %s", arg[i],
				Q_isdigit(name[0]) ? strchr(name, ' ') + 1 : "");
		} else if (kind[i] == 1) {
			const char *pack = packs[arg[i]];

			JmpMenuRow_Set(ent, row, PMENU_ALIGN_LEFT, arg[i], JmpMenuPickPack, "%s %.19s (%d)",
				JmpStartsOn(ent, pack) ? "[x]" : "[ ]", pack[0] ? pack : "Jumps in no collection",
				counts[arg[i]]);
			continue;
		} else {
			JmpMenuRow_Set(ent, row, PMENU_ALIGN_LEFT, arg[i], JmpMenuPickSpot, "    %.26s",
				client->resp.jmp_starts[arg[i] - JMP_SPOT_JUMPS].name);
		}
		if (*cur < 0 && arg[i] == client->jmp_menu_pick)
			*cur = row;
	}

	row = JMP_MENU_FIRST + JMP_MENU_SPOTS;
	if (client->jmp_menu_top > 0)
		JmpMenuRow_Set(ent, row, PMENU_ALIGN_LEFT, -JMP_MENU_SPOTS, JmpMenuPage, "Previous page");
	if (client->jmp_menu_top + JMP_MENU_SPOTS < n)
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

	jmp_name_ent = ent;

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

	for (i = 0; i < client->resp.jmp_ghud_count; i++)
		Ghud_RemoveElement(ent, client->resp.jmp_ghud[i]);
	client->resp.jmp_ghud_count = 0;
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

// what is marked: the map's spawnpoints, then the listed jumps' starts
typedef struct {
	vec3_t	origin;
	edict_t	*spot;		// a spawnpoint's, NULL for a start
	const char	*name;	// a start's jump
} jmp_point_t;

static int JmpPoints(edict_t *ent, jmp_point_t *pts, int *map)
{
	gclient_t *client = ent->client;
	edict_t *spots[JMP_SPOTS_MAX];
	int i, n = 0;

	*map = Jmp_Spots(spots, JMP_SPOTS_MAX);
	for (i = 0; i < *map; i++, n++) {
		VectorCopy(spots[i]->s.origin, pts[n].origin);
		pts[n].spot = spots[i];
		pts[n].name = NULL;
	}
	for (i = 0; i < client->resp.jmp_starts_count; i++, n++) {
		VectorCopy(client->resp.jmp_starts[i].origin, pts[n].origin);
		pts[n].spot = NULL;
		pts[n].name = client->resp.jmp_starts[i].name;
	}
	return n;
}

#define JMP_AIM_TAN		0.045f	// a marker's radius, as the tangent of its angle on screen
#define JMP_CLICK_MS	300		// a second click within this is a double one

// the marker the view points into, as a spawn's number; 0 none
static int JmpAim(edict_t *ent, jmp_point_t *pts, int count, int map)
{
	vec3_t eye, forward, d, off;
	float along, ratio, best = 1;
	int i, aim = 0;

	VectorCopy(ent->s.origin, eye);
	eye[2] += ent->viewheight;
	AngleVectors(ent->client->v_angle, forward, NULL, NULL);

	for (i = 0; i < count; i++) {
		// the ring's centre; it is drawn the same size from 150 units out
		VectorSubtract(pts[i].origin, eye, d);
		d[2] += 8;
		along = DotProduct(d, forward);
		if (along < JMP_MARKER_NEAR)
			continue;
		VectorMA(d, -along, forward, off);
		ratio = VectorLength(off) / (JMP_AIM_TAN * max(along, 150));
		if (ratio < best) {
			best = ratio;
			aim = i < map ? i + 1 : JMP_SPOT_JUMPS + i - map;
		}
	}
	return aim;
}

// what a click on the marker does, under the crosshair while one is aimed at
static void JmpTip(edict_t *ent, qboolean on)
{
	gclient_t *client = ent->client;
	int i;

	if (on == client->resp.jmp_tip_on)
		return;
	client->resp.jmp_tip_on = on;

	for (i = 0; i < 2; i++) {
		if (!on) {
			Ghud_RemoveElement(ent, client->resp.jmp_tip[i]);
			continue;
		}
		client->resp.jmp_tip[i] = Ghud_NewElement(ent, GHT_TEXT);
		Ghud_SetAnchor(ent, client->resp.jmp_tip[i], 0.5f, 0.5f);
		Ghud_SetPosition(ent, client->resp.jmp_tip[i], 0, 22 + 10 * i);
		Ghud_SetTextFlags(ent, client->resp.jmp_tip[i], UI_CENTER);
		Ghud_SetColor(ent, client->resp.jmp_tip[i], 255, 255, 255, i ? 170 : 255);
	}
	if (!on)
		return;
	if (client->jmp_menu_rec && client->layout == LAYOUT_MENU) {
		Ghud_SetText(ent, client->resp.jmp_tip[0], "click: record a jump from here");
		Ghud_SetText(ent, client->resp.jmp_tip[1], "");
	} else {
		Ghud_SetText(ent, client->resp.jmp_tip[0], "click: spawn here   double click: delay, repeat");
		Ghud_SetText(ent, client->resp.jmp_tip[1], "shift + click: record a jump from here");
	}
}

/*
With the spawnpoint list up - or the markers alone, "jmod markers" -
the markers are buttons: fire on the one
aimed at spawns there at once, a second click in time opens its delay
and repeat instead, and with shift down - or when the list is asking
where to record from - a take is started with the spawn. Called with
every command.
*/
void Jmp_MenuFire(edict_t *ent, usercmd_t *ucmd)
{
	gclient_t *client = ent->client;
	qboolean fire = (ucmd->buttons & BUTTON_ATTACK) != 0;
	qboolean pressed = fire && !client->jmp_fire_held;
	qboolean bare = client->resp.jmp_markers && client->layout != LAYOUT_MENU
		&& !(ent->svflags & SVF_NOCLIENT);	// not over a jump being watched
	qboolean list = bare || (client->layout == LAYOUT_MENU && client->menu.entries == client->jmp_menu
		&& client->jmp_menu_step == 0);
	int spot = client->jmp_menu_aim;

	client->jmp_fire_held = fire;

	// a click waiting to see if it is a double one
	if (client->jmp_click_ms > 0) {
		client->jmp_click_ms -= ucmd->msec;
		if (!list) {
			client->jmp_click_ms = 0;
		} else if (pressed && spot == client->jmp_click_spot) {
			client->jmp_click_ms = 0;
			client->jmp_menu_pick = spot;
			client->jmp_menu_rec = false;
			JmpMenuShow(ent, 1, -1);
			return;
		} else if (pressed) {
			client->jmp_click_ms = 0;	// on another marker: a click of its own
		} else if (client->jmp_click_ms <= 0) {
			client->jmp_click_ms = 0;
			client->jmp_menu_pick = client->jmp_click_spot;
			PMenu_Close(ent);
			JmpSpawn(ent, client->jmp_click_spot, 0, 0);
			return;
		}
	}

	if (!pressed || !list || !spot)
		return;
	client->latched_buttons &= ~BUTTON_ATTACK;

	if ((client->jmp_menu_rec && !bare) || ucmd->impulse == IMPULSE_SHIFT) {
		client->jmp_menu_pick = spot;
		PMenu_Close(ent);
		if (!JmpIsRecording(ent))
			Cmd_JumpRec_f(ent);
		JmpSpawn(ent, spot, JMP_REC_COUNTDOWN, 0);
		return;
	}
	client->jmp_click_spot = spot;
	client->jmp_click_ms = JMP_CLICK_MS;
}

static qboolean JmpNear(edict_t *ent, const vec3_t origin)
{
	vec3_t d;

	VectorSubtract(origin, ent->s.origin, d);
	return VectorLength(d) < JMP_MARKER_NEAR;
}

static void JmpMarkersUpdate(edict_t *ent)
{
	gclient_t *client = ent->client;
	jmp_point_t pts[JMP_POINTS_MAX];
	pmenu_t *p;
	char text[64], loc[128];
	int i, count, map, hl = 0, key, ring, step = client->jmp_menu_step;
	qboolean bare = client->resp.jmp_markers && client->layout != LAYOUT_MENU
		&& !(ent->svflags & SVF_NOCLIENT);	// not over a jump being watched

	// a new map has cleared every slot, ours went with them
	if (client->resp.jmp_ghud_count && client->resp.jmp_ghud_made > level.framenum) {
		client->resp.jmp_ghud_count = 0;
		client->resp.jmp_tip_on = false;
	}

	// up with the spawnpoint menu - the jumps menu shares its rows, and
	// marks nothing - or on their own, as the list would have them
	if (bare) {
		step = 0;
	} else if (client->layout != LAYOUT_MENU || client->menu.entries != client->jmp_menu
		|| client->jmp_menu_step >= JMP_MENU_JUMPS) {
		client->jmp_menu_aim = 0;
		JmpTip(ent, false);
		JmpMarkersClear(ent);
		return;
	}

	count = JmpPoints(ent, pts, &map);

	// the marker aimed at, on the list: lit over the cursor's, and a button
	client->jmp_menu_aim = step == 0 ? JmpAim(ent, pts, count, map) : 0;
	JmpTip(ent, client->jmp_menu_aim != 0);

	// which one to light: the cursor's, or the pick - a start's number
	// is past the spawnpoints'
	if (client->jmp_menu_aim) {
		hl = client->jmp_menu_aim;
	} else if (bare) {
		hl = 0;
	} else if (step == 0) {
		if (client->menu.cur >= 0) {
			p = &client->jmp_menu[client->menu.cur];
			if (p->SelectFunc == JmpMenuPickSpot)
				hl = (int)(intptr_t)p->arg;
		}
	} else {
		hl = client->jmp_menu_pick;
	}
	if (hl >= JMP_SPOT_JUMPS)
		hl = map + 1 + hl - JMP_SPOT_JUMPS;
	else if (hl < 0)
		hl = JmpClosestNumber(ent);

	// a collection turned on or off: other points, made anew
	if (client->resp.jmp_ghud_count && client->resp.jmp_ghud_count != 2 * count)
		JmpMarkersClear(ent);

	// the client scales a 3D image by 300 / distance, unclamped, so a
	// far ring shrinks to a dot: size it by distance here to keep it
	// about the same on screen, updated when it drifts past 10%
	for (i = 0; i < count && 2 * i < client->resp.jmp_ghud_count; i++) {
		vec3_t d;
		int want, *had = &client->resp.jmp_ghud_size[i];

		VectorSubtract(pts[i].origin, ent->s.origin, d);
		want = (i + 1 == hl ? JMP_MARKER_HL : JMP_MARKER_PX) * max(VectorLength(d), 150) / 300;
		want = min(want, 30000);
		if (abs(want - *had) * 10 > *had) {
			*had = want;
			Ghud_SetSize(ent, client->resp.jmp_ghud[2 * i], want, want);
		}
	}

	// redo them when the light or the spot you stand on changes
	key = (step << 24) | ((hl & 0xff) << 16);
	for (i = 0; i < count; i++)
		if (JmpNear(ent, pts[i].origin))
			key |= (i + 1) & 0xffff;
	if (client->resp.jmp_ghud_count && key == client->resp.jmp_ghud_key)
		return;
	client->resp.jmp_ghud_key = key;
	memset(client->resp.jmp_ghud_size, 0, sizeof(client->resp.jmp_ghud_size));	// resize on the next frame

	if (!client->resp.jmp_ghud_count) {
		ring = gi.imageindex((char *)Jmp_MarkerPic());
		client->resp.jmp_ghud_made = level.framenum;
		for (i = 0; i < count; i++) {
			vec_t *o = pts[i].origin;
			int el = Ghud_NewElement(ent, GHT_IMG);

			Ghud_SetFlags(ent, el, GHF_3DPOS);
			Ghud_SetPosition3D(ent, el, o[0], o[1], o[2] + 8);
			Ghud_SetInt(ent, el, ring);
			Ghud_SetSize(ent, el, JMP_MARKER_PX, JMP_MARKER_PX);
			client->resp.jmp_ghud[client->resp.jmp_ghud_count++] = el;

			el = Ghud_NewElement(ent, GHT_TEXT);
			Ghud_SetFlags(ent, el, GHF_3DPOS);
			Ghud_SetPosition3D(ent, el, o[0], o[1], o[2] + 72);
			Ghud_SetTextFlags(ent, el, UI_CENTER);	// the client shifts a 3D element by its "size": keep it small
			client->resp.jmp_ghud[client->resp.jmp_ghud_count++] = el;
		}
	}

	for (i = 0; i < count && 2 * i + 1 < client->resp.jmp_ghud_count; i++) {
		int el_ring = client->resp.jmp_ghud[2 * i], el = client->resp.jmp_ghud[2 * i + 1];
		int flags = GHF_3DPOS;

		// none on the spot you stand on: it would hang right over your eye
		if (JmpNear(ent, pts[i].origin))
			flags |= GHF_HIDE;

		// a spawnpoint by its number, a jump's start by the jump's name, in blue
		if (i + 1 == hl) {
			if (pts[i].name)
				Q_snprintf(text, sizeof(text), "> %s <", pts[i].name);
			else if (GetPlayerLocation(pts[i].spot, loc))
				Q_snprintf(text, sizeof(text), "> %d %.40s <", i + 1, loc);
			else
				Q_snprintf(text, sizeof(text), "> %d <", i + 1);
			Ghud_SetColor(ent, el, 255, 220, 0, 255);
			Ghud_SetColor(ent, el_ring, 255, 220, 0, 255);
		} else {
			if (pts[i].name) {
				Q_snprintf(text, sizeof(text), "%s", pts[i].name);
				Ghud_SetColor(ent, el, 150, 210, 255, 255);
				Ghud_SetColor(ent, el_ring, 110, 190, 255, 255);
			} else {
				Q_snprintf(text, sizeof(text), "%d", i + 1);
				Ghud_SetColor(ent, el, 255, 255, 255, 255);
				Ghud_SetColor(ent, el_ring, 120, 255, 120, 255);
			}
			if (step)
				flags |= GHF_HIDE;
		}
		Ghud_SetText(ent, el, text);
		Ghud_SetFlags(ent, el, flags);
		Ghud_SetFlags(ent, el_ring, flags);
	}
}

// the game has cleared every HUD element of the client's: the markers
// and the tooltip went with them, and are made again where still wanted
void Jmp_HudCleared(edict_t *ent)
{
	ent->client->resp.jmp_ghud_count = 0;
	ent->client->resp.jmp_tip_on = false;
}

// jmod markers - the markers without the list, for a bind
static void Cmd_JumpMarkers_f(edict_t *ent)
{
	gclient_t *client = ent->client;

	client->resp.jmp_markers = !client->resp.jmp_markers;
	if (client->resp.jmp_markers)
		gi.cprintf(ent, PRINT_HIGH, "Markers up: fire on one to spawn there. Escape puts them away\n");
}

// Escape with no menu up: true if it put the markers away
qboolean Jmp_MarkersEscape(edict_t *ent)
{
	if (!ent->client->resp.jmp_markers)
		return false;
	ent->client->resp.jmp_markers = false;
	return true;
}

// from the jmod item menu, and "jmod spawns"
void Jmp_OpenSpawnMenu(edict_t *ent, pmenu_t *p)
{
	ent->client->jmp_menu_rec = false;
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

	Jmp_RecordRestart(ent);
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

/*
Recorded jumps. "jmod rec" samples the player as his commands arrive -
where he is, where he looks, his animation frame, the movement keys held
and his speed - and "jmod save" writes the take, under a name and with
a line describing it, to <game>/jumps/<map>/<name>.jmp, a text file:
everybody on the server gets it in the jumps menu, and the file itself
can be passed on to another install. A jump can be saved into a
collection - a player's own pack, say - which is the map's: a
directory, <game>/jumps/<map>/<collection>/, to pass on whole; anybody
may add to one. The frame rate in it is the
player's cl_maxfps, which his client keeps the game told of (cvarsync);
a client without that gets the rate its commands came at instead, which
is not quite the same number (66 sends 15 ms commands, 66.7 a second).

Played back, a translucent ghost in the watcher's skin runs the take in
the map, over and over, and the watcher's view rides behind it or in
its eyes - fire switches, Escape ends it - with the keys and the frame
rate on the HUD.
The watcher is put on the take's first step afterwards, facing the same
way, to try it.
*/
#define JMP_REC_MAX_MS		120000
#define JMP_REC_STEP_MS		16		// a sample at most this often
#define JMP_REC_MAX			(JMP_REC_MAX_MS / JMP_REC_STEP_MS + 2)
#define JMP_REC_LEAD_MS		500		// kept of the standing still before the first move
#define JMP_PLAY_TAIL_MS	1000	// held on the last sample
#define JMP_DESC_MAX		57		// two menu rows, indented
#define JMP_ASK_SECS		60		// a question waits this long for its answer
#define JMP_ASK_NAME		1
#define JMP_ASK_DESC		2
#define JMP_ASK_PACK		3		// the name of a new collection
#define JMP_ASK_FIND		4		// what to search the collections for
#define JMP_PACKS_ROWS		8		// collections on a page of the save menu
#define JMP_PACKS_FIRST		7		// the row of the first one
#define JMP_PACKS_PAGE		12		// rows on a page of the collections screen
#define JMP_LIST_MAX		128
#define JMP_LIST_ROWS		8		// jumps on a menu page
#define JMP_LIST_FIRST		3		// the row of the first one
#define JMP_CAM_DIST		110		// third person: behind the ghost,
#define JMP_CAM_PITCH		15		// looking down at it,
#define JMP_CAM_TURN		0.2f	// its turns followed in about this many seconds
#define JMP_FILE_VERSION	1

#define JMP_KEY_FWD		1
#define JMP_KEY_BACK	2
#define JMP_KEY_LEFT	4
#define JMP_KEY_RIGHT	8
#define JMP_KEY_JUMP	16
#define JMP_KEY_DUCK	32
#define JMP_KEYS		6

// the HUD: an element per key, in bit order, then three lines of text
#define JMP_HUD_INFO	JMP_KEYS
#define JMP_HUD_DESC	(JMP_KEYS + 1)
#define JMP_HUD_HINT	(JMP_KEYS + 2)
#define JMP_HUD_COUNT	(JMP_KEYS + 3)

static const struct {
	char *label;
	int x, y;
} jmp_hud_keys[JMP_KEYS] = {
	{ "W", 0, -88 }, { "S", 0, -76 }, { "A", -14, -76 }, { "D", 14, -76 },
	{ "JUMP", 56, -76 }, { "DUCK", 56, -88 }
};

typedef struct {
	int		ms;			// since the take began, on the player's own clock
	vec3_t	origin;
	float	pitch, yaw;	// the view's
	short	frame;		// the model's animation frame
	short	speed;
	byte	keys;		// held at any time since the sample before
	signed char	viewheight;
} jmp_sample_t;

typedef struct {
	char	name[JMP_NAME_MAX];
	char	pack[JMP_NAME_MAX];	// its collection, "" none
	char	author[16];
	char	desc[JMP_DESC_MAX];
	int		fps;
	int		ms;			// its length
	int		count;
	jmp_sample_t	*samples;
} jmp_take_t;

typedef struct {
	int		seen;			// level.framenum it was last used at; a map change resets it

	qboolean	recording;
	jmp_take_t	take;		// being recorded, or the last one
	int		rec_ms, rec_cmds;
	int		rec_keys;
	int		rec_mark;		// the samples there were when the take menu came up

	int		ask;			// the question out to the player: JMP_ASK_*, 0 none
	int		ask_frame;		// level.framenum it lapses at
	char	ask_name[JMP_NAME_MAX];
	char	ask_pack[JMP_NAME_MAX];
	char	ask_desc[JMP_DESC_MAX];
	qboolean	ask_packed;	// the collection is settled, be it none
	char	save_find[JMP_NAME_MAX];	// the save menu's search, "" all the collections
	int		save_top;		// its first collection on the page

	qboolean	playing, pov;
	qboolean	play_started;
	jmp_take_t	play;
	float	play_ms;
	int		play_i;			// the sample at or before play_ms
	float	cam_yaw;
	int		held;			// fire as last seen, to act on a press only
	qboolean	play_layout;	// the client has a layout of somebody's to be blanked
	qboolean	play_tele;		// the view's teleport bit, flipped at each restart
	edict_t	*ghost;
	int		hud[JMP_HUD_COUNT];
	int		hud_keys;

	int		menu_top, menu_pick;
	char	menu_pack[JMP_NAME_MAX];	// the collection the list shows
	int		packs_top;		// the collections screen's first row on the page, from 0
	int		menu_sel;		// the jump under the list's cursor, its description shown; -1 none
	int		menu_cur;		// the cursor's row as the list was built
	qboolean	menu_keep;	// the list is rebuilt for the cursor: not read again
} jmp_state_t;

static jmp_state_t jmp_states[MAX_CLIENTS];
static jmp_take_t jmp_list[JMP_LIST_MAX];	// the map's stored jumps, without their samples
static int jmp_list_count;

static void JmpTakeFree(jmp_take_t *t)
{
	if (t->samples)
		gi.TagFree(t->samples);
	memset(t, 0, sizeof(*t));
}

static jmp_state_t *JmpState(edict_t *ent)
{
	jmp_state_t *st = &jmp_states[ent - g_edicts - 1];

	// a new map: the ghost and the HUD went with the old one
	if (st->seen > level.framenum) {
		JmpTakeFree(&st->take);
		JmpTakeFree(&st->play);
		memset(st, 0, sizeof(*st));
	}
	st->seen = level.framenum;
	return st;
}

static const char *JmpGameDir(void)
{
	cvar_t *game_cvar = gi.cvar("game", "action", 0);

	return *game_cvar->string ? game_cvar->string : GAMEVERSION;
}

// a jump's name is its file's: lower case letters, digits, - and _
static qboolean JmpNameClean(const char *in, char *out, size_t size)
{
	size_t n = 0;

	for (; *in; in++) {
		int c = Q_tolower(*in);

		if (!Q_isalnum(c) && c != '-' && c != '_')
			return false;
		if (n + 1 >= size)
			return false;
		out[n++] = c;
	}
	out[n] = 0;
	return n > 0;
}

// "collection/name" or "name" into its two clean halves
static qboolean JmpSplit(const char *arg, char *pack, char *name)
{
	char buf[JMP_NAME_MAX * 2 + 2], *slash;

	pack[0] = 0;
	if (strlen(arg) >= sizeof(buf))
		return false;
	strcpy(buf, arg);
	slash = strchr(buf, '/');
	if (!slash)
		return JmpNameClean(buf, name, JMP_NAME_MAX);
	*slash = 0;
	return JmpNameClean(buf, pack, JMP_NAME_MAX) && JmpNameClean(slash + 1, name, JMP_NAME_MAX);
}

// where a collection's jumps are - it is this map's - or those in none
static void JmpDir(char *path, size_t size, const char *pack)
{
	if (pack[0])
		Q_snprintf(path, size, "%s/jumps/%s/%s", JmpGameDir(), level.mapname, pack);
	else
		Q_snprintf(path, size, "%s/jumps/%s", JmpGameDir(), level.mapname);
}

static void JmpPath(char *path, size_t size, const char *pack, const char *name)
{
	char dir[MAX_OSPATH];

	JmpDir(dir, sizeof(dir), pack);
	Q_snprintf(path, size, "%s/%s.jmp", dir, name);
}

// the player's name as a file holds it
static void JmpAuthor(edict_t *ent, char *author, size_t size)
{
	size_t n = 0;
	char *c;

	for (c = ent->client->pers.netname; *c && n + 1 < size; c++)
		if (*c >= 32 && *c < 127)
			author[n++] = *c;
	author[n] = 0;
}

static void JmpSecs(int ms, char *buf, size_t size)
{
	Q_snprintf(buf, size, "%d.%d", ms / 1000, ms % 1000 / 100);
}

/*
The file: a line per header field, then a line per sample -

	jmodjump 1
	map urban2
	author zaviori
	desc from the roof to the balcony, strafe left
	fps 125
	ms 6410
	samples 388
	<ms> <x> <y> <z> <pitch> <yaw> <frame> <keys> <viewheight> <speed>
*/
static qboolean JmpWrite(jmp_take_t *t)
{
	char path[MAX_OSPATH];
	FILE *f;
	int i;

	Q_snprintf(path, sizeof(path), "%s/jumps", JmpGameDir());
	os_mkdir(path);
	JmpDir(path, sizeof(path), "");
	os_mkdir(path);
	if (t->pack[0]) {
		JmpDir(path, sizeof(path), t->pack);
		os_mkdir(path);
	}

	JmpPath(path, sizeof(path), t->pack, t->name);
	f = fopen(path, "w");
	if (!f)
		return false;

	fprintf(f, "jmodjump %d\nmap %s\nauthor %s\ndesc %s\nfps %d\nms %d\nsamples %d\n",
		JMP_FILE_VERSION, level.mapname, t->author, t->desc, t->fps, t->ms, t->count);
	for (i = 0; i < t->count; i++) {
		jmp_sample_t *s = &t->samples[i];

		fprintf(f, "%d %.3f %.3f %.3f %.2f %.2f %d %d %d %d\n", s->ms,
			s->origin[0], s->origin[1], s->origin[2], s->pitch, s->yaw,
			s->frame, s->keys, s->viewheight, s->speed);
	}
	return fclose(f) == 0;
}

// the header alone into the list, or the samples with it to play
static qboolean JmpRead(const char *pack, const char *name, jmp_take_t *t, qboolean samples)
{
	char path[MAX_OSPATH], line[256], key[16], value[64];
	FILE *f;
	int i, version = 0, count = 0;

	memset(t, 0, sizeof(*t));
	JmpPath(path, sizeof(path), pack, name);
	f = fopen(path, "r");
	if (!f)
		return false;

	Q_strncpyz(t->name, name, sizeof(t->name));
	Q_strncpyz(t->pack, pack, sizeof(t->pack));
	while (fgets(line, sizeof(line), f)) {
		value[0] = 0;
		if (sscanf(line, "%15s %63[^\r\n]", key, value) < 1)
			break;
		if (!strcmp(key, "jmodjump"))
			version = atoi(value);
		else if (!strcmp(key, "author"))
			Q_strncpyz(t->author, value, sizeof(t->author));
		else if (!strcmp(key, "desc"))
			Q_strncpyz(t->desc, value, sizeof(t->desc));
		else if (!strcmp(key, "fps"))
			t->fps = atoi(value);
		else if (!strcmp(key, "ms"))
			t->ms = atoi(value);
		else if (!strcmp(key, "samples")) {
			count = atoi(value);
			break;
		}
	}

	if (version != JMP_FILE_VERSION || count < 2 || count > JMP_REC_MAX || t->ms <= 0) {
		fclose(f);
		return false;
	}
	if (!samples) {
		fclose(f);
		return true;
	}

	t->samples = gi.TagMalloc(count * sizeof(jmp_sample_t), TAG_GAME);
	for (i = 0; i < count && fgets(line, sizeof(line), f); i++) {
		jmp_sample_t *s = &t->samples[i];
		int frame, keys, viewheight, speed;

		if (sscanf(line, "%d %f %f %f %f %f %d %d %d %d", &s->ms,
			&s->origin[0], &s->origin[1], &s->origin[2], &s->pitch, &s->yaw,
			&frame, &keys, &viewheight, &speed) != 10)
			break;
		if (s->ms < (i ? s[-1].ms : 0))
			break;
		s->frame = frame;
		s->keys = keys;
		s->viewheight = viewheight;
		s->speed = speed;
	}
	fclose(f);

	if (i < count) {
		JmpTakeFree(t);
		return false;
	}
	t->count = count;
	t->ms = t->samples[count - 1].ms;
	return true;
}

// by collection, those in none first, then by name
static int JmpListCmp(const void *a, const void *b)
{
	const jmp_take_t *ta = a, *tb = b;
	int c = strcmp(ta->pack, tb->pack);

	return c ? c : strcmp(ta->name, tb->name);
}

static void JmpListScan(const char *pack)
{
	char path[MAX_OSPATH], name[JMP_NAME_MAX];
	struct dirent *e;
	DIR *dir;
	size_t len;

	JmpDir(path, sizeof(path), pack);
	dir = opendir(path);
	if (!dir)
		return;

	while (jmp_list_count < JMP_LIST_MAX && (e = readdir(dir)) != NULL) {
		len = strlen(e->d_name);
		if (len < 5 || len - 4 >= sizeof(name) || strcmp(e->d_name + len - 4, ".jmp"))
			continue;
		memcpy(name, e->d_name, len - 4);
		name[len - 4] = 0;
		if (JmpRead(pack, name, &jmp_list[jmp_list_count], false))
			jmp_list_count++;
	}
	closedir(dir);
}

static int JmpPackCmp(const void *a, const void *b)
{
	return strcmp(a, b);
}

// the map's collections: the directories among its jumps, which a
// file's name with its dot cannot be taken for
static int JmpPacksAll(char packs[][JMP_NAME_MAX], int max)
{
	char path[MAX_OSPATH];
	struct dirent *e;
	DIR *dir;
	int count = 0;

	JmpDir(path, sizeof(path), "");
	dir = opendir(path);
	if (!dir)
		return 0;

	while (count < max && (e = readdir(dir)) != NULL) {
		if (JmpNameClean(e->d_name, packs[count], JMP_NAME_MAX) && !strcmp(packs[count], e->d_name))
			count++;
	}
	closedir(dir);

	qsort(packs, count, JMP_NAME_MAX, JmpPackCmp);
	return count;
}

// the collections a search finds, the closest first: fuzzy, the letters
// in order anywhere in the name
static int JmpPacksFind(char packs[][JMP_NAME_MAX], int max, const char *find)
{
	int score[JMP_PACKS_MAX], count = JmpPacksAll(packs, min(max, JMP_PACKS_MAX));
	char name[JMP_NAME_MAX];
	int i, j, n = 0, sc;

	if (!find[0])
		return count;

	// an insertion sort as they pass: it keeps the names' order among equals
	for (i = 0; i < count; i++) {
		sc = Fuzzy_Score(find, packs[i]);
		if (sc < 0)
			continue;
		Q_strncpyz(name, packs[i], sizeof(name));
		for (j = n; j > 0 && score[j - 1] < sc; j--) {
			score[j] = score[j - 1];
			memcpy(packs[j], packs[j - 1], JMP_NAME_MAX);
		}
		score[j] = sc;
		memcpy(packs[j], name, JMP_NAME_MAX);
		n++;
	}
	return n;
}

// the map's jumps: those in no collection and each collection's
static void JmpListLoad(void)
{
	char packs[JMP_PACKS_MAX][JMP_NAME_MAX];
	int i, count;

	jmp_list_count = 0;
	JmpListScan("");
	count = JmpPacksAll(packs, JMP_PACKS_MAX);
	for (i = 0; i < count; i++)
		JmpListScan(packs[i]);

	qsort(jmp_list, jmp_list_count, sizeof(jmp_list[0]), JmpListCmp);
}

// the map has jumps in a collection: sorted, so the last one tells
static qboolean JmpListPacked(void)
{
	return jmp_list_count && jmp_list[jmp_list_count - 1].pack[0];
}

// a collection's stretch of the list
static void JmpListRange(const char *pack, int *lo, int *hi)
{
	for (*lo = 0; *lo < jmp_list_count && strcmp(jmp_list[*lo].pack, pack); (*lo)++)
		;
	for (*hi = *lo; *hi < jmp_list_count && !strcmp(jmp_list[*hi].pack, pack); (*hi)++)
		;
}

/*
Stored jumps on the spawnpoint list: a collection turned on there has
the places its jumps start from listed and marked with the spawnpoints,
to spawn at. Up to JMP_STARTS_PACKS collections at a time, the key "/"
standing for the jumps in none; kept with the player for the map.
*/
static const char *JmpStartsKey(const char *pack)
{
	return pack[0] ? pack : "/";
}

static qboolean JmpStartsOn(edict_t *ent, const char *pack)
{
	gclient_t *client = ent->client;
	int i;

	for (i = 0; i < JMP_STARTS_PACKS; i++)
		if (!strcmp(client->resp.jmp_starts_packs[i], JmpStartsKey(pack)))
			return true;
	return false;
}

// the map's collections that have jumps, with how many; "" is those in none
static int JmpSpotPacks(char packs[][JMP_NAME_MAX], int *counts, int max)
{
	int i, j, n = 0;

	JmpListLoad();
	for (i = 0; i < jmp_list_count && n < max; i = j, n++) {
		for (j = i; j < jmp_list_count && !strcmp(jmp_list[j].pack, jmp_list[i].pack); j++)
			;
		Q_strncpyz(packs[n], jmp_list[i].pack, JMP_NAME_MAX);
		counts[n] = j - i;
	}
	return n;
}

// a jump's first sample alone
static qboolean JmpReadStart(const char *pack, const char *name, jmp_start_t *start)
{
	char path[MAX_OSPATH], line[256];
	float speed;
	int ms, frame, keys, viewheight;
	qboolean ok = false;
	FILE *f;

	JmpPath(path, sizeof(path), pack, name);
	f = fopen(path, "r");
	if (!f)
		return false;

	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "samples ", 8))
			continue;
		ok = fgets(line, sizeof(line), f) && sscanf(line, "%d %f %f %f %f %f %d %d %d %f", &ms,
			&start->origin[0], &start->origin[1], &start->origin[2], &start->pitch, &start->yaw,
			&frame, &keys, &viewheight, &speed) == 10;
		break;
	}
	fclose(f);
	if (!ok)
		return false;

	start->ducked = viewheight < 10;
	Q_strncpyz(start->name, name, sizeof(start->name));
	Q_strncpyz(start->pack, pack, sizeof(start->pack));
	return true;
}

// the starts of every collection that is on, read again
static void JmpStartsLoad(edict_t *ent)
{
	gclient_t *client = ent->client;
	int i;

	client->resp.jmp_starts_count = 0;
	JmpListLoad();
	for (i = 0; i < jmp_list_count && client->resp.jmp_starts_count < JMP_STARTS_MAX; i++) {
		if (!JmpStartsOn(ent, jmp_list[i].pack))
			continue;
		if (JmpReadStart(jmp_list[i].pack, jmp_list[i].name,
			&client->resp.jmp_starts[client->resp.jmp_starts_count]))
			client->resp.jmp_starts_count++;
	}
}

static void JmpStartsToggle(edict_t *ent, const char *pack)
{
	gclient_t *client = ent->client;
	int i, slot = -1;

	for (i = 0; i < JMP_STARTS_PACKS; i++) {
		if (!strcmp(client->resp.jmp_starts_packs[i], JmpStartsKey(pack))) {
			client->resp.jmp_starts_packs[i][0] = 0;
			JmpStartsLoad(ent);
			return;
		}
		if (slot < 0 && !client->resp.jmp_starts_packs[i][0])
			slot = i;
	}
	if (slot < 0) {
		gi.cprintf(ent, PRINT_HIGH, "%d collections at a time: turn one off first\n", JMP_STARTS_PACKS);
		return;
	}
	Q_strncpyz(client->resp.jmp_starts_packs[slot], JmpStartsKey(pack), JMP_NAME_MAX);
	JmpStartsLoad(ent);
	if (client->resp.jmp_starts_count >= JMP_STARTS_MAX)
		gi.cprintf(ent, PRINT_HIGH, "The list takes %d starts: some are left out\n", JMP_STARTS_MAX);
}

// a jump saved or deleted: everybody's listed starts are read again, so
// none is marked that is gone and a new one is there
static void JmpStartsRefresh(void)
{
	edict_t *ent;
	int i, j;

	for (i = 0, ent = g_edicts + 1; i < game.maxclients; i++, ent++) {
		if (!ent->inuse || !ent->client)
			continue;
		for (j = 0; j < JMP_STARTS_PACKS; j++) {
			if (ent->client->resp.jmp_starts_packs[j][0]) {
				JmpStartsLoad(ent);
				break;
			}
		}
	}
}

//
// recording
//
static void JmpRecReset(jmp_state_t *st)
{
	st->take.count = 0;
	st->rec_ms = st->rec_cmds = st->rec_keys = 0;
}

// a teleport while recording: the take begins again from there
void Jmp_RecordRestart(edict_t *ent)
{
	jmp_state_t *st = JmpState(ent);

	if (st->recording)
		JmpRecReset(st);
}

// a take over within its countdown: the start it waited for is off
static void JmpRecUnspawn(edict_t *ent)
{
	gclient_t *client = ent->client;

	if (client->resp.jmp_spawn_frame && !client->resp.jmp_spawn_repeat) {
		client->resp.jmp_spawn_frame = 0;
		gi.centerprintf(ent, " ");
	}
}

static void JmpRecStop(edict_t *ent)
{
	jmp_state_t *st = JmpState(ent);
	jmp_take_t *t = &st->take;
	jmp_sample_t *s = t->samples;
	char secs[16];
	int i, first, start, fps;

	st->recording = false;
	JmpRecUnspawn(ent);

	// from half a second before the first key or step on
	for (i = 1; i < t->count; i++) {
		if (s[i].keys || fabsf(s[i].origin[0] - s[0].origin[0]) > 1
			|| fabsf(s[i].origin[1] - s[0].origin[1]) > 1)
			break;
	}
	if (i >= t->count) {
		t->count = 0;
		gi.cprintf(ent, PRINT_HIGH, "Recording stopped, nothing moved\n");
		return;
	}
	start = s[i].ms - JMP_REC_LEAD_MS;
	for (first = 0; s[first].ms < start; first++)
		;
	if (first) {
		t->count -= first;
		memmove(s, s + first, t->count * sizeof(*s));
	}
	start = s[0].ms;
	for (i = 0; i < t->count; i++)
		s[i].ms -= start;

	t->ms = s[t->count - 1].ms;
	fps = atoi(ent->client->cl_cvar[clcvar_cl_maxfps]);
	t->fps = fps > 0 ? fps : st->rec_cmds * 1000 / max(st->rec_ms, 1);
	t->name[0] = 0;
	t->pack[0] = 0;
	t->desc[0] = 0;
	JmpAuthor(ent, t->author, sizeof(t->author));

	JmpSecs(t->ms, secs, sizeof(secs));
	gi.cprintf(ent, PRINT_HIGH, "Recorded %s seconds at %d fps: \"jmod play\" to watch it, "
		"\"jmod save\" to keep it\n", secs, t->fps);
}

// called with every command the player moves by
void Jmp_RecordCmd(edict_t *ent, usercmd_t *ucmd)
{
	jmp_state_t *st = &jmp_states[ent - g_edicts - 1];
	gclient_t *client = ent->client;
	jmp_take_t *t = &st->take;
	jmp_sample_t *s;

	if (!st->recording)
		return;

	if (ucmd->forwardmove > 0)
		st->rec_keys |= JMP_KEY_FWD;
	else if (ucmd->forwardmove < 0)
		st->rec_keys |= JMP_KEY_BACK;
	if (ucmd->sidemove < 0)
		st->rec_keys |= JMP_KEY_LEFT;
	else if (ucmd->sidemove > 0)
		st->rec_keys |= JMP_KEY_RIGHT;
	if (ucmd->upmove > 0)
		st->rec_keys |= JMP_KEY_JUMP;
	else if (ucmd->upmove < 0)
		st->rec_keys |= JMP_KEY_DUCK;

	st->rec_ms += ucmd->msec;
	st->rec_cmds++;
	if (t->count && st->rec_ms - t->samples[t->count - 1].ms < JMP_REC_STEP_MS)
		return;

	if (st->rec_ms > JMP_REC_MAX_MS || t->count >= JMP_REC_MAX) {
		gi.cprintf(ent, PRINT_HIGH, "A take is %d seconds at most\n", JMP_REC_MAX_MS / 1000);
		JmpRecStop(ent);
		return;
	}

	s = &t->samples[t->count++];
	s->ms = st->rec_ms;
	VectorCopy(ent->s.origin, s->origin);
	s->pitch = client->v_angle[PITCH];
	s->yaw = client->v_angle[YAW];
	s->frame = ent->s.frame;
	s->speed = min(VectorLength(ent->velocity), 32000);
	s->keys = st->rec_keys;
	s->viewheight = ent->viewheight;
	st->rec_keys = 0;
}

// jmod rec - start a take, or end the one running
static void Cmd_JumpRec_f(edict_t *ent)
{
	jmp_state_t *st = JmpState(ent);

	if (st->recording) {
		JmpRecStop(ent);
		return;
	}
	if (ent->deadflag || ent->client->pers.spectator) {
		gi.cprintf(ent, PRINT_HIGH, "This command cannot be used by spectators\n");
		return;
	}
	if (st->playing) {
		gi.cprintf(ent, PRINT_HIGH, "Stop the playback first: jmod stop\n");
		return;
	}

	if (!st->take.samples)
		st->take.samples = gi.TagMalloc(JMP_REC_MAX * sizeof(jmp_sample_t), TAG_GAME);
	JmpRecReset(st);
	st->recording = true;
	gi.cprintf(ent, PRINT_HIGH, "Recording from here. \"jmod recall\" for another go - a teleport "
		"starts the take again - and \"jmod rec\" ends it\n");

	// the start is stored, and begun with a countdown that ends back on
	// the spot
	JmpStoreHere(ent);
	JmpSpawn(ent, JMP_SPOT_STORED, JMP_REC_COUNTDOWN, 0);
}

static qboolean JmpIsRecording(edict_t *ent)
{
	return jmp_states[ent - g_edicts - 1].recording;
}

/*
Saving asks for what the command did not give - a line describing the
jump, then its name, then the collection. The collection is picked from
a menu;
the texts are asked with the client's messageprompt, a box with the
question over the line typed, answered as "jmod answer <text>". A client
without the command sends it on as an unknown one: that one is asked in
its chat prompt instead, and what it says next is the answer and goes to
nobody.
*/
static void JmpSaveToShow(edict_t *ent);
static void JmpSaveToOpen(edict_t *ent);

static void JmpAsk(edict_t *ent, int ask)
{
	jmp_state_t *st = JmpState(ent);
	gclient_t *client = ent->client;

	st->ask = ask;
	st->ask_frame = level.framenum + JMP_ASK_SECS * HZ;
	if (client->layout == LAYOUT_MENU)
		PMenu_Close(ent);

	// the box says it all; a client without it is told in Jmp_Answer
	if (ask == JMP_ASK_DESC)
		stuffcmd(ent, "messageprompt \"Describe the jump\" \"Where it goes, what the trick is\" jmod answer\n");
	else if (ask == JMP_ASK_NAME)
		stuffcmd(ent, "messageprompt \"Name the jump\" \"Letters, digits, - and _\" jmod answer\n");
	else if (ask == JMP_ASK_PACK)
		stuffcmd(ent, "messageprompt \"Name the new collection\" \"Letters, digits, - and _\" jmod answer\n");
	else
		stuffcmd(ent, "messageprompt \"Search the collections\" \"A few letters of the name\" jmod answer\n");
}

// the name is free in its collection, or the player's own to overwrite
static qboolean JmpSaveFree(edict_t *ent)
{
	jmp_state_t *st = JmpState(ent);
	jmp_take_t old;

	if (JmpRead(st->ask_pack, st->ask_name, &old, false) && strcmp(old.author, st->take.author)) {
		gi.cprintf(ent, PRINT_HIGH, "%s has a jump called \"%s\" there already\n",
			old.author, st->ask_name);
		return false;
	}
	return true;
}

// a description as the file holds it: a line of plain text
static void JmpDescClean(const char *in, char *out, size_t size)
{
	size_t n = 0;

	for (; *in && n + 1 < size; in++)
		if (*in >= 32 && *in < 127 && *in != '"' && (n || *in != ' '))
			out[n++] = *in;
	while (n && out[n - 1] == ' ')
		n--;
	out[n] = 0;
}

static void JmpSaveWrite(edict_t *ent)
{
	jmp_state_t *st = JmpState(ent);
	jmp_take_t *t = &st->take;
	char path[MAX_OSPATH];

	// the collection menu is done with, whatever comes of the write
	if (ent->client->layout == LAYOUT_MENU)
		PMenu_Close(ent);

	Q_strncpyz(t->desc, st->ask_desc, sizeof(t->desc));
	Q_strncpyz(t->name, st->ask_name, sizeof(t->name));
	Q_strncpyz(t->pack, st->ask_pack, sizeof(t->pack));
	if (!JmpWrite(t)) {
		t->name[0] = 0;	// a take with a name is a saved one
		gi.cprintf(ent, PRINT_HIGH, "Could not write the jump\n");
		return;
	}
	JmpStartsRefresh();
	JmpPath(path, sizeof(path), t->pack, t->name);
	gi.centerprintf(ent, "Saved %s\n", t->name);
	gi.cprintf(ent, PRINT_HIGH, "Saved \"%s\"%s%s: %s\n", t->name,
		t->pack[0] ? " in " : "", t->pack, path);
}

// the next thing a save is missing, in the order it is asked - the
// description, the name, the collection - or the save itself
static void JmpSaveNext(edict_t *ent)
{
	jmp_state_t *st = JmpState(ent);

	if (!st->ask_desc[0]) {
		JmpAsk(ent, JMP_ASK_DESC);
	} else if (!st->ask_name[0]) {
		JmpAsk(ent, JMP_ASK_NAME);
	} else if (!st->ask_packed) {
		JmpSaveToOpen(ent);
	} else if (!JmpSaveFree(ent)) {
		// another name, for the same collection
		st->ask_name[0] = 0;
		JmpAsk(ent, JMP_ASK_NAME);
	} else {
		JmpSaveWrite(ent);
	}
}

// jmod save [[collection/]name] [description]
static void Cmd_JumpSave_f(edict_t *ent)
{
	jmp_state_t *st = JmpState(ent);
	qboolean typed = !Q_stricmp(gi.argv(0), "jmod");	// not so from the menu
	char desc[256];
	int i;

	if (st->recording)
		JmpRecStop(ent);
	st->ask = 0;
	if (!st->take.count) {
		gi.cprintf(ent, PRINT_HIGH, "Record a jump first: jmod rec\n");
		return;
	}

	st->ask_desc[0] = st->ask_name[0] = st->ask_pack[0] = 0;
	st->ask_packed = false;
	if (typed && gi.argc() >= 3) {
		if (!JmpSplit(gi.argv(2), st->ask_pack, st->ask_name)) {
			gi.cprintf(ent, PRINT_HIGH, "Usage: jmod save [[collection/]name] [description] - "
				"names are letters, digits, - and _, %d at most\n", JMP_NAME_MAX - 1);
			return;
		}
		// the whole command given saves without a question: a bare name is in no collection
		st->ask_packed = st->ask_pack[0] || gi.argc() >= 4;

		desc[0] = 0;
		for (i = 3; i < gi.argc(); i++) {
			if (i > 3)
				Q_strlcat(desc, " ", sizeof(desc));
			Q_strlcat(desc, gi.argv(i), sizeof(desc));
		}
		JmpDescClean(desc, st->ask_desc, sizeof(st->ask_desc));
	}
	JmpSaveNext(ent);
}

// the answer to the question that is out; NULL turns it down
static void JmpAnswerText(edict_t *ent, const char *text)
{
	jmp_state_t *st = JmpState(ent);
	char buf[256], *p;
	size_t len;
	int ask = st->ask;

	st->ask = 0;
	if (!ask || level.framenum > st->ask_frame || !st->take.count)
		return;

	// the save menu's search: back to the menu with it, or without on Escape
	if (ask == JMP_ASK_FIND) {
		for (len = 0; text && *text && len + 1 < sizeof(st->save_find); text++)
			if (*text > 32 && *text < 127 && *text != '"')
				st->save_find[len++] = *text;
		st->save_find[len] = 0;
		st->save_top = 0;
		JmpSaveToShow(ent);
		return;
	}
	if (!text) {
		gi.cprintf(ent, PRINT_HIGH, "Not saved. \"jmod save\" asks again\n");
		return;
	}

	Q_strncpyz(buf, text, sizeof(buf));
	for (p = buf; *p == ' '; p++)
		;
	for (len = strlen(p); len && p[len - 1] == ' '; len--)
		p[len - 1] = 0;

	if (ask == JMP_ASK_NAME) {
		char pack[JMP_NAME_MAX];

		if (!JmpSplit(p, pack, st->ask_name)) {
			st->ask_name[0] = 0;
			gi.cprintf(ent, PRINT_HIGH, "A name is letters, digits, - and _, %d at most\n",
				JMP_NAME_MAX - 1);
		} else if (pack[0]) {
			// "collection/name" settles both
			Q_strncpyz(st->ask_pack, pack, sizeof(st->ask_pack));
			st->ask_packed = true;
		}
	} else if (ask == JMP_ASK_PACK) {
		if (JmpNameClean(p, st->ask_pack, sizeof(st->ask_pack)))
			st->ask_packed = true;
		else
			gi.cprintf(ent, PRINT_HIGH, "A name is letters, digits, - and _, %d at most\n",
				JMP_NAME_MAX - 1);
	} else {
		JmpDescClean(p, st->ask_desc, sizeof(st->ask_desc));
	}
	JmpSaveNext(ent);
}

// a client's commands while a question is out: true if this one was to do with it
qboolean Jmp_Answer(edict_t *ent)
{
	jmp_state_t *st = &jmp_states[ent - g_edicts - 1];
	char text[256], *p;
	size_t len;

	if (!st->ask)
		return false;

	// no messageprompt in this client: its chat prompt will do
	if (!Q_stricmp(gi.argv(0), "messageprompt")) {
		gi.centerprintf(ent, st->ask == JMP_ASK_NAME ? "Name the jump\n"
			: st->ask == JMP_ASK_PACK ? "Name the new collection\n"
			: st->ask == JMP_ASK_FIND ? "Search the collections\n" : "Describe the jump\n");
		gi.cprintf(ent, PRINT_HIGH, "%s\n", st->ask == JMP_ASK_NAME || st->ask == JMP_ASK_PACK
			? "A name: letters, digits, - and _. Type it and press Enter"
			: st->ask == JMP_ASK_FIND ? "A few letters of the collection's name. Type them and press Enter"
			: "A line on the jump: where it goes, what the trick is. Type it and press Enter");
		stuffcmd(ent, "messagemode\n");
		return true;
	}
	if (Q_stricmp(gi.argv(0), "say") && Q_stricmp(gi.argv(0), "say_team"))
		return false;
	if (level.framenum > JmpState(ent)->ask_frame) {
		st->ask = 0;
		return false;
	}

	// the prompt sends it quoted
	Q_strncpyz(text, gi.args(), sizeof(text));
	p = text;
	len = strlen(p);
	if (len >= 2 && p[0] == '"' && p[len - 1] == '"') {
		p[len - 1] = 0;
		p++;
	}
	JmpAnswerText(ent, p);
	return true;
}

// a jump is its author's to delete; in a local game, the player's whose game it is
static qboolean JmpMayDelete(edict_t *ent, jmp_take_t *t)
{
	char author[sizeof(t->author)];

	if (!dedicated->value)
		return true;
	JmpAuthor(ent, author, sizeof(author));
	return !strcmp(author, t->author);
}

static qboolean JmpDelete(edict_t *ent, const char *pack, const char *name)
{
	char path[MAX_OSPATH];
	jmp_take_t t;

	if (!JmpRead(pack, name, &t, false)) {
		gi.cprintf(ent, PRINT_HIGH, "No jump called \"%s\" there on %s\n", name, level.mapname);
		return false;
	}
	if (!JmpMayDelete(ent, &t)) {
		gi.cprintf(ent, PRINT_HIGH, "\"%s\" is %s's to delete\n", name, t.author);
		return false;
	}
	JmpPath(path, sizeof(path), pack, name);
	if (remove(path)) {
		gi.cprintf(ent, PRINT_HIGH, "Could not delete %s\n", path);
		return false;
	}
	JmpStartsRefresh();
	gi.cprintf(ent, PRINT_HIGH, "Deleted \"%s\"\n", name);
	return true;
}

/*
A collection's jumps: counted, with whether any is somebody else's, or
deleted and the collection's directory after them. Returns the jumps
there were.
*/
static int JmpPackWalk(edict_t *ent, const char *pack, qboolean *foreign, qboolean del)
{
	char dir[MAX_OSPATH], path[MAX_OSPATH], line[128], author[16], mine[16];
	struct dirent *e;
	DIR *d;
	FILE *f;
	size_t len;
	int count = 0;

	*foreign = false;
	JmpAuthor(ent, mine, sizeof(mine));
	JmpDir(dir, sizeof(dir), pack);
	d = opendir(dir);
	if (!d)
		return 0;

	while ((e = readdir(d)) != NULL) {
		len = strlen(e->d_name);
		if (len < 5 || strcmp(e->d_name + len - 4, ".jmp"))
			continue;
		Q_snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		count++;
		if (del) {
			remove(path);
			continue;
		}

		// whose it is: the author line is among the first few
		author[0] = 0;
		f = fopen(path, "r");
		if (f) {
			while (fgets(line, sizeof(line), f) && strncmp(line, "samples", 7))
				if (sscanf(line, "author %15[^\r\n]", author) == 1)
					break;
			fclose(f);
		}
		if (strcmp(author, mine))
			*foreign = true;
	}
	closedir(d);
	if (del)
		rmdir(dir);
	return count;
}

// a collection is deleted by the one whose every jump in it is; in a
// local game, by the player whose game it is
static qboolean JmpPackMayDelete(edict_t *ent, const char *pack)
{
	qboolean foreign;

	if (!dedicated->value)
		return true;
	JmpPackWalk(ent, pack, &foreign, false);
	return !foreign;
}

static qboolean JmpPackExists(const char *pack)
{
	char root[MAX_OSPATH];
	DIR *d;

	JmpDir(root, sizeof(root), pack);
	d = opendir(root);
	if (d)
		closedir(d);
	return d != NULL;
}

static void JmpPackDelete(edict_t *ent, const char *pack)
{
	qboolean foreign;
	int count;

	if (!JmpPackMayDelete(ent, pack)) {
		gi.cprintf(ent, PRINT_HIGH, "\"%s\" has other players' jumps in it, and stays\n", pack);
		return;
	}
	count = JmpPackWalk(ent, pack, &foreign, true);
	JmpStartsRefresh();
	if (JmpPackExists(pack))
		gi.cprintf(ent, PRINT_HIGH, "Could not delete all of \"%s\"\n", pack);
	else if (count)
		gi.cprintf(ent, PRINT_HIGH, "Deleted the collection \"%s\" and its %d jumps\n", pack, count);
	else
		gi.cprintf(ent, PRINT_HIGH, "Deleted the collection \"%s\"\n", pack);
}

// jmod delete <[collection/]name>, or <collection/> [all] for a collection
static void Cmd_JumpDelete_f(edict_t *ent)
{
	char pack[JMP_NAME_MAX], name[JMP_NAME_MAX], arg[JMP_NAME_MAX + 2];
	qboolean foreign;
	size_t len;
	int count;

	Q_strncpyz(arg, gi.argv(2), sizeof(arg));
	len = strlen(arg);
	if (gi.argc() >= 3 && len > 1 && arg[len - 1] == '/') {
		arg[len - 1] = 0;
		if (!JmpNameClean(arg, pack, sizeof(pack)) || !JmpPackExists(pack)) {
			gi.cprintf(ent, PRINT_HIGH, "No collection called \"%s\"\n", arg);
			return;
		}
		// its jumps go with it: that has to be said twice
		count = JmpPackWalk(ent, pack, &foreign, false);
		if (count && Q_stricmp(gi.argv(3), "all")) {
			gi.cprintf(ent, PRINT_HIGH, "\"%s\" has %d jump%s. \"jmod delete %s/ all\" "
				"deletes them with it\n", pack, count, count == 1 ? "" : "s", pack);
			return;
		}
		JmpPackDelete(ent, pack);
		return;
	}

	if (gi.argc() < 3 || !JmpSplit(gi.argv(2), pack, name)) {
		gi.cprintf(ent, PRINT_HIGH, "Usage: jmod delete <[collection/]name>\n");
		return;
	}
	JmpDelete(ent, pack, name);
}

//
// playback
//
static void JmpHudHint(edict_t *ent, jmp_state_t *st)
{
	Ghud_SetText(ent, st->hud[JMP_HUD_HINT], st->pov
		? "player's view - fire: third person, Esc: stop"
		: "third person - fire: player's view, Esc: stop");
}

static void JmpHudKeys(edict_t *ent, jmp_state_t *st, int keys, qboolean all)
{
	int i;

	for (i = 0; i < JMP_KEYS; i++) {
		if (!all && !((keys ^ st->hud_keys) & (1 << i)))
			continue;
		if (keys & (1 << i))
			Ghud_SetColor(ent, st->hud[i], 255, 220, 0, 255);
		else
			Ghud_SetColor(ent, st->hud[i], 255, 255, 255, 70);
	}
	st->hud_keys = keys;
}

static void JmpHudMake(edict_t *ent, jmp_state_t *st)
{
	jmp_take_t *t = &st->play;
	char text[64], secs[16];
	int i, el;

	for (i = 0; i < JMP_HUD_COUNT; i++) {
		el = st->hud[i] = Ghud_NewElement(ent, GHT_TEXT);
		Ghud_SetAnchor(ent, el, 0.5f, 1);
		Ghud_SetTextFlags(ent, el, UI_CENTER);
		if (i < JMP_KEYS) {
			Ghud_SetPosition(ent, el, jmp_hud_keys[i].x, jmp_hud_keys[i].y);
			Ghud_SetText(ent, el, jmp_hud_keys[i].label);
		} else {
			Ghud_SetPosition(ent, el, 0, -60 + 10 * (i - JMP_HUD_INFO));
			Ghud_SetColor(ent, el, 255, 255, 255, i == JMP_HUD_HINT ? 140 : 255);
		}
	}

	JmpSecs(t->ms, secs, sizeof(secs));
	Q_snprintf(text, sizeof(text), "%s%s%s%s%.15s - %d fps - %s s", t->pack, t->pack[0] ? "/" : "",
		t->name, t->name[0] ? " by " : "", t->author, t->fps, secs);
	Ghud_SetText(ent, st->hud[JMP_HUD_INFO], text);
	Ghud_SetText(ent, st->hud[JMP_HUD_DESC], t->desc);
	JmpHudHint(ent, st);
	JmpHudKeys(ent, st, 0, true);
}

// on a spot, looking a way, standing still - as recall does it
static void JmpPlace(edict_t *ent, const vec3_t origin, float pitch, float yaw)
{
	gclient_t *client = ent->client;
	int i;

	client->jumping = 0;
	gi.unlinkentity(ent);

	VectorCopy(origin, ent->s.origin);
	VectorCopy(origin, ent->s.old_origin);
	VectorClear(ent->velocity);
	client->ps.pmove.pm_time = 160 >> 3;
	ent->s.event = EV_PLAYER_TELEPORT;

	VectorSet(client->v_angle, pitch, yaw, 0);
	VectorCopy(client->v_angle, client->ps.viewangles);
	VectorSet(ent->s.angles, 0, yaw, 0);
	VectorClear(client->ps.kick_angles);
	VectorClear(client->kick_angles);
	for (i = 0; i < 3; i++)
		client->ps.pmove.delta_angles[i] = ANGLE2SHORT(client->v_angle[i] - client->resp.cmd_angles[i]);
	client->fall_time = 0;
	client->fall_value = 0;

	gi.linkentity(ent);
}

// a jump's start as the stored spot: where it began, facing its way
static void JmpStoreStart(edict_t *ent, jmp_take_t *t)
{
	gclient_t *client = ent->client;
	jmp_sample_t *s = &t->samples[0];

	VectorCopy(s->origin, client->resp.jmp_teleport_origin);
	VectorSet(client->resp.jmp_teleport_v_angle, s->pitch, s->yaw, 0);
	client->resp.jmp_teleport_ducked = s->viewheight < 10;
}

// end the playback; place puts the watcher on the take's first step
static void JmpPlayStop(edict_t *ent, qboolean place)
{
	jmp_state_t *st = JmpState(ent);
	gclient_t *client = ent->client;
	int i;

	if (!st->playing)
		return;
	st->playing = false;

	if (st->ghost)
		G_FreeEdict(st->ghost);
	st->ghost = NULL;
	for (i = 0; i < JMP_HUD_COUNT; i++)
		Ghud_RemoveElement(ent, st->hud[i]);

	ent->svflags &= ~SVF_NOCLIENT;
	ent->movetype = MOVETYPE_WALK;
	ent->viewheight = 22;
	client->ps.pmove.pm_flags &= ~PMF_NO_PREDICTION;
	client->ps.pmove.pm_type = PM_NORMAL;
	if (place) {
		JmpPlace(ent, st->play.samples[0].origin, st->play.samples[0].pitch, st->play.samples[0].yaw);
		JmpStoreStart(ent, &st->play);	// and recall is back here
	}

	JmpTakeFree(&st->play);
}

// st->play is loaded: take the watcher's view and send the ghost off
static void JmpPlayStart(edict_t *ent, qboolean pov)
{
	jmp_state_t *st = JmpState(ent);
	gclient_t *client = ent->client;
	edict_t *ghost;

	// no drill teleports him out of it
	client->resp.jmp_spawn_frame = 0;
	client->resp.jmp_spawn_repeat = 0;
	if (client->layout == LAYOUT_MENU)
		PMenu_Close(ent);

	st->playing = true;
	st->play_started = false;
	st->pov = pov;
	st->play_ms = 0;
	st->play_i = 0;
	st->cam_yaw = st->play.samples[0].yaw;
	st->held = BUTTON_ATTACK;	// a key still down from before is not a press
	st->play_layout = true;

	// the player model in the watcher's skin, without a weapon
	ghost = st->ghost = G_Spawn();
	ghost->classname = "jmp_ghost";
	ghost->owner = ent;
	ghost->movetype = MOVETYPE_NONE;
	ghost->solid = SOLID_NOT;
	ghost->s.modelindex = 255;
	ghost->s.skinnum = ent - g_edicts - 1;
	ghost->s.renderfx = RF_TRANSLUCENT;
	VectorCopy(st->play.samples[0].origin, ghost->s.origin);
	VectorCopy(ghost->s.origin, ghost->s.old_origin);
	gi.linkentity(ghost);

	ent->svflags |= SVF_NOCLIENT;
	ent->movetype = MOVETYPE_NOCLIP;
	VectorClear(ent->velocity);

	JmpHudMake(ent, st);
}

// the ghost is not drawn for the one looking out of its eyes
qboolean Jmp_GhostHidden(edict_t *clent, edict_t *ent)
{
	jmp_state_t *st;

	if (!jump->value || ent->owner != clent || !clent->client)
		return false;

	st = &jmp_states[clent - g_edicts - 1];
	return st->playing && st->pov && ent == st->ghost;
}

// the watcher's commands: nothing moves him, fire is the one control
qboolean Jmp_PlayThink(edict_t *ent, usercmd_t *ucmd)
{
	jmp_state_t *st = &jmp_states[ent - g_edicts - 1];
	gclient_t *client = ent->client;
	int held, pressed;

	if (!st->playing)
		return false;

	client->resp.cmd_angles[0] = SHORT2ANGLE(ucmd->angles[0]);
	client->resp.cmd_angles[1] = SHORT2ANGLE(ucmd->angles[1]);
	client->resp.cmd_angles[2] = SHORT2ANGLE(ucmd->angles[2]);
	client->ps.pmove.pm_type = PM_FREEZE;

	client->oldbuttons = client->buttons;
	client->buttons = ucmd->buttons;
	client->latched_buttons = 0;

	held = ucmd->buttons & BUTTON_ATTACK;
	pressed = held & ~st->held;
	st->held = held;

	if (pressed & BUTTON_ATTACK) {
		st->pov = !st->pov;
		JmpHudHint(ent, st);
	}
	return true;
}

// once a server frame, after the views are built: move the ghost along
// the take and put the watcher's view on it
void Jmp_PlayFrame(edict_t *ent)
{
	jmp_state_t *st = JmpState(ent);
	gclient_t *client = ent->client;
	jmp_take_t *t = &st->play;
	jmp_sample_t *a, *b;
	edict_t *ghost = st->ghost;
	vec3_t origin, angles, eye, forward, goal, o;
	trace_t trace;
	float ms, frac, pitch, yaw;
	int i, keys = 0;

	if (!st->playing)
		return;

	if (st->play_started)
		st->play_ms += 1000.0f / HZ;
	st->play_started = true;
	// over, and held a moment: again from the top, nothing sliding back there
	if (st->play_ms > t->ms + JMP_PLAY_TAIL_MS) {
		st->play_ms = 0;
		st->play_i = 0;
		st->cam_yaw = t->samples[0].yaw;
		ghost->s.event = EV_OTHER_TELEPORT;
		client->ps.pmove.pm_flags ^= PMF_TELEPORT_BIT;
		st->play_tele = !st->play_tele;
	}

	// the two samples around now, and every key held since the last frame
	ms = min(st->play_ms, t->ms);
	i = st->play_i;
	while (st->play_i < t->count - 2 && t->samples[st->play_i + 1].ms <= ms)
		st->play_i++;
	a = &t->samples[st->play_i];
	b = a + 1;
	frac = b->ms > a->ms ? Q_clipf((ms - a->ms) / (b->ms - a->ms), 0, 1) : 1;
	if (st->play_ms <= t->ms)
		for (i++; i <= st->play_i + 1; i++)
			keys |= t->samples[i].keys;

	LerpVector(a->origin, b->origin, frac, origin);
	pitch = LerpAngle(a->pitch, b->pitch, frac);
	yaw = LerpAngle(a->yaw, b->yaw, frac);

	VectorCopy(origin, ghost->s.origin);
	VectorSet(ghost->s.angles, pitch / 3, yaw, 0);
	ghost->s.frame = a->frame;
	gi.linkentity(ghost);

	if (st->pov) {
		VectorSet(angles, pitch, yaw, 0);
		VectorCopy(origin, goal);
		VectorSet(client->ps.viewoffset, 0, 0, a->viewheight);
		st->cam_yaw = yaw;
	} else {
		// behind where it looks, the turns smoothed: a strafe jump's
		// mouse swings would throw the camera about
		st->cam_yaw = LerpAngle(st->cam_yaw, yaw, 1 - expf(-1.0f / (HZ * JMP_CAM_TURN)));
		VectorSet(angles, JMP_CAM_PITCH, st->cam_yaw, 0);
		VectorCopy(origin, eye);
		eye[2] += a->viewheight;
		AngleVectors(angles, forward, NULL, NULL);
		VectorMA(eye, -JMP_CAM_DIST, forward, o);
		trace = gi.trace(eye, vec3_origin, vec3_origin, o, ghost, MASK_SOLID);
		VectorMA(trace.endpos, 2, forward, goal);

		// pad for floors and ceilings
		VectorCopy(goal, o);
		o[2] += 6;
		trace = gi.trace(goal, vec3_origin, vec3_origin, o, ghost, MASK_SOLID);
		if (trace.fraction < 1) {
			VectorCopy(trace.endpos, goal);
			goal[2] -= 6;
		}
		VectorCopy(goal, o);
		o[2] -= 6;
		trace = gi.trace(goal, vec3_origin, vec3_origin, o, ghost, MASK_SOLID);
		if (trace.fraction < 1) {
			VectorCopy(trace.endpos, goal);
			goal[2] += 6;
		}
		VectorClear(client->ps.viewoffset);
	}

	VectorCopy(goal, ent->s.origin);
	VectorScale(goal, 8, client->ps.pmove.origin);
	VectorClear(ent->velocity);
	VectorClear(client->ps.pmove.velocity);
	for (i = 0; i < 3; i++)
		client->ps.pmove.delta_angles[i] = ANGLE2SHORT(angles[i] - client->resp.cmd_angles[i]);
	VectorCopy(angles, client->ps.viewangles);
	VectorCopy(angles, client->v_angle);
	VectorClear(client->ps.kick_angles);
	client->ps.gunindex = client->ps.gunframe = 0;
	client->ps.pmove.pm_type = PM_FREEZE;
	client->ps.pmove.pm_flags |= PMF_NO_PREDICTION;
	client->ps.stats[STAT_SPEEDX] = a->speed + frac * (b->speed - a->speed);
	ent->viewheight = 0;
	gi.linkentity(ent);

	// the extended protocol's "do not glide there", built anew each frame
	if (st->play_tele)
		client->ps.rdflags |= RDF_TELEPORT_BIT;
	else
		client->ps.rdflags &= ~RDF_TELEPORT_BIT;

	/*
	Escape is the client's own key, and is sent on ("putaway") only
	while a layout is up: an empty one is, for as long as this plays -
	sent again after a menu or the scores have had the client's.
	*/
	if (client->layout != LAYOUT_NONE) {
		st->play_layout = true;
	} else {
		if (st->play_layout) {
			gi.WriteByte(svc_layout);
			gi.WriteString("");
			gi.unicast(ent, true);
			st->play_layout = false;
		}
		client->ps.stats[STAT_LAYOUTS] |= 1;
	}

	JmpHudKeys(ent, st, keys, false);
}

// Escape with no menu up: true if it ended a playback
qboolean Jmp_PlayEscape(edict_t *ent)
{
	if (!JmpState(ent)->playing)
		return false;
	JmpPlayStop(ent, true);
	return true;
}

// "jmod stop": true if there was a playback or a recording to end
static qboolean Jmp_Stop(edict_t *ent)
{
	jmp_state_t *st = JmpState(ent);

	if (st->playing) {
		JmpPlayStop(ent, true);
		return true;
	}
	if (st->recording) {
		JmpRecStop(ent);
		return true;
	}
	return false;
}

// a stored jump by collection and name, or the last take with no name
static void JmpPlay(edict_t *ent, const char *pack, const char *name, qboolean pov)
{
	jmp_state_t *st = JmpState(ent);
	jmp_take_t *t = &st->take;

	if (ent->deadflag || ent->client->pers.spectator) {
		gi.cprintf(ent, PRINT_HIGH, "This command cannot be used by spectators\n");
		return;
	}
	if (st->recording)
		JmpRecStop(ent);
	JmpPlayStop(ent, false);

	if (name) {
		if (!JmpRead(pack, name, &st->play, true)) {
			gi.cprintf(ent, PRINT_HIGH, "No jump called \"%s\" there on %s: jmod jumps lists them\n",
				name, level.mapname);
			return;
		}
	} else {
		if (!t->count) {
			gi.cprintf(ent, PRINT_HIGH, "Usage: jmod play <[collection/]name> [3rd], or record a take first\n");
			return;
		}
		// a copy: the next recording takes the buffer back
		st->play = *t;
		st->play.samples = gi.TagMalloc(t->count * sizeof(jmp_sample_t), TAG_GAME);
		memcpy(st->play.samples, t->samples, t->count * sizeof(jmp_sample_t));
	}
	JmpPlayStart(ent, pov);
}

// to a stored jump's start, as a spawn: stored, so recall and respawn
// come back to it, and begun with the usual delay
static void JmpGoStart(edict_t *ent, const char *pack, const char *name)
{
	gclient_t *client = ent->client;
	jmp_take_t t;

	if (ent->deadflag || client->pers.spectator) {
		gi.cprintf(ent, PRINT_HIGH, "This command cannot be used by spectators\n");
		return;
	}
	if (!JmpRead(pack, name, &t, true)) {
		gi.cprintf(ent, PRINT_HIGH, "No jump called \"%s\" there on %s: jmod jumps lists them\n",
			name, level.mapname);
		return;
	}
	JmpPlayStop(ent, false);
	if (client->layout == LAYOUT_MENU)
		PMenu_Close(ent);

	JmpStoreStart(ent, &t);
	JmpTakeFree(&t);
	JmpSpawn(ent, JMP_SPOT_STORED, client->resp.jmp_spawn_delay, 0);
}

// jmod start <[collection/]name>
static void Cmd_JumpStart_f(edict_t *ent)
{
	char pack[JMP_NAME_MAX], name[JMP_NAME_MAX];

	if (gi.argc() < 3 || !JmpSplit(gi.argv(2), pack, name)) {
		gi.cprintf(ent, PRINT_HIGH, "Usage: jmod start <[collection/]name>\n");
		return;
	}
	JmpGoStart(ent, pack, name);
}

// jmod play [[collection/]name] [3rd]
static void Cmd_JumpPlay_f(edict_t *ent)
{
	char pack[JMP_NAME_MAX], name[JMP_NAME_MAX];
	int arg = 2;
	qboolean named = false;

	// "jmod play 3rd" is the last take in third person, unless a jump is called that
	if (gi.argc() > arg && (gi.argc() > arg + 1 || Q_stricmp(gi.argv(arg), "3rd"))) {
		if (!JmpSplit(gi.argv(arg), pack, name)) {
			gi.cprintf(ent, PRINT_HIGH, "No jump called \"%s\"\n", gi.argv(arg));
			return;
		}
		named = true;
		arg++;
	}
	JmpPlay(ent, pack, named ? name : NULL, !(gi.argc() > arg && !Q_stricmp(gi.argv(arg), "3rd")));
}

void Jmp_ClientDisconnect(edict_t *ent)
{
	jmp_state_t *st = &jmp_states[ent - g_edicts - 1];

	if (st->playing && st->seen <= level.framenum && st->ghost)
		G_FreeEdict(st->ghost);
	JmpTakeFree(&st->take);
	JmpTakeFree(&st->play);
	memset(st, 0, sizeof(*st));
}

/*
The jumps menu, in the spawnpoint menu's rows: the map's stored jumps a
page at a time, to watch; a pick asks how. Recording is its own row of
the item menu: it asks from where, or first what to do with the take
there is - one running, or the last one while it is not saved.
The jump under the cursor has its description in the rows below it, the
rest of the list making way - the menu is rebuilt as the cursor moves
(JmpJumpsFrame), since its rows are only text to the client.
*/
#define JMP_DESC_ROW	28	// characters of a description on a menu row, after its indent

// how much of a description goes on its first row
static int JmpDescBreak(const char *desc)
{
	int i = strlen(desc);

	if (i <= JMP_DESC_ROW)
		return i;
	for (i = JMP_DESC_ROW; i > 0 && desc[i] != ' '; i--)
		;
	return i ? i : JMP_DESC_ROW;
}
// from where: here, or a spawnpoint off the spawnpoint list, markers and all
static void JmpJumpsRec(edict_t *ent, pmenu_t *p)
{
	ent->client->jmp_menu_rec = true;
	JmpMenuShow(ent, 0, -1);
}

// the take ends where the menu came up, not where its row was picked
static void JmpTakeSave(edict_t *ent, pmenu_t *p)
{
	jmp_state_t *st = JmpState(ent);

	if (st->recording && st->rec_mark >= 2 && st->rec_mark < st->take.count) {
		st->take.count = st->rec_mark;
		st->rec_ms = st->take.samples[st->rec_mark - 1].ms;
	}
	Cmd_JumpSave_f(ent);
}

static void JmpTakeDiscard(edict_t *ent, pmenu_t *p)
{
	jmp_state_t *st = JmpState(ent);

	st->recording = false;
	st->take.count = 0;
	JmpRecUnspawn(ent);
	PMenu_Close(ent);
	gi.cprintf(ent, PRINT_HIGH, "Take discarded\n");
}

static void JmpTakeKeep(edict_t *ent, pmenu_t *p)
{
	PMenu_Close(ent);
}

static void JmpJumpsLast(edict_t *ent, pmenu_t *p)
{
	JmpPlay(ent, "", NULL, true);
}

static void JmpJumpsSave(edict_t *ent, pmenu_t *p)
{
	Cmd_JumpSave_f(ent);
}

static void JmpJumpsPick(edict_t *ent, pmenu_t *p)
{
	JmpState(ent)->menu_pick = (int)(intptr_t)p->arg;
	JmpJumpsShow(ent, JMP_MENU_VIEW, -1);
}

static void JmpJumpsPage(edict_t *ent, pmenu_t *p)
{
	jmp_state_t *st = JmpState(ent);

	st->menu_top = max(st->menu_top + (int)(intptr_t)p->arg, 0);
	JmpJumpsShow(ent, JMP_MENU_JUMPS, p - ent->client->jmp_menu);
}

static void JmpJumpsWatch(edict_t *ent, pmenu_t *p)
{
	jmp_state_t *st = JmpState(ent);
	jmp_take_t t;

	if (st->menu_pick < 0 || st->menu_pick >= jmp_list_count)
		return;
	t = jmp_list[st->menu_pick];
	JmpPlay(ent, t.pack, t.name, p->arg != NULL);
}

static void JmpJumpsStart(edict_t *ent, pmenu_t *p)
{
	jmp_state_t *st = JmpState(ent);
	jmp_take_t t;

	if (st->menu_pick < 0 || st->menu_pick >= jmp_list_count)
		return;
	t = jmp_list[st->menu_pick];
	JmpGoStart(ent, t.pack, t.name);
}

static void JmpJumpsBack(edict_t *ent, pmenu_t *p)
{
	JmpJumpsShow(ent, JMP_MENU_JUMPS, -1);
}

// up from the list: to the collections where there are any, else out
static void JmpJumpsUp(edict_t *ent, pmenu_t *p)
{
	char packs[1][JMP_NAME_MAX];

	if (JmpPacksAll(packs, 1))
		JmpJumpsShow(ent, JMP_MENU_PACKS, -1);
	else
		JmpMenuItems(ent, p);
}

// a row's argument is the collection's place among them all, from 1; 0 is none
static void JmpPacksPick(edict_t *ent, pmenu_t *p)
{
	jmp_state_t *st = JmpState(ent);
	char packs[JMP_PACKS_MAX][JMP_NAME_MAX];
	int i = (int)(intptr_t)p->arg, count = JmpPacksAll(packs, JMP_PACKS_MAX);

	if (i < 0 || i > count)
		return;
	Q_strncpyz(st->menu_pack, i ? packs[i - 1] : "", sizeof(st->menu_pack));
	st->menu_top = st->menu_pick = 0;
	JmpJumpsShow(ent, JMP_MENU_JUMPS, -1);
}

static void JmpPacksPage(edict_t *ent, pmenu_t *p)
{
	jmp_state_t *st = JmpState(ent);

	st->packs_top = max(st->packs_top + (int)(intptr_t)p->arg, 0);
	JmpJumpsShow(ent, JMP_MENU_PACKS, p - ent->client->jmp_menu);
}

static void JmpPackDelAsk(edict_t *ent, pmenu_t *p)
{
	JmpJumpsShow(ent, JMP_MENU_PACKDEL, -1);
}

// out of a collection that is gone, or was not to go
static void JmpPackDelDone(edict_t *ent)
{
	jmp_state_t *st = JmpState(ent);

	if (!JmpPackExists(st->menu_pack))
		st->menu_pack[0] = 0;
	PMenu_Close(ent);
	Jmp_OpenJumpMenu(ent, NULL);
}

// the first yes: enough for an empty one, one with jumps asks again
static void JmpPackDelYes(edict_t *ent, pmenu_t *p)
{
	jmp_state_t *st = JmpState(ent);
	qboolean foreign;

	if (JmpPackWalk(ent, st->menu_pack, &foreign, false)) {
		JmpJumpsShow(ent, JMP_MENU_PACKDEL2, -1);
		return;
	}
	JmpPackDelete(ent, st->menu_pack);
	JmpPackDelDone(ent);
}

static void JmpPackDelAll(edict_t *ent, pmenu_t *p)
{
	JmpPackDelete(ent, JmpState(ent)->menu_pack);
	JmpPackDelDone(ent);
}

static void JmpJumpsDeleteAsk(edict_t *ent, pmenu_t *p)
{
	JmpJumpsShow(ent, JMP_MENU_DELETE, -1);
}

static void JmpJumpsDeleteYes(edict_t *ent, pmenu_t *p)
{
	jmp_state_t *st = JmpState(ent);
	jmp_take_t t;

	if (st->menu_pick >= 0 && st->menu_pick < jmp_list_count) {
		t = jmp_list[st->menu_pick];
		JmpDelete(ent, t.pack, t.name);
	}
	JmpJumpsShow(ent, JMP_MENU_JUMPS, -1);	// read again, without it
}

// saving: the collection picked - a row's text is its name
static void JmpSaveToPick(edict_t *ent, pmenu_t *p)
{
	jmp_state_t *st = JmpState(ent);

	Q_strncpyz(st->ask_pack, p->arg ? p->text : "", sizeof(st->ask_pack));
	st->ask_packed = true;
	JmpSaveNext(ent);
}

static void JmpSaveToNew(edict_t *ent, pmenu_t *p)
{
	JmpAsk(ent, JMP_ASK_PACK);
}

static void JmpSaveToFind(edict_t *ent, pmenu_t *p)
{
	JmpAsk(ent, JMP_ASK_FIND);
}

static void JmpSaveToPage(edict_t *ent, pmenu_t *p)
{
	jmp_state_t *st = JmpState(ent);

	st->save_top = max(st->save_top + (int)(intptr_t)p->arg, 0);
	JmpJumpsShow(ent, JMP_MENU_SAVETO, p - ent->client->jmp_menu);
}

static void JmpSaveToCancel(edict_t *ent, pmenu_t *p)
{
	PMenu_Close(ent);
	gi.cprintf(ent, PRINT_HIGH, "Not saved. \"jmod save\" asks again\n");
}

static void JmpSaveToShow(edict_t *ent)
{
	JmpJumpsShow(ent, JMP_MENU_SAVETO, -1);
}

// a name just given: the menu from its top, the search cleared
static void JmpSaveToOpen(edict_t *ent)
{
	jmp_state_t *st = JmpState(ent);

	st->save_find[0] = 0;
	st->save_top = 0;
	JmpSaveToShow(ent);
}

static void JmpJumpsShow(edict_t *ent, int step, int cur)
{
	jmp_state_t *st = JmpState(ent);
	gclient_t *client = ent->client;
	jmp_take_t *t;
	char secs[16], packs[JMP_PACKS_MAX][JMP_NAME_MAX];
	int i, j, row, lo, hi;

	for (i = 0; i < JMP_MENU_ROWS; i++)
		JmpMenuRow_Set(ent, i, PMENU_ALIGN_LEFT, 0, NULL, NULL);
	JmpMenuRow_Set(ent, 1, PMENU_ALIGN_CENTER, 0, NULL, "%s", jmp_menu_line);

	client->jmp_menu_step = step;
	if (step == JMP_MENU_PACKS) {
		// a row a collection with its jumps, those in none first - the
		// empty ones too, to be looked into and deleted
		JmpListLoad();
		j = JmpPacksAll(packs, JMP_PACKS_MAX);
		JmpMenuRow_Set(ent, 0, PMENU_ALIGN_CENTER, 0, NULL, "*Recorded jumps (%d)", jmp_list_count);

		// a page of them; a fresh look opens on the page of the one last in
		if (cur < 0)
			for (i = 1; i <= j; i++)
				if (!strcmp(packs[i - 1], st->menu_pack))
					st->packs_top = i / JMP_PACKS_PAGE * JMP_PACKS_PAGE;
		if (st->packs_top > j)
			st->packs_top = j / JMP_PACKS_PAGE * JMP_PACKS_PAGE;

		row = 3;
		for (i = st->packs_top; i <= j && row < 3 + JMP_PACKS_PAGE; i++, row++) {
			const char *pack = i ? packs[i - 1] : "";

			JmpListRange(pack, &lo, &hi);
			JmpMenuRow_Set(ent, row, PMENU_ALIGN_LEFT, i, JmpPacksPick, "%-24.24s %3d",
				i ? pack : "In no collection", hi - lo);
			if (cur < 0 && !strcmp(pack, st->menu_pack))
				cur = row;
		}

		row = 3 + JMP_PACKS_PAGE;
		if (st->packs_top > 0)
			JmpMenuRow_Set(ent, row, PMENU_ALIGN_LEFT, -JMP_PACKS_PAGE, JmpPacksPage, "Previous page");
		if (st->packs_top + JMP_PACKS_PAGE <= j)
			JmpMenuRow_Set(ent, row + 1, PMENU_ALIGN_LEFT, JMP_PACKS_PAGE, JmpPacksPage, "Next page");

		JmpMenuRow_Set(ent, JMP_MENU_ROWS - 1, PMENU_ALIGN_LEFT, 0, JmpMenuItems, "Back");
		if (cur < 0)
			cur = 3;
	} else if (step == JMP_MENU_PACKDEL || step == JMP_MENU_PACKDEL2) {
		qboolean foreign;
		int count = JmpPackWalk(ent, st->menu_pack, &foreign, false);

		if (step == JMP_MENU_PACKDEL) {
			JmpMenuRow_Set(ent, 0, PMENU_ALIGN_CENTER, 0, NULL, "*Delete this collection?");
			JmpMenuRow_Set(ent, 2, PMENU_ALIGN_LEFT, 0, NULL, "%s", st->menu_pack);
			JmpMenuRow_Set(ent, 4, PMENU_ALIGN_LEFT, 0, JmpJumpsBack, "No, keep it");
			JmpMenuRow_Set(ent, 5, PMENU_ALIGN_LEFT, 0, JmpPackDelYes, "Yes, delete it");
		} else {
			JmpMenuRow_Set(ent, 0, PMENU_ALIGN_CENTER, 0, NULL, "*It is not empty!");
			JmpMenuRow_Set(ent, 2, PMENU_ALIGN_LEFT, 0, NULL, "%s has", st->menu_pack);
			JmpMenuRow_Set(ent, 3, PMENU_ALIGN_LEFT, 0, NULL, "%d jump%s in it,",
				count, count == 1 ? "" : "s");
			JmpMenuRow_Set(ent, 4, PMENU_ALIGN_LEFT, 0, NULL, "deleted with it.");
			JmpMenuRow_Set(ent, 6, PMENU_ALIGN_LEFT, 0, JmpJumpsBack, "No, keep them");
			JmpMenuRow_Set(ent, 7, PMENU_ALIGN_LEFT, 0, JmpPackDelAll, "Yes, delete them all");
		}
		if (cur < 0)
			cur = step == JMP_MENU_PACKDEL ? 4 : 6;
	} else if (step == JMP_MENU_SAVETO) {
		// the collections a page at a time, all of them or those the search finds
		j = JmpPacksFind(packs, JMP_PACKS_MAX, st->save_find);
		if (st->save_top >= j)
			st->save_top = max(j - 1, 0) / JMP_PACKS_ROWS * JMP_PACKS_ROWS;

		JmpMenuRow_Set(ent, 0, PMENU_ALIGN_CENTER, 0, NULL, "*Save %s to", st->ask_name);
		JmpMenuRow_Set(ent, 3, PMENU_ALIGN_LEFT, 0, JmpSaveToPick, "No collection");
		JmpMenuRow_Set(ent, 4, PMENU_ALIGN_LEFT, 0, JmpSaveToNew, "A new collection...");
		if (st->save_find[0])
			JmpMenuRow_Set(ent, 5, PMENU_ALIGN_LEFT, 0, JmpSaveToFind, "Search: %s (%d)", st->save_find, j);
		else
			JmpMenuRow_Set(ent, 5, PMENU_ALIGN_LEFT, 0, JmpSaveToFind, "Search the %d collections...", j);

		row = JMP_PACKS_FIRST;
		for (i = st->save_top; i < j && row < JMP_PACKS_FIRST + JMP_PACKS_ROWS; i++, row++)
			JmpMenuRow_Set(ent, row, PMENU_ALIGN_LEFT, 1, JmpSaveToPick, "%s", packs[i]);
		if (!j && st->save_find[0])
			JmpMenuRow_Set(ent, JMP_PACKS_FIRST, PMENU_ALIGN_LEFT, 0, NULL, "No collection like that");

		row = JMP_PACKS_FIRST + JMP_PACKS_ROWS;
		if (st->save_top > 0)
			JmpMenuRow_Set(ent, row, PMENU_ALIGN_LEFT, -JMP_PACKS_ROWS, JmpSaveToPage, "Previous page");
		if (st->save_top + JMP_PACKS_ROWS < j)
			JmpMenuRow_Set(ent, row + 1, PMENU_ALIGN_LEFT, JMP_PACKS_ROWS, JmpSaveToPage, "Next page");

		JmpMenuRow_Set(ent, JMP_MENU_ROWS - 1, PMENU_ALIGN_LEFT, 0, JmpSaveToCancel, "Cancel");
		// after a search the cursor is on what it found
		if (cur < 0)
			cur = st->save_find[0] && j ? JMP_PACKS_FIRST : 3;
	} else if (step == JMP_MENU_DELETE) {
		if (st->menu_pick < 0 || st->menu_pick >= jmp_list_count) {
			JmpJumpsShow(ent, JMP_MENU_JUMPS, -1);
			return;
		}
		JmpMenuRow_Set(ent, 0, PMENU_ALIGN_CENTER, 0, NULL, "*Delete %s?", jmp_list[st->menu_pick].name);
		JmpMenuRow_Set(ent, 3, PMENU_ALIGN_LEFT, 0, JmpJumpsBack, "No, keep it");
		JmpMenuRow_Set(ent, 4, PMENU_ALIGN_LEFT, 0, JmpJumpsDeleteYes, "Yes, delete it");
		if (cur < 0)
			cur = 3;
	} else if (step == JMP_MENU_JUMPS) {
		// read again each time it is opened: somebody may have saved one since
		if (!st->menu_keep)
			JmpListLoad();
		st->menu_keep = false;

		// the collection's stretch of the list, a page of it
		JmpListRange(st->menu_pack, &lo, &hi);
		if (st->menu_top >= hi)
			st->menu_top = lo + max(hi - lo - 1, 0) / JMP_LIST_ROWS * JMP_LIST_ROWS;
		if (st->menu_top < lo)
			st->menu_top = lo;

		// the cursor goes to the jump last under it, or the page's first
		st->menu_sel = -1;
		if (cur < 0 && hi > lo)
			st->menu_sel = st->menu_pick = Q_clip(st->menu_pick, st->menu_top,
				min(st->menu_top + JMP_LIST_ROWS, hi) - 1);

		JmpMenuRow_Set(ent, 0, PMENU_ALIGN_CENTER, 0, NULL, "*%s (%d)", st->menu_pack[0] ? st->menu_pack
			: JmpListPacked() ? "In no collection" : "Recorded jumps", hi - lo);

		row = JMP_LIST_FIRST;
		for (i = st->menu_top; i < hi && i < st->menu_top + JMP_LIST_ROWS; i++) {
			t = &jmp_list[i];
			JmpSecs(t->ms, secs, sizeof(secs));
			JmpMenuRow_Set(ent, row, PMENU_ALIGN_LEFT, i, JmpJumpsPick, "%-15.15s %3d fps %5.5ss",
				t->name, t->fps, secs);
			if (i == st->menu_sel)
				cur = row;
			row++;
			if (i == st->menu_sel && t->desc[0]) {
				int len = JmpDescBreak(t->desc);

				JmpMenuRow_Set(ent, row++, PMENU_ALIGN_LEFT, 0, NULL, "%c  %.*s", PMENU_NOTE, len, t->desc);
				if (t->desc[len])
					JmpMenuRow_Set(ent, row++, PMENU_ALIGN_LEFT, 0, NULL, "%c  %.*s", PMENU_NOTE,
						JMP_DESC_ROW, t->desc + len + (t->desc[len] == ' '));
			}
		}
		if (hi == lo)
			JmpMenuRow_Set(ent, JMP_LIST_FIRST, PMENU_ALIGN_LEFT, 0, NULL, "None here yet");

		// under the list and the two rows a description may take
		row = JMP_LIST_FIRST + JMP_LIST_ROWS + 2;
		if (st->menu_top > lo)
			JmpMenuRow_Set(ent, row, PMENU_ALIGN_LEFT, -JMP_LIST_ROWS, JmpJumpsPage, "Previous page");
		if (st->menu_top + JMP_LIST_ROWS < hi)
			JmpMenuRow_Set(ent, row + 1, PMENU_ALIGN_LEFT, JMP_LIST_ROWS, JmpJumpsPage, "Next page");

		if (st->menu_pack[0] && JmpPackMayDelete(ent, st->menu_pack))
			JmpMenuRow_Set(ent, JMP_MENU_ROWS - 3, PMENU_ALIGN_LEFT, 0, JmpPackDelAsk, "Delete this collection...");
		JmpMenuRow_Set(ent, JMP_MENU_ROWS - 1, PMENU_ALIGN_LEFT, 0, JmpJumpsUp, "Back");
		if (cur < 0)
			cur = JMP_MENU_ROWS - 1;
		st->menu_cur = cur;
	} else if (step == JMP_MENU_TAKE && st->recording) {
		st->rec_mark = st->take.count;
		JmpSecs(st->take.count ? st->take.samples[st->take.count - 1].ms : 0, secs, sizeof(secs));
		JmpMenuRow_Set(ent, 0, PMENU_ALIGN_CENTER, 0, NULL, "*Recording, %s seconds", secs);
		JmpMenuRow_Set(ent, 3, PMENU_ALIGN_LEFT, 0, JmpTakeSave, "Save the take...");
		JmpMenuRow_Set(ent, 4, PMENU_ALIGN_LEFT, 0, JmpTakeDiscard, "Discard the take");
		JmpMenuRow_Set(ent, 5, PMENU_ALIGN_LEFT, 0, JmpTakeKeep, "Keep recording");
		if (cur < 0)
			cur = 3;
	} else if (step == JMP_MENU_TAKE) {
		// the last take, not saved: that first, before another is recorded over it
		JmpSecs(st->take.ms, secs, sizeof(secs));
		JmpMenuRow_Set(ent, 0, PMENU_ALIGN_CENTER, 0, NULL, "*Your last take, %s seconds", secs);
		JmpMenuRow_Set(ent, 3, PMENU_ALIGN_LEFT, 0, JmpJumpsLast, "Watch it");
		JmpMenuRow_Set(ent, 4, PMENU_ALIGN_LEFT, 0, JmpJumpsSave, "Save it...");
		JmpMenuRow_Set(ent, 5, PMENU_ALIGN_LEFT, 0, JmpTakeDiscard, "Discard it");
		JmpMenuRow_Set(ent, 7, PMENU_ALIGN_LEFT, 0, JmpJumpsRec, "Record a new one");
		JmpMenuRow_Set(ent, JMP_MENU_ROWS - 1, PMENU_ALIGN_LEFT, 0, JmpMenuItems, "Back");
		if (cur < 0)
			cur = 3;
	} else {
		if (st->menu_pick < 0 || st->menu_pick >= jmp_list_count) {
			JmpJumpsShow(ent, JMP_MENU_JUMPS, -1);
			return;
		}
		t = &jmp_list[st->menu_pick];
		JmpSecs(t->ms, secs, sizeof(secs));
		JmpMenuRow_Set(ent, 0, PMENU_ALIGN_CENTER, 0, NULL, "*%s", t->name);
		JmpMenuRow_Set(ent, 2, PMENU_ALIGN_LEFT, 0, NULL, "by %s", t->author);
		JmpMenuRow_Set(ent, 3, PMENU_ALIGN_LEFT, 0, NULL, "%d fps, %s seconds", t->fps, secs);
		JmpMenuRow_Set(ent, 5, PMENU_ALIGN_LEFT, 1, JmpJumpsWatch, "Watch from the player's view");
		JmpMenuRow_Set(ent, 6, PMENU_ALIGN_LEFT, 0, JmpJumpsWatch, "Watch in third person");
		JmpMenuRow_Set(ent, 8, PMENU_ALIGN_LEFT, 0, JmpJumpsStart, "Spawn at its start");
		if (JmpMayDelete(ent, t))
			JmpMenuRow_Set(ent, 10, PMENU_ALIGN_LEFT, 0, JmpJumpsDeleteAsk, "Delete it...");
		JmpMenuRow_Set(ent, JMP_MENU_ROWS - 1, PMENU_ALIGN_LEFT, 0, JmpJumpsBack, "Back");
		if (cur < 0)
			cur = 5;
	}

	if (client->layout == LAYOUT_MENU)
		PMenu_Close(ent);
	PMenu_Open(ent, client->jmp_menu, cur, JMP_MENU_ROWS);
	st->menu_cur = client->menu.cur;	// where the menu really put it
}

// each frame: the list follows its cursor with the description
static void JmpJumpsFrame(edict_t *ent)
{
	gclient_t *client = ent->client;
	jmp_state_t *st = JmpState(ent);
	int cur = client->menu.cur;
	pmenu_t *p;

	if (client->layout != LAYOUT_MENU || client->menu.entries != client->jmp_menu
		|| client->jmp_menu_step != JMP_MENU_JUMPS || cur == st->menu_cur)
		return;

	p = cur >= 0 && cur < JMP_MENU_ROWS ? &client->jmp_menu[cur] : NULL;
	st->menu_keep = true;
	if (p && p->SelectFunc == JmpJumpsPick) {
		st->menu_pick = (int)(intptr_t)p->arg;
		JmpJumpsShow(ent, JMP_MENU_JUMPS, -1);
	} else if (st->menu_sel >= 0) {
		// off the list: the description goes, the rows under it are where they were
		JmpJumpsShow(ent, JMP_MENU_JUMPS, max(cur, 0));
	} else {
		st->menu_keep = false;
		st->menu_cur = cur;
	}
}

// from the jmod item menu, and "jmod jumps"
void Jmp_OpenJumpMenu(edict_t *ent, pmenu_t *p)
{
	char packs[1][JMP_NAME_MAX];

	if (Jmp_OpenTakeMenu(ent))
		return;

	// the collections first, where there are any
	if (JmpPacksAll(packs, 1)) {
		JmpJumpsShow(ent, JMP_MENU_PACKS, -1);
	} else {
		JmpState(ent)->menu_pack[0] = 0;
		JmpJumpsShow(ent, JMP_MENU_JUMPS, -1);
	}
}

// "Record a jump..." in the item menu: from where - after the take there
// is, if one runs or waits to be saved
void Jmp_OpenRecordMenu(edict_t *ent, pmenu_t *p)
{
	jmp_state_t *st = JmpState(ent);

	if (st->recording || (st->take.count && !st->take.name[0]))
		JmpJumpsShow(ent, JMP_MENU_TAKE, -1);
	else
		JmpJumpsRec(ent, NULL);
}

// any jmod menu while a take runs: save it, discard it, or carry on
qboolean Jmp_OpenTakeMenu(edict_t *ent)
{
	if (!JmpState(ent)->recording)
		return false;
	JmpJumpsShow(ent, JMP_MENU_TAKE, -1);
	return true;
}
