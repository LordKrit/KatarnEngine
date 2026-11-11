// KatarnEngine - skin subsurface scattering (screen space, Jimenez): the lit skin is blurred along the surface in two
// passes, red further than green and blue, so light bleeds softly under the skin the way it does in Unreal's
// subsurface profile. Only pixels with the skin flag, only from skin neighbours at about the same depth.
#include "common.hlsli"

cbuffer Draw : register(b0)
{
	uint srcSrv;
	uint outUav;
	uint dirY;        // 0 horizontal, 1 vertical
	float width;      // scatter radius in world units
};

bool IsSkin(int2 p)
{
	float w = Tex2D[indices0.y].Load(int3(p, 0)).w;
	return w > 3.5 && w < 7.5;
}

[numthreads(8, 8, 1)]
void BlurCS(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= (uint)viewport.z || id.y >= (uint)viewport.w) return;
	int2 px = int2(viewport.xy) + int2(id.xy);
	if (!IsSkin(px)) return;
	float d = Tex2D[indices0.w].Load(int3(px, 0)).r;
	if (d <= 0) return;
	float z = ViewDepth(d);
	float4 c = Tex2D[srcSrv].Load(int3(px, 0));
	// pixels per world unit at this depth
	float ppu = projParams.y * viewport.w * 0.5 / z;
	float stepPx = clamp(width * ppu / 3.0, 0.0, 6.0);
	if (stepPx < 0.3) { UavTex[outUav][px] = c; return; }
	int2 dir = dirY ? int2(0, 1) : int2(1, 0);
	// per-channel Gaussian (red scatters furthest)
	const float3 sigma = float3(1.0, 0.42, 0.28) * 1.6;
	float3 acc = c.rgb, wsum = 1;
	int2 lo = int2(viewport.xy), hi = int2(viewport.xy + viewport.zw) - 1;
	[unroll] for (int k = -6; k <= 6; k++) {
		if (k == 0) continue;
		float o = k * 0.5;
		int2 q = clamp(px + dir * (int)round(o * stepPx), lo, hi);
		if (!IsSkin(q)) continue;
		float dq = Tex2D[indices0.w].Load(int3(q, 0)).r;
		if (dq <= 0 || abs(ViewDepth(dq) - z) > width * 2.5) continue;
		float3 w = exp(-0.5 * (o * o) / (sigma * sigma));
		acc += Tex2D[srcSrv].Load(int3(q, 0)).rgb * w;
		wsum += w;
	}
	UavTex[outUav][px] = float4(acc / wsum, c.a);
}
