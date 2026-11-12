// KatarnEngine - deferred lighting: sun with cascaded shadows, local lights, sky ambient + reflections, height fog
#include "common.hlsli"

cbuffer Draw : register(b0)
{
	uint outUav;
	uint shBuf;
	uint lightBuf;
	uint lightCount;
	uint aoTex;
	uint lflags;     // 1 = AO on, 2 = shadows on, 4 = volumetric sun
	uint debugMode;
	uint ssrSrv;     // last frame's lit scene (0 = no screen-space reflections)
	uint giSrv;      // bounced light (half res: rgb light, a share of rays that hit something), 0 = none
};

struct Light { float3 pos; float radius; float3 color; float type; float3 dir; float spotCos; float spotInner; float invSquare; float2 lp; };
Light LoadLight(uint i)
{
	float4 a = asfloat(BUF(lightBuf).Load4(i * 64)), c = asfloat(BUF(lightBuf).Load4(i * 64 + 16)), d = asfloat(BUF(lightBuf).Load4(i * 64 + 32)), e = asfloat(BUF(lightBuf).Load4(i * 64 + 48));
	Light l;
	l.pos = a.xyz; l.radius = a.w; l.color = c.rgb; l.type = c.w; l.dir = d.xyz; l.spotCos = d.w; l.spotInner = e.x; l.invSquare = e.y; l.lp = e.zw;
	return l;
}

// the HD world's sky texture (Blender equirectangular), else Katarn's atmosphere LUT
float3 SkyRadianceLod(float3 d, float lod)
{
	if (indices2.x) {
		float2 uv = float2(0.5 - atan2(d.y, d.x) / (2 * PI), 0.5 - asin(clamp(d.z, -1, 1)) / PI);
		return Tex2D[indices2.x].SampleLevel(sWrap, uv, lod).rgb * skyParams.y;
	}
	float phi = atan2(d.y, d.x);
	float el = asin(clamp(d.z, -1, 1));
	float2 uv = float2(phi / (2 * PI) + 0.5, 0.5 - el / PI);
	return Tex2D[indices1.z].SampleLevel(sWrap, uv, 0).rgb;
}

float SkyMips()
{
	if (!indices2.x) return 1;
	uint w, h, m;
	Tex2D[indices2.x].GetDimensions(0, w, h, m);
	return m;
}

float3 SkyRadiance(float3 d)
{
	if (indices2.x) return SkyRadianceLod(d, 0);
	float phi = atan2(d.y, d.x);
	float el = asin(clamp(d.z, -1, 1));
	float2 uv = float2(phi / (2 * PI) + 0.5, 0.5 - el / PI);
	return Tex2D[indices1.z].SampleLevel(sWrap, uv, 0).rgb;
}

float3 SkyAmbient(float3 n)
{
	if (indices2.y) return EvalSH(n);
	float3 sh[9];
	for (int k = 0; k < 9; k++) sh[k] = asfloat(BUF(shBuf).Load3(k * 16));
	float3 r = sh[0] * 0.282095;
	r += sh[1] * 0.488603 * n.y + sh[2] * 0.488603 * n.z + sh[3] * 0.488603 * n.x;
	r += sh[4] * 1.092548 * n.x * n.y + sh[5] * 1.092548 * n.y * n.z;
	r += sh[6] * 0.315392 * (3.0 * n.z * n.z - 1.0);
	r += sh[7] * 1.092548 * n.x * n.z + sh[8] * 0.546274 * (n.x * n.x - n.y * n.y);
	return max(r, 0);
}

// reflection probes (captured cubes): the nearest two, sphere-parallax corrected, blended by distance
float3 ProbeOne(uint i, float3 P, float3 R, float rough, out float dist)
{
	float4 a = asfloat(BUF(probeParams.x).Load4(i * 32));
	uint2 b = BUF(probeParams.x).Load2(i * 32 + 16);
	float3 L = P - a.xyz;
	dist = length(L);
	float bq = dot(L, R), cq = dot(L, L) - a.w * a.w;
	float3 dir = R;
	if (cq < 0) dir = L + R * (-bq + sqrt(max(bq * bq - cq, 0)));
	return TexCube[NonUniformResourceIndex(b.x)].SampleLevel(sClamp, float3(dir.x, dir.z, dir.y), rough * (b.y - 1)).rgb;
}

