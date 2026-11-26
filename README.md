Q2PRO extras for AQtion
=======================

The `extras` branch is the AQtion q2pro client (upstream `aqtion`) plus a
set of additions, released as `extras-vN` on
[GitHub](https://github.com/Zaviori/q2pro/releases):

- **TrueType text** - the HUD, chat, console, scoreboard and menus drawn
  in a real font, sharp at any HUD scale.
- **Sharp crosshairs, scope and HUD digits** - the stock pixel-art
  crosshairs redrawn as vectors, the scope as lines, the digits in the font.
- **Stainmaps** - blood, bullet scars and grenade scorch burned into the
  walls.
- **Typing in your own keyboard layout** on Windows.
- **The jump mod**, fleshed out: a *Practice Jumps* menu, spawnpoints with
  markers, delays and drills, **recorded jumps** to watch and practise
  from, and **player pathing** - the routes players ran out of a server
  demo, drawn on the map and run by ghosts.
- **The servers' demos in the demo browser** - every pickup and clan war
  demo on aq2world, listed by month, who played in it, downloaded with a key.
- **A shell-like console** - completion that cycles and finds names
  fuzzily, a fuzzy history search, the editing keys of bash.
- **Playing on a server's version of a map** you have another version of.
- **A scoreboard with weapon stats** - on a server that sends them: every
  player's kills by weapon, and under any player their shots, hits,
  accuracy and hit locations per weapon, for the map or any round.

Everything is a cvar or a menu entry, listed below with its default. The
settings are archived: a value already saved in `config.cfg` overrides a
new default.

For building, consult `INSTALL.md` (`./build-windows.sh` cross-builds the
Windows client); for the client and server in general, the manuals in
`doc/`. How a release is cut is in `doc/extras-release.md`.


Install
-------

It installs **beside** the stock client and overwrites nothing: the client
is `q2prox`, and it looks for its own game library, `gamex86_64x`, before
the stock one. The stock `q2pro` and its `gamex86_64` stay as they are and
keep working. The dedicated server is there too, as `q2prodedx`, beside
the stock `q2proded`: it loads `gamex86_64x` first as well, and the stats
scoreboard needs it (or a local game) on the server's side.

- **Windows:** unzip into the AQtion folder - `q2prox.exe` and
  `q2prodedx.exe` land next to `q2pro.exe`, and `action/gamex86_64x.dll`, `action/fonts/` and
  `action/pics/` go into `action`. Start `q2prox.exe`.
- **Linux:** untar into the AQtion folder the same way (`q2prox`,
  `q2prodedx`, `action/gamex86_64x.so`, `action/fonts/`, `action/pics/`) and start
  `q2prox`. Built on Ubuntu, dynamically linked against the system
  libraries (SDL2, curl, libpng, libjpeg).

Both clients share `q2config.cfg`; the settings this one adds are ignored
by the stock client. The fonts are built into the client as well, so
`action/fonts/` is only needed to replace them; without `action/pics/xhair/` and
`action/pics/glyph/` the crosshairs and the kill report's skull are the
pak's pixel art; without `action/pics/hud_boot.png` and `hud_fist.png`
the stats scoreboard shows the slippers for kicks and nothing for punches.

The version says which release a client is: `AQtion r3755~26984efb~zextras-v12`
in the console banner and the `version` cvar (a build past a release adds
`+N`).


Menus
-----

Everything below can be set in the game: **Settings → Extras**, with a
**TrueType Text**, a **Crosshair**, a **Stainmaps**, a **Jump Mod Keys**
and (on Windows) a **Keyboard** menu. The jump mod's own menus are under
**Practice Jumps** in the main menu.


TrueType text
-------------

Off by default: `r_ttf 1`, or Settings → Extras → **TrueType Text**, turns
it on. In Noto Sans (SIL OFL 1.1, licence in `action/fonts/OFL.txt`), which
is also built into the client: a font missing from `action/fonts/` falls
back to the built-in copy, and a file there comes first, so `r_font` /
`r_font_bold` can name another font.

`r_ttf` (0) is the master switch: `0` is the old 8x8 characters
everywhere. A config that has run an earlier release keeps its saved `1`. The places below have a switch each; every other text -
menus, server browser, inventory - has `r_ttf_menus` (1), and is drawn at
its natural width inside the space the old characters took, so every
layout lines up. Its size is `r_ttf_size` (10), and `r_ttf_menu_size`
(14) in the menus; `r_ttf_cells 1` sets each letter in an old character's
cell instead. Menu and list rows grow to fit the menu text.

Every place has its own switch - `0` brings the old characters back - and
a size, the font's pixel height in HUD units (an old character is 8):

| Switch | Size | What |
| --- | --- | --- |
| `con_notify_font` | `con_notify_size` 12 | the lines over the game - chat, frags, messages - and the `say:` input |
| `scr_chathud_font` | `scr_chathud_size` 12 | the chat HUD (`scr_chathud 1`) |
| `scr_center_font` | `scr_center_size` 16 | centre prints (LIGHTS... ACTION!), outlined |
| `scr_layout_font` | `scr_layout_size` 10 | scoreboard, mod menus, the "Viewing" line: proportional text kept on the mod's columns (numbers and captions line up, captains' and subs' marks follow their team digit) |
| `con_text_font` | `con_text_size` 10 | the console. Its rows grow with the size past 10; `con_scale` scales the whole console |
| `scr_ghud_font` | `scr_ghud_size` 16 | the game's own HUD elements (spectator nameplates, timer); labels it places in the world at `scr_ghud_label_size` 20, bold and outlined |
| `scr_hudnum_font` | `scr_hudnum_size` 38 | the HUD's digits - health, ammo, scores, the jump mod's readouts - in the bold font, outlined; 38 draws a digit the height of the old pictures |

- The charset's icons - the skull and gun of kill reports - are drawn
  inline at the font's size, and its coloured letters (chat's orange
  brackets) keep their colour. An icon can be replaced by a picture of
  its own, `action/pics/glyph/<byte>.png`; `6.png` is the skull, centred
  on its disc, which the pak's sits a pixel high of.
