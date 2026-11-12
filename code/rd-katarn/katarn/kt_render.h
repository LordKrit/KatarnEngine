// KatarnEngine - frame renderer interface (used by the vanilla back end and the GL translation layer)
#pragma once
#include "kt_device.h"

namespace kt {

// ------------------------------------------------------------------------------------------------ legacy draw stream
// Draws coming from the fixed-function translation (kt_gl.cpp). They are recorded on the CPU and replayed into the
// right target once Katarn knows where they go (2D screen, a 3D view's HDR scene, or after deferred lighting).
enum LegacyFlags : uint32_t {
	LF_TEX0 = 1u << 0, LF_TEX1 = 1u << 1,
	LF_CLAMP0 = 1u << 2, LF_CLAMP1 = 1u << 3, LF_POINT0 = 1u << 4, LF_POINT1 = 1u << 5,
	LF_ENV_SHIFT = 6,            // 2 bits: 0 modulate, 1 add, 2 replace, 3 decal (unit 1)
	LF_ATEST_SHIFT = 8,          // 3 bits: 0 off, 1 >, 2 <, 3 >=, 4 <=, 5 ==, 6 !=
	LF_GLOW = 1u << 11,          // shader stage marked glow: boosted in HDR so bloom picks it up
	LF_ENV0_SHIFT = 12,          // 2 bits: unit 0 env (replace ignores vertex colour)
	LF_SABER = 1u << 14,         // lightsaber blade / glow: composited after the tone map, the way rend2 shows it
};

struct PsoKey {
	uint32_t bits = 0;
	// blend: src 4 bits, dst 4 bits, enable 1 | depth: test 1, write 1, func 3 | cull 2 | offset 1 | colormask 4 | lines 1 | wire 1
};

struct LegacyDraw {
	uint32_t pso;                // see kt_gl.cpp PSO_* bits (target added at replay)
	uint32_t flags;
	float alphaRef;
	uint32_t tex0, tex1;         // GL texture names, resolved to bindless views at replay
	float mvp[16];
	D3D12_GPU_VIRTUAL_ADDRESS vb, ib;
	uint32_t vbSize, indexCount;
	int vp[4];                   // GL viewport (x, y from bottom)
	int sc[4];                   // GL scissor
	float depthMin, depthMax;    // already in the target's depth convention
	uint8_t clear;               // 0 draw, 1 colour clear, 2 depth clear, 3 both
	float clearColor[4];
	float clearDepth;
	const void* stage;           // shaderStage_t* that produced the draw (for deferred classification), may be null
	float scale;                 // extra brightness in the HDR scene (saber glows), 0 = 1
};

// ------------------------------------------------------------------------------------------------ view description
struct ViewDesc {
	float origin[3];
	float axis[3][3];            // forward, left, up (Quake)
	float fovX, fovY;            // degrees
	int x, y, w, h;              // GL viewport, y from bottom
	float zNear;
	float worldToEye[16];        // backEnd.viewParms.world.model_matrix (GL column-major)
	bool world;                  // has a world model (not a UI model view)
	bool skyPortal;
	float time;                  // seconds
	int rdflags;
};

// ------------------------------------------------------------------------------------------------ frame interface
bool R_Init(void* hwnd, int width, int height);
void R_Shutdown();
void R_BeginFrame();                                // RC_DRAW_BUFFER
void R_EndFrame();                                  // RC_SWAP_BUFFERS
void R_BeginView(const ViewDesc& v);                // RB_BeginDrawingView
void R_EndView();                                   // after RB_RenderDrawSurfList
bool R_InView();
bool R_ViewIsWorld();
void R_Begin2D();

void R_AppendLegacy(const LegacyDraw& d);
void R_AddSaberLight(const float* start, const float* dir, float length, const uint8_t* rgba);
float SaberGlowBoost();
bool SaberDraw();
void R_AddSaberBlade(const float* start, const float* dir, float length, float radius, const uint8_t* rgba, uint32_t glowTex, const uint8_t* vertexRgba, bool coreOnly);
void R_AddSaberCore(const float* start, const float* end, float radius, const uint8_t* rgba, uint32_t tex, bool alphaBlend);
void GL_SetStage(const void* stage, bool glow);
void GL_SetSaber(bool v);   // the next draws are lightsaber blades (LF_SABER)
void GL_SetScale(float v);   // extra HDR brightness for the next draws (saber glow sprites)   // the shader stage being drawn (glow boost)
// projection to use for 3D views (D3D, reversed-Z infinite) - the GL translation layer swaps it in for the
// front end's GL projection
const float* R_ViewProjection();
bool R_ReversedZ();

// legacy textures (GL names -> Katarn textures)
struct GLTexture;
GLTexture* R_GLTexture(uint32_t name, bool create);
void R_ResolveTexture(GLTexture* t);               // creates / updates GPU data before use
uint32_t R_TextureSrv(GLTexture* t, bool srgb);

// screenshots: reads the screen target back (blocking)
bool R_ReadScreen(int x, int y, int w, int h, uint8_t* rgbOut, bool rgba);

// legacy opaque surfaces that go into the GBuffer (Ghoul2 characters, models, BSP surfaces in HD worlds)
struct DeferredDraw {
	const float* xyz; int xyzStride;           // bytes
	const float* normal; int normalStride;
	const float* uv;                           // vec2 per vertex (base stage, after tcMod)
	const float* lmuv; int lmuvStride;         // may be null
	const uint8_t* colors;                     // rgba per vertex, may be null
	const uint32_t* indexes;
	int numVerts, numIndexes;
	float mvp[16];                             // P_katarn * modelview (as the GL translation would build it)
	float objToWorld[12];                      // 3 rows
	uint32_t albedoTex, lightmapTex, specTex;  // GL names, 0 = none
	uint32_t normalTex, rmoTex;                // GL names, 0 = none (rend2-style PBR maps)
	int rmoOrder;
	float skin;                                // subsurface strength (rend2 skin keyword), 0 = none
	float gridAmbient[3], gridDirected[3], gridDir[3];   // flag 16: light-grid lighting (0-255 scale, world-space direction)
	uint32_t flags;                            // see legacy_gbuffer.hlsl gflags
	float alphaRef, roughness, metallic, bakedScale;
	float tint[4];
	uint32_t deluxeTex;                        // GL name of the lightmap's deluxemap (light direction), 0 = none
	float parallax;                            // parallax depth from the normal map's alpha, 0 = none
	int cull;                                  // 0 none, 1 cull GL front faces, 2 cull GL back faces
	bool castShadow;
};
void R_AppendDeferred(const DeferredDraw& d);
const float* R_LegacyMVP(const float* modelView);   // P_katarn * modelView (static storage)

// shader sources (embedded, or <kt_shaderDir>/<file>.hlsl when set)
Shader R_Shader(const char* file, const char* entry, const char* target);

// HD world
bool World_Load(const char* mapName);              // loads maps/<name>.kworld if present
void World_Unload();
bool World_Active();

// reflection probes (rend2 cubemaps): placed from the map's entities, captured by the renderer itself in the first
// seconds of play (a few 128x128 views per frame), sampled by the deferred lighting with sphere parallax
constexpr int RDF_KT_PROBE = 0x40000000;           // rdflags of a capture view: | probe << 20 | face << 26
void Probes_SetEntities(const char* entityText);
void GlowLights_Set(const float* data, int count);    // tr_bsp: lights made from glowing map surfaces (pos, color, radius: 7 floats each)   // tr_bsp: the BSP entity string, after the world loaded
void Probes_BeginFrame();                           // RE_BeginFrame: queues this frame's capture views
void Probes_NoteScene(const void* refdef);          // RE_RenderScene: the game drew a world view (refdef_t*)

// sun shadows on old BSP maps (rend2 r_forceSun / r_sunlightMode 1): the static opaque world, drawn into the sun
// cascades from one buffer (casters behind the camera too); positions only, world space
void R_SetBspShadowMesh(const float* pos, uint32_t numVerts, const uint32_t* idx, uint32_t numIdx);
bool R_BspShadowMesh();

void R_GfxInfo();
extern void (*g_logCallback)(const char* msg);

} // namespace kt
