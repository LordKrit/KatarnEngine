// KatarnEngine - D3D12 device layer
#include "kt_device.h"
#include <d3dcompiler.h>
#include <d3d12sdklayers.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unordered_map>
#include <mutex>

namespace kt {

DeviceState g;
void (*g_logCallback)(const char* msg) = nullptr;
static FILE* g_logFile = nullptr;

void Log(const char* fmt, ...)
{
	char buf[4096];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (g_logCallback) g_logCallback(buf);
	if (!g_logFile) g_logFile = fopen("katarn_log.txt", "w");
	if (g_logFile) { fputs(buf, g_logFile); fflush(g_logFile); }
}

#define KT_CHECK(x, what) do { HRESULT _hr = (x); if (FAILED(_hr)) { Log("^1KatarnEngine: %s failed (0x%08x)\n", what, (unsigned)_hr); return false; } } while (0)

// --------------------------------------------------------------------------------------------- per-frame state
struct FrameCtx {
	std::vector<ID3D12CommandAllocator*> allocators;
	uint32_t allocUsed = 0;
	std::vector<IUnknown*> releases;
	std::vector<IUnknown*> copyTemps;   // one-off upload buffers used only by the upload list
	std::vector<uint32_t> freeDescriptors;
};
static std::vector<IUnknown*> pendingCopyTemps;
static FrameCtx frames[kFrames];
static std::vector<ID3D12GraphicsCommandList*> listPool;
static uint32_t listUsed = 0;
static ID3D12GraphicsCommandList* uploadList = nullptr;
static std::vector<ID3D12CommandList*> pendingSubmit;

// upload ring
static ID3D12Resource* ring = nullptr;
static uint8_t* ringCpu = nullptr;
static D3D12_GPU_VIRTUAL_ADDRESS ringGpu = 0;
static const uint64_t kRingPerFrame = 96ull << 20;
static uint64_t ringHead = 0;

// descriptors
static std::vector<uint32_t> freeSrv;
static uint32_t rtvNext = 0, dsvNext = 0;
static const uint32_t kRtvCount = 1024, kDsvCount = 256;
static std::vector<uint32_t> freeRtv, freeDsv;

// compiler
static pD3DCompile pCompile = nullptr;
static std::string shaderCacheDir;

DXGI_FORMAT Typeless(DXGI_FORMAT f)
{
	switch (f) {
	case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
	case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
	case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB: return DXGI_FORMAT_BC1_TYPELESS;
	case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB: return DXGI_FORMAT_BC2_TYPELESS;
	case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB: return DXGI_FORMAT_BC3_TYPELESS;
	case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB: return DXGI_FORMAT_BC7_TYPELESS;
	default: return f;
	}
}
static DXGI_FORMAT Srgb(DXGI_FORMAT f)
{
	switch (f) {
	case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	case DXGI_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
	case DXGI_FORMAT_BC1_UNORM: return DXGI_FORMAT_BC1_UNORM_SRGB;
	case DXGI_FORMAT_BC2_UNORM: return DXGI_FORMAT_BC2_UNORM_SRGB;
	case DXGI_FORMAT_BC3_UNORM: return DXGI_FORMAT_BC3_UNORM_SRGB;
	case DXGI_FORMAT_BC7_UNORM: return DXGI_FORMAT_BC7_UNORM_SRGB;
	default: return f;
	}
}
static bool IsBC(DXGI_FORMAT f)
{
	return (f >= DXGI_FORMAT_BC1_TYPELESS && f <= DXGI_FORMAT_BC5_SNORM) || (f >= DXGI_FORMAT_BC6H_TYPELESS && f <= DXGI_FORMAT_BC7_UNORM_SRGB);
}
uint32_t FormatBytes(DXGI_FORMAT f)   // bytes per pixel, or per 4x4 block for BC
{
	switch (f) {
	case DXGI_FORMAT_R8_UNORM: return 1;
	case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R16_FLOAT: return 2;
	case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R11G11B10_FLOAT: case DXGI_FORMAT_R10G10B10A2_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_R32_UINT:
		return 4;
	case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R32G32_FLOAT: return 8;
	case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
	case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB: case DXGI_FORMAT_BC4_UNORM: return 8;
	case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB: case DXGI_FORMAT_BC5_UNORM:
	case DXGI_FORMAT_BC5_SNORM: case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB: case DXGI_FORMAT_BC6H_UF16: return 16;
	default: return 4;
	}
}

// --------------------------------------------------------------------------------------------- descriptors
uint32_t AllocDescriptor()
{
	if (freeSrv.empty()) { Log("^1KatarnEngine: out of descriptors\n"); return kNullSrv; }
	uint32_t i = freeSrv.back(); freeSrv.pop_back();
	return i;
}
void FreeDescriptor(uint32_t idx)
{
	if (idx != kNullSrv) frames[g.frameIndex].freeDescriptors.push_back(idx);
}
D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle(uint32_t idx)
{
	D3D12_CPU_DESCRIPTOR_HANDLE h = g.srvHeap->GetCPUDescriptorHandleForHeapStart();
	h.ptr += (SIZE_T)idx * g.srvInc;
	return h;
}
D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle(uint32_t idx)
{
	D3D12_GPU_DESCRIPTOR_HANDLE h = g.srvHeap->GetGPUDescriptorHandleForHeapStart();
	h.ptr += (UINT64)idx * g.srvInc;
	return h;
}
static D3D12_CPU_DESCRIPTOR_HANDLE AllocRtv()
{
	uint32_t i;
	if (!freeRtv.empty()) { i = freeRtv.back(); freeRtv.pop_back(); }
	else i = rtvNext++;
	D3D12_CPU_DESCRIPTOR_HANDLE h = g.rtvHeap->GetCPUDescriptorHandleForHeapStart();
	h.ptr += (SIZE_T)i * g.rtvInc;
	return h;
}
static D3D12_CPU_DESCRIPTOR_HANDLE AllocDsv()
{
	uint32_t i;
	if (!freeDsv.empty()) { i = freeDsv.back(); freeDsv.pop_back(); }
	else i = dsvNext++;
	D3D12_CPU_DESCRIPTOR_HANDLE h = g.dsvHeap->GetCPUDescriptorHandleForHeapStart();
	h.ptr += (SIZE_T)i * g.dsvInc;
	return h;
}
static void FreeRtv(D3D12_CPU_DESCRIPTOR_HANDLE h)
{
	if (h.ptr) freeRtv.push_back((uint32_t)((h.ptr - g.rtvHeap->GetCPUDescriptorHandleForHeapStart().ptr) / g.rtvInc));
}
static void FreeDsv(D3D12_CPU_DESCRIPTOR_HANDLE h)
{
	if (h.ptr) freeDsv.push_back((uint32_t)((h.ptr - g.dsvHeap->GetCPUDescriptorHandleForHeapStart().ptr) / g.dsvInc));
}

void BindRoot(ID3D12GraphicsCommandList* cl, bool compute)
{
	ID3D12DescriptorHeap* heaps[] = { g.srvHeap };
	cl->SetDescriptorHeaps(1, heaps);
	D3D12_GPU_DESCRIPTOR_HANDLE base = g.srvHeap->GetGPUDescriptorHandleForHeapStart();
	if (compute) {
		cl->SetComputeRootSignature(g.rootSig);
		for (UINT p = RP_TEX2D; p < RP_COUNT; p++) cl->SetComputeRootDescriptorTable(p, base);
	}
	else {
		cl->SetGraphicsRootSignature(g.rootSig);
		for (UINT p = RP_TEX2D; p < RP_COUNT; p++) cl->SetGraphicsRootDescriptorTable(p, base);
	}
}

// --------------------------------------------------------------------------------------------- release
void Release(IUnknown* p)
{
	if (p) frames[g.frameIndex].releases.push_back(p);
}

// --------------------------------------------------------------------------------------------- root signature
static bool CreateRootSignature()
{
	auto serialize = (PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE)GetProcAddress(g.d3d12dll, "D3D12SerializeVersionedRootSignature");
	if (!serialize) { Log("^1KatarnEngine: D3D12SerializeVersionedRootSignature missing (Windows too old)\n"); return false; }

	D3D12_DESCRIPTOR_RANGE1 ranges[RP_COUNT] = {};
	D3D12_ROOT_PARAMETER1 params[RP_COUNT] = {};
	params[RP_CONSTANTS].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	params[RP_CONSTANTS].Constants.ShaderRegister = 0;
	params[RP_CONSTANTS].Constants.Num32BitValues = 32;
	params[RP_VIEW_CBV].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[RP_VIEW_CBV].Descriptor.ShaderRegister = 1;
	params[RP_VIEW_CBV].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
	params[RP_EXTRA_CBV].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[RP_EXTRA_CBV].Descriptor.ShaderRegister = 2;
	params[RP_EXTRA_CBV].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
	struct { RootParam p; D3D12_DESCRIPTOR_RANGE_TYPE t; UINT space; } tables[] = {
		{ RP_TEX2D, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1 }, { RP_TEXCUBE, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2 },
		{ RP_TEX2DARRAY, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3 }, { RP_BUFFERS, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4 },
		{ RP_TEX3D, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5 },
		{ RP_UAV_TEX, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1 }, { RP_UAV_BUF, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2 },
		{ RP_UAV_TEX2DARRAY, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3 },
	};
	for (auto& t : tables) {
		D3D12_DESCRIPTOR_RANGE1& r = ranges[t.p];
		r.RangeType = t.t;
		r.NumDescriptors = UINT_MAX;   // unbounded, as the shaders declare them
		r.BaseShaderRegister = 0;
		r.RegisterSpace = t.space;
		r.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
		r.OffsetInDescriptorsFromTableStart = 0;
		params[t.p].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[t.p].DescriptorTable.NumDescriptorRanges = 1;
		params[t.p].DescriptorTable.pDescriptorRanges = &r;
	}
	for (auto& p : params) p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	D3D12_STATIC_SAMPLER_DESC s[6] = {};
	for (int i = 0; i < 6; i++) {
		s[i].ShaderRegister = i;
		s[i].MaxLOD = D3D12_FLOAT32_MAX;
		s[i].MaxAnisotropy = 1;
		s[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		s[i].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
		s[i].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
	}
	auto addr = [](D3D12_STATIC_SAMPLER_DESC& d, D3D12_TEXTURE_ADDRESS_MODE m) { d.AddressU = d.AddressV = d.AddressW = m; };
	s[0].Filter = D3D12_FILTER_ANISOTROPIC; s[0].MaxAnisotropy = 8; addr(s[0], D3D12_TEXTURE_ADDRESS_MODE_WRAP);       // s0 wrap aniso
	s[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR; addr(s[1], D3D12_TEXTURE_ADDRESS_MODE_CLAMP);                         // s1 clamp linear
	s[2].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT; addr(s[2], D3D12_TEXTURE_ADDRESS_MODE_WRAP);                           // s2 wrap point
	s[3].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT; addr(s[3], D3D12_TEXTURE_ADDRESS_MODE_CLAMP);                          // s3 clamp point
	s[4].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT; addr(s[4], D3D12_TEXTURE_ADDRESS_MODE_BORDER);       // s4 shadow compare
	s[4].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	s[5].Filter = D3D12_FILTER_ANISOTROPIC; s[5].MaxAnisotropy = 8; addr(s[5], D3D12_TEXTURE_ADDRESS_MODE_CLAMP);      // s5 clamp aniso

	D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = {};
	desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
	desc.Desc_1_1.NumParameters = RP_COUNT;
	desc.Desc_1_1.pParameters = params;
	desc.Desc_1_1.NumStaticSamplers = 6;
	desc.Desc_1_1.pStaticSamplers = s;
	desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
	ID3DBlob* blob = nullptr, * errBlob = nullptr;
	HRESULT hr = serialize(&desc, &blob, &errBlob);
	if (FAILED(hr)) {
		Log("^1KatarnEngine: root signature: %s\n", errBlob ? (const char*)errBlob->GetBufferPointer() : "?");
		return false;
	}
	KT_CHECK(g.dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g.rootSig)), "CreateRootSignature");
	blob->Release();
	return true;
}

// --------------------------------------------------------------------------------------------- init
static bool CreateSwapchainTargets()
{
	for (int i = 0; i < kFrames; i++) {
		Texture& t = g.backbuffer[i];
		t = Texture();
		if (FAILED(g.swap->GetBuffer(i, IID_PPV_ARGS(&t.res)))) return false;
		t.format = DXGI_FORMAT_R8G8B8A8_UNORM;
		t.width = g.width; t.height = g.height;
		t.state = D3D12_RESOURCE_STATE_PRESENT;
		t.flags = TEX_RT;
		t.rtv[0] = AllocRtv();
		g.dev->CreateRenderTargetView(t.res, nullptr, t.rtv[0]);
		t.name = "backbuffer";
	}
	return true;
}

bool Init(HWND hwnd, uint32_t width, uint32_t height, const Config& cfg, std::string& err)
{
	g.hwnd = hwnd; g.width = width; g.height = height; g.cfg = cfg;
	g.d3d12dll = LoadLibraryA("d3d12.dll");
	g.dxgidll = LoadLibraryA("dxgi.dll");
	if (!g.d3d12dll || !g.dxgidll) { err = "d3d12.dll / dxgi.dll not found (needs Windows 10 or newer)"; return false; }
	auto createDevice = (PFN_D3D12_CREATE_DEVICE)GetProcAddress(g.d3d12dll, "D3D12CreateDevice");
	auto getDebug = (PFN_D3D12_GET_DEBUG_INTERFACE)GetProcAddress(g.d3d12dll, "D3D12GetDebugInterface");
	typedef HRESULT(WINAPI* PFN_CreateDXGIFactory2)(UINT, REFIID, void**);
	auto createFactory = (PFN_CreateDXGIFactory2)GetProcAddress(g.dxgidll, "CreateDXGIFactory2");
	if (!createDevice || !createFactory) { err = "D3D12CreateDevice / CreateDXGIFactory2 missing"; return false; }

	if (cfg.debug && getDebug) {
		ID3D12Debug* dbg = nullptr;
		if (SUCCEEDED(getDebug(IID_PPV_ARGS(&dbg)))) { dbg->EnableDebugLayer(); dbg->Release(); Log("KatarnEngine: D3D12 debug layer on\n"); }
	}
	if (FAILED(createFactory(cfg.debug ? DXGI_CREATE_FACTORY_DEBUG : 0, IID_PPV_ARGS(&g.factory))) &&
		FAILED(createFactory(0, IID_PPV_ARGS(&g.factory)))) { err = "CreateDXGIFactory2 failed"; return false; }

	// pick the high-performance adapter
	IDXGIFactory6* f6 = nullptr;
	if (SUCCEEDED(g.factory->QueryInterface(IID_PPV_ARGS(&f6)))) {
		for (UINT i = 0; f6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&g.adapter)) != DXGI_ERROR_NOT_FOUND; i++) {
			DXGI_ADAPTER_DESC1 d; g.adapter->GetDesc1(&d);
			if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && SUCCEEDED(createDevice(g.adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g.dev)))) break;
			g.adapter->Release(); g.adapter = nullptr;
		}
		f6->Release();
	}
	if (!g.dev) {
		for (UINT i = 0; g.factory->EnumAdapters1(i, &g.adapter) != DXGI_ERROR_NOT_FOUND; i++) {
			if (SUCCEEDED(createDevice(g.adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.dev)))) break;
			g.adapter->Release(); g.adapter = nullptr;
		}
	}
	if (!g.dev) { err = "no Direct3D 12 GPU found"; return false; }
	{
		DXGI_ADAPTER_DESC1 d; g.adapter->GetDesc1(&d);
		char name[256]; WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name), nullptr, nullptr);
		g.adapterName = name; g.vram = d.DedicatedVideoMemory;
	}
	D3D12_FEATURE_DATA_D3D12_OPTIONS opts = {};
	g.dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opts, sizeof(opts));
	Log("KatarnEngine: %s, %u MB VRAM, resource binding tier %d\n", g.adapterName.c_str(), (unsigned)(g.vram >> 20), (int)opts.ResourceBindingTier);
	if (opts.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_2) { err = "GPU needs resource binding tier 2 (bindless)"; return false; }

	if (cfg.debug) {
		ID3D12InfoQueue* iq = nullptr;
		if (SUCCEEDED(g.dev->QueryInterface(IID_PPV_ARGS(&iq)))) {
			iq->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
			iq->Release();
		}
	}

	D3D12_COMMAND_QUEUE_DESC qd = {};
	qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	if (FAILED(g.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue)))) { err = "CreateCommandQueue failed"; return false; }

	// tearing support (vsync off on variable refresh displays)
	IDXGIFactory5* f5 = nullptr;
	if (SUCCEEDED(g.factory->QueryInterface(IID_PPV_ARGS(&f5)))) {
		BOOL allow = FALSE;
		if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow)))) g.tearing = allow != FALSE;
		f5->Release();
	}
	DXGI_SWAP_CHAIN_DESC1 sd = {};
	sd.Width = width; sd.Height = height;
	sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.SampleDesc.Count = 1;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount = kFrames;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	sd.Flags = g.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
	IDXGISwapChain1* sc1 = nullptr;
	if (FAILED(g.factory->CreateSwapChainForHwnd(g.queue, hwnd, &sd, nullptr, nullptr, &sc1))) { err = "CreateSwapChainForHwnd failed"; return false; }
	sc1->QueryInterface(IID_PPV_ARGS(&g.swap)); sc1->Release();
	g.factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

	// heaps
	D3D12_DESCRIPTOR_HEAP_DESC hd = {};
	hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = kHeapSize; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	if (FAILED(g.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.srvHeap)))) { err = "CreateDescriptorHeap failed"; return false; }
	hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = kRtvCount; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	g.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.rtvHeap));
	hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; hd.NumDescriptors = kDsvCount;
	g.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.dsvHeap));
	g.srvInc = g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	g.rtvInc = g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	g.dsvInc = g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
	// fill every slot with a null view so stray indices read black instead of faulting
	freeSrv.clear();
	for (uint32_t i = 0; i < kHeapSize; i++) {
		D3D12_SHADER_RESOURCE_VIEW_DESC nd = {};
		nd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		nd.Texture2D.MipLevels = 1;
		g.dev->CreateShaderResourceView(nullptr, &nd, CpuHandle(i));
	}
	for (uint32_t i = kHeapSize - 1; i >= 1; i--) freeSrv.push_back(i);

	if (FAILED(g.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)))) { err = "CreateFence failed"; return false; }
	g.fenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);

	// upload ring
	D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
	D3D12_RESOURCE_DESC rd = {};
	rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = kRingPerFrame * kFrames; rd.Height = 1; rd.DepthOrArraySize = 1;
	rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&ring)))) {
		err = "upload ring allocation failed"; return false;
	}
	D3D12_RANGE none = { 0, 0 };
	ring->Map(0, &none, (void**)&ringCpu);
	ringGpu = ring->GetGPUVirtualAddress();

	if (!CreateRootSignature()) { err = "root signature"; return false; }
	if (!CreateSwapchainTargets()) { err = "swap chain buffers"; return false; }

	g.compilerdll = LoadLibraryA("d3dcompiler_47.dll");
	if (g.compilerdll) pCompile = (pD3DCompile)GetProcAddress(g.compilerdll, "D3DCompile");
	if (!pCompile) Log("^3KatarnEngine: d3dcompiler_47.dll not found - only cached shaders can be used\n");

	g.frameIndex = 0;
	g.frameNumber = 0;
	BeginFrame();
	return true;
}

