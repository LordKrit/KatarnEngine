// KatarnEngine - the frame.
//
// Per 3D world view:
//   GBuffer      HD world meshes + legacy opaque surfaces (characters) -> albedo / normal+roughness+metal / emissive
//   Shadows      4 stabilised sun cascades (HD world + characters)
//   Sky          single-scattering atmosphere LUT + SH irradiance, every frame (the sun can move)
//   AO           horizon-based SSAO + depth-aware blur
//   Lighting     compute: GGX/Smith/Schlick sun + local lights, sky ambient + reflections, height fog
//   Forward      legacy blended content (sabers, effects, glass, HUD-in-world) into the HDR scene
//   Camera       histogram auto exposure, bloom, ACES filmic, FXAA
// 2D (menus, HUD, console) goes to the screen target, which is copied to the swap chain with r_gamma.
#include "kt_render.h"
#include "kt_world.h"
#include "kt_shaders_gen.h"
#include "../tr_local.h"
#include <math.h>
#include <unordered_map>
#include <vector>
#include <string>

namespace kt {

void GL_ShutdownTextures();
void GL_ResetState();

// ------------------------------------------------------------------------------------------------ cvars
static cvar_t* kt_debug, * kt_ssao, * kt_shadows, * kt_shadowRes, * kt_shadowDist, * kt_bloom, * kt_exposure, * kt_manualExposure;
static cvar_t* kt_fxaa, * kt_glowBoost, * kt_debugView, * kt_shaderDir, * kt_legacyDeferred, * kt_sun, * kt_fog, * kt_aoRadius, * kt_aoIntensity;
static cvar_t* kt_tonemap = nullptr;
static cvar_t* kt_fogScale = nullptr;
static cvar_t* kt_dynGlow, * kt_contactShadows, * kt_lightShadows;
static cvar_t* kt_lights, * kt_dlightScale, * kt_speeds, * kt_ssr, * kt_volumetric, * kt_saberLight, * kt_saberGlow, * kt_saberDraw;

static void RegisterCvars()
{
	kt_debug = ri.Cvar_Get("kt_debug", "0", CVAR_LATCH);
	kt_ssao = ri.Cvar_Get("kt_ssao", "1", CVAR_ARCHIVE);
	kt_shadows = ri.Cvar_Get("kt_shadows", "1", CVAR_ARCHIVE);
	kt_shadowRes = ri.Cvar_Get("kt_shadowRes", "2048", CVAR_ARCHIVE | CVAR_LATCH);
	kt_shadowDist = ri.Cvar_Get("kt_shadowDist", "6000", CVAR_ARCHIVE);
	kt_bloom = ri.Cvar_Get("kt_bloom", "0.06", CVAR_ARCHIVE);
	kt_exposure = ri.Cvar_Get("kt_exposure", "0", CVAR_ARCHIVE);
	kt_tonemap = ri.Cvar_Get("kt_tonemap", "1", CVAR_ARCHIVE);           // HD worlds: 1 AgX (Blender's look), 0 ACES
	kt_fogScale = ri.Cvar_Get("kt_fogScale", "0.25", CVAR_ARCHIVE);      // HD world fog density (Blender renders have none)
	kt_manualExposure = ri.Cvar_Get("kt_manualExposure", "0", CVAR_ARCHIVE);
	kt_fxaa = ri.Cvar_Get("kt_fxaa", "1", CVAR_ARCHIVE);
	kt_glowBoost = ri.Cvar_Get("kt_glowBoost", "3", CVAR_ARCHIVE);
	kt_debugView = ri.Cvar_Get("kt_debugView", "0", 0);
	kt_shaderDir = ri.Cvar_Get("kt_shaderDir", "", CVAR_ARCHIVE | CVAR_LATCH);
	kt_legacyDeferred = ri.Cvar_Get("kt_legacyDeferred", "1", CVAR_ARCHIVE);
	kt_sun = ri.Cvar_Get("kt_sun", "1", CVAR_ARCHIVE);
	kt_fog = ri.Cvar_Get("kt_fog", "1", CVAR_ARCHIVE);
	kt_aoRadius = ri.Cvar_Get("kt_aoRadius", "40", CVAR_ARCHIVE);
	kt_aoIntensity = ri.Cvar_Get("kt_aoIntensity", "1.0", CVAR_ARCHIVE);
	kt_lights = ri.Cvar_Get("kt_lights", "1", CVAR_ARCHIVE);
	kt_dlightScale = ri.Cvar_Get("kt_dlightScale", "1.0", CVAR_ARCHIVE);
	kt_speeds = ri.Cvar_Get("kt_speeds", "0", 0);
	kt_contactShadows = ri.Cvar_Get("kt_contactShadows", "1", CVAR_ARCHIVE); // screen-space contact shadows (sun)
	kt_lightShadows = ri.Cvar_Get("kt_lightShadows", "1", CVAR_ARCHIVE);     // screen-space shadows for sabers / dynamic lights
	kt_dynGlow = ri.Cvar_Get("kt_dynamicGlow", "1", CVAR_ARCHIVE);   // rend2-style dynamic glow strength (glow stages blurred into the bloom)
	kt_ssr = ri.Cvar_Get("kt_ssr", "1", CVAR_ARCHIVE);                 // screen-space reflections (HD worlds)
	kt_volumetric = ri.Cvar_Get("kt_volumetric", "1", CVAR_ARCHIVE);   // sun shafts in the fog
	kt_saberLight = ri.Cvar_Get("kt_saberLight", "1", CVAR_ARCHIVE);   // lightsaber blades as line lights (scale)
	kt_saberGlow = ri.Cvar_Get("kt_saberGlowScale", "1", CVAR_ARCHIVE);   // saber glow brightness in the HDR scene (1 = as authored, like rend2)
	kt_saberDraw = ri.Cvar_Get("kt_saberDraw2", "0", CVAR_ARCHIVE);   // 1: Katarn draws its own procedural blades instead of the game's
}

// ------------------------------------------------------------------------------------------------ math
static void MatMul(float* out, const float* a, const float* b)
{
	float r[16];
	for (int c = 0; c < 4; c++)
		for (int rr = 0; rr < 4; rr++)
			r[c * 4 + rr] = a[0 * 4 + rr] * b[c * 4 + 0] + a[1 * 4 + rr] * b[c * 4 + 1] + a[2 * 4 + rr] * b[c * 4 + 2] + a[3 * 4 + rr] * b[c * 4 + 3];
	memcpy(out, r, 64);
}
static bool MatInverse(const float* m, float* out)
{
	float inv[16];
	inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
	inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
	inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
	inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
	inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
	inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
	inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
	inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
	inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
	inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
	inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
	inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
	inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
	inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
	inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
	inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
	float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
	if (fabsf(det) < 1e-30f) return false;
	det = 1.0f / det;
	for (int i = 0; i < 16; i++) out[i] = inv[i] * det;
	return true;
}
static inline float Dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static inline void Cross3(const float* a, const float* b, float* o) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }
static inline void Norm3(float* a) { float l = sqrtf(Dot3(a, a)); if (l > 0) { a[0] /= l; a[1] /= l; a[2] /= l; } }

// ------------------------------------------------------------------------------------------------ GPU structs
struct ViewConstants {
	float viewProj[16], invViewProj[16], worldToEye[16], prevViewProj[16];
	float camPos[4], viewport[4], invTargetSize[4], projParams[4], sunDir[4], sunColor[4], skyParams[4], fogParams[4], fogColor[4];
	float cascadeViewProj[4][16];
	float cascadeSplits[4], shadowParams[4];
	uint32_t indices0[4], indices1[4], indices2[4];
	float postParams[4];
	float ambientSH[9][4];
	float extraParams[4];      // x overbright scale (2^r_overBrightBits, applied when the frame is presented)
	float viewProjNoJitter[16];   // this frame's view-projection without the TAA jitter
};

struct LVert { float pos[3]; float normal[3]; float uv[2]; float lmuv[2]; uint32_t color; };
static_assert(sizeof(LVert) == 44, "LVert");

struct DeferredRec {
	D3D12_GPU_VIRTUAL_ADDRESS vb, ib, cb;
	uint32_t vbSize, indexCount;
	int cull; bool alpha; bool castShadow;
};

// ------------------------------------------------------------------------------------------------ state
static struct RState {
	bool ready = false;
	uint32_t w = 0, h = 0;
	ID3D12GraphicsCommandList* cl = nullptr;
	// targets
	Texture screen, screenDepth, ldr;
	Texture hdr, depth, gb0, gb1, gb2, ao, aoTmp, shadow, skyLut;
	Texture glow;                    // glow stages (saber blades, lights) drawn again here; blurred into the bloom (rend2 dynamic glow)
	Texture hdrPrev;                 // last frame's lit scene (screen-space reflections)
	bool hdrPrevValid = false;
	std::vector<GpuLight> tubes;     // lightsaber blades this view (line lights)
	struct Blade { float a[3], glowR, b[3], coreR, color[3], intensity; uint32_t tex, mode, alpha, pad; float vcol[3], pad2; };
	std::vector<Blade> blades;       // lightsaber blades to draw
	ID3D12PipelineState* bladePSO = nullptr;
	Texture bloomDown[6], bloomUp[6];
	int bloomLevels = 0;
	Buffer exposure, sh;
	uint32_t lightSrv[kFrames] = {};
	uint32_t shadowRes = 2048;
	// pipelines
	Shader legacyVS, legacyPS, fsVS, presentPS, tonemapPS, fxaaPS, lgVS, lgPS, lgShadowPS;
	std::unordered_map<uint64_t, ID3D12PipelineState*> psos;
	ID3D12PipelineState* presentPSO = nullptr, * tonemapPSO = nullptr, * fxaaPSO = nullptr;
	ID3D12PipelineState* lgPSO[3] = {}, * lgShadowPSO = nullptr, * lgShadowAlphaPSO = nullptr;
	ID3D12PipelineState* csSkyLut = nullptr, * csSkySh = nullptr, * csLighting = nullptr, * csAo = nullptr, * csAoBlur = nullptr;
	ID3D12PipelineState* csHist = nullptr, * csExposure = nullptr, * csBloomDown = nullptr, * csBloomUp = nullptr;
	// frame
	std::vector<LegacyDraw> stream;
	std::vector<DeferredRec> deferred;
	bool inView = false, viewWorld = false, reversedZ = false;
	ViewDesc view;
	float proj[16] = {}, viewProj[16] = {}, prevViewProj[16] = {};
	float vpNoJitter[16] = {}, lastVpNoJitter[16] = {};   // TAA: unjittered, this and the last full-screen world view
	D3D12_GPU_VIRTUAL_ADDRESS viewCB = 0, defaultCB = 0;
	ViewConstants vc;
	int lastMsec = 0;
	float dt = 0.016f;
	bool firstFrame = true;
	float cascadeTexel[4] = {};
	int lastSpeeds = 0;
} R;

