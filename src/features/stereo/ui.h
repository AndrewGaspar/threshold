#pragma once
#include <string>
#include <vector>

#include "core/module.h"

namespace th::stereo {

// The UI layer in side-by-side frames (docs/stereo.md "World markers" and "Pause background").
// The HUD is Coherent GT: renoir records its paint and ui_rmdwin10_f.dll's backend
// (ui::UIRendererBackend) executes it on the renderer thread inside renderHUD, where gpu.cpp
// duplicates every draw into the right half. Coherent composites each animated element (a layer)
// into the colour target with one quad whose "Transform" constant carries the element's screen
// position. ui.cpp hooks the backend's per-draw state to tell draws apart:
//  - world markers (interaction/collectible prompts, mission markers: everything the HUD projects
//    with exe+0x5edb50): their quads are moved per eye to the marker's depth, i.e. the anchor's
//    distance, or just in front of the first surface on the camera ray to it if that is nearer;
//  - the pause menu's background (the engine's copy of the presented frame, RendererX86 slot 22):
//    captured per eye, and the right half's background draw samples the right eye's capture.

// Engine entry points stereo.cpp already resolved and checked.
struct UiEngine {
  void(__cdecl* copy_region)(void* dst, int, int, const int*, void* src, int, int, const int*, const int*) = nullptr;
  void(__fastcall* tex2d_init)(void* self, int w, int h, int mips, int fmt, int usage, int a6, const void* data,
                               uint64_t size, int misc, int a10, float a11, int a12, const float* clear,
                               int tile) = nullptr;
  void*(__fastcall* rt_alloc)(void* mgr, const void* init, const char* name) = nullptr;
  void(__fastcall* rt_free)(void* mgr, void* tex) = nullptr;
  void* (*native)(void* tex2d) = nullptr;   // Texture2D* -> NativeTexture*
  void* (*back_buffer)() = nullptr;          // NativeTexture*
};
// Hooks the UI backend, the HUD projection and the pause capture (signature-checked; each part
// that does not match stays off). Only called with a packed output.
bool ui_start(const Image& renderer, const UiEngine& eng);

// Per side-by-side frame, from stereo.cpp.
struct UiFrame {
  int w = 0, h = 0;          // one eye (output size)
  int pack = 0;              // 1 full/sbs (2W back buffer), 2 hsbs
  double sep_m = 0;          // per eye
  double conv_m = 0;         // 0 = parallel
  double m00 = 0;            // the game's projection x scale
  int sign = 1;              // -1 with swap_eyes
  bool proj_conv = false;    // convergence in the projection (else the image shift below)
  int shift[2] = {0, 0};     // image-shift convergence per eye (px)
  double cam_pos[3] = {}, cam_fwd[3] = {};  // the centre camera
  int render_w = 0, render_h = 0;
  void* sbs_native = nullptr;  // the side-by-side texture (NativeTexture*, 2W x H): eyes + HUD, pre-gamma
};
void ui_frame(const UiFrame& f);  // after the pair's world renders, before renderHUD
// renderHUD of a frame without a pair: true if the UI wants it side by side (the pause background
// per eye); the caller then runs renderHUD over the side-by-side texture with the duplication armed.
bool ui_pause_sbs();
// Around renderHUD's original call. sbs: the duplication is armed (eye width w).
void ui_hud_begin(bool sbs, int w);
void ui_hud_end();
// A stereo pair was rendered (the pause background ends) / the level changes.
void ui_pair_rendered();
// [stereo] ui_markers / ui_pause (live), from stereo.cpp's config listener.
void ui_option(const std::string& id, const std::string& v);
std::string ui_status();
std::string ui_verb(const std::vector<std::string>& a, bool& ok);

}  // namespace th::stereo
