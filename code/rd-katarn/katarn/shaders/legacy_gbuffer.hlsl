// KatarnEngine - legacy opaque surfaces (Ghoul2 characters, BSP world, MD3 models) written into the GBuffer, so they
// get Katarn's sun, shadows, lights, sky ambient and AO like the HD world does.
#include "common.hlsli"

cbuffer DrawC : register(b0)
{
	uint cascadeIdx;
	uint gmode;         // 0 gbuffer, 1 shadow
};

// per draw (b2, from the upload ring)
cbuffer DrawG : register(b2)
{
	float4x4 gMvp;                      // the same matrix the GL translation uses, so EQUAL-depth extra stages match exactly
	float4 objRow0;
	float4 objRow1;
	float4 objRow2;                     // object -> world
	uint albedoTex;     // sRGB view
	uint lightmapTex;   // sRGB view, 0 = none
	uint gflags;        // 1 alpha test, 2 tint by vertex colour, 4 baked = lightmap, 8 baked = vertex colour
	float alphaRef;
	float roughness;
	float metallic;
	float bakedScale;
	uint specTex;
	float4 tint;        // constant tint (rgbGen const / entity)
};

struct VsIn { float3 pos : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0; float2 lmuv : TEXCOORD1; float4 color : COLOR; };
struct VsOut { float4 pos : SV_Position; float3 normal : NORMAL; float2 uv : TEXCOORD0; float2 lmuv : TEXCOORD1; float4 color : COLOR; };

VsOut VS(VsIn i)
{
	float4 p = float4(i.pos, 1);
	float3 wp = float3(dot(objRow0, p), dot(objRow1, p), dot(objRow2, p));
	VsOut o;
	o.pos = gmode == 1 ? mul(cascadeViewProj[cascadeIdx], float4(wp, 1)) : mul(gMvp, p);
	o.normal = float3(dot(objRow0.xyz, i.normal), dot(objRow1.xyz, i.normal), dot(objRow2.xyz, i.normal));
	o.uv = i.uv;
	o.lmuv = i.lmuv;
	o.color = i.color;
	return o;
}

struct GBufferOut { float4 albedo : SV_Target0; float4 normal : SV_Target1; float4 emissive : SV_Target2; };

GBufferOut PS(VsOut i, bool front : SV_IsFrontFace)
{
	float4 a = tint;
	if (albedoTex) a *= Tex2D[NonUniformResourceIndex(albedoTex)].Sample(sWrap, i.uv);
	if (gflags & 2) a *= float4(SrgbToLinear(i.color.rgb), i.color.a);
	if (gflags & 1) clip(a.a - alphaRef);
	float3 n = normalize(i.normal);
	// Quake's front faces are clockwise (it culls GL_FRONT with the CCW default), so a "front" face here is the back
	// of the surface: flip the normal towards the viewer only then
	if (front) n = -n;
	float rough = roughness, metal = metallic;
	if (specTex) {
		float3 sp = Tex2D[NonUniformResourceIndex(specTex)].Sample(sWrap, i.uv).rgb;
		rough = saturate(1.0 - Luminance(sp) * 0.9);
	}
	float3 baked = 0;
	bool hasBaked = false;
	if ((gflags & 4) && lightmapTex) { baked = Tex2D[NonUniformResourceIndex(lightmapTex)].Sample(sClamp, i.lmuv).rgb * bakedScale; hasBaked = true; }
	else if (gflags & 8) { baked = SrgbToLinear(i.color.rgb) * bakedScale; hasBaked = true; }
	GBufferOut o;
	o.albedo = float4(a.rgb, 1);
	rough = clamp(rough, 0.045, 1.0);
	o.normal = float4(EncodeNormal(n), hasBaked ? -rough : rough, metal);
	o.emissive = float4(hasBaked ? baked * a.rgb : 0, 0);
	return o;
}

void ShadowPS(VsOut i)
{
	if (gflags & 1) {
		float al = tint.a;
		if (albedoTex) al *= Tex2D[NonUniformResourceIndex(albedoTex)].Sample(sWrap, i.uv).a;
		clip(al - alphaRef);
	}
}
