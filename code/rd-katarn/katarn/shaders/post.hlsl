// KatarnEngine - camera: auto exposure, bloom, filmic tone mapping, FXAA
#include "common.hlsli"

cbuffer Draw : register(b0)
{
	uint srcSrv;
	uint dstUav;
	uint histBuf;      // RW: 64 bins + exposure state
	uint bloomSrv;
	uint2 dstSize;
	float dt;
	uint pmode;
};

static const float HMin = -10.0, HMax = 8.0;
groupshared uint ghist[64];

[numthreads(16, 16, 1)]
void HistogramCS(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
	if (gi < 64) ghist[gi] = 0;
	GroupMemoryBarrierWithGroupSync();
	if (id.x < (uint)viewport.z / 2 && id.y < (uint)viewport.w / 2) {
		uint2 px = id.xy * 2 + (uint2)viewport.xy;
		float l = Luminance(Tex2D[srcSrv].Load(int3(px, 0)).rgb);
		uint bin = l < 1e-6 ? 0 : (uint)clamp((log2(l) - HMin) / (HMax - HMin) * 62.0 + 1.0, 1.0, 63.0);
		// weight the centre of the screen more (photographers meter the subject)
		float2 c = (float2(id.xy) / (viewport.zw * 0.5)) - 0.5;
		uint w = (uint)(1 + 3 * saturate(1 - dot(c, c) * 3));
		InterlockedAdd(ghist[bin], w);
	}
	GroupMemoryBarrierWithGroupSync();
	if (gi < 64 && ghist[gi]) UAVBUF(histBuf).InterlockedAdd(16 + gi * 4, ghist[gi]);
}

[numthreads(64, 1, 1)]
void ExposureCS(uint gi : SV_GroupIndex)
{
	ghist[gi] = UAVBUF(histBuf).Load(16 + gi * 4);
	GroupMemoryBarrierWithGroupSync();
	if (gi == 0) {
		float total = 0;
		for (int i = 1; i < 64; i++) total += ghist[i];
		float lo = total * 0.5, hi = total * 0.95;
		float acc = 0, sum = 0, cnt = 0;
		for (int j = 1; j < 64; j++) {
			float c = ghist[j];
			float take = clamp(acc + c, lo, hi) - clamp(acc, lo, hi);
			acc += c;
			float lum = (j - 1) / 62.0 * (HMax - HMin) + HMin;
			sum += lum * take; cnt += take;
		}
		float avgLog = cnt > 0 ? sum / cnt : -2.0;
		float avg = exp2(avgLog);
		float target = 0.18 / max(avg, 1e-5) * exp2(postParams.x);
		float prev = asfloat(UAVBUF(histBuf).Load(0));
		if (!(prev > 0) || prev > 1e9) prev = target;
		float speed = target > prev ? 2.5 : 1.2;
		float e = prev + (target - prev) * (1 - exp(-dt * speed));
		if (postParams.z > 0) e = postParams.z;
		e = clamp(e, 1e-6, 1e6);
		UAVBUF(histBuf).Store2(0, asuint(float2(e, avg)));
	}
	GroupMemoryBarrierWithGroupSync();
	UAVBUF(histBuf).Store(16 + gi * 4, 0);
}

// ---------------------------------------------------------------- bloom
float3 Fetch(uint tex, float2 uv) { return Sane(Tex2D[tex].SampleLevel(sClamp, uv, 0).rgb); }

