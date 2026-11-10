// KatarnEngine - reflection probe filtering: each mip of a captured cube is a wider cone of the level above it, so
// rougher surfaces read blurrier reflections (sampled across face edges, no seams)
#include "common.hlsli"

cbuffer Draw : register(b0)
{
	uint srcSrv;     // the level above, Texture2DArray of the six faces
	uint dstUav;     // this level, RWTexture2DArray
	uint dstSize;
	uint mip;
};

// D3D cube face conventions (face, s, t in -1..1) <-> direction
float3 FaceDir(uint face, float2 st)
{
	float s = st.x, t = st.y;
	if (face == 0) return float3(1, -t, -s);
	if (face == 1) return float3(-1, -t, s);
	if (face == 2) return float3(s, 1, t);
	if (face == 3) return float3(s, -1, -t);
	if (face == 4) return float3(s, -t, 1);
	return float3(-s, -t, -1);
}

float3 Fetch(float3 d)
{
	float3 a = abs(d);
	uint face; float ma, sc, tc;
	if (a.x >= a.y && a.x >= a.z) { face = d.x > 0 ? 0 : 1; ma = a.x; sc = d.x > 0 ? -d.z : d.z; tc = -d.y; }
	else if (a.y >= a.z) { face = d.y > 0 ? 2 : 3; ma = a.y; sc = d.x; tc = d.y > 0 ? d.z : -d.z; }
	else { face = d.z > 0 ? 4 : 5; ma = a.z; sc = d.z > 0 ? d.x : -d.x; tc = -d.y; }
	float2 uv = float2(sc, tc) / ma * 0.5 + 0.5;
	return Tex2DArray[srcSrv].SampleLevel(sClamp, float3(uv, face), 0).rgb;
}

[numthreads(8, 8, 1)]
void FilterCS(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= dstSize || id.y >= dstSize) return;
	float3 d = normalize(FaceDir(id.z, (id.xy + 0.5) / dstSize * 2.0 - 1.0));
	float3 t = normalize(cross(abs(d.z) < 0.99 ? float3(0, 0, 1) : float3(1, 0, 0), d));
	float3 b = cross(d, t);
	float r = 2.0 / dstSize * (0.6 + 0.12 * mip);   // a bit more than this texel's footprint
	float3 acc = 0;
	float wsum = 0;
	[loop] for (int j = 0; j < 5; j++)
		[loop] for (int i = 0; i < 5; i++) {
			float2 o = float2(i, j) * 0.5 - 1.0;
			float w = exp(-2.0 * dot(o, o));
			acc += min(Fetch(normalize(d + (t * o.x + b * o.y) * r)), 512.0) * w;
			wsum += w;
		}
	UavTexArray[dstUav][id] = float4(acc / wsum, 1);
}
