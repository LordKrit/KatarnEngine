/*
tr_hdworld.cpp - "HD world": high-poly static geometry loaded next to a BSP map.

When a map loads, maps/<name>.hdw (and maps/<name>.1.hdw, .2.hdw ... for large worlds) is
read if present. Each file holds material names and chunks of indexed triangles exported
from Blender (tools/hdmap/export_hdw.py). Chunks become static VBO/IBO surfaces drawn as
part of the world: frustum-culled per chunk, lit like models (sun + ambient), and they
cast sun shadows. The BSP still provides sky, entities and (for now) collision.

File layout (little endian):
  "HDW1" u32 version u32 numShaders u32 numChunks
  numShaders x char[64]
  numChunks x { u32 shader, f32 mins[3], f32 maxs[3], u32 numVerts, u32 numIndexes,
                numVerts x { f32 pos[3], f32 normal[3], f32 uv[2] }, numIndexes x u32 }
*/

#include "tr_local.h"
#include "kworld_format.h"
#include <vector>
#include <string>
#include <algorithm>

struct hdChunk_t
{
	srfVBOMDVMesh_t surf;
	shader_t* shader;
	vec3_t mins, maxs;
};

static std::vector<hdChunk_t*> hdChunks;
static cvar_t* r_hdWorld;
static cvar_t* r_hdAmbient;
static cvar_t* r_hdSun;

