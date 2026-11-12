// katarn_cook: baked lighting for kworld v3 (included by katarn_cook.cpp)
//
// Every vertex gets the light it receives, as KatarnEngine would light it but frozen: the sun with its shadow,
// the sky (the world's SH irradiance) times how much of the sky the vertex sees, the placed lights. The result is
// the VCOL section (what rend2 multiplies the textures by: "rgbGen vertex") and a light grid (LGRD) for players
// and other models. Rays are traced against a coarse LOD of the opaque world (BVH, all cores).

struct BakeTri { float a[3], e1[3], e2[3]; };
struct BvhNode { float mn[3], mx[3]; uint32_t left, count; };   // count > 0: leaf (left = first triangle)

struct BakeBVH {
	std::vector<BakeTri> tris;
	std::vector<BvhNode> nodes;

	void build()
	{
		size_t n = tris.size();
		std::vector<uint32_t> ids(n);
		std::vector<float> cen(n * 3);
		for (size_t i = 0; i < n; i++) {
			ids[i] = (uint32_t)i;
			for (int k = 0; k < 3; k++) cen[i * 3 + k] = tris[i].a[k] + (tris[i].e1[k] + tris[i].e2[k]) / 3.0f;
		}
		nodes.clear();
		nodes.reserve(n / 2 + 16);
		struct Job { uint32_t node, first, count; };
		std::vector<Job> stack;
		nodes.push_back({});
		stack.push_back({ 0, 0, (uint32_t)n });
		while (!stack.empty()) {
			Job j = stack.back(); stack.pop_back();
			BvhNode& nd = nodes[j.node];
			float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f }, cmn[3] = { 1e30f, 1e30f, 1e30f }, cmx[3] = { -1e30f, -1e30f, -1e30f };
			for (uint32_t i = j.first; i < j.first + j.count; i++) {
				const BakeTri& t = tris[ids[i]];
				for (int k = 0; k < 3; k++) {
					float p0 = t.a[k], p1 = t.a[k] + t.e1[k], p2 = t.a[k] + t.e2[k];
					mn[k] = std::min(mn[k], std::min(p0, std::min(p1, p2)));
					mx[k] = std::max(mx[k], std::max(p0, std::max(p1, p2)));
					cmn[k] = std::min(cmn[k], cen[ids[i] * 3 + k]); cmx[k] = std::max(cmx[k], cen[ids[i] * 3 + k]);
				}
			}
			memcpy(nd.mn, mn, 12); memcpy(nd.mx, mx, 12);
			int ax = 0;
			for (int k = 1; k < 3; k++) if (cmx[k] - cmn[k] > cmx[ax] - cmn[ax]) ax = k;
			if (j.count <= 4 || cmx[ax] - cmn[ax] < 1e-3f) { nd.left = j.first; nd.count = j.count; continue; }
			uint32_t mid = j.first + j.count / 2;
			std::nth_element(ids.begin() + j.first, ids.begin() + mid, ids.begin() + j.first + j.count,
				[&](uint32_t x, uint32_t y) { return cen[x * 3 + ax] < cen[y * 3 + ax]; });
			uint32_t l = (uint32_t)nodes.size();
			nodes[j.node].left = l; nodes[j.node].count = 0;
			nodes.push_back({}); nodes.push_back({});
			stack.push_back({ l, j.first, mid - j.first });
			stack.push_back({ l + 1, mid, j.first + j.count - mid });
		}
		std::vector<BakeTri> sorted(n);
		for (size_t i = 0; i < n; i++) sorted[i] = tris[ids[i]];
		tris.swap(sorted);
	}

	// any hit within (0, tmax)
	bool occluded(const float* o, const float* d, float tmax) const
	{
		if (nodes.empty()) return false;
		float inv[3] = { 1.0f / (fabsf(d[0]) > 1e-9f ? d[0] : 1e-9f), 1.0f / (fabsf(d[1]) > 1e-9f ? d[1] : 1e-9f), 1.0f / (fabsf(d[2]) > 1e-9f ? d[2] : 1e-9f) };
		uint32_t stack[64]; int sp = 0;
		stack[sp++] = 0;
		while (sp) {
			const BvhNode& nd = nodes[stack[--sp]];
			float t0 = 0, t1 = tmax;
			bool miss = false;
			for (int k = 0; k < 3 && !miss; k++) {
				float a = (nd.mn[k] - o[k]) * inv[k], b = (nd.mx[k] - o[k]) * inv[k];
				if (a > b) std::swap(a, b);
				t0 = std::max(t0, a); t1 = std::min(t1, b);
				miss = t0 > t1;
			}
			if (miss) continue;
			if (nd.count) {
				for (uint32_t i = nd.left; i < nd.left + nd.count; i++) {
					const BakeTri& t = tris[i];
					float p[3] = { d[1] * t.e2[2] - d[2] * t.e2[1], d[2] * t.e2[0] - d[0] * t.e2[2], d[0] * t.e2[1] - d[1] * t.e2[0] };
					float det = t.e1[0] * p[0] + t.e1[1] * p[1] + t.e1[2] * p[2];
					if (fabsf(det) < 1e-10f) continue;
					float id = 1.0f / det;
					float s[3] = { o[0] - t.a[0], o[1] - t.a[1], o[2] - t.a[2] };
					float u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * id;
					if (u < 0 || u > 1) continue;
					float q[3] = { s[1] * t.e1[2] - s[2] * t.e1[1], s[2] * t.e1[0] - s[0] * t.e1[2], s[0] * t.e1[1] - s[1] * t.e1[0] };
					float v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * id;
					if (v < 0 || u + v > 1) continue;
					float tt = (t.e2[0] * q[0] + t.e2[1] * q[1] + t.e2[2] * q[2]) * id;
					if (tt > 0.01f && tt < tmax) return true;
				}
			}
			else if (sp < 62) { stack[sp++] = nd.left; stack[sp++] = nd.left + 1; }
		}
		return false;
	}
};