[numthreads(8, 8, 1)]
void BloomDownCS(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= dstSize)) return;
	float2 ts = 1.0 / float2(dstSize);
	float2 uv = (id.xy + 0.5) * ts;
	float2 h = ts * 0.5;
	if (pmode == 0) {
		// first level: from the HDR scene inside the view, exposed, fireflies tamed, soft threshold
		uv = (viewport.xy + uv * viewport.zw) * invTargetSize.xy;
		h = invTargetSize.xy;
	}
	float3 a = Fetch(srcSrv, uv + float2(-2, -2) * h), b = Fetch(srcSrv, uv + float2(0, -2) * h), c = Fetch(srcSrv, uv + float2(2, -2) * h);
	float3 d = Fetch(srcSrv, uv + float2(-1, -1) * h), e = Fetch(srcSrv, uv + float2(1, -1) * h);
	float3 f = Fetch(srcSrv, uv + float2(-2, 0) * h), g0 = Fetch(srcSrv, uv), hh = Fetch(srcSrv, uv + float2(2, 0) * h);
	float3 i = Fetch(srcSrv, uv + float2(-1, 1) * h), j = Fetch(srcSrv, uv + float2(1, 1) * h);
	float3 k = Fetch(srcSrv, uv + float2(-2, 2) * h), l = Fetch(srcSrv, uv + float2(0, 2) * h), m = Fetch(srcSrv, uv + float2(2, 2) * h);
	float3 r;
	if (pmode == 0) {
		float ex = Exposure();
		float3 g1 = (d + e + i + j) * 0.25, g2 = (a + b + g0 + f) * 0.25, g3 = (b + c + hh + g0) * 0.25, g4 = (f + g0 + k + l) * 0.25, g5 = (g0 + hh + l + m) * 0.25;
		float w1 = 1 / (1 + Luminance(g1) * ex), w2 = 1 / (1 + Luminance(g2) * ex), w3 = 1 / (1 + Luminance(g3) * ex), w4 = 1 / (1 + Luminance(g4) * ex), w5 = 1 / (1 + Luminance(g5) * ex);
		r = (g1 * w1 * 0.5 + (g2 * w2 + g3 * w3 + g4 * w4 + g5 * w5) * 0.125) / (w1 * 0.5 + (w2 + w3 + w4 + w5) * 0.125);
		r *= ex;
		float br = max(r.r, max(r.g, r.b));
		float knee = 0.5, thr = 1.0;
		float soft = clamp(br - thr + knee, 0, 2 * knee);
		soft = soft * soft / (4 * knee + 1e-4);
		r *= max(soft, br - thr) / max(br, 1e-4);
		if (histBuf) {
			// rend2 dynamic glow: the glow stages, blurred with the bloom (no threshold)
			float3 gl = 0;
			[unroll] for (int gy = -1; gy <= 1; gy++) [unroll] for (int gx = -1; gx <= 1; gx++) gl += Fetch(histBuf, uv + float2(gx, gy) * h);
			r += gl / 9.0 * ex * dt;
		}
	}
	else r = (d + e + i + j) * 0.125 + (a + c + k + m) * 0.03125 + (b + f + hh + l) * 0.0625 + g0 * 0.125;
	UavTex[dstUav][id.xy] = float4(r, 1);
}

[numthreads(8, 8, 1)]
void BloomUpCS(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= dstSize)) return;
	float2 uv = (id.xy + 0.5) / float2(dstSize);
	float2 h = 1.0 / float2(dstSize);
	float3 s = Fetch(bloomSrv, uv + float2(-1, -1) * h) + Fetch(bloomSrv, uv + float2(1, -1) * h) + Fetch(bloomSrv, uv + float2(-1, 1) * h) + Fetch(bloomSrv, uv + float2(1, 1) * h);
	s += 2 * (Fetch(bloomSrv, uv + float2(0, -1) * h) + Fetch(bloomSrv, uv + float2(-1, 0) * h) + Fetch(bloomSrv, uv + float2(1, 0) * h) + Fetch(bloomSrv, uv + float2(0, 1) * h));
	s += 4 * Fetch(bloomSrv, uv);
	s /= 16;
	float3 base = Tex2D[srcSrv].Load(int3(id.xy, 0)).rgb;
	UavTex[dstUav][id.xy] = float4(base + s, 1);
}

// ---------------------------------------------------------------- tone mapping (PS over the view rect)
float3 ACESFitted(float3 c)
{
	const float3x3 inM = float3x3(0.59719, 0.35458, 0.04823, 0.07600, 0.90834, 0.01566, 0.02840, 0.13383, 0.83777);
	const float3x3 outM = float3x3(1.60475, -0.53108, -0.07367, -0.10208, 1.10813, -0.00605, -0.00327, -0.07276, 1.07602);
	c = mul(inM, c);
	float3 a = c * (c + 0.0245786) - 0.000090537;
	float3 b = c * (0.983729 * c + 0.4329510) + 0.238081;
	return saturate(mul(outM, a / b));
}

// AgX (Blender's default view transform since 4.0): inset into a wider gamut, log2 encoding over -12.47..+4.03 EV
// around middle grey, the AgX sigmoid, outset back. Same image formation as the Blender renders the worlds come from.
float3 AgxContrast(float3 x)
{
	float3 x2 = x * x, x4 = x2 * x2;
	return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}
float3 AgX(float3 c)
{
	const float3x3 inset = float3x3(0.842479062253094, 0.0784335999999992, 0.0792237451477643,
	                                0.0423282422610123, 0.878468636469772, 0.0791661274605434,
	                                0.0423756549057051, 0.0784336, 0.879142973793104);
	const float3x3 outset = float3x3(1.19687900512017, -0.0980208811401368, -0.0990297440797205,
	                                 -0.0528968517574562, 1.15190312990417, -0.0989611768448433,
	                                 -0.0529716355144438, -0.0980434501171241, 1.15107367264116);
	const float minEv = -12.47393, maxEv = 4.026069;
	c = mul(inset, max(c, 1e-10));
	c = saturate((log2(c) - minEv) / (maxEv - minEv));
	c = AgxContrast(c);
	c = mul(outset, c);
	return pow(saturate(c), 2.2);   // display-referred, linearised again for the sRGB encode below
}

