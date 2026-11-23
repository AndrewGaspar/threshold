#include "features/stereo/gpu.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "core/hook.h"
#include "core/log.h"
#include "core/module.h"
#include "core/present.h"
#include "features/stereo/engine.h"

namespace th::stereo {
namespace {

using Void_t = void(__cdecl*)();
using GetBB_t = void*(__cdecl*)();
using GetNative_t = void*(__fastcall*)(void*);
Void_t g_submit;
GetBB_t g_get_bb;
GetNative_t g_get_native;

ID3D12Device* g_dev = nullptr;
ID3D12CommandQueue* g_queue = nullptr;

template <typename T>
T& at(void* base, size_t off) {
  return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + off);
}

// A ring of command lists with one fence (renderer thread only).
struct Ring {
  static constexpr int kN = 16;
  ID3D12CommandAllocator* alloc[kN]{};
  ID3D12GraphicsCommandList* list[kN]{};
  uint64_t val[kN]{};
  ID3D12Fence* fence = nullptr;
  HANDLE ev = nullptr;
  uint64_t next = 0;
  int pos = 0, cur = -1;
  bool init() {
    if (FAILED(g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&fence))) return false;
    ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    for (int i = 0; i < kN; i++) {
      if (FAILED(g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                               (void**)&alloc[i])) ||
          FAILED(g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[i], nullptr,
                                          __uuidof(ID3D12GraphicsCommandList), (void**)&list[i])))
        return false;
      list[i]->SetName(L"threshold stereo");
      list[i]->Close();
    }
    return true;
  }
  void wait(uint64_t v) {
    if (!v || fence->GetCompletedValue() >= v) return;
    fence->SetEventOnCompletion(v, ev);
    WaitForSingleObject(ev, 2000);
  }
  ID3D12GraphicsCommandList* begin() {
    cur = pos;
    pos = (pos + 1) % kN;
    wait(val[cur]);
    alloc[cur]->Reset();
    list[cur]->Reset(alloc[cur], nullptr);
    return list[cur];
  }
  uint64_t submit() {
    ID3D12GraphicsCommandList* cl = list[cur];
    cl->Close();
    ID3D12CommandList* l = cl;
    g_queue->ExecuteCommandLists(1, &l);
    g_queue->Signal(fence, ++next);
    val[cur] = next;
    return next;
  }
} g_ring;
bool g_ring_ok = false;

struct Eye {
  ID3D12Resource* r = nullptr;
  D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_COPY_SOURCE;
  uint32_t w = 0, h = 0;
  DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
};
Eye g_eye[2];
ID3D12Resource* g_zero = nullptr;  // upload buffer of zeros (black columns of a shifted eye)
uint64_t g_zero_size = 0;
ID3D12Resource* g_readback = nullptr;
uint64_t g_readback_size = 0;
std::atomic<uint64_t> g_n_stash{0}, g_n_place{0}, g_n_dup{0}, g_n_kick{0}, g_n_fail{0}, g_n_capture{0};
std::atomic<uint64_t> g_n_untracked{0}, g_n_copy{0};

void barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
  if (a == b || !r) return;
  D3D12_RESOURCE_BARRIER x{};
  x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  x.Transition.pResource = r;
  x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  x.Transition.StateBefore = a;
  x.Transition.StateAfter = b;
  cl->ResourceBarrier(1, &x);
}

// An engine texture as the engine tracks it (copyRegion's logic, d3d 0x3d190): the resource, and
// the state it is in now if the engine tracks it (else it is used as is).
struct Tex {
  ID3D12Resource* r = nullptr;
  D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_COMMON;
  bool tracked = false;
  int w = 0, h = 0;
};
bool engine_tex(void* nt, Tex& t) {
  if (!nt) return false;
  t.r = at<ID3D12Resource*>(nt, kNtResource);
  if (!t.r) return false;
  t.w = at<int>(nt, kNtWidth);
  t.h = at<int>(nt, kNtHeight);
  uint8_t* blk = at<uint8_t*>(nt, kNtStatePtr);
  if (!blk) blk = static_cast<uint8_t*>(nt) + kNtStateInline;
  t.st = D3D12_RESOURCE_STATES(*reinterpret_cast<uint32_t*>(blk + kNtStateOff));
  t.tracked = (at<uint8_t>(nt, kNtFlags) & 0x30) == 0 && at<uint8_t>(nt, kNtUntracked) == 0;
  if (!t.tracked) g_n_untracked++;
  return true;
}
// Into/out of a copy state and back to the engine's state.
void enter(ID3D12GraphicsCommandList* cl, const Tex& t, D3D12_RESOURCE_STATES s) {
  if (t.tracked) barrier(cl, t.r, t.st, s);
}
void leave(ID3D12GraphicsCommandList* cl, const Tex& t, D3D12_RESOURCE_STATES s) {
  if (t.tracked) barrier(cl, t.r, s, t.st);
}

