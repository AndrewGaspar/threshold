# DLSS Frame Generation / Multi Frame Generation

> **Defaults:** `[fg] pace = smooth` (measured better than `ready` in every valid pair, section 4).
> `count = auto` (opt-in) and HDR frame generation are verified live on rayblaster (behaviour and HDR
> captures). Latency measured (section 8): FG adds ~3 ms (2x) / ~4 ms (4x) to the first present at
> ~200 rendered fps. The headroom cap (`auto_cap`, 7) and injected Reflex (`reflex`, 9) work but cost
> or gain nothing measurable here, so both default off. Stereo 3D with FG works per eye (section 10).
> Resume notes at the end.

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

### 7. Headroom cap (`[fg] auto_cap`, rayblaster r7, default **off**)

Pharos's `auto_cap`, folded into the once-a-second controller (`auto_step` in `fg.cpp`): if DLSS-G
finished later than `auto_cap_lag` (0.7) of a frame interval after the game's Present, or the game
waited for the present thread on more than 2 % of its frames, the cap drops to 96 % of the rate
reached; within `auto_cap_lag − 0.15` and no waits it rises 1 % (4 % below `− 0.35`), only while it
binds. The limiter applies min(headroom cap, `target_hz` / multiplier). `count = auto` steps up when
the headroom cap can't hold the target.

A/B live (`fg-series`, `target_hz = 1000` so only the headroom cap binds, `SETTLE=15`, each trace
under `quiver lab gpu`; host: `control-stereo` and `awr-bridge` (Torchbearer) on the GPU, 79–98 %
busy):

| pair | cap | real fps | game waited /2 s | DLSS-G done ms | interval sd ms | residual px |
|---|---|---|---|---|---|---|
| h2-m2 / c2 | off / on | 103.9 / 98.5 | 61 / 0 | 13.0 / 4.2 | 0.61 / 1.03 | 7.10 / 3.69 |
| h2-m2b / c2b | off / on | 150.2 / 112.7 | 0 / 0 | 6.1 / 4.8 | 0.64 / 0.86 | 3.33 / 6.74 |
| h2-m4 / c4 | off / on | 135.0 / 91.0 | 159 / 0 | 8.1 / 5.6 | 0.59 / 0.52 | 2.92 / 3.28 |
| h2-m4b / c4b | off / on | 127.4 / 95.1 | 159 / 1 | 8.9 / 5.6 | 0.64 / 0.70 | 3.56 / 2.37 |

It does what it is for (the waits go to 0, DLSS-G finishes in 0.5–0.7 frames) but costs 5–33 % of
the frame rate, and pacing and residual don't improve consistently. The latency (8) decides it:

### 8. Latency (rayblaster r7–r8, `tools/fg/latency`)

