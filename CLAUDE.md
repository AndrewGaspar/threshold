# Threshold

Stereoscopic 3D, DLSS Multi Frame Generation and current-DLSS-model mod for Remedy's **Control**
(GOG Ultimate Edition, `gog:2049187585`, `Control_DX12.exe`, Northlight engine, **Direct3D 12**
under vkd3d-proton), packaged as a Quiver mod (`dist/quiver-mod.toml`) with a Quiver options
surface (`dist/quiver-options.toml`, surface `threshold`). Sibling of Torchbearer
(`~/code/torchbearer`, Alan Wake Remastered: same engine family, same core) and Pharos
(`~/code/ac-dlss`, AC Origins: DLSS-G / MFG and Reflex).

## Layout

- `src/proxy.cpp`: `dxgi.dll` proxy (`dxgi=n,b`; `d3d_rmdwin10_f.dll` imports dxgi statically, so
  we load before the game's code). Forwards to the system dxgi (DXVK's) and hooks factories ->
  swapchains.
- `src/core/`: copied from Torchbearer: log (`threshold.log` in `%QUIVER_LAB_RUN_DIR%` in the lab,
  else next to the exe), config (`threshold.ini`, ids `section.key`, polled every 250 ms, also fed
  by the Quiver SDK; `cfg_listen`), hooks (MinHook `hook_fn`, `hook_vtable`), lab channel
  (`lab_state`, `lab_verb`), Quiver SDK glue, Present/Resize listeners, `module_image(L"x.dll")`,
  `module_export`, `sig_match`.
- `src/features/`: one file (or folder) per feature; register in `features_register()`, hook in
  `features_start()`. `rsettings.cpp` edits the game's `renderer.ini` (JSON, in the **install
  folder** for GOG) from DllMain: intro videos, DLSS legacy model. `dlss.cpp`: refuses the game's
  broken Ray Reconstruction feature, logs presets/DLL version (docs/dlss.md). `fg/`: DLSS Frame
  Generation / MFG via the game's NGX, proxy back buffers, present thread (docs/fg.md). `stereo/`:
  same-frame stereo 3D (docs/stereo.md). `nav.cpp`: lab verbs `where`/`teleport`/`jump`/`restart`
  (docs/navigation.md; locations in the lab fixtures' `locations.txt`).
- `dist/`: `threshold.ini` (shipped defaults), `quiver-mod.toml`, `quiver-options.toml`. Change
  them together and lint: `quiver mods gog:2049187585 options lint build/mod`.
- `re/`: notes (`re/*.md`: `dlss.md`, `stereo.md`, `levels.md`), `re/exports/*.dem` (every
  engine DLL's exports, demangled: `mangled<TAB>demangled`), `re/scripts/` (Ghidra query server
  `Serve.java` + client `ghq`), `re/bin/` (copies of the binaries, gitignored, hashes in
  `re/bin.sha256`), `re/ghidra/<dll>/` (one analysed project per DLL, gitignored).

## Reverse engineering

The engine is split into DLLs (`renderer_`, `d3d_`, `rl_`, `coregame_`, ... `_rmdwin10_f.dll`)
that **export thousands of C++ symbols**: grep `re/exports/*.dem` before opening Ghidra.
Addresses in notes and code are **RVAs per module** (image base 0x180000000 for the DLLs,
0x140000000 for the exe), written `renderer+0x123456`. Code resolves them with
`module_image(L"renderer_rmdwin10_f.dll").at(rva)` or by export name, and verifies a byte
signature first, logging and disabling the feature on a mismatch.

**Never open `re/ghidra/<dll>` directly from more than one process**: copy it
(`cp -r --reflink=auto re/ghidra/renderer re/ghidra-$NAME`) and run
`/opt/ghidra/support/analyzeHeadless re/ghidra-$NAME control -process renderer_rmdwin10_f.dll
-noanalysis -readOnly -scriptPath re/scripts -postScript Serve.java <reqdir>` against your copy.

## Build and run

```sh
scripts/build                                   # mingw-w64 -> build/mod/{dxgi.dll,threshold.ini,...}
quiver lab launch control --wait frame          # the instance's mod_dir is build/mod
quiver lab logs control                         # run folder; threshold.log is there
quiver lab set control install:threshold.ini:[section]key=value --wait   # live config change
tools/lab/moddir OUT fg.enabled=1 ...           # a mod copy with start-only settings (launch --mod OUT)
tools/lab/continue control [--mod OUT]          # launch and drive the menus into the world (Continue)
```

Lab instances: `control` (main), `control-nav`, `control-stereo` (`quiver lab list`).

**In-game lab work runs on rayblaster** (the user's desktop: RTX 5090 32 GB, 32 threads). It has
the GOG copy installed through Quiver for the lab; its Steam copy (steam:870780, the user's real
play, 65 h) is Steamworks and refused by the lab by design. The Steam and GOG builds share
byte-identical engine DLLs, and the Steam `Control_DX12.exe` equals GOG's pre-patch `.bak` (only
store URLs differ), so every RVA/signature applies to both. rayblaster has no mingw: build here and
drive it with **`quiver --machine rayblaster lab …`** (quiver r710+): `--mod build/mod` /
`mod_dir=` upload the folder, and `shot`/`logs`/`fixture save` land under
`~/.cache/quiver/remote/rayblaster/lab/…` (path printed). For scripts that must run there
(tools/lab/continue, re/scripts), `tools/lab/rb-sync` copies build/mod, tools, re/scripts and the
lab fixtures to `~/threshold-lab`, and `tools/lab/rb CMD…` runs a command there over SSH. Keep lab
resolution modest (1920x1080 or less) unless a test needs more. rayblaster's default Proton is
GE-Proton11-12 (11-6 asserts in the lab's steam.exe). A new instance needs `quiver lab fixture
install <i> game-start` + `chmod -R u+w` on the prefix's `Documents/My Games/Control/Saves` (the
fixture carries the user's EULA acceptance and save). On rayblaster (32 GB VRAM) several Control instances can run at
once (check `quiver --machine rayblaster lab host` for free VRAM). Timing measurements (fps,
pacing, latency) only under the GPU lock (`quiver lab gpu -- …`, which throttles the other
instances for its duration), and say so next to the numbers.

Always pass `--source agent:<name>` (or `QUIVER_SOURCE`). Follow `quiver lab brief gog:2049187585`.
The lab instance `control` runs 1920x1080@60. Galaxy is offline in the lab; the game shows a
"could not restore your progress on locked achievements" box at start (Return dismisses it).
The EULA is the user's to accept (they did, in their own prefix; the lab is re-golded from it).
The GPU (RTX 5070 Laptop, 8 GB) is shared: check `quiver lab host`, and `quiver lab down`
instances you're done with. The person's play comes first.

**Run at most one Control instance in the world at a time** (DLSS + RT, ~3 GB VRAM each). On
2026-11-22, three at once filled VRAM (`NV_ERR_NO_MEMORY` from 17:11), skewed every measurement,
and wedged the host's NVIDIA driver at ~18:29 (Xwayland stuck, reboot needed). Parallel agents
share one instance in turn, or do static work while another holds the GPU (`quiver lab gpu` lock).

## Conventions

- Every claim in `docs/`/`re/` notes says how it was established (export, static, live, measured).
- Features must be inert when their config is off; a hook with the feature off is a pass-through.
- Don't commit unless the user asks.
