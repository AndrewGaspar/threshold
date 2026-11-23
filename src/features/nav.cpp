#include "features/nav.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/hook.h"
#include "core/lab.h"
#include "core/log.h"
#include "core/module.h"

namespace th {
namespace {

constexpr wchar_t kCore[] = L"coregame_rmdwin10_f.dll";
constexpr wchar_t kRl[] = L"rl_rmdwin10_f.dll";
constexpr wchar_t kInput[] = L"input_rmdwin10_f.dll";
constexpr double kDeg = 180.0 / 3.14159265358979323846;

// ---- engine types (re/levels.md 2-4) ----------------------------------------------------------

struct alignas(16) Vec4 {
  float x, y, z, w;
};
struct alignas(16) SIMDTransform {  // m::SIMDTransform: rotation quaternion, then position
  Vec4 q, p;
};
// r::InplaceString<64> (static: exe+0x2d07f0 builds one as {buf, buf, 0x40, 0x40, len=0}, inline
// buffer at +0x20; rl String::operator= keeps data at +0x08, capacity at +0x14, length at +0x18).
struct IStr64 {
  char* buf;
  char* data;
  uint32_t cap, cap2, len, pad;
  char inl[64];
};
static_assert(sizeof(IStr64) == 0x60, "InplaceString<64> is 0x60 bytes");
void istr_set(IStr64& s, const std::string& v) {
  std::memset(&s, 0, sizeof s);
  s.buf = s.data = s.inl;
  s.cap = s.cap2 = 64;
  size_t n = v.size() < 63 ? v.size() : 63;
  std::memcpy(s.inl, v.data(), n);
  s.len = uint32_t(n);
}
// An r::String the engine owns: data pointer at +0x08, length at +0x18.
std::string rstr(const void* s) {
  if (!s) return "";
  auto* p = static_cast<const uint8_t*>(s);
  const char* d = *reinterpret_cast<char* const*>(p + 8);
  uint32_t n = *reinterpret_cast<const uint32_t*>(p + 0x18);
  if (!d || n > 1024) return "";
  return std::string(d, n);
}

// coregame / rl exports
using Update_t = void (*)(void* ses, float dt);
using GetAvatar_t = void* (*)(void* ses);
using GetTypeId_t = unsigned (*)();
using GetComponent_t = void* (*)(const void* obj, unsigned typeId);
using HandleTeleport_t = void (*)(void* xf, float x, float y, float z, float yawDeg);
using Teleport_t = void (*)(void* xf, const SIMDTransform* prev, const SIMDTransform* cur);
using StartLoadingEpisode_t = void (*)(void* ses, const IStr64* episode, const IStr64* jumpPoint, bool flag);
using StartReloading_t = void (*)(void* ses, const IStr64* jumpPoint, bool flag);
// startLoadingLevel(world, episode, jumppoint, bool, fromSave): what the game's own fast travel
// and episode transitions call (static: exe+0x206d90, exe+0x1c8270, exe+0x5d1910, all with
// (.., false, true)). fromSave=true makes the server's loading mode 4, the one checkpoint
// restarts use (static: exe+0x54baa0).
using StartLoadingLevel_t = void (*)(void* ses, const IStr64* world, const IStr64* episode, const IStr64* jumpPoint,
                                     bool p4, bool fromSave);
using InputQuery_t = bool (*)(void* im);
// GameHelper::saveGame(role, threadSafe, force): role 2 = server (static: exe+0x1c8270 maps the
// component's role bits 3 -> 2 before calling it).
using SaveGame_t = void (*)(int role, bool threadSafe, bool force);
using SkipTimeline_t = void (*)(void* timeline);               // exe+0x5aa840
using IdEq_t = bool (*)(const void* id, const void* other);    // r::StateMachineDefinition::ID::operator==
using GetActionName_t = void* (*)(void* im, void* ret, int id);  // returns InplaceString<128> by value
using Restart_t = void (*)(void* ses);
// exe, by RVA (no exports)
using FindJumpPoint_t = void* (*)(int role, const IStr64* name);
using UseJumpPoint_t = void (*)(void* jp, bool reloadEpisode, bool debugJump);
using JpName_t = const void* (*)(void* jp);  // vfunc +0x2C0, r::String const&

Update_t o_update;
GetAvatar_t g_getAvatar;
GetTypeId_t g_xfTypeId;
GetComponent_t g_getComponent;
HandleTeleport_t g_handleTeleport;
Teleport_t g_teleport;
StartLoadingEpisode_t g_startLoadingEpisode;
StartReloading_t g_startReloading;
StartLoadingLevel_t g_startLoadingLevel;
SaveGame_t g_saveGame;
SkipTimeline_t g_skipTimeline;
IdEq_t g_idEq;
uintptr_t g_tlPlaying;  // exe+0x12cbe68: the timeline state ID "playing" the TimelineManager tests
InputQuery_t g_isPlayerControl, g_isMenuOn;
GetActionName_t g_getActionName;
void** g_inputMgr;     // input::InputManager::sm_pInstance
void** g_serverBase;   // coregame::GameHelper::s_pGameServerBase
Restart_t g_restartCheckpoint, g_restartLevel;
FindJumpPoint_t g_findJumpPoint;
UseJumpPoint_t g_useJumpPoint;

// exe data
uintptr_t g_jpArray, g_jpCount, g_jpLock;  // jump point registry + its recursive lock
uintptr_t g_optionsPtr;                   // DAT_14129a588: the game-thread options struct
uintptr_t g_renderView;                   // g_renderView (m::PerspectiveView, doubles)
uintptr_t g_saveMgr;                      // DAT_14129a6e0: save manager pointer
uintptr_t g_timelineMgr;                  // DAT_141249540: TimelineManager* (cinematics)

bool readable(const void* p, size_t n);

// The save manager's world / episode / jump point (InplaceString<32> at +0x288/+0x2C8/+0x308,
// live r2): what the last save (or Continue) recorded, not necessarily where the player is now.
std::string save_info() {
  if (!g_saveMgr) return "";
  uintptr_t sm = *reinterpret_cast<uintptr_t*>(g_saveMgr);
  if (!sm || !readable(reinterpret_cast<void*>(sm + 0x288), 0xA0)) return "";
  std::string out;
  const char* keys[] = {"world", "episode", "jumppoint"};
  for (int i = 0; i < 3; i++) {
    auto* str = reinterpret_cast<const uint8_t*>(sm + 0x288 + 0x40 * i);
    const char* d = *reinterpret_cast<char* const*>(str + 8);
    uint32_t n = *reinterpret_cast<const uint32_t*>(str + 0x18);
    std::string v = d && n < 64 && readable(d, n) ? std::string(d, n) : "";
    out += std::string(i ? "," : "") + "\"" + keys[i] + "\":" + json_str(v);
  }
  return "{" + out + "}";
}

// The world and episode the server has loaded now (unlike save_info, this follows episode loads).
// Path from the exe's own "current level" reads (static: exe+0x206d90 fast travel and
// exe+0x54baa0 restart): GameServerBase +0x1e8 -> +0x730 = level; level +0xb8 = world (char*),
// level +0x308 -> +0x148 = episode, name inline at +0x138.
bool level_info(std::string& world, std::string& episode) {
  world.clear();
  episode.clear();
  if (!g_serverBase) return false;
  auto rd = [](uintptr_t a, uintptr_t& out) {
    if (!a || !readable(reinterpret_cast<void*>(a), 8)) return false;
    out = *reinterpret_cast<uintptr_t*>(a);
    return out != 0;
  };
  uintptr_t srv = reinterpret_cast<uintptr_t>(*g_serverBase), a, lvl, e1, ep, wname;
  if (!srv || !rd(srv + 0x1e8, a) || !rd(a + 0x730, lvl)) return false;
  if (rd(lvl + 0xb8, wname) && readable(reinterpret_cast<void*>(wname), 64))
    world.assign(reinterpret_cast<const char*>(wname), strnlen(reinterpret_cast<const char*>(wname), 63));
  if (rd(lvl + 0x308, e1) && rd(e1 + 0x148, ep) && readable(reinterpret_cast<void*>(ep + 0x138), 64))
    episode.assign(reinterpret_cast<const char*>(ep + 0x138), strnlen(reinterpret_cast<const char*>(ep + 0x138), 63));
  return true;
}

// Cinematics: the exe's TimelineManager (DAT_141249540; ctor exe+0x5b5ca0, update exe+0x5b6080)
// recomputes each frame +0x80 "a skippable timeline plays", +0x81 "a timeline plays", +0x82
// letterbox; its update skips the skippable one (exe+0x5aa840, script OnTimelineSkipped) when the
// skip input is read, or always while options +0x13a is set (static: exe+0x5b6209).
struct Cine {
  bool known = false, skippable = false, active = false, letterbox = false;
};
Cine cine_state() {
  Cine c;
  if (!g_timelineMgr) return c;
  uintptr_t tm = *reinterpret_cast<uintptr_t*>(g_timelineMgr);
  if (!tm || !readable(reinterpret_cast<void*>(tm + 0x80), 3)) return c;
  auto* b = reinterpret_cast<const volatile uint8_t*>(tm + 0x80);
  c.known = true;
  c.skippable = b[0];
  c.active = b[1];
  c.letterbox = b[2];
  return c;
}

// input::InputManager::isPlayerControlEnabled (+0x7e && +0x7d) and isMenuOn (+0x74); -1 unknown.
int player_control() {
  if (!g_inputMgr || !g_isPlayerControl || !*g_inputMgr) return -1;
  return g_isPlayerControl(*g_inputMgr) ? 1 : 0;
}
int menu_on() {
  if (!g_inputMgr || !g_isMenuOn || !*g_inputMgr) return -1;
  return g_isMenuOn(*g_inputMgr) ? 1 : 0;
}

// `skip`: hold options +0x13a (the TimelineManager's "skip every skippable timeline") until no
// skippable timeline has played for a while, then put the old value back.
std::atomic<DWORD> g_skipUntil{0};  // GetTickCount deadline; 0 = not skipping
std::atomic<int> g_skipCount{0};    // skippable timelines seen ending while we held the byte

// The save manager's episode and jump point (the checkpoint a restart goes back to).
bool save_point(std::string& ep, std::string& jp) {
  ep.clear();
  jp.clear();
  if (!g_saveMgr) return false;
  uintptr_t sm = *reinterpret_cast<uintptr_t*>(g_saveMgr);
  if (!sm || !readable(reinterpret_cast<void*>(sm + 0x2C8), 0x80)) return false;
  for (int i = 0; i < 2; i++) {
    auto* str = reinterpret_cast<const uint8_t*>(sm + 0x2C8 + 0x40 * i);
    const char* d = *reinterpret_cast<char* const*>(str + 8);
    uint32_t n = *reinterpret_cast<const uint32_t*>(str + 0x18);
    (i ? jp : ep) = d && n < 64 && readable(d, n) ? std::string(d, n) : "";
  }
  return !ep.empty();
}

bool g_ok = false;          // update hooked and the avatar path resolved
unsigned g_tid = 0;         // TransformComponentState type id (resolved on the server thread)
uintptr_t g_onIsLoading;    // coregame::onIsLoading, a std::function<bool()> (MSVC: impl at +0x38)

// coregame::onIsLoading(): the predicate GameHelper::saveGame also asks (static: coregame+0xdda51
// `mov rcx,[onIsLoading+0x38]; call [vtbl+0x10]`). False when unset.
bool engine_loading() {
  if (!g_onIsLoading) return false;
  void* impl = *reinterpret_cast<void**>(g_onIsLoading + 0x38);
  if (!impl) return false;
  auto vt = *reinterpret_cast<uintptr_t**>(impl);
  static void* checked = nullptr;  // the _Do_call we last verified lives in a loaded module
  if (reinterpret_cast<void*>(vt[2]) != checked) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(vt[2]), &m)) {
      TH_LOG("nav: onIsLoading's call target %p is not in a module; loading state disabled", (void*)vt[2]);
      g_onIsLoading = 0;
      return false;
    }
    checked = reinterpret_cast<void*>(vt[2]);
  }
  return reinterpret_cast<bool (*)(void*)>(vt[2])(impl);
}