static float g_sh[9][3];
static bool g_haveSH = false;
static BakeImage g_envImg;
static float g_envStrength = 1.0f;
static int BAKE_SKY_RAYS = 8;
static bool BAKE_OFF = false;

static void OctDecode(const int16_t* o, float* n)
{
	float x = o[0] / 32767.0f, y = o[1] / 32767.0f, z = 1.0f - fabsf(x) - fabsf(y);
	if (z < 0) { float tx = (1 - fabsf(y)) * (x >= 0 ? 1.0f : -1.0f), ty = (1 - fabsf(x)) * (y >= 0 ? 1.0f : -1.0f); x = tx; y = ty; }
	float l = sqrtf(x * x + y * y + z * z);
	n[0] = x / l; n[1] = y / l; n[2] = z / l;
}
static void SHIrradiance(const float* n, float* out)
{
	float b[9] = { 0.282095f, 0.488603f * n[1], 0.488603f * n[2], 0.488603f * n[0], 1.092548f * n[0] * n[1], 1.092548f * n[1] * n[2],
		0.315392f * (3 * n[2] * n[2] - 1), 1.092548f * n[0] * n[2], 0.546274f * (n[0] * n[0] - n[1] * n[1]) };
	for (int c = 0; c < 3; c++) {
		float s = 0;
		for (int k = 0; k < 9; k++) s += g_sh[k][c] * b[k];
		out[c] = std::max(0.0f, s);
	}
}
static inline uint8_t EncLight(float v) { return (uint8_t)std::min(255.0f, sqrtf(std::max(0.0f, v) * 0.25f) * 255.0f + 0.5f); }

