#pragma once
#include <cstdint>
#include <string>

struct IDXGISwapChain;
struct ID3D12GraphicsCommandList;

namespace th::stereo {

// GPU side of stereo: two eye textures and copies between them and the engine's textures
// (d3d::NativeTexture*, whose resource state the engine tracks), on our own command lists
// submitted to the game's direct queue. Everything is on the renderer thread.
//
// Ordering: a copy that reads what the engine just rendered must reach the queue after the engine's
// commands. Mid-frame the engine's current list is first submitted (gpu_kick: the renderer's own
// d3d::DeviceUtilDX12::submitGraphicsCommandList, which its passes call too); at Present the engine
// has already submitted everything.
bool gpu_start();                  // resolve the d3d exports (signature-checked)
bool gpu_attach(IDXGISwapChain* sc);  // device + queue (first Present)
bool gpu_ready();
void gpu_kick();
void* gpu_back_buffer();  // d3d::DeviceUtil::getAutoBackBuffer(): NativeTexture*
void* gpu_native(void* texture2d);  // NativeTextureContainer::getNativeTexture

// Copy a region of an engine texture into eye texture `eye` (0 left, 1 right), sized w x h.
bool gpu_stash(int eye, void* nt, int x, int y, int w, int h);
// Copy eye texture `eye` (all of it) into an engine texture at (x, y), shifted horizontally by
// `shift` pixels (convergence as an image shift; uncovered columns black).
bool gpu_place(int eye, void* nt, int x, int y, int shift = 0);
// Copy a region between two engine textures.
bool gpu_copy(void* src_nt, int sx, int sy, int w, int h, void* dst_nt, int dx, int dy);
// Copy [0, w) of an engine texture to [w, 2w) (through eye texture 0): one image in both halves.
// w, h <= 0: half the resource's width, its height.
bool gpu_duplicate(void* nt, int w, int h);
// Read an engine texture back and write it as a 24-bit BMP (float formats clamped, gamma 2.2), or with
// a path ending in ".raw" its texels as they are (header "THRW", w, h, pitch, DXGI format).
bool gpu_capture(void* nt, const std::string& path);
uint32_t gpu_eye_width();
// One of our command lists for other recording (NGX feature creation); submit returns its fence value.
void* gpu_list_begin();  // ID3D12GraphicsCommandList*
uint64_t gpu_list_submit();
bool gpu_fence_done(uint64_t v);
std::string gpu_status();

// Draw duplication (the HUD into both halves with one paint): while armed on this thread, every draw
// recorded on any D3D12 command list is issued a second time with its viewport and scissor moved
// right by `dx` pixels. gpu_dup_install hooks the (shared) command-list vtable once.
bool gpu_dup_install();
void gpu_dup_arm(int dx);  // 0 disarms
uint64_t gpu_dup_count();

// Per-draw hook for the UI layer (ui.cpp): called on the armed thread for every draw while the
// duplication is armed, or while observing (gpu_draw_observe, no duplication). Phase 0 before the
// original draw: `shift` (pixels, per eye: [0] the original draw, [1] the duplicate) may be set; a
// shifted draw has its viewport moved and its scissor kept inside its eye's half. Phase 1 right
// before the duplicate, phase 2 right after it (both only while duplicating). `cl` is the
// engine's command list the draw is recorded on.
struct DupDraw {
  unsigned count = 0, start = 0;  // indices (or vertices)
  int base = 0;
  float vp_x = 0, vp_y = 0, vp_w = 0, vp_h = 0;  // the viewport as the engine set it
  int dx = 0;                                    // the duplicate's offset (0: observing only)
  float shift[2] = {0, 0};
};
using DupCb = void (*)(ID3D12GraphicsCommandList* cl, int phase, DupDraw& d);
void gpu_dup_callback(DupCb cb);
void gpu_draw_observe(bool on);  // this thread's draws reach the callback without duplication
// A copy recorded on `cl` (an engine list, mid-pass) from one engine texture into another of the
// same size, with barriers from and back to each one's tracked state.
bool gpu_copy_on(ID3D12GraphicsCommandList* cl, void* src_nt, void* dst_nt);
// Texel probes: one texel of an engine texture copied on `cl` into readback slot (ring, slot), read
// on the CPU a few frames later (ring of kProbeRing frames). R16_FLOAT / R32_FLOAT / D32 formats.
constexpr int kProbeSlots = 64, kProbeRing = 4;
bool gpu_probe(ID3D12GraphicsCommandList* cl, void* nt, int ring, int slot, int x, int y);
bool gpu_probe_value(int ring, int slot, float* v);  // the slot's last copied value (as float)

// The engine texture's size and tracked state, for logs ("1920x1080 state 0x4 res 0x...").
std::string gpu_describe(void* nt);

}  // namespace th::stereo
