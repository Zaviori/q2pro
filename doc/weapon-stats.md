# AQtion per-weapon statistics channel

Draft. The game module sends each player's per-weapon totals, hit
locations included, to spectators that ask for them and into every MVD,
with the history of every round of the map.

## Why

The broadcast client gets these numbers today by scraping at every
round end: `score` twice for the `LAYOUT_SCORES2` columns
(`sv_parse_scoreboard`, `src/client/screen.c`) and `stats <id>` per
player, weapons recognised by name (`sv_parse_stats_response`). That
works only live - never in a demo or through a relay - breaks when the
mod changes a column or the `scoreboard` cvar moves one, and has no
per-weapon hit locations (the mod counts locations per player only).

## Carrier: `svc_userstatistic`

Command 31 (`inc/common/protocol.h`), already in the AQtion protocol and
never sent: a string and a long. The client reads both and discards
them (`src/client/parse.c`); it will store them instead.

- String: one event, `<event> key=value ...` (below). Long: 0.
- Reliable: `gi.unicast(ent, true)`, ordered with frames and prints.
- **Sent only to AQtion-protocol clients** - an older client drops on
  the unknown command.
- Subscription: userinfo `aq2feed` (1), re-read on
  `ClientUserinfoChanged`. The MVD dummy is always subscribed
  (`wstats_mvd 0` keeps the stats out of the demo). Bots never are.
- **The engine records only the dummy's copy**
  (`filter_unicast_data`, `src/server/mvd.c`): unicasts to players are
  recorded too, so without that every subscriber's copy - and its
  private `sid` token - would land in the demo. **Playback sends it to
  every AQtion-protocol viewer, whoever they chase, and to no older
  one** (`MVD_UnicastSend`, `src/server/mvd/parse.c`): a protocol-36
  viewer dropped with "illegible server message: 31" before.
- The engine says it has both with the game extension
  `Stats_Channel`; **on an engine without it the module sends
  nothing**, since its demos would drop their older viewers.
- One unicast per player per send, holding all their commands: each
  `gi.unicast` takes a slot of the reliable queue (1024, plus 32 KB for
  messages over 46 bytes; `src/server/send.c`) and a full queue drops
  the client. An MVD unicast holds at most 2048 bytes.

## Scope: one map

The server does not know how many maps a match has, so everything here
- counters, history, stats ids - lives for **one map** and is reset at
the next.

## Stats ids

A client number is a slot, and slots are reused: a player who drops in
round 9 and reconnects lands wherever is free, and somebody else may
have their old slot. Stats are keyed by a **stats id** `p` instead,
assigned by the server, never chosen by the client.

On connect the module looks the player up in the map's table, most
reliable first:

1. a **token** it issued earlier, sent back in userinfo `aq2sid`;
2. `steamid` from userinfo, when present and not `0`
   (`find_player_by_steamid`, `tng_stats.c`);
3. IP and name, the ghost rule (`use_ghosts`, `a_cmds.c`).

A hit reuses that `p`; otherwise the next free number (1, 2, 3 …) is
assigned, never reused within the scope. A new `p` comes with a token,
a random 64-bit value sent to that client alone:

```
sid p=3 token=9f2c41d07ab3e655
```

The client keeps it for the map and puts it in `aq2sid`; it survives a name change, a new IP and a
different slot. A `p` that is connected cannot be claimed twice, and
a token from an earlier scope is ignored. The worst a stolen token can
do is file the thief's stats under another id. A restored ghost keeps
its `p`.

Who is where is its own event, sent on every join, reconnect, slot,
name and team change, and in the subscribe snapshot:

```
sidmap p=3 c=6 name="FuBaR:RewinD" team=2
sidmap p=3 c=-1                            ; left
```

The MVD dummy gets every `sidmap` (never a `sid` token), so a demo
reader links ids the same way.

## Counters

Per stats id, per weapon: `gunStats_t` (`g_local.h`) plus locations.
Zero counters are omitted.

| key | counter |
| --- | --- |
| `shots` `hits` `k` `dmg` | shots, hits, kills, damage |
| `hs` | headshots (head or helmet, as `Stats_AddHit` counts them) |
| `head` `chest` `stomach` `legs` | `LOC_HDAM` … `LOC_LDAM` |
| `helmet` `vest` | `LOC_KVLR_HELMET`, `LOC_KVLR_VEST` |
| `noloc` | `LOC_NO` - shotgun, hand cannon, grenade splash |

