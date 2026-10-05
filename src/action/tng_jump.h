#define JMP_MENU_ROWS	20
#define JMP_SPOTS_MAX	40				// spawnpoints jmod lists
#define JMP_GHUD_MAX	(JMP_SPOTS_MAX * 2)	// a marker and a label each
#define JMP_MARKER_PX	64				// its size on screen
#define JMP_MARKER_HL	96				// the highlighted one's
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
void Cmd_Clear_f (edict_t *ent);
void Cmd_Reset_f (edict_t *ent);
void Cmd_Store_f (edict_t *ent);
void Cmd_Recall_f (edict_t *ent);
void Cmd_Toggle_f(edict_t *ent, char *toggle);
void PMLaserSight(edict_t *ent);
void PMStealthSlippers(edict_t *ent);