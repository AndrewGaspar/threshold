# DLSS Frame Generation / Multi Frame Generation

> **Branches:** on `main`, `[fg] pace` defaults to `ready` (the measured v1 behaviour). `smooth` is
> built but not yet A/B-measured; branch `wip/fg-pacing` makes it the default. `count = auto` is opt-in
> and has not been run live yet (resume notes at the end).

Control has no frame generation. Threshold adds it through NGX directly (no Streamline), on the
game's own D3D12 device, queue and NGX instance. Code: `src/features/fg/`: `fg.cpp` (inputs,
evaluation, present thread, pacing, limiter, `count = auto`), `engine.cpp` (camera and HUD-less taps
on the renderer), `trace.cpp` (the present trace), `common.h`. Tools: `tools/fg/`. Static
background: [`re/dlss.md`](../re/dlss.md) sections 2–3, [`re/stereo.md`](../re/stereo.md) 2.3–2.4.
Prior art: Pharos (`~/code/ac-dlss/docs/FRAME_GENERATION.md`), which needed a second D3D12 device and
shared textures because Origins is D3D11.

## How it works

1. **Inputs.** A hook on the game's exported `NVSDK_NGX_D3D12_EvaluateFeature_C` runs the game's
   DLSS SR evaluation, then reads `Depth`, `MotionVectors`, jitter, MV scale and the render subrect
   from the game's own parameter block and records copies of depth and MVs on the game's command
   list (queue order, no CPU sync). In the same call (renderer thread, inside the world render) it
   reads the **camera** (below) and arms the **HUD-less** copy for this frame.
2. **Proxy back buffers.** The engine keeps its own back-buffer index (it flips 0/1 after each
   Present and never asks DXGI; static `re/dlss.md` 2.2), so `GetBuffer(0/1)` returns two textures of
   ours. The engine renders into those; the real swapchain (3 buffers, `[fg] buffers`) only ever
   receives our copies. At Present, `d3d::DeviceUtil::getAutoBackBuffer()` (export) tells which proxy
   the frame is in (the index flips after the Present call).
3. **Evaluation at the game's Present** (renderer thread): copy the proxy aside (`real`), then one
   DLSS-G `EvaluateFeature` per generated index (`MultiFrameCount = count - 1`, `MultiFrameIndex =
   1..N`) into per-index outputs, on the game's queue. The feature is created once for the GPU's
   maximum (5 generated frames on RTX 50), so `count` changes are free.
