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

}  // namespace th
