# Control (D3D12, GOG Ultimate): camera, frame structure and temporal state for same-frame stereo

Static reverse engineering of the copies in `re/bin/`, done to prepare same-update stereoscopic 3D
(both eyes rendered from one game update and composed side by side), as Torchbearer does for Alan
Wake Remastered (`~/code/torchbearer/docs/stereo.md`, "TB" below). Nothing here was checked live.

Conventions: every address is an **RVA in the named module**, written `renderer+0x11daf0`
(image base 0x180000000 for the DLLs, 0x140000000 for `Control_DX12.exe` = `exe`). Modules:
`renderer` = `renderer_rmdwin10_f.dll`, `rl` = `rl_rmdwin10_f.dll`, `d3d` = `d3d_rmdwin10_f.dll`,
`coregame` = `coregame_rmdwin10_f.dll`, `exe` = `Control_DX12.exe`. How each claim was established:
- "export": the demangled export lists `re/exports/*.dem` (Ghidra also names those functions).
- "static: X": read off the decompilation or disassembly of function X in Ghidra (copies
  `re/ghidra-stereo-{renderer,rl,d3d,coregame,exe}`, the exe imported and analysed for this work;
  `re/scripts/Serve.java`).
- "string": a defined string and its xrefs.

The DLSS / NGX / swapchain side is in [dlss.md](dlss.md). Where the two overlap, they agree.

---

## 0. Summary

- **Two threads, not one.** The **"RMD - Game Thread"** (`exe+0x654840`) runs the simulation
  and *describes* the frame (`GameClient::update` `exe+0x396380` → `GameClient::render`
  `exe+0x396ce0`). Every renderer call it makes (`rend::RendererInterfaceWrapper::begin /
  renderWorld / renderHUD / end / present`, exports) only posts a packet. The **"RMD - Renderer
  Thread"** (`renderer+0x10d150`) pops the packets and runs **`RendererX86`**'s virtual methods.
  These record and submit D3D12 and call Present. So, unlike AWR, the place to render an eye
  twice is a **renderer-thread** hook.
- **The main view is easy to isolate.** The game's camera reaches the renderer as one
  `m::View` by value, in the `renderWorld` packet. RendererX86 slot 15 (`renderer+0x11daf0`)
  copies it into the renderer's main `rend::View` at RendererX86+0x678 and calls the frame
  render `renderer+0x11dc50`. That function builds the TAA/DLSS jitter as a post-projection
  matrix on a *local* copy of the view, then runs the render graph `renderer+0x11ee20`. Shadow,
  probe and reflection views are separate `rend::View` objects. There is no need to filter a
  global `setViewToWorld`: biasing the `m::View` argument of slot 15 biases the main view and
  nothing else.
- **Matrices are doubles, row-vector** (translation in the last row), with the same projection
  formula as AWR. Fixed horizontal FOV up to 16:9, Hor+ beyond.
- **Temporal state is pointer-held, not name-pooled**, unlike AWR. The pool
  (`RenderTargetManager::allocTexture2D`) matches on `Texture2DInit` only. Each effect owns its
  history pointer(s). All of them are keyed on **one counter, `renderer+0x8029d0`**, which is
  incremented once per RendererX86 `begin`. Per-eye state is therefore a matter of swapping a
  known list of pointers and small structs around each eye's `renderWorld`. Section 3 has the
  list.
- **The engine already renders several frames per game update**: the high-resolution screenshot
  (`exe+0x569360`) issues 64 full `begin / world / HUD / end / present` sequences from one
  simulated state. A second render per update is therefore within the engine's design.

---

## 1. Frame structure and threads

### 1.1 Threads

| Thread | Entry | How established |
|---|---|---|
| "RMD - Game Thread", `r::registerThread(1)` | `exe+0x654840` | string + static: names itself, reads `CommandLine.txt` next to the exe (like AWR), then runs the app |
| "RMD - Renderer Thread", `r::registerThread(2)`, also registered with the job system | `renderer+0x10d150` | string + static: loop over `RendererThreading` (`sm_pInstance` renderer+0x802948, export) `MultiProducerFifo` at +0xb0: `peek` → `FUN_18010d450` (execute packet) → `pop` |
| "RMD - Video Renderer", `registerThread(0xd)` | `exe+0x642d30` | string + static: a loop that runs the same frame functions (`exe+0x568620`, `exe+0x568b20`) while videos or loading play, with no world |
| Job workers | `r::experimental::JobSystem` | the render graph and visibility launch jobs (e.g. "Velocity pass job generator", `VisibilitySystem::launchAsyncQuery`) |

How a renderer call travels (static: `RendererInterfaceWrapper::renderWorld` renderer+0x102c40,
`FUN_18010b4e0`, `FUN_18010ade0`, `FUN_180102de0`):
- On the renderer thread (`rend::sm_eRendererThread`, renderer+0x7ec134, export) it executes
  inline.
- On thread id 1 (the game thread) it is appended to the current packet batch
  (`FUN_180013860` / `FUN_180013940`; `RendererThreading::startPacketBatching` /
  `endPacketBatching`, exports).
- Otherwise it is pushed through the locked FIFO (+0x138 SRW lock, +0x148 array, +0x158 count),
  followed by `releaseSemaphore`.

The packet is a type-5 or type-7 record with a function pointer. For `renderWorld` it carries a
copy of the `m::View` (0x2e0 bytes). The renderer thread then calls the RendererX86 vtable slot
(`FUN_180102a90`: slot +0x80; `FUN_180102de0`: slot +0x78).

The game thread can run ahead of the renderer thread by however many batches the FIFO holds. The
depth was not determined statically (open question Q1). Present waits for the GPU to be at most one
frame behind (dlss.md 2.3, "DX12:Wait for present").

### 1.2 The game thread's frame

```
exe+0x396380  GameClient::update(dt)            (vtable; string "gameclient.cpp")
  coregame::GameClientBase::update(dt)          simulation (export)
  state machines, UI input, sound ...
  exe+0x396ce0  GameClient::render              (static: only caller is update, +0x761)
    exe+0x5688b0  frame begin:
      GameWindow::dispatchPreRenderingBeginCalls
      exe+0x568620:  callOnRendererThread(...)  (exe+0x569f70: timer)
                     RendererInterfaceWrapper::setRenderFrameNumber(sm_instances+0x40)
                     RendererInterfaceWrapper::begin()
                     callOnRendererThread(GameWindow::processMessages)  (exe+0x56a740)
      Umbra tome update; VectorBlurWrapper::setFreeCameraMoved(freecam)
      SpaceInterfaceWrapper::updateAllEntities()            entity transforms → renderer packets
    ui::System::update, state-machine render actions, ShapeEngine debug HUD ...
    exe+0x568b20  frame render (this = renderer client, global exe+0x129a750):
      (*(this+0x20))->vtbl[+0x28]()   = SingleplayerClientWorldUpdateLogic slot 5
                                         (vtable exe+0xe54ab8; MP: exe+0xe40ee8)
          exe+0x2a5ec0 → exe+0x65b110(worldRenderer, &g_renderView exe+0x12b9f50)   WORLD RENDER
      ShapeEngine::pushToRenderer x0x16
      RendererInterfaceWrapper::renderHUD()
      RendererInterfaceWrapper::end()
      ++sm_instances+0x40                     game-side render frame number
      (debug: depth / back-buffer dumps)
      RendererInterfaceWrapper::present()
      (debug: if tweak > 0, exe+0x569360 high-res screenshot)
  frame pacing / sleep, RendererInterfaceWrapper::setPresentInterval
```

