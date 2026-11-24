// Stereoscopic 3D for Control (docs/stereo.md, re/stereo.md).
//
// The lever: RendererX86 vtable slot 15 (renderer+0x11daf0) receives the game's camera as one
// m::View by value (the renderWorld packet) and copies it into the main rend::View before rendering
// the frame graph. Shadow, probe and reflection views are separate objects. So an eye is a copy of
// that m::View with its position moved along its own right axis (and, for convergence, an off-axis
// term in its projection), passed to the original slot 15. Everything the renderer derives from the
// main view (culling aside, which ran on the game thread with the centre camera) follows.
//
// Modes ([stereo] mode):
//   off        pass-through
//   left/right every frame rendered from one eye (mono-frame bias; measurement)
//   sbs        both eyes inside one engine frame: slot 15 runs twice (left, right), each eye's final
//              image is copied out, the HUD (slot 17) is drawn once into each half of a 2W back
//              buffer, then the engine's own end/present run once
//   auto       sbs while the game's monitor is a HypXRland stereo output, off elsewhere
#include "features/stereo/stereo.h"

#include <windows.h>
#include <dxgi.h>
#include <d3d12.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/hook.h"
#include "core/hyprland.h"
#include "core/lab.h"
#include "core/log.h"
#include "core/module.h"
#include "core/present.h"
#include "core/quiver.h"
#include "features/fg/fg.h"
#include "features/stereo/engine.h"
#include "features/stereo/eyestate.h"
#include "features/stereo/gpu.h"
#include "features/stereo/ui.h"
#include "features/stereo/window.h"

