/* Stats Command */
void ResetStats(edict_t *ent);
void Stats_AddShot(edict_t *ent, int gun);
void Stats_AddHit(edict_t *ent, int gun, int hitPart);
qboolean Stats_GunTracked(int mod);
int Stats_GunSlot(int mod);
void Stats_AddDamage(edict_t *attacker, edict_t *targ, int mod, int damage);

float CalculateAccuracy(edict_t* ent);
void A_ScoreboardEndLevel (edict_t * ent, edict_t * killer);
void Cmd_Stats_f (edict_t *targetent, char *arg);
void Cmd_Statmode_f(edict_t *ent);
edict_t *find_player_by_steamid(const char* steamid);