- `con_hitcolors` (1) colours the fight's messages: hits you land blue,
  damage you take red, and players' kill reports ("Enemy Down", or the
  skull or dead face in their binds) cyan.
- `r_font` / `r_font_bold` pick other `.ttf` files from `action/fonts/`;
  `r_font_test 1` draws a sample panel at several sizes, the kill
  report's icons in it.
- Text being typed keeps its spacing, and the loading plaque is text.


Sharp crosshairs, scope and HUD digits
--------------------------------------

The stock crosshairs are 32x32 pixel art, drawn 1:1 at 1080p and blocky
or blurred at any other size; the scope a 640x480 hairline picture; the
HUD's digits 16x24 pictures. Settings → Extras → **Crosshair**:

- `ch_sharp` (1) draws the crosshair from `action/pics/xhair/chN.png` -
  the same twenty designs redrawn as vectors at eight times the size
  (`tools/xhair-gen.py`), mipmapped - identical to the pixel art at 1:1
  and clean at any other size: a cross stays straight-edged, a ring is
  round. The stock scope (`scope2x`, `4x`, `6x`) is drawn as lines in its
  box, a pixel thin as the picture was at 1:1, with the arms' fade taken
  from the art; custom scope packs on servers stay pictures. `0` is the
  pak's pixel art for both.
- `crosshair` (which one, 0-20), `ch_scale` (1), `ch_x`, `ch_y`,
  `ch_hide_while_zoomed` (1) and the `ch_red`/`ch_green`/`ch_blue`/
  `ch_alpha` tint work as before; the sharp set takes the tint too.
- The HUD's digits are the font's under `scr_hudnum_font` (see the table
  above). The weapon and item icons stay pictures; the pak carries 4x
  versions of those already.


Keyboard layout (Windows)
-------------------------

The console, the menus and the chat line used to type as if every
keyboard were American: on a Finnish or German one Shift+2 typed `@`, and
AltGr and dead keys typed nothing. Text comes in the layout Windows has
selected - AltGr (`@ { [ ] } \ | ~`), dead keys and all. The game's
characters have no accents, so `ä` types `a`; a key that types another
alphabet types what it would on a US keyboard. The key left of Z (`<`
`>` `|` on European keyboards) types in the console.