static void R_HDLoadFile(const char* path)
{
	byte* buf = nullptr;
	const int len = ri.FS_ReadFile(path, (void**)&buf);
	if (len <= 16 || !buf)
		return;

	const byte* p = buf;
	const byte* end = buf + len;
	if (memcmp(p, "HDW1", 4) != 0)
	{
		ri.Printf(PRINT_WARNING, "hdworld: %s is not an HDW1 file\n", path);
		ri.FS_FreeFile(buf);
		return;
	}
	uint32_t numShaders, numChunks;
	memcpy(&numShaders, p + 8, 4);
	memcpy(&numChunks, p + 12, 4);
	p += 16;

	std::vector<shader_t*> shaders(numShaders);
	for (uint32_t i = 0; i < numShaders && p + 64 <= end; i++, p += 64)
	{
		char name[65];
		memcpy(name, p, 64);
		name[64] = 0;
		shaders[i] = R_FindShader(name, lightmapsNone, stylesDefault, qtrue);
	}

	int totalTris = 0;
	for (uint32_t c = 0; c < numChunks; c++)
	{
		if (p + 36 > end) break;
		uint32_t sh, nv, ni;
		hdChunk_t* ch = new hdChunk_t();
		memcpy(&sh, p, 4);
		memcpy(ch->mins, p + 4, 12);
		memcpy(ch->maxs, p + 16, 12);
		memcpy(&nv, p + 28, 4);
		memcpy(&ni, p + 32, 4);
		p += 36;
		const size_t vbytes = (size_t)nv * 32, ibytes = (size_t)ni * 4;
		if (p + vbytes + ibytes > end) { delete ch; break; }
		const float* src = (const float*)p;
		const glIndex_t* idx = (const glIndex_t*)(p + vbytes);
		p += vbytes + ibytes;

		// interleaved: position, packed normal, uv, packed tangent; then the constant
		// colour / lightmap uv / light direction the model shaders expect
		const int stride = sizeof(vec3_t) + sizeof(uint32_t) + sizeof(vec2_t) + sizeof(uint32_t);
		const int dataSize = nv * stride + sizeof(vec4_t) + sizeof(vec2_t) + sizeof(uint32_t);
		byte* data = (byte*)Hunk_AllocateTempMemory(dataSize);

		// tangents from uv derivatives, accumulated per vertex
		vec3_t* tan = (vec3_t*)Hunk_AllocateTempMemory(sizeof(vec3_t) * nv);
		vec3_t* bit = (vec3_t*)Hunk_AllocateTempMemory(sizeof(vec3_t) * nv);
		memset(tan, 0, sizeof(vec3_t) * nv);
		memset(bit, 0, sizeof(vec3_t) * nv);
		for (uint32_t t = 0; t + 2 < ni; t += 3)
		{
			const float* v0 = src + idx[t] * 8, * v1 = src + idx[t + 1] * 8, * v2 = src + idx[t + 2] * 8;
			vec3_t e1, e2;
			VectorSubtract(v1, v0, e1);
			VectorSubtract(v2, v0, e2);
			const float du1 = v1[6] - v0[6], dv1 = v1[7] - v0[7], du2 = v2[6] - v0[6], dv2 = v2[7] - v0[7];
			float r = du1 * dv2 - du2 * dv1;
			if (fabsf(r) < 1e-12f) continue;
			r = 1.0f / r;
			vec3_t sdir, tdir;
			for (int k = 0; k < 3; k++)
			{
				sdir[k] = (e1[k] * dv2 - e2[k] * dv1) * r;
				tdir[k] = (e2[k] * du1 - e1[k] * du2) * r;
			}
			for (int k = 0; k < 3; k++)
			{
				VectorAdd(tan[idx[t + k]], sdir, tan[idx[t + k]]);
				VectorAdd(bit[idx[t + k]], tdir, bit[idx[t + k]]);
			}
		}
		for (uint32_t v = 0; v < nv; v++)
		{
			byte* o = data + v * stride;
			const float* s = src + v * 8;
			vec3_t n = { s[3], s[4], s[5] };
			memcpy(o, s, 12);
			*(uint32_t*)(o + 12) = R_VboPackNormal(n);
			memcpy(o + 16, s + 6, 8);
			// Gram-Schmidt tangent with handedness in w
			vec3_t tv;
			VectorMA(tan[v], -DotProduct(n, tan[v]), n, tv);
			if (VectorNormalize(tv) < 1e-6f) PerpendicularVector(tv, n);
			vec3_t cr;
			CrossProduct(n, tv, cr);
			vec4_t t4 = { tv[0], tv[1], tv[2], DotProduct(cr, bit[v]) < 0.0f ? -1.0f : 1.0f };
			*(uint32_t*)(o + 24) = R_VboPackTangent(t4);
		}
		Hunk_FreeTempMemory(bit);
		Hunk_FreeTempMemory(tan);

		const int ofsColor = nv * stride;
		VectorSet4((float*)(data + ofsColor), 1.0f, 1.0f, 1.0f, 1.0f);
		const int ofsLM = ofsColor + sizeof(vec4_t);
		VectorSet2((float*)(data + ofsLM), 0.0f, 0.0f);
		const int ofsLD = ofsLM + sizeof(vec2_t);
		*(uint32_t*)(data + ofsLD) = 0;

		VBO_t* vbo = R_CreateVBO(data, dataSize, VBO_USAGE_STATIC, va("hdw%d", c));
		IBO_t* ibo = R_CreateIBO((byte*)idx, ibytes, VBO_USAGE_STATIC, va("hdw%d", c));
		Hunk_FreeTempMemory(data);

		vbo->offsets[ATTR_INDEX_POSITION] = 0;
		vbo->offsets[ATTR_INDEX_NORMAL] = 12;
		vbo->offsets[ATTR_INDEX_TEXCOORD0] = 16;
		vbo->offsets[ATTR_INDEX_TANGENT] = 24;
		vbo->offsets[ATTR_INDEX_COLOR] = ofsColor;
		vbo->offsets[ATTR_INDEX_TEXCOORD1] = ofsLM;
		vbo->offsets[ATTR_INDEX_TEXCOORD2] = ofsLM;
		vbo->offsets[ATTR_INDEX_TEXCOORD3] = ofsLM;
		vbo->offsets[ATTR_INDEX_TEXCOORD4] = ofsLM;
		vbo->offsets[ATTR_INDEX_LIGHTDIRECTION] = ofsLD;
		for (int a : { ATTR_INDEX_POSITION, ATTR_INDEX_NORMAL, ATTR_INDEX_TEXCOORD0, ATTR_INDEX_TANGENT })
			vbo->strides[a] = stride;
		vbo->sizes[ATTR_INDEX_POSITION] = sizeof(vec3_t);
		vbo->sizes[ATTR_INDEX_NORMAL] = sizeof(uint32_t);
		vbo->sizes[ATTR_INDEX_TEXCOORD0] = sizeof(vec2_t);
		vbo->sizes[ATTR_INDEX_TANGENT] = sizeof(uint32_t);
		vbo->strides[ATTR_INDEX_COLOR] = vbo->sizes[ATTR_INDEX_COLOR] = sizeof(vec4_t);
		for (int a : { ATTR_INDEX_TEXCOORD1, ATTR_INDEX_TEXCOORD2, ATTR_INDEX_TEXCOORD3, ATTR_INDEX_TEXCOORD4 })
			vbo->strides[a] = vbo->sizes[a] = sizeof(vec2_t);
		vbo->strides[ATTR_INDEX_LIGHTDIRECTION] = vbo->sizes[ATTR_INDEX_LIGHTDIRECTION] = sizeof(uint32_t);
		for (int a : { ATTR_INDEX_COLOR, ATTR_INDEX_TEXCOORD1, ATTR_INDEX_TEXCOORD2, ATTR_INDEX_TEXCOORD3, ATTR_INDEX_TEXCOORD4, ATTR_INDEX_LIGHTDIRECTION })
			vbo->stepRates[a] = MAX_INSTANCES;

		ch->surf.surfaceType = SF_VBO_MDVMESH;
		ch->surf.mdvModel = nullptr;
		ch->surf.mdvSurface = nullptr;
		ch->surf.vbo = vbo;
		ch->surf.ibo = ibo;
		ch->surf.indexOffset = 0;
		ch->surf.numIndexes = ni;
		ch->surf.numVerts = nv;
		ch->surf.minIndex = 0;
		ch->surf.maxIndex = nv - 1;
		ch->shader = (sh < numShaders && shaders[sh]) ? shaders[sh] : tr.defaultShader;
		hdChunks.push_back(ch);
		totalTris += ni / 3;
	}
	ri.FS_FreeFile(buf);
	ri.Printf(PRINT_ALL, "hdworld: %d triangles loaded\n", totalTris);
}


