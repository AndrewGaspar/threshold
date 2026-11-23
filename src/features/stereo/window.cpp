// The full side-by-side window (output = full on a plain 2W-wide desktop; docs/stereo.md).
//
// The game's window is app::WindowX86 (app_rmdwin10_f.dll; static, docs/stereo.md "The full side-by-side window"):
//   +0x10 input object (+0x10 int: 0 absolute cursor, else captured/relative), +0x20 HWND,
//   +0x3c/+0x40 the client size the engine renders at, +0x147 set while the engine's own
//   create/resize call SetWindowPos (its WM_WINDOWPOSCHANGING then forces the size to +0x3c/+0x40),
//   +0x230 borderless (popup at the monitor's origin, sized to the monitor).
// The engine learns its size from its own create (slot 1, app+0x5100) / resize (slot 8, +0x5660) /
// borderless (slot 11, +0x6000) calls (GetClientRect after its SetWindowPos) and from WM_SIZE (its
// window procedure app+0x4150 keeps the size for WM_EXITSIZEMOVE). Absolute mouse positions are
// normalised by the real client rectangle (app+0x6c20 for WM_MOUSEMOVE, the cursor getter
// app+0x6b30). Monitor rectangles come from its own list (app+0x20de8, MONITORINFOEXA entries),
// refreshed by app+0x4c90 (EnumDisplayMonitors) before every use and read by the exported
// app::getMonitorInfo.
//
// With output = full (the swapchain is already 2x wide: core/present):
//   - a monitor at least three times as wide as it is tall (a doubled 16:9 desktop, e.g. 3840x1080)
//     is reported half as wide, so borderless sizes the engine to one eye and windowed modes are
//     centred in the left half;
//   - the real window is widened to a 2W client on the window's own thread (a subclass of the
//     window procedure; widening is posted to it, never done from the renderer thread), keeping the
//     engine's W in the object. When the engine's own resize collapses it back to W (WM_SIZE with
//     W), it is widened again;
//   - WM_SIZE reaches the engine with the width it renders at (half the real one);
//   - absolute mouse positions reach the engine as the position within the eye under the cursor
//     (x mod W), as does the cursor getter. Relative (captured) mouse mode is untouched.
#include "features/stereo/window.h"

#include <windows.h>
#include <dxgi.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/hook.h"
#include "core/log.h"
#include "core/module.h"

namespace th::stereo {
namespace {

constexpr wchar_t kApp[] = L"app_rmdwin10_f.dll";
constexpr uint32_t kAppStamp = 0x67e06a82;  // re/bin
constexpr uintptr_t kRvaWindowObj = 0x20c50;  // WindowX86* (WM_CREATE)
constexpr uintptr_t kRvaMonitorScan = 0x4c90;
constexpr char kSigMonitorScan[] = "40 53 48 83 ec 20 33 db 4c 8d 05 ?? ?? ?? ?? 45 33 c9 89 1d";
constexpr uintptr_t kRvaMonList = 0x20de8, kRvaMonCount = 0x20df0;  // MONITORINFOEXA*, u32
constexpr uintptr_t kRvaMonCopy = 0x20e08, kRvaMonCopyCount = 0x20e10;
constexpr size_t kMonEntry = 0x48, kMonRect = 0x4, kMonWork = 0x14;
constexpr uintptr_t kRvaCursorNorm = 0x6b30;  // Mouse::cursor(this, float out[2]): GetCursorPos / client
constexpr char kSigCursorNorm[] = "48 89 5c 24 18 55 56 57 48 83 ec 40 48 8b 05 ?? ?? ?? ?? 48 33 c4";
constexpr size_t kWinInput = 0x10, kWinHwnd = 0x20, kWinW = 0x3c, kWinH = 0x40, kWinBorderless = 0x230;
constexpr size_t kInputRelative = 0x10;

Image g_app;
std::atomic<bool> g_on{false};
std::atomic<int> g_engine_w{0}, g_engine_h{0}, g_real_w{0};
std::atomic<int> g_n_widen{0}, g_n_size{0}, g_n_mouse{0}, g_n_halved{0}, g_n_collapse{0};
std::atomic<bool> g_refused{false};
HWND g_hwnd = nullptr;
WNDPROC g_prev = nullptr;
UINT g_msg_widen = 0;

template <typename T>
T& at(void* base, size_t off) {
  return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + off);
}
void* win_obj() { return *reinterpret_cast<void**>(g_app.at(kRvaWindowObj)); }

uint32_t stamp(const Image& m) {
  auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m.base);
  auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(m.base + dos->e_lfanew);
  return nt->FileHeader.TimeDateStamp;
}

