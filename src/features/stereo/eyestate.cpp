#include "features/stereo/eyestate.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "features/stereo/engine.h"
#include "core/present.h"
#include "features/stereo/gpu.h"

namespace th::stereo {
namespace {

template <typename T>
T& at(void* base, size_t off) {
  return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + off);
}

// stereo.per_eye: 1 everything per eye; 0 shared state (the measurement control).
std::atomic<int> g_per_eye{1};
std::atomic<int> g_dlss_per_eye{1};  // stereo.dlss_per_eye
std::atomic<int> g_probe{0};
bool g_sbs = false;  // the last frame was a stereo pair
Image g_rimg, g_dimg;
bool g_data_ok = false;  // the renderer / d3d builds are the analysed ones (data RVAs valid)

// ---- T1: the main view's previous camera ---------------------------------------------------------
// Camera motion vectors and every g_m*Previous* uniform come from the main rend::View's stored
// previous matrices. The engine's storePrevious (renderer+0x180700, from slot 18 `end`) copies the
// current world-to-view (+0x20) and view-to-clip (+0xe0) to +0x450 / +0x3d0 and sets +0x4b0, once
// per frame: after a pair that is the right eye's. Per eye: after each eye's render its current
// matrices are kept as that eye's previous; before its next render they are put back, so slot 15's
// own setViewToClip/setViewToWorld (FUN_18017f4a0) derive "previous" from the same eye.
struct ViewPrev {
  double clip[16];
  double w2v[12];
  bool have = false;
};
ViewPrev g_prev[2];
std::atomic<uint64_t> g_n_view_swaps{0};

void view_enter(void* r, int eye) {
  void* v = at<void*>(r, kRMainView);
  if (!v) return;
  ViewPrev& p = g_prev[eye];
  if (p.have) {
    memcpy(&at<double>(v, kViewPrevClip), p.clip, sizeof p.clip);
    memcpy(&at<double>(v, kViewPrevW2V), p.w2v, sizeof p.w2v);
    at<uint8_t>(v, kViewHasPrev) = 1;
  } else {
    at<uint8_t>(v, kViewHasPrev) = 0;  // no previous for this eye yet: a camera cut for it
  }
  g_n_view_swaps++;
}
void view_leave(void* r, int eye) {
  void* v = at<void*>(r, kRMainView);
  if (!v) return;
  ViewPrev& p = g_prev[eye];
  memcpy(p.clip, &at<double>(v, kViewM + kMViewToClip), sizeof p.clip);
  memcpy(p.w2v, &at<double>(v, kViewM + kMWorldToView), sizeof p.w2v);
  p.have = true;
}

// ---- T8: objects' previous transforms (object motion vectors) -------------------------------------
// The graph's Cleanup pass copies every drawn object's transform into its previous one after each
// world render, but only on the last tile of a high-res screenshot (index == count). The left eye
// is rendered as a non-final tile (count = index + 1), so both eyes see last update's transforms as
// "previous" and only the right eye's render advances them; the ambient probe, guarded the same
// way, is updated once per update (by the right eye).
std::atomic<uint64_t> g_n_tile_holds{0};
int g_saved_count = 0;
bool g_tile_held = false;
void tile_enter(int eye) {
  if (eye != 0 || !g_data_ok) return;
  int& idx = *reinterpret_cast<int*>(g_rimg.at(kRvaTileIndex));
  int& cnt = *reinterpret_cast<int*>(g_rimg.at(kRvaTileCount));
  if (idx != cnt) return;  // a real high-res screenshot is running: leave it alone
  g_saved_count = cnt;
  cnt = idx + 1;
  g_tile_held = true;
  g_n_tile_holds++;
}
void tile_leave() {
  if (!g_tile_held) return;
  *reinterpret_cast<int*>(g_rimg.at(kRvaTileCount)) = g_saved_count;
  g_tile_held = false;
}

// ---- T10: DLSS, one feature per eye ---------------------------------------------------------------
// doAntiAliasing (d3d 0x1f990) evaluates DLSSData+0x28 (handle) with DLSSData+0x30 (parameters), and
// sets every input on that block each call; the handle is the only per-feature (history) state. The
// right eye gets a second feature created from the engine's own create parameters (same sizes,
// quality, flags, preset hints) through the game's NGX (its exported entry points) on our command
// list, and swapped in around its render. Re-created when the engine's feature changes; the old one
// released once the GPU is past its last use.
struct Ngx {
  uint32_t (*Alloc)(void**);
  uint32_t (*Destroy)(void*);
  uint32_t (*Create)(void*, int, void*, void**);
  uint32_t (*Release)(void*);
  uint32_t (*GetUI)(void*, const char*, unsigned*);
  uint32_t (*GetI)(void*, const char*, int*);
  void (*SetUI)(void*, const char*, unsigned);
  void (*SetI)(void*, const char*, int);
} N;
bool g_ngx_ok = false;
bool* g_dlss_enabled = nullptr;
struct Feature {
  void* handle = nullptr;
  void* params = nullptr;
  void* engine_handle = nullptr;  // the engine feature it mirrors
  uint32_t key[4]{};
};
Feature g_rf;
struct Retired {
  void* handle;
  void* params;
  uint64_t fence;
};
std::vector<Retired> g_retired;
uint64_t g_last_use_present = 0;
void* g_saved_handle = nullptr;
void* g_saved_params = nullptr;
bool g_dlss_swapped = false;
std::atomic<uint64_t> g_n_dlss_swaps{0}, g_n_dlss_created{0}, g_n_dlss_released{0}, g_n_dlss_fail{0};
uint32_t g_dlss_last_err = 0;
bool ngx_ok(uint32_t r) { return (r & 0xfff00000u) != 0xbad00000u; }

void retire(Feature& f) {
  if (f.handle || f.params) g_retired.push_back({f.handle, f.params, g_last_use_present});
  f = Feature{};
}
void reap(bool all) {
  for (size_t i = 0; i < g_retired.size();) {
    Retired& x = g_retired[i];
    // `fence` is the present count at its last use: the engine's Present waits until the GPU is at
    // most one frame behind (re/dlss.md 2.3), so 3 presents later its last evaluate has executed.
    if (all || present_frame() >= x.fence + 3) {
      uint32_t rr = x.handle ? N.Release(x.handle) : 1;
      if (x.params) N.Destroy(x.params);
      g_n_dlss_released++;
      TH_LOG("stereo dlss: right-eye feature %p released (0x%08x)", x.handle, rr);
      g_retired[i] = g_retired.back();
      g_retired.pop_back();
    } else {
      i++;
    }
  }
}

bool create_right(void* dd) {
  void* ep = at<void*>(dd, kDlssParams);
  if (!ep) return false;
  void* p = nullptr;
  if (!ngx_ok(N.Alloc(&p)) || !p) return false;
  static const char* kUI[] = {"CreationNodeMask", "VisibilityNodeMask", "Width", "Height", "OutWidth", "OutHeight",
                              "DLSS.Hint.Render.Preset.DLAA", "DLSS.Hint.Render.Preset.UltraQuality",
                              "DLSS.Hint.Render.Preset.Quality", "DLSS.Hint.Render.Preset.Balanced",
                              "DLSS.Hint.Render.Preset.Performance", "DLSS.Hint.Render.Preset.UltraPerformance"};
  static const char* kI[] = {"PerfQualityValue", "DLSS.Feature.Create.Flags", "DLSS.Enable.Output.Subrects"};
  for (const char* k : kUI) {
    unsigned v = 0;
    if (N.GetUI(ep, k, &v) == 1) N.SetUI(p, k, v);
  }
  for (const char* k : kI) {
    int v = 0;
    if (N.GetI(ep, k, &v) == 1) N.SetI(p, k, v);
  }
  auto* cl = gpu_list_begin();
  void* h = nullptr;
  uint32_t r = cl ? N.Create(cl, 1, p, &h) : 0xbad00000u;
  if (cl) gpu_list_submit();
  if (!ngx_ok(r) || !h) {
    g_dlss_last_err = r;
    g_n_dlss_fail++;
    N.Destroy(p);
    TH_LOG("stereo dlss: right-eye feature creation failed (0x%08x); DLSS history is shared", r);
    return false;
  }
  g_rf.handle = h;
  g_rf.params = p;
  g_rf.engine_handle = at<void*>(dd, kDlssHandle);
  memcpy(g_rf.key, &at<uint32_t>(dd, kDlssRenderW), sizeof g_rf.key);
  g_n_dlss_created++;
  TH_LOG("stereo dlss: right-eye feature %p (render %ux%u, output %ux%u)", h, g_rf.key[0], g_rf.key[1], g_rf.key[2],
         g_rf.key[3]);
  return true;
}

// The engine's DLSS reset flag (renderer+0x128d1e6): set by `begin` (feature re-created, settings
// changed) and by slot 15 on a camera cut, passed to doAntiAliasing and cleared right after it
// (static: renderer 0x13f581 / 0x1300e6). A reset raised in `begin` would reach only the left eye's
// evaluation; the right eye's feature gets the same reset (slot 15's own cut reset is raised again
// by the right eye's render).
bool g_dlss_reset_seen = false;
std::atomic<uint64_t> g_n_dlss_reset_replay{0};

uint64_t g_fail_backoff = 0;
void dlss_enter(int eye) {
  if (g_data_ok && eye == 0) g_dlss_reset_seen = *reinterpret_cast<uint8_t*>(g_rimg.at(kRvaDlssReset)) != 0;
  if (eye != 1 || !g_ngx_ok || !g_dlss_per_eye.load() || !g_dlss_enabled || !*g_dlss_enabled) return;
  void* dd = *reinterpret_cast<void**>(g_dimg.at(kRvaDlssData));
  if (!dd || !at<void*>(dd, kDlssHandle)) return;
  reap(false);
  bool changed = g_rf.handle && (g_rf.engine_handle != at<void*>(dd, kDlssHandle) ||
                                 memcmp(g_rf.key, &at<uint32_t>(dd, kDlssRenderW), sizeof g_rf.key) != 0);
  if (changed) retire(g_rf);
  if (!g_rf.handle) {
    if (g_fail_backoff && g_fail_backoff-- > 0) return;
    if (!create_right(dd)) {
      g_fail_backoff = 600;
      return;
    }
  }
  g_saved_handle = at<void*>(dd, kDlssHandle);
  g_saved_params = at<void*>(dd, kDlssParams);
  at<void*>(dd, kDlssHandle) = g_rf.handle;
  at<void*>(dd, kDlssParams) = g_rf.params;
  g_dlss_swapped = true;
  g_n_dlss_swaps++;
  if (g_dlss_reset_seen) {
    *reinterpret_cast<uint8_t*>(g_rimg.at(kRvaDlssReset)) = 1;
    g_n_dlss_reset_replay++;
  }
}
void dlss_leave() {
  if (!g_dlss_swapped) return;
  void* dd = *reinterpret_cast<void**>(g_dimg.at(kRvaDlssData));
  if (dd) {
    at<void*>(dd, kDlssHandle) = g_saved_handle;
    at<void*>(dd, kDlssParams) = g_saved_params;
  }
  g_dlss_swapped = false;
  g_last_use_present = present_frame();
}

// ---- T2-T6: histories the right eye keeps separately -----------------------------------------------
// Pointers (and a few small structs) the renderer keeps across frames; the left eye uses the engine's
// own, the right eye a second set swapped in around its render. Every texture in the right set
// comes from the engine's own pool, allocated by the engine's own code when it finds a null (the
// DLF filters, SSAO prepare, the TAA resolve) or falling back when null (volumetric history).
using RtFree_t = void(__fastcall*)(void* mgr, void* tex);
RtFree_t g_free_tex, g_free_depth;
using Prepare_t = void(__fastcall*)(void*);
struct RightSet {
  uint8_t graph[16]{};  // the persisted-resource vector header
  int graph_w = 0, graph_h = 0;
  void* dlf[4]{};
  void* ssao_obj = nullptr;
  void* ssao[6]{};
  void* vol_obj = nullptr;
  void* vol_hist = nullptr;
  uint8_t vol_cam[kVolCameraSize]{};
  void* ssaa_obj = nullptr;
  uint8_t ssaa[kSsaaSlot * kSsaaSlots]{};
  bool ssaa_init = false;
};
RightSet g_rs, g_ls;  // the right eye's set; the engine's (left) values while the right one is in
bool g_rs_in = false;
std::atomic<int> g_hist_per_eye{1};  // stereo.history_per_eye (part of per_eye; a separate control)
// stereo.history_mask (debug): 1 graph persisted, 2 DLF, 4 SSAO pairs, 8 volumetric, 16 TAA ring
std::atomic<int> g_hist_mask{30};  // graph list (1) shared by default: docs/stereo.md, "Render-graph history"
int g_mask_now = 0;  // the mask the current swap used (leave undoes exactly that)
std::atomic<uint64_t> g_n_hist_swaps{0}, g_n_graph_flush{0};
void* rt_manager() { return *reinterpret_cast<void**>(g_rimg.at(kRvaRtManager)); }
void free_tex(void* t) {
  if (t && g_free_tex) g_free_tex(rt_manager(), t);
}

void graph_flush(RightSet& s) {
  auto* data = *reinterpret_cast<uint8_t**>(s.graph);
  uint32_t& n = *reinterpret_cast<uint32_t*>(s.graph + 8);
  for (uint32_t i = 0; i < n && data; i++) {
    uint8_t* e = data + size_t(i) * kGraphEntry;
    void* res = *reinterpret_cast<void**>(e + kGraphEntryRes);
    int type = *reinterpret_cast<int*>(e + kGraphEntryType);
    if (!res) continue;
    if (type == 0) free_tex(res);
    else if (type == 1 && g_free_depth) g_free_depth(rt_manager(), res);
  }
  n = 0;
  g_n_graph_flush++;
}

// The right set's textures back to the pool (sbs ended): what the engine would free for its own.
void right_release() {
  graph_flush(g_rs);
  for (auto& t : g_rs.dlf) free_tex(t), t = nullptr;
  for (auto& t : g_rs.ssao) free_tex(t), t = nullptr;
  free_tex(g_rs.vol_hist);
  g_rs.vol_hist = nullptr;
  if (g_rs.ssaa_init)
    for (size_t i = 0; i < kSsaaSlots; i++) {
      void*& t = *reinterpret_cast<void**>(g_rs.ssaa + i * kSsaaSlot + kSsaaTex);
      free_tex(t);
      t = nullptr;
    }
  g_rs.ssaa_init = false;
}

// Debug (stereo.graph_cross, a bit per persisted entry: 1 lineardepthcopy, 2 colorBufferCopy-2): with
// per-eye lists, that entry's content is taken from the other eye's latest instead (copied into this
// eye's own texture before its render), i.e. shared content with per-eye ownership. For bisecting
// which persisted input a cross-eye artefact comes from.
std::atomic<int> g_graph_cross{0};
constexpr uint64_t kHashLinearDepthCopy = 0xca970354dafdca24ull, kHashColorCopy2 = 0x25e4aa95ba15746cull;
using CopyRegion_t = void(__cdecl*)(void* dst, int, int, const int*, void* src, int, int, const int*, const int*);
CopyRegion_t g_copy_region;
void* graph_find(const uint8_t* hdr, uint64_t hash) {
  auto* data = *reinterpret_cast<uint8_t* const*>(hdr);
  uint32_t n = *reinterpret_cast<const uint32_t*>(hdr + 8);
  for (uint32_t i = 0; i < n && data; i++) {
    uint8_t* e = data + size_t(i) * kGraphEntry;
    if (*reinterpret_cast<uint64_t*>(e) == hash && *reinterpret_cast<int*>(e + kGraphEntryType) == 0)
      return *reinterpret_cast<void**>(e + kGraphEntryRes);
  }
  return nullptr;
}
// Copy the crossed entries' content from list `from` into list `to` (engine-list copies, in order).
void graph_cross(const uint8_t* from, const uint8_t* to) {
  int c = g_graph_cross.load();
  if (!c || !g_copy_region) return;
  const uint64_t hs[2] = {kHashLinearDepthCopy, kHashColorCopy2};
  for (int b = 0; b < 2; b++) {
    if (!(c & (1 << b))) continue;
    void* s = graph_find(from, hs[b]);
    void* d = graph_find(to, hs[b]);
    if (!s || !d || s == d) continue;
    void* sn = gpu_native(s);
    void* dn = gpu_native(d);
    if (!sn || !dn) continue;
    int z[3] = {0, 0, 0};
    g_copy_region(dn, 0, 0, z, sn, 0, 0, z, nullptr);
  }
}

void hist_enter(void* r) {
  if (!g_hist_per_eye.load() || !g_free_tex) return;
  int rw = *reinterpret_cast<int*>(g_rimg.at(kRvaRenderSize)), rh = *reinterpret_cast<int*>(g_rimg.at(kRvaRenderSize + 4));
  if (rw != g_rs.graph_w || rh != g_rs.graph_h) {  // its persisted resources are the old size
    graph_flush(g_rs);
    g_rs.graph_w = rw, g_rs.graph_h = rh;
  }
  int m = g_mask_now = g_hist_mask.load();
  uint8_t* gp = reinterpret_cast<uint8_t*>(g_rimg.at(kRvaGraphPrev));
  if (m & 1) {
    graph_cross(gp, g_rs.graph);  // debug: the left eye's latest into the right list
    memcpy(g_ls.graph, gp, 16);
    memcpy(gp, g_rs.graph, 16);
  }
  for (int i = 0; i < 4 && (m & 2); i++) {
    void*& g = *reinterpret_cast<void**>(g_rimg.at(kRvaDlfHistory[i]));
    g_ls.dlf[i] = g;
    g = g_rs.dlf[i];
  }
  if (void* ss = at<void*>(r, kRSsao); ss && (m & 4)) {
    if (ss != g_rs.ssao_obj) {  // a new SSAO object: the old set belonged to the old one
      for (auto& t : g_rs.ssao) t = nullptr;
      g_rs.ssao_obj = ss;
    }
    for (int i = 0; i < 6; i++) {
      g_ls.ssao[i] = at<void*>(ss, kSsaoPairs[i]);
      at<void*>(ss, kSsaoPairs[i]) = g_rs.ssao[i];
    }
  }
  if (void* vo = at<void*>(r, kRVolumetric); vo && (m & 8)) {
    if (vo != g_rs.vol_obj) {
      g_rs.vol_hist = nullptr;
      memset(g_rs.vol_cam, 0, sizeof g_rs.vol_cam);
      g_rs.vol_obj = vo;
    }
    g_ls.vol_hist = at<void*>(vo, kVolHistory);
    at<void*>(vo, kVolHistory) = g_rs.vol_hist;
    memcpy(g_ls.vol_cam, &at<uint8_t>(vo, kVolCamera), kVolCameraSize);
    memcpy(&at<uint8_t>(vo, kVolCamera), g_rs.vol_cam, kVolCameraSize);
  }
  if (void* sa = at<void*>(r, kRSsaa); sa && (m & 16)) {
    if (sa != g_rs.ssaa_obj || !g_rs.ssaa_init) {  // start as a copy of the left ring, no textures
      memcpy(g_rs.ssaa, &at<uint8_t>(sa, kSsaaRing), sizeof g_rs.ssaa);
      for (size_t i = 0; i < kSsaaSlots; i++) *reinterpret_cast<void**>(g_rs.ssaa + i * kSsaaSlot + kSsaaTex) = nullptr;
      g_rs.ssaa_obj = sa;
      g_rs.ssaa_init = true;
    }
    memcpy(g_ls.ssaa, &at<uint8_t>(sa, kSsaaRing), sizeof g_ls.ssaa);
    memcpy(&at<uint8_t>(sa, kSsaaRing), g_rs.ssaa, sizeof g_rs.ssaa);
  }
  g_rs_in = true;
  g_n_hist_swaps++;
}

void hist_leave(void* r) {
  if (!g_rs_in) return;
  g_rs_in = false;
  int m = g_mask_now;
  uint8_t* gp = reinterpret_cast<uint8_t*>(g_rimg.at(kRvaGraphPrev));
  if (m & 1) {
    memcpy(g_rs.graph, gp, 16);
    memcpy(gp, g_ls.graph, 16);
  }
  for (int i = 0; i < 4 && (m & 2); i++) {
    void*& g = *reinterpret_cast<void**>(g_rimg.at(kRvaDlfHistory[i]));
    g_rs.dlf[i] = g;
    g = g_ls.dlf[i];
  }
  if (void* ss = at<void*>(r, kRSsao); ss && (m & 4) && ss == g_rs.ssao_obj)
    for (int i = 0; i < 6; i++) {
      g_rs.ssao[i] = at<void*>(ss, kSsaoPairs[i]);
      at<void*>(ss, kSsaoPairs[i]) = g_ls.ssao[i];
    }
  if (void* vo = at<void*>(r, kRVolumetric); vo && (m & 8) && vo == g_rs.vol_obj) {
    g_rs.vol_hist = at<void*>(vo, kVolHistory);
    at<void*>(vo, kVolHistory) = g_ls.vol_hist;
    memcpy(g_rs.vol_cam, &at<uint8_t>(vo, kVolCamera), kVolCameraSize);
    memcpy(&at<uint8_t>(vo, kVolCamera), g_ls.vol_cam, kVolCameraSize);
  }
  if (void* sa = at<void*>(r, kRSsaa); sa && (m & 16) && sa == g_rs.ssaa_obj) {
    memcpy(g_rs.ssaa, &at<uint8_t>(sa, kSsaaRing), sizeof g_rs.ssaa);
    memcpy(&at<uint8_t>(sa, kSsaaRing), g_ls.ssaa, sizeof g_ls.ssaa);
  }
}

// What `begin` prepares once per frame and a world render consumes (rend::SSAO's buffers: the RT
// pass frees two of them after each render). Called for the right eye after its set is in place,
// so it also allocates the right set's missing textures.
void ssao_prepare(void* r) {
  void* ss = at<void*>(r, kRSsao);
  if (!ss) return;
  void** vt = *reinterpret_cast<void***>(ss);
  if (vt[1] == (void*)g_rimg.at(kRvaSsaoPrepare)) reinterpret_cast<Prepare_t>(vt[1])(ss);
}

void probe(void* r, int eye, const char* when) {
  if (g_probe.load() <= 0) return;
  void* v = at<void*>(r, kRMainView);
  if (!v) return;
  const double* w2v = &at<double>(v, kViewM + kMWorldToView);
  const double* pw2v = &at<double>(v, kViewPrevW2V);
  void* dd = g_data_ok ? *reinterpret_cast<void**>(g_dimg.at(kRvaDlssData)) : nullptr;
  TH_LOG("stereo probe %s eye %d: w2v.t=(%.4f %.4f %.4f) prev.t=(%.4f %.4f %.4f) has_prev=%u m20=%.5f prev.m20=%.5f "
         "tile=%d/%d dlss=%p counter=%u",
         when, eye, w2v[9], w2v[10], w2v[11], pw2v[9], pw2v[10], pw2v[11], at<uint8_t>(v, kViewHasPrev),
         at<double>(v, kViewM + kMViewToClip + 8 * 8), at<double>(v, kViewPrevClip + 8 * 8),
         g_data_ok ? *reinterpret_cast<int*>(g_rimg.at(kRvaTileIndex)) : -1,
         g_data_ok ? *reinterpret_cast<int*>(g_rimg.at(kRvaTileCount)) : -1, dd ? at<void*>(dd, kDlssHandle) : nullptr,
         *reinterpret_cast<uint32_t*>(g_rimg.at(kRvaFrameCounter)));
  if (std::string(when) == "leave" && eye != 0) g_probe--;
}

uint32_t stamp(const Image& m) {
  auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m.base);
  auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(m.base + dos->e_lfanew);
  return nt->FileHeader.TimeDateStamp;
}