void WaitIdle()
{
	if (!g.queue) return;
	g.fenceValue++;
	g.queue->Signal(g.fence, g.fenceValue);
	if (g.fence->GetCompletedValue() < g.fenceValue) {
		g.fence->SetEventOnCompletion(g.fenceValue, g.fenceEvent);
		WaitForSingleObject(g.fenceEvent, INFINITE);
	}
}

static void RecycleFrame(FrameCtx& f)
{
	for (IUnknown* p : f.releases) p->Release();
	f.releases.clear();
	for (IUnknown* p : f.copyTemps) p->Release();
	f.copyTemps.clear();
	for (uint32_t d : f.freeDescriptors) freeSrv.push_back(d);
	f.freeDescriptors.clear();
}

void Shutdown()
{
	if (!g.dev) return;
	if (uploadList) { uploadList->Close(); }
	WaitIdle();
	for (auto& f : frames) {
		RecycleFrame(f);
		for (auto* a : f.allocators) a->Release();
		f.allocators.clear(); f.allocUsed = 0;
	}
	for (auto* l : listPool) l->Release();
	listPool.clear(); listUsed = 0; uploadList = nullptr; pendingSubmit.clear();
	for (auto& b : g.backbuffer) { if (b.res) b.res->Release(); b = Texture(); }
	if (ring) { ring->Release(); ring = nullptr; }
	if (g.rootSig) g.rootSig->Release();
	if (g.srvHeap) g.srvHeap->Release();
	if (g.rtvHeap) g.rtvHeap->Release();
	if (g.dsvHeap) g.dsvHeap->Release();
	if (g.swap) { g.swap->SetFullscreenState(FALSE, nullptr); g.swap->Release(); }
	if (g.fence) g.fence->Release();
	if (g.queue) g.queue->Release();
	if (g.fenceEvent) CloseHandle(g.fenceEvent);
	ULONG left = g.dev->Release();
	if (left) Log("KatarnEngine: %lu D3D12 references still alive at shutdown\n", left);
	if (g.adapter) g.adapter->Release();
	if (g.factory) g.factory->Release();
	freeSrv.clear(); freeRtv.clear(); freeDsv.clear(); rtvNext = dsvNext = 0;
	DeviceState fresh; g = fresh;
	if (g_logFile) { fclose(g_logFile); g_logFile = nullptr; }
}