static std::string ShaderSource(const char* file)
{
	std::string dir = kt_shaderDir ? kt_shaderDir->string : "";
	if (!dir.empty()) {
		auto readAll = [](const std::string& p, std::string& out) {
			FILE* f = fopen(p.c_str(), "rb");
			if (!f) return false;
			fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
			out.resize(n);
			bool ok = fread(&out[0], 1, n, f) == (size_t)n;
			fclose(f);
			return ok;
		};
		std::string src, common;
		if (readAll(dir + "/" + file + ".hlsl", src) && readAll(dir + "/common.hlsli", common)) {
			size_t at = src.find("#include \"common.hlsli\"");
			if (at != std::string::npos) src.replace(at, strlen("#include \"common.hlsli\""), common);
			return src;
		}
	}
	struct E { const char* name; const char* const* parts; };
	static const E table[] = {
		{ "legacy", kt_src_legacy_parts }, { "present", kt_src_present_parts }, { "world", kt_src_world_parts },
		{ "legacy_gbuffer", kt_src_legacy_gbuffer_parts }, { "sky", kt_src_sky_parts }, { "lighting", kt_src_lighting_parts },
		{ "ssao", kt_src_ssao_parts }, { "post", kt_src_post_parts }, { "saber", kt_src_saber_parts },
	};
	for (auto& e : table) if (!strcmp(e.name, file)) { std::string s; for (int i = 0; e.parts[i]; i++) s += e.parts[i]; return s; }
	return std::string();
}

Shader R_Shader(const char* file, const char* entry, const char* target)
{
	std::string src = ShaderSource(file);
	return CompileShader(file, src.c_str(), entry, target);
}

static D3D12_SHADER_BYTECODE BC(const Shader& s)
{
	D3D12_SHADER_BYTECODE b = {};
	if (s.ok) { b.pShaderBytecode = s.blob->GetBufferPointer(); b.BytecodeLength = s.blob->GetBufferSize(); }
	return b;
}

static D3D12_GRAPHICS_PIPELINE_STATE_DESC BasePso()
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
	d.SampleMask = 0xffffffff;
	d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	d.RasterizerState.FrontCounterClockwise = TRUE;
	d.RasterizerState.DepthClipEnable = TRUE;
	for (auto& rt : d.BlendState.RenderTarget) rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL, rt.SrcBlend = D3D12_BLEND_ONE,
		rt.DestBlend = D3D12_BLEND_ZERO, rt.BlendOp = D3D12_BLEND_OP_ADD, rt.SrcBlendAlpha = D3D12_BLEND_ONE, rt.DestBlendAlpha = D3D12_BLEND_ZERO,
		rt.BlendOpAlpha = D3D12_BLEND_OP_ADD, rt.LogicOp = D3D12_LOGIC_OP_NOOP;
	d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	d.SampleDesc.Count = 1;
	return d;
}

