#pragma once
// Present trace for frame-pacing analysis (Pharos's fg-trace method; docs/fg.md "Measuring").
// `quiver lab call <i> fg trace [presents] [shots] [y] [h]`: the next `presents` presents get timestamps
// (the game's Present arrival, our Present call and return, QPC us) and the first `shots` of them a
// copy of a horizontal band of exactly what was presented, in present order. Written to
// %QUIVER_LAB_RUN_DIR%\fgtrace\{presents.csv,bands.bin}; tools/fg/fg-pacing.py analyses them.
#include <d3d12.h>
#include <dxgi.h>

#include <cstdint>
#include <string>

namespace th::fg {

// Lab verb thread: arms a trace. Returns a message for the caller.
std::string trace_arm(unsigned presents, unsigned shots, int y = -1, int h = 0);  // y/h: the band (px)
std::string trace_status_json();

// Present thread, around one Present of `src` (a texture of ours in COPY_SOURCE state, w x h, the
// swapchain's format). trace_copy records the band copy on `cl` (before it is submitted) and returns
// an event index, or -1 when no trace is running.
// kind: 'G' generated, 'R' real (after generated ones), 'N' real without generated frames.
int trace_copy(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, ID3D12Resource* src, DXGI_FORMAT fmt, char kind,
               uint64_t frame, int64_t arrive_us, int64_t step_us, int64_t interval_us, int gi, int gn);
void trace_call(int idx);                       // right before the Present call
void trace_ret(int idx, ID3D12Fence* f, uint64_t v);  // after it returned; v: fence value of the band copy

}  // namespace th::fg
