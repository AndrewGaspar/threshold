#pragma once

namespace th {

// Edits to the game's renderer.ini (rend::UserRendererSettings, JSON, in the install folder for
// the GOG build) before the renderer reads it. Called from DllMain: our dxgi.dll is a static
// import of d3d_rmdwin10_f.dll, so this runs before the game's code. Plain file I/O only.
//   [game] skip_intro = 1      -> "m_bSkipIntroVideos": true
//   [dlss] legacy_model = 0    -> "m_bDLSSLegacyModel": false (the game's default is true: the
//                                 old CNN model; false selects the transformer model)
// Only keys present in the file are changed (the game writes the file on its first run), and an
// unset option leaves the game's value alone.
void rsettings_early();
// Logs what rsettings_early() did (call once the log is open).
void rsettings_report();

}  // namespace th