Each hit has one location, so locations sum to `hits`. `r` is the round
(1-based in the map), `f` the server frame. `w` is the weapon:

| `w` | weapon | `w` | weapon |
| --- | --- | --- | --- |
| 1 | MK23 | 8 | knife, slashed |
| 2 | MP5 | 9 | hand grenade (splash, direct hit, held) |
| 3 | M4 | 10 | knife, thrown |
| 4 | M3 | 11 | kick |
| 5 | handcannon | 12 | punch |
| 6 | sniper | 13 | grenade impact |
| 7 | dual MK23 | | |

1-9 are the mod's `*_NUM`. Friendly fire: a hit on a teammate counts in
`hits` (as the mod's own stats always have) but its damage does not.

## Events

```
wstats p=3 r=17 w=3 shots=41 hits=12 hs=2 k=1 dmg=280 head=2 chest=5 stomach=3 legs=1 vest=1
wstats p=3 scope=map w=3 shots=312 hits=88 hs=14 k=9 dmg=2140 head=14 chest=40 stomach=21 legs=12 vest=1
wlive p=3 r=17 w=3 shots=44 hits=13 dmg=312 chest=6 f=7702
```

**`wstats`** - keyframe, every counter of every weapon used. At round
end the round's lines (`r=`) and then the new map totals of whoever
played it (`scope=map`); at map end and on subscribe every `scope=map`
line - so a late joiner or a demo seek is whole within a round. A
`scope=map` line holds **the rounds that are over**: on subscribe in
the middle of a round it leaves that round out, and a spectator gets
it as `wlive` on top. Map total = `scope=map` + the current round's
`wlive`, until the round's end brings new `scope=map` lines.

**`wlive`** - delta: the counters changed since the last `wlive` or
keyframe, as **current values**, never increments (idempotent; a missed
one is fixed by the next). Coalesced to once a second (`wstats_live_ms`,
1000; 0 = off). Round-scope. Never sent after its round's end; the
round's `wstats` replace it. **Only to spectators (team none) and the
dummy**: a player in the game would learn the enemy's shots live.

The scoreboard's own numbers, to all subscribers and the dummy, on
subscribe and once a second when they change (the ping at most every
5 s):

```
pstats p=3 score=12 k=10 d=8 dmg=2140 ping=41   ; the mod's resp counters
wteam t=1 name="RED" score=7 icon=../players/male/ctf_r_i
```

Framing lines, to all subscribers and the dummy:

```
wscope map=tokyo r=3 going=1            ; first line of a snapshot
wscope map=tokyo r=0 going=0 state=reset ; the match was reset (ResetScores)
wscope map=tokyo r=17 state=end         ; after the map's scope=map lines
wround r=4 state=start f=1041
wround r=4 state=end f=1902             ; after the round's wstats
```

Without teamplay there are no rounds: the map is round 0, `wlive`
runs all map, and only the `scope=map` keyframes come.

## Round history

The module keeps every round's `wstats` for the scope, players who left
included - ~90 KB for 30 rounds of 5v5, a few hundred KB at most. A
client that joins mid-map gets it two ways, both ordinary `wstats`
lines with their `r=`:

- **On request**: `wstats_get <p|*> <r|all>` (client command) answers
  with those lines - ~300 bytes for one player's round - and
  `wstats_get state=done n=<lines>`. Paced like the backfill.
- **Backfill**: after the subscribe snapshot, past rounds newest first,
  at most ~1 KB a frame and only while live traffic leaves room; a
  request jumps the queue. It ends with `wstats_hist state=done r=1-14`.
  ~90 KB is ~20 s at 4 KB/s. Sending it all at once would not fit the
  32 KB of queue a client has (and crowds out the frames).

History is never sent to the MVD dummy: the demo already holds every
round's keyframe from when it happened.

## Grenade counting fixes

Done on this branch. The counters were wrong or empty for grenades;
the channel would have carried the errors. The player's total `resp.damage_dealt` is
right - both `T_Damage` branches add every mod but telefrag - only the
per-weapon slot `gunstats[MOD_HG_SPLASH]` is affected.

| Where | Today | Fix |
| --- | --- | --- |
| `p_client.c:416`, `Add_Frag` | `(mod = MOD_GRENADE_IMPACT)`: every gun kill also adds a kill to grenade impact | `==` |
| `g_combat.c:873`, killing hit | counts `mod < MAX_GUNSTAT` only: the damage of the grenade that kills is dropped (kick, punch, impact too) | the splash/kick/punch/impact branch of the wounding path (`g_combat.c:943`) here too |
| `g_weapon.c:723`, direct hit | `MOD_HANDGRENADE` counted under no weapon | count under the grenade slot |
| held grenade | `MOD_HELD_GRENADE` counted under no weapon | count under the grenade slot |
| hits | never counted - radius damage skips the location code (`g_combat.c:515`), so accuracy is 0 | `Stats_AddHit(attacker, MOD_HG_SPLASH, LOC_NO)` once per victim per explosion, friendly fire excluded |
| `g_weapon.c:854`, throw | `gunstats[MOD_HG_SPLASH].shots++` directly, counted in warmup and between rounds | through `Stats_AddShot` |

`Stats_GunTracked()` and `Stats_GunSlot()` (`tng_stats.c`) are the one
rule now: the guns, grenade (all three mods in the `MOD_HG_SPLASH`
slot), kick, punch and grenade impact; `Stats_AddDamage()` is the one
damage path, both `T_Damage` branches. Your own grenade is no hit and
no damage. Grenade accuracy is victims per throw and can pass 100%.

## Implementation

- `src/action/tng_wstats.c`: the channel. Counters per stats id and
  weapon, its own table (not `resp`, which a disconnect or a ghost
  restore would reset or restore): `tot` the map's, `base` at the
  round's start, `sent` as of the last `wlive`. History records of the
  nonzero round lines. Reset at `SpawnEntities` and by `ResetScores`.
- Hooks: `StartRound`, `WonGame` (round end), `BeginIntermission` and
  matchmode's `SendScores` (map end), `ClientBeginDeathmatch` (bots
  begin there, never through `ClientBegin`), `ClientUserinfoChanged`,
  `ClientDisconnect`, `ClientEndServerFrames` (the frame), the
  `wstats_get` command. Counting rides `Stats_AddShot`, `Stats_AddHit`,
  `Stats_AddDamage` and `Add_Frag`.
