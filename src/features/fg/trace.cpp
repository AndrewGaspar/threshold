#include "features/fg/trace.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#include "core/lab.h"
#include "core/log.h"
#include "features/fg/common.h"

namespace th::fg {
namespace {

struct Ev {
  int64_t arrive, call, ret;
  uint64_t frame;
  char kind;
  int32_t step_us, interval_us, shot;
  int32_t gi, gn;
};

struct Tracer {
  std::mutex m;
  std::atomic<bool> active{false};
  bool complete = false, writing = false;
  std::vector<Ev> ev;
  size_t max_ev = 0, max_shots = 0, shots = 0;
  ID3D12Resource* rb = nullptr;  // readback buffer: max_shots bands
  UINT w = 0, h = 0, y0 = 0;
  int want_y = -1, want_h = 0;  // band position/height asked for (-1: the default)
  UINT64 pitch = 0, slice = 0;
  DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
  ID3D12Fence* fence = nullptr;
  uint64_t last_v = 0;
  std::string last_result = "none";
  unsigned n_written = 0;
} T;

std::wstring out_dir() {
  wchar_t buf[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"QUIVER_LAB_RUN_DIR", buf, MAX_PATH);
  std::wstring d = n && n < MAX_PATH ? std::wstring(buf) : std::wstring(L".");
  return d + L"\\fgtrace";
}

// Format code for bands.bin: 0 RGBA8, 1 BGRA8, 2 R10G10B10A2 (little-endian u32: R bits 0-9).
uint32_t fmt_code(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return 1;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return 2;
    default: return 0;
  }
}

void write_out() {
  // The present thread no longer touches T (complete); wait for the last band copy on the GPU.
  HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  fence_wait(T.fence, T.last_v, e, 5000);
  CloseHandle(e);
  std::wstring dir = out_dir();
  CreateDirectoryW(dir.c_str(), nullptr);
  if (FILE* f = _wfopen((dir + L"\\presents.csv").c_str(), L"w")) {
    fprintf(f, "kind,frame,arrive_us,call_us,ret_us,half_us,interval_us,shot,gi,gn\n");
    for (const Ev& e : T.ev)
      fprintf(f, "%c,%llu,%lld,%lld,%lld,%d,%d,%d,%d,%d\n", e.kind, (unsigned long long)e.frame, (long long)e.arrive,
              (long long)e.call, (long long)e.ret, e.step_us, e.interval_us, e.shot, e.gi, e.gn);
    fclose(f);
  }
  if (FILE* f = _wfopen((dir + L"\\bands.bin").c_str(), L"wb")) {
    uint32_t hdr[5] = {0x42474641u, T.w, T.h, uint32_t(T.shots), fmt_code(T.fmt)};
    fwrite(hdr, sizeof(hdr), 1, f);
    void* p = nullptr;
    D3D12_RANGE rr{0, SIZE_T(T.slice * T.shots)};
    if (T.rb && SUCCEEDED(T.rb->Map(0, &rr, &p)) && p) {
      for (size_t i = 0; i < T.shots; ++i)
        for (UINT y = 0; y < T.h; ++y)
          fwrite(static_cast<const uint8_t*>(p) + i * T.slice + y * T.pitch, 1, size_t(T.w) * 4, f);
      D3D12_RANGE none{0, 0};
      T.rb->Unmap(0, &none);
    }
    fclose(f);
  }
  TH_LOG("fg: trace written (%zu presents, %zu band shots %ux%u at y=%u) to fgtrace", T.ev.size(), T.shots, T.w, T.h,
         T.y0);
  std::lock_guard lk(T.m);
  safe_release(T.rb);
  T.active = false;
  T.complete = false;
  T.writing = false;
  T.n_written++;
  T.last_result = "written";
}

}  // namespace

std::string trace_arm(unsigned presents, unsigned shots, int y, int h) {
  std::lock_guard lk(T.m);
  if (T.active) return "a trace is already running";
  T.ev.clear();
  T.max_ev = std::max(presents, 8u);
  T.ev.reserve(T.max_ev);
  T.max_shots = std::min<size_t>(shots, T.max_ev);
  T.shots = 0;
  T.want_y = y;
  T.want_h = h;
  T.complete = false;
  safe_release(T.rb);  // created on the first traced present (needs the device and the size)
  T.last_result = "running";
  T.active = true;
  TH_LOG("fg: trace armed (%zu presents, %zu band shots)", T.max_ev, T.max_shots);
  return "trace armed: " + std::to_string(T.max_ev) + " presents, " + std::to_string(T.max_shots) + " shots";
}