/*
=====================================================================================================
kworld v3 (KatarnEngine HD world, cooked by tools/katarn/katarn_cook): maps/<name>.kworld next to the
collision BSP maps/<name>.bsp. Clusters with LOD chains, per-vertex baked light (VCOL), material
shaders by name (MNAM, shaders/kw_<map>.shader), DDS textures (tr_image.cpp), a light grid for
entities (LGRD). Each cluster gets one VBO; its LODs are index ranges sorted by material, so a
cluster at one LOD is a handful of draws.
=====================================================================================================
*/
#define KW_MAX_LODS 8
struct kwDraw_t
{
	srfVBOMDVMesh_t surf;
	shader_t* shader;
};
struct kwCluster_t
{
	vec3_t mins, maxs;
	int numLods;
	float lodError[KW_MAX_LODS];
	int firstDraw[KW_MAX_LODS + 1];
	uint32_t cls, flags;
};
static std::vector<kwCluster_t> kwClusters;
static std::vector<kwDraw_t> kwDraws;
static KwGridHeader kwGrid;
static std::vector<KwGridPoint> kwGridPts;
static vec3_t kwSunDir = { 0.35f, 0.25f, 0.9f };
static cvar_t* r_kwLodPixels;
static cvar_t* r_kwLight;

struct kwFile_t
{
	FILE* f = nullptr;
	byte* mem = nullptr;
	long memLen = 0;
	bool read(uint64_t off, void* dst, uint64_t size)
	{
		if (f)
		{
#ifdef _WIN32
			if (_fseeki64(f, (long long)off, SEEK_SET)) return false;
#else
			if (fseeko(f, (off_t)off, SEEK_SET)) return false;
#endif
			return fread(dst, 1, (size_t)size, f) == size;
		}
		if (!mem || off + size > (uint64_t)memLen) return false;
		memcpy(dst, mem + off, (size_t)size);
		return true;
	}
	void close()
	{
		if (f) fclose(f);
		if (mem) ri.FS_FreeFile(mem);
		f = nullptr; mem = nullptr;
	}
};

static bool R_KWOpen(const char* base, kwFile_t& kf)
{
	// loose file (a cooked world is large; read it piece by piece), else from a pk3
	const char* roots[] = { ri.Cvar_VariableString("fs_homepath"), ri.Cvar_VariableString("fs_basepath"), ri.Cvar_VariableString("fs_cdpath") };
	const char* dirs[] = { ri.Cvar_VariableString("fs_game"), "base" };
	for (const char* r : roots)
	{
		if (!r || !*r) continue;
		for (const char* d : dirs)
		{
			if (!d || !*d) continue;
			std::string p = std::string(r) + "/" + d + "/" + base + ".kworld";
			if ((kf.f = fopen(p.c_str(), "rb")) != nullptr)
				return true;
		}
	}
	kf.memLen = ri.FS_ReadFile(va("%s.kworld", base), (void**)&kf.mem);
	return kf.memLen > (long)sizeof(KwHeader) && kf.mem;
}

