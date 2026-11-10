// KatarnEngine - HD worlds: GPU-resident instanced meshes with PBR materials, cooked from Blender (.kworld)
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

struct InstGpu { float r[12]; float n[12]; };     // 96 bytes: object->world rows, normal matrix rows (xyz + pad)

static struct World {
	bool active = false;
	std::string name;
	Buffer vb, ib, materials;
	std::vector<Texture> textures;
	std::vector<KwMesh> meshes;
	std::vector<KwSubmesh> subs;
	std::vector<KwInstance> inst;
	std::vector<InstGpu> instGpu;
	std::vector<KwMaterial> mats;
	std::vector<std::vector<uint32_t>> byMesh;   // instance indices per mesh
	std::vector<KwLight> lights;
	WorldEnv env;
	uint32_t instSrv[kFrames][5] = {};
	ID3D12PipelineState* gbPSO[2] = {};          // cull back, two-sided
	ID3D12PipelineState* shPSO[2] = {};          // opaque, alpha-tested
	Shader vs, ps, shadowPS;
	bool pipelines = false;
	float bmin[3], bmax[3];
	std::vector<uint32_t> visible;
} W;

static WorldEnv defaultEnv;

bool World_Active() { return W.active; }
const WorldEnv& World_Env() { return W.active ? W.env : defaultEnv; }

static const D3D12_INPUT_ELEMENT_DESC kWorldLayout[] = {
	{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
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
	W.vs = R_Shader("world", "VS", "vs_5_1");
	W.ps = R_Shader("world", "PS", "ps_5_1");
	W.shadowPS = R_Shader("world", "ShadowPS", "ps_5_1");
	if (!W.vs.ok || !W.ps.ok) return false;
	for (int i = 0; i < 2; i++) {
		D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
		d.SampleMask = 0xffffffff;
		d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		d.RasterizerState.CullMode = i ? D3D12_CULL_MODE_NONE : D3D12_CULL_MODE_BACK;
		d.RasterizerState.FrontCounterClockwise = TRUE;
		d.RasterizerState.DepthClipEnable = TRUE;
		for (auto& rt : d.BlendState.RenderTarget) rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL, rt.SrcBlend = D3D12_BLEND_ONE,
			rt.DestBlend = D3D12_BLEND_ZERO, rt.BlendOp = D3D12_BLEND_OP_ADD, rt.SrcBlendAlpha = D3D12_BLEND_ONE, rt.DestBlendAlpha = D3D12_BLEND_ZERO,
			rt.BlendOpAlpha = D3D12_BLEND_OP_ADD, rt.LogicOp = D3D12_LOGIC_OP_NOOP;
		d.DepthStencilState.DepthEnable = TRUE;
		d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
		d.InputLayout = { kWorldLayout, 4 };
		d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		d.SampleDesc.Count = 1;
		d.VS = BCode(W.vs); d.PS = BCode(W.ps);
		d.NumRenderTargets = 3;
		d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; d.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT; d.RTVFormats[2] = DXGI_FORMAT_R11G11B10_FLOAT;
		d.DSVFormat = DXGI_FORMAT_D32_FLOAT;
		W.gbPSO[i] = CreateGraphicsPSO(d, "world gbuffer");
		// shadows: two-sided (Blender scenes are full of single-sided walls), slope-scaled bias
		d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		d.RasterizerState.DepthBias = 1000; d.RasterizerState.SlopeScaledDepthBias = 2.0f; d.RasterizerState.DepthBiasClamp = 0.01f;
		d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		d.NumRenderTargets = 0;
		for (auto& f : d.RTVFormats) f = DXGI_FORMAT_UNKNOWN;
		d.PS = i ? BCode(W.shadowPS) : D3D12_SHADER_BYTECODE{};
		W.shPSO[i] = CreateGraphicsPSO(d, "world shadow");
	}
	for (int f = 0; f < kFrames; f++) for (int k = 0; k < 5; k++) W.instSrv[f][k] = AllocDescriptor();
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
	W.meshes.clear(); W.subs.clear(); W.inst.clear(); W.instGpu.clear(); W.mats.clear(); W.byMesh.clear(); W.lights.clear();
	W.active = false;
	W.name.clear();
}

void World_Shutdown()
{
	World_Unload();
	for (int i = 0; i < 2; i++) {
		if (W.gbPSO[i]) W.gbPSO[i]->Release();
		if (W.shPSO[i]) W.shPSO[i]->Release();
		W.gbPSO[i] = W.shPSO[i] = nullptr;
	}
	Shader* s[] = { &W.vs, &W.ps, &W.shadowPS };
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

static void ParseEnv(const std::string& text, WorldEnv& e)
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
	}
}

