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
	float w = bladeAlpha == 2 ? coreR * 2.0 : glowR * 2.2;
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
	float hum = 1.0 + 0.03 * sin(tm * 47.0 + s * 9.0) + 0.02 * sin(tm * 113.0);

	// Battlefront II (2017) profile: a solid white core, the blade colour at full saturation right at its edge, a
	// tight glow a few core widths out and a faint wide halo
	float rc = max(coreR, 0.5) * 1.15;
	float w = glowR * 2.2;
	float window = saturate(1 - dist / w); window *= window;
	float core = 1 - smoothstep(rc * 0.72, rc * 1.12, dist);
	float tight = exp(-pow(dist / (rc * 2.6), 2));
	float wide = exp(-dist / (glowR * 0.55)) * window;
	float k = bladeIntensity * 2.0 * vis * hum;

	if (bladePad == 0) {
		// HDR scene: only a soft halo for the bloom (and the darksaber's black core)
		if (bladeAlpha == 1) return float4(0, 0, 0, saturate(core * vis));
		float3 halo = bladeColor * (tight * 1.2 + wide * 0.5) * k / max(Exposure(), 1e-6);
		return float4(Sane(halo), 0);
	}
	// display overlay (added after the tone map and clamped there)
	if (bladeAlpha == 1) {
		// darksaber: white crackling rim around the black core
		float rim = (tight * 1.3 + wide * 0.25) * (1 - core);
		return float4(rim * k, rim * k, rim * k, 0);
	}
	float3 Cg = pow(saturate(bladeColor), 1.0 / 2.2);          // the blade colour as displayed
	Cg = saturate(lerp(dot(Cg, float3(0.3, 0.6, 0.1)), Cg, 1.15)); // a touch more saturated
	float3 col = core + Cg * (tight * 1.7 + wide * 0.4) * (1 - core * 0.5);
	return float4(Sane(col * k), 0);
}
