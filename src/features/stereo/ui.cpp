// The UI layer in side-by-side frames (ui.h; docs/stereo.md "World markers", "Pause background").
#include "features/stereo/ui.h"

#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "core/hook.h"
#include "core/lab.h"
#include "core/log.h"
#include "core/module.h"
#include "core/present.h"
#include "features/stereo/engine.h"
#include "features/stereo/gpu.h"

namespace th::stereo {
namespace {

template <typename T>
T& at(void* base, size_t off) {
  return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + off);
}

constexpr wchar_t kUi[] = L"ui_rmdwin10_f.dll";
// ui::UIRendererBackend (ui_rmdwin10_f.dll, static): ExecuteRendering (vtable slot 24, ui+0x32080)
// runs renoir's command buffers; it sets the vertex shader constants through ui+0x2c1a0 (type 0
// "Transform" float4x4, row-major, translation in [3] / [7]) and binds textures through ui+0x2cd40
// (txBuffer0..3), then d3d::DeviceUtil::drawIndexedPrimitive. Render targets through the d3d export
// setRenderTargets.
constexpr uintptr_t kRvaUiSetConst = 0x2c1a0;
constexpr char kSigUiSetConst[] = "83 f9 06 0f 87 71 0b 00 00 48 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57";
constexpr uintptr_t kRvaUiBindTex = 0x2cd40;
constexpr char kSigUiBindTex[] = "48 8b c4 57 48 83 ec 60 48 c7 40 d8 fe ff ff ff 48 89 58 08 48 89 68 10";
constexpr uintptr_t kRvaUiExecute = 0x32080;
constexpr char kSigUiExecute[] = "48 8b c4 48 89 48 08 55 56 57 41 54 41 55 41 56 41 57 48 8d 6c 24 b0 48 81 ec 50 01 00 00";
constexpr char kExpSetRTs[] =
    "?setRenderTargets@DeviceUtil@d3d@@SAXAEAVDeviceState@2@HQEAPEBVNativeTexture@2@PEBV42@@Z";
// RendererX86 vtable slot 22 (renderer+0x135840, static): the pause background. Copies the back
// buffer at (viewport offset, the destination's size) into the Texture2D it is given; the engine's
// ShapeEngine then draws that texture full screen under the pause menu (live r5: the copy from
// renderer+0x135901 on the frame the menu opens, then one 6-index draw outside the UI backend per
// paused frame).
constexpr size_t kSlotCapture = 22;
constexpr uintptr_t kRvaCapture = 0x135840;
constexpr char kSigCapture[] = "48 89 5c 24 08 48 89 74 24 10 57 48 83 ec 70 48 8b f2 ff 15";
// Control_DX12.exe+0x5edb50 (static): the HUD's world-to-screen for world-anchored widgets (mission
// markers from exe+0x621410, interaction markers from exe+0x605180, and others). (offset dir, world
// pos, out UI pixels, {scale, up}, safe area): projects P = pos + dir * scale + (0, up, 0) with the
// game camera's 16:9 projection; an off-screen point is clamped to the safe-area edge (returns true).
constexpr uintptr_t kRvaHudProject = 0x5edb50;
constexpr char kSigHudProject[] = "48 8b c4 55 57 41 56 48 8d a8 d8 fc ff ff 48 81 ec 10 04 00 00 48 c7 45 80 fe ff ff ff";
// The render graph's persisted "lineardepthcopy" (R16_FLOAT, render size; read next frame as
// previouslineardepth): the left eye's after a pair (eyestate swaps the right eye's list back).
constexpr uint64_t kHashLinearDepthCopy = 0xca970354dafdca24ull;

using SetConst_t = void(__fastcall*)(int type, const void* data);
using BindTex_t = void(__fastcall*)(unsigned slot, void* nt);
using Execute_t = void(__fastcall*)(void* be, void* bufs, unsigned n, void* res, unsigned nres);
using SetRTs_t = void(__fastcall*)(void* ds, int n, void* const* rts, const void* depth);
using Capture_t = void(__fastcall*)(void* r, void* tex2d);
using HudProject_t = bool(__fastcall*)(const float* dir, const float* pos, float* out, const float* su, float safe);
SetConst_t o_set_const;
BindTex_t o_bind_tex;
Execute_t o_execute;
SetRTs_t o_set_rts;
Capture_t o_capture;
HudProject_t o_hud_project;
UiEngine g_eng;
Image g_ren;
std::atomic<bool> g_markers_on{true}, g_pause_on{true};

// --- per-draw UI state (renderer thread) -----------------------------------------------------------
thread_local float t_xf[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
thread_local void* t_tex[4] = {};
thread_local void* t_rt = nullptr;
thread_local int t_in_exec = 0;
void __fastcall hk_set_const(int type, const void* data) {
  if (type == 0 && data) memcpy(t_xf, data, sizeof t_xf);
  o_set_const(type, data);
}
void __fastcall hk_bind_tex(unsigned slot, void* nt) {
  if (slot < 4) t_tex[slot] = nt;
  o_bind_tex(slot, nt);
}
void __fastcall hk_set_rts(void* ds, int n, void* const* rts, const void* depth) {
  t_rt = n > 0 && rts ? rts[0] : nullptr;
  o_set_rts(ds, n, rts, depth);
}
void __fastcall hk_execute(void* be, void* bufs, unsigned n, void* res, unsigned nres) {
  t_in_exec++;
  o_execute(be, bufs, n, res, nres);
  t_in_exec--;
}
int nt_w(void* nt) { return nt ? at<int>(nt, kNtWidth) : 0; }
int nt_h(void* nt) { return nt ? at<int>(nt, kNtHeight) : 0; }

// --- HUD projections (game thread) -----------------------------------------------------------------
struct Proj {
  float P[3];
  float sx, sy;  // UI pixels
  bool clamped;
  uint64_t frame;
};
std::mutex g_pmu;
constexpr int kProjRing = 128;
Proj g_proj[kProjRing];
int g_proj_pos = 0;
std::atomic<uint64_t> g_n_proj{0};
bool __fastcall hk_hud_project(const float* dir, const float* pos, float* out, const float* su, float safe) {
  bool r = o_hud_project(dir, pos, out, su, safe);
  g_n_proj++;
  if (out && g_markers_on.load(std::memory_order_relaxed)) {
    Proj p;
    p.P[0] = pos[0] + dir[0] * su[0];
    p.P[1] = pos[1] + dir[1] * su[0] + su[1];
    p.P[2] = pos[2] + dir[2] * su[0];
    p.sx = out[0], p.sy = out[1];
    p.clamped = r;
    p.frame = present_frame();
    std::lock_guard lk(g_pmu);
    g_proj[g_proj_pos] = p;
    g_proj_pos = (g_proj_pos + 1) % kProjRing;
  }
  return r;
}

// --- markers of this frame (renderer thread) ---------------------------------------------------
struct Marker {
  float P[3];
  float x, y;           // eye pixels (centre camera)
  bool clamped;
  float z_anchor;       // view depth of the anchor (m)
  float z_surface = 0;  // the first surface on the ray (probe, a few frames old), 0 unknown
  float z = 0;          // where it is drawn, 0 = screen depth (not moved)
  float s[2] = {0, 0};  // per-eye shift (px)
};
std::vector<Marker> g_mk;
UiFrame g_f;
bool g_f_ok = false;
struct ProbeMeta {
  int n = 0;
  float P[kProbeSlots][3];
};
ProbeMeta g_pm[kProbeRing];
uint64_t g_hud_n = 0;
bool g_probed = false;
int g_pass_ring = 0;
std::atomic<uint64_t> g_n_moved{0}, g_n_probes{0}, g_n_markers{0}, g_n_occluded{0};
std::string g_last_mk;  // status: the markers of the last HUD pass
std::mutex g_smu;

double dot3(const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// The disparity (px) of a point at depth z in eye e relative to the centre image, plus convergence:
// what the scene at that depth gets, so the marker sits at depth z.
float eye_shift(int e, double z) {
  double K = g_f.sep_m * g_f.m00 * g_f.w / 2.0;  // px * m
  double d = g_f.sign * K / z;
  if (g_f.proj_conv && g_f.conv_m > 0) d -= g_f.sign * K / g_f.conv_m;
  return float(e == 0 ? d + (g_f.proj_conv ? 0 : g_f.shift[0]) : -d + (g_f.proj_conv ? 0 : g_f.shift[1]));
}

void build_markers() {
  g_mk.clear();
  if (!g_f_ok || !g_markers_on.load()) return;
  uint64_t now = present_frame();
  {
    std::lock_guard lk(g_pmu);
    for (int k = 0; k < kProjRing; k++) {
      const Proj& p = g_proj[(g_proj_pos - 1 - k + 2 * kProjRing) % kProjRing];
      if (!p.frame || p.frame + 3 < now) continue;
      bool dup = false;
      for (auto& m : g_mk)
        if (!memcmp(m.P, p.P, sizeof p.P)) {
          dup = true;
          break;
        }
      if (dup) continue;  // the newest of each anchor only
      Marker m;
      memcpy(m.P, p.P, sizeof m.P);
      m.x = p.sx, m.y = p.sy;  // the UI is the output size (live: 1920x1080 at 1920x1080)
      m.clamped = p.clamped;
      double d[3] = {p.P[0] - g_f.cam_pos[0], p.P[1] - g_f.cam_pos[1], p.P[2] - g_f.cam_pos[2]};
      m.z_anchor = float(dot3(d, g_f.cam_fwd));
      g_mk.push_back(m);
      if (g_mk.size() >= size_t(kProbeSlots)) break;
    }
  }
  // Surface depths probed kProbeRing - 1 passes ago (the oldest ring entry), matched by anchor.
  int rr = int((g_hud_n + 1) % kProbeRing);
  for (int i = 0; i < g_pm[rr].n; i++) {
    float v = 0;
    if (!gpu_probe_value(rr, i, &v) || !(v > 0) || !std::isfinite(v)) continue;
    for (auto& m : g_mk)
      if (!memcmp(m.P, g_pm[rr].P[i], sizeof m.P)) m.z_surface = v;
  }
  for (auto& m : g_mk) {
    float z = m.clamped ? m.z_surface : m.z_anchor;
    // Obstructed (a surface on the ray nearer than the anchor): just in front of that surface.
    if (!m.clamped && m.z_surface > 0 && m.z_surface < m.z_anchor) {
      z = m.z_surface * 0.97f;
      g_n_occluded++;
    }
    m.z = z > 0.3f ? z : 0;
    if (m.z > 0)
      for (int e = 0; e < 2; e++) m.s[e] = eye_shift(e, m.z);
  }
  g_n_markers += g_mk.size();
}

void* linear_depth() {
  const uint8_t* hdr = reinterpret_cast<const uint8_t*>(g_ren.at(kRvaGraphPrev));
  auto* data = *reinterpret_cast<uint8_t* const*>(hdr);
  uint32_t n = *reinterpret_cast<const uint32_t*>(hdr + 8);
  for (uint32_t i = 0; i < n && data && i < 64; i++) {
    uint8_t* e = data + size_t(i) * kGraphEntry;
    if (*reinterpret_cast<uint64_t*>(e) == kHashLinearDepthCopy && *reinterpret_cast<int*>(e + kGraphEntryType) == 0) {
      void* t = *reinterpret_cast<void**>(e + kGraphEntryRes);
      return t ? g_eng.native(t) : nullptr;
    }
  }
  return nullptr;
}

// On the engine's list at the HUD's first draw (after both eyes): the left eye's linear depth where
// each marker's anchor appears in the left eye.
void probe_markers(ID3D12GraphicsCommandList* cl) {
  g_probed = true;
  ProbeMeta& pm = g_pm[g_pass_ring];
  pm.n = 0;
  void* depth = linear_depth();
  if (!depth || g_f.w <= 0) return;
  int rw = nt_w(depth), rh = nt_h(depth);
  for (auto& m : g_mk) {
    if (pm.n >= kProbeSlots) break;
    double x = m.x, y = m.y;
    if (!m.clamped && m.z_anchor > 0.3f) {  // the anchor's position in the left eye's own render
      double K = g_f.sep_m * g_f.m00 * g_f.w / 2.0;
      x += g_f.sign * K / m.z_anchor - (g_f.proj_conv && g_f.conv_m > 0 ? g_f.sign * K / g_f.conv_m : 0);
    }
    int px = int(x * rw / g_f.w), py = int(y * rh / g_f.h);
    if (gpu_probe(cl, depth, g_pass_ring, pm.n, px, py)) {
      memcpy(pm.P[pm.n], m.P, sizeof m.P);
      pm.n++;
      g_n_probes++;
    }
  }
}

// --- pause background ----------------------------------------------------------------------------
// The engine's capture (slot 22) is only noted. At the next renderHUD without a pair (stereo.cpp's
// pause branch, the place where the pair path already allocates and copies), the side-by-side
// texture still holds the last presented pair (both eyes + HUD, 1:1): it is copied once into a
// texture of ours. Every paused frame then starts from that copy, and the engine's own background
// draw (ShapeEngine, one full-screen quad sampling the capture) is skipped in both halves. No game
// texture is written and nothing is allocated or copied inside slot 22.
struct Pause {
  bool pending = false;  // a capture happened; the next pause frame takes the side-by-side copy
  bool active = false;   // paused frames draw the per-eye copy
  int pairs_since = 0;   // world renders since the capture while pending
  void* tex = nullptr;   // our Texture2D (2W x H, the side-by-side texture's format)
  int w = 0, h = 0, fmt = -1;
} g_pause;
std::atomic<uint64_t> g_n_capture{0}, g_n_bg_skips{0}, g_n_pause_end{0};
bool g_hud_pause = false, g_hud_sbs = false, g_bg_seen = false;
int g_bg_missing = 0;
void* g_pause_target = nullptr;  // the side-by-side texture of this paused frame
int g_hud_w = 0, g_hud_h = 0;

void* pause_tex(void* r, int w2, int h, int fmt) {
  if (g_pause.tex && g_pause.w == w2 && g_pause.h == h && g_pause.fmt == fmt) return g_pause.tex;
  void* mgr = at<void*>(r, kRRtManager);
  if (!mgr || !g_eng.tex2d_init || !g_eng.rt_alloc) return nullptr;
  if (g_pause.tex) g_eng.rt_free(*reinterpret_cast<void**>(g_ren.at(kRvaRtManager)), g_pause.tex);
  g_pause.tex = nullptr;
  alignas(16) uint8_t init[kTexture2DInitSize + 0x20] = {};
  alignas(16) float clear[4] = {0, 0, 0, 0};
  g_eng.tex2d_init(init, w2, h, 1, fmt, 5, 0, nullptr, 0, 0, 0, 0.5f, 1, clear, -1);
  g_pause.tex = g_eng.rt_alloc(mgr, init, "Threshold::pauseSideBySide");
  g_pause.w = w2, g_pause.h = h, g_pause.fmt = fmt;
  TH_LOG("stereo ui: pause texture %dx%d format %d: %p", w2, h, fmt, g_pause.tex);
  return g_pause.tex;
}

void pause_end(const char* why) {
  if (g_pause.pending || g_pause.active) {
    g_n_pause_end++;
    TH_LOG("stereo ui: pause background ends (%s)", why);
  }
  g_pause.pending = g_pause.active = false;
}

void __fastcall hk_capture(void* r, void* tex2d) {
  o_capture(r, tex2d);
  g_pause.active = false;
  g_pause.pending = g_pause_on.load() && g_f_ok && g_f.pack != 0;
  g_pause.pairs_since = 0;
  if (g_pause.pending) {
    g_n_capture++;
    TH_LOG("stereo ui: pause capture (frame %llu): the next frame without a pair takes both eyes",
           (unsigned long long)present_frame());
  }
}

// --- per draw ----------------------------------------------------------------------------------
std::atomic<int> g_dump_huds{0};
int g_dump_draw = 0;

std::string tex_desc(void* nt) {
  if (!nt) return "-";
  char b[96];
  snprintf(b, sizeof b, "%p %dx%d f%d", nt, nt_w(nt), nt_h(nt), int(at<int8_t>(nt, kNtPixelFormat)));
  return b;
}

bool g_hud_obs = false;
void on_draw(ID3D12GraphicsCommandList* cl, int phase, DupDraw& d) {
  // A paused frame: the engine's background quad (the first full-screen draw outside the UI
  // backend: live, ShapeEngine's 6-index quad) is not drawn; the halves already hold both eyes.
  if (g_hud_pause && phase == 0 && !t_in_exec && !g_bg_seen && d.count == 6 && d.vp_x == 0 && d.vp_y == 0 &&
      int(d.vp_w) == g_hud_w && int(d.vp_h) == g_hud_h) {
    g_bg_seen = true;
    d.skip = true;
    // In its place (after renderHUD's clear, before the menu): both eyes of the last pair.
    if (g_pause.tex && g_pause_target && gpu_copy_on(cl, g_eng.native(g_pause.tex), g_pause_target)) g_n_bg_skips++;
    return;
  }
  if (!g_hud_sbs && !g_hud_obs) {
    if (phase == 0 && g_dump_huds.load(std::memory_order_relaxed) > 0)
      TH_LOG("ui draw (outside HUD): n=%u vp=(%.0f,%.0f %.0fx%.0f) rt=%s exec=%d tex=%s frame %llu", d.count, d.vp_x, d.vp_y,
             d.vp_w, d.vp_h, tex_desc(t_rt).c_str(), t_in_exec, tex_desc(t_tex[0]).c_str(), (unsigned long long)present_frame());
    return;
  }
  if (phase == 0) {
    if (!g_probed && !g_mk.empty()) probe_markers(cl);
    bool dump = g_dump_huds.load(std::memory_order_relaxed) > 0;
    const Marker* hit = nullptr;
    // A Coherent layer composited into the side-by-side target (2W): a small texture drawn with its
    // element's screen position in the Transform's translation (live: x = T[3] * viewport width,
    // y = T[7] * viewport height, within a few px of the projected anchor).
    if (t_in_exec && nt_w(t_rt) == 2 * g_hud_w && !g_mk.empty()) {
      void* tx = t_tex[0];
      if (tx && nt_w(tx) <= 256 && nt_h(tx) <= 256) {
        float x = t_xf[3] * d.vp_w, y = t_xf[7] * d.vp_h;
        float best = 1e9f;
        for (auto& m : g_mk) {
          float dx = x - m.x, dy = y - m.y;
          if (std::fabs(dx) > 56 || std::fabs(dy) > 80) continue;
          float dd = dx * dx + dy * dy;
          if (dd < best) best = dd, hit = &m;
        }
      }
      if (hit && hit->z > 0) {
        d.shift[0] = hit->s[0];
        d.shift[1] = hit->s[1];
        g_n_moved++;
      }
    }
    if (dump) {
      const float* m = t_xf;
      TH_LOG("ui draw %d: n=%u vp=(%.0f,%.0f %.0fx%.0f) T=[%.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g] tex=%s rt=%s exec=%d -> %s %.2f/%.2f",
             g_dump_draw, d.count, d.vp_x, d.vp_y, d.vp_w, d.vp_h, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
             tex_desc(t_tex[0]).c_str(), tex_desc(t_rt).c_str(), t_in_exec, hit ? "marker" : "-", d.shift[0], d.shift[1]);
    }
    g_dump_draw++;
    return;
  }
  (void)cl;
}

}  // namespace

bool ui_start(const Image& renderer, const UiEngine& eng) {
  g_ren = renderer;
  g_eng = eng;
  Image ui = module_image(kUi);
  bool ok = ui && sig_match((void*)ui.at(kRvaUiSetConst), kSigUiSetConst) &&
            sig_match((void*)ui.at(kRvaUiBindTex), kSigUiBindTex) && sig_match((void*)ui.at(kRvaUiExecute), kSigUiExecute);
  void* set_rts = module_export(L"d3d_rmdwin10_f.dll", kExpSetRTs, "stereo ui");
  if (ok && set_rts) {
    ok = hook_fn((void*)ui.at(kRvaUiSetConst), (void*)hk_set_const, (void**)&o_set_const, "ui backend set constants") &&
         hook_fn((void*)ui.at(kRvaUiBindTex), (void*)hk_bind_tex, (void**)&o_bind_tex, "ui backend bind texture") &&
         hook_fn((void*)ui.at(kRvaUiExecute), (void*)hk_execute, (void**)&o_execute, "ui backend ExecuteRendering") &&
         hook_fn(set_rts, (void*)hk_set_rts, (void**)&o_set_rts, "d3d setRenderTargets");
  } else {
    ok = false;
  }
  if (Image exe = game_image(); ok && exe && sig_match((void*)exe.at(kRvaHudProject), kSigHudProject)) {
    hook_fn((void*)exe.at(kRvaHudProject), (void*)hk_hud_project, (void**)&o_hud_project, "HUD world-to-screen");
  } else {
    TH_LOG("stereo ui: UI backend or HUD projection (exe+0x5edb50) signature mismatch: markers stay at screen depth");
    g_markers_on = false;
  }
  void** vt = reinterpret_cast<void**>(renderer.at(kRvaRendererVtbl));
  if (sig_match((void*)renderer.at(kRvaCapture), kSigCapture) && vt[kSlotCapture] == (void*)renderer.at(kRvaCapture) &&
      g_eng.copy_region) {
    hook_fn((void*)renderer.at(kRvaCapture), (void*)hk_capture, (void**)&o_capture, "RendererX86 slot 22 (pause capture)");
  } else {
    TH_LOG("stereo ui: RendererX86 slot 22 signature mismatch: the pause background stays mono");
    g_pause_on = false;
  }
  gpu_dup_callback(on_draw);
  return ok;
}

void ui_option(const std::string& id, const std::string& v) {
  bool on = !(v == "0" || v == "false" || v == "off");
  if (id == "stereo.ui_markers") g_markers_on = on;
  else if (id == "stereo.ui_pause") g_pause_on = on;
}

void ui_frame(const UiFrame& f) {
  g_f = f;
  g_f_ok = f.w > 0 && f.m00 > 0;
}

// A world render ends an active pause background. A capture still pending survives a short run of
// world renders: the collectibles menu captures as it opens and keeps rendering the world while it
// animates in, then stops (live r22-r24); the frozen background is then the last pair before the stop.
void ui_pair_rendered() {
  if (g_pause.active) pause_end("a world render");
  else if (g_pause.pending && ++g_pause.pairs_since > 240) pause_end("the world kept rendering after the capture");
}
void ui_pause_cancel(const char* why) { pause_end(why); }

bool ui_pause_sbs() {
  return g_pause_on.load() && (g_pause.pending || g_pause.active) && o_capture && g_f_ok && g_f.pack != 0;
}

bool ui_pause_prepare(void* r, void* sbs_nt, int w, int h) {
  if (!ui_pause_sbs() || !sbs_nt || nt_w(sbs_nt) != 2 * w || nt_h(sbs_nt) != h || !g_eng.copy_region) {
    pause_end("no side-by-side texture");
    return false;
  }
  int fmt = at<int8_t>(sbs_nt, kNtPixelFormat);
  int z[3] = {0, 0, 0}, size[3] = {2 * w, h, 1};
  if (g_pause.pending) {  // the last pair, as presented minus `end`'s gamma blit
    void* t = pause_tex(r, 2 * w, h, fmt);
    if (!t) {
      pause_end("no texture");
      return false;
    }
    g_eng.copy_region(g_eng.native(t), 0, 0, z, sbs_nt, 0, 0, z, size);
    g_pause.pending = false;
    g_pause.active = true;
    g_bg_missing = 0;
    TH_LOG("stereo ui: pause background per eye (%dx%d format %d)", 2 * w, h, fmt);
  }
  if (!g_pause.tex || g_pause.w != 2 * w || g_pause.h != h || g_pause.fmt != fmt) {
    pause_end("size or format changed");
    return false;
  }
  g_pause_target = sbs_nt;  // renderHUD clears it first (+0x474/+0x475, live r20): filled at the bg draw
  return true;
}

void ui_hud_begin(bool sbs, int w) {
  g_hud_sbs = sbs && o_set_const;
  g_hud_w = w;
  g_hud_h = g_f.h;
  g_bg_seen = false;
  g_hud_n++;
  g_pass_ring = int(g_hud_n % kProbeRing);
  g_pm[g_pass_ring].n = 0;
  g_probed = false;
  g_dump_draw = 0;
  g_hud_pause = g_hud_sbs && g_pause.active && g_pause_on.load();
  g_hud_obs = !sbs && g_dump_huds.load() > 0;
  if (g_hud_obs) gpu_draw_observe(true);
  if (g_hud_sbs && !g_hud_pause && o_hud_project) build_markers();
  else g_mk.clear();
  if (g_dump_huds.load() > 0) {
    TH_LOG("ui hud begin frame %llu sbs %d pause %d w %d markers %zu capture %p", (unsigned long long)present_frame(),
           int(sbs), int(g_hud_pause), w, g_mk.size(), g_pause.tex);
    for (auto& m : g_mk)
      TH_LOG("ui marker P (%.2f %.2f %.2f) at (%.1f %.1f)%s z anchor %.2f surface %.2f -> %.2f shift %.2f / %.2f", m.P[0],
             m.P[1], m.P[2], m.x, m.y, m.clamped ? " clamped" : "", m.z_anchor, m.z_surface, m.z, m.s[0], m.s[1]);
  }
  if (!g_mk.empty()) {
    std::lock_guard lk(g_smu);
    g_last_mk = "[";
    for (size_t i = 0; i < g_mk.size() && i < 8; i++) {
      const Marker& m = g_mk[i];
      char b[256];
      snprintf(b, sizeof b,
               "%s{\"x\":%.1f,\"y\":%.1f,\"clamped\":%s,\"z_anchor\":%.2f,\"z_surface\":%.2f,\"z\":%.2f,\"shift\":[%.2f,%.2f]}",
               i ? "," : "", m.x, m.y, m.clamped ? "true" : "false", m.z_anchor, m.z_surface, m.z, m.s[0], m.s[1]);
      g_last_mk += b;
    }
    g_last_mk += "]";
  }
}

void ui_hud_end() {
  // Paused frames without the engine's background draw: the game no longer shows its capture (the
  // first paused frames draw nothing at all, live r5: so only after 30 in a row).
  if (g_hud_pause) {
    g_bg_missing = g_bg_seen ? 0 : g_bg_missing + 1;
    if (g_bg_missing >= 30) pause_end("no background draw for 30 frames");
  }
  if (g_hud_obs) gpu_draw_observe(g_dump_huds.load() > 1);
  g_hud_obs = false;
  g_hud_sbs = false;
  g_hud_pause = false;
  if (g_dump_huds.load() > 0) {
    TH_LOG("ui hud end: %d draws", g_dump_draw);
    g_dump_huds--;
  }
}

std::string ui_status() {
  std::string mk;
  {
    std::lock_guard lk(g_smu);
    mk = g_last_mk.empty() ? "[]" : g_last_mk;
  }
  char b[512];
  snprintf(b, sizeof b,
           "{\"markers_on\":%s,\"pause_on\":%s,\"projections\":%llu,\"markers\":%llu,\"moved_draws\":%llu,\"probes\":%llu,"
           "\"occluded\":%llu,\"pause_captures\":%llu,\"pause_active\":%s,\"bg_skips\":%llu,\"pause_ends\":%llu,\"last\":",
           g_markers_on.load() ? "true" : "false", g_pause_on.load() ? "true" : "false",
           (unsigned long long)g_n_proj.load(), (unsigned long long)g_n_markers.load(),
           (unsigned long long)g_n_moved.load(), (unsigned long long)g_n_probes.load(),
           (unsigned long long)g_n_occluded.load(), (unsigned long long)g_n_capture.load(),
           g_pause.active ? "true" : "false", (unsigned long long)g_n_bg_skips.load(),
           (unsigned long long)g_n_pause_end.load());
  return b + mk + "}";
}

std::string ui_verb(const std::vector<std::string>& a, bool& ok) {
  std::string sub = a.size() > 1 ? a[1] : "status";
  if (sub == "dump") {
    int n = a.size() > 2 ? atoi(a[2].c_str()) : 1;
    g_dump_huds = n;
    return json_str("dumping the next " + std::to_string(n) + " HUD passes to threshold.log");
  }
  if (sub == "status") return ui_status();
  ok = false;
  return json_str("usage: stereo ui [status|dump N]");
}

}  // namespace th::stereo