bool ensure_eye(int i, uint32_t w, uint32_t h, DXGI_FORMAT fmt) {
  Eye& e = g_eye[i];
  if (e.r && e.w == w && e.h == h && e.fmt == fmt) return true;
  if (e.r) {
    g_ring.wait(g_ring.next);  // the GPU may still read the old one
    e.r->Release();
    e.r = nullptr;
  }
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = w;
  d.Height = h;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.Format = fmt;
  d.SampleDesc.Count = 1;
  d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  HRESULT hr = g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr,
                                              __uuidof(ID3D12Resource), (void**)&e.r);
  if (FAILED(hr)) {
    TH_LOG("stereo gpu: eye texture %ux%u fmt %d: 0x%lx", w, h, int(fmt), hr);
    e.r = nullptr;
    return false;
  }
  e.r->SetName(i ? L"threshold stereo eye R" : L"threshold stereo eye L");
  e.st = D3D12_RESOURCE_STATE_COPY_SOURCE;
  e.w = w, e.h = h, e.fmt = fmt;
  TH_LOG("stereo gpu: eye %d texture %ux%u fmt %d", i, w, h, int(fmt));
  return true;
}

bool ensure_zero(uint64_t bytes) {
  if (g_zero && g_zero_size >= bytes) return true;
  if (g_zero) {
    g_ring.wait(g_ring.next);
    g_zero->Release();
    g_zero = nullptr;
  }
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_UPLOAD;
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  d.Width = bytes;
  d.Height = 1;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.SampleDesc.Count = 1;
  d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                            __uuidof(ID3D12Resource), (void**)&g_zero)))
    return false;
  void* p = nullptr;
  if (SUCCEEDED(g_zero->Map(0, nullptr, &p))) {
    memset(p, 0, size_t(bytes));
    g_zero->Unmap(0, nullptr);
  }
  g_zero_size = bytes;
  return true;
}

void copy_box(ID3D12GraphicsCommandList* cl, ID3D12Resource* dst, int dx, int dy, ID3D12Resource* src, int sx, int sy,
              int w, int h) {
  if (w <= 0 || h <= 0) return;
  D3D12_TEXTURE_COPY_LOCATION d{}, s{};
  d.pResource = dst;
  d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  s.pResource = src;
  s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_BOX b{UINT(sx), UINT(sy), 0, UINT(sx + w), UINT(sy + h), 1};
  cl->CopyTextureRegion(&d, UINT(dx), UINT(dy), 0, &s, &b);
}

// Black columns [x, x+w) x [y, y+h) of dst (in COPY_DEST), from the zero buffer.
void black(ID3D12GraphicsCommandList* cl, ID3D12Resource* dst, int x, int y, int w, int h) {
  if (w <= 0 || h <= 0) return;
  D3D12_RESOURCE_DESC dd = dst->GetDesc();
  UINT pitch = (UINT(w) * 4 + 255) & ~255u;
  if (!ensure_zero(uint64_t(pitch) * UINT(h))) return;
  D3D12_TEXTURE_COPY_LOCATION d{}, s{};
  d.pResource = dst;
  d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  s.pResource = g_zero;
  s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  s.PlacedFootprint.Offset = 0;
  s.PlacedFootprint.Footprint.Format = dd.Format;
  s.PlacedFootprint.Footprint.Width = UINT(w);
  s.PlacedFootprint.Footprint.Height = UINT(h);
  s.PlacedFootprint.Footprint.Depth = 1;
  s.PlacedFootprint.Footprint.RowPitch = pitch;
  cl->CopyTextureRegion(&d, UINT(x), UINT(y), 0, &s, nullptr);
}

