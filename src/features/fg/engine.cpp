#include "features/fg/engine.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>

#include "core/log.h"
#include "core/module.h"
#include "features/fg/common.h"

namespace th::fg {
namespace {

constexpr wchar_t kRenderer[] = L"renderer_rmdwin10_f.dll";
constexpr wchar_t kD3D[] = L"d3d_rmdwin10_f.dll";

// renderer (static, re/stereo.md 1.3 / 2.3; signatures from re/bin)
constexpr char kExpGetInstance[] = "?getInstance@RendererX86@rend@@SAPEAVRendererInterface@2@XZ";
constexpr char kSigGetInstance[] = "48 8b 05 ?? ?? ?? ?? c3";
constexpr uintptr_t kRvaVtbl = 0x6323d8;  // RendererX86 main vtable
constexpr size_t kSlotRenderHUD = 17;
constexpr uintptr_t kRvaRenderHUD = 0x1343b0;
constexpr char kSigRenderHUD[] = "48 8b c4 55 53 56 57 41 54 41 55 41 56 41 57 48 8d a8 18 fe ff ff 48 81 ec a8 02 00 00";
constexpr uintptr_t kRvaFrameCounter = 0x8029d0;  // u32, ++ per frame begin (slot 14)
constexpr uint32_t kRendererStamp = 0x67e06a60;   // analysed build (PE TimeDateStamp)
constexpr size_t kRMainView = 0x678;              // rend::View*
constexpr size_t kRRtManager = 0x20;              // rend::RenderTargetManager*
constexpr size_t kRColor = 0x3c8;                 // Texture2D*: the frame's colour target (HUD draws into it)
// rend::View: embedded m::View at +0x10 (+0x10 world-to-view 4x3, +0x70 view-to-world 4x3, +0xd0
// view-to-clip 4x4, +0x150 clip-to-view 4x4, all double, row vectors); stored previous view-to-clip
// +0x3d0 (4x4), world-to-view +0x450 (4x3), "has previous" +0x4b0 (written by storePrevious in `end`).
constexpr size_t kVW2V = 0x10 + 0x10, kVV2W = 0x10 + 0x70, kVV2C = 0x10 + 0xd0, kVC2V = 0x10 + 0x150;
constexpr size_t kVPrevV2C = 0x3d0, kVPrevW2V = 0x450, kVHasPrev = 0x4b0;
// The engine's own float copies of two uniforms (diagnostics only; ctor 0x17dfe0 strings):
constexpr size_t kVUniC2PNoJ = 0x7e8;  // g_mClipToPreviousClipNoJitter

// d3d (exports; signatures as in src/features/stereo/engine.h)
constexpr char kExpTexture2DInit[] =
    "??0Texture2DInit@d3d@@QEAA@HHHW4PixelFormat@1@HHPEBX_KW4MiscFlags@1@HMHV?$Vector4Template@M@m@@W4TileMode@1@@Z";
constexpr char kSigTexture2DInit[] = "f3 0f 10 44 24 60 33 c0 89 41 08 0f b6 44 24 28 88 41 14 8b 44 24 30 89";
constexpr char kExpAllocTexture2D[] =
    "?allocTexture2D@RenderTargetManager@rend@@QEAAPEAVTexture2D@d3d@@AEBVTexture2DInit@4@PEBD@Z";  // renderer
constexpr char kSigAllocTexture2D[] = "48 8b c4 55 41 54 41 55 41 56 41 57 48 8d 6c 24 90 48 81 ec 70 01 00 00";
constexpr char kExpGetNativeTexture[] = "?getNativeTexture@NativeTextureContainer@d3d@@QEBAPEAVNativeTexture@2@XZ";
constexpr char kSigGetNativeTexture[] = "48 8b 41 08 c3";
constexpr char kExpCopyRegion[] =
    "?copyRegion@NativeTextureUtil@d3d@@SAXPEAVNativeTexture@2@HHAEBV?$Vector3Template@H@m@@0HH1PEBV45@@Z";
constexpr char kSigCopyRegion[] = "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8d 6c 24 98 48 81 ec 68 01 00";
constexpr size_t kTexture2DInitSize = 0x60;
// d3d::NativeTexture (static: NativeTextureUtil::copyRegion 0x3d190)
constexpr size_t kNtWidth = 0x0, kNtHeight = 0x4, kNtPixelFormat = 0x24, kNtFlags = 0x38, kNtStatePtr = 0x50,
                 kNtStateInline = 0x40, kNtStateOff = 0x20, kNtUntracked = 0x68, kNtResource = 0x88;

using GetInstance_t = void*(__cdecl*)();
using Tex2DInit_t = void(__fastcall*)(void* self, int w, int h, int mips, int fmt, int usage, int a6, const void* data,
                                      uint64_t size, int misc, int a10, float a11, int a12, const float* clear, int tile);
using Alloc_t = void*(__fastcall*)(void* mgr, const void* init, const char* name);
using GetNative_t = void*(__fastcall*)(void* t2d);
using CopyRegion_t = void(__cdecl*)(void* dst, int dslice, int dmip, const int* dpos, void* src, int sslice, int smip,
                                    const int* spos, const int* size);
using RenderHUD_t = void(__fastcall*)(void* r);

Image g_rimg;
GetInstance_t g_instance;
Tex2DInit_t g_tex2d_init;
Alloc_t g_rt_alloc;
GetNative_t g_get_native;
CopyRegion_t g_copy_region;
RenderHUD_t o_renderHUD;
bool g_ok = false;

template <typename T>
T& at(void* base, size_t off) {
  return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + off);
}

// ---- matrices (double, row vectors) ----
struct M4 {
  double m[4][4] = {};
};
M4 load44(const void* p) {
  M4 r;
  std::memcpy(r.m, p, sizeof r.m);
  return r;
}
M4 load43(const void* p) {  // 4 rows of 3: the 4th column is (0, 0, 0, 1)
  const double* d = static_cast<const double*>(p);
  M4 r;
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 3; ++j) r.m[i][j] = d[i * 3 + j];
    r.m[i][3] = i == 3 ? 1 : 0;
  }
  return r;
}
M4 mul(const M4& a, const M4& b) {
  M4 r;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      for (int k = 0; k < 4; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
  return r;
}
bool inverse(const M4& a, M4& out) {
  double t[4][8];
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 8; ++j) t[i][j] = j < 4 ? a.m[i][j] : (j - 4 == i ? 1 : 0);
  for (int c = 0; c < 4; ++c) {
    int p = c;
    for (int r = c + 1; r < 4; ++r)
      if (std::fabs(t[r][c]) > std::fabs(t[p][c])) p = r;
    if (std::fabs(t[p][c]) < 1e-30) return false;
    if (p != c)
      for (int j = 0; j < 8; ++j) std::swap(t[p][j], t[c][j]);
    double d = t[c][c];
    for (int j = 0; j < 8; ++j) t[c][j] /= d;
    for (int r = 0; r < 4; ++r)
      if (r != c) {
        double f = t[r][c];
        for (int j = 0; j < 8; ++j) t[r][j] -= f * t[c][j];
      }
  }
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) out.m[i][j] = t[i][j + 4];
  return true;
}
void store(const M4& a, float out[16]) {
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) out[i * 4 + j] = float(a.m[i][j]);
}
bool finite(const M4& a) {
  for (auto& r : a.m)
    for (double v : r)
      if (!std::isfinite(v)) return false;
  return true;
}

