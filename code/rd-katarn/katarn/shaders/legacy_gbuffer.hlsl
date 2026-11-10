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
	uint normalTex;     // rend2-style normal map (tangent space, from the stage's normalMap), 0 = none
	uint rmoTex;        // roughness / metal / occlusion map, 0 = none
	uint rmoOrder;      // 0 rmo, 1 orm
	float skinAmount;   // rend2 skin keyword (subsurface), 0 = none
	float4 gridAmbient;  // gflags 16: light grid (old BSP maps), 0-255 scale
	float4 gridDirected;
	float4 gridDir;      // world space, towards the light
};

struct VsIn { float3 pos : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0; float2 lmuv : TEXCOORD1; float4 color : COLOR; };
struct VsOut { float4 pos : SV_Position; float3 normal : NORMAL; float2 uv : TEXCOORD0; float2 lmuv : TEXCOORD1; float4 color : COLOR; float3 wpos : TEXCOORD2; };

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
	o.wpos = wp;
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
	if (normalTex) {
		// no vertex tangents in the old formats: a cotangent frame from the screen-space derivatives
		float3 dp1 = ddx(i.wpos), dp2 = ddy(i.wpos);
		float2 duv1 = ddx(i.uv), duv2 = ddy(i.uv);
		float3 dp2perp = cross(dp2, n), dp1perp = cross(n, dp1);
		float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
		float3 B = dp2perp * duv1.y + dp1perp * duv2.y;
		float invmax = rsqrt(max(max(dot(T, T), dot(B, B)), 1e-20));
		float3 tn = Tex2D[NonUniformResourceIndex(normalTex)].Sample(sWrap, i.uv).xyz * 2 - 1;
		tn.z = sqrt(saturate(1 - dot(tn.xy, tn.xy)));
		float3 nm = normalize(T * invmax * tn.x - B * invmax * tn.y + n * tn.z);   // uv v runs down: flip the bitangent
		if (all(isfinite(nm))) n = nm;
	}
	float rough = roughness, metal = metallic, matAO = 1;
	if (rmoTex) {
		float3 r = Tex2D[NonUniformResourceIndex(rmoTex)].Sample(sWrap, i.uv).rgb;
		if (rmoOrder == 1) r = r.gbr;     // orm -> rmo
		rough = r.x; metal = r.y; matAO = r.z;
	}
	if (specTex) {
		// rend2 specularMap: rgb = specular colour, a = gloss
		float4 sp = Tex2D[NonUniformResourceIndex(specTex)].Sample(sWrap, i.uv);
		rough = sp.a < 0.999 ? 1.0 - sp.a : saturate(1.0 - Luminance(sp.rgb) * 0.9);
	}
	// models without PBR maps are flagged (metal + 2) so the lighting keeps their reflections soft, like rend2
	// (no specular / gloss data: a dull dielectric, not a coated one)
	bool authored = rmoTex != 0 || specTex != 0;
	float3 baked = 0;
	bool hasBaked = false;
	if ((gflags & 4) && lightmapTex) { baked = Tex2D[NonUniformResourceIndex(lightmapTex)].Sample(sClamp, i.lmuv).rgb * bakedScale; hasBaked = true; }
	else if (gflags & 8) { baked = SrgbToLinear(i.color.rgb) * bakedScale; hasBaked = true; }
	float3 gridLit = 0;
	if (gflags & 16) {
		// old BSP map: lit from the light grid like rend2 - ambient + directed (per pixel, with the normal map),
		// GGX highlight from the directed light; the lighting pass adds dynamic lights, AO and nothing else
		float3 L = normalize(gridDir.xyz + float3(0, 0, 1e-4));
		float nl = dot(n, L);
		float ndl = skinAmount > 0 ? saturate((nl + 0.4) / 1.4) : saturate(nl);
		float3 amb = gridAmbient.rgb / 255.0, dirc = gridDirected.rgb / 255.0;
		float3 lightDisp = saturate(amb + dirc * ndl);
		float3 lit = a.rgb * (1 - saturate(metal)) * SrgbToLinear(lightDisp);
		float3 V = normalize(camPos.xyz - i.wpos);
		float3 H = normalize(L + V);
		float r2 = max(rough * rough, 0.002);
		float nh = saturate(dot(n, H)), nv = max(dot(n, V), 1e-3);
		float D = r2 * r2 / (PI * pow(nh * nh * (r2 * r2 - 1) + 1, 2));
		float k = r2 * 0.5;
		float G = (saturate(nl) / (saturate(nl) * (1 - k) + k)) * (nv / (nv * (1 - k) + k));
		float3 f0 = lerp(authored ? 0.04 : 0.012, a.rgb, saturate(metal));
		float3 F = f0 + (1 - f0) * pow(1 - saturate(dot(H, V)), 5);
		float3 spec = D * G * F / max(4 * saturate(nl) * nv, 1e-3) * saturate(nl) * SrgbToLinear(saturate(dirc));
		lit += spec + f0 * SrgbToLinear(saturate(amb)) * 0.5;   // a dim ambient reflection
		gridLit = lit;
		hasBaked = true;
	}
	GBufferOut o;
	o.albedo = float4(a.rgb, matAO);
	rough = clamp(rough, 0.045, 1.0);
	o.normal = float4(EncodeNormal(n), hasBaked ? -rough : rough, saturate(metal) + (skinAmount > 0 ? 4.0 : (authored ? 0.0 : 2.0)));
	o.emissive = float4((gflags & 16) ? gridLit : (hasBaked ? baked * a.rgb : 0), 0);
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
