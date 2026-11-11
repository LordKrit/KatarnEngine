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

struct PsOut { float4 c : SV_Target0; float4 glow : SV_Target1; float4 overlay : SV_Target2; };
float4 Shade(VsOut i, out float4 display);
PsOut PS(VsOut i)
{
	PsOut o;
	float4 display;
	o.c = Shade(i, display);
	// glow stages also go to the glow target (blurred into the bloom); everything else only covers it
	o.glow = (drawTarget == 1 && (flags & (1u << 11))) ? o.c : float4(0, 0, 0, o.c.a);
	o.overlay = float4(0, 0, 0, o.c.a);
	if (drawTarget == 1 && (flags & (1u << 14))) {
		// lightsaber blade: kept out of the HDR scene (no tone-map whitening, no fog, no TAA smear) and added to the
		// display after the tone map with the plain clamp rend2 has; a softer copy feeds the bloom
		o.overlay = display;
		o.glow = float4(o.c.rgb * 0.35, o.c.a);
		o.c = float4(0, 0, 0, o.c.a);
	}
	return o;
}
float4 Shade(VsOut i, out float4 display)
{
	float4 c = i.color;
	display = 0;
	const bool saber = drawTarget == 1 && (flags & (1u << 14));
	if (drawTarget == 1 && !saber) c.rgb = SrgbToLinear(c.rgb);
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
	if (saber) {
		// display-space colour (gamma, as authored), scaled by kt_saberGlowScale; the linear copy feeds the bloom
		display = float4(c.rgb * (drawScale > 0 ? drawScale : 1.0), c.a);
		c.rgb = SrgbToLinear(saturate(c.rgb));
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
