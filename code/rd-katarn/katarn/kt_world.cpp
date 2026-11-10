// KatarnEngine - HD worlds: GPU-resident world-space clusters with LOD chains and bindless PBR materials (.kworld v2)
#include "kt_world.h"
#include "kt_render.h"
#include "kworld_format.h"
#include "../tr_local.h"
#include <math.h>
#include <stdio.h>
#include <vector>
#include <string>
#include <algorithm>

namespace kt {

static struct World {
	bool active = false;
	std::string name;
	Buffer vb, ib, materials;
	std::vector<Texture> textures;
	std::vector<KwCluster> clusters;
	std::vector<KwLod> lods;
	std::vector<KwMaterial> mats;
	std::vector<KwLight> lights;
	WorldEnv env;
	ID3D12PipelineState* gbPSO[3] = {};          // per cluster class
	ID3D12PipelineState* shPSO[2] = {};          // opaque, alpha-tested
	ID3D12PipelineState* fxPSO = nullptr;
	bool hasFX = false;         // water / flowing water into the HDR scene
	Shader vs, ps, shadowPS, fxPS;
	bool pipelines = false;
	float bmin[3], bmax[3];
	uint64_t drawnTris = 0;
	uint32_t drawnClusters = 0;
} W;

static WorldEnv defaultEnv;
static cvar_t* kt_lodError = nullptr;
static cvar_t* kt_lodLock = nullptr;

bool World_Active() { return W.active; }
const WorldEnv& World_Env() { return W.active ? W.env : defaultEnv; }

static const D3D12_INPUT_ELEMENT_DESC kWorldLayout[] = {
	{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "NORMAL", 0, DXGI_FORMAT_R16G16_SNORM, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "TANGENT", 0, DXGI_FORMAT_R16G16_SNORM, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "BLENDINDICES", 0, DXGI_FORMAT_R16G16_UINT, 0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
};

static D3D12_SHADER_BYTECODE BCode(const Shader& s)
{
	D3D12_SHADER_BYTECODE b = {};
	if (s.ok) { b.pShaderBytecode = s.blob->GetBufferPointer(); b.BytecodeLength = s.blob->GetBufferSize(); }
	return b;
}

static bool CreateWorldPipelines()
{
	if (W.pipelines) return true;
	kt_lodError = ri.Cvar_Get("kt_lodError", "1.0", CVAR_ARCHIVE);
	kt_lodLock = ri.Cvar_Get("kt_lodLock", "-1", 0);
	W.vs = R_Shader("world", "VS", "vs_5_1");
	W.ps = R_Shader("world", "PS", "ps_5_1");
	W.shadowPS = R_Shader("world", "ShadowPS", "ps_5_1");
	W.fxPS = R_Shader("world", "FxPS", "ps_5_1");
	if (!W.vs.ok || !W.ps.ok) return false;
	D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
	d.SampleMask = 0xffffffff;
	d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	d.RasterizerState.FrontCounterClockwise = TRUE;
	d.RasterizerState.DepthClipEnable = TRUE;
	for (auto& rt : d.BlendState.RenderTarget) rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL, rt.SrcBlend = D3D12_BLEND_ONE,
		rt.DestBlend = D3D12_BLEND_ZERO, rt.BlendOp = D3D12_BLEND_OP_ADD, rt.SrcBlendAlpha = D3D12_BLEND_ONE, rt.DestBlendAlpha = D3D12_BLEND_ZERO,
		rt.BlendOpAlpha = D3D12_BLEND_OP_ADD, rt.LogicOp = D3D12_LOGIC_OP_NOOP;
	d.DepthStencilState.DepthEnable = TRUE;
	d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
	d.InputLayout = { kWorldLayout, 5 };
	d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	d.SampleDesc.Count = 1;
	d.VS = BCode(W.vs); d.PS = BCode(W.ps);
	d.NumRenderTargets = 3;
	d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; d.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT; d.RTVFormats[2] = DXGI_FORMAT_R11G11B10_FLOAT;
	d.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	for (int c = 0; c < 3; c++) {
		d.RasterizerState.CullMode = c == KWC_OPAQUE ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
		W.gbPSO[c] = CreateGraphicsPSO(d, "world gbuffer");
	}
	// shadows: two-sided (Blender scenes are full of single-sided walls), slope-scaled bias
	d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	d.RasterizerState.DepthBias = 1000; d.RasterizerState.SlopeScaledDepthBias = 2.0f; d.RasterizerState.DepthBiasClamp = 0.01f;
	d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	d.NumRenderTargets = 0;
	for (auto& f : d.RTVFormats) f = DXGI_FORMAT_UNKNOWN;
	d.PS = D3D12_SHADER_BYTECODE{};
	W.shPSO[0] = CreateGraphicsPSO(d, "world shadow");
	d.PS = BCode(W.shadowPS);
	W.shPSO[1] = CreateGraphicsPSO(d, "world shadow alpha");
	// FX: no depth buffer (the shader tests against the scene depth and reads it for absorption); rgb + dst * a
	if (W.fxPS.ok) {
		D3D12_GRAPHICS_PIPELINE_STATE_DESC f = d;
		f.RasterizerState.DepthBias = 0; f.RasterizerState.SlopeScaledDepthBias = 0; f.RasterizerState.DepthBiasClamp = 0;
		f.DepthStencilState.DepthEnable = FALSE;
		f.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		f.DSVFormat = DXGI_FORMAT_UNKNOWN;
		f.NumRenderTargets = 1;
		f.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
		auto& rt = f.BlendState.RenderTarget[0];
		rt.BlendEnable = TRUE; rt.SrcBlend = D3D12_BLEND_ONE; rt.DestBlend = D3D12_BLEND_SRC_ALPHA; rt.BlendOp = D3D12_BLEND_OP_ADD;
		rt.SrcBlendAlpha = D3D12_BLEND_ZERO; rt.DestBlendAlpha = D3D12_BLEND_ONE; rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
		rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		f.PS = BCode(W.fxPS);
		W.fxPSO = CreateGraphicsPSO(f, "world fx");
	}
	W.pipelines = true;
	return W.gbPSO[0] && W.shPSO[0];
}

void World_Unload()
{
	if (!W.active && W.textures.empty()) return;
	WaitIdle();
	DestroyBuffer(W.vb);
	DestroyBuffer(W.ib);
	DestroyBuffer(W.materials);
	for (auto& t : W.textures) DestroyTexture(t);
	W.textures.clear();
	W.clusters.clear(); W.lods.clear(); W.mats.clear(); W.lights.clear();
	W.active = false;
	W.name.clear();
}

void World_Shutdown()
{
	World_Unload();
	for (auto*& p : W.gbPSO) { if (p) p->Release(); p = nullptr; }
	for (auto*& p : W.shPSO) { if (p) p->Release(); p = nullptr; }
	if (W.fxPSO) { W.fxPSO->Release(); W.fxPSO = nullptr; }
	Shader* s[] = { &W.vs, &W.ps, &W.shadowPS, &W.fxPS };
	for (auto* x : s) { if (x->blob) x->blob->Release(); *x = Shader(); }
	W.pipelines = false;
}

static FILE* OpenWorldFile(const char* mapName, std::string& path)
{
	const char* base = ri.Cvar_VariableString("fs_basepath");
	const char* home = ri.Cvar_VariableString("fs_homepath");
	const char* game = ri.Cvar_VariableString("fs_game");
	const char* dirs[] = { game, "base" };
	const char* roots[] = { home, base };
	for (const char* root : roots) {
		if (!root || !*root) continue;
		for (const char* d : dirs) {
			if (!d || !*d) continue;
			path = std::string(root) + "/" + d + "/maps/" + mapName + ".kworld";
			FILE* f = fopen(path.c_str(), "rb");
			if (f) return f;
		}
	}
	return nullptr;
}

static void ParseEnv(const std::string& text, WorldEnv& e, int& envTex)
{
	size_t p = 0;
	while (p < text.size()) {
		size_t nl = text.find('\n', p);
		if (nl == std::string::npos) nl = text.size();
		std::string line = text.substr(p, nl - p);
		p = nl + 1;
		char key[64]; float a = 0, b = 0, c = 0;
		int n = sscanf(line.c_str(), "%63s %f %f %f", key, &a, &b, &c);
		if (n < 2) continue;
		std::string k = key;
		if (k == "sunDir" && n >= 4) { e.sunDir[0] = a; e.sunDir[1] = b; e.sunDir[2] = c; }
		else if (k == "sunColor" && n >= 4) { e.sunColor[0] = a; e.sunColor[1] = b; e.sunColor[2] = c; }
		else if (k == "sunIntensity") e.sunIntensity = a;
		else if (k == "sunAngle") e.sunAngle = a;
		else if (k == "skyIntensity") e.skyIntensity = a;
		else if (k == "haze") e.haze = a;
		else if (k == "ambient") e.ambient = a;
		else if (k == "fogDensity") e.fogDensity = a;
		else if (k == "fogFalloff") e.fogFalloff = a;
		else if (k == "fogHeight") e.fogHeight = a;
		else if (k == "fogStart") e.fogStart = a;
		else if (k == "fogMax") e.fogMax = a;
		else if (k == "fogColor" && n >= 4) { e.fogColor[0] = a; e.fogColor[1] = b; e.fogColor[2] = c; }
		else if (k == "exposureEV") e.exposureEV = a;
		else if (k == "envTex") envTex = (int)a;
		else if (k == "envStrength") { e.envStrength = a; e.envLight = a; }
		else if (k == "envLight") e.envLight = a;
		else if (k.size() == 3 && k[0] == 's' && k[1] == 'h' && k[2] >= '0' && k[2] <= '8' && n >= 4) {
			int i = k[2] - '0';
			e.sh[i][0] = a; e.sh[i][1] = b; e.sh[i][2] = c;
			e.hasSH = true;
		}
	}
}

bool World_Load(const char* mapName)
{
	World_Unload();
	std::string path;
	FILE* f = OpenWorldFile(mapName, path);
	if (!f) return false;
	if (!CreateWorldPipelines()) { fclose(f); Log("^1KatarnEngine: world shaders failed\n"); return false; }
	int t0 = ri.Milliseconds();
	KwHeader h;
	if (fread(&h, sizeof(h), 1, f) != 1 || memcmp(h.magic, KW_MAGIC, 8) || h.version != KW_VERSION) {
		Log("^1KatarnEngine: %s is not a version %d .kworld - cook it again with tools\\katarn_cook\n", path.c_str(), KW_VERSION);
		fclose(f);
		return false;
	}
	std::vector<KwSection> secs(h.numSections);
	fread(secs.data(), sizeof(KwSection), h.numSections, f);
	auto find = [&](uint32_t type) -> const KwSection* { for (auto& s : secs) if (s.type == type) return &s; return nullptr; };
	auto readVec = [&](uint32_t type, auto& vec) {
		const KwSection* s = find(type);
		using T = typename std::remove_reference<decltype(vec)>::type::value_type;
		vec.clear();
		if (!s) return;
		vec.resize(s->size / sizeof(T));
		_fseeki64(f, (long long)s->offset, SEEK_SET);
		fread(vec.data(), sizeof(T), vec.size(), f);
	};
	readVec(KW_CLUS, W.clusters);
	readVec(KW_LODS, W.lods);
	readVec(KW_MATL, W.mats);
	readVec(KW_LITE, W.lights);
	std::vector<uint8_t> splatRaw;
	readVec(KW_SPLT, splatRaw);
	std::vector<KwTexture> texs;
	readVec(KW_TEXT, texs);
	W.env = WorldEnv();
	int envTex = -1;
	if (const KwSection* s = find(KW_ENVT)) {
		std::string text(s->size, '\0');
		_fseeki64(f, (long long)s->offset, SEEK_SET);
		fread(&text[0], 1, s->size, f);
		ParseEnv(text, W.env, envTex);
	}
	{
		// maps/<map>.kenv next to the .kworld: the same "key value" lines, read after the cooked ones, so sun, fog,
		// exposure and sky can be tuned without cooking the world again
		std::string ep = path.substr(0, path.size() - 7) + ".kenv";
		if (FILE* ef = fopen(ep.c_str(), "rb")) {
			std::string t;
			char buf[4096]; size_t n;
			while ((n = fread(buf, 1, sizeof(buf), ef)) > 0) t.append(buf, n);
			fclose(ef);
			int dummy = envTex;
			ParseEnv(t, W.env, dummy);
			ri.Printf(PRINT_ALL, "KatarnEngine: environment overrides from %s\n", ep.c_str());
		}
	}
	memcpy(W.bmin, h.boundsMin, 12);
	memcpy(W.bmax, h.boundsMax, 12);

	auto uploadSection = [&](uint32_t type, Buffer& buf, const char* name) -> bool {
		const KwSection* s = find(type);
		if (!s || !s->size) return false;
		if (!CreateBuffer(buf, s->size, false, name)) return false;
		std::vector<uint8_t> chunk;
		const uint64_t CH = 64ull << 20;
		for (uint64_t off = 0; off < s->size; off += CH) {
			uint64_t n = std::min<uint64_t>(CH, s->size - off);
			chunk.resize(n);
			_fseeki64(f, (long long)(s->offset + off), SEEK_SET);
			fread(chunk.data(), 1, n, f);
			UploadBuffer(buf, off, chunk.data(), n);
			FlushUploads();
		}
		return true;
	};
	bool ok = uploadSection(KW_VERT, W.vb, "world vertices") && uploadSection(KW_INDX, W.ib, "world indices");
	if (!ok) { Log("^1KatarnEngine: %s: geometry upload failed\n", path.c_str()); fclose(f); World_Unload(); return false; }

	const KwSection* tdat = find(KW_TDAT);
	W.textures.resize(texs.size());
	uint64_t pending = 0, texBytes = 0;
	std::vector<uint8_t> data;
	for (size_t i = 0; i < texs.size() && tdat; i++) {
		const KwTexture& k = texs[i];
		Texture& t = W.textures[i];
		DXGI_FORMAT fmt = (DXGI_FORMAT)k.format;
		if (!CreateTexture(t, k.width, k.height, k.mips, fmt, k.srgb ? TEX_SRGB_VIEW : 0, 1, nullptr, "world texture")) continue;
		data.resize(k.size);
		_fseeki64(f, (long long)(tdat->offset + k.offset), SEEK_SET);
		fread(data.data(), 1, k.size, f);
		bool bc = fmt != DXGI_FORMAT_R8G8B8A8_UNORM;
		uint32_t bpb = FormatBytes(fmt);
		size_t off = 0;
		for (uint32_t m = 0; m < k.mips; m++) {
			uint32_t w = Q_max(1u, k.width >> m), hh = Q_max(1u, k.height >> m);
			uint32_t rowBytes = bc ? ((w + 3) / 4) * bpb : w * 4;
			uint32_t rows = bc ? (hh + 3) / 4 : hh;
			if (off + (size_t)rowBytes * rows > data.size()) break;
			UploadTexture(t, m, 0, data.data() + off, rowBytes);
			off += (size_t)rowBytes * rows;
		}
		pending += k.size;
		texBytes += k.size;
		if (pending > (192ull << 20)) { FlushUploads(); pending = 0; }
	}
	FlushUploads();
	fclose(f);
	if (envTex >= 0 && envTex < (int)W.textures.size() && W.textures[envTex].res) W.env.envSrv = W.textures[envTex].srvSrgb;

	std::vector<KwMaterial> gm = W.mats;
	auto view = [&](int32_t ti, bool srgb) -> int32_t {
		if (ti < 0 || ti >= (int)W.textures.size() || !W.textures[ti].res) return 0;
		return (int32_t)(srgb ? W.textures[ti].srvSrgb : W.textures[ti].srv);
	};
	for (auto& m : gm) {
		m.albedoTex = view(m.albedoTex, true);
		m.normalTex = view(m.normalTex, false);
		m.ormTex = view(m.ormTex, false);
		m.emissiveTex = view(m.emissiveTex, true);
	}
	if (gm.empty()) { KwMaterial d = {}; d.baseColor[0] = d.baseColor[1] = d.baseColor[2] = d.baseColor[3] = 0.8f; d.roughness = 0.6f; gm.push_back(d); }
	// splat (terrain) records go after the materials in the same buffer; their texture indices become views too
	std::vector<uint8_t> splat = splatRaw;
	const uint32_t splatBase = (uint32_t)(gm.size() * sizeof(KwMaterial));
	for (size_t o = 0; o + sizeof(KwSplatHeader) <= splat.size(); ) {
		KwSplatHeader* sh = (KwSplatHeader*)&splat[o];
		sh->tintTex = view(sh->tintTex, false);
		for (uint32_t i = 0; i < sh->numLayers && o + sizeof(KwSplatHeader) + (i + 1) * sizeof(KwSplatLayer) <= splat.size(); i++) {
			uint8_t* lp = &splat[o + sizeof(KwSplatHeader) + i * sizeof(KwSplatLayer)];
			if (sh->kind == 1) { KwFlowLayer* F = (KwFlowLayer*)lp; F->tex = view(F->tex, false); continue; }
			KwSplatLayer* L = (KwSplatLayer*)lp;
			L->albedoTex = view(L->albedoTex, true);
			L->nohTex = view(L->nohTex, false);
			L->maskTex = view(L->maskTex, false);
		}
		o += sizeof(KwSplatHeader) + sh->numLayers * sizeof(KwSplatLayer);
	}
	for (size_t i = 0; i < gm.size(); i++)
		if (W.mats[i].flags & (KWM_SPLAT | KWM_FLOW)) gm[i].albedoTex = (int32_t)(splatBase + (uint32_t)W.mats[i].albedoTex);
	size_t matBytes = gm.size() * sizeof(KwMaterial) + splat.size();
	CreateBuffer(W.materials, (matBytes + 15) & ~(size_t)15, false, "world materials");
	UploadBuffer(W.materials, 0, gm.data(), gm.size() * sizeof(KwMaterial));
	if (!splat.empty()) UploadBuffer(W.materials, splatBase, splat.data(), splat.size());
	FlushUploads();

	W.hasFX = false;
	for (auto& c : W.clusters) if (c.cls == KWC_FX) W.hasFX = true;
	W.active = true;
	W.name = mapName;
	uint64_t tris = 0;
	for (auto& c : W.clusters) if (c.numLods) tris += W.lods[c.firstLod].indexCount / 3;
	Log("KatarnEngine: HD world %s - %u clusters, %.2fM triangles (LOD0), %.0f MB geometry, %u materials, %u textures (%llu MB), %u lights, sky %s, %d ms\n",
		mapName, (unsigned)W.clusters.size(), tris / 1e6, (W.vb.size + W.ib.size) / 1048576.0, (unsigned)W.mats.size(),
		(unsigned)W.textures.size(), (unsigned long long)(texBytes >> 20), (unsigned)W.lights.size(), W.env.envSrv ? "texture" : "atmosphere",
		ri.Milliseconds() - t0);
	return true;
}

// ------------------------------------------------------------------------------------------------ culling + drawing
struct Frustum { float p[6][4]; int n; };
static void FrustumFromMatrix(const float* m, Frustum& fr, bool ortho)
{
	auto row = [&](int r, float* o) { o[0] = m[r]; o[1] = m[4 + r]; o[2] = m[8 + r]; o[3] = m[12 + r]; };
	float r0[4], r1[4], r2[4], r3[4];
	row(0, r0); row(1, r1); row(2, r2); row(3, r3);
	int n = 0;
	for (int i = 0; i < 4; i++) fr.p[n][i] = r3[i] + r0[i]; n++;
	for (int i = 0; i < 4; i++) fr.p[n][i] = r3[i] - r0[i]; n++;
	for (int i = 0; i < 4; i++) fr.p[n][i] = r3[i] + r1[i]; n++;
	for (int i = 0; i < 4; i++) fr.p[n][i] = r3[i] - r1[i]; n++;
	if (ortho) {
		for (int i = 0; i < 4; i++) fr.p[n][i] = r2[i]; n++;
		for (int i = 0; i < 4; i++) fr.p[n][i] = r3[i] - r2[i]; n++;
	}
	else { for (int i = 0; i < 4; i++) fr.p[n][i] = r3[i] - r2[i]; n++; }
	fr.n = n;
}
static bool BoxVisible(const Frustum& fr, const float* mn, const float* mx)
{
	for (int i = 0; i < fr.n; i++) {
		const float* p = fr.p[i];
		float x = p[0] >= 0 ? mx[0] : mn[0], y = p[1] >= 0 ? mx[1] : mn[1], z = p[2] >= 0 ? mx[2] : mn[2];
		if (p[0] * x + p[1] * y + p[2] * z + p[3] < 0) return false;
	}
	return true;
}
static float BoxDistance(const float* p, const float* mn, const float* mx)
{
	float d2 = 0;
	for (int i = 0; i < 3; i++) { float d = p[i] < mn[i] ? mn[i] - p[i] : p[i] > mx[i] ? p[i] - mx[i] : 0; d2 += d * d; }
	return sqrtf(d2);
}

static void BindGeometry(ID3D12GraphicsCommandList* cl)
{
	Transition(cl, W.vb, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	Transition(cl, W.ib, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	Transition(cl, W.materials, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	D3D12_VERTEX_BUFFER_VIEW vbv = { W.vb.va, (UINT)Q_min((uint64_t)0xffffffffu, W.vb.size), sizeof(KwVertex) };
	D3D12_INDEX_BUFFER_VIEW ibv = { W.ib.va, (UINT)Q_min((uint64_t)0xffffffffu, W.ib.size), DXGI_FORMAT_R32_UINT };
	cl->IASetVertexBuffers(0, 1, &vbv);
	cl->IASetIndexBuffer(&ibv);
	cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void World_DrawGBuffer(ID3D12GraphicsCommandList* cl, const WorldView& v)
{
	if (!W.active || !W.pipelines) return;
	Frustum fr;
	FrustumFromMatrix(v.viewProj, fr, false);
	BindGeometry(cl);
	float allowed = Q_max(0.1f, kt_lodError ? kt_lodError->value : 1.0f) * Q_max(0.1f, v.lodError);
	int lock = kt_lodLock ? kt_lodLock->integer : -1;
	ID3D12PipelineState* last = nullptr;
	W.drawnTris = 0; W.drawnClusters = 0;
	for (const KwCluster& c : W.clusters) {
		if (!c.numLods || c.cls == KWC_FX || !BoxVisible(fr, c.boundsMin, c.boundsMax)) continue;
		float dist = Q_max(1.0f, BoxDistance(v.cam, c.boundsMin, c.boundsMax));
		uint32_t l = 0;
		if (lock >= 0) l = Q_min((uint32_t)lock, c.numLods - 1);
		else while (l + 1 < c.numLods && W.lods[c.firstLod + l + 1].error * v.pixelScale / dist <= allowed) l++;
		const KwLod& lod = W.lods[c.firstLod + l];
		ID3D12PipelineState* p = W.gbPSO[c.cls < 3 ? c.cls : 0];
		if (p != last) {
			cl->SetPipelineState(p); last = p;
			uint32_t k[4] = { W.materials.srv, 0, 0, 0 };
			cl->SetGraphicsRoot32BitConstants(RP_CONSTANTS, 4, k, 0);
		}
		cl->DrawIndexedInstanced(lod.indexCount, 1, lod.firstIndex, (INT)c.baseVertex, 0);
		W.drawnTris += lod.indexCount / 3;
		W.drawnClusters++;
	}
}

// water / flowing water: forward into the HDR target (bound by the caller), after lighting
void World_DrawFX(ID3D12GraphicsCommandList* cl, const WorldView& v)
{
	if (!W.active || !W.pipelines || !W.fxPSO || !W.hasFX) return;
	Frustum fr;
	FrustumFromMatrix(v.viewProj, fr, false);
	BindGeometry(cl);
	cl->SetPipelineState(W.fxPSO);
	uint32_t k[4] = { W.materials.srv, 0, 0, 0 };
	cl->SetGraphicsRoot32BitConstants(RP_CONSTANTS, 4, k, 0);
	for (const KwCluster& c : W.clusters) {
		if (!c.numLods || c.cls != KWC_FX || !BoxVisible(fr, c.boundsMin, c.boundsMax)) continue;
		const KwLod& lod = W.lods[c.firstLod];
		cl->DrawIndexedInstanced(lod.indexCount, 1, lod.firstIndex, (INT)c.baseVertex, 0);
	}
}

void World_DrawShadow(ID3D12GraphicsCommandList* cl, int cascade, const float* vp, float texelWorld)
{
	if (!W.active || !W.pipelines) return;
	Frustum fr;
	FrustumFromMatrix(vp, fr, true);
	BindGeometry(cl);
	ID3D12PipelineState* last = nullptr;
	for (const KwCluster& c : W.clusters) {
		if (!c.numLods || (c.flags & 1) || c.cls == KWC_FX || !BoxVisible(fr, c.boundsMin, c.boundsMax)) continue;
		uint32_t l = 0;
		while (l + 1 < c.numLods && W.lods[c.firstLod + l + 1].error <= texelWorld * 1.5f) l++;
		const KwLod& lod = W.lods[c.firstLod + l];
		ID3D12PipelineState* p = W.shPSO[c.cls == KWC_MASKED ? 1 : 0];
		if (p != last) {
			cl->SetPipelineState(p); last = p;
			uint32_t k[4] = { W.materials.srv, (uint32_t)cascade, 1, 0 };
			cl->SetGraphicsRoot32BitConstants(RP_CONSTANTS, 4, k, 0);
		}
		cl->DrawIndexedInstanced(lod.indexCount, 1, lod.firstIndex, (INT)c.baseVertex, 0);
	}
}

void World_AppendLights(std::vector<GpuLight>& out, const float* cam)
{
	if (!W.active || W.lights.empty()) return;
	std::vector<std::pair<float, uint32_t>> order;
	order.reserve(W.lights.size());
	for (uint32_t i = 0; i < W.lights.size(); i++) {
		const KwLight& l = W.lights[i];
		float d[3] = { l.pos[0] - cam[0], l.pos[1] - cam[1], l.pos[2] - cam[2] };
		order.push_back({ sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) - l.radius, i });
	}
	std::sort(order.begin(), order.end());
	for (size_t k = 0; k < order.size() && k < 256; k++) {
		const KwLight& l = W.lights[order[k].second];
		GpuLight g = {};
		memcpy(g.pos, l.pos, 12); g.radius = l.radius;
		memcpy(g.color, l.color, 12); g.type = (float)l.type;
		memcpy(g.dir, l.dir, 12); g.spotCos = l.spotCos;
		g.spotInner = l.spotInnerCos; g.invSquare = l.invSquare;
		out.push_back(g);
	}
}

void World_Stats(uint32_t& clusters, uint64_t& tris) { clusters = W.drawnClusters; tris = W.drawnTris; }

} // namespace kt
