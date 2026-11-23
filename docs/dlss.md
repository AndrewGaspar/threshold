# DLSS Super Resolution in Control: models, presets and the Ray Reconstruction crash

All findings 2026-11-22, lab instance `control` (1920x1080, GE-Proton11-12, driver 620.70.06,
RTX 5070 Laptop). Static RE is in [`re/dlss.md`](../re/dlss.md).

## What the game does by itself

- It ships `nvngx_dlss.dll` **310.2.1** (export, file version). NGX loads it from the install
  folder (live: `nvngx.log`, "feature dlss snippet: …/Control/nvngx_dlss.dll version: 310.2.1").
- It asks for its preset itself (static, `re/dlss.md` 1.4): **CNN presets E/F** when
  `m_bDLSSLegacyModel` is true, **preset K** (DLSS 4 transformer) in all six modes when false.
  Live: Threshold logs the hints at feature creation, `preset hints (DLAA UQ Q B P UP): K K K K K K`
  with `m_bDLSSLegacyModel: false` (run r12).
- **The game's default is `m_bDLSSLegacyModel: true`** (live: the renderer.ini it writes on first run,
  and the user's own install). So out of the box Control runs the CNN model, not the transformer.

## The Ray Reconstruction crash (game bug, exposed by Proton)

`m_eDLSSQuality` is 0 off, 1 SR, **2 SR + Ray Reconstruction** (static, `re/dlss.md` 5). The game's
RR path is unfinished: both `doAntiAliasing` callers pass NULL normals and albedos (static). On
Windows the RR feature never gets created because the install has no `nvngx_dlssd.dll`, and the
game falls back to SR. Under Proton, NGX finds the driver's
`/usr/lib/nvidia/wine/nvngx_dlssd.dll` (310.9.0) as a fallback snippet (live: `nvngx.log`, "Found
dlssd driver fallback snippet"), RR is created, and the first in-world evaluation crashes:

| run | m_eDLSSQuality | legacy | mod | result |
|---|---|---|---|---|
| r8 | 2 | true | yes (no RR guard yet) | 0xC0000005 within 60 s of Continue |
| r9 | 2 | true | no | 0xC0000005 |
| r10 | 1 | true | no | in-world, alive |
| r11 | 2 | true | RR guard | alive, RR refused, SR created |
| r12 | 2 | false | RR guard | alive, preset K |

The user's real install has `m_eDLSSQuality: 2` (the menu's DLSS on with RT effects on), so a real
session would hit this too. **Threshold refuses RR feature creation** (`src/features/dlss.cpp`, a hook
on the game's exported `NVSDK_NGX_D3D12_CreateFeature`, feature 13) unless `[dlss]
ray_reconstruction = 1`, which puts the game on its own Windows fallback path.

**Without the mod:** the Wine DLL override `nvngx_dlssd=d` also stops it. The disabled load order
covers NGX's full-path load of `/usr/lib/nvidia/wine/nvngx_dlssd.dll`, and NGX then reports
"Selected feature module (best choice) is disqualified. Feature is not available". The game takes the
same SR fallback, and the vanilla game with `m_eDLSSQuality: 2` stays up in-world (live, run r15,
`--no-mod`). Quiver ships it as the built-in fix `control-no-ray-reconstruction` (quiver
0.1.0.r696, `[launch] dll = { nvngx_dlssd = "d" }`), installed for the user's Control on 2026-11-22.
Threshold's own refusal stays in place for installs without that fix.

## Latest models: Threshold + Quiver Tweaks

Quiver's `dlss.sr.model` tweak sets dxvk-nvapi's DRS override
(`DXVK_NVAPI_DRS_NGX_DLSS_SR_OVERRIDE=on`, `…_RENDER_PRESET_SELECTION=render_preset_X`). The
override can only pick presets present in the DLL the game loads, and 310.2.1 predates the DLSS 4.5
presets M and L. Threshold ships the SDK's **`nvngx_dlss.dll` 310.9.16**. Live (run r13, with the
tweak's variables set in the instance):

- `nvngx_dlss_310_9_16.log`: "Reading NGX_OVERRIDE_RENDER_PRESET_SELECTION from DRS, value = 13",
  then "(DLAA|Quality|Balanced|Perf|UltraPerf|UltraQuality) Using DRS Overridden Preset M".
- The DLSS indicator (`DXVK_NVAPI_SET_NGX_DEBUG_OPTIONS=DLSSIndicator=1024`) in game:
  "Render Preset: M", "DLSS v310.9.16".

So for current models: keep Threshold enabled (DLL + RR guard + `legacy_model = 0` for K without any
tweak), and choose M or L with Quiver's DLSS Super Resolution model tweak.

## Options

| `[dlss]` | default | |
|---|---|---|
| `legacy_model` | 0 | written to renderer.ini before the game reads it; 0 = the game asks for K |
| `ray_reconstruction` | 0 | 1 lets the game create RR (crashes with the game's NULL inputs) |
