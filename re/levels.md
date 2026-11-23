# Jumping to scenarios (teleport, jump points, episodes, level select, saves)

Static RE of the GOG Ultimate Edition `Control_DX12.exe` (sha256 `57a8912f…`, same as `re/bin`) and
its Northlight DLLs, done to let lab runs reach varied scenes without playing. Approaches A, B, C
and parts of E were then implemented (`src/features/nav.cpp`, usage in `docs/navigation.md`) and
run live in lab instance `control-nav`, runs r1 to r4 (section 0). Runs r6 to r20 fixed the black
screen after episode loads and the opening cinematic (section 9). Every claim is tagged with how it
was found:

- **export**: a demangled export in `re/exports/*.dem` (the mod can `GetProcAddress` the mangled name)
- **static: exe+RVA** / **static: coregame+RVA**: Ghidra decompile/disassembly (image bases 0x140000000 and
  0x180000000; RVAs are relative to those). I worked on scratch copies of `re/ghidra-stereo-exe`
  (the analysed exe) and `re/ghidra/coregame`, then deleted them. To reproduce, copy those again and use `Serve.java`.
  rizin was used for rl.
- **string**: `strings` on the binary
- **listing**: a file listing of the install or the prefix
- **live rN**: observed in lab instance `control-nav` run rN (Threshold's nav verbs, threshold.log,
  screenshots), starting from the person's save (fixture `game-start`: start of the game)

Signatures are the first 16 bytes at the address. The exe has **no exports**, so its functions have to be
found by RVA in this exact build (check the 16 bytes before calling, and fail closed if they differ). The
coregame and rl functions are exported, so resolve those by name and keep the bytes only as a check.

Side note (listing): `Control_DX12.exe.bak` in the install differs from `Control_DX12.exe` in 97 bytes at
file offset 0xE6215D (.rdata). That is where the two Humble Bundle DLC store URLs were replaced with GOG
URLs. It has no effect on anything below.

---

## 0. Live results (control-nav r1-r4; C and the cinematic redone in r6-r20, section 9)

| # | approach | live result |
|---|---|---|
| A | teleport | **works** (r1, r4): `handleTeleport` / `teleport` on the server avatar from a `ServerGameSession::update` hook. Only inside the streamed-in area: a far raw teleport fell through the unloaded floor and the game reloaded the checkpoint (r1) |
| B | jump points | **works** (r1, r3, r4): 244 registry entries, 122 server-role, at the start of the game; `useJumpPoint(jp, false, true)` streams the area in, then shows the player. 19 distinct points reached, 13 kept as named locations |
| C | episode / reload / restart | `requestRestartCheckpoint` **works** (r2). `startLoadingEpisode` and `startReloadingEpisodeFromJumpPoint` load, but the screen stays black (r2, r3). **Fixed in r10**: the game's own path is `startLoadingLevel(world, episode, jp, false, true)` (section 9); it reaches research, maintenance, containment, foundation and awe, rendered and playable. The hub stays black at any point but `Game_Entry_Point` from the game-start save: that is the hub's story state (the intro), not the load |
| D | main-menu level select | not tried (deletes slot 0) |
| E | saves | slot 0 is flat files `savegame-slot-00_{meta,global,persistent,hub}` (listing, live r3); jumps can trigger a checkpoint save (r3). A forced save does not move the checkpoint: Continue still starts the hub intro (r19), so no save fixtures at other story points (section 9) |

---

## 1. Summary / ranking

| # | approach | robustness | effort | notes |
|---|---|---|---|---|
| A | **Teleport the player**: `TransformComponentState::teleport` / `handleTeleport` on the server avatar | high | low | all exported. The retail debug "Teleport Player" uses exactly this path (§3) |
| B | **Named jump points** in the loaded episode: the exe's jump-point registry plus "use jump point" | high | medium | the designers' own spawn/debug points. Names can be listed at runtime. Exe RVAs (§4) |
| C | **Switch episode at a jump point**: `ServerGameSession::startLoadingEpisode(episode, jumppoint, bool)` / `startReloadingEpisodeFromJumpPoint` | medium-high | low | exported. This is the in-world "level" verb. Valid names come from B, C or D |
| D | **Main-menu level select**: set the developer flag (options+0x130). The menu then builds "CreateLevelMenu" from `menu_levels.json`, or call `UIMainMenu::onLevelSelected(world, episode, jumppoint)` directly | medium | medium | the same function New Game uses. It **deletes save slot 0** first (§5) |
| E | **Save fixtures**: Documents/My Games/Control/Saves/savegame-slot-00 = Continue | medium | low (no code) | no saves exist in the prefix yet. The layout of the files inside a slot is inferred (§7) |
| F | Command line / `CommandLine.txt` | none | n/a | read, but the retail parser defines no game options (§6) |
| G | Remedy debug panel (in-game) | low | high | it exists, but needs debug input enabled. Its "Load World" list is empty in retail (§6) |

---

## 2. Engine model you need (client/server, components)

Northlight runs a local **server** and **client** even in single player. Gameplay state is authoritative on
the server, and the client replicates it.

- `coregame::GameHelper::s_pGameServerBase` / `s_pGameClientBase`: exported data
  (`?s_pGameServerBase@GameHelper@coregame@@2PEAVGameServerBase@2@EA`, `?s_pGameClientBase@…@2PEAVGameClientBase@2@EA`).
  The exe sets both in its game-thread entry (static: exe+0x654840, `*s_pGameServerBase_exref = &local_1688`).
- `GameServerBase::getGameSession()` = `*(ServerGameSession**)(this+0xE8)` (export, static: coregame+0x135d20,
  `48 8b 81 e8 00 00 00 c3`). It is null outside a session (main menu).
- `GameClientBase::getGameSession()` = `*(this+0x140)` (export, coregame+0x135570). `ClientGameSession::getPlayerController()` =
  `*(this+0x6D0)` (export, coregame+0x42840). `ClientPlayerController::getAvatarObject()` (export, coregame+0x1119f0).
- `ServerGameSession::getSinglePlayerAvatarObject()` (export
  `?getSinglePlayerAvatarObject@ServerGameSession@coregame@@QEAAPEAVEntityState@2@XZ`, coregame+0x19bd50,
  `83 b9 a8 00 00 00 00 75 19 83 b9 78 06 00 00 01`). It returns the server player `EntityState*` when
  `*(int*)(this+0xA8)==0 && *(int*)(this+0x678)==1`, and null otherwise. **Live r1**: non-null in the
  world (including cinematics), null for about 7 s around a checkpoint restart or episode load.
  `ServerGameSession::getPlayerController(int)` (export, coregame+0x19c710) followed by `ServerPlayerController::getAvatarObject()`
  is what the exe's debug teleport uses (static: exe+0x2d04a0).
