#include "features/fg/fg.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_defs_dlssg.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/config.h"
#include "core/hook.h"
#include "core/lab.h"
#include "core/log.h"
#include "core/module.h"
#include "core/present.h"
#include "features/fg/common.h"
#include "features/fg/engine.h"
#include "features/fg/hdr.h"
#include "features/fg/latency.h"
#include "features/fg/reflex.h"
#include "features/fg/trace.h"

namespace th::fg {

bool make_tex(ID3D12Device* dev, Tex& t, D3D12_RESOURCE_DESC d, D3D12_RESOURCE_STATES st, const wchar_t* name) {
  safe_release(t.r);
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  d.Alignment = 0;
  HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, nullptr, __uuidof(ID3D12Resource), (void**)&t.r);
  if (FAILED(hr)) {
    TH_LOG("fg: CreateCommittedResource(%ls %llux%u fmt %d flags 0x%x) = 0x%lx", name, (unsigned long long)d.Width,
           d.Height, int(d.Format), int(d.Flags), hr);
    t.r = nullptr;
    return false;
  }
  t.r->SetName(name);
  t.st = st;
  return true;
}

bool fence_wait(ID3D12Fence* fence, uint64_t v, HANDLE ev, DWORD ms) {
  if (!fence) return false;
  if (fence->GetCompletedValue() >= v) return true;
  const int64_t deadline = now_us() + int64_t(ms) * 1000;
  for (;;) {
    fence->SetEventOnCompletion(v, ev);
    if (fence->GetCompletedValue() >= v) return true;
    int64_t left = deadline - now_us();
    if (left <= 0) return false;
    WaitForSingleObject(ev, DWORD(std::max<int64_t>(1, left / 1000)));
    if (fence->GetCompletedValue() >= v) return true;  // else a stale wake: register again
  }
}

bool Ring::init(ID3D12Device* dev, const wchar_t* name) {
  if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&fence))) return false;
  fence->SetName(name);
  ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  for (int i = 0; i < kN; ++i) {
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&alloc[i])) ||
        FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[i], nullptr, __uuidof(ID3D12GraphicsCommandList),
                                      (void**)&list[i])))
      return false;
    list[i]->Close();
  }
  return true;
}
void Ring::wait(uint64_t v) {
  if (v) fence_wait(fence, v, ev, 2000);
}
ID3D12GraphicsCommandList* Ring::begin() {
  cur = pos;
  pos = (pos + 1) % kN;
  wait(val[cur]);
  alloc[cur]->Reset();
  list[cur]->Reset(alloc[cur], nullptr);
  return list[cur];
}
uint64_t Ring::submit(ID3D12CommandQueue* q) {
  ID3D12GraphicsCommandList* cl = list[cur];
  cl->Close();
  ID3D12CommandList* l = cl;
  q->ExecuteCommandLists(1, &l);
  q->Signal(fence, ++next);
  val[cur] = next;
  return next;
}

namespace {

constexpr wchar_t kD3D[] = L"d3d_rmdwin10_f.dll";
constexpr unsigned kMaxGen = 5;  // 6x: DLSSG.MultiFrameCountMax is 5 on RTX 50 with nvngx_dlssg 310.9.16
constexpr unsigned kSets = 2;
const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

bool ngx_failed(uint32_t r) { return (r & 0xFFF00000u) == 0xBAD00000u; }


// ---- the game's NGX (the SDK is linked statically into d3d_rmdwin10_f.dll and exported) ----------

struct Ngx {
  uint32_t (*Alloc)(void**);
  uint32_t (*Destroy)(void*);
  uint32_t (*Caps)(void**);
  uint32_t (*Create)(ID3D12GraphicsCommandList*, int, void*, void**);
  uint32_t (*Release)(void*);
  void (*SetUI)(void*, const char*, unsigned);
  void (*SetI)(void*, const char*, int);
  void (*SetF)(void*, const char*, float);
  void (*SetPtr)(void*, const char*, void*);
  void (*SetRes)(void*, const char*, ID3D12Resource*);
  uint32_t (*GetUI)(void*, const char*, unsigned*);
  uint32_t (*GetI)(void*, const char*, int*);
  uint32_t (*GetF)(void*, const char*, float*);
  uint32_t (*GetRes)(void*, const char*, ID3D12Resource**);
} N;

using Eval_t = uint32_t(__cdecl*)(ID3D12GraphicsCommandList*, const void*, const void*, void*);
Eval_t o_Eval;  // NVSDK_NGX_D3D12_EvaluateFeature_C, unhooked
using GetAutoBackBuffer_t = void*(__cdecl*)();
GetAutoBackBuffer_t g_auto_bb;  // d3d::DeviceUtil::getAutoBackBuffer: NativeTexture*, resource at +0x88

template <typename T>
bool resolve(T& fn, const char* name) {
  fn = reinterpret_cast<T>(module_export(kD3D, name, "fg"));
  return fn != nullptr;
}

bool resolve_ngx() {
  return resolve(N.Alloc, "NVSDK_NGX_D3D12_AllocateParameters") && resolve(N.Destroy, "NVSDK_NGX_D3D12_DestroyParameters") &&
         resolve(N.Caps, "NVSDK_NGX_D3D12_GetCapabilityParameters") && resolve(N.Create, "NVSDK_NGX_D3D12_CreateFeature") &&
         resolve(N.Release, "NVSDK_NGX_D3D12_ReleaseFeature") && resolve(N.SetUI, "NVSDK_NGX_Parameter_SetUI") &&
         resolve(N.SetI, "NVSDK_NGX_Parameter_SetI") && resolve(N.SetF, "NVSDK_NGX_Parameter_SetF") &&
         resolve(N.SetPtr, "NVSDK_NGX_Parameter_SetVoidPointer") &&
         resolve(N.SetRes, "NVSDK_NGX_Parameter_SetD3d12Resource") && resolve(N.GetUI, "NVSDK_NGX_Parameter_GetUI") &&
         resolve(N.GetI, "NVSDK_NGX_Parameter_GetI") && resolve(N.GetF, "NVSDK_NGX_Parameter_GetF") &&
         resolve(N.GetRes, "NVSDK_NGX_Parameter_GetD3d12Resource") &&
         resolve(g_auto_bb, "?getAutoBackBuffer@DeviceUtil@d3d@@SAPEAVNativeTexture@2@XZ");
}

// ---- our textures and command lists ------------------------------------------------------------

// ---- state ----------------------------------------------------------------------------------------

struct Inputs {
  bool ok = false;
  unsigned rw = 0, rh = 0;
  float jx = 0, jy = 0, mvx = 1, mvy = 1;
  bool reset = false;
  Camera cam;  // the main view at the DLSS evaluate tap (same frame as the jitter and MVs)
};

struct Set {
  // depth / mv / in: [0] the frame's only render, or the left eye of a stereo pair; [1] the right eye.
  Tex real, depth[2], mv[2], out[kMaxGen];
  // HDR (FP16 scRGB swapchain): DLSS-G works on HDR10 PQ copies (hdr.cpp). enc / hud_enc: the real
  // frame and the HUD-less colour encoded; dec: the generated frames decoded back to scRGB (presented).
  Tex enc, hud_enc, dec[kMaxGen];
  D3D12_RESOURCE_DESC depth_desc[2]{}, mv_desc[2]{};
  Inputs in[2];
};

// One DLSS-G evaluation's share of the frame: the whole frame, or one eye's half of a side-by-side
// frame (stereo). Sub-rectangles in pixels; y is always 0 and the height the frame's.
struct View {
  unsigned bx = 0, bw = 0;     // backbuffer / HUD-less / outputs
  unsigned hx = 0, hw = 0;     // HUD-less (its own texture may be one eye wide)
  bool hud = false;
  unsigned dx = 0, dw = 0;     // depth and MVs (render pixels)
  int crop_p0 = 0, crop_wc = 0, crop_w = 0;  // the eye image columns shown (camera crop); wc 0 = all
  unsigned eye = 0;            // inputs / feature index
  unsigned fw = 0;             // the feature's width (= bw: DLSS-G wants the sub-rectangle at its size)
};

// A DLSS-G feature: one for mono, one per eye with stereo.
struct Feature {
  void* h = nullptr;
  unsigned w = 0, hh = 0, rw = 0, rh = 0, gen = 0;
  DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
  bool reset = true;
};

struct Job {
  unsigned set = 0, n_gen = 0;
  uint64_t eval_v = 0;
  unsigned sync = 0, flags = 0;
  int64_t arrive_us = 0;
  int64_t step_us = 0;  // real-frame interval / multiplier
  int pace = 1;         // 0 ready, 1 smooth (default: better in every A/B pair, docs/fg.md 4)
  double relax = 0.5;
  uint64_t id = 0;
  uint64_t frame = 0;  // S.frame of this job (latency rows)
};

struct State {
  bool enabled = false;  // [fg] enabled at start: proxies + present thread
  std::atomic<unsigned> count{2};        // [fg] count (presents per rendered frame) when not auto
  std::atomic<bool> count_auto{false};    // [fg] count = auto
  std::atomic<unsigned> count_min{2}, count_max{4};
  std::atomic<double> target_hz{0};       // [fg] target_hz: presents/s to aim for (0 = the display refresh)
  std::atomic<double> fps_limit{0};       // [fg] fps_limit: rendered frames/s cap (0 = none; auto sets its own)
  std::atomic<bool> auto_cap{false};      // [fg] auto_cap: headroom cap (Pharos), with the refresh cap
  std::atomic<double> auto_cap_lag{0.7};  // [fg] auto_cap_lag: DLSS-G done after the game's Present, in frames
  double head = 0;                        // the headroom cap (fps; 0 = not yet set)
  int64_t fg_since = 0;                   // first frame of the current run of generated frames
  unsigned aw_joins = 0;                  // renderer-thread waits for the present thread in this window
  double refresh_hz = 0;                  // detected display refresh
  unsigned auto_count = 2;                // count = auto: the current multiplier
  // limiter (renderer thread, end of takeover) and the frame's own work time
  int64_t lim_next = 0, lim_sleep_us = 0;
  double cap_now = 0;
  // count = auto controller windows
  int64_t aw_start = 0, aw_work = 0;
  std::atomic<int64_t> aw_lag_sum{0}, aw_lag_n{0};  // DLSS-G done after the game's Present (window)
  unsigned aw_frames = 0;
  int up_streak = 0, down_streak = 0;
  int64_t last_up_us = 0, last_down_us = 0, down_block_until = 0, down_block_s = 10;
  std::atomic<bool> on{true};  // live toggle ([fg] active)
  enum Show { kAll, kGenerated, kReal, kHudless };
  std::atomic<int> show{kAll};  // all | generated | real | hudless (the HUD-less copy in place of real frames)
  bool engine_ok = false;               // engine.cpp resolved (camera, HUD-less)
  std::atomic<bool> cam_engine{true};   // [fg] camera = engine | identity
  std::atomic<bool> hudless{true};      // [fg] hudless