`key_layout` (1), also in Settings → Extras → Keyboard:

- `0` - the old US layout everywhere.
- `1` - typing in the system layout. Binds keep their US key names, so
  nobody's config moves.
- `2` - binds follow the layout too: a key is named by what it types
  unshifted (AZERTY's A key is `a`, the Finnish `+` key is `+`). Keys
  that type no ASCII character, like `ö` `ä` `å`, keep their US names
  (`;` `'` `[`), and the console key stays put. Existing binds on keys
  whose name changes have to be made again.

On Linux the client still types in the US layout.


Stainmaps
---------

Blood, bullet scars and grenade scorch burned into the lightmap
([Bloodstains v1](https://github.com/Zaviori/q2pro/releases/tag/bloodstains-v1)).
On by default - Settings → Extras → **Stainmaps**, or `gl_stainmaps 0` to
turn them off. A config that ran an earlier release keeps its saved setting.

- Stains are made only during a live round (after LIGHTS), or in demo
  playback.
- `gl_stain_scale` (5) - reach of every mark; hits only stain surfaces
  near the body.
- `gl_stain_blood_bright` (0.8), `gl_stain_blood` (210) - blood's tint,
  never brighter than the wall.
- `gl_stain_darkness` (0.15), `gl_stain_floor` (0.35) - how dark marks get.
- `cl_stain_gibs` (1) - gibs and heavy blood leave marks;
  `cl_stain_bullets` (0) - bullet scars.
- `stain <radius> [0-3]` - a test mark at the camera.


Console
-------

**Completion.** TAB cycles through the matches: when a completion is
ambiguous the matches are listed over the console, in columns; TAB again
puts the next one on the line (Shift-TAB the previous), Up/Down step by
one, Left/Right by a column, and past either end the line is the common
prefix again. The chosen one is marked in the list, with `3/94` under
it. Typing anything starts afresh. When nothing starts with what you
typed, the names with its letters in order anywhere are listed instead,
best first, the letters picked out in green: `clfs` finds
`cl_footsteps`. Arguments - maps, demos, cvar values, `jmod`'s
subcommands - get it too. Macros (`cl_fps`, `com_date`, `loc_here`...)
are in the list, marked with a `$` before the name; and a `$` opens an
expansion anywhere on the line - `echo $cl_f` TAB completes the name
after it from the cvars and the macros, `${` likewise. A macro's name
typed alone prints its value, as a cvar's does; `macrolist` completes
its filter.

**History.** Ctrl-R is a bash-like history search, fuzzy: the matching
history lines are listed over the console as fzf lists them - best next
to the prompt, your letters picked out in green, `2/7` under them. Type
to narrow, Up/Down (or Ctrl-R/Ctrl-S, Ctrl-P/Ctrl-N) to move, PgUp/PgDn
a page, Enter runs the line, Escape or Ctrl-G cancels,
Left/Right/Home/End/Tab take it into the line to edit. The console keeps
1024 lines of history (`con_history`) in `action/.conhistory`, saved
after every command, so a crash loses none.

**Keys like a shell.** Ctrl-C drops the line, as bash does: it is shown
with `^C`, not kept in the history, and you get a fresh prompt
(Ctrl-Shift-C copies the line). Ctrl-U, Ctrl-K, Ctrl-W and Alt-D /
Alt-Backspace (a word forward / back) keep what they cut, and Ctrl-Y puts
it back.


Demo browser and aq2world demos
-------------------------------

**The demo browser filters as you type**: the letters go to a filter
shown at the bottom left, the matching directories and demos best first,
the letters matched lit; the filter goes by the map and the players, not
only the file's name. Backspace takes a letter back (and goes up a
directory when the filter is empty), Escape clears it. The players of the
demo under the cursor are listed under its row, by team, and on the
status line. **Shift+Enter** on a server demo starts its map in the jump
mod with the player pathing menu open on it.

**aq2world: the servers' demos...**, the browser's first row, lists the
demos the AQ2 servers upload to the aq2world bucket, a source and a month
at a time, and downloads the one picked into `demos/`, where it plays like
any demo there and the pathing can read it. Only what is picked is
downloaded; a listing is a few kilobytes.

| Key | What |
| --- | --- |
| Left / Right | the month before / after |
| Tab | the next source: `pickup` (vrol), `cw-pickup` (the clan war server), `aq2world_eu` (de.aq2.rocks) |
| letters | filter the month's list, fuzzy, by the demo's name, map and players; Backspace edits, Escape clears |
| Enter | download the demo under the cursor; play it when it is already in `demos/` |
| Shift+Enter | its map in the jump mod with the pathing menu open on the demo |
| F5 | list the month again, whatever the cache has |

The columns are the time played, the teams, the map, the size and whether
it is in `demos/` already (green). The players of the demo under the
cursor go in a box under its row, by team, read out of the file's head.
One download at a time, with its progress on the bottom line; it goes on
if the menu is left. `aq2world <filter>` opens the list with the filter
typed in; `aq2world map <name>` locks it on that map over the last three
months. The command completes with the maps' names. A month's listing is
kept for the session and on disk in `s3cache/`; this month's is asked
for again after an hour, a past month's after a year. `ui_s3demos_url`
is the bucket (`https://aq2world.s3.amazonaws.com`). The details are in
`doc/demo-s3.md`.


Jump mod
--------

AQtion's jump mod, for practising jumps on your own. Everything is a
`jmod` command, so it can be bound to a key; type `jmod` for the list,
and the console completes `jmod` and its subcommands. Keys for all of it
in Settings → Extras → **Jump Mod Keys**. The jump mod is game code:
these work in local games with the included game library
(`gamex86_64x.dll`), and on servers running it.

- **Practice Jumps** in the main menu (and the in-game one): every
  installed map in a list under a search field. Type a few letters to
  filter, fuzzy ("tj" finds teamjungle, "dry" drydock_b1), and Enter
  starts the map in jump mode.
- **Spawn with a delay, or repeat it as a drill.** `jmod spawnp [#]
  [delay] [repeat]` teleports to spawnpoint `#` (0 or none: a random
  one), after `delay` seconds and then again every `repeat` seconds;
  `jmod spawnc [delay] [repeat]` does the same at the spawnpoint closest
  to you. A countdown shows the last three seconds. `jmod delay <secs>`
  sets the default delay, `jmod repeat <secs>` repeats the last spawn,
  `jmod respawn` does the last spawn again (restarting its delay and
  repeat), `jmod stop` ends a pending or repeating one. Spawnpoints
  include the teams' spawns, where rounds start on most maps.
- **A spawnpoint menu** (`jmod spawns`, or Spawnpoints... in the jump
  menu): pick a spawnpoint, then the wait, then the repeat. While it is
  open every spawnpoint is marked in the world, through walls - a ring
  with its number, the one under the cursor in yellow with its location's
  name (`action/pics/jmod_spawn.png`). **Click a marker to spawn there**:
  the marker you aim at lights up and fire acts on it - a click spawns
  there at once, a double click opens its delay and repeat, Shift+click
  starts recording a jump from it. `jmod markers` shows the markers
  without the list; the same key or Escape puts them away.
- `jmod menu` opens the jump menu: the spawns, **Record a jump...**,
  **Recorded jumps...** and **View player pathing...**. `jmod noclip`,
  `jmod laser`, `jmod slippers`, `jmod store` / `jmod recall` (a position
  kept and gone back to) and `jmod goto x y z` work as commands.

### Recorded jumps

Record a jump, save it, and anybody on the server can watch it played
back in the map.

- **Record:** *Record a jump...* in the jump menu asks from where - *Start
  here* or a spawnpoint - counts three seconds down and starts the take.
  `jmod rec` does the same from where you stand, and ends the take. The
  start is stored as `jmod store` does, so `jmod recall` is another go
  from the same spot. A take is two minutes at most.
- **Save:** asks for a line describing the jump, then its name, then
  where to put it, in a box on screen. `jmod save [[collection/]name]
  [description]` does it in one line. A jump can go into a
  **collection** - your own pack of jumps for the map; anybody can add
  to one.
- **Watch:** *Recorded jumps...* lists the map's collections and their
  jumps with frame rate and length. A translucent ghost runs the jump,
  over and over, and your view looks out of its eyes or follows behind
  it - fire switches, Escape stops. The movement keys held light up on
  the HUD, with the name, the author and the `cl_maxfps` it was recorded
  at.
- **Practise:** stopping a playback puts you on the jump's start, facing
  its way; *Spawn at its start* does it without watching. In the
  spawnpoint list every collection has a row: Enter turns it on, and the
  places its jumps start from are marked in the world, in blue under the
  jump's name, to spawn at like any spawnpoint.
- **Delete:** *Delete it...* on a jump's screen and *Delete this
  collection...* under a collection's list. On a server only your own; in
  a local game anything.
- **Share:** jumps are small text files, `action/jumps/<map>/<name>.jmp`,
  a collection a folder there. Everybody on a server sees the server's;
  copy the files to pass them on.
- Console: `jmod play`, `jmod start`, `jmod delete` take
  `[collection/]name`; `jmod stop` ends a playback or a take.

### Player pathing

The jump mod reads an `.mvd2` server demo and shows how the players
opened their rounds on this map: the route each took from LIGHTS, through
ACTION!, and on. It is for studying a map's openings and running them
beside the player who ran them. The details are in `doc/jmod-paths.md`.

In a jump mod game open the jump menu and pick **View player
pathing...**, or type `jmod paths`.

1. **A demo.** The `.mvd2` files in `demos/` with this map's name, newest
   first (`jmod paths <file>` loads one by name). **Download a recent
   pickup game of this map...** opens the aq2world listing locked on the
   map; **Search for player in local demos...** finds the games a player
   is in.
2. **A player.** Everybody who played a round in it, most rounds first.
   Picking one puts the spawnpoint markers up: **Shift+fire on a marker**
   draws the player's routes from that spawn (`jmod pathspawn <#>`, 0
   for every spawn) and sends a **ghost** down each, all setting off
   together from LIGHTS to "LIGHTS... CAMERA... ACTION!", running in a
   loop, with a skull left where a ghost's player died, each ghost's
   name over its head and its weapon's icon while it shoots; a plain
   fire on the marker spawns you there to run beside them. **Fire on a
   route's number** follows its ghost from the player's own view (fire
   for third person and back), with the rest of that round running as
   ghosts in their teams' colours; Escape lets go. **Shift+fire** on a
   number **drills** it: the route stays drawn, its ghost runs dim beside
   you and sets off again every time you spawn, after the spawn menu's
   questions (how long until you spawn at its start, how often again,
   the round's others drawn or running too); Shift+Alt+fire drills it
   with the others running from the start. **Alt+fire** on a number
   shows what the other players did that round, after a question of how
   much of the round to run (15, 30, 45 or 60 s past ACTION!, or to its
   end). `jmod pathmap` draws every route without ghosts.
3. **Their routes.** One row per route, the rounds in which they ran the
   same route from the same spawn folded into it (`R7 x3`), named by the
   location reached. A row offers **Draw it on the map** (the route as a
   line, run from its LIGHTS in real time, the round's others dim round
   it), **Watch its ghost** (third person or from their view, as a
   recorded jump plays, an MVD badge over the keyboard since the keys
   are guessed from the movement) and **All the routes on the map**.

The overview draws every listed route at once, green at LIGHTS shading
to orange up to ACTION! (a white cross), bright red from there on, with a
numbered marker at each route's end; fire on a marker draws that route
alone, a second click within a third of a second sends its ghost. With a
round selected, its kill shots show as a ray from the killer's eye to the
man he killed, the weapon's icon by the shooter, for five seconds after
the kill. Routes and ghosts are drawn through the walls, and stay up when
you noclip out of the world.

| Cvar | Default | What |
| --- | --- | --- |
| `jmod_path_secs` | 6 | seconds past ACTION! a route is shown and run; the routes menu's "Shown to N s past ACTION!" row steps it (3 ... 180, the round's end) |
| `jmod_path_lca` | 1 | 0 silences the "LIGHTS... CAMERA... ACTION!" the ghosts set off to |
| `jpath_depth` | 0 | 1 puts the lines behind the walls again |
| `jpath_cut` | 3 | seconds after a kill of his or his death a player's path is taken down |
| `jpath_kills` | 1 | the kill rays with a round selected; 0 drops them |
| `jpath_look` | 0 | 1 draws each player's shots as the bullets went: faint red to the wall hit, green to the player hit |
| `jpath_lift` | 0 | raises the lines off the floor, in units |


Playing on the server's version of a map
----------------------------------------

A map whose local copy differs from the server's (their checksums
disagree) was an error, and the client's retry - delete the local file,
download again - could do nothing for a map in a pak, which is where
AQtion keeps them. The server's copy can be fetched into `override/`
under the game folder, a search path ahead of the paks, and played on;
the local file is left alone. It needs `allow_download_maps 1`.