All of the above is static (decompilation of each function named). The world render
`exe+0x65b110` (static):
1. Copies the view.
2. Picks the sun light by time of day.
3. Calls `setupStreamingPositions` and `setBackdropNear`.
4. Calls `RendererInterfaceWrapper::renderWorld(view, space0, sun, space1, false)` (the 5-argument
   overload, renderer+0x102c40). The 4-argument overload, renderer+0x102930, is not used by the exe.

`RendererInterfaceWrapper::renderWorld` (renderer+0x102c40, export, static) does two things:
- On the **game thread**, it builds a temporary `rend::View` from the `m::View` and calls
  `VisibilitySystem::launchAsyncQuery(view, sun, flag)` (renderer+0x1af180, export): Umbra
  culling, started as a job.
- It posts the packet that makes the renderer thread call **RendererX86 slot 15**.

The visibility system has *one* global query state (`renderer+0x129b240` View copy, job handle
+0x129bbc0). A second launch waits for the first job, then overwrites it (static). One query per
`renderWorld` call is the design.

### 1.3 The renderer thread's frame: RendererX86

`rend::RendererX86` (RTTI). The instance is `renderer+0x802a00` (`RendererX86::getInstance`
renderer+0x1367b0, export). The main vtable is **renderer+0x6323d8** (static: RTTI locator, slot
contents). Relevant slots, mapped from the wrapper packets (each wrapper lambda is `jmp
[vtbl+N]`; static):

| Slot (+off) | RVA | Wrapper | What it does (static) |
|---|---|---|---|
| 2 (+0x10) | renderer+0x11b0c0 | `setScreen` (and from renderFrame) | sets viewport offset / output / back-buffer / render sizes (section 4) |
| 10 (+0x50) | renderer+0x11b470 | `setRenderFrameNumber` | `d3d::setCurrentFrame(n)` |
| 14 (+0x70) | renderer+0x11b4c0 | `begin` | **frame begin**: RT feature flags, HDR, `d3d::DLSS::resetDLSSFeature`, `FrameBeginHandler::beginFrame`, frame time (renderer+0x802998 total, +0x802994 dt), **`++renderer+0x8029d0` (render frame counter)**, MIP bias, "Renderer:Debug Value N" tweakables |
| 15 (+0x78) | renderer+0x11daf0 | `renderWorld` (5 args, used by the exe) | **the main view**: `mainView = *(this+0x678)`; `mainView.m::View = *arg` (rend::View+0x10); `View::setViewToClip(arg+0xd0)`; `View::setViewToWorld(arg+0x70)`; camera position floats → renderer+0x128d1d8/1dc/1e0; `renderer+0x8029d8 = arg` (the "camera view" for `g_mViewToCameraView`); then **renderFrame** renderer+0x11dc50(this, mainView, space, sun, space2) |
| 16 (+0x80) | renderer+0x11d9e0 | `renderWorld` (4 args) | the same without the second space |
| 17 (+0x88) | renderer+0x1343b0 | `renderHUD` | UI (Coherent, RendererX86+0x680) into RendererX86+0x3c8 or the back buffer (section 4.3) |
| 18 (+0x90) | renderer+0x134bb0 | `end` | frees the frame's RTs, clears the camera-cut flag renderer+0x802923, copies +0x3c8 to the back buffer (copyWithGamma), **`storePrevious` on every `rend::View`** (section 2.4), frame-drawn fences, video capture |
| 25-27 | renderer+0x11e540 / +0x11e5d0 / +0x11e770 | high-res screenshot prepare / tile / save | write renderer+0x7ec2f3 (use the jitter pattern) and +0x128d1e8/ec (custom sub-pixel offset) read by renderFrame |
| 30 (+0xf0) | renderer+0x1363e0 | `present` | `d3d::DeviceUtil::present()` unless device-lost (+0x476) |

Per world render (static):

```
renderer+0x11daf0  RendererX86 slot 15 (renderWorld)       ← the renderFrame equivalent's entry
  renderer+0x11dc50  renderFrame(r, mainView, space, sun, space2)
    slot 2 setScreen(current sizes)                (no-op unless something changed)
    if !RenderOptions::ScreenSpaceAntialiasing: graph(r, mainView, mainView, ...)
    else:
      jitter phase = counter & 3 (rotated grid, renderer+0x1295c30) or Halton(2,3) for DLSS
      J = identity, J[3][0..1] = jitter (NDC); r+0x480/+0x488 = jitter (doubles)
      jView = rend::View(); renderer+0x180940(jView, mainView, &J, &prevJ)   jittered copy
      jView.use(); set g_uTemporalFrame, g_uCurrentFrame, g_uCameraCut, g_vPostProcessDejitter
      renderer+0x11ee20  render graph (r, mainView, jView, space, sun, space2)
    space+0x32c = 1
```

The render graph `renderer+0x11ee20` (static) declares passes into the global pass list
renderer+0x910ba0 and resources into renderer+0x910c08, with names as strings ("Scene update",
"Shadow maps", "Gbuffer pass 1/2", "Deferred shadows", "velocitypass", "TAA and Upscale Depth",
"Bloom", "final image pass", "DLSS anti aliasing", "Final Post Process and Display Map",
"Cleanup", ...). Then `renderer+0xa03c0` (sort), and `renderer+0xa09a0` **executes the passes
in order, inline, on the renderer thread**: for each pass it allocates the transient RTs from the
pool, invokes the pass's `std::function` (vtable +0x10) and frees RTs at their last use. Passes
may fan out to jobs internally. So a `renderWorld` call has finished recording its passes when it
returns.

Present is the engine's own `d3d::DeviceUtil::present` (dlss.md 2.3). It flips its back-buffer
index itself (`idx ^= 1`) and never asks DXGI. Swallowing a Present the way TB's frame path does
would desynchronise it. This favours a design with **one Present per update** (section 6).

### 1.4 Can the main view be rendered twice without re-simulating?

Yes, on either thread (static):