// ---- HUD-less ----
std::atomic<bool> g_hud_arm{false};
bool g_hud_have = false;  // a copy was taken since the last take (renderer thread)
// Texture2D* from the engine's pool, one per size/format seen (at most two: the eye-wide colour target
// and stereo's 2W side-by-side texture); g_hud_cur is the latest copy's.
struct HudTex {
  void* tex = nullptr;
  int w = 0, h = 0, fmt = -1;
};
HudTex g_hud[2];
int g_hud_cur = 0, g_hud_next = 0;
bool (*g_hud_pending)() = nullptr;
std::atomic<uint64_t> g_hud_n{0};

void* hud_texture(void* r, void* src_nt) {
  int w = at<int>(src_nt, kNtWidth), h = at<int>(src_nt, kNtHeight), fmt = at<int8_t>(src_nt, kNtPixelFormat);
  for (int i = 0; i < 2; ++i)
    if (g_hud[i].tex && g_hud[i].w == w && g_hud[i].h == h && g_hud[i].fmt == fmt) {
      g_hud_cur = i;
      return g_get_native(g_hud[i].tex);
    }
  void* mgr = at<void*>(r, kRRtManager);
  if (!mgr) return nullptr;
  // A size or format not cached: a new texture in the next slot. The one it replaces stays allocated
  // from the pool (one per resolution change, a few MB); it is never handed back, so nothing else can
  // be given it while a copy may still be in flight.
  alignas(16) uint8_t init[kTexture2DInitSize + 0x20] = {};
  alignas(16) float clear[4] = {0, 0, 0, 0};
  // As renderHUD makes the colour target: (w, h, 1 mip, format, usage 5, ..., 0.5f, 1, clear, -1).
  g_tex2d_init(init, w, h, 1, fmt, 5, 0, nullptr, 0, 0, 0, 0.5f, 1, clear, -1);
  const int i = g_hud_next;
  g_hud_next = (g_hud_next + 1) % 2;
  g_hud[i] = {g_rt_alloc(mgr, init, "Threshold::fgHudLess"), w, h, fmt};
  g_hud_cur = i;
  void* nt = g_hud[i].tex ? g_get_native(g_hud[i].tex) : nullptr;
  ID3D12Resource* res = nt ? at<ID3D12Resource*>(nt, kNtResource) : nullptr;
  ID3D12Resource* sres = at<ID3D12Resource*>(src_nt, kNtResource);
  TH_LOG("fg: HUD-less texture %dx%d pixel format %d (DXGI %d; source DXGI %d): %p", w, h, fmt,
         res ? int(res->GetDesc().Format) : -1, sres ? int(sres->GetDesc().Format) : -1, g_hud[i].tex);
  return nt;
}

