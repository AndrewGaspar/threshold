#pragma once
// The renderer-side inputs of frame generation that the NGX parameter block doesn't carry
// (docs/fg.md): the main camera (RendererX86's main rend::View, re/stereo.md 2.3) and the HUD-less
// colour (the frame's colour target at renderHUD entry, re/dlss.md 3). Renderer thread only.
#include <d3d12.h>

#include <cstdint>

namespace th::fg {

bool engine_init();  // resolve and signature-check; false (logged) disables both

// DLSS-G camera, in DLSS-G's convention (row vectors, row-major: v' = v M), no TAA jitter.
struct Camera {
  bool ok = false;
  bool has_prev = false;  // the main view had a previous frame (else: camera cut, Reset)
  float view_to_clip[16], clip_to_view[16], clip_to_prev[16], prev_to_clip[16];
  float pos[3], up[3], right[3], fwd[3];
  float near_z = 0, far_z = 0, fov = 0, aspect = 0;
  uint32_t counter = 0;  // renderer frame counter when read
};
// Reads the main view now (call inside the world render: the DLSS evaluate tap).
bool camera_read(Camera& c);
void camera_log(const Camera& c);  // occasional diagnostics

// HUD-less colour: a copy of the colour target taken at renderHUD entry (RendererX86 vtable slot 17,
// patched in the vtable: the slot is only ever called through it, and a vtable patch composes with
// an inline detour of the function body, e.g. stereo's). Armed per frame by the DLSS evaluate tap.
bool hudless_install();
void hudless_arm(bool on);  // copy at the next renderHUD
struct EngTex {
  ID3D12Resource* r = nullptr;
  D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_COMMON;  // the engine's tracked state (restore it)
  bool tracked = false;
};
// The copy taken since the last call (consumed), if any.
bool hudless_take(EngTex& t);
uint64_t hudless_copies();

}  // namespace th::fg
