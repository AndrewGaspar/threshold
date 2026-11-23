#pragma once
// HDR for frame generation (docs/fg.md "HDR"): Control's HDR swapchain is FP16 scRGB (linear BT.709,
// 1.0 = 80 nits); DLSS-G interpolates HDR10 (R10G10B10A2, PQ, BT.2020). As Pharos does, the real frame
// (and the HUD-less colour) are encoded to PQ for DLSS-G and the generated frames decoded back to scRGB
// for presenting. Compute shaders compiled at start-up with the game's own d3dcompiler_47.dll.
#include <d3d12.h>

namespace th::fg {

// Compiles the shaders and creates the root signature, PSOs and a shader-visible descriptor heap.
// Logs and returns false on failure (frame generation then stays off in HDR).
bool hdr_init(ID3D12Device* dev);
bool hdr_ready();

// Records one conversion on `cl`: src (SRV state: NON_PIXEL_SHADER_RESOURCE) -> dst (UNORDERED_ACCESS),
// same size. encode: scRGB FP16 -> PQ BT.2020; else PQ -> scRGB. `slot` (0..kHdrSlots-1) picks the
// descriptor pair; one slot per (texture pair) so descriptors in flight are never rewritten with
// different views.
constexpr unsigned kHdrSlots = 32;
void hdr_convert(ID3D12GraphicsCommandList* cl, ID3D12Resource* src, ID3D12Resource* dst, bool encode,
                 unsigned slot);

}  // namespace th::fg