namespace th {
namespace stereo {
namespace {

enum class Mode { Off, Left, Right, Sbs, Auto };

struct Params {
  Mode mode = Mode::Off;
  double separation_mm = 32.5;  // per eye
  double convergence_m = 0.0;   // 0 = parallel
  bool swap = false;
  bool hud_both = true;       // draw the HUD into each half (else the left half only)
  int conv_shift = 1;         // full sbs convergence: 1 = shift the finished eye images, 0 = projection
};
std::mutex g_pmu;
Params g_p;
std::atomic<bool> g_packed{false};   // the output is side by side
std::atomic<int> g_pack{0};          // 0 none, 1 full/sbs (2W swapchain, eyes 1:1), 2 hsbs (W, squeezed)
std::atomic<bool> g_disabled{false};  // a start-up condition switched stereo off (signatures)
bool g_fg_linked = false;             // frame generation evaluates per eye (fg_stereo_link, start-up)

Params params() {
  Params p;
  {
    std::lock_guard lk(g_pmu);
    p = g_p;
  }
  if (p.mode == Mode::Auto) p.mode = g_packed.load() && hypr_display_stereo() == 1 ? Mode::Sbs : Mode::Off;
  if (g_disabled.load()) p.mode = Mode::Off;
  return p;
}

Mode parse_mode(const std::string& s) {
  if (s == "left") return Mode::Left;
  if (s == "right") return Mode::Right;
  if (s == "sbs") return Mode::Sbs;
  if (s == "auto") return Mode::Auto;
  return Mode::Off;
}
const char* mode_name(Mode m) {
  switch (m) {
    case Mode::Left: return "left";
    case Mode::Right: return "right";
    case Mode::Sbs: return "sbs";
    case Mode::Auto: return "auto";
    default: return "off";
  }
}
bool truthy(const std::string& v) { return v == "1" || v == "true" || v == "on" || v == "yes"; }

template <typename T>
T& at(void* base, size_t off) {
  return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + off);
}
Image g_rimg;  // renderer_rmdwin10_f.dll
template <typename T>
T& rglob(uintptr_t rva) {
  return *reinterpret_cast<T*>(g_rimg.at(rva));
}

std::string where(uintptr_t a);
// --- hooks / engine functions ------------------------------------------------------------------
using RenderWorld_t = void(__fastcall*)(void* r, const void* view, void* a3, void* a4, void* a5);
using RenderHUD_t = void(__fastcall*)(void* r);
using SetM43_t = void(__fastcall*)(void* view, const double* m);
RenderWorld_t o_renderWorld;
RenderHUD_t o_renderHUD;
RenderHUD_t o_end;
using Tex2DInit_t = void(__fastcall*)(void* self, int w, int h, int mips, int fmt, int usage, int a6, const void* data,
                                      uint64_t size, int misc, int a10, float a11, int a12, const float* clear, int tile);
using Alloc_t = void*(__fastcall*)(void* mgr, const void* init, const char* name);
using Free_t = void(__fastcall*)(void* mgr, void* tex);
Tex2DInit_t g_tex2d_init;
using CopyRegion_t = void(__cdecl*)(void* dst, int dslice, int dmip, const int* dpos, void* src, int sslice, int smip,
                                    const int* spos, const int* size);
using ClearRT_t = void(__cdecl*)(void* ds, void* nt, const float* color);
using RetrieveDS_t = void*(__cdecl*)();
CopyRegion_t g_copy_region;
ClearRT_t g_clear_rt;
RetrieveDS_t g_retrieve_ds;
Alloc_t g_rt_alloc;
Free_t g_rt_free;
SetM43_t g_setViewToWorld, g_setViewToClip;

// --- per-frame state (renderer thread) -----------------------------------------------------------
std::atomic<int> g_census{0};
bool census_on() { return g_census.load(std::memory_order_relaxed) > 0; }
void census(const char* fmt, ...) __attribute__((format(gnu_printf, 1, 2)));
void census(const char* fmt, ...) {
  char buf[640];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  TH_LOG("census f=%llu tid=%lu %s", (unsigned long long)present_frame(), GetCurrentThreadId(), buf);
}

int g_worlds_this_frame = 0;  // slot 15 calls since the last Present
bool g_pair = false;          // both eyes of this frame rendered (the left in eye texture 0)
bool g_composed = false;      // both eyes + HUD are in the side-by-side texture
bool g_end_composed = false;  // `end` blitted the side-by-side texture this frame
int g_pair_w = 0, g_pair_h = 0;
int g_shift[2] = {0, 0};      // convergence image shift per eye (px), sbs with conv_shift
std::atomic<int> g_eye_now{0};
std::atomic<uint64_t> g_n_worlds{0}, g_n_pairs{0}, g_n_biased{0}, g_n_hud2{0}, g_n_dup{0}, g_n_mono_fallback{0};
std::atomic<uint64_t> g_n_created_color{0}, g_n_mono_dup{0}, g_n_pause_sbs{0};
void* g_renderer = nullptr;
IDXGISwapChain* g_sc = nullptr;
double g_last_m00 = 0;
double g_pan_rad = 0;
std::atomic<double> g_pan_rate{0};  // stereo.test_pan, degrees per update (measurement lever)
uint64_t g_pan_frame = 0;

bool packed_now() {
  int pack = g_pack.load();
  if (pack == 1) return g_sc && present_width_scale(g_sc) > 1;
  return pack == 2;
}

// The side-by-side texture `end` blits: 2W x H, the colour target's format, from the engine's own
// render-target pool (so `end`'s blit can sample it), kept until the size or format changes.
void* g_sbs = nullptr;
int g_sbs_w = 0, g_sbs_h = 0, g_sbs_fmt = -1;
void* sbs_texture(void* r, void* cc) {
  if (!cc || !g_tex2d_init || !g_rt_alloc) return nullptr;
  int w = rglob<int>(kRvaOutputSize), h = rglob<int>(kRvaOutputSize + 4);
  int fmt = at<int8_t>(cc, kNtPixelFormat);
  if (g_sbs && g_sbs_w == 2 * w && g_sbs_h == h && g_sbs_fmt == fmt) return g_sbs;
  void* mgr = at<void*>(r, kRRtManager);
  if (!mgr) return nullptr;
  if (g_sbs && g_rt_free) g_rt_free(*reinterpret_cast<void**>(g_rimg.at(kRvaRtManager)), g_sbs);
  g_sbs = nullptr;
  alignas(16) uint8_t init[kTexture2DInitSize + 0x20] = {};
  alignas(16) float clear[4] = {0, 0, 0, 0};
  // As renderHUD makes the colour target: (w, h, 1 mip, format, usage 5, ..., 0.5f, 1, clear, -1).
  g_tex2d_init(init, 2 * w, h, 1, fmt, 5, 0, nullptr, 0, 0, 0, 0.5f, 1, clear, -1);
  g_sbs = g_rt_alloc(mgr, init, "Threshold::stereoSideBySide");
  g_sbs_w = 2 * w, g_sbs_h = h, g_sbs_fmt = fmt;
  TH_LOG("stereo: side-by-side texture %dx%d format %d: %p (%s)", 2 * w, h, fmt, g_sbs,
         gpu_describe(gpu_native(g_sbs)).c_str());
  return g_sbs;
}

// Frames without a pair (menus, videos, loads) are duplicated into a second 2W texture of the same
// kind, so the side-by-side texture keeps the last pair (the pause background takes both eyes from
// it, ui.cpp: a mono frame between the last pair and the capture had made it the mono image).
void* g_mono = nullptr;
int g_mono_w = 0, g_mono_h = 0, g_mono_fmt = -1;
void* mono_texture(void* r, void* cc) {
  if (!cc || !g_tex2d_init || !g_rt_alloc) return nullptr;
  int w = rglob<int>(kRvaOutputSize), h = rglob<int>(kRvaOutputSize + 4);
  int fmt = at<int8_t>(cc, kNtPixelFormat);
  if (g_mono && g_mono_w == 2 * w && g_mono_h == h && g_mono_fmt == fmt) return g_mono;
  void* mgr = at<void*>(r, kRRtManager);
  if (!mgr) return nullptr;
  if (g_mono && g_rt_free) g_rt_free(*reinterpret_cast<void**>(g_rimg.at(kRvaRtManager)), g_mono);
  g_mono = nullptr;
  alignas(16) uint8_t init[kTexture2DInitSize + 0x20] = {};
  alignas(16) float clear[4] = {0, 0, 0, 0};
  g_tex2d_init(init, 2 * w, h, 1, fmt, 5, 0, nullptr, 0, 0, 0, 0.5f, 1, clear, -1);
  g_mono = g_rt_alloc(mgr, init, "Threshold::stereoMonoDuplicate");
  g_mono_w = 2 * w, g_mono_h = h, g_mono_fmt = fmt;
  TH_LOG("stereo: mono duplicate texture %dx%d format %d: %p", 2 * w, h, fmt, g_mono);
  return g_mono;
}

// --- visibility results reused by the right eye (engine.h kRvaVisReady) ---------------------------
using Pop_t = void*(__fastcall*)(void* q);
using Push_t = void(__fastcall*)(void* q, void* v);
Pop_t o_pop;
Push_t o_push;
std::atomic<int> g_vis_phase{0};     // 1 left eye, 2 right eye (renderer thread)
std::atomic<DWORD> g_vis_tid{0};
void* g_vis_kept[2] = {};
bool g_vis_held_now[2] = {}, g_vis_returned[2] = {};
std::atomic<uint64_t> g_n_vis_reuse{0}, g_n_vis_held{0}, g_n_vis_miss{0};
int vis_ready_index(void* q) {
  for (int i = 0; i < 2; i++)
    if (q == (void*)g_rimg.at(kRvaVisReady[i])) return i;
  return -1;
}
int vis_free_index(void* q) {
  for (int i = 0; i < 2; i++)
    if (q == (void*)g_rimg.at(kRvaVisFree[i])) return i;
  return -1;
}
void* __fastcall hk_pop(void* q) {
  int ph = g_vis_phase.load(std::memory_order_relaxed);
  if (ph == 0 || GetCurrentThreadId() != g_vis_tid.load(std::memory_order_relaxed)) return o_pop(q);
  int i = vis_ready_index(q);
  if (i < 0) return o_pop(q);
  if (ph == 2 && g_vis_kept[i]) {
    g_n_vis_reuse++;
    return g_vis_kept[i];
  }
  void* v = o_pop(q);
  if (ph == 1) g_vis_kept[i] = v;
  return v;
}
void __fastcall hk_push(void* q, void* v) {
  int ph = g_vis_phase.load(std::memory_order_relaxed);
  if (ph != 0 && GetCurrentThreadId() == g_vis_tid.load()) {
    int i = vis_free_index(q);
    if (i >= 0 && v == g_vis_kept[i]) {
      if (ph == 1) {  // the right eye returns it
        g_vis_held_now[i] = true;
        g_n_vis_held++;
        return;
      }
      g_vis_returned[i] = true;
    }
  }
  o_push(q, v);
}
// After the right eye: a buffer the left eye held back that the right eye did not return goes back
// now, so the pool of 2 never shrinks.
void vis_end() {
  g_vis_phase = 0;
  for (int i = 0; i < 2; i++) {
    if (g_vis_held_now[i] && !g_vis_returned[i] && g_vis_kept[i]) {
      o_push((void*)g_rimg.at(kRvaVisFree[i]), g_vis_kept[i]);
      g_n_vis_miss++;
    }
    g_vis_kept[i] = nullptr;
    g_vis_held_now[i] = g_vis_returned[i] = false;
  }
}

// Engine-list copies (engine.h kExpCopyRegion): nt are NativeTexture*.
void eng_copy(void* dst, int dx, int dy, void* src, int sx, int sy, int w, int h) {
  if (!dst || !src || w <= 0 || h <= 0) return;
  int dp[3] = {dx, dy, 0}, sp[3] = {sx, sy, 0}, sz[3] = {w, h, 1};
  g_copy_region(dst, 0, 0, dp, src, 0, 0, sp, sz);
}
// The eye textures (W x H, the colour target's format) from the engine's pool, like g_sbs.
void* g_eye_tex[2] = {};
int g_eye_w = 0, g_eye_h = 0, g_eye_fmt = -1;
void* eye_texture(void* r, int eye, void* cc) {
  if (!cc) return nullptr;
  int w = rglob<int>(kRvaOutputSize), h = rglob<int>(kRvaOutputSize + 4);
  int fmt = at<int8_t>(cc, kNtPixelFormat);
  if (g_eye_tex[eye] && g_eye_w == w && g_eye_h == h && g_eye_fmt == fmt) return gpu_native(g_eye_tex[eye]);
  void* mgr = at<void*>(r, kRRtManager);
  if (!mgr) return nullptr;
  if (g_eye_w != w || g_eye_h != h || g_eye_fmt != fmt) {
    for (auto& t : g_eye_tex)
      if (t) g_rt_free(*reinterpret_cast<void**>(g_rimg.at(kRvaRtManager)), t), t = nullptr;
    g_eye_w = w, g_eye_h = h, g_eye_fmt = fmt;
  }
  alignas(16) uint8_t init[kTexture2DInitSize + 0x20] = {};
  alignas(16) float clear[4] = {0, 0, 0, 0};
  g_tex2d_init(init, w, h, 1, fmt, 5, 0, nullptr, 0, 0, 0, 0.5f, 1, clear, -1);
  g_eye_tex[eye] = g_rt_alloc(mgr, init, eye ? "Threshold::stereoEyeR" : "Threshold::stereoEyeL");
  TH_LOG("stereo: eye %d texture %dx%d format %d: %p", eye, w, h, fmt, g_eye_tex[eye]);
  return gpu_native(g_eye_tex[eye]);
}

// --- rand() replay ---------------------------------------------------------------------------------
// The renderer draws CRT rand() per render (the tonemap noise offset g_vTonemapNoiseOffset,
// renderer+0x22cf50, re-drawn every render while its update rate is <= 0): the right eye replays the
// left eye's values in order, so the generator advances once per update and both eyes get the same
// noise. Only on the renderer thread during an sbs pair (the UCRT generator state is per thread).
using Rand_t = int(__cdecl*)();
Rand_t o_rand;
std::vector<int> g_rand_vals;
size_t g_rand_pos = 0;
std::atomic<uint64_t> g_n_rand_rec{0}, g_n_rand_replay{0};
std::atomic<bool> g_rand_replay{true};
int __cdecl hk_rand() {
  int ph = g_vis_phase.load(std::memory_order_relaxed);
  if (ph == 0 || !g_rand_replay.load(std::memory_order_relaxed) ||
      GetCurrentThreadId() != g_vis_tid.load(std::memory_order_relaxed))
    return o_rand();
  if (ph == 1) {
    int v = o_rand();
    g_rand_vals.push_back(v);
    g_n_rand_rec++;
    return v;
  }
  if (g_rand_pos < g_rand_vals.size()) {
    g_n_rand_replay++;
    return g_rand_vals[g_rand_pos++];
  }
  return o_rand();
}

// One video decode per update (engine.h kExpVideoUpdateAsync): the right eye's call is skipped.
using VideoUpdate_t = void(__fastcall*)(void* self, float t, int where);
VideoUpdate_t o_videoUpdate;
std::atomic<uint64_t> g_n_video_skip{0};
std::atomic<bool> g_video_once{true};  // stereo.video_once (dev): 0 = the right eye decodes too
using VideoCount_t = int(__fastcall*)(const void* self);
VideoCount_t g_videoCount;              // VideoStreamerFactory::countActiveSources (diagnostics)
std::atomic<int> g_video_active{0};     // active sources at the last update
std::atomic<uint32_t> g_video_where{0};  // VideoLocation values seen (bit per value)
void __fastcall hk_videoUpdate(void* self, float t, int where) {
  if (g_videoCount) g_video_active = g_videoCount(self);
  g_video_where |= 1u << (where & 31);
  if (g_video_once.load(std::memory_order_relaxed) && g_vis_phase.load(std::memory_order_relaxed) == 2 && GetCurrentThreadId() == g_vis_tid.load()) {
    g_n_video_skip++;
    return;
  }
  o_videoUpdate(self, t, where);
}

// GfxGraph effects (engine.h kRvaGfxGraphRecord): the game's node-graph VFX (Polaris, the Hiss'
// damage / cloak / shield effects) reach the renderer as two lists the render thread fills once per
// update from RendererThreading messages, and each world render's GfxGraphEngine pass records them
// into its draw buckets and then empties them. The right eye found them empty: those effects were
// drawn in the left eye only. The left eye's lists are kept and put back for the right eye.
using GfxRecord_t = void(__fastcall*)(void* eng, void* a2, void* a3, void* a4);
GfxRecord_t o_gfxRecord;
std::atomic<bool> g_gfx_replay{true};  // stereo.gfxgraph_replay (dev, live): 0 = main's behaviour
std::vector<void*> g_gfx_saved[2];     // +0x38 render-to-pass commands, +0x48 particle-system commands
std::atomic<uint64_t> g_n_gfx_calls[3], g_n_gfx_cmds[3], g_n_gfx_ps[3], g_n_gfx_replays{0}, g_n_gfx_cap{0};
struct GfxList {  // r::Vector: {T* data, u32 count, u32 capacity | flags}
  void** data;
  uint32_t count, cap;
};
void __fastcall hk_gfxRecord(void* eng, void* a2, void* a3, void* a4) {
  int ph = g_vis_phase.load(std::memory_order_relaxed);
  if (ph < 0 || ph > 2) ph = 0;
  auto* lists = reinterpret_cast<GfxList*>(static_cast<uint8_t*>(eng) + kGfxEngRenderToPass);  // +0x38, +0x48
  if (ph == 2 && g_gfx_replay.load(std::memory_order_relaxed) && lists[0].count == 0 && lists[1].count == 0) {
    bool put = false;
    for (int i = 0; i < 2; i++) {
      auto& s = g_gfx_saved[i];
      if (s.empty()) continue;
      if ((lists[i].cap & 0x3fffffffu) < s.size() || !lists[i].data) {
        g_n_gfx_cap++;
        continue;
      }
      memcpy(lists[i].data, s.data(), s.size() * sizeof(void*));
      lists[i].count = uint32_t(s.size());
      put = true;
    }
    if (put) g_n_gfx_replays++;
  }
  g_n_gfx_calls[ph]++;
  g_n_gfx_cmds[ph] += lists[0].count;
  g_n_gfx_ps[ph] += lists[1].count;
  for (int i = 0; i < 2; i++) {
    if (ph == 1)
      g_gfx_saved[i].assign(lists[i].data, lists[i].data + lists[i].count);
    else
      g_gfx_saved[i].clear();
  }
  o_gfxRecord(eng, a2, a3, a4);
}

// The other half of the GfxGraph frame (engine.h kRvaGfxGraphDeclare): every GfxGraph command of the
// update (+0x28: resource alloc/free, clears, render-to-texture, compute, swaps, render-to-pass) is
// declared into the world render's graph at the end of its build, then the list is emptied. The right
// eye's graph got none of the offscreen work, so effects composed from a GfxGraph render texture
// (the Launch target outline, live) had nothing to sample there. The left eye's list is declared
// again into the right eye's graph.
using GfxDeclare_t = void(__fastcall*)(void* eng, void* a2, void* a3, void* a4);
GfxDeclare_t o_gfxDeclare;
std::atomic<int> g_gfx_declare{1};  // stereo.gfxgraph_declare (dev, live): 0 off, 1 all, 2 without swaps/compute
std::vector<void*> g_gfx_decl_saved;
std::atomic<uint64_t> g_n_gfx_decl[3], g_n_gfx_decl_replays{0}, g_n_gfx_decl_dropped{0};
// command kinds by vtable RVA (static): counted in the left eye's list for `stereo status`
constexpr uintptr_t kGfxVt[] = {0x6231d0, 0x6231f8, 0x623248, 0x623220, 0x6231a8, 0x623180,
                                0x623158, 0x623130, 0x6230d8, 0x623108};
constexpr const char* kGfxVtName[] = {"rtt", "rtp", "alloc", "free", "clear_rt", "clear_db",
                                      "swap_rt", "swap_db", "compute", "debug"};
constexpr int kGfxKinds = sizeof(kGfxVt) / sizeof(kGfxVt[0]);
std::atomic<uint64_t> g_n_gfx_kind[kGfxKinds + 1];
int gfx_kind(void* cmd) {
  uintptr_t vt = *reinterpret_cast<uintptr_t*>(cmd) - g_rimg.base;
  for (int i = 0; i < kGfxKinds; i++)
    if (vt == kGfxVt[i]) return i;
  return kGfxKinds;
}
void __fastcall hk_gfxDeclare(void* eng, void* a2, void* a3, void* a4) {
  int ph = g_vis_phase.load(std::memory_order_relaxed);
  if (ph < 0 || ph > 2) ph = 0;
  auto* list = reinterpret_cast<GfxList*>(static_cast<uint8_t*>(eng) + kGfxEngAll);
  int mode = g_gfx_declare.load(std::memory_order_relaxed);
  if (ph == 2 && mode && list->count == 0 && !g_gfx_decl_saved.empty()) {
    uint32_t n = 0;
    if ((list->cap & 0x3fffffffu) >= g_gfx_decl_saved.size() && list->data) {
      for (void* c : g_gfx_decl_saved) {
        int k = gfx_kind(c);
        if (mode == 2 && (k == 6 || k == 7 || k == 8)) {
          g_n_gfx_decl_dropped++;
          continue;
        }
        list->data[n++] = c;
      }
      list->count = n;
      g_n_gfx_decl_replays++;
    } else {
      g_n_gfx_cap++;
    }
  }
  g_n_gfx_decl[ph] += list->count;
  if (ph == 1) {
    g_gfx_decl_saved.assign(list->data, list->data + list->count);
    for (void* c : g_gfx_decl_saved) g_n_gfx_kind[gfx_kind(c)]++;
  } else {
    g_gfx_decl_saved.clear();
  }
  o_gfxDeclare(eng, a2, a3, a4);
}
std::string gfx_kinds_json() {
  std::string j = "{";
  for (int i = 0; i <= kGfxKinds; i++) {
    char b[64];
    snprintf(b, sizeof b, "%s\"%s\":%llu", i ? "," : "", i < kGfxKinds ? kGfxVtName[i] : "other",
             (unsigned long long)g_n_gfx_kind[i].load());
    j += b;
  }
  return j + "}";
}

// ---- fence trace (dev, stereo.fence_trace = 1 at start-up) ----------------------------------------
// The engine's GPU-side fence waits and signals (d3d exports; d3d::Fence = {u64 value, ID3D12Fence*}),
// with the caller, the eye phase and the fence's completed value, in a ring dumped by `stereo
// fences N`: a wait whose value the fence never reaches is a GPU wait-before-signal.
struct FenceEv {
  uint64_t frame;
  char kind;
  int8_t phase;
  uintptr_t ret;
  void* queue;
  void* fence;
  uint64_t value, completed;
};
std::mutex g_fmu;
FenceEv g_fev[4096];
uint64_t g_fev_n = 0;
uint64_t fence_completed(void* f) {
  return f ? reinterpret_cast<ID3D12Fence*>(f)->GetCompletedValue() : 0;
}
void fence_log(char kind, void* q, void* f, uint64_t v, void* ret) {
  FenceEv e{present_frame(), kind, int8_t(g_vis_phase.load(std::memory_order_relaxed)), reinterpret_cast<uintptr_t>(ret),
            q, f, v, fence_completed(f)};
  std::lock_guard lk(g_fmu);
  g_fev[g_fev_n++ % 4096] = e;
}
using FSig_t = void(__cdecl*)(void* q, const uint64_t* f);
using FWait3_t = void(__cdecl*)(void* q, const uint64_t* f, uint64_t v);
using FGpu_t = void(__cdecl*)(const uint64_t* f);
FSig_t o_fsignal, o_fwait;
FWait3_t o_fwait3;
FGpu_t o_fgpu, o_fgpu_nosub;
void __cdecl hk_fsignal(void* q, const uint64_t* f) {
  fence_log('S', q, reinterpret_cast<void*>(f[1]), f[0], __builtin_return_address(0));
  o_fsignal(q, f);
}
void __cdecl hk_fwait(void* q, const uint64_t* f) {
  fence_log('W', q, reinterpret_cast<void*>(f[1]), f[0], __builtin_return_address(0));
  o_fwait(q, f);
}
void __cdecl hk_fwait3(void* q, const uint64_t* f, uint64_t v) {
  fence_log('V', q, reinterpret_cast<void*>(f[1]), v, __builtin_return_address(0));
  o_fwait3(q, f, v);
}
void __cdecl hk_fgpu(const uint64_t* f) {
  if (f[1]) fence_log('G', nullptr, reinterpret_cast<void*>(f[1]), f[0], __builtin_return_address(0));
  o_fgpu(f);
}
void __cdecl hk_fgpu_nosub(const uint64_t* f) {
  if (f[1]) fence_log('g', nullptr, reinterpret_cast<void*>(f[1]), f[0], __builtin_return_address(0));
  o_fgpu_nosub(f);
}
// The queues themselves (vtable of the game's ID3D12CommandQueue, shared by every queue): Signal (14),
// Wait (15) and ExecuteCommandLists (10), so waits the engine issues outside d3d::FenceUtil show too.
using QSig_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);
using QExec_t = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
QSig_t o_qsignal, o_qwait;
QExec_t o_qexec;
HRESULT STDMETHODCALLTYPE hk_qsignal(ID3D12CommandQueue* q, ID3D12Fence* f, UINT64 v) {
  fence_log('s', q, f, v, __builtin_return_address(0));
  return o_qsignal(q, f, v);
}
HRESULT STDMETHODCALLTYPE hk_qwait(ID3D12CommandQueue* q, ID3D12Fence* f, UINT64 v) {
  fence_log('w', q, f, v, __builtin_return_address(0));
  return o_qwait(q, f, v);
}
void STDMETHODCALLTYPE hk_qexec(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* l) {
  fence_log('x', q, nullptr, n, __builtin_return_address(0));
  o_qexec(q, n, l);
}
bool g_qtrace = false;
void fence_trace_queue(IDXGISwapChain* sc) {
  if (g_qtrace) return;
  g_qtrace = true;
  auto* q = static_cast<ID3D12CommandQueue*>(present_queue(sc));
  if (!q) return;
  o_qsignal = reinterpret_cast<QSig_t>(hook_vtable(q, 14, (void*)hk_qsignal));
  o_qwait = reinterpret_cast<QSig_t>(hook_vtable(q, 15, (void*)hk_qwait));
  o_qexec = reinterpret_cast<QExec_t>(hook_vtable(q, 10, (void*)hk_qexec));
  TH_LOG("stereo: fence trace on the queue vtable too");
}
bool g_fence_trace = false;
void fence_trace_start() {
  g_fence_trace = true;
  struct {
    const char* exp;
    void* hk;
    void** o;
  } hs[] = {{"?signalFence@FenceUtil@d3d@@SAXAEAVCommandQueue@2@AEBUFence@2@@Z", (void*)hk_fsignal, (void**)&o_fsignal},
            {"?waitFence@FenceUtil@d3d@@SAXAEAVCommandQueue@2@AEBUFence@2@@Z", (void*)hk_fwait, (void**)&o_fwait},
            {"?waitFence@FenceUtil@d3d@@SAXAEAVCommandQueue@2@AEBUFence@2@_K@Z", (void*)hk_fwait3, (void**)&o_fwait3},
            {"?waitForFenceGPU@DeviceUtil@d3d@@SAXUFence@2@@Z", (void*)hk_fgpu, (void**)&o_fgpu},
            {"?waitForFenceGPUNoSubmit@DeviceUtil@d3d@@SAXUFence@2@@Z", (void*)hk_fgpu_nosub, (void**)&o_fgpu_nosub}};
  for (auto& h : hs)
    if (void* p = module_export(kD3D, h.exp, "stereo fence trace")) hook_fn(p, h.hk, h.o, h.exp);
  TH_LOG("stereo: fence trace on");
}
std::string where(uintptr_t a);
void fence_dump(int n) {
  std::lock_guard lk(g_fmu);
  uint64_t end = g_fev_n, start = end > uint64_t(n) ? end - n : 0;
  if (end - start > 4096) start = end - 4096;
  TH_LOG("fence dump: events %llu..%llu", (unsigned long long)start, (unsigned long long)end);
  for (uint64_t i = start; i < end; i++) {
    const FenceEv& e = g_fev[i % 4096];
    uint64_t now = fence_completed(e.fence);
    TH_LOG("fence %c f=%llu ph=%d q=%p fence=%p value=%llu completed(then)=%llu completed(now)=%llu %s from %s",
           e.kind, (unsigned long long)e.frame, e.phase, e.queue, e.fence, (unsigned long long)e.value,
           (unsigned long long)e.completed, (unsigned long long)now,
           (e.fence && strchr("WVGgw", e.kind) && now < e.value) ? "STUCK" : "", where(e.ret).c_str());
  }
}

// RT acceleration-structure builds on the graphics queue while pairs render (engine.h
// kRvaRtAsyncBuild). stereo.rt_async_build = 1 (main's default: verified in SDR) leaves the engine's
// setting alone; 0 holds the async build off while pairs render (the untested HDR level-change hang
// fix, branch wip/stereo-hdr-level-change).
std::atomic<bool> g_rt_async_keep{true};  // default: the engine's settings (holding them off did not fix the hang, r9)
int g_rt_async_saved = -1;  // the engine's value while we hold it off
int g_rt_upload_saved = -1;
uint8_t* d3d_rt_upload() {
  static uint8_t* p = nullptr;
  static bool done = false;
  if (!done) {
    done = true;
    Image d = module_image(kD3D);
    auto* dos = d ? reinterpret_cast<const IMAGE_DOS_HEADER*>(d.base) : nullptr;
    auto* nt = dos ? reinterpret_cast<const IMAGE_NT_HEADERS64*>(d.base + dos->e_lfanew) : nullptr;
    if (nt && nt->FileHeader.TimeDateStamp == kD3DStamp) p = reinterpret_cast<uint8_t*>(d.at(kRvaD3DRtAsyncUpload));
    else TH_LOG("stereo: d3d build differs: RT async upload left as is");
  }
  return p;
}
void hold_byte(uint8_t* v, int& saved, bool hold, const char* what) {
  if (!v) return;
  if (hold) {
    if (saved < 0) {
      saved = *v;
      if (*v) TH_LOG("stereo: %s off while side-by-side renders", what);
    }
    *v = 0;
  } else if (saved >= 0) {
    *v = uint8_t(saved);
    saved = -1;
  }
}
void rt_async_hold(bool hold) {
  if (!g_rimg) return;
  bool h = hold && !g_rt_async_keep.load();
  hold_byte(&rglob<uint8_t>(kRvaRtAsyncBuild), g_rt_async_saved, h, "RT async build");
  hold_byte(d3d_rt_upload(), g_rt_upload_saved, h, "RT async upload (async ray dispatch)");
}

// ---- level changes render one eye ----------------------------------------------------------------
// A level change (jump point: fast travel, teleports; checkpoint reload) in sbs hung the GPU on HDR
// runs: a page fault (Xid 13) or context-switch timeout a few seconds into the jump's streaming
// wait (docs/stereo.md, "Level-change hang"). The same jump with one render per update (mode
// left/off) is fine, so while the engine changes level the frame is rendered mono (`end` puts it in
// both halves: 2D in the glasses for the load) and pairs resume once it is done.
// - jump points: exe+0x4823c0 useJumpPoint(jp, reload, debug) sets bits 0/1 of jp+0xB8 while its
//   streaming wait / player hide is pending (static; nav.cpp reads the same bits);
// - episode reloads: coregame ServerGameSession::startReloadingEpisodeFromJumpPoint (export);
// - coregame::onIsLoading() (export, a std::function<bool()>), what the engine itself asks.
// Plus a 2 s tail after each. stereo.load_mono = 0 renders pairs through loads (for testing).
using UseJp_t = void(__fastcall*)(void* jp, bool reload, bool debug);
using Reload_t = void(__fastcall*)(void* ses, const void* jp, bool flag);
UseJp_t o_useJp;
Reload_t o_reload;
std::atomic<void*> g_lc_jp{nullptr};
std::atomic<uint64_t> g_lc_reload_tick{0}, g_lc_tail_until{0};
std::atomic<uint64_t> g_lc_begin{0};  // when the current level change began (0: none)
std::atomic<bool> g_load_mono{true};  // stereo.load_mono: with as_once 5 the right eye builds new BLASes, which hung at level changes (r41/r42); mono until streaming settles (r43)
std::atomic<uint64_t> g_n_load_mono{0}, g_n_level_changes{0};
uintptr_t g_onIsLoading = 0;
void __fastcall hk_useJp(void* jp, bool reload, bool debug) {
  g_lc_jp = jp;
  if (!g_lc_begin.load()) g_lc_begin = GetTickCount64();
  g_lc_tail_until = GetTickCount64() + 2000;
  g_n_level_changes++;
  o_useJp(jp, reload, debug);
}
void __fastcall hk_reload(void* ses, const void* jp, bool flag) {
  g_lc_reload_tick = GetTickCount64();
  if (!g_lc_begin.load()) g_lc_begin = GetTickCount64();
  g_lc_tail_until = GetTickCount64() + 5000;
  g_n_level_changes++;
  o_reload(ses, jp, flag);
}
bool readable_byte(const void* p) {
  MEMORY_BASIC_INFORMATION mi{};
  if (!VirtualQuery(p, &mi, sizeof mi) || mi.State != MEM_COMMIT) return false;
  return (mi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) != 0;
}
bool engine_loading() {
  if (!g_onIsLoading) return false;
  void* impl = *reinterpret_cast<void**>(g_onIsLoading + 0x38);  // MSVC std::function: impl at +0x38
  if (!impl) return false;
  auto vt = *reinterpret_cast<uintptr_t**>(impl);
  HMODULE m = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(vt[2]), &m))
    return false;
  return reinterpret_cast<bool (*)(void*)>(vt[2])(impl);
}
// Streaming activity: the RT acceleration-structure job (renderer+0xa89c0, once per world render)
// builds a BLAS for every object that came in since the last one (lists at scene+0x40/+0x50, counts
// +0x48/+0x58; static, docs/stereo.md). Observed only (pass-through), on the job's worker thread.
using AsJob_t = void(__fastcall*)(void** cap);
AsJob_t o_asJob;
std::atomic<uint64_t> g_last_blas_tick{0}, g_n_blas_builds{0};
std::atomic<double> g_blas_avg{8.0};
// The right eye's job also skips the BLAS build (+0x40/+0x50) and compaction (+0x60, count +0x68)
// lists: the left eye's job has built them for this update (the right eye's TLAS, refits and rays use
// those). A second build or compaction of the same new BLASes in one update looked like the
// level-change use-after-free (compaction frees the source BLAS). stereo.as_once = 0 runs them twice.
// stereo.as_once is a mask: 1 = builds + compaction (above); 2 = also the dynamic-BLAS refit list
// (+0x20, count +0x28: same pose for both eyes in one update, so the right eye's refit is redundant).
// 4 = the right eye's job still runs the build entries the left eye's job did not run in this pair
// (by entry pointer): objects enter the lists between the two eyes (~0.1 per pair in the world, live
// r41), and dropping their builds left BLASes unbuilt while the TLAS used them (the in-world RT fault,
// r20-r40). 8 = the same for compaction entries: off by default, running them hung the first teleport
// (r41: Xid 109).
std::atomic<int> g_as_once{21};  // 16 when its hooks are in (r50-r51), else 5 with load_mono
std::atomic<uint64_t> g_n_as_skipped{0}, g_n_refit_skipped{0}, g_n_as_late{0}, g_n_as_extra{0}, g_n_as_new{0}, g_n_as_new_built{0};
// The job runs on a worker thread, possibly after the render thread has moved on to the right eye, so
// the eye is the job's order within the pair (reset when the pair starts), not g_vis_phase at run time.
std::atomic<int> g_as_pair_jobs{2};
// The left eye's build (+0x40, +0x50) and compaction (+0x60) entries of this pair, sorted.
constexpr size_t kAsLists[3] = {0x40, 0x50, 0x60};
std::vector<void*> g_as_left[3];
// 16 = the right eye shares the left eye's acceleration structures (docs/stereo.md, "Shared acceleration
// structures"): its RT scene update (renderer+0xa81b0, which empties and refills the job's lists, the
// instance and per-geometry records, and steps the static-mesh BLAS state machine) is skipped and
// its render tick (RT+0xd8) undone, and its AS job runs with no instances (so nothing is refit, built
// or compacted) and without its TLAS build (so the left eye's TLAS, records and shader table stay).
// Objects the right eye would have listed are listed by the next left update through the engine's
// own path, so they appear one frame later in both eyes; nothing is marked built without being built.
std::atomic<uint64_t> g_n_rt_upd_skipped{0}, g_n_tlas_skipped{0}, g_n_left_builds{0}, g_n_left_compactions{0};
thread_local bool t_tlas_skip = false;
using RtUpdate_t = void(__fastcall*)(void** p);
RtUpdate_t o_rtUpdate;
void __fastcall hk_rtUpdate(void** p) {
  uint8_t* rt = p && *p ? static_cast<uint8_t*>(*p) : nullptr;
  if (rt && (g_as_once.load() & 16) && g_vis_phase.load() == 2) {
    --*reinterpret_cast<uint64_t*>(rt + 0xd8);  // the caller ticked it for this render: one tick per update
    g_n_rt_upd_skipped++;
    return;
  }
  o_rtUpdate(p);
}
using BuildTlas_t = void(__cdecl*)(int, void*, int, void*, void*, bool);
BuildTlas_t o_buildTlas;
void __cdecl hk_buildTlas(int flags, void* inst, int n, void* dst, void* scratch, bool update) {
  if (t_tlas_skip) {
    g_n_tlas_skipped++;
    return;
  }
  o_buildTlas(flags, inst, n, dst, scratch, update);
}
void __fastcall hk_asJob(void** cap) {
  uint8_t* sc = cap && *cap ? static_cast<uint8_t*>(*cap) : nullptr;
  if (!sc) return o_asJob(cap);
  auto cnt = [&](size_t off) -> uint32_t& { return *reinterpret_cast<uint32_t*>(sc + off); };
  auto arr = [&](size_t off) -> void**& { return *reinterpret_cast<void***>(sc + off); };
  uint32_t n = cnt(0x48) + cnt(0x58);
  g_n_blas_builds += n;
  // Dynamic objects get BLAS builds every render (~7 per render in the lobby, live r17), so streaming
  // is a burst above the usual rate: an average kept outside level changes, and "active" above 2x+8.
  double avg = g_blas_avg.load();
  if (!g_lc_begin.load()) g_blas_avg = avg * 0.98 + n * 0.02;
  else if (n > 2 * avg + 8) g_last_blas_tick = GetTickCount64();
  int k = g_as_pair_jobs.fetch_add(1);
  if (k == 0 && g_vis_phase.load() == 2) g_n_as_late++;  // the left eye's job ran after the right eye began
  if (k == 2) g_n_as_extra++;                           // a third job in one pair (not expected)
  int m = g_as_once.load();
  if ((m & 16) && o_rtUpdate && o_buildTlas) {
    if (k == 0) {
      g_n_left_builds += n;
      g_n_left_compactions += cnt(0x68);
    }
    if (k != 1) return o_asJob(cap);
    uint32_t inst = cnt(0x78);  // > 0 gates the refit/build/compaction/record work in the job
    cnt(0x78) = 0;
    t_tlas_skip = true;
    o_asJob(cap);
    t_tlas_skip = false;
    if (cnt(0x78) == 0) cnt(0x78) = inst;
    return;
  }
  if (k == 0 && (m & 1)) {  // the left eye's entries, for the right eye's comparison
    for (int l = 0; l < 3; l++) {
      auto& v = g_as_left[l];
      v.assign(arr(kAsLists[l]), arr(kAsLists[l]) + (arr(kAsLists[l]) ? cnt(kAsLists[l] + 8) : 0));
      std::sort(v.begin(), v.end());
    }
  }
  if (!(m & 2) && !(m & 1 && (n || cnt(0x68)))) return o_asJob(cap);
  if (!m || k != 1) return o_asJob(cap);
  // Build/compaction lists: the right eye's entries not run by the left eye (kept with bit 4).
  void** sarr[3] = {};
  uint32_t scnt[3] = {};
  std::vector<void*> keep[3];
  bool swapped[3] = {};
  if (m & 1)
    for (int l = 0; l < 3; l++) {
      size_t o = kAsLists[l];
      uint32_t c = cnt(o + 8);
      if (!c || !arr(o)) continue;
      for (uint32_t j = 0; j < c; j++)
        if (!std::binary_search(g_as_left[l].begin(), g_as_left[l].end(), arr(o)[j])) keep[l].push_back(arr(o)[j]);
      bool run_new = m & (l < 2 ? 4 : 8);
      g_n_as_new += keep[l].size();
      if (l < 2) g_n_as_new_built += keep[l].size();
      g_n_as_skipped += c - (run_new ? keep[l].size() : 0);
      sarr[l] = arr(o), scnt[l] = c, swapped[l] = true;
      if (run_new) {
        arr(o) = keep[l].data();
        cnt(o + 8) = uint32_t(keep[l].size());
      } else {
        cnt(o + 8) = 0;
      }
    }
  uint32_t refit = 0;
  if (m & 2) {
    refit = cnt(0x28);
    cnt(0x28) = 0;
    g_n_refit_skipped += refit;
  }
  o_asJob(cap);
  for (int l = 0; l < 3; l++) {
    if (!swapped[l]) continue;
    size_t o = kAsLists[l];
    arr(o) = sarr[l];
    if (cnt(o + 8) == 0 || cnt(o + 8) == keep[l].size()) cnt(o + 8) = scnt[l];  // the engine clears them later
  }
  if (refit && cnt(0x28) == 0) cnt(0x28) = refit;
}
// A level change keeps the frame mono until the engine is done (signals above) and then until no
// new geometry has streamed in (no BLAS builds) for kSettleMs, at most kChangeMaxMs after it began.
constexpr uint64_t kSettleMs = 3000, kChangeMaxMs = 90000;
bool as_shared() { return (g_as_once.load() & 16) && o_rtUpdate && o_buildTlas; }
bool level_changing() {
  if (!g_load_mono.load() || as_shared()) return false;  // with shared AS, level changes stay in 3D
  uint64_t now = GetTickCount64();
  bool busy = false;
  if (void* jp = g_lc_jp.load()) {
    auto* b = static_cast<uint8_t*>(jp) + 0xB8;
    if (readable_byte(b) && (*b & 3)) busy = true;
    else g_lc_jp = nullptr;
  }
  uint64_t rt = g_lc_reload_tick.load();
  if (rt && now - rt < 60000) busy = true;  // until the engine reports the load (below), at most 60 s
  if (engine_loading()) {
    busy = true;
    g_lc_reload_tick = 0;
  }
  if (busy) {
    if (!g_lc_begin.load()) g_lc_begin = now;
    g_lc_tail_until = now + kSettleMs;
    return true;
  }
  uint64_t b = g_lc_begin.load();
  if (!b) return false;
  if (now - b > kChangeMaxMs) {  // never settled: give up waiting
    g_lc_begin = 0;
    return false;
  }
  uint64_t quiet_from = std::max(g_lc_tail_until.load(), g_last_blas_tick.load() + kSettleMs);
  if (now < quiet_from) return true;
  TH_LOG("stereo: level change settled after %.1f s (BLAS builds so far %llu): 3D again", (now - b) / 1000.0,
         (unsigned long long)g_n_blas_builds.load());
  g_lc_begin = 0;
  return false;
}
void level_hooks_start() {
  const Image& exe = game_image();
  void* uj = reinterpret_cast<void*>(exe.at(0x4823c0));
  if (sig_match(uj, "48 8b c4 55 57 41 54 41 56 41 57 48 8d 6c 24 a0"))
    hook_fn(uj, (void*)hk_useJp, (void**)&o_useJp, "useJumpPoint (stereo: mono during level changes)");
  else
    TH_LOG("stereo: useJumpPoint signature mismatch: jumps render in 3D");
  constexpr wchar_t kCore[] = L"coregame_rmdwin10_f.dll";
  if (void* rl = module_export(kCore,
                               "?startReloadingEpisodeFromJumpPoint@ServerGameSession@coregame@@QEAAXAEBV?$InplaceString@$0EA@@r@@_N@Z",
                               "stereo");
      rl && sig_match(rl, "40 57 48 81 ec f0 00 00 00 48 c7 44 24 28 fe ff"))
    hook_fn(rl, (void*)hk_reload, (void**)&o_reload, "startReloadingEpisodeFromJumpPoint (stereo)");
  void* aj = reinterpret_cast<void*>(g_rimg.at(0xa89c0));
  if (sig_match(aj, "40 55 56 41 57 48 83 ec 50 48 8b f1 33 ed 8b cd 38 0d ?? ?? ?? ?? 0f 95"))
    hook_fn(aj, (void*)hk_asJob, (void**)&o_asJob, "RT acceleration-structure job (stereo: streaming activity)");
  void* ru = reinterpret_cast<void*>(g_rimg.at(0xa81b0));
  void* bt = module_export(kD3D, "?buildTopLevelAccelerationStructure@DeviceUtilRaytracing@d3d@@SAXW4RaytracingAccelerationBuildFlags@2@AEAVNativeBuffer@2@H11_N@Z", "stereo");
  if (o_asJob && bt && sig_match(ru, "4c 8b dc 55 53 57 41 57 49 8d ab 78 fe ff ff 48 81 ec 68 02 00 00")) {
    hook_fn(ru, (void*)hk_rtUpdate, (void**)&o_rtUpdate, "RT scene update (stereo: shared acceleration structures)");
    hook_fn(bt, (void*)hk_buildTlas, (void**)&o_buildTlas, "d3d buildTopLevelAccelerationStructure (stereo: shared acceleration structures)");
  } else {
    TH_LOG("stereo: RT scene update / TLAS build not matched: as_once 16 unavailable");
  }
  g_onIsLoading = reinterpret_cast<uintptr_t>(
      module_export(kCore, "?onIsLoading@coregame@@3V?$function@$$A6A_NXZ@std@@A", "stereo"));
}