// ---- signature helpers ------------------------------------------------------------------------

template <class T>
bool resolve_export(T& out, const wchar_t* mod, const char* name, const char* sig, const char* what) {
  void* p = module_export(mod, name, "nav");
  if (!p) return false;
  if (!sig_match(p, sig)) {
    TH_LOG("nav: %s: signature mismatch at %ls export, disabled", what, mod);
    return false;
  }
  out = reinterpret_cast<T>(p);
  return true;
}

template <class T>
bool resolve_exe(T& out, uintptr_t rva, const char* sig, const char* what) {
  void* p = reinterpret_cast<void*>(game_image().at(rva));
  if (!sig_match(p, sig)) {
    TH_LOG("nav: %s: signature mismatch at exe+0x%llx, disabled", what, (unsigned long long)rva);
    return false;
  }
  out = reinterpret_cast<T>(p);
  return true;
}

// Target RVA of a RIP-relative operand: `insn_rva` is where the instruction starts, the 32-bit
// displacement is at `disp_off` and the instruction is `len` bytes long.
uintptr_t rip_target(uintptr_t insn_rva, int disp_off, int len) {
  auto* p = reinterpret_cast<const uint8_t*>(game_image().at(insn_rva));
  int32_t d;
  std::memcpy(&d, p + disp_off, 4);
  return insn_rva + len + d;
}

bool readable(const void* p, size_t n) {
  MEMORY_BASIC_INFORMATION mi;
  auto a = reinterpret_cast<uintptr_t>(p);
  while (n) {
    if (!VirtualQuery(reinterpret_cast<void*>(a), &mi, sizeof mi)) return false;
    if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    uintptr_t end = reinterpret_cast<uintptr_t>(mi.BaseAddress) + mi.RegionSize;
    size_t chunk = end - a < n ? end - a : n;
    a += chunk;
    n -= chunk;
  }
  return true;
}

// ---- server-thread work queue -----------------------------------------------------------------

struct Job {
  std::function<std::string(void* ses, bool& ok)> fn;
  std::string res;
  bool ok = true, done = false;
};
std::mutex g_qmu;
std::condition_variable g_qcv;
std::deque<std::shared_ptr<Job>> g_q;

struct Snap {
  DWORD tick = 0;    // GetTickCount of the last ServerGameSession::update
  bool player = false;
  bool loading = false;  // coregame::onIsLoading()
  bool jumping = false;  // our last useJumpPoint's streaming wait is still pending
  DWORD loadReq = 0;     // GetTickCount of our last level load request (0: none pending)
  float pos[3]{}, quat[4]{};
  uint64_t updates = 0;
};
std::mutex g_smu;
Snap g_snap;
// Held while the state file is computed and written, so a verb's "loading" can't be overwritten by
// a monitor pass that read the snapshot just before the verb changed it.
std::mutex g_statemu;
void state_loading() {
  std::lock_guard lk(g_statemu);
  lab_state("loading");
}

std::string run_on_server(std::function<std::string(void*, bool&)> fn, bool& ok, DWORD timeout_ms = 10000) {
  if (!g_ok) {
    ok = false;
    return json_str("nav is disabled (see threshold.log)");
  }
  {
    std::lock_guard lk(g_smu);
    if (GetTickCount() - g_snap.tick > 3000) {
      ok = false;
      return json_str("no game session (main menu or loading)");
    }
  }
  auto j = std::make_shared<Job>();
  j->fn = std::move(fn);
  std::unique_lock lk(g_qmu);
  g_q.push_back(j);
  if (!g_qcv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return j->done; })) {
    for (auto it = g_q.begin(); it != g_q.end(); ++it)
      if (*it == j) {
        g_q.erase(it);
        break;
      }
    ok = false;
    return json_str("the server thread did not run the request (session paused or gone?)");
  }
  ok = j->ok;
  return j->res;
}

// The player's transform component, or null.
void* player_xf(void* ses) {
  void* ent = g_getAvatar(ses);
  if (!ent) return nullptr;
  if (!g_tid) g_tid = g_xfTypeId();
  return g_getComponent(ent, g_tid);
}

