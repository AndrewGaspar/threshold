#pragma once
// Shared helpers of the frame-generation files (src/features/fg/): time, textures, barriers, our
// command-list rings on the game's queue.
#include <windows.h>
#include <d3d12.h>

#include <cstdint>

namespace th::fg {

inline int64_t now_us() {
  static LARGE_INTEGER f = [] {
    LARGE_INTEGER v;
    QueryPerformanceFrequency(&v);
    return v;
  }();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return c.QuadPart * 1000000 / f.QuadPart;
}

template <typename T>
void safe_release(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

struct Tex {
  ID3D12Resource* r = nullptr;
  D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_COMMON;
};

inline void barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES from,
                    D3D12_RESOURCE_STATES to) {
  if (from == to || !r) return;
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = r;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = from;
  b.Transition.StateAfter = to;
  cl->ResourceBarrier(1, &b);
}
inline void to(ID3D12GraphicsCommandList* cl, Tex& t, D3D12_RESOURCE_STATES s) {
  barrier(cl, t.r, t.st, s);
  t.st = s;
}

bool make_tex(ID3D12Device* dev, Tex& t, D3D12_RESOURCE_DESC d, D3D12_RESOURCE_STATES st, const wchar_t* name);

// A single-threaded ring of command lists with its own fence.
struct Ring {
  static constexpr int kN = 24;
  ID3D12CommandAllocator* alloc[kN]{};
  ID3D12GraphicsCommandList* list[kN]{};
  uint64_t val[kN]{};
  ID3D12Fence* fence = nullptr;
  HANDLE ev = nullptr;
  uint64_t next = 0;
  int pos = 0;
  int cur = -1;

  bool init(ID3D12Device* dev, const wchar_t* name);
  void wait(uint64_t v);
  ID3D12GraphicsCommandList* begin();
  uint64_t submit(ID3D12CommandQueue* q);
  bool done(uint64_t v) const { return fence->GetCompletedValue() >= v; }
};

// Waits for `fence` to reach `v` with an event of the caller's (never the ring's shared one), at most
// `ms`. Re-checks the fence itself after every wake (a timed-out registration can fire later and wake a
// later wait early: Pharos's "event latch").
bool fence_wait(ID3D12Fence* fence, uint64_t v, HANDLE ev, DWORD ms);

}  // namespace th::fg
