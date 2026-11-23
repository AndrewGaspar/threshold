# Control (D3D12, GOG Ultimate): DLSS / NGX / swapchain / present

Static reverse engineering of the copies in `re/bin/` (hashes in `re/bin.sha256`). All addresses
are **RVAs** in the named DLL (Ghidra image base 0x180000000). "static: X" means read off the
decompilation or disassembly of function X (Ghidra projects copied to `re/ghidra-dlss-*`, queried
with `re/scripts/Serve.java`). "export" means the demangled export lists in `re/exports/*.dem`.
"string" means a defined string and its xrefs. Nothing here was checked live. Camera and stereo
work is covered elsewhere; this file only touches the camera where frame generation needs it.

Modules: `d3d` = `d3d_rmdwin10_f.dll`, `renderer` = `renderer_rmdwin10_f.dll`, `coregame` =
`coregame_rmdwin10_f.dll`.

---

## 1. DLSS Super Resolution

### 1.1 Globals and the DLSSData object (d3d)

| What | Where | How established |
|---|---|---|
| `DLSSData*` (anon-namespace class, RTTI `_anon_C8AC1655::DLSSData`) | d3d `0x111be0` | static: `initDLSS` 0x1e7f0 allocates it with `FUN_18001ecc0` (0xb8 bytes) |
| `NativeDevice*` (engine device object) | d3d `0x136d28` | static: `DeviceUtil::create` 0x2dc50, `present` 0x31b50 |
| `ID3D12Device*` | d3d `0x111c08` | static: `FUN_18002a6e0` stores device+0x78 there after D3D12CreateDevice |
| `ID3D12Resource*` inside a `d3d::NativeTexture` | `NativeTexture+0x88` | static: `doAntiAliasing` reads `tex+0x88` for every NGX resource; `prepareForConsuming` 0x3a520 tests it |

DLSSData layout (static: `initDLSS` 0x1e7f0, `resetDLSSFeature` 0x1f3f0, `doAntiAliasing` 0x1f990, `FUN_18001e1d0`):

| Offset | Meaning |
|---|---|
| +0x08 u8 | SR available (capability param `"#\x01"` = `NVSDK_NGX_EParameter_SuperSampling_Available` > 0) |
| +0x09 u8 | RR supported (`GetFeatureRequirements(RayReconstruction)` returned success and `FeatureSupported == 0`) |
| +0x0c / +0x10 u32 | current render width / height |
| +0x14 / +0x18 u32 | current output width / height |
| +0x1c u8 | AutoExposure requested (always false, see 1.4) |
| +0x1d u8 | HDR input flag |
| +0x1e u8 | Ray Reconstruction active |
| +0x1f u8 | "legacy model" flag (`m_bDLSSLegacyModel`) |
| +0x20 f32 | sharpness from OptimalSettings (default 0.5) |
| +0x28 | current `NVSDK_NGX_Handle*` |
| +0x30 | current per-feature `NVSDK_NGX_Parameter*` |
| +0x38 | capability parameters (`GetCapabilityParameters`) |
| +0x40..+0xa0 | feature cache: hash map CityHash64(key) -> {handle, params}; entries 0x18 bytes at +0xa0 |
| +0xa8 | map of "good render resolutions" per output resolution (`populateListsOfGoodRenderResolutions`, `getRenderResolutionForOutputResolution` 0x1f340) |

### 1.2 Loading NGX (`d3d::DLSS::initDLSS`, d3d 0x1e7f0, export)

- Caller: renderer `FUN_180116070` (RendererX86 init) at renderer 0x116760, right after
  `d3d::DeviceUtil::create` (static, xref of the import pointer renderer 0x5df700).
- The NGX SDK is linked statically into d3d. Init is `FUN_180052cc0` = the SDK's
  `NVSDK_NGX_D3D12_Init_ProjectID` (static):
  `Init_ProjectID("305914b8-cf5b-4535-8e53-5589bf8cefa5", NVSDK_NGX_ENGINE_TYPE_CUSTOM (0), "1.0", L".", ID3D12Device*, FeatureCommonInfo = NULL, SDK version 0x15)`.
  0x15 is `NVSDK_NGX_VERSION_API_MACRO` in the 310.9.16 headers (`third_party/DLSS-310.9.16/include/nvsdk_ngx_defs.h:56`).
  App data path `L"."` (the SDK writes `nvsdk_ngx.log` there, string `/nvsdk_ngx.log`, static `FUN_180052110`).
- NGX core loader: `FUN_180052110` -> `FUN_180054d10` (static, strings). Order: `_nvngx.dll` / `nvngx.dll` next to the exe;
  then the driver store via D3DKMT QueryAdapterInfo (`FUN_1800549c0`, string "Path to driverStore found using QAI");
  then registry `HKLM\System\CurrentControlSet\Services\nvlddmkm\NGXCore\NGXPath` and
  `HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore\FullPath`. The loaded core is checked with
  WinVerifyTrust / CertGetNameString (`FUN_180055f00`, strings), so a fake `_nvngx.dll` next to the exe is rejected.
  The core's exports are cached as function pointers (static `FUN_180052110`):

  | d3d RVA | pointer |
  |---|---|
  | 0x101160 | HMODULE of the core |
  | 0x1011a0 / 0x1011a8 / 0x1011b0 | D3D12_Init / Init_Ext / Init_ProjectID |
  | 0x1011b8 / 0x1011c0 | Shutdown / Shutdown1 |
  | 0x1011c8 / 0x1011d0 | GetScratchBufferSize / GetParameters |
  | 0x1011d8 | **CreateFeature** |
  | 0x1011e0 | **EvaluateFeature** |
  | 0x1011e8 | ReleaseFeature |
  | 0x1011f0 / 0x1011f8 / 0x101200 / 0x101208 | GetFeatureRequirements / AllocateParameters / GetCapabilityParameters / DestroyParameters |