// The jump point our last `jump` used (server thread only). useJumpPoint sets bits 0/1 of
// jp+0xB8 while its streaming wait / player hide is pending and clears them when it completes
// (static: exe+0x4823c0, `*(byte*)(param_1+0x17) |= 1 / |= 2`; FUN_140482980 finishes).
void* g_lastJp = nullptr;

bool jump_pending() {
  if (!g_lastJp) return false;
  auto* b = static_cast<uint8_t*>(g_lastJp) + 0xB8;
  if (!readable(b, 1)) {
    g_lastJp = nullptr;
    return false;
  }
  if (*b & 3) return true;
  g_lastJp = nullptr;
  return false;
}

// The TimelineManager lists client and server copies of each timeline (+0x10 array of 16-byte
// entries, count +0x18) and its own skip (options +0x13a) skips the *last* skippable one it finds
// (static: exe+0x5b6280 loop). When that is the client copy nothing happens and the cinematic
// plays out (live r20: the intro ran 3.5 min with the switch held, after a research -> hub load).
// So while `skip` is active, skip every playing, skippable, server-role timeline ourselves with
// the same function (exe+0x5aa840: jump to the next skip marker, or the end + OnTimelineSkipped).
void skip_server_timelines() {
  uintptr_t tm = *reinterpret_cast<uintptr_t*>(g_timelineMgr);
  if (!tm || !readable(reinterpret_cast<void*>(tm + 0x10), 0x10)) return;
  auto* arr = *reinterpret_cast<uintptr_t**>(tm + 0x10);
  uint32_t n = *reinterpret_cast<uint32_t*>(tm + 0x18);
  if (!arr || n > 4096 || !readable(arr, size_t(n) * 16)) return;
  for (uint32_t i = 0; i < n; i++) {
    uintptr_t tl = arr[i * 2];
    if (!tl || !readable(reinterpret_cast<void*>(tl), 0x200)) continue;
    if ((*reinterpret_cast<uint64_t*>(tl + 0x18) >> 62) != 3) continue;  // server role
    if (!*reinterpret_cast<uint8_t*>(tl + 0x1c0)) continue;                // skippable
    if (!g_idEq(reinterpret_cast<void*>(tl + 0x180), reinterpret_cast<void*>(g_tlPlaying))) continue;
    if (*reinterpret_cast<uint8_t*>(tl + 0x1c1)) continue;  // a skip is already pending
    // A few tries per timeline: when the skip does not take (rayblaster r2: a hub intro after a
    // research -> hub load took 676 calls and played on), repeating it only spams OnTimelineSkipped.
    static uintptr_t last_tl = 0;
    static int tries = 0;
    if (tl != last_tl) last_tl = tl, tries = 0;
    if (++tries > 4) continue;
    TH_LOG("nav: skipping server timeline %p", (void*)tl);
    g_skipTimeline(reinterpret_cast<void*>(tl));
  }
}

void hk_update(void* ses, float dt) {
  o_update(ses, dt);
  void* xf = player_xf(ses);
  bool loading = engine_loading();
  if (loading || !xf) g_lastJp = nullptr;  // an episode (re)load drops the registry
  bool jumping = jump_pending();
  {
    std::lock_guard lk(g_smu);
    bool had = g_snap.player;
    // A level load we requested counts as loading until the engine shows it (or 15 s pass).
    if (g_snap.loadReq && (loading || !xf || GetTickCount() - g_snap.loadReq > 15000)) g_snap.loadReq = 0;
    loading = loading || g_snap.loadReq;
    if (loading != g_snap.loading)
      TH_LOG("nav: loading %s (update %llu)", loading ? "on" : "off", (unsigned long long)g_snap.updates);
    g_snap.loading = loading;
    if (jumping != g_snap.jumping)
      TH_LOG("nav: jump streaming wait %s (update %llu)", jumping ? "pending" : "done", (unsigned long long)g_snap.updates);
    g_snap.jumping = jumping;
    g_snap.tick = GetTickCount();
    g_snap.updates++;
    g_snap.player = xf != nullptr;
    if (xf) {
      auto* f = reinterpret_cast<const float*>(static_cast<uint8_t*>(xf) + 0xD0);
      auto* q = reinterpret_cast<const float*>(static_cast<uint8_t*>(xf) + 0xC0);
      for (int i = 0; i < 3; i++) g_snap.pos[i] = f[i];
      for (int i = 0; i < 4; i++) g_snap.quat[i] = q[i];
    }
    if (had != g_snap.player)
      TH_LOG("nav: player %s (session %p, update %llu)", xf ? "present" : "gone", ses,
             (unsigned long long)g_snap.updates);
  }
  if (g_skipUntil && g_skipTimeline && g_idEq && g_tlPlaying && g_timelineMgr && (g_snap.updates % 16) == 0)
    skip_server_timelines();
  std::deque<std::shared_ptr<Job>> jobs;
  {
    std::lock_guard lk(g_qmu);
    jobs.swap(g_q);
  }
  if (jobs.empty()) return;
  for (auto& j : jobs) {
    bool ok = true;
    std::string r = j->fn(ses, ok);
    std::lock_guard lk(g_qmu);
    j->res = std::move(r);
    j->ok = ok;
    j->done = true;
  }
  g_qcv.notify_all();
}

// ---- formatting -------------------------------------------------------------------------------

std::string fnum(double v, int prec = 3) {
  if (!std::isfinite(v)) return "null";
  char b[64];
  snprintf(b, sizeof b, "%.*f", prec, v);
  return b;
}

double yaw_of_quat(const float* q) {  // rotation about +Y (up), degrees; handleTeleport's yaw
  double x = q[0], y = q[1], z = q[2], w = q[3];
  return std::atan2(2 * (w * y + x * z), 1 - 2 * (y * y + x * x)) * kDeg;
}

// Camera yaw/pitch (degrees) from g_renderView's view-to-world forward row (+0xA0, doubles).
// Same yaw convention as the player's facing: yaw = atan2(fwd.x, fwd.z), pitch = asin(fwd.y).
bool camera_angles(double& yaw, double& pitch, double* pos = nullptr) {
  if (!g_renderView) return false;
  auto* m = reinterpret_cast<const double*>(g_renderView + 0x70);
  const double* f = m + 6;
  double len = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
  if (!(len > 0.5 && len < 1.5)) return false;
  yaw = std::atan2(f[0], f[2]) * kDeg;
  double s = f[1] / len;
  pitch = std::asin(s < -1 ? -1 : s > 1 ? 1 : s) * kDeg;
  if (pos)
    for (int i = 0; i < 3; i++) pos[i] = m[9 + i];
  return true;
}

double parse_num(const std::string& s, bool& good) {
  char* e = nullptr;
  double v = std::strtod(s.c_str(), &e);
  good = e && *e == 0 && !s.empty() && std::isfinite(v);
  return v;
}

// ---- verbs ------------------------------------------------------------------------------------

std::string verb_where(const std::vector<std::string>&, bool& ok) {
  Snap s;
  {
    std::lock_guard lk(g_smu);
    s = g_snap;
  }
  DWORD age = GetTickCount() - s.tick;
  if (!g_ok || !s.player || age > 3000) {
    ok = false;
    return json_str(!g_ok ? "nav is disabled (see threshold.log)" : "no player (main menu or loading)");
  }
  std::string o = "{\"x\":" + fnum(s.pos[0]) + ",\"y\":" + fnum(s.pos[1]) + ",\"z\":" + fnum(s.pos[2]) +
                  ",\"facing\":" + fnum(yaw_of_quat(s.quat), 2);
  double yaw, pitch, cp[3];
  if (camera_angles(yaw, pitch, cp))
    o += ",\"camera\":{\"yaw\":" + fnum(yaw, 2) + ",\"pitch\":" + fnum(pitch, 2) + ",\"x\":" + fnum(cp[0]) +
         ",\"y\":" + fnum(cp[1]) + ",\"z\":" + fnum(cp[2]) + "}";
  std::string sv = save_info();
  if (!sv.empty()) o += ",\"save\":" + sv;
  std::string lw, le;
  if (level_info(lw, le)) o += ",\"level\":{\"world\":" + json_str(lw) + ",\"episode\":" + json_str(le) + "}";
  Cine c = cine_state();
  if (c.known)
    o += std::string(",\"cinematic\":{\"active\":") + (c.active ? "true" : "false") +
         ",\"skippable\":" + (c.skippable ? "true" : "false") + ",\"letterbox\":" + (c.letterbox ? "true" : "false") + "}";
  int pc = player_control(), mo = menu_on();
  if (pc >= 0) o += std::string(",\"control\":") + (pc ? "true" : "false");
  if (mo >= 0) o += std::string(",\"menu\":") + (mo ? "true" : "false");
  o += std::string(",\"loading\":") + (s.loading ? "true" : "false") + ",\"jumping\":" + (s.jumping ? "true" : "false") +
       ",\"age_ms\":" + std::to_string(age) + "}";
  return o;
}

