#pragma once
// The handful of NVAPI declarations Reflex needs, transcribed from the NVAPI SDK headers
// (third_party/nvapi-*, MIT; nvapi.h itself does not compile with mingw because of its SAL
// annotations). Layouts are asserted below.
#include <cstddef>
#include <cstdint>

namespace th::nv {

using NvU8 = uint8_t;
using NvU32 = uint32_t;
using NvU64 = uint64_t;
using NvBool = NvU8;
using Status = int;  // NvAPI_Status; 0 = NVAPI_OK

constexpr NvU32 make_version(size_t size, NvU32 ver) { return NvU32(size) | (ver << 16); }

// nvapi_interface.h
constexpr NvU32 kId_Initialize = 0x0150e828;
constexpr NvU32 kId_D3D_SetSleepMode = 0xac1ca9e0;
constexpr NvU32 kId_D3D_Sleep = 0x852cd1d2;
constexpr NvU32 kId_D3D_SetLatencyMarker = 0xd9984c05;
constexpr NvU32 kId_D3D_GetSleepStatus = 0xaef96ca1;

#pragma pack(push, 8)
struct SetSleepModeParams {  // NV_SET_SLEEP_MODE_PARAMS_V1
  NvU32 version;
  NvBool bLowLatencyMode;
  NvBool bLowLatencyBoost;
  NvU32 minimumIntervalUs;
  NvBool bUseMarkersToOptimize;
  NvBool bUseMinQueueTime;
  NvU8 rsvd[30];
};
static_assert(sizeof(SetSleepModeParams) == 44 && offsetof(SetSleepModeParams, minimumIntervalUs) == 8);

struct GetSleepStatusParams {  // NV_GET_SLEEP_STATUS_PARAMS_V1
  NvU32 version;
  NvBool bLowLatencyMode;
  NvBool bFsVrr;
  NvBool bCplVsyncOn;
  NvU32 sleepIntervalUs;
  NvBool bUseGameSleep;
  NvBool bFullscreenIFlip;
  NvU8 fgMultiplier;
  NvBool bDfgControl;
  NvU32 dfgFrameTimeTargetUs;
  NvU8 rsvd[114];
};
static_assert(sizeof(GetSleepStatusParams) == 136 && offsetof(GetSleepStatusParams, dfgFrameTimeTargetUs) == 16);

enum LatencyMarker : int {  // NV_LATENCY_MARKER_TYPE
  SIMULATION_START = 0,
  SIMULATION_END = 1,
  RENDERSUBMIT_START = 2,
  RENDERSUBMIT_END = 3,
  PRESENT_START = 4,
  PRESENT_END = 5,
  INPUT_SAMPLE = 6,
};

struct LatencyMarkerParams {  // NV_LATENCY_MARKER_PARAMS_V1
  NvU32 version;
  NvU64 frameID;
  LatencyMarker markerType;
  NvU64 rsvd0;
  NvU8 rsvd[56];
};
static_assert(sizeof(LatencyMarkerParams) == 88 && offsetof(LatencyMarkerParams, markerType) == 16);
#pragma pack(pop)

constexpr NvU32 kSetSleepModeVer = make_version(sizeof(SetSleepModeParams), 1);
constexpr NvU32 kGetSleepStatusVer = make_version(sizeof(GetSleepStatusParams), 1);
constexpr NvU32 kLatencyMarkerVer = make_version(sizeof(LatencyMarkerParams), 1);

}  // namespace th::nv