- Cvars: `wstats` (1; 0 counts, sends nothing), `wstats_mvd` (1),
  `wstats_live_ms` (1000).
- Engine: the two MVD filters and the `Stats_Channel` extension above.
- Client (`src/client/wstats.c`): userinfo `aq2feed` (**1**) and
  `aq2sid` (set from the `sid` line); `cl_stats_echo 1` prints every
  line as `[stat] ...`.

## The client's scoreboard

Where the server sends the channel - a snapshot has come since the
connect or the map load - the `score` command (F1) opens the client's
own board instead of asking the server for its layout; anywhere else
`score` goes to the server as before, and the server's scoreboard is
the fallback. `cl_statsboard 0` keeps the server's board everywhere.
`scr_statsboard_x` (0.5) is the board's middle as a fraction of the
width, `scr_statsboard_y` (0.25) its upper edge as a fraction of the
height; a board too tall for the room under that climbs instead of
being cut. In the TrueType build `scr_statsboard_font` (1) and
`scr_statsboard_size` (10) set its text.

- The header: team names, icons and big scores, the map and round.
- Per team a band, then its players best score first: score, kills,
  deaths, accuracy (the aimed weapons: no knife slash, kick, punch or
  grenade), damage, K/D, ping, and WEAPONS: the (up to) three weapons
  they got their kills with, most first, as a count and the kill-feed
  icon - from the channel's per-weapon `k`, the round being played
  included. Who left stays, greyed, with "left".
- **Up/Down** walks the players, **Enter** opens the selected player's
  weapons under their row - shots, hits, accuracy, headshots, kills,
  damage and the locations per weapon, each row opening with the
  weapon's kill-feed icon - and closes it again. The locations read
  HEAD, (HELM,) CHEST, VEST, STOM, LEGS, (NOLOC): HELM and NOLOC only
  when a weapon in the table has a count there.
  **Left/Right** steps that table between the map total and each
  round (newest first; the round being played is "live" to a
  spectator). **Esc** or `score` closes the board. Releases pass
  through, so a key held before it opened is let go of.

## Size

| | Bytes |
| --- | --- |
| `wstats` line, with locations | ~95 (~55 without) |
| Round keyframe, 5v5, ~3 weapons each | ~2.9 KB (worst ~9.5 KB) |
| `wlive` | ~45 a line; ~450 B/s peak, 50-150 B/s average |
| History, 30 rounds of 5v5 | ~90 KB, backfilled or on request |

Live, a few KB per round per subscriber and the same in the MVD,
against a game stream of 5-15 KB/s.
