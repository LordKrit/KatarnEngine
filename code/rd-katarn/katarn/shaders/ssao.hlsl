// KatarnEngine - screen-space ambient occlusion (horizon based) + depth-aware blur
#include "common.hlsli"

cbuffer Draw : register(b0)
{
	uint outUav;
	uint inSrv;
	float radius;       // world units
	float intensity;
	int2 blurDir;
	uint aoPad0;
	uint aoPad1;
};

float3 EyePos(uint2 px, float depth)
{
	float2 uv = (float2(px) - viewport.xy + 0.5) / viewport.zw;
	float2 ndc = uv * float2(2, -2) + float2(-1, 1);
	float z = projParams.z / max(depth, 1e-7);
	return float3(ndc.x * z / projParams.x, ndc.y * z / projParams.y, -z);
}

float Ign(float2 p) { return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715)))); }

[numthreads(8, 8, 1)]
void AoCS(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= (uint)viewport.z || id.y >= (uint)viewport.w) return;
	uint2 px = id.xy + (uint2)viewport.xy;
	float depth = Tex2D[indices0.w].Load(int3(px, 0)).r;
	if (depth <= 0) { UavTex[outUav][px] = 1; return; }
	float3 P = EyePos(px, depth);
	float3 Nw = DecodeNormal(Tex2D[indices0.y].Load(int3(px, 0)).xy);
	float3 N = normalize(mul((float3x3)worldToEye, Nw));
	float pixRadius = radius * projParams.y * viewport.w * 0.5 / max(-P.z, 1);
	pixRadius = min(pixRadius, 96);
	if (pixRadius < 1.5) { UavTex[outUav][px] = 1; return; }
	float rnd = Ign(float2(px) + (indices2.w % 8) * 7.0) * 2 * PI;
	float occ = 0;
	const int DIRS = 4, STEPS = 4;
	[unroll] for (int d = 0; d < DIRS; d++) {
		float a = rnd + d * (PI / DIRS);
		float2 dir = float2(cos(a), sin(a));
		[unroll] for (int s = 1; s <= STEPS; s++) {
			float t = (s - 0.5 + frac(rnd * 3.7 + s * 0.31)) / STEPS;
			int2 o = int2(dir * pixRadius * t);
			[unroll] for (int side = -1; side <= 1; side += 2) {
				int2 q = int2(px) + o * side;
				if (q.x < viewport.x || q.y < viewport.y || q.x >= viewport.x + viewport.z || q.y >= viewport.y + viewport.w) continue;
				float dq = Tex2D[indices0.w].Load(int3(q, 0)).r;
				float3 Q = EyePos((uint2)q, dq);
				float3 v = Q - P;
				float l2 = dot(v, v);
				float ndv = dot(N, v) * rsqrt(max(l2, 1e-4));
				float fall = saturate(1 - l2 / (radius * radius));
				occ += saturate(ndv - 0.1) * fall;
			}
		}
	}
	occ /= DIRS * STEPS * 2;
	float ao = saturate(1 - occ * intensity * 2);
	UavTex[outUav][px] = float4(ao, ao, ao, 1);
}

[numthreads(8, 8, 1)]
void BlurCS(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= (uint)viewport.z || id.y >= (uint)viewport.w) return;
	uint2 px = id.xy + (uint2)viewport.xy;
	float d0 = ViewDepth(Tex2D[indices0.w].Load(int3(px, 0)).r);
	float sum = 0, wsum = 0;
	[unroll] for (int i = -4; i <= 4; i++) {
		int2 q = int2(px) + blurDir * i;
		q = clamp(q, int2(viewport.xy), int2(viewport.xy + viewport.zw - 1));
		float dq = ViewDepth(Tex2D[indices0.w].Load(int3(q, 0)).r);
		float w = exp(-i * i / 8.0) * saturate(1 - abs(dq - d0) / (d0 * 0.05 + 1));
		sum += Tex2D[inSrv].Load(int3(q, 0)).r * w;
		wsum += w;
	}
	float ao = sum / max(wsum, 1e-4);
	UavTex[outUav][px] = float4(ao, ao, ao, 1);
}