The mod logs per rendered frame the camera basis and CPU time at the game's DLSS evaluation, GPU
timestamps right after it, when the queue reached our Present-time work (the frame's own GPU work
done) and after DLSS-G, and our Present calls for that frame (`fg latency start|stop` → `latency.csv`,
`latency.cpp`). `tools/fg/latency` injects 40 mouse-look motions (`quiver lab mouse --repeat 40 --gap
0.4-0.7 --log`, CLOCK_MONOTONIC_RAW) and `latency.py` takes the first frame whose camera turned (by
> 0.05°) after each one. Stages are ms after the input (median, p10–p90); "first present" is our
first Present call carrying the frame (a generated frame interpolated towards it, with FG), "real
present" the real frame's. Scan-out and compositor are not included (headless). FG off = `active =
0` (the same proxy path, without generation). 40 of 40 reactions found in every run. Each run under
`quiver lab gpu`; DLSS Quality 1280x720 → 1920x1080, RT off.

| run | fps | eval recorded | frame done (GPU) | first present | real present | waits /2 s | host GPU |
|---|---|---|---|---|---|---|---|
| FG off (l1-offb) | 219.8 | 11.4 (9.2–16.6) | 12.5 (10.3–17.7) | **11.9** (9.6–17.1) | 11.9 | 0 | 41 %, control-stereo |
| 2x (l1-m2b) | 200.4 | 12.0 (10.0–15.0) | 13.2 (11.3–16.5) | **15.0** (12.7–17.8) | 17.8 (15.3–20.2) | 0 | 53 % |
| 2x + cap (l1-c2b) | 201.0 | 12.9 (10.0–16.8) | 14.1 (11.3–18.0) | 15.6 (12.8–19.4) | 18.1 (15.2–22.1) | 0 | 59 % |
| 4x (l1-m4b) | 197.4 | 12.9 (9.9–16.4) | 14.2 (11.5–17.5) | **16.2** (13.4–19.4) | 20.2 (17.2–23.3) | 13 | 63 % |
| 4x + cap (l1-c4b) | 170.5 | 14.8 (11.6–17.7) | 16.0 (12.9–18.9) | 17.9 (15.7–21.4) | 22.4 (20.0–25.8) | 0 | 61 % |
| 2x, contended (l1-m2) | 173.7 | 14.2 | 18.1 | 20.2 (17.1–23.3) | 23.5 | 9 | 93 %, + awr-bridge |
| 2x + cap, contended (l1-c2) | 152.9 | 16.3 | 18.8 | 22.1 (16.9–24.4) | 25.5 | 0 | 65 % |
| 4x, contended (l1-m4) | 160.1 | 16.1 | 20.0 | 22.9 (17.9–28.4) | 27.5 | 157 | 87 % |
| 4x + cap, contended (l1-c4) | 108.2 | 21.7 | 24.3 | 28.1 (24.4–32.2) | 35.0 (31.6–39.3) | 0 | 90 % |

- **Frame generation costs** about +3 ms (2x) and +4 ms (4x) to the first present at ~200 rendered
  fps, and +6 / +8 ms to the real frame (it is held (m−1)/m of an interval behind the generated ones).
- **The headroom cap costs latency here** in every pair (+0.3 to +5 ms to the first present): the
  waits it removes are short (1–1.6 ms) on a GPU with DLSS-G at 2–7 ms, while the lower frame rate
  lengthens the whole pipeline. Pharos's D3D11 game, with DLSS-G on a second device the game's
  queue knew nothing about, had longer queues to drain. So `auto_cap` stays available, **off by
  default**; `count = auto` still caps at the refresh target.

### 9. Reflex (`[fg] reflex`, rayblaster r8)

Control has no Reflex. `reflex.cpp` (after Pharos's `src/reflex/`) loads `nvapi64.dll` (dxvk-nvapi
v0.9.3 in Proton with `PROTON_ENABLE_NVAPI=1`) and calls `NvAPI_D3D_SetSleepMode` /
`NvAPI_D3D_Sleep` / `NvAPI_D3D_SetLatencyMarker` with the game's **ID3D12Device**: dxvk-nvapi maps
D3D12 to vkd3d-proton's `ID3DLowLatencyDevice`, which drives `VK_NV_low_latency2` (the device has it:
`low_latency2 true` in the Sight layer's line). Live: `SetSleepMode(low_latency=1) -> 0`,
`GetSleepStatus -> 0 (low_latency=1)`; the sleep runs once per rendered frame at the end of the game's
Present, after our cap (`perf: reflex sleep avg ...`), on a sleeper thread with a 500 ms watchdog (0
overruns). Markers: SIMULATION_START after the sleep, PRESENT_START / PRESENT_END around the game's
Present (one app frame per rendered frame; the generated presents come from our thread). No driver
minimum interval: Pharos found DXVK's tracker drops it with several presents per frame.

Same build and settings as 8, A/B live (`latency-series r1`, each under `quiver lab gpu`; host:
`control-stereo` only, 40–70 % busy except x2r0):

| run | Reflex | fps | first present | real present | eval recorded |
|---|---|---|---|---|---|
| r1-off0 / off1 | off / on | 211.5 / 204.2 | 12.6 / 12.3 | – | 12.1 / 11.5 |
| r1-off0b / off1b | off / on | 211.5 / 213.1 | 12.5 / 11.1 | – | 11.8 / 10.4 |
| r1-x2r0b / x2r1b | off / on | 207.4 / 199.7 | 14.4 / 15.0 | 16.9 / 17.6 | 11.5 / 12.1 |
| r1-x4r0 / x4r1 | off / on | 203.3 / 191.6 | 15.9 / 16.9 | 19.8 / 20.8 | 12.7 / 13.4 |
| r1-x4r0b / x4r1b | off / on | 193.8 / 192.3 | 16.3 / 15.8 | 20.3 / 19.8 | 12.9 / 12.4 |
| (r1-x2r0 / x2r1: the stereo instance at 97 % during x2r0, not comparable) | | 164.6 / 202.0 | 22.1 / 14.3 | 25.2 / 16.9 | |

The Reflex sleep averaged 0.1 ms per frame: at ~200 fps on the RTX 5090 the game is not GPU-queue
bound (the renderer thread is what limits it), so there is nothing for Reflex to drain, and the
differences (−1.4 to +1.0 ms) are within run-to-run noise. Heavier (r9: render 2880x1620 supersampled DLAA, RT
on; ~145–155 fps, GPU 83–97 % busy with `control-stereo`; reaction threshold raised to 0.21° by
`latency.py` because this camera swayed 0.07° per frame):

| run | Reflex | fps | first present | real present |
|---|---|---|---|---|
| r2-off0 / off1 | off / on | 155.1 / 150.4 | 16.7 (14.6–19.7) / 17.4 (14.2–23.5) | – |
| r2-x2r0 / x2r1 | off / on | 144.0 / 142.6 | 23.5 / 24.7 | 27.2 / 28.4 |
| r2-x2r0b / x2r1b | off / on | 144.1 / 139.8 | 25.2 / 24.2 | 28.6 / 27.8 |

Still no effect (sleep 0.3 ms per frame). The likely reason, besides the fast GPU: Control's own
`d3d::DeviceUtil::present` already spins until the GPU is at most one frame behind ("DX12:Wait for
present", default on; static, re/dlss.md 2.3), so there is no render queue for Reflex to drain. Reflex
therefore stays **off by default**; it is there for slower GPUs, where it is still to be measured.

### 10. Stereo (side-by-side 3D, `[stereo] mode = sbs`)

With stereo's packed output (`full`/`sbs`: a 2W swapchain) the frame FG gets is both eyes side by
side, rendered from one update (docs/stereo.md). FG then works per eye (docs/stereo.md "Stereo with
frame generation" has the measurements):

- `fg_stereo_link` (stereo, at start, only with a packed output): `eye()` tells the Eval tap whether
  the DLSS evaluation belongs to the left (0) or right (1) eye's render, or neither (−1). The tap
  keeps `Set::in[2]`, `depth[2]`, `mv[2]`, one per eye (camera read in that eye's render).
- `fg_stereo_frame` (stereo's `end`): the frame's layout. `frame_views` makes one `View` per eye:
  backbuffer/output/HUD-less sub-rectangle (the eye's half), depth/MV sub-rectangle, camera crop.
- `evaluate` runs per generated index and view, with feature `feat[v]` (one per eye, made at one
  eye's size: DLSS-G refuses a sub-rectangle narrower than its feature, `0xBAD00005`) and
  `DLSSG.EvalFlags = UpdateOnlyInsideExtents` so neither eye overwrites the other's half. Columns no
  view covers (an image-shift convergence) are copied from the real frame.
- HUD-less: `fg_stereo_hudless` (stereo's renderHUD, both eyes composed, before the HUD); FG's own
  renderHUD tap defers to it (`hudless_defer`). The HUD-less texture cache holds two sizes (the
  eye-wide colour target and the 2W side-by-side texture) so pair and mono frames don't reallocate.
- `GetBuffer`: the proxies and FG's size come from the real buffer (2W); the swapchain's `GetDesc`
  reports the game's own width under stereo's width scale.
- `fg status`: `feature_right`, `stereo_link`, `frames_per_eye`, `size`; the perf line adds `per eye
  N frames`.
- Dev `[fg] stereo_features = 1`: one feature (the left eye's) for both eyes, the negative control
  (wrong by design: the right eye interpolates from the left eye's previous image).
- `fg grab N TAG` / `fg grab files`: the real and generated frames of the next N jobs, whole, as
  THRW `.raw` in the run folder (`tools/fg/stereo-grab`, `tools/fg/stereo-gen.py`).

## Options (`[fg]`)

| key | default | |
|---|---|---|
| `enabled` | 0 | start-only: proxies, present thread, the input hook, the renderHUD slot |
| `count` | 2 | presents per rendered frame, 2..6, or `auto`; live |
| `count_min`, `count_max` | 2, 4 | `auto`'s range; live |
| `target_hz` | 0 | presents/s `auto` aims for; 0 = the display refresh (detected: `fg: display refresh`); live |
| `fps_limit` | 0 | rendered frames/s cap (0 = none; `auto` caps at `target_hz / multiplier` itself); live |
| `auto_cap`, `auto_cap_lag` | 0, 0.7 | headroom cap (7), with the refresh cap; live |
| `reflex`, `reflex_markers` | off, 1 | injected Reflex: `off` / `on` / `boost` (9); live |
| `active` | 1 | live on/off (off: rendered frames only, through the same path) |
| `camera` | engine | `engine` / `identity`; live |
| `hudless` | 1 | pass the HUD-less copy as `DLSSG.HUDLess`; live |
| `pace` | smooth | `smooth` / `ready` (v1); live |
| `pace_relax` | 0.5 | `smooth`: how much of an early DLSS-G finish the first generated frame may use; live |
| `show` | all | `generated` / `real` / `hudless` (the HUD-less copy instead of real frames, generated frames off) for debugging and captures |
| `buffers` | 3 | real swapchain buffers while enabled |
| dev: `stereo_features` | 2 | with stereo: `1` = one DLSS-G feature for both eyes (the negative control of section 10; wrong by design); live |

Needs the game's DLSS on (any quality including DLAA). HDR (FP16 scRGB swapchain) works through the PQ conversion (6).
Menus, loading screens and the game's pre-rendered video cutscenes don't run DLSS, so they have no
inputs and are presented as rendered (`no input` in the perf line); generation resumes in-world.

Perf line (every 2 s): real + generated presents/s, multiplier, count (`auto`), the cap, the real
interval, when DLSS-G finished after the game's Present, how often and how long the game waited for
the present thread, failures, frames without inputs, generated frames dropped (not done in 100 ms)
and skipped (job superseded). A crash logger (`fg: exception ... at module+RVA`, stack scan) is
installed with `enabled = 1`.

## Open

- Every measurement shared rayblaster's GPU with other lab instances (noted per row); absolute fps
  move with their load.
- Latency on a slower GPU (where the game is GPU-bound and Reflex / the headroom cap could matter);
  the laptop's RTX 5070 is the obvious place once the lab can run one instance there again.
- Scan-out: the latency stops at our Present call; Quiver's Sight layer (`VK_NV_low_latency2` / present
  timing) could extend it to the display.
- Moving HUD elements (reticle, damage numbers) under HUD-less: not measured.
- HUD-less assumes the default in-game brightness (`copyWithGamma` identity); check another
  brightness, or take the HUD-less image after `end`'s blit.
- Reflex markers: one app frame per rendered frame while our thread makes 2–6 presents; whether
  vkd3d-proton's tracker copes as DXVK's didn't (Pharos) is not checked (latency unchanged either
  way).
- HDR: the trace can't decode FP16 bands; HUD-less in HDR assumes `copyWithGamma` is the identity for
  scRGB as well (measured equal outside the HUD, 6).
- Stereo + FG (section 10; measurements in docs/stereo.md "Stereo with frame generation", 2x/4x, SDR
  and HDR, hsbs smoke-tested, 20 level changes passed): not tested with RT on or for latency.

## Resume notes: stereo (2026-11-23, branch `wip/stereo-fg`)

Section 10 is implemented and measured (docs/stereo.md has the numbers and its own resume notes).
FG-only behaviour is unchanged: without a packed stereo output stereo doesn't link and FG evaluates
the whole frame with one feature (live r7). `GetBuffer` now takes FG's size from the real buffer
(identical without stereo's width scale).

## Resume notes (2026-11-23, branch `wip/fg-latency-reflex`)

Done: 7 (headroom cap, default off), 8 (latency), 9 (Reflex injection, default off; works, no
measured effect at these frame rates). Instance `control-fg` on rayblaster is down, back on the
standard scene settings (SDR 1920x1080@165, render 1280x720, RT off). Commands as before (`tools/fg/launch`, `tools/lab/rb
tools/fg/fg-series`, `tools/lab/rb tools/fg/latency-series`, `tools/fg/latency-table.py`).