template <typename T>
bool resolve(T& fn, const wchar_t* mod, const char* name) {
  fn = reinterpret_cast<T>(module_export(mod, name, "stereo"));
  return fn != nullptr;
}

}  // namespace

void eyestate_register() {
  cfg_listen("stereo.per_eye", [](const std::string&, const std::string& v) {
    g_per_eye = (v == "0" || v == "false" || v == "off") ? 0 : 1;
  });
  cfg_listen("stereo.history_per_eye", [](const std::string&, const std::string& v) {
    g_hist_per_eye = (v == "0" || v == "false" || v == "off") ? 0 : 1;
  });
  cfg_listen("stereo.history_mask", [](const std::string&, const std::string& v) {
    g_hist_mask = v.empty() ? 30 : atoi(v.c_str());
  });
  cfg_listen("stereo.graph_cross", [](const std::string&, const std::string& v) { g_graph_cross = atoi(v.c_str()); });
  cfg_listen("stereo.dlss_per_eye", [](const std::string&, const std::string& v) {
    g_dlss_per_eye = (v == "0" || v == "false" || v == "off") ? 0 : 1;
  });
}

void eyestate_start(const Image& renderer) {
  g_rimg = renderer;
  g_dimg = module_image(kD3D);
  g_data_ok = g_rimg && g_dimg && stamp(g_rimg) == kRendererStamp && stamp(g_dimg) == kD3DStamp;
  if (!g_data_ok) {
    TH_LOG("stereo: d3d build differs from the analysed one: per-eye object/DLSS state off");
    return;
  }
  g_free_tex = reinterpret_cast<RtFree_t>(module_export(kRenderer, kExpRtFree, "stereo"));
  g_copy_region = reinterpret_cast<CopyRegion_t>(module_export(kD3D, kExpCopyRegion, "stereo"));
  if (g_copy_region && !sig_match((void*)g_copy_region, kSigCopyRegion)) g_copy_region = nullptr;
  g_free_depth = reinterpret_cast<RtFree_t>(module_export(kRenderer, kExpRtFreeDepth, "stereo"));
  if (!g_free_depth || !sig_match((void*)g_free_depth, kSigRtFreeDepth)) g_free_depth = nullptr;
  g_dlss_enabled = reinterpret_cast<bool*>(module_export(kRenderer, kExpDLSSEnabled, "stereo"));
  g_ngx_ok = g_dlss_enabled && resolve(N.Alloc, kD3D, "NVSDK_NGX_D3D12_AllocateParameters") &&
             resolve(N.Destroy, kD3D, "NVSDK_NGX_D3D12_DestroyParameters") &&
             resolve(N.Create, kD3D, "NVSDK_NGX_D3D12_CreateFeature") &&
             resolve(N.Release, kD3D, "NVSDK_NGX_D3D12_ReleaseFeature") &&
             resolve(N.GetUI, kD3D, "NVSDK_NGX_Parameter_GetUI") && resolve(N.GetI, kD3D, "NVSDK_NGX_Parameter_GetI") &&
             resolve(N.SetUI, kD3D, "NVSDK_NGX_Parameter_SetUI") && resolve(N.SetI, kD3D, "NVSDK_NGX_Parameter_SetI");
  if (!g_ngx_ok) TH_LOG("stereo: NGX exports missing: DLSS history shared between eyes");
}

