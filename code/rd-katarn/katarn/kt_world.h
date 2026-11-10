// KatarnEngine - HD worlds (.kworld)
#pragma once
#include "kt_device.h"

namespace kt {

struct GpuLight {           // matches lighting.hlsl Light (64 bytes)
	float pos[3]; float radius;
	float color[3]; float type;          // 0 point, 1 spot
	float dir[3]; float spotCos;
	float spotInner; float invSquare; float pad[2];
};

struct WorldEnv {
	float sunDir[3] = { 0.4f, 0.3f, 0.86f };   // towards the sun
	float sunColor[3] = { 1.0f, 0.95f, 0.88f };
	float sunIntensity = 6.0f;
	float sunAngle = 0.0093f;
	float skyIntensity = 1.0f;
	float haze = 0.4f;
	float ambient = 1.0f;
	float fogDensity = 0.00002f, fogFalloff = 0.0004f, fogHeight = 0.0f, fogStart = 600.0f;
	float fogColor[3] = { 1, 1, 1 };
	float fogMax = 0.85f;
	float exposureEV = 0.0f;
};

bool World_Load(const char* mapName);
void World_Unload();
bool World_Active();
const WorldEnv& World_Env();
void World_AppendLights(std::vector<GpuLight>& out, const float* camPos);
// GBuffer: targets and view constants already bound
void World_DrawGBuffer(ID3D12GraphicsCommandList* cl, const float* viewProj);
void World_DrawShadow(ID3D12GraphicsCommandList* cl, int cascade, const float* cascadeViewProj);
void World_Shutdown();

} // namespace kt
