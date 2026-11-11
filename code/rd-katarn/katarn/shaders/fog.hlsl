// KatarnEngine - volumetric fog: integrate the froxel grid front to back (Frostbite's energy-conserving form), then
// apply it to the lit scene by depth
#include "common.hlsli"

cbuffer Draw : register(b0)
{
	uint srcSrv;     // integrate: injected froxels; apply: integrated froxels
	uint outUav;     // integrate: integrated froxels (3D); apply: the HDR scene
	uint dimXY;      // x | y << 16
	uint dimZ;
};

float FroxelDepth(float s) { return vfog1.w * pow(vfog0.w / vfog1.w, s / dimZ); }

[numthreads(8, 8, 1)]
void IntegrateCS(uint3 id : SV_DispatchThreadID)
{
	uint2 dim = uint2(dimXY & 0xffff, dimXY >> 16);
	if (id.x >= dim.x || id.y >= dim.y) return;
	float2 vuv = (float2(id.xy) + 0.5) / float2(dim);
	// distance along the ray per unit of view depth
	float2 ndc = vuv * float2(2, -2) + float2(-1, 1);
	float stretch = length(float3(ndc.x / projParams.x, ndc.y / projParams.y, 1));
	float3 acc = 0;
	float T = 1, z0 = 0;
	for (uint s = 0; s < dimZ; s++) {
		float z1 = FroxelDepth(s + 1);
		float dz = (z1 - z0) * stretch;
		float4 v = Tex3D[srcSrv].Load(int4(id.xy, s, 0));
		float ext = max(v.a, 1e-8);
		float tr = exp(-ext * dz);
		acc += T * (v.rgb - v.rgb * tr) / ext;
		T *= tr;
		UavTex3D[outUav][uint3(id.xy, s)] = float4(acc, T);
		z0 = z1;
	}
}

[numthreads(8, 8, 1)]
void ApplyCS(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= (uint)viewport.z || id.y >= (uint)viewport.w) return;
	int2 px = int2(viewport.xy) + int2(id.xy);
	float d = Tex2D[indices0.w].Load(int3(px, 0)).r;
	float viewZ = d > 0 ? ViewDepth(d) : min(1500.0, vfog0.w);   // the sky: only the air near by (a sky box is far away art)
	float s = log(max(viewZ, vfog1.w) / vfog1.w) / log(vfog0.w / vfog1.w) * dimZ - 0.5;
	float3 uvw = float3((float2(id.xy) + 0.5) / viewport.zw, saturate(s / dimZ));   // slice i holds the fog up to the far side of slice i
	float4 f = s < 0 ? float4(0, 0, 0, 1) : Tex3D[srcSrv].SampleLevel(sClamp, uvw, 0);
	float4 c = UavTex[outUav][px];
	UavTex[outUav][px] = float4(c.rgb * f.a + f.rgb, c.a);
}