std::string do_jump(const std::string& name, bool reload, bool& ok);
bool wait_jump_done(DWORD timeout_ms);
std::string load_level(std::string world, std::string ep, const std::string& jp, bool save, bool& ok,
                       bool use_restart = true);
// Start holding the skip now. Loads that skip should start it before the load: the opening
// cinematic skipped within a second whenever the skip was already on when it began (Continue,
// restart; laptop r8-r14, rayblaster r2), but played on for 2.5 to 4 min, both the options byte
// and the direct skip held, when the skip started as the player appeared (laptop r20, rayblaster
// r2/r3).
void skip_from_now(DWORD secs) {
  g_skipCount = 0;
  g_skipUntil = GetTickCount() + secs * 1000;
}
std::string wait_level(DWORD timeout_ms, bool skip);

// teleport x y z [yaw [pitch]] [via=<jumppoint>] [episode=<e> [entry=<jumppoint>]]: with
// episode, first load that episode (the game's level load) when another one is loaded, at
// `entry` (default: via), skip the cinematics that follow and wait for the player's control; with
// via, jump there (the jump's streaming wait loads the area); then place the player exactly.
std::string verb_teleport(const std::vector<std::string>& args, bool& ok) {
  std::vector<std::string> a;
  std::string via, episode, entry;
  for (auto& s : args) {
    if (s.rfind("via=", 0) == 0) via = s.substr(4);
    else if (s.rfind("episode=", 0) == 0) episode = s.substr(8);
    else if (s.rfind("entry=", 0) == 0) entry = s.substr(6);
    else if (s.find('=') == std::string::npos) a.push_back(s);
  }
  if (a.size() < 3) {
    ok = false;
    return json_str("usage: teleport x y z [yaw [pitch]] [via=<jumppoint>] [episode=<e> [entry=<jumppoint>]]");
  }
  double v[5]{};
  size_t n = a.size() < 5 ? a.size() : 5;
  for (size_t i = 0; i < n; i++) {
    bool good;
    v[i] = parse_num(a[i], good);
    if (!good) {
      ok = false;
      return json_str("not a number: " + a[i]);
    }
  }
  std::string cw, ce;
  if (!episode.empty() && (!level_info(cw, ce) || _stricmp(ce.c_str(), episode.c_str()) != 0)) {
    if (entry.empty()) entry = via;
    skip_from_now(240);  // before the load: see skip_from_now
    std::string r = load_level("", episode, entry, false, ok);
    if (!ok) return r;
    r = wait_level(240000, true);
    g_skipUntil = 0;  // the player has control (or we give up): let the switch go
    if (!r.empty()) {
      ok = false;
      return json_str("loaded " + episode + " at " + entry + ", but " + r);
    }
    if (_stricmp(entry.c_str(), via.c_str()) == 0) via.clear();  // already standing there
  }
  if (!via.empty()) {
    std::string r = do_jump(via, false, ok);
    if (!ok) return r;
    if (!wait_jump_done(120000)) {
      ok = false;
      return json_str("jumped to " + via + ", but its streaming wait did not finish within 120 s");
    }
  }
  bool has_yaw = n >= 4, has_pitch = n >= 5;
  float x = float(v[0]), y = float(v[1]), z = float(v[2]), yaw = float(v[3]);
  std::string r = run_on_server(
      [=](void* ses, bool& ok2) -> std::string {
        void* xf = player_xf(ses);
        if (!xf) {
          ok2 = false;
          return json_str("no player");
        }
        if (has_yaw) {
          g_handleTeleport(xf, x, y, z, yaw);
        } else {
          SIMDTransform t;
          std::memcpy(&t.q, static_cast<uint8_t*>(xf) + 0xC0, 16);
          t.p = {x, y, z, 0.f};
          g_teleport(xf, &t, &t);
        }
        if (has_yaw) TH_LOG("nav: teleport %.3f %.3f %.3f yaw %.1f", x, y, z, yaw);
        else TH_LOG("nav: teleport %.3f %.3f %.3f (keeping rotation)", x, y, z);
        return "";
      },
      ok);
  if (!ok) return r;
  std::string o = "{\"x\":" + fnum(x) + ",\"y\":" + fnum(y) + ",\"z\":" + fnum(z);
  if (has_yaw) {
    double cy, cpitch;
    double p = has_pitch ? v[4] : (camera_angles(cy, cpitch) ? cpitch : 0.0);
    o += ",\"want\":{\"yaw\":" + fnum(v[3], 2) + ",\"pitch\":" + fnum(p, 2) + "}";
  }
  return o + "}";
}

// Registry lock: the exe's own recursive lock (SRWLOCK, owner thread id at +8, count at +0xC),
// taken exactly as findJumpPoint takes it (static: exe+0x481c60).
struct JpLock {
  JpLock() {
    DWORD me = GetCurrentThreadId();
    auto* owner = reinterpret_cast<volatile DWORD*>(g_jpLock + 8);
    if (*owner != me) {
      AcquireSRWLockExclusive(reinterpret_cast<PSRWLOCK>(g_jpLock));
      *owner = me;
    }
    auto* cnt = reinterpret_cast<volatile int*>(g_jpLock + 0xC);
    *cnt = *cnt + 1;
  }
  ~JpLock() {
    auto* cnt = reinterpret_cast<volatile int*>(g_jpLock + 0xC);
    *cnt = *cnt - 1;
    if (*cnt == 0) {
      *reinterpret_cast<volatile DWORD*>(g_jpLock + 8) = 0xFFFFFFFF;
      ReleaseSRWLockExclusive(reinterpret_cast<PSRWLOCK>(g_jpLock));
    }
  }
};

// role as findJumpPoint numbers it: bits 62-63 of +0x18, 2 -> 1 (client), 3 -> 2 (server).
int jp_role(void* jp) {
  uint64_t r = *reinterpret_cast<uint64_t*>(static_cast<uint8_t*>(jp) + 0x18) >> 62;
  return r == 2 ? 1 : r == 3 ? 2 : 0;
}

std::string jump_list(bool all) {
  JpLock lk;
  auto* arr = *reinterpret_cast<void***>(g_jpArray);
  uint32_t n = *reinterpret_cast<uint32_t*>(g_jpCount);
  std::string o = "[";
  int clients = 0;
  bool first = true;
  for (uint32_t i = 0; arr && i < n; i++) {
    void* jp = arr[i];
    if (!jp) continue;
    int role = jp_role(jp);
    if (role != 2 && !all) {
      clients++;
      continue;
    }
    auto vt = *reinterpret_cast<uintptr_t**>(jp);
    std::string name = rstr(reinterpret_cast<JpName_t>(vt[0x2C0 / 8])(jp));
    auto* content = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(jp) + 0x38);
    void* owner = *reinterpret_cast<void**>(static_cast<uint8_t*>(jp) + 0x28);
    void* xf = owner ? g_getComponent(owner, g_tid ? g_tid : (g_tid = g_xfTypeId())) : nullptr;
    if (!first) o += ",";
    first = false;
    o += "{\"name\":" + json_str(name);
    if (all) o += ",\"role\":" + std::to_string(role);
    if (xf) {
      auto* p = reinterpret_cast<const float*>(static_cast<uint8_t*>(xf) + 0xD0);
      auto* q = reinterpret_cast<const float*>(static_cast<uint8_t*>(xf) + 0xC0);
      o += ",\"x\":" + fnum(p[0]) + ",\"y\":" + fnum(p[1]) + ",\"z\":" + fnum(p[2]) + ",\"yaw\":" + fnum(yaw_of_quat(q), 2);
    }
    if (content) o += std::string(",\"startup\":") + (content[0x10] ? "true" : "false") +
                      ",\"moves_player\":" + (content[0x11] ? "true" : "false");
    o += "}";
  }
  (void)clients;
  return o + "]";
}