float3 SampleProbes(float3 P, float3 R, float rough, out float weight)
{
	weight = 0;
	uint n = min(probeParams.y, 64u);
	if (!n) return 0;
	float d0 = 1e30, d1 = 1e30, r0 = 1;
	uint i0 = 0, i1 = 0;
	[loop] for (uint i = 0; i < n; i++) {
		float4 a = asfloat(BUF(probeParams.x).Load4(i * 32));
		float3 dd = a.xyz - P;
		float q = dot(dd, dd);
		if (q < d0) { d1 = d0; i1 = i0; d0 = q; i0 = i; r0 = a.w; }
		else if (q < d1) { d1 = q; i1 = i; }
	}
	float e0, e1;
	float3 c = ProbeOne(i0, P, R, rough, e0);
	if (n > 1) {
		float3 c1 = ProbeOne(i1, P, R, rough, e1);
		c = lerp(c, c1, e0 / max(e0 + e1, 1e-3));
	}
	weight = saturate(2.0 - e0 / max(r0, 1.0));    // full inside the nearest probe's space, gone at twice its size
	return min(c, 64.0);
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

// screen-space shadow: march from P towards the light through the depth buffer (contact shadows; shadows of
// characters / props for lights that have no shadow map - sabers, dynamic lights)
float ScreenShadow(float3 P, float3 N, float3 L, float dist, int steps, float thick, float jitter)
{
	float3 O = P + N * 1.0 + L * 1.0;
	float stepLen = dist / steps;
	float occ = 0;
	[loop] for (int i = 0; i < steps; i++) {
		float3 Q = O + L * stepLen * (i + jitter);
		float4 cp = mul(viewProj, float4(Q, 1));
		if (cp.w <= 0) break;
		float2 suv = cp.xy / cp.w * float2(0.5, -0.5) + 0.5;
		if (any(suv < 0) || any(suv > 1)) break;
		float sd = Tex2D[indices0.w].Load(int3(viewport.xy + suv * viewport.zw, 0)).r;
		if (sd <= 0) continue;
		float dz = ViewDepth(cp.z / cp.w) - ViewDepth(sd);
		if (dz > 0.4 && dz < thick) { occ = max(occ, saturate(1 - (float)i / steps) * 0.5 + 0.5); if (occ > 0.99) break; }
	}
	return 1 - occ;
}

// one shadow tap (volumetric fog marching)
float ShadowFast(float3 P, float viewZ)
{
	int c = 0;
	[unroll] for (int i = 0; i < 4; i++) if (viewZ > cascadeSplits[i]) c = i + 1;
	if (c >= 4) return 1;
	float4 sc = mul(cascadeViewProj[c], float4(P, 1));
	float2 suv = sc.xy * float2(0.5, -0.5) + 0.5;
	if (any(suv < 0) || any(suv > 1) || sc.z > 1) return 1;
	return Tex2DArray[indices1.x].SampleCmpLevelZero(sShadow, float3(suv, c), sc.z - shadowParams.y * 2);
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
	// a blurred sky (top mips): the raw horizon row of a sky texture has sharp vertical detail, which the fog would
	// smear into streaks running up the whole screen
	float3 inscatter = (indices2.x ? SkyRadianceLod(hd, max(SkyMips() - 4, 0)) : SkyRadiance(hd)) * fogColor.rgb;
	float sunVis = 1;
	if ((lflags & 4) && (lflags & 2) && shadowParams.w > 0 && sunColor.w > 0) {
		// volumetric: how much of the view ray inside the shadow range sees the sun (shafts through the fog)
		float maxD = min(dist, cascadeSplits[3]);
		float jitter = frac(52.9829189 * frac(dot(P.xy * 0.37 + indices2.w * 0.618, float2(0.06711056, 0.00583715))));
		float lit = 0;
		const int STEPS = 16;
		[loop] for (int k = 0; k < STEPS; k++) {
			float t = (k + jitter) / STEPS;
			t *= t;                                          // more samples near the camera
			float3 Q = camPos.xyz - V * (maxD * t);
			lit += ShadowFast(Q, maxD * t);
		}
		sunVis = lit / STEPS;
		sunVis = lerp(1, sunVis, saturate(maxD / max(dist, 1)));
	}
	float phase = pow(saturate(dot(-V, sunDir.xyz)), 8) * 0.25 + 0.04;   // forward scattering + a little everywhere
	inscatter += sunColor.rgb * sunColor.w * phase * fogColor.rgb * (sunVis * 1.6 - 0.15 * (1 - sunVis));
	return lerp(col, max(inscatter, 0), opacity);
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
	float fw = g1.w;
	bool eye = fw > 31.5; if (eye) fw -= 32.0;   // character eyes (wet, sharp highlights)
	bool hair = fw > 15.5; if (hair) fw -= 16.0; // character hair: Kajiya-Kay highlights
	bool foliage = fw > 7.5;                     // HD world leaves / grass: light passes through
	bool skin = !foliage && fw > 3.5;            // rend2 skin keyword: soft, subsurface-scattered diffuse
	bool plainLegacy = !foliage && !skin && fw > 1.5;        // old model without PBR maps
	float rough = abs(g1.z), metal = foliage ? fw - 8.0 : skin ? fw - 4.0 : plainLegacy ? fw - 2.0 : fw;
	float3 albedo = g0.rgb;
	float ao = g0.a;
	if (lflags & 1) ao *= Tex2D[aoTex].Load(int3(px, 0)).r;
	float3 diff = albedo * (1 - metal);
	float3 f0 = lerp(0.04, albedo, metal);
	if (plainLegacy) f0 *= 0.3;                  // old models without PBR maps: barely any specular (rend2 gives them almost none)
	float viewZ = ViewDepth(depth);
	float pxJitter = frac(52.9829189 * frac(dot(float2(px), float2(0.06711056, 0.00583715)) + indices2.w * 0.618));

	float3 Lo = 0;
	// sun
	float3 L = sunDir.xyz;
	if (foliage && sunColor.w > 0) {
		// leaves: lit on the side facing the sun, and glowing through (a warmer, yellower green) when the sun is behind
		// them; the forward-scatter lobe brightens canopies seen against the sun like Cycles' translucent BSDF
		float nl = dot(N, L);
		float sh = Shadow(P, nl < 0 ? -N : N, viewZ);
		float3 thru = diff * float3(1.05, 1.1, 0.55);
		float fwd = pow(saturate(dot(-V, L)), 4);
		Lo += (diff * saturate(nl) + thru * saturate(-nl) * 0.65 + thru * fwd * 0.35) / PI * sunColor.rgb * sunColor.w * sh;
		if (nl > 0) Lo += Brdf(N, V, L, 0, f0, max(rough, 0.5)) * sunColor.rgb * sunColor.w * sh * 0.5;
	}
	else if (skin && sunColor.w > 0) {
		// wrapped diffuse with a warm scattered terminator (light bleeding through skin), GGX on top
		float nl = dot(N, L);
		float wrap = saturate((nl + 0.25) / 1.25);
		float sh = Shadow(P, N, viewZ);
		float3 scatter = lerp(float3(1.0, 0.62, 0.52), 1.0, saturate(nl * 3.0 + 0.3));
		Lo += diff / PI * wrap * scatter * sunColor.rgb * sunColor.w * lerp(0.35, 1.0, sh);
		if (nl > 0) Lo += (Brdf(N, V, L, 0, f0, rough)) * sunColor.rgb * sunColor.w * sh;
	}
	else if (hair && sunColor.w > 0) {
		float sh = Shadow(P, N, viewZ);
		float nl = dot(N, L);
		Lo += diff / PI * saturate((nl + 0.3) / 1.3) * sunColor.rgb * sunColor.w * sh;
		Lo += HairSpec(HairStrand(N), N, V, L, albedo, rough) * sunColor.rgb * sunColor.w * sh;
	}
	else if (sunColor.w > 0 && dot(N, L) > 0) {
		float sh = Shadow(P, N, viewZ);
		if ((lflags & 8) && sh > 0.01) sh *= ScreenShadow(P, N, L, 24 + viewZ * 0.004, 10, 6 + viewZ * 0.002, pxJitter);
		Lo += Brdf(N, V, L, diff, f0, rough) * sunColor.rgb * sunColor.w * sh;
	}
	// local lights (the strongest few get screen-space shadows)
	uint shadowed = 0;
	for (uint i = 0; i < lightCount; i++) {
		Light l = LoadLight(i);
		if (l.type == 2) {
			// line light (lightsaber): nearest point on the segment for diffuse, the point nearest the reflection
			// ray for the highlight
			float3 A = l.pos, Bv = l.dir;
			float len2 = max(dot(Bv, Bv), 1e-4);
			float tP = saturate(dot(P - A, Bv) / len2);
			float3 Cd = A + Bv * tP - P;
			float dd2 = dot(Cd, Cd);
			if (dd2 > l.radius * l.radius) continue;
			float wnd = saturate(1 - pow(dd2 / (l.radius * l.radius), 2)); wnd *= wnd;
			float att = wnd * l.invSquare / (dd2 + l.lp.x);   // pad[0]: the blade's soft radius squared
			float3 Rr = reflect(-V, N);
			float3 A0 = A - P;
			float rb = dot(Rr, Bv), ra = dot(Rr, A0);
			float tR = saturate((dot(A0, Bv) * 0 + ra * rb - dot(A0, Bv)) / max(len2 - rb * rb, 1e-3));
			float3 Cs = A + Bv * tR - P;
			float3 Ldiff = Cd * rsqrt(max(dd2, 1e-4)), Lspec = Cs * rsqrt(max(dot(Cs, Cs), 1e-4));
			float NoLd = saturate(dot(N, Ldiff));
			if ((lflags & 16) && shadowed < 3 && NoLd > 0 && att * Luminance(l.color) > 0.02) {
				shadowed++;
				float sdist = min(sqrt(dd2), 320.0);
				float s = ScreenShadow(P, N, Ldiff, sdist, 14, 40, pxJitter);
				att *= s;
			}
			Lo += diff / PI * NoLd * l.color * att;
			// r22 (Frostbite 2014 / Karis 2013 area lights): the highlight is the blade's reflection - widen the lobe by
			// the blade's apparent size from the reflection point and keep its energy (a / a'), attenuated from that point,
			// so armour, helmets and wet skin show a long bright streak instead of a pin-point
			{
				float dS2 = max(dot(Cs, Cs), 1.0), dS = sqrt(dS2);
				float a0 = max(rough * rough, 0.0144);
				float aSph = saturate(a0 + 1.5 / (2.0 * dS));              // blade core radius ~1.5 units
				float aLin = saturate(a0 + sqrt(len2) / (2.0 * dS));       // along the blade
				float energy = (a0 / aSph) * (a0 / aSph) * (a0 / aLin);
				float attS = wnd * l.invSquare / (dS2 + l.lp.x);
				Lo += Brdf(N, V, Lspec, 0, f0, sqrt(a0)) * energy * l.color * attS;
			}
			continue;
		}
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
		if ((lflags & 16) && shadowed < 3 && dot(N, Ld) > 0 && att * Luminance(l.color) > 0.02) {
			shadowed++;
			att *= ScreenShadow(P, N, Ld, min(dl, 320.0), 14, 40, pxJitter);
		}
		if (hair) Lo += (diff / PI * saturate(dot(N, Ld)) + HairSpec(HairStrand(N), N, V, Ld, albedo, rough)) * l.color * att;
		else Lo += Brdf(N, V, Ld, diff, f0, rough) * l.color * att;
	}
	// ambient
	float NoV = max(dot(N, V), 1e-4);
	float3 R = reflect(-V, N);
	float3 env = EnvBRDFApprox(f0, rough, NoV);
	float specOcc = saturate(pow(NoV + ao, exp2(-16 * rough - 1)) - 1 + ao);
	float3 spec = 0;
	float ssrHit = 0;
	float probeW = 0;
	{
		// rough reflections: a small cone of sky samples around R
		float3 t = normalize(abs(R.z) < 0.99 ? cross(R, float3(0, 0, 1)) : cross(R, float3(1, 0, 0)));
		float3 b = cross(R, t);
		float cone = rough * rough * 1.2;
		if (indices2.x) spec = SkyRadianceLod(R, rough * (SkyMips() - 1) * 0.8) * asfloat(indices2.z);
		else {
			float3 acc = SkyRadiance(R);
			[unroll] for (int k = 0; k < 6; k++) {
				float a = k * (2 * PI / 6);
				acc += SkyRadiance(normalize(R + (t * cos(a) + b * sin(a)) * cone));
			}
			spec = acc / 7.0;
		}
		// below the horizon the "sky" is the ground: darken reflections that point down
		spec *= saturate(R.z * 4 + 0.6);
		// reflection probes: what is really around (rooms, walls, the ground) instead of the open sky
		if (probeParams.y) {
			float3 pc = SampleProbes(P, R, rough, probeW);
			spec = lerp(spec, pc * asfloat(probeParams.z) / max(skyParams.z, 1e-3), probeW);
		}
		// screen-space reflections from last frame's lit scene
		if (ssrSrv && rough < 0.45 && !plainLegacy) {
			float3 O = P + N * (2.0 + viewZ * 0.002);
			float stepLen = 6.0 + viewZ * 0.01;
			float jit = frac(52.9829189 * frac(dot(float2(px), float2(0.06711056, 0.00583715)) + indices2.w * 0.618));
			float3 Q = O + R * stepLen * jit;
			float hit = 0; float2 huv = 0;
			[loop] for (int sI = 0; sI < 40; sI++) {
				Q += R * stepLen;
				stepLen *= 1.08;
				float4 cp = mul(viewProj, float4(Q, 1));
				if (cp.w <= 0) break;
				float2 suv = cp.xy / cp.w * float2(0.5, -0.5) + 0.5;
				if (any(suv < 0) || any(suv > 1)) break;
				float2 spx = viewport.xy + suv * viewport.zw;
				float sd = Tex2D[indices0.w].Load(int3(spx, 0)).r;
				float rayZ = ViewDepth(cp.z / cp.w), sceneZ = ViewDepth(sd);
				float diffZ = rayZ - sceneZ;
				if (sd > 0 && diffZ > 0 && diffZ < stepLen * 2.5) {
					float4 pp = mul(prevViewProj, float4(Q, 1));
					huv = pp.xy / pp.w * float2(0.5, -0.5) + 0.5;
					float2 e = saturate(min(huv, 1 - huv) * 10);
					hit = e.x * e.y * saturate(1 - sI / 40.0) * saturate((0.45 - rough) / 0.2);
					break;
				}
			}
			if (hit > 0) {
				float3 sc = Tex2D[ssrSrv].SampleLevel(sClamp, (viewport.xy + huv * viewport.zw) * invTargetSize.xy, 0).rgb;
				sc = min(sc, 64.0);                              // no fireflies from the sun disc
				spec = lerp(spec, sc / max(skyParams.z, 1e-3), hit);
				ssrHit = hit;
			}
		}
	}
	if (baked) {
		float3 bakedL = g2;
		if (bspSun.x > 0) {
			// rend2's forced sun (r_sunlightMode 1): the map's light stays where the sun reaches; in its shadow and on
			// surfaces turned away from it the light is scaled down (old models get half of that)
			float sunVis = Shadow(P, N, viewZ) * saturate(dot(N, sunDir.xyz));
			float keep = plainLegacy ? lerp(bspSun.y, 1.0, 0.5) : bspSun.y;
			bakedL *= lerp(keep, 1.0, sunVis);
		}
		Lo += bakedL * ao;
		// old BSP maps: no sky light to reflect (their light grid / lightmaps carry it) - only the probes and SSR.
		// rend2 scales a cubemap reflection by the light at the surface over the cubemap's brightness, so a model in
		// a dark corner does not mirror the bright room around it: the same here (against the probe's blurriest level)
		float reflScale = 1.0;
		if (probeW > 0 || ssrHit > 0) {
			float pw2 = 0;
			float3 around = probeParams.y ? SampleProbes(P, N, 1.0, pw2) * asfloat(probeParams.z) / max(skyParams.z, 1e-3) : spec;
			reflScale = saturate(Luminance(g2) / max(Luminance(around) * 1.5, 1e-3));
		}
		Lo += spec * env * specOcc * lerp(0.35 * skyParams.w, 1.0, max(probeW, ssrHit)) * reflScale;
	}
	else {
		// indirect diffuse: the sky, the reflection probes' surroundings (their blurriest level), bounced light
		float3 amb = SkyAmbient(N) * skyParams.z;
		if (probeParams.y) {
			float pw;
			float3 pirr = SampleProbes(P, N, 1.0, pw);
			amb = lerp(amb, pirr * asfloat(probeParams.z), pw * 0.85);
		}
		if (giSrv) {
			float4 gi = Tex2D[giSrv].SampleLevel(sClamp, (float2(px) - viewport.xy + 0.5) * invTargetSize.xy, 0);
			amb = amb * (1 - saturate(gi.a) * 0.8) + gi.rgb;
		}
		Lo += amb * diff * ao;
		Lo += spec * env * specOcc * skyParams.z * (plainLegacy ? 0.25 : 1.0);
		Lo += g2;
	}
	Lo = ApplyFog(Lo, P, V, dist);
	if (debugMode == 1) Lo = albedo;
	else if (debugMode == 2) Lo = N * 0.5 + 0.5;
	else if (debugMode == 3) Lo = ao;
	else if (debugMode == 4) Lo = Shadow(P, N, viewZ);
	if (debugMode) Lo /= max(Exposure(), 1e-6);
	UavTex[outUav][px] = float4(Sane(Lo), 1);
}

// ------------------------------------------------------------------------------------------------ volumetric fog
// Frostbite-style froxels: a grid over the view frustum (exponential depth slices); each cell gets the fog's density and
// the light scattered towards the camera from the sun (through its shadow map), the local lights and lightsabers, and
// the sky. Uses this pass's constants: outUav = the froxel grid (RWTexture3D), lightBuf / lightCount, lflags.
float FroxelDepth(float s) { return vfog1.w * pow(vfog0.w / vfog1.w, s / vfogIdx.w); }

[numthreads(8, 8, 1)]
void FogInjectCS(uint3 id : SV_DispatchThreadID)
{
	uint3 dim = uint3(vfogIdx.y & 0xffff, vfogIdx.y >> 16, vfogIdx.w);
	if (id.x >= dim.x || id.y >= dim.y) return;
	// slice jitter: changes every frame only under TAA (which averages it); without TAA it stays put (no shimmer)
	float jit = frac(sin(dot(float2(id.xy) + (taaParams.w > 0.5 ? projParams.w * 7.13 : 0.0), float2(12.9898, 78.233))) * 43758.5453);
	for (uint z = 0; z < dim.z; z++) {
		float viewZ = FroxelDepth(z + jit);
		float2 vuv = (float2(id.xy) + 0.5) / float2(dim.xy);
		float4 w = mul(invViewProj, float4(vuv * float2(2, -2) + float2(-1, 1), projParams.z / viewZ, 1));
		float3 P = w.xyz / w.w;
		float3 V = normalize(P - camPos.xyz);
		float dens = vfog0.x * exp(-vfog0.y * max(P.z - (camPos.z - 64), 0));
		float3 S = 0;
		// sun: Henyey-Greenstein forward scattering through the cascades
		if (vfog1.x > 0 && (lflags & 2)) {
			float g = 0.6, ct = dot(V, sunDir.xyz);
			float hg = (1 - g * g) / (4 * PI * pow(abs(1 + g * g - 2 * g * ct), 1.5));
			float3 sunC = sunColor.w > 0 ? sunColor.rgb * sunColor.w : float3(1, 0.96, 0.9);
			S += sunC * hg * ShadowFast(P, viewZ) * vfog1.x;
		}
		// local lights and lightsabers (no shadows), scaled up the way Unreal's volumetric scattering intensity is
		[loop] for (uint i = 0; i < lightCount; i++) {
			Light l = LoadLight(i);
			if (l.lp.y > 0.5) continue;   // r21: lights made from glowing surfaces stay out of the air
			float3 d;
			if (l.type == 2) {
				float len2 = max(dot(l.dir, l.dir), 1e-4);
				d = l.pos + l.dir * saturate(dot(P - l.pos, l.dir) / len2) - P;
			}
			else d = l.pos - P;
			float d2 = dot(d, d);
			if (d2 > l.radius * l.radius) continue;
			float wnd = saturate(1 - pow(d2 / (l.radius * l.radius), 2)); wnd *= wnd;
			float att = l.invSquare > 0 ? wnd * l.invSquare / (d2 + (l.type == 2 ? l.lp.x : 256.0)) : wnd;
			if (l.type == 1) att *= smoothstep(l.spotCos, l.spotInner, dot(-normalize(d), l.dir));
			S += l.color * att / (4 * PI) * vfog1.y;
		}
		// sky / ambient
		S += (skyParams.w > 0.5 ? SkyAmbient(float3(0, 0, 1)) * skyParams.z : vfog1.zzz) * 0.5;
		S *= vfogColor.rgb * dens;
		UavTex3D[outUav][uint3(id.xy, z)] = float4(S, dens);
	}
}
