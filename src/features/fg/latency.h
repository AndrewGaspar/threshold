#pragma once
// Input-to-photon(ish) latency logging for frame generation (docs/fg.md "Latency"), after Pharos's
// src/core/latency.cpp: per rendered frame the camera basis and CPU time at the game's DLSS evaluation,
// GPU timestamps right after that evaluation, when the queue reached our Present-time work (the
// frame's own GPU work done) and after DLSS-G, and the present thread's Present calls for that frame.
// `quiver lab call <i> fg latency start|stop` writes %QUIVER_LAB_RUN_DIR%\latency.csv (times in
// CLOCK_MONOTONIC_RAW ns = QPC under Wine, comparable with `quiver lab mouse --log`);
// tools/fg/latency.py matches injected mouse-look to the first frame whose camera turned.
#include <d3d12.h>

#include <cstdint>
#include <string>

namespace th::fg {

struct Camera;

std::string lat_start(ID3D12Device* dev, ID3D12CommandQueue* q);
std::string lat_stop();
bool lat_on();

// Renderer thread, in the DLSS evaluate tap: `cl` is the game's list, right after its DLSS SR.
void lat_eval(ID3D12GraphicsCommandList* cl, const Camera& cam, uint64_t frame);
// Renderer thread, at the game's Present, on our evaluation list: before our work (the frame's own
// GPU work is done when it runs) and after DLSS-G (resolves this frame's queries).
void lat_begin(ID3D12GraphicsCommandList* cl, uint64_t frame);
void lat_end(ID3D12GraphicsCommandList* cl, uint64_t frame, unsigned n_gen, int64_t arrive_us);
// Present thread: a Present call of this frame's job ('G' generated, 'R'/'N' real).
void lat_present(uint64_t frame, char kind);
// Renderer thread: rows whose GPU work finished (fence value reached) are completed.
void lat_poll(uint64_t frame_done_upto);

}  // namespace th::fg
