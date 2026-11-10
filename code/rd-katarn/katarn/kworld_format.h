// KatarnEngine .kworld - cooked HD world (shared by the renderer and tools/katarn_cook)
//
// File: KwHeader, then sections anywhere in the file (KwSection table right after the header).
// Coordinates are Quake units, Z up. Matrices are 3 rows (object -> world).
#pragma once
#include <stdint.h>

#define KW_MAGIC "KWORLD01"
#define KW_VERSION 1

enum KwSectionType : uint32_t {
	KW_VERT = 0x54524556,   // 'VERT' KwVertex[]
	KW_INDX = 0x58444E49,   // 'INDX' uint32_t[]
	KW_MESH = 0x4853454D,   // 'MESH' KwMesh[]
	KW_SUBM = 0x4D425553,   // 'SUBM' KwSubmesh[]
	KW_INST = 0x54534E49,   // 'INST' KwInstance[]
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
struct KwVertex {               // 48 bytes
	float pos[3];
	float normal[3];
	float tangent[4];           // xyz, w = bitangent sign
	float uv[2];
};
struct KwMesh {
	uint32_t firstSubmesh, numSubmeshes;
	uint32_t baseVertex, numVertices;
	float boundsMin[3], boundsMax[3];   // object space
};
struct KwSubmesh {
	uint32_t firstIndex, indexCount;
	uint32_t material, pad;
};
struct KwInstance {
	float m[12];                // 3 rows object -> world
	uint32_t mesh;
	uint32_t flags;             // 1 = no shadow
	float boundsMin[3], boundsMax[3];   // world space
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
	uint32_t format;            // DXGI_FORMAT (BC7_UNORM 98, BC5_UNORM 83, BC4_UNORM 80, R8G8B8A8_UNORM 28)
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
