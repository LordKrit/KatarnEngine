/*[Vertex]*/
out vec2 var_ScreenTex;

void main()
{
	vec2 position = vec2(2.0 * float(gl_VertexID & 2) - 1.0, 4.0 * float(gl_VertexID & 1) - 1.0);
	gl_Position = vec4(position, 0.0, 1.0);
	var_ScreenTex = position * 0.5 + vec2(0.5);
}

/*[Fragment]*/
// Screen-space reflections for glossy floors (rend2 is forward-rendered, so there is no
// material buffer: the normal comes from the depth buffer and only upward-facing surfaces
// reflect). Output is the reflected light only; it is added on top of the scene.
uniform sampler2D u_ScreenImageMap;   // copy of the HDR scene
uniform sampler2D u_ScreenDepthMap;   // scene depth
uniform vec4 u_ViewInfo;              // znear, zfar, proj[0][0], proj[1][1]
uniform vec4 u_Color;                 // world up in view space (xyz), strength (w)
uniform vec4 u_Local0;                // max distance, thickness, steps, roughness blur
uniform vec4 u_Local1;                // roughness rejection (x)
uniform vec2 u_InvTexRes;

in vec2 var_ScreenTex;
out vec4 out_Color;

float LinearZ(vec2 uv)
{
	float d = texture(u_ScreenDepthMap, uv).r;
	return 1.0 / mix(1.0 / u_ViewInfo.x, 1.0 / u_ViewInfo.y, d);
}

// view space: x right, y up, z = distance in front of the camera
vec3 ViewPos(vec2 uv)
{
	float z = LinearZ(uv);
	vec2 ndc = uv * 2.0 - 1.0;
	return vec3(ndc.x * z / u_ViewInfo.z, ndc.y * z / u_ViewInfo.w, z);
}

vec2 Project(vec3 p)
{
	return vec2(p.x * u_ViewInfo.z, p.y * u_ViewInfo.w) / p.z * 0.5 + 0.5;
}

float Hash(vec2 p)
{
	return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

void main()
{
	vec2 uv = var_ScreenTex;
	float depth = texture(u_ScreenDepthMap, uv).r;
	if (depth >= 0.99999) { out_Color = vec4(0.0); return; }   // sky

	vec3 P = ViewPos(uv);
	// normal from the nearer neighbour on each axis (avoids smearing across edges)
	vec3 Pr = ViewPos(uv + vec2(u_InvTexRes.x, 0.0)), Pl = ViewPos(uv - vec2(u_InvTexRes.x, 0.0));
	vec3 Pu = ViewPos(uv + vec2(0.0, u_InvTexRes.y)), Pd = ViewPos(uv - vec2(0.0, u_InvTexRes.y));
	vec3 dx = abs(Pr.z - P.z) < abs(P.z - Pl.z) ? Pr - P : P - Pl;
	vec3 dy = abs(Pu.z - P.z) < abs(P.z - Pd.z) ? Pu - P : P - Pd;
	vec3 N = normalize(cross(dy, dx));
	vec3 V = normalize(P);
	if (dot(N, V) > 0.0) N = -N;

	// only floors (and gently sloped ground) reflect
	float floorMask = smoothstep(0.80, 0.97, dot(N, normalize(u_Color.xyz)));
	if (floorMask <= 0.0) { out_Color = vec4(0.0); return; }

	vec3 R = reflect(V, N);

	float maxDist = u_Local0.x;
	int steps = int(u_Local0.z);
	float stepLen = maxDist / float(steps);
	float jitter = Hash(uv * 1024.0);
	vec3 ray = P + N * (0.5 + 0.002 * P.z);
	vec2 hitUV = vec2(-1.0);
	float travelled = 0.0;

	for (int i = 0; i < 96; i++)
	{
		if (i >= steps) break;
		// steps grow with distance so far reflections stay cheap
		float s = stepLen * (0.5 + float(i) / float(steps)) * (1.0 + jitter * 0.5);
		ray += R * s;
		travelled += s;
		if (ray.z <= u_ViewInfo.x) break;
		vec2 suv = Project(ray);
		if (any(lessThan(suv, vec2(0.0))) || any(greaterThan(suv, vec2(1.0)))) break;
		float sceneZ = LinearZ(suv);
		float dz = ray.z - sceneZ;
		if (dz > 0.0 && dz < u_Local0.y + s * 1.5)
		{
			// refine with a short binary search
			vec3 a = ray - R * s, b = ray;
			for (int j = 0; j < 5; j++)
			{
				vec3 m = (a + b) * 0.5;
				vec2 muv = Project(m);
				if (m.z - LinearZ(muv) > 0.0) b = m; else a = m;
			}
			hitUV = Project(b);
			break;
		}
	}

	if (hitUV.x < 0.0) { out_Color = vec4(0.0); return; }

	// fade near the screen border, with distance travelled, and for rays facing the camera
	vec2 edge = smoothstep(vec2(0.0), vec2(0.12), hitUV) * smoothstep(vec2(0.0), vec2(0.12), 1.0 - hitUV);
	float fade = edge.x * edge.y * (1.0 - smoothstep(0.6, 1.0, travelled / maxDist)) * smoothstep(-0.2, 0.15, R.z);

	// cheap gloss: average a few taps around the hit, wider for longer rays
	float blur = u_Local0.w * (0.5 + travelled / maxDist);
	vec3 col = texture(u_ScreenImageMap, hitUV).rgb * 0.4;
	col += texture(u_ScreenImageMap, hitUV + vec2( blur, 0.0) * u_InvTexRes * 4.0).rgb * 0.15;
	col += texture(u_ScreenImageMap, hitUV + vec2(-blur, 0.0) * u_InvTexRes * 4.0).rgb * 0.15;
	col += texture(u_ScreenImageMap, hitUV + vec2(0.0,  blur) * u_InvTexRes * 4.0).rgb * 0.15;
	col += texture(u_ScreenImageMap, hitUV + vec2(0.0, -blur) * u_InvTexRes * 4.0).rgb * 0.15;

	// rough surfaces (rock, carpet, grating) have busy textures: measure local contrast of the
	// floor around this pixel and fade the reflection out on high-contrast ones
	vec2 px = u_InvTexRes * 1.5;
	float l0 = dot(texture(u_ScreenImageMap, uv).rgb, vec3(0.333));
	float sum = 0.0, sum2 = 0.0;
	for (int k = 0; k < 8; k++)
	{
		float a = float(k) * 0.785398;
		vec2 o = vec2(cos(a), sin(a)) * px * (1.0 + float(k & 1));
		float l = dot(texture(u_ScreenImageMap, uv + o).rgb, vec3(0.333));
		sum += l; sum2 += l * l;
	}
	float mean = sum / 8.0;
	float stdev = sqrt(max(sum2 / 8.0 - mean * mean, 0.0));
	float busy = (stdev + abs(l0 - mean)) / max(mean, 0.02);
	float glossy = 1.0 - smoothstep(0.06, 0.06 + 0.25 / max(u_Local1.x, 0.05), busy);

	// Schlick fresnel: polished stone / metal floor
	float NdotV = clamp(dot(N, -V), 0.0, 1.0);
	float fresnel = 0.25 + 0.75 * pow(1.0 - NdotV, 4.0);

	out_Color = vec4(col * fresnel * fade * floorMask * glossy * u_Color.w, 1.0);
}
