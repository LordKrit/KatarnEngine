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
	float3 wpos : TEXCOORD1;
	nointerpolation uint mat : MATERIAL;
};

// wind on leaves, flowers, vines and curtains (KWM_FOLIAGE): a sway that varies across the world, with gusts
float3 Wind(float3 p, float t)
{
	float ph = dot(p, float3(0.013, 0.017, 0.007));
	float gust = 0.55 + 0.45 * sin(t * 0.37 + p.x * 0.0011 + p.y * 0.0007);
	float3 d;
	d.x = sin(t * 1.7 + ph * 6.2831) + 0.5 * sin(t * 3.9 + ph * 13.0);
	d.y = 0.7 * cos(t * 1.3 + ph * 5.1) + 0.4 * sin(t * 4.7 + ph * 11.0);
	d.z = 0.35 * sin(t * 2.9 + ph * 9.0);
	return d * gust;
}

VsOut VS(VsIn i)
{
	VsOut v;
	if (extraParams.w > 0 && (BUF(materialBuf).Load(i.mat.x * 64 + 60) & 128))
		i.pos += Wind(i.pos, projParams.w) * extraParams.w;
	v.pos = mul(drawMode == 1 ? cascadeViewProj[cascade] : viewProj, float4(i.pos, 1));
	v.normal = DecodeNormal(i.n);
	v.tangent = float4(DecodeNormal(i.t), i.mat.y > 32767 ? -1.0 : 1.0);
	v.uv = i.uv;
	v.wpos = i.pos;
	v.mat = i.mat.x;
	return v;
}

struct GBufferOut {
	float4 albedo : SV_Target0;     // sRGB target: albedo, a = material AO
	float4 normal : SV_Target1;     // oct normal xy, roughness (negative = baked lighting), metallic
	float4 emissive : SV_Target2;   // emitted / baked radiance
};

// ---- splat (terrain) materials: tiled layers blended by world-position masks (KwSplatHeader / KwSplatLayer)
void Splat(uint o, float3 wpos, float3 ng, out float3 albedo, out float rough, out float3 n)
{
	uint4 h0 = BUF(materialBuf).Load4(o);
	float4 maskXf = asfloat(BUF(materialBuf).Load4(o + 16));
	float4 tintXf = asfloat(BUF(materialBuf).Load4(o + 32));
	float4 h3 = asfloat(BUF(materialBuf).Load4(o + 48));
	uint numLayers = min(h0.x, 24u);
	uint tintTex = h0.y;
	float tintMul = asfloat(h0.z), normalStrength = asfloat(h0.w);
	float invUnits = h3.x, roughConst = h3.y;
	float2 p = wpos.xy * invUnits;                       // Blender metres
	float2 muv = p * maskXf.xy + maskXf.zw; muv.y = 1 - muv.y;
	albedo = 0; rough = roughConst < 0 ? 0.8 : roughConst;
	float2 dn = 0;                                       // detail normal (tangent space, world X / Y)
	float2 dpx = ddx(p), dpy = ddy(p);                   // gradients outside the (divergent) layer loop
	[loop] for (uint li = 0; li < numLayers; li++) {
		uint lo = o + 64 + li * 32;
		uint4 a = BUF(materialBuf).Load4(lo);
		uint4 b = BUF(materialBuf).Load4(lo + 16);
		uint albedoTex = a.x, nohTex = a.y; float scale = asfloat(a.z); uint maskTex = a.w;
		uint chan = b.x; float smin = asfloat(b.y), smax = asfloat(b.z); uint flags = b.w;
		float w = 1;
		if (li > 0) {
			if (chan == 4) w = 1 - saturate((ng.z - smin) / max(smax - smin, 1e-4));
			else if (maskTex) {
				float4 m = Tex2D[NonUniformResourceIndex(maskTex)].SampleLevel(sClamp, muv, 0);
				w = chan == 0 ? m.r : chan == 1 ? m.g : chan == 2 ? m.b : m.a;
			}
			else w = 0;
		}
		if (w <= 0.002) continue;
		float2 luv = p * scale; luv.y = 1 - luv.y;
		if (!(flags & 1) && albedoTex) {
			float4 c = Tex2D[NonUniformResourceIndex(albedoTex)].SampleGrad(sWrap, luv, dpx * scale * float2(1, -1), dpy * scale * float2(1, -1));
			albedo = lerp(albedo, c.rgb, w);
			if (roughConst < 0) rough = lerp(rough, 1 - c.a, w);
		}
		if (nohTex) {
			float2 t = Tex2D[NonUniformResourceIndex(nohTex)].SampleGrad(sWrap, luv, dpx * scale * float2(1, -1), dpy * scale * float2(1, -1)).rg;
			dn = lerp(dn, float2(t.x * 2 - 1, 1 - t.y * 2), w);
		}
	}
	if (tintTex) {
		float2 tuv = p * tintXf.xy + tintXf.zw; tuv.y = 1 - tuv.y;
		albedo *= Tex2D[NonUniformResourceIndex(tintTex)].SampleLevel(sClamp, tuv, 0).rgb * tintMul;
	}
	dn *= normalStrength;
	float3 T = normalize(float3(1, 0, 0) - ng * ng.x), B = cross(ng, T);
	n = normalize(ng + T * dn.x + B * dn.y);
}