// --------------------------------------------------------------------------------------------- frames
void BeginFrame()
{
	FrameCtx& f = frames[g.frameIndex];
	if (g.fence->GetCompletedValue() < g.frameFence[g.frameIndex]) {
		g.fence->SetEventOnCompletion(g.frameFence[g.frameIndex], g.fenceEvent);
		WaitForSingleObject(g.fenceEvent, INFINITE);
	}
	RecycleFrame(f);
	for (uint32_t i = 0; i < f.allocUsed; i++) f.allocators[i]->Reset();
	f.allocUsed = 0;
	listUsed = 0;
	ringHead = 0;
}

static ID3D12GraphicsCommandList* NewList()
{
	FrameCtx& f = frames[g.frameIndex];
	if (f.allocUsed == f.allocators.size()) {
		ID3D12CommandAllocator* a = nullptr;
		g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
		f.allocators.push_back(a);
	}
	ID3D12CommandAllocator* a = f.allocators[f.allocUsed++];
	ID3D12GraphicsCommandList* cl;
	if (listUsed == listPool.size()) {
		g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, nullptr, IID_PPV_ARGS(&cl));
		listPool.push_back(cl);
	}
	else {
		cl = listPool[listUsed];
		cl->Reset(a, nullptr);
	}
	listUsed++;
	return cl;
}