static void OctDecode(const int16_t* o, vec3_t n)
{
	float x = o[0] / 32767.0f, y = o[1] / 32767.0f, z = 1.0f - fabsf(x) - fabsf(y);
	if (z < 0) { float tx = (1 - fabsf(y)) * (x >= 0 ? 1.0f : -1.0f), ty = (1 - fabsf(x)) * (y >= 0 ? 1.0f : -1.0f); x = tx; y = ty; }
	VectorSet(n, x, y, z);
	VectorNormalize(n);
}
// VCOL / LGRD encoding: byte = sqrt(light / 4) * 255, light 1 = the texture as painted (linear)
static inline float KWDecode(uint8_t b) { const float v = b / 255.0f; return v * v * 4.0f; }

static void R_KWLoad(const char* base)
{
	kwFile_t kf;
	if (!R_KWOpen(base, kf))
		return;
	KwHeader h;
	if (!kf.read(0, &h, sizeof(h)) || memcmp(h.magic, KW_MAGIC, 8) || h.version < 3 || h.numSections > 64)
	{
		ri.Printf(PRINT_WARNING, "kworld: %s.kworld is not a v3 kworld (cook it with katarn_cook --format 3)\n", base);
		kf.close();
		return;
	}
	std::vector<KwSection> secs(h.numSections);
	kf.read(sizeof(h), secs.data(), secs.size() * sizeof(KwSection));
	auto find = [&](uint32_t t) -> const KwSection* { for (auto& s : secs) if (s.type == t && s.size) return &s; return nullptr; };
	auto readAll = [&](uint32_t t, auto& vec) {
		vec.clear();
		const KwSection* s = find(t);
		if (!s) return;
		vec.resize(s->size / sizeof(vec[0]));
		kf.read(s->offset, vec.data(), vec.size() * sizeof(vec[0]));
	};
	std::vector<KwCluster> clus;
	std::vector<KwLod> lods;
	std::vector<char> mnam, envt, lgrd;
	std::vector<uint32_t> vcol;
	readAll(KW_CLUS, clus);
	readAll(KW_LODS, lods);
	readAll(KW_MNAM, mnam);
	readAll(KW_ENVT, envt);
	readAll(KW_LGRD, lgrd);
	readAll(KW_VCOL, vcol);
	const KwSection* sv = find(KW_VERT), * si = find(KW_INDX);
	if (!sv || !si || clus.empty())
	{
		kf.close();
		return;
	}
	// environment: the sun
	{
		std::string e(envt.begin(), envt.end());
		size_t p = e.find("sunDir ");
		if (p != std::string::npos && sscanf(e.c_str() + p + 7, "%f %f %f", &kwSunDir[0], &kwSunDir[1], &kwSunDir[2]) == 3)
			VectorNormalize(kwSunDir);
	}
	if (lgrd.size() >= sizeof(KwGridHeader))
	{
		memcpy(&kwGrid, lgrd.data(), sizeof(KwGridHeader));
		const size_t n = (size_t)kwGrid.dims[0] * kwGrid.dims[1] * kwGrid.dims[2];
		if (n && lgrd.size() >= sizeof(KwGridHeader) + n * sizeof(KwGridPoint))
		{
			kwGridPts.resize(n);
			memcpy(kwGridPts.data(), lgrd.data() + sizeof(KwGridHeader), n * sizeof(KwGridPoint));
		}
	}
	const size_t numMats = mnam.size() / 64;
	std::vector<shader_t*> shaders(numMats, nullptr);
	auto shaderFor = [&](uint32_t m) -> shader_t* {
		if (m >= numMats) return tr.defaultShader;
		if (!shaders[m])
		{
			char nm[65];
			memcpy(nm, &mnam[m * 64], 64);
			nm[64] = 0;
			shaders[m] = R_FindShader(nm, lightmapsVertex, stylesDefault, qtrue);
			if (!shaders[m]) shaders[m] = tr.defaultShader;
		}
		return shaders[m];
	};
	const bool haveLight = vcol.size() * sizeof(KwVertex) >= sv->size;
	const uint32_t packedSun = R_VboPackNormal(kwSunDir);

	struct packedKw_t { vec3_t pos; uint32_t normal, tangent; vec2_t uv; vec4_t color; uint32_t lightDir; };
	std::vector<KwVertex> kv;
	std::vector<uint32_t> ki;
	std::vector<packedKw_t> pv;
	std::vector<glIndex_t> out;
	std::vector<uint32_t> tri;
	size_t totalTris = 0, totalVerts = 0;
	kwClusters.reserve(clus.size());
	for (size_t ci = 0; ci < clus.size(); ci++)
	{
		const KwCluster& c = clus[ci];
		if (!c.numVertices || !c.numLods || c.firstLod + c.numLods > lods.size()) continue;
		kv.resize(c.numVertices);
		if (!kf.read(sv->offset + (uint64_t)c.baseVertex * sizeof(KwVertex), kv.data(), kv.size() * sizeof(KwVertex))) continue;
		const KwLod& l0 = lods[c.firstLod];
		const KwLod& ll = lods[c.firstLod + c.numLods - 1];
		const uint32_t i0 = l0.firstIndex, i1 = std::max(l0.firstIndex + l0.indexCount, ll.firstIndex + ll.indexCount);
		ki.resize(i1 - i0);
		if (!kf.read(si->offset + (uint64_t)i0 * 4, ki.data(), ki.size() * 4)) continue;

		pv.resize(c.numVertices);
		for (uint32_t v = 0; v < c.numVertices; v++)
		{
			const KwVertex& s = kv[v];
			packedKw_t& o = pv[v];
			VectorCopy(s.pos, o.pos);
			vec3_t n, t;
			OctDecode(s.normal, n);
			OctDecode(s.tangent, t);
			o.normal = R_VboPackNormal(n);
			vec4_t t4 = { t[0], t[1], t[2], s.tangentSign < 0 ? -1.0f : 1.0f };
			o.tangent = R_VboPackTangent(t4);
			o.uv[0] = s.uv[0]; o.uv[1] = s.uv[1];
			if (haveLight)
			{
				const uint32_t c32 = vcol[c.baseVertex + v];
				// rend2 shades in gamma space: the linear light multiplier, gamma encoded
				for (int k = 0; k < 3; k++)
					o.color[k] = powf(KWDecode((c32 >> (8 * k)) & 255) * r_kwLight->value, 1.0f / 2.2f);
			}
			else
				VectorSet(o.color, 0.85f, 0.85f, 0.85f);
			o.color[3] = 1.0f;
			o.lightDir = packedSun;
		}

		kwCluster_t kc = {};
		VectorCopy(c.boundsMin, kc.mins);
		VectorCopy(c.boundsMax, kc.maxs);
		kc.cls = c.cls;
		kc.flags = c.flags;
		kc.numLods = std::min<int>(c.numLods, KW_MAX_LODS);
		out.clear();
		struct run_t { uint32_t mat, first, count; };
		std::vector<run_t> runs;
		std::vector<int> lodRunStart;
		for (int l = 0; l < kc.numLods; l++)
		{
			const KwLod& L = lods[c.firstLod + l];
			kc.lodError[l] = L.error;
			lodRunStart.push_back((int)runs.size());
			const uint32_t* idx = ki.data() + (L.firstIndex - i0);
			const uint32_t nt = L.indexCount / 3;
			tri.resize(nt);
			for (uint32_t t = 0; t < nt; t++) tri[t] = t;
			std::stable_sort(tri.begin(), tri.end(), [&](uint32_t a, uint32_t b) { return kv[idx[a * 3]].material < kv[idx[b * 3]].material; });
			for (uint32_t t = 0; t < nt; t++)
			{
				const uint32_t m = kv[idx[tri[t] * 3]].material;
				if (runs.size() == (size_t)lodRunStart.back() || runs.back().mat != m)
					runs.push_back({ m, (uint32_t)out.size(), 0 });
				for (int k = 0; k < 3; k++) out.push_back(idx[tri[t] * 3 + k]);
				runs.back().count += 3;
			}
			if (l == 0) totalTris += nt;
		}
		lodRunStart.push_back((int)runs.size());
		if (out.empty()) continue;

		VBO_t* vbo = R_CreateVBO((byte*)pv.data(), (int)(pv.size() * sizeof(packedKw_t)), VBO_USAGE_STATIC, va("kw%d", (int)ci));
		IBO_t* ibo = R_CreateIBO((byte*)out.data(), (int)(out.size() * sizeof(glIndex_t)), VBO_USAGE_STATIC, va("kw%d", (int)ci));
		const int stride = sizeof(packedKw_t);
		vbo->offsets[ATTR_INDEX_POSITION] = offsetof(packedKw_t, pos);
		vbo->offsets[ATTR_INDEX_NORMAL] = offsetof(packedKw_t, normal);
		vbo->offsets[ATTR_INDEX_TANGENT] = offsetof(packedKw_t, tangent);
		vbo->offsets[ATTR_INDEX_TEXCOORD0] = offsetof(packedKw_t, uv);
		vbo->offsets[ATTR_INDEX_TEXCOORD1] = offsetof(packedKw_t, uv);
		vbo->offsets[ATTR_INDEX_TEXCOORD2] = offsetof(packedKw_t, uv);
		vbo->offsets[ATTR_INDEX_TEXCOORD3] = offsetof(packedKw_t, uv);
		vbo->offsets[ATTR_INDEX_TEXCOORD4] = offsetof(packedKw_t, uv);
		vbo->offsets[ATTR_INDEX_COLOR] = offsetof(packedKw_t, color);
		vbo->offsets[ATTR_INDEX_LIGHTDIRECTION] = offsetof(packedKw_t, lightDir);
		for (int a : { ATTR_INDEX_POSITION, ATTR_INDEX_NORMAL, ATTR_INDEX_TANGENT, ATTR_INDEX_TEXCOORD0, ATTR_INDEX_TEXCOORD1, ATTR_INDEX_TEXCOORD2,
			ATTR_INDEX_TEXCOORD3, ATTR_INDEX_TEXCOORD4, ATTR_INDEX_COLOR, ATTR_INDEX_LIGHTDIRECTION })
			vbo->strides[a] = stride;
		vbo->sizes[ATTR_INDEX_POSITION] = sizeof(vec3_t);
		vbo->sizes[ATTR_INDEX_NORMAL] = sizeof(uint32_t);
		vbo->sizes[ATTR_INDEX_TANGENT] = sizeof(uint32_t);
		for (int a : { ATTR_INDEX_TEXCOORD0, ATTR_INDEX_TEXCOORD1, ATTR_INDEX_TEXCOORD2, ATTR_INDEX_TEXCOORD3, ATTR_INDEX_TEXCOORD4 })
			vbo->sizes[a] = sizeof(vec2_t);
		vbo->sizes[ATTR_INDEX_COLOR] = sizeof(vec4_t);
		vbo->sizes[ATTR_INDEX_LIGHTDIRECTION] = sizeof(uint32_t);

		for (int l = 0; l <= kc.numLods; l++)
			kc.firstDraw[l] = (int)kwDraws.size() + lodRunStart[l];
		for (const run_t& r : runs)
		{
			kwDraw_t d = {};
			d.surf.surfaceType = SF_VBO_MDVMESH;
			d.surf.vbo = vbo;
			d.surf.ibo = ibo;
			d.surf.indexOffset = r.first;
			d.surf.numIndexes = r.count;
			d.surf.numVerts = c.numVertices;
			d.surf.minIndex = 0;
			d.surf.maxIndex = c.numVertices - 1;
			d.shader = shaderFor(r.mat);
			kwDraws.push_back(d);
		}
		kwClusters.push_back(kc);
		totalVerts += c.numVertices;
	}
	kf.close();
	ri.Printf(PRINT_ALL, "kworld: %s.kworld: %d clusters, %d draws, %d triangles at full detail, %d vertices, %s, light grid %s\n", base,
		(int)kwClusters.size(), (int)kwDraws.size(), (int)totalTris, (int)totalVerts, haveLight ? "baked light" : "no baked light (cook without --bake 0)",
		kwGridPts.empty() ? "none" : va("%ux%ux%u", kwGrid.dims[0], kwGrid.dims[1], kwGrid.dims[2]));
}

