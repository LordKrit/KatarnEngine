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
#include <vector>

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

void R_HDWorldLoad(const char* bspName)
{
	if (!r_hdWorld)
	{
		r_hdWorld = ri.Cvar_Get("r_hdWorld", "1", CVAR_ARCHIVE);
		r_hdAmbient = ri.Cvar_Get("r_hdAmbient", "0.45 0.42 0.38", CVAR_ARCHIVE);
		r_hdSun = ri.Cvar_Get("r_hdSun", "1.0 0.95 0.85", CVAR_ARCHIVE);
	}
	hdChunks.clear();   // VBOs/IBOs are freed with the rest of the renderer's buffers

	char base[MAX_QPATH];
	COM_StripExtension(bspName, base, sizeof(base));
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
}

qboolean R_HDWorldActive(void)
{
	return (qboolean)(!hdChunks.empty() && r_hdWorld && r_hdWorld->integer);
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
	vec3_t amb = { 0.45f, 0.42f, 0.38f }, sun = { 1, 0.95f, 0.85f };
	sscanf(r_hdAmbient->string, "%f %f %f", &amb[0], &amb[1], &amb[2]);
	sscanf(r_hdSun->string, "%f %f %f", &sun[0], &sun[1], &sun[2]);
	VectorScale(amb, 255.0f * 0.8f, ent->ambientLight);
	VectorScale(sun, 255.0f * 0.6f, ent->directedLight);
	VectorCopy(tr.sunDirection, ent->lightDir);
}
