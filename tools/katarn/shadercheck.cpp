// Compiles every KatarnEngine shader entry point with d3dcompiler_47 (run on Windows or under wine)
#include <windows.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string>
#include "../../code/rd-katarn/katarn/kt_shaders_gen.h"
static std::string Join(const char* const* p) { std::string s; for (int i = 0; p[i]; i++) s += p[i]; return s; }
int main()
{
	HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
	pD3DCompile c = m ? (pD3DCompile)GetProcAddress(m, "D3DCompile") : nullptr;
	if (!c) { printf("no d3dcompiler_47\n"); return 1; }
	struct E { const char* const* src; const char* name; const char* entry; const char* target; } list[] = {
		{ kt_src_legacy_parts, "legacy", "VS", "vs_5_1" }, { kt_src_legacy_parts, "legacy", "PS", "ps_5_1" },
		{ kt_src_present_parts, "present", "FullscreenVS", "vs_5_1" }, { kt_src_present_parts, "present", "PS", "ps_5_1" },
		{ kt_src_post_parts, "post", "TonemapPS", "ps_5_1" }, { kt_src_post_parts, "post", "FxaaPS", "ps_5_1" },
		{ kt_src_post_parts, "post", "HistogramCS", "cs_5_1" }, { kt_src_post_parts, "post", "ExposureCS", "cs_5_1" },
		{ kt_src_post_parts, "post", "BloomDownCS", "cs_5_1" }, { kt_src_post_parts, "post", "BloomUpCS", "cs_5_1" },
		{ kt_src_legacy_gbuffer_parts, "legacy_gbuffer", "VS", "vs_5_1" }, { kt_src_legacy_gbuffer_parts, "legacy_gbuffer", "PS", "ps_5_1" },
		{ kt_src_legacy_gbuffer_parts, "legacy_gbuffer", "ShadowPS", "ps_5_1" },
		{ kt_src_world_parts, "world", "VS", "vs_5_1" }, { kt_src_world_parts, "world", "PS", "ps_5_1" }, { kt_src_world_parts, "world", "ShadowPS", "ps_5_1" },
		{ kt_src_sky_parts, "sky", "LutCS", "cs_5_1" }, { kt_src_sky_parts, "sky", "ShCS", "cs_5_1" },
		{ kt_src_lighting_parts, "lighting", "CS", "cs_5_1" },
		{ kt_src_ssao_parts, "ssao", "AoCS", "cs_5_1" }, { kt_src_ssao_parts, "ssao", "BlurCS", "cs_5_1" },
	};
	int fails = 0;
	for (auto& e : list) {
		std::string s = Join(e.src);
		ID3DBlob* code = nullptr, * err = nullptr;
		HRESULT hr = c(s.data(), s.size(), e.name, nullptr, nullptr, e.entry, e.target,
			D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES | D3DCOMPILE_ALL_RESOURCES_BOUND, 0, &code, &err);
		printf("%-16s %-14s %s %s\n", e.name, e.entry, SUCCEEDED(hr) ? "ok" : "FAILED", err ? (const char*)err->GetBufferPointer() : "");
		if (FAILED(hr)) fails++;
	}
	printf("%d failed\n", fails);
	return fails;
}
