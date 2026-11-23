#include "features/fg/latency.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <vector>

#include "core/log.h"
#include "features/fg/common.h"
#include "features/fg/engine.h"

namespace th::fg {
namespace {

constexpr uint32_t kFrames = 16384;  // a capture holds at most this many rendered frames
constexpr uint32_t kQ = 3;           // queries per frame: after DLSS SR, frame done, DLSS-G done

int64_t qpc_ns(int64_t ticks) {
  static const int64_t f = [] {
    LARGE_INTEGER v;
    QueryPerformanceFrequency(&v);
    return v.QuadPart;
  }();
  return (ticks / f) * 1000000000LL + (ticks % f) * 1000000000LL / f;
}
int64_t now_ns() {
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return qpc_ns(c.QuadPart);
}

struct Row {
  uint64_t frame = 0;
  bool eval = false, begin = false, end = false;
  int64_t cpu_eval_ns = 0, arrive_ns = 0;
  float basis[9] = {};  // right, up, forward
  unsigned n_gen = 0;
  int64_t first_present_ns = 0, real_present_ns = 0;
};

struct Lat {
  std::mutex m;
  std::atomic<bool> on{false};
  ID3D12Device* dev = nullptr;
  ID3D12CommandQueue* q = nullptr;
  ID3D12QueryHeap* heap = nullptr;
  ID3D12Resource* rb = nullptr;
  uint64_t gpu_freq = 0, gpu0 = 0;
  int64_t cpu0_ns = 0;
  uint64_t first = 0;  // first frame of the capture
  bool have_first = false;
  std::vector<Row> rows;
} L;

Row* row(uint64_t frame) {
  if (!L.have_first) {
    L.first = frame;
    L.have_first = true;
  }
  if (frame < L.first || frame - L.first >= kFrames) return nullptr;
  Row& r = L.rows[size_t(frame - L.first)];
  r.frame = frame;
  return &r;
}

}  // namespace

bool lat_on() { return L.on.load(std::memory_order_relaxed); }

std::string lat_start(ID3D12Device* dev, ID3D12CommandQueue* q) {
  std::lock_guard lk(L.m);
  if (L.on) return "latency logging already on";
  if (!dev || !q) return "no device yet";
  L.dev = dev;
  L.q = q;
  if (!L.heap) {
    D3D12_QUERY_HEAP_DESC hd{};
    hd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    hd.Count = kFrames * kQ;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = uint64_t(kFrames) * kQ * 8;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(dev->CreateQueryHeap(&hd, __uuidof(ID3D12QueryHeap), (void**)&L.heap)) ||
        FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            __uuidof(ID3D12Resource), (void**)&L.rb)))
      return "query heap / readback buffer failed";
  }
  UINT64 g = 0, c = 0;
  if (FAILED(q->GetTimestampFrequency(&L.gpu_freq)) || FAILED(q->GetClockCalibration(&g, &c))) return "no GPU clock";
  L.gpu0 = g;
  L.cpu0_ns = qpc_ns(int64_t(c));
  L.rows.assign(kFrames, Row{});
  L.have_first = false;
  L.on = true;
  TH_LOG("fg: latency logging on (GPU timestamps %llu Hz)", (unsigned long long)L.gpu_freq);
  return "latency logging on";
}

void lat_eval(ID3D12GraphicsCommandList* cl, const Camera& cam, uint64_t frame) {
  if (!lat_on()) return;
  std::lock_guard lk(L.m);
  Row* r = row(frame);
  if (!r) return;
  r->eval = cam.ok;
  r->cpu_eval_ns = now_ns();
  for (int i = 0; i < 3; ++i) {
    r->basis[i] = cam.right[i];
    r->basis[3 + i] = cam.up[i];
    r->basis[6 + i] = cam.fwd[i];
  }
  cl->EndQuery(L.heap, D3D12_QUERY_TYPE_TIMESTAMP, UINT((frame % kFrames) * kQ));
}