// useJumpPoint(findJumpPoint(server, name), reload, debugJump=true) on the server thread, as the
// dev RPC does (static: exe+0x2d07f0). Without reload the lab state reads "loading" until the
// streaming wait completes.
std::string do_jump(const std::string& name, bool reload, bool& ok) {
  if (!g_findJumpPoint || !g_useJumpPoint || !g_jpArray) {
    ok = false;
    return json_str("jump points are disabled (signature mismatch, see threshold.log)");
  }
  std::string r = run_on_server(
      [=](void*, bool& ok2) -> std::string {
        IStr64 n;
        istr_set(n, name);
        void* jp = g_findJumpPoint(2, &n);
        if (!jp) {
          ok2 = false;
          return json_str("no server jump point named " + name + " in the loaded layers (jump list)");
        }
        TH_LOG("nav: useJumpPoint(%s, reload=%d, debug=1)", name.c_str(), reload);
        g_useJumpPoint(jp, reload, true);
        if (!reload) {
          g_lastJp = jp;
          bool pending = jump_pending();
          std::lock_guard lk(g_smu);
          g_snap.jumping = pending;
        }
        return "{\"jump\":" + json_str(name) + ",\"reload\":" + (reload ? "true" : "false") + "}";
      },
      ok);
  if (ok) state_loading();  // before the caller's next `lab wait in-world` can look
  return r;
}

// Wait (lab thread) until the jump's streaming wait is over; false on timeout.
bool wait_jump_done(DWORD timeout_ms) {
  DWORD t0 = GetTickCount();
  for (;;) {
    {
      std::lock_guard lk(g_smu);
      if (!g_snap.jumping && g_snap.player && !g_snap.loading) return true;
    }
    if (GetTickCount() - t0 > timeout_ms) return false;
    Sleep(50);
  }
}

std::string verb_jump(const std::vector<std::string>& a, bool& ok) {
  if (!g_findJumpPoint || !g_useJumpPoint || !g_jpArray) {
    ok = false;
    return json_str("jump points are disabled (signature mismatch, see threshold.log)");
  }
  if (a.empty() || a[0] == "list" || a[0] == "all") {
    bool all = !a.empty() && a[0] == "all";
    return run_on_server([all](void*, bool&) { return jump_list(all); }, ok);
  }
  bool reload = a.size() > 1 && a[1] == "reload";
  bool wait = a.size() > 1 && a[1] == "wait";
  std::string r = do_jump(a[0], reload, ok);
  if (ok && wait && !wait_jump_done(120000)) {
    ok = false;
    return json_str("jumped, but the streaming wait did not finish within 120 s");
  }
  return r;
}

// Options after positional args: flag=0|1.
bool opt_flag(const std::vector<std::string>& a, size_t from, bool def) {
  for (size_t i = from; i < a.size(); i++)
    if (a[i].rfind("flag=", 0) == 0) return a[i].substr(5) == "1" || a[i].substr(5) == "true";
  return def;
}
std::vector<std::string> positional(const std::vector<std::string>& a) {
  std::vector<std::string> p;
  for (auto& s : a)
    if (s.find('=') == std::string::npos) p.push_back(s);
  return p;
}

std::string opt_str(const std::vector<std::string>& a, const char* key, const std::string& def) {
  std::string k = std::string(key) + "=";
  for (auto& s : a)
    if (s.rfind(k, 0) == 0) return s.substr(k.size());
  return def;
}

// The game's own level switch: startLoadingLevel(world, episode, jumppoint, false, true), as fast
// travel and the episode transitions call it (re/levels.md 5). An empty world means the current
// one (exe+0x1c8270 passes ""); we pass the loaded world when we know it.
std::string load_level(std::string world, std::string ep, const std::string& jp, bool save, bool& ok,
                       bool use_restart) {
  // Going back to the save's own checkpoint (the hub's Game_Entry_Point from game-start): restart
  // from the checkpoint instead. Same loading mode, but only after a restart does the opening
  // cinematic skip; after startLoadingLevel(hub, Game_Entry_Point) it played its 3.5 min with the
  // skip held, both the options byte and the direct server-side skip (live rayblaster r2).
  std::string sep, sjp;
  if (use_restart && !save && g_restartCheckpoint && !jp.empty() && save_point(sep, sjp) &&
      _stricmp(sep.c_str(), ep.c_str()) == 0 && _stricmp(sjp.c_str(), jp.c_str()) == 0) {
    std::string r = run_on_server(
        [=](void* ses, bool&) -> std::string {
          TH_LOG("nav: %s/%s is the save's checkpoint: requestRestartCheckpoint", ep.c_str(), jp.c_str());
          g_restartCheckpoint(ses);
          std::lock_guard lk(g_smu);
          g_snap.loading = true;
          g_snap.loadReq = GetTickCount();
          return "{\"episode\":" + json_str(ep) + ",\"jumppoint\":" + json_str(jp) + ",\"via\":\"restart\"}";
        },
        ok);
    if (ok) state_loading();
    return r;
  }
  if (!g_startLoadingLevel) {
    ok = false;
    return json_str("startLoadingLevel is disabled (signature mismatch)");
  }
  std::string r = run_on_server(
      [=](void* ses, bool&) -> std::string {
        std::string w = world, e = ep, cw, ce;
        level_info(cw, ce);
        if (w.empty()) w = cw;
        if (e.empty()) e = ce;
        IStr64 iw, ie, ij;
        istr_set(iw, w);
        istr_set(ie, e);
        istr_set(ij, jp);
        // The game's episode transition saves first (exe+0x1c8270: saveGame(server, false, true),
        // then startLoadingLevel), so the load (mode 4, "from the save") carries the story state.
        if (save && g_saveGame) {
          TH_LOG("nav: saveGame(server, 0, 1) before the load");
          g_saveGame(2, false, true);
        }
        TH_LOG("nav: startLoadingLevel(%s, %s, %s, 0, 1) from %s/%s", w.c_str(), e.c_str(), jp.c_str(), cw.c_str(),
               ce.c_str());
        g_startLoadingLevel(ses, &iw, &ie, &ij, false, true);
        {
          std::lock_guard lk(g_smu);
          g_snap.loading = true;  // until the hook sees the load; keeps `lab wait in-world` from returning early
          g_snap.loadReq = GetTickCount();
        }
        return "{\"world\":" + json_str(w) + ",\"episode\":" + json_str(e) + ",\"jumppoint\":" + json_str(jp) +
               ",\"from\":" + json_str(cw + "/" + ce) + ",\"saved\":" + (save && g_saveGame ? "true" : "false") + "}";
      },
      ok);
  if (ok) state_loading();
  return r;
}

// Wait (lab thread) until a level load we requested has been seen and has finished, then, if the
// player has no control (a cinematic), skip (optional) and wait for control. "" or what failed.
std::string wait_level(DWORD timeout_ms, bool skip) {
  DWORD t0 = GetTickCount();
  for (;;) {
    {
      std::lock_guard lk(g_smu);
      if (!g_snap.loadReq && !g_snap.loading && g_snap.player && GetTickCount() - g_snap.tick < 3000) break;
    }
    if (GetTickCount() - t0 > timeout_ms) return "the load did not finish in time";
    Sleep(100);
  }
  if (player_control() == 0 && skip && !g_skipUntil) skip_from_now(timeout_ms / 1000);
  for (;;) {
    if (player_control() != 0) return "";
    if (GetTickCount() - t0 > timeout_ms) {
      Cine c = cine_state();
      return std::string("the player has no control (") + (c.active ? "a timeline plays" : "no timeline") +
             (c.skippable ? ", skippable" : ", not skippable") +
             "; a black screen here is the story state: see docs/navigation.md)";
    }
    Sleep(100);
  }
}