- **Which nvngx_dlss.dll**: `FeatureCommonInfo` is NULL, so no extra search paths. The driver's
  core loads `nvngx_dlss.dll` from the exe directory (the install root, which ships 310.2.1:
  FileVersion resource `310,2,1,0`) or its own OTA/override copy. The game code has no version check
  on the snippet: no GetFileVersionInfo or version.dll import, and no version strings near the DLSS
  code (static, strings). A newer DLL in the install root is picked up by the normal NGX rules.
- After init: `GetCapabilityParameters` -> DLSSData+0x38, `GetI("#\x01")` -> +0x08 (static
  disasm 0x1e8e1: vtable slot 0x58 = GetI in the MSVC order documented in `ac-dlss/src/dlss/ngx.cpp`).
  Then `GetFeatureRequirements(adapter = NativeDevice+0x180, {sdk 0x15, feature 13 = RayReconstruction, project id})`
  -> +0x09.
- Two detached threads call `NVSDK_NGX_UpdateFeature(appId, 1)` (SuperSampling, `FUN_180020b00`)
  and `NVSDK_NGX_UpdateFeature(appId, 13)` (RR, only if +0x09, `FUN_180020a60`). These are NGX OTA update requests (static).
- `isSupported` 0x1ef00 returns +0x08; `isRRSupported` 0x1ef20 returns +0x09 (exports, static).

### 1.3 Choosing the feature (`d3d::DLSS::resetDLSSFeature`, d3d 0x1f3f0, export)

Signature (export, static): `bool resetDLSSFeature(uint outW, uint outH, uint renderW, uint renderH, bool autoExposure, bool hdr, bool rr, bool legacyModel, bool& reset)`.

- Returns false when SR is unavailable. `rr` is ANDed with +0x09.
- If anything differs from the cached state (sizes, the four flags, or no current handle), it sets
  `reset = true`. It then loops `PerfQualityValue` 0..5 (MaxPerf, Balanced, MaxQuality, UltraPerformance,
  UltraQuality, DLAA) and calls `DLSSOptimalSettingsCallback` (or `DLSSDOptimalSettingsCallback` for RR)
  with `Width/Height = output`, `RTXValue = 0`. It reads `OutWidth/OutHeight`,
  `DLSS.Get.Dynamic.{Max,Min}.Render.{Width,Height}` and `Sharpness`. The chosen mode is the one whose optimal
  render size equals the requested render size. Failing that, it takes the mode whose dynamic range contains
  the requested size and is closest to it (Manhattan distance). If render >= output, the mode is forced to DLAA (5).
  **So the engine stores a render resolution, not a quality mode.** The mode is derived from it.
- Then `FUN_18001e1d0` (d3d 0x1e1d0) gets or creates the feature and stores handle/params at +0x28/+0x30.
- Caller: renderer `FUN_18011b4c0` (RendererX86 vtable slot 14, the per-frame begin; import call at renderer 0x11b697), every frame (static):
  `resetDLSSFeature(r+0x710, r+0x714, r+0x718, r+0x71c, false, early||hdrDisplay, rrTweak, *(u8*)(r+0x749), &DAT_18128d1e6)`
  (r = RendererX86). If the "same-res" flag `DAT_18128d1e4` is set, render size = output size.
  On failure it sets `DLSSEnabled = false` and clears the RR and early flags.

### 1.4 Creating the feature (`FUN_18001e1d0`, d3d 0x1e1d0, internal)

Static, d3d 0x1e1d0. Args: `(rr, out pair*, outW, outH, renderW, renderH, quality, autoExposure, hdr, rr, legacy)`.

- Cache key = CityHash64 of {outW, outH, renderW, renderH, 4 flag bytes} (0x14 bytes). A cached
  {handle, params} is reused. **Changing presets therefore needs `d3d::DLSS::clearFeatures` (0x1f860, export) first.**
- `NVSDK_NGX_D3D12_AllocateParameters` makes a fresh parameter block per feature.
- **Preset hints, which the game sets itself** (static disasm 0x1e384..0x1e3ba):
  ```
  preset = (quality == DLAA || (outW <= renderW && outH <= renderH) || quality == UltraPerformance) ? 6 (F) : 5 (E)
  if (!legacyModel) preset = 0xb (K)          ; d3d 0x1e3ae: b8 0b 00 00 00   mov eax,0xb ; cmovz r15d,eax
  ```
  The same value goes into all six `DLSS.Hint.Render.Preset.{DLAA,UltraQuality,Quality,Balanced,Performance,UltraPerformance}`
  hints, or into `RayReconstruction.Hint.Render.Preset.*` for RR (strings at d3d 0x65360..0x65550).
  With the default `m_bDLSSLegacyModel = 0`, the game already requests **preset K** (DLSS 4 transformer) in every mode.
  `m_bDLSSLegacyModel = 1` selects the CNN presets E/F. For RR, 0xb is "do not use, reverts to default"
  (`nvsdk_ngx_defs_dlssd.h:49`), so legacy=0 with RR means the RR default preset.
- Create params (`NVSDK_NGX_DLSS_Create_Params`, static): `InWidth/InHeight = render`,
  `InTargetWidth/Height = output`, `InPerfQualityValue = quality`,
  **`InFeatureCreateFlags = MVLowRes (2) | AutoExposure (0x40 if autoExposure, never set by the caller) | IsHDR (1 if hdr or rr)`**,
  `InEnableOutputSubrects = 0`. **Not set: DepthInverted, MVJittered, DoSharpening.**
