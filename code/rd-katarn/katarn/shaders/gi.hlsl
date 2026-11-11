// KatarnEngine - bounced light (screen-space global illumination, Unreal's SSGI idea): at half resolution, a few
// cosine-weighted rays per pixel march the depth buffer; a hit takes last frame's lit scene at that point as the light
// arriving from there. Accumulated over frames and filtered; the lighting uses it as the indirect diffuse light and as
// sky occlusion (light from the sky only where the rays got out).
#include "common.hlsli"

cbuffer Draw : register(b0)
{
	uint srcSrv;      // trace: last frame's lit scene; temporal: this frame's raw GI; spatial: accumulated GI
	uint outUav;
	uint histSrv;     // temporal: last frame's accumulated GI
	uint halfW;
	uint halfH;
	uint frame;
	uint histValid;
	float radius;     // world units a ray may travel
};

float Hash(uint2 p, uint s)
{
	uint h = p.x * 1973u + p.y * 9277u + s * 26699u;
	h = (h ^ 61u) ^ (h >> 16); h *= 9u; h ^= h >> 4; h *= 0x27d4eb2du; h ^= h >> 15;
	return (h & 0xffffff) / 16777216.0;
}

float3 WorldPos(int2 px, float d)
{
	float2 vuv = (float2(px - int2(viewport.xy)) + 0.5) / viewport.zw;
	float4 w = mul(invViewProj, float4(vuv * float2(2, -2) + float2(-1, 1), d, 1));
	return w.xyz / w.w;
}

// full-res pixel of a half-res texel (a different one of the four each frame)
int2 FullPixel(uint2 id)
{
	static const int2 o[4] = { int2(0, 0), int2(1, 1), int2(1, 0), int2(0, 1) };
	return int2(viewport.xy) + int2(id) * 2 + o[frame & 3];
}

[numthreads(8, 8, 1)]
void TraceCS(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= halfW || id.y >= halfH) return;
	int2 px = min(FullPixel(id.xy), int2(viewport.xy + viewport.zw) - 1);
	float d = Tex2D[indices0.w].Load(int3(px, 0)).r;
	if (d <= 0) { UavTex[outUav][id.xy] = float4(0, 0, 0, 0); return; }
	float3 P = WorldPos(px, d);
	float3 N = DecodeNormal(Tex2D[indices0.y].Load(int3(px, 0)).xy);
	float3 T = normalize(cross(abs(N.z) < 0.99 ? float3(0, 0, 1) : float3(1, 0, 0), N));
	float3 B = cross(N, T);
	float dist = length(P - camPos.xyz);
	float3 acc = 0;
	float hits = 0;
	const int RAYS = 4, STEPS = 10;
	[loop] for (int r = 0; r < RAYS; r++) {
		float u1 = Hash(id.xy, frame * 4 + r), u2 = Hash(id.xy + 17, frame * 4 + r + 1);
		float phi = 2 * PI * u1, ct = sqrt(1 - u2), st = sqrt(u2);
		float3 dir = T * (cos(phi) * st) + B * (sin(phi) * st) + N * ct;
		float t = 4 + dist * 0.002;
		float stepScale = pow(radius / t, 1.0 / STEPS);
		float jit = 0.5 + Hash(id.xy + 31, frame + r);
		[loop] for (int s = 0; s < STEPS; s++) {
			t *= lerp(1.0, stepScale, jit);
			jit = 1;
			float3 Q = P + N * 2 + dir * t;
			float4 qc = mul(viewProj, float4(Q, 1));
			if (qc.w <= 0) break;
			float2 quv = qc.xy / qc.w * float2(0.5, -0.5) + 0.5;
			if (any(quv < 0) || any(quv > 1)) break;
			int2 qp = int2(viewport.xy + quv * viewport.zw);
			float sd = Tex2D[indices0.w].Load(int3(qp, 0)).r;
			float qd = qc.z / qc.w;
			if (sd > qd) {
				// in front of the ray point: a hit if the surface is not far in front (thickness)
				float3 S = WorldPos(qp, sd);
				float thick = 40 + t * 0.25;
				if (length(S - camPos.xyz) > length(Q - camPos.xyz) - thick) {
					float4 pc = mul(prevViewProj, float4(S, 1));
					if (pc.w > 0) {
						float2 puv = pc.xy / pc.w * float2(0.5, -0.5) + 0.5;
						if (all(puv >= 0) && all(puv <= 1)) {
							float3 L = Tex2D[srcSrv].SampleLevel(sClamp, (viewport.xy + puv * viewport.zw) * invTargetSize.xy, 0).rgb;
							acc += min(Sane(L), 16.0);
						}
					}
					hits += 1;
					break;
				}
			}
		}
	}
	UavTex[outUav][id.xy] = float4(acc / RAYS, hits / RAYS);
}

// accumulate over frames: reproject last frame's GI with this pixel's depth
[numthreads(8, 8, 1)]
void TemporalCS(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= halfW || id.y >= halfH) return;
	float4 cur = Tex2D[srcSrv].Load(int3(id.xy, 0));
	int2 px = min(int2(viewport.xy) + int2(id.xy) * 2, int2(viewport.xy + viewport.zw) - 1);
	float d = Tex2D[indices0.w].Load(int3(px, 0)).r;
	float4 outc = cur;
	if (histValid && d > 0) {
		float3 P = WorldPos(px, d);
		float4 pc = mul(prevViewProj, float4(P, 1));
		float2 puv = pc.xy / pc.w * float2(0.5, -0.5) + 0.5;
		if (pc.w > 0 && all(puv >= 0) && all(puv <= 1)) {
			float4 h = Tex2D[histSrv].SampleLevel(sClamp, puv * viewport.zw * invTargetSize.xy, 0);   // half-res target, same layout
			outc = lerp(h, cur, 0.12);
		}
	}
	UavTex[outUav][id.xy] = outc;
}

// edge-aware blur (5x5, depth and normal weights), at half resolution
[numthreads(8, 8, 1)]
void SpatialCS(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= halfW || id.y >= halfH) return;
	int2 px = min(int2(viewport.xy) + int2(id.xy) * 2, int2(viewport.xy + viewport.zw) - 1);
	float d0 = Tex2D[indices0.w].Load(int3(px, 0)).r;
	if (d0 <= 0) { UavTex[outUav][id.xy] = 0; return; }
	float3 N0 = DecodeNormal(Tex2D[indices0.y].Load(int3(px, 0)).xy);
	float z0 = length(WorldPos(px, d0) - camPos.xyz);
	float4 acc = 0;
	float wsum = 0;
	[unroll] for (int y = -2; y <= 2; y++)
		[unroll] for (int x = -2; x <= 2; x++) {
			int2 q = clamp(int2(id.xy) + int2(x, y) * 2, int2(0, 0), int2(halfW - 1, halfH - 1));
			int2 qp = min(int2(viewport.xy) + q * 2, int2(viewport.xy + viewport.zw) - 1);
			float dq = Tex2D[indices0.w].Load(int3(qp, 0)).r;
			if (dq <= 0) continue;
			float3 Nq = DecodeNormal(Tex2D[indices0.y].Load(int3(qp, 0)).xy);
			float zq = length(WorldPos(qp, dq) - camPos.xyz);
			float w = exp(-abs(zq - z0) / (z0 * 0.03 + 4)) * pow(saturate(dot(N0, Nq)), 8) * exp(-(x * x + y * y) / 8.0);
			acc += Tex2D[srcSrv].Load(int3(q, 0)) * w;
			wsum += w;
		}
	UavTex[outUav][id.xy] = wsum > 0 ? acc / wsum : 0;
}
