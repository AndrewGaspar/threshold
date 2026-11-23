#pragma once

namespace th {

// DLSS Frame Generation / Multi Frame Generation for Control's native D3D12 renderer (docs/fg.md).
//
// - Inputs: right after the game's DLSS Super Resolution evaluation (NVSDK_NGX_D3D12_EvaluateFeature_C,
//   exported by d3d_rmdwin10_f.dll) we copy its depth and motion vectors, in queue order, on the
//   game's own command list, and keep its jitter and MV scale.
// - Proxy back buffers: the engine tracks the back-buffer index itself (re/dlss.md section 2), so it
//   gets two textures of ours from GetBuffer and never sees the real swapchain buffers. That lets us
//   present any number of frames per game frame.
// - At the game's Present (renderer thread): the real frame is copied aside and DLSS-G evaluates
//   [fg] count - 1 generated frames through the game's own NGX instance on the game's queue.
// - A present thread then presents generated 1..N and the real frame, paced at the real frame
//   interval / count.
//
// Needs the game's DLSS on (any quality, DLAA included): the inputs come from its evaluation. With
// DLSS off, or before the first evaluation, frames are presented as they come.
// [fg] enabled is read at start (the proxies must exist from the swapchain's creation); count, show
// and pacing are live.
void fg_register();
void fg_start();
bool fg_enabled();  // [fg] enabled at start (after fg_start)

// Stereo 3D with frame generation (docs/stereo.md "Stereo with frame generation", docs/fg.md
// "Stereo"). Stereo renders both eyes in one engine frame and presents them side by side; FG then
// keeps one input set (depth, MVs, jitter, camera) and one DLSS-G feature per eye, and evaluates each
// eye in its own sub-rectangle of the shared side-by-side textures. Stereo registers its side at
// start and reports each frame's layout; without it (or in a frame it doesn't report) FG works on
// the whole frame as before.
struct FgStereoLink {
  // Renderer thread, inside a world render (the DLSS evaluate tap): 0 / 1 = the left / right eye of a
  // side-by-side pair, -1 = not a pair (one render this frame).
  int (*eye)();
  // renderHUD entry (FG's HUD-less tap): true when stereo is about to compose a pair and will hand the
  // side-by-side texture over with fg_stereo_hudless instead.
  bool (*hud_pending)();
};
void fg_stereo_link(const FgStereoLink& link);
// Stereo's renderHUD, after both eyes are in the side-by-side texture (d3d::NativeTexture*) and
// before the HUD is drawn into it: the HUD-less colour of the whole 2W frame.
void fg_stereo_hudless(void* renderer, void* sbs_nt);
// Stereo's `end`, for every frame it blits with a packed output: pack 1 = a 2W back buffer (each eye
// 1:1), 2 = W (eyes squeezed 2:1); w = one eye's width (the engine's output size); shift = each eye
// image's horizontal shift in the frame (convergence, eye pixels); pair = both eyes rendered (false:
// one image in both halves).
void fg_stereo_frame(int pack, int w, int shift_left, int shift_right, bool pair);

}  // namespace th
