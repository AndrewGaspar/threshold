#include "features/dlss.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>

#include "core/config.h"
#include "core/hook.h"
#include "core/log.h"
#include "core/module.h"
#include "core/quiver.h"

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
using GetI_t = uint32_t(__cdecl*)(void* params, const char* name, int* value);
GetI_t g_GetI;  // NVSDK_NGX_Parameter_GetI
using ReleaseFeature_t = uint32_t(__cdecl*)(void* handle);
ReleaseFeature_t o_ReleaseFeature;

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

// HUD metrics (Quiver's Sight): the upscaler and the render size, from the game's latest Super
// Resolution feature creation; "off" once it has released every SR feature (DLSS off in its menu).
::quiver_metric g_m_upscaler = QUIVER_NO_METRIC, g_m_res = QUIVER_NO_METRIC;
std::mutex g_sr_mu;
std::set<void*> g_sr;  // live SR feature handles (stereo's per-eye DLSS makes two)

unsigned get_ui(void* params, const char* name) {
  unsigned v = 0;
  return g_GetUI && params && g_GetUI(params, name, &v) == 1 ? v : 0;
}

// NVSDK_NGX_PerfQuality_Value (nvsdk_ngx_defs.h): MaxPerf, Balanced, MaxQuality, UltraPerformance,
// UltraQuality, DLAA; the hint key's mode name and the label shown.
struct Mode {
  const char* hint;
  const char* label;
};
constexpr Mode kModes[] = {{"Performance", "Performance"}, {"Balanced", "Balanced"}, {"Quality", "Quality"},
                           {"UltraPerformance", "Ultra Perf"}, {"UltraQuality", "Ultra Quality"}, {"DLAA", "DLAA"}};

void report_sr(void* params) {
  int q = -1;
  if (!g_GetI || !params || g_GetI(params, "PerfQualityValue", &q) != 1) q = -1;
  std::string text = "DLSS";
  if (q >= 0 && q < int(std::size(kModes))) {
    text += std::string(" ") + kModes[q].label;
    unsigned p = get_ui(params, (std::string("DLSS.Hint.Render.Preset.") + kModes[q].hint).c_str());
    if (p) text += std::string(" ") + char('A' + p - 1);
  }
  const unsigned w = get_ui(params, "Width"), h = get_ui(params, "Height");
  const unsigned ow = get_ui(params, "OutWidth"), oh = get_ui(params, "OutHeight");
  char res[48];
  snprintf(res, sizeof res, "%u\u00d7%u", w, h);
  ::quiver_text(quiver_sdk(), g_m_upscaler, text.c_str());
  if (w && h) ::quiver_text(quiver_sdk(), g_m_res, res);
  TH_LOG("dlss: HUD: upscaler \"%s\", render %ux%u -> %ux%u", text.c_str(), w, h, ow, oh);
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
  if (feature == kFeatureSuperSampling && r == 1 && handle && *handle) {
    {
      std::lock_guard lk(g_sr_mu);
      g_sr.insert(*handle);
    }
    report_sr(params);
  }
  static bool once = false;
  if (!once && feature == kFeatureSuperSampling) {
    once = true;
    TH_LOG("dlss: nvngx_dlss.dll %s", module_version(L"nvngx_dlss.dll").c_str());
  }
  return r;
}

uint32_t __cdecl hk_ReleaseFeature(void* handle) {
  bool last = false;
  {
    std::lock_guard lk(g_sr_mu);
    last = g_sr.erase(handle) && g_sr.empty();
  }
  if (last) {
    ::quiver_text(quiver_sdk(), g_m_upscaler, "off");
    ::quiver_text(quiver_sdk(), g_m_res, "native");
    TH_LOG("dlss: HUD: the last Super Resolution feature was released: upscaler \"off\"");
  }
  return o_ReleaseFeature(handle);
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
  g_GetI = (GetI_t)module_export(kD3D, "NVSDK_NGX_Parameter_GetI", "dlss");
  hook_fn(p, (void*)hk_CreateFeature, (void**)&o_CreateFeature, "NVSDK_NGX_D3D12_CreateFeature");
  // The upscaler for Sight's HUD (docs/dlss.md "HUD metrics"), set at feature creation and release.
  g_m_upscaler = metric_declare("upscaler", QUIVER_TEXT,
      "\"label\":\"Upscaler\",\"short\":\"SR\",\"help\":\"The game's DLSS Super Resolution: its quality mode and the "
      "preset the game asks for (a driver override can replace it), or off.\",\"group\":\"Upscaling\",\"order\":10,"
      "\"level\":\"basic\"");
  g_m_res = metric_declare("res.internal", QUIVER_TEXT,
      "\"label\":\"Internal resolution\",\"help\":\"The size the game renders at before DLSS upscales it (one eye's in "
      "stereo 3D).\",\"group\":\"Upscaling\",\"order\":11,\"level\":\"basic\"");
  void* rel = module_export(kD3D, "NVSDK_NGX_D3D12_ReleaseFeature", "dlss");
  if (rel && sig_match(rel, "48 89 5c 24 08 57 48 83 ec 20 48 8b 1d"))
    hook_fn(rel, (void*)hk_ReleaseFeature, (void**)&o_ReleaseFeature, "NVSDK_NGX_D3D12_ReleaseFeature");
  else
    TH_LOG("dlss: NVSDK_NGX_D3D12_ReleaseFeature missing or signature mismatch at %p: the HUD won't see DLSS turned off", rel);
}

}  // namespace th