float4 TonemapPS(FsOut i) : SV_Target0
{
	float2 uv = (viewport.xy + i.uv * viewport.zw) * invTargetSize.xy;
	// sky portal maps: pixels the main view drew nothing on (depth 0) keep the portal view drawn before it. Only then:
	// an ordinary sky box writes no depth either, and discarding it left last frame's picture (hall-of-mirrors sky)
	if (extraParams.z > 0.5 && skyParams.w <= 0.5 && Tex2D[indices0.w].Load(int3(i.pos.xy, 0)).r <= 0.0) discard;
	float3 c = Sane(Tex2D[srcSrv].SampleLevel(sClampPoint, uv, 0).rgb) * Exposure();
	if (bloomSrv) c += Tex2D[bloomSrv].SampleLevel(sClamp, i.uv, 0).rgb * postParams.y;
	if (skyParams.w > 0.5) {
		c = extraParams.y > 0.5 ? AgX(c) : ACESFitted(c * 1.4);
		float2 v = i.uv - 0.5;
		c *= 1 - dot(v, v) * 0.2;
	}
	else c = saturate(saturate(c) * Overbright());   // legacy BSP maps: display-referred, plus the overbright shift the old gamma ramp did
	c = LinearToSrgb(c);
	float n = frac(sin(dot(i.pos.xy + indices2.w * 0.37, float2(12.9898, 78.233))) * 43758.5453);
	c += (n - 0.5) / 255.0;
	return float4(c, dot(c, float3(0.299, 0.587, 0.114)));
}

// FXAA (console quality) on the tone mapped image, luma in alpha
float4 FxaaPS(FsOut i) : SV_Target0
{
	float2 rcp = 1.0 / float2(dstSize);
	float2 uv = (viewport.xy + i.uv * viewport.zw) * rcp;
	// sky portal maps: pixels the main view drew nothing on (depth 0) keep the portal view drawn before it. Only then:
	// an ordinary sky box writes no depth either, and discarding it left last frame's picture (hall-of-mirrors sky)
	if (extraParams.z > 0.5 && skyParams.w <= 0.5 && Tex2D[indices0.w].Load(int3(i.pos.xy, 0)).r <= 0.0) discard;
	float4 m = Tex2D[srcSrv].SampleLevel(sClamp, uv, 0);
	float nw = Tex2D[srcSrv].SampleLevel(sClamp, uv + float2(-0.5, -0.5) * rcp, 0).a;
	float ne = Tex2D[srcSrv].SampleLevel(sClamp, uv + float2(0.5, -0.5) * rcp, 0).a;
	float sw = Tex2D[srcSrv].SampleLevel(sClamp, uv + float2(-0.5, 0.5) * rcp, 0).a;
	float se = Tex2D[srcSrv].SampleLevel(sClamp, uv + float2(0.5, 0.5) * rcp, 0).a;
	float lmax = max(max(nw, ne), max(sw, se)), lmin = min(min(nw, ne), min(sw, se));
	if (max(lmax, m.a) - min(lmin, m.a) < max(0.05, lmax * 0.125)) return float4(m.rgb, 1);
	float2 dir = float2(-((nw + ne) - (sw + se)), (nw + sw) - (ne + se));
	float red = max((nw + ne + sw + se) * 0.03125, 1.0 / 128.0);
	float rmin = 1.0 / (min(abs(dir.x), abs(dir.y)) + red);
	dir = clamp(dir * rmin, -8.0, 8.0) * rcp;
	float3 a = 0.5 * (Tex2D[srcSrv].SampleLevel(sClamp, uv - dir / 6, 0).rgb + Tex2D[srcSrv].SampleLevel(sClamp, uv + dir / 6, 0).rgb);
	float3 b = a * 0.5 + 0.25 * (Tex2D[srcSrv].SampleLevel(sClamp, uv - dir * 0.5, 0).rgb + Tex2D[srcSrv].SampleLevel(sClamp, uv + dir * 0.5, 0).rgb);
	float lb = dot(b, float3(0.299, 0.587, 0.114));
	return float4((lb < min(lmin, m.a) || lb > max(lmax, m.a)) ? a : b, 1);
}
