// KatarnEngine - shared HLSL (shader model 5.1, bindless descriptor tables)
#ifndef KT_COMMON
#define KT_COMMON

Texture2D Tex2D[] : register(t0, space1);
TextureCube TexCube[] : register(t0, space2);
Texture2DArray Tex2DArray[] : register(t0, space3);
ByteAddressBuffer Buf[] : register(t0, space4);
Texture3D Tex3D[] : register(t0, space5);
RWTexture2D<float4> UavTex[] : register(u0, space1);
RWByteAddressBuffer UavBuf[] : register(u0, space2);
RWTexture2DArray<float4> UavTexArray[] : register(u0, space3);

#ifdef KT_GLSLANG   // offline syntax check with glslang, which lacks arrays of byte-address buffers
#define NonUniformResourceIndex(x) (x)
ByteAddressBuffer BufOne : register(t1, space0);
RWByteAddressBuffer UavBufOne : register(u1, space0);
#define BUF(i) BufOne
#define UAVBUF(i) UavBufOne
#else
#define BUF(i) Buf[NonUniformResourceIndex(i)]
#define UAVBUF(i) UavBuf[NonUniformResourceIndex(i)]
#endif

SamplerState sWrap : register(s0);
SamplerState sClamp : register(s1);
SamplerState sWrapPoint : register(s2);
SamplerState sClampPoint : register(s3);
SamplerComparisonState sShadow : register(s4);
SamplerState sClampAniso : register(s5);

static const float PI = 3.14159265359;

// per-view constants (root CBV b1) - must match kt::ViewConstants
cbuffer View : register(b1)
{
	float4x4 viewProj;         // world -> clip (reversed-Z, infinite)
	float4x4 invViewProj;
	float4x4 worldToEye;       // GL eye space (-Z forward)
	float4x4 prevViewProj;
	float4 camPos;             // xyz, w = near plane
	float4 viewport;           // x, y (top-left, pixels), w, h
	float4 invTargetSize;      // 1/w, 1/h of the full-size scene targets, w,h
	float4 projParams;         // sx, sy (1/tan half fov), near, time
	float4 sunDir;             // towards the sun, w = sun angular radius
	float4 sunColor;           // linear radiance (rgb), w = intensity scale
	float4 skyParams;          // x turbidity / haze, y sky intensity, z ambient scale, w world mode (1 = HD world)
	float4 fogParams;          // x density, y height falloff, z base height, w start distance
	float4 fogColor;           // rgb inscatter colour, w max opacity
	float4x4 cascadeViewProj[4];
	float4 cascadeSplits;      // view depth of each cascade's far end
	float4 shadowParams;       // x texel size, y bias, z normal bias, w enabled
	uint4 indices0;            // x gbuffer0, y gbuffer1, z gbuffer2, w depth (SRV)
	uint4 indices1;            // x shadow array, y ao, z sky LUT, w exposure buffer
	uint4 indices2;            // x light buffer, y light count, z hdr srv, w frame
	float4 postParams;         // x exposure compensation (EV), y bloom intensity, z manual exposure (0 = auto), w legacy glow boost
	float4 ambientSH[9];       // sky irradiance SH (rgb)
	float4 extraParams;        // x overbright scale (2^r_overBrightBits, applied when the frame is presented)
	float4x4 viewProjNoJitter; // TAA: this frame without the jitter
};
float3 Sane(float3 c) { c = isnan(c) ? 0 : c; return clamp(c, 0, 32000.0); }   // NaN -> 0, overflow (inf) -> bright, never black
float Overbright() { return max(extraParams.x, 1.0); }

float3 SrgbToLinear(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float3 LinearToSrgb(float3 c) { c = max(c, 0); return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }
float Luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

float2 OctWrap(float2 v) { return (1.0 - abs(v.yx)) * (v.xy >= 0.0 ? 1.0 : -1.0); }
float2 EncodeNormal(float3 n)
{
	n /= (abs(n.x) + abs(n.y) + abs(n.z));
	n.xy = n.z >= 0.0 ? n.xy : OctWrap(n.xy);
	return n.xy;
}
float3 DecodeNormal(float2 f)
{
	float3 n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
	float t = saturate(-n.z);
	n.xy += n.xy >= 0.0 ? -t : t;
	return normalize(n);
}

float3 WorldFromDepth(float2 uv, float depth)
{
	float4 clip = float4(uv * float2(2, -2) + float2(-1, 1), depth, 1);
	float4 w = mul(invViewProj, clip);
	return w.xyz / w.w;
}
float ViewDepth(float depth) { return projParams.z / max(depth, 1e-7); }   // reversed-Z infinite: near / depth

float Exposure()
{
	return asfloat(BUF(indices1.w).Load(0));
}

float3 EvalSH(float3 n)
{
	float3 r = ambientSH[0].rgb * 0.282095;
	r += ambientSH[1].rgb * 0.488603 * n.y + ambientSH[2].rgb * 0.488603 * n.z + ambientSH[3].rgb * 0.488603 * n.x;
	r += ambientSH[4].rgb * 1.092548 * n.x * n.y + ambientSH[5].rgb * 1.092548 * n.y * n.z;
	r += ambientSH[6].rgb * 0.315392 * (3.0 * n.z * n.z - 1.0);
	r += ambientSH[7].rgb * 1.092548 * n.x * n.z + ambientSH[8].rgb * 0.546274 * (n.x * n.x - n.y * n.y);
	return max(r, 0);
}

// fullscreen triangle
struct FsOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
FsOut FullscreenVS(uint id : SV_VertexID)
{
	FsOut o;
	o.uv = float2((id << 1) & 2, id & 2);
	o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
	return o;
}

#endif
