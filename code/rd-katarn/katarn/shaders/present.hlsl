// KatarnEngine - final copy of the screen target to the swap chain (gamma like r_gamma did with hardware ramps)
#include "common.hlsli"

cbuffer Draw : register(b0)
{
	uint srcTex;
	float gamma;
	float brightness;
	uint presentPad;
};

float4 PS(FsOut i) : SV_Target0
{
	float3 c = Tex2D[srcTex].SampleLevel(sClampPoint, i.uv, 0).rgb;
	c = pow(saturate(c * brightness), 1.0 / gamma);
	return float4(c, 1);
}