float half(uint16_t h) {
  uint32_t e = (h >> 10) & 0x1f, m = h & 0x3ff, sgn = uint32_t(h >> 15) << 31, f;
  if (e == 0) f = m ? 0 : sgn;  // denormals as 0
  else if (e == 31) f = sgn | 0x7f800000u | (m << 13);
  else f = sgn | ((e + 112) << 23) | (m << 13);
  float v;
  memcpy(&v, &f, 4);
  return m && e == 0 ? 0.f : v;
}
float fsmall(uint32_t v, int mbits) {  // unsigned 5-bit-exponent floats (R11G11B10)
  uint32_t e = v >> mbits, m = v & ((1u << mbits) - 1);
  if (e == 31) return m ? NAN : INFINITY;
  return e == 0 ? 0.f : ldexpf(1.f + float(m) / float(1u << mbits), int(e) - 15);
}
float f11(uint32_t v) { return fsmall(v, 6); }
float f10(uint32_t v) { return fsmall(v, 5); }
uint8_t to8(float v) {
  if (!(v > 0)) return 0;  // NaN and negatives black
  v = v >= 1 ? 1 : powf(v, 1 / 2.2f);
  return uint8_t(v * 255 + 0.5f);
}

// The raw texels: "THRW", u32 width, height, row pitch, DXGI format, then height rows of pitch bytes.
bool write_raw(const std::string& path, const uint8_t* src, uint32_t w, uint32_t h, uint32_t pitch, DXGI_FORMAT fmt) {
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return false;
  uint32_t hd[5] = {0x57524854u, w, h, pitch, uint32_t(fmt)};
  fwrite(hd, 4, 5, f);
  fwrite(src, 1, size_t(pitch) * h, f);
  fclose(f);
  return true;
}

bool write_bmp(const std::string& path, const uint8_t* src, uint32_t w, uint32_t h, uint32_t pitch, DXGI_FORMAT fmt) {
  uint32_t row = (w * 3 + 3) & ~3u;
  std::vector<uint8_t> out(54 + size_t(row) * h);
  uint8_t* hd = out.data();
  auto u32 = [&](size_t o, uint32_t v) { memcpy(hd + o, &v, 4); };
  hd[0] = 'B';
  hd[1] = 'M';
  u32(2, uint32_t(out.size()));
  u32(10, 54);
  u32(14, 40);
  u32(18, w);
  u32(22, uint32_t(-int32_t(h)));  // top-down
  hd[26] = 1;
  hd[28] = 24;
  for (uint32_t y = 0; y < h; y++) {
    const uint32_t* s = reinterpret_cast<const uint32_t*>(src + size_t(y) * pitch);
    uint8_t* d = hd + 54 + size_t(y) * row;
    for (uint32_t x = 0; x < w; x++) {
      uint32_t p = s[x];
      uint8_t r, g, b;
      if (fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) {  // scRGB (linear, 1.0 = 80 nits): clamped, sRGB-ish gamma
        const uint16_t* q = reinterpret_cast<const uint16_t*>(src + size_t(y) * pitch) + 4 * x;
        r = to8(half(q[0])), g = to8(half(q[1])), b = to8(half(q[2]));
      } else if (fmt == DXGI_FORMAT_R11G11B10_FLOAT) {
        r = to8(f11(p & 0x7ff)), g = to8(f11((p >> 11) & 0x7ff)), b = to8(f10((p >> 22) & 0x3ff));
      } else if (fmt == DXGI_FORMAT_R10G10B10A2_UNORM) {
        r = uint8_t((p & 0x3ff) >> 2);
        g = uint8_t(((p >> 10) & 0x3ff) >> 2);
        b = uint8_t(((p >> 20) & 0x3ff) >> 2);
      } else if (fmt == DXGI_FORMAT_B8G8R8A8_UNORM || fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
        b = uint8_t(p), g = uint8_t(p >> 8), r = uint8_t(p >> 16);
      } else {
        r = uint8_t(p), g = uint8_t(p >> 8), b = uint8_t(p >> 16);
      }
      d[3 * x] = b, d[3 * x + 1] = g, d[3 * x + 2] = r;
    }
  }
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return false;
  fwrite(out.data(), 1, out.size(), f);
  fclose(f);
  return true;
}

}  // namespace

