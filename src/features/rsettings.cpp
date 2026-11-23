#include "features/rsettings.h"

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/module.h"

namespace th {
namespace {

std::vector<std::string> g_report;

// -1 unset, else 0/1, from threshold.ini (the config system isn't up yet in DllMain).
int ini_bool(const wchar_t* section, const wchar_t* key) {
  std::wstring path = game_dir() + L"\\threshold.ini";
  wchar_t buf[32];
  GetPrivateProfileStringW(section, key, L"", buf, 32, path.c_str());
  std::wstring v(buf);
  while (!v.empty() && (v.back() == L' ' || v.back() == L'\t')) v.pop_back();
  if (v.empty()) return -1;
  return (v == L"1" || v == L"true" || v == L"on" || v == L"yes") ? 1 : 0;
}

bool read_file(const std::wstring& path, std::string& out) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER sz{};
  GetFileSizeEx(h, &sz);
  out.resize((size_t)sz.QuadPart);
  DWORD n = 0;
  bool ok = ReadFile(h, out.data(), (DWORD)out.size(), &n, nullptr) && n == out.size();
  CloseHandle(h);
  return ok;
}

bool write_file(const std::wstring& path, const std::string& data) {
  std::wstring tmp = path + L".threshold.tmp";
  HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD n = 0;
  bool ok = WriteFile(h, data.data(), (DWORD)data.size(), &n, nullptr) && n == data.size();
  CloseHandle(h);
  return ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// Sets `"key": <bool>` in the JSON text; returns false if the key isn't there.
bool set_bool(std::string& json, const char* key, bool value, std::string& was) {
  std::string k = std::string("\"") + key + "\"";
  size_t p = json.find(k);
  if (p == std::string::npos) return false;
  size_t colon = json.find(':', p + k.size());
  if (colon == std::string::npos) return false;
  size_t v = json.find_first_not_of(" \t", colon + 1);
  size_t e = json.find_first_of(",\r\n}", v);
  if (v == std::string::npos || e == std::string::npos) return false;
  was = json.substr(v, e - v);
  json.replace(v, e - v, value ? "true" : "false");
  return true;
}

}  // namespace

void rsettings_early() {
  struct Want { const wchar_t* section; const wchar_t* key; const char* json; bool invert; };
  const Want wants[] = {
      {L"game", L"skip_intro", "m_bSkipIntroVideos", false},
      {L"dlss", L"legacy_model", "m_bDLSSLegacyModel", false},
  };
  std::wstring path = game_dir() + L"\\renderer.ini";
  std::string json;
  bool have = read_file(path, json);
  std::string orig = json;
  char line[256];
  for (const Want& w : wants) {
    int v = ini_bool(w.section, w.key);
    if (v < 0) continue;
    std::string was;
    if (!have) {
      snprintf(line, sizeof line, "renderer.ini: %s wanted %s, but the file doesn't exist yet (the game writes it on its first run)",
               w.json, v ? "true" : "false");
    } else if (set_bool(json, w.json, v != 0, was)) {
      snprintf(line, sizeof line, "renderer.ini: %s %s -> %s", w.json, was.c_str(), v ? "true" : "false");
    } else {
      snprintf(line, sizeof line, "renderer.ini: %s not found, left alone", w.json);
    }
    g_report.push_back(line);
  }
  if (have && json != orig && !write_file(path, json)) g_report.push_back("renderer.ini: write FAILED");
}

void rsettings_report() {
  for (auto& l : g_report) TH_LOG("%s", l.c_str());
}

}  // namespace th
