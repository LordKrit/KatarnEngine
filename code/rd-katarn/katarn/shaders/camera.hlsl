// KatarnEngine - camera effects for cinematics: depth of field (bokeh discs, Unreal's cinematic DOF idea done as a
// gather) and camera motion blur (from depth reprojection: the camera's own movement, not the characters')
#include "common.hlsli"

cbuffer Draw : register(b0)
{
	uint srcSrv;
	uint outUav;
	uint focusBuf;    // UAV buffer: smoothed focus distance at byte 384 (the exposure buffer; the histogram ends at 272)
	uint autoFocus;
	float focusDist;  // when not automatic (units)
	float aperture;   // blur strength (pixels of blur for something at twice / half the focus distance, at 1080p)
	float shutter;    // motion blur: share of the frame the shutter is open
	float dt;
};

// automatic focus: what is under the middle of the screen, eased over time
[numthreads(1, 1, 1)]
void FocusCS(uint3 id : SV_DispatchThreadID)
{
	float best = 1e9;
	[unroll] for (int y = -1; y <= 1; y++)
		[unroll] for (int x = -1; x <= 1; x++) {
			int2 p = int2(viewport.xy + viewport.zw * (0.5 + float2(x, y) * 0.03));
			float d = Tex2D[indices0.w].Load(int3(p, 0)).r;
			best = min(best, d > 0 ? ViewDepth(d) : 1e5);
		}
	float prev = asfloat(UAVBUF(focusBuf).Load(384));
	if (!(prev > 0 && prev < 1e6)) prev = best;
	float k = 1 - exp(-dt * 4.0);
	UAVBUF(focusBuf).Store(384, asuint(lerp(prev, best, k)));
}

float CoC(float z, float focus, float scale)
{
	return clamp(aperture * scale * abs(1 - focus / max(z, 1)), 0, 24);
}

[numthreads(8, 8, 1)]
void DofCS(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= (uint)viewport.z || id.y >= (uint)viewport.w) return;
	int2 px = int2(viewport.xy) + int2(id.xy);
	float focus = autoFocus ? asfloat(BUF(indices1.w).Load(384)) : focusDist;
	focus = max(focus, 8);
	float scale = viewport.w / 1080.0;
	float d = Tex2D[indices0.w].Load(int3(px, 0)).r;
	float z = d > 0 ? ViewDepth(d) : 1e5;
	float c0 = CoC(z, focus, scale);
	float4 center = Tex2D[srcSrv].Load(int3(px, 0));
	if (c0 < 0.5 && z <= focus) { UavTex[outUav][px] = center; return; }
	// golden-angle disc; a sample counts if its own blur reaches this pixel (near things spread over far ones)
	float3 acc = center.rgb;
	float wsum = 1;
	float maxR = max(c0, 8.0 * scale);   // wide enough to catch near things spreading over this pixel
	int2 lo = int2(viewport.xy), hi = int2(viewport.xy + viewport.zw) - 1;
	[loop] for (int i = 1; i < 32; i++) {
		float r = sqrt(i / 31.0) * maxR;
		float a = i * 2.39996;
		int2 q = clamp(px + int2(round(float2(cos(a), sin(a)) * r)), lo, hi);
		float dq = Tex2D[indices0.w].Load(int3(q, 0)).r;
		float zq = dq > 0 ? ViewDepth(dq) : 1e5;
		float cq = CoC(zq, focus, scale);
		float reach = zq < z ? cq : min(cq, c0);    // behind: no more blur than this pixel's own
		float w = saturate(reach - r + 1);
		float3 s = Tex2D[srcSrv].Load(int3(q, 0)).rgb;
		w *= 1 + 2 * saturate(max(s.r, max(s.g, s.b)) - 1);   // bright points make bokeh discs
		acc += s * w;
		wsum += w;
	}
	UavTex[outUav][px] = float4(acc / wsum, center.a);
}

[numthreads(8, 8, 1)]
void MotionBlurCS(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= (uint)viewport.z || id.y >= (uint)viewport.w) return;
	int2 px = int2(viewport.xy) + int2(id.xy);
	float d = Tex2D[indices0.w].Load(int3(px, 0)).r;
	float2 vuv = (float2(id.xy) + 0.5) / viewport.zw;
	float4 wp = mul(invViewProj, float4(vuv * float2(2, -2) + float2(-1, 1), d, 1));
	float4 pc = mul(prevViewProj, wp);
	float4 center = Tex2D[srcSrv].Load(int3(px, 0));
	if (pc.w <= 1e-6) { UavTex[outUav][px] = center; return; }
	float2 puv = pc.xy / pc.w * float2(0.5, -0.5) + 0.5;
	float2 vel = (vuv - puv) * viewport.zw * shutter;
	float len = length(vel);
	if (len < 1.0) { UavTex[outUav][px] = center; return; }
	vel *= min(len, 48.0) / len;
	float3 acc = center.rgb;
	float n = 1;
	int2 lo = int2(viewport.xy), hi = int2(viewport.xy + viewport.zw) - 1;
	[unroll] for (int i = 1; i <= 8; i++) {
		float t = i / 8.0 - 0.5;
		int2 q = clamp(px + int2(round(vel * t)), lo, hi);
		acc += Tex2D[srcSrv].Load(int3(q, 0)).rgb;
		n += 1;
	}
	UavTex[outUav][px] = float4(acc / n, center.a);
}