  ID3D12Device* dev = nullptr;
  ID3D12CommandQueue* q = nullptr;
  IDXGISwapChain3* sc = nullptr;
  DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
  UINT w = 0, h = 0;
  Tex proxy[2];
  ID3D12Resource* bb[8]{};
  UINT nbb = 0;

  Ring rring, pring;  // render thread, present thread
  Set sets[kSets];
  unsigned frame = 0;  // game presents since start, picks the set

  bool ngx_ok = false, ngx_tried = false;
  Feature feat[2];  // [1]: the right eye's (stereo)
  void* cparams = nullptr;
  void* eparams = nullptr;
  unsigned caps_max_gen = 1;
  bool hdr = false;  // the swapchain is FP16 scRGB: DLSS-G runs on HDR10 PQ copies
  bool reset = true;
  uint64_t bb_id = 0;

  // stereo (fg_stereo_*): the link, and the layout stereo's `end` reported for the frame being presented
  FgStereoLink link{};
  struct Layout {
    bool on = false;
    int pack = 0, w = 0, shift[2] = {0, 0};
    bool pair = false;
  } lay, cur;  // lay: as reported (renderer thread, stereo's `end`); cur: the frame takeover works on
  std::atomic<unsigned> p_views2{0};  // frames evaluated per eye (perf window)
  std::atomic<bool> stereo_shared{false};  // dev [fg] stereo_features = 1: one feature (history) for both eyes
  std::atomic<uint64_t> n_views2{0}, n_views2_mono{0};

  // present thread
  std::mutex mu;
  std::condition_variable cv, cv_done;
  std::deque<Job> jobs;
  uint64_t jobs_done = 0, jobs_posted = 0;
  bool busy = false;
  int64_t interval_us = 16667;  // real frame interval EMA
  int64_t last_arrive_us = 0;
  int64_t last_present_us = 0;  // our last Present returned
  int64_t last_call_us = 0;     // our last Present called
  int64_t last_real_call_us = 0;  // our last real frame's Present called
  std::atomic<int> pace{1};       // [fg] pace: 0 ready, 1 smooth
  std::atomic<double> relax{0.5}; // [fg] pace_relax
  int64_t join_us = 0;            // the renderer thread's last wait for the present thread
  bool chain = false;             // the previous game Present was handled with frame generation