// a fixed set of cosine-weighted directions around +Z, rotated per vertex
static void HemiDir(int i, int n, float rot, float* d)
{
	float u = (i + 0.5f) / n, phi = 2.39996323f * i + rot;
	float r = sqrtf(u);
	d[0] = r * cosf(phi); d[1] = r * sinf(phi); d[2] = sqrtf(std::max(0.0f, 1 - u));
}
static void Basis(const float* n, float* t, float* b)
{
	if (fabsf(n[2]) < 0.9f) { t[0] = -n[1]; t[1] = n[0]; t[2] = 0; }
	else { t[0] = 0; t[1] = -n[2]; t[2] = n[1]; }
	float l = sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
	t[0] /= l; t[1] /= l; t[2] /= l;
	b[0] = n[1] * t[2] - n[2] * t[1]; b[1] = n[2] * t[0] - n[0] * t[2]; b[2] = n[0] * t[1] - n[1] * t[0];
}

struct BakeOut {
	std::vector<uint32_t> vcol;
	KwGridHeader grid = {};
	std::vector<KwGridPoint> points;
};

static void BakeLighting(const std::vector<KwCluster>& clusters, const std::vector<KwLod>& lods, const std::vector<Bucket*>& cbucket,
	const std::vector<KwLight>& lights, const float* sunDir, const float* sunColor, float sunStrength,
	const float* cmin, const float* cmax, uint64_t vCount, BakeOut& out)
{
	double t0 = Now();
	// ---- occluders: every opaque / masked cluster at a coarse LOD (error up to 12 units)
	BakeBVH bvh;
	for (size_t ci = 0; ci < clusters.size(); ci++) {
		const KwCluster& c = clusters[ci];
		if (c.cls == KWC_FX) continue;
		uint32_t pick = 0;
		for (uint32_t l = 0; l < c.numLods; l++) { if (lods[c.firstLod + l].error > 12.0f) break; pick = l; }
		const KwLod& L = lods[c.firstLod + pick];
		std::vector<KwVertex> v(c.numVertices);
		std::vector<uint32_t> idx(L.indexCount);
		SpillRead(cbucket[ci]->vAt, v.data(), v.size() * sizeof(KwVertex));
		SpillRead(cbucket[ci]->iAt + (uint64_t)(L.firstIndex - lods[c.firstLod].firstIndex) * 4, idx.data(), idx.size() * 4);
		for (size_t t = 0; t + 2 < idx.size(); t += 3) {
			const float* a = v[idx[t]].pos, * b = v[idx[t + 1]].pos, * d = v[idx[t + 2]].pos;
			BakeTri bt;
			for (int k = 0; k < 3; k++) { bt.a[k] = a[k]; bt.e1[k] = b[k] - a[k]; bt.e2[k] = d[k] - a[k]; }
			bvh.tris.push_back(bt);
		}
	}
	bvh.build();
	printf("  bake: %zu occluder triangles, BVH %zu nodes (%.0fs)\n", bvh.tris.size(), bvh.nodes.size(), Now() - t0);
	fflush(stdout);

	// ---- exposure: an upward surface in full sun = 1 (the texture as painted)
	float sunE[3] = { sunColor[0] * sunStrength, sunColor[1] * sunStrength, sunColor[2] * sunStrength };
	float up[3] = { 0, 0, 1 }, skyUp[3] = { 0, 0, 0 };
	if (g_haveSH) SHIrradiance(up, skyUp);
	else { skyUp[0] = 0.25f * sunStrength; skyUp[1] = 0.28f * sunStrength; skyUp[2] = 0.33f * sunStrength; }
	float ref = std::max(sunE[0] * std::max(0.0f, sunDir[2]) + skyUp[0], std::max(sunE[1] * std::max(0.0f, sunDir[2]) + skyUp[1], sunE[2] * std::max(0.0f, sunDir[2]) + skyUp[2]));
	float expo = 0.9f / std::max(1e-4f, ref);

	unsigned nth = std::max(1u, std::thread::hardware_concurrency());
	auto skyLight = [&](const float* p, const float* n, float rot, float* E, float& vis) {
		float t[3], b[3];
		Basis(n, t, b);
		int open = 0;
		for (int i = 0; i < BAKE_SKY_RAYS; i++) {
			float h[3]; HemiDir(i, BAKE_SKY_RAYS, rot, h);
			float d[3] = { t[0] * h[0] + b[0] * h[1] + n[0] * h[2], t[1] * h[0] + b[1] * h[1] + n[1] * h[2], t[2] * h[0] + b[2] * h[1] + n[2] * h[2] };
			if (!bvh.occluded(p, d, 3072.0f)) open++;
		}
		vis = BAKE_SKY_RAYS ? open / (float)BAKE_SKY_RAYS : 1.0f;
		float sk[3];
		if (g_haveSH) SHIrradiance(n, sk);
		else { float f = 0.5f + 0.5f * n[2]; sk[0] = skyUp[0] * f; sk[1] = skyUp[1] * f; sk[2] = skyUp[2] * f; }
		// light that does not reach the sky still bounces: a little fill, tinted by the sun
		for (int c = 0; c < 3; c++) E[c] = sk[c] * (0.15f + 0.85f * vis) + sunE[c] * std::max(0.0f, sunDir[2]) * 0.06f * (1 - vis);
	};
	auto sunVis = [&](const float* p) { return (sunDir[2] > -0.05f && !bvh.occluded(p, sunDir, 65536.0f)) ? 1.0f : 0.0f; };
	auto pointLight = [&](const KwLight& L, const float* p, const float* n, float* E, bool shadow) {
		float d[3] = { L.pos[0] - p[0], L.pos[1] - p[1], L.pos[2] - p[2] };
		float d2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
		if (d2 >= L.radius * L.radius) return;
		float dl = sqrtf(d2), Ld[3] = { d[0] / dl, d[1] / dl, d[2] / dl };
		float nl = n ? Ld[0] * n[0] + Ld[1] * n[1] + Ld[2] * n[2] : 0.6f;
		if (nl <= 0) return;
		float r4 = (d2 * d2) / (L.radius * L.radius * L.radius * L.radius), w = std::max(0.0f, 1 - r4); w *= w;
		float att = L.invSquare > 0 ? w * L.invSquare / std::max(d2, 64.0f) : w;
		if (L.type == 1) {
			float cs = -(Ld[0] * L.dir[0] + Ld[1] * L.dir[1] + Ld[2] * L.dir[2]);
			float x = std::min(1.0f, std::max(0.0f, (cs - L.spotCos) / std::max(1e-4f, L.spotInnerCos - L.spotCos)));
			att *= x * x * (3 - 2 * x);
		}
		float c = att * nl;
		if (std::max(L.color[0], std::max(L.color[1], L.color[2])) * c * expo < 0.004f) return;
		if (shadow && bvh.occluded(p, Ld, dl - 4.0f)) return;
		for (int k = 0; k < 3; k++) E[k] += L.color[k] * c;
	};

	// ---- vertices
	out.vcol.assign((size_t)vCount, 0xFF000000u);
	std::atomic<size_t> next(0), doneV(0);
	auto worker = [&]() {
		std::vector<KwVertex> v;
		std::vector<int> nearLights;
		for (;;) {
			size_t ci = next++;
			if (ci >= clusters.size()) break;
			const KwCluster& c = clusters[ci];
			nearLights.clear();
			for (size_t li = 0; li < lights.size(); li++) {
				const KwLight& L = lights[li];
				bool in = true;
				for (int k = 0; k < 3; k++) if (L.pos[k] + L.radius < c.boundsMin[k] || L.pos[k] - L.radius > c.boundsMax[k]) in = false;
				if (in) nearLights.push_back((int)li);
			}
			v.resize(c.numVertices);
			SpillRead(cbucket[ci]->vAt, v.data(), v.size() * sizeof(KwVertex));
			for (uint32_t i = 0; i < c.numVertices; i++) {
				float n[3]; OctDecode(v[i].normal, n);
				float p[3] = { v[i].pos[0] + n[0] * 2.0f, v[i].pos[1] + n[1] * 2.0f, v[i].pos[2] + n[2] * 2.0f };
				float E[3], vis;
				uint32_t hsh = (uint32_t)(v[i].pos[0] * 73.0f) * 2654435761u ^ (uint32_t)(v[i].pos[1] * 31.0f) * 40503u ^ (uint32_t)(v[i].pos[2] * 17.0f);
				skyLight(p, n, (hsh & 1023) * (6.2831853f / 1024.0f), E, vis);
				float nl = n[0] * sunDir[0] + n[1] * sunDir[1] + n[2] * sunDir[2];
				bool foliage = c.cls == KWC_MASKED;
				if (foliage && nl < 0) nl = -nl * 0.5f;   // leaves: light through from behind
				if (nl > 0) { float s = sunVis(p); for (int k = 0; k < 3; k++) E[k] += sunE[k] * nl * s; }
				for (int li : nearLights) pointLight(lights[li], p, n, E, true);
				uint32_t r = EncLight(E[0] * expo), g = EncLight(E[1] * expo), b = EncLight(E[2] * expo), a = (uint32_t)(vis * 255.0f + 0.5f);
				out.vcol[c.baseVertex + i] = r | (g << 8) | (b << 16) | (a << 24);
			}
			doneV += c.numVertices;
		}
	};
	{
		std::vector<std::thread> th;
		for (unsigned i = 0; i < nth; i++) th.emplace_back(worker);
		while (true) {
			bool all = next.load() >= clusters.size();
			printf("\r  bake: vertices %5.1f%%", vCount ? 100.0 * doneV.load() / vCount : 100.0); fflush(stdout);
			if (all) break;
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
		}
		for (auto& t : th) t.join();
	}
	printf("\r  bake: %llu vertices lit (%.0fs)            \n", (unsigned long long)vCount, Now() - t0);

	// ---- light grid over the playable core (plus a margin), 96 units, at most ~1.5M points
	float step = 96.0f, lo[3], hi[3];
	bool core = cmin[0] <= cmax[0] && cmin[1] <= cmax[1] && cmin[2] <= cmax[2];
	float wmn[3] = { 1e30f, 1e30f, 1e30f }, wmx[3] = { -1e30f, -1e30f, -1e30f };
	for (const KwCluster& c : clusters) for (int k = 0; k < 3; k++) { wmn[k] = std::min(wmn[k], c.boundsMin[k]); wmx[k] = std::max(wmx[k], c.boundsMax[k]); }
	if (clusters.empty()) { printf("  bake: no geometry, no light grid\n"); return; }
	for (int k = 0; k < 3; k++) { lo[k] = (core ? cmin[k] : wmn[k]) - 512; hi[k] = (core ? cmax[k] : wmx[k]) + (k == 2 ? 1024 : 512); }
	for (;;) {
		double n = 1;
		for (int k = 0; k < 3; k++) n *= ceil((hi[k] - lo[k]) / step) + 1;
		if (n <= 1.5e6) break;
		step *= 1.25f;
	}
	KwGridHeader& G = out.grid;
	for (int k = 0; k < 3; k++) { G.origin[k] = lo[k]; G.step[k] = step; G.dims[k] = (uint32_t)ceil((hi[k] - lo[k]) / step) + 1; }
	size_t np = (size_t)G.dims[0] * G.dims[1] * G.dims[2];
	out.points.assign(np, KwGridPoint{});
	std::atomic<size_t> gnext(0);
	auto gworker = [&]() {
		for (;;) {
			size_t z = gnext++;
			if (z >= G.dims[2]) break;
			for (uint32_t y = 0; y < G.dims[1]; y++) for (uint32_t x = 0; x < G.dims[0]; x++) {
				float p[3] = { G.origin[0] + x * step, G.origin[1] + y * step, G.origin[2] + z * step };
				// ambient: sky seen from the point (both hemispheres, the lower one is the ground bounce)
				float E[3] = { 0, 0, 0 };
				int open = 0, rays = std::max(4, BAKE_SKY_RAYS);
				for (int i = 0; i < rays; i++) {
					float h[3]; HemiDir(i, rays, 0.7f * z, h);
					if (!bvh.occluded(p, h, 3072.0f)) open++;
				}
				float vis = open / (float)rays, sk[3];
				if (g_haveSH) SHIrradiance(up, sk); else { sk[0] = skyUp[0]; sk[1] = skyUp[1]; sk[2] = skyUp[2]; }
				float s = sunVis(p);
				for (int c = 0; c < 3; c++) E[c] = sk[c] * (0.25f + 0.75f * vis) + sunE[c] * std::max(0.0f, sunDir[2]) * (0.12f + 0.1f * s);
				for (const KwLight& L : lights) pointLight(L, p, nullptr, E, true);
				KwGridPoint& g = out.points[((size_t)z * G.dims[1] + y) * G.dims[0] + x];
				for (int c = 0; c < 3; c++) { g.ambient[c] = EncLight(E[c] * expo); g.directed[c] = EncLight(sunE[c] * s * expo); }
			}
		}
	};
	{
		std::vector<std::thread> th;
		for (unsigned i = 0; i < nth; i++) th.emplace_back(gworker);
		for (auto& t : th) t.join();
	}
	printf("  bake: light grid %u x %u x %u (%.0f units), done in %.0fs\n", G.dims[0], G.dims[1], G.dims[2], step, Now() - t0);
}

