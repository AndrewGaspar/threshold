#include "core/module.h"

#include <windows.h>

#include <cctype>

#include "core/log.h"

namespace th {

std::wstring game_dir() {
  wchar_t buf[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  std::wstring p(buf, n);
  size_t slash = p.find_last_of(L"\\/");
  return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}

static Image image_of(HMODULE m) {
  Image i;
  if (!m) return i;
  auto* base = reinterpret_cast<uint8_t*>(m);
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  i.base = reinterpret_cast<uintptr_t>(base);
  i.size = nt->OptionalHeader.SizeOfImage;
  return i;
}

const Image& game_image() {
  static Image img = image_of(GetModuleHandleW(nullptr));
  return img;
}

Image module_image(const wchar_t* name) { return image_of(GetModuleHandleW(name)); }

void* module_export(const wchar_t* module, const char* name, const char* what) {
  HMODULE m = GetModuleHandleW(module);
  void* p = m ? reinterpret_cast<void*>(GetProcAddress(m, name)) : nullptr;
  if (!p && what) TH_LOG("%s: export %s not found in %ls%s", what, name, module, m ? "" : " (module not loaded)");
  return p;
}

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  c = (char)std::tolower((unsigned char)c);
  return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

bool sig_match(const void* p, const char* sig) {
  auto* b = static_cast<const uint8_t*>(p);
  size_t i = 0;
  for (const char* s = sig; *s;) {
    if (*s == ' ') { ++s; continue; }
    if (s[0] == '?' && s[1] == '?') { s += 2; ++i; continue; }
    int hi = hexval(s[0]), lo = s[1] ? hexval(s[1]) : -1;
    if (hi < 0 || lo < 0) return false;
    if (b[i] != (uint8_t)(hi << 4 | lo)) return false;
    s += 2;
    ++i;
  }
  return true;
}

}  // namespace th