ID3D12GraphicsCommandList* OpenList()
{
	ID3D12GraphicsCommandList* cl = NewList();
	BindRoot(cl, false);
	return cl;
}

ID3D12GraphicsCommandList* UploadList()
{
	if (!uploadList) uploadList = NewList();
	return uploadList;
}

static void ExecuteUploads()
{
	if (!uploadList) return;
	uploadList->Close();
	ID3D12CommandList* l[] = { uploadList };
	g.queue->ExecuteCommandLists(1, l);
	uploadList = nullptr;
	auto& ct = frames[g.frameIndex].copyTemps;
	ct.insert(ct.end(), pendingCopyTemps.begin(), pendingCopyTemps.end());
	pendingCopyTemps.clear();
}

void SubmitList(ID3D12GraphicsCommandList* cl)
{
	ExecuteUploads();
	cl->Close();
	ID3D12CommandList* l[] = { cl };
	g.queue->ExecuteCommandLists(1, l);
}

void FlushUploads()
{
	if (!uploadList) return;
	ExecuteUploads();
	// level loads upload hundreds of MB: let the GPU drain and free the one-off upload buffers now
	WaitIdle();
	FrameCtx& f = frames[g.frameIndex];
	for (IUnknown* p : f.copyTemps) p->Release();
	f.copyTemps.clear();
}

