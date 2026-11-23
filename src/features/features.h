#pragma once

namespace th {

// Config listeners and lab verbs (before config_init, so they see the initial values).
void features_register();
// Hooks into the game's code (after config_init).
void features_start();

}  // namespace th
