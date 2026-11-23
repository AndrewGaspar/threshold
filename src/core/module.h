#pragma once
#include <cstdint>
#include <string>

namespace th {

// The game's install folder (where Control_DX12.exe and our dxgi.dll live), no trailing slash.
std::wstring game_dir();

// A loaded module's image: base and size, for RVA <-> address and range checks. Control's
// engine lives in DLLs (renderer_rmdwin10_f.dll, d3d_rmdwin10_f.dll, ...); notes give RVAs per
// module (re/*.md).
struct Image {
  uintptr_t base = 0;
  size_t size = 0;
  uintptr_t at(uintptr_t rva) const { return base + rva; }
  bool contains(uintptr_t p) const { return p >= base && p < base + size; }
  explicit operator bool() const { return base != 0; }
};
const Image& game_image();  // the main executable

// A module by file name (e.g. L"renderer_rmdwin10_f.dll"); empty Image if it isn't loaded.
// Not cached: the engine DLLs are static imports of the exe, so they're loaded before our init.
Image module_image(const wchar_t* name);

// The address of a module's export (mangled C++ names work: the engine DLLs export them), or
// nullptr. Logged on failure when `what` is given.
void* module_export(const wchar_t* module, const char* name, const char* what = nullptr);

// Whether `p` starts with `sig` (hex bytes, "??" wildcards, spaces ignored).
bool sig_match(const void* p, const char* sig);

}  // namespace th