GBufferOut PS(VsOut i, bool front : SV_IsFrontFace)
{
	Material m = LoadMaterial(i.mat);
	if (m.mflags & 16) {
		float3 ng = normalize(i.normal);
		if (!front) ng = -ng;
		float3 sa, sn; float sr;
		Splat(m.albedoTex, i.wpos, ng, sa, sr, sn);
		GBufferOut so;
		so.albedo = float4(saturate(sa), 1);
		so.normal = float4(EncodeNormal(sn), clamp(sr, 0.045, 1.0), 0);
		so.emissive = 0;
		return so;
	}
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
	// foliage (Blender Translucent BSDF): +8 in the metal channel, the lighting pass lets sun through the leaves
	o.normal = float4(EncodeNormal(n), clamp(rough, 0.045, 1.0), saturate(metal) + ((m.mflags & 128) ? 8.0 : 0.0));
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

// ---- FX pass: water and flowing water, drawn into the lit HDR scene (blend: rgb + dst * a)
float3 FxSky(float3 d, float lod)
{
	if (indices2.x) {
		float2 uv = float2(0.5 - atan2(d.y, d.x) / (2 * PI), 0.5 - asin(clamp(d.z, -1, 1)) / PI);
		uint w, h, mips;
		Tex2D[indices2.x].GetDimensions(0, w, h, mips);
		return Tex2D[indices2.x].SampleLevel(sWrap, uv, min(lod, mips - 1.0)).rgb * skyParams.y;
	}
	return EvalSH(d) * PI;
}
// height fog as the lighting pass applies it (inscatter, opacity)
float4 FxFog(float3 P, float3 V, float dist)
{
	if (fogParams.x <= 0) return 0;
	float rayLen = max(dist - fogParams.w, 0);
	float dz = P.z - camPos.z;
	float f = fogParams.y;
	float camDens = fogParams.x * exp(-f * (camPos.z - fogParams.z));
	float fz = f * dz;
	float lineInt = abs(fz) > 1e-4 ? (1 - exp(-fz)) / fz : 1 - 0.5 * fz;
	float opacity = min(1 - exp(-camDens * lineInt * rayLen), fogColor.w);
	float3 hd = normalize(float3(-V.xy, 0.05));
	float3 inscatter = FxSky(hd, 8) * fogColor.rgb;
	inscatter += sunColor.rgb * sunColor.w * pow(saturate(dot(-V, sunDir.xyz)), 8) * 0.25 * fogColor.rgb;
	return float4(inscatter, opacity);
}
float WaveHeight(float2 p, float t, out float2 grad)
{
	// a few directional waves + two scrolling octaves of ripples (metres, seconds)
	static const float4 W[6] = {
		float4(0.80, 0.60, 0.55, 1.10), float4(-0.45, 0.89, 0.90, 1.50), float4(0.20, -0.98, 1.70, 2.10),
		float4(-0.92, -0.38, 2.60, 2.90), float4(0.60, -0.80, 4.30, 3.70), float4(0.98, 0.20, 6.10, 4.30) };
	float h = 0; grad = 0;
	[unroll] for (int k = 0; k < 6; k++) {
		float2 dir = W[k].xy; float f = W[k].z, sp = W[k].w;
		float a = 0.035 / f;
		float ph = dot(dir, p) * f + t * sp;
		h += a * sin(ph);
		grad += a * f * cos(ph) * dir;
	}
	return h;
}
float4 FxPS(VsOut i, bool front : SV_IsFrontFace) : SV_Target0
{
	float sceneD = Tex2D[indices0.w].Load(int3(i.pos.xy, 0)).r;
	if (i.pos.z < sceneD) discard;                       // reversed Z: behind the scene
	Material m = LoadMaterial(i.mat);
	float3 V = normalize(camPos.xyz - i.wpos);
	float t = projParams.w;
	float3 L = sunDir.xyz, sunC = sunColor.rgb * sunColor.w;
	float3 ng = normalize(i.normal);
	if (dot(ng, V) < 0) ng = -ng;
	if (m.mflags & 32) {
		// ---- still water
		float2 p = i.wpos.xy / 40.0;                     // metres
		float2 g; WaveHeight(p, t, g);
		float3 n = normalize(ng + float3(-g * m.normalScale * 4.0, 0) * saturate(ng.z * 2));
		float NdV = saturate(dot(n, V));
		float F = 0.02 + 0.98 * pow(1 - NdV, 5);
		float3 R = reflect(-V, n);
		R.z = abs(R.z);                                  // no reflections from below the horizon
		float3 refl = FxSky(R, 1) * asfloat(indices2.z);
		float3 H = normalize(L + V);
		float a2 = max(m.roughness, 0.02); a2 *= a2;
		float NdH = saturate(dot(n, H));
		float D = a2 / (PI * pow(NdH * NdH * (a2 - 1) + 1, 2));
		float3 spec = sunC * D * F * saturate(dot(n, L)) * 0.25;
		// absorption through the water to the bottom
		float thick = sceneD > 0 ? max(ViewDepth(sceneD) - ViewDepth(i.pos.z), 0) / 40.0 : 50.0;
		float3 sigma = (1 - saturate(m.emissive)) * 1.2 + 0.15;
		float3 Tr = exp(-sigma * thick);
		float T = dot(Tr, 1.0 / 3);
		float3 scatter = m.baseColor.rgb * (EvalSH(float3(0, 0, 1)) * PI + sunC * saturate(L.z) * 0.15);
		float3 body = scatter * (1 - Tr);
		float3 rgb = refl * F + spec + body * (1 - F);
		float a = T * (1 - F);
		float4 fog = FxFog(i.wpos, V, length(camPos.xyz - i.wpos));
		return float4(lerp(rgb, fog.rgb * (1 - a), fog.a), a);
	}
	if (m.mflags & 64) {
		// ---- flowing water sheet: alpha from the scrolling channels of the flow record
		uint o = m.albedoTex;
		uint4 h0 = BUF(materialBuf).Load4(o);
		uint nl = min(h0.x, 8u);
		float mul = asfloat(h0.z);
		float frame = t * 24.0;
		float2 uvB = float2(i.uv.x, 1 - i.uv.y);         // Blender UV
		float a = 0;
		[loop] for (uint li = 0; li < nl; li++) {
			uint lo = o + 64 + li * 32;
			uint4 x = BUF(materialBuf).Load4(lo);
			float4 y = asfloat(BUF(materialBuf).Load4(lo + 16));
			uint tex = x.x, chan = x.y; float2 sc = asfloat(x.zw); float2 sp = y.xy; uint op = asuint(y.z);
			float v = 1;
			if (tex) {
				float2 uv = uvB * sc + sp * frame;
				float4 c = Tex2D[NonUniformResourceIndex(tex)].Sample(sWrap, float2(uv.x, 1 - uv.y));
				v = chan == 0 ? c.r : chan == 1 ? c.g : chan == 2 ? c.b : c.a;
			}
			a = op == 0 ? v : op == 1 ? a + v : a * v;
		}
		// softer than the raw sum (the Blender sheets fade out by vertex colour, which is not cooked): a gentler
		// curve, and edge-on sheets fade away
		a = saturate(a * mul);
		a = a * a * (3 - 2 * a) * 0.85;
		a *= saturate(abs(dot(normalize(i.normal), V)) * 3);
		float3 light = EvalSH(ng) * PI + sunC * saturate(dot(ng, L)) / PI;
		float3 col = m.baseColor.rgb * light * 0.8 + m.emissive;
		float4 fog = FxFog(i.wpos, V, length(camPos.xyz - i.wpos));
		col = lerp(col, fog.rgb, fog.a);
		return float4(col * a, 1 - a);
	}
	return float4(0, 0, 0, 1);
}
