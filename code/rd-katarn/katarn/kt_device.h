// KatarnEngine - D3D12 device layer
#pragma once

#ifndef WIDL_EXPLICIT_AGGREGATE_RETURNS
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace kt {

constexpr int kFrames = 3;
constexpr uint32_t kHeapSize = 65536;      // shader-visible CBV/SRV/UAV descriptors (bindless)
constexpr uint32_t kNullSrv = 0;           // slot 0: null Texture2D

enum RootParam : UINT {
	RP_CONSTANTS = 0,   // b0: 32 x 32-bit draw constants
	RP_VIEW_CBV,        // b1: view constants
	RP_EXTRA_CBV,       // b2
	RP_TEX2D,           // t0 space1 Texture2D[]
	RP_TEXCUBE,         // t0 space2 TextureCube[]
	RP_TEX2DARRAY,      // t0 space3 Texture2DArray[]
	RP_BUFFERS,         // t0 space4 ByteAddressBuffer[]
	RP_TEX3D,           // t0 space5 Texture3D[]
	RP_UAV_TEX,         // u0 space1 RWTexture2D<float4>[]
	RP_UAV_BUF,         // u0 space2 RWByteAddressBuffer[]
	RP_UAV_TEX2DARRAY,  // u0 space3 RWTexture2DArray<float4>[]
	RP_UAV_TEX3D,       // u0 space4 RWTexture3D<float4>[]
	RP_COUNT
};

enum TexFlags : uint32_t {
	TEX_RT = 1, TEX_DEPTH = 2, TEX_UAV = 4, TEX_CUBE = 8, TEX_SRGB_VIEW = 16, TEX_ARRAY = 32, TEX_3D = 64
};

struct Texture {
	ID3D12Resource* res = nullptr;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	uint32_t width = 0, height = 0, depth = 1, mips = 1, layers = 1, flags = 0;
	uint32_t srv = 0;          // bindless index (linear / typed view)
	uint32_t srvSrgb = 0;      // bindless index of the sRGB view (8-bit colour textures), else = srv
	uint32_t uav[16] = {};     // bindless UAV per mip
	D3D12_CPU_DESCRIPTOR_HANDLE rtv[8] = {};   // per array slice (or one)
	D3D12_CPU_DESCRIPTOR_HANDLE dsv[8] = {};
	D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
	const char* name = "";
};

struct Buffer {
	ID3D12Resource* res = nullptr;
	uint64_t size = 0;
	uint32_t srv = 0, uav = 0;
	D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
	D3D12_GPU_VIRTUAL_ADDRESS va = 0;
};

struct UploadAlloc {
	void* cpu = nullptr;
	D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
	ID3D12Resource* res = nullptr;
	uint64_t offset = 0;
};

struct Shader {
	ID3DBlob* blob = nullptr;
	bool ok = false;
};

struct Config {
	bool vsync = true;
	bool debug = false;
};

struct DeviceState {
	HMODULE d3d12dll = nullptr, dxgidll = nullptr, compilerdll = nullptr;
	IDXGIFactory4* factory = nullptr;
	IDXGIAdapter1* adapter = nullptr;
	ID3D12Device* dev = nullptr;
	ID3D12CommandQueue* queue = nullptr;
	IDXGISwapChain3* swap = nullptr;
	ID3D12Fence* fence = nullptr;
	HANDLE fenceEvent = nullptr;
	uint64_t fenceValue = 0;
	uint64_t frameFence[kFrames] = {};
	uint32_t frameIndex = 0;      // 0..kFrames-1
	uint64_t frameNumber = 0;
	HWND hwnd = nullptr;
	uint32_t width = 0, height = 0;
	bool tearing = false;
	Config cfg;
	std::string adapterName;
	uint64_t vram = 0;
	ID3D12RootSignature* rootSig = nullptr;
	ID3D12DescriptorHeap* srvHeap = nullptr, * rtvHeap = nullptr, * dsvHeap = nullptr;
	uint32_t srvInc = 0, rtvInc = 0, dsvInc = 0;
	Texture backbuffer[kFrames];
};

extern DeviceState g;

// lifecycle
bool Init(HWND hwnd, uint32_t width, uint32_t height, const Config& cfg, std::string& err);
void Shutdown();
void WaitIdle();

// frames
void BeginFrame();                         // waits for the frame slot, resets allocators and the upload ring
ID3D12GraphicsCommandList* OpenList();     // a command list for this frame (recorded in order of opening)
void SubmitList(ID3D12GraphicsCommandList* cl);
void Present();
Texture& CurrentBackbuffer();

// descriptors
uint32_t AllocDescriptor();
void FreeDescriptor(uint32_t idx);         // deferred until the GPU is done with this frame
D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle(uint32_t idx);
D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle(uint32_t idx);
void BindRoot(ID3D12GraphicsCommandList* cl, bool compute);

// memory
UploadAlloc Upload(uint64_t size, uint64_t align = 256);
bool CreateTexture(Texture& t, uint32_t w, uint32_t h, uint32_t mips, DXGI_FORMAT fmt, uint32_t flags, uint32_t layers = 1,
	const float* clear = nullptr, const char* name = "");
void DestroyTexture(Texture& t);            // deferred release
bool CreateBuffer(Buffer& b, uint64_t size, bool uav, const char* name = "", bool upload = false);
void DestroyBuffer(Buffer& b);
void Release(IUnknown* p);                  // deferred release
// copy data into a texture mip / slice (recorded on the upload list, runs before this frame's work)
void UploadTexture(Texture& t, uint32_t mip, uint32_t slice, const void* data, uint32_t rowPitchBytes,
	uint32_t x = 0, uint32_t y = 0, uint32_t w = 0, uint32_t h = 0);
void UploadBuffer(Buffer& b, uint64_t offset, const void* data, uint64_t size);
ID3D12GraphicsCommandList* UploadList();   // the list that runs first in this frame
void FlushUploads();                        // submit pending uploads now (level load)

// barriers
void Transition(ID3D12GraphicsCommandList* cl, Texture& t, D3D12_RESOURCE_STATES s);
void Transition(ID3D12GraphicsCommandList* cl, Buffer& b, D3D12_RESOURCE_STATES s);
void UavBarrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r = nullptr);

// shaders and pipelines
Shader CompileShader(const char* name, const char* source, const char* entry, const char* target,
	const char* const* defines = nullptr);
ID3D12PipelineState* CreateGraphicsPSO(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, const char* name);
ID3D12PipelineState* CreateComputePSO(const Shader& cs, const char* name);
void SetShaderCacheDir(const std::string& dir);

// misc
void Log(const char* fmt, ...);
uint32_t FormatBytes(DXGI_FORMAT f);

} // namespace kt