`cl_map_override` (0):

- `-1` - never: the mismatch is reported and the connection dropped, as
  before.
- `0` - ask: the console prints which map and both checksums and asks
  you to type `yes` or `no`. The connection waits; `yes` fetches and
  goes on, `no` disconnects.
- `1` - fetch the server's version without asking.

The fetch goes over HTTP first, but AQtion servers advertise the public
asset host as their download server, whose copy is the one you have; so
what arrived is checked against the server's checksum and, if it still
differs, fetched from the game server itself. A map in `override/` is
the one every load of that name gets afterwards - a local `map`, demos,
the pathing - until the file is deleted from `action/override/maps/`.


Stats scoreboard
----------------

On a server that sends the **per-weapon stats channel** - one running this
release's engine and game library, which a local game does - the scoreboard
key (`score`, F1) opens the client's own board instead of the server's.
Anywhere else `score` goes to the server and its own scoreboard shows as
before; `cmd score` opens the server's board on any server.

**Three layouts**, `scr_statsboard_style`, cycled in **Settings > Extras >
Stats Scoreboard** (its **show the board** closes the menu and opens the
board, to see one):

- **0, the dark board** (default): the teams' names, icons and scores,
  the map and the round; per team its players, best score first: score,
  kills, deaths, accuracy (the aimed weapons: no knife slash, kick, punch
  or grenade), damage, K/D, ping and **WEAPONS** - the three weapons they
  got most kills with, as a count and the kill feed's icon. A team's subs
  come after its players under **SUBS**, spectators on a line under the
  teams. In the TrueType font where `scr_statsboard_font` is on.