// episode <name> [jumppoint] [wait|skip] [world=<w>] [save=0|1] [via=level|episode] [flag=0|1]
std::string verb_episode(const std::vector<std::string>& a, bool& ok) {
  std::vector<std::string> p;
  bool wait = false, skip = false;
  for (auto& s : positional(a)) {
    if (s == "wait") wait = true;
    else if (s == "skip") wait = skip = true;
    else p.push_back(s);
  }
  if (skip) skip_from_now(DWORD(std::strtoul(opt_str(a, "timeout", "180").c_str(), nullptr, 10)));
  if (p.empty()) {
    ok = false;
    return json_str("usage: episode <name> [jumppoint] [wait|skip] [world=<w>] [save=0|1] [via=level|episode]");
  }
  std::string ep = p[0], jp = p.size() > 1 ? p[1] : "";
  std::string via = opt_str(a, "via", "auto");
  if (via != "episode") {
    std::string r = load_level(opt_str(a, "world", ""), ep, jp, opt_str(a, "save", "0") == "1", ok, via == "auto");
    if (ok && wait) {
      std::string w = wait_level(DWORD(std::strtoul(opt_str(a, "timeout", "180").c_str(), nullptr, 10)) * 1000, skip);
      if (skip) g_skipUntil = 0;
      if (!w.empty()) {
        ok = false;
        return json_str("loaded, but " + w);
      }
    }
    return r;
  }
  if (!g_startLoadingEpisode) {
    ok = false;
    return json_str("episode loading is disabled (signature mismatch)");
  }
  bool flag = opt_flag(a, 0, false);
  return run_on_server(
      [=](void* ses, bool&) -> std::string {
        IStr64 e, j;
        istr_set(e, ep);
        istr_set(j, jp);
        TH_LOG("nav: startLoadingEpisode(%s, %s, %d)", ep.c_str(), jp.c_str(), flag);
        g_startLoadingEpisode(ses, &e, &j, flag);
        return "{\"episode\":" + json_str(ep) + ",\"jumppoint\":" + json_str(jp) + ",\"via\":\"episode\"}";
      },
      ok);
}

// reload [jumppoint] [via=level|episode] [flag=0|1]: the current episode again, from a jump point.
std::string verb_reload(const std::vector<std::string>& a, bool& ok) {
  auto p = positional(a);
  std::string jp = p.empty() ? "" : p[0];
  if (opt_str(a, "via", "level") != "episode") return load_level("", "", jp, opt_str(a, "save", "0") == "1", ok);
  if (!g_startReloading) {
    ok = false;
    return json_str("reload is disabled (signature mismatch)");
  }
  bool flag = opt_flag(a, 0, false);
  return run_on_server(
      [=](void* ses, bool&) -> std::string {
        IStr64 j;
        istr_set(j, jp);
        TH_LOG("nav: startReloadingEpisodeFromJumpPoint(%s, %d)", jp.c_str(), flag);
        g_startReloading(ses, &j, flag);
        return "{\"reload\":" + json_str(jp) + ",\"via\":\"episode\"}";
      },
      ok);
}

// save: a checkpoint save now (slot 0), as the episode transition does; for `quiver lab fixture save`.
std::string verb_save(const std::vector<std::string>&, bool& ok) {
  if (!g_saveGame) {
    ok = false;
    return json_str("saveGame is disabled (signature mismatch)");
  }
  return run_on_server(
      [](void*, bool&) -> std::string {
        TH_LOG("nav: saveGame(server, 0, 1)");
        g_saveGame(2, false, true);
        return json_str("save requested");
      },
      ok);
}

// skip [for=<s>] | skip status | skip off: skip skippable cinematics the game's own way, from now
// until none has played for 4 s after the first one (or for <s> seconds, default 30, if none comes).
std::string verb_skip(const std::vector<std::string>& a, bool& ok) {
  if (!g_optionsPtr || !g_timelineMgr) {
    ok = false;
    return json_str("skip is disabled (options or TimelineManager not found)");
  }
  bool status = !a.empty() && a[0] == "status";
  if (!a.empty() && a[0] == "off") g_skipUntil = 0;
  else if (!status) {
    g_skipCount = 0;
    DWORD secs = 30;
    for (auto& s : a)
      if (s.rfind("for=", 0) == 0) secs = std::strtoul(s.c_str() + 4, nullptr, 10);
    g_skipUntil = GetTickCount() + secs * 1000;
    TH_LOG("nav: skip cinematics for up to %lu s", (unsigned long)secs);
  }
  Cine c = cine_state();
  return std::string("{\"skipping\":") + (g_skipUntil ? "true" : "false") + ",\"active\":" + (c.active ? "true" : "false") +
         ",\"skippable\":" + (c.skippable ? "true" : "false") + ",\"skipped\":" + std::to_string(g_skipCount.load()) + "}";
}

std::string verb_restart(const std::vector<std::string>& a, bool& ok) {
  bool level = !a.empty() && a[0] == "level";
  Restart_t fn = level ? g_restartLevel : g_restartCheckpoint;
  if (!fn) {
    ok = false;
    return json_str("restart is disabled (signature mismatch)");
  }
  return run_on_server(
      [=](void* ses, bool&) -> std::string {
        TH_LOG("nav: requestRestart%s", level ? "Level" : "Checkpoint");
        fn(ses);
        return json_str(level ? "restart level" : "restart checkpoint");
      },
      ok);
}

// nav info | nav peek <addr> [len]: live RE helpers. addr = 0x.. | exe+0x.. | core+0x.. | rl+0x..
uintptr_t parse_addr(const std::string& s) {
  auto plus = s.find('+');
  uintptr_t base = 0;
  std::string off = s;
  if (plus != std::string::npos) {
    std::string m = s.substr(0, plus);
    off = s.substr(plus + 1);
    if (m == "exe") base = game_image().base;
    else if (m == "core") base = module_image(kCore).base;
    else if (m == "rl") base = module_image(kRl).base;
    else if (m == "opt" && g_optionsPtr) base = *reinterpret_cast<uintptr_t*>(g_optionsPtr);
    else if (m == "rv") base = g_renderView;
    else return 0;
  }
  return base + std::strtoull(off.c_str(), nullptr, 0);
}

std::string verb_nav(const std::vector<std::string>& a, bool& ok) {
  if (!a.empty() && a[0] == "peek" && a.size() >= 2) {
    uintptr_t p = parse_addr(a[1]);
    size_t n = a.size() > 2 ? std::strtoul(a[2].c_str(), nullptr, 0) : 64;
    if (n > 4096) n = 4096;
    if (a.size() > 3 && a[3] == "ptr") {  // dereference first
      if (!readable(reinterpret_cast<void*>(p), 8)) return ok = false, json_str("unreadable");
      p = *reinterpret_cast<uintptr_t*>(p);
    }
    if (!p || !readable(reinterpret_cast<void*>(p), n)) {
      ok = false;
      return json_str("unreadable");
    }
    std::string hex, txt;
    char b[4];
    for (size_t i = 0; i < n; i++) {
      uint8_t c = reinterpret_cast<uint8_t*>(p)[i];
      snprintf(b, sizeof b, "%02x", c);
      hex += b;
      txt += c >= 0x20 && c < 0x7f ? char(c) : '.';
    }
    char addr[32];
    snprintf(addr, sizeof addr, "0x%llx", (unsigned long long)p);
    return "{\"addr\":" + json_str(addr) + ",\"hex\":" + json_str(hex) + ",\"text\":" + json_str(txt) + "}";
  }
  if (!a.empty() && a[0] == "poke" && a.size() >= 3) {  // nav poke <addr> <hex bytes>: live RE only
    uintptr_t p = parse_addr(a[1]);
    std::string hx = a[2];
    if (hx.size() % 2 || hx.empty() || hx.size() > 64 || !readable(reinterpret_cast<void*>(p), hx.size() / 2)) {
      ok = false;
      return json_str("bad address or bytes");
    }
    for (size_t i = 0; i < hx.size() / 2; i++)
      reinterpret_cast<volatile uint8_t*>(p)[i] = uint8_t(std::strtoul(hx.substr(i * 2, 2).c_str(), nullptr, 16));
    TH_LOG("nav: poke %s %s", a[1].c_str(), hx.c_str());
    return json_str("ok");
  }
  if (!a.empty() && a[0] == "action" && a.size() >= 2) {  // nav action <id>: the input action's name
    if (!g_getActionName || !g_inputMgr || !*g_inputMgr) return ok = false, json_str("no input manager");
    alignas(16) uint8_t buf[0x100]{};
    g_getActionName(*g_inputMgr, buf, int(std::strtol(a[1].c_str(), nullptr, 0)));
    return json_str(rstr(buf));
  }
  // info
  Snap s;
  {
    std::lock_guard lk(g_smu);
    s = g_snap;
  }
  uintptr_t opt = g_optionsPtr ? *reinterpret_cast<uintptr_t*>(g_optionsPtr) : 0;
  std::string o = "{\"enabled\":" + std::string(g_ok ? "true" : "false") +
                  ",\"updates\":" + std::to_string(s.updates) + ",\"update_age_ms\":" +
                  std::to_string(s.tick ? GetTickCount() - s.tick : 0) + ",\"player\":" + (s.player ? "true" : "false") +
                  ",\"loading\":" + (s.loading ? "true" : "false");
  if (opt)
    o += ",\"opt_keep_ticking\":" + std::to_string(*reinterpret_cast<uint8_t*>(opt + 0x406)) +
         ",\"opt_developer\":" + std::to_string(*reinterpret_cast<uint8_t*>(opt + 0x130));
  if (g_jpCount) o += ",\"jump_points\":" + std::to_string(*reinterpret_cast<uint32_t*>(g_jpCount));
  if (opt) o += ",\"opt_skip_timelines\":" + std::to_string(*reinterpret_cast<uint8_t*>(opt + 0x13a));
  std::string lw, le;
  if (level_info(lw, le)) o += ",\"level\":" + json_str(lw + "/" + le);
  HWND fg = GetForegroundWindow();
  DWORD pid = 0;
  if (fg) GetWindowThreadProcessId(fg, &pid);
  o += ",\"foreground_is_game\":" + std::string(pid == GetCurrentProcessId() ? "true" : "false");
  return o + "}";
}

