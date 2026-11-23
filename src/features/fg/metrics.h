#pragma once
// Frame generation's metrics for Quiver's Sight HUD and panel (docs/fg.md "HUD metrics"): the FG
// state and the multiplier in effect, rendered and generated frames per second, dropped generated
// frames, injected Reflex, and (once Quiver offers mod rings) a record per present. Only with
// [fg] enabled: without it nothing is declared or sent.
#include <cstdint>

namespace th::fg {

// fg_start, FG enabled: declare the metrics.
void metrics_init();
// Present thread, after each of our Presents: kind 'G' generated, 'R' real after generated frames,
// 'N' real alone; arrive = the game's Present, call/ret = ours (now_us()).
void metrics_present(char kind, int64_t arrive_us, int64_t call_us, int64_t ret_us);
void metrics_dropped(unsigned n);  // generated frames not shown: not done in time, or superseded
void metrics_resize();             // the swapchain's buffers go: a new present sequence
// Renderer thread, at the end of each game Present FG handled: publishes every 500 ms.
// state: "off" | "on" | "auto" | "idle" | "failed"; mult: presents per rendered frame now (1 = none);
// reflex() (called only when publishing): 0 off, 1 on, 2 boost (applied).
void metrics_tick(int64_t now, const char* state, unsigned mult, int (*reflex)());

}  // namespace th::fg