- Exe app object `DAT_14129a640` (exe RVA 0x129a640): `+0x30` GameClientBase*, `+0x38` GameServerBase*
  (static: exe+0x4823c0 and exe+0x65b340 use them that way).
- Components: `r::GameObjectState::getComponentByTypeId(uint)` (rl export
  `?getComponentByTypeId@GameObjectState@r@@QEBAPEAVComponentStateBase@2@I@Z`, rl+0x1b5a00,
  `8b 41 70 44 0f b7 49 62 49 c1 e1 09 4c 03 0d 2d`), with
  `TransformComponentState::getTypeIDStatic()` (export `?getTypeIDStatic@TransformComponentState@coregame@@SAIXZ`,
  coregame+0x1bd0e0, `40 53 48 83 ec 30 48 c7 44 24 20 fe ff ff ff 8b`). `EntityState::getTransform` does exactly this
  (static: coregame+0x127bc0).

**Threads.** The game thread ("RMD - Game Thread", static: exe+0x654840) runs the per-frame tick
`FUN_140366c20` (exe+0x366c20, `40 57 48 83 ec 60 48 c7 44 24 20 fe ff ff ff 48`). That tick drives input, UI and
the client, and brackets the server with `coregame::LocalGameServerRunner::beginFrame/endFrame`. The server
frame is `GameServer::update` (exe+0x401d90, `40 57 48 83 ec 40 48 c7 44 24 20 fe ff ff ff 48`, vtable slot 6
of `GameServer::vftable` exe+0xe2cd00). Each frame it calls the exported, **non-virtual**
`coregame::ServerGameSession::update(float)` (`?update@ServerGameSession@coregame@@QEAAXM@Z`, coregame+0x19b6f0,
`48 89 5c 24 08 57 48 83 ec 30 48 8b 99 70 06 00`) while a session exists. That function is the natural hook for
server-side lab commands (`this` = the session). **Live r1**: hooked with MinHook; it runs only while a session
exists (never in the main menu), on one thread, at well over the frame rate (about 170 calls/s at 1280x720), and the
session pointer stays the same across checkpoint restarts and episode reloads.

**Loading flag** (export): `coregame::onIsLoading` (`?onIsLoading@coregame@@3V?$function@$$A6A_NXZ@std@@A`,
coregame+0x8a0c10) is an MSVC `std::function<bool()>`: impl pointer at +0x38, `_Do_call` at vtable +0x10 (static:
coregame+0xdda51 in `saveGame` calls it that way; this is the "blocking predicate" of section 7). **Live r2**: true from a
load request until about when the avatar reappears (Continue 5 s, restart 7 s, episode load 3.5 to 10 s). Client analogue: `?update@ClientGameSession@coregame@@QEAAXM@Z`
(coregame+0xfff00, `40 53 48 83 ec 30 48 8b d9 0f 29 74 24 20 48 8b`). `GameServerBase::update` and
`GameClientBase::update` are empty shared `ret` stubs (coregame+0xf630, `c2 00 00`), so they **cannot be hooked**.
The engine posts actions across threads with `GameServerBase::sendActionThreadSafe` / a lockless FIFO
(static: coregame+0xdd9f0 `saveGame`).

**Focus.** The game loop only calls the tick when the window has focus **or** the options byte `+0x406` is set
(static: exe+0x654840 main loop: `if (DAT_14129a588 && *(char*)(DAT_14129a588+0x406)) bVar5 = true;` then `if (bVar5) FUN_140366c20()`).
An unfocused lab instance therefore freezes unless that byte is set (see §6 for the options pointer). **Live r1-r4**:
the byte already reads 1 a quarter second after start (before the menu), and the game's window is the foreground
window in the lab's gamescope, so headless runs keep ticking. The mod still sets it (config `nav.keep_ticking`, default on).

---

## 3. Player position and teleport

### Read (`where`)