bool copy_from(void* r, void* src) {
  void* dst = src ? hud_texture(r, src) : nullptr;
  if (!dst) return false;
  int w = at<int>(src, kNtWidth), h = at<int>(src, kNtHeight);
  int p0[3] = {0, 0, 0}, sz[3] = {w, h, 1};
  g_copy_region(dst, 0, 0, p0, src, 0, 0, p0, sz);
  g_hud_have = true;
  g_hud_n++;
  return true;
}

void __fastcall hk_renderHUD(void* r) {
  if (!(g_hud_pending && g_hud_pending()) && g_hud_arm.exchange(false)) {
    void* t2d = at<void*>(r, kRColor);
    if (!copy_from(r, t2d ? g_get_native(t2d) : nullptr)) {
      static int n = 0;
      if (n++ < 3) TH_LOG("fg: renderHUD without a colour target (+0x3c8 %p): no HUD-less this frame", t2d);
    }
  }
  o_renderHUD(r);
}

}  // namespace

bool engine_init() {
  g_rimg = module_image(kRenderer);
  if (!g_rimg) return false;
  auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_rimg.base);
  auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_rimg.base + dos->e_lfanew);
  if (nt->FileHeader.TimeDateStamp != kRendererStamp) {
    TH_LOG("fg: renderer_rmdwin10_f.dll build %#lx, analysed %#x: identity camera, no HUD-less",
           (unsigned long)nt->FileHeader.TimeDateStamp, kRendererStamp);
    return false;
  }
  g_instance = reinterpret_cast<GetInstance_t>(module_export(kRenderer, kExpGetInstance, "fg"));
  g_tex2d_init = reinterpret_cast<Tex2DInit_t>(module_export(kD3D, kExpTexture2DInit, "fg"));
  g_rt_alloc = reinterpret_cast<Alloc_t>(module_export(kRenderer, kExpAllocTexture2D, "fg"));
  g_get_native = reinterpret_cast<GetNative_t>(module_export(kD3D, kExpGetNativeTexture, "fg"));
  g_copy_region = reinterpret_cast<CopyRegion_t>(module_export(kD3D, kExpCopyRegion, "fg"));
  if (!g_instance || !sig_match((void*)g_instance, kSigGetInstance) || !g_tex2d_init ||
      !sig_match((void*)g_tex2d_init, kSigTexture2DInit) || !g_rt_alloc ||
      !sig_match((void*)g_rt_alloc, kSigAllocTexture2D) || !g_get_native ||
      !sig_match((void*)g_get_native, kSigGetNativeTexture) || !g_copy_region ||
      !sig_match((void*)g_copy_region, kSigCopyRegion)) {
    TH_LOG("fg: renderer/d3d exports missing or signature mismatch: identity camera, no HUD-less");
    return false;
  }
  g_ok = true;
  return true;
}

