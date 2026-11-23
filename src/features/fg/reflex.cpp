#include "features/fg/reflex.h"

#include <windows.h>
#include <d3d12.h>

#include <atomic>
#include <cstdio>
#include <mutex>

#include "core/log.h"
#include "features/fg/common.h"
#include "features/fg/nvapi_min.h"

namespace th::fg {
namespace {

using PFN_QueryInterface = void*(__cdecl*)(nv::NvU32);
using PFN_Initialize = nv::Status(__cdecl*)();
using PFN_SetSleepMode = nv::Status(__cdecl*)(IUnknown*, nv::SetSleepModeParams*);
using PFN_Sleep = nv::Status(__cdecl*)(IUnknown*);
using PFN_GetSleepStatus = nv::Status(__cdecl*)(IUnknown*, nv::GetSleepStatusParams*);
using PFN_SetLatencyMarker = nv::Status(__cdecl*)(IUnknown*, nv::LatencyMarkerParams*);

constexpr uint32_t kArmFrames = 240;  // stable presents before arming (not during start-up)

struct State {
  std::mutex m;
  std::atomic<int> want_mode{0};
  std::atomic<bool> want_markers{true};
  bool loaded = false, available = false;
  PFN_SetSleepMode set_mode = nullptr;
  PFN_Sleep sleep = nullptr;
  PFN_GetSleepStatus status = nullptr;
  PFN_SetLatencyMarker marker = nullptr;
  int applied = -1;
  bool sleeping = false;
  uint32_t stable = 0;
  uint64_t frame_id = 1;
  bool begun = false;
  // the sleep runs on a sleeper thread with a watchdog (vkd3d waits on the driver without a timeout)
  HANDLE req = nullptr, done = nullptr;
  std::atomic<bool> busy{false};
  std::atomic<int64_t> started{0};
  uint32_t overruns = 0;
  std::atomic<uint64_t> wait_us{0};
  double sleep_ms = 0;
  uint32_t sleeps = 0;
  int64_t last_report = 0;
  int last_mode_st = 0;
  ID3D12Device* dev = nullptr;
} R;

bool load() {
  if (R.loaded) return R.available;
  R.loaded = true;
  HMODULE m = LoadLibraryW(L"nvapi64.dll");
  auto qi = m ? reinterpret_cast<PFN_QueryInterface>(GetProcAddress(m, "nvapi_QueryInterface")) : nullptr;
  if (!qi) {
    TH_LOG("fg: reflex: nvapi64.dll not available (Proton: PROTON_ENABLE_NVAPI=1)");
    return false;
  }
  auto init = reinterpret_cast<PFN_Initialize>(qi(nv::kId_Initialize));
  R.set_mode = reinterpret_cast<PFN_SetSleepMode>(qi(nv::kId_D3D_SetSleepMode));
  R.sleep = reinterpret_cast<PFN_Sleep>(qi(nv::kId_D3D_Sleep));
  R.status = reinterpret_cast<PFN_GetSleepStatus>(qi(nv::kId_D3D_GetSleepStatus));
  R.marker = reinterpret_cast<PFN_SetLatencyMarker>(qi(nv::kId_D3D_SetLatencyMarker));
  nv::Status st = init ? init() : -1;
  R.available = st == 0 && R.set_mode && R.sleep;
  TH_LOG("fg: reflex: NvAPI_Initialize -> %d; SetSleepMode %p Sleep %p SetLatencyMarker %p", st, (void*)R.set_mode,
         (void*)R.sleep, (void*)R.marker);
  return R.available;
}

DWORD WINAPI sleeper(void*) {
  for (;;) {
    WaitForSingleObject(R.req, INFINITE);
    R.sleep(R.dev);
    R.busy = false;
    SetEvent(R.done);
  }
}

void mark(nv::LatencyMarker type) {
  if (!R.marker || !R.want_markers) return;
  nv::LatencyMarkerParams p{};
  p.version = nv::kLatencyMarkerVer;
  p.frameID = R.frame_id;
  p.markerType = type;
  R.marker(R.dev, &p);
}

void apply(int mode) {
  nv::SetSleepModeParams p{};
  p.version = nv::kSetSleepModeVer;
  p.bLowLatencyMode = mode >= 1;
  p.bLowLatencyBoost = mode == 2;
  p.minimumIntervalUs = 0;  // the cap is ours (limit()): the driver's interval breaks with several presents per frame
  p.bUseMarkersToOptimize = false;
  nv::Status st = R.set_mode(R.dev, &p);
  R.applied = mode;
  R.sleeping = st == 0 && mode >= 1;
  R.last_mode_st = st;
  R.begun = false;
  nv::GetSleepStatusParams gs{};
  gs.version = nv::kGetSleepStatusVer;
  nv::Status gst = R.status ? R.status(R.dev, &gs) : -1;
  TH_LOG("fg: reflex: SetSleepMode(low_latency=%d boost=%d) on the D3D12 device -> %d; GetSleepStatus -> %d (low_latency=%d)",
         p.bLowLatencyMode, p.bLowLatencyBoost, st, gst, gs.bLowLatencyMode);
}

}  // namespace

void reflex_config(int mode, bool markers) {
  R.want_mode = mode;
  R.want_markers = markers;
}

uint64_t reflex_wait_us() { return R.wait_us.load(); }

void reflex_present_begin(ID3D12Device* dev) {
  std::lock_guard lk(R.m);
  if (R.stable < kArmFrames) {
    ++R.stable;
    return;
  }
  const int want = R.want_mode.load();
  if (want == 0 && R.applied <= 0) return;
  if (!dev || !load()) return;
  R.dev = dev;
  if (want != R.applied) apply(want);
  if (!R.sleeping) return;
  if (!R.begun) {
    mark(nv::SIMULATION_START);
    R.begun = true;
  }
  mark(nv::PRESENT_START);
}

void reflex_present_end(ID3D12Device* dev) {
  std::lock_guard lk(R.m);
  if (!R.sleeping || !dev) return;
  mark(nv::PRESENT_END);
  ++R.frame_id;
  if (!R.req) {
    R.req = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    R.done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE t = CreateThread(nullptr, 0, sleeper, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
  }
  const int64_t t0 = now_us();
  if (R.busy) {  // the previous sleep never returned: go on without sleeping until it does
    mark(nv::SIMULATION_START);
    return;
  }
  R.busy = true;
  R.started = t0;
  ResetEvent(R.done);
  SetEvent(R.req);
  if (WaitForSingleObject(R.done, 500) != WAIT_OBJECT_0) {
    ++R.overruns;
    TH_LOG("fg: reflex: sleep did not return within 500 ms; not sleeping until it does");
  }
  const int64_t t1 = now_us();
  mark(nv::SIMULATION_START);
  R.wait_us += uint64_t(t1 - t0);
  R.sleep_ms += (t1 - t0) / 1000.0;
  R.sleeps++;
  if (t1 - R.last_report >= 2000000) {
    if (R.last_report)
      TH_LOG("perf: reflex sleep avg %.2f ms over %u frames (overruns %u)", R.sleeps ? R.sleep_ms / R.sleeps : 0.0,
             R.sleeps, R.overruns);
    R.last_report = t1;
    R.sleep_ms = 0;
    R.sleeps = 0;
  }
}

std::string reflex_status_json() {
  std::lock_guard lk(R.m);
  char b[200];
  snprintf(b, sizeof b, "{\"mode\":%d,\"applied\":%d,\"sleeping\":%s,\"available\":%s,\"set_mode_status\":%d,\"overruns\":%u}",
           R.want_mode.load(), R.applied, R.sleeping ? "true" : "false", R.available ? "true" : "false", R.last_mode_st,
           R.overruns);
  return b;
}

}  // namespace th::fg