void eyestate_frame(void*, bool sbs) {
  if (sbs && !g_sbs) {  // a pair after mono frames: no eye has a previous of its own yet
    for (auto& p : g_prev) p.have = false;
  }
  g_sbs = sbs;
}

void eyestate_enter(void* r, int eye) {
  if (eye < 0) return;
  bool pe = g_per_eye.load() && g_data_ok;
  if (pe) {
    view_enter(r, eye);
    tile_enter(eye);
    dlss_enter(eye);
    if (eye == 1) hist_enter(r);
    if (eye == 0 && (g_hist_mask.load() & 1))  // debug: the right eye's latest into the engine list
      graph_cross(g_rs.graph, reinterpret_cast<const uint8_t*>(g_rimg.at(kRvaGraphPrev)));
  }
  if (eye == 1 && g_data_ok) ssao_prepare(r);  // needed with or without per-eye state
  probe(r, eye, "enter");
}

void eyestate_leave(void* r, int eye) {
  if (eye < 0) return;
  probe(r, eye, "leave");
  hist_leave(r);
  dlss_leave();
  tile_leave();
  if (!g_per_eye.load()) return;
  view_leave(r, eye);
}

void eyestate_mono(void*) {
  if (g_sbs && g_rf.handle) retire(g_rf);  // sbs ended: the right eye's feature goes
  if (g_sbs && g_data_ok) right_release();
  g_sbs = false;
  if (!g_retired.empty()) reap(false);
}

