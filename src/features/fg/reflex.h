#pragma once
// NVIDIA Reflex injected for frame generation (Control has none; docs/fg.md "Reflex"), after Pharos's
// src/reflex/: NvAPI_D3D_SetSleepMode / NvAPI_D3D_Sleep / NvAPI_D3D_SetLatencyMarker on the game's
// D3D12 device, through dxvk-nvapi -> vkd3d-proton's ID3DLowLatencyDevice -> VK_NV_low_latency2.
// The sleep runs on the renderer thread at the end of the game's Present (after our limiter), so the
// next frame starts as late as the GPU allows. The frame cap stays ours (no driver minimum interval).
#include <cstdint>
#include <string>

struct ID3D12Device;

namespace th::fg {

// [fg] reflex: 0 off, 1 on, 2 boost; markers: send SIMULATION_START / PRESENT_START / PRESENT_END.
void reflex_config(int mode, bool markers);
// Renderer thread, at the game's Present: before our work / at the very end (sleep).
void reflex_present_begin(ID3D12Device* dev);
void reflex_present_end(ID3D12Device* dev);
std::string reflex_status_json();
int reflex_applied();  // the sleep mode in force: 0 off, 1 on, 2 boost
uint64_t reflex_wait_us();  // total time slept in NvAPI_D3D_Sleep

}  // namespace th::fg
