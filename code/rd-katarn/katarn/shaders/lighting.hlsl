// KatarnEngine - deferred lighting: sun with cascaded shadows, local lights, sky ambient + reflections, height fog
#include "common.hlsli"

cbuffer Draw : register(b0)
{
	uint outUav;
	uint shBuf;
	uint lightBuf;
	uint lightCount;
	uint aoTex;
	uint lflags;     // 1 = AO on, 2 = shadows on
	uint debugMode;
	uint lpad0;
};

struct Light { float3 pos; float radius; float3 color; float type; float3 dir; float spotCos; float spotInner; float invSquare; float2 lp; };
Light LoadLight(uint i)
{
	float4 a = asfloat(BUF(lightBuf).Load4(i * 64)), c = asfloat(BUF(lightBuf).Load4(i * 64 + 16)), d = asfloat(BUF(lightBuf).Load4(i * 64 + 32)), e = asfloat(BUF(lightBuf).Load4(i * 64 + 48));
	Light l;
	l.pos = a.xyz; l.radius = a.w; l.color = c.rgb; l.type = c.w; l.dir = d.xyz; l.spotCos = d.w; l.spotInner = e.x; l.invSquare = e.y; l.lp = e.zw;
	return l;
}

float3 SkyRadiance(float3 d)
{
	float phi = atan2(d.y, d.x);
	float el = asin(clamp(d.z, -1, 1));
	float2 uv = float2(phi / (2 * PI) + 0.5, 0.5 - el / PI);
	return Tex2D[indices1.z].SampleLevel(sWrap, uv, 0).rgb;
}

float3 SkyAmbient(float3 n)
{
	float3 sh[9];
	for (int k = 0; k < 9; k++) sh[k] = asfloat(BUF(shBuf).Load3(k * 16));
	float3 r = sh[0] * 0.282095;
	r += sh[1] * 0.488603 * n.y + sh[2] * 0.488603 * n.z + sh[3] * 0.488603 * n.x;
	r += sh[4] * 1.092548 * n.x * n.y + sh[5] * 1.092548 * n.y * n.z;
	r += sh[6] * 0.315392 * (3.0 * n.z * n.z - 1.0);
	r += sh[7] * 1.092548 * n.x * n.z + sh[8] * 0.546274 * (n.x * n.x - n.y * n.y);
	return max(r, 0) / PI;
}

float D_GGX(float NoH, float a) { float a2 = a * a; float f = (NoH * a2 - NoH) * NoH + 1.0; return a2 / (PI * f * f); }
float V_Smith(float NoV, float NoL, float a)
{
	float a2 = a * a;
	float gv = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
	float gl = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
	return 0.5 / max(gv + gl, 1e-5);
}
float3 F_Schlick(float3 f0, float VoH) { return f0 + (1.0 - f0) * pow(1.0 - VoH, 5.0); }
float3 EnvBRDFApprox(float3 f0, float rough, float NoV)
{
	const float4 c0 = float4(-1, -0.0275, -0.572, 0.022);
	const float4 c1 = float4(1, 0.0425, 1.04, -0.04);
	float4 r = rough * c0 + c1;
	float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
	float2 ab = float2(-1.04, 1.04) * a004 + r.zw;
	return f0 * ab.x + ab.y;
}

float3 Brdf(float3 N, float3 V, float3 L, float3 diff, float3 f0, float rough)
{
	float3 H = normalize(V + L);
	float NoL = saturate(dot(N, L)), NoV = max(dot(N, V), 1e-4), NoH = saturate(dot(N, H)), VoH = saturate(dot(V, H));
	float a = rough * rough;
	float3 spec = D_GGX(NoH, a) * V_Smith(NoV, NoL, a) * F_Schlick(f0, VoH);
	return (diff / PI + spec) * NoL;
}

float Shadow(float3 P, float3 N, float viewZ)
{
	if (!(lflags & 2) || shadowParams.w == 0) return 1;
	int c = 0;
	[unroll] for (int i = 0; i < 4; i++) if (viewZ > cascadeSplits[i]) c = i + 1;
	if (c >= 4) return 1;
	float texel = shadowParams.x;
	float3 Pn = P + N * shadowParams.z * (1 + c * 1.5);
	float4 sc = mul(cascadeViewProj[c], float4(Pn, 1));
	float2 suv = sc.xy * float2(0.5, -0.5) + 0.5;
	float z = sc.z - shadowParams.y;
	if (any(suv < 0) || any(suv > 1) || z > 1) return 1;
	float s = 0;
	[unroll] for (int y = -2; y <= 2; y++)
		[unroll] for (int x = -2; x <= 2; x++)
			s += Tex2DArray[indices1.x].SampleCmpLevelZero(sShadow, float3(suv + float2(x, y) * texel, c), z);
	s /= 25.0;
	float fade = saturate((cascadeSplits[3] - viewZ) / (cascadeSplits[3] * 0.1));
	return lerp(1, s, c == 3 ? fade : 1);
}