Texture& CurrentBackbuffer()
{
	return g.backbuffer[g.swap->GetCurrentBackBufferIndex()];
}

void Present()
{
	ExecuteUploads();
	UINT flags = (!g.cfg.vsync && g.tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0;
	HRESULT hr = g.swap->Present(g.cfg.vsync ? 1 : 0, flags);
	if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
		HRESULT reason = g.dev->GetDeviceRemovedReason();
		Log("^1KatarnEngine: GPU device removed (0x%08x)\n", (unsigned)reason);
	}
	g.fenceValue++;
	g.queue->Signal(g.fence, g.fenceValue);
	g.frameFence[g.frameIndex] = g.fenceValue;
	g.frameIndex = (g.frameIndex + 1) % kFrames;
	g.frameNumber++;
	BeginFrame();
}

// --------------------------------------------------------------------------------------------- memory
static UploadAlloc UploadImpl(uint64_t size, uint64_t align, bool copy)
{
	UploadAlloc a;
	uint64_t base = (uint64_t)g.frameIndex * kRingPerFrame;
	uint64_t off = (ringHead + align - 1) & ~(align - 1);
	if (off + size <= kRingPerFrame) {
		ringHead = off + size;
		a.cpu = ringCpu + base + off;
		a.gpu = ringGpu + base + off;
		a.res = ring;
		a.offset = base + off;
		return a;
	}
	// too big for what is left of the ring: a one-off upload buffer, released with this frame
	D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
	D3D12_RESOURCE_DESC rd = {};
	rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1;
	rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	ID3D12Resource* r = nullptr;
	if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&r)))) {
		Log("^1KatarnEngine: upload allocation of %llu bytes failed\n", (unsigned long long)size);
		return a;
	}
	D3D12_RANGE none = { 0, 0 };
	r->Map(0, &none, &a.cpu);
	a.gpu = r->GetGPUVirtualAddress();
	a.res = r;
	a.offset = 0;
	if (copy) pendingCopyTemps.push_back(r);
	else Release(r);
	return a;
}
UploadAlloc Upload(uint64_t size, uint64_t align) { return UploadImpl(size, align, false); }