// ---- draw duplication -------------------------------------------------------------------------
namespace {
using SetVp_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_VIEWPORT*);
using SetSc_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RECT*);
using Draw_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
using DrawIdx_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
SetVp_t o_setvp;
SetSc_t o_setsc;
Draw_t o_draw;
DrawIdx_t o_drawidx;
std::atomic<DWORD> g_dup_tid{0};
std::atomic<int> g_dup_dx{0};
std::atomic<uint64_t> g_n_dup_draws{0};
thread_local D3D12_VIEWPORT t_vp{};
thread_local D3D12_RECT t_sc{};
thread_local bool t_vp_set = false, t_sc_set = false;
bool dup_on() {
  return g_dup_dx.load(std::memory_order_relaxed) != 0 && GetCurrentThreadId() == g_dup_tid.load(std::memory_order_relaxed);
}
void STDMETHODCALLTYPE hk_setvp(ID3D12GraphicsCommandList* cl, UINT n, const D3D12_VIEWPORT* v) {
  if (n >= 1 && v) t_vp = v[0], t_vp_set = true;  // per thread, always: the engine skips redundant sets
  o_setvp(cl, n, v);
}
void STDMETHODCALLTYPE hk_setsc(ID3D12GraphicsCommandList* cl, UINT n, const D3D12_RECT* r) {
  if (n >= 1 && r) t_sc = r[0], t_sc_set = true;
  o_setsc(cl, n, r);
}
std::atomic<DupCb> g_dup_cb{nullptr};
std::atomic<DWORD> g_obs_tid{0};
std::atomic<bool> g_obs{false};
bool obs_on() {
  return g_obs.load(std::memory_order_relaxed) && GetCurrentThreadId() == g_obs_tid.load(std::memory_order_relaxed);
}
D3D12_RECT cur_scissor() {
  return t_sc_set ? t_sc
                  : D3D12_RECT{LONG(t_vp.TopLeftX), LONG(t_vp.TopLeftY), LONG(t_vp.TopLeftX + t_vp.Width),
                               LONG(t_vp.TopLeftY + t_vp.Height)};
}
// One issue of a draw with the viewport moved by `off` pixels (fractional allowed) and the scissor
// by its rounded value, clipped to [lo, hi) when hi > lo (the eye's half); then put back.
template <typename F>
void issue_at(ID3D12GraphicsCommandList* cl, float off, int lo, int hi, F&& draw) {
  if (!t_vp_set || (off == 0 && hi <= lo)) {
    draw();
    return;
  }
  D3D12_VIEWPORT v = t_vp;
  v.TopLeftX += off;
  D3D12_RECT sc = cur_scissor(), sc0 = sc;
  LONG o = LONG(std::lround(off));
  sc.left += o;
  sc.right += o;
  if (hi > lo) {
    sc.left = std::max<LONG>(sc.left, lo);
    sc.right = std::min<LONG>(sc.right, hi);
    if (sc.right <= sc.left) return;  // entirely outside its half
  }
  o_setvp(cl, 1, &v);
  o_setsc(cl, 1, &sc);
  draw();
  o_setvp(cl, 1, &t_vp);
  o_setsc(cl, 1, &sc0);
}
// A draw while armed or observing: the original issue (shifted if the callback says so) and, armed,
// the duplicate moved right by dx plus its own shift. Without a callback (or with no shift) the
// draws are issued exactly as before the UI hook existed.
template <typename F>
void issue(ID3D12GraphicsCommandList* cl, UINT count, UINT start, INT base, F&& draw) {
  bool dup = dup_on();
  DupCb cb = g_dup_cb.load(std::memory_order_relaxed);
  if (!dup && !(cb && obs_on())) {
    draw();
    return;
  }
  DupDraw d;
  d.count = count, d.start = start, d.base = base;
  d.vp_x = t_vp.TopLeftX, d.vp_y = t_vp.TopLeftY, d.vp_w = t_vp.Width, d.vp_h = t_vp.Height;
  d.dx = dup ? g_dup_dx.load() : 0;
  if (cb) cb(cl, 0, d);
  bool moved = d.shift[0] != 0 || d.shift[1] != 0;
  if (!dup) {
    issue_at(cl, d.shift[0], 0, 0, draw);
    return;
  }
  issue_at(cl, d.shift[0], 0, moved ? d.dx : 0, draw);
  if (!t_vp_set) return;
  if (cb) cb(cl, 1, d);
  issue_at(cl, float(d.dx) + d.shift[1], moved ? d.dx : 0, moved ? 2 * d.dx : 0, draw);
  if (cb) cb(cl, 2, d);
  g_n_dup_draws++;
}
void STDMETHODCALLTYPE hk_draw(ID3D12GraphicsCommandList* cl, UINT a, UINT b, UINT c, UINT d) {
  issue(cl, a, c, 0, [&] { o_draw(cl, a, b, c, d); });
}
void STDMETHODCALLTYPE hk_drawidx(ID3D12GraphicsCommandList* cl, UINT a, UINT b, UINT c, INT d, UINT e) {
  issue(cl, a, c, d, [&] { o_drawidx(cl, a, b, c, d, e); });
}
}  // namespace