float3 ApplyFog(float3 col, float3 P, float3 V, float dist)
{
	if (fogParams.x <= 0) return col;
	float rayLen = max(dist - fogParams.w, 0);
	float dz = P.z - camPos.z;
	float f = fogParams.y;
	float camDens = fogParams.x * exp(-f * (camPos.z - fogParams.z));
	float fz = f * dz;
	float lineInt = abs(fz) > 1e-4 ? (1 - exp(-fz)) / fz : 1 - 0.5 * fz;
	float amount = camDens * lineInt * rayLen;
	float opacity = min(1 - exp(-amount), fogColor.w);
	float3 hd = normalize(float3(-V.xy, 0.05));
	float3 inscatter = SkyRadiance(hd) * fogColor.rgb;
	inscatter += sunColor.rgb * sunColor.w * pow(saturate(dot(-V, sunDir.xyz)), 8) * 0.25 * fogColor.rgb;
	return lerp(col, inscatter, opacity);
}

[numthreads(8, 8, 1)]
void CS(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= (uint)viewport.z || id.y >= (uint)viewport.w) return;
	uint2 px = id.xy + (uint2)viewport.xy;
	float2 uv = (id.xy + 0.5) / viewport.zw;
	float depth = Tex2D[indices0.w].Load(int3(px, 0)).r;
	float3 P = WorldFromDepth(uv, depth);
	float3 toCam = camPos.xyz - P;
	float dist = length(toCam);
	float3 V = toCam / max(dist, 1e-4);
	if (depth <= 0.0) {
		float3 d = normalize(WorldFromDepth(uv, 1e-6) - camPos.xyz);
		float3 c = SkyRadiance(d);
		float cosSun = dot(d, sunDir.xyz);
		float cosR = cos(sunDir.w);
		if (cosSun > cosR && d.z > -0.02) c += sunColor.rgb * sunColor.w * 4000.0 * smoothstep(cosR, cosR + 0.00002, cosSun) * saturate(d.z * 20 + 0.4);
		UavTex[outUav][px] = float4(c, 1);
		return;
	}
	float4 g0 = Tex2D[indices0.x].Load(int3(px, 0));
	float4 g1 = Tex2D[indices0.y].Load(int3(px, 0));
	float3 g2 = Tex2D[indices0.z].Load(int3(px, 0)).rgb;
	float3 N = DecodeNormal(g1.xy);
	bool baked = g1.z < 0;
	float rough = abs(g1.z), metal = g1.w;
	float3 albedo = g0.rgb;
	float ao = g0.a;
	if (lflags & 1) ao *= Tex2D[aoTex].Load(int3(px, 0)).r;
	float3 diff = albedo * (1 - metal);
	float3 f0 = lerp(0.04, albedo, metal);
	float viewZ = ViewDepth(depth);

	float3 Lo = 0;
	// sun
	float3 L = sunDir.xyz;
	if (sunColor.w > 0 && dot(N, L) > 0) {
		float sh = Shadow(P, N, viewZ);
		Lo += Brdf(N, V, L, diff, f0, rough) * sunColor.rgb * sunColor.w * sh;
	}
	// local lights
	for (uint i = 0; i < lightCount; i++) {
		Light l = LoadLight(i);
		float3 d = l.pos - P;
		float d2 = dot(d, d);
		if (d2 > l.radius * l.radius) continue;
		float dl = sqrt(d2);
		float3 Ld = d / max(dl, 1e-4);
		float w = saturate(1 - pow(d2 / (l.radius * l.radius), 2));
		w *= w;
		float att = l.invSquare > 0 ? w * l.invSquare / max(d2, 64.0) : w;
		if (l.type == 1) att *= smoothstep(l.spotCos, l.spotInner, dot(-Ld, l.dir));
		if (att <= 0) continue;
		Lo += Brdf(N, V, Ld, diff, f0, rough) * l.color * att;
	}
	// ambient
	float NoV = max(dot(N, V), 1e-4);
	float3 R = reflect(-V, N);
	float3 env = EnvBRDFApprox(f0, rough, NoV);
	float specOcc = saturate(pow(NoV + ao, exp2(-16 * rough - 1)) - 1 + ao);
	float3 spec = 0;
	{
		// rough reflections: a small cone of sky samples around R
		float3 t = normalize(abs(R.z) < 0.99 ? cross(R, float3(0, 0, 1)) : cross(R, float3(1, 0, 0)));
		float3 b = cross(R, t);
		float cone = rough * rough * 1.2;
		float3 acc = SkyRadiance(R);
		[unroll] for (int k = 0; k < 6; k++) {
			float a = k * (2 * PI / 6);
			acc += SkyRadiance(normalize(R + (t * cos(a) + b * sin(a)) * cone));
		}
		spec = acc / 7.0;
		// below the horizon the "sky" is the ground: darken reflections that point down
		spec *= saturate(R.z * 4 + 0.6);
	}
	if (baked) {
		Lo += g2 * ao;
		Lo += spec * env * specOcc * 0.35;
	}
	else {
		Lo += SkyAmbient(N) * diff * ao * skyParams.z;
		Lo += spec * env * specOcc * skyParams.z;
		Lo += g2;
	}
	Lo = ApplyFog(Lo, P, V, dist);
	if (debugMode == 1) Lo = albedo;
	else if (debugMode == 2) Lo = N * 0.5 + 0.5;
	else if (debugMode == 3) Lo = ao;
	else if (debugMode == 4) Lo = Shadow(P, N, viewZ);
	if (debugMode) Lo /= max(Exposure(), 1e-6);
	UavTex[outUav][px] = float4(Lo, 1);
}