bool CreateTexture(Texture& t, uint32_t w, uint32_t h, uint32_t mips, DXGI_FORMAT fmt, uint32_t flags, uint32_t layers,
	const float* clear, const char* name)
{
	t = Texture();
	t.width = w; t.height = h; t.mips = mips ? mips : 1; t.format = fmt; t.flags = flags; t.name = name;
	t.layers = (flags & TEX_CUBE) ? 6 * layers : layers;
	bool depth = (flags & TEX_DEPTH) != 0;
	bool is3d = (flags & TEX_3D) != 0;
	D3D12_RESOURCE_DESC rd = {};
	rd.Dimension = is3d ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	rd.Width = w; rd.Height = h;
	rd.DepthOrArraySize = (UINT16)t.layers;
	rd.MipLevels = (UINT16)t.mips;
	rd.Format = depth ? DXGI_FORMAT_R32_TYPELESS : ((flags & TEX_SRGB_VIEW) ? Typeless(fmt) : fmt);
	rd.SampleDesc.Count = 1;
	if (flags & TEX_RT) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	if (depth) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
	if (flags & TEX_UAV) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_CLEAR_VALUE cv = {};
	D3D12_CLEAR_VALUE* pcv = nullptr;
	if (depth) { cv.Format = DXGI_FORMAT_D32_FLOAT; cv.DepthStencil.Depth = clear ? clear[0] : 0.0f; pcv = &cv; }
	else if (flags & TEX_RT) { cv.Format = fmt; if (clear) memcpy(cv.Color, clear, 16); pcv = &cv; }
	t.state = depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : (flags & TEX_RT) ? D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_COPY_DEST;
	HRESULT hr = g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, t.state, pcv, IID_PPV_ARGS(&t.res));
	if (FAILED(hr)) {
		Log("^1KatarnEngine: texture %s %ux%u failed (0x%08x)\n", name, w, h, (unsigned)hr);
		t.res = nullptr;
		return false;
	}
	if (name && *name) {
		wchar_t wn[128]; MultiByteToWideChar(CP_UTF8, 0, name, -1, wn, 128);
		t.res->SetName(wn);
	}
	// SRV
	D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
	sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	sd.Format = depth ? DXGI_FORMAT_R32_FLOAT : fmt;
	if (is3d) { sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D; sd.Texture3D.MipLevels = t.mips; }
	else if (flags & TEX_CUBE) { sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE; sd.TextureCube.MipLevels = t.mips; }
	else if ((flags & TEX_ARRAY) || t.layers > 1) {
		sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
		sd.Texture2DArray.MipLevels = t.mips; sd.Texture2DArray.ArraySize = t.layers;
	}
	else { sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = t.mips; }
	t.srv = AllocDescriptor();
	g.dev->CreateShaderResourceView(t.res, &sd, CpuHandle(t.srv));
	t.srvSrgb = t.srv;
	if (flags & TEX_SRGB_VIEW) {
		sd.Format = Srgb(fmt);
		t.srvSrgb = AllocDescriptor();
		g.dev->CreateShaderResourceView(t.res, &sd, CpuHandle(t.srvSrgb));
	}
	if (flags & TEX_UAV) {
		for (uint32_t m = 0; m < t.mips && m < 16; m++) {
			D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
			ud.Format = fmt;
			if (is3d) { ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D; ud.Texture3D.MipSlice = m; ud.Texture3D.WSize = (UINT)-1; }
			else if (t.layers > 1) {
				ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY; ud.Texture2DArray.MipSlice = m; ud.Texture2DArray.ArraySize = t.layers;
			}
			else { ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D; ud.Texture2D.MipSlice = m; }
			t.uav[m] = AllocDescriptor();
			g.dev->CreateUnorderedAccessView(t.res, nullptr, &ud, CpuHandle(t.uav[m]));
		}
	}
	if (flags & TEX_RT) {
		uint32_t n = t.layers > 1 ? (t.layers < 8 ? t.layers : 8) : 1;
		for (uint32_t i = 0; i < n; i++) {
			D3D12_RENDER_TARGET_VIEW_DESC vd = {};
			vd.Format = fmt;
			if (t.layers > 1) { vd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY; vd.Texture2DArray.FirstArraySlice = i; vd.Texture2DArray.ArraySize = 1; }
			else vd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
			t.rtv[i] = AllocRtv();
			g.dev->CreateRenderTargetView(t.res, &vd, t.rtv[i]);
		}
	}
	if (depth) {
		uint32_t n = t.layers > 1 ? (t.layers < 8 ? t.layers : 8) : 1;
		for (uint32_t i = 0; i < n; i++) {
			D3D12_DEPTH_STENCIL_VIEW_DESC vd = {};
			vd.Format = DXGI_FORMAT_D32_FLOAT;
			if (t.layers > 1) { vd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY; vd.Texture2DArray.FirstArraySlice = i; vd.Texture2DArray.ArraySize = 1; }
			else vd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
			t.dsv[i] = AllocDsv();
			g.dev->CreateDepthStencilView(t.res, &vd, t.dsv[i]);
		}
	}
	return true;
}