double eye_offset(const Params& p, int eye) {
  if (eye == 0) return 0;
  double e = p.separation_mm / 1000.0 * eye;
  return p.swap ? -e : e;
}

// An eye's copy of the game's m::View: position += right * off (and the synthetic test pan), and
// for convergence the off-axis term viewToClip[2][0] += off * m00 / Zc (NDC x += z * that / w,
// w = z: a constant NDC shift that zeroes the disparity at Zc). Through m::View's own setters so
// world-to-view, world-to-clip and their inverses follow.
struct alignas(16) ViewBuf {
  uint8_t b[0x300];
};
void make_eye_view(const void* src, ViewBuf& dst, double off, double conv_ndc) {
  memcpy(dst.b, src, kMViewSize);
  double m[12];
  memcpy(m, dst.b + kMViewToWorld, sizeof m);
  if (g_pan_rad != 0) {  // yaw about world up (+y), rows right/up/forward
    double c = std::cos(g_pan_rad), s = std::sin(g_pan_rad);
    for (int r = 0; r < 3; r++) {
      double x = m[3 * r], z = m[3 * r + 2];
      m[3 * r] = c * x + s * z;
      m[3 * r + 2] = -s * x + c * z;
    }
  }
  m[9] += m[0] * off;
  m[10] += m[1] * off;
  m[11] += m[2] * off;
  g_setViewToWorld(dst.b, m);
  if (conv_ndc != 0) {
    double c[16];
    memcpy(c, dst.b + kMViewToClip, sizeof c);
    c[8] += conv_ndc;
    g_setViewToClip(dst.b, c);
  }
}