- **Renderer thread, inside one begin/end** (TB's "view path"). Slot 15 is self-contained:
  - It overwrites the main `rend::View` from its argument.
  - It rebuilds the render graph from scratch (`renderer+0x9fd60` resets the pass and resource
    lists).
  - It runs to completion.

  A second call with another `m::View` renders another image of the same packet stream (same
  entity state, same frame counter, same frame time). It advances or overwrites:
  1. The render-graph **persisted resources** (`lastcolorbuffer`, `previouslineardepth`): the
     second call's "previous" would be the first call's output.
  2. Every pointer-held history in section 3 that is written per render (SSAA ring slot
     `counter % 3`, the shadow-buffer ping-pongs `counter & 1`, the DLF denoiser histories,
     volumetric temporal buffers, the DLSS feature history). With a shared counter both eyes
     write the *same* slot, so the second eye overwrites the first.
  3. Per-render GPU simulations whose state lives on the GPU (auto exposure adaptation; flares;
     film grain random). These are unverified (Q6).
  4. The final image in the back buffer (or +0x3c8): the second eye overwrites the first unless
     the first is copied out in between.

  What it does **not** advance: the counter renderer+0x8029d0, the frame time, d3d's current frame
  (slot 10), `View::storePrevious` (runs in `end`), the camera-cut flag (cleared in `end`). These
  all stay per update, as wanted.
- **Game thread, a full extra frame.** `exe+0x569360` (high-res screenshot) runs `begin → world
  render (exe+0x65b110 with a saved view) → renderHUD → end → present` 64 times from one simulated
  state, with no simulation step in between (static). Every extra frame advances everything in
  slot 14 and slot 18, and presents. It is the analogue of TB's frame path, with the
  Present-index problem above.

---

## 2. The camera

### 2.1 Where the main camera comes from (game thread)

Static, `exe+0x383c60` (camera post-update; reached through a vtable, exe+0x1332dc8 DATA):

- The active camera is `coregame::CameraComponentState` `exe+0x129a670[exe+0x129a5cc]` (index 0
  = game camera; another index is used by the free camera). `CameraComponentState::getView` is an
  export. Its `m::PerspectiveView` is at component+0xc0.
- Each update it is copied into the **global render view `g_renderView` = exe+0x12b9f50**
  (`m::PerspectiveView`, 0x2f0 bytes), then:
  1. `fov = scaleFOV(exe+0x123904c (deg) * pi/180, FOVMultiplier)`, where `FOVMultiplier` is
     `rend::RenderOptions::FOVMultiplier`, renderer+0x7ec13c (export), clamped ≥ 1.
  2. If the tweakable "Camera:Override FOV" is set, the camera's FOV is replaced by "Camera:Override
     FOV Value" (deg).
  3. `setFov(fov)`, `setZoomFactor(tan(camFov/2) / tan(fov/2))`,
     `calculateAspectRatioAdjustedProjection(16/9, false)`.
  4. `coreshared::g_pRenderView = &g_renderView` (export; game-side particles use it).
- exe+0x123904c is written by the script command "SetMaximumFOV" (exe+0x519e90, string). The
  options menu ("MENU_OPTIONS_FOV_SCALE", string) drives the multiplier.
- `exe+0x2a5ec0` passes `&g_renderView` to the world render `exe+0x65b110`, which copies it and
  sends it to the renderer.

### 2.2 m::View / m::PerspectiveView (rl)

Layout (static: `m::View::View(copy)` rl+0x110c00, `setViewToWorld` rl+0x1121c0,
`PerspectiveView::calculateProjection` rl+0x11eb40, `calculateAspectRatioAdjustedProjection`
rl+0x11ee40; exports):

| Offset | Field |
|---|---|
| +0x000 | vtable (`m::View` rl+0x2522f8, `m::PerspectiveView` rl+0x252878) |
| +0x010 | world-to-view, `double[4][3]` |
| +0x070 | **view-to-world, `double[4][3]`: rows right, up, forward, position** (position = +0xb8/+0xc0/+0xc8; static: slot 15 reads the camera position there) |
| +0x0d0 | **view-to-clip (projection), `double[4][4]`** |
| +0x150 | clip-to-view `double[4][4]` |
| +0x1d0 | world-to-clip `double[4][4]` (= world-to-view × view-to-clip: row-vector, `v' = v·M`) |
| +0x250 | clip-to-world `double[4][4]` |
| +0x2d0 | zoom factor (float) |
| +0x2e0 | PerspectiveView: FOV (float, radians, horizontal while aspect ≤ 16/9) |
| +0x2e4 | aspect (w/h) |
| +0x2e8 / +0x2ec | near / far |

Size: `m::View` ≈ 0x2d8 (the packet and stack copies are 0x2e0); PerspectiveView 0x2f0.

- **Setters**:
  - `m::View::setViewToWorld` (rl+0x1121c0) stores +0x70 and +0x10, inverts, and recomputes +0x1d0
    and +0x250.
  - `setViewToClip` (rl+0x1115f0) and `setWorldToView` (rl+0x111c80) do the same from the other
    side.
  - The PerspectiveView setters (`setFov` rl+0x11e9f0, `setAspectRatio` rl+0x11ea10, `setNear`
    rl+0x11ea20, `setFar` rl+0x11ea30, `setZoomFactor` rl+0x11ea00) recalculate the projection.
  - All are exports.
- **Projection** (`calculateProjection`): `m00 = 1/tan(fov/2)/zoom`, `m11 = aspect/tan(fov/2)/zoom`,
  `m22 = f/(f-n)`, `m23 = 1`, `m32 = -n·f/(f-n)`. This is D3D left-handed with conventional Z
  (dlss.md 3 agrees: not reversed). It is the same formula as AWR (TB section 1).
- `calculateAspectRatioAdjustedProjection(16/9, ...)`: if aspect ≤ 16/9, FOV is horizontal;
  wider aspects keep the 16:9 vertical FOV and widen horizontally (Hor+). One eye rendered at the
  engine's own 16:9 resolution keeps the game's frustum exactly, as in TB.
- **Eye axis**: view +x is camera right (row 0 of +0x70), the left-handed convention, as TB
  measured for AWR. To be re-confirmed live (Q11).

### 2.3 rend::View (renderer)

`rend::View` (vtable renderer+0x62e1f0) = vtable + an embedded `m::View` at **+0x10** + previous
matrices and uniform handles. Layout (static: ctor renderer+0x17dfe0, `setView` renderer+0x17eb20,
`use` renderer+0x17eb60, `FUN_18017f4a0`, `FUN_180180700`, `FUN_180180940`):

| Offset | Field |
|---|---|
| +0x010.. | `m::View` (so +0x20 world-to-view, +0x80 view-to-world, +0xe0 view-to-clip, +0x1e0 world-to-clip, +0x2e0 zoom) |
| +0x2f0 | previous view-to-clip used this frame, `double[4][4]` (= +0x3d0 if +0x4b0, else current) |
| +0x370 | previous world-to-view used this frame, `double[4][3]` (= +0x450 or current) |
| +0x3d0 | **stored previous view-to-clip** (written by `storePrevious`) |
| +0x450 | **stored previous world-to-view** |
| +0x4b0 | u8 "has previous" |
| +0x4c0.. | uniform handles + float copies: `g_mWorldToView` +0x4c8, `g_mViewToWorld` +0x518, `g_mViewToClip` +0x568, `g_mClipToView` +0x5b8, `g_mWorldToClip` +0x608, `g_mClipToWorld` +0x658, `g_mClipToPreviousClip` +0x6a8, `g_mViewToPreviousClip` +0x6f8, `g_mPreviousViewToView` +0x748, `g_mPreviousClipToClipNoJitter` +0x798, `g_mClipToPreviousClipNoJitter` +0x7e8, `g_vViewPoint` +0x838, `g_mViewToCameraView` +0x858, `g_mCameraViewToCameraClip` +0x8a8, `g_mCameraClipToView` +0x8f8 (strings in the ctor) |

- Every `rend::View` registers itself in a global array (renderer+0x9199a8, count +0x9199b0,
  under the SRW lock +0x7ec3e0) in its ctor and leaves it in its dtor (static).
- Uniforms go through `d3d::ConstantValueProviderRegistry::setProviderData` (named globals, export),
  not per-thread slots as in AWR. `View::use` (renderer+0x17eb60) publishes them.
- `rend::View::setViewToWorld` (renderer+0x145b0), `setViewToClip` (renderer+0x14a50),
  `setWorldToView` (renderer+0x14100) and `validateProjection` (renderer+0x17f3e0) all call
  **`FUN_18017f4a0`** (renderer+0x17f4a0). It sets the "used previous" (+0x2f0/+0x370) from the
  stored previous or from current, and computes the previous-clip uniforms (static).

### 2.4 Telling the main view from the others

- The main scene view is **`*(RendererX86+0x678)`**. Only slots 15/16 write it, from the game's
  `m::View` (static). The jittered view used for rasterisation is a stack-local `rend::View`
  inside renderFrame, built from the main view by `renderer+0x180940`.
- Shadow, spot and cube-probe views are their own `rend::View` / `m::View` objects. Examples:
  `ShadowUtils::setupDirLightView*` (exports, take the main `m::View` as input and output their own
  matrices), `getSpotViewFromView` (renderer+0x18a320), `VirtualCubeTexture`. The UI does not use a
  `rend::View` (renderHUD draws in screen space).
- So **the hook point that scopes the bias is slot 15's `m::View` argument**, the counterpart of
  TB's renderFrame argument. Modify a copy (view-to-world row 3 += right × offset) through
  `m::View::setViewToWorld` (rl export) so all derived matrices follow, and pass the copy on.
  Biasing on the game side (g_renderView, or `RendererInterfaceWrapper::renderWorld`) would also
  move the Umbra query, `coreshared::g_pRenderView` (particles) and audio. That is acceptable but
  per update, not per eye.

### 2.5 Jitter (TAA / DLSS)

Static, renderFrame `renderer+0x11dc50`:
- Enabled by **`rend::RenderOptions::ScreenSpaceAntialiasing`** (renderer+0x7ec159, export; also
  cached in renderer+0x7ec2f2).
- **TAA** phase = `counter & 3` into a rotated-grid table (±0.25 / ±0.75, renderer+0x1295c30),
  scaled by the tweakable **"SSAA:Temporal Jitter Scale"** (object renderer+0x911420, value
  renderer+0x9114d0) / render size. The previous frame's jitter, `(counter-1) & 3`, goes to
  renderer+0x7ec380/388.
- **DLSS** phase = Halton(2,3) of `counter % lrintf(8·(outH/renderH)²)` (1024 with RR), in NDC
  `×2/size`.
- **High-res screenshot** (renderer+0x7ec2f3 == 0): a fixed offset renderer+0x128d1e8/ec instead.
- The jitter goes into a post-projection matrix `J` (identity, `J[3][0..1]` = jitter), applied as
  `jView.viewToClip = mainView.viewToClip × J` by `renderer+0x180940`. The jittered view's previous
  is built with the previous jitter. RendererX86+0x480/+0x488 keep the jitter (DLSS input, dlss.md 3).
- Everything keys on **renderer+0x8029d0**, incremented once per slot 14 `begin`. Two eyes inside
  one begin/end therefore get the same jitter, which is what TB arranged by hand.
- **Convergence**: `J` is a local, so there is no setter to hook like TB's `setPostProjection`. The
  equivalent is an off-axis term in the `m::View`'s projection before slot 15:
  `viewToClip[2][0] += ±offset·m00/Zc`. With row vectors, `x_clip += z·s`, `w = z`, so this is a
  constant NDC shift. Apply it through `m::View::setViewToClip` (rl+0x1115f0). It then flows into
  the main view, the jittered view, the TAA ring's stored views and the previous-clip uniforms
  consistently. Doing it per eye on the **unjittered** projection keeps it out of the jitter.

---

## 3. Temporal state that would cross eyes

Every history is held by its owner as a pointer, or as a small array indexed by renderer+0x8029d0.
The RT pool is a plain free list matched on `Texture2DInit ==` (`RenderTargetManager::allocTexture2D`
renderer+0xa9760, export; entries 0x70 bytes: +0 texture, +8 init, +0x60 in-use, +0x68 debug
name; static). So the TB trick of renaming pool requests does not apply. Instead, **save/restore
pointers and structs around each eye**. With both eyes inside one begin/end, the counter is the
same for both eyes, so "the slot this frame writes" and "the slot this frame reads" are the same
for both eyes. Each eye needs its own copy of the slots.

| # | State | Owner / where | Indexing | Established |
|---|---|---|---|---|
| T1 | **Camera previous matrices** (camera motion vectors, `g_mClipToPreviousClip*`, `g_mViewToPreviousClip`, VectorBlur, SSAO/SSR/volumetric reprojection) | main `rend::View` = `*(RendererX86+0x678)`: stored previous **+0x3d0..+0x4b0** (`double[4][4]` + `double[4][3]` + flag) | written once per frame by `storePrevious` renderer+0x180700 from **slot 18 `end`** for every registered view; read via `FUN_18017f4a0` on every set | static |
| T2 | **TAA history ("SSAA")**: 3-frame ring of {jittered `m::View`, `Texture2D*` "SSAA::m_pPreviousFrame"} | `SSAA* = *(RendererX86+0x138)`: slots at **+0x40 + i·0x2f0** (`m::View` 0x2e0, texture at slot+0x2e0, i.e. +0x320/+0x610/+0x900); +0x10 "copy via DMA" flag, +0x18/+0x20 DMA fence | written slot `counter % 3`; the resolve reads all three (`g_tPreviousColors`, `g_mSSAAClipToPreviousClip`, `g_vSSAAJitterOffset`[]) | static: TAA pass renderer+0x132000 → `renderer+0x146a20` (3-frame SSAA path) / **`renderer+0x1477c0` (TAA + upscale, `SSAA.rfx` reprojectAndClamp / pureCompose)** |
| T3 | **Render-graph persisted resources**: `lastcolorbuffer` (← "colorBufferCopy-2", previous frame's colour, used by SSR `g_tPreviousColor` renderer+0x245e60 and the RT pass), `previouslineardepth` (← "lineardepthcopy", used by TAA / RT / `g_tPreviousLinearDepth`) | graph resource table; resources flagged persistent are moved at the end of execution into the **previous-frame list renderer+0x910c20 (ptr) / +0x910c28 (count)** and reclaimed by name hash next frame (alias records `renderer+0xa26a0`, type 2) | per graph execution, i.e. **per `renderWorld` call** | static: `renderer+0x11ee20` (aliases at 0x11f598, 0x120cd0), executor `renderer+0xa09a0` (`FUN_1800a2820` move, free of unclaimed) |
| T4 | **Deferred shadow buffer** temporal (sun shadow, SSR, AO, "Shadow Buffer:Temporal blur sun shadows", "SSR:Temporal blur SSR color", "SSAO:Temporal blur SSAO", `g_tShadowPreviousFrame`, `g_tReflectionsPreviousFrame`, `g_mPreviousViewToWorld`, `g_vPreviousViewPos`) | object from RendererX86+0xd8 (getter `FUN_1800133a0`): ping-pong **pairs +0x100/+0x108, +0x118/+0x120, +0x130/+0x138**, plus +0xf0, +0xf8, +0x128, +0x140; sizes +0x178/+0x17c | `counter & 1` (write) / `(counter+1) & 1` (read) | static: `renderer+0x248940`, `renderer+0x247f90` (called from the RT pass) |
| T5 | **RT denoiser histories** (`rend::DeferredLightFiltering`, "DLF", `temporal_feedback`, `m_pColorHistory`) | globals, each a `Texture2D*` at render resolution: **renderer+0x90eac8** (RT indirect diffuse GI, `filterDiffuseNoise` from the RT pass), **renderer+0x912b18** (RT reflections, `filterSpecularNoise` renderer+0x15bd0), **renderer+0x910b98** (shadow-buffer diffuse, `filterDiffuseNoise` from renderer+0x248940), **renderer+0x90eb98** (`filterColorNoise` renderer+0x17050: RT spot contact shadows, `rt_spot_deferred.rfx`, "rt:Enable Shadow Temporal Filter") | one buffer each, read-modify-write per render | static: `renderer+0x14c90`, `+0x15bd0`, `+0x17050`, callers RT pass `renderer+0x128d20` (strings `rt_deferredReflection`, `rt_deferredDiffuseGI`, `rt_SunShadow`, `rt_AO`), `renderer+0x23ed10` |
| T6 | **Volumetric light temporal blur** ("Volumetric Light:Enable Temporal Blur", `g_sTemporalBuffer`) | object (this of renderer+0x196dd0): +0x10 temporal `Texture2D*`, **+0x40..+0x6c remembered camera** (position/direction) | per render | static `renderer+0x196dd0` (same scheme as TB's VolumetricLight) |
| T7 | **View-volume (froxel) lighting temporal filter** ("VolumeLight:Enable Temporal Filtering", `g_tViewVolumePreviousLighting`, `g_mViewToPreviousScreen`) | object (this of renderer+0x191de0): 3D lighting volumes (`param_1[6]`, `[7]` and an array of previous volumes) | per render; index not resolved | static `renderer+0x191de0` (partial) |
| T8 | **Object previous transforms** (object velocity) | `rend::Object` +0x70 current local-to-world, **+0xa0 previous** (float 4x3; `setLocalToWorld` renderer+0x1c77d0, `setPreviousLocalToWorld` renderer+0x1c7910, exports); GPU: `RenderInstanceBuffers` current +0x58 / **previous +0x60**, skinning +0x68 / **previous skinning +0x70**, CPU arrays +0x80 / +0x90, filled in the "Scene update" pass `renderer+0x123d60` → `RenderInstanceManager::beginForRendering` renderer+0x10f8e0 (export) | **where +0xa0 is refreshed from +0x70 is not yet found** (game side only calls `setPreviousLocalToWorld` at construction: coregame `construct` +0x18e6a0) | static, partial (Q4) |
| T9 | **VectorBlur** (motion blur and the full-screen camera velocity that DLSS also consumes, dlss.md 3) | `*(RendererX86+0x118)`, singleton `VectorBlur::getInstance` renderer+0x115550 (global +0x902d38): +0x50..+0x10c matrices (float), +0x114 reset, +0x115..+0x117 paused / free-cam flags | derived **each render from the main view's T1**; no long history except the velocity buffers | static `renderer+0x22e8f0` |
| T10 | **DLSS feature history** | d3d (`DLSSData` feature cache) | per feature | dlss.md 1, other agent |
| T11 | **Camera cut** renderer+0x802923 | set by `VectorBlur::makeCameraCut` (renderer+0x22dfc0) and by renderFrame when the main view has no previous (`|= mainView[0x4b0]==0`); slot 15 also clears mainView+0x4b0 when VectorBlur+0x114 (reset) or the cut flag is set; cleared in `end` | per frame | static; shared by both eyes by construction, as wanted |
| T12 | Auto exposure ("autoexposure" pass), flares, film grain, GPU particles | not resolved | per render? | Q6 |

Camera motion vectors: velocity = full-screen camera reprojection from T1 (VectorBlur `init_velocity`,
`g_mVectorBlurScreenToPreviousScreen` = VectorBlur+0x50; dlss.md 3) plus object velocity from T8.
With T1 per eye, the eye offset never reads as camera motion. The TAA (T2) does not use motion
vectors for the camera at all: it reprojects with the stored `m::View` of each ring slot. A
per-eye ring is both necessary and sufficient for TAA.

---

## 4. Render resolution, the RT pool, UI composition and the back buffer

### 4.1 Sizes

`RendererX86::setScreen` slot 2, renderer+0x11b0c0 (static; arguments `(vpX, vpY, outW, outH,
bbW, bbH, windowMode, bool)`):

| Global | Meaning |
|---|---|
| renderer+0x90e188 / +0x90e18c | **viewport offset** of the output inside the back buffer |
| renderer+0x90e248 / +0x90e24c | **output size** (UI, final image, TAA/DLSS output) |
| renderer+0x90e308 / +0x90e30c | **back-buffer size** → `d3d::DeviceUtil::resizeBackBuffer(bbW, bbH, ...)` (export) |
| renderer+0x90e3c8 / +0x90e3cc | TAA output size (= render size, or back-buffer size when renderer+0x802922) |
| renderer+0x90e488 / +0x90e48c | **render (internal) resolution** = RendererX86+0x718 / +0x71c (all G-buffer, depth, lighting and DLF histories are created at this size) |

- When any size changes, it flushes the graph's persisted resources (`renderer+0xa26f0`) and
  calls `RenderTargetManager::trim`.
- renderFrame calls slot 2 with the current values every render.
- The back-buffer size and the output size are independent, and the engine offsets the output by
  (vpX, vpY) when blitting and drawing the HUD (`end`, `renderHUD`). A 2W-wide back buffer with a
  W-wide output at offset 0 or W is therefore expressible in the engine's own terms (Q9).

### 4.2 The pool

`rend::RenderTargetManager` (exports: `allocTexture2D`, `allocDepthBuffer`, `free`, `trim`; instance
renderer+0x8028d8 and RendererX86+0x20). It is a free list matched on the full `Texture2DInit`; the
name is a debug label (section 3). Transient graph resources are allocated and freed around their
first and last pass. Histories are allocated by their owners and kept.

### 4.3 Final image, UI and back buffer

Static:
- The render graph registers the **back buffer** (`d3d::DeviceUtil::getAutoBackBufferTexture`, export)
  as external resource #0 (hash 0x606c944f51897ca3) in `renderer+0x9fd60`. The final passes
  ("final image pass", TAA/DLSS, "Final Post Process and Display Map") produce the output-resolution
  image.
- `renderHUD` (slot 17): if RendererX86+0x3c8 is null and +0x108 is set, it creates **+0x3c8
  "RendererX86::m_pCreatedColorTarget"** (output size). It draws ShapeEngine and the Coherent UI
  (`(*(*(r+0x680)))()`, gated by `RenderOptions::PaintUI` renderer+0x7ec146) into +0x3c8, or into
  the back buffer at the viewport offset. In HDR it draws into **"RendererX86::pSeparateHudColorTarget"**
  first and composes with `copy.rfx/composeSceneAndUI` (compute). dlss.md 3 has the same reading.
- `end` (slot 18): if +0x3c8 exists and is not the back buffer, it is drawn into the back buffer at
  (vpX, vpY, outW, outH) with `copy.rfx/copyWithGamma` (or `renderer+0x1e6160`). Then Present.
- Exactly which target the final post writes in the shipped SDR configuration (back buffer directly,
  or +0x3c8) is open (Q2).

Engine-side copy helpers usable by a mod on the renderer thread (exports, d3d):
- `d3d::NativeTextureUtil::copy(dst, src)` and `copyRegion(dst, sub, ..., dstPos, src, ..., srcPos, size)`
  record into the engine's current command list; the TAA uses `copy` itself.
- `d3d::NativeTextureUtil::createTexture2D(...)`.
- `d3d::DeviceUtilDX12::submitGraphicsCommandList()`.

These let a mod record its eye copies in order inside the engine's own stream, which is what TB
had to build its command-list layer for.

---

## 5. Tweakables and options

- **Registry**: `d::BaseTweakable` in **rl** (exports):
  - ctor `(name, type)` rl+0x1dc770 appends `this` to a null-terminated array **rl+0x3a7be0**
    (count rl+0x3a7bd8, max 0x1000, mutex rl+0x3b6f70).
  - `getTweakable(name)` rl+0x1dc950 (linear search), `getTweakables()` rl+0x1dc940,
    `setTweakable(name, valueString, bool)` rl+0x1dccf0, `getTweakableStrValue(name, bool)`
    rl+0x1dc9c0.
  - Layout: +0x08 `r::String` name (chars at +0x10), +0xa8 type, +0xac changed-by-code, **+0xb0
    value**; float: +0xb4 min, +0xb8 max, +0xbc default (static: `Tweakable<float>` ctor
    renderer+0x132b0).
  - Types: 0 bool, 2 float (static, ctors).
  - Most are function-local statics, so they exist only once their code has run (as in AWR).
    `setTweakable` by name is the cheap lab lever.
- Relevant tweakables (strings):
  - "Camera:Override FOV" (bool, exe+0x12ba6d0) and "Camera:Override FOV Value" (float, value
    exe+0x12ba840): an absolute FOV in degrees, read in `exe+0x383c60`.
  - "SSAA:Temporal Jitter Scale" (renderer+0x911420), "SSAA:Use Temporal AA + FXAA", "SSAA:Draw
    previous", "SSAA:Depth delta bias", "SSAA:Velocity rejection strength".
  - "Renderer:Make Camera Cut", "Renderer:Temporal MIP Bias".
  - "rt:Enable Reflections", "rt:Enable indirect diffuse", "rt:Enable Sun Shadows", "rt:Enable Spot
    Contact Shadows", "rt:Enable Raytraced Transparents", "rt:Enable Shadow Temporal Filter".
  - "Shadow Buffer:Temporal blur sun shadows", "SSR:Temporal blur SSR color", "SSAO:Temporal blur
    SSAO".
  - "Volumetric Light:Enable Temporal Blur", "VolumeLight:Enable Temporal Filtering".
  - "Camera_DOF:DOF Enabled".
- **Render options** are plain exported statics, `rend::RenderOptions::*`:
  - `ScreenSpaceAntialiasing` renderer+0x7ec159: **the TAA on/off flag** (renderFrame gates the
    jitter on it).
  - `Antialiasing` +0x7ec15a; `DLSSEnabled` +0x802a3f; `RenderMotionBlur` +0x7ec145;
    `RenderVectorBlur` +0x7ec149; `RenderVelocityPass` +0x7ec153; `RenderVolumetricLights`
    +0x7ec14f; `RenderSSAO` +0x7ec043; `EnableRayTracingFeatures` +0x7ec15b; `PaintUI` +0x7ec146;
    `FOVMultiplier` +0x7ec13c.
  - Many more are in the exports.
- **FOV**: base exe+0x123904c (degrees, script "SetMaximumFOV"), times the menu multiplier
  `RenderOptions::FOVMultiplier`, applied in `exe+0x383c60`. Section 2.1 has the details.

---

## 6. Recommended stereo design for Threshold

### 6.1 Shape: two `renderWorld`s inside one renderer frame (TB's view path)

On the **renderer thread**, hook **RendererX86 vtable slot 15** (renderer+0x11daf0, through the
vtable renderer+0x6323d8 or by detour; also slot 16 renderer+0x11d9e0 for completeness). When
stereo is active and the argument is the world view:

1. Make two copies of the incoming `m::View`. For eye e = ∓1:
   - `row3 += e·sep·row0` through `m::View::setViewToWorld` (rl+0x1121c0).
   - Optionally `viewToClip[2][0] += e·sep·m00/Zc` through `m::View::setViewToClip` (rl+0x1115f0)
     for convergence.
2. `eyestate_enter(L)` (below); call the original with the left view; `eyestate_leave(L)`.
3. Copy the left eye's output out (back buffer or +0x3c8 region) with the engine's own
   `d3d::NativeTextureUtil::copyRegion`. It is recorded in order on the renderer thread's list, so
   no command-list layer is needed (verify, Q3).
4. `eyestate_enter(R)`; original with the right view; `eyestate_leave(R)`.
5. Compose: left image to the left half, right image to the right half, of a 2W back buffer (the
   Present agent's swapchain, or the engine's own via setScreen, Q9).

`begin` (slot 14), `renderHUD` (slot 17), `end` (slot 18) and `present` (slot 30) then run once per
update. The counter, frame time, jitter phase, camera cut and `storePrevious` stay per update.
There is one Present per update, as the engine's own back-buffer index requires (dlss.md 2.3).

Per-eye state (`eyestate`), all pointer/struct swaps of section 3. The left eye uses the engine's
own state; the right eye gets a second set:
- **T1**: main view (RendererX86+0x678) +0x3d0..+0x4b0 per eye. Because `storePrevious` runs only
  in `end`, call it for the left eye's copy yourself (or snapshot the left eye's current matrices
  after its render). Restore the engine's copy before `end`.
- **T2**: SSAA ring, `*(RendererX86+0x138)` +0x40..+0x9d0 (three `m::View` + texture pointers) and
  +0x10..+0x28. The right eye's textures come from the pool on its first use (the engine allocates
  them when a slot pointer is null).
- **T3**: graph previous-frame list renderer+0x910c20 / +0x910c28 (and its capacity word): swap per
  eye so `lastcolorbuffer` / `previouslineardepth` are the same eye's.
- **T4**: shadow-buffer ping-pong pointers (+0xf0..+0x140 of the object at RendererX86+0xd8).
- **T5**: DLF globals renderer+0x90eac8, +0x912b18, +0x910b98, +0x90eb98.
- **T6**: volumetric temporal buffer + remembered camera.
- **T7**: froxel previous volumes (once their slots are pinned down).
- **T10**: DLSS: a second feature per eye (other agent).
- **T8**: object previous: see Q4. If it is refreshed per render, hold it across the first eye, as
  TB does.

### 6.2 HUD

`renderHUD` draws once, after both eyes, in screen space. Two options:
- Duplicate its draws into both halves, as TB does.
- Prefer the engine's own isolation: in HDR (and with RendererX86+0x108 set) the UI is drawn into
  `pSeparateHudColorTarget` and composed by `composeSceneAndUI`. Forcing that path gives a HUD
  texture to compose into both halves (Q10).

### 6.3 Pass-2 shortcuts (later)

Shadow maps ("Shadow maps" pass) and cube probes are re-rendered by the second `renderWorld`.
The TB measurements suggest reusing them for the second eye (+10 %). The graph's pass list makes
skipping a named pass plausible.

### 6.4 Hook table with signatures (first bytes, from `re/bin`)

| Function | RVA | First bytes | Notes |
|---|---|---|---|
| RendererX86 slot 15 renderWorld (5 args) | renderer+0x11daf0 | `48 89 5c 24 08 48 89 6c 24 10 48 89 74 24 18 48 89 7c 24 20 41 56 48 83 ec 40 48 8b 99 78 06 00` | **main hook**. Identical to slot 16 for 32 bytes; check the vtable slot too |
| RendererX86 slot 16 renderWorld (4 args) | renderer+0x11d9e0 | same 32 bytes | |
| renderFrame (jitter + graph) | renderer+0x11dc50 | `48 8b c4 4c 89 48 20 55 56 57 41 54 41 55 41 56 41 57 48 8d a8 38 f6 ff` | alternative hook (receives the main `rend::View`) |
| render graph | renderer+0x11ee20 | `48 8b c4 55 56 57 41 54 41 55 41 56 41 57 48 8d a8 98 f9 ff ff 48 81 ec` | (r, mainView, jView, space, sun, space2) |
| graph executor | renderer+0xa09a0 | `4c 8b dc 41 56 48 81 ec a0 00 00 00 48 8b 05 5d b6 74 00 48` | persisted-resource move (T3) |
| RendererX86 slot 14 begin | renderer+0x11b4c0 | `48 8b c4 55 41 54 41 55 41 56 41 57 48 8d 68 a1 48 81 ec d0 00 00 00 48 c7 45 df fe ff ff ff 48` | ++counter renderer+0x8029d0 |
| RendererX86 slot 17 renderHUD | renderer+0x1343b0 | `48 8b c4 55 53 56 57 41 54 41 55 41 56 41 57 48 8d a8 18 fe ff ff 48 81` | HUD |
| RendererX86 slot 18 end | renderer+0x134bb0 | `48 8b c4 55 57 41 55 41 56 41 57 48 8d a8 28 fa ff ff 48 81 ec b0 06 00 00 48 c7 44 24 50 fe ff` | storePrevious, +0x3c8 blit |
| RendererX86 slot 30 present | renderer+0x1363e0 | `48 89 4c 24 08 48 83 ec 38 48 c7 44 24 20 fe ff ff ff 80 b9 76 04 00 00` | → d3d present |
| RendererX86 slot 2 setScreen | renderer+0x11b0c0 | `48 8b c4 56 57 41 55 41 56 41 57 48 81 ec 00 01` | sizes |
| `rend::View` storePrevious | renderer+0x180700 | `48 89 5c 24 08 48 89 7c 24 10 55 48 8b ec 48 83 ec 60 48 8d 91 e0 00 00 00 48 8b f9 48 8d 4d c0` | T1 |
| `rend::View` previous/uniform update | renderer+0x17f4a0 | `48 8b c4 48 89 58 08 48 89 70 10 48 89 78 18 4c 89 70 20 55 48 8d a8 38` | |
| jittered view build | renderer+0x180940 | `48 89 5c 24 08 48 89 74 24 10 48 89 7c 24 18 4c` | |
| TAA pass | renderer+0x132000 | `48 89 4c 24 08 55 56 57 41 54 41 55 41 56 41 57` | |
| TAA resolve (TAA + upscale) | renderer+0x1477c0 | `48 8b c4 55 53 56 57 41 54 41 55 41 56 41 57 48 8d a8 c8 fc ff ff 48 81` | T2 |
| SSAA 3-frame resolve | renderer+0x146a20 | `48 8b c4 4c 89 48 20 4c 89 40 18 48 89 50 10 55 56 57 41 54 41 55 41 56` | T2 |
| shadow buffer temporal | renderer+0x248940 | `48 8b c4 55 56 57 41 54 41 55 41 56 41 57 48 8d a8 88 fd ff ff 48 81 ec` | T4 |
| DLF filterDiffuseNoise / Specular / Color | renderer+0x14c90 / +0x15bd0 / +0x17050 | `48 8b c4 44 89 40 18 48 89 50 10 48 89 48 08 55` / `48 8b c4 44 88 48 20 44 89 40 18 48 89 50 10 48` / `48 8b c4 4c 89 40 18 48 89 50 10 48 89 48 08 55` | T5 |
| RT pass | renderer+0x128d20 | (not taken) | |
| volumetric temporal | renderer+0x196dd0 | `48 8b c4 55 41 54 41 55 41 56 41 57 48 8d 6c 24` | T6 |
| view volume temporal | renderer+0x191de0 | `48 8b c4 48 89 48 08 55 53 56 57 41 54 41 55 41` | T7 |
| VectorBlur update | renderer+0x22e8f0 | `48 8b c4 55 56 57 41 56 41 57 48 8d a8 e8 fc ff` | T9 |
| `RendererInterfaceWrapper::renderWorld` (5 args) | renderer+0x102c40 | `48 8b c4 55 41 56 41 57 48 8d a8 58 f4 ff ff 48` | game-thread side, export |
| `VisibilitySystem::launchAsyncQuery` | renderer+0x1af180 | `48 8b c4 55 57 41 56 48 8d a8 18 fd ff ff 48 81` | export |
| renderer thread loop | renderer+0x10d150 | `48 8b c4 57 48 83 ec 70 48 c7 40 a8 fe ff ff ff` | |
| `m::View::setViewToWorld` | rl+0x1121c0 | `48 81 ec 88 00 00 00 48 8b 02 4c 8b c9 48 89 41` | export, call it |
| `m::View::setViewToClip` | rl+0x1115f0 | `48 81 ec 88 00 00 00 48 8b 02 4c 8b d1 48 89 81` | export, call it |
| `m::PerspectiveView::calculateProjection` | rl+0x11eb40 | `40 53 48 81 ec 80 00 00 00 f3 0f 10 99 ec 02 00` | export |
| `d::BaseTweakable::getTweakable` / `setTweakable` | rl+0x1dc950 / rl+0x1dccf0 | `48 89 5c 24 08 48 89 74 24 10 57 48 83 ec 20 48` / `40 55 56 57 41 54 41 55 41 56 41 57 48 8d 6c 24` | exports |
| GameClient::update | exe+0x396380 | `48 8b c4 57 48 81 ec 80 00 00 00 48 c7 40 98 fe ff ff ff 48 89 58 08 48 89 70 10 0f 29 70 e8 0f` | game thread tick |
| GameClient::render | exe+0x396ce0 | `40 57 48 81 ec 80 00 00 00 48 8b 81 70 05 00 00 48 8b f9 80 b8 a1 01 00 00 00 74 22 48 8b 05 d5` | |
| frame begin | exe+0x5688b0 | `48 89 5c 24 08 48 89 74 24 10 57 48 83 ec 30 41 0f b6 f8 0f b6 f2 48 8b d9 ff 15 a1 3a 83 00 48` | |
| frame render / HUD / end / present | exe+0x568b20 | `48 89 5c 24 18 55 56 57 48 81 ec 40 02 00 00 48 8b 05 da 04 cd 00 48 33 c4 48 89 84 24 30 02 00` | |
| world render (view → wrapper) | exe+0x65b110 | `48 8b c4 41 56 48 81 ec 40 06 00 00 48 c7 44 24 30 fe ff ff ff 48 89 58 08 48 89 68 10 48 89 70` | |
| camera publish (g_renderView, FOV) | exe+0x383c60 | `48 8b c4 55 48 8d 68 a1 48 81 ec c0 00 00 00 48 c7 45 b7 fe ff ff ff 48 89 58 08 48 89 70 10 48` | |
| high-res screenshot (N renders per update) | exe+0x569360 | `4c 8b dc 55 56 57 41 54 41 55 41 56 41 57 48 81 ec e0 03 00 00 48 c7 44 24 40 fe ff ff ff 49 89` | proof of concept |

Several prologues are generic (`48 89 5c 24 08 ...`). Use at least 24-32 bytes, or check the
vtable slot contents, before hooking.

### 6.5 Open questions for the lab

- **Q1 Threads and depth.** Confirm that slot 15 / begin / end / present run on "RMD - Renderer
  Thread" (thread id ≠ game thread). Measure how many frames the game thread runs ahead (packet
  batch count) and whether `RendererThreading` ever runs inline.