- **1, the classic board**: the teams side by side like the old AQ2
  scoreboard - name, icon and big score over each team's players (score,
  kills, deaths, accuracy, damage, ping), light grey with dark text.
  See-through (`scr_statsboard_alpha` under 0.5) it takes the old board's
  light letters with a dark edge.
- **2, the server's own**: AQtion's matchmode scoreboard as the server
  draws it, in conchars at its places - team pics, totals, big scores,
  names, subs, Ready / Not Ready.

On all three: who left stays, greyed; a captain has an **@**; who is alive
is in the server board's green where the server's own boards show it (the
dead and spectators, or between rounds; nobody under `limchasecam`).

- **F1** cycles as the server's own key: the board, then the **player
  list** - the server's second page (frags, time, ping, damage, team) with
  **Acc** and the **kills per weapon**, a column per weapon used, its icon
  over it, most used on the left (`scr_statslist_weapons 0` leaves them
  out) - then closed. With a server layout up (a menu, its own board), F1
  puts it away and goes on from there.
- Both open plain: no highlight and no key help. **Enter** shows the
  highlight on you; the **arrows** move it (Up/Down a player, Left/Right
  the other team), **Enter** opens that player's **weapon table** -
  shots, hits, accuracy, headshots, kills, damage and where the hits
  landed (head, helmet, chest, vest, stomach, legs, and no location for
  the shotgun and handcannon; helmet and no location only when there is
  any) - with the key help under it. While it is open the arrows show the
  next player's, **PgUp/PgDn** step it between the map total and each
  round ("live" to a spectator), and "loading rounds" spins while past
  rounds still arrive after joining mid-map. **Esc** closes.
