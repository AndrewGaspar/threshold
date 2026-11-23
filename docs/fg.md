# DLSS Frame Generation / Multi Frame Generation

> **Defaults:** `[fg] pace = smooth` (measured better than `ready` in every valid pair, section 4).
> `count = auto` (opt-in) and HDR frame generation are verified live on rayblaster (behaviour and HDR
> captures). Still open: Pharos's headroom cap for uncapped high multipliers; latency. Resume notes at the end.

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

### 4. Pacing (`pace = smooth`, rayblaster r6)

`pace = smooth` (Pharos's): the first generated frame is held until one step after the previous real
frame's Present *call*, minus `pace_relax` (0.5) of how early DLSS-G finished relative to that; every
later present one step after the previous Present's call (not its return). The interval average
excludes the renderer thread's waits for the present thread and caps hitches at 2.5x.

A/B in one session (`control-fg` on rayblaster, RTX 5090, SDR 1920x1080@165, DLSS Quality, RT off;
`PRESENTS=960 SHOTS=360 tools/fg/fg-series ...`), each trace under `quiver lab gpu` (other lab
instances throttled). No lab limits in force (user not playing). Host during the valid rows:
`aw2-crash` (Alan Wake 2, another session) and `control-stereo` were GPU users and the GPU read
91–98 % busy, except s4cb / r2c / s2c (15–21 %, `aw2-crash` idle or gone). Interval sd: all
present-call intervals (G→G / G→R / R→G); residual: |position residual| mean / p90, VRR model.

| pair | config | real fps | game waited /2 s | interval sd ms, ready → smooth | residual px, ready → smooth | shift ratio |
|---|---|---|---|---|---|---|
| r2 / s2 | 2x uncapped | 146 / 145 | 0 / 0 | 1.34 (R→G 1.85) → **0.87** (R→G 1.20) | 5.20 / 10.2 → **3.92 / 7.45** | 0.50 both, 0 / 1 of 358 outside |
| r4 / s4 | 4x uncapped | 116 / 118 | 100 / 126 | 1.47 (G→G 1.20) → **0.76** (G→G 0.32) | 4.32 / 9.33 → **3.22 / 6.81** | k/4 ± 0.003 both, 0 outside |
| r4b / s4b | 4x uncapped | 85 / 86 | 122 / 153 | 2.10 (G→G 1.98) → **1.21** (0.92) | 7.55 / 14.9 → **4.67 / 9.53** | same |
| r4c / s4c | 4x, fps_limit 40 | 40.0 / 40.0 | 0 / 0 | 1.13 (R→G 2.25) → **0.78** (1.54) | 3.33 / 7.79 → **2.52 / 5.65** | same |
| r4cb / s4cb | 4x, fps_limit 40 | 39.5 / 40.0 | 0 / 0 | 1.15 (R→G 2.29) → **0.52** (1.01) | 3.29 / 7.24 → **1.80 / 3.59** | same |
| r2c / s2c | 2x, fps_limit 80 | 80.0 / 79.5 | 0 / 0 | 1.06 (R→G 1.49) → **0.77** (1.08) | 4.41 / 8.84 → **2.56 / 5.13** | same |

Not used: r2b / s2b and r2cb / s2cb (the last four traces), taken after a host game and
`awr-bridge` started: VRAM 32.1 of 32.6 GB, real fps 39–62 and swinging, 17–105 waits.

Result: **`smooth` is better in all six valid pairs**: present-interval sd 30–55 % lower, position
residual 25–45 % lower, interpolation unchanged (shift ratios identical). The remaining spread is the
first generated frame after a real one (R→G), as in Pharos (their sd 0.28–0.46 ms; ours 0.52–1.21).
Uncapped at 4x the game waits for the present thread 100–150 times per 2 s with either pacing
(DLSS-G done 9–15 ms after the game's Present against an 8.5–12 ms interval), slightly more with
`smooth` (it holds frames): that is the case for Pharos's headroom cap (`auto_cap`), still open; a
`fps_limit` (or `count = auto`'s refresh cap) removes the waits entirely (0 in every capped row).
Latency was not measured.

### 5. `count = auto` (live, rayblaster r2–r4, behaviour only)

Pharos's controller without Reflex: once a second, from the rendered frames' own work time (arrival
interval minus the limiter's sleep and the waits for the present thread), within `[count_min,
count_max]` (2..4): one step up when the game can't hold `target / multiplier` (10 % short, 2 s); one
step down when it could hold `target / (multiplier − 1)` with 15 % to spare **and** DLSS-G finished
within 0.55 of that shorter interval (3 s). The achievable rate is `min(work rate, fps_limit)`, so a
manual cap raises the multiplier toward the target. A step down undone within 30 s blocks steps down
for 10 s, doubling to 60 s; windows below 60 % of the cap change nothing. The rendered rate is capped
at `target / multiplier` (the limiter, exact at fixed caps), so presents stay at the target.
`target_hz = 0` uses the display refresh (`GetContainingOutput` + `EnumDisplaySettings`). Changes are
logged as `fg: count auto 2x -> 3x (work, real fps, DLSS-G done, target, cap)`.

Live on rayblaster (`control-fg`, gamescope 1920x1080@165, DLSS Quality, RT off), **under lab limits**
(the user was playing: GPU ≤ 20 %, 6 cores), so these show behaviour, not timing:

- `fg: display refresh 165 Hz` detected; `count = auto` started at 2x with cap 82.5 → 165 presents/s
  (r2). When the throttled GPU let the game fall to ~53 fps it stepped to 3x (cap 55), and back to
  2x once it held 82.5 again (r2, 6 s apart: that bounce is why the undo window is now 30 s).
- `target_hz = 240`: 2x → 3x (cap 80, 240.1 presents/s) when 2x couldn't hold 120 (r2).
- `fps_limit = 40` (r3): 2x → 3x → 4x within 4 s, then 160 presents/s at 40.0 real fps, no waits;
  `count_max = 3` then holds 3x (120 presents/s).
- Oscillation (r4, `target_hz = 240`, final controller): one 2x→3x→2x→3x sequence in the first 30 s
  of play, then 3x for the rest of the 2 min (40 of 60 perf lines at 3x).

Why it can bounce: the renderer thread's work time only sees the GPU when frames back up, so a game
capped with headroom (3x at 80) looks able to do 200+ fps; DLSS-G's lag (2–4 ms at the cap vs 15–20 ms
when GPU-bound) is the better signal and is now required for a step down. A GPU-time measurement
(timestamp queries around the game's frame) would be better still.

### 6. HDR (live, rayblaster r5, `display.hdr=on`, renderer.ini `m_eAllowHDR 1`)

The game's HDR swapchain is FP16 scRGB (DXGI 10). As in Pharos, DLSS-G gets HDR10: the real frame
and the HUD-less colour are encoded to PQ BT.2020 R10G10B10A2 (`BackbufferFormat` 24,
`ColorBuffersHDR = 1`), and each generated frame is decoded back to scRGB for presenting (`hdr.cpp`:
two compute shaders from Pharos's `color.cpp`, compiled at start with the game's
`d3dcompiler_47.dll`, on our own list). In HDR the colour target at renderHUD entry is FP16 too (pixel
format 23, DXGI 10), so HUD-less works unchanged (it is encoded like the real frame).

`quiver lab shot --hdr` (scRGB raw), still camera, 2x, `show` = real / generated / hudless / real:

| comparison | mean abs luminance difference (outside the hint box) | p99 | hint box |
|---|---|---|---|
| real vs real (two captures) | 0.17 nits | 3.0 | 0.02 |
| generated vs real | 0.20–0.24 nits | 3.6–4.4 | 0.09 |
| HUD-less vs real | 0.23 nits | 4.7 | 24.5 (HUD gone: 3.2 vs 24.7 nits mean) |

Frame stats: max 249–266 nits, average 12, p99 142–144 for all four; no NaN or negative values. The
PQ round trip is right (generated ≈ real to within capture noise), and the HUD survives in generated
frames. The present trace's bands assume 4-byte pixels: in HDR they are not decodable yet.

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

Needs the game's DLSS on (any quality including DLAA). HDR (FP16 scRGB swapchain) works through the PQ conversion (6).
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
- HDR: the trace can't decode FP16 bands; HUD-less in HDR assumes `copyWithGamma` is the identity for
  scRGB as well (measured equal outside the HUD, 6).
- Stereo + FG: not combined (stereo stays off with `[fg] enabled`). The HUD-less vtable patch chains
  into stereo's detour if both ever run.

## Resume notes (2026-11-23, branch `wip/fg-pacing`)

**State.** All measured: 1–3 on the laptop; 4 (pacing A/B: `smooth` better in every valid pair), 5
(`count = auto` behaviour) and 6 (HDR) on rayblaster. Next: Pharos's headroom cap for uncapped
play (4: the game waits for the present thread at 4x uncapped), and latency (`smooth` vs `ready`).
The commands below retake 4.

Instance `control-fg` on rayblaster: 1920x1080@165, currently **HDR** (`display.hdr=on`,
renderer.ini `m_eAllowHDR 1`; otherwise DLSS Quality 1280x720, RT off, motion blur / film grain off,
intro skip). The trace bands need SDR: switch back first.

```sh
cd ~/code/threshold-wt/fg && scripts/build
export QUIVER_SOURCE=agent:threshold-fg QUIVER_LAB_OWNER=claude:threshold-fg Q="quiver --machine rayblaster"
$Q lab edit control-fg display.hdr=off && $Q lab down control-fg && $Q lab up control-fg
ssh rayblaster "python3 -c \"import json;p='/home/ajg/Games/quiver-lab/instances/control-fg/install/renderer.ini';d=json.load(open(p));d['m_eAllowHDR']=0;json.dump(d,open(p,'w'),indent=1)\""
$Q lab status control-fg | grep -i throttl          # must print nothing
tools/fg/moddir /tmp/mod-fg fg.enabled=1
tools/fg/launch control-fg /tmp/mod-fg              # Q set: drives rayblaster, uploads the mod
rsync -a tools/fg/ rayblaster:threshold-lab/tools/fg/
# fg-trace/fg-series run where the instance is (they read its run folder and spin via `quiver lab exec`):
tools/lab/rb tools/fg/fg-series control-fg p2 "r2|pace=ready count=2 fps_limit=0" "s2|pace=smooth" \
  "r4|pace=ready count=4" "s4|pace=smooth" "r4b|pace=ready" "s4b|pace=smooth" \
  "r4c|pace=ready fps_limit=40" "s4c|pace=smooth" "r2c|pace=ready count=2 fps_limit=80" "s2c|pace=smooth"
rsync -a rayblaster:threshold-lab/build-fg/fgtrace/ build-fg/fgtrace/
python3 -I tools/fg/fg-table.py build-fg/fgtrace/p2-*
$Q lab down control-fg
```

Then fill section 4. If uncapped `smooth` still shows "game waited" in the perf lines with DLSS-G done
> 0.7 of the interval, add Pharos's headroom cap (`auto_cap`) in `limit()` / `auto_step()` in
`fg.cpp`.