double conv_ndc_for(const Params& p, const void* view, double off) {
  if (off == 0 || p.convergence_m <= 0) return 0;
  const double* c = reinterpret_cast<const double*>(static_cast<const uint8_t*>(view) + kMViewToClip);
  double m00 = c[0];
  g_last_m00 = m00;
  if (!(m00 > 0.05 && m00 < 50)) return 0;
  return off * m00 / p.convergence_m;
}

void render_eye(void* r, const void* view, void* a3, void* a4, void* a5, const Params& p, int eye, bool proj_conv,
                int pair_eye) {
  double off = eye_offset(p, eye);
  double ndc = proj_conv ? conv_ndc_for(p, view, off) : 0;
  if (off == 0 && ndc == 0 && g_pan_rad == 0) {
    eyestate_enter(r, pair_eye);
    o_renderWorld(r, view, a3, a4, a5);
    eyestate_leave(r, pair_eye);
    return;
  }
  ViewBuf v;
  make_eye_view(view, v, off, ndc);
  g_eye_now = eye;
  eyestate_enter(r, pair_eye);
  o_renderWorld(r, v.b, a3, a4, a5);
  eyestate_leave(r, pair_eye);
  g_eye_now = 0;
  g_n_biased++;
}

void __fastcall hk_renderWorld(void* r, const void* view, void* a3, void* a4, void* a5) {
  g_renderer = r;
  ui_pair_rendered();  // any world render: the pause background (ui.cpp) ends
  g_n_worlds++;
  int nth = g_worlds_this_frame++;
  Params p = params();
  if (census_on()) {
    const double* m = reinterpret_cast<const double*>(static_cast<const uint8_t*>(view) + kMViewToWorld);
    const double* c = reinterpret_cast<const double*>(static_cast<const uint8_t*>(view) + kMViewToClip);
    census("renderWorld #%d r=%p view=%p pos=(%.3f %.3f %.3f) right=(%.3f %.3f %.3f) fwd=(%.3f %.3f %.3f) m00=%.4f "
           "m11=%.4f m20=%.5f zoom=%.4f fov=%.4f aspect=%.4f out=%dx%d vp=%d,%d render=%dx%d counter=%u",
           nth, r, view, m[9], m[10], m[11], m[0], m[1], m[2], m[6], m[7], m[8], c[0], c[5], c[8],
           *reinterpret_cast<const float*>(static_cast<const uint8_t*>(view) + 0x2d0),
           *reinterpret_cast<const float*>(static_cast<const uint8_t*>(view) + 0x2e0),
           *reinterpret_cast<const float*>(static_cast<const uint8_t*>(view) + 0x2e4),
           rglob<int>(kRvaOutputSize), rglob<int>(kRvaOutputSize + 4), rglob<int>(kRvaViewportOffset),
           rglob<int>(kRvaViewportOffset + 4), rglob<int>(kRvaRenderSize), rglob<int>(kRvaRenderSize + 4),
           rglob<uint32_t>(kRvaFrameCounter));
  }
  if (p.mode == Mode::Off) {
    rt_async_hold(false);
    eyestate_mono(r);
    o_renderWorld(r, view, a3, a4, a5);
    return;
  }
  if (p.mode == Mode::Left || p.mode == Mode::Right) {
    rt_async_hold(false);
    eyestate_mono(r);
    render_eye(r, view, a3, a4, a5, p, p.mode == Mode::Left ? -1 : 1, true, -1);
    return;
  }
  // sbs: needs a packed output and our GPU side; only the first world render of a frame; not while
  // the engine changes level (level_changing).
  bool loading = nth == 0 && level_changing();
  if (loading) g_n_load_mono++;
  if (!packed_now() || !gpu_ready() || nth != 0 || loading || at<uint8_t>(r, kRDeviceLost)) {
    if (nth == 0) g_n_mono_fallback++;
    if (nth == 0) rt_async_hold(false);
    eyestate_mono(r);
    o_renderWorld(r, view, a3, a4, a5);
    return;
  }
  int w = rglob<int>(kRvaOutputSize), h = rglob<int>(kRvaOutputSize + 4);
  // Convergence: by default a shift of each finished eye image (after TAA/DLSS, which would partly
  // undo a projection shift; Torchbearer measured that), else in the projection.
  // With frame generation the projection (its per-eye DLSS-G features are sized to the eye; an image
  // shift would re-size them whenever the game's zoom moves the shift; measured equal, docs/stereo.md).
  bool shift_conv = p.conv_shift != 0 && !g_fg_linked;
  g_shift[0] = g_shift[1] = 0;
  if (shift_conv && p.convergence_m > 0) {
    double ndc = std::fabs(conv_ndc_for(p, view, p.separation_mm / 1000.0));
    int total = int(std::lround(ndc * w));  // both eyes, pixels: 2 * ndc * W/2
    int a = total / 2, b = total - a;
    // The left eye's image moves left, the right eye's right (screen plane nearer than infinity).
    g_shift[p.swap ? 1 : 0] = -a;
    g_shift[p.swap ? 0 : 1] = b;
  }
  eyestate_frame(r, true);
  rt_async_hold(true);
  g_vis_tid = GetCurrentThreadId();
  g_rand_vals.clear();
  g_rand_pos = 0;
  g_as_pair_jobs = 0;
  g_vis_phase = 1;
  render_eye(r, view, a3, a4, a5, p, -1, !shift_conv, 0);
  g_vis_phase = 2;
  // The graph's Cleanup pass left the final colour in +0x3c8 (a pool texture the next render may
  // reuse): copy it out now, on the engine's own list right after the left eye's passes.
  void* cc = gpu_native(at<void*>(r, kRCreatedColor));
  void* e0 = eye_texture(r, 0, cc);
  bool ok = cc && e0;
  if (ok) eng_copy(e0, 0, 0, cc, 0, 0, w, h);
  if (census_on()) census("sbs: left eye rendered, stash %d (colour %s)", ok, gpu_describe(cc).c_str());
  render_eye(r, view, a3, a4, a5, p, 1, !shift_conv, 1);
  vis_end();
  g_pair = ok;
  g_pair_w = w, g_pair_h = h;
  if (ok) g_n_pairs++;
  // The UI layer (world markers' depth, the pause background): this pair's centre camera and stereo.
  UiFrame uf;
  uf.w = w, uf.h = h, uf.pack = g_pack.load();
  uf.sep_m = p.separation_mm / 1000.0;
  uf.conv_m = p.convergence_m;
  const double* c = reinterpret_cast<const double*>(static_cast<const uint8_t*>(view) + kMViewToClip);
  const double* m = reinterpret_cast<const double*>(static_cast<const uint8_t*>(view) + kMViewToWorld);
  uf.m00 = c[0] > 0.05 && c[0] < 50 ? c[0] : 0;
  uf.sign = p.swap ? -1 : 1;
  uf.proj_conv = !shift_conv;
  uf.shift[0] = g_shift[0], uf.shift[1] = g_shift[1];
  for (int k = 0; k < 3; k++) uf.cam_fwd[k] = m[6 + k], uf.cam_pos[k] = m[9 + k];
  uf.render_w = rglob<int>(kRvaRenderSize), uf.render_h = rglob<int>(kRvaRenderSize + 4);
  uf.sbs_native = g_sbs ? gpu_native(g_sbs) : nullptr;
  ui_frame(uf);
}

