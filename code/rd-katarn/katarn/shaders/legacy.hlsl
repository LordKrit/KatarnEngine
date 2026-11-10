// KatarnEngine - legacy fixed-function content (menus, HUD, BSP surfaces, effects) translated from GL 1.x
#include "common.hlsli"

cbuffer Draw : register(b0)
{
	float4x4 mvp;
	uint tex0;
	uint tex1;
	uint flags;
	float alphaRef;
	uint drawTarget;        // 0 screen (display-referred, as GL drew it), 1 HDR scene (linear radiance)
	float glowBoost;
	float drawScale;        // extra brightness in the HDR scene (saber glows)
	uint drawPad1;
	float4 drawExtra[2];
};

struct VsIn { float3 pos : POSITION; float2 uv0 : TEXCOORD0; float2 uv1 : TEXCOORD1; float4 color : COLOR; };
struct VsOut { float4 pos : SV_Position; float2 uv0 : TEXCOORD0; float2 uv1 : TEXCOORD1; float4 color : COLOR; };

VsOut VS(VsIn i)
{
	VsOut o;
	o.pos = mul(mvp, float4(i.pos, 1));
	o.uv0 = i.uv0;
	o.uv1 = i.uv1;
	o.color = i.color;
	return o;
}

float4 SampleUnit(uint tex, float2 uv, bool clampMode, bool pointMode)
{
	if (pointMode) return clampMode ? Tex2D[NonUniformResourceIndex(tex)].Sample(sClampPoint, uv) : Tex2D[NonUniformResourceIndex(tex)].Sample(sWrapPoint, uv);
	return clampMode ? Tex2D[NonUniformResourceIndex(tex)].Sample(sClampAniso, uv) : Tex2D[NonUniformResourceIndex(tex)].Sample(sWrap, uv);
}

struct PsOut { float4 c : SV_Target0; float4 glow : SV_Target1; };
float4 Shade(VsOut i);
PsOut PS(VsOut i)
{
	PsOut o;
	o.c = Shade(i);
	// glow stages also go to the glow target (blurred into the bloom); everything else only covers it
	o.glow = (drawTarget == 1 && (flags & (1u << 11))) ? o.c : float4(0, 0, 0, o.c.a);
	return o;
}
float4 Shade(VsOut i)
{
	float4 c = i.color;
	if (drawTarget == 1) c.rgb = SrgbToLinear(c.rgb);
	if (flags & 1) {
		float4 t = SampleUnit(tex0, i.uv0, (flags & 4) != 0, (flags & 16) != 0);
		uint env0 = (flags >> 12) & 3;
		if (env0 == 2) c = t; else c *= t;
	}
	if (flags & 2) {
		float4 t = SampleUnit(tex1, i.uv1, (flags & 8) != 0, (flags & 32) != 0);
		uint env = (flags >> 6) & 3;
		if (env == 0) c *= t;
		else if (env == 1) { c.rgb += t.rgb; c.a *= t.a; }
		else if (env == 2) c = t;
		else c.rgb = lerp(c.rgb, t.rgb, t.a);
	}
	uint at = (flags >> 8) & 7;
	if (at != 0) {
		bool keep = true;
		if (at == 1) keep = c.a > alphaRef;
		else if (at == 2) keep = c.a < alphaRef;
		else if (at == 3) keep = c.a >= alphaRef;
		else if (at == 4) keep = c.a <= alphaRef;
		else if (at == 5) keep = c.a == alphaRef;
		else if (at == 6) keep = c.a != alphaRef;
		else keep = false;
		if (!keep) discard;
	}
	if (drawTarget == 1) {
		// LDR content in an HDR scene: undo the camera exposure so it shows at its authored brightness
		float s = (skyParams.w > 0.5 ? Overbright() : 1.0) / max(Exposure(), 1e-6);   // HD: legacy colours are scaled by identityLight
		if ((flags & (1u << 11)) && skyParams.w > 0.5) s *= glowBoost;   // HD worlds only: old maps keep their authored glow
		s *= drawScale > 0 ? drawScale : 1.0;
		c.rgb = min(c.rgb * s, 1500.0);   // stacked additive sprites (saber glow) must not overflow the half-float scene
	}
	return c;
}