static void BuildInstGpu(const KwInstance& k, InstGpu& g)
{
	memcpy(g.r, k.m, 48);
	// normal matrix = inverse transpose of the 3x3
	const float* m = k.m;
	float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], gg = m[8], h = m[9], i = m[10];
	float A = e * i - f * h, B = -(d * i - f * gg), C = d * h - e * gg;
	float D = -(b * i - c * h), E = a * i - c * gg, F = -(a * h - b * gg);
	float G = b * f - c * e, H = -(a * f - c * d), I = a * e - b * d;
	float det = a * A + b * B + c * C;
	float s = fabsf(det) > 1e-20f ? 1.0f / det : 1.0f;
	// inverse = adj^T / det; inverse transpose = adj / det (cofactor matrix)
	float n[9] = { A * s, B * s, C * s, D * s, E * s, F * s, G * s, H * s, I * s };
	g.n[0] = n[0]; g.n[1] = n[1]; g.n[2] = n[2]; g.n[3] = 0;
	g.n[4] = n[3]; g.n[5] = n[4]; g.n[6] = n[5]; g.n[7] = 0;
	g.n[8] = n[6]; g.n[9] = n[7]; g.n[10] = n[8]; g.n[11] = 0;
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
		Log("^1KatarnEngine: %s is not a version %d .kworld\n", path.c_str(), KW_VERSION);
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
	readVec(KW_MESH, W.meshes);
	readVec(KW_SUBM, W.subs);
	readVec(KW_INST, W.inst);
	readVec(KW_MATL, W.mats);
	readVec(KW_LITE, W.lights);
	std::vector<KwTexture> texs;
	readVec(KW_TEXT, texs);
	W.env = WorldEnv();
	if (const KwSection* s = find(KW_ENVT)) {
		std::string text(s->size, '\0');
		_fseeki64(f, (long long)s->offset, SEEK_SET);
		fread(&text[0], 1, s->size, f);
		ParseEnv(text, W.env);
	}
	memcpy(W.bmin, h.boundsMin, 12);
	memcpy(W.bmax, h.boundsMax, 12);

	// geometry, uploaded in chunks
	auto uploadSection = [&](uint32_t type, Buffer& buf, const char* name) -> bool {
		const KwSection* s = find(type);
		if (!s || !s->size) return false;
		if (!CreateBuffer(buf, s->size, false, name)) return false;
		std::vector<uint8_t> chunk;
		const uint64_t CH = 32ull << 20;
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
	if (!ok) { Log("^1KatarnEngine: %s has no geometry\n", path.c_str()); fclose(f); World_Unload(); return false; }

	// textures
	const KwSection* tdat = find(KW_TDAT);
	W.textures.resize(texs.size());
	uint64_t pending = 0, texBytes = 0;
	std::vector<uint8_t> data;
	for (size_t i = 0; i < texs.size(); i++) {
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

	// materials -> GPU (texture indices -> bindless views)
	std::vector<KwMaterial> gm = W.mats;
	for (auto& m : gm) {
		auto view = [&](int32_t ti, bool srgb) -> int32_t {
			if (ti < 0 || ti >= (int)W.textures.size() || !W.textures[ti].res) return 0;
			return (int32_t)(srgb ? W.textures[ti].srvSrgb : W.textures[ti].srv);
		};
		m.albedoTex = view(m.albedoTex, true);
		m.normalTex = view(m.normalTex, false);
		m.ormTex = view(m.ormTex, false);
		m.emissiveTex = view(m.emissiveTex, true);
	}
	if (gm.empty()) { KwMaterial d = {}; d.baseColor[0] = d.baseColor[1] = d.baseColor[2] = d.baseColor[3] = 0.8f; d.roughness = 0.6f; gm.push_back(d); }
	CreateBuffer(W.materials, gm.size() * sizeof(KwMaterial), false, "world materials");
	UploadBuffer(W.materials, 0, gm.data(), gm.size() * sizeof(KwMaterial));
	FlushUploads();

	W.instGpu.resize(W.inst.size());
	W.byMesh.assign(W.meshes.size(), {});
	for (size_t i = 0; i < W.inst.size(); i++) {
		BuildInstGpu(W.inst[i], W.instGpu[i]);
		if (W.inst[i].mesh < W.meshes.size()) W.byMesh[W.inst[i].mesh].push_back((uint32_t)i);
	}
	W.active = true;
	W.name = mapName;
	uint64_t tris = 0;
	for (auto& s : W.subs) tris += s.indexCount / 3;
	Log("KatarnEngine: HD world %s - %u meshes, %u instances, %llu unique triangles, %u materials, %u textures (%llu MB), %u lights, %d ms\n",
		mapName, (unsigned)W.meshes.size(), (unsigned)W.inst.size(), (unsigned long long)tris, (unsigned)W.mats.size(),
		(unsigned)W.textures.size(), (unsigned long long)(texBytes >> 20), (unsigned)W.lights.size(), ri.Milliseconds() - t0);
	return true;
}

// ------------------------------------------------------------------------------------------------ culling + drawing
struct Frustum { float p[6][4]; int n; };
static void FrustumFromMatrix(const float* m, Frustum& fr, bool ortho)
{
	// column-major m: row r = (m[r], m[4+r], m[8+r], m[12+r])
	auto row = [&](int r, float* o) { o[0] = m[r]; o[1] = m[4 + r]; o[2] = m[8 + r]; o[3] = m[12 + r]; };
	float r0[4], r1[4], r2[4], r3[4];
	row(0, r0); row(1, r1); row(2, r2); row(3, r3);
	int n = 0;
	for (int i = 0; i < 4; i++) { fr.p[n][i] = r3[i] + r0[i]; } n++;
	for (int i = 0; i < 4; i++) { fr.p[n][i] = r3[i] - r0[i]; } n++;
	for (int i = 0; i < 4; i++) { fr.p[n][i] = r3[i] + r1[i]; } n++;
	for (int i = 0; i < 4; i++) { fr.p[n][i] = r3[i] - r1[i]; } n++;
	if (ortho) {
		for (int i = 0; i < 4; i++) { fr.p[n][i] = r2[i]; } n++;          // z >= 0 (well behind: casters are kept by the deep z range)
		for (int i = 0; i < 4; i++) { fr.p[n][i] = r3[i] - r2[i]; } n++;  // z <= 1
	}
	else {
		for (int i = 0; i < 4; i++) { fr.p[n][i] = r3[i] - r2[i]; } n++;  // reversed-Z: near plane is z <= w
	}
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

static void DrawWorld(ID3D12GraphicsCommandList* cl, const float* matrix, bool shadow, int cascade)
{
	if (!W.active || !W.pipelines) return;
	Frustum fr;
	FrustumFromMatrix(matrix, fr, shadow);
	// visible instances grouped by mesh, written into the upload ring
	std::vector<uint32_t> counts(W.meshes.size(), 0), starts(W.meshes.size(), 0);
	W.visible.clear();
	for (size_t mi = 0; mi < W.meshes.size(); mi++) {
		starts[mi] = (uint32_t)W.visible.size();
		for (uint32_t ii : W.byMesh[mi]) {
			const KwInstance& k = W.inst[ii];
			if (shadow && (k.flags & 1)) continue;
			if (!BoxVisible(fr, k.boundsMin, k.boundsMax)) continue;
			W.visible.push_back(ii);
		}
		counts[mi] = (uint32_t)W.visible.size() - starts[mi];
	}
	if (W.visible.empty()) return;
	UploadAlloc a = Upload(W.visible.size() * sizeof(InstGpu), 256);
	if (!a.cpu) return;
	InstGpu* dst = (InstGpu*)a.cpu;
	for (size_t i = 0; i < W.visible.size(); i++) dst[i] = W.instGpu[W.visible[i]];
	uint32_t srvIdx = W.instSrv[g.frameIndex][shadow ? 1 + cascade : 0];
	D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
	sd.Format = DXGI_FORMAT_R32_TYPELESS;
	sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	sd.Buffer.FirstElement = a.offset / 4;
	sd.Buffer.NumElements = (UINT)(W.visible.size() * sizeof(InstGpu) / 4);
	sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
	g.dev->CreateShaderResourceView(a.res, &sd, CpuHandle(srvIdx));

	Transition(cl, W.vb, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	D3D12_VERTEX_BUFFER_VIEW vbv = { W.vb.va, (UINT)W.vb.size, sizeof(KwVertex) };
	D3D12_INDEX_BUFFER_VIEW ibv = { W.ib.va, (UINT)W.ib.size, DXGI_FORMAT_R32_UINT };
	cl->IASetVertexBuffers(0, 1, &vbv);
	cl->IASetIndexBuffer(&ibv);
	cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ID3D12PipelineState* last = nullptr;
	for (size_t mi = 0; mi < W.meshes.size(); mi++) {
		if (!counts[mi]) continue;
		const KwMesh& mesh = W.meshes[mi];
		for (uint32_t si = 0; si < mesh.numSubmeshes; si++) {
			const KwSubmesh& sm = W.subs[mesh.firstSubmesh + si];
			uint32_t matFlags = sm.material < W.mats.size() ? W.mats[sm.material].flags : 0;
			ID3D12PipelineState* p = shadow ? W.shPSO[(matFlags & KWM_MASK) ? 1 : 0] : W.gbPSO[(matFlags & KWM_TWOSIDED) ? 1 : 0];
			if (!p) continue;
			if (p != last) { cl->SetPipelineState(p); last = p; }
			uint32_t c[8] = { starts[mi], sm.material < W.mats.size() ? sm.material : 0, srvIdx, W.materials.srv, (uint32_t)cascade, shadow ? 1u : 0u, 0, 0 };
			cl->SetGraphicsRoot32BitConstants(RP_CONSTANTS, 8, c, 0);
			cl->DrawIndexedInstanced(sm.indexCount, counts[mi], sm.firstIndex, (INT)mesh.baseVertex, 0);
		}
	}
}

void World_DrawGBuffer(ID3D12GraphicsCommandList* cl, const float* viewProj) { DrawWorld(cl, viewProj, false, 0); }
void World_DrawShadow(ID3D12GraphicsCommandList* cl, int cascade, const float* vp) { DrawWorld(cl, vp, true, cascade); }

void World_AppendLights(std::vector<GpuLight>& out, const float* cam)
{
	if (!W.active || W.lights.empty()) return;
	// nearest lights first (the lighting pass loops over all of them)
	std::vector<std::pair<float, uint32_t>> order;
	order.reserve(W.lights.size());
	for (uint32_t i = 0; i < W.lights.size(); i++) {
		const KwLight& l = W.lights[i];
		float d[3] = { l.pos[0] - cam[0], l.pos[1] - cam[1], l.pos[2] - cam[2] };
		float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) - l.radius;
		order.push_back({ dist, i });
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

} // namespace kt
