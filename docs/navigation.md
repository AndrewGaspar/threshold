# Navigation verbs

Lab verbs that put the player somewhere for an automated test (`src/features/nav.cpp`,
RE in `re/levels.md`, sections 4, 5 and 9). They exist only in the lab: outside it (no `QUIVER_LAB`)
nothing is hooked and no verb is registered. Every server-side action runs on the game's server
thread, from a hook on the exported `coregame::ServerGameSession::update(float)`. Unless a line
says otherwise, everything here was verified live in lab instance `control-nav`, starting from the
person's save (fixture `game-start`: the very start of the game, `hub` / `Game_Entry_Point`). Runs
r1 to r20 were on the laptop (RTX 5070); runs written `rb-rN` are on rayblaster (RTX 5090), where
in-game lab work now runs (CLAUDE.md; `tools/lab/rb`, `tools/lab/rb-sync`).

## Usage

```sh
export QUIVER_SOURCE=agent:<you>
tools/lab/continue control                         # launch, Continue, skip the intro: returns in-world (~50 s)
quiver lab wait control in-world --timeout 300     # lab state: menu | loading | cinematic | in-world
quiver lab where control                           # {"x","y","z","facing","camera":{...},"level":{"world","episode"},"cinematic":{...},"control",...}
quiver lab teleport control list                   # named locations (fixtures/gog-2049187585/locations.txt)
quiver lab teleport control quarry --timeout 300   # load the episode if needed, skip, jump, place, turn the camera
quiver lab teleport control 131.7 3.15 -257.3 5    # raw teleport (same area only, see caveats)
quiver lab call control episode research SectorLobby skip --timeout 200   # the game's level load; returns once the player has control
quiver lab call control episode foundation JP_HUB  # ... returns at once (state loading, then in-world)
quiver lab call control reload Mail_Room           # the current episode again, at a jump point
quiver lab call control skip                       # skip the cinematic(s) now playing (and those that follow, for up to 30 s)
quiver lab call control jump list                  # the server jump points in the loaded episode (name, x/y/z, yaw)
quiver lab call control jump Mail_Room wait        # the designers' "use jump point" (debug jump), same episode only
quiver lab call control restart                    # restart from the last checkpoint (`restart level` exists, not run live)
quiver lab call control save                       # a checkpoint save now (slot 0)
quiver lab call control nav info                   # hook state, update count, jump point count, level, skip switch
```

- **Lab state** (`%QUIVER_LAB%\state`): `menu` when no server session ticks (main menu), `loading`
  while the engine loads (`coregame::onIsLoading()`), there is no player yet, a load we asked for
  has not started yet, or a `jump` streaming wait is pending; `cinematic` when the player is in the
  world but has no control (`InputManager::isPlayerControlEnabled()` false: the opening cinematic,
  scripted scenes; r8); else `in-world` (the player has control: mouse-look turns the camera, r8).
  The pause menu does not count as cinematic. `nav.cinematic_state = 0` in `threshold.ini` turns
  `cinematic` off (then it reads `in-world`, as before r8).
- **The opening cinematic** (about 2.5 min after Continue from `game-start`) owns the camera. `skip`
  skips it the game's own way: it holds the options byte +0x13a ("skip every skippable timeline",
  read only by the TimelineManager) and also calls the skip on each playing, skippable, server-side
  timeline (at most 4 times each), until no skippable timeline has played for 4 s after one did (or
  30 s pass, `skip for=<s>`); then the byte goes back. **The skip must already be on when the
  cinematic starts**: with it on before the load (Continue, a restart) the intro was skipped 0.25 to
  1 s after its skippable part began (laptop r8, r9, r12, r14; rb-r2, rb-r4); switched on as the
  player appeared it sometimes did nothing at all, the byte nor the direct call (the intro played
  its 2.5 to 4 min: laptop r20 twice, rb-r2, rb-r3 three times). So `tools/lab/continue` calls
  `skip for=120` right after pressing Continue, and `episode ... skip` / `teleport episode=` turn it
  on before they load and off once the player has control. `--watch` (continue) keeps the
  cinematic. Camera after the skip: the lobby rest pose (yaw 179.82, pitch -4.44).
  `nav.skip_cinematics = 1` keeps the byte on for a whole run.
