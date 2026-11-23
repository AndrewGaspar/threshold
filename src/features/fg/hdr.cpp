#include "features/fg/hdr.h"

#include <windows.h>
#include <d3dcompiler.h>

#include <cstring>

#include "core/log.h"

namespace th::fg {
namespace {

// ST 2084 absolute luminance, BT.709 <-> BT.2020 with D65 white (Pharos src/fg/color.cpp). scRGB 1.0
// = 80 cd/m2. Clip only to the representable BT.2020/PQ range after converting primaries; wide-gamut
// scRGB can have negative channels.
constexpr char kShader[] = R"(
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
static const float m1 = 2610./16384., m2 = 2523./32.;
static const float c1 = 3424./4096., c2 = 2413./128., c3 = 2392./128.;
[numthreads(8,8,1)]
void main(uint2 xy : SV_DispatchThreadID) {
    uint w,h; src.GetDimensions(w,h);
    if (xy.x >= w || xy.y >= h) return;
    float4 v = src.Load(int3(xy,0));
#ifdef ENCODE
    float3 light = mul(float3x3(.627404, .329283, .043313,
                                 .069097, .919540, .011362,
                                 .016391, .088013, .895595), v.rgb);
    float3 n = pow(saturate(light * (80./10000.)), m1);
    dst[xy] = float4(pow((c1+c2*n)/(1.+c3*n), m2), saturate(v.a));
#else
    float3 n = pow(saturate(v.rgb), 1./m2);
    float3 light = pow(max(n-c1,0.)/(c2-c3*n), 1./m1) * (10000./80.);
    dst[xy] = float4(mul(float3x3(1.660491,-.587641,-.072850,
                                -.124550,1.132900,-.008349,
                                -.018151,-.100579,1.118730), light), v.a);
#endif
}
)";

using D3DCompile_t = decltype(&D3DCompile);

ID3D12Device* g_dev = nullptr;
ID3D12RootSignature* g_root = nullptr;
ID3D12PipelineState* g_pso[2] = {};  // decode, encode
ID3D12DescriptorHeap* g_heap = nullptr;
UINT g_inc = 0;
ID3D12Resource* g_bound[kHdrSlots][2] = {};  // the views written into each slot (src, dst)
bool g_ok = false;

ID3DBlob* compile(D3DCompile_t fn, bool encode) {
  D3D_SHADER_MACRO defs[] = {{"ENCODE", "1"}, {nullptr, nullptr}};
  ID3DBlob *code = nullptr, *err = nullptr;
  HRESULT hr = fn(kShader, sizeof(kShader) - 1, encode ? "threshold_scrgb_to_pq" : "threshold_pq_to_scrgb",
                  encode ? defs : nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
  if (FAILED(hr)) {
    TH_LOG("fg: HDR shader compile (%s) = 0x%lx: %s", encode ? "encode" : "decode", hr,
           err ? static_cast<const char*>(err->GetBufferPointer()) : "");
    if (err) err->Release();
    return nullptr;
  }
  if (err) err->Release();
  return code;
}

}  // namespace

bool hdr_ready() { return g_ok; }

bool hdr_init(ID3D12Device* dev) {
  if (g_ok) return true;
  static bool tried = false;
  if (tried) return false;
  tried = true;
  g_dev = dev;
  // The game ships d3dcompiler_47.dll next to its exe.
  HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
  auto fn = dc ? reinterpret_cast<D3DCompile_t>(GetProcAddress(dc, "D3DCompile")) : nullptr;
  if (!fn) {
    TH_LOG("fg: HDR: d3dcompiler_47.dll / D3DCompile not found");
    return false;
  }
  D3D12_DESCRIPTOR_RANGE ranges[2]{};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[0].NumDescriptors = 1;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  ranges[1].NumDescriptors = 1;
  ranges[1].OffsetInDescriptorsFromTableStart = 1;
  D3D12_ROOT_PARAMETER param{};
  param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  param.DescriptorTable.NumDescriptorRanges = 2;
  param.DescriptorTable.pDescriptorRanges = ranges;
  param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC rs{};
  rs.NumParameters = 1;
  rs.pParameters = &param;
  auto ser = reinterpret_cast<decltype(&D3D12SerializeRootSignature)>(
      GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeRootSignature"));
  ID3DBlob *blob = nullptr, *err = nullptr;
  if (!ser || FAILED(ser(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
      FAILED(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature),
                                      (void**)&g_root))) {
    TH_LOG("fg: HDR: root signature failed");
    return false;
  }
  blob->Release();
  for (int e = 0; e < 2; ++e) {
    ID3DBlob* cs = compile(fn, e == 1);
    if (!cs) return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_root;
    pd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
    HRESULT hr = dev->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), (void**)&g_pso[e]);
    cs->Release();
    if (FAILED(hr)) {
      TH_LOG("fg: HDR: compute pipeline state = 0x%lx", hr);
      return false;
    }
  }
  D3D12_DESCRIPTOR_HEAP_DESC hd{};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors = kHdrSlots * 2;
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**)&g_heap))) {
    TH_LOG("fg: HDR: descriptor heap failed");
    return false;
  }
  g_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  g_ok = true;
  TH_LOG("fg: HDR conversion ready (scRGB <-> HDR10 PQ compute shaders)");
  return true;
}

void hdr_convert(ID3D12GraphicsCommandList* cl, ID3D12Resource* src, ID3D12Resource* dst, bool encode,
                 unsigned slot) {
  if (!g_ok || !src || !dst || slot >= kHdrSlots) return;
  D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_heap->GetCPUDescriptorHandleForHeapStart();
  cpu.ptr += SIZE_T(slot) * 2 * g_inc;
  if (g_bound[slot][0] != src || g_bound[slot][1] != dst) {
    // A slot is rewritten only when its textures change (re-created after a resize), when nothing that
    // used it is in flight any more.
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = src->GetDesc().Format;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    g_dev->CreateShaderResourceView(src, &sv, cpu);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.Format = dst->GetDesc().Format;
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu2 = cpu;
    cpu2.ptr += g_inc;
    g_dev->CreateUnorderedAccessView(dst, nullptr, &uv, cpu2);
    g_bound[slot][0] = src;
    g_bound[slot][1] = dst;
  }
  D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_heap->GetGPUDescriptorHandleForHeapStart();
  gpu.ptr += UINT64(slot) * 2 * g_inc;
  cl->SetDescriptorHeaps(1, &g_heap);
  cl->SetComputeRootSignature(g_root);
  cl->SetPipelineState(g_pso[encode ? 1 : 0]);
  cl->SetComputeRootDescriptorTable(0, gpu);
  D3D12_RESOURCE_DESC d = dst->GetDesc();
  cl->Dispatch(UINT((d.Width + 7) / 8), (d.Height + 7) / 8, 1);
}

}  // namespace th::fg
