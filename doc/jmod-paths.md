# Player pathing: the round-start routes out of a demo

The jump mod can read an `.mvd2` server demo and show how the players
opened their rounds on this map: the route each took from LIGHTS, through
ACTION!, and a few seconds on. It is for studying a map's openings and
running them beside the player who ran them.

## Using it

In a jump mod game (Practice Jumps, or `jump 1`) open the jump menu and
pick **View player pathing...**, or type `jmod paths`.

1. **A demo.** The menu lists the `.mvd2` files in the game's `demos/`
   directory whose name carries this map's name, newest first. `jmod paths
   <file>` loads one by name. A demo of another map is refused.
2. **A player.** Everybody who played a round in it, most rounds first.
3. **Their routes.** One row per route, the rounds in which they ran the
   same route from the same spawn folded into it (`R7 x3`: round 7's
   route, taken three times). The row names the location they reached
   (from the map's `tng/<map>.aqg` location file; the coordinates when
   there is none). Picking a row shows where the route goes and offers:
   - **Draw it on the map** - the route as a line, run from its LIGHTS in
     real time, with the round's other players drawn dim round it
     (teammates blue, the others grey) for context.
   - **Watch its ghost** - the player's ghost runs the route, third person
     or from their view, exactly as a recorded jump plays: the demo's
     model animation included, the keys read off the movement.
   - **All the routes on the map** - the overview.

The **overview** draws every listed route at once, green at LIGHTS
shading to red at ACTION! and darkening after it, with a numbered marker
at each route's end. The markers are buttons, like the spawnpoint ones:
fire on one draws that route alone with its round's company, a second
click within a third of a second sends its ghost. Escape puts the lines
and markers away (a ghost's Escape ends the ghost first).

Console forms: `jmod path <#> [ghost|pov]` for route `#` of the listed
player, `jmod pathmap` to toggle the overview.

`jmod_path_secs` (6) is how many seconds past ACTION! a route is followed,
up to 15. On the client, `jpath_depth 0` draws the lines through walls
and `jpath_lift` raises them off the floor.

## How it is built

- `src/shared/mvdpath.c` reads the demo: the player states' origins,
  view angles and offsets and the player entities' animation frames,
  frame by frame, from each LIGHTS (an `svc_centerprint`) to ACTION!
  plus 15 s. A trail ends early when its player dies. The duplicates are
  folded by `MvdPath_Unique`: the same spawn (64 units) and a mean
  distance under 80 units over the compared span, sampled every half
  second. Only old-protocol demos are read, which is what every AQ2
  server records.
- The game (`src/action/tng_paths.c`) and the client
  (`src/client/jpath.c`) both parse the demo with that reader, so a
  trail's index means the same on both sides. The game drives the menus,
  the markers and the ghost, and tells the client what to draw over
  stuffcmd: `jpath show|play <demo> <frames> <trails>`, `jpath off`.
- The lines are the renderer's debug lines (`src/refresh/debug.c`),
  which this branch compiles into every build rather than debug builds
  only; the client adds them afresh each frame in `CL_AddEntities`.
- The demo's samples come ten to the second; the ghost interpolates
  between them. The LIGHTS frame has the view's angles zeroed by the
  teleport, so the ghost's first sample takes the angles of the third.
