// KatarnEngine - lightsaber blades, drawn analytically into the HDR scene (replaces the old glow sprites):
// a white-hot core and a coloured glow around the hilt-to-tip segment, occluded by the scene depth.
#include "common.hlsli"

cbuffer Blade : register(b0)
{
	float3 bladeA; float glowR;      // hilt (world), glow radius
	float3 bladeB; float coreR;      // tip, core radius
	float3 bladeColor; float bladeIntensity;
};

struct VsOut { float4 pos : SV_Position; float3 wpos : TEXCOORD0; };

VsOut VS(uint vid : SV_VertexID)
{
	// a camera-facing quad around the segment, extended by the glow radius
	float3 ax = bladeB - bladeA;
	float len = max(length(ax), 1e-3);
	float3 d = ax / len;
	float3 mid = (bladeA + bladeB) * 0.5;
	float3 toCam = normalize(camPos.xyz - mid);
	float3 side = cross(d, toCam);
	side = dot(side, side) > 1e-6 ? normalize(side) : normalize(cross(d, float3(0, 0, 1)));
	float w = glowR * 2.5;
	static const float2 c[6] = { float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1) };
	float2 q = c[vid];
	float3 p = (q.y < 0 ? bladeA - d * w : bladeB + d * w) + side * q.x * w + toCam * w;
	VsOut o;
	o.pos = mul(viewProj, float4(p, 1));
	o.wpos = p;
	return o;
}

float4 PS(VsOut i) : SV_Target0
{
	// closest points between the view ray through this pixel and the blade segment
	float3 ro = camPos.xyz, rd = normalize(i.wpos - ro);
	float3 ab = bladeB - bladeA, ao = ro - bladeA;
	float a = dot(ab, ab), b = dot(ab, rd), c = dot(ab, ao), e = dot(rd, ao);
	float den = max(a - b * b, 1e-6);
	float s = saturate((c - b * e) / den);            // along the blade
	float3 Pb = bladeA + ab * s;
	float t = max(dot(Pb - ro, rd), 0);               // along the ray
	float dist = length(ro + rd * t - Pb);
	// hidden behind the scene: compare with the depth at this pixel
	float sd = Tex2D[indices0.w].Load(int3(i.pos.xy, 0)).r;
	float4 cp = mul(viewProj, float4(Pb, 1));
	float bd = cp.z / max(cp.w, 1e-4);
	float vis = (sd > 0 && ViewDepth(sd) < ViewDepth(bd) - glowR * 0.5) ? 0.0 : 1.0;
	float core = saturate(1 - dist / max(coreR, 0.1));
	core = core * core * (3 - 2 * core);
	float glow = exp(-(dist * dist) / max(glowR * glowR * 0.35, 1e-3));
	float halo = exp(-dist / max(glowR * 1.2, 1e-3)) * 0.25;
	float3 col = bladeColor * (glow + halo) * 3.0 + lerp(bladeColor, 1.0, 0.85) * core * 10.0;
	col *= bladeIntensity / max(Exposure(), 1e-6) * vis;
	return float4(Sane(col), 0);
}
