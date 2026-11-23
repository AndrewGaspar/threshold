#include "features/dlss.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>

#include "core/config.h"
#include "core/hook.h"
#include "core/log.h"
#include "core/module.h"

namespace th {
namespace {

constexpr wchar_t kD3D[] = L"d3d_rmdwin10_f.dll";

// NGX values (third_party/DLSS-310.9.16/include/nvsdk_ngx_defs.h).
constexpr int kFeatureSuperSampling = 1;
constexpr int kFeatureRayReconstruction = 13;
constexpr uint32_t kFailFeatureNotSupported = 0xBAD00001;

using CreateFeature_t = uint32_t(__cdecl*)(void* cmdList, int feature, void* params, void** handle);
CreateFeature_t o_CreateFeature;
using GetUI_t = uint32_t(__cdecl*)(void* params, const char* name, unsigned* value);
GetUI_t g_GetUI;  // NVSDK_NGX_Parameter_GetUI, exported by the game's d3d DLL

// "a.b.c.d" of a loaded module's file version, or "" if it isn't loaded.
std::string module_version(const wchar_t* name) {
  HMODULE m = GetModuleHandleW(name);
  if (!m) return "";
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(m, path, MAX_PATH);
  DWORD h = 0, n = GetFileVersionInfoSizeW(path, &h);
  std::string out = "?";
  if (n) {
    std::string buf(n, '\0');
    VS_FIXEDFILEINFO* fi = nullptr;
    UINT len = 0;
    if (GetFileVersionInfoW(path, 0, n, buf.data()) && VerQueryValueW(buf.data(), L"\\", (void**)&fi, &len) && fi)
      out = std::to_string(HIWORD(fi->dwFileVersionMS)) + "." + std::to_string(LOWORD(fi->dwFileVersionMS)) + "." +
            std::to_string(HIWORD(fi->dwFileVersionLS)) + "." + std::to_string(LOWORD(fi->dwFileVersionLS));
  }
  char p8[MAX_PATH * 3];
  WideCharToMultiByte(CP_UTF8, 0, path, -1, p8, sizeof p8, nullptr, nullptr);
  return out + " (" + p8 + ")";
}

// The preset hints the game put in a feature's parameters ("K K K K K K", "-" if unset).
std::string preset_hints(void* params, int feature) {
  static const char* modes[] = {"DLAA", "UltraQuality", "Quality", "Balanced", "Performance", "UltraPerformance"};
  std::string out;
  for (const char* m : modes) {
    std::string key = std::string(feature == kFeatureRayReconstruction ? "RayReconstruction" : "DLSS") +
                      ".Hint.Render.Preset." + m;
    unsigned v = 0;
    bool ok = g_GetUI && params && g_GetUI(params, key.c_str(), &v) == 1;
    if (!out.empty()) out += ' ';
    out += !ok ? "-" : v == 0 ? "default" : std::string(1, char('A' + v - 1));
  }
  return out;
}

std::atomic<bool> g_rr{false};
std::atomic<unsigned> g_rr_refused{0};

const char* feature_name(int f) {
  switch (f) {
    case kFeatureSuperSampling: return "SuperSampling";
    case kFeatureRayReconstruction: return "RayReconstruction";
    default: return "?";
  }
}

uint32_t __cdecl hk_CreateFeature(void* cmdList, int feature, void* params, void** handle) {
  if (feature == kFeatureRayReconstruction && !g_rr.load()) {
    if (g_rr_refused.fetch_add(1) == 0)
      TH_LOG("dlss: refused Ray Reconstruction feature creation (the game's RR inputs are NULL; "
             "[dlss] ray_reconstruction=1 allows it): the game falls back to Super Resolution");
    if (handle) *handle = nullptr;
    return kFailFeatureNotSupported;
  }
  std::string hints = preset_hints(params, feature);
  uint32_t r = o_CreateFeature(cmdList, feature, params, handle);
  TH_LOG("dlss: CreateFeature(%s=%d) = 0x%08x handle %p; preset hints (DLAA UQ Q B P UP): %s", feature_name(feature),
         feature, r, handle ? *handle : nullptr, hints.c_str());
  static bool once = false;
  if (!once && feature == kFeatureSuperSampling) {
    once = true;
    TH_LOG("dlss: nvngx_dlss.dll %s", module_version(L"nvngx_dlss.dll").c_str());
  }
  return r;
}

}  // namespace

void dlss_register() {
  cfg_listen("dlss.ray_reconstruction", [](const std::string&, const std::string& v) {
    g_rr = v == "1" || v == "true" || v == "on";
  });
}

void dlss_start() {
  // Exported by the game's d3d DLL (the NGX SDK is linked statically there); re/dlss.md section 8.
  void* p = module_export(kD3D, "NVSDK_NGX_D3D12_CreateFeature", "dlss");
  if (!p) return;
  if (!sig_match(p, "48 89 6c 24 10 48 89 74 24 18 48 89 7c 24 20 41 56 48 83 ec 20 48 83 3d")) {
    TH_LOG("dlss: NVSDK_NGX_D3D12_CreateFeature signature mismatch at %p: DLSS hooks disabled", p);
    return;
  }
  g_GetUI = (GetUI_t)module_export(kD3D, "NVSDK_NGX_Parameter_GetUI", "dlss");
  hook_fn(p, (void*)hk_CreateFeature, (void**)&o_CreateFeature, "NVSDK_NGX_D3D12_CreateFeature");
}

}  // namespace th