// six sky box faces (Quake 3 order and orientation) from the world's equirect environment
static void WriteSkyBox(const std::string& dir, const std::string& base)
{
	if (g_envImg.px.empty()) return;
	static const char* suf[6] = { "rt", "bk", "lf", "ft", "up", "dn" };
	static const int order[6] = { 0, 2, 1, 3, 4, 5 };
	static const int st2v[6][3] = { { 3, -1, 2 }, { -3, 1, 2 }, { 1, 3, 2 }, { -1, -3, 2 }, { -2, -1, 3 }, { 2, -1, -3 } };
	const int S = 512;
	std::vector<uint8_t> img((size_t)S * S * 3);
	for (int f = 0; f < 6; f++) {
		int axis = order[f];
		for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
			float s = (x + 0.5f) / S * 2 - 1, t = 1 - (y + 0.5f) / S * 2;
			float b[3] = { s, t, 1 }, d[3];
			for (int j = 0; j < 3; j++) { int k = st2v[axis][j]; d[j] = k < 0 ? -b[-k - 1] : b[k - 1]; }
			float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
			for (int j = 0; j < 3; j++) d[j] /= l;
			float u = 0.5f - atan2f(d[1], d[0]) / 6.2831853f, v = 0.5f - asinf(std::max(-1.0f, std::min(1.0f, d[2]))) / 3.14159265f;
			int px = std::min(g_envImg.w - 1, std::max(0, (int)(u * g_envImg.w))), py = std::min(g_envImg.h - 1, std::max(0, (int)(v * g_envImg.h)));
			const float* c = &g_envImg.px[((size_t)py * g_envImg.w + px) * 4];
			for (int k = 0; k < 3; k++) {
				float lin = c[k] * g_envStrength;
				lin = lin / (1 + lin * 0.25f);   // soft shoulder for the bright sky nearLights the sun
				float sr = lin <= 0.0031308f ? lin * 12.92f : 1.055f * powf(lin, 1 / 2.4f) - 0.055f;
				img[((size_t)y * S + x) * 3 + k] = (uint8_t)std::min(255.0f, std::max(0.0f, sr * 255.0f + 0.5f));
			}
		}
		std::string p = dir + "/" + base + "_" + suf[f] + ".tga";
		stbi_write_tga(p.c_str(), S, S, 3, img.data());
	}
}
