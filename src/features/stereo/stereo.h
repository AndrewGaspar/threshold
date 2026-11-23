#pragma once

namespace th {

// Stereoscopic 3D: both eyes rendered from the same game update and composed side by side
// (docs/stereo.md). The game thread describes a frame once; on the renderer thread the main view's
// world render (RendererX86 vtable slot 15) runs once per eye inside the same engine frame, the HUD
// is drawn into each half, and the engine presents once.
void stereo_register();
void stereo_start();

}  // namespace th
