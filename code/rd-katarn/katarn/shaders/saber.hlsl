// KatarnEngine - lightsaber blades, drawn into the HDR scene in place of the old glow sprites and core lines.
// mode 0: the glow around the blade (the saber's own glow texture as a radial profile, or a soft gaussian);
// mode 1: the core strip with the saber's own core texture (additive, or alpha-blended for dark blades).
// Premultiplied output: rgb + dst * (1 - a).
#include "common.hlsli"

cbuffer Blade : register(b0)
{
	float3 bladeA; float glowR;      // glow: hilt; core: muzzle end
	float3 bladeB; float coreR;      // tip
	float3 bladeColor; float bladeIntensity;
	uint bladeTex; uint bladeMode; uint bladeAlpha; uint bladePad;
	float3 vertexColor; float bladePad2;
};

struct VsOut { float4 pos : SV_Position; float3 wpos : TEXCOORD0; float2 uv : TEXCOORD1; };

VsOut VS(uint vid : SV_VertexID)
{
	float3 ax = bladeB - bladeA;
	float len = max(length(ax), 1e-3);
	float3 d = ax / len;
	float3 mid = (bladeA + bladeB) * 0.5;
	float3 toCam = normalize(camPos.xyz - mid);
	float3 side = cross(d, toCam);
	side = dot(side, side) > 1e-6 ? normalize(side) : normalize(cross(d, float3(0, 0, 1)));
	float w = bladeMode == 0 ? glowR * 2.5 : coreR;
	float ext = bladeMode == 0 ? w : 0;
	static const float2 c[6] = { float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1) };
	float2 q = c[vid];
	float3 p = (q.y < 0 ? bladeA - d * ext : bladeB + d * ext) + side * q.x * w + (bladeMode == 0 ? toCam * w : 0);
	VsOut o;
	o.pos = mul(viewProj, float4(p, 1));
	o.wpos = p;
	o.uv = float2(q.x * 0.5 + 0.5, q.y < 0 ? 1.0 : 0.0);   // core textures: u across, v along (tip at 0)
	return o;
}

float4 PS(VsOut i) : SV_Target0
{
	float3 ro = camPos.xyz, rd = normalize(i.wpos - ro);
	float3 ab = bladeB - bladeA, ao = ro - bladeA;
	float a = dot(ab, ab), b = dot(ab, rd), c = dot(ab, ao), e = dot(rd, ao);
	float s = saturate((c * 1 - b * e) / max(a - b * b, 1e-6));
	float3 Pb = bladeA + ab * s;
	float t = max(dot(Pb - ro, rd), 0);
	float dist = length(ro + rd * t - Pb);
	// hidden behind the scene
	float sd = Tex2D[indices0.w].Load(int3(i.pos.xy, 0)).r;
	float4 cp = mul(viewProj, float4(Pb, 1));
	float bd = cp.z / max(cp.w, 1e-4);
	float vis = (sd > 0 && ViewDepth(sd) < ViewDepth(bd) - glowR * 0.5) ? 0.0 : 1.0;
	float scale = bladeIntensity / max(Exposure(), 1e-6) * vis;

	if (bladeMode == 1) {
		float4 tc = bladeTex ? Tex2D[NonUniformResourceIndex(bladeTex)].Sample(sWrap, i.uv) : float4(1, 1, 1, 1);
		float3 col = tc.rgb * vertexColor;
		if (bladeAlpha) {
			float al = saturate(tc.a * vis);
			return float4(Sane(col * al), al);   // dark blade: covers what is behind it
		}
		return float4(Sane(col * scale * 2.0), 0);
	}
	float w = glowR * 2.5;
	float window = saturate(1 - dist / w);
	window *= window;                                   // fades out before the edge of its quad
	float3 glow;
	if (bladeTex) {
		// the saber's glow texture is a radial blob: read its profile from the centre outwards
		float r = saturate(dist / w) * 0.5;
		glow = Tex2D[NonUniformResourceIndex(bladeTex)].SampleLevel(sClamp, float2(0.5 + r, 0.5), 0).rgb * vertexColor * 2.0;
	}
	else glow = bladeColor * (exp(-(dist * dist) / max(glowR * glowR * 0.35, 1e-3)) * 3.0);
	float3 col = glow * window;
	// keep a little white-hot centre even when the core texture is missing or faint
	float core = saturate(1 - dist / max(coreR, 0.1));
	col += lerp(bladeColor, 1.0, 0.85) * core * core * 4.0;
	return float4(Sane(col * scale), 0);
}