static void R_KWShutdown(void)
{
	kwClusters.clear();
	kwDraws.clear();
	kwGridPts.clear();
	memset(&kwGrid, 0, sizeof(kwGrid));
}

static void R_KWAddSurfaces(viewParms_t* viewParms)
{
	const int planeBits = (viewParms->flags & VPF_FARPLANEFRUSTUM) ? 5 : 4;
	const bool shadowPass = (viewParms->flags & VPF_DEPTHSHADOW) != 0;
	// pixels per world unit at distance 1
	const float fovY = viewParms->fovY > 1.0f ? viewParms->fovY : 75.0f;
	const float projScale = viewParms->viewportHeight / (2.0f * tanf(DEG2RAD(fovY) * 0.5f));
	const float maxPx = Q_max(0.25f, r_kwLodPixels->value) * (shadowPass ? 3.0f : 1.0f);
	for (const kwCluster_t& c : kwClusters)
	{
		if (shadowPass && ((c.flags & 1) || c.cls == KWC_FX))
			continue;
		bool culled = false;
		for (int i = 0; i < planeBits && !culled; i++)
		{
			const cplane_t* fr = &viewParms->frustum[i];
			vec3_t far_;
			for (int k = 0; k < 3; k++)
				far_[k] = fr->normal[k] >= 0 ? c.maxs[k] : c.mins[k];
			if (DotProduct(far_, fr->normal) < fr->dist)
				culled = true;
		}
		if (culled)
			continue;
		float d2 = 0;
		for (int k = 0; k < 3; k++)
		{
			const float g = Q_max(0.0f, Q_max(c.mins[k] - viewParms->ori.origin[k], viewParms->ori.origin[k] - c.maxs[k]));
			d2 += g * g;
		}
		const float dist = Q_max(1.0f, sqrtf(d2));
		int lod = 0;
		for (int l = c.numLods - 1; l > 0; l--)
			if (c.lodError[l] * projScale / dist <= maxPx) { lod = l; break; }
		for (int d = c.firstDraw[lod]; d < c.firstDraw[lod + 1]; d++)
		{
			kwDraw_t& dr = kwDraws[d];
			if (shadowPass && dr.shader->sort > SS_OPAQUE)
				continue;
			R_AddDrawSurf((surfaceType_t*)&dr.surf, REFENTITYNUM_WORLD, dr.shader, 0, 0, 0, 0);
		}
	}
}