// ---- monitors: a doubled desktop reported as one eye wide --------------------------------------
using Scan_t = void(__cdecl*)();
Scan_t o_scan;
void halve(uint8_t* list, uint32_t n) {
  for (uint32_t i = 0; list && i < n && i < 16; i++) {
    uint8_t* e = list + size_t(i) * kMonEntry;
    for (size_t r : {kMonRect, kMonWork}) {
      int* rc = reinterpret_cast<int*>(e + r);  // left, top, right, bottom
      int w = rc[2] - rc[0], h = rc[3] - rc[1];
      if (h > 0 && w >= 3 * h) {
        rc[2] = rc[0] + w / 2;
        if (r == kMonRect) g_n_halved++;
      }
    }
  }
}
void __cdecl hk_scan() {
  o_scan();
  if (!g_on.load()) return;
  halve(*reinterpret_cast<uint8_t**>(g_app.at(kRvaMonList)), *reinterpret_cast<uint32_t*>(g_app.at(kRvaMonCount)));
  uint8_t* copy = *reinterpret_cast<uint8_t**>(g_app.at(kRvaMonCopy));
  if (copy != *reinterpret_cast<uint8_t**>(g_app.at(kRvaMonList)))
    halve(copy, *reinterpret_cast<uint32_t*>(g_app.at(kRvaMonCopyCount)));
}

// ---- the window ----------------------------------------------------------------------------------
// Make the real client 2W x H (the engine's W x H stays in the object). Window thread only.
void widen() {
  void* win = win_obj();
  if (!win || !g_hwnd) return;
  int w = at<int>(win, kWinW), h = at<int>(win, kWinH);
  if (w <= 0 || h <= 0) return;
  g_engine_w = w, g_engine_h = h;
  RECT cr{};
  GetClientRect(g_hwnd, &cr);
  if (cr.right - cr.left == 2 * w && cr.bottom - cr.top == h) return;
  DWORD style = DWORD(GetWindowLongW(g_hwnd, GWL_STYLE)), ex = DWORD(GetWindowLongW(g_hwnd, GWL_EXSTYLE));
  RECT r{0, 0, 2 * w, h};
  AdjustWindowRectEx(&r, style, FALSE, ex);
  RECT wr{};
  GetWindowRect(g_hwnd, &wr);
  MONITORINFO mi{sizeof mi};
  GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
  int cx = r.right - r.left, cy = r.bottom - r.top, x = wr.left, y = wr.top;
  if (mi.rcMonitor.right - mi.rcMonitor.left < 2 * w) {  // a 2W client would not fit: leave the window
    // (the side-by-side image is then scaled into it, as without this feature; live, r23: a 3840 window
    // on a 1920 gamescope output showed black)
    if (!g_refused.exchange(true))
      TH_LOG("stereo window: the monitor is %ld wide, less than 2x the engine's %d: the window is not widened",
             mi.rcMonitor.right - mi.rcMonitor.left, w);
    return;
  }
  if (at<uint8_t>(win, kWinBorderless)) {
    x = mi.rcMonitor.left, y = mi.rcMonitor.top;
  } else {  // windowed: keep it on the monitor if it fits, centred as the engine would
    int mw = mi.rcMonitor.right - mi.rcMonitor.left, mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
    x = mi.rcMonitor.left + (cx <= mw ? (mw - cx) / 2 : 0);
    y = mi.rcMonitor.top + (cy <= mh ? (mh - cy) / 2 : 0);
  }
  SetWindowPos(g_hwnd, nullptr, x, y, cx, cy, SWP_NOZORDER | SWP_NOACTIVATE);
  g_n_widen++;
  RECT got{};
  GetClientRect(g_hwnd, &got);
  at<int>(win, kWinW) = w;
  at<int>(win, kWinH) = h;
  bool ok = got.right - got.left == 2 * w && got.bottom - got.top == h;
  g_refused = !ok;
  TH_LOG("stereo window: engine %dx%d (%s), client %ldx%ld -> %ldx%ld at %d,%d%s", w, h,
         at<uint8_t>(win, kWinBorderless) ? "borderless" : "windowed", cr.right - cr.left, cr.bottom - cr.top,
         got.right - got.left, got.bottom - got.top, x, y,
         ok ? "" : " (the window manager kept another size: the side-by-side image is scaled into it)");
}

bool absolute_mouse() {
  void* win = win_obj();
  void* in = win ? at<void*>(win, kWinInput) : nullptr;
  return in && at<int>(in, kInputRelative) == 0;
}