// HDR: renderHUD paints the UI into a separate target ("RendererX86::pSeparateHudColorTarget", RGBA8,
// allocated at the output size for each call) and composes it over the colour target with
// copy.rfx/composeSceneAndUI, dispatched over the colour target's own size (static, renderer
// 0x1343b0). With the side-by-side texture as the colour target the compose already covers 2W; only
// the UI target is one eye wide, so the duplicated UI draws fell outside it (live r24: HUD in the left
// eye only). While our renderHUD call runs, that one allocation is made 2W wide.
Alloc_t o_rt_alloc;
std::atomic<int> g_hud_hdr_w{0};  // armed: the eye width
// stereo.hdr_hud_wide (default 1): the HDR UI target 2W wide during our renderHUD. The laptop's r25
// "hang" with it was the wedged driver (VRAM); on rayblaster it ran through r3, r15 and r18 (teleports,
// restart) with the HUD in both eyes.
std::atomic<bool> g_hud_wide_on{true};
std::atomic<uint64_t> g_n_hud_wide{0};
void* __fastcall hk_rt_alloc(void* mgr, const void* init, const char* name) {
  int w = g_hud_hdr_w.load(std::memory_order_relaxed);
  if (w > 0 && name && strcmp(name, "RendererX86::pSeparateHudColorTarget") == 0) {
    int h = rglob<int>(kRvaOutputSize + 4);
    alignas(16) uint8_t wide[kTexture2DInitSize + 0x20] = {};
    alignas(16) float clear[4] = {0, 0, 0, 0};
    // As renderHUD makes it: (w, h, 1 mip, PixelFormat 0xe, usage 1, ..., 0.5f, 1, clear 0, -1).
    g_tex2d_init(wide, 2 * w, h, 1, 0xe, 1, 0, nullptr, 0, 0, 0, 0.5f, 1, clear, -1);
    g_n_hud_wide++;
    return o_rt_alloc(mgr, wide, name);
  }
  return o_rt_alloc(mgr, init, name);
}

