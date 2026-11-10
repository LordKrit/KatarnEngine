// KatarnEngine - physically based sky: single-scattering Rayleigh + Mie into a lat-long LUT, and its irradiance as SH
#include "common.hlsli"

cbuffer Draw : register(b0)
{
	uint lutUav;
	uint lutSrv;
	uint shUav;
	uint lutW;
	uint lutH;
	float groundAlbedo;
	uint skyPad0;
	uint skyPad1;
};

static const float Rg = 6360.0, Rt = 6460.0;
static const float3 BetaR = float3(5.802e-3, 13.558e-3, 33.1e-3);
static const float BetaM = 3.996e-3, BetaMExt = 4.44e-3;
static const float HR = 8.0, HM = 1.2;

float2 RaySphere(float3 o, float3 d, float r)
{
	float b = dot(o, d), c = dot(o, o) - r * r, h = b * b - c;
	if (h < 0) return float2(-1, -1);
	h = sqrt(h);
	return float2(-b - h, -b + h);
}

float3 Transmittance(float3 p, float3 l)
{
	float2 t = RaySphere(p, l, Rt);
	float len = t.y;
	float2 od = 0;
	const int N = 8;
	float ds = len / N;
	for (int i = 0; i < N; i++) {
		float h = length(p + l * ((i + 0.5) * ds)) - Rg;
		od += exp(-float2(h / HR, h / HM)) * ds;
	}
	return exp(-(BetaR * od.x + BetaMExt * od.y * (1.0 + skyParams.x)));
}

float3 Atmosphere(float3 dir)
{
	float3 L = sunDir.xyz;
	float3 o = float3(0, 0, Rg + 0.25);
	float2 ta = RaySphere(o, dir, Rt);
	float2 tg = RaySphere(o, dir, Rg);
	bool ground = tg.x > 0;
	float len = ground ? tg.x : ta.y;
	len = min(len, 400.0);
	const int N = 24;
	float ds = len / N;
	float3 sumR = 0, sumM = 0;
	float2 od = 0;
	for (int i = 0; i < N; i++) {
		float3 p = o + dir * ((i + 0.5) * ds);
		float h = length(p) - Rg;
		float2 dens = exp(-float2(h / HR, h / HM)) * ds;
		od += dens;
		float3 tv = exp(-(BetaR * od.x + BetaMExt * od.y * (1.0 + skyParams.x)));
		// sun visible from p?
		float2 tgl = RaySphere(p, L, Rg);
		if (tgl.x > 0) continue;
		float3 tl = Transmittance(p, L);
		sumR += dens.x * tv * tl;
		sumM += dens.y * tv * tl;
	}
	float mu = dot(dir, L);
	float pr = 3.0 / (16.0 * PI) * (1.0 + mu * mu);
	float g = 0.76;
	float pm = 3.0 / (8.0 * PI) * ((1.0 - g * g) * (1.0 + mu * mu)) / ((2.0 + g * g) * pow(abs(1.0 + g * g - 2.0 * g * mu), 1.5));
	float3 c = sumR * BetaR * pr + sumM * BetaM * (1.0 + skyParams.x) * pm;
	if (ground) {
		float3 p = o + dir * tg.x;
		float3 n = normalize(p);
		float3 tv = exp(-(BetaR * od.x + BetaMExt * od.y));
		c += tv * Transmittance(p, L) * saturate(dot(n, L)) * groundAlbedo / PI;
	}
	return c * sunColor.rgb * sunColor.w * skyParams.y;
}

float3 LutDir(float2 uv)
{
	float phi = (uv.x - 0.5) * 2 * PI;
	float el = (0.5 - uv.y) * PI;
	return float3(cos(el) * cos(phi), cos(el) * sin(phi), sin(el));
}

[numthreads(8, 8, 1)]
void LutCS(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= lutW || id.y >= lutH) return;
	float2 uv = (id.xy + 0.5) / float2(lutW, lutH);
	UavTex[lutUav][id.xy] = float4(Atmosphere(LutDir(uv)), 1);
}

groupshared float3 shAcc[256][9];

[numthreads(256, 1, 1)]
void ShCS(uint3 gt : SV_GroupThreadID)
{
	float3 acc[9];
	for (int k = 0; k < 9; k++) acc[k] = 0;
	uint total = lutW * lutH;
	for (uint i = gt.x; i < total; i += 256) {
		uint2 p = uint2(i % lutW, i / lutW);
		float2 uv = (p + 0.5) / float2(lutW, lutH);
		float3 d = LutDir(uv);
		float el = (0.5 - uv.y) * PI;
		float w = cos(el) * (2 * PI / lutW) * (PI / lutH);
		float3 c = Tex2D[lutSrv].Load(int3(p, 0)).rgb * w;
		acc[0] += c * 0.282095;
		acc[1] += c * 0.488603 * d.y; acc[2] += c * 0.488603 * d.z; acc[3] += c * 0.488603 * d.x;
		acc[4] += c * 1.092548 * d.x * d.y; acc[5] += c * 1.092548 * d.y * d.z;
		acc[6] += c * 0.315392 * (3 * d.z * d.z - 1);
		acc[7] += c * 1.092548 * d.x * d.z; acc[8] += c * 0.546274 * (d.x * d.x - d.y * d.y);
	}
	for (int k2 = 0; k2 < 9; k2++) shAcc[gt.x][k2] = acc[k2];
	GroupMemoryBarrierWithGroupSync();
	for (uint s = 128; s > 0; s >>= 1) {
		if (gt.x < s) for (int k3 = 0; k3 < 9; k3++) shAcc[gt.x][k3] += shAcc[gt.x + s][k3];
		GroupMemoryBarrierWithGroupSync();
	}
	if (gt.x == 0) {
		// convolve with the clamped cosine lobe -> irradiance / PI (diffuse radiance)
		static const float A[9] = { 1.0, 2.0 / 3.0, 2.0 / 3.0, 2.0 / 3.0, 0.25, 0.25, 0.25, 0.25, 0.25 };
		for (int k4 = 0; k4 < 9; k4++) UAVBUF(shUav).Store4(k4 * 16, asuint(float4(shAcc[0][k4] * A[k4], 0)));
	}
}