// ---- state + focus thread ---------------------------------------------------------------------

DWORD WINAPI monitor(void*) {
  std::string last;
  DWORD last_write = 0;
  bool focus_done = false;
  bool skip_held = false, prev_skippable = false, skip_seen = false;
  uint8_t skip_saved = 0;
  DWORD quiet_since = 0;
  Cine prev_c;
  int prev_pc = -2, prev_mo = -2;
  for (;;) {
    Sleep(250);
    if (!focus_done && g_optionsPtr) {
      uintptr_t opt = *reinterpret_cast<uintptr_t*>(g_optionsPtr);
      if (opt) {
        auto* b = reinterpret_cast<volatile uint8_t*>(opt + 0x406);
        uint8_t was = *b;
        if (cfg_bool("nav.keep_ticking", true)) *b = 1;
        TH_LOG("nav: options at %p, keep-ticking-unfocused (+0x406) was %u, now %u", (void*)opt, was, *b);
        focus_done = true;
      }
    }
    DWORD now = GetTickCount();
    Cine c = cine_state();
    int pc = player_control(), mo = menu_on();
    // skip: hold options +0x13a while skippable timelines keep coming, then restore it.
    if (g_optionsPtr) {
      uintptr_t opt = *reinterpret_cast<uintptr_t*>(g_optionsPtr);
      auto* b = opt ? reinterpret_cast<volatile uint8_t*>(opt + 0x13a) : nullptr;
      bool always = cfg_bool("nav.skip_cinematics", false);
      DWORD until = g_skipUntil;
      if (b && (until || always)) {
        if (!skip_held) {
          skip_seen = false;
          skip_saved = *b;
          skip_held = true;
          quiet_since = now;
          TH_LOG("nav: skip on (options +0x13a was %u; timeline active %d skippable %d)", skip_saved, c.active, c.skippable);
        }
        *b = 1;
        if (prev_skippable && !c.skippable) g_skipCount++;
        if (c.skippable) quiet_since = now;
        // Done once skippable timelines have stopped coming for 4 s (after at least one), or at
        // the deadline.
        if (c.skippable) skip_seen = true;
        if (until && !always && ((skip_seen && now - quiet_since > 4000) || int(now - until) > 0)) g_skipUntil = 0;
      } else if (b && skip_held) {
        *b = skip_saved;
        skip_held = false;
        TH_LOG("nav: skip off after %d skipped timeline(s) (options +0x13a back to %u)", g_skipCount.load(), skip_saved);
      }
    }
    prev_skippable = c.skippable;
    if (c.active != prev_c.active || c.skippable != prev_c.skippable || c.letterbox != prev_c.letterbox || pc != prev_pc ||
        mo != prev_mo)
      TH_LOG("nav: timeline active %d skippable %d letterbox %d, player control %d, menu %d", c.active, c.skippable,
             c.letterbox, pc, mo);
    prev_c = c;
    prev_pc = pc;
    prev_mo = mo;
    // in-world: a player, the engine not loading and the player in control; cinematic: the same,
    // but the player has no control (InputManager::isPlayerControlEnabled false: the opening
    // cinematic, scripted scenes). Not "a timeline plays": ambient timelines run in gameplay too.
    // loading: a session ticks but is loading (or has no player yet); menu: no session ticks.
    std::unique_lock slk(g_statemu);
    std::string st;
    {
      std::lock_guard lk(g_smu);
      bool session = g_snap.tick && now - g_snap.tick < 3000;
      st = !session ? "menu" : g_snap.player && !g_snap.loading && !g_snap.jumping ? "in-world" : "loading";
    }
    if (st == "in-world" && cfg_bool("nav.cinematic_state", true) && mo != 1 && pc == 0) st = "cinematic";
    if (st != last || now - last_write > 2000) {
      if (st != last) TH_LOG("nav: lab state %s", st.c_str());
      lab_state(st);
      last = st;
      last_write = now;
    }
  }
}

}  // namespace

void nav_register() {
  if (!lab_active()) return;
  lab_verb("where", verb_where);
  lab_verb("teleport", verb_teleport);
  lab_verb("jump", verb_jump);
  lab_verb("episode", verb_episode);
  lab_verb("reload", verb_reload);
  lab_verb("restart", verb_restart);
  lab_verb("skip", verb_skip);
  lab_verb("save", verb_save);
  lab_verb("nav", verb_nav);
}