void gpu_dup_callback(DupCb cb) { g_dup_cb = cb; }
void gpu_draw_observe(bool on) {
  g_obs_tid = GetCurrentThreadId();
  g_obs = on;
}

bool gpu_dup_install() {
  if (o_draw) return true;
  if (!g_ring_ok || !g_ring.list[0]) return false;
  ID3D12GraphicsCommandList* cl = g_ring.list[0];
  o_setvp = reinterpret_cast<SetVp_t>(hook_vtable(cl, 21, (void*)hk_setvp));
  o_setsc = reinterpret_cast<SetSc_t>(hook_vtable(cl, 22, (void*)hk_setsc));
  o_drawidx = reinterpret_cast<DrawIdx_t>(hook_vtable(cl, 13, (void*)hk_drawidx));
  o_draw = reinterpret_cast<Draw_t>(hook_vtable(cl, 12, (void*)hk_draw));
  TH_LOG("stereo gpu: command-list draw duplication hooks installed");
  return o_draw && o_drawidx && o_setvp && o_setsc;
}
void gpu_dup_arm(int dx) {
  g_dup_tid = GetCurrentThreadId();
  g_dup_dx = dx;
}
uint64_t gpu_dup_count() { return g_n_dup_draws.load(); }

bool gpu_start() {
  g_get_bb = reinterpret_cast<GetBB_t>(module_export(kD3D, kExpGetAutoBackBuffer, "stereo"));
  g_submit = reinterpret_cast<Void_t>(module_export(kD3D, kExpSubmitGraphics, "stereo"));
  g_get_native = reinterpret_cast<GetNative_t>(module_export(kD3D, kExpGetNativeTexture, "stereo"));
  if (!g_get_bb || !g_submit || !g_get_native) return false;
  if (!sig_match((void*)g_get_bb, kSigGetAutoBackBuffer) || !sig_match((void*)g_submit, kSigSubmitGraphics)) {
    TH_LOG("stereo gpu: d3d getAutoBackBuffer / submitGraphicsCommandList signature mismatch");
    g_get_bb = nullptr;
    return false;
  }
  return true;
}

bool gpu_attach(IDXGISwapChain* sc) {
  if (g_ring_ok) return true;
  if (!g_get_bb) return false;
  static bool tried = false;
  if (tried) return false;
  tried = true;
  auto* q = static_cast<IUnknown*>(present_queue(sc));
  if (!q || FAILED(q->QueryInterface(__uuidof(ID3D12CommandQueue), (void**)&g_queue)) || !g_queue) {
    TH_LOG("stereo gpu: no D3D12 queue for the swapchain");
    return false;
  }
  g_queue->Release();  // the swapchain keeps it alive
  if (FAILED(g_queue->GetDevice(__uuidof(ID3D12Device), (void**)&g_dev)) || !g_dev) return false;
  g_dev->Release();
  if (!g_ring.init()) {
    TH_LOG("stereo gpu: command list ring failed");
    return false;
  }
  g_ring_ok = true;
  TH_LOG("stereo gpu: attached (queue %p)", (void*)g_queue);
  gpu_dup_install();
  return true;
}

bool gpu_ready() { return g_ring_ok; }

void gpu_kick() {
  if (!g_submit) return;
  g_submit();
  g_n_kick++;
}

void* gpu_back_buffer() { return g_get_bb ? g_get_bb() : nullptr; }
void* gpu_native(void* t2d) { return t2d && g_get_native ? g_get_native(t2d) : nullptr; }