std::string trace_status_json() {
  std::lock_guard lk(T.m);
  return "{\"state\":" + json_str(T.last_result) + ",\"events\":" + std::to_string(T.ev.size()) +
         ",\"shots\":" + std::to_string(T.shots) + ",\"written\":" + std::to_string(T.n_written) + "}";
}

int trace_copy(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, ID3D12Resource* src, DXGI_FORMAT fmt, char kind,
               uint64_t frame, int64_t arrive_us, int64_t step_us, int64_t interval_us, int gi, int gn) {
  if (!T.active.load(std::memory_order_relaxed)) return -1;
  std::lock_guard lk(T.m);
  if (T.complete || T.ev.size() >= T.max_ev) return -1;
  Ev e{arrive_us, 0, 0, frame, kind, int32_t(step_us), int32_t(interval_us), -1, gi, gn};
  if (T.shots < T.max_shots && src) {
    if (!T.rb) {
      D3D12_RESOURCE_DESC sd = src->GetDesc();
      T.w = UINT(sd.Width);
      T.h = T.want_h > 0 ? std::min<UINT>(UINT(T.want_h), sd.Height) : std::min<UINT>(sd.Height / 16, 64);
      // Default: above the player character, below most of the ceiling.
      T.y0 = T.want_y >= 0 ? std::min<UINT>(UINT(T.want_y), sd.Height - T.h) : sd.Height * 9 / 32;
      T.fmt = fmt;
      D3D12_RESOURCE_DESC bd = sd;
      bd.Height = T.h;
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
      UINT64 total = 0;
      dev->GetCopyableFootprints(&bd, 0, 1, 0, &fp, nullptr, nullptr, &total);
      T.pitch = fp.Footprint.RowPitch;
      T.slice = (total + 511) & ~UINT64(511);
      D3D12_HEAP_PROPERTIES hp{};
      hp.Type = D3D12_HEAP_TYPE_READBACK;
      D3D12_RESOURCE_DESC rd{};
      rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      rd.Width = T.slice * T.max_shots;
      rd.Height = 1;
      rd.DepthOrArraySize = 1;
      rd.MipLevels = 1;
      rd.SampleDesc.Count = 1;
      rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              __uuidof(ID3D12Resource), (void**)&T.rb))) {
        TH_LOG("fg: trace readback buffer (%llu bytes) failed: timestamps only", (unsigned long long)rd.Width);
        T.rb = nullptr;
        T.max_shots = 0;
      }
    }
    if (T.rb) {
      D3D12_TEXTURE_COPY_LOCATION dst{}, s{};
      dst.pResource = T.rb;
      dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      dst.PlacedFootprint.Offset = T.slice * T.shots;
      dst.PlacedFootprint.Footprint.Format = fmt;
      dst.PlacedFootprint.Footprint.Width = T.w;
      dst.PlacedFootprint.Footprint.Height = T.h;
      dst.PlacedFootprint.Footprint.Depth = 1;
      dst.PlacedFootprint.Footprint.RowPitch = UINT(T.pitch);
      s.pResource = src;
      s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      s.SubresourceIndex = 0;
      D3D12_BOX box{0, T.y0, 0, T.w, T.y0 + T.h, 1};
      cl->CopyTextureRegion(&dst, 0, 0, 0, &s, &box);
      e.shot = int32_t(T.shots++);
    }
  }
  T.ev.push_back(e);
  return int(T.ev.size() - 1);
}

void trace_call(int idx) {
  if (idx < 0) return;
  std::lock_guard lk(T.m);
  T.ev[size_t(idx)].call = now_us();
}

void trace_ret(int idx, ID3D12Fence* f, uint64_t v) {
  if (idx < 0) return;
  bool done = false;
  {
    std::lock_guard lk(T.m);
    T.ev[size_t(idx)].ret = now_us();
    if (T.ev[size_t(idx)].shot >= 0) T.fence = f, T.last_v = v;
    if (T.ev.size() >= T.max_ev && !T.complete) {
      T.complete = true;
      T.writing = true;
      done = true;
    }
  }
  if (done) std::thread(write_out).detach();
}

}  // namespace th::fg
