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