- SR path: `FUN_18001ccf0` (d3d 0x1ccf0, the SDK's `NGX_D3D12_CREATE_DLSS_EXT`) sets `CreationNodeMask = VisibilityNodeMask = 1`,
  Width/Height/OutWidth/OutHeight/PerfQualityValue, `DLSS.Feature.Create.Flags` and `DLSS.Enable.Output.Subrects`, then
  `NVSDK_NGX_D3D12_CreateFeature(cmdList, 1 = SuperSampling, params, &handle)`.
  The command list is the calling thread's `DeviceState` list (TLS+8 -> +0x18), falling back to d3d `0x111c18`, under the SRW lock d3d `0x111c20`.
- RR path: `FUN_18001d320` (d3d 0x1d320, `NGX_D3D12_CREATE_DLSSD_EXT`): the same, plus `DLSS.Denoise.Mode = 1`,
  `DLSS.Roughness.Mode = 1` (packed in the normals), `DLSS.Use.HW.Depth = 1`, then `CreateFeature(13)`.
- On failure it retries once with `DLSS.Hint.Render.Preset.Balanced = 0` and quality = Balanced (static 0x1e562..0x1e5c4).
- Afterwards it calls `d3d::DeviceUtilDX12::reapplyDeviceState` (NGX clobbers bound state).

### 1.5 Evaluating (`d3d::DLSS::doAntiAliasing`, d3d 0x1f990, export)

Signature (export, static):
```
bool doAntiAliasing(NativeTexture* output, NativeTexture* color, NativeTexture* mv, NativeTexture* depth,
                    NativeTexture* exposure /*opt*/, NativeTexture* hitDist /*opt*/,
                    NativeTexture* rrNormals, NativeTexture* rrDiffuseAlbedo, NativeTexture* rrSpecularAlbedo,
                    bool reset, double jitterX, double jitterY, float frameTime, float preExposure, float sharpness)
```
It builds an `NVSDK_NGX_D3D12_DLSS_Eval_Params` (memset 0x168, layout of `nvsdk_ngx_helpers_d3d.h:225`) and calls
`FUN_18001cdf0` (d3d 0x1cdf0, `NGX_D3D12_EVALUATE_DLSS_EXT`) -> `NVSDK_NGX_D3D12_EvaluateFeature_C(cmdList, DLSSData+0x28, DLSSData+0x30, NULL)`.
For RR it calls `FUN_18001d460` (0x1d460, DLSSD evaluate) instead.

| Eval field | Value | Notes |
|---|---|---|
| Color | `color+0x88` | `prepareForConsuming` |
| Output | `output+0x88` | `prepareAsUAV` |
| Depth | `depth+0x88` | render res, hardware depth (`gbufferdepth`) |
| MotionVectors | `mv+0x88` | render res |
| Jitter X / Y (px) | `renderW * jitterX * 0.5`, `renderH * jitterY * -0.5` | the engine jitter is in NDC |
| RenderSubrectDimensions | DLSSData+0x0c/+0x10 | |
| Reset | `reset` | |
| MVScale X / Y | `(float)renderW`, `(float)renderH` | **MVs are in UV units** |
| ExposureTexture | `exposure+0x88` or NULL | |
| PreExposure | `preExposure` | |
| Sharpness | `sharpness`, or DLSSData+0x20 if < 0 | both callers pass -1.0 |
| TonemapperType | 1 | |
| FrameTimeDeltaInMsec | `frameTime / 1000` | callers pass renderer global 0x8029f4 |
| RayTracingHitDistance (SR) / DLSSD.SpecularHitDistance (RR) | `hitDist` | callers pass NULL |
| RR: DiffuseAlbedo, SpecularAlbedo, GBuffer.Normals | args 8, 9, 7 | callers pass NULL |

Afterwards it calls `reapplyDeviceState`. The result is `(r & 0xfff00000) != 0xbad00000`.

**Callers** (renderer, the only two xrefs to the import pointer 0x5df898, static):

1. **Early path, `FUN_18012ff20`** (renderer 0x12ff20, render-graph pass "TAA and Upscale Depth" / `TAA_Upscale_Depth`). Used when
   global renderer `0x802922` ("DLSS before post") is set. Input is the HDR linear scene colour right after
   transparents, *before* post-processing.
   `doAntiAliasing(colorbufferOUT ("colorBufferUpscaled", output res), colorbufferIN, VectorBlur->m_pVelocityGeometryBuffer, gbufferdepthIN, *(*(r+0x160)+0x20) exposure, NULL x4, DAT_18128d1e6, r+0x480, r+0x488, frameTime, 1/DAT_180a88ab0, -1)`.
   Right after this it runs `ssaa.rfx` / `upscaleAndDejitterDepth`, which writes **output-resolution, de-jittered
   `gbufferdepthUpscaled` (gbufferdepthOUT) and `lineardepthUpscaled`** for post-processing. It also sets
   `g_vSSAAJitterOffsetCurrent`.
2. **Late path, `FUN_18013f3e0`** (renderer 0x13f3e0, pass "DLSS anti aliasing" / `dlss_aa_done`). Used when the early flag is clear.
   It runs after "final image pass" (`color_finalimage`, tone-mapped) and in place of TAA.
   `doAntiAliasing(colorbufferOUT ("color_dlss_aa"), colorbufferIN, VectorBlur mv, gbufferdepth, *(ctx+0x28), NULL x4, reset, jitter, frameTime, 1.0, -1)`.

When each path is used (static, renderer `FUN_1801374b0` = RendererX86 vtable slot 40, apply settings, 0x137e86..0x137ee4):
`early = RR || (DLSS && (upscaling || PostProcessQuality == 2 || HDR output))`. When PostProcessQuality == 1, only RR or upscaling selects early.
`DAT_18128d1e4` = early && upscaling. The HDR flag passed to resetDLSSFeature is `early || HDR display`.
So DLSS with upscaling always uses the early path (linear HDR input, IsHDR=1). The late path only covers
native-resolution SDR (DLAA-like, tone-mapped input, IsHDR=0).

### 1.6 Ray Reconstruction: plumbed, not finished

- RR feature create exists (`FUN_18001d320`, `CreateFeature(13)`), and so does RR evaluate (`FUN_18001d460`, which sets DLSSD inputs
  and `WorldToViewMatrix` / `ViewToClipMatrix`). RR is enabled through `m_eDLSSQuality == 2` (renderer settings +0x58, see 5) or the
  tweakable `"dlss:Enable DLSS RR (requires SR)"` (renderer flag `0x914620`, default 0, `FUN_180006640`).
  The value 2 is honoured only when ray tracing is supported and at least one RT effect is on (static `FUN_1801374b0` 0x137df6).
- **Both `doAntiAliasing` callers pass NULL for normals and the two albedos.** With RR active, `doAntiAliasing` calls
  `prepareForConsuming(NULL)` (d3d 0x3a520 dereferences `[rcx+0x88]`), which is an access violation (static).
  The install ships no `nvngx_dlssd.dll`, so in practice `CreateFeature(13)` fails, `resetDLSSFeature` returns false,
  and the renderer clears the RR flag (static `FUN_18011b4c0` 0x11b6a7..0x11b6ad). **Do not enable RR without supplying those inputs.**
  The world/view matrices in the RR eval block stay zero (memset; nothing writes eval index 0x45/0x46).

### 1.7 Where to set presets M / L / K

NGX parameter names (`nvsdk_ngx_defs.h:848-853`), type UI:
`DLSS.Hint.Render.Preset.{DLAA,Quality,Balanced,Performance,UltraPerformance,UltraQuality}`.
Values: K = 11 (0xb), L = 12, M = 13, 0 = DLL default (`nvsdk_ngx_defs.h:70-88`). In the 310.9.16 header the defaults are
K for DLAA/Quality/Balanced, M for Performance and L for Ultra Performance.
**The shipped 310.2.1 DLL has no L/M**, so the mod must place a newer `nvngx_dlss.dll` (for example from `third_party/DLSS-310.9.16`) in the install root.
Hints are only read at CreateFeature.

Options, in order of preference:
1. **Hook the CreateFeature path** and overwrite the six hints on the parameter block when `feature == 1`, before forwarding.
   You can hook the exported `NVSDK_NGX_D3D12_CreateFeature` (d3d 0x528d0, export; it forwards through pointer 0x1011d8),
   the cached core pointer itself (d3d `0x1011d8`, a data slot: swap the pointer, no code patch), or `FUN_18001ccf0` (0x1ccf0).
   The hints go through the exported `NVSDK_NGX_Parameter_SetUI` (d3d 0x51f50). This matches what Pharos does
   (`ac-dlss/src/dlss/ngx.cpp:106`, vtable slot 4 = SetUI). Per mode: read `PerfQualityValue` from the same block (`GetI`).
   Then call `clearFeatures` once so the cache is rebuilt.
2. One-byte patch: d3d **0x1e3af** (`0b` -> `0d` for M, `0c` for L). This forces one preset for every mode when legacy = 0.
   The neighbouring bytes are given in the signature table (8).
3. Preset 0 ("DLL default") also works with a new DLL and lets the NVIDIA app/DRS override apply.

---

## 2. Swapchain and Present

### 2.1 Creation (d3d `FUN_18002a6e0`, called from `d3d::DeviceUtil::create` 0x2dc50, export)

Static, `FUN_18002a6e0`:
- `CreateDXGIFactory1` (the only DXGI import, in d3d only; strings). Factory at NativeDevice+0x188,
  `MakeWindowAssociation(hwnd, 3)` (NO_WINDOW_CHANGES | NO_ALT_ENTER).
- Tearing: QI `IDXGIFactory5` -> `CheckFeatureSupport(PRESENT_ALLOW_TEARING)` -> NativeDevice+0x1a8. If supported, swapchain flags |= 0x800 (ALLOW_TEARING).
- Adapter via `FUN_18002a510` -> NativeDevice+0x180 (IDXGIAdapter, also used by NGX `GetFeatureRequirements`).
- `D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0 (0xb000), ...)` is **imported from d3d12.dll by ordinal 101**
  (102 = D3D12GetDebugInterface, used when create flag 0x10 is set). Ghidra shows `Ordinal_101` / `Ordinal_102`, and the error string reads "D3D12CreateDevice".
  A d3d12 proxy must export those ordinals.
- Queues: `FUN_18002b450(0/1/2)` -> d3d `0x111c38` / `0x111c30` / `0x111c28` (wrapper; `+0x10` = `ID3D12CommandQueue*`). 0x111c38 is the one
  passed to CreateSwapChain, so it is the direct queue.
- **`IDXGIFactory::CreateSwapChain` (legacy, vtable 0x50)** with the `DXGI_SWAP_CHAIN_DESC` at NativeDevice+0xa0:
  | field | value |
  |---|---|
  | BufferDesc | w, h = args; refresh 0/0; **Format R10G10B10A2_UNORM (0x18)**, or **R16G16B16A16_FLOAT (10)** when HDR is requested and the display supports it |
  | SampleDesc | 1, 0 |
  | BufferUsage | RENDER_TARGET_OUTPUT (0x20) |
  | **BufferCount** | **2** |
  | Windowed | `!fullscreen` |
  | **SwapEffect** | **FLIP_DISCARD (4)** |
  | Flags | ALLOW_MODE_SWITCH (2) \| ALLOW_TEARING (0x800 if supported) |
  The swapchain is stored at **NativeDevice+0x178**.
- Colour space: `IDXGISwapChain3::SetColorSpace1(hdr ? 1 : 0)` (vtable 0x130) in resize. 1 = scRGB G10/P709 with FP16, 0 = G22/P709 (static `FUN_18002bfb0`).

### 2.2 Resize (`FUN_18002bfb0`, d3d 0x2bfb0; `DeviceUtil::resizeBackBuffer` 0x31780)

Static: HDR re-detection (`isHDREnabledDisplay`), factory recreate if `!IsCurrent`, release of the 2 back-buffer wrappers,
a GPU drain, then `SetFullscreenState(fs)` (exclusive fullscreen is used), `ResizeTarget` x2,
`ResizeBuffers(0, w, h, fmt, flags)` and `SetColorSpace1`. Back buffers are recreated by `FUN_18002c890` (`GetBuffer(i)` for i = 0, 1).
**The back-buffer index NativeDevice+0x138 is reset to 0.**

### 2.3 Present (`d3d::DeviceUtil::present`, d3d 0x31b50, export)

Static:
1. `FUN_180031340` flushes/submits the frame's command lists. Unless the tweakable `"DX12:Decompress before present"` is off,
   it runs `prepareForPresent` on `backbuffer[idx]`.
2. **Sync interval** = NativeDevice+0xf4 (set by `DeviceUtil::setPresentInterval` 0x317a0: 0 off, 1 vsync, 2 half; initial value from create flags 2/4).
   If tearing is unsupported (+0x1a8 == 0) it is clamped to at least 1.
   **Flags** = `DXGI_PRESENT_ALLOW_TEARING (0x200)` iff windowed (+0xf0 == 0) and interval == 0, otherwise 0.
3. **Call site: d3d 0x31cde** `call [rax+0x40]` (IDXGISwapChain::Present) on NativeDevice+0x178. On failure it resizes and
   retries at 0x31d2d. On DEVICE_REMOVED-class errors it reports "Present Failed".
4. **The back-buffer index is the engine's own**: `idx = (idx + 1) & 1` at NativeDevice+0x138 (d3d 0x31db9..0x31dcf).
   `getAutoBackBuffer` (0x317d0, export) returns `*(NativeDevice+0x140 + idx*8)`. The engine **never calls `GetCurrentBackBufferIndex`**,
   nor `SetMaximumFrameLatency` or `GetFrameLatencyWaitableObject`. Every swapchain call site (loads of [x+0x178]) was enumerated:
   Present, GetBuffer, GetDesc, SetFullscreenState, ResizeTarget, ResizeBuffers, SetColorSpace1, GetFrameStatistics, Release (static, findbytes).
5. It signals the frame fence on the direct queue (`ID3D12CommandQueue::Signal`, vtable 0x70). Frame fence = NativeDevice+0x88 `{u64 next; ID3D12Fence*}`.
   With tweakable `"DX12:Wait for present"` (default on), it spins with `r::yieldCurrentThread` until `fence->GetCompletedValue() + 1 >= next`.
   That allows at most one frame queued behind the GPU. This is the engine's only frame-latency control.
6. Then `FrameEndHandler::endFrame`, and `GetFrameStatistics` to measure the refresh rate (NativeDevice+0x128 int, +0x130 double).

**Consequence for frame generation**: the engine assumes exactly one Present per frame and flips its index 0/1 on its own.
Extra Presents on the same swapchain (2x, 4x) desynchronise its index from DXGI's.
Either present generated frames through a proxy back buffer, or rewrite NativeDevice+0x138 from `GetCurrentBackBufferIndex()`
after the mod's presents. NativeDevice is d3d `[0x136d28]`.

### 2.4 Threads

- The renderer has a dedicated thread **"RMD - Renderer Thread"** (renderer `FUN_18010d150`, `r::registerThread(2)`, string).
  It pops packets from a `MultiProducerFifo` and dispatches them in `FUN_18010d450`.
- The game thread calls `rend::RendererInterfaceWrapper::{present, renderHUD, setPresentInterval, ...}` (exports, renderer 0x100460 / 0x100130 / 0x100e10).
  These only post a type-5 packet into `RendererThreading` (static).
  The renderer thread then runs **RendererX86 vtable** (renderer 0x6323d8) slot 30 = `FUN_1801363e0` -> `d3d::DeviceUtil::present`
  (skipped while r+0x476, device-lost, is set). Slot 17 = `FUN_1801343b0` renderHUD, slot 14 = `FUN_18011b4c0` frame begin (DLSS reset),
  slots 15/16 = `FUN_18011daf0` / `FUN_18011d9e0` renderWorld -> `FUN_18011dc50` (jitter) -> `FUN_18011ee20` (render graph),
  slot 40 = `FUN_1801374b0` apply settings.
- **So Present, DLSS evaluate and HUD rendering all run on the renderer thread**, not the game thread
  (unless RendererThreading runs inline, which has not been checked live).

---

## 3. Frame-generation inputs

| DLSS-G input | Where it is | Format / convention | How established |
|---|---|---|---|
| **Depth** | `gbufferdepth`, the `depth` arg of `doAntiAliasing` (render res). In the early path, **`gbufferdepthUpscaled` (output res, de-jittered)** after `upscaleAndDejitterDepth` in `FUN_18012ff20` | **Conventional Z (not reversed)**: depth states used are LessEqual/Less (renderer imports `DepthLessEqual_*`, `DepthLess_WriteEnabled`; only one Greater use, `FUN_1801cdff0`), the depth target's init carries clear 1.0, and DLSS is created without DepthInverted. Format: PixelFormat at r+0x45e (not resolved; PixelFormat 1/2 = D24S8, 8 = D32) | static, `FUN_18011ee20` 0x334, `FUN_18001e1d0` |
| **Motion vectors** | `VectorBlur::m_pVelocityGeometryBuffer` = `*(*(r+0x118)+0x120)` (the `mv` arg) | **R16G16_FLOAT** (PixelFormat 0xb -> DXGI 34, table d3d 0x62420), render res, **UV units** (DLSS MVScale = render size), not jittered (no MVJittered). **Includes camera motion**: `init_velocity` (VectorBlur.rfx, `FUN_18022fbf0`) draws a full-screen camera-reprojection pass (`g_mVectorBlurScreenToPreviousScreen` = VectorBlur+0x50) into `m_pVelocityBuffer`, then geometry velocity is drawn over it (`FUN_18022fce0`). That is copied to `m_pVelocityGeometryBuffer` before particle/effects velocity is added (`FUN_18022fd90`). Sign as DLSS expects (prev - cur), since it is fed with positive scale | static `FUN_180128aa0`, `FUN_18022f800`, `FUN_18022fd90` |
| **Jitter** | RendererX86 **r+0x480 / r+0x488** (doubles, NDC), written in `FUN_18011dc50`. Pixels = `(halton2 - 0.5)*s`, `(halton3 - 0.5)*s` (s = renderer float 0x9114d0, presumably "SSAA:Temporal Jitter Scale"); phase count `lrintf(8*(outH/renderH)^2)`, 1024 with RR | static `FUN_18011dc50` 0x11de60..0x11df20, `doAntiAliasing` conversion |
| **Exposure** | 1x1 texture passed as `exposure`; pre-exposure `1/DAT_180a88ab0` (early) or 1.0 (late) | | static callers |
| **Camera matrices** | `rend::View` constant providers (`g_mWorldToView`, `g_mViewToClip`, `g_mClipToView`, `g_mWorldToClip`, `g_mClipToWorld`, `g_mPreviousClipToClipNoJitter`, `g_mClipToPreviousClipNoJitter`; ctor renderer 0x17dfe0). VectorBlur+0x50 holds the screen->previous-screen matrix. Details belong to the camera RE | strings + xrefs |
| **HUD-less colour** | The render target at entry of **renderHUD `FUN_1801343b0`** (slot 17): `r+0x3c8` (`RendererX86::m_pCreatedColorTarget`) if present, else `getAutoBackBuffer()`. At that point the frame has had "Final Post Process and Display Map" but no UI | static `FUN_1801343b0` |
| **UI** | Coherent GT draws through `CoherentRendererInterface` at **r+0x680** (set by `setCoherentInterface`, slot 53): `(*(*(r+0x680)))()` inside renderHUD, gated by `rend::RenderOptions::PaintUI` (export). In SDR it draws straight into the target/back buffer. In HDR with r+0x108 set, it draws into **`RendererX86::pSeparateHudColorTarget`** (PixelFormat 0xe = R8G8B8A8_UNORM, cleared to r+0x464), then the compute pass `copy.rfx/composeSceneAndUI` blends it over (`g_tFillSource` UI, `g_rwtBackground` scene). ShapeEngine debug shapes are drawn before (1) and after (2) the UI | static `FUN_1801343b0` |
| **Backbuffer** | `getAutoBackBuffer()` at present, or NativeDevice+0x140[idx] | R10G10B10A2 SDR / FP16 scRGB HDR | 2.1 |

At the DLSS evaluate tap (`doAntiAliasing` entry) you have render-res colour (linear HDR in the early path), depth, MVs,
jitter (args 11/12), exposure, reset, render/output sizes (DLSSData+0x0c..+0x18) and the command list
(TLS DeviceState+0x18, or d3d 0x111c18). On exit you have the output-res DLSS colour. On the early path the next pass
produces output-res de-jittered depth, which is a better depth for DLSS-G than render-res depth.

Render graph order (static, pass names in `FUN_18011ee20`): ... Primarypass -> Transparents -> **TAA and Upscale Depth (early DLSS)** -> Post TAA Downscale -> Vectorblur -> Hilights / volumetrics / Tornado -> DoF -> Vectorblur -> Displacement -> autoexposure -> flares -> Bloom -> final image pass (tonemap/grade) -> TAA **or DLSS anti aliasing (late DLSS)** -> **Final Post Process and Display Map** -> debug -> Cleanup. Then on the renderer thread: renderHUD (slot 17) -> present (slot 30).

---

## 4. Reflex / NVAPI

None. There is no `nvapi`, `NvAPI_`, `Reflex`, `LowLatency`, `SetSleepMode`, `sl.interposer`, `nvngx_dlssg`/`nvngx_dlssd`, AMD AGS or FFX string,
ASCII or UTF-16, in `Control_DX12.exe` or any `*_rmdwin10_f.dll` of the install (string scan). Only `nvngx_dlss.dll` itself mentions NVAPI.
Frame pacing is the vsync interval plus the "DX12:Wait for present" fence spin (2.3). Reflex would have to come from the mod
(NVAPI via `third_party/nvapi-*`: `SetSleepMode` / `Sleep` on the game's device, markers around the renderer thread's frame).

---

## 5. Options and where the DLSS mode lives

- Persisted renderer settings (`renderer.ini`; strings in renderer `load` 0x102ec0 / `save` 0x103100). The struct is
  **coregame global 0x8a2ac0** (`g_graphicsOptions` model). The renderer copies it to **RendererX86+0x6a0** in apply-settings `FUN_1801374b0` (0xb0 bytes):
  | settings offset | name (string) | meaning |
  |---|---|---|
  | +0x58 | `m_eDLSSQuality` | **0 = off, 1 = DLSS SR, 2 = SR + RR** (RR only with RT on). Sets tweakables `dlss:Enable DLSS SR` (renderer 0x914530) and `dlss:Enable DLSS RR (requires SR)` (0x914620) |
  | +0x70 / +0x74 | | output (display) resolution (r+0x710/0x714) |
  | +0x78 / +0x7c | | **render resolution** (r+0x718/0x71c). With DLSS on, this chosen resolution determines the quality mode (1.3) |
  | +0xa8 | `m_bDLSSConservativeMipBias` | mip bias offset 0 vs -1 (renderer 0x7ec2f4) |
  | +0xa9 | `m_bDLSSLegacyModel` | 1 = CNN presets E/F, 0 = preset K (1.4) |
  (field names: static serializer `FUN_180107c30`, `lea r8,[rdi+0x58]` beside `m_eDLSSQuality`, `[rdi+0xa8]`/`[rdi+0xa9]` beside the two bools)
- In-game UI (coregame `UIGraphicsOptionsModel::initProperties` 0x1cb490): a **bool `MENU_OPTIONS_NVIDIA_DLSS`**. Its callback 0x1c94e0
  writes coregame 0x8a2b18 (= settings+0x58) and forces AA (0x8a2af4) off when DLSS is on. The UI never offers 2.
  DLSS "quality" is chosen as a render resolution: `initializeDLSSSettings` (coregame 0x1cb110, export) and `addDLSSResolutionOptions`
  (0x1cad00, export) list `getRenderResolutionForOutputResolution(out, mode)` for modes 3, 0, 1, 2, 4, 5 (UltraPerf, Perf, Balanced, Quality, UltraQuality, DLAA),
  plus DLAA of 1.25x / 1.333x / 1.5x the output (super-sampled entries).
- Live state: `rend::RenderOptions::DLSSEnabled` (renderer export, the result of resetDLSSFeature each frame).
- Debug tweakables (strings): `dlss:Enable DLSS SR`, `dlss:Enable DLSS RR (requires SR)`, `dlss:Show info`,
  `DX12:Decompress before present`, `DX12:Wait for present`, `SSAA:Temporal Jitter Scale`.

---

## 6. Other notes

- d3d exports the whole NGX parameter API (`NVSDK_NGX_Parameter_{Set,Get}{I,UI,F,D,ULL,VoidPointer,D3d12Resource}`) and
  `NVSDK_NGX_D3D12_{AllocateParameters,CreateFeature,EvaluateFeature(_C),ReleaseFeature,GetCapabilityParameters,GetFeatureRequirements,GetParameters,GetScratchBufferSize,Shutdown(1)}` (export).
  It does **not** export `Init`. A mod can reuse the game's initialised NGX instance (same device, same core) for an extra SR feature
  per eye, or for DLSS-G (feature 11: the core looks for `nvngx_dlssg.dll` in the exe directory, since no path list was given).
- Per-eye DLSS (the torchbearer approach): the game's cache key does not include an eye, so two eyes with equal sizes share one feature.
  Create a second feature with the same create params (the exported `NVSDK_NGX_D3D12_CreateFeature`, a fresh `AllocateParameters`
  block, the same hints and flags), and swap DLSSData+0x28/+0x30 around the right eye's `doAntiAliasing`. Release it on `clearFeatures` or a size change.
- The install dir currently has `Control_DX12.exe.bak` (differs from the exe, both dated today 13:57) and `wtf.dll`, which are not part of this RE.
  The analysed exe matches the install's current `Control_DX12.exe` (sha256 57a8912f...).

---

## 7. Recommended hook points for Threshold

1. **NGX CreateFeature** (preset override): swap the pointer at d3d **0x1011d8** (core CreateFeature), or hook the export `NVSDK_NGX_D3D12_CreateFeature` (d3d 0x528d0).
   When feature == 1, `SetUI` the six `DLSS.Hint.Render.Preset.*` from config (default K/M/L by `PerfQualityValue`).
   Ship nvngx_dlss >= the 310.9.16 SDK DLL. Call `?clearFeatures@DLSS@d3d@@SAXXZ` after a config change.
2. **`?doAntiAliasing@DLSS@d3d@@SA_NPEAVNativeTexture@2@00000000_NNNMMM@Z`** (d3d 0x1f990): the tap for depth, MVs (RG16F UV), jitter (args, NDC),
   reset, exposure and the output, and the place to swap per-eye features (DLSSData `[0x111be0]`+0x28/+0x30). It runs on the renderer thread.
3. **`?resetDLSSFeature@DLSS@d3d@@SA_NIIII_N000AEA_N@Z`** (d3d 0x1f3f0): render and output sizes and flags per frame, for (re)creating
   the mod's own features (second eye, DLSS-G) at the same sizes.
4. **renderHUD, RendererX86 slot 17 `FUN_1801343b0`** (renderer 0x1343b0) entry: copy the HUD-less target (`r+0x3c8` or `getAutoBackBuffer()`).
   Optionally bracket the Coherent call `(*(*(r+0x680)))()` to isolate the UI, or use `pSeparateHudColorTarget` on the HDR path.
5. **`?present@DeviceUtil@d3d@@SAXXZ`** (d3d 0x31b50), or `IDXGISwapChain::Present` via a dxgi.dll proxy (the factory comes from `CreateDXGIFactory1`
   in d3d). Generated frames are inserted here. Fix up NativeDevice+0x138 (back-buffer index) or present through a proxy back buffer, and mind the
   "DX12:Wait for present" fence spin. Reflex sleep/markers also go here (no NVAPI in the game).
6. **`FUN_18011dc50`** (renderer 0x11dc50, renderWorld): the per-frame jitter writer (r+0x480/0x488). It is the stereo agent's territory, but FG needs the jitter
   and camera at the same frame index.
7. Read-only: `m_eDLSSQuality` / render resolution at coregame 0x8a2ac0+0x58/+0x78, and `rend::RenderOptions::DLSSEnabled` (renderer export).

---

## 8. Byte signatures (first 24 bytes at each RVA)

d3d_rmdwin10_f.dll:
| RVA | function | bytes |
|---|---|---|
| 0x1e7f0 | `DLSS::initDLSS` (export) | `48 8b c4 55 48 8d a8 88 fe ff ff 48 81 ec 70 02 00 00 48 c7 45 f0 fe ff` |
| 0x1f3f0 | `DLSS::resetDLSSFeature` (export) | `40 55 41 54 41 55 41 56 41 57 48 8d 6c 24 f1 48 81 ec b0 00 00 00 4c 8b` |
| 0x1f860 | `DLSS::clearFeatures` (export) | `48 89 5c 24 08 48 89 74 24 10 57 48 83 ec 20 48 8b 3d 6a 23 0f 00 0f 57` |
| 0x1f990 | `DLSS::doAntiAliasing` (export) | `48 8b c4 48 89 58 18 48 89 50 10 48 89 48 08 55 56 57 41 54 41 55 41 56` |
| 0x1f340 | `DLSS::getRenderResolutionForOutputResolution` (export) | `48 89 5c 24 08 48 89 7c 24 10 48 8b 05 8f 28 0f 00 48 8b 98 a8 00 00 00` |
| 0x1e1d0 | feature get/create (preset logic) | `40 55 56 57 41 54 41 55 41 56 41 57 48 8d 6c 24 f9 48 81 ec d0 00 00 00` |
| 0x1e39c | preset select (patch byte at 0x1e3af) | `41 bf 05 00 00 00 41 83 fd 03 75 06 41 bf 06 00 00 00 b8 0b 00 00 00 80 bd 97 00 00 00 00 44 0f 44 f8` |
| 0x1ccf0 | NGX_D3D12_CREATE_DLSS_EXT | `48 89 5c 24 08 48 89 6c 24 10 48 89 74 24 18 57 48 83 ec 20 48 8b 7c 24` |
| 0x1d320 | NGX_D3D12_CREATE_DLSSD_EXT | `48 89 5c 24 08 48 89 6c 24 10 48 89 74 24 18 57 48 83 ec 20 48 8b 7c 24` |
| 0x1cdf0 | NGX_D3D12_EVALUATE_DLSS_EXT | `48 89 5c 24 08 48 89 6c 24 10 48 89 74 24 18 57 48 83 ec 40 49 8b d8 0f` |
| 0x1d460 | NGX_D3D12_EVALUATE_DLSSD_EXT | `48 89 5c 24 08 48 89 6c 24 10 48 89 74 24 18 57 48 83 ec 40 49 8b f8 0f` |
| 0x528d0 | `NVSDK_NGX_D3D12_CreateFeature` (export) | `48 89 6c 24 10 48 89 74 24 18 48 89 7c 24 20 41 56 48 83 ec 20 48 83 3d` |
| 0x52a20 | `NVSDK_NGX_D3D12_EvaluateFeature_C` (export) | `48 83 ec 38 48 8b c1 48 8b 0d b2 e7 0a 00 48 85 c9 75 0a b8 07 00 d0 ba` |
| 0x51f50 | `NVSDK_NGX_Parameter_SetUI` (export) | `48 89 5c 24 08 48 89 74 24 10 48 89 7c 24 18 41 56 48 83 ec 20 48 8b 01` |
| 0x52cc0 | NGX Init_ProjectID (static lib) | `48 89 5c 24 10 48 89 6c 24 18 56 41 54 41 55 41 56 41 57 48 83 ec 70 48` |
| 0x52110 | NGX core loader / pointer table fill | `48 8b c4 55 41 56 41 57 48 8d 68 a1 48 81 ec b0 00 00 00 48 c7 45 37 fe` |
| 0x31b50 | `DeviceUtil::present` (export) | `48 8b c4 41 54 41 56 41 57 48 83 ec 70 48 c7 40 b8 fe ff ff ff 48 89 58` |
| 0x317a0 | `DeviceUtil::setPresentInterval` (export) | `48 8b 05 81 55 10 00 89 88 f4 00 00 00 c3 cc cc 48 8b 05 71 55 10 00 8b` |
| 0x317d0 | `DeviceUtil::getAutoBackBuffer` (export) | `48 8b 0d 51 55 10 00 48 63 81 38 01 00 00 48 8b 84 c1 40 01 00 00 c3 cc` |
| 0x2dc50 | `DeviceUtil::create` (export) | `48 8b c4 55 41 54 41 55 41 56 41 57 48 8d 68 b8 48 81 ec 20 01 00 00 48` |
| 0x2a6e0 | factory/device/swapchain init | `48 8b c4 57 41 54 41 55 41 56 41 57 48 81 ec 40 07 00 00 48 c7 80 80 f9` |
| 0x2bfb0 | swapchain resize | `48 8b c4 55 56 57 41 54 41 55 41 56 41 57 48 8d 68 b8 48 81 ec 10 01 00` |
| 0x3a520 | `NativeTexture::prepareForConsuming` (export) | `41 56 48 83 ec 50 48 c7 44 24 20 fe ff ff ff 48 89 5c 24 68 48 89 74 24` |

renderer_rmdwin10_f.dll:
| RVA | function | bytes |
|---|---|---|
| 0x11b4c0 | RendererX86 slot 14, frame begin (resetDLSSFeature) | `48 8b c4 55 41 54 41 55 41 56 41 57 48 8d 68 a1 48 81 ec d0 00 00 00 48` |
| 0x1374b0 | slot 40, apply settings (DLSS mode, early flag) | `48 8b c4 57 48 83 ec 60 0f 10 02 48 89 58 08 48 8b f9 48 89 68 10 4c 89` |
| 0x11dc50 | renderWorld (jitter -> r+0x480/0x488) | `48 8b c4 4c 89 48 20 55 56 57 41 54 41 55 41 56 41 57 48 8d a8 38 f6 ff` |
| 0x11ee20 | render graph build | `48 8b c4 55 56 57 41 54 41 55 41 56 41 57 48 8d a8 98 f9 ff ff 48 81 ec` |
| 0x12ff20 | early DLSS pass + upscaleAndDejitterDepth | `48 8b c4 48 89 48 08 55 56 57 41 54 41 55 41 56 41 57 48 81 ec d0 01 00` |
| 0x13f3e0 | late DLSS pass | `48 89 5c 24 08 48 89 6c 24 10 48 89 74 24 18 48 89 7c 24 20 41 54 41 56` |
| 0x1343b0 | slot 17, renderHUD (UI compose) | `48 8b c4 55 53 56 57 41 54 41 55 41 56 41 57 48 8d a8 18 fe ff ff 48 81` |
| 0x1363e0 | slot 30, present -> d3d present | `48 89 4c 24 08 48 83 ec 38 48 c7 44 24 20 fe ff ff ff 80 b9 76 04 00 00` |
| 0x128aa0 | VectorBlur velocity passes | `41 56 48 83 ec 30 48 c7 44 24 20 fe ff ff ff 48 89 5c 24 40 48 89 74 24` |
| 0x22fd90 | velocity geometry copy + effects velocity | `48 8b c4 41 56 48 81 ec a0 01 00 00 48 c7 80 e8 fe ff ff fe ff ff ff 48` |
| 0x100460 | `RendererInterfaceWrapper::present` (export, game thread) | `48 89 5c 24 08 48 89 74 24 10 57 48 83 ec 20 48 8b 35 d2 24 70 00 ba 10` |
| 0x100130 | `RendererInterfaceWrapper::renderHUD` (export, game thread) | `48 89 5c 24 08 48 89 74 24 10 57 48 83 ec 20 48 8b 35 02 28 70 00 ba 10` |

coregame_rmdwin10_f.dll:
| RVA | function | bytes |
|---|---|---|
| 0x1cb110 | `UIGraphicsOptionsModel::initializeDLSSSettings` (export) | `48 89 4c 24 08 55 48 8b ec 48 81 ec 80 00 00 00 ff 15 ca 84 3e 00 84 c0` |
| 0x1c94e0 | DLSS toggle callback | `83 3d 31 96 6d 00 01 8b 05 07 96 6d 00 41 0f 9d c0 0f b6 d1 45 33 c9 89` |

Exported functions should be resolved with `GetProcAddress` by their mangled names (listed in `re/exports/*.dem`) and then
checked against these bytes. Internal ones (no export) need the RVA plus a signature check.
