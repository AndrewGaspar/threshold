#pragma once

namespace th {

// Scenario navigation for lab runs (re/levels.md, docs/navigation.md). Lab only: outside the lab
// (no QUIVER_LAB) nothing is hooked and no verb is registered.
//  - where: player position, facing, camera yaw/pitch, loaded level, cinematic/control state;
//    lab state menu / loading / cinematic / in-world.
//  - teleport x y z [yaw [pitch]]: TransformComponentState::handleTeleport on the server avatar.
//  - jump list | jump <name>: the exe's jump point registry and its useJumpPoint.
//  - episode <name> [jumppoint] [wait|skip] | reload [jumppoint] | restart: the game's level load
//    (ServerGameSession::startLoadingLevel, as fast travel does) and checkpoint restart.
//  - skip: skip the playing cinematic(s) the game's own way; save: a checkpoint save now.
// Every server-side action runs on the game's server thread, from a hook on the exported
// coregame::ServerGameSession::update(float).
void nav_register();
void nav_start();

}  // namespace th
