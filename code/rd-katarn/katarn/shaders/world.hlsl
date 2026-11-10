// KatarnEngine - HD world (.kworld) meshes: GBuffer and shadow depth
#include "common.hlsli"

cbuffer Draw : register(b0)
{
	uint materialBuf;     // ByteAddressBuffer: Material records
	uint cascade;         // shadow pass: cascade index
	uint drawMode;        // 0 gbuffer, 1 shadow
	uint drawPad0;
};

struct Material {
	float4 baseColor;
	float roughness;
	float metallic;
	float normalScale;
	float alphaCutoff;
	uint albedoTex;
	uint normalTex;
	uint ormTex;
	uint emissiveTex;
	float3 emissive; uint mflags;   // 1 alpha mask, 2 two-sided, 4 orm has AO, 8 blend
};
Material LoadMaterial(uint i)
{
	uint o = i * 64;
	Material m;
	m.baseColor = asfloat(BUF(materialBuf).Load4(o));
	float4 f = asfloat(BUF(materialBuf).Load4(o + 16));
	m.roughness = f.x; m.metallic = f.y; m.normalScale = f.z; m.alphaCutoff = f.w;
	uint4 t = BUF(materialBuf).Load4(o + 32);
	m.albedoTex = t.x; m.normalTex = t.y; m.ormTex = t.z; m.emissiveTex = t.w;
	uint4 e = BUF(materialBuf).Load4(o + 48);
	m.emissive = asfloat(e.xyz); m.mflags = e.w;
	return m;
}

struct VsIn { float3 pos : POSITION; float2 n : NORMAL; float2 t : TANGENT; float2 uv : TEXCOORD0; uint2 mat : BLENDINDICES; };
struct VsOut {
	float4 pos : SV_Position;
	float3 normal : NORMAL;
	float4 tangent : TANGENT;
	float2 uv : TEXCOORD0;
	nointerpolation uint mat : MATERIAL;
};

VsOut VS(VsIn i)
{
	VsOut v;
	v.pos = mul(drawMode == 1 ? cascadeViewProj[cascade] : viewProj, float4(i.pos, 1));
	v.normal = DecodeNormal(i.n);
	v.tangent = float4(DecodeNormal(i.t), i.mat.y > 32767 ? -1.0 : 1.0);
	v.uv = i.uv;
	v.mat = i.mat.x;
	return v;
}

struct GBufferOut {
	float4 albedo : SV_Target0;     // sRGB target: albedo, a = material AO
	float4 normal : SV_Target1;     // oct normal xy, roughness (negative = baked lighting), metallic
	float4 emissive : SV_Target2;   // emitted / baked radiance
};

GBufferOut PS(VsOut i, bool front : SV_IsFrontFace)
{
	Material m = LoadMaterial(i.mat);
	float4 albedo = m.baseColor;
	if (m.albedoTex) albedo *= Tex2D[NonUniformResourceIndex(m.albedoTex)].Sample(sWrap, i.uv);
	if (m.mflags & 1) clip(albedo.a - m.alphaCutoff);
	float3 n = normalize(i.normal);
	if (!front) n = -n;
	if (m.normalTex) {
		float2 nt = Tex2D[NonUniformResourceIndex(m.normalTex)].Sample(sWrap, i.uv).rg * 2 - 1;
		nt *= m.normalScale;
		float3 tn = float3(nt, sqrt(saturate(1 - dot(nt, nt))));
		float3 t = i.tangent.xyz;
		t = normalize(t - n * dot(n, t));
		float3 b = cross(n, t) * (i.tangent.w < 0 ? -1.0 : 1.0);
		if (!front) b = -b;
		n = normalize(t * tn.x + b * tn.y + n * tn.z);
	}
	float ao = 1, rough = m.roughness, metal = m.metallic;
	if (m.ormTex) {
		float3 orm = Tex2D[NonUniformResourceIndex(m.ormTex)].Sample(sWrap, i.uv).rgb;
		if (m.mflags & 4) ao = orm.r;
		rough *= orm.g; metal *= orm.b;
	}
	float3 em = m.emissive;
	if (m.emissiveTex) em *= Tex2D[NonUniformResourceIndex(m.emissiveTex)].Sample(sWrap, i.uv).rgb;
	GBufferOut o;
	o.albedo = float4(albedo.rgb, ao);
	o.normal = float4(EncodeNormal(n), clamp(rough, 0.045, 1.0), saturate(metal));
	o.emissive = float4(em, 0);
	return o;
}

// shadow depth (alpha-tested materials only; opaque uses a null pixel shader)
void ShadowPS(VsOut i)
{
	Material m = LoadMaterial(i.mat);
	float a = m.baseColor.a;
	if (m.albedoTex) a *= Tex2D[NonUniformResourceIndex(m.albedoTex)].Sample(sWrap, i.uv).a;
	clip(a - m.alphaCutoff);
}
