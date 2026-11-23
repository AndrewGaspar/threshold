// dxgi.dll proxy. The game loads this from its own folder (Proton prefers native dxgi); we load
// the real dxgi.dll from the system directory (DXVK's under Proton) and forward every export,
// hooking each factory we hand out so swapchains get our Present hooks.
//
// Initialisation happens once, on the first export call or in a thread started from DllMain
// (DllMain itself only records the module and starts that thread: no loader-lock work).
#include <windows.h>
#include <dxgi1_4.h>

#include <mutex>
#include <string>

#include "core/config.h"
#include "core/hook.h"
#include "core/lab.h"
#include "core/log.h"
#include "core/module.h"
#include "core/present.h"
#include "core/quiver.h"
#include "features/features.h"
#include "features/rsettings.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {

HMODULE g_real = nullptr;
std::once_flag g_init_once;

HMODULE real_dxgi() {
  if (g_real) return g_real;
  wchar_t path[MAX_PATH];
  UINT n = GetSystemDirectoryW(path, MAX_PATH);
  std::wstring p(path, n);
  p += L"\\dxgi.dll";
  g_real = LoadLibraryW(p.c_str());
  if (g_real == reinterpret_cast<HMODULE>(&__ImageBase)) g_real = nullptr;
  return g_real;
}

void init() {
  std::call_once(g_init_once, [] {
    th::log_init();
    const auto& img = th::game_image();
    TH_LOG("threshold %s (built %s %s), game image %p size 0x%zx", TH_VERSION, __DATE__, __TIME__,
           (void*)img.base, img.size);
    TH_LOG("real dxgi: %p", (void*)real_dxgi());
    th::hook_init();
    th::lab_init();
    th::rsettings_report();
    th::features_register();  // listeners first, so they see the initial config
    th::config_init();
    th::quiver_init();
    th::features_start();
  });
}

template <typename T>
T real_proc(const char* name) {
  HMODULE m = real_dxgi();
  T fn = m ? reinterpret_cast<T>(GetProcAddress(m, name)) : nullptr;
  if (!fn) TH_LOG("real dxgi has no %s", name);
  return fn;
}

void hook_factory(HRESULT hr, REFIID riid, void** out) {
  if (FAILED(hr) || !out || !*out) return;
  IUnknown* u = static_cast<IUnknown*>(*out);
  IDXGIFactory* f = nullptr;
  if (SUCCEEDED(u->QueryInterface(__uuidof(IDXGIFactory), (void**)&f)) && f) {
    th::present_hook_factory(f);
    f->Release();
  }
}

DWORD WINAPI init_thread(void*) {
  init();
  return 0;
}

}  // namespace

extern "C" {

__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** out) {
  init();
  static auto fn = real_proc<HRESULT(WINAPI*)(REFIID, void**)>("CreateDXGIFactory");
  if (!fn) return E_FAIL;
  HRESULT hr = fn(riid, out);
  hook_factory(hr, riid, out);
  return hr;
}

__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** out) {
  init();
  static auto fn = real_proc<HRESULT(WINAPI*)(REFIID, void**)>("CreateDXGIFactory1");
  if (!fn) return E_FAIL;
  HRESULT hr = fn(riid, out);
  hook_factory(hr, riid, out);
  return hr;
}

__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void** out) {
  init();
  static auto fn = real_proc<HRESULT(WINAPI*)(UINT, REFIID, void**)>("CreateDXGIFactory2");
  if (!fn) return E_FAIL;
  HRESULT hr = fn(flags, riid, out);
  hook_factory(hr, riid, out);
  return hr;
}

__declspec(dllexport) HRESULT WINAPI DXGIDeclareAdapterRemovalSupport() {
  init();
  static auto fn = real_proc<HRESULT(WINAPI*)()>("DXGIDeclareAdapterRemovalSupport");
  return fn ? fn() : S_OK;
}

__declspec(dllexport) HRESULT WINAPI DXGIGetDebugInterface1(UINT flags, REFIID riid, void** out) {
  init();
  static auto fn = real_proc<HRESULT(WINAPI*)(UINT, REFIID, void**)>("DXGIGetDebugInterface1");
  return fn ? fn(flags, riid, out) : E_NOINTERFACE;
}

}  // extern "C"

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(inst);
    // Must beat the renderer reading renderer.ini: plain file I/O only.
    th::rsettings_early();
    // The game imports dxgi statically, so we're loaded before its entry point runs. Start init
    // off the loader lock; the first export call also waits for it.
    CloseHandle(CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr));
  }
  return TRUE;
}