// The HUD of a side-by-side frame: the engine's HUD pass draws into the colour target (+0x3c8)
// once per eye, and each eye + HUD is collected into its half of the side-by-side texture that
// `end` then blits (hk_end).
void __fastcall hk_renderHUD(void* r) {
  void* cct = at<void*>(r, kRCreatedColor);
  if (census_on()) {
    void* ui = at<void*>(r, 0x680);
    void* paint = ui ? (*reinterpret_cast<void***>(ui))[0] : nullptr;
    census("renderHUD r=%p colour=%p (%s) +0x108=%p bb=(%s) ui=%p paint=%s", r, cct,
           gpu_describe(gpu_native(cct)).c_str(), at<void*>(r, 0x108), gpu_describe(gpu_back_buffer()).c_str(), ui,
           where(reinterpret_cast<uintptr_t>(paint)).c_str());
  }
  if (!g_pair) {
    if (engine_loading()) ui_pause_cancel("a load");  // quit to menu, a load: the pause background ends
    if (!ui_pause_sbs() || !packed_now() || !gpu_ready() || params().mode != Mode::Sbs) {
      ui_hud_begin(false, rglob<int>(kRvaOutputSize));
      o_renderHUD(r);
      ui_hud_end();
      return;
    }
    // The pause menu over a per-eye background (ui.cpp): the side-by-side texture is filled with
    // the last pair (both eyes), the HUD pass runs over it with every draw repeated in the right half
    // except the engine's own background quad, and `end` blits it like a pair.
    int w = rglob<int>(kRvaOutputSize), h = rglob<int>(kRvaOutputSize + 4);
    void* sbs = g_sbs;  // the pairs' side-by-side texture (the colour target's format)
    if (cct || !sbs || g_sbs_w != 2 * w || g_sbs_h != h || !ui_pause_prepare(r, gpu_native(sbs), w, h)) {
      if (!cct) ui_pause_cancel("no side-by-side texture or a colour target set");
      o_renderHUD(r);
      return;
    }
    at<void*>(r, kRCreatedColor) = sbs;
    at<uint8_t>(r, kRCreatedOwned) = 0;
    gpu_dup_arm(w);
    if (o_rt_alloc && g_hud_wide_on.load()) g_hud_hdr_w = w;
    ui_hud_begin(true, w);
    o_renderHUD(r);
    ui_hud_end();
    g_hud_hdr_w = 0;
    gpu_dup_arm(0);
    // +0x3c8 stays the side-by-side texture (not owned): `end` blits it and clears it.
    g_shift[0] = g_shift[1] = 0;
    g_n_pause_sbs++;
    g_composed = true;
    return;
  }
  g_pair = false;
  Params p = params();
  void* cc = gpu_native(cct);
  void* sbs = sbs_texture(r, cc);
  int w = g_pair_w, h = g_pair_h;
  if (!cc || !sbs) {
    o_renderHUD(r);
    return;
  }
  void* sn = gpu_native(sbs);
  void* e0 = eye_texture(r, 0, cc);
  void* e1 = eye_texture(r, 1, cc);
  bool ok = e0 && e1;
  if (!ok) {
    o_renderHUD(r);
    return;
  }
  // Both eyes into their halves of the side-by-side texture (convergence: each shifted, uncovered
  // columns black), then the HUD drawn once into it with every draw repeated in the right half.
  eng_copy(e1, 0, 0, cc, 0, 0, w, h);  // the right eye's scene
  if (g_shift[0] || g_shift[1]) {
    static const float black[4] = {0, 0, 0, 1};
    g_clear_rt(g_retrieve_ds(), sn, black);
  }
  for (int e = 0; e < 2; e++) {
    int s = g_shift[e] < -(w - 1) ? -(w - 1) : g_shift[e] > w - 1 ? w - 1 : g_shift[e];
    void* src = e ? e1 : e0;
    if (s >= 0) eng_copy(sn, e * w + s, 0, src, 0, 0, w - s, h);
    else eng_copy(sn, e * w, 0, src, -s, 0, w + s, h);
  }
  fg_stereo_hudless(r, sn);  // frame generation: both eyes before the HUD (inert with [fg] off)
  at<void*>(r, kRCreatedColor) = sbs;
  uint8_t owned = at<uint8_t>(r, kRCreatedOwned);
  at<uint8_t>(r, kRCreatedOwned) = 0;
  if (p.hud_both) gpu_dup_arm(w);
  if (p.hud_both && o_rt_alloc && g_hud_wide_on.load()) g_hud_hdr_w = w;
  ui_hud_begin(p.hud_both, w);
  o_renderHUD(r);
  ui_hud_end();
  g_hud_hdr_w = 0;
  gpu_dup_arm(0);
  at<void*>(r, kRCreatedColor) = cct;
  at<uint8_t>(r, kRCreatedOwned) = owned;
  g_n_hud2++;
  g_composed = ok;
  if (census_on()) census("sbs: eyes + HUD collected %d (sbs %s)", ok, gpu_describe(sn).c_str());
}

// `end` blits the colour target (+0x3c8) into the back buffer at (viewport offset, output size) with
// the engine's own copy/gamma shader, then forgets it. With a packed output the side-by-side texture
// is blitted instead: full/sbs over a 2W-wide output (each eye 1:1), hsbs over the usual W (the
// shader's bilinear fetch at 2:1 averages each pixel pair: the squeeze). A frame without a pair
// (stereo off, menus, videos) puts the one image into both halves.
void __fastcall hk_end(void* r) {
  int pack = g_pack.load();
  void* cct = at<void*>(r, kRCreatedColor);
  if (!pack || !cct || !gpu_ready() || at<uint8_t>(r, kRDeviceLost)) {
    g_composed = false;
    o_end(r);
    return;
  }
  void* cc = gpu_native(cct);
  void* sbs = g_composed ? sbs_texture(r, cc) : mono_texture(r, cc);
  if (!sbs) {
    g_composed = false;
    o_end(r);
    return;
  }
  int w = rglob<int>(kRvaOutputSize), h = rglob<int>(kRvaOutputSize + 4);
  void* sn = gpu_native(sbs);
  if (!g_composed) {
    eng_copy(sn, 0, 0, cc, 0, 0, w, h);
    eng_copy(sn, w, 0, cc, 0, 0, w, h);
    g_n_mono_dup++;
  }
  // Frame generation evaluates each eye in its half (inert with [fg] off).
  if (g_composed) fg_stereo_frame(pack, w, std::max(-(w - 1), std::min(w - 1, g_shift[0])),
                                  std::max(-(w - 1), std::min(w - 1, g_shift[1])), true);
  else fg_stereo_frame(pack, w, 0, 0, false);
  g_composed = false;
  uint8_t owned = at<uint8_t>(r, kRCreatedOwned);
  at<void*>(r, kRCreatedColor) = sbs;
  at<uint8_t>(r, kRCreatedOwned) = 0;
  int& ow = rglob<int>(kRvaOutputSize);
  if (pack == 1) ow = 2 * w;
  o_end(r);
  ow = w;
  // `end` cleared +0x3c8; it would have freed a target renderHUD created.
  if (owned && g_rt_free) g_rt_free(*reinterpret_cast<void**>(g_rimg.at(kRvaRtManager)), cct);
  g_end_composed = true;
  if (census_on()) census("end: side-by-side blit (pack %d, %dx%d)", pack, pack == 1 ? 2 * w : w, h);
}

// --- captures ----------------------------------------------------------------------------------
std::mutex g_cmu;
int g_shots_left = 0, g_shot_skip = 0;
std::string g_shot_tag, g_shot_dir;
std::vector<std::string> g_shot_files;
std::string g_grab_kind, g_grab_tag;  // a pending `stereo grab`
int g_grab_i = 0;

LONG CALLBACK on_exception(EXCEPTION_POINTERS* ep);
void on_present(IDXGISwapChain* sc, uint64_t frame) {
  g_sc = sc;
  window_present(sc);
  if (g_fence_trace) fence_trace_queue(sc);
  // Inert while off: the GPU side (our command lists, the command-list hooks) and the exception
  // log are set up only with a side-by-side output (or for a lab capture).
  if (!gpu_ready()) {
    bool shots;
    {
      std::lock_guard lk(g_cmu);
      shots = g_shots_left > 0;
    }
    if (g_disabled.load() || (g_pack.load() == 0 && !shots)) return;
    if (gpu_attach(sc)) AddVectoredExceptionHandler(1, on_exception);
  }
  if (census_on()) {
    census("Present worlds=%d composed=%d bb=(%s)", g_worlds_this_frame, int(g_end_composed),
           gpu_describe(gpu_back_buffer()).c_str());
    if (--g_census == 0) TH_LOG("census done");
  }
  // Packed output without a stereo pair this frame (stereo off, menus, videos): the engine's one
  // image goes to both halves (2D in the glasses) instead of one half and an undefined one.
  void* bb = gpu_back_buffer();
  if (!g_end_composed && present_width_scale(sc) > 1 && gpu_ready() && bb && gpu_duplicate(bb, 0, 0)) g_n_dup++;
  {
    std::lock_guard lk(g_cmu);
    if (!g_grab_kind.empty() && gpu_ready()) {
      char name[512];
      void* t = g_grab_kind == "bb" ? bb : eyestate_texture(g_grab_kind, g_grab_i);
      snprintf(name, sizeof name, "%s\\stereo_%08llu_%s_%s%d.raw", g_shot_dir.c_str(), (unsigned long long)frame,
               g_grab_tag.c_str(), g_grab_kind.c_str(), g_grab_i);
      if (t && gpu_capture(t, name)) g_shot_files.push_back(name);
      else TH_LOG("stereo grab %s %d: no texture", g_grab_kind.c_str(), g_grab_i);
      g_grab_kind.clear();
    }
    if (g_shot_skip > 0) {
      g_shot_skip--;
    } else if (g_shots_left > 0 && bb && gpu_ready()) {
      char name[512];
      snprintf(name, sizeof name, "%s\\stereo_%08llu_%s_%s.%s", g_shot_dir.c_str(), (unsigned long long)frame,
               g_shot_tag.c_str(), g_end_composed ? "SBS" : "M", g_shot_tag.find("raw") != std::string::npos ? "raw" : "bmp");
      if (gpu_capture(bb, name)) g_shot_files.push_back(name);
      g_shots_left--;
    }
  }
  g_pair = false;
  g_composed = false;
  g_end_composed = false;
  g_worlds_this_frame = 0;
  if (frame != g_pan_frame) {
    g_pan_frame = frame;
    double rate = g_pan_rate.load();
    g_pan_rad = rate != 0 ? g_pan_rad + rate * M_PI / 180.0 : 0;
  }
}

// --- output / Hyprland ---------------------------------------------------------------------------
// [stereo] output, decided before the swapchain exists (it sets the swapchain's width):
//   full   2x-wide swapchain, each eye 1:1 (a plain 3840-wide desktop)
//   sbs    the same buffer under HypXRland, with a Proton that lets the swapchain be larger than its
//          window (Wine's Wayland driver with WINE_VULKAN_ALLOW_LARGER_SWAPCHAIN=1)
//   hsbs   normal swapchain; both eyes rendered at the game's resolution and squeezed 2:1 into the
//          halves (HypXRland's 3D mode with any Proton: it stretches each half back)
//   none   normal swapchain; 3D modes render a normal image
//   auto   HypXRland stereo display: sbs with the larger-swapchain Proton, else hsbs; elsewhere
//          none, or full when mode is forced to sbs
std::string g_output_req, g_output_now;  // req empty until the config names it (then auto by default)
std::string g_tag_layout;
bool g_tag_on = true;
bool larger_swapchain_ok() {
  return process_env("WINE_VULKAN_ALLOW_LARGER_SWAPCHAIN") == "1" && process_env("PROTON_ENABLE_WAYLAND") == "1";
}
void update_tag() {
  hypr_set_window_tag(g_tag_on && !g_tag_layout.empty() ? "stereo-" + g_tag_layout : "");
  quiver_stereo_layout(g_tag_layout.empty() ? "mono" : g_tag_layout.c_str());
}
void apply_output_locked() {
  std::string v = g_output_req.empty() ? "auto" : g_output_req;
  if (v == "auto") {
    int ds = g_p.mode == Mode::Auto || g_p.mode == Mode::Sbs ? hypr_display_stereo_now() : -1;
    if (ds == 1) v = larger_swapchain_ok() ? "sbs" : "hsbs";
    else v = g_p.mode == Mode::Sbs ? "full" : "none";
    TH_LOG("stereo: output auto -> %s (display %s, larger-swapchain Proton %s)", v.c_str(),
           ds == 1 ? "stereo" : ds == 0 ? "mono" : "unknown", larger_swapchain_ok() ? "yes" : "no");
  }
  if (v == "half") v = "hsbs";
  if (v == g_output_now) return;
  if (!g_output_now.empty() && present_frame() > 0)
    TH_LOG("stereo: output %s -> %s takes effect at the next launch (the swapchain exists)", g_output_now.c_str(),
           v.c_str());
  g_output_now = v;
  bool wide = v == "full" || v == "sbs";
  bool packed = (wide || v == "hsbs") && !g_disabled.load();
  if (present_frame() == 0) present_set_width_scale(wide && packed ? 2 : 1);
  g_packed = packed;
  g_pack = !packed ? 0 : wide ? 1 : 2;
  g_tag_layout = !packed ? "" : wide ? "sbs" : "hsbs";
  update_tag();
}