`TransformComponentState` (the player's transform component; static: coregame+0x127bc0 / +0x1bda70 / +0x1bda80):

| offset | type | meaning |
|---|---|---|
| +0xC0 | float4 | world rotation quaternion (x, y, z, w) (`getWorldRotation`, export, coregame+0x1bda80, `0f 28 81 c0 00 00 00 48 8b c2 66 0f 7f 02 c3`) |
| +0xD0 | float4 | world position (x, y, z, 0) (`getWorldPosition`, export, coregame+0x1bda70, `0f 28 81 d0 00 00 00 48 8b c2 66 0f 7f 02 c3`) |
| +0x168 | pin | `m_out_funcTeleport` output pin, fired by teleport (static: coregame+0x1bdaa0, `FUN_18003a070(FlowConnectionManager, this+0x168, xform)`) |
| +0x180 | u8 | teleport counter, replicated to the client so it skips interpolation (static: coregame+0x1bdaa0 `INC byte [RBX+0x180]`, `onTeleportCounterChanged` coregame+0x1be050 sets the dirty bit at +0x181) |

`GameObjectState` vfunc **+0x290** = `getTransform()` → `Matrix4x3` (12 floats, 3×3 rotation then translation
at byte +0x24) (static: coregame+0x1bdb90 `handleTeleportTo` reads `+0x24` from the result; exe+0x65b340 uses
it on the avatar).

**Axes.** Y is up. `handleTeleport(x,y,z,yawDeg)` builds the quaternion `(0, sin(h/2), 0, cos(h/2))` with
`h = yaw·π/180` (static: coregame+0x1bdaa0: the input is `(0,h,0,h)·0.5`, then sincos, then blendvps with the
mask `ff…ff 00000000` at coregame+0x64ea10, which takes xyz from sin and w from cos). The debug "Drop player"
puts the capsule at `+1.5`/`+0.5` on the second component (static: exe+0x65b340). **Live r1**: yaw 0 faces +Z
and yaw 90 faces +X (`handleTeleport(…, 90)` turned Jesse to screen-left while the camera looked down −Z). Yaw from
a quaternion: `yaw = atan2(2(w·y + x·z), 1 − 2(y² + x²))` (in degrees); it read back exactly the yaw passed in.

**Camera (live r1)**: `g_renderView` (exe+0x12b9f50, re/stereo.md 2.1) view-to-world rows (doubles at +0x70) give the
camera: forward row +0xA0, position +0xB8. `yaw = atan2(fwd.x, fwd.z)`, `pitch = asin(fwd.y)` uses the same
convention as the player's facing (behind-the-shoulder camera: camera yaw = facing within 0.2°). It also follows
cinematic cameras.

The player character has no pitch. Camera yaw/pitch belong to the camera (exported
`?getMatrix@CameraComponentState@coregame@@QEBA?BV?$Matrix4x3Template@M@m@@XZ`. The exe keeps the camera
component states in `DAT_14129a670[DAT_14129a5cc]`, exe RVAs 0x129a670 / 0x129a5cc, static: exe+0x65b340). The
camera/stereo notes cover this. Teleport cannot set camera pitch, because the gameplay camera follows the player.

### Write (`teleport`)

Two exported entry points do the same thing. Both bump the counter, set previous and current world
transform, and fire `m_out_funcTeleport`. The blueprint wires that pin into
`CharacterControllerComponentState::m_in_funcTeleport` → `physics::CharacterController::teleportAbsolute`
(static: coregame+0xc1d90 and its processFlowPins at coregame+0xbfb00, which registers `m_in_funcTeleport` → `teleport`).
The character controller is therefore moved safely, not fighting the transform.

- `?handleTeleport@TransformComponentState@coregame@@QEAAXMMMM@Z`: `void(TransformComponentState*, float x, float y, float z, float yawDeg)`.
  This is the script handler behind the script "Teleport" action. coregame+0x1bdaa0,
  `48 8b c4 53 48 81 ec 90 00 00 00 f3 0f 10 ac 24`. **Use this for `teleport x y z yaw`.**
- `?teleport@TransformComponentState@coregame@@QEAAXVSIMDTransform@m@@0@Z`: `void(this, SIMDTransform const* prev, SIMDTransform const* cur)`.
  The struct is passed by pointer (x64 ABI for 32-byte classes). `SIMDTransform = { float4 quat(xyzw); float4 pos(xyz,0) }`.
  coregame+0x1be560, `48 89 5c 24 08 57 48 83 ec 40 0f 28 02 49 8b f8`.
  Use it to keep the current rotation (copy +0xC0) and change only the position.

The retail **"Teleport Player"** debug button proves the path. The client page (static: exe+0x65b340,
`48 8b c4 55 41 54 41 55 41 56 41 57 48 8d a8 08`) takes the avatar's current matrix, replaces the translation
with sliders X/Y/Z, and sends DevCommandServer RPC id 0x0C. The server handler (static: exe+0x2d04a0,
`48 8b c4 55 48 8d a8 d8 fc ff ff 48 81 ec 20 04`) does
`getPlayerController(caller)->getAvatarObject()->getComponentByTypeId(Transform)->teleport(x, x)`. That runs
on the server inside RPC dispatch. **Do the same from a `ServerGameSession::update` hook.**

Mod recipe (server thread):
```
srv  = *s_pGameServerBase;  ses = srv ? *(ServerGameSession**)(srv+0xE8) : 0;   // or the update hook's this
ent  = ses ? getSinglePlayerAvatarObject(ses) : 0;
xf   = ent ? getComponentByTypeId(ent, TransformComponentState::getTypeIDStatic()) : 0;
xf  && handleTeleport(xf, x, y, z, yaw);
```
**Live r1**: both entry points move the player cleanly (no physics fight); the camera follows the position at once but
keeps its own heading (the lab turns it with mouse-look, `quiver lab turn`, which converged in 1-2 moves on `where`).
Positions at floor height (jump point y) need no offset. A far raw teleport does **not** stream in: from the lobby to
Central Executive (190, −4.04, −215.3) the player fell to y −153, the area title showed, then she died and the game
reloaded the checkpoint. Jump first (§4), then teleport (the mod's `teleport … via=<jump point>`, live r4).

---

## 4. Jump points (named designer spawn points)

Control's levels contain `JumpPointComponent` entities: `m_strJumpPointName`, "JumpPoint:Startup", "JumpPoint:Move
Player Character" (strings). The debug panel also offers them as `world:episode:jumppoint` (static: exe+0x486670). The
exe keeps a **registry of every loaded jump point**:

| exe RVA | what |
|---|---|
| 0x12c4e48 | `JumpPointComponentState**` array (static: exe+0x481c60 loop `for p in DAT_1412c4e48[0..DAT_1412c4e50)`) |
| 0x12c4e50 | u32 count |
| 0x1240460 | recursive lock: SRWLOCK, then +8 owner thread id, +0xC recursion count (static: exe+0x481c60) |

Per entry (a ComponentState):
- `+0x18` qword, bits 62–63 = network role (2 client, 3 server)
- vfunc `+0x2C0` returns its name as `r::String const&` (data `char*` at +0x08, length u32 at +0x18)
- `+0x28` owner `GameObjectState*` (its vfunc +0x290 gives the matrix/position)
- `+0x38` content: `+0x10` bool Startup, `+0x11` bool Move-Player-Character (static: exe+0x481d70 and exe+0x4823c0)

Functions (exe, no exports, so call by RVA after a byte check):
- `findJumpPoint(int role, r::String const* name)`: exe+0x481c60, `40 57 41 56 41 57 48 83 ec 30 48 c7 44 24 20 fe`.
  role: 1 = client, 2 = server. Case-insensitive.
- `findStartupJumpPoint(int role)`: exe+0x481d70, `48 89 5c 24 08 57 48 83 ec 20 8b f9 ff 15 5e 83`.
- `useJumpPoint(JumpPointComponentState*, bool reloadEpisode, bool debugJump)`: exe+0x4823c0,
  `48 8b c4 55 57 41 54 41 56 41 57 48 8d 6c 24 a0`.
  - With `reloadEpisode` on a server jump point it calls `ServerGameSession::startReloadingEpisodeFromJumpPoint(name, !debugJump)`.
  - Otherwise it records the name, posts the script message `OnJumpToPoint` (plus `OnDebugJump` when debugJump), starts a
    **streaming wait** (hides the player, sends the streaming-wait action to client and server), and
    `TransformComponentState::teleport`s every player avatar to the jump point's transform when Move-Player-Character is set.
  - The dev RPC `useJumpPoint` handler (static: exe+0x2d07f0, `40 55 48 8d ac 24 e0 fc ff ff 48 81 ec 20 04 00`)
    calls `findJumpPoint(2, name)` (or the startup one if the name is empty), then `useJumpPoint(jp, <rpc bool>, true)`.
  - Episode start (static: exe+0x65fc90) calls `useJumpPoint(jp, false, …)` on the requested or startup point.

So: **`jump list`** = lock the registry, keep server-role entries, print name and position. **`jump <name>`** =
on the server thread, `useJumpPoint(findJumpPoint(2,name), false, true)`, which is what the debug menu's jump does.

**Live r1-r4**: the layout above is right (name via vfunc +0x2C0, owner +0x28 whose Transform gives the position,
+0x38 flags: every listed point has Move-Player-Character, only `Hub_Pyramid` is Startup). At the start of the game
(lobby, intro cinematic) the registry already holds 244 entries, 122 server-role, spread over the whole hub episode
(Executive, Mail Room, Cafeteria, Dead Letters, Engine Room, Motel, Astral Plane…), so it is not limited to the
player's surroundings. `useJumpPoint` teleports the avatar at once, hides it, and shows it again when the area has
streamed in, 5 to 15 s later; **completion is `*(u8*)(jp+0xB8) & 3 == 0`** (the bits set at `param_1[0x17] |= 1 / |= 2`,
live r4). The jump runs the point's scripts (`OnJumpToPoint`, `OnDebugJump`): after a few jumps Jesse holds the
service weapon, and a checkpoint save was written (r3). Jumps right after Continue or a restart (during the intro
cinematic) render too, but the hub is then in another state (red-lit Executive corridor, the player taking damage; r4);
`DEBUG_Hub_FreeRoam_Start` during the intro left the screen black (r2). `Hub_Pyramid` (Central Executive) killed the
player within about 4 s from the game-start save (r4, twice). Building the name argument: an `r::String` (rl export
`??4String@r@@QEAAAEAV01@PEBD@Z` = `operator=(char const*)`). Layout from static use: +0x00 buffer ptr, +0x08 data
ptr, +0x10 u32 capacity, +0x14 u32 capacity/flags, +0x18 u32 length, inline buffer at +0x20 for `InplaceString<N>`
(every `InplaceString<64>` in these functions is initialised as `{buf, buf, 0x40, 0x40, …}`; rl `String::operator=(char const*)`
rl+0x15d780 copies with `strcpy_s(data, cap@+0x14, src)` and sets the length at +0x18). **Live r1-r4**: a hand-built
`InplaceString<64>` (`{inline, inline, 64, 64, len}`, text at +0x20, 0x60 bytes) works for `findJumpPoint`,
`startLoadingEpisode` and `startReloadingEpisodeFromJumpPoint`.

---

## 5. Episodes, worlds, level select

### In-world: switch or reload episode (exported, server)

- **Use `startLoadingLevel` instead (section 9)**: the two functions below lead to the program flow's
  `LoadingJumpPoint` states (modes 5/6) that no game code reaches from the exe (it does not even
  import `startLoadingEpisode`), and leave the screen black.
- `?startLoadingEpisode@ServerGameSession@coregame@@QEAAXAEBV?$InplaceString@$0EA@@r@@0_N@Z`:
  `void(ServerGameSession*, InplaceString<64> const& episode, InplaceString<64> const& jumpPoint, bool)`. It sends
  `CoreProgramFlow::ActionStartLoadingEpisode` with (episode, jumppoint, flag ? 1 : −1) and mirrors it to the client by RPC.
  coregame+0x19b8c0, `40 55 53 56 57 48 8d 6c 24 e8 48 81 ec 18 01 00`. **This is the `level <episode> [jumppoint]` verb**
  for use in-world. The meaning of the bool is unverified (the same int is "1 / −1" in ProgramFlowLoadingStateParameters
  at +300, possibly a save-slot index). **Live r2** (`episode hub Mail_Room`, flag 0): loads in 3.5 s and puts the
  player at the jump point, but the screen stays black (from the game-start save).
- `?startReloadingEpisodeFromJumpPoint@ServerGameSession@coregame@@QEAAXAEBV?$InplaceString@$0EA@@r@@_N@Z`: reloads the current
  episode starting at a named jump point. coregame+0x19ba00, `40 57 48 81 ec f0 00 00 00 48 c7 44 24 28 fe ff`.
  **Live r2, r3**: reloads in 6 to 10 s with the player at the point (or at `Hub_Pyramid`, the startup point, for an empty
  name), but the screen stays fully black (no HUD either; the pause menu still draws), with flag 0 or 1, from gameplay or
  from the intro, and also through `useJumpPoint(jp, true, true)`. Only `Game_Entry_Point` recovers: the intro cinematic
  plays again. The fade setter exe+0x3b1e10 (`SetFade` / `setFadeParams`) was hooked in r3 and never called, so the black
  is not that fade. Most likely the game-start narrative state expects the intro to fade the world in; untested from a
  later save.
- `?requestRestartCheckpoint@ServerGameSession@coregame@@QEAAXXZ` (coregame+0x41b70, `40 53 48 83 ec 20 48 8b d9 48 8d 15 84 02 86 00`)
  and `requestRestartLevel` (coregame+0x41b30, `40 53 48 83 ec 20 48 8b d9 48 8d 15 c8 02 86 00`): menu-equivalent restarts.
  **Live r2**: `requestRestartCheckpoint` reloads the checkpoint in 7 s and renders (the intro cinematic, from the
  game-start save). `requestRestartLevel` was not run.
- Program flow action names (export `coregame::CoreProgramFlow::Action*`): ActionRequestLoadWorld, ActionStartLoadingWorld,
  ActionStartLoadingEpisode, ActionStartReloadingEpisode, ActionDebugPageLoadWorld / ReloadEpisode / LoadCheckpoint / SaveCheckpoint,
  ActionRestartCheckpoint, ActionRestartLevel, ActionRequestCheckpointSave.

### Names

- World/episode/jump point triples are written `world:episode:jumppoint` (string `%s:%s:%s`, static: exe+0x486670).
  The packfile index paths show the world **`gameworld`** with `hub`, `hub_mission_01_the_bureau`, `hub_mission_02_the_hotline`,
  `hub_persistent` (listing: `strings data_packfiles/ep100-000-generic.packmeta`. These may be layers rather than episodes).
- Save chunk names give the sector episodes: **`hub`, `maintenance`, `research`, `containment`, `foundation` (DLC1), `awe` (DLC2)**
  (table at exe+0xdb6078, used by the slot copier exe+0x1f5ee0; strings). These are the most likely episode names.
  **Live r2**: the save manager reads world `gameworld`, episode `hub`, jump point `Game_Entry_Point` for the game-start save,
  and the slot holds a `_hub` chunk, so `hub` is an episode name. **Live r10**: `research`, `maintenance`, `containment`,
  `foundation` and `awe` all load with `startLoadingLevel` (listing: they are also the `gameworld\<episode>` folders of
  `ep101-000-generic.packmeta`; `hub` is in `ep100`). Their jump point names: section 9.
- The full menu list is in `data/globaldb/worlds/menu_levels.json` (inside the .rmdp packs, not extractable here). Its format,
  parsed by `UILevelsModel::createLevelDefinition` (export, coregame+0x1d4890):
  `{"m_levels":[{"<world>":[ "<episode>" | ["<episode>","<jumppoint>"], … ]}, …], "m_startLevel":[…]}`.
  `UILevelsModel::loadLevelDefinitions(InplaceString<256> const& path, Vector<LevelDefinitions>&, LevelDefinitions& start)`
  (export `?loadLevelDefinitions@UILevelsModel@coregame@@QEAA_NAEBV?$InplaceString@$0BAA@@r@@AEAV?$Vector@ULevelDefinitions@UILevelsModel@coregame@@V?$Allocator@ULevelDefinitions@UILevelsModel@coregame@@$0A@@r@@@4@AEAULevelDefinitions@12@@Z`,
  coregame+0x1d4520, `40 56 57 41 56 48 81 ec 30 01 00 00 48 c7 44 24`) does `FileUtils::fileExists`, then `JSONDocument::loadFile`.
  **Hook it and log what it parses** to get the real list on the first menu visit. `LevelDefinitions` is 0x70 bytes:
  world `InplaceString<64>` (0x60), then `Vector<LevelStartPoint>` at +0x60 (ptr) / +0x68 (count). `LevelStartPoint` is 0xC0 bytes:
  episode `InplaceString<64>` + jumppoint `InplaceString<64>`.
- The menu first tries **`data/globaldb/worlds/menu_levels_override.json`** and falls back to `menu_levels.json`, or the
  `_demo_press` / `_demo_showfloor` variants when the demo mode global `DAT_14129a6c8` (exe RVA 0x129a6c8) is 2 or 1
  (static: exe+0x61b680). If loose files under `<install>/data/` override packs (the install has a loose
  `data/shaders`, listing), dropping an override JSON there would define our own level list. **Unverified.** The lab
  could test it, but must not write into the user's install.

### Main menu: level select / new game (exe)

`UIMainMenu` (Coherent UI page, `uiresources\p7\menu.ui`). Populate function exe+0x61b680
(`48 8b c4 55 41 54 41 55 41 56 41 57 48 8d a8 e8`, `param_1` = UIMainMenu*. Hook it to capture the menu pointer):
- `*(char*)(options+0x130)` (the developer flag, §6) gates `CreateLevelMenu` (the level list built from the JSON above) and is
  passed to the UI as `m_bIsDeveloper` (static: exe+0x61b680 `cVar3 = local_1e8 = *(DAT_14129a588+0x130)`, string `m_bIsDeveloper`).
- Handlers bound with `ui::Page::bind` (static: exe+0x61a9b0):
  - `OnLevelSelected(world, episode, jumppoint)` → exe+0x61c5d0 `void(UIMainMenu*, String const&, String const&, String const&)`,
    `48 89 5c 24 10 48 89 6c 24 18 56 41 56 41 57 48`. It **deletes container `savegame-slot-00`** (exe+0x1f7570,
    `40 57 48 81 ec a0 01 00 00 48 c7 44 24 28 fe ff`), resets the save manager (exe+0x3f06e0), then fires the delegates at
    UIMainMenu+0xD0 with `(world, episode, jumppoint, true)`. These reach the menu program-flow state, which builds
    `GameSessionParameters::startSinglePlayerLevel(world, episode, jumppoint, …)` (export, coregame+0x14bca0; static: exe+0x53db10).
  - `OnNewGameClicked` → exe+0x61c0d0: deletes slot 0, then calls the same exe+0x61c5d0 with the **start level**
    (UIMainMenu+0x238 world, `*(UIMainMenu+0x298)` episode, `+0x60` jumppoint, `+0x2A0` count). **New Game is level select
    with the JSON's `m_startLevel`.**
  - `OnContinueGameClicked` → exe+0x61c120: checks the PlayGo availability of the saved episode, then fires the delegates at +0x110.
  - `OnMissionSelected(slot)` → exe+0x61c660: loads slot N's `global` chunk (exe+0x1f6600), then fires the delegates at +0xF0 with
    (slot, world = save manager +0x288, episode = +0x2C8, jumppoint = +0x308).
- Save manager global `DAT_14129a6e0` (exe RVA 0x129a6e0): `+0x74` current slot index; `+0x288/+0x2C8/+0x308` world/episode/jumppoint
  of the loaded save metadata. **Live r2**: right offsets, but they are `InplaceString<32>` (capacity 0x20 at +0x10/+0x14,
  length at +0x18, inline text at +0x20, stride 0x40). They keep the loaded save's values after jumps and saves.

So `level <world> <episode> [jumppoint]` **from the main menu** = call exe+0x61c5d0 with the captured UIMainMenu on the game thread.
This mirrors AWR, where the level verb also had to deal with the checkpoint slot. It wipes slot 0, so restore fixtures afterwards.

---

## 6. Developer mode, command line, debug panel

### Options struct and developer flag

- The game-thread entry exe+0x654840 (`40 57 41 56 41 57 b8 50 18 00 00 e8 50 2a 69 00`) builds a stack options struct
  (init exe+0x2a7dc0, defaults exe+0x2a80c0) and stores its address in **`DAT_14129a588` (exe RVA 0x129a588)**.
  - `+0x130` u8 **developer** flag, default 0. `coregame::onIsDeveloperMode` (export, a `std::function<bool()>` at
    coregame+0x8a0c50) is set to a lambda that returns exactly this byte (static: exe+0x364060 assigns
    `_Func_impl<lambda_4fe93ea8…>`; its invoke at exe+0x367f60 is `48 8b 05 21 26 f3 00 0f b6 80 30 01 00 00 c3`. The pattern
    `48 8b 05 ?? ?? ?? ?? 0f b6 80 30 01 00 00 c3` is **unique in the exe**, findbytes, so it also locates the options pointer
    by RIP-relative decode). `coregame::GameHelper::isDeveloperMode()` (export, coregame+0xdd8c0, `48 83 ec 28 48 8b 0d bd 33 7c 00 48 85 c9 74 0f`) calls it.
    Other readers: the main menu level list, debug mouse picking (exe+0x2bbfa0, together with `InputManager::isDebugButtonsOn`),
    the dev overlay text (exe+0x46f380), and renderer debug settings (exe+0x367990).
  - `+0x406` u8: keep ticking when unfocused (main loop, see §2). `+0x410` int processor limit, `+0x420`/`+0x430` text/voice
    locale overrides, `+0x124` sound flag, `+0x139`/`+0x13B` skip the start videos (all static: exe+0x654840 / exe+0x364060).
  - No code writes `+0x130` other than the default dword write in exe+0x2a80c0 (findbytes `c6 80 30 01 00 00`: no hits; the
    `c6 81 30 01…` / `88 ?? 30 01…` hits are other structs).
- **A mod enables developer mode by writing `*(u8*)(*(void**)(exe+0x129a588) + 0x130) = 1`** once the pointer is non-null.
  The pointer is set at the start of the game thread, before the menu. What else the flag unlocks is not fully mapped.
  Test that it doesn't break saves or achievements.

### Command line / CommandLine.txt

- exe+0x654840: when the real command line is empty, the game reads `<exe dir>\CommandLine.txt` (string `\CommandLine.txt`,
  at most 255 bytes, CR/LF become NUL, so effectively only the **first line** counts), then constructs `r::CmdLineParser(cmdline)`.
- The retail binaries register **no game parameters**. The exe imports only `CmdLineParser::CmdLineParser(char const*)`,
  `~CmdLineParser` and `suppressAppReporterErrors` (rizin `ii` on the exe and every `*_rmdwin10_f.dll`). The only parameter
  constructed inside rl is `paramsfile` ("File containing command line parameters, one per line.", static rl+0x1ef00). The
  options struct keeps its defaults. **There are no `-level`, `-checkpoint`, `-skipintro` or dev switches in this build.**
  (`Control.exe` is the GOG launcher that picks DX11 or DX12; not analysed.)

### Remedy debug panel

Present in retail (strings, static). `DebugPanel::MainPage` (builder exe+0x4860c0, vtable exe+0xe3a0c0) has Drop player,
Player Invulnerable/Immortal, Kill/Resurrect, Kill all enemies, One shot kills, Reload Level/Episode, Load Next Episode,
Unload World, "Save Checkpoint TO DISK" / "Load Checkpoint FROM DISK", screenshots (back buffer, high-res, depth, cube map),
DVR save, Teleport X/Y/Z + "Teleport Player", and "Load World". Other pages: Free Camera (with FreeCam save/load slots),
Render Options, World Testing, Trials, Unlocks and more (strings). Button presses become per-id counters at `page+0x178`. They
are consumed in the client debug update (exe+0x65b340) and the dispatcher (exe+0x2bba30), which sends DevCommandServer RPCs:
debugTeleport (0x0C), debugPageLoadWorld (0x05, exe+0x2d2c00), useJumpPoint, episodeControllerLoadEpisode / JumpToTask /
LoadTask / SetTaskComplete (strings `DevCommandServerRPCBase::*Params`).
- "Load World" lists `File::findSubfolders("data\\worlds")` and `data\worlds\<w>\episodes\<e>\JumpPoints.txt` from **loose files**
  (static: exe+0x486670), so it is **empty in retail** (no loose `data/worlds`, listing).
- Opening the panel needs `input::InputManager::isDebugButtonsOn()` (exe+0x2b2b90). `input::InputManager::setDebugButtons(bool)`
  is exported from input_rmdwin10_f.dll but not imported by the exe. Which key opens the panel was **not mapped**. This is
  low value: everything useful in it (teleport, jump points, episode load) is reachable directly (§3–§5).

---

## 7. Save games (fixtures)

- Location (listing): `C:\users\steamuser\Documents\My Games\Control\Saves\` in the prefix
  (`/home/ajg/Games/ubi-prefixes/control/drive_c/users/steamuser/Documents/My Games/Control/Saves/`). It currently holds only
  `preferences_data` (764 bytes, written 14:36; contains `default`, `en`, a GUID and key bindings). **No save slots exist yet**,
  so the user has not reached a checkpoint. The path comes from `r::PlatformSpecific::getMyDocumentsPath` (rl export) plus
  platform storage. The literal folder string is not in any binary (built at runtime).
- Storage model (platform_rmdwin10_f exports + exe statics): `platform::storage` **containers** named `savegame-slot-%02d`
  (string; exe+0x1f4850 … exe+0x1f7390), each holding **chunks**:
  `meta` (exe+0x1f7390, `SaveGame::MetaData`), `global` (exe+0x1f6600), `persistent`, and one chunk per sector episode
  `hub` / `maintenance` / `research` / `containment` / `foundation` / `awe` (exe+0x1f66f0 writes persistent + global + the
  lower-cased current sector). The platform DLL has the string `.chunk`, so on disk it is most likely
  `Saves\savegame-slot-NN\<chunk>.chunk`. **Inferred, verify** once a save exists.
- Slots: **slot 0 is the live checkpoint save**. New Game and level select delete slot 0 (exe+0x1f7570). Continue resumes the
  save the menu has loaded (slot 0). Mission saves: on mission completion the game copies slot 0's chunks into slot N =
  `DAT_141239054` (exe RVA 0x1239054, set from the content's `m_iCompletionSaveSlot`; static exe+0x3a79e0, exe+0x1f5ee0).
  Post-game "mission select" (`MissionSaveSlot`, `m_bHasMissionSaves`) loads slot N (exe+0x61c660).
- Saving on demand: `?saveGame@GameHelper@coregame@@SAXW4NetworkRole@net@@_N1@Z`, `saveGame(role, threadSafe, force)`
  (coregame+0xdd9f0, `48 89 5c 24 08 48 89 6c 24 10 48 89 74 24 18 57`). Use role 2 (server). `threadSafe=true` pushes
  `ActionRequestCheckpointSave` into the server's lockless FIFO, so it is safe from any thread. `force=true` skips the 20 s
  throttle. A blocking predicate (`std::function` at coregame+0x8a0c48) can still defer it.
- **Live r3** (listing of the lab prefix): slot 0 is four flat files in `Saves\`, `savegame-slot-00_meta` (32 bytes),
  `_global`, `_persistent` and `_hub` (one per episode visited), not a folder of `.chunk` files. A jump survey rewrote all four
  (a checkpoint save during jumps). The game-start save is fixture `game-start` (`quiver lab fixture install <i> game-start`;
  it installs the files read-only, so `chmod u+w` them before playing on).
- Fixture use: copy the `savegame-slot-00_*` files into the lab prefix, then press Continue. AWR showed that read-only
  fixture files break later saves, so copy them writable. Because `level` from the menu deletes slot 0, install fixtures again
  after using it.

---

## 8. Recommended approach for Threshold

Hooks (all in-process, after the DLLs load):
1. **Server tick**: inline-hook the exported `coregame::ServerGameSession::update(float)`. Drain a queue of server commands
   (teleport, jump, episode, save) there.
2. **Game tick**: hook exe+0x366c20 (byte-checked) for menu/UI commands, and use it to publish a lab state
   (`menu` / `loading` / `in-world`).
3. **Menu capture**: hook exe+0x61b680 to remember the `UIMainMenu*` (and optionally `UILevelsModel::loadLevelDefinitions`
   to log the level list).
4. At startup set options `+0x406 = 1` (ticks while unfocused). `+0x130` (developer) only behind a config switch.

Lab verbs:
- `where` → `{x,y,z,yaw}` from the server or client avatar's TransformComponentState (+0xD0 pos, +0xC0 quat → yaw), plus the episode/jump
  point last used (record what `useJumpPoint` / our calls set; save manager +0x2C8/+0x308 as a fallback), plus camera matrix from the
  camera notes.
- `teleport x y z [yaw]` → `handleTeleport(xf, x, y, z, yaw)` on the server thread. Without yaw, use `teleport(prev=cur=xf with the new pos)`
  and keep the quaternion from +0xC0.
- `jump list` / `jump <name>` → registry at exe+0x12c4e48 (lock 0x1240460), `findJumpPoint(2,name)` + `useJumpPoint(jp,false,true)`.
  This is the most "designed" way to reach a scene, because the streaming wait and script `OnJumpToPoint` run.
- `episode <name> [jumppoint]` (in-world) → `ServerGameSession::startLoadingEpisode`. `reload [jumppoint]` →
  `startReloadingEpisodeFromJumpPoint`. `restart` → `requestRestartCheckpoint`.
- `level <world> <episode> [jumppoint]` (main menu) → exe+0x61c5d0 with the captured menu. `level` with no args lists what
  `loadLevelDefinitions` returned.
- `save` → `GameHelper::saveGame(2, true, true)` to mint fixtures (`quiver lab fixture save` after it lands).
- Boot config: like Torchbearer's `[dev] start_level`, have the mod call the `level` path as soon as the menu populates
  (no command line exists to do it).

What needed live verification (status after r1-r4 in brackets):
1. `handleTeleport` on the server avatar moves the player cleanly (no physics fight, the camera follows), and which way yaw 0 faces. [done r1: clean; camera follows position, not heading; yaw 0 = +Z]
2. The real world/episode/jump point names (log `loadLevelDefinitions` and the jump point registry), and whether the sector names
   (`hub`, `research`, …) are the episode names. [partly: `gameworld` / `hub` / 122 jump point names from the registry; other sectors not loaded]
3. That calling exe+0x61c5d0 from the game tick behaves like a UI click (Coherent may expect UI-thread context), and the meaning of
   the bool in `startLoadingEpisode`. [not done: level select deletes slot 0; the bool made no visible difference]
4. That the registry holds jump points across the whole episode, not only streamed layers. [done r1: the whole hub episode at game start]
5. The `+0x406` focus bypass in a headless lab instance, and the side effects of the developer flag `+0x130`. [done r1: already 1, window focused; +0x130 untouched (reads 0)]
6. The save slot directory layout on disk after the first checkpoint, and that Continue loads `savegame-slot-00`. [done r3: flat `savegame-slot-00_<chunk>` files]
7. Whether loose `data/globaldb/worlds/menu_levels_override.json` is honoured (only in a lab install).

Candidate scenes once names are known: Executive sector and Central Executive (big halls, `hub`), Research (labs), Maintenance
(dark industrial), Containment, Foundation (DLC1, `foundation`) and the Investigations sector (DLC2, `awe`), the Ashtray Maze
(combat + music), and the Quarry (outdoor-ish; most likely a jump point inside `maintenance` or `hub`, verify). For cutscenes,
mission jump points that trigger `OnJumpToPoint` scripts are a good bet.

---

## 9. Level loads, the black screen, cinematics (live r6-r20)

### Why `episode` / `reload` stayed black

Two separate causes.

1. **The wrong entry point.** The game never calls `startLoadingEpisode` (the exe does not import it;
   static: import list) and calls `startReloadingEpisodeFromJumpPoint` only from the debug path of
   `useJumpPoint`. Its own level switches all call the exported
   `?startLoadingLevel@ServerGameSession@coregame@@QEAAXAEBV?$InplaceString@$0EA@@r@@00_N1@Z`
   `(world, episode, jumppoint, false, true)` (coregame+0x19b790, `40 53 55 56 57 41 56 41 57 48 81 ec 98 01 00 00`):
   - fast travel from a control point (static: exe+0x206d90, the handler that posts `OnStartJumpToControlPoint`):
     same episode -> `useJumpPoint(jp, false, false)` (not a debug jump); other episode -> `startLoadingLevel`
     with the control point's world/episode/jump point. It first records the target in the save manager
     (+0x408/+0x448/+0x488, exe+0x3a7850);
   - the episode transition component (static: exe+0x1c8270): `GameHelper::saveGame(role, false, true)`, then
     `startLoadingLevel("", episode, jumppoint, false, true)` (an empty world means the current one);
   - another transition (static: exe+0x5d1910) and the session's own `startLoadingLevel()` (exe+0x54af60).

   In the server's in-game program-flow state (static: exe+0x54baa0, the action handler), `ActionStartLoadingWorld`
   (what `startLoadingLevel` sends) becomes a `ProgramFlowLoadingStateParameters` with mode `3 + (int != -1)`, i.e.
   **mode 4 with the last bool true, the mode `ActionRestartCheckpoint` uses**, and transitions to the
   `LoadingLevel` state. `ActionStartLoadingEpisode` is mode 5 and `ActionStartReloadingEpisode` mode 6, both into
   the `LoadingJumpPoint` states (strings `ProgramFlowServerStateLoadingJumpPoint` / `...ClientState...`; the client
   handler exe+0x536ac0 does the same). The client's in-game state has a fade-in parameter (`fFadeInWaitTime`,
   `ProgramFlowClientStateInGame::Parameters`, exe+0x539080) that the jump-point path apparently never completes.
   **Live r10-r14**: `startLoadingLevel(gameworld, <episode>, <jump point>, false, true)` loads research,
   maintenance, containment, foundation and awe in 6 to 15 s, rendered, with the player in control, from the
   game-start save, with or without a save first. Same-episode reloads also go through it (`reload`).
2. **The hub's story state.** From the game-start save, the hub loaded anywhere but `Game_Entry_Point` (by
   `startLoadingLevel` too, r8/r9/r10) stays black with the player's control off and a non-skippable timeline
   playing, for good. Saving first (as exe+0x1c8270 does; r9), or a `DEBUG_Hub_FreeRoam_Start` debug jump and a
   save first (r10: that jump does put the hub in free roam, "NEW MISSION: DIRECTORIAL OVERRIDE"), does not help:
   the save's checkpoint stays at mission 1's start (a `restart` after it plays the intro again, r10). The hub
   at `Game_Entry_Point` works (the intro plays; skip it, below), and jumps inside the hub then reach every
   hub point. Not a missing fade call: the fade setter was never called (r3), and the other episodes render.
   Not the thread either: everything runs on the server thread, as the game's own calls do.

Other live findings:
- Every load writes the slot: the game saves the episode's chunk on arrival (`savegame-slot-00_<episode>`, r10
  listing), and the target's chunk is created when missing. Reinstall the fixture (and delete extra chunks:
  `fixture install` leaves them) for a known start.
- Story-state jump points can crash the game when an episode is **loaded** at them: research
  `Frame_05_Fight_Start` (r11, r12) and containment `st03_torment_fight` (r13) both died with an access
  violation (exit -1073741819) a few seconds into the load. Location points (rooms, control points, `JP_HUB`) are
  fine. The registry of a loaded episode depends on its story state: research loaded at `SectorLobby` (r14) lacked
  `Frame_05_Fight_Start`, which its startup load listed (r10).
- The awe startup point (empty jump point) killed the player about 5 s after arrival (red screen, then the game
  reloaded the checkpoint, r10); `HUB`, `West_AWE`, `East_entrance` are fine (r11).
- Jump point counts per episode at the game-start story state (`jump list`, r10/r11): hub 122, research 117,
  maintenance 82, containment 110, foundation 84, awe 31. Each sector has `DEBUG_<Sector>_FreeRoam_Start`
  and `control_point_*::PublicInterface` points (the control points; fast travel targets).
- `0xD15EA5E` exits a few seconds after launch, before the menu (r6, r7, r15, r16, r18), from the game-start
  save too, mostly while other Control instances were using most of the GPU memory: environmental, not the nav
  verbs (nothing of ours runs before the menu but hooks). A relaunch works.

### Current world / episode

The exe's own reads of "the loaded level" (static: exe+0x206d90 role-3 branch, exe+0x54baa0):
`GameServerBase +0x1e8 -> +0x730` = level object; `+0xb8` world name (`char*`); `+0x308 -> +0x148` = episode,
name inline at `+0x138`. **Live r10**: `gameworld` / `hub|research|maintenance|containment|foundation|awe`,
following every load (the save manager's strings at +0x288/+0x2C8 do not). `where` reports it as `level`.

### Cinematics and player control

- `input::InputManager::isPlayerControlEnabled()` (input export, input+0x8f00, `80 79 7e 00 74 09 80 79 7d 00 74 03 b0 01 c3`:
  `+0x7e && +0x7d`, the latter `isPlayerControlEnabledByScript`) and `isMenuOn()` (`+0x74`) on
  `InputManager::sm_pInstance`. **Live r8**: control is off from the moment the player appears after Continue
  until the opening cinematic ends, and on in gameplay; the lab state uses it (`cinematic`).
- The exe's `TimelineManager` (`DAT_141249540`, exe RVA 0x1249540; ctor exe+0x5b5ca0, per-frame update
  exe+0x5b6080) keeps `+0x10`/`+0x18` an array of 16-byte entries (timeline component state first) and count, and
  recomputes `+0x80` a skippable timeline plays (entry `+0x1c0`), `+0x81` a timeline in state "playing" (`+0x180`
  equals the ID at exe+0x12cbe68), `+0x82` letterbox (`+0x1c9`). **Live r8**: about 170 entries; ambient timelines
  play during gameplay too (`+0x81` stays 1), so "a timeline plays" is not "cinematic".
- **Skip**: the update reads input actions 10 `ABORT_TIMELINE` (shows the callout for 6 s) and 11
  `ABORT_TIMELINE_CONFIRM` (live r8: `InputManager::getActionName`) and then calls the skip, exe+0x5aa840
  (`40 53 48 83 ec 20 83 b9 f8 01 00 00 00 48 8b d9`): jump to the next skip marker after the current time, or
  to the end and post script `OnTimelineSkipped`. **Options byte +0x13a** makes both inputs read as pressed
  (static: exe+0x5b6209 and exe+0x5b6246; the only readers of +0x13a, findbytes), i.e. "skip every skippable
  timeline". **Live r8/r9**: the opening cinematic is a short non-skippable timeline (about 3 s, black, voice)
  and then a skippable one; with +0x13a set it is skipped at once and the player has control in the lobby
  (camera yaw 179.82, pitch -4.44: the rest pose r1-r4 reached after 2.5 min). **But** the manager skips the
  *last* skippable entry in its array (static: exe+0x5b62bb keeps overwriting), and the array holds client and
  server copies: after a research -> hub load (r20) the switch was held while the intro played through (3.5 min).
  The mod therefore also calls exe+0x5aa840 itself, on the server thread, for every playing, skippable,
  server-role (`+0x18` bits 62-63 = 3) entry without a pending skip (`+0x1c1`). **Live rb-r2/rb-r3
  (rayblaster)**: that did not help either: after research -> hub loads, the direct call ran 676 times on
  the server timeline (its `+0x1c1` cleared each time, so the request was consumed) and the intro still
  played to its end. The client-copy hypothesis is therefore wrong or incomplete. What does decide it, live:
  **whether the skip was already on when the cinematic began**. Switched on before the load (Continue, or a
  checkpoint restart with `skip` sent first) it skipped within a second every time (laptop r8-r14, rb-r2
  restart test 12 s door to door, rb-r4 five hub returns of 17 to 34 s); switched on as the player appeared
  (after the load) it failed in laptop r20 (2x), rb-r2 and rb-r3 (3x) and worked only sometimes. A guess,
  not checked: the skip is only honoured before the timeline's first skip marker, or the time driver
  (exe+0x5b8030 sets the time from its clip each frame) overrides a skip that comes later. The mod now
  turns the skip on before every load that should skip, and keeps the direct call to 4 tries per timeline.
- **Hub returns use the checkpoint restart.** A load of the save's own checkpoint (`hub` / `Game_Entry_Point`
  from `game-start`) goes through `requestRestartCheckpoint` (live rb-r4: 17 to 34 s from research,
  maintenance, containment, awe, research again, all with the intro skipped).