LRESULT CALLBACK sub_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == g_msg_widen && g_msg_widen) {
    widen();
    return 0;
  }
  if (msg == WM_SIZE && wp != SIZE_MINIMIZED) {
    int real = LOWORD(lp), h = HIWORD(lp), ew = g_engine_w.load();
    g_real_w = real;
    g_n_size++;
    if (ew > 0 && real == 2 * ew) {
      lp = MAKELPARAM(ew, h);
    } else if (ew > 0 && real == ew) {  // the engine's own resize set it back to W: widen again
      if (!g_refused.load()) {
        g_n_collapse++;
        PostMessageW(hwnd, g_msg_widen, 0, 0);
      }
    } else if (real > 1 && !g_refused.load()) {  // the player resized / maximised: eyes are half
      g_engine_w = real / 2;
      lp = MAKELPARAM(real / 2, h);
    }
  } else if (msg >= WM_MOUSEMOVE && msg <= WM_MBUTTONDBLCLK && absolute_mouse()) {
    // The engine normalises by the real client width: x' / real must be (x mod W) / W.
    int ew = g_engine_w.load(), real = g_real_w.load();
    int x = short(LOWORD(lp)), y = short(HIWORD(lp));
    if (ew > 0 && real > ew && x >= 0) {
      x = int(int64_t(x % ew) * real / ew);
      lp = MAKELPARAM(x, y);
      g_n_mouse++;
    }
  }
  return CallWindowProcW(g_prev, hwnd, msg, wp, lp);
}

using Cursor_t = float*(__fastcall*)(void*, float*);
Cursor_t o_cursor;
float* __fastcall hk_cursor(void* mouse, float* out) {
  float* r = o_cursor(mouse, out);
  int ew = g_engine_w.load(), real = g_real_w.load();
  if (g_on.load() && out && ew > 0 && real > ew && out[0] >= 0.f && out[0] <= 1.f) {
    float x = out[0] * float(real) / float(ew);
    out[0] = x - std::floor(x);  // the position within the eye the cursor is over
  }
  return r;
}

}  // namespace

bool window_start() {
  g_app = module_image(kApp);
  if (!g_app || stamp(g_app) != kAppStamp) {
    TH_LOG("stereo window: app_rmdwin10_f.dll missing or not the analysed build: the window is not widened");
    return false;
  }
  if (!sig_match((void*)g_app.at(kRvaMonitorScan), kSigMonitorScan) ||
      !sig_match((void*)g_app.at(kRvaCursorNorm), kSigCursorNorm)) {
    TH_LOG("stereo window: app signature mismatch: the window is not widened");
    return false;
  }
  bool ok = hook_fn((void*)g_app.at(kRvaMonitorScan), (void*)hk_scan, (void**)&o_scan, "app monitor scan") &&
            hook_fn((void*)g_app.at(kRvaCursorNorm), (void*)hk_cursor, (void**)&o_cursor, "app Mouse::cursor");
  if (!ok) return false;
  g_msg_widen = RegisterWindowMessageW(L"threshold.stereo.widen");
  g_on = true;
  TH_LOG("stereo window: output full: the window will be 2x the engine's width");
  return true;
}

void window_present(IDXGISwapChain* sc) {
  if (!g_on.load() || g_prev) return;
  BOOL fs = FALSE;
  if (sc && SUCCEEDED(sc->GetFullscreenState(&fs, nullptr)) && fs) {
    // Exclusive fullscreen: the display mode is the 2x swapchain's own size; the window is left to
    // DXGI (subclassing it here made WM_SIZE arrive every frame and the renderer stall; live, wide r2).
    static bool logged = false;
    if (!logged) TH_LOG("stereo window: exclusive fullscreen: the window is left as DXGI sizes it");
    logged = true;
    return;
  }
  void* win = win_obj();
  HWND hwnd = win ? at<HWND>(win, kWinHwnd) : nullptr;
  if (!hwnd) return;
  g_hwnd = hwnd;
  g_engine_w = at<int>(win, kWinW);
  g_engine_h = at<int>(win, kWinH);
  RECT cr{};
  GetClientRect(hwnd, &cr);
  g_real_w = cr.right - cr.left;
  g_prev = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(sub_proc)));
  if (!g_prev) {
    TH_LOG("stereo window: subclassing the window failed (%lu)", GetLastError());
    g_on = false;
    return;
  }
  PostMessageW(hwnd, g_msg_widen, 0, 0);  // widened on the window's own thread
}

std::string window_status() {
  char b[320];
  snprintf(b, sizeof b,
           "{\"on\":%s,\"engine\":\"%dx%d\",\"real_w\":%d,\"widen\":%d,\"collapse\":%d,\"wm_size\":%d,\"mouse\":%d,"
           "\"monitors_halved\":%d,\"refused\":%s}",
           g_on.load() ? "true" : "false", g_engine_w.load(), g_engine_h.load(), g_real_w.load(), g_n_widen.load(),
           g_n_collapse.load(), g_n_size.load(), g_n_mouse.load(), g_n_halved.load(), g_refused.load() ? "true" : "false");
  return b;
}

}  // namespace th::stereo