  // perf
  std::atomic<unsigned> p_real{0}, p_gen{0}, p_fail{0}, p_noinput{0}, p_drop{0}, p_skip{0}, p_join{0};
  std::atomic<int64_t> lag_sum{0}, lag_n{0}, join_sum{0};  // DLSS-G done after the game's Present; joins
  int64_t perf_last = 0;
  std::mutex perf_mu;
  std::string perf_json = "null";
} S;

// ---- input tap: after the game's DLSS SR evaluation ------------------------------------------------

uint32_t __cdecl hk_Eval(ID3D12GraphicsCommandList* cl, const void* handle, const void* params, void* cb) {
  uint32_t r = o_Eval(cl, handle, params, cb);
  if (!S.enabled || !S.dev || handle == S.feat[0].h || handle == S.feat[1].h || ngx_failed(r) || !params) return r;
  void* p = const_cast<void*>(params);
  ID3D12Resource* depth = nullptr;
  ID3D12Resource* mv = nullptr;
  if (ngx_failed(N.GetRes(p, NVSDK_NGX_Parameter_Depth, &depth)) || !depth ||
      ngx_failed(N.GetRes(p, NVSDK_NGX_Parameter_MotionVectors, &mv)) || !mv)
    return r;
  // Stereo: the eye whose world render this is (each eye has its own SR feature, depth, MVs and
  // camera); else the frame's only render.
  const int eye = S.link.eye ? S.link.eye() : -1;
  const unsigned e = eye == 1 ? 1 : 0;
  Set& st = S.sets[S.frame % kSets];
  Inputs& in = st.in[e];
  if (eye < 0) st.in[1].ok = false;
  unsigned rw = 0, rh = 0;
  N.GetUI(p, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &rw);
  N.GetUI(p, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &rh);
  D3D12_RESOURCE_DESC dd = depth->GetDesc(), md = mv->GetDesc();
  in.rw = rw ? rw : unsigned(dd.Width);
  in.rh = rh ? rh : dd.Height;
  N.GetF(p, NVSDK_NGX_Parameter_Jitter_Offset_X, &in.jx);
  N.GetF(p, NVSDK_NGX_Parameter_Jitter_Offset_Y, &in.jy);
  N.GetF(p, NVSDK_NGX_Parameter_MV_Scale_X, &in.mvx);
  N.GetF(p, NVSDK_NGX_Parameter_MV_Scale_Y, &in.mvy);
  int reset = 0;
  N.GetI(p, NVSDK_NGX_Parameter_Reset, &reset);
  in.reset = reset != 0;
  auto same = [](const D3D12_RESOURCE_DESC& a, const D3D12_RESOURCE_DESC& b) {
    return a.Width == b.Width && a.Height == b.Height && a.Format == b.Format && a.Flags == b.Flags;
  };
  if (!st.depth[e].r || !same(st.depth_desc[e], dd)) {
    if (!make_tex(S.dev, st.depth[e], dd, D3D12_RESOURCE_STATE_COPY_DEST, e ? L"threshold fg depth R" : L"threshold fg depth")) return r;
    st.depth_desc[e] = dd;
    TH_LOG("fg: depth input%s %llux%u fmt %d flags 0x%x, motion vectors %llux%u fmt %d", e ? " (right eye)" : "",
           (unsigned long long)dd.Width, dd.Height, int(dd.Format), int(dd.Flags), (unsigned long long)md.Width,
           md.Height, int(md.Format));
  }
  if (!st.mv[e].r || !same(st.mv_desc[e], md)) {
    if (!make_tex(S.dev, st.mv[e], md, D3D12_RESOURCE_STATE_COPY_DEST, e ? L"threshold fg mv R" : L"threshold fg mv")) return r;
    st.mv_desc[e] = md;
  }
  // NGX takes its inputs as shader resources; the engine leaves them so (re/dlss.md 1.5).
  const auto kSrv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  barrier(cl, depth, kSrv, D3D12_RESOURCE_STATE_COPY_SOURCE);
  barrier(cl, mv, kSrv, D3D12_RESOURCE_STATE_COPY_SOURCE);
  to(cl, st.depth[e], D3D12_RESOURCE_STATE_COPY_DEST);
  to(cl, st.mv[e], D3D12_RESOURCE_STATE_COPY_DEST);
  cl->CopyResource(st.depth[e].r, depth);
  cl->CopyResource(st.mv[e].r, mv);
  barrier(cl, depth, D3D12_RESOURCE_STATE_COPY_SOURCE, kSrv);
  barrier(cl, mv, D3D12_RESOURCE_STATE_COPY_SOURCE, kSrv);
  in.ok = true;
  if (!camera_read(in.cam)) in.cam.ok = false;
  if (e == 0) lat_eval(cl, in.cam, S.frame);
  hudless_arm(S.engine_ok && S.hudless.load());  // this frame has DLSS inputs: copy its HUD-less colour
  static int logged = 0;
  if (logged++ < (S.link.eye ? 4 : 2))
    TH_LOG("fg: inputs%s render %ux%u jitter %.3f,%.3f px, MV scale %.1f,%.1f", eye < 0 ? "" : e ? " (right eye)" : " (left eye)",
           in.rw, in.rh, in.jx, in.jy, in.mvx, in.mvy);
  return r;
}

// ---- NGX DLSS-G -------------------------------------------------------------------------------------

bool ensure_ngx() {
  if (S.ngx_tried) return S.ngx_ok;
  S.ngx_tried = true;
  void* caps = nullptr;
  if (ngx_failed(N.Caps(&caps)) || !caps) {
    TH_LOG("fg: NGX capability parameters unavailable (is the game's DLSS initialised?)");
    return false;
  }
  int avail = -1;
  unsigned max_gen = 0;
  N.GetI(caps, NVSDK_NGX_Parameter_FrameGeneration_Available, &avail);
  N.GetUI(caps, NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &max_gen);
  S.caps_max_gen = std::max(1u, std::min(max_gen, kMaxGen));
  TH_LOG("fg: FrameGeneration available=%d, MultiFrameCountMax=%u (up to %ux)", avail, max_gen, S.caps_max_gen + 1);
  if (avail != 1) return false;
  N.Alloc(&S.cparams);
  N.Alloc(&S.eparams);
  S.ngx_ok = S.cparams && S.eparams;
  return S.ngx_ok;
}

// The format DLSS-G sees: the swapchain's, or HDR10 (R10G10B10A2 PQ) for an scRGB swapchain.
DXGI_FORMAT ngx_fmt() { return S.hdr ? DXGI_FORMAT_R10G10B10A2_UNORM : S.fmt; }

// Feature `i` for a `w` x S.h sub-rectangle (the whole frame, or one eye) at render size rw x rh.
bool ensure_feature(unsigned i, unsigned w, unsigned rw, unsigned rh) {
  Feature& f = S.feat[i];
  unsigned gen = S.caps_max_gen;  // created for the largest count; evaluations may use fewer
  if (f.h && f.w == w && f.hh == S.h && f.rw == rw && f.rh == rh && f.gen == gen && f.fmt == ngx_fmt()) return true;
  if (f.h) {
    S.rring.wait(S.rring.next);
    N.Release(f.h);
    f.h = nullptr;
  }
  void* p = S.cparams;
  N.SetUI(p, NVSDK_NGX_Parameter_CreationNodeMask, 1);
  N.SetUI(p, NVSDK_NGX_Parameter_VisibilityNodeMask, 1);
  N.SetUI(p, NVSDK_NGX_Parameter_Width, w);
  N.SetUI(p, NVSDK_NGX_Parameter_Height, S.h);
  N.SetUI(p, NVSDK_NGX_DLSSG_Parameter_Width, w);
  N.SetUI(p, NVSDK_NGX_DLSSG_Parameter_Height, S.h);
  N.SetUI(p, NVSDK_NGX_DLSSG_Parameter_BackbufferFormat, unsigned(ngx_fmt()));
  N.SetUI(p, NVSDK_NGX_DLSSG_Parameter_MultiFrameCount, gen);
  N.SetUI(p, NVSDK_NGX_DLSSG_Parameter_InternalWidth, rw);
  N.SetUI(p, NVSDK_NGX_DLSSG_Parameter_InternalHeight, rh);
  N.SetUI(p, NVSDK_NGX_DLSSG_Parameter_DynamicResolution, 0);
  int64_t t0 = now_us();
  ID3D12GraphicsCommandList* cl = S.rring.begin();
  uint32_t r = N.Create(cl, NVSDK_NGX_Feature_FrameGeneration, p, &f.h);
  S.rring.wait(S.rring.submit(S.q));
  TH_LOG("fg: CreateFeature(FrameGeneration%s %ux%u, render %ux%u, format %d, count %u) = 0x%08x (%.0f ms)",
         i ? " right eye" : "", w, S.h, rw, rh, int(ngx_fmt()), gen, r, (now_us() - t0) / 1000.0);
  if (ngx_failed(r) || !f.h) {
    f.h = nullptr;
    return false;
  }
  f.w = w, f.hh = S.h, f.rw = rw, f.rh = rh, f.gen = gen, f.fmt = ngx_fmt();
  f.reset = true;
  return true;
}

// The stereo right eye's feature when it isn't used (stereo off or one view): released after the GPU
// is done with it, once.
void release_feature(unsigned i) {
  Feature& f = S.feat[i];
  if (!f.h) return;
  S.rring.wait(S.rring.next);
  N.Release(f.h);
  TH_LOG("fg: DLSS-G feature %u released", i);
  f = Feature{};
}

bool ensure_outputs(Set& st, unsigned n) {
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = S.w;
  d.Height = S.h;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.Format = S.fmt;
  d.SampleDesc.Count = 1;
  if (!st.real.r) {
    d.Flags = D3D12_RESOURCE_FLAG_NONE;
    if (!make_tex(S.dev, st.real, d, D3D12_RESOURCE_STATE_COPY_DEST, L"threshold fg real")) return false;
  }
  d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  if (S.hdr) {
    // scRGB decoded generated frames (presented), then the HDR10 textures DLSS-G works on.
    for (unsigned i = 0; i < n; ++i)
      if (!st.dec[i].r && !make_tex(S.dev, st.dec[i], d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"threshold fg generated scRGB"))
        return false;
    d.Format = ngx_fmt();
    if (n && !st.enc.r && !make_tex(S.dev, st.enc, d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"threshold fg real PQ")) return false;
    if (n && !st.hud_enc.r && !make_tex(S.dev, st.hud_enc, d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"threshold fg HUD-less PQ"))
      return false;
  }
  d.Format = ngx_fmt();
  for (unsigned i = 0; i < n; ++i)
    if (!st.out[i].r && !make_tex(S.dev, st.out[i], d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"threshold fg generated"))
      return false;
  return true;
}

// The feature view v evaluates with (dev [fg] stereo_features = 1: the left eye's for both).
unsigned fi(unsigned v) { return S.stereo_shared.load() ? 0 : v; }

// Records copy-aside + DLSS-G evaluation for this frame (one evaluation per view and generated index);
// returns the number of generated frames.
unsigned evaluate(Set& st, ID3D12Resource* proxy, unsigned n_gen, const EngTex* hud, const View* views, unsigned nv) {
  ID3D12GraphicsCommandList* cl = S.rring.begin();
  lat_begin(cl, S.frame);
  // The real frame, set aside (the engine renders into this proxy again two frames later).
  barrier(cl, proxy, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
  to(cl, st.real, D3D12_RESOURCE_STATE_COPY_DEST);
  cl->CopyResource(st.real.r, proxy);
  barrier(cl, proxy, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
  if (!n_gen) {
    to(cl, st.real, D3D12_RESOURCE_STATE_COPY_SOURCE);
    return 0;
  }
  const auto kSrv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  to(cl, st.real, kSrv);
  for (unsigned v = 0; v < nv; ++v) {
    to(cl, st.depth[views[v].eye], kSrv);
    to(cl, st.mv[views[v].eye], kSrv);
  }
  void* p = S.eparams;
  const unsigned slot0 = unsigned(&st - S.sets) * 8;  // hdr.cpp descriptor slots of this set
  if (S.hdr) {  // scRGB -> HDR10 PQ for DLSS-G
    to(cl, st.enc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    hdr_convert(cl, st.real.r, st.enc.r, true, slot0);
    to(cl, st.enc, kSrv);
  }
  Tex& input = S.hdr ? st.enc : st.real;  // what DLSS-G reads as the backbuffer
  // HUD-less colour (engine.cpp): the colour target before the UI, in the engine's tracked state.
  ID3D12Resource* hl = nullptr;
  if (hud) {
    barrier(cl, hud->r, hud->st, kSrv);
    hl = hud->r;
    if (S.hdr) {  // the HUD-less colour is scRGB too (checked at the take): encoded like the real frame
      to(cl, st.hud_enc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      hdr_convert(cl, hud->r, st.hud_enc.r, true, slot0 + 1);
      to(cl, st.hud_enc, kSrv);
      hl = st.hud_enc.r;
    }
  }
  for (unsigned i = 0; i < n_gen; ++i) to(cl, st.out[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  // Camera per view: the engine's main view read in that view's own render ([fg] camera = engine,
  // engine.cpp; cropped to the columns the view shows), else identity matrices and a synthetic basis
  // (an all-zero basis is "not a valid frame" for DLSS-G).
  static Camera cam[2];  // NGX keeps the pointers until the evaluation is recorded
  bool use_cam[2] = {false, false};
  for (unsigned v = 0; v < nv; ++v) {
    const Inputs& in = st.in[views[v].eye];
    use_cam[v] = S.cam_engine.load() && in.cam.ok;
    if (use_cam[v]) camera_crop(in.cam, views[v].crop_p0, views[v].crop_wc, views[v].crop_w, cam[v]);
  }
  bool reset[2];
  for (unsigned v = 0; v < nv; ++v) {
    const Inputs& in = st.in[views[v].eye];
    reset[v] = S.reset || S.feat[fi(v)].reset || in.reset || (use_cam[v] && !cam[v].has_prev);
  }
  unsigned made = n_gen;
  for (unsigned i = 1; i <= n_gen && made == n_gen; ++i) {
    for (unsigned v = 0; v < nv; ++v) {
      const View& vw = views[v];
      const Inputs& in = st.in[vw.eye];
      N.SetRes(p, NVSDK_NGX_DLSSG_Parameter_Backbuffer, input.r);
      N.SetRes(p, NVSDK_NGX_DLSSG_Parameter_Depth, st.depth[vw.eye].r);
      N.SetRes(p, NVSDK_NGX_DLSSG_Parameter_MVecs, st.mv[vw.eye].r);
      N.SetRes(p, NVSDK_NGX_DLSSG_Parameter_HUDLess, hl && vw.hud ? hl : nullptr);
      if (hl && vw.hud) {
        N.SetUI(p, NVSDK_NGX_DLSSG_Parameter_HUDLessSubrectBaseX, vw.hx);
        N.SetUI(p, NVSDK_NGX_DLSSG_Parameter_HUDLessSubrectBaseY, 0);
        N.SetUI(p, NVSDK_NGX_DLSSG_Parameter_HUDLessSubrectWidth, vw.hw);
        N.SetUI(p, NVSDK_NGX_DLSSG_Parameter_HUDLessSubrectHeight, S.h);
      }
      const Camera& c = cam[v];
      if (use_cam[v]) {
        N.SetPtr(p, NVSDK_NGX_DLSSG_Parameter_CameraViewToClip, const_cast<float*>(c.view_to_clip));
        N.SetPtr(p, NVSDK_NGX_DLSSG_Parameter_ClipToCameraView, const_cast<float*>(c.clip_to_view));
        N.SetPtr(p, NVSDK_NGX_DLSSG_Parameter_ClipToPrevClip, const_cast<float*>(c.clip_to_prev));
        N.SetPtr(p, NVSDK_NGX_DLSSG_Parameter_PrevClipToClip, const_cast<float*>(c.prev_to_clip));
        N.SetPtr(p, NVSDK_NGX_DLSSG_Parameter_ClipToLensClip, const_cast<float*>(kIdentity));
        if (i == 1 && v == 0) camera_log(c);
      } else {
        for (const char* m : {NVSDK_NGX_DLSSG_Parameter_CameraViewToClip, NVSDK_NGX_DLSSG_Parameter_ClipToCameraView,
                              NVSDK_NGX_DLSSG_Parameter_ClipToLensClip, NVSDK_NGX_DLSSG_Parameter_ClipToPrevClip,
                              NVSDK_NGX_DLSSG_Parameter_PrevClipToClip})
          N.SetPtr(p, m, const_cast<float*>(kIdentity));
      }
      const float synth[4][3] = {{0, 0, 0}, {0, 1, 0}, {1, 0, 0}, {0, 0, 1}};  // pos, up, right, forward
      const char* names[4][3] = {
          {NVSDK_NGX_DLSSG_Parameter_CameraPosX, NVSDK_NGX_DLSSG_Parameter_CameraPosY, NVSDK_NGX_DLSSG_Parameter_CameraPosZ},
          {NVSDK_NGX_DLSSG_Parameter_CameraUpX, NVSDK_NGX_DLSSG_Parameter_CameraUpY, NVSDK_NGX_DLSSG_Parameter_CameraUpZ},
          {NVSDK_NGX_DLSSG_Parameter_CameraRightX, NVSDK_NGX_DLSSG_Parameter_CameraRightY, NVSDK_NGX_DLSSG_Parameter_CameraRightZ},
          {NVSDK_NGX_DLSSG_Parameter_CameraFwdX, NVSDK_NGX_DLSSG_Parameter_CameraFwdY, NVSDK_NGX_DLSSG_Parameter_CameraFwdZ}};
      for (int b = 0; b < 4; ++b) {
        const float* src = !use_cam[v] ? synth[b] : b == 0 ? c.pos : b == 1 ? c.up : b == 2 ? c.right : c.fwd;
        for (int k = 0; k < 3; ++k) N.SetF(p, names[b][k], src[k]);
      }
      N.SetF(p, NVSDK_NGX_DLSSG_Parameter_CameraNear, use_cam[v] ? c.near_z : 0.1f);
      N.SetF(p, NVSDK_NGX_DLSSG_Parameter_CameraFar, use_cam[v] ? c.far_z : 1000.0f);
      N.SetF(p, NVSDK_NGX_DLSSG_Parameter_CameraFOV, use_cam[v] ? c.fov : 1.2f);
      N.SetF(p, NVSDK_NGX_DLSSG_Parameter_CameraAspectRatio, use_cam[v] ? c.aspect : float(vw.bw) / float(S.h));
      N.SetF(p, NVSDK_NGX_DLSSG_Parameter_CameraPinholeOffsetX, 0.0f);
      N.SetF(p, NVSDK_NGX_DLSSG_Parameter_CameraPinholeOffsetY, 0.0f);
      // The game's MVs are in UV units; DLSS SR's own MV scale (render size) turns them into pixels.
      N.SetF(p, NVSDK_NGX_DLSSG_Parameter_MvecScaleX, in.mvx);
      N.SetF(p, NVSDK_NGX_DLSSG_Parameter_MvecScaleY, in.mvy);
      N.SetF(p, NVSDK_NGX_DLSSG_Parameter_JitterOffsetX, in.jx);
      N.SetF(p, NVSDK_NGX_DLSSG_Parameter_JitterOffsetY, in.jy);
      N.SetF(p, NVSDK_NGX_DLSSG_Parameter_MvecInvalidValue, 1.17549435e-38f);
      if (i == 1 && v == 0) ++S.bb_id;
      N.SetUI(p, NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, unsigned(S.bb_id));
      N.SetUI(p, NVSDK_NGX_DLSSG_Parameter_ColorBuffersHDR, S.hdr ? 1 : 0);
      N.SetUI(p, NVSDK_NGX_DLSSG_Parameter_DepthInverted, 0);  // conventional Z (re/dlss.md 3)
      N.SetUI(p, NVSDK_NGX_DLSSG_Parameter_CameraMotionIncluded, 1);
      N.SetUI(p, NVSDK_NGX_DLSSG_Parameter_Reset, reset[v] ? 1 : 0);
      N.SetUI(p, NVSDK_NGX_DLSSG_Parameter_NotRenderingGameFrames, 0);
      N.SetUI(p, NVSDK_NGX_DLSSG_Parameter_OrthoProjection, 0);
      N.SetUI(p, NVSDK_NGX_DLSSG_Parameter_MvecDilated, 0);
      N.SetUI(p, NVSDK_NGX_DLSSG_Parameter_MenuDetectionEnabled, 0);
      N.SetF(p, NVSDK_NGX_DLSSG_Parameter_MinRelativeLinearDepthObjectSeparation, 40.0f);
      N.SetUI(p, NVSDK_NGX_DLSSG_Parameter_MVecsSubrectBaseX, vw.dx);
      N.SetUI(p, NVSDK_NGX_DLSSG_Parameter_MVecsSubrectBaseY, 0);
      N.SetUI(p, NVSDK_NGX_DLSSG_Parameter_MVecsSubrectWidth, vw.dw);
      N.SetUI(p, NVSDK_NGX_DLSSG_Parameter_MVecsSubrectHeight, in.rh);
      N.SetUI(p, NVSDK_NGX_DLSSG_Parameter_DepthSubrectBaseX, vw.dx);
      N.SetUI(p, NVSDK_NGX_DLSSG_Parameter_DepthSubrectBaseY, 0);
      N.SetUI(p, NVSDK_NGX_DLSSG_Parameter_DepthSubrectWidth, vw.dw);
      N.SetUI(p, NVSDK_NGX_DLSSG_Parameter_DepthSubrectHeight, in.rh);
      N.SetUI(p, NVSDK_NGX_DLSSG_Parameter_InputBackbufferSubrectBaseX, vw.bx);
      N.SetUI(p, NVSDK_NGX_DLSSG_Parameter_InputBackbufferSubrectBaseY, 0);
      N.SetUI(p, NVSDK_NGX_DLSSG_Parameter_InputBackbufferSubrectWidth, vw.bw);
      N.SetUI(p, NVSDK_NGX_DLSSG_Parameter_InputBackbufferSubrectHeight, S.h);
      N.SetUI(p, NVSDK_NGX_DLSSG_Parameter_OutputInterpolatedSubrectBaseX, vw.bx);
      N.SetUI(p, NVSDK_NGX_DLSSG_Parameter_OutputInterpolatedSubrectBaseY, 0);
      N.SetUI(p, NVSDK_NGX_DLSSG_Parameter_OutputInterpolatedSubrectWidth, vw.bw);
      N.SetUI(p, NVSDK_NGX_DLSSG_Parameter_OutputInterpolatedSubrectHeight, S.h);
      // Two views write one output texture: each only inside its own sub-rectangle (by default DLSS-G
      // also writes the uninterpolated backbuffer outside it, over the other eye's half).
      N.SetUI(p, NVSDK_NGX_DLSSG_Parameter_EvalFlags, nv > 1 ? NVSDK_NGX_DLSSG_EvalFlags_UpdateOnlyInsideExtents : 0);
      N.SetUI(p, NVSDK_NGX_DLSSG_Parameter_MultiFrameCount, n_gen);
      N.SetUI(p, NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, i);
      N.SetRes(p, NVSDK_NGX_DLSSG_Parameter_OutputInterpolated, st.out[i - 1].r);
      uint32_t r = o_Eval(cl, S.feat[fi(v)].h, p, nullptr);
      if (ngx_failed(r)) {
        static int n = 0;
        if (n++ < 5) TH_LOG("fg: Evaluate(view %u, index %u of %u) = 0x%08x", v, i, n_gen, r);
        made = i - 1;
        break;
      }
    }
  }
  S.reset = false;
  for (unsigned v = 0; v < nv; ++v) S.feat[fi(v)].reset = false;
  // Stereo: the columns no view covers (a convergence shift leaves them black in the real frame) are
  // copied from the real frame into each generated one.
  if (nv > 1 && made) {
    D3D12_BOX boxes[4];
    unsigned nb = 0;
    const unsigned half = S.w / 2;
    for (unsigned v = 0; v < nv; ++v) {
      const unsigned x0 = v * half, x1 = x0 + half;  // view v is half v
      if (views[v].bx > x0) boxes[nb++] = {x0, 0, 0, views[v].bx, S.h, 1};
      if (views[v].bx + views[v].bw < x1) boxes[nb++] = {views[v].bx + views[v].bw, 0, 0, x1, S.h, 1};
    }
    if (nb) {
      to(cl, input, D3D12_RESOURCE_STATE_COPY_SOURCE);
      for (unsigned i = 0; i < made; ++i) {
        to(cl, st.out[i], D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dl{}, sl{};
        dl.pResource = st.out[i].r;
        dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        sl.pResource = input.r;
        sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        for (unsigned b = 0; b < nb; ++b) cl->CopyTextureRegion(&dl, boxes[b].left, 0, 0, &sl, &boxes[b]);
      }
      to(cl, input, kSrv);
    }
  }
  if (S.hdr) {  // HDR10 PQ -> scRGB, what is presented
    for (unsigned i = 0; i < made; ++i) {
      to(cl, st.out[i], kSrv);
      to(cl, st.dec[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      hdr_convert(cl, st.out[i].r, st.dec[i].r, false, slot0 + 2 + i);
      to(cl, st.dec[i], D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
  }
  for (unsigned i = 0; i < n_gen; ++i) to(cl, st.out[i], D3D12_RESOURCE_STATE_COPY_SOURCE);
  if (hud) {
    // [fg] show = hudless: the HUD-less copy presented in place of the real frame (same size and format only).
    D3D12_RESOURCE_DESC hd = hud->r->GetDesc();
    if (S.show == State::kHudless && hd.Format == S.fmt && hd.Width == S.w && hd.Height == S.h) {
      barrier(cl, hud->r, kSrv, D3D12_RESOURCE_STATE_COPY_SOURCE);
      to(cl, st.real, D3D12_RESOURCE_STATE_COPY_DEST);
      cl->CopyResource(st.real.r, hud->r);
      barrier(cl, hud->r, D3D12_RESOURCE_STATE_COPY_SOURCE, hud->st);
    } else {
      barrier(cl, hud->r, kSrv, hud->st);
    }
  }
  to(cl, st.real, D3D12_RESOURCE_STATE_COPY_SOURCE);
  static int logged = 0;
  if (logged++ < 3 || (nv > 1 && logged < 40 && S.n_views2.load() < 3))
    TH_LOG("fg: evaluated %u of %u generated frames, %u view%s (render %ux%u; view 0 backbuffer x %u w %u, depth x %u w %u%s)",
           made, n_gen, nv, nv > 1 ? "s" : "", st.in[views[0].eye].rw, st.in[views[0].eye].rh, views[0].bx, views[0].bw,
           views[0].dx, views[0].dw, hl && views[0].hud ? ", HUD-less" : "");
  return made;
}

// ---- grabs (`fg grab N TAG`): the real and generated frames of N jobs, whole, as raw texels ---------
// For checking generated frames against real ones (stereo: per eye). Copied on the job's own list after
// DLSS-G, read back once its fence is done (renderer thread, a later Present), written as THRW .raw
// (header "THRW", w, h, pitch, DXGI format: re/scripts/stereo_sbs.py raw / tools/fg/stereo-gen.py).
struct Grab {
  ID3D12Resource* rb = nullptr;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
  DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
  uint64_t fence = 0;
  std::string path;
};
std::vector<Grab> g_grabs;  // recorded, not yet written (renderer thread)
std::mutex g_grab_mu;
int g_grab_jobs = 0;  // jobs still to grab
std::string g_grab_tag;
std::vector<std::string> g_grab_files;

void grab_record(ID3D12GraphicsCommandList* cl, ID3D12Resource* src, const std::string& path) {
  D3D12_RESOURCE_DESC d = src->GetDesc();
  Grab g;
  UINT64 total = 0;
  S.dev->GetCopyableFootprints(&d, 0, 1, 0, &g.fp, nullptr, nullptr, &total);
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC bd{};
  bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bd.Width = total;
  bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
  bd.SampleDesc.Count = 1;
  bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(S.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            __uuidof(ID3D12Resource), (void**)&g.rb)))
    return;
  D3D12_TEXTURE_COPY_LOCATION dst{}, sl{};
  dst.pResource = g.rb;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = g.fp;
  sl.pResource = src;
  sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  cl->CopyTextureRegion(&dst, 0, 0, 0, &sl, nullptr);  // src is in COPY_SOURCE after evaluate()
  g.fmt = d.Format;
  g.path = path;
  g_grabs.push_back(g);
}

// This job's frames (its list is still open), if a grab is armed.
void grab_job(const Set& st, unsigned n_gen, unsigned nv) {
  std::string tag;
  {
    std::lock_guard lk(g_grab_mu);
    if (g_grab_jobs <= 0 || !n_gen) return;
    g_grab_jobs--;
    tag = g_grab_tag;
  }
  const char* d = getenv("QUIVER_LAB_RUN_DIR");
  char base[400];
  snprintf(base, sizeof base, "%s\\fggrab_%06u_%s", d ? d : ".", S.frame, tag.c_str());
  ID3D12GraphicsCommandList* cl = S.rring.list[S.rring.cur];
  grab_record(cl, st.real.r, std::string(base) + "_R.raw");
  for (unsigned i = 0; i < n_gen; ++i) {
    char suf[32];
    snprintf(suf, sizeof suf, "_G%uof%u.raw", i + 1, n_gen + 1);
    grab_record(cl, S.hdr ? st.dec[i].r : st.out[i].r, std::string(base) + suf);
  }
  TH_LOG("fg grab: frame %u, %u generated, %u view%s (layout: %s pack %d w %d shift %d/%d)", S.frame, n_gen, nv,
         nv > 1 ? "s" : "", S.cur.on ? (S.cur.pair ? "pair" : "mono") : "none", S.cur.pack, S.cur.w, S.cur.shift[0],
         S.cur.shift[1]);
}

void grab_set_fence(uint64_t v) {
  for (Grab& g : g_grabs)
    if (!g.fence) g.fence = v;
}

void grab_poll() {
  for (size_t i = 0; i < g_grabs.size();) {
    Grab& g = g_grabs[i];
    if (!g.fence || !S.rring.done(g.fence)) {
      ++i;
      continue;
    }
    void* p = nullptr;
    D3D12_RANGE r{0, SIZE_T(g.fp.Offset + uint64_t(g.fp.Footprint.RowPitch) * g.fp.Footprint.Height)};
    if (SUCCEEDED(g.rb->Map(0, &r, &p))) {
      if (FILE* f = fopen(g.path.c_str(), "wb")) {
        uint32_t hd[5] = {0x57524854u, g.fp.Footprint.Width, g.fp.Footprint.Height, g.fp.Footprint.RowPitch, uint32_t(g.fmt)};
        fwrite(hd, 4, 5, f);
        fwrite(static_cast<uint8_t*>(p) + g.fp.Offset, 1, size_t(g.fp.Footprint.RowPitch) * g.fp.Footprint.Height, f);
        fclose(f);
        std::lock_guard lk(g_grab_mu);
        g_grab_files.push_back(g.path);
      }
      D3D12_RANGE none{0, 0};
      g.rb->Unmap(0, &none);
    }
    g.rb->Release();
    g_grabs[i] = g_grabs.back();
    g_grabs.pop_back();
  }
}

// ---- presenting -------------------------------------------------------------------------------------

bool real_buffers() {
  if (S.nbb) return true;
  DXGI_SWAP_CHAIN_DESC d{};
  S.sc->GetDesc(&d);
  S.nbb = std::min<UINT>(d.BufferCount, 8);
  for (UINT i = 0; i < S.nbb; ++i)
    if (FAILED(present_real_buffer(S.sc, i, __uuidof(ID3D12Resource), (void**)&S.bb[i]))) {
      TH_LOG("fg: GetBuffer(%u) of the real swapchain failed", i);
      S.nbb = 0;
      return false;
    }
  return true;
}

// One Present of `src` (ours, COPY_SOURCE): copied into the real back buffer, then presented.
// kind/gi/gn label it for the present trace ('G' generated index gi of multiplier gn, 'R' real after
// generated frames, 'N' real alone).
void present_tex(ID3D12Resource* src, const Job& j, char kind, int gi, int gn, int64_t step) {
  UINT i = S.sc->GetCurrentBackBufferIndex();
  if (i >= S.nbb) return;
  ID3D12GraphicsCommandList* cl = S.pring.begin();
  barrier(cl, S.bb[i], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
  cl->CopyResource(S.bb[i], src);
  barrier(cl, S.bb[i], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
  int ti = trace_copy(S.dev, cl, src, S.fmt, kind, j.id, j.arrive_us, step, S.interval_us, gi, gn);
  uint64_t v = S.pring.submit(S.q);
  trace_call(ti);
  lat_present(j.frame, kind);
  S.last_call_us = now_us();
  present_real(S.sc, j.sync, j.flags);
  S.last_present_us = now_us();
  trace_ret(ti, S.pring.fence, v);
}

void sleep_until(int64_t t) {
  for (;;) {
    int64_t d = t - now_us();
    if (d <= 0) return;
    if (d > 2000) Sleep(DWORD((d - 1000) / 1000));
    else SwitchToThread();
  }
}

// Pacing ([fg] pace). `step` = the real-frame interval / multiplier.
//  ready  (v1): the first generated frame as soon as DLSS-G is done, every later present one step after
//         the previous Present *returned*.
//  smooth (Pharos's default): the first generated frame not sooner than one step after the previous real
//         present's call, minus [fg] pace_relax of how early DLSS-G finished relative to that (so a delay
//         never sticks); every later present one step after the previous Present's *call* (a Present that
//         blocks must not push the next one later).
void present_thread() {
  HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  for (;;) {
    Job j;
    {
      std::unique_lock lk(S.mu);
      S.cv.wait(lk, [] { return !S.jobs.empty(); });
      j = S.jobs.front();
      S.jobs.pop_front();
      S.busy = true;
    }
    Set& st = S.sets[j.set];
    const unsigned m = j.n_gen + 1;
    const int64_t step = j.step_us;
    const bool smooth = j.pace == 1;
    // Superseded (the game is ahead of us): show only the newest real frame.
    bool behind;
    {
      std::lock_guard lk(S.mu);
      behind = !S.jobs.empty();
    }
    unsigned shown_gen = 0;
    int64_t prev_call = 0, prev_ret = 0;
    if (behind && j.n_gen) S.p_skip += j.n_gen;
    if (!behind && j.n_gen && (S.show == State::kAll || S.show == State::kGenerated)) {
      // DLSS-G finished? Don't wait longer than 100 ms (then this job's generated frames are dropped).
      if (fence_wait(S.rring.fence, j.eval_v, ev, 100)) {
        const int64_t t_ready = now_us();
        S.lag_sum += t_ready - j.arrive_us;
        S.lag_n++;
        S.aw_lag_sum += t_ready - j.arrive_us;
        S.aw_lag_n++;
        for (unsigned i = 0; i < j.n_gen; ++i) {
          int64_t target = 0;
          if (i == 0) {
            if (smooth && S.last_real_call_us)
              target = S.last_real_call_us + step - int64_t(j.relax * double(S.last_real_call_us + step - t_ready));
          } else {
            target = (smooth ? prev_call : prev_ret) + step;
          }
          int64_t now = now_us();
          if (target > now && target - now < 2 * step) sleep_until(target);
          present_tex(S.hdr ? st.dec[i].r : st.out[i].r, j, 'G', int(i) + 1, int(m), step);
          prev_call = S.last_call_us;
          prev_ret = S.last_present_us;
          ++shown_gen;
        }
        S.p_gen += shown_gen;
      } else {
        ++S.p_drop;
      }
    }
    if (S.show != State::kGenerated || !shown_gen) {
      if (shown_gen) sleep_until((smooth ? prev_call : prev_ret) + step);
      present_tex(st.real.r, j, shown_gen ? 'R' : 'N', 0, int(shown_gen ? m : 1), step);
      S.last_real_call_us = S.last_call_us;
      ++S.p_real;
    }
    {
      std::lock_guard lk(S.mu);
      S.busy = false;
      S.jobs_done = j.id;
    }
    S.cv_done.notify_all();
  }
}

// The render thread waits until the present thread no longer reads `set` (it was posted two frames ago).
void join_set(uint64_t id) {
  std::unique_lock lk(S.mu);
  S.cv_done.wait_for(lk, std::chrono::milliseconds(200), [&] { return S.jobs_done >= id; });
}

void drain() {
  std::unique_lock lk(S.mu);
  S.cv_done.wait_for(lk, std::chrono::milliseconds(500), [] { return S.jobs.empty() && !S.busy; });
}

unsigned count_now();

void perf() {
  int64_t now = now_us();
  if (!S.perf_last) S.perf_last = now;
  double secs = (now - S.perf_last) / 1e6;
  if (secs < 2.0) return;
  unsigned real = S.p_real.exchange(0), gen = S.p_gen.exchange(0);
  unsigned fail = S.p_fail.exchange(0), noin = S.p_noinput.exchange(0), drop = S.p_drop.exchange(0);
  unsigned skip = S.p_skip.exchange(0), joins = S.p_join.exchange(0), views2 = S.p_views2.exchange(0);
  int64_t lag_n = S.lag_n.exchange(0), lag_sum = S.lag_sum.exchange(0), join_sum = S.join_sum.exchange(0);
  double lag_ms = lag_n ? lag_sum / 1000.0 / lag_n : 0;
  TH_LOG("perf: fg real %.1f + generated %.1f = %.1f presents/s (x%.2f, count %u%s, cap %.1f, interval %.2f ms; DLSS-G done %.2f ms "
         "after the game's Present; game waited %u x %.2f ms; fail %u, no input %u, dropped %u, skipped %u%s)",
         real / secs, gen / secs, (real + gen) / secs, real ? 1.0 + double(gen) / real : 0.0, count_now(),
         S.count_auto ? " auto" : "", S.cap_now, S.interval_us / 1000.0, lag_ms, joins, joins ? join_sum / 1000.0 / joins : 0.0, fail, noin, drop, skip,
         S.link.eye ? (std::string("; per eye ") + std::to_string(views2) + " frames").c_str() : "");
  char buf[512];
  snprintf(buf, sizeof buf,
           "{\"real_fps\":%.2f,\"presents_per_s\":%.2f,\"multiplier\":%.3f,\"count\":%u,\"auto\":%s,\"cap\":%.2f,\"interval_ms\":%.3f,"
           "\"lag_ms\":%.3f,\"joins\":%u,\"fail\":%u,\"no_input\":%u,\"dropped\":%u,\"skipped\":%u,\"per_eye_frames\":%u}",
           real / secs, (real + gen) / secs, real ? 1.0 + double(gen) / real : 0.0, count_now(),
           S.count_auto ? "true" : "false", S.cap_now, S.interval_us / 1000.0,
           lag_ms, joins, fail, noin, drop, skip, views2);
  {
    std::lock_guard lk(S.perf_mu);
    S.perf_json = buf;
  }
  S.perf_last = now;
}

unsigned count_now() { return S.count_auto ? S.auto_count : S.count.load(); }

double target_now() {
  double t = S.target_hz.load();
  return t > 0 ? t : S.refresh_hz > 0 ? S.refresh_hz : 60.0;
}

// The display refresh (the swapchain's output's current mode), once.
void detect_refresh() {
  static bool done = false;
  if (done || !S.sc) return;
  done = true;
  IDXGIOutput* o = nullptr;
  if (SUCCEEDED(S.sc->GetContainingOutput(&o)) && o) {
    DXGI_OUTPUT_DESC d{};
    o->GetDesc(&d);
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsW(d.DeviceName, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
      S.refresh_hz = dm.dmDisplayFrequency;
    o->Release();
  }
  TH_LOG("fg: display refresh %.0f Hz%s", S.refresh_hz, S.refresh_hz > 0 ? "" : " (unknown: 60 assumed for count = auto)");
}

// Frame limiter at the end of the game's Present (the renderer thread sleeps before it starts the next
// frame; the frame just handed to the present thread is not delayed). Cap: [fg] fps_limit, or with
// count = auto the target / multiplier (presents stay at or below the target).
void limit(bool fg_frame) {
  double cap = S.fps_limit.load();
  if (fg_frame && cap <= 0) {
    // An explicit fps_limit overrides; else auto_cap's headroom cap and the refresh cap, or count =
    // auto's refresh cap alone.
    const double r = target_now() / std::max(1u, count_now());
    if (S.auto_cap) cap = S.head > 0 ? std::min(S.head, r) : 0;
    else if (S.count_auto) cap = r;
  }
  S.cap_now = cap;
  S.lim_sleep_us = 0;
  if (cap <= 0) {
    S.lim_next = 0;
    return;
  }
  const int64_t period = int64_t(1e6 / cap), now = now_us();
  if (!S.lim_next || now - S.lim_next > period) S.lim_next = now;  // behind by more than a frame: restart
  if (S.lim_next > now) {
    sleep_until(S.lim_next);
    S.lim_sleep_us = now_us() - now;
  }
  S.lim_next += period;
}

// Once a second (renderer thread), from this window's frames:
//
// auto_cap (Pharos's headroom cap, [fg] auto_cap = 1): without headroom the GPU queue fills, DLSS-G
// finishes more than a frame after the game's Present, the present thread falls behind and the game
// blocks on it at irregular times (uneven game frame times, more latency; docs/fg.md 4). If DLSS-G
// finished later than auto_cap_lag (0.7) of a frame interval after the game's Present on average, or
// the game waited for the present thread on more than 2 % of its frames, the headroom cap drops to
// 96 % of the rate reached; if it finished within auto_cap_lag - 0.15 and the game never waited, it
// rises 1 % (4 % below auto_cap_lag - 0.35), only while it is the binding cap. It starts 2 s into a
// run of generated frames at 90 % of the rate then. The limiter applies min(headroom cap, target /
// multiplier): presents stay at or below the refresh target.
//
// count = auto: in [count_min, count_max]: one step up when the game can't hold target / multiplier
// (its own work rate, a manual fps_limit or the headroom cap 10 % short, 2 s); one step down when its
// work rate could hold target / (multiplier - 1) with 15 % to spare and DLSS-G finished within 0.55 of
// that shorter interval (3 s). A step down undone within 30 s blocks further steps down for 10 s,
// doubling up to 60 s (Pharos: 6 s; the renderer-side work time overestimates a capped game). Windows
// with a hitch (below 60 % of the cap) change nothing.
void auto_step(int64_t now) {
  if (!S.aw_start) S.aw_start = now;
  if (now - S.aw_start < 1000000) return;
  const double secs = (now - S.aw_start) / 1e6;
  const unsigned frames = S.aw_frames, joins = S.aw_joins;
  const double work_fps = S.aw_work > 0 ? frames * 1e6 / double(S.aw_work) : 0;
  const double real_fps = frames / secs;
  S.aw_start = now;
  S.aw_work = 0;
  S.aw_frames = 0;
  S.aw_joins = 0;
  const int64_t lag_n = S.aw_lag_n.exchange(0), lag_sum = S.aw_lag_sum.exchange(0);
  const double lag_us = lag_n ? double(lag_sum) / lag_n : 0;
  const double target = target_now();
  const double lim = S.fps_limit.load();
  const bool generating = S.chain && frames >= 5;
  if (!generating) S.fg_since = 0;
  else if (!S.fg_since) S.fg_since = now;
  const bool hitch = S.cap_now > 0 && real_fps < 0.6 * S.cap_now;

  // ---- headroom cap
  const double ratio = real_fps > 1 && lag_n ? lag_us * real_fps / 1e6 : 0;  // DLSS-G lag in frames
  const double thr = S.auto_cap_lag.load();
  const bool overloaded = ratio > thr || joins > real_fps * secs * 0.02;
  if (!S.auto_cap || lim > 0 || !generating || now - S.fg_since < 2000000) {
    if (!generating) S.head = 0;
  } else if (!hitch) {
    const double r = target / std::max(1u, count_now());
    const double was = S.head;
    if (S.head <= 0) S.head = (S.cap_now > 0 ? std::min(S.cap_now, real_fps) : real_fps) * 0.9;
    else if (overloaded) S.head = std::min(S.cap_now > 0 ? S.cap_now : S.head, real_fps) * 0.96;
    else if (S.head <= r * 1.001 && joins == 0) {
      if (ratio < thr - 0.35) S.head *= 1.04;
      else if (ratio < thr - 0.15) S.head *= 1.01;
    }
    S.head = std::max(20.0, std::min(500.0, S.head));
    if (std::fabs(S.head - was) > 3.0 || was <= 0)
      TH_LOG("fg: auto cap %.1f fps (headroom %.1f, refresh %.1f; real %.1f fps, DLSS-G done %.1f ms = %.2f frames after "
             "the game's Present, game waited %u times)", std::min(S.head, r), S.head, r, real_fps, lag_us / 1000.0,
             ratio, joins);
  }

  // ---- count = auto
  if (!S.count_auto) {
    S.auto_count = std::max(2u, std::min(S.count.load(), S.caps_max_gen + 1));
    S.up_streak = S.down_streak = 0;
    return;
  }
  const unsigned lo = std::max(2u, S.count_min.load()), hi = std::min(std::max(lo, S.count_max.load()), S.caps_max_gen + 1);
  unsigned m = std::max(lo, std::min(hi, S.auto_count));
  if (frames < 5 || hitch) {  // hitch, menus, loading
    S.up_streak = S.down_streak = 0;
    S.auto_count = m;
    return;
  }
  // What the game can deliver: its own work rate, no more than a manual cap ([fg] fps_limit 40 with a
  // 165 Hz target means 4x) or the headroom cap.
  double can = lim > 0 ? std::min(work_fps, lim) : work_fps;
  const double can_up = S.auto_cap && lim <= 0 && S.head > 0 ? std::min(can, S.head) : can;
  // The renderer thread's work time misses the GPU once frames are capped with headroom, so a step down
  // also needs DLSS-G to finish early enough for the shorter interval (Pharos: lag below 0.55 of it).
  const bool lag_ok = lag_us < 0.55 * 1e6 * (m - 1) / target && !overloaded;
  S.up_streak = m < hi && can_up < 0.9 * target / m ? S.up_streak + 1 : 0;
  S.down_streak = m > lo && can > 1.15 * target / (m - 1) && lag_ok ? S.down_streak + 1 : 0;
  unsigned was = m;
  if (S.up_streak >= 2) {
    m++;
    if (S.last_down_us && now - S.last_down_us < 30000000) {  // a step down undone: hold off
      S.down_block_until = now + S.down_block_s * 1000000;
      S.down_block_s = std::min<int64_t>(S.down_block_s * 2, 60);
    }
    S.last_up_us = now;
  } else if (S.down_streak >= 3 && now >= S.down_block_until) {
    m--;
    S.last_down_us = now;
    S.head = std::max(S.head, target / m);  // the headroom cap may rise to the new refresh cap
  }
  if (m != was) {
    S.up_streak = S.down_streak = 0;
    TH_LOG("fg: count auto %ux -> %ux (work %.1f fps, real %.1f fps, DLSS-G done %.1f ms after Present, headroom %.1f, "
           "target %.0f presents/s)", was, m, work_fps, real_fps, lag_us / 1000.0, S.head, target);
  }
  S.auto_count = m;
}

// This frame's views (0 = no inputs): the whole frame, or with a side-by-side stereo layout reported
// by stereo's `end` one view per eye, each in its half of the frame (docs/fg.md "Stereo").
//  pack 1: the frame is 2W, each eye 1:1; pack 2: W, each eye squeezed into W/2.
//  An eye image shifted by s (convergence) shows eye columns [max(-s, 0), W - max(s, 0)) at
//  half x + max(s, 0): the view covers that, with depth/MVs and the camera cropped to the same columns.
//  A frame without a pair (one render duplicated into both halves) gives both views the one input set.
unsigned frame_views(const Set& st, const EngTex* hud, View* out) {
  const unsigned hw = hud ? unsigned(hud->r->GetDesc().Width) : 0;
  const bool stereo = S.cur.on && S.cur.w > 0 && (S.cur.pack == 1 ? S.w == 2u * unsigned(S.cur.w) : S.cur.pack == 2 && S.w == unsigned(S.cur.w));
  if (!stereo) {
    if (!st.in[0].ok) return 0;
    View& v = out[0];
    v.bx = 0, v.bw = S.w;
    v.hud = hud != nullptr, v.hx = 0, v.hw = std::min(hw, S.w);
    v.dx = 0, v.dw = st.in[0].rw;
    v.eye = 0, v.fw = S.w;
    return 1;
  }
  const bool pair = S.cur.pair;
  if (!st.in[0].ok || (pair && !st.in[1].ok)) return 0;
  const int W = S.cur.w;
  const unsigned half = S.w / 2;  // the output width of one eye
  for (unsigned e = 0; e < 2; ++e) {
    View& v = out[e];
    const Inputs& in = st.in[pair ? e : 0];
    const int s = pair ? std::max(-(W - 1), std::min(W - 1, S.cur.shift[e])) : 0;
    const int p0 = std::max(-s, 0), wc = W - std::abs(s);
    const unsigned x0 = S.cur.pack == 1 ? unsigned(std::max(s, 0)) : unsigned(std::max(s, 0)) / 2;
    v.bx = e * half + x0;
    v.bw = S.cur.pack == 1 ? unsigned(wc) : std::min(half - x0, unsigned(wc + 1) / 2);
    v.dx = unsigned(int64_t(p0) * in.rw / W);
    v.dw = std::min(in.rw - v.dx, unsigned((int64_t(wc) * in.rw + W / 2) / W));
    v.crop_p0 = p0, v.crop_wc = wc, v.crop_w = W;
    v.eye = pair ? e : 0;
    // DLSS-G rejects a backbuffer sub-rectangle narrower than the feature (0xBAD00005, live r2): the
    // feature is made at the shown width, so a convergence change re-creates it.
    v.fw = v.bw;
    // HUD-less: the side-by-side texture before the HUD (same layout as the frame), or for a frame
    // without a pair the one eye-wide colour target; none when the frame is squeezed (pack 2).
    v.hud = false;
    if (hud && S.cur.pack == 1 && hw == S.w) v.hud = true, v.hx = v.bx, v.hw = v.bw;
    else if (hud && S.cur.pack == 1 && !pair && hw == half) v.hud = true, v.hx = v.bx - e * half, v.hw = v.bw;
  }
  if (!pair) S.n_views2_mono++;
  return 2;
}

// The game's Present (renderer thread).
bool takeover(IDXGISwapChain* sc, unsigned sync, unsigned flags) {
  if (!S.enabled || !S.sc || sc != (IDXGISwapChain*)S.sc || !S.proxy[0].r) return false;
  S.cur = S.lay;  // stereo reports each frame it composes; a frame it doesn't is the whole frame
  S.lay.on = false;
  if (!real_buffers()) return false;
  detect_refresh();
  if (!g_grabs.empty()) grab_poll();
  reflex_present_begin(S.dev);
  // Which proxy did the engine just render? Its own index flips after Present (re/dlss.md 2.2).
  void* nt = g_auto_bb();
  ID3D12Resource* proxy = nt ? *reinterpret_cast<ID3D12Resource**>(static_cast<char*>(nt) + 0x88) : nullptr;
  if (proxy != S.proxy[0].r && proxy != S.proxy[1].r) {
    static int n = 0;
    if (n++ < 3) TH_LOG("fg: getAutoBackBuffer() = %p is not one of our proxies: presenting normally", (void*)proxy);
    return false;
  }
  // The HUD-less tap: RendererX86's renderHUD vtable slot, patched on the renderer thread between frames
  // (lazily, after every feature's start-up checks of that vtable have run).
  static bool hud_patched = false;
  if (!hud_patched && S.engine_ok) {
    hud_patched = true;
    if (!hudless_install()) S.hudless = false;
  }
  int64_t t = now_us();
  // The real-frame interval (EMA of the game's Present arrivals), without the time the renderer thread
  // waited for the present thread (else a present thread behind the game stretches its own step: a
  // loop with no way back, Pharos), hitches counted at most 2.5x, and a fresh start after frames
  // without generation (menus, loading, `active` off).
  if (S.last_arrive_us && S.chain) {
    int64_t d = t - S.last_arrive_us - S.join_us;
    d = std::max<int64_t>(1000, std::min<int64_t>(d, S.interval_us * 5 / 2));
    S.interval_us = (S.interval_us * 7 + d) / 8;
    // The frame's own work: without the limiter's sleep either (count = auto's measure).
    S.aw_work += std::max<int64_t>(1000, t - S.last_arrive_us - S.join_us - S.lim_sleep_us);
    S.aw_frames++;
  }
  S.last_arrive_us = t;
  S.join_us = 0;

  const unsigned si = S.frame % kSets;
  Set& st = S.sets[si];
  uint64_t id = ++S.jobs_posted;
  if (id > kSets) {  // the present thread is done with this set
    int64_t j0 = now_us();
    join_set(id - kSets);
    S.join_us = now_us() - j0;
    if (S.join_us > 500) {
      S.p_join++;
      S.aw_joins++;
      S.join_sum += S.join_us;
    }
  }

  unsigned want = std::min(std::max(count_now(), 1u), S.caps_max_gen + 1) - 1;
  if (!S.on) want = 0;
  unsigned n_gen = 0;
  EngTex hud;
  bool have_hud = hudless_take(hud) && hud.tracked && S.hudless.load();
  // In HDR the HUD-less colour must be scRGB like the swapchain to be encoded like the real frame.
  if (have_hud && S.hdr && hud.r->GetDesc().Format != S.fmt) {
    static int n = 0;
    if (n++ < 1) TH_LOG("fg: HDR: HUD-less colour is DXGI %d, not the swapchain's %d: not used", int(hud.r->GetDesc().Format), int(S.fmt));
    have_hud = false;
  }
  View views[2];
  const unsigned nv = frame_views(st, have_hud ? &hud : nullptr, views);
  const bool have_in = nv > 0;
  if (want && !have_in) ++S.p_noinput;
  bool feats = have_in && ensure_ngx();
  const unsigned nf = nv > 1 && S.stereo_shared.load() ? 1 : nv;  // dev: one feature for both eyes
  for (unsigned v = 0; feats && v < nf; ++v)
    feats = ensure_feature(v, views[v].fw, st.in[views[v].eye].rw, st.in[views[v].eye].rh);
  if (want && feats && ensure_outputs(st, want)) {
    n_gen = evaluate(st, proxy, want, have_hud ? &hud : nullptr, views, nv);
    if (n_gen < want) ++S.p_fail;
    if (nv > 1 && n_gen) {
      S.p_views2++;
      S.n_views2++;
    }
  } else {
    ensure_outputs(st, 0);
    evaluate(st, proxy, 0, nullptr, views, 0);
  }
  if (nf == 1 && S.feat[1].h) release_feature(1);  // one view again (stereo off): the right eye's goes
  if (!have_in) S.reset = true;
  st.in[0].ok = st.in[1].ok = false;
  if (!n_gen && S.chain) S.interval_us = 16667;
  if (!n_gen) S.interval_us = std::min<int64_t>(S.interval_us, 33333);
  S.chain = n_gen > 0;
  Job j;
  j.set = si;
  j.n_gen = n_gen;
  j.step_us = std::min<int64_t>(S.interval_us, 50000) / (n_gen + 1);
  j.pace = S.pace.load();
  j.relax = S.relax.load();
  lat_end(S.rring.list[S.rring.cur], S.frame, n_gen, t);
  grab_job(st, n_gen, nv);
  j.frame = S.frame;
  j.eval_v = S.rring.submit(S.q);
  grab_set_fence(j.eval_v);
  j.sync = sync;
  j.flags = flags;
  j.arrive_us = t;
  j.id = id;
  {
    std::lock_guard lk(S.mu);
    S.jobs.push_back(j);
  }
  S.cv.notify_one();
  ++S.frame;
  perf();
  auto_step(t);
  limit(n_gen > 0);
  // Reflex (reflex.cpp): the driver's sleep before the next frame starts, after our cap. Counted with
  // the limiter's sleep as time the frame didn't work.
  const int64_t r0 = now_us();
  reflex_present_end(S.dev);
  S.lim_sleep_us += now_us() - r0;
  return true;
}

// GetBuffer: our proxies (created on first request at the swapchain's current size and format).
bool getbuffer(IDXGISwapChain* sc, unsigned i, REFIID riid, void** out, long* hr) {
  if (!S.enabled || i > 1) return false;
  if (S.sc != sc) {
    safe_release(S.sc);
    if (FAILED(sc->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&S.sc))) return false;
    auto* q = static_cast<IUnknown*>(present_queue(sc));
    ID3D12CommandQueue* queue = nullptr;
    if (!q || FAILED(q->QueryInterface(__uuidof(ID3D12CommandQueue), (void**)&queue))) {
      TH_LOG("fg: the swapchain's creator isn't a D3D12 queue: frame generation off");
      S.enabled = false;
      return false;
    }
    S.q = queue;  // keep the reference
    if (!S.dev) {
      S.q->GetDevice(__uuidof(ID3D12Device), (void**)&S.dev);
      if (!S.rring.init(S.dev, L"threshold fg render") || !S.pring.init(S.dev, L"threshold fg present")) {
        TH_LOG("fg: command list setup failed: frame generation off");
        S.enabled = false;
        return false;
      }
      std::thread(present_thread).detach();
    }
  }
  DXGI_SWAP_CHAIN_DESC d{};
  sc->GetDesc(&d);
  // HDR: the game's FP16 swapchain is scRGB (linear BT.709; docs/stereo.md "HDR"). DLSS-G gets HDR10 PQ
  // copies (hdr.cpp).
  const bool hdr = d.BufferDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
  if (hdr && !hdr_init(S.dev)) {
    static bool said = false;
    if (!said) TH_LOG("fg: HDR (FP16 scRGB) swapchain without the PQ conversion: frame generation off");
    said = true;
    S.enabled = false;
    return false;
  }
  if (hdr != S.hdr) TH_LOG("fg: swapchain %s: DLSS-G input %s", hdr ? "FP16 scRGB (HDR)" : "SDR",
                           hdr ? "HDR10 PQ (converted)" : "as presented");
  S.hdr = hdr;
  // The real buffer's size: GetDesc reports what the game asked for, which a stereo side-by-side
  // swapchain (core/present.cpp width scale) doubles.
  ID3D12Resource* real = nullptr;
  if (FAILED(present_real_buffer(sc, i, __uuidof(ID3D12Resource), (void**)&real))) return false;
  D3D12_RESOURCE_DESC rd = real->GetDesc();
  real->Release();
  if (!S.proxy[i].r || S.w != rd.Width || S.h != rd.Height || S.fmt != d.BufferDesc.Format) {
    S.w = unsigned(rd.Width);
    S.h = rd.Height;
    S.fmt = d.BufferDesc.Format;
    if (!make_tex(S.dev, S.proxy[i], rd, D3D12_RESOURCE_STATE_PRESENT, i ? L"threshold proxy 1" : L"threshold proxy 0")) return false;
    TH_LOG("fg: proxy back buffer %u: %ux%u fmt %d flags 0x%x", i, S.w, S.h, int(S.fmt), int(rd.Flags));
  }
  *hr = S.proxy[i].r->QueryInterface(riid, out);
  return true;
}

void before_resize(IDXGISwapChain* sc) {
  if (!S.enabled || sc != (IDXGISwapChain*)S.sc) return;
  drain();
  S.rring.wait(S.rring.next);
  S.pring.wait(S.pring.next);
  for (UINT i = 0; i < S.nbb; ++i) safe_release(S.bb[i]);
  S.nbb = 0;
  for (Tex& p : S.proxy) safe_release(p.r);
  for (Set& st : S.sets) {
    safe_release(st.real.r);
    safe_release(st.enc.r);
    safe_release(st.hud_enc.r);
    for (Tex& o : st.out) safe_release(o.r);
    for (Tex& o : st.dec) safe_release(o.r);
    st.in[0].ok = st.in[1].ok = false;
  }
  S.reset = true;
  TH_LOG("fg: resize: released proxies and generated-frame textures");
}

// Crash diagnostics: fatal exception codes with the faulting address as module+RVA and a short stack
// scan (first few only). Installed with [fg] enabled.
std::string where(uintptr_t a) {
  HMODULE m = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(a), &m) || !m)
    return "?";
  wchar_t name[MAX_PATH];
  GetModuleFileNameW(m, name, MAX_PATH);
  const wchar_t* b = wcsrchr(name, L'\\');
  char out[300];
  snprintf(out, sizeof out, "%ls+%#llx", b ? b + 1 : name, (unsigned long long)(a - uintptr_t(m)));
  return out;
}
LONG CALLBACK on_exception(EXCEPTION_POINTERS* ep) {
  DWORD c = ep->ExceptionRecord->ExceptionCode;
  if (c != EXCEPTION_ACCESS_VIOLATION && c != EXCEPTION_ILLEGAL_INSTRUCTION && c != EXCEPTION_STACK_OVERFLOW &&
      c != EXCEPTION_INT_DIVIDE_BY_ZERO && c != 0xC0000409)
    return EXCEPTION_CONTINUE_SEARCH;
  static std::atomic<int> n{0};
  if (n++ >= 4) return EXCEPTION_CONTINUE_SEARCH;
  uintptr_t pc = uintptr_t(ep->ExceptionRecord->ExceptionAddress);
  TH_LOG("fg: exception %#lx at %s (thread %lu, info %#llx %#llx)", c, where(pc).c_str(), GetCurrentThreadId(),
         (unsigned long long)ep->ExceptionRecord->ExceptionInformation[0],
         (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
  const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(ep->ContextRecord->Rsp);
  for (int i = 0, shown = 0; i < 64 && shown < 10; ++i) {
    std::string w = where(sp[i]);
    if (w == "?") continue;
    TH_LOG("fg:   [rsp+%#x] %s", i * 8, w.c_str());
    shown++;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace
}  // namespace th::fg

namespace th {
using namespace fg;

void fg_register() {
  cfg_listen("fg.", [](const std::string& id, const std::string& v) {
    if (id == "fg.count") {
      S.count_auto = v == "auto";
      if (!S.count_auto) S.count = std::max(1, std::min(6, atoi(v.c_str())));
    } else if (id == "fg.count_min") S.count_min = std::max(2, std::min(6, atoi(v.c_str())));
    else if (id == "fg.count_max") S.count_max = std::max(2, std::min(6, atoi(v.c_str())));
    else if (id == "fg.target_hz") S.target_hz = std::max(0.0, atof(v.c_str()));
    else if (id == "fg.fps_limit") S.fps_limit = std::max(0.0, atof(v.c_str()));
    else if (id == "fg.auto_cap") S.auto_cap = v != "0" && v != "false" && v != "off";
    else if (id == "fg.reflex") reflex_config(v == "boost" ? 2 : v == "on" || v == "1" ? 1 : 0, cfg_bool("fg.reflex_markers", true));
    else if (id == "fg.reflex_markers") reflex_config(cfg_str("fg.reflex") == "boost" ? 2 : cfg_str("fg.reflex") == "on" ? 1 : 0, v != "0" && v != "false" && v != "off");
    else if (id == "fg.auto_cap_lag") S.auto_cap_lag = std::max(0.2, std::min(2.0, atof(v.c_str())));
    else if (id == "fg.active") S.on = v != "0" && v != "false" && v != "off";
    else if (id == "fg.show")
      S.show = v == "generated" ? State::kGenerated : v == "real" ? State::kReal : v == "hudless" ? State::kHudless : State::kAll;
    else if (id == "fg.camera") S.cam_engine = v != "identity";
    else if (id == "fg.pace") S.pace = v == "smooth" ? 1 : 0;
    else if (id == "fg.pace_relax") S.relax = std::max(0.0, std::min(1.0, atof(v.c_str())));
    else if (id == "fg.hudless") S.hudless = v != "0" && v != "false" && v != "off";
    else if (id == "fg.stereo_features") S.stereo_shared = v == "1";
  });
  lab_verb("fg", [](const std::vector<std::string>& a, bool& ok) -> std::string {
    std::string sub = a.empty() ? "status" : a[0];
    if (sub == "status") {
      std::string perf;
      {
        std::lock_guard lk(S.perf_mu);
        perf = S.perf_json;
      }
      return "{\"enabled\":" + std::string(S.enabled ? "true" : "false") + ",\"feature\":" +
             std::string(S.feat[0].h ? "true" : "false") + ",\"feature_right\":" + std::string(S.feat[1].h ? "true" : "false") +
             ",\"stereo_link\":" + std::string(S.link.eye ? "true" : "false") + ",\"frames_per_eye\":" +
             std::to_string(S.n_views2.load()) + ",\"frames_per_eye_mono\":" + std::to_string(S.n_views2_mono.load()) +
             ",\"size\":\"" + std::to_string(S.w) + "x" + std::to_string(S.h) + "\"" +
             ",\"max_count\":" + std::to_string(S.caps_max_gen + 1) +
             ",\"perf\":" + perf + ",\"trace\":" + trace_status_json() + ",\"reflex\":" + reflex_status_json() + "}";
    }
    if (sub == "latency") {
      std::string what = a.size() > 1 ? a[1] : "";
      if (what == "start") return json_str(lat_start(S.dev, S.q));
      if (what == "stop") return json_str(lat_stop());
      ok = false;
      return json_str("usage: fg latency start|stop");
    }
    if (sub == "trace") {
      unsigned n = a.size() > 1 ? unsigned(atoi(a[1].c_str())) : 480;
      unsigned shots = a.size() > 2 ? unsigned(atoi(a[2].c_str())) : 240;
      int y = a.size() > 3 ? atoi(a[3].c_str()) : -1, h = a.size() > 4 ? atoi(a[4].c_str()) : 0;
      return json_str(trace_arm(n, shots, y, h));
    }
    if (sub == "grab") {  // grab JOBS TAG: the next JOBS jobs with generated frames, written as .raw
      std::lock_guard lk(g_grab_mu);
      if (a.size() > 1 && a[1] == "files") {
        std::string r = "[";
        for (size_t i = 0; i < g_grab_files.size(); i++) r += (i ? "," : "") + json_str(g_grab_files[i]);
        return r + "]";
      }
      g_grab_jobs = a.size() > 1 ? std::max(1, std::min(8, atoi(a[1].c_str()))) : 1;
      g_grab_tag = a.size() > 2 ? a[2] : "grab";
      g_grab_files.clear();
      return json_str("grabbing the next " + std::to_string(g_grab_jobs) + " jobs (fg grab files)");
    }
    ok = false;
    return json_str("usage: fg [status|trace [presents] [shots] [band y] [band h]|grab [jobs] [tag]|grab files]");
  });
}

bool fg_enabled() { return S.enabled; }

void fg_stereo_link(const FgStereoLink& link) {
  if (!S.enabled) return;
  S.link = link;
  hudless_defer(link.hud_pending);
  TH_LOG("fg: stereo linked: one input set and DLSS-G feature per eye in side-by-side frames");
}

void fg_stereo_hudless(void* renderer, void* sbs_nt) {
  if (S.enabled) hudless_copy_from(renderer, sbs_nt);
}

void fg_stereo_frame(int pack, int w, int shift_left, int shift_right, bool pair) {
  if (!S.enabled) return;
  S.lay.on = true;
  S.lay.pack = pack, S.lay.w = w, S.lay.shift[0] = shift_left, S.lay.shift[1] = shift_right, S.lay.pair = pair;
}

void fg_start() {
  S.enabled = cfg_bool("fg.enabled", false);
  if (!S.enabled) return;
  if (!resolve_ngx()) {
    S.enabled = false;
    return;
  }
  void* p = module_export(kD3D, "NVSDK_NGX_D3D12_EvaluateFeature_C", "fg");
  if (!p || !sig_match(p, "48 83 ec 38 48 8b c1 48 8b 0d")) {
    TH_LOG("fg: NVSDK_NGX_D3D12_EvaluateFeature_C missing or signature mismatch: frame generation off");
    S.enabled = false;
    return;
  }
  if (!sig_match((void*)g_auto_bb, "48 8b 0d ?? ?? ?? ?? 48 63 81 38 01 00 00 48 8b 84 c1 40 01 00 00 c3")) {
    TH_LOG("fg: getAutoBackBuffer signature mismatch: frame generation off");
    S.enabled = false;
    return;
  }
  hook_fn(p, (void*)hk_Eval, (void**)&o_Eval, "NVSDK_NGX_D3D12_EvaluateFeature_C");
  S.engine_ok = engine_init();
  AddVectoredExceptionHandler(0, on_exception);
  present_set_buffer_count(cfg_num("fg.buffers", 3) > 1 ? unsigned(cfg_num("fg.buffers", 3)) : 0);
  present_getbuffer(getbuffer);
  present_takeover(takeover);
  present_before_resize(before_resize);
  TH_LOG("fg: enabled (count %u)", S.count.load());
}

}  // namespace th
