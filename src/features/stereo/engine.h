#pragma once
// Control's renderer for stereo: RVAs and signatures (re/stereo.md, re/dlss.md; docs/stereo.md).
// Code RVAs are checked against their first bytes before hooking or calling; data RVAs are only
// trusted when the module is the build they were read from (PE TimeDateStamp) and the code that
// uses them matched.
#include <cstddef>
#include <cstdint>

namespace th::stereo {

constexpr wchar_t kRenderer[] = L"renderer_rmdwin10_f.dll";
constexpr wchar_t kD3D[] = L"d3d_rmdwin10_f.dll";
constexpr wchar_t kRl[] = L"rl_rmdwin10_f.dll";

// The analysed build (re/bin, PE header TimeDateStamp).
constexpr uint32_t kRendererStamp = 0x67e06a60;
constexpr uint32_t kD3DStamp = 0x67e06a12;

// --- renderer_rmdwin10_f.dll -----------------------------------------------------------------
// rend::RendererX86 main vtable and the slots used (static: RTTI locator, slot contents).
constexpr uintptr_t kRvaRendererVtbl = 0x6323d8;
constexpr size_t kSlotRenderWorld = 15;  // (r, const m::View*, space, sun, space2): the main view
constexpr size_t kSlotRenderHUD = 17;    // (r)
constexpr size_t kSlotEnd = 18;          // (r)
constexpr uintptr_t kRvaRenderWorld = 0x11daf0;
constexpr uintptr_t kRvaRenderHUD = 0x1343b0;
constexpr uintptr_t kRvaEnd = 0x134bb0;
// Slots 15 and 16 share these 40 bytes; the vtable check tells them apart.
constexpr char kSigRenderWorld[] =
    "48 89 5c 24 08 48 89 6c 24 10 48 89 74 24 18 48 89 7c 24 20 41 56 48 83 ec 40 48 8b 99 78 06 00 00 48 8b f1 49 "
    "8b e9 4d";
constexpr char kSigRenderHUD[] = "48 8b c4 55 53 56 57 41 54 41 55 41 56 41 57 48 8d a8 18 fe ff ff 48 81 ec a8 02 00 00";
constexpr char kSigEnd[] = "48 8b c4 55 57 41 55 41 56 41 57 48 8d a8 28 fa ff ff 48 81 ec b0 06 00 00 48 c7 44 24 50 fe ff";

// RendererX86 fields (static: slots 14/15/17/18).
constexpr size_t kRMainView = 0x678;      // rend::View* (embedded m::View at +0x10)
constexpr size_t kRCreatedColor = 0x3c8;  // Texture2D*: the frame's colour target. The graph's Cleanup
                                          // pass (0x134040) sets it to the final colorbufferIN; the HUD
                                          // draws into it; `end` blits it to the back buffer at
                                          // (viewport offset, output size) and clears it
constexpr size_t kRCreatedOwned = 0x3d0;  // u8: +0x3c8 was created by renderHUD; `end` frees it
constexpr size_t kRRtManager = 0x20;      // rend::RenderTargetManager*
constexpr size_t kRSsaa = 0x138;          // SSAA* (TAA ring)
constexpr size_t kRDeviceLost = 0x476;    // u8
constexpr size_t kRSsao = 0xd8;           // rend::SSAO* (also the RT shadow/reflection buffers)
// rend::SSAO vtable slot 1: per-frame allocation of its buffers (+0xf0/+0xf8/+0x128/pairs), called by
// `begin` once per frame; the RT pass's filter (0x248940) frees +0xf8/+0x128 at the end of every
// world render, so a second render in the same frame needs it again (live: null +0x128 in 0x245e60).
constexpr uintptr_t kRvaSsaoPrepare = 0x2434a0;
constexpr char kSigSsaoPrepare[] = "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8d ac 24 18 fc ff ff 48 81 ec";

// Globals (static: setScreen slot 2, renderHUD, slot 14).
constexpr uintptr_t kRvaViewportOffset = 0x90e188;  // int x, y: the output inside the back buffer
constexpr uintptr_t kRvaOutputSize = 0x90e248;      // int w, h
constexpr uintptr_t kRvaRenderSize = 0x90e488;      // int w, h
constexpr uintptr_t kRvaFrameCounter = 0x8029d0;    // u32, ++ per slot 14 begin
constexpr uintptr_t kRvaDlssReset = 0x128d1e6;      // u8, the DLSS reset flag passed to doAntiAliasing

// rend::View (static: ctor 0x17dfe0, storePrevious 0x180700).
constexpr size_t kViewM = 0x10;          // the embedded m::View
constexpr size_t kViewPrevClip = 0x3d0;  // stored previous view-to-clip, double[4][4]
constexpr size_t kViewPrevW2V = 0x450;   // stored previous world-to-view, double[4][3]
constexpr size_t kViewHasPrev = 0x4b0;   // u8

// --- m::View (rl) -----------------------------------------------------------------------------
constexpr size_t kMViewSize = 0x2e0;     // the renderWorld packet's copy
constexpr size_t kMWorldToView = 0x10;   // double[4][3]
constexpr size_t kMViewToWorld = 0x70;   // double[4][3]: rows right, up, forward, position
constexpr size_t kMViewToClip = 0xd0;    // double[4][4], row vectors (v' = v M)
constexpr char kExpSetViewToWorld[] = "?setViewToWorld@View@m@@QEAAXAEBV?$Matrix4x3Template@N@2@@Z";
constexpr char kExpSetViewToClip[] = "?setViewToClip@View@m@@QEAAXAEBV?$Matrix4Template@N@2@@Z";
constexpr char kSigSetViewToWorld[] = "48 81 ec 88 00 00 00 48 8b 02 4c 8b c9 48 89 41";
constexpr char kSigSetViewToClip[] = "48 81 ec 88 00 00 00 48 8b 02 4c 8b d1 48 89 81";

constexpr char kExpAllocTexture2D[] =
    "?allocTexture2D@RenderTargetManager@rend@@QEAAPEAVTexture2D@d3d@@AEBVTexture2DInit@4@PEBD@Z";
constexpr char kExpRtFree[] = "?free@RenderTargetManager@rend@@QEAAXPEAVTexture2D@d3d@@@Z";
constexpr char kSigAllocTexture2D[] = "48 8b c4 55 41 54 41 55 41 56 41 57 48 8d 6c 24 90 48 81 ec 70 01 00 00";
constexpr char kSigRtFree[] = "40 56 57 41 56 48 83 ec 30 48 c7 44 24 20 fe ff ff ff 48 89 5c 24 58 48";
constexpr uintptr_t kRvaRtManager = 0x8028d8;  // rend::RenderTargetManager* (what `end` frees with)

// --- d3d_rmdwin10_f.dll -------------------------------------------------------------------------
constexpr char kExpGetAutoBackBuffer[] = "?getAutoBackBuffer@DeviceUtil@d3d@@SAPEAVNativeTexture@2@XZ";
constexpr char kSigGetAutoBackBuffer[] = "48 8b 0d ?? ?? ?? ?? 48 63 81 38 01 00 00 48 8b 84 c1 40 01 00 00 c3";
constexpr char kExpSubmitGraphics[] = "?submitGraphicsCommandList@DeviceUtilDX12@d3d@@SAXXZ";
constexpr char kSigSubmitGraphics[] = "8b 05 ?? ?? ?? ?? 65 48 8b 0c 25 58 00 00 00 ba 18 00 00 00 48 8b 0c c1";
constexpr char kExpGetNativeTexture[] = "?getNativeTexture@NativeTextureContainer@d3d@@QEBAPEAVNativeTexture@2@XZ";

constexpr char kExpTexture2DInit[] =
    "??0Texture2DInit@d3d@@QEAA@HHHW4PixelFormat@1@HHPEBX_KW4MiscFlags@1@HMHV?$Vector4Template@M@m@@W4TileMode@1@@Z";

// Copies and clears recorded on the engine's own current command list (the renderer thread's
// context, TLS), with the engine's state tracking: in order with the passes that wrote the source.
constexpr char kExpCopyRegion[] =
    "?copyRegion@NativeTextureUtil@d3d@@SAXPEAVNativeTexture@2@HHAEBV?$Vector3Template@H@m@@0HH1PEBV45@@Z";
constexpr char kSigCopyRegion[] = "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8d 6c 24 98 48 81 ec 68 01 00";
constexpr char kExpClearRT[] =
    "?clearRenderTarget@DeviceUtil@d3d@@SAXAEAVDeviceState@2@PEAVNativeTexture@2@AEBV?$Vector4Template@M@m@@@Z";
constexpr char kSigClearRT[] = "40 57 41 56 41 57 48 83 ec 40 48 c7 44 24 30 fe ff ff ff 48 89 5c 24 60";
constexpr char kExpRetrieveDS[] = "?retrieveForThread@DeviceState@d3d@@SAAEAV12@XZ";
constexpr char kSigRetrieveDS[] = "8b 15 ?? ?? ?? ?? 65 48 8b 0c 25 58 00 00 00";
constexpr char kSigTexture2DInit[] = "f3 0f 10 44 24 60 33 c0 89 41 08 0f b6 44 24 28 88 41 14 8b 44 24 30 89";
constexpr char kSigGetNativeTexture[] = "48 8b 41 08 c3";
constexpr size_t kTexture2DInitSize = 0x60;

// DLSS (re/dlss.md 1): DLSSData* global; the current feature's handle and parameter block, which
// doAntiAliasing evaluates (it sets every input on the block each call).
constexpr uintptr_t kRvaDlssData = 0x111be0;
constexpr size_t kDlssRenderW = 0x0c;  // u32 render w, h, output w, h
constexpr size_t kDlssHandle = 0x28;
constexpr size_t kDlssParams = 0x30;
constexpr char kExpDLSSEnabled[] = "?DLSSEnabled@RenderOptions@rend@@2_NA";  // renderer

// Visibility results (renderer+0x1b0300, from the "Scene update" pass): each game-thread renderWorld
// launches one culling query; its job fills a buffer and pushes it to the ready queues; the render
// pops one ready buffer per queue (blocking), uses it and pushes it back to the free queues. Pools
// of 2. A second render in the same frame would wait forever, so the right eye reuses the left
// eye's results: during the left eye the free-queue pushes are held back, during the right eye the
// ready-queue pops return the left eye's buffers.
constexpr uintptr_t kRvaQueuePop = 0x1b3f20;   // T pop(queue*) (WaitForSingleObject on its semaphore)
constexpr uintptr_t kRvaQueuePush = 0x1b3fe0;  // void push(queue*, T)
constexpr char kSigQueuePop[] = "48 89 5c 24 08 48 89 74 24 10 57 48 83 ec 20 48 8b f9 bb 01 00 00 00 8b";
constexpr char kSigQueuePush[] = "40 57 48 83 ec 30 48 c7 44 24 20 fe ff ff ff 48 89 5c 24 48 48 89 6c 24";
constexpr uintptr_t kRvaVisReady[2] = {0x129bd70, 0x129bd10};
constexpr uintptr_t kRvaVisFree[2] = {0x129bd40, 0x129bce0};

// Per-eye histories (re/stereo.md section 3; docs/stereo.md): swapped for the right eye.
// T3 render-graph persisted resources (lastcolorbuffer, previouslineardepth, ...): the executor
// (0xa09a0) claims entries of this vector by name hash at the start of a graph run, frees the rest
// and appends this run's persisted resources. {ptr, u32 count, u32 capacity|flags}, entries 0xa0
// bytes: +0x88 type (0 Texture2D, 1 DepthBuffer), +0x90 the resource.
constexpr uintptr_t kRvaGraphPrev = 0x910c20;
constexpr size_t kGraphEntry = 0xa0, kGraphEntryType = 0x88, kGraphEntryRes = 0x90;
// T5 RT denoiser (DeferredLightFiltering) histories: Texture2D* globals, allocated when null and
// re-created on a size change by their filters (0x14c90, 0x15bd0, 0x17050).
// The RT reflection filter (filterSpecularNoise 0x15bd0) keeps two: 0x912b18 (the temporal pass's
// history) and 0x912b20 (its output history, written after the spatial passes, read by the next
// render; allocated and freed together with 0x912b18: static). history_mask bit 32 swaps 0x912b20.
constexpr size_t kDlfHistories = 5;
constexpr uintptr_t kRvaDlfHistory[kDlfHistories] = {0x90eac8, 0x912b18, 0x910b98, 0x90eb98, 0x912b20};
// T4 rend::SSAO (r+0xd8) ping-pong pairs indexed by counter & 1 (SSR / AO / sun shadow temporal),
// allocated when null by its prepare (slot 1).
constexpr size_t kSsaoPairs[6] = {0x100, 0x108, 0x118, 0x120, 0x130, 0x138};
// T6 volumetric light (r+0xe8): +0x18 temporal history (falls back to +0x10 when null), +0x40..+0x70
// the camera it remembers (weights the blend).
constexpr size_t kRVolumetric = 0xe8;
constexpr size_t kVolHistory = 0x18, kVolCamera = 0x40, kVolCameraSize = 0x30;
// T2 TAA ("SSAA", r+0x138): 3-slot ring of {jittered m::View 0x2e0, Texture2D* at +0x2e0}.
constexpr size_t kSsaaRing = 0x40, kSsaaSlot = 0x2f0, kSsaaSlots = 3, kSsaaTex = 0x2e0;
constexpr char kExpRtFreeDepth[] = "?free@RenderTargetManager@rend@@QEAAXPEAVDepthBuffer@d3d@@@Z";
constexpr char kSigRtFreeDepth[] = "40 56 57 41 56 48 83 ec 30 48 c7 44 24 20 fe ff";

// Video textures (Bink in the world): each world render's "VideoDecoderStart" pass (renderer+0x123880)
// calls VideoStreamerFactory::updateAsync, which starts a decode job (handle at this+0x50) that the
// "Scene update" pass waits for (renderer+0x124215). Once per update: the right eye skips it (its
// wait then finds the left eye's finished job) - a second decode per update hung the renderer after a
// teleport into the Executive sector (live, r20: renderer thread in JobSystem::waitJob).
constexpr char kExpVideoUpdateAsync[] = "?updateAsync@VideoStreamerFactory@rend@@QEAAXMW4VideoLocation@2@@Z";
constexpr char kSigVideoUpdateAsync[] = "48 89 5c 24 10 57 48 83 ec 50 80 3d ?? ?? ?? ?? 00 41 0f b6 d8";

// GfxGraph effects (the game's node-graph VFX: Polaris, the Hiss' damage / cloak / shield effects;
// static, renderer). rend::GfxGraphEngine (instance renderer+0x802898) keeps
// +0x38 GfxGraphRenderToPassCommand* and +0x48 GfxGraphDrawParticleSystemsCommand* (r::Vector
// {data, u32 count, u32 capacity | flags}), appended on the render thread by the RendererThreading
// messages of GfxGraphEngineWrapper::addRenderToPassCommand / addDrawParticleSystemsCommand
// (handlers 0x4a6f0 / 0x4a7c0, gated by "GfxGraph:Enable"). The world render's pass 0x13e3a0 (after
// the particles' light pass, the flares and the decals) calls 0x4eb80 ("GfxGraphEngine" event): it
// records every render-to-pass command into the render's draw buckets and every particle-system
// command through ParticleEngine::recordCommandsForLightPass, and then sets both counts to 0.
// The commands live in the game-frame allocator until frame end (FrameEndReleaser), so they are
// still valid for the second render of the update.
constexpr uintptr_t kRvaGfxGraphRecord = 0x4eb80;
constexpr char kSigGfxGraphRecord[] = "48 8b c4 48 89 50 10 55 41 56 41 57 48 8d 68 a8 48 81 ec 40 01 00 00";
constexpr size_t kGfxEngRenderToPass = 0x38;  // then +0x48, 0x10 bytes apart
// The engine's +0x28 list holds every GfxGraph command of the update (GfxGraphEngineWrapper::addCommand
// and addRenderToPassCommand). renderer+0x4ea30, called at the end of each world render's graph build
// (renderer+0x1237c9, the build 0x11ee20), declares each into that graph (vtable +8, OrderSolver
// 0x180910ba0), calls its clear (+0x18: resets the handles' last-writer links) and sets the count to 0;
// then it starts the video streamers of GfxGraph textures referenced this frame.
constexpr uintptr_t kRvaGfxGraphDeclare = 0x4ea30;
constexpr char kSigGfxGraphDeclare[] = "48 89 54 24 10 57 48 83 ec 30 48 c7 44 24 20 fe ff ff ff 48 89 5c 24 40";
constexpr size_t kGfxEngAll = 0x28;
// GfxGraphDrawParticleSystemsCommand (vtable renderer+0x623070): its own draw bucket at +0x120, filled by
// the GfxGraph pass (0x4eb80: ParticleEngine::recordCommandsForLightPass with the command's bucket in
// the context) and drawn by its execute (0x4e560 -> 0x112e50, which walks the bucket's list). Entries
// come from the recording thread's per-render scratch (0x112af0). Buckets are reset like the render
// context's (renderer+0x125300): head +8, count +0x10, +0x20 to 0, map (+0) through 0x1126c0.
constexpr uintptr_t kGfxVtDrawParticles = 0x623070;
// GfxGraphDrawObjectsCommand: Matrix4x3 world-to-view at +0x530 (from its constructor's argument, set by
// the game thread's GfxGraph nodes), composed with each object's local-to-world (+0x70) into
// g_mLocalToView by its execute (0x4d8f0, static).
constexpr size_t kGfxDrawObjView = 0x530;
constexpr size_t kGfxPsBucket = 0x120;
constexpr size_t kGfxRttDraws = 0x110, kGfxRttDrawCount = 0x220;  // GfxGraphRenderToTextureCommand
constexpr uintptr_t kRvaBucketMapReset = 0x1126c0;
constexpr char kSigBucketMapReset[] = "48 83 79 38 00 76 29 48 8b 41 58";

// "rtd:Enable async build" (bool tweakable value, default 1; static: its constructor renderer+0x3b10):
// with it the RT acceleration structures are built by a job on the async compute queue, fenced
// against graphics once per world render (renderer+0x124158 signal, +0xa8bf0 compute wait / signal,
// +0x128f40 graphics wait). In sbs, with two renders per update, a level change deadlocked the three
// queues (graphics, async compute, d3d's async raytrace queue; live fence trace, docs/stereo.md), so
// it is switched off while side-by-side pairs render (the build then runs on the graphics queue).
constexpr uintptr_t kRvaRtAsyncBuild = 0x90e540;
// "rtd:Enable async upload" (d3d bool tweakable value, default 1; constructor d3d+0x1760), latched into
// d3d+0xf6c30 by `present` (d3d+0x31b50): with it each DeviceUtilRaytracing::raytrace (d3d+0x353e0)
// records its ray work as a job that a second queue executes later (flushed at 16 pending calls or at
// Present, d3d+0x31340), lagging graphics by two calls, and graphics waits for it. Off, the rays run
// on the graphics queue in order. In sbs the right eye rebuilds the acceleration structures while
// the left eye's rays may still be in flight on that queue; at a level change that hung the GPU
// (rayblaster r8: the ray queue's list never completed, with the async build already off).
constexpr uintptr_t kRvaD3DRtAsyncUpload = 0x136d20;

// High-res screenshot tiles (slots 25-27 write them): the renderer's own "several renders per
// update" guards compare them. The graph's Cleanup pass refreshes object previous transforms
// (Object +0xa0 <- +0x70, renderer+0x134040) and the ambient probe updates (renderer+0x211c80) only
// when index == count, i.e. on the last tile. Stereo holds the count only inside the Cleanup pass
// (eyestate.cpp tile_hold 2): holding it for the whole left render also skipped the probe there.
constexpr uintptr_t kRvaTileIndex = 0x802978;
constexpr uintptr_t kRvaTileCount = 0x80297c;

// d3d::NativeTexture (static: NativeTextureUtil::copyRegion 0x3d190).
constexpr size_t kNtWidth = 0x0, kNtHeight = 0x4;
constexpr size_t kNtPixelFormat = 0x24;    // i8 d3d::PixelFormat
constexpr size_t kNtFlags = 0x38;          // u8: & 0x30 = not state-tracked
constexpr size_t kNtStatePtr = 0x50;       // state block pointer, else the block at +0x40
constexpr size_t kNtStateInline = 0x40;
constexpr size_t kNtStateOff = 0x20;       // D3D12_RESOURCE_STATES in the block
constexpr size_t kNtUntracked = 0x68;      // u8
constexpr size_t kNtResource = 0x88;       // ID3D12Resource*

}  // namespace th::stereo
