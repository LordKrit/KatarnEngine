// KatarnEngine - lightsaber blades in the style of Star Wars Battlefront II (2017): a thin, white-hot core with a
// saturated coloured edge, a tight bright inner glow and a wide soft outer glow, rounded ends, a slight hum
// flicker. Drawn into the HDR scene (bloom does the rest), occluded by the scene depth.
// bladeAlpha 1 = darksaber: a black core with a white rim. Premultiplied output: rgb + dst * (1 - a).
#include "common.hlsli"

cbuffer Blade : register(b0)
{
	float3 bladeA; float glowR;      // hilt, glow radius (the game's saber radius)
	float3 bladeB; float coreR;      // tip, core radius
	float3 bladeColor; float bladeIntensity;
	uint bladeTex; uint bladeMode; uint bladeAlpha; uint bladePad;
	float3 vertexColor; float bladePad2;
};

struct VsOut { float4 pos : SV_Position; float3 wpos : TEXCOORD0; };

VsOut VS(uint vid : SV_VertexID)
{
	float3 ax = bladeB - bladeA;
	float len = max(length(ax), 1e-3);
	float3 d = ax / len;
	float3 mid = (bladeA + bladeB) * 0.5;
	float3 toCam = normalize(camPos.xyz - mid);
	float3 side = cross(d, toCam);
	side = dot(side, side) > 1e-6 ? normalize(side) : normalize(cross(d, float3(0, 0, 1)));
	float w = glowR * 2.2;
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
	// distance from the view ray to the blade segment (a capsule: rounded tip)
	float3 ro = camPos.xyz, rd = normalize(i.wpos - ro);
	float3 ab = bladeB - bladeA, ao = ro - bladeA;
	float a = dot(ab, ab), b = dot(ab, rd), c = dot(ab, ao), e = dot(rd, ao);
	float s = saturate((c - b * e) / max(a - b * b, 1e-6));
	float3 Pb = bladeA + ab * s;
	float t = max(dot(Pb - ro, rd), 0);
	float dist = length(ro + rd * t - Pb);

	float sd = Tex2D[indices0.w].Load(int3(i.pos.xy, 0)).r;
	float4 cp = mul(viewProj, float4(Pb, 1));
	float bd = cp.z / max(cp.w, 1e-4);
	float vis = (sd > 0 && ViewDepth(sd) < ViewDepth(bd) - glowR * 0.5) ? 0.0 : 1.0;

	// hum: a slight flicker along and over time
	float tm = projParams.w;
	float hum = 1.0 + 0.035 * sin(tm * 47.0 + s * 9.0) + 0.025 * sin(tm * 113.0);

	float rc = max(coreR, 0.35);                     // core radius
	float w = glowR * 2.2;
	float window = saturate(1 - dist / w); window *= window;
	float core = 1 - smoothstep(rc * 0.55, rc * 1.25, dist);           // white-hot centre
	float edge = exp(-pow(dist / (rc * 1.9), 2)) * (1 - core);        // saturated rim right outside the core
	float inner = exp(-pow(dist / (rc * 3.5), 2));
	float outer = exp(-pow(dist / (glowR * 0.95), 2)) * window;
	float scale = bladeIntensity / max(Exposure(), 1e-6) * vis * hum;

	if (bladeAlpha) {
		// darksaber: black core, white crackling rim
		float rim = (edge * 3.0 + inner * 0.6 + outer * 0.2);
		float3 col = float3(1, 1, 1) * rim * scale;
		return float4(Sane(col), saturate(core * vis));
	}
	float3 C = bladeColor;
	float3 col = 1.0 * core * 14.0
		+ C * edge * 9.0
		+ C * inner * 3.0
		+ C * outer * 0.9;
	return float4(Sane(col * scale), 0);
}
