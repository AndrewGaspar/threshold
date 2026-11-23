#define QUIVER_MOD_VERSION TH_VERSION
#define QUIVER_MOD_IMPLEMENTATION
#include "quiver_mod.h"

#include "core/quiver.h"

#include <mutex>
#include <thread>
#include <vector>
#include <windows.h>
#include <string>

#include "core/config.h"
#include "core/log.h"

namespace th {
namespace {
quiver_mod* g_q = nullptr;
std::string g_layout;  // last stereo layout, for before quiver_init
std::mutex g_metrics_mu;
std::vector<std::pair<std::string, ::quiver_metric>> g_metrics;  // declared, for the verdict log

// Quiver answers declarations after connecting (metrics_ok): log each verdict once, so a refused
// metric (a kind this Quiver doesn't take, a bad descriptor) shows in threshold.log.
void log_verdicts() {
  for (int i = 0; i < 20; i++) {
    bool pending = false;
    {
      std::lock_guard lk(g_metrics_mu);
      for (auto& [n, h] : g_metrics) pending |= quiver_metric_status(g_q, h) == 0;
    }
    if (!pending) break;
    Sleep(250);
  }
  std::lock_guard lk(g_metrics_mu);
  std::string ok, refused, pending;
  for (auto& [n, h] : g_metrics) {
    int st = quiver_metric_status(g_q, h);
    std::string& to = st > 0 ? ok : st < 0 ? refused : pending;
    to += (to.empty() ? "" : " ") + n;
  }
  if (!g_metrics.empty())
    TH_LOG("quiver SDK: metrics accepted: %s; refused: %s; unanswered: %s", ok.empty() ? "(none)" : ok.c_str(),
           refused.empty() ? "(none)" : refused.c_str(), pending.empty() ? "(none)" : pending.c_str());
}

int on_values(void*, const quiver_value* v, int n) {
  for (int i = 0; i < n; i++) cfg_set(v[i].id, v[i].text ? v[i].text : "", "quiver");
  return 0;
}
}  // namespace

void quiver_init() {
  g_q = quiver_open("threshold", nullptr, 0);
  quiver_on_values_cb(g_q, on_values, nullptr);
  TH_LOG("quiver SDK: %s", quiver_connected(g_q) ? "connected" : "opened (connects in background)");
  if (g_q && !g_layout.empty()) quiver_set_stereo_layout(g_q, g_layout.c_str());
  // Say whether the SDK ever connects (it does so in the background): without this the log can't
  // tell "not offered by Quiver" from "connected" (2026-11-19: Sight drew mono and nothing said why).
  if (g_q)
    std::thread([] {
      for (int i = 0; i < 120; i++) {
        if (quiver_connected(g_q)) {
          TH_LOG("quiver SDK: connected (%.1f s after open); stereo layout %s", i * 0.5,
                 g_layout.empty() ? "(none)" : g_layout.c_str());
          log_verdicts();
          return;
        }
        Sleep(500);
      }
      TH_LOG("quiver SDK: not connected 60 s after open (no channel offered, or quiverd unreachable)");
    }).detach();
}

void quiver_live(const char* id) { quiver_handle_option(g_q, id); }

void quiver_stereo_layout(const char* layout) {
  if (g_layout == layout) return;
  g_layout = layout;
  if (g_q) quiver_set_stereo_layout(g_q, layout);
  TH_LOG("quiver SDK: stereo layout %s", layout);
}

void quiver_report(const std::string& id, const std::string& text, bool persist) {
  quiver_changed(g_q, id.c_str(), text.c_str(), persist ? 1 : 0);
}

::quiver_metric metric_declare(const char* name, quiver_kind kind, const char* json_members) {
  std::string json = std::string("{") + (json_members ? json_members : "") + "}";
  ::quiver_metric h = quiver_metric_declare(g_q, name, kind, json.c_str());
  if (h != QUIVER_NO_METRIC) {
    std::lock_guard lk(g_metrics_mu);
    g_metrics.emplace_back(name, h);
  }
  return h;
}

quiver_mod* quiver_sdk() { return g_q; }

void quiver_text(const std::string& metric, const std::string& text) {
  quiver_readout(g_q, metric.c_str(), text.c_str());
}

}  // namespace th