std::string eyestate_status() {
  char b[768];
  snprintf(b, sizeof b,
           "{\"per_eye\":%d,\"view_swaps\":%llu,\"tile_holds\":%llu,\"hist_swaps\":%llu,\"graph_flush\":%llu,\"dlss\":{\"per_eye\":%d,\"on\":%s,\"right\":\"%p\","
           "\"swaps\":%llu,\"created\":%llu,\"released\":%llu,\"failed\":%llu,\"pending\":%zu,\"err\":\"0x%08x\",\"reset_replay\":%llu}}",
           g_per_eye.load(), (unsigned long long)g_n_view_swaps.load(), (unsigned long long)g_n_tile_holds.load(),
           (unsigned long long)g_n_hist_swaps.load(), (unsigned long long)g_n_graph_flush.load(),
           g_dlss_per_eye.load(), g_dlss_enabled && *g_dlss_enabled ? "true" : "false", g_rf.handle,
           (unsigned long long)g_n_dlss_swaps.load(), (unsigned long long)g_n_dlss_created.load(),
           (unsigned long long)g_n_dlss_released.load(), (unsigned long long)g_n_dlss_fail.load(), g_retired.size(),
           g_dlss_last_err, (unsigned long long)g_n_dlss_reset_replay.load());
  return b;
}