bool camera_read(Camera& c) {
  c.ok = false;
  if (!g_ok) return false;
  void* r = g_instance();
  void* v = r ? at<void*>(r, kRMainView) : nullptr;
  if (!v) return false;
  uint8_t* b = static_cast<uint8_t*>(v);
  M4 v2c = load44(b + kVV2C), c2v = load44(b + kVC2V), v2w = load43(b + kVV2W), w2v = load43(b + kVW2V);
  c.has_prev = at<uint8_t>(v, kVHasPrev) != 0;
  M4 pv2c = c.has_prev ? load44(b + kVPrevV2C) : v2c;
  M4 pw2v = c.has_prev ? load43(b + kVPrevW2V) : w2v;
  // current clip -> view -> world -> previous view -> previous clip (row vectors: left to right)
  M4 c2p = mul(mul(c2v, v2w), mul(pw2v, pv2c)), p2c;
  if (!inverse(c2p, p2c) || !finite(c2p) || !finite(v2c)) return false;
  store(v2c, c.view_to_clip);
  store(c2v, c.clip_to_view);
  store(c2p, c.clip_to_prev);
  store(p2c, c.prev_to_clip);
  auto unit = [](const M4& m, int row, float out[3]) {
    double l = std::sqrt(m.m[row][0] * m.m[row][0] + m.m[row][1] * m.m[row][1] + m.m[row][2] * m.m[row][2]);
    for (int i = 0; i < 3; ++i) out[i] = float(l > 0 ? m.m[row][i] / l : 0);
  };
  unit(v2w, 0, c.right);  // view-to-world rows: right, up, forward, position (left-handed, +z forward)
  unit(v2w, 1, c.up);
  unit(v2w, 2, c.fwd);
  for (int i = 0; i < 3; ++i) c.pos[i] = float(v2w.m[3][i]);
  // Projection (rl PerspectiveView::calculateProjection): m00 = 1/tan(fovx/2)/zoom, m11 = aspect * m00,
  // m22 = f/(f-n), m23 = 1, m32 = -n f/(f-n): conventional Z.
  double m22 = v2c.m[2][2], m32 = v2c.m[3][2];
  if (m22 == 0 || m22 == 1) return false;
  c.near_z = float(-m32 / m22);
  c.far_z = float(m22 * (-m32 / m22) / (m22 - 1));
  c.fov = float(2.0 * std::atan(1.0 / std::fabs(v2c.m[1][1])));  // vertical
  c.aspect = float(std::fabs(v2c.m[1][1] / v2c.m[0][0]));
  c.counter = *reinterpret_cast<uint32_t*>(g_rimg.at(kRvaFrameCounter));
  c.ok = std::isfinite(c.near_z) && c.near_z > 0 && std::isfinite(c.far_z) && c.far_z > c.near_z;
  return c.ok;
}

void camera_crop(const Camera& in, int p0, int wc, int w, Camera& out) {
  out = in;
  if (!in.ok || wc <= 0 || w <= 0 || (p0 == 0 && wc == w)) return;
  const double k = double(w) / wc, o = double(w - 2 * p0) / wc - 1;
  M4 c, ci;  // identity but column 0: x' = x k + w o
  for (int i = 0; i < 4; ++i) c.m[i][i] = ci.m[i][i] = 1;
  c.m[0][0] = k, c.m[3][0] = o;
  ci.m[0][0] = 1 / k, ci.m[3][0] = -o / k;
  auto ld = [](const float f[16]) {
    M4 m;
    for (int i = 0; i < 16; ++i) m.m[i / 4][i % 4] = f[i];
    return m;
  };
  store(mul(ld(in.view_to_clip), c), out.view_to_clip);
  store(mul(ci, ld(in.clip_to_view)), out.clip_to_view);
  store(mul(mul(ci, ld(in.clip_to_prev)), c), out.clip_to_prev);
  store(mul(mul(ci, ld(in.prev_to_clip)), c), out.prev_to_clip);
  out.aspect = float(in.aspect / k);
}

