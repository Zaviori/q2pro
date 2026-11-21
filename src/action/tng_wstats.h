// Per-weapon statistics channel (doc/weapon-stats.md): per-weapon totals
// with hit locations, keyed by a server-assigned stats id, sent as
// svc_userstatistic to subscribed AQtion clients and the MVD dummy.

// set when the engine records the channel safely (see tng_wstats.c)
extern void *engine_Stats_Channel;

void WS_Init(void);
void WS_MapStart(void);
void WS_Frame(void);

void WS_RoundStart(void);
void WS_RoundEnd(void);
void WS_MapEnd(void);
void WS_ResetScope(void);

void WS_ClientBegin(edict_t *ent);
void WS_ClientUserinfoChanged(edict_t *ent);
void WS_ClientDisconnect(edict_t *ent);
void WS_Cmd_Get(edict_t *ent);

// counting, called beside the gunstats increments
void WS_Shot(edict_t *ent, int mod);
void WS_Hit(edict_t *ent, int mod, int loc);
void WS_Damage(edict_t *ent, int mod, int damage);
void WS_Kill(edict_t *ent, int mod);