// light for models from the cooked light grid (trilinear); false: outside the grid
static bool R_KWEntityLighting(trRefEntity_t* ent)
{
	if (kwGridPts.empty())
		return false;
	vec3_t org;
	if (ent->e.renderfx & RF_LIGHTING_ORIGIN) VectorCopy(ent->e.lightingOrigin, org);
	else VectorCopy(ent->e.origin, org);
	org[2] += 32.0f;
	int i0[3];
	float f[3];
	for (int k = 0; k < 3; k++)
	{
		float g = (org[k] - kwGrid.origin[k]) / kwGrid.step[k];
		g = Q_max(0.0f, Q_min(g, (float)kwGrid.dims[k] - 1.001f));
		i0[k] = (int)g;
		f[k] = g - i0[k];
	}
	vec3_t amb = { 0, 0, 0 }, dir = { 0, 0, 0 };
	for (int c = 0; c < 8; c++)
	{
		int x = i0[0] + (c & 1), y = i0[1] + ((c >> 1) & 1), z = i0[2] + ((c >> 2) & 1);
		x = Q_min(x, (int)kwGrid.dims[0] - 1); y = Q_min(y, (int)kwGrid.dims[1] - 1); z = Q_min(z, (int)kwGrid.dims[2] - 1);
		const float w = ((c & 1) ? f[0] : 1 - f[0]) * ((c & 2) ? f[1] : 1 - f[1]) * ((c & 4) ? f[2] : 1 - f[2]);
		const KwGridPoint& p = kwGridPts[((size_t)z * kwGrid.dims[1] + y) * kwGrid.dims[0] + x];
		for (int k = 0; k < 3; k++)
		{
			amb[k] += KWDecode(p.ambient[k]) * w;
			dir[k] += KWDecode(p.directed[k]) * w;
		}
	}
	const float s = r_kwLight->value;
	for (int k = 0; k < 3; k++)
	{
		// rend2 entity light: 0..255, gamma space
		ent->ambientLight[k] = 255.0f * powf(Q_max(0.0f, amb[k] * s * 0.6f), 1.0f / 2.2f);
		ent->directedLight[k] = 255.0f * powf(Q_max(0.0f, dir[k] * s * 0.8f), 1.0f / 2.2f);
	}
	VectorCopy(kwSunDir, ent->lightDir);
	return true;
}

