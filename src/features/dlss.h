#pragma once

namespace th {

// The game's own DLSS (d3d::DLSS in d3d_rmdwin10_f.dll, re/dlss.md).
//  - Ray Reconstruction guard: the game's RR path is unfinished (both doAntiAliasing callers pass
//    NULL normals/albedo, re/dlss.md 1.6). On Windows the RR feature never gets created (no
//    nvngx_dlssd.dll in the install), but Proton's NGX finds the driver's nvngx_dlssd.dll, so with
//    m_eDLSSQuality = 2 the game crashes on its first in-world frame (lab r8/r9, 0xC0000005).
//    Unless [dlss] ray_reconstruction = 1, RR feature creation is refused, and the game falls back
//    to Super Resolution exactly as it does on Windows.
void dlss_register();
void dlss_start();

}  // namespace th