4. **Present thread**: waits for DLSS-G's fence (re-checked after every wake, at most 100 ms, else
   the job's generated frames are dropped), presents generated 1..N, then the real frame, `step =
   interval / count` apart (pacing below). A job superseded before it starts presents only its real
   frame (`skipped`). Two texture sets; the renderer thread waits for the present thread before
   reusing one (`game waited` in the perf line).
5. **Limiter** (`[fg] fps_limit`, or `count = auto`'s own cap): the renderer thread sleeps at the end
   of the game's Present, after the frame was handed to the present thread, so the next frame starts
   later and the finished one isn't delayed.

### Camera (`[fg] camera = engine`, default)

`rend::RendererX86::getInstance()` (renderer export; `mov rax,[renderer+0x802a00]`) `+0x678` is the
main `rend::View` (static, re/stereo.md 2.3–2.4; only slots 15/16 write it, from the game's
`m::View`). Read in the DLSS evaluate tap, so it is the frame's own view, with the same frame's
jitter and MVs. Matrices are doubles, row vectors, row-major, which is DLSS-G's convention (`v' = v
M`), so they are passed as they are (converted to float):

- `CameraViewToClip` = view-to-clip (`+0x10+0xd0`), `ClipToCameraView` = clip-to-view (`+0x10+0x150`).
  The main view's projection has no TAA jitter: renderFrame applies the jitter to a local copy.
- `ClipToPrevClip` = clip-to-view · view-to-world (`+0x10+0x70`) · previous world-to-view (`+0x450`,
  stored by `storePrevious` in `end`) · previous view-to-clip (`+0x3d0`); `PrevClipToClip` its
  inverse. Without a previous (`+0x4b0` = 0: camera cut) the current matrices are used and Reset is
  set.
- Basis from the view-to-world rows (right, up, forward, position; left-handed, +z forward);
  near/far from the projection (`m22 = f/(f-n)`, `m32 = -n f/(f-n)`): **near 0.200, far 4498.1**,
  vertical FOV 39–43° (the game animates its zoom), aspect 1.778 (live r4).
- **Verified live** (r4): our `ClipToPrevClip` equals the engine's own float copy of
  `g_mClipToPreviousClipNoJitter` (`rend::View+0x7e8`, 16 floats after an 8-byte handle) in every
  logged frame, e.g. row 0 `0.9619 0 0 0`, row 3 `-0.0002 -2.3823 3.3309 4.3308` both.

`camera = identity` keeps the v1 stand-in (identity matrices, a synthetic basis, near 0.1, far 1000).

### HUD-less colour (`[fg] hudless = 1`, default)

The UI (Coherent GT) is drawn by `renderHUD` (RendererX86 vtable slot 17, `renderer+0x1343b0`) into
the frame's colour target `RendererX86+0x3c8`, which `end` then blits to the back buffer with
`copy.rfx/copyWithGamma` (re/dlss.md 3, docs/stereo.md). FG patches **slot 17 in the vtable**
(`renderer+0x6323d8`) instead of hooking the function: the slot is only reached through the vtable
(the wrappers `jmp [vtbl+0x88]`), and a vtable patch chains into whatever the body is, including
stereo's inline detour, so nothing is hooked twice. The patch is made lazily at the first frame
generation Present, after every feature's start-up checks of that vtable (stereo compares slot
contents at start). With FG disabled nothing is patched. When the DLSS tap armed it, the patched slot
copies `+0x3c8` into a texture from the engine's own render-target pool
(`RenderTargetManager::allocTexture2D`, same size and format) with the engine's exported
`d3d::NativeTextureUtil::copyRegion`, so the copy is recorded in order on the engine's own command
list with the engine's state tracking (the mechanism docs/stereo.md found reliable), then calls the
original. At Present the copy is passed as `DLSSG.HUDLess` (transitioned from the engine's tracked
state and back on our list).

- Format: the colour target is DXGI 24 (R10G10B10A2), the swapchain's format (live r4).
- **Verified live** (r6, `[fg] show = hudless` presents the HUD-less copy instead of the real frame,
  generated frames off): the "W, A, S, D Move" hint is gone, and outside the hint box the HUD-less
  frame equals the presented real frame to 0.80/255 mean (p99 21/255), less than two real frames
  of the same still scene differ (2.5/255): `copyWithGamma` is the identity at the default brightness.
  (A different in-game brightness would make HUD-less and back buffer differ; not checked.)

## Measuring (`tools/fg/`, ported from Pharos)

- `quiver lab call <i> fg trace [presents] [shots] [band y] [band h]` arms the present trace
  (`trace.cpp`): per present the game's Present arrival, our Present call and return (QPC µs), kind
  (G / R / N), generated index and multiplier, and for the first `shots` a copy of a 1920x64 band
  (y = 303 by default) of exactly what was presented, in present order, into a readback buffer on
  the present list. Written to `%QUIVER_LAB_RUN_DIR%\fgtrace\{presents.csv,bands.bin}` (10-bit
  bands, format code 2). `fg status` reports the perf numbers and the trace state.
- `tools/fg/fg-trace <inst> <label> [dx] [period_ms]`: `tools/fg/spin.py` (XTEST relative mouse-look
  inside the container, 2 px every 2 ms on an absolute timeline: ~4000 px/s of image motion here),
  arms the trace 2.5 s into the spin, copies it to `build-fg/fgtrace/<label>/` and runs
  `tools/fg/fg-pacing.py`: shift ratio per generated index by phase correlation on two sub-bands
  (expected k/N), ratios outside [0,1], ordering, present-call intervals by transition, and the
  position residual (cumulative image shift against present time, minus a 9-present local linear
  fit; what reads as judder on a VRR display). `tools/fg/fg-series` runs live A/B specs in one
  session (`quiver lab set`, then fg-trace under `quiver lab gpu`); `tools/fg/fg-table.py` makes the
  rows below; `tools/fg/hud-ghost.py` measures the HUD in generated frames from a trace whose band
  is on a HUD element.
- Test settings (instance `control-fg`, its own install copy): 1920x1080, DLSS Quality (render
  1280x720), ray tracing off, texture quality 1, **motion blur and film grain off** (the blur smears
  the bands and the grain changes every frame, both blur the phase correlation), Oldest House
  entrance from the `game-start` fixture after the intro (`skip` verb). RTX 5070 Laptop.
- **Contention:** the GPU was shared with other agents' instances throughout (`control-stereo`,
  sometimes `control-nav`; `quiver lab gpu` takes the lock but they have no throttle line). With three
  Control instances VRAM was full (7.7 of 8 GB) and every game ran at 1–20 fps (and the v1 binary
  exited twice at load time in that state, r2/r3, no exception logged); those runs (s2-*-b, p1-*) are
  not used. The rows below are from windows with one other instance mostly idle, and each A/B is
  interleaved in one session. Real fps vary between rows with that load.

## Measured

All rows: run, real (rendered) fps, presents/s, DLSS-G done after the game's Present (perf line),
shift ratio median (p10–p90) per generated index, ratios outside [0,1], present-call interval sd,
position residual |mean| / p90 (VRR model). Traces in `build-fg/fgtrace/<label>/`.

### 1. v1 baseline (camera identity, no HUD-less, `pace = ready`; run r5)

The baseline is the current build with the v1 behaviour selected live (`camera = identity`,
`hudless = 0`, `pace = ready`). It differs from v1 only in the interval average (waits for the
present thread are taken out, hitches capped at 2.5x) and the fence wait (re-checked after wakes,
100 ms), which change nothing in a steady state. (The v1 binary itself, with the trace added, was run
first, r1: under heavy contention at 10–18 real fps 55 of 73 and 126 of 203 ratios fell outside [0,1],
residual 26–17 px; not comparable.)

| trace | run | real fps | presents/s | DLSS-G done ms | shift ratio per index | outside [0,1] | interval sd ms | residual px |
|---|---|---|---|---|---|---|---|---|
| base-2x | r5 | 115.1 | 230.2 | 11.2 | 0.500 (0.489–0.513) | 6 of 238 | 0.37 | 4.92 / 9.76 |
| s2-id-2x | r5 | 92.2 | 184.4 | 13.1 | 0.501 (0.492–0.515) | 6 of 238 | 0.44 | 4.16 / 8.22 |
| s1-base-4x | r5 | 82.1 | 328.3 | 14.8 | 0.250 (0.245–0.254), 0.500 (0.494–0.505), 0.750 (0.745–0.754) | 0 of 354 | **2.67** | **9.45 / 18.97** |

Interpolation is right: every index sits at k/N with a tight spread, nothing out of order. The 6 of
238 outliers at 2x are measurement misses, not interpolation errors: all in the right sub-band over
12 consecutive presents, where the correlation locked on static content (R→G and G→R shift 0.0 px
while R→R′ was −22 to −43 px; peaks 0.12–0.31), the left band fine. Similar counts with the engine
camera. The weak point is **4x pacing**: present-call intervals
R→G 0.18 ms, G→G bimodal 2.4 / 9.2 ms (sd 2.64), G→R 2.55 ms: DLSS-G finishes 14.8 ms after the
game's Present while the real interval is 10 ms, so with `ready` the first generated frame goes out
the moment it's done, right after the previous real frame, and the game waited for the present
thread 130–142 times per 2 s (~3 ms each).

### 2. Engine camera (r5, interleaved with identity)

| trace | camera | real fps | DLSS-G done ms | shift ratio per index | outside | interval sd | residual px |
|---|---|---|---|---|---|---|---|
| s1-base-4x | identity | 82.1 | 14.8 | 0.250 (0.245–0.254), 0.500 (0.494–0.505), 0.750 (0.745–0.754); sd 0.023 / 0.016 / 0.010, min 0.001 | 0 of 354 | 2.67 | 9.45 / 18.97 |
| s1-cam-4x | engine | 81.0 | 14.7 | 0.250 (0.246–0.255), 0.500 (0.494–0.506), 0.750 (0.747–0.755); sd 0.007 / 0.008 / 0.008, min 0.225 | 0 of 354 | 2.66 | 10.88 / 19.00 |
| s3-i2 / s3-i2b | identity, cap 40 | 40.0 | 7.5 / 7.2 | 0.500 (0.479–0.509) / 0.500 (0.484–0.517) | 7 / 9 of 236 | 1.08 / 1.26 | 4.27 / 8.38, 4.30 / 9.13 |
| s3-e2 / s3-e2b | engine, cap 40 | 40.0 | 7.6 / 7.5 | 0.500 (0.473–0.524) / 0.500 (0.477–0.511) | 8 / 9 of 236 | 1.11 / 1.18 | 3.59 / 7.27, 3.99 / 7.99 |
| s3-i4 / s3-i4b | identity, cap 30 | 30.0 / 29.2 | 9.6 / 16.7 | 0.250 (0.241–0.257), 0.500 (0.489–0.509), 0.750 (0.739–0.757) / similar | 0 of 354 | 1.29 / 1.28 | 3.28 / 6.49, 4.89 / 9.74 |
| s3-e4 / s3-e4b | engine, cap 30 | 29.5 / 30.0 | 8.5 / 10.4 | 0.250 (0.239–0.260), 0.500 (0.487–0.511), 0.750 (0.741–0.761) / similar | 0 of 348 / 354 | 1.64 / 0.93 | 4.95 / 11.75, 3.43 / 7.91 |

Result: **no measurable change** in the shift ratio or residual (as Pharos found with its engine
camera); the one uncapped 4x pair has a tighter per-index spread and no near-0 outlier with the
engine camera, the capped pairs don't separate. The game's MVs include camera motion (re/dlss.md 3),
which is what DLSS-G mostly uses. Kept on by default because it is what DLSS-G is specified to get
(correct depth linearisation from real near/far, camera cut → Reset).

### 3. HUD-less (r5, band on the "W, A, S, D Move" hint, y 776–832, 4x at a 30 fps cap)

`BAND_Y=776 BAND_H=56 tools/fg/fg-series control-fg h1 ...`, then `tools/fg/hud-ghost.py <dir>
--hud 120 380 --scene 900 1160`: |G − mean(R, R′)| over the hint's glyph pixels (bright in both
real frames; static, so ideally 0), over the hint box, and over a scene box of the same size.

| trace | hudless | glyph pixels | hint box | scene box | shift ratio sd per index (band through the hint's row) | out of order |
|---|---|---|---|---|---|---|
| h1-nohud4 | 0 | 0.0009 (p90 0.0012) | 0.0031 | 0.0191 | 0.025 / 0.065 / 0.046 (min 0.081, 0.031, 0.515; 1 outside) | 1 |
| h1-hud4 | 1 | **0.0005** (p90 0.0007) | 0.0033 | 0.0209 | **0.007 / 0.010 / 0.009** | 0 |
| h1-nohud4b | 0 | 0.0009 (p90 0.0011) | 0.0034 | 0.0178 | 0.045 / 0.064 / 0.036 (min 0.001, 0.036, 0.652) | 0 |
| h1-hud4b | 1 | **0.0005** (p90 0.0007) | 0.0032 | 0.0212 | **0.009 / 0.010 / 0.009** | 0 |

(Values are fractions of full scale; real→real on the glyphs is 0.0000.) DLSS-G alone already keeps
this static hint nearly intact (a generated-only capture during a spin shows it clean either way),
so the visible gain on this element is small: the glyph error halves. The clear effect is on the
**scene around the HUD**: without HUD-less the static hint box drags the motion estimate of the image
rows it sits in (index spreads 4–6x wider, single frames at ratio 0.03–0.08 where 0.25/0.5 was
expected); with HUD-less they are as tight as elsewhere. HUD elements that move or animate (the
reticle, damage numbers, objective text over a moving scene) are where ghosting would show; not
measured separately.

### 4. Pacing (`pace = smooth`)

**Implemented, not measured cleanly.** `pace = smooth` (default) is Pharos's: the first generated
frame is held until one step after the previous real frame's Present *call*, minus `pace_relax` (0.5)
of how early DLSS-G finished relative to that; every later present one step after the previous
Present's call (not its return). The interval average excludes the renderer thread's waits for the
present thread (Pharos's neutral loop) and caps hitches at 2.5x; a frame without generation restarts
it. It ran in r6 (no failures, no drops), but every trace of that session (`p1-*`) was taken with
three Control instances on the GPU at 7–13 real fps (DLSS-G done 120–230 ms after the game's
Present), and the host's NVIDIA driver then wedged (NVRM `dmaAllocMapping_GM107: can't update VA
space` at 18:29, `nvidia-smi` stuck), so the A/B could not be retaken.

What the v1 (`ready`) traces show and `smooth` targets:

- Uncapped 4x (`s1-base-4x`, above): bursts, R→G 0.18 ms, G→G 2.4 / 9.2 ms, interval sd 2.67 ms,
  residual 9.45 / 18.97 px against 4.92 / 9.76 at 2x.
- Capped (`fps_limit`, r5, `ready`): the limiter holds the rate exactly (40.0 / 30.0 real fps, game
  waited 0 times) and every transition except the first generated frame is even: 2x G→R 12.56 ±
  0.09 ms but R→G 12.40 ± **1.51**; 4x G→G 8.39 ± 0.10, G→R 8.41 ± 0.11, R→G 8.05 ± **2.56** (p1 1.0,
  p99 14.4 ms). That R→G spread is the "first generated frame as soon as DLSS-G is done" rule;
  `smooth` replaces it with a target one step after the real frame's call. Pharos measured the same
  change at 0.46 vs 2.2–2.5 ms interval sd and residual 6.7 vs 9.8 px.
- Pharos's `pace = smooth` reference: present-interval sd 0.28–0.46 ms, VRR residual 1.7–4.2 px
  mean. Threshold capped `ready`: sd 0.93–1.64 ms, residual 3.3–5.0 px mean (rows s3-* above).

Retake: `tools/fg/fg-series control-fg p2 "r4|pace=ready count=4 fps_limit=30" "s4|pace=smooth"
"r4b|pace=ready" "s4b|pace=smooth" "r4u|pace=ready fps_limit=0" "s4u|pace=smooth"` on an idle GPU.

### 5. `count = auto`

**Implemented, not exercised live** (the driver wedged before its run). Pharos's controller without
Reflex: once a second, from the rendered frames' own work time (arrival interval minus the limiter's
sleep and the waits for the present thread), within `[count_min, count_max]` (2..4): one step up when
the game can't hold `target / multiplier` (10 % short, 2 s), one step down when it could hold
`target / (multiplier − 1)` with 15 % to spare (3 s); a step down undone within 6 s blocks steps down
for 10 s, doubling to 60 s; windows below 60 % of the cap change nothing. The rendered rate is capped
at `target / multiplier` (the limiter above, verified exact at fixed caps), so presents stay at the
target. `target_hz = 0` uses the display refresh (`fg: display refresh 60 Hz` in the lab, from
`GetContainingOutput` + `EnumDisplaySettings`). Changes are logged as `fg: count auto 2x -> 3x (...)`.
In the 60 Hz lab, `auto` means 2x at 30 fps; test with `target_hz = 120` / `240`.

## Options (`[fg]`)

| key | default | |
|---|---|---|
| `enabled` | 0 | start-only: proxies, present thread, the input hook, the renderHUD slot |
| `count` | 2 | presents per rendered frame, 2..6, or `auto`; live |
| `count_min`, `count_max` | 2, 4 | `auto`'s range; live |
| `target_hz` | 0 | presents/s `auto` aims for; 0 = the display refresh (detected: `fg: display refresh`); live |
| `fps_limit` | 0 | rendered frames/s cap (0 = none; `auto` caps at `target_hz / multiplier` itself); live |
| `active` | 1 | live on/off (off: rendered frames only, through the same path) |
| `camera` | engine | `engine` / `identity`; live |
| `hudless` | 1 | pass the HUD-less copy as `DLSSG.HUDLess`; live |
| `pace` | smooth | `smooth` / `ready` (v1); live |
| `pace_relax` | 0.5 | `smooth`: how much of an early DLSS-G finish the first generated frame may use; live |
| `show` | all | `generated` / `real` / `hudless` (the HUD-less copy instead of real frames, generated frames off) for debugging and captures |
| `buffers` | 3 | real swapchain buffers while enabled |

Needs the game's DLSS on (any quality including DLAA). HDR (FP16 scRGB swapchain) is refused for now.
Menus, loading screens and the game's pre-rendered video cutscenes don't run DLSS, so they have no
inputs and are presented as rendered (`no input` in the perf line); generation resumes in-world.

Perf line (every 2 s): real + generated presents/s, multiplier, count (`auto`), the cap, the real
interval, when DLSS-G finished after the game's Present, how often and how long the game waited for
the present thread, failures, frames without inputs, generated frames dropped (not done in 100 ms)
and skipped (job superseded). A crash logger (`fg: exception ... at module+RVA`, stack scan) is
installed with `enabled = 1`.

## Open

- **Pacing A/B (`smooth` vs `ready`) and `count = auto` need a run on an idle GPU** (commands in 4
  and 5). Every measurement here shared the GPU with other lab instances; absolute fps should be
  retaken too.
- A headroom cap like Pharos's `auto_cap` (cap from how late DLSS-G finishes): uncapped, DLSS-G
  finishes 1.2–1.5 real intervals after the game's Present at 4x and the game waits for the present
  thread; `fps_limit` (or `count = auto`'s refresh cap) is the manual answer for now.
- Moving HUD elements (reticle, damage numbers) under HUD-less: not measured.
- HUD-less assumes the default in-game brightness (`copyWithGamma` identity); check another
  brightness, or take the HUD-less image after `end`'s blit.
- Reflex: the game has none; latency markers/sleep would have to be injected (Pharos did, through
  dxvk-nvapi). The limiter is a plain CPU sleep on the renderer thread.
- HDR: DLSS-G needs PQ-encoded input for an scRGB swapchain (Pharos converts); HUD-less in HDR would
  come from `pSeparateHudColorTarget` instead.
- Stereo + FG: not combined (stereo stays off with `[fg] enabled`). The HUD-less vtable patch chains
  into stereo's detour if both ever run.

## Resume notes (2026-11-22, session stopped for a laptop reboot)

**State.** Everything above is implemented in `src/features/fg/` and builds clean (`cmake --build
build-fg`; nothing committed). Measured on the laptop (runs r5/r6 of the old local `control-fg`):
1 (baseline), 2 (camera), 3 (HUD-less). **Not measured:** 4 (`pace = smooth` vs `ready`) and 5
(`count = auto`, never run live). Traces so far: `build-fg/fgtrace/` (local, gitignored; rows via
`python3 -I tools/fg/fg-table.py build-fg/fgtrace/<dirs>`). Last usable numbers: capped `ready` 4x
at 30 fps (s3-i4, r5): G→G 8.39 ± 0.10 ms, G→R 8.41 ± 0.11, R→G 8.05 ± 2.56, residual 3.28 / 6.49 px;
uncapped `ready` 4x (s1-base-4x): interval sd 2.67 ms, residual 9.45 / 18.97 px.

**Lab now runs on rayblaster** (CLAUDE.md). Instance `control-fg` exists there (1920x1080@165 for
the refresh target; created, `game-start` fixture installed, saves made writable); it is **down**
and has not run yet. Its `install/renderer.ini` already has the test settings (output 1920x1080,
render 1280x720 = DLSS Quality, `m_eDLSSQuality` 1, RT off, motion blur and film grain off,
`m_bSkipIntroVideos` true), copied from the laptop instance. `tools/fg/launch` (new: fixture,
menus, `skip`, wait for camera control) and `tools/fg/*` were synced to `rayblaster:threshold-lab/
tools/fg/`; the mod copy is `rayblaster:threshold-lab/mod-fg` (fg.enabled=1, defaults otherwise).

**Next, in order** (every trace under the GPU lock; note `quiver lab host` next to the numbers):

```sh
export QUIVER_SOURCE=agent:threshold-fg QUIVER_LAB_OWNER=claude:threshold-fg
cmake --build build-fg && tools/fg/moddir /tmp/mod-fg fg.enabled=1
rsync -a --delete /tmp/mod-fg/ rayblaster:threshold-lab/mod-fg/ && rsync -a tools/fg/ rayblaster:threshold-lab/tools/fg/
quiver --machine rayblaster lab up control-fg
tools/lab/rb tools/fg/launch control-fg /home/ajg/threshold-lab/mod-fg
# 4. pacing A/B, 2x and 4x, capped and uncapped (fg-series sets [fg] keys live, traces under quiver lab gpu)
tools/lab/rb tools/fg/fg-series control-fg p2 "r2|pace=ready count=2 fps_limit=0" "s2|pace=smooth" \
  "r4|pace=ready count=4" "s4|pace=smooth" "r4b|pace=ready" "s4b|pace=smooth" \
  "r4c|pace=ready fps_limit=40" "s4c|pace=smooth" "r2c|pace=ready count=2 fps_limit=60" "s2c|pace=smooth"
# 5. count=auto against the 165 Hz display (expect "fg: display refresh 165 Hz", then "fg: count auto ..." lines)
tools/lab/rb tools/fg/fg-series control-fg a1 "auto|count=auto fps_limit=0 pace=smooth" "auto240|target_hz=240"
tools/lab/rb sh -c 'grep -E "display refresh|count auto" $(ls -1td ~/Games/quiver-lab/instances/control-fg/lab/runs/* | head -1)/threshold.log'
rsync -a rayblaster:threshold-lab/build-fg/fgtrace/ build-fg/fgtrace/
python3 -I tools/fg/fg-table.py build-fg/fgtrace/p2-* build-fg/fgtrace/a1-*
quiver --machine rayblaster lab down control-fg
```

`tools/fg/fg-trace` runs on the machine that hosts the instance (it reads
`~/Games/quiver-lab/instances/<i>/lab/runs/` and uses `quiver lab exec` for the spin), hence
`tools/lab/rb`. Then: fill sections 4 and 5 above with the rows; decide whether the headroom cap
(Pharos `auto_cap`) is needed: if uncapped `smooth` still shows "game waited" in the perf line and DLSS-G
done > 0.7 of the interval, add it in `limit()`/`auto_step()` in `fg.cpp`. After that, HDR input (PQ
conversion for an scRGB swapchain, Pharos `src/fg/color.cpp`).
