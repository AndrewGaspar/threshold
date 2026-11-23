#pragma once
#include <string>

struct IDXGISwapChain;

namespace th::stereo {

// output = full on a plain side-by-side desktop (e.g. 3840x1080 for two 1920x1080 eyes): the game's
// window is made twice the engine's width while the engine keeps one eye's size (docs/stereo.md,
// "The full side-by-side window"). Inert unless started.
bool window_start();                       // hooks (app_rmdwin10_f.dll, signature-checked); at start-up
void window_present(IDXGISwapChain* sc);   // renderer thread, each Present: subclass the window once
std::string window_status();

}  // namespace th::stereo
