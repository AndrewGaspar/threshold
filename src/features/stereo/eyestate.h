#pragma once
#include <string>

#include "core/module.h"

namespace th::stereo {

// Per-eye temporal state (docs/stereo.md "Temporal state per eye"; re/stereo.md section 3). With
// both eyes rendered inside one engine frame, every history the renderer keeps ("last frame") would
// be the other eye's. Around each eye's world render the engine's state is switched to that eye's
// copy; the left eye uses the engine's own, the right eye a second set.
void eyestate_register();
void eyestate_start(const Image& renderer);
// Once per sbs frame, before the first eye (sbs = both eyes follow).
void eyestate_frame(void* r, bool sbs);
// Around one eye's world render: eye 0 left, 1 right (sbs), -1 a single-eye/mono frame.
void eyestate_enter(void* r, int eye);
void eyestate_leave(void* r, int eye);
// A frame without a stereo pair: the engine's own state is in place (restores it if needed).
void eyestate_mono(void* r);
std::string eyestate_status();
// The render graph's persisted-resource lists (the engine's, and the right eye's), as JSON.
std::string eyestate_graph();
// A history texture for a capture (NativeTexture*): kind "gl"/"gr" (graph list entry `i`, engine /
// right eye), "dlfL"/"dlfR" (DLF history i), "ssaoL"/"ssaoR" (SSAO pair pointer i). Between frames only.
void* eyestate_texture(const std::string& kind, int i);
void eyestate_probe(int n);

}  // namespace th::stereo
