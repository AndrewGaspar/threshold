#include "features/fg/metrics.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "core/log.h"
#include "core/quiver.h"

namespace th::fg {
namespace {

struct M {
  bool on = false;
  ::quiver_metric frame = QUIVER_NO_METRIC, state = QUIVER_NO_METRIC, mult = QUIVER_NO_METRIC,
                  real = QUIVER_NO_METRIC, gen = QUIVER_NO_METRIC, dropped = QUIVER_NO_METRIC,
                  reflex = QUIVER_NO_METRIC;
  // present thread
  std::atomic<uint64_t> n_real{0}, n_gen{0};
  std::atomic<uint32_t> swapchain{1};
  uint64_t seq = 0;
  int64_t qpc_freq = 10000000;
  // renderer thread
  int64_t last = 0;
  uint64_t last_real = 0, last_gen = 0;
  // what was last published (fg status "hud", and a log line when the state or multiplier changes)
  std::mutex mu;
  std::string p_state;
  unsigned p_mult = 0;
  double p_real = 0, p_gen = 0;
  int p_reflex = 0;
  uint64_t p_dropped = 0, p_ticks = 0;
} g;

int64_t qpc(int64_t us) { return us * g.qpc_freq / 1000000; }

}  // namespace

void metrics_init() {
  quiver_mod* q = quiver_sdk();
  if (!q) return;
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  g.qpc_freq = f.QuadPart;
  // The same descriptors as dist/quiver-options.toml's [metric.*] (keep them together). The HUD's
  // mod strip shows the basic gauges, states and texts in `order`.
  g.state = metric_declare("fg.state", QUIVER_STATE,
      "\"label\":\"Frame Generation\",\"short\":\"FG\",\"help\":\"Threshold's DLSS Frame Generation: on, on with count = auto, "
      "no input (menus, videos, the game's DLSS off), off, or failed.\",\"values\":["
      "{\"value\":\"off\",\"label\":\"off\",\"level\":\"dim\"},{\"value\":\"on\",\"label\":\"on\"},"
      "{\"value\":\"auto\",\"label\":\"auto\"},{\"value\":\"idle\",\"label\":\"no input\",\"level\":\"dim\"},"
      "{\"value\":\"failed\",\"label\":\"failed\",\"level\":\"bad\"}],"
      "\"display\":\"badge\",\"roles\":[\"fg.state\"],\"group\":\"Frame generation\",\"order\":1,\"level\":\"basic\"");
  g.mult = metric_declare("fg.multiplier", QUIVER_GAUGE,
      "\"label\":\"FG multiplier\",\"help\":\"Presents per rendered frame now ([fg] count, or what count = auto chose); "
      "1 while no frames are generated.\",\"unit\":\"x\",\"format\":{\"precision\":0},\"sample\":2,"
      "\"roles\":[\"fg.ratio\"],\"group\":\"Frame generation\",\"order\":2,\"level\":\"basic\"");
  g.real = metric_declare("fps.real", QUIVER_GAUGE,
      "\"label\":\"Rendered FPS\",\"short\":\"BASE\",\"help\":\"Frames the game rendered per second (each shown once, "
      "after its generated frames).\",\"unit\":\"fps\",\"format\":{\"precision\":0},\"better\":\"higher\",\"sample\":2,"
      "\"roles\":[\"fps.base\"],\"group\":\"Frame generation\",\"order\":3,\"level\":\"basic\"");
  g.gen = metric_declare("fps.generated", QUIVER_GAUGE,
      "\"label\":\"Generated FPS\",\"short\":\"GEN\",\"help\":\"DLSS-G frames presented per second.\",\"unit\":\"fps\","
      "\"format\":{\"precision\":0},\"sample\":2,\"roles\":[\"fps.generated\"],\"group\":\"Frame generation\",\"order\":4,\"level\":\"basic\"");
  g.dropped = metric_declare("fg.dropped", QUIVER_COUNTER,
      "\"label\":\"Generated frames dropped\",\"help\":\"Generated frames not shown: DLSS-G wasn't done within 100 ms, "
      "or the next frame was already waiting.\",\"unit\":\"count\",\"better\":\"lower\",\"group\":\"Frame generation\",\"order\":6,\"level\":\"advanced\"");
  g.reflex = metric_declare("reflex.mode", QUIVER_STATE,
      "\"label\":\"Reflex (injected)\",\"short\":\"RFX\",\"values\":[{\"value\":\"off\",\"label\":\"off\",\"level\":\"dim\"},"
      "{\"value\":\"on\",\"label\":\"on\"},{\"value\":\"boost\",\"label\":\"boost\"}],\"display\":\"badge\","
      "\"group\":\"Latency\",\"order\":7,\"level\":\"advanced\"");
  // Per-present records (SIGHT-HUD 3.10, 3.18): with quiver_mod.h 3.1 the SDK maps a frame ring once
  // Quiver grants one, and quiver_perf_wanted() turns 1; records then count as the mod's frames.
  g.frame = metric_declare("frame", QUIVER_FRAME,
      "\"label\":\"FG presents\",\"help\":\"One record per Present Threshold makes: generated, real after generated, "
      "or real alone.\",\"clock\":\"qpc\",\"key\":\"present_seq\",\"fields\":[\"game_present\",\"present_call\","
      "\"present_return\"],\"sample\":\"per-frame\",\"group\":\"Frame generation\",\"level\":\"advanced\"");
  g.on = true;
}

void metrics_present(char kind, int64_t arrive_us, int64_t call_us, int64_t ret_us) {
  if (!g.on) return;
  (kind == 'G' ? g.n_gen : g.n_real).fetch_add(1, std::memory_order_relaxed);
  quiver_mod* q = quiver_sdk();
  if (!quiver_perf_wanted(q)) return;
  quiver_frame fr{};
  fr.key = ++g.seq;
  fr.swapchain = g.swapchain.load(std::memory_order_relaxed);
  fr.flags = kind == 'G' ? QUIVER_FRAME_GENERATED : kind == 'R' ? QUIVER_FRAME_REAL : QUIVER_FRAME_REAL | QUIVER_FRAME_NOFG;
  fr.t = qpc(call_us);
  fr.nfields = 3;
  fr.f[0] = qpc(arrive_us), fr.f[1] = qpc(call_us), fr.f[2] = qpc(ret_us);
  quiver_frame_submit(q, g.frame, &fr);
}

void metrics_dropped(unsigned n) {
  if (!g.on || !n) return;
  quiver_count(quiver_sdk(), g.dropped, n);
  std::lock_guard lk(g.mu);
  g.p_dropped += n;
}

void metrics_resize() {
  if (!g.on) return;
  g.swapchain.fetch_add(1, std::memory_order_relaxed);
  g.seq = 0;  // the present thread is drained (before_resize)
}

void metrics_tick(int64_t now, const char* state, unsigned mult, int (*reflex)()) {
  if (!g.on) return;
  if (!g.last) {
    g.last = now;
    return;
  }
  const int64_t dt = now - g.last;
  if (dt < 500000) return;
  const uint64_t r = g.n_real.load(std::memory_order_relaxed), n = g.n_gen.load(std::memory_order_relaxed);
  quiver_mod* q = quiver_sdk();
  const double real_fps = double(r - g.last_real) * 1e6 / double(dt), gen_fps = double(n - g.last_gen) * 1e6 / double(dt);
  const int rm = reflex ? reflex() : 0;
  ::quiver_state(q, g.state, state);
  ::quiver_gauge(q, g.mult, mult);
  ::quiver_gauge(q, g.real, real_fps);
  ::quiver_gauge(q, g.gen, gen_fps);
  ::quiver_state(q, g.reflex, rm == 2 ? "boost" : rm == 1 ? "on" : "off");
  g.last = now, g.last_real = r, g.last_gen = n;
  std::lock_guard lk(g.mu);
  if (g.p_state != state || g.p_mult != mult)
    TH_LOG("fg: HUD: %s, %ux (%.0f rendered + %.0f generated fps)", state, mult, real_fps, gen_fps);
  g.p_state = state, g.p_mult = mult, g.p_real = real_fps, g.p_gen = gen_fps, g.p_reflex = rm, g.p_ticks++;
}

std::string metrics_json() {
  std::lock_guard lk(g.mu);
  if (!g.on) return "null";
  char b[256];
  snprintf(b, sizeof b,
           "{\"state\":\"%s\",\"multiplier\":%u,\"real_fps\":%.1f,\"generated_fps\":%.1f,\"dropped\":%llu,"
           "\"reflex\":%d,\"published\":%llu}",
           g.p_state.c_str(), g.p_mult, g.p_real, g.p_gen, (unsigned long long)g.p_dropped, g.p_reflex,
           (unsigned long long)g.p_ticks);
  return b;
}

}  // namespace th::fg
