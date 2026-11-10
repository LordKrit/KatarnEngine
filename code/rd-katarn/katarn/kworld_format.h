// KatarnEngine .kworld - cooked HD world (shared by the renderer and tools/katarn_cook)
//
// File: KwHeader, KwSection table, then the sections (256-byte aligned).
// The world is baked into world-space clusters (one spatial cell x one material class each). Every cluster has a
// LOD chain over the same vertices; the renderer picks a LOD per cluster from its screen-space error. Each vertex
// carries its material index, so a cluster is drawn with one call whatever materials it holds (bindless).
// Coordinates are Quake units, Z up.
#pragma once
#include <stdint.h>

#define KW_MAGIC "KWORLD02"
#define KW_VERSION 2

enum KwSectionType : uint32_t {
	KW_VERT = 0x54524556,   // 'VERT' KwVertex[]
	KW_INDX = 0x58444E49,   // 'INDX' uint32_t[] (relative to the cluster's baseVertex)
	KW_CLUS = 0x53554C43,   // 'CLUS' KwCluster[]
	KW_LODS = 0x53444F4C,   // 'LODS' KwLod[]
	KW_MATL = 0x4C54414D,   // 'MATL' KwMaterial[]
	KW_TEXT = 0x54584554,   // 'TEXT' KwTexture[]
	KW_TDAT = 0x54414454,   // 'TDAT' texture mip data (offsets in KwTexture are relative to this section)
	KW_LITE = 0x4554494C,   // 'LITE' KwLight[]
	KW_ENVT = 0x54564E45,   // 'ENVT' environment, text: "key value value ..." per line
};

#pragma pack(push, 4)
struct KwHeader {
	char magic[8];
	uint32_t version;
	uint32_t numSections;
	float boundsMin[3], boundsMax[3];
};
struct KwSection {
	uint32_t type;
	uint32_t pad;
	uint64_t offset;
	uint64_t size;
};
struct KwVertex {               // 32 bytes
	float pos[3];
	int16_t normal[2];          // octahedral, snorm
	int16_t tangent[2];         // octahedral, snorm
	float uv[2];
	uint16_t material;
	int16_t tangentSign;        // +1 / -1 (bitangent = cross(n, t) * sign)
};
enum KwClusterClass : uint32_t { KWC_OPAQUE = 0, KWC_TWOSIDED = 1, KWC_MASKED = 2 };
struct KwCluster {
	float boundsMin[3], boundsMax[3];
	uint32_t baseVertex, numVertices;
	uint32_t firstLod, numLods;
	uint32_t cls;               // KwClusterClass
	uint32_t flags;             // 1 = no shadow
};
struct KwLod {
	uint32_t firstIndex, indexCount;
	float error;                // world units
	uint32_t pad;
};
enum KwMaterialFlags : uint32_t { KWM_MASK = 1, KWM_TWOSIDED = 2, KWM_ORM_AO = 4, KWM_BLEND = 8 };
struct KwMaterial {             // 64 bytes, same layout the shader reads (texture fields are texture indices, -1 = none)
	float baseColor[4];
	float roughness, metallic, normalScale, alphaCutoff;
	int32_t albedoTex, normalTex, ormTex, emissiveTex;
	float emissive[3];
	uint32_t flags;
};
struct KwTexture {
	uint64_t offset, size;      // in TDAT
	uint32_t width, height, mips;
	uint32_t format;            // DXGI_FORMAT (BC7_UNORM 98, BC5_UNORM 83, R8G8B8A8_UNORM 28)
	uint32_t srgb;              // colour data: sample through the sRGB view
	uint32_t pad;
};
struct KwLight {
	float pos[3];
	float radius;               // cut-off distance
	float color[3];             // linear, already multiplied by intensity
	uint32_t type;              // 0 point, 1 spot
	float dir[3];
	float spotCos, spotInnerCos;
	float invSquare;            // >0: intensity / d^2 falloff (physical), 0: smooth window only
	uint32_t pad[2];
};
#pragma pack(pop)
