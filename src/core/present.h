#pragma once
#include <cstdint>
#include <functional>

struct IDXGISwapChain;
struct IDXGIFactory;
struct _GUID;

namespace th {

// DXGI factory/swapchain hooks. Listeners run on the game's presenting thread, just before the
// real Present (and after ResizeBuffers).
void present_hook_factory(IDXGIFactory* factory);

using PresentFn = std::function<void(IDXGISwapChain* sc, uint64_t frame)>;
void present_on(PresentFn fn);
using ResizeFn = std::function<void(IDXGISwapChain* sc, unsigned w, unsigned h)>;
void resize_on(ResizeFn fn);

uint64_t present_frame();

// Present filters run after the listeners; if any returns false the real Present is skipped
// (the call returns S_OK and the back buffer is not flipped). Used by stereo to drop the first
// of two engine frames per game update.
using PresentFilterFn = std::function<bool(IDXGISwapChain* sc, uint64_t frame)>;
void present_filter(PresentFilterFn fn);

// Swapchain width multiplier (1 or 2), applied when the game creates or resizes its swapchain:
// with 2 the real buffers are twice as wide as the game asked for (full side-by-side output),
// while GetDesc/GetDesc1 keep reporting the game's own width. Set it before the swapchain exists.
void present_set_width_scale(unsigned scale);
unsigned present_width_scale(IDXGISwapChain* sc);  // the scale this swapchain was created with
unsigned present_width_scale_pending();             // the scale new swapchains will get

// The object the swapchain was created with: for Direct3D 12 that is the game's
// ID3D12CommandQueue (not AddRef'd; lives as long as the swapchain). nullptr if unknown.
void* present_queue(IDXGISwapChain* sc);

// Frame generation (src/features/fg/) takes the game's presents over:
//  - present_takeover: called for every game Present after the listeners and filters; returning true
//    means it handled the frame and the real Present is not called (returns S_OK to the game).
//  - present_real: the original Present, bypassing every hook (for presents the mod makes itself, from
//    any thread).
//  - present_getbuffer: when set and returning true, supplies IDXGISwapChain::GetBuffer's result
//    (proxy back buffers the game renders into).
//  - present_set_buffer_count: the real buffer count for new swapchains and resizes (0 = the game's).
//  - present_before_resize: runs before ResizeBuffers (release anything holding the old buffers).
using PresentTakeoverFn = std::function<bool(IDXGISwapChain* sc, unsigned sync, unsigned flags)>;
void present_takeover(PresentTakeoverFn fn);
long present_real(IDXGISwapChain* sc, unsigned sync, unsigned flags);
using GetBufferFn = std::function<bool(IDXGISwapChain* sc, unsigned index, const ::_GUID& riid, void** out, long* hr)>;
void present_getbuffer(GetBufferFn fn);
void present_set_buffer_count(unsigned n);
// The real swapchain buffer (IDXGISwapChain::GetBuffer, bypassing present_getbuffer).
long present_real_buffer(IDXGISwapChain* sc, unsigned index, const ::_GUID& riid, void** out);
void present_before_resize(std::function<void(IDXGISwapChain* sc)> fn);

}  // namespace th