std::string status_json() {
  Params p = params();
  std::string raw_mode;
  {
    std::lock_guard lk(g_pmu);
    raw_mode = mode_name(g_p.mode);
  }
  char b[6144];
  snprintf(b, sizeof b,
           "{\"mode\":\"%s\",\"mode_setting\":\"%s\",\"output\":\"%s\",\"packed\":%s,\"disabled\":%s,"
           "\"separation_mm\":%.2f,\"convergence_m\":%.2f,\"swap\":%s,\"shift\":[%d,%d],\"m00\":%.4f,"
           "\"worlds\":%llu,\"pairs\":%llu,\"biased\":%llu,\"hud2\":%llu,\"hud_dup_draws\":%llu,\"dup\":%llu,\"mono_dup\":%llu,\"mono_fallback\":%llu,"
           "\"rand_rec\":%llu,\"rand_replay\":%llu,\"vis_reuse\":%llu,\"vis_held\":%llu,\"vis_miss\":%llu,\"video_skip\":%llu,\"video\":{\"active\":%d,\"where\":%u},\"gfx\":{\"replay\":%d,\"calls\":[%llu,%llu,%llu],\"cmds\":[%llu,%llu,%llu],\"ps\":[%llu,%llu,%llu],\"replays\":%llu,\"cap_short\":%llu,\"declare\":%d,\"decl\":[%llu,%llu,%llu],\"decl_replays\":%llu,\"decl_dropped\":%llu,\"kinds\":%s},\"level_changes\":%llu,\"load_mono\":%llu,\"blas_builds\":%llu,\"as_skipped\":%llu,\"refit_skipped\":%llu,\"as_late\":%llu,\"as_extra\":%llu,\"as_new\":%llu,\"as_new_built\":%llu,\"rt_upd_skipped\":%llu,\"tlas_skipped\":%llu,\"left_builds\":%llu,\"left_compactions\":%llu,\"hud_wide\":%llu,\"created_color\":%llu,\"present\":%llu,\"display_stereo\":%d,\"gpu\":%s,\"eyestate\":%s,\"window\":%s,\"pause_sbs\":%llu,\"ui\":%s}",
           mode_name(p.mode), raw_mode.c_str(), g_output_now.c_str(), g_packed.load() ? "true" : "false",
           g_disabled.load() ? "true" : "false", p.separation_mm, p.convergence_m, p.swap ? "true" : "false",
           g_shift[0], g_shift[1], g_last_m00, (unsigned long long)g_n_worlds.load(),
           (unsigned long long)g_n_pairs.load(), (unsigned long long)g_n_biased.load(),
           (unsigned long long)g_n_hud2.load(), (unsigned long long)gpu_dup_count(), (unsigned long long)g_n_dup.load(), (unsigned long long)g_n_mono_dup.load(),
           (unsigned long long)g_n_mono_fallback.load(), (unsigned long long)g_n_rand_rec.load(),
           (unsigned long long)g_n_rand_replay.load(), (unsigned long long)g_n_vis_reuse.load(),
           (unsigned long long)g_n_vis_held.load(), (unsigned long long)g_n_vis_miss.load(), (unsigned long long)g_n_video_skip.load(), g_video_active.load(), g_video_where.load(), int(g_gfx_replay.load()), (unsigned long long)g_n_gfx_calls[0].load(), (unsigned long long)g_n_gfx_calls[1].load(), (unsigned long long)g_n_gfx_calls[2].load(), (unsigned long long)g_n_gfx_cmds[0].load(), (unsigned long long)g_n_gfx_cmds[1].load(), (unsigned long long)g_n_gfx_cmds[2].load(), (unsigned long long)g_n_gfx_ps[0].load(), (unsigned long long)g_n_gfx_ps[1].load(), (unsigned long long)g_n_gfx_ps[2].load(), (unsigned long long)g_n_gfx_replays.load(), (unsigned long long)g_n_gfx_cap.load(), g_gfx_declare.load(), (unsigned long long)g_n_gfx_decl[0].load(), (unsigned long long)g_n_gfx_decl[1].load(), (unsigned long long)g_n_gfx_decl[2].load(), (unsigned long long)g_n_gfx_decl_replays.load(), (unsigned long long)g_n_gfx_decl_dropped.load(), gfx_kinds_json().c_str(), (unsigned long long)g_n_level_changes.load(), (unsigned long long)g_n_load_mono.load(), (unsigned long long)g_n_blas_builds.load(), (unsigned long long)g_n_as_skipped.load(), (unsigned long long)g_n_refit_skipped.load(), (unsigned long long)g_n_as_late.load(), (unsigned long long)g_n_as_extra.load(), (unsigned long long)g_n_as_new.load(), (unsigned long long)g_n_as_new_built.load(), (unsigned long long)g_n_rt_upd_skipped.load(), (unsigned long long)g_n_tlas_skipped.load(), (unsigned long long)g_n_left_builds.load(), (unsigned long long)g_n_left_compactions.load(), (unsigned long long)g_n_hud_wide.load(),
           (unsigned long long)g_n_created_color.load(),
           (unsigned long long)present_frame(), hypr_display_stereo(), gpu_status().c_str(),
           eyestate_status().c_str(), window_status().c_str(), (unsigned long long)g_n_pause_sbs.load(),
           ui_status().c_str());
  return b;
}

bool g_hooked = false;

// First-chance access violations and the like, logged with module+offset and the code addresses on
// the stack (a crash otherwise leaves nothing: the lab's game.log shows only the exit). Logs only.
std::string where(uintptr_t a) {
  HMODULE m = nullptr;
  if (a < 0x10000 || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                         reinterpret_cast<LPCWSTR>(a), &m) || !m)
    return {};
  wchar_t path[MAX_PATH];
  DWORD n = GetModuleFileNameW(m, path, MAX_PATH);
  std::wstring w(path, n);
  size_t sl = w.find_last_of(L"\\/");
  char b[200];
  snprintf(b, sizeof b, "%ls+%#llx", (sl == std::wstring::npos ? w : w.substr(sl + 1)).c_str(),
           (unsigned long long)(a - reinterpret_cast<uintptr_t>(m)));
  return b;
}
std::atomic<int> g_exc_logged{0};
LONG CALLBACK on_exception(EXCEPTION_POINTERS* ep) {
  DWORD code = ep->ExceptionRecord->ExceptionCode;
  if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
      code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != 0xc0000409)
    return EXCEPTION_CONTINUE_SEARCH;
  if (g_exc_logged.fetch_add(1) >= 4) return EXCEPTION_CONTINUE_SEARCH;
  auto* c = ep->ContextRecord;
  TH_LOG("exception %#lx at %s (%#llx) tid %lu, info %#llx %#llx; rcx %#llx rdx %#llx r8 %#llx rsp %#llx", code,
         where(c->Rip).c_str(), (unsigned long long)c->Rip, GetCurrentThreadId(),
         (unsigned long long)ep->ExceptionRecord->ExceptionInformation[0],
         (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1], (unsigned long long)c->Rcx,
         (unsigned long long)c->Rdx, (unsigned long long)c->R8, (unsigned long long)c->Rsp);
  auto* sp = reinterpret_cast<uintptr_t*>(c->Rsp);
  int shown = 0;
  for (int i = 0; i < 256 && shown < 16; i++) {
    if (IsBadReadPtr(sp + i, 8)) break;
    std::string w = where(sp[i]);
    if (w.empty() || w.find("+0x") == std::string::npos) continue;
    TH_LOG("exception:   [rsp+%#x] %s", i * 8, w.c_str());
    shown++;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

bool check_build() {
  g_rimg = module_image(kRenderer);
  if (!g_rimg) {
    TH_LOG("stereo: renderer_rmdwin10_f.dll not loaded; stereo disabled");
    return false;
  }
  auto stamp = [](const Image& m) -> uint32_t {
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m.base);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(m.base + dos->e_lfanew);
    return nt->FileHeader.TimeDateStamp;
  };
  if (stamp(g_rimg) != kRendererStamp) {
    TH_LOG("stereo: renderer_rmdwin10_f.dll build %#x, analysed %#x: stereo disabled", stamp(g_rimg), kRendererStamp);
    return false;
  }
  void** vt = reinterpret_cast<void**>(g_rimg.at(kRvaRendererVtbl));
  struct {
    uintptr_t rva;
    const char* sig;
    size_t slot;
    const char* name;
  } fns[] = {{kRvaRenderWorld, kSigRenderWorld, kSlotRenderWorld, "RendererX86 slot 15 renderWorld"},
             {kRvaRenderHUD, kSigRenderHUD, kSlotRenderHUD, "RendererX86 slot 17 renderHUD"},
             {kRvaEnd, kSigEnd, kSlotEnd, "RendererX86 slot 18 end"}};
  if (!sig_match((void*)g_rimg.at(kRvaQueuePop), kSigQueuePop) ||
      !sig_match((void*)g_rimg.at(kRvaQueuePush), kSigQueuePush)) {
    TH_LOG("stereo: visibility queue signature mismatch: stereo disabled");
    return false;
  }
  if (!sig_match((void*)g_rimg.at(kRvaSsaoPrepare), kSigSsaoPrepare)) {
    TH_LOG("stereo: rend::SSAO prepare signature mismatch: stereo disabled");
    return false;
  }
  for (auto& f : fns) {
    if (!sig_match((void*)g_rimg.at(f.rva), f.sig) || vt[f.slot] != (void*)g_rimg.at(f.rva)) {
      TH_LOG("stereo: %s signature/vtable mismatch at renderer+%#llx: stereo disabled", f.name,
             (unsigned long long)f.rva);
      return false;
    }
  }
  g_setViewToWorld = reinterpret_cast<SetM43_t>(module_export(kRl, kExpSetViewToWorld, "stereo"));
  g_setViewToClip = reinterpret_cast<SetM43_t>(module_export(kRl, kExpSetViewToClip, "stereo"));
  if (!g_setViewToWorld || !g_setViewToClip || !sig_match((void*)g_setViewToWorld, kSigSetViewToWorld) ||
      !sig_match((void*)g_setViewToClip, kSigSetViewToClip)) {
    TH_LOG("stereo: m::View setters missing or signature mismatch: stereo disabled");
    return false;
  }
  g_tex2d_init = reinterpret_cast<Tex2DInit_t>(module_export(kD3D, kExpTexture2DInit, "stereo"));
  g_rt_alloc = reinterpret_cast<Alloc_t>(module_export(kRenderer, kExpAllocTexture2D, "stereo"));
  g_rt_free = reinterpret_cast<Free_t>(module_export(kRenderer, kExpRtFree, "stereo"));
  if (!g_tex2d_init || !g_rt_alloc || !g_rt_free || !sig_match((void*)g_tex2d_init, kSigTexture2DInit) ||
      !sig_match((void*)g_rt_alloc, kSigAllocTexture2D) || !sig_match((void*)g_rt_free, kSigRtFree)) {
    TH_LOG("stereo: render-target pool exports missing or signature mismatch: stereo disabled");
    return false;
  }
  g_copy_region = reinterpret_cast<CopyRegion_t>(module_export(kD3D, kExpCopyRegion, "stereo"));
  g_clear_rt = reinterpret_cast<ClearRT_t>(module_export(kD3D, kExpClearRT, "stereo"));
  g_retrieve_ds = reinterpret_cast<RetrieveDS_t>(module_export(kD3D, kExpRetrieveDS, "stereo"));
  if (!g_copy_region || !g_clear_rt || !g_retrieve_ds || !sig_match((void*)g_copy_region, kSigCopyRegion) ||
      !sig_match((void*)g_clear_rt, kSigClearRT) || !sig_match((void*)g_retrieve_ds, kSigRetrieveDS)) {
    TH_LOG("stereo: d3d copy/clear exports missing or signature mismatch: stereo disabled");
    return false;
  }
  if (!gpu_start()) {
    TH_LOG("stereo: d3d exports missing or signature mismatch: stereo disabled");
    return false;
  }
  return true;
}

}  // namespace
}  // namespace stereo