void R_HDWorldLoad(const char* bspName)
{
	if (!r_hdWorld)
	{
		r_hdWorld = ri.Cvar_Get("r_hdWorld", "1", CVAR_ARCHIVE);
		r_hdAmbient = ri.Cvar_Get("r_hdAmbient", "0.45 0.42 0.38", CVAR_ARCHIVE);
		r_hdSun = ri.Cvar_Get("r_hdSun", "1.0 0.95 0.85", CVAR_ARCHIVE);
		r_kwLodPixels = ri.Cvar_Get("r_kwLodPixels", "1.5", CVAR_ARCHIVE);
		r_kwLight = ri.Cvar_Get("r_kwLight", "1", CVAR_ARCHIVE | CVAR_LATCH);
	}
	hdChunks.clear();
	R_KWShutdown();   // VBOs/IBOs are freed with the rest of the renderer's buffers

	char base[MAX_QPATH];
	COM_StripExtension(bspName, base, sizeof(base));
	R_KWLoad(base);
	R_HDLoadFile(va("%s.hdw", base));
	for (int part = 1; part < 64; part++)
	{
		const char* p = va("%s.%d.hdw", base, part);
		if (ri.FS_ReadFile(p, nullptr) <= 0)
			break;
		R_HDLoadFile(p);
	}
}

