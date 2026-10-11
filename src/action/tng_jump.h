#define JMP_MENU_ROWS	20
#define JMP_SPOTS_MAX	40				// spawnpoints jmod lists
#define JMP_STARTS_MAX	24				// stored jumps' starts listed with them
#define JMP_STARTS_PACKS	4			// collections whose starts are, at a time
#define JMP_POINTS_MAX	(JMP_SPOTS_MAX + JMP_STARTS_MAX)
#define JMP_GHUD_MAX	(JMP_POINTS_MAX * 2)	// a marker and a label each
#define JMP_NAME_MAX	24				// a jump's name, a collection's
#define JMP_PACKS_MAX	64				// collections a map may have
#define JMP_SPOT_JUMPS	1000			// jmp_spawn_spot: from here on, a start in jmp_starts

// where a stored jump begins, as a place to spawn
typedef struct {
	vec3_t	origin;
	float	pitch, yaw;
	qboolean	ducked;
	char	name[JMP_NAME_MAX];
	char	pack[JMP_NAME_MAX];
} jmp_start_t;
#define JMP_MARKER_PX	40				// its size on screen
#define JMP_MARKER_HL	56				// the highlighted one's
#define JMP_MARKER_NEAR	64				// no marker on a spot this close
#define JMP_AIM_TAN		0.045f			// a marker's radius, as the tangent of its angle on screen
#define JMP_DESC_MAX	57				// a jump's description: two menu rows, indented

#define JMP_KEY_FWD		1
#define JMP_KEY_BACK	2
#define JMP_KEY_LEFT	4
#define JMP_KEY_RIGHT	8
#define JMP_KEY_JUMP	16
#define JMP_KEY_DUCK	32
#define JMP_KEYS		6

// a recorded jump: a sample a frame, replayed by a ghost
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
	int		skinnum;	// the ghost's player skin slot, when skinset; else the watcher's
	qboolean	skinset;
} jmp_take_t;

// the jmod menus share the client's rows (jmp_menu): what builds them
extern const char jmp_menu_line[];
void Jmp_MenuRow(edict_t *ent, int row, int align, intptr_t arg,
	void (*func)(edict_t *, pmenu_t *), const char *fmt, ...);
void Jmp_MenuItems(edict_t *ent, pmenu_t *p);
const char *Jmp_GameDir(void);
int Jmp_AimAt(edict_t *ent, const vec3_t *origins, int count);
void Jmp_PlayTake(edict_t *ent, jmp_take_t *take, qboolean pov);

// tng_paths.c: the players' round-start routes out of a demo
#define JMP_MENU_PATHS		20	// the demo to read
#define JMP_MENU_PATH_PLAYERS	21	// whose routes
#define JMP_MENU_PATH_ROUTES	22	// the routes, duplicates folded
#define JMP_MENU_PATH_ROUTE	23	// one of them: watch it, show it
#define JMP_MENU_PATH_FOUND	24	// the demos a player was found in
void JmpPath_Cmd(edict_t *ent, const char *cmd);
void Jmp_OpenPathMenu(edict_t *ent, pmenu_t *p);
qboolean JmpPath_Fire(edict_t *ent, usercmd_t *ucmd);
void JmpPath_Frame(edict_t *ent);
qboolean JmpPath_Escape(edict_t *ent);
qboolean JmpPath_SpawnPick(edict_t *ent, int spot);
void JmpPath_Spawned(edict_t *ent);
void Jmp_AskDrill(edict_t *ent, const vec3_t origin, float pitch, float yaw);
void JmpPath_FindAnswer(edict_t *ent, const char *text);
void Jmp_AskPathPlayer(edict_t *ent);
const char *JmpPath_SpawnTip(edict_t *ent);
void JmpPath_HudCleared(edict_t *ent);
void JmpPath_ClientDisconnect(edict_t *ent);
qboolean GetLocationAt(const vec3_t origin, char *buf);
#define STAT_SPEEDX					1
#define STAT_HIGHSPEED					2
#define STAT_FALLDMGLAST				3

extern char *jump_statusbar;
extern cvar_t *jump;

void Jmp_EquipClient(edict_t *ent);
void Jmp_SetStats(edict_t *ent);

void Cmd_Jmod_f (edict_t *ent);
void Cmd_PMLCA_f (edict_t *ent);
void Cmd_RHS_f (edict_t *ent);
void Cmd_Goto_f (edict_t *ent);
void Cmd_GotoP_f_compat (edict_t *ent, pmenu_t *p);
void Cmd_GotoPC_f_compat (edict_t *ent, pmenu_t *p);
void Cmd_GotoP_f (edict_t *ent);
void Cmd_GotoPC_f (edict_t *ent);
void Jmp_RunSpawn (edict_t *ent);
int Jmp_Spots (edict_t **spots, int max);
const char *Jmp_MarkerPic (void);
void Cmd_SpawnDelay_f (edict_t *ent);
void Cmd_SpawnRepeat_f (edict_t *ent);
void Cmd_SpawnCancel_f (edict_t *ent);
void Cmd_Respawn_f (edict_t *ent);
void Cmd_Respawn_f_compat (edict_t *ent, pmenu_t *p);
void Jmp_OpenSpawnMenu (edict_t *ent, pmenu_t *p);
void Jmp_OpenJumpMenu (edict_t *ent, pmenu_t *p);
void Jmp_OpenRecordMenu (edict_t *ent, pmenu_t *p);
void Jmp_RecordCmd (edict_t *ent, usercmd_t *ucmd);
void Jmp_RecordRestart (edict_t *ent);
qboolean Jmp_PlayThink (edict_t *ent, usercmd_t *ucmd);
void Jmp_PlayFrame (edict_t *ent);
qboolean Jmp_GhostHidden (edict_t *clent, edict_t *ent);
void Jmp_ClientDisconnect (edict_t *ent);
qboolean Jmp_Answer (edict_t *ent);
qboolean Jmp_PlayEscape (edict_t *ent);
void Jmp_MenuFire (edict_t *ent, usercmd_t *ucmd);
qboolean Jmp_MarkersEscape (edict_t *ent);
void Jmp_HudCleared (edict_t *ent);
qboolean Jmp_OpenTakeMenu (edict_t *ent);
void Cmd_Clear_f (edict_t *ent);
void Cmd_Reset_f (edict_t *ent);
void Cmd_Store_f (edict_t *ent);
void Cmd_Recall_f (edict_t *ent);
void Cmd_Toggle_f(edict_t *ent, char *toggle);
void PMLaserSight(edict_t *ent);
void PMStealthSlippers(edict_t *ent);