void nav_start() {
  if (!lab_active()) return;
  // Server session and avatar (re/levels.md 2, 3).
  void* upd = nullptr;
  bool core = resolve_export(upd, kCore, "?update@ServerGameSession@coregame@@QEAAXM@Z",
                             "48 89 5c 24 08 57 48 83 ec 30 48 8b 99 70 06 00", "ServerGameSession::update") &&
              resolve_export(g_getAvatar, kCore, "?getSinglePlayerAvatarObject@ServerGameSession@coregame@@QEAAPEAVEntityState@2@XZ",
                             "83 b9 a8 00 00 00 00 75 19 83 b9 78 06 00 00 01", "getSinglePlayerAvatarObject") &&
              resolve_export(g_xfTypeId, kCore, "?getTypeIDStatic@TransformComponentState@coregame@@SAIXZ",
                             "40 53 48 83 ec 30 48 c7 44 24 20 fe ff ff ff 8b", "TransformComponentState::getTypeIDStatic") &&
              resolve_export(g_getComponent, kRl, "?getComponentByTypeId@GameObjectState@r@@QEBAPEAVComponentStateBase@2@I@Z",
                             "8b 41 70 44 0f b7 49 62 49 c1 e1 09 4c 03 0d", "GameObjectState::getComponentByTypeId") &&
              resolve_export(g_handleTeleport, kCore, "?handleTeleport@TransformComponentState@coregame@@QEAAXMMMM@Z",
                             "48 8b c4 53 48 81 ec 90 00 00 00 f3 0f 10 ac 24", "TransformComponentState::handleTeleport") &&
              resolve_export(g_teleport, kCore, "?teleport@TransformComponentState@coregame@@QEAAXVSIMDTransform@m@@0@Z",
                             "48 89 5c 24 08 57 48 83 ec 40 0f 28 02 49 8b f8", "TransformComponentState::teleport");
  // Episodes (optional).
  resolve_export(g_startLoadingEpisode, kCore,
                 "?startLoadingEpisode@ServerGameSession@coregame@@QEAAXAEBV?$InplaceString@$0EA@@r@@0_N@Z",
                 "40 55 53 56 57 48 8d 6c 24 e8 48 81 ec 18 01 00", "ServerGameSession::startLoadingEpisode");
  resolve_export(g_startReloading, kCore,
                 "?startReloadingEpisodeFromJumpPoint@ServerGameSession@coregame@@QEAAXAEBV?$InplaceString@$0EA@@r@@_N@Z",
                 "40 57 48 81 ec f0 00 00 00 48 c7 44 24 28 fe ff", "ServerGameSession::startReloadingEpisodeFromJumpPoint");
  resolve_export(g_startLoadingLevel, kCore,
                 "?startLoadingLevel@ServerGameSession@coregame@@QEAAXAEBV?$InplaceString@$0EA@@r@@00_N1@Z",
                 "40 53 55 56 57 41 56 41 57 48 81 ec 98 01 00 00", "ServerGameSession::startLoadingLevel");
  g_serverBase = static_cast<void**>(
      module_export(kCore, "?s_pGameServerBase@GameHelper@coregame@@2PEAVGameServerBase@2@EA", "nav"));
  // Input: player control and menu flags (exports; input+0x8f00 `cmp byte [rcx+0x7e],0`, ...).
  g_inputMgr = static_cast<void**>(module_export(kInput, "?sm_pInstance@InputManager@input@@0PEAV12@EA", "nav"));
  resolve_export(g_isPlayerControl, kInput, "?isPlayerControlEnabled@InputManager@input@@QEAA_NXZ",
                 "80 79 7e 00 74 09 80 79 7d 00 74 03 b0 01 c3", "InputManager::isPlayerControlEnabled");
  resolve_export(g_isMenuOn, kInput, "?isMenuOn@InputManager@input@@QEAA_NXZ", "0f b6 41 74 c3",
                 "InputManager::isMenuOn");
  g_getActionName = reinterpret_cast<GetActionName_t>(
      module_export(kInput, "?getActionName@InputManager@input@@QEAA?AV?$InplaceString@$0IA@@r@@H@Z", "nav"));
  // TimelineManager*: `mov rbx,[rip+X]; test rbx,rbx` at exe+0x5f5c64 (the HUD's skip callout).
  if (sig_match(reinterpret_cast<void*>(game_image().at(0x5f5c64)), "48 8b 1d ?? ?? ?? ?? 48 85 db") &&
      rip_target(0x5f5c64, 3, 7) == 0x1249540 &&
      sig_match(reinterpret_cast<void*>(game_image().at(0x5b6209)), "48 8b 05 ?? ?? ?? ?? 80 b8 3a 01 00 00"))
    g_timelineMgr = game_image().at(0x1249540);
  else
    TH_LOG("nav: TimelineManager reference not found, no cinematic state or skip");
  // The skip itself and the "playing" state ID (operand of the lea at exe+0x5b6283).
  if (resolve_exe(g_skipTimeline, 0x5aa840, "40 53 48 83 ec 20 83 b9 f8 01 00 00 00 48 8b d9 c6 81 c1 01 00 00 01",
                  "timeline skip") &&
      sig_match(reinterpret_cast<void*>(game_image().at(0x5b6283)), "48 8d 15") && rip_target(0x5b6283, 3, 7) == 0x12cbe68)
    g_tlPlaying = game_image().at(0x12cbe68);
  else
    g_skipTimeline = nullptr;
  g_idEq = reinterpret_cast<IdEq_t>(module_export(kRl, "??8ID@StateMachineDefinition@r@@QEBA_NAEBU012@@Z", "nav"));
  resolve_export(g_saveGame, kCore, "?saveGame@GameHelper@coregame@@SAXW4NetworkRole@net@@_N1@Z",
                 "48 89 5c 24 08 48 89 6c 24 10 48 89 74 24 18 57", "GameHelper::saveGame");
  resolve_export(g_restartCheckpoint, kCore, "?requestRestartCheckpoint@ServerGameSession@coregame@@QEAAXXZ",
                 "40 53 48 83 ec 20 48 8b d9 48 8d 15", "ServerGameSession::requestRestartCheckpoint");
  resolve_export(g_restartLevel, kCore, "?requestRestartLevel@ServerGameSession@coregame@@QEAAXXZ",
                 "40 53 48 83 ec 20 48 8b d9 48 8d 15", "ServerGameSession::requestRestartLevel");
  // Jump points (exe, by RVA; the registry addresses are decoded from findJumpPoint's own code).
  if (resolve_exe(g_findJumpPoint, 0x481c60, "40 57 41 56 41 57 48 83 ec 30 48 c7 44 24 20 fe ff ff ff 48 89 5c 24 50 48 89 6c 24 58 48 89 74 24 68 48 8b ea 44 8b f1 4c 8d 3d",
                  "findJumpPoint") &&
      resolve_exe(g_useJumpPoint, 0x4823c0, "48 8b c4 55 57 41 54 41 56 41 57 48 8d 6c 24 a0", "useJumpPoint")) {
    uintptr_t lock = rip_target(0x481c88, 3, 7), arr = rip_target(0x481cb9, 3, 7), cnt = rip_target(0x481cc0, 2, 6);
    if (lock == 0x1240460 && arr == 0x12c4e48 && cnt == 0x12c4e50 &&
        sig_match(reinterpret_cast<void*>(game_image().at(0x481cb9)), "48 8b 1d") &&
        sig_match(reinterpret_cast<void*>(game_image().at(0x481cc0)), "8b 05")) {
      g_jpLock = game_image().at(lock);
      g_jpArray = game_image().at(arr);
      g_jpCount = game_image().at(cnt);
    } else {
      TH_LOG("nav: jump point registry operands differ (lock %llx arr %llx cnt %llx), jump disabled",
             (unsigned long long)lock, (unsigned long long)arr, (unsigned long long)cnt);
      g_findJumpPoint = nullptr;
      g_useJumpPoint = nullptr;
    }
  }
  // Options pointer: the developer-mode lambda `mov rax,[rip+X]; movzx eax,byte [rax+0x130]; ret`.
  if (sig_match(reinterpret_cast<void*>(game_image().at(0x367f60)), "48 8b 05 ?? ?? ?? ?? 0f b6 80 30 01 00 00 c3") &&
      rip_target(0x367f60, 3, 7) == 0x129a588)
    g_optionsPtr = game_image().at(0x129a588);
  else
    TH_LOG("nav: options pointer not found, keep-ticking-unfocused not set");
  // Save manager pointer: `mov rax,[rip+X]` at exe+0x950a4 (a reader of it).
  if (sig_match(reinterpret_cast<void*>(game_image().at(0x950a4)), "48 8b 05") && rip_target(0x950a4, 3, 7) == 0x129a6e0)
    g_saveMgr = game_image().at(0x129a6e0);
  else
    TH_LOG("nav: save manager reference not found, no save info in where");
  g_onIsLoading = reinterpret_cast<uintptr_t>(module_export(kCore, "?onIsLoading@coregame@@3V?$function@$$A6A_NXZ@std@@A", "nav"));
  // Camera: g_renderView, filled by the camera post-update exe+0x383c60 (re/stereo.md 2.1).
  if (sig_match(reinterpret_cast<void*>(game_image().at(0x383c60)),
                "48 8b c4 55 48 8d 68 a1 48 81 ec c0 00 00 00 48 c7 45 b7 fe ff ff ff 48 89 58 08 48 89 70 10 48"))
    g_renderView = game_image().at(0x12b9f50);
  else
    TH_LOG("nav: camera publish signature mismatch, no camera angles");

  if (core && hook_fn(upd, (void*)hk_update, (void**)&o_update, "ServerGameSession::update")) {
    g_ok = true;
    TH_LOG("nav: ready (episodes %s, jump points %s)", g_startLoadingEpisode ? "on" : "off",
           g_findJumpPoint ? "on" : "off");
  } else {
    TH_LOG("nav: disabled");
  }
  CloseHandle(CreateThread(nullptr, 0, monitor, nullptr, 0, nullptr));
}

}  // namespace th