void camera_log(const Camera& c) {
  static int64_t last = 0;
  static int n = 0;
  int64_t t = now_us();
  if (n >= 3 && t - last < 10000000) return;
  n++;
  last = t;
  // The engine's own g_mClipToPreviousClipNoJitter float copy, for comparison (layout unverified: the
  // first 16 floats after an 8-byte handle are shown).
  float eng[16] = {};
  if (g_ok)
    if (void* r = g_instance())
      if (void* v = at<void*>(r, kRMainView)) std::memcpy(eng, static_cast<uint8_t*>(v) + kVUniC2PNoJ + 8, sizeof eng);
  TH_LOG("fg: camera pos %.2f %.2f %.2f fwd %.3f %.3f %.3f up %.3f %.3f %.3f near %.3f far %.1f fov %.1f deg aspect %.3f "
         "prev %d counter %u; clip->prev row0 %.4f %.4f %.4f %.4f row3 %.4f %.4f %.4f %.4f (engine +0x7f0: %.4f %.4f %.4f "
         "%.4f / %.4f %.4f %.4f %.4f)",
         c.pos[0], c.pos[1], c.pos[2], c.fwd[0], c.fwd[1], c.fwd[2], c.up[0], c.up[1], c.up[2], c.near_z, c.far_z,
         c.fov * 57.29578f, c.aspect, int(c.has_prev), c.counter, c.clip_to_prev[0], c.clip_to_prev[1],
         c.clip_to_prev[2], c.clip_to_prev[3], c.clip_to_prev[12], c.clip_to_prev[13], c.clip_to_prev[14],
         c.clip_to_prev[15], eng[0], eng[1], eng[2], eng[3], eng[12], eng[13], eng[14], eng[15]);
}

// The body's first instruction jumps (directly, or through MinHook's relay) into this module: our own
// inline detour of it (stereo's, made after stereo checked the same signature).
bool detoured_by_us(void* fn) {
  HMODULE self = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&detoured_by_us), &self);
  const uint8_t* p = static_cast<const uint8_t*>(fn);
  for (int hop = 0; hop < 3 && p; ++hop) {
    HMODULE m = nullptr;
    if (hop && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  reinterpret_cast<LPCWSTR>(p), &m) && m == self)
      return true;
    if (p[0] == 0xE9) p = p + 5 + *reinterpret_cast<const int32_t*>(p + 1);
    else if (p[0] == 0xFF && p[1] == 0x25) p = *reinterpret_cast<const uint8_t* const*>(p + 6 + *reinterpret_cast<const int32_t*>(p + 2));
    else return false;
  }
  return false;
}

bool hudless_install() {
  if (!g_ok) return false;
  void** vt = reinterpret_cast<void**>(g_rimg.at(kRvaVtbl));
  void* body = (void*)g_rimg.at(kRvaRenderHUD);
  if (!sig_match(body, kSigRenderHUD) && !detoured_by_us(body)) {
    TH_LOG("fg: renderHUD signature mismatch: no HUD-less");
    return false;
  }
  if (vt[kSlotRenderHUD] == (void*)hk_renderHUD) return true;
  DWORD old = 0;
  if (!VirtualProtect(&vt[kSlotRenderHUD], sizeof(void*), PAGE_READWRITE, &old)) return false;
  o_renderHUD = reinterpret_cast<RenderHUD_t>(vt[kSlotRenderHUD]);  // the body, or someone's detour of it
  vt[kSlotRenderHUD] = (void*)hk_renderHUD;
  VirtualProtect(&vt[kSlotRenderHUD], sizeof(void*), old, &old);
  TH_LOG("fg: HUD-less: RendererX86 vtable slot 17 (renderHUD) patched (was %p, body renderer+%#llx)",
         (void*)o_renderHUD, (unsigned long long)kRvaRenderHUD);
  return true;
}

void hudless_arm(bool on) { g_hud_arm = on; }

void hudless_defer(bool (*pending)()) { g_hud_pending = pending; }

bool hudless_copy_from(void* r, void* src_nt) {
  if (!g_ok || !g_hud_arm.exchange(false)) return false;
  return copy_from(r, src_nt);
}

bool hudless_take(EngTex& t) {
  void* tex = g_hud[g_hud_cur].tex;
  if (!g_hud_have || !tex) return false;
  g_hud_have = false;
  void* nt = g_get_native(tex);
  if (!nt) return false;
  t.r = at<ID3D12Resource*>(nt, kNtResource);
  uint8_t* blk = at<uint8_t*>(nt, kNtStatePtr);
  if (!blk) blk = static_cast<uint8_t*>(nt) + kNtStateInline;
  t.st = D3D12_RESOURCE_STATES(*reinterpret_cast<uint32_t*>(blk + kNtStateOff));
  t.tracked = (at<uint8_t>(nt, kNtFlags) & 0x30) == 0 && at<uint8_t>(nt, kNtUntracked) == 0;
  return t.r != nullptr;
}

uint64_t hudless_copies() { return g_hud_n.load(); }

}  // namespace th::fg
