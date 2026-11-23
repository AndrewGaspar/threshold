#pragma once
#include <string>

#include "quiver_mod.h"

namespace th {

// Quiver mod SDK (third_party/quiver_mod.h): surface "threshold". Values Quiver sends live go
// into the config (cfg_set), so features only ever listen to the config.
void quiver_init();

// Tell Quiver about a value the mod changed itself (persist: save it to the profile).
void quiver_report(const std::string& id, const std::string& text, bool persist);
void quiver_text(const std::string& metric, const std::string& text);

// Option ids the mod applies live (sent as `set` while the game runs).
void quiver_live(const char* id);
// The packing of the presented image ("sbs", "hsbs", "mono"), so Quiver's Sight HUD/panel draws
// once per eye (mod protocol 3, quiver_set_stereo_layout). Any thread; kept until the SDK is open.
void quiver_stereo_layout(const char* layout);

// Metrics (Quiver's design/SIGHT-HUD.md section 3; namespace "threshold."), shown in Sight's HUD
// (the Full preset's mod strip: basic gauges, states and texts) and panel. Declare at start with
// the full descriptor as JSON members ("\"label\":\"...\",..."): this Quiver doesn't merge the
// [metric.*] tables of dist/quiver-options.toml into a runtime declaration, so they are repeated
// there for the catalog and lint. QUIVER_NO_METRIC without Quiver (then every call is a no-op).
// Quiver's verdict on each is logged once the SDK has connected.
quiver_metric metric_declare(const char* name, quiver_kind kind, const char* json_members);
quiver_mod* quiver_sdk();  // the open handle (null before quiver_init)

}  // namespace th