using namespace stereo;

void stereo_register() {
  cfg_listen("stereo.", [](const std::string& id, const std::string& v) {
    std::lock_guard lk(g_pmu);
    if (id == "stereo.mode") {
      g_p.mode = parse_mode(v);
      if (!g_output_req.empty()) apply_output_locked();
    } else if (id == "stereo.separation") {
      g_p.separation_mm = atof(v.c_str());
    } else if (id == "stereo.convergence") {
      g_p.convergence_m = atof(v.c_str());
    } else if (id == "stereo.swap_eyes") {
      g_p.swap = truthy(v);
    } else if (id == "stereo.hud_both") {
      g_p.hud_both = truthy(v);
    } else if (id == "stereo.convergence_shift") {
      g_p.conv_shift = truthy(v) ? 1 : 0;
    } else if (id == "stereo.rand_replay") {
      g_rand_replay = truthy(v) || v.empty();
    } else if (id == "stereo.test_pan") {
      g_pan_rate = atof(v.c_str());
    } else if (id == "stereo.output") {
      g_output_req = v.empty() ? "auto" : v;
      apply_output_locked();
    } else if (id == "stereo.as_once") {
      g_as_once = (v == "false" || v == "off") ? 0 : (v == "true" || v == "on" || v.empty()) ? 21 : atoi(v.c_str());
    } else if (id == "stereo.video_once") {
      g_video_once = !(v == "0" || v == "false" || v == "off");
    } else if (id == "stereo.gfxgraph_declare") {
      g_gfx_declare = v.empty() ? 1 : atoi(v.c_str());
    } else if (id == "stereo.gfxgraph_replay") {
      g_gfx_replay = !(v == "0" || v == "false" || v == "off");
    } else if (id == "stereo.load_mono") {
      g_load_mono = !(v == "0" || v == "false" || v == "off");
    } else if (id == "stereo.rt_async_build") {
      g_rt_async_keep = truthy(v);
    } else if (id == "stereo.hdr_hud_wide") {
      g_hud_wide_on = truthy(v);
    } else if (id == "stereo.ui_markers" || id == "stereo.ui_pause" || id == "stereo.ui_reticle") {
      ui_option(id, v);
    } else if (id == "stereo.hyprland_tag") {
      g_tag_on = !(v == "0" || v == "false" || v == "off");
      update_tag();
    }
  });
  eyestate_register();
  lab_verb("stereo", [](const std::vector<std::string>& a, bool& ok) -> std::string {
    std::string sub = a.empty() ? "status" : a[0];
    if (sub == "status") return status_json();
    if (sub == "hyprland") return hypr_status();
    if (!g_hooked) {
      ok = false;
      return json_str("stereo hooks not installed (see threshold.log)");
    }
    if (sub == "census") {
      g_census = a.size() > 1 ? atoi(a[1].c_str()) : 2;
      return json_str("census armed for " + std::to_string(g_census.load()) + " presents");
    }
    if (sub == "shots") {
      std::lock_guard lk(g_cmu);
      g_shots_left = a.size() > 1 ? atoi(a[1].c_str()) : 1;
      g_shot_tag = a.size() > 2 ? a[2] : "shot";
      g_shot_skip = a.size() > 3 ? atoi(a[3].c_str()) : 0;
      const char* d = getenv("QUIVER_LAB_RUN_DIR");
      g_shot_dir = d ? d : ".";
      g_shot_files.clear();
      return json_str("capturing " + std::to_string(g_shots_left) + " frames to " + g_shot_dir);
    }
    if (sub == "files") {
      std::lock_guard lk(g_cmu);
      std::string r = "[";
      for (size_t i = 0; i < g_shot_files.size(); i++) r += (i ? "," : "") + json_str(g_shot_files[i]);
      return r + "]";
    }
    if (sub == "tweak" && a.size() > 2) {  // dev: d::BaseTweakable::setTweakable(name, value) (rl export)
      using SetTw_t = bool(__cdecl*)(const char*, const char*, bool);
      auto set = reinterpret_cast<SetTw_t>(module_export(kRl, "?setTweakable@BaseTweakable@d@@SA_NPEBD0_N@Z", "stereo"));
      std::string name = a[1];
      for (size_t k = 2; k + 1 < a.size(); k++) name += " " + a[k];  // names contain spaces
      bool r = set && set(name.c_str(), a.back().c_str(), false);
      TH_LOG("stereo: tweak '%s' = %s -> %d", name.c_str(), a.back().c_str(), int(r));
      return json_str(std::string(r ? "set " : "failed ") + name + " = " + a.back());
    }
    if (sub == "graph") return eyestate_graph();
    if (sub == "ui" && a.size() > 1 && a[1] == "shot") {  // ui shot TAG: a HUD dump and the frames around it
      std::lock_guard lk(g_cmu);
      g_shots_left = 2;
      g_shot_tag = a.size() > 2 ? a[2] : "ui";
      g_shot_skip = 0;
      const char* d = getenv("QUIVER_LAB_RUN_DIR");
      g_shot_dir = d ? d : ".";
      g_shot_files.clear();
      std::vector<std::string> dump = {"ui", "dump", "1"};
      return ui_verb(dump, ok);
    }
    if (sub == "ui") return ui_verb(a, ok);
    if (sub == "fences") {
      fence_dump(a.size() > 1 ? atoi(a[1].c_str()) : 300);
      return json_str("dumped to threshold.log");
    }
    if (sub == "grab" && a.size() > 2) {  // grab KIND I [TAG]: one history texture at the next Present
      std::lock_guard lk(g_cmu);
      g_grab_kind = a[1];
      g_grab_i = atoi(a[2].c_str());
      g_grab_tag = a.size() > 3 ? a[3] : "grab";
      const char* d = getenv("QUIVER_LAB_RUN_DIR");
      g_shot_dir = d ? d : ".";
      g_shot_files.clear();
      return json_str("grabbing " + g_grab_kind + " " + a[2] + " at the next Present");
    }
    if (sub == "probe") {
      eyestate_probe(a.size() > 1 ? atoi(a[1].c_str()) : 4);
      return json_str("probing the next eye renders into threshold.log");
    }
    ok = false;
    return json_str("usage: stereo [status|hyprland|census N|shots N TAG [SKIP]|files|probe N]");
  });
}

void stereo_start() {
  {
    std::lock_guard lk(g_pmu);
    if (g_output_now.empty()) apply_output_locked();  // no [stereo] output key: auto
  }
  if (!check_build()) {
    std::lock_guard lk(g_pmu);
    g_disabled = true;
    g_packed = false;
    g_pack = 0;
    g_tag_layout.clear();
    update_tag();
    present_set_width_scale(1);
    return;
  }
  bool ok = hook_fn((void*)g_rimg.at(kRvaRenderWorld), (void*)hk_renderWorld, (void**)&o_renderWorld,
                    "RendererX86::renderWorld (slot 15)") &&
            hook_fn((void*)g_rimg.at(kRvaRenderHUD), (void*)hk_renderHUD, (void**)&o_renderHUD,
                    "RendererX86::renderHUD (slot 17)") &&
            hook_fn((void*)g_rimg.at(kRvaEnd), (void*)hk_end, (void**)&o_end, "RendererX86::end (slot 18)") &&
            hook_fn((void*)g_rimg.at(kRvaQueuePop), (void*)hk_pop, (void**)&o_pop, "renderer queue pop") &&
            hook_fn((void*)g_rimg.at(kRvaQueuePush), (void*)hk_push, (void**)&o_push, "renderer queue push");
  if (!ok) {
    TH_LOG("stereo: hooks failed; stereo disabled");
    std::lock_guard lk(g_pmu);
    g_disabled = true;
    g_packed = false;
    g_pack = 0;
    g_tag_layout.clear();
    update_tag();
    present_set_width_scale(1);
    return;
  }
  eyestate_start(g_rimg);
  if (g_pack.load() != 0) {
    UiEngine ue;
    ue.copy_region = g_copy_region;
    ue.tex2d_init = g_tex2d_init;
    ue.rt_alloc = g_rt_alloc;
    ue.rt_free = g_rt_free;
    ue.native = gpu_native;
    ue.back_buffer = gpu_back_buffer;
    ui_start(g_rimg, ue);
  }
  if (cfg_bool("stereo.fence_trace", false)) fence_trace_start();
  level_hooks_start();
  if (cfg_bool("stereo.hdr_hud_wide", true) &&
      !hook_fn((void*)g_rt_alloc, (void*)hk_rt_alloc, (void**)&o_rt_alloc, "RenderTargetManager::allocTexture2D"))
    o_rt_alloc = nullptr;  // only hooked when the dev option asks for it (start-up)
  if (void* vu = module_export(kRenderer, kExpVideoUpdateAsync, "stereo"); vu && sig_match(vu, kSigVideoUpdateAsync)) {
    g_videoCount = reinterpret_cast<VideoCount_t>(module_export(kRenderer, "?countActiveSources@VideoStreamerFactory@rend@@QEBAHXZ", "stereo"));
    if (!hook_fn(vu, (void*)hk_videoUpdate, (void**)&o_videoUpdate, "VideoStreamerFactory::updateAsync"))
      TH_LOG("stereo: video decode not hooked; world video textures decode twice per update in sbs");
  } else {
    TH_LOG("stereo: VideoStreamerFactory::updateAsync missing or signature mismatch; video textures decode twice per update in sbs");
  }
  if (void* gd = reinterpret_cast<void*>(g_rimg.at(kRvaGfxGraphDeclare)); sig_match(gd, kSigGfxGraphDeclare)) {
    if (!hook_fn(gd, (void*)hk_gfxDeclare, (void**)&o_gfxDeclare, "GfxGraphEngine declare (stereo: GfxGraph offscreen work in both eyes)"))
      TH_LOG("stereo: GfxGraphEngine declare not hooked; GfxGraph offscreen work only in the left eye");
  } else {
    TH_LOG("stereo: GfxGraphEngine declare signature mismatch; GfxGraph offscreen work only in the left eye");
  }
  if (void* gg = reinterpret_cast<void*>(g_rimg.at(kRvaGfxGraphRecord)); sig_match(gg, kSigGfxGraphRecord)) {
    if (!hook_fn(gg, (void*)hk_gfxRecord, (void**)&o_gfxRecord, "GfxGraphEngine pass (stereo: GfxGraph effects in both eyes)"))
      TH_LOG("stereo: GfxGraphEngine pass not hooked; GfxGraph effects (Polaris, Hiss) only in the left eye");
  } else {
    TH_LOG("stereo: GfxGraphEngine pass signature mismatch; GfxGraph effects (Polaris, Hiss) only in the left eye");
  }
  if (HMODULE crt = GetModuleHandleW(L"ucrtbase.dll")) {
    void* r = (void*)GetProcAddress(crt, "rand");
    if (!r || !hook_fn(r, (void*)hk_rand, (void**)&o_rand, "ucrtbase!rand"))
      TH_LOG("stereo: rand() not hooked; the eyes' render noise may differ");
  }
  // output = full on a plain side-by-side desktop: the game's window twice the engine's width.
  if (g_output_now == "full") window_start();
  present_on(on_present);
  hypr_start();
  // Frame generation (docs/stereo.md "Stereo with frame generation"): FG keeps one input set and
  // DLSS-G feature per eye, told which eye each DLSS evaluation belongs to. Only with a packed output
  // (start-up): without one no frame is side by side, and FG runs exactly as it does alone.
  if (fg_enabled() && g_pack.load() != 0) {
    fg_stereo_link({[]() -> int {
                      int ph = g_vis_phase.load(std::memory_order_relaxed);
                      if (ph == 0 || GetCurrentThreadId() != g_vis_tid.load(std::memory_order_relaxed)) return -1;
                      return ph == 1 ? 0 : 1;
                    },
                    []() { return g_pair; }});
    g_fg_linked = true;
  }
  g_hooked = true;
  TH_LOG("stereo: hooks installed (output %s)", g_output_now.c_str());
}

}  // namespace th