void DestroyTexture(Texture& t)
{
	if (!t.res) { t = Texture(); return; }
	Release(t.res);
	FreeDescriptor(t.srv);
	if (t.srvSrgb != t.srv) FreeDescriptor(t.srvSrgb);
	for (uint32_t m = 0; m < 16; m++) if (t.uav[m]) FreeDescriptor(t.uav[m]);
	for (int i = 0; i < 8; i++) { FreeRtv(t.rtv[i]); FreeDsv(t.dsv[i]); }
	t = Texture();
}

bool CreateBuffer(Buffer& b, uint64_t size, bool uav, const char* name, bool upload)
{
	b = Buffer();
	size = (size + 15) & ~15ull;
	D3D12_HEAP_PROPERTIES hp = {}; hp.Type = upload ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC rd = {};
	rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1;
	rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	if (uav) rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	b.state = upload ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COMMON;
	if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, b.state, nullptr, IID_PPV_ARGS(&b.res)))) {
		Log("^1KatarnEngine: buffer %s (%llu bytes) failed\n", name, (unsigned long long)size);
		return false;
	}
	b.size = size;
	b.va = b.res->GetGPUVirtualAddress();
	D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
	sd.Format = DXGI_FORMAT_R32_TYPELESS;
	sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	sd.Buffer.NumElements = (UINT)(size / 4);
	sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
	b.srv = AllocDescriptor();
	g.dev->CreateShaderResourceView(b.res, &sd, CpuHandle(b.srv));
	if (uav) {
		D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
		ud.Format = DXGI_FORMAT_R32_TYPELESS;
		ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
		ud.Buffer.NumElements = (UINT)(size / 4);
		ud.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
		b.uav = AllocDescriptor();
		g.dev->CreateUnorderedAccessView(b.res, nullptr, &ud, CpuHandle(b.uav));
	}
	return true;
}

void DestroyBuffer(Buffer& b)
{
	if (b.res) Release(b.res);
	FreeDescriptor(b.srv);
	if (b.uav) FreeDescriptor(b.uav);
	b = Buffer();
}

void UploadTexture(Texture& t, uint32_t mip, uint32_t slice, const void* data, uint32_t rowPitchBytes,
	uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
	if (!t.res) return;
	uint32_t mw = t.width >> mip; if (!mw) mw = 1;
	uint32_t mh = t.height >> mip; if (!mh) mh = 1;
	if (!w) w = mw;
	if (!h) h = mh;
	bool bc = IsBC(t.format);
	uint32_t rows = bc ? (h + 3) / 4 : h;
	uint32_t rowBytes = bc ? ((w + 3) / 4) * FormatBytes(t.format) : w * FormatBytes(t.format);
	uint32_t pitch = (rowBytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
	UploadAlloc a = UploadImpl((uint64_t)pitch * rows, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, true);
	if (!a.cpu) return;
	for (uint32_t r = 0; r < rows; r++)
		memcpy((uint8_t*)a.cpu + (size_t)r * pitch, (const uint8_t*)data + (size_t)r * rowPitchBytes, rowBytes);
	ID3D12GraphicsCommandList* cl = UploadList();
	D3D12_RESOURCE_STATES prev = t.state;
	Transition(cl, t, D3D12_RESOURCE_STATE_COPY_DEST);
	D3D12_TEXTURE_COPY_LOCATION dst = {};
	dst.pResource = t.res;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dst.SubresourceIndex = mip + slice * t.mips;
	D3D12_TEXTURE_COPY_LOCATION src = {};
	src.pResource = a.res;
	src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	src.PlacedFootprint.Offset = a.offset;
	src.PlacedFootprint.Footprint.Format = (t.flags & TEX_SRGB_VIEW) ? t.format : t.format;
	src.PlacedFootprint.Footprint.Width = bc ? ((w + 3) & ~3u) : w;
	src.PlacedFootprint.Footprint.Height = bc ? ((h + 3) & ~3u) : h;
	src.PlacedFootprint.Footprint.Depth = 1;
	src.PlacedFootprint.Footprint.RowPitch = pitch;
	cl->CopyTextureRegion(&dst, x, y, 0, &src, nullptr);
	Transition(cl, t, (prev == D3D12_RESOURCE_STATE_COPY_DEST || prev == D3D12_RESOURCE_STATE_COMMON) ?
		(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) : prev);
}

void UploadBuffer(Buffer& b, uint64_t offset, const void* data, uint64_t size)
{
	if (!b.res || !size) return;
	UploadAlloc a = UploadImpl(size, 16, true);
	if (!a.cpu) return;
	memcpy(a.cpu, data, size);
	ID3D12GraphicsCommandList* cl = UploadList();
	D3D12_RESOURCE_STATES prev = b.state;
	Transition(cl, b, D3D12_RESOURCE_STATE_COPY_DEST);
	cl->CopyBufferRegion(b.res, offset, a.res, a.offset, size);
	Transition(cl, b, prev == D3D12_RESOURCE_STATE_COMMON || prev == D3D12_RESOURCE_STATE_COPY_DEST ?
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_INDEX_BUFFER |
		D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER : prev);
}

// --------------------------------------------------------------------------------------------- barriers
void Transition(ID3D12GraphicsCommandList* cl, Texture& t, D3D12_RESOURCE_STATES s)
{
	if (!t.res || t.state == s) return;
	D3D12_RESOURCE_BARRIER b = {};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = t.res;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	b.Transition.StateBefore = t.state;
	b.Transition.StateAfter = s;
	cl->ResourceBarrier(1, &b);
	t.state = s;
}
void Transition(ID3D12GraphicsCommandList* cl, Buffer& bf, D3D12_RESOURCE_STATES s)
{
	if (!bf.res || bf.state == s || bf.state == D3D12_RESOURCE_STATE_GENERIC_READ) return;
	D3D12_RESOURCE_BARRIER b = {};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = bf.res;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	b.Transition.StateBefore = bf.state;
	b.Transition.StateAfter = s;
	cl->ResourceBarrier(1, &b);
	bf.state = s;
}
void UavBarrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r)
{
	D3D12_RESOURCE_BARRIER b = {};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	b.UAV.pResource = r;
	cl->ResourceBarrier(1, &b);
}