- **Q2 Final image target.** At slot 17 entry, is RendererX86+0x3c8 null (final post wrote the back
  buffer) or not? Check RendererX86+0x108 in SDR and HDR.
- **Q3 Copy ordering.** Are the left eye's lists submitted before the right eye's passes record
  (`submitGraphicsCommandList` inside the graph), or all at end/present? Does
  `NativeTextureUtil::copyRegion` from the slot-15 hook land between the eyes on the GPU? Check
  with a census like TB's `stereo census N cl`.
- **Q4 Object previous transforms.** Find who refreshes `Object+0xa0` / the previous-instance and
  previous-skinning buffers, and whether it happens per `renderWorld` (then the second eye sees
  zero object motion, TB section 5). A watchpoint on one object's +0xa0 answers it.
- **Q5 Second-render side effects.** Run slot 15 twice at separation 0 and diff the halves (TB's
  control). It catches anything per render not in section 3.
- **Q6 GPU sims per render.** Check auto exposure, flares, film grain, GPU particles.
- **Q7 Visibility.** Both eyes use the one Umbra query launched on the game thread with the centre
  camera. Check for edge popping at 32.5 mm. If needed, launch the query with a widened FOV.
- **Q8 DLSS per eye.** With the other agent: one NGX feature per eye; reset behaviour.
- **Q9 2W back buffer.** Can the engine run with `setScreen(vpX, 0, W, H, 2W, H)`, i.e. a 3840x1080
  swapchain and a 1920x1080 output? Then per eye vpX = 0 / 1920 (only `end`'s blit and the HUD use
  the offset) or the Present agent's widened swapchain.
- **Q10 HUD.** Force the separate-HUD path (`pSeparateHudColorTarget`) in SDR and compose it into
  both halves, or duplicate the draws.
- **Q11 Signs.** Check the eye axis and sign (row 0 = right, left eye −sep) and convergence (`[2][0]`
  term) against measured disparity, as in TB section 2.
- **Q12 Froxel lighting (T7).** Resolve the previous-volume array indexing.
- **Q13 Cutscenes and photo mode.** Cutscenes and photo mode use other camera indices
  (exe+0x129a5cc) but the same world render; Bink videos go through `renderWorldVideoOnly`
  (no stereo).