void R_HDWorldShutdown(void)
{
	for (hdChunk_t* c : hdChunks)
		delete c;
	hdChunks.clear();
	R_KWShutdown();
}

qboolean R_HDWorldActive(void)
{
	return (qboolean)((!hdChunks.empty() || !kwClusters.empty()) && r_hdWorld && r_hdWorld->integer);
}

// world entity lighting for the HD meshes: map sun + an ambient fill
void R_HDWorldSetupLighting(void)
{
	if (!R_HDWorldActive())
		return;
	trRefEntity_t* ent = &tr.worldEntity;
	vec3_t amb = { 0.45f, 0.42f, 0.38f }, sun = { 1, 0.95f, 0.85f };
	sscanf(r_hdAmbient->string, "%f %f %f", &amb[0], &amb[1], &amb[2]);
	sscanf(r_hdSun->string, "%f %f %f", &sun[0], &sun[1], &sun[2]);
	VectorScale(amb, 255.0f, ent->ambientLight);
	VectorScale(sun, 255.0f, ent->directedLight);
	VectorCopy(tr.sunDirection, ent->lightDir);
	VectorCopy(tr.sunDirection, ent->modelLightDir);
	ent->ambientCube[0][3] = 0.0f;
	ent->lightingCalculated = qtrue;
}

void R_HDWorldAddSurfaces(viewParms_t* viewParms)
{
	if (!R_HDWorldActive())
		return;
	R_HDWorldSetupLighting();
	if (!kwClusters.empty())
		R_KWAddSurfaces(viewParms);
	const int planeBits = (viewParms->flags & VPF_FARPLANEFRUSTUM) ? 5 : 4;
	for (hdChunk_t* c : hdChunks)
	{
		// frustum cull against the view planes
		bool culled = false;
		for (int i = 0; i < planeBits && !culled; i++)
		{
			const cplane_t* fr = &viewParms->frustum[i];
			vec3_t far_;
			for (int k = 0; k < 3; k++)
				far_[k] = fr->normal[k] >= 0 ? c->maxs[k] : c->mins[k];
			if (DotProduct(far_, fr->normal) < fr->dist)
				culled = true;
		}
		if (culled)
			continue;
		if (viewParms->flags & VPF_DEPTHSHADOW && c->shader->sort > SS_OPAQUE)
			continue;
		R_AddDrawSurf((surfaceType_t*)&c->surf, REFENTITYNUM_WORLD, c->shader, 0, 0, 0, 0);
	}
}

void R_HDWorldEntityLighting(trRefEntity_t* ent)
{
	if (R_KWEntityLighting(ent))
		return;
	vec3_t amb = { 0.45f, 0.42f, 0.38f }, sun = { 1, 0.95f, 0.85f };
	sscanf(r_hdAmbient->string, "%f %f %f", &amb[0], &amb[1], &amb[2]);
	sscanf(r_hdSun->string, "%f %f %f", &sun[0], &sun[1], &sun[2]);
	VectorScale(amb, 255.0f * 0.8f, ent->ambientLight);
	VectorScale(sun, 255.0f * 0.6f, ent->directedLight);
	VectorCopy(tr.sunDirection, ent->lightDir);
}