void eyestate_probe(int n) { g_probe = n; }

namespace {
std::string graph_json(const uint8_t* hdr) {
  auto* data = *reinterpret_cast<uint8_t* const*>(hdr);
  uint32_t n = *reinterpret_cast<const uint32_t*>(hdr + 8), cap = *reinterpret_cast<const uint32_t*>(hdr + 12);
  char b[256];
  snprintf(b, sizeof b, "{\"data\":\"%p\",\"count\":%u,\"cap\":\"0x%x\",\"entries\":[", data, n, cap);
  std::string s = b;
  for (uint32_t i = 0; i < n && data && i < 32; i++) {
    uint8_t* e = data + size_t(i) * kGraphEntry;
    void* res = *reinterpret_cast<void**>(e + kGraphEntryRes);
    int type = *reinterpret_cast<int*>(e + kGraphEntryType);
    snprintf(b, sizeof b, "%s{\"hash\":\"%016llx\",\"type\":%d,\"flags\":\"0x%x\",\"res\":\"%p\",\"tex\":\"%s\"}", i ? "," : "",
             (unsigned long long)*reinterpret_cast<uint64_t*>(e), type, *reinterpret_cast<uint32_t*>(e + 0x8c), res,
             type == 0 && res ? gpu_describe(gpu_native(res)).c_str() : "");
    s += b;
  }
  return s + "]}";
}
}  // namespace