// ------------------------------------------------------------------------------------------------ legacy pipelines
static const D3D12_INPUT_ELEMENT_DESC kLegacyLayout[] = {
	{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
};
static const D3D12_INPUT_ELEMENT_DESC kLgLayout[] = {
	{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
};

enum : uint32_t {   // must match kt_gl.cpp
	PSO_SRC_SHIFT = 0, PSO_DST_SHIFT = 4, PSO_BLEND = 1u << 8,
	PSO_DEPTHTEST = 1u << 9, PSO_DEPTHWRITE = 1u << 10, PSO_DFUNC_SHIFT = 11,
	PSO_CULL_SHIFT = 14, PSO_OFFSET = 1u << 16, PSO_MASK_SHIFT = 17, PSO_LINES = 1u << 21, PSO_WIRE = 1u << 22,
};

static D3D12_BLEND BlendOf(uint32_t c, bool alpha)
{
	static const D3D12_BLEND col[] = { D3D12_BLEND_ZERO, D3D12_BLEND_ONE, D3D12_BLEND_SRC_COLOR, D3D12_BLEND_INV_SRC_COLOR, D3D12_BLEND_SRC_ALPHA,
		D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_DEST_ALPHA, D3D12_BLEND_INV_DEST_ALPHA, D3D12_BLEND_DEST_COLOR, D3D12_BLEND_INV_DEST_COLOR, D3D12_BLEND_SRC_ALPHA_SAT };
	static const D3D12_BLEND alp[] = { D3D12_BLEND_ZERO, D3D12_BLEND_ONE, D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_SRC_ALPHA,
		D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_DEST_ALPHA, D3D12_BLEND_INV_DEST_ALPHA, D3D12_BLEND_DEST_ALPHA, D3D12_BLEND_INV_DEST_ALPHA, D3D12_BLEND_SRC_ALPHA_SAT };
	if (c > 10) c = 1;
	return alpha ? alp[c] : col[c];
}

static ID3D12PipelineState* LegacyPSO(uint32_t bits, int target)   // target 0 screen (RGBA8), 1 HDR (RGBA16F)
{
	uint64_t key = (uint64_t)bits | ((uint64_t)target << 32) | ((uint64_t)(R.reversedZ ? 1 : 0) << 34);
	auto it = R.psos.find(key);
	if (it != R.psos.end()) return it->second;
	D3D12_GRAPHICS_PIPELINE_STATE_DESC d = BasePso();
	d.VS = BC(R.legacyVS); d.PS = BC(R.legacyPS);
	d.InputLayout = { kLegacyLayout, 4 };
	auto& rt = d.BlendState.RenderTarget[0];
	if (bits & PSO_BLEND) {
		rt.BlendEnable = TRUE;
		uint32_t s = (bits >> PSO_SRC_SHIFT) & 15, ds = (bits >> PSO_DST_SHIFT) & 15;
		rt.SrcBlend = BlendOf(s, false); rt.DestBlend = BlendOf(ds, false);
		rt.SrcBlendAlpha = BlendOf(s, true); rt.DestBlendAlpha = BlendOf(ds, true);
	}
	rt.RenderTargetWriteMask = (UINT8)((bits >> PSO_MASK_SHIFT) & 15);
	if (bits & PSO_DEPTHTEST) {
		d.DepthStencilState.DepthEnable = TRUE;
		static const D3D12_COMPARISON_FUNC f[] = { D3D12_COMPARISON_FUNC_NEVER, D3D12_COMPARISON_FUNC_LESS, D3D12_COMPARISON_FUNC_EQUAL,
			D3D12_COMPARISON_FUNC_LESS_EQUAL, D3D12_COMPARISON_FUNC_GREATER, D3D12_COMPARISON_FUNC_NOT_EQUAL, D3D12_COMPARISON_FUNC_GREATER_EQUAL,
			D3D12_COMPARISON_FUNC_ALWAYS };
		d.DepthStencilState.DepthFunc = f[(bits >> PSO_DFUNC_SHIFT) & 7];
		d.DepthStencilState.DepthWriteMask = (bits & PSO_DEPTHWRITE) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
	}
	uint32_t cull = (bits >> PSO_CULL_SHIFT) & 3;
	d.RasterizerState.CullMode = cull == 1 ? D3D12_CULL_MODE_FRONT : cull == 2 ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
	if (cull == 3) d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;   // front and back: GL draws nothing; rare
	if (bits & PSO_OFFSET) {
		d.RasterizerState.DepthBias = R.reversedZ ? 2 : -2;
		d.RasterizerState.SlopeScaledDepthBias = R.reversedZ ? 1.0f : -1.0f;
		d.RasterizerState.DepthBiasClamp = 0.0f;
	}
	if (bits & PSO_WIRE) d.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
	d.PrimitiveTopologyType = (bits & PSO_LINES) ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	d.NumRenderTargets = target == 1 ? 2 : 1;   // HDR: + the glow target (same blend)
	d.RTVFormats[0] = target == 1 ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
	if (target == 1) d.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
	d.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	ID3D12PipelineState* p = CreateGraphicsPSO(d, "legacy");
	R.psos[key] = p;
	return p;
}

static ID3D12PipelineState* FullscreenPSO(const Shader& ps, DXGI_FORMAT fmt, const char* name)
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC d = BasePso();
	d.VS = BC(R.fsVS); d.PS = BC(ps);
	d.NumRenderTargets = 1;
	d.RTVFormats[0] = fmt;
	return CreateGraphicsPSO(d, name);
}

static bool CreatePipelines()
{
	R.legacyVS = R_Shader("legacy", "VS", "vs_5_1");
	R.legacyPS = R_Shader("legacy", "PS", "ps_5_1");
	R.fsVS = R_Shader("present", "FullscreenVS", "vs_5_1");
	R.presentPS = R_Shader("present", "PS", "ps_5_1");
	R.tonemapPS = R_Shader("post", "TonemapPS", "ps_5_1");
	R.fxaaPS = R_Shader("post", "FxaaPS", "ps_5_1");
	R.lgVS = R_Shader("legacy_gbuffer", "VS", "vs_5_1");
	R.lgPS = R_Shader("legacy_gbuffer", "PS", "ps_5_1");
	R.lgShadowPS = R_Shader("legacy_gbuffer", "ShadowPS", "ps_5_1");
	{
		// compile everything once up front so one run reports every shader error in katarn_log.txt
		const char* all[][3] = { { "world", "VS", "vs_5_1" }, { "world", "PS", "ps_5_1" }, { "world", "ShadowPS", "ps_5_1" },
			{ "sky", "LutCS", "cs_5_1" }, { "sky", "ShCS", "cs_5_1" }, { "lighting", "CS", "cs_5_1" }, { "ssao", "AoCS", "cs_5_1" },
			{ "ssao", "BlurCS", "cs_5_1" }, { "post", "HistogramCS", "cs_5_1" }, { "post", "ExposureCS", "cs_5_1" },
			{ "post", "BloomDownCS", "cs_5_1" }, { "post", "BloomUpCS", "cs_5_1" } };
		int bad = 0;
		for (auto& e : all) { Shader s = R_Shader(e[0], e[1], e[2]); if (!s.ok) bad++; if (s.blob) s.blob->Release(); }
		Shader* mine[] = { &R.legacyVS, &R.legacyPS, &R.fsVS, &R.presentPS, &R.tonemapPS, &R.fxaaPS, &R.lgVS, &R.lgPS, &R.lgShadowPS };
		for (auto* s : mine) if (!s->ok) bad++;
		if (bad) { Log("^1KatarnEngine: %d shaders failed to compile\n", bad); return false; }
	}
	R.presentPSO = FullscreenPSO(R.presentPS, DXGI_FORMAT_R8G8B8A8_UNORM, "present");
	R.tonemapPSO = FullscreenPSO(R.tonemapPS, DXGI_FORMAT_R8G8B8A8_UNORM, "tonemap");
	{
		Shader bvs = R_Shader("saber", "VS", "vs_5_1"), bps = R_Shader("saber", "PS", "ps_5_1");
		if (bvs.ok && bps.ok) {
			D3D12_GRAPHICS_PIPELINE_STATE_DESC d = BasePso();
			d.VS = BC(bvs); d.PS = BC(bps);
			auto& rt = d.BlendState.RenderTarget[0];
			// premultiplied: additive parts write a = 0, alpha-blended cores (dark blades) a = coverage
			rt.BlendEnable = TRUE; rt.SrcBlend = D3D12_BLEND_ONE; rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
			rt.SrcBlendAlpha = D3D12_BLEND_ZERO; rt.DestBlendAlpha = D3D12_BLEND_ONE;
			d.DepthStencilState.DepthEnable = FALSE;
			d.NumRenderTargets = 1;
			d.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
			R.bladePSO = CreateGraphicsPSO(d, "saber blades");
		}
	}
	R.fxaaPSO = FullscreenPSO(R.fxaaPS, DXGI_FORMAT_R8G8B8A8_UNORM, "fxaa");

	// legacy -> GBuffer, one per cull mode
	for (int c = 0; c < 3; c++) {
		D3D12_GRAPHICS_PIPELINE_STATE_DESC d = BasePso();
		d.VS = BC(R.lgVS); d.PS = BC(R.lgPS);
		d.InputLayout = { kLgLayout, 5 };
		d.RasterizerState.CullMode = c == 1 ? D3D12_CULL_MODE_FRONT : c == 2 ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
		d.DepthStencilState.DepthEnable = TRUE;
		d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
		d.NumRenderTargets = 3;
		d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; d.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT; d.RTVFormats[2] = DXGI_FORMAT_R11G11B10_FLOAT;
		d.DSVFormat = DXGI_FORMAT_D32_FLOAT;
		R.lgPSO[c] = CreateGraphicsPSO(d, "legacy gbuffer");
	}
	for (int a = 0; a < 2; a++) {
		D3D12_GRAPHICS_PIPELINE_STATE_DESC d = BasePso();
		d.VS = BC(R.lgVS);
		if (a) d.PS = BC(R.lgShadowPS);
		d.InputLayout = { kLgLayout, 5 };
		d.DepthStencilState.DepthEnable = TRUE;
		d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		d.RasterizerState.DepthBias = 1000; d.RasterizerState.SlopeScaledDepthBias = 2.0f; d.RasterizerState.DepthBiasClamp = 0.01f;
		d.NumRenderTargets = 0;
		d.DSVFormat = DXGI_FORMAT_D32_FLOAT;
		(a ? R.lgShadowAlphaPSO : R.lgShadowPSO) = CreateGraphicsPSO(d, "legacy shadow");
	}
	R.csSkyLut = CreateComputePSO(R_Shader("sky", "LutCS", "cs_5_1"), "sky lut");
	R.csSkySh = CreateComputePSO(R_Shader("sky", "ShCS", "cs_5_1"), "sky sh");
	R.csLighting = CreateComputePSO(R_Shader("lighting", "CS", "cs_5_1"), "lighting");
	R.csAo = CreateComputePSO(R_Shader("ssao", "AoCS", "cs_5_1"), "ssao");
	R.csAoBlur = CreateComputePSO(R_Shader("ssao", "BlurCS", "cs_5_1"), "ssao blur");
	R.csHist = CreateComputePSO(R_Shader("post", "HistogramCS", "cs_5_1"), "histogram");
	R.csExposure = CreateComputePSO(R_Shader("post", "ExposureCS", "cs_5_1"), "exposure");
	R.csBloomDown = CreateComputePSO(R_Shader("post", "BloomDownCS", "cs_5_1"), "bloom down");
	R.csBloomUp = CreateComputePSO(R_Shader("post", "BloomUpCS", "cs_5_1"), "bloom up");
	return R.presentPSO != nullptr;
}

// ------------------------------------------------------------------------------------------------ targets
static void DestroyTargets()
{
	Texture* all[] = { &R.screen, &R.screenDepth, &R.ldr, &R.hdr, &R.glow, &R.hdrPrev, &R.depth, &R.gb0, &R.gb1, &R.gb2, &R.ao, &R.aoTmp, &R.shadow, &R.skyLut };
	for (Texture* t : all) DestroyTexture(*t);
	for (auto& t : R.bloomDown) DestroyTexture(t);
	for (auto& t : R.bloomUp) DestroyTexture(t);
}

static bool CreateTargets(uint32_t w, uint32_t h)
{
	const float black[4] = { 0, 0, 0, 0 };
	const float zero[1] = { 0.0f };
	const float one[1] = { 1.0f };
	bool ok = true;
	ok &= CreateTexture(R.screen, w, h, 1, DXGI_FORMAT_R8G8B8A8_UNORM, TEX_RT, 1, black, "screen");
	ok &= CreateTexture(R.screenDepth, w, h, 1, DXGI_FORMAT_D32_FLOAT, TEX_DEPTH, 1, zero, "screen depth");
	ok &= CreateTexture(R.ldr, w, h, 1, DXGI_FORMAT_R8G8B8A8_UNORM, TEX_RT, 1, black, "ldr");
	ok &= CreateTexture(R.hdr, w, h, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, TEX_RT | TEX_UAV, 1, black, "hdr");
	ok &= CreateTexture(R.glow, w, h, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, TEX_RT, 1, black, "glow");
	ok &= CreateTexture(R.hdrPrev, w, h, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 1, nullptr, "hdr previous");
	R.hdrPrevValid = false;
	ok &= CreateTexture(R.depth, w, h, 1, DXGI_FORMAT_D32_FLOAT, TEX_DEPTH, 1, zero, "depth");
	ok &= CreateTexture(R.gb0, w, h, 1, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, TEX_RT, 1, black, "gbuffer albedo");
	ok &= CreateTexture(R.gb1, w, h, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, TEX_RT, 1, black, "gbuffer normal");
	ok &= CreateTexture(R.gb2, w, h, 1, DXGI_FORMAT_R11G11B10_FLOAT, TEX_RT, 1, black, "gbuffer emissive");
	ok &= CreateTexture(R.ao, w, h, 1, DXGI_FORMAT_R16_FLOAT, TEX_UAV, 1, nullptr, "ao");
	ok &= CreateTexture(R.aoTmp, w, h, 1, DXGI_FORMAT_R16_FLOAT, TEX_UAV, 1, nullptr, "ao tmp");
	R.shadowRes = (uint32_t)Q_max(512, Q_min(8192, kt_shadowRes->integer));
	ok &= CreateTexture(R.shadow, R.shadowRes, R.shadowRes, 1, DXGI_FORMAT_D32_FLOAT, TEX_DEPTH | TEX_ARRAY, 4, one, "sun shadows");
	ok &= CreateTexture(R.skyLut, 128, 64, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, TEX_UAV, 1, nullptr, "sky lut");
	R.bloomLevels = 0;
	uint32_t bw = w / 2, bh = h / 2;
	for (int i = 0; i < 6 && bw >= 4 && bh >= 4; i++, bw /= 2, bh /= 2) {
		ok &= CreateTexture(R.bloomDown[i], bw, bh, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, TEX_UAV, 1, nullptr, "bloom down");
		ok &= CreateTexture(R.bloomUp[i], bw, bh, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, TEX_UAV, 1, nullptr, "bloom up");
		R.bloomLevels++;
	}
	R.w = w; R.h = h;
	return ok;
}

// ------------------------------------------------------------------------------------------------ init
void InstallCrashHandler();
bool R_Init(void* hwnd, int width, int height)
{
	InstallCrashHandler();
	RegisterCvars();
	Config cfg;
	cvar_t* swap = ri.Cvar_Get("r_swapInterval", "0", CVAR_ARCHIVE);
	cfg.vsync = swap->integer != 0;
	cfg.debug = kt_debug->integer != 0;
	std::string err;
	if (!Init((HWND)hwnd, width, height, cfg, err)) {
		Log("^1KatarnEngine: Direct3D 12 start failed: %s\n", err.c_str());
		return false;
	}
	char cacheDir[1024];
	Com_sprintf(cacheDir, sizeof(cacheDir), "%s/katarn_shadercache", ri.Cvar_VariableString("fs_homepath"));
	CreateDirectoryA(cacheDir, nullptr);
	SetShaderCacheDir(cacheDir);
	if (!CreatePipelines()) { Log("^1KatarnEngine: shaders failed to build - see katarn_log.txt\n"); return false; }
	if (!CreateTargets(width, height)) { Log("^1KatarnEngine: render targets failed\n"); return false; }
	CreateBuffer(R.exposure, 512, true, "exposure");
	{
		float init[128] = {};
		init[0] = 1.0f; init[1] = 0.18f;
		UploadBuffer(R.exposure, 0, init, sizeof(init));
	}
	CreateBuffer(R.sh, 256, true, "sky sh");
	for (int i = 0; i < kFrames; i++) R.lightSrv[i] = AllocDescriptor();
	R.ready = true;
	R.firstFrame = true;
	GL_ResetState();
	Log("KatarnEngine: %dx%d, shadows %u, ready\n", width, height, R.shadowRes);
	return true;
}

void R_Shutdown()
{
	if (!g.dev) return;
	if (R.cl) { SubmitList(R.cl); R.cl = nullptr; }
	WaitIdle();
	World_Shutdown();
	GL_ShutdownTextures();
	DestroyTargets();
	DestroyBuffer(R.exposure);
	DestroyBuffer(R.sh);
	for (auto& kv : R.psos) if (kv.second) kv.second->Release();
	R.psos.clear();
	ID3D12PipelineState* ps[] = { R.presentPSO, R.tonemapPSO, R.fxaaPSO, R.lgPSO[0], R.lgPSO[1], R.lgPSO[2], R.lgShadowPSO, R.lgShadowAlphaPSO,
		R.csSkyLut, R.csSkySh, R.csLighting, R.csAo, R.csAoBlur, R.csHist, R.csExposure, R.csBloomDown, R.csBloomUp };
	for (auto* p : ps) if (p) p->Release();
	Shader* sh[] = { &R.legacyVS, &R.legacyPS, &R.fsVS, &R.presentPS, &R.tonemapPS, &R.fxaaPS, &R.lgVS, &R.lgPS, &R.lgShadowPS };
	for (auto* s : sh) if (s->blob) s->blob->Release();
	WaitIdle();
	R = RState();
	Shutdown();
}

// ------------------------------------------------------------------------------------------------ frame
static void EnsureList()
{
	if (!R.cl) {
		R.cl = OpenList();
		Transition(R.cl, R.screen, D3D12_RESOURCE_STATE_RENDER_TARGET);
		Transition(R.cl, R.screenDepth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		if (R.firstFrame) {
			const float c[4] = { 0, 0, 0, 1 };
			R.cl->ClearRenderTargetView(R.screen.rtv[0], c, 0, nullptr);
			R.firstFrame = false;
		}
		// a default view constant buffer so every shader that declares it reads something valid
		UploadAlloc a = Upload(sizeof(ViewConstants), 256);
		memset(a.cpu, 0, sizeof(ViewConstants));
		ViewConstants* v = (ViewConstants*)a.cpu;
		v->indices1[3] = R.exposure.srv;
		R.defaultCB = a.gpu;
		R.cl->SetGraphicsRootConstantBufferView(RP_VIEW_CBV, R.defaultCB);
		R.cl->SetGraphicsRootConstantBufferView(RP_EXTRA_CBV, R.defaultCB);
	}
}

static void SetViewportGL(ID3D12GraphicsCommandList* cl, const int* vp, const int* sc, float dmin, float dmax, uint32_t th)
{
	D3D12_VIEWPORT v = { (float)vp[0], (float)(int)(th - (vp[1] + vp[3])), (float)vp[2], (float)vp[3], dmin, dmax };
	cl->RSSetViewports(1, &v);
	LONG l = Q_max(sc[0], vp[0]), r = Q_min(sc[0] + sc[2], vp[0] + vp[2]);
	LONG b = Q_max(sc[1], vp[1]), t = Q_min(sc[1] + sc[3], vp[1] + vp[3]);
	if (r < l) r = l;
	if (t < b) t = b;
	D3D12_RECT rc = { l, (LONG)th - t, r, (LONG)th - b };
	if (rc.top < 0) rc.top = 0;
	cl->RSSetScissorRects(1, &rc);
}

// replays the recorded legacy draws into target 0 (screen) or 1 (HDR scene)
static void FlushStream(int target)
{
	if (R.stream.empty()) return;
	EnsureList();
	ID3D12GraphicsCommandList* cl = R.cl;
	Texture& rt = target == 1 ? R.hdr : R.screen;
	Texture& ds = target == 1 ? R.depth : R.screenDepth;
	Transition(cl, rt, D3D12_RESOURCE_STATE_RENDER_TARGET);
	Transition(cl, ds, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	if (target == 1) {
		Transition(cl, R.glow, D3D12_RESOURCE_STATE_RENDER_TARGET);
		D3D12_CPU_DESCRIPTOR_HANDLE two[2] = { rt.rtv[0], R.glow.rtv[0] };
		cl->OMSetRenderTargets(2, two, FALSE, &ds.dsv[0]);
	}
	else cl->OMSetRenderTargets(1, &rt.rtv[0], FALSE, &ds.dsv[0]);
	if (target == 1) cl->SetGraphicsRootConstantBufferView(RP_VIEW_CBV, R.viewCB);
	else cl->SetGraphicsRootConstantBufferView(RP_VIEW_CBV, R.defaultCB);
	ID3D12PipelineState* last = nullptr;
	D3D_PRIMITIVE_TOPOLOGY lastTopo = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
	float glow = kt_glowBoost->value;
	for (const LegacyDraw& d : R.stream) {
		if (d.clear) {
			int y0 = (int)rt.height - (d.sc[1] + d.sc[3]);
			D3D12_RECT rc = { d.sc[0], Q_max(0, y0), d.sc[0] + d.sc[2], y0 + d.sc[3] };
			if (d.clear & 1) cl->ClearRenderTargetView(rt.rtv[0], d.clearColor, 1, &rc);
			if (d.clear & 2) cl->ClearDepthStencilView(ds.dsv[0], D3D12_CLEAR_FLAG_DEPTH, R.reversedZ ? 1.0f - d.clearDepth : d.clearDepth, 0, 1, &rc);
			continue;
		}
		ID3D12PipelineState* p = LegacyPSO(d.pso, target);
		if (!p) continue;
		if (p != last) { cl->SetPipelineState(p); last = p; }
		D3D_PRIMITIVE_TOPOLOGY topo = (d.pso & PSO_LINES) ? D3D_PRIMITIVE_TOPOLOGY_LINELIST : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		if (topo != lastTopo) { cl->IASetPrimitiveTopology(topo); lastTopo = topo; }
		SetViewportGL(cl, d.vp, d.sc, d.depthMin, d.depthMax, rt.height);
		uint32_t c[32] = {};
		memcpy(c, d.mvp, 64);
		GLTexture* t0 = (d.flags & LF_TEX0) ? R_GLTexture(d.tex0, false) : nullptr;
		GLTexture* t1 = (d.flags & LF_TEX1) ? R_GLTexture(d.tex1, false) : nullptr;
		R_ResolveTexture(t0); R_ResolveTexture(t1);
		uint32_t flags = d.flags;
		c[16] = R_TextureSrv(t0, target == 1);
		c[17] = R_TextureSrv(t1, target == 1);
		if (!c[16]) flags &= ~LF_TEX0;
		if (!c[17]) flags &= ~LF_TEX1;
		c[18] = flags;
		memcpy(&c[19], &d.alphaRef, 4);
		c[20] = (uint32_t)target;
		memcpy(&c[21], &glow, 4);
		{ float sc = d.scale > 0 ? d.scale : 1.0f; memcpy(&c[22], &sc, 4); }
		cl->SetGraphicsRoot32BitConstants(RP_CONSTANTS, 24, c, 0);
		D3D12_VERTEX_BUFFER_VIEW vbv = { d.vb, d.vbSize, 32 };
		D3D12_INDEX_BUFFER_VIEW ibv = { d.ib, d.indexCount * 4, DXGI_FORMAT_R32_UINT };
		cl->IASetVertexBuffers(0, 1, &vbv);
		cl->IASetIndexBuffer(&ibv);
		cl->DrawIndexedInstanced(d.indexCount, 1, 0, 0, 0);
	}
	R.stream.clear();
}

void R_AppendLegacy(const LegacyDraw& d)
{
	if (!R.ready) return;
	EnsureList();
	R.stream.push_back(d);
	if (!R.inView && R.stream.size() > 4096) FlushStream(0);
}

void R_BeginFrame()
{
	if (!R.ready) return;
	EnsureList();
	int now = ri.Milliseconds();
	R.dt = R.lastMsec ? Q_min(0.25f, Q_max(0.0005f, (now - R.lastMsec) * 0.001f)) : 0.016f;
	R.lastMsec = now;
}

void R_EndFrame()
{
	if (!R.ready) return;
	EnsureList();
	FlushStream(0);
	ID3D12GraphicsCommandList* cl = R.cl;
	Texture& bb = CurrentBackbuffer();
	Transition(cl, R.screen, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	Transition(cl, bb, D3D12_RESOURCE_STATE_RENDER_TARGET);
	cl->OMSetRenderTargets(1, &bb.rtv[0], FALSE, nullptr);
	D3D12_VIEWPORT v = { 0, 0, (float)g.width, (float)g.height, 0, 1 };
	D3D12_RECT rc = { 0, 0, (LONG)g.width, (LONG)g.height };
	cl->RSSetViewports(1, &v);
	cl->RSSetScissorRects(1, &rc);
	cl->SetPipelineState(R.presentPSO);
	cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	float gamma = r_gamma ? Q_max(0.5f, Q_min(3.0f, r_gamma->value)) : 1.0f;
	float bright = 1.0f;
	uint32_t c[4] = { R.screen.srv, 0, 0, 0 };
	memcpy(&c[1], &gamma, 4);
	memcpy(&c[2], &bright, 4);
	cl->SetGraphicsRoot32BitConstants(RP_CONSTANTS, 4, c, 0);
	cl->DrawInstanced(3, 1, 0, 0);
	Transition(cl, bb, D3D12_RESOURCE_STATE_PRESENT);
	Transition(cl, R.screen, D3D12_RESOURCE_STATE_RENDER_TARGET);
	SubmitList(cl);
	R.cl = nullptr;
	Present();
}

bool R_InView() { return R.inView; }
bool R_ViewIsWorld() { return R.inView && R.viewWorld; }
bool R_ReversedZ() { return R.reversedZ; }
const float* R_ViewProjection() { return R.proj; }

const float* R_LegacyMVP(const float* modelView)
{
	static float m[16];
	MatMul(m, R.proj, modelView);
	return m;
}

void R_Begin2D()
{
	R.reversedZ = false;
}

void R_BeginView(const ViewDesc& v)
{
	if (!R.ready) return;
	EnsureList();
	FlushStream(0);
	R.view = v;
	R.inView = true;
	R.viewWorld = v.world;
	R.reversedZ = true;
	// reversed-Z infinite perspective in GL eye space
	float sx = 1.0f / tanf(v.fovX * (float)M_PI / 360.0f);
	float sy = 1.0f / tanf(v.fovY * (float)M_PI / 360.0f);
	memset(R.proj, 0, 64);
	R.proj[0] = sx; R.proj[5] = sy; R.proj[11] = -1.0f; R.proj[14] = v.zNear;
	MatMul(R.vpNoJitter, R.proj, v.worldToEye);
	memcpy(R.prevViewProj, R.viewProj, 64);
	MatMul(R.viewProj, R.proj, v.worldToEye);
	R.deferred.clear();
	R.tubes.clear();
	R.blades.clear();

	// view constants
	ViewConstants& c = R.vc;
	memset(&c, 0, sizeof(c));
	memcpy(c.viewProj, R.viewProj, 64);
	MatInverse(R.viewProj, c.invViewProj);
	memcpy(c.worldToEye, v.worldToEye, 64);
	memcpy(c.prevViewProj, R.prevViewProj, 64);
	memcpy(c.viewProjNoJitter, R.vpNoJitter, 64);
	c.camPos[0] = v.origin[0]; c.camPos[1] = v.origin[1]; c.camPos[2] = v.origin[2]; c.camPos[3] = v.zNear;
	c.viewport[0] = (float)v.x; c.viewport[1] = (float)((int)R.h - (v.y + v.h)); c.viewport[2] = (float)v.w; c.viewport[3] = (float)v.h;
	c.invTargetSize[0] = 1.0f / R.w; c.invTargetSize[1] = 1.0f / R.h; c.invTargetSize[2] = (float)R.w; c.invTargetSize[3] = (float)R.h;
	c.projParams[0] = sx; c.projParams[1] = sy; c.projParams[2] = v.zNear; c.projParams[3] = v.time;
	const WorldEnv& env = World_Env();
	float sd[3] = { env.sunDir[0], env.sunDir[1], env.sunDir[2] };
	Norm3(sd);
	c.sunDir[0] = sd[0]; c.sunDir[1] = sd[1]; c.sunDir[2] = sd[2]; c.sunDir[3] = Q_max(0.004f, env.sunAngle);
	c.sunColor[0] = env.sunColor[0]; c.sunColor[1] = env.sunColor[1]; c.sunColor[2] = env.sunColor[2];
	c.sunColor[3] = (kt_sun->integer && World_Active()) ? env.sunIntensity : 0.0f;   // old BSP maps: their own baked light only
	c.skyParams[0] = env.haze; c.skyParams[1] = env.skyIntensity; c.skyParams[2] = env.ambient; c.skyParams[3] = World_Active() ? 1.0f : 0.0f;
	if (env.envSrv) { c.indices2[0] = env.envSrv; c.skyParams[1] = env.envStrength; float rs = env.envLight / Q_max(1e-4f, env.envStrength); memcpy(&c.indices2[2], &rs, 4); }
	if (env.hasSH) { c.indices2[1] = 1; for (int k = 0; k < 9; k++) for (int q = 0; q < 3; q++) c.ambientSH[k][q] = env.sh[k][q]; }
	if (kt_fog->integer) {
		c.fogParams[0] = env.fogDensity * Q_max(0.0f, kt_fogScale->value); c.fogParams[1] = env.fogFalloff; c.fogParams[2] = env.fogHeight; c.fogParams[3] = env.fogStart;
	}
	c.fogColor[0] = env.fogColor[0]; c.fogColor[1] = env.fogColor[1]; c.fogColor[2] = env.fogColor[2]; c.fogColor[3] = env.fogMax;
	c.indices0[0] = R.gb0.srv; c.indices0[1] = R.gb1.srv; c.indices0[2] = R.gb2.srv; c.indices0[3] = R.depth.srv;
	c.indices1[0] = R.shadow.srv; c.indices1[1] = R.ao.srv; c.indices1[2] = R.skyLut.srv; c.indices1[3] = R.exposure.srv;
	c.indices2[3] = (uint32_t)g.frameNumber;
	c.postParams[0] = kt_exposure->value + env.exposureEV;
	c.postParams[1] = kt_bloom->value;
	c.postParams[2] = kt_manualExposure->value;
	c.postParams[3] = kt_glowBoost->value;
	c.extraParams[0] = (float)(1 << tr.overbrightBits);
	c.extraParams[1] = kt_tonemap->integer ? 1.0f : 0.0f;
	if (!World_Active()) c.postParams[2] = 1.0f;   // legacy maps: fixed exposure (their content is display-referred)

	// sun cascades
	float shadowDist = Q_max(500.0f, kt_shadowDist->value);
	bool shadows = v.world && kt_shadows->integer && World_Active() && c.sunColor[3] > 0 && sd[2] > -0.05f;
	if (shadows) {
		float n = Q_max(4.0f, v.zNear), f = shadowDist;
		float splits[5] = { n };
		for (int i = 1; i <= 4; i++) {
			float t = i / 4.0f;
			splits[i] = 0.82f * n * powf(f / n, t) + 0.18f * (n + (f - n) * t);
		}
		float tx = tanf(v.fovX * (float)M_PI / 360.0f), ty = tanf(v.fovY * (float)M_PI / 360.0f);
		float fwd[3] = { v.axis[0][0], v.axis[0][1], v.axis[0][2] };
		float lft[3] = { v.axis[1][0], v.axis[1][1], v.axis[1][2] };
		float up[3] = { v.axis[2][0], v.axis[2][1], v.axis[2][2] };
		float L[3] = { -sd[0], -sd[1], -sd[2] };        // direction light travels
		float ref[3] = { 0, 0, 1 };
		if (fabsf(L[2]) > 0.99f) { ref[0] = 1; ref[2] = 0; }
		float rgt[3], upl[3];
		Cross3(L, ref, rgt); Norm3(rgt);
		Cross3(rgt, L, upl); Norm3(upl);
		for (int ci = 0; ci < 4; ci++) {
			float d0 = splits[ci], d1 = splits[ci + 1];
			// bounding sphere of the frustum slice (stable under rotation)
			float corners[8][3];
			int k = 0;
			for (int s = 0; s < 2; s++) {
				float d = s ? d1 : d0;
				for (int yy = -1; yy <= 1; yy += 2)
					for (int xx = -1; xx <= 1; xx += 2, k++)
						for (int a = 0; a < 3; a++)
							corners[k][a] = v.origin[a] + fwd[a] * d - lft[a] * (xx * tx * d) + up[a] * (yy * ty * d);
			}
			float ctr[3] = { 0, 0, 0 };
			for (int i = 0; i < 8; i++) for (int a = 0; a < 3; a++) ctr[a] += corners[i][a] / 8.0f;
			float rad = 0;
			for (int i = 0; i < 8; i++) {
				float dd[3] = { corners[i][0] - ctr[0], corners[i][1] - ctr[1], corners[i][2] - ctr[2] };
				rad = Q_max(rad, sqrtf(Dot3(dd, dd)));
			}
			rad = ceilf(rad / 16.0f) * 16.0f;
			float texel = 2.0f * rad / R.shadowRes;
			R.cascadeTexel[ci] = texel;
			// snap the centre to whole texels in light space
			float cx = Dot3(ctr, rgt), cy = Dot3(ctr, upl), cz = Dot3(ctr, L);
			cx = floorf(cx / texel) * texel; cy = floorf(cy / texel) * texel;
			float back = 30000.0f;                // casters up to this far towards the sun
			float zr = rad + back + rad;
			float* m = c.cascadeViewProj[ci];
			memset(m, 0, 64);
			// rows: x = (p.rgt - cx)/rad, y = (p.upl - cy)/rad, z = (p.L - (cz - rad - back)) / zr
			m[0] = rgt[0] / rad; m[4] = rgt[1] / rad; m[8] = rgt[2] / rad; m[12] = -cx / rad;
			m[1] = upl[0] / rad; m[5] = upl[1] / rad; m[9] = upl[2] / rad; m[13] = -cy / rad;
			m[2] = L[0] / zr; m[6] = L[1] / zr; m[10] = L[2] / zr; m[14] = -(cz - rad - back) / zr;
			m[15] = 1.0f;
			c.cascadeSplits[ci] = d1;
			if (ci == 0) {
				c.shadowParams[0] = 1.0f / R.shadowRes;
				c.shadowParams[1] = 0.0004f;
				c.shadowParams[2] = texel * 1.2f;
			}
		}
		c.shadowParams[3] = 1.0f;
	}
	UploadAlloc a = Upload(sizeof(ViewConstants), 256);
	memcpy(a.cpu, &c, sizeof(c));
	R.viewCB = a.gpu;
}

void R_AppendDeferred(const DeferredDraw& d)
{
	if (!R.inView || !d.numIndexes || !d.numVerts) return;
	UploadAlloc vb = Upload((uint64_t)d.numVerts * sizeof(LVert), 16);
	UploadAlloc ib = Upload((uint64_t)d.numIndexes * 4, 4);
	UploadAlloc cb = Upload(256, 256);
	if (!vb.cpu || !ib.cpu || !cb.cpu) return;
	LVert* v = (LVert*)vb.cpu;
	for (int i = 0; i < d.numVerts; i++) {
		const float* p = (const float*)((const uint8_t*)d.xyz + (size_t)i * d.xyzStride);
		const float* n = (const float*)((const uint8_t*)d.normal + (size_t)i * d.normalStride);
		LVert& o = v[i];
		o.pos[0] = p[0]; o.pos[1] = p[1]; o.pos[2] = p[2];
		o.normal[0] = n[0]; o.normal[1] = n[1]; o.normal[2] = n[2];
		o.uv[0] = d.uv[i * 2]; o.uv[1] = d.uv[i * 2 + 1];
		if (d.lmuv) { const float* l = (const float*)((const uint8_t*)d.lmuv + (size_t)i * d.lmuvStride); o.lmuv[0] = l[0]; o.lmuv[1] = l[1]; }
		else o.lmuv[0] = o.lmuv[1] = 0;
		o.color = d.colors ? (d.colors[i * 4] | (d.colors[i * 4 + 1] << 8) | (d.colors[i * 4 + 2] << 16) | ((uint32_t)d.colors[i * 4 + 3] << 24)) : 0xffffffffu;
	}
	memcpy(ib.cpu, d.indexes, (size_t)d.numIndexes * 4);
	// per-draw constants (b2): mvp, object rows, textures, material
	uint8_t* c = (uint8_t*)cb.cpu;
	memcpy(c, d.mvp, 64);
	memcpy(c + 64, d.objToWorld, 48);
	GLTexture* ta = d.albedoTex ? R_GLTexture(d.albedoTex, false) : nullptr;
	GLTexture* tl = d.lightmapTex ? R_GLTexture(d.lightmapTex, false) : nullptr;
	GLTexture* ts = d.specTex ? R_GLTexture(d.specTex, false) : nullptr;
	R_ResolveTexture(ta); R_ResolveTexture(tl); R_ResolveTexture(ts);
	uint32_t u[4] = { R_TextureSrv(ta, true), R_TextureSrv(tl, true), d.flags, 0 };
	memcpy(&u[3], &d.alphaRef, 4);
	memcpy(c + 112, u, 16);
	float f[4] = { d.roughness, d.metallic, d.bakedScale, 0 };
	uint32_t spec = R_TextureSrv(ts, false);
	memcpy(c + 128, f, 12);
	memcpy(c + 140, &spec, 4);
	memcpy(c + 144, d.tint, 16);
	{
		GLTexture* tn = d.normalTex ? R_GLTexture(d.normalTex, false) : nullptr;
		GLTexture* tr2 = d.rmoTex ? R_GLTexture(d.rmoTex, false) : nullptr;
		R_ResolveTexture(tn); R_ResolveTexture(tr2);
		uint32_t pbr[4] = { R_TextureSrv(tn, false), R_TextureSrv(tr2, false), (uint32_t)d.rmoOrder, 0 };
		memcpy(&pbr[3], &d.skin, 4);
		memcpy(c + 160, pbr, 16);
		float grid[12] = { d.gridAmbient[0], d.gridAmbient[1], d.gridAmbient[2], 0, d.gridDirected[0], d.gridDirected[1], d.gridDirected[2], 0,
			d.gridDir[0], d.gridDir[1], d.gridDir[2], 0 };
		memcpy(c + 176, grid, 48);
	}
	DeferredRec r;
	r.vb = vb.gpu; r.ib = ib.gpu; r.cb = cb.gpu;
	r.vbSize = d.numVerts * sizeof(LVert);
	r.indexCount = d.numIndexes;
	r.cull = d.cull;
	r.alpha = (d.flags & 1) != 0;
	r.castShadow = d.castShadow;
	R.deferred.push_back(r);
}

static void DrawDeferredList(ID3D12GraphicsCommandList* cl, bool shadow, int cascade)
{
	cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	uint32_t rc[2] = { (uint32_t)cascade, shadow ? 1u : 0u };
	cl->SetGraphicsRoot32BitConstants(RP_CONSTANTS, 2, rc, 0);
	ID3D12PipelineState* last = nullptr;
	for (const DeferredRec& r : R.deferred) {
		if (shadow && !r.castShadow) continue;
		ID3D12PipelineState* p = shadow ? (r.alpha ? R.lgShadowAlphaPSO : R.lgShadowPSO) : R.lgPSO[r.cull];
		if (!p) continue;
		if (p != last) { cl->SetPipelineState(p); last = p; }
		cl->SetGraphicsRootConstantBufferView(RP_EXTRA_CBV, r.cb);
		D3D12_VERTEX_BUFFER_VIEW vbv = { r.vb, r.vbSize, sizeof(LVert) };
		D3D12_INDEX_BUFFER_VIEW ibv = { r.ib, r.indexCount * 4, DXGI_FORMAT_R32_UINT };
		cl->IASetVertexBuffers(0, 1, &vbv);
		cl->IASetIndexBuffer(&ibv);
		cl->DrawIndexedInstanced(r.indexCount, 1, 0, 0, 0);
	}
}

static void Dispatch(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* p, const uint32_t* consts, int n, uint32_t gx, uint32_t gy)
{
	if (!p) return;
	cl->SetPipelineState(p);
	cl->SetComputeRoot32BitConstants(RP_CONSTANTS, n, consts, 0);
	cl->Dispatch(gx, gy, 1);
}

static void FullscreenPass(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* p, Texture& rt, const uint32_t* consts, int n)
{
	cl->OMSetRenderTargets(1, &rt.rtv[0], FALSE, nullptr);
	D3D12_VIEWPORT vp = { R.vc.viewport[0], R.vc.viewport[1], R.vc.viewport[2], R.vc.viewport[3], 0, 1 };
	D3D12_RECT rc = { (LONG)vp.TopLeftX, (LONG)vp.TopLeftY, (LONG)(vp.TopLeftX + vp.Width), (LONG)(vp.TopLeftY + vp.Height) };
	cl->RSSetViewports(1, &vp);
	cl->RSSetScissorRects(1, &rc);
	cl->SetPipelineState(p);
	cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cl->SetGraphicsRoot32BitConstants(RP_CONSTANTS, n, consts, 0);
	cl->DrawInstanced(3, 1, 0, 0);
}

static void RenderWorldView()
{
	ID3D12GraphicsCommandList* cl = R.cl;
	const ViewDesc& v = R.view;
	const ViewConstants& c = R.vc;
	bool hd = World_Active();
	D3D12_RECT rect = { (LONG)c.viewport[0], (LONG)c.viewport[1], (LONG)(c.viewport[0] + c.viewport[2]), (LONG)(c.viewport[1] + c.viewport[3]) };
	D3D12_VIEWPORT vp = { c.viewport[0], c.viewport[1], c.viewport[2], c.viewport[3], 0, 1 };
	uint32_t gx = ((uint32_t)v.w + 7) / 8, gy = ((uint32_t)v.h + 7) / 8;

	cl->SetGraphicsRootConstantBufferView(RP_VIEW_CBV, R.viewCB);
	cl->SetComputeRootSignature(g.rootSig);
	BindRoot(cl, true);
	BindRoot(cl, false);
	cl->SetGraphicsRootConstantBufferView(RP_VIEW_CBV, R.viewCB);
	cl->SetGraphicsRootConstantBufferView(RP_EXTRA_CBV, R.viewCB);
	cl->SetComputeRootConstantBufferView(RP_VIEW_CBV, R.viewCB);
	cl->SetComputeRootConstantBufferView(RP_EXTRA_CBV, R.viewCB);

	// ---- GBuffer
	Transition(cl, R.gb0, D3D12_RESOURCE_STATE_RENDER_TARGET);
	Transition(cl, R.gb1, D3D12_RESOURCE_STATE_RENDER_TARGET);
	Transition(cl, R.gb2, D3D12_RESOURCE_STATE_RENDER_TARGET);
	Transition(cl, R.depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	const float zero4[4] = { 0, 0, 0, 0 };
	cl->ClearRenderTargetView(R.gb0.rtv[0], zero4, 1, &rect);
	cl->ClearRenderTargetView(R.gb1.rtv[0], zero4, 1, &rect);
	cl->ClearRenderTargetView(R.gb2.rtv[0], zero4, 1, &rect);
	cl->ClearDepthStencilView(R.depth.dsv[0], D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 1, &rect);
	D3D12_CPU_DESCRIPTOR_HANDLE gbs[3] = { R.gb0.rtv[0], R.gb1.rtv[0], R.gb2.rtv[0] };
	cl->OMSetRenderTargets(3, gbs, FALSE, &R.depth.dsv[0]);
	cl->RSSetViewports(1, &vp);
	cl->RSSetScissorRects(1, &rect);
	if (hd) {
		WorldView wv;
		wv.viewProj = R.viewProj;
		wv.cam[0] = v.origin[0]; wv.cam[1] = v.origin[1]; wv.cam[2] = v.origin[2];
		wv.pixelScale = v.h * 0.5f * c.projParams[1];
		wv.lodError = 1.0f;
		World_DrawGBuffer(cl, wv);
	}
	if (!R.deferred.empty()) DrawDeferredList(cl, false, 0);

	// ---- sun shadows
	bool shadows = c.shadowParams[3] > 0;
	if (shadows) {
		Transition(cl, R.shadow, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		D3D12_VIEWPORT svp = { 0, 0, (float)R.shadowRes, (float)R.shadowRes, 0, 1 };
		D3D12_RECT src = { 0, 0, (LONG)R.shadowRes, (LONG)R.shadowRes };
		for (int ci = 0; ci < 4; ci++) {
			cl->ClearDepthStencilView(R.shadow.dsv[ci], D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
			cl->OMSetRenderTargets(0, nullptr, FALSE, &R.shadow.dsv[ci]);
			cl->RSSetViewports(1, &svp);
			cl->RSSetScissorRects(1, &src);
			World_DrawShadow(cl, ci, c.cascadeViewProj[ci], R.cascadeTexel[ci]);
			if (!R.deferred.empty()) DrawDeferredList(cl, true, ci);
		}
	}
	Transition(cl, R.shadow, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

	// ---- GBuffer -> readable
	D3D12_RESOURCE_STATES srv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	Transition(cl, R.gb0, srv); Transition(cl, R.gb1, srv); Transition(cl, R.gb2, srv);
	Transition(cl, R.depth, D3D12_RESOURCE_STATE_DEPTH_READ | srv);

	// ---- sky LUT + SH
	{
		Transition(cl, R.skyLut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		Transition(cl, R.sh, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		float ga = 0.3f;
		uint32_t k[8] = { R.skyLut.uav[0], R.skyLut.srv, R.sh.uav, 128, 64, 0, 0, 0 };
		memcpy(&k[5], &ga, 4);
		if (!c.indices2[0]) Dispatch(cl, R.csSkyLut, k, 8, 16, 8);
		Transition(cl, R.skyLut, srv);
		if (!c.indices2[1]) Dispatch(cl, R.csSkySh, k, 8, 1, 1);
		Transition(cl, R.sh, srv);
	}

	// ---- AO
	bool ao = kt_ssao->integer && R.csAo && R.csAoBlur;
	if (ao) {
		Transition(cl, R.ao, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		float rad = kt_aoRadius->value, inten = kt_aoIntensity->value;
		uint32_t k[8] = { R.ao.uav[0], 0, 0, 0, 0, 0, 0, 0 };
		memcpy(&k[2], &rad, 4); memcpy(&k[3], &inten, 4);
		Dispatch(cl, R.csAo, k, 8, gx, gy);
		Transition(cl, R.ao, srv);
		Transition(cl, R.aoTmp, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		uint32_t b1[8] = { R.aoTmp.uav[0], R.ao.srv, 0, 0, 1, 0, 0, 0 };
		Dispatch(cl, R.csAoBlur, b1, 8, gx, gy);
		Transition(cl, R.aoTmp, srv);
		Transition(cl, R.ao, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		uint32_t b2[8] = { R.ao.uav[0], R.aoTmp.srv, 0, 0, 0, 1, 0, 0 };
		Dispatch(cl, R.csAoBlur, b2, 8, gx, gy);
		Transition(cl, R.ao, srv);
	}

	// ---- lights (HD world lights + game dynamic lights)
	std::vector<GpuLight> lights;
	if (kt_lights->integer) {
		World_AppendLights(lights, v.origin);
		for (int i = 0; i < backEnd.refdef.num_dlights && lights.size() < 1024; i++) {
			const dlight_t& dl = backEnd.refdef.dlights[i];
			GpuLight l = {};
			l.pos[0] = dl.origin[0]; l.pos[1] = dl.origin[1]; l.pos[2] = dl.origin[2];
			l.radius = dl.radius;
			float s = kt_dlightScale->value * (hd ? 4.0f : 1.0f);
			l.color[0] = dl.color[0] * s; l.color[1] = dl.color[1] * s; l.color[2] = dl.color[2] * s;
			l.type = 0;
			l.invSquare = 0;
			lights.push_back(l);
		}
	}
	if (kt_lights->integer && kt_saberLight->value > 0)
		for (const GpuLight& t : R.tubes) if (lights.size() < 1024) {
			GpuLight l = t;
			for (int q = 0; q < 3; q++) l.color[q] *= kt_saberLight->value;
			lights.push_back(l);
		}
	uint32_t nl = (uint32_t)lights.size();
	if (!nl) { GpuLight dummy = {}; lights.push_back(dummy); }
	UploadAlloc la = Upload(lights.size() * sizeof(GpuLight), 256);
	memcpy(la.cpu, lights.data(), lights.size() * sizeof(GpuLight));
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
		sd.Format = DXGI_FORMAT_R32_TYPELESS;
		sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		sd.Buffer.FirstElement = la.offset / 4;
		sd.Buffer.NumElements = (UINT)(lights.size() * sizeof(GpuLight) / 4);
		sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
		g.dev->CreateShaderResourceView(la.res, &sd, CpuHandle(R.lightSrv[g.frameIndex]));
	}
	// exposure buffer must be readable by the lighting debug views and the forward pass
	Transition(cl, R.exposure, srv);

	// ---- lighting
	Transition(cl, R.hdr, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	{
		uint32_t k[8] = { R.hdr.uav[0], R.sh.srv, R.lightSrv[g.frameIndex], nl, R.ao.srv,
			(ao ? 1u : 0u) | (shadows ? 2u : 0u) | (kt_volumetric->integer ? 4u : 0u) | (kt_contactShadows->integer ? 8u : 0u) | (kt_lightShadows->integer ? 16u : 0u), (uint32_t)Q_max(0, kt_debugView->integer),
			(kt_ssr->integer && R.hdrPrevValid) ? R.hdrPrev.srv : 0u };
		Dispatch(cl, R.csLighting, k, 8, gx, gy);
	}

	// ---- water / flowing water (reads the scene depth, blends into the lit scene)
	Transition(cl, R.hdr, D3D12_RESOURCE_STATE_RENDER_TARGET);
	if (hd) {
		cl->OMSetRenderTargets(1, &R.hdr.rtv[0], FALSE, nullptr);
		cl->RSSetViewports(1, &vp);
		cl->RSSetScissorRects(1, &rect);
		cl->SetGraphicsRootConstantBufferView(RP_VIEW_CBV, R.viewCB);
		WorldView wv;
		wv.viewProj = R.viewProj;
		wv.cam[0] = v.origin[0]; wv.cam[1] = v.origin[1]; wv.cam[2] = v.origin[2];
		wv.pixelScale = v.h * 0.5f * c.projParams[1];
		wv.lodError = 1.0f;
		World_DrawFX(cl, wv);
	}

	// ---- forward legacy content into the HDR scene
	Transition(cl, R.depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	{
		const float zero4g[4] = { 0, 0, 0, 0 };
		Transition(cl, R.glow, D3D12_RESOURCE_STATE_RENDER_TARGET);
		cl->ClearRenderTargetView(R.glow.rtv[0], zero4g, 0, nullptr);
	}
	FlushStream(1);
	Transition(cl, R.glow, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

	// ---- camera: exposure
	D3D12_RESOURCE_STATES srvAll = srv;
	// lightsaber blades (after the legacy content, so the depth holds characters and BSP surfaces)
	if (!R.blades.empty() && R.bladePSO) {
		Transition(cl, R.depth, D3D12_RESOURCE_STATE_DEPTH_READ | srv);
		Transition(cl, R.hdr, D3D12_RESOURCE_STATE_RENDER_TARGET);
		cl->OMSetRenderTargets(1, &R.hdr.rtv[0], FALSE, nullptr);
		cl->RSSetViewports(1, &vp);
		cl->RSSetScissorRects(1, &rect);
		cl->SetGraphicsRootConstantBufferView(RP_VIEW_CBV, R.viewCB);
		cl->SetPipelineState(R.bladePSO);
		cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		for (int pass = 0; pass < 2; pass++)            // glows, then cores on top
			for (const auto& bl0 : R.blades) {
				if ((int)bl0.mode != pass) continue;
				RState::Blade bl = bl0;
				GLTexture* t = bl.tex ? R_GLTexture(bl.tex, false) : nullptr;
				R_ResolveTexture(t);
				bl.tex = t ? R_TextureSrv(t, true) : 0;
				cl->SetGraphicsRoot32BitConstants(RP_CONSTANTS, 20, &bl, 0);
				cl->DrawInstanced(6, 1, 0, 0);
			}
	}
	// keep this frame's lit scene for next frame's screen-space reflections
	if (kt_ssr->integer) {
		Transition(cl, R.hdr, D3D12_RESOURCE_STATE_COPY_SOURCE);
		Transition(cl, R.hdrPrev, D3D12_RESOURCE_STATE_COPY_DEST);
		cl->CopyResource(R.hdrPrev.res, R.hdr.res);
		Transition(cl, R.hdrPrev, srvAll);
		R.hdrPrevValid = true;
	}
	Transition(cl, R.hdr, srvAll);
	Transition(cl, R.exposure, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	{
		uint32_t k[8] = { R.hdr.srv, 0, R.exposure.uav, 0, 0, 0, 0, 0 };
		memcpy(&k[6], &R.dt, 4);
		Dispatch(cl, R.csHist, k, 8, ((uint32_t)v.w / 2 + 15) / 16, ((uint32_t)v.h / 2 + 15) / 16);
		UavBarrier(cl, R.exposure.res);
		Dispatch(cl, R.csExposure, k, 8, 1, 1);
	}
	Transition(cl, R.exposure, srvAll);

	// ---- bloom
	uint32_t bloomSrv = 0;
	if (kt_bloom->value > 0 && R.bloomLevels >= 2 && R.csBloomDown && R.csBloomUp) {
		uint32_t src = R.hdr.srv;
		for (int i = 0; i < R.bloomLevels; i++) {
			Texture& d = R.bloomDown[i];
			Transition(cl, d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			uint32_t dw = i == 0 ? Q_max(1, v.w / 2) : d.width, dh = i == 0 ? Q_max(1, v.h / 2) : d.height;
			if (i > 0) { dw = Q_max(1u, (uint32_t)(R.bloomDown[i - 1].width / 2)); dh = Q_max(1u, (uint32_t)(R.bloomDown[i - 1].height / 2)); }
			dw = Q_min(dw, d.width); dh = Q_min(dh, d.height);
			uint32_t k[8] = { src, d.uav[0], 0, 0, d.width, d.height, 0, i == 0 ? 0u : 1u };
			if (i == 0) { k[2] = R.glow.srv; float gs = kt_dynGlow->value / Q_max(1e-3f, kt_bloom->value); memcpy(&k[6], &gs, 4); }
			Dispatch(cl, R.csBloomDown, k, 8, (d.width + 7) / 8, (d.height + 7) / 8);
			Transition(cl, d, srvAll);
			src = d.srv;
		}
		// up: up[n-1] = down[n-1]; up[i] = down[i] + tent(up[i+1])
		uint32_t prev = R.bloomDown[R.bloomLevels - 1].srv;
		for (int i = R.bloomLevels - 2; i >= 0; i--) {
			Texture& u = R.bloomUp[i];
			Transition(cl, u, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			uint32_t k[8] = { R.bloomDown[i].srv, u.uav[0], 0, prev, u.width, u.height, 0, 0 };
			Dispatch(cl, R.csBloomUp, k, 8, (u.width + 7) / 8, (u.height + 7) / 8);
			Transition(cl, u, srvAll);
			prev = u.srv;
		}
		bloomSrv = prev;
	}

	// ---- tone map (+ FXAA) into the screen
	cl->SetGraphicsRootConstantBufferView(RP_VIEW_CBV, R.viewCB);
	bool fxaa = kt_fxaa->integer && R.fxaaPSO;
	Texture& tmTarget = fxaa ? R.ldr : R.screen;
	Transition(cl, R.depth, D3D12_RESOURCE_STATE_DEPTH_READ | srv);
	Transition(cl, tmTarget, D3D12_RESOURCE_STATE_RENDER_TARGET);
	{
		uint32_t k[8] = { R.hdr.srv, 0, 0, bloomSrv, R.w, R.h, 0, 0 };
		FullscreenPass(cl, R.tonemapPSO, tmTarget, k, 8);
	}
	if (fxaa) {
		Transition(cl, R.ldr, srvAll);
		Transition(cl, R.screen, D3D12_RESOURCE_STATE_RENDER_TARGET);
		uint32_t k[8] = { R.ldr.srv, 0, 0, 0, R.w, R.h, 0, 0 };
		FullscreenPass(cl, R.fxaaPSO, R.screen, k, 8);
	}
	Transition(cl, R.screen, D3D12_RESOURCE_STATE_RENDER_TARGET);
	cl->SetGraphicsRootConstantBufferView(RP_VIEW_CBV, R.defaultCB);
}

// a lightsaber blade (from the glow surface): a line light from the hilt to the tip
void R_AddSaberLight(const float* start, const float* dir, float length, const uint8_t* rgba)
{
	if (!R.inView || length < 1.0f || R.tubes.size() >= 64) return;
	GpuLight l = {};
	for (int q = 0; q < 3; q++) { l.pos[q] = start[q]; l.dir[q] = dir[q] * length; }
	l.radius = 220.0f + length;
	// the blade colour, saturated (the glow sprites carry it), as radiance; intensity / d^2 from the nearest point
	float c[3] = { rgba[0] / 255.0f, rgba[1] / 255.0f, rgba[2] / 255.0f };
	float mx = Q_max(c[0], Q_max(c[1], c[2]));
	if (mx < 0.01f) { c[0] = c[1] = c[2] = 1; mx = 1; }
	for (int q = 0; q < 3; q++) l.color[q] = powf(c[q] / mx, 2.2f) * 1.0f;
	l.type = 2;
	l.invSquare = 220.0f;   // a soft fill, about what rend2's saber dynamic light gives
	R.tubes.push_back(l);
}
float SaberGlowBoost() { return kt_saberGlow ? kt_saberGlow->value : 2.0f; }
bool SaberDraw() { return kt_saberDraw && kt_saberDraw->integer != 0; }
void R_AddSaberBlade(const float* start, const float* dir, float length, float radius, const uint8_t* rgba, uint32_t glowTex, const uint8_t* vertexRgba, bool coreOnly)
{
	if (!R.inView || length < 0.5f || R.blades.size() >= 128) return;
	RState::Blade b = {};
	float c[3] = { rgba[0] / 255.0f, rgba[1] / 255.0f, rgba[2] / 255.0f };
	float mx = Q_max(c[0], Q_max(c[1], c[2]));
	const bool dark = mx < 0.01f;   // darksaber style: black core, white rim
	for (int q = 0; q < 3; q++) { b.a[q] = start[q]; b.b[q] = start[q] + dir[q] * length; b.color[q] = dark ? 1.0f : powf(c[q] / mx, 2.2f); }
	b.glowR = Q_max(1.0f, radius);
	b.coreR = Q_max(0.6f, radius * 0.22f);
	b.intensity = kt_saberGlow ? kt_saberGlow->value * 0.5f : 1.0f;
	b.tex = 0; b.mode = 0; b.alpha = dark ? 1 : 0;   // procedural (BF2-style) blade
	if (coreOnly) { if (dark) return; b.alpha = 2; b.coreR = Q_max(0.9f, radius * 0.3f); }
	// the glow texture is tinted by the entity colour (rgbGen vertex): use that, linearised
	for (int q = 0; q < 3; q++) b.vcol[q] = powf(vertexRgba[q] / 255.0f, 2.2f);
	R.blades.push_back(b);
}
void R_AddSaberCore(const float* start, const float* end, float radius, const uint8_t* rgba, uint32_t tex, bool alphaBlend)
{
	if (!R.inView || R.blades.size() >= 128) return;
	RState::Blade b = {};
	for (int q = 0; q < 3; q++) { b.a[q] = start[q]; b.b[q] = end[q]; b.color[q] = powf(rgba[q] / 255.0f, 2.2f); b.vcol[q] = b.color[q]; }
	b.glowR = Q_max(0.3f, radius);
	b.coreR = b.glowR;
	b.intensity = kt_saberGlow ? kt_saberGlow->value * 0.5f : 1.0f;
	b.tex = tex; b.mode = 1; b.alpha = alphaBlend ? 1 : 0;
	R.blades.push_back(b);
}

void R_EndView()
{
	if (R.ready && R.inView && R.viewWorld && kt_speeds && kt_speeds->integer && World_Active()) {
		int now = ri.Milliseconds();
		if (now - R.lastSpeeds > 1000) {
			R.lastSpeeds = now;
			uint32_t cl; uint64_t tr;
			World_Stats(cl, tr);
			ri.Printf(PRINT_ALL, "katarn: %.1f ms, %u clusters, %.2fM triangles (gbuffer), %d characters/models\n", R.dt * 1000.0f, cl, tr / 1e6, (int)R.deferred.size());
		}
	}
	if (!R.ready || !R.inView) return;
	EnsureList();
	ID3D12GraphicsCommandList* cl = R.cl;
	bool world = R.viewWorld && (World_Active() || kt_legacyDeferred->integer >= 0);
	if (world && R.csLighting && R.tonemapPSO) {
		RenderWorldView();
	}
	else {
		// UI model view (or no world): forward into the screen with its own depth
		const ViewConstants& c = R.vc;
		D3D12_RECT rect = { (LONG)c.viewport[0], (LONG)c.viewport[1], (LONG)(c.viewport[0] + c.viewport[2]), (LONG)(c.viewport[1] + c.viewport[3]) };
		Transition(cl, R.screenDepth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		cl->ClearDepthStencilView(R.screenDepth.dsv[0], D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 1, &rect);
		cl->SetGraphicsRootConstantBufferView(RP_VIEW_CBV, R.viewCB);
		FlushStream(0);
	}
	R.stream.clear();
	R.deferred.clear();
	R.inView = false;
	R.reversedZ = false;
}

bool R_ReadScreen(int x, int y, int w, int h, uint8_t* out, bool rgba)
{
	if (!R.ready || w <= 0 || h <= 0) return false;
	EnsureList();
	FlushStream(0);
	ID3D12GraphicsCommandList* cl = R.cl;
	uint32_t pitch = ((uint32_t)w * 4 + 255) & ~255u;
	D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
	D3D12_RESOURCE_DESC rd = {};
	rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = (uint64_t)pitch * h; rd.Height = 1; rd.DepthOrArraySize = 1;
	rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	ID3D12Resource* rb = nullptr;
	if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)))) return false;
	Transition(cl, R.screen, D3D12_RESOURCE_STATE_COPY_SOURCE);
	D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
	dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, (UINT)w, (UINT)h, 1, pitch };
	src.pResource = R.screen.res; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	int top = (int)R.h - (y + h);
	D3D12_BOX box = { (UINT)x, (UINT)Q_max(0, top), 0, (UINT)(x + w), (UINT)(Q_max(0, top) + h), 1 };
	cl->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
	Transition(cl, R.screen, D3D12_RESOURCE_STATE_RENDER_TARGET);
	SubmitList(cl);
	WaitIdle();
	R.cl = OpenList();
	R.cl->SetGraphicsRootConstantBufferView(RP_VIEW_CBV, R.defaultCB);
	R.cl->SetGraphicsRootConstantBufferView(RP_EXTRA_CBV, R.defaultCB);
	uint8_t* p = nullptr;
	D3D12_RANGE range = { 0, (SIZE_T)rd.Width };
	if (FAILED(rb->Map(0, &range, (void**)&p))) { rb->Release(); return false; }
	// GL order: bottom row first
	for (int r = 0; r < h; r++) {
		const uint8_t* s = p + (size_t)(h - 1 - r) * pitch;
		uint8_t* d = out + (size_t)r * w * (rgba ? 4 : 3);
		for (int i = 0; i < w; i++) {
			d[0] = s[i * 4]; d[1] = s[i * 4 + 1]; d[2] = s[i * 4 + 2];
			if (rgba) { d[3] = 255; d += 4; }
			else d += 3;
		}
	}
	D3D12_RANGE none = { 0, 0 };
	rb->Unmap(0, &none);
	rb->Release();
	return true;
}

void R_GfxInfo()
{
	ri.Printf(PRINT_ALL, "KatarnEngine (Direct3D 12): %s, %u MB VRAM\n", g.adapterName.c_str(), (unsigned)(g.vram >> 20));
	ri.Printf(PRINT_ALL, "  %ux%u, vsync %s, shadows %ux%u x4, HD world: %s\n", R.w, R.h, g.cfg.vsync ? "on" : "off", R.shadowRes, R.shadowRes,
		World_Active() ? "loaded" : "none");
}

} // namespace kt