- The thrown knife and the slash have icons of their own.

| Cvar | Default | What |
| --- | --- | --- |
| `cl_statsboard` | 1 | 0 keeps the server's scoreboard everywhere (not saved) |
| `scr_statsboard_style` | 0 | 0 the dark board, 1 the classic light one, 2 the server's own in conchars |
| `scr_statsboard_alpha` | 0.85 | the background's opacity, 0 (none) to 1 (opaque); dark and classic |
| `scr_statsboard_font` | 1 | dark and classic in the TrueType font; 0 the old characters |
| `scr_statsboard_size` | 0 | their text size; 0 the layout's own: 16 classic (about the conchars' size), 10 dark |
| `scr_statsboard_x` | 0.5 | the board's middle, a fraction of the screen's width (dark and classic) |
| `scr_statsboard_y` | 0.1 | its upper edge, a fraction of the height (at most 0.9) |
| `scr_statslist_weapons` | 1 | the player list's kills per weapon; 0 leaves them out |
| `aq2feed` | 1 | asks the server for the channel (userinfo); 0 never |
| `cl_stats_echo` | 0 | 1 prints every line of the channel as `[stat] ...` |
| `aq2events` | 0 | 1 asks for the event lines too (kills, the match's state, chat...): see `doc/weapon-stats.md` |

On the server (`q2proded` and the game library of this release): `wstats`
(1; 0 counts but sends nothing), `wstats_events` (1; 0 no event lines), `wstats_mvd` (1; the stats in recorded
MVDs, which need `sv_mvd_enable 1`) and `wstats_live_ms` (1000; how often
spectators get the round being played, 0 never). The format, and what an
older client or engine does with a demo that has them, is
`doc/weapon-stats.md`.


Other changes
-------------

- **Bots have names.** They took them from name lists (`action/bots/
  males.txt` and the rest) that no install has, and without them were
  named whatever was in memory - garbage, or blank. A built-in list stands
  in now; bots added together no longer all draw the same name, and one
  brought back from the previous map with a garbage name gets a new one.
- **Grenades in the weapon stats.** The grenade's damage counted only the
  hits that did not kill, a direct hit or a held grenade counted under no
  weapon, and its hits were never counted (0% accuracy); and every gun
  kill also counted a kill for grenade impact. All fixed - a grenade's
  accuracy is enemies hit per throw, and can pass 100%.
- **Upstream AQtion v1.3.8**: scoreboards sent to the players a frame at
  a time, ten players a team on them, and a lighter ghud on the wire.

- **No impact markers.** Every bullet impact drew a small red, green and
  blue cross (and with `gl_shaders 0` coloured lines to a corner of the
  map), visible through walls - a debug marker the renderer puts on
  anything whose model is missing. It is drawn only with `gl_showorigins`.
- **Local games always use the newest protocol.** `cl_protocol` set to
  an older one applies to remote servers only.
- **`cl_protocol` is 38 (AQtion) by default**; a server without it gets the
  newest it has. A config setting another one gets a warning on every
  connect: the game's extensions - the ghud, the stats scoreboard - need 38.
- The jump mod's LCA countdown ran four times too fast at 40 server fps,
  and spawning on a map without spawnpoints crashed the server; the
  server's layouts draw over the game's HUD elements; the mismatch error
  printed a model's name for the server's checksum.
- The renderer's debug lines are in every build, not debug builds only.


About Q2PRO
-----------

Q2PRO is an enhanced Quake 2 client and server for Windows and Linux:
a unified OpenGL renderer, ZIP packfiles, PNG/JPEG textures, MD3 and MD5
models, Ogg music and cinematics, HTTP downloads, OpenAL sound, demo
seeking, server side multiview demos and more. For building it, consult
`INSTALL.md`; for using and configuring it, the client and server manuals
in `doc/`.