// --------------------------------------------------------------------------------------------- shaders
void SetShaderCacheDir(const std::string& dir) { shaderCacheDir = dir; }

static uint64_t Fnv(const void* p, size_t n, uint64_t h = 1469598103934665603ull)
{
	const uint8_t* b = (const uint8_t*)p;
	for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
	return h;
}

struct BlobFromMemory : public ID3DBlob {
	std::vector<uint8_t> data;
	LONG refs = 1;
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** o) override { *o = this; AddRef(); return S_OK; }
	ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
	ULONG STDMETHODCALLTYPE Release() override { LONG r = --refs; if (!r) delete this; return r; }
	LPVOID STDMETHODCALLTYPE GetBufferPointer() override { return data.data(); }
	SIZE_T STDMETHODCALLTYPE GetBufferSize() override { return data.size(); }
};

Shader CompileShader(const char* name, const char* source, const char* entry, const char* target, const char* const* defines)
{
	Shader s;
	std::string key = std::string(source) + "|" + entry + "|" + target;
	std::vector<D3D_SHADER_MACRO> macros;
	if (defines) for (int i = 0; defines[i]; i += 2) { macros.push_back({ defines[i], defines[i + 1] }); key += std::string("|") + defines[i] + "=" + defines[i + 1]; }
	macros.push_back({ nullptr, nullptr });
	char file[512] = {};
	if (!shaderCacheDir.empty()) {
		snprintf(file, sizeof(file), "%s/%s_%s_%016llx.cso", shaderCacheDir.c_str(), name, entry, (unsigned long long)Fnv(key.data(), key.size()));
		FILE* f = fopen(file, "rb");
		if (f) {
			BlobFromMemory* b = new BlobFromMemory();
			fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
			b->data.resize(n);
			if (n > 0 && fread(b->data.data(), 1, n, f) == (size_t)n) { s.blob = b; s.ok = true; }
			else b->Release();
			fclose(f);
			if (s.ok) return s;
		}
	}
	if (!pCompile) { Log("^1KatarnEngine: cannot compile %s:%s (no d3dcompiler_47.dll)\n", name, entry); return s; }
	ID3DBlob* code = nullptr, * err = nullptr;
	UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES | D3DCOMPILE_ALL_RESOURCES_BOUND;
	HRESULT hr = pCompile(source, strlen(source), name, macros.data(), nullptr, entry, target, flags, 0, &code, &err);
	if (FAILED(hr)) {
		Log("^1KatarnEngine: shader %s:%s (%s) failed:\n%s\n", name, entry, target, err ? (const char*)err->GetBufferPointer() : "?");
		if (err) err->Release();
		return s;
	}
	if (err) err->Release();
	s.blob = code; s.ok = true;
	if (file[0]) {
		FILE* f = fopen(file, "wb");
		if (f) { fwrite(code->GetBufferPointer(), 1, code->GetBufferSize(), f); fclose(f); }
	}
	return s;
}

ID3D12PipelineState* CreateGraphicsPSO(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, const char* name)
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC d = desc;
	d.pRootSignature = g.rootSig;
	ID3D12PipelineState* p = nullptr;
	HRESULT hr = g.dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&p));
	if (FAILED(hr)) { Log("^1KatarnEngine: PSO %s failed (0x%08x)\n", name, (unsigned)hr); return nullptr; }
	return p;
}

ID3D12PipelineState* CreateComputePSO(const Shader& cs, const char* name)
{
	if (!cs.ok) return nullptr;
	D3D12_COMPUTE_PIPELINE_STATE_DESC d = {};
	d.pRootSignature = g.rootSig;
	d.CS.pShaderBytecode = cs.blob->GetBufferPointer();
	d.CS.BytecodeLength = cs.blob->GetBufferSize();
	ID3D12PipelineState* p = nullptr;
	HRESULT hr = g.dev->CreateComputePipelineState(&d, IID_PPV_ARGS(&p));
	if (FAILED(hr)) { Log("^1KatarnEngine: compute PSO %s failed (0x%08x)\n", name, (unsigned)hr); return nullptr; }
	return p;
}

} // namespace kt