void lat_begin(ID3D12GraphicsCommandList* cl, uint64_t frame) {
  if (!lat_on()) return;
  std::lock_guard lk(L.m);
  Row* r = row(frame);
  if (!r) return;
  r->begin = true;
  cl->EndQuery(L.heap, D3D12_QUERY_TYPE_TIMESTAMP, UINT((frame % kFrames) * kQ + 1));
}

void lat_end(ID3D12GraphicsCommandList* cl, uint64_t frame, unsigned n_gen, int64_t arrive_us) {
  if (!lat_on()) return;
  std::lock_guard lk(L.m);
  Row* r = row(frame);
  if (!r) return;
  r->end = true;
  r->n_gen = n_gen;
  r->arrive_ns = arrive_us * 1000;
  const UINT base = UINT((frame % kFrames) * kQ);
  cl->EndQuery(L.heap, D3D12_QUERY_TYPE_TIMESTAMP, base + 2);
  // The DLSS SR query was ended on the game's list earlier on the same queue; resolve all three.
  UINT first = r->eval ? base : base + 1;
  cl->ResolveQueryData(L.heap, D3D12_QUERY_TYPE_TIMESTAMP, first, base + kQ - first, L.rb, uint64_t(first) * 8);
}

void lat_present(uint64_t frame, char kind) {
  if (!lat_on()) return;
  int64_t t = now_ns();
  std::lock_guard lk(L.m);
  if (!L.have_first || frame < L.first || frame - L.first >= kFrames) return;
  Row& r = L.rows[size_t(frame - L.first)];
  if (!r.first_present_ns) r.first_present_ns = t;
  if (kind != 'G') r.real_present_ns = t;
}

std::string lat_stop() {
  if (!lat_on()) return "latency logging is off";
  L.on = false;
  Sleep(300);  // the last frames' GPU work and presents
  std::lock_guard lk(L.m);
  wchar_t dir[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"QUIVER_LAB_RUN_DIR", dir, MAX_PATH);
  std::wstring path = (n && n < MAX_PATH ? std::wstring(dir) : std::wstring(L".")) + L"\\latency.csv";
  FILE* f = _wfopen(path.c_str(), L"w");
  if (!f) return "cannot write latency.csv";
  void* p = nullptr;
  D3D12_RANGE rr{0, SIZE_T(kFrames) * kQ * 8};
  const uint64_t* ts = SUCCEEDED(L.rb->Map(0, &rr, &p)) ? static_cast<const uint64_t*>(p) : nullptr;
  auto gpu_ns = [&](uint64_t g) -> int64_t {
    return g ? L.cpu0_ns + int64_t((int64_t(g) - int64_t(L.gpu0)) * 1e9 / double(L.gpu_freq)) : 0;
  };
  fprintf(f, "frame,cpu_eval_ns,gpu_eval_ns,gpu_frame_ns,gpu_fg_ns,arrive_ns,first_present_ns,real_present_ns,n_gen,"
             "r00,r01,r02,r10,r11,r12,r20,r21,r22\n");
  size_t rows = 0;
  for (const Row& r : L.rows) {
    if (!r.end) continue;
    const uint64_t* t = ts ? ts + (r.frame % kFrames) * kQ : nullptr;
    fprintf(f, "%llu,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%u", (unsigned long long)r.frame, (long long)r.cpu_eval_ns,
            (long long)(t && r.eval ? gpu_ns(t[0]) : 0), (long long)(t ? gpu_ns(t[1]) : 0), (long long)(t ? gpu_ns(t[2]) : 0),
            (long long)r.arrive_ns, (long long)r.first_present_ns, (long long)r.real_present_ns, r.n_gen);
    for (float b : r.basis) fprintf(f, ",%.6f", b);
    fprintf(f, "\n");
    rows++;
  }
  if (ts) {
    D3D12_RANGE none{0, 0};
    L.rb->Unmap(0, &none);
  }
  fclose(f);
  TH_LOG("fg: latency logging off: %zu frames written to latency.csv", rows);
  return "latency.csv: " + std::to_string(rows) + " frames";
}

}  // namespace th::fg
