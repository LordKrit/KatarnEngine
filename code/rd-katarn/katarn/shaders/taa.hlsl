// KatarnEngine - temporal anti-aliasing (Unreal-style): jittered frames, history reprojected from depth, the history
// clipped to the current neighbourhood's colour range in YCoCg (no ghosting from things without motion vectors), a
// luma-weighted blend in a tone-mapped space. The final sharpen happens in the tone map pass (taaParams.z).
#include "common.hlsli"

cbuffer Draw : register(b0)
{
	uint curSrv;      // this frame's lit scene (HDR)
	uint histSrv;     // last frame's resolved scene
	uint outUav;
	uint depthSrv;
	uint histValid;
	float feedbackMin;  // weight of this frame when still (0.06 = long history)
	float feedbackMax;  // ... when moving fast
};

float3 RGBToYCoCg(float3 c) { return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25))); }
float3 YCoCgToRGB(float3 c) { return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }
float3 Compress(float3 c) { return c / (1.0 + max(c.r, max(c.g, c.b))); }       // tone-map before blending (Karis)
float3 Expand(float3 c) { return c / max(1.0 - max(c.r, max(c.g, c.b)), 1e-4); }

[numthreads(8, 8, 1)]
void ResolveCS(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= (uint)viewport.z || id.y >= (uint)viewport.w) return;
	int2 px = int2(viewport.xy) + int2(id.xy);
	int2 lo = int2(viewport.xy), hi = int2(viewport.xy + viewport.zw) - 1;

	// neighbourhood: colour moments and the nearest depth (edges follow the foreground)
	float3 m1 = 0, m2 = 0, cur = 0;
	float dBest = 0;
	int2 pBest = px;
	[unroll] for (int y = -1; y <= 1; y++)
		[unroll] for (int x = -1; x <= 1; x++) {
			int2 p = clamp(px + int2(x, y), lo, hi);
			float3 s = RGBToYCoCg(Compress(max(Tex2D[curSrv].Load(int3(p, 0)).rgb, 0)));
			if (x == 0 && y == 0) cur = s;
			m1 += s; m2 += s * s;
			float d = Tex2D[depthSrv].Load(int3(p, 0)).r;
			if (d > dBest) { dBest = d; pBest = p; }      // reversed Z: larger = nearer
		}
	m1 /= 9; m2 /= 9;
	float3 sigma = sqrt(max(m2 - m1 * m1, 0));
	float3 outc = cur;

	if (histValid) {
		// reproject the nearest surface (sky: a direction, w = 0 at infinite far - kept homogeneous)
		float2 vuv = (float2(pBest - int2(viewport.xy)) + 0.5) / viewport.zw;
		float2 ndc = vuv * float2(2, -2) + float2(-1, 1);
		float4 wp = mul(invViewProj, float4(ndc, dBest, 1));
		float4 pc = mul(prevViewProj, wp);
		float2 puv = pc.w > 1e-6 ? (pc.xy / pc.w) * float2(0.5, -0.5) + 0.5 : float2(-1, -1);
		float2 cuv = (float2(px - int2(viewport.xy)) + 0.5) / viewport.zw;
		puv = cuv + (puv - vuv);
		if (all(puv >= 0) && all(puv <= 1)) {
			float2 tuv = (viewport.xy + puv * viewport.zw) * invTargetSize.xy;
			float3 h = RGBToYCoCg(Compress(max(Tex2D[histSrv].SampleLevel(sClamp, tuv, 0).rgb, 0)));
			// clip the history to the neighbourhood's colour box (variance clipping)
			float3 bmin = m1 - sigma * 1.25, bmax = m1 + sigma * 1.25;
			float3 center = 0.5 * (bmax + bmin), ext = max(0.5 * (bmax - bmin), 1e-5);
			float3 off = h - center;
			float3 ts = abs(off / ext);
			float tmax = max(ts.x, max(ts.y, ts.z));
			if (tmax > 1) h = center + off / tmax;
			float speed = length((puv - cuv) * viewport.zw);
			float a = lerp(feedbackMin, feedbackMax, saturate(speed / 12.0));
			// luma weighting keeps bright specks from flickering
			float wc = a / (1 + cur.x), wh = (1 - a) / (1 + h.x);
			outc = (cur * wc + h * wh) / (wc + wh);
		}
	}
	UavTex[outUav][px] = float4(Expand(YCoCgToRGB(outc)), 1);
}