std::string eyestate_graph() {
  if (!g_data_ok) return "null";
  const uint8_t* gp = reinterpret_cast<const uint8_t*>(g_rimg.at(kRvaGraphPrev));
  return "{\"in\":" + std::string(g_rs_in ? "\"right\"" : "\"engine\"") + ",\"engine\":" + graph_json(gp) +
         ",\"right\":" + graph_json(g_rs.graph) + "}";
}

void* eyestate_texture(const std::string& kind, int i) {
  if (!g_data_ok || g_rs_in || i < 0) return nullptr;
  if (kind == "gl" || kind == "gr") {
    const uint8_t* hdr = kind == "gl" ? reinterpret_cast<const uint8_t*>(g_rimg.at(kRvaGraphPrev)) : g_rs.graph;
    auto* data = *reinterpret_cast<uint8_t* const*>(hdr);
    uint32_t n = *reinterpret_cast<const uint32_t*>(hdr + 8);
    if (!data || uint32_t(i) >= n) return nullptr;
    uint8_t* e = data + size_t(i) * kGraphEntry;
    if (*reinterpret_cast<int*>(e + kGraphEntryType) != 0) return nullptr;
    return gpu_native(*reinterpret_cast<void**>(e + kGraphEntryRes));
  }
  if ((kind == "dlfL" || kind == "dlfR") && i < 4) {
    void* t = kind == "dlfL" ? *reinterpret_cast<void**>(g_rimg.at(kRvaDlfHistory[i])) : g_rs.dlf[i];
    return t ? gpu_native(t) : nullptr;
  }
  if ((kind == "ssaoL" || kind == "ssaoR") && i < 6) {
    void* t = kind == "ssaoR" ? g_rs.ssao[i] : g_rs.ssao_obj ? at<void*>(g_rs.ssao_obj, kSsaoPairs[i]) : nullptr;
    return t ? gpu_native(t) : nullptr;
  }
  return nullptr;
}

}  // namespace th::stereo
