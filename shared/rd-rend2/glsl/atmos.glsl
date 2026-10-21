/*[Vertex]*/
out vec2 var_ScreenTex;

void main()
{
	vec2 position = vec2(2.0 * float(gl_VertexID & 2) - 1.0, 4.0 * float(gl_VertexID & 1) - 1.0);
	gl_Position = vec4(position, 0.0, 1.0);
	var_ScreenTex = position * 0.5 + vec2(0.5);
}

/*[Fragment]*/
// Atmosphere: distance + height haze lit by the sun, and screen-space sun shafts.
// Output rgb = in-scattered light, a = how much of the scene it hides; blended
// with src alpha / one minus src alpha... except the shafts, which are added (a = 0 part).
uniform sampler2D u_ScreenImageMap;   // copy of the HDR scene
uniform sampler2D u_ScreenDepthMap;
uniform vec4 u_ViewInfo;              // znear, zfar, proj[0][0], proj[1][1]
uniform vec4 u_Color;                 // haze colour (rgb), density (a)
uniform vec4 u_Local0;                // world up in view space (xyz), camera height (w)
uniform vec4 u_Local1;                // sun dir in view space (xyz), shaft strength (w)
uniform vec4 u_Local2;                // sun colour (rgb), height falloff (a)
uniform vec4 u_Local3;                // sun screen pos (xy), sun visible (z), haze start (w)

in vec2 var_ScreenTex;
out vec4 out_Color;

float LinearZ(vec2 uv)
{
	float d = texture(u_ScreenDepthMap, uv).r;
	return 1.0 / mix(1.0 / u_ViewInfo.x, 1.0 / u_ViewInfo.y, d);
}

void main()
{
	vec2 uv = var_ScreenTex;
	float d = texture(u_ScreenDepthMap, uv).r;
	bool sky = d >= 0.99999;
	float z = sky ? u_ViewInfo.y : LinearZ(uv);
	vec2 ndc = uv * 2.0 - 1.0;
	vec3 P = vec3(ndc.x * z / u_ViewInfo.z, ndc.y * z / u_ViewInfo.w, z);
	float dist = length(P);
	vec3 V = P / dist;
	vec3 up = normalize(u_Local0.xyz);

	// --- haze: exponential height fog integrated along the view ray
	float density = u_Color.a;
	float falloff = max(u_Local2.a, 1e-5);
	float camH = u_Local0.w;
	float rayUp = dot(V, up);
	float travel = max(dist - u_Local3.w, 0.0);
	float h0 = camH;                      // height relative to the haze base (world z = 0 here, shifted on the CPU)
	float fogAmt;
	if (abs(rayUp) > 1e-3)
		fogAmt = density * exp(-falloff * h0) * (1.0 - exp(-falloff * rayUp * travel)) / (falloff * rayUp);
	else
		fogAmt = density * exp(-falloff * h0) * travel;
	float hide = sky ? 0.0 : clamp(1.0 - exp(-fogAmt), 0.0, 0.85);

	// sun forward scattering (Henyey-Greenstein-ish) tints the haze toward the sun
	float cosSun = dot(V, normalize(u_Local1.xyz));
	float g = 0.6;
	float hg = (1.0 - g * g) / pow(1.0 + g * g - 2.0 * g * cosSun, 1.5) * 0.08;
	vec3 haze = u_Color.rgb + u_Local2.rgb * hg;

	// --- sun shafts: march toward the sun on screen, collecting sky that is visible
	vec3 shafts = vec3(0.0);
	if (u_Local1.w > 0.0 && u_Local3.z > 0.0)
	{
		vec2 toSun = u_Local3.xy - uv;
		const int N = 40;
		vec2 stepUV = toSun / float(N) * 0.9;
		vec2 s = uv;
		float decay = 1.0, acc = 0.0;
		for (int i = 0; i < N; i++)
		{
			s += stepUV;
			if (any(lessThan(s, vec2(0.0))) || any(greaterThan(s, vec2(1.0)))) break;
			float sd = texture(u_ScreenDepthMap, s).r;
			float lum = dot(texture(u_ScreenImageMap, s).rgb, vec3(0.2126, 0.7152, 0.0722));
			acc += (sd >= 0.99999 ? min(lum, 4.0) : 0.0) * decay;
			decay *= 0.96;
		}
		float facing = smoothstep(-0.1, 0.6, cosSun);
		shafts = u_Local2.rgb * acc / float(N) * u_Local1.w * facing * u_Local3.z;
	}

	// premultiplied: result = scene * (1 - hide) + haze * hide + shafts
	out_Color = vec4(haze * hide + shafts, hide);
}