bool gpu_stash(int eye, void* nt, int x, int y, int w, int h) {
  Tex t;
  if (!g_ring_ok || !engine_tex(nt, t) || w <= 0 || h <= 0) return g_n_fail++, false;
  D3D12_RESOURCE_DESC d = t.r->GetDesc();
  if (!ensure_eye(eye, uint32_t(w), uint32_t(h), d.Format)) return g_n_fail++, false;
  Eye& e = g_eye[eye];
  auto* cl = g_ring.begin();
  enter(cl, t, D3D12_RESOURCE_STATE_COPY_SOURCE);
  barrier(cl, e.r, e.st, D3D12_RESOURCE_STATE_COPY_DEST);
  copy_box(cl, e.r, 0, 0, t.r, x, y, std::min<int>(w, int(d.Width) - x), std::min<int>(h, int(d.Height) - y));
  barrier(cl, e.r, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  e.st = D3D12_RESOURCE_STATE_COPY_SOURCE;
  leave(cl, t, D3D12_RESOURCE_STATE_COPY_SOURCE);
  g_ring.submit();
  g_n_stash++;
  return true;
}

bool gpu_place(int eye, void* nt, int x, int y, int shift) {
  Tex t;
  Eye& e = g_eye[eye];
  if (!g_ring_ok || !e.r || !engine_tex(nt, t)) return g_n_fail++, false;
  D3D12_RESOURCE_DESC d = t.r->GetDesc();
  int w = std::min<int>(int(e.w), int(d.Width) - x), h = std::min<int>(int(e.h), int(d.Height) - y);
  if (w <= 0 || h <= 0) return g_n_fail++, false;
  int s = std::max(-w + 1, std::min(w - 1, shift));
  auto* cl = g_ring.begin();
  enter(cl, t, D3D12_RESOURCE_STATE_COPY_DEST);
  if (s >= 0) {
    copy_box(cl, t.r, x + s, y, e.r, 0, 0, w - s, h);
    black(cl, t.r, x, y, s, h);
  } else {
    copy_box(cl, t.r, x, y, e.r, -s, 0, w + s, h);
    black(cl, t.r, x + w + s, y, -s, h);
  }
  leave(cl, t, D3D12_RESOURCE_STATE_COPY_DEST);
  g_ring.submit();
  g_n_place++;
  return true;
}

bool gpu_copy(void* src_nt, int sx, int sy, int w, int h, void* dst_nt, int dx, int dy) {
  Tex s, d;
  if (!g_ring_ok || !engine_tex(src_nt, s) || !engine_tex(dst_nt, d) || s.r == d.r) return g_n_fail++, false;
  D3D12_RESOURCE_DESC sd = s.r->GetDesc(), dd = d.r->GetDesc();
  w = std::min({w, int(sd.Width) - sx, int(dd.Width) - dx});
  h = std::min({h, int(sd.Height) - sy, int(dd.Height) - dy});
  if (w <= 0 || h <= 0) return g_n_fail++, false;
  auto* cl = g_ring.begin();
  enter(cl, s, D3D12_RESOURCE_STATE_COPY_SOURCE);
  enter(cl, d, D3D12_RESOURCE_STATE_COPY_DEST);
  copy_box(cl, d.r, dx, dy, s.r, sx, sy, w, h);
  leave(cl, d, D3D12_RESOURCE_STATE_COPY_DEST);
  leave(cl, s, D3D12_RESOURCE_STATE_COPY_SOURCE);
  g_ring.submit();
  g_n_copy++;
  return true;
}

bool gpu_duplicate(void* nt, int w, int h) {
  Tex t;
  if (!g_ring_ok || !engine_tex(nt, t)) return g_n_fail++, false;
  D3D12_RESOURCE_DESC d = t.r->GetDesc();
  if (w <= 0) w = int(d.Width / 2);
  if (h <= 0) h = int(d.Height);
  if (int(d.Width) < 2 * w || !ensure_eye(0, uint32_t(w), uint32_t(h), d.Format)) return g_n_fail++, false;
  Eye& e = g_eye[0];
  auto* cl = g_ring.begin();
  enter(cl, t, D3D12_RESOURCE_STATE_COPY_SOURCE);
  barrier(cl, e.r, e.st, D3D12_RESOURCE_STATE_COPY_DEST);
  copy_box(cl, e.r, 0, 0, t.r, 0, 0, w, h);
  barrier(cl, e.r, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  e.st = D3D12_RESOURCE_STATE_COPY_SOURCE;
  barrier(cl, t.r, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  copy_box(cl, t.r, w, 0, e.r, 0, 0, w, h);
  barrier(cl, t.r, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  leave(cl, t, D3D12_RESOURCE_STATE_COPY_SOURCE);
  g_ring.submit();
  g_n_dup++;
  return true;
}

bool gpu_capture(void* nt, const std::string& path) {
  Tex t;
  if (!g_ring_ok || !engine_tex(nt, t)) return g_n_fail++, false;
  D3D12_RESOURCE_DESC d = t.r->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
  UINT64 total = 0;
  g_dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
  if (!g_readback || g_readback_size < total) {
    if (g_readback) g_readback->Release();
    g_readback = nullptr;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              __uuidof(ID3D12Resource), (void**)&g_readback)))
      return g_n_fail++, false;
    g_readback_size = total;
  }
  auto* cl = g_ring.begin();
  enter(cl, t, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
  dst.pResource = g_readback;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = fp;
  src.pResource = t.r;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  leave(cl, t, D3D12_RESOURCE_STATE_COPY_SOURCE);
  g_ring.wait(g_ring.submit());
  void* p = nullptr;
  D3D12_RANGE r{0, SIZE_T(total)};
  bool ok = false;
  if (SUCCEEDED(g_readback->Map(0, &r, &p))) {
    bool raw = path.size() > 4 && path.compare(path.size() - 4, 4, ".raw") == 0;
    ok = (raw ? write_raw : write_bmp)(path, static_cast<uint8_t*>(p) + fp.Offset, fp.Footprint.Width, fp.Footprint.Height,
                   fp.Footprint.RowPitch, d.Format);
    D3D12_RANGE none{0, 0};
    g_readback->Unmap(0, &none);
  }
  ok ? g_n_capture++ : g_n_fail++;
  return ok;
}

uint32_t gpu_eye_width() { return g_eye[0].w; }

void* gpu_list_begin() { return g_ring_ok ? g_ring.begin() : nullptr; }
uint64_t gpu_list_submit() { return g_ring.submit(); }
bool gpu_fence_done(uint64_t v) { return g_ring_ok && g_ring.fence->GetCompletedValue() >= v; }

std::string gpu_describe(void* nt) {
  Tex t;
  if (!engine_tex(nt, t)) return "none";
  D3D12_RESOURCE_DESC d = t.r->GetDesc();
  char b[160];
  snprintf(b, sizeof b, "nt %dx%d res %llux%u fmt %d state 0x%x%s", t.w, t.h, (unsigned long long)d.Width, d.Height,
           int(d.Format), unsigned(t.st), t.tracked ? "" : " untracked");
  return b;
}

std::string gpu_status() {
  char b[256];
  snprintf(b, sizeof b,
           "{\"ready\":%s,\"eye\":\"%ux%u\",\"copy\":%llu,\"stash\":%llu,\"place\":%llu,\"dup\":%llu,\"kick\":%llu,\"capture\":%llu,"
           "\"fail\":%llu,\"untracked\":%llu}",
           g_ring_ok ? "true" : "false", g_eye[0].w, g_eye[0].h, (unsigned long long)g_n_copy.load(),
           (unsigned long long)g_n_stash.load(),
           (unsigned long long)g_n_place.load(), (unsigned long long)g_n_dup.load(),
           (unsigned long long)g_n_kick.load(), (unsigned long long)g_n_capture.load(),
           (unsigned long long)g_n_fail.load(), (unsigned long long)g_n_untracked.load());
  return b;
}


bool gpu_copy_on(ID3D12GraphicsCommandList* cl, void* src_nt, void* dst_nt) {
  Tex s, d;
  if (!cl || !engine_tex(src_nt, s) || !engine_tex(dst_nt, d) || s.r == d.r) return g_n_fail++, false;
  D3D12_RESOURCE_DESC sd = s.r->GetDesc(), dd = d.r->GetDesc();
  int w = int(std::min<UINT64>(sd.Width, dd.Width)), h = int(std::min(sd.Height, dd.Height));
  enter(cl, s, D3D12_RESOURCE_STATE_COPY_SOURCE);
  enter(cl, d, D3D12_RESOURCE_STATE_COPY_DEST);
  copy_box(cl, d.r, 0, 0, s.r, 0, 0, w, h);
  leave(cl, d, D3D12_RESOURCE_STATE_COPY_DEST);
  leave(cl, s, D3D12_RESOURCE_STATE_COPY_SOURCE);
  g_n_copy++;
  return true;
}

// Texel probes: a persistently mapped readback buffer, kProbeRing x kProbeSlots placed footprints
// of one texel (512-byte aligned each).
namespace {
ID3D12Resource* g_probe_buf = nullptr;
uint8_t* g_probe_map = nullptr;
DXGI_FORMAT g_probe_fmt[kProbeRing][kProbeSlots] = {};
constexpr UINT64 kProbeStride = 512;
float half_to_float(uint16_t h) {
  uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1f, m = h & 0x3ff;
  float v = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? INFINITY : std::ldexp(float(m | 0x400), int(e) - 25);
  return s ? -v : v;
}
}  // namespace

bool gpu_probe(ID3D12GraphicsCommandList* cl, void* nt, int ring, int slot, int x, int y) {
  Tex t;
  if (!cl || !g_dev || ring < 0 || ring >= kProbeRing || slot < 0 || slot >= kProbeSlots || !engine_tex(nt, t))
    return false;
  if (!g_probe_buf) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = kProbeStride * kProbeSlots * kProbeRing;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              __uuidof(ID3D12Resource), (void**)&g_probe_buf)))
      return false;
    void* p = nullptr;
    D3D12_RANGE r{0, SIZE_T(bd.Width)};
    if (FAILED(g_probe_buf->Map(0, &r, &p))) {
      g_probe_buf->Release();
      g_probe_buf = nullptr;
      return false;
    }
    g_probe_map = static_cast<uint8_t*>(p);
    memset(g_probe_map, 0, size_t(bd.Width));
  }
  D3D12_RESOURCE_DESC d = t.r->GetDesc();
  DXGI_FORMAT f = d.Format;
  if (f != DXGI_FORMAT_R16_FLOAT && f != DXGI_FORMAT_R32_FLOAT && f != DXGI_FORMAT_R32_TYPELESS &&
      f != DXGI_FORMAT_D32_FLOAT && f != DXGI_FORMAT_R16_TYPELESS)
    return false;
  x = std::max(0, std::min(int(d.Width) - 1, x));
  y = std::max(0, std::min(int(d.Height) - 1, y));
  D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
  dst.pResource = g_probe_buf;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint.Offset = (UINT64(ring) * kProbeSlots + slot) * kProbeStride;
  dst.PlacedFootprint.Footprint.Format = f == DXGI_FORMAT_D32_FLOAT ? DXGI_FORMAT_R32_TYPELESS : f;
  dst.PlacedFootprint.Footprint.Width = 1;
  dst.PlacedFootprint.Footprint.Height = 1;
  dst.PlacedFootprint.Footprint.Depth = 1;
  dst.PlacedFootprint.Footprint.RowPitch = 256;
  src.pResource = t.r;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_BOX box{UINT(x), UINT(y), 0, UINT(x + 1), UINT(y + 1), 1};
  enter(cl, t, D3D12_RESOURCE_STATE_COPY_SOURCE);
  cl->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
  leave(cl, t, D3D12_RESOURCE_STATE_COPY_SOURCE);
  g_probe_fmt[ring][slot] = f;
  return true;
}

bool gpu_probe_value(int ring, int slot, float* v) {
  if (!g_probe_map || ring < 0 || ring >= kProbeRing || slot < 0 || slot >= kProbeSlots) return false;
  DXGI_FORMAT f = g_probe_fmt[ring][slot];
  const uint8_t* p = g_probe_map + (UINT64(ring) * kProbeSlots + slot) * kProbeStride;
  if (f == DXGI_FORMAT_R16_FLOAT || f == DXGI_FORMAT_R16_TYPELESS) {
    uint16_t h;
    memcpy(&h, p, 2);
    *v = half_to_float(h);
    return true;
  }
  if (f == DXGI_FORMAT_R32_FLOAT || f == DXGI_FORMAT_R32_TYPELESS || f == DXGI_FORMAT_D32_FLOAT) {
    memcpy(v, p, 4);
    return true;
  }
  return false;
}

}  // namespace th::stereo