- **`episode <name> [jumppoint] [wait|skip] [world=<w>] [save=1] [via=auto|level|episode]`**: the
  game's own level load, `ServerGameSession::startLoadingLevel(world, episode, jumppoint, false,
  true)`, what fast travel between sectors and the episode transitions call (re/levels.md 9). When
  the target is the save's own checkpoint (`hub` / `Game_Entry_Point` from `game-start`), it uses
  `requestRestartCheckpoint` instead (`via=level` forces the level load): same place, and the
  checkpoint restart is the path the intro skip worked after every time (rb-r2, rb-r4). Episodes: `hub`, `research`,
  `maintenance`, `containment`, `foundation` (DLC 1), `awe` (DLC 2). An empty jump point is the
  episode's startup point. `wait` returns once the load is over and the player has control; `skip`
  also skips the cinematics on the way (use `--timeout 200`). Without either, the call returns at
  once; `quiver lab wait <i> in-world` can then see the old state for a moment, so wait for
  `loading` first or use `wait`. A load takes 6 to 15 s (r10-r14). `save=1` saves first, as the
  episode transition does (no visible difference, r9). `via=episode` uses the old
  `startLoadingEpisode` (black screen; kept for RE).
- **`reload [jumppoint]`**: `episode` for the loaded episode (same options).
- **`teleport x y z [yaw [pitch]] [via=<jp>] [episode=<e> [entry=<jp>]]`**: with `episode` and another
  episode loaded, loads it at `entry` (default `via`), skips the cinematics and waits for control
  (up to 240 s); with `via`, `jump <via>` and its streaming wait; then the exact place. With a yaw it
  calls `TransformComponentState::handleTeleport` (sets position and facing); without, `teleport`
  keeps the current rotation. The camera follows the position but not the facing, so with a yaw the
  answer carries `"want":{"yaw","pitch"}` and `quiver lab teleport` turns the camera with mouse-look.
- **`jump <name> [wait|reload]`**: `useJumpPoint(findJumpPoint(server, name), reload, debug=true)`,
  what Remedy's debug menu does. Only points of the loaded episode (`jump list`). The player is
  hidden while the area streams in (2 to 25 s); the lab state reads `loading` until then.
- **`where`** reports `level` (the world and episode loaded now, r10), `save` (what the loaded save
  recorded; it does not follow loads), `cinematic` (`active`: a timeline plays, ambient ones too;
  `skippable`; `letterbox`), `control`, `menu`, `loading`, `jumping`, `age_ms`.
- **Axes**: Y is up. `yaw` 0 faces +Z and 90 faces +X; the player's `facing` and the camera's
  `yaw` use the same convention, so a location's yaw is both. `pitch` < 0 looks down.
- The lab channel answers one request at a time: while a `teleport`/`episode ... wait` runs (up to
  4 min), `where` and other calls queue behind it (r20).

## Locations

`/home/ajg/Games/quiver-lab/fixtures/gog-2049187585/locations.txt`. Every row carries `via=` and
`episode=`, so it works from anywhere: `quiver lab teleport control <name> --timeout 300`.
**All 26 rows were run with `quiver lab teleport` in rb-r4** (from `game-start` after
`tools/lab/continue`, in an order that changes episode 12 times and returns to the hub from every
other episode): each answered, `where` showed the player at the row with control, and the
screenshots show the described scene. Within the loaded episode a teleport took 1 to 11 s; into
another episode 9 to 16 s; into the hub from another episode 17 to 34 s (restart to
`Game_Entry_Point`, skip, jump). Hub rows go through `Game_Entry_Point` when you come from another
episode (see the black-screen caveat).

| name | episode | what it is for |
|---|---|---|
| `entry-lobby` | hub | entrance lobby, reception desk and flags; polished stone floor (reflections) |
| `directors-office` | hub | Director's office, dark, backlit window |
| `executive-corridor` | hub | bright corridor with red carpet |
| `cafeteria` | hub | cafeteria, tree and red chairs, daylight |
| `upper-offices` | hub | open-plan offices, scattered papers |
| `communications` | hub | outside the Mail Room doors, cool light |
| `mail-room` | hub | Mail Room, pigeonholes, window light |
| `hotline-chamber` | hub | dark, red-lit corridor by the Hotline |
| `engine-room` | hub | industrial hall, stairs (dim) |
| `dead-letters` | hub | Dead Letters, red light, very dark |
| `motel` | hub | Oceanview Motel corridor, strong orange/blue light |
| `astral-plane` | hub | Astral Plane: white sky, dark monoliths |
| `research-lobby` | research | **large space + reflections**: sector atrium, trees, daylight, glass balconies, polished floor |
| `hypnosis-lab` | research | **reflections**: lab hall with glass walls and a glossy yellow-lit floor |
| `hedron-chamber` | research | **volumetrics**: light shafts through haze in a big hall |
| `maze-corridor` | research | hotel-style corridor, patterned walls, warm lamps |
| `quarry` | maintenance | **outdoor / large**: Black Rock Quarry, night sky, cranes, distant floodlight |
| `furnace` | maintenance | **volumetrics + bloom**: stairs up to a blazing furnace, orange haze |
| `watercave` | maintenance | water-cave pump room, pipes, wet floor |
| `maint-control-room` | maintenance | NSC control room, red displays, fluorescent light |
| `transport-chamber` | containment | **large space + reflections**: rail hall with a giant rock, shiny floor |
| `medical` | containment | **reflections**: Medical wing, glass partitions, plants |
| `threshold-lobby` | containment | **volumetrics**: red laser beams and lamps in haze |
| `foundation-hub` | foundation | **large space + fog**: vast cavern, red sand, distant ruins (DLC 1) |
| `foundation-former` | foundation | **volumetric fog**: dense grey fog around floating blocks (DLC 1) |
| `awe-west` | awe | **fog + depth**: long hazy hall with rail tracks and floodlights (DLC 2) |

Earlier checks: hub rows r4; the other rows' jump points r10-r14 (laptop). In rb-r3 (before the
skip-before-load fix) all rows also arrived, but three hub returns waited out the intro (174, 220
and 41 s). **Combat with enemies**: none
found. Encounter points (`JP_Cave_medium_encounter`, maintenance `Aircon_Combat`) are empty without
their triggers (r11, r14: walked in 10 s, nothing spawned), fight-start points crash the game when
loaded into (below), and Central Executive (`Hub_Pyramid`, red Hiss lighting) killed the player in
r4 but not in r14, with no enemy on screen.

## Caveats

- **The hub at a jump point other than `Game_Entry_Point` stays black from the game-start save**
  (r8-r10), player control off, a non-skippable timeline playing. That is the hub's story state
  (mission 1's intro must run), not the load: every other episode loads and renders, and saving
  first, or a `DEBUG_Hub_FreeRoam_Start` debug jump plus a save, does not change it. Use
  `episode hub Game_Entry_Point skip` and then `jump` (what the hub locations do).
- **Story-state jump points can crash the game when loaded into**: `episode research
  Frame_05_Fight_Start` (r11, r12) and `episode containment st03_torment_fight` (r13) both ended in
  an access violation during the load. Use room and control-point jump points (`jump list`).
- **The awe startup point** (`episode awe` without a jump point) killed the player within 5 s and
  the game reloaded the hub checkpoint (r10). Give awe a jump point (`HUB`, `West_AWE`).
- **Loads and jumps write the save.** The game saves each episode's chunk on arrival and jumps run
  `OnJumpToPoint`/`OnDebugJump` scripts (a checkpoint save, the service weapon). Reinstall the save
  for a known start, after deleting the extra chunks `fixture install` leaves behind:
  `rm .../Saves/savegame-slot-00_*; quiver lab fixture install <i> game-start; chmod u+w .../Saves/*`.
- **No save fixtures at other story points** (gap 4): a forced `save` after an episode load writes
  the chunks, but Continue still starts the hub intro (r19): the checkpoint stays at mission 1. So
  `game-start` + `continue` + `teleport` is the way to a scene.
- **Raw teleport only works inside the streamed-in area.** From the lobby straight to Central
  Executive coordinates the player fell through the unloaded floor and died (r1). Use `via=`.
- **Launches sometimes die before the menu** (exit code 0xD15EA5E, r6, r7, r15, r16, r18), mostly
  while other Control instances held most of the GPU memory; `tools/lab/continue` relaunches once.
- Not every spot is safe: Black Rock (maintenance `Black_Rock_Quarry`, red screen, health dropping,
  r11). The area title card covers the first seconds after arriving; settle about 8 s before a
  screenshot, longer for texture streaming after an episode load.
- **Focus**: the "keep ticking unfocused" options byte (+0x406) was already 1 at start, and the
  game's window has the foreground in the lab, so nothing freezes (r1).
- **Never** use the main-menu level select (re/levels.md 5): it deletes save slot 0.
