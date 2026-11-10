// KatarnEngine cooker - evaluates Blender shader node graphs over a UV grid ("baking" without Blender).
// Supports the nodes game-ripped and hand-made materials use: image textures, mapping, math, vector math, mix,
// colour ops, ramps, separate/combine, normal map, bump, groups, mix/add shader, principled, emission.
// View-dependent and procedural nodes fall back to sensible constants.
#pragma once
#include <vector>
#include <string>
#include <map>
#include <memory>
#include <functional>
#include <math.h>
#include <string.h>
#include <array>
#include <algorithm>

// a value over the bake grid: n channels, either constant (data.size()==n) or per texel (w*h*n)
struct Field {
	int n = 1;
	std::vector<float> d;
	bool constant() const { return d.size() == (size_t)n; }
	float at(size_t i, int c) const { return constant() ? d[c] : d[i * n + c]; }
	static Field C1(float v) { Field f; f.n = 1; f.d = { v }; return f; }
	static Field C3(float a, float b, float c) { Field f; f.n = 3; f.d = { a, b, c }; return f; }
};

struct BakeImage { int w = 0, h = 0; std::vector<float> px; };   // RGBA, row 0 = top (as decoded)

struct NodeCtx {
	int W = 1, H = 1;
	int y0 = 0, fullH = 0;           // tiled bakes: this tile is rows y0..y0+H of a fullH-row grid (0 = untiled)
	int FH() const { return fullH > 0 ? fullH : H; }
	std::function<std::shared_ptr<BakeImage>(const std::string& file, bool linearize)> loadImage;
	std::vector<std::string> warnings;
	const Json* groups = nullptr;
	bool worldMode = false;          // baking a world (sky) graph: texture coordinates are view directions
	float cameraRay = 1.0f;          // Light Path "Is Camera Ray"
	void warn(const std::string& s) { for (auto& w : warnings) if (w == s) return; warnings.push_back(s); }
};

// ------------------------------------------------------------------------------------------------ helpers
static inline float Clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

static Field Broadcast(const Field& f, int n)
{
	if (f.n == n) return f;
	Field r; r.n = n;
	size_t cnt = f.constant() ? 1 : f.d.size() / f.n;
	r.d.resize(cnt * n);
	for (size_t i = 0; i < cnt; i++) {
		if (f.n == 1) for (int c = 0; c < n; c++) r.d[i * n + c] = f.d[i];
		else if (n == 1) { // colour/vector -> value: luminance-ish average like Blender's implicit conversion
			float s = 0; for (int c = 0; c < std::min(f.n, 3); c++) s += f.d[i * f.n + c];
			r.d[i] = f.n >= 3 ? (0.2126f * f.d[i * f.n] + 0.7152f * f.d[i * f.n + 1] + 0.0722f * f.d[i * f.n + 2]) : s;
		}
		else for (int c = 0; c < n; c++) r.d[i * n + c] = c < f.n ? f.d[i * f.n + c] : 0.0f;
	}
	return r;
}

// applies fn(texel, out) over the grid; out has n channels; inputs determine constant-ness
static Field Map(NodeCtx& ctx, int n, std::initializer_list<const Field*> ins, const std::function<void(size_t, float*)>& fn)
{
	bool allConst = true;
	for (auto* f : ins) if (f && !f->constant()) allConst = false;
	Field r; r.n = n;
	size_t cnt = allConst ? 1 : (size_t)ctx.W * ctx.H;
	r.d.resize(cnt * n);
	for (size_t i = 0; i < cnt; i++) fn(i, &r.d[i * n]);
	return r;
}

static void RGB2HSV(const float* c, float* o)
{
	float mx = std::max(c[0], std::max(c[1], c[2])), mn = std::min(c[0], std::min(c[1], c[2])), d = mx - mn;
	float h = 0;
	if (d > 1e-8f) {
		if (mx == c[0]) h = fmodf((c[1] - c[2]) / d, 6.0f);
		else if (mx == c[1]) h = (c[2] - c[0]) / d + 2;
		else h = (c[0] - c[1]) / d + 4;
		h /= 6; if (h < 0) h += 1;
	}
	o[0] = h; o[1] = mx > 1e-8f ? d / mx : 0; o[2] = mx;
}
static void HSV2RGB(const float* hsv, float* o)
{
	float h = hsv[0] - floorf(hsv[0]), s = hsv[1], v = hsv[2];
	float c = v * s, x = c * (1 - fabsf(fmodf(h * 6, 2) - 1)), m = v - c;
	float r = 0, g = 0, b = 0;
	int k = (int)(h * 6) % 6;
	switch (k) { case 0: r = c; g = x; break; case 1: r = x; g = c; break; case 2: g = c; b = x; break; case 3: g = x; b = c; break; case 4: r = x; b = c; break; default: r = c; b = x; }
	o[0] = r + m; o[1] = g + m; o[2] = b + m;
}

static float MathOp(const std::string& op, float a, float b, float c)
{
	if (op == "ADD") return a + b; if (op == "SUBTRACT") return a - b; if (op == "MULTIPLY") return a * b;
	if (op == "DIVIDE") return b != 0 ? a / b : 0; if (op == "MULTIPLY_ADD") return a * b + c;
	if (op == "POWER") return (a < 0 && b != floorf(b)) ? 0 : powf(a, b);
	if (op == "LOGARITHM") return (a > 0 && b > 0) ? logf(a) / logf(b) : 0;
	if (op == "SQRT") return a > 0 ? sqrtf(a) : 0; if (op == "INVERSE_SQRT") return a > 0 ? 1 / sqrtf(a) : 0;
	if (op == "ABSOLUTE") return fabsf(a); if (op == "EXPONENT") return expf(a);
	if (op == "MINIMUM") return std::min(a, b); if (op == "MAXIMUM") return std::max(a, b);
	if (op == "LESS_THAN") return a < b ? 1.0f : 0.0f; if (op == "GREATER_THAN") return a > b ? 1.0f : 0.0f;
	if (op == "SIGN") return a > 0 ? 1.0f : a < 0 ? -1.0f : 0.0f;
	if (op == "COMPARE") return fabsf(a - b) <= std::max(c, 1e-5f) ? 1.0f : 0.0f;
	if (op == "SMOOTH_MIN") { if (c != 0) { float h = std::max(c - fabsf(a - b), 0.0f) / c; return std::min(a, b) - h * h * h * c / 6; } return std::min(a, b); }
	if (op == "SMOOTH_MAX") { if (c != 0) { float h = std::max(c - fabsf(a - b), 0.0f) / c; return std::max(a, b) + h * h * h * c / 6; } return std::max(a, b); }
	if (op == "ROUND") return floorf(a + 0.5f); if (op == "FLOOR") return floorf(a); if (op == "CEIL") return ceilf(a);
	if (op == "TRUNC") return truncf(a); if (op == "FRACT") return a - floorf(a);
	if (op == "MODULO") return b != 0 ? fmodf(a, b) : 0; if (op == "FLOORED_MODULO") return b != 0 ? a - floorf(a / b) * b : 0;
	if (op == "WRAP") { float r = b - c; return r != 0 ? a - r * floorf((a - c) / r) : c; }
	if (op == "SNAP") return b != 0 ? floorf(a / b) * b : 0;
	if (op == "PINGPONG") { if (b == 0) return 0; float t = fmodf(a, 2 * b); if (t < 0) t += 2 * b; return t > b ? 2 * b - t : t; }
	if (op == "SINE") return sinf(a); if (op == "COSINE") return cosf(a); if (op == "TANGENT") return tanf(a);
	if (op == "ARCSINE") return asinf(std::max(-1.0f, std::min(1.0f, a))); if (op == "ARCCOSINE") return acosf(std::max(-1.0f, std::min(1.0f, a)));
	if (op == "ARCTANGENT") return atanf(a); if (op == "ARCTAN2") return atan2f(a, b);
	if (op == "SINH") return sinhf(a); if (op == "COSH") return coshf(a); if (op == "TANH") return tanhf(a);
	if (op == "RADIANS") return a * 0.0174532925f; if (op == "DEGREES") return a * 57.2957795f;
	return a;
}

static void Blend(const std::string& mode, float t, const float* a, const float* b, float* o)
{
	float s = 1 - t;
	for (int c = 0; c < 3; c++) {
		float A = a[c], B = b[c], r = A;
		if (mode == "MIX") r = s * A + t * B;
		else if (mode == "ADD") r = A + t * B;
		else if (mode == "MULTIPLY") r = A * (s + t * B);
		else if (mode == "SUBTRACT") r = A - t * B;
		else if (mode == "SCREEN") r = 1 - (s + t * (1 - B)) * (1 - A);
		else if (mode == "DIVIDE") r = B != 0 ? s * A + t * A / B : A;
		else if (mode == "DIFFERENCE") r = s * A + t * fabsf(A - B);
		else if (mode == "EXCLUSION") r = std::max(s * A + t * (A + B - 2 * A * B), 0.0f);
		else if (mode == "DARKEN") r = s * A + t * std::min(A, B);
		else if (mode == "LIGHTEN") r = s * A + t * std::max(A, B);
		else if (mode == "OVERLAY") r = A < 0.5f ? A * (s + 2 * t * B) : 1 - (s + 2 * t * (1 - B)) * (1 - A);
		else if (mode == "DODGE") { float tmp = 1 - t * B; r = A != 0 ? (tmp > 0 ? std::min(A / tmp, 1.0f) : 1.0f) : 0; }
		else if (mode == "BURN") { float tmp = s + t * B; r = tmp > 0 ? Clamp01(1 - (1 - A) / tmp) : 0; }
		else if (mode == "SOFT_LIGHT") { float scr = 1 - (1 - B) * (1 - A); r = s * A + t * ((1 - A) * B * A + A * scr); }
		else if (mode == "LINEAR_LIGHT") r = A + t * (2 * B - 1);
		else r = s * A + t * B;   // HUE/SATURATION/VALUE/COLOR handled below
		o[c] = r;
	}
	if (mode == "HUE" || mode == "SATURATION" || mode == "VALUE" || mode == "COLOR") {
		float ha[3], hb[3]; RGB2HSV(a, ha); RGB2HSV(b, hb);
		float h[3] = { ha[0], ha[1], ha[2] };
		if (mode == "HUE") { if (hb[1] > 0) h[0] = hb[0]; }
		else if (mode == "SATURATION") { if (ha[1] > 0) h[1] = hb[1]; }
		else if (mode == "VALUE") h[2] = hb[2];
		else { h[0] = hb[0]; h[1] = hb[1]; }
		float rgb[3]; HSV2RGB(h, rgb);
		for (int c = 0; c < 3; c++) o[c] = s * a[c] + t * rgb[c];
	}
}

// ------------------------------------------------------------------------------------------------ evaluator
struct NodeGraph {
	const Json* nodes = nullptr;          // {"name": node}
	NodeCtx* ctx = nullptr;
	const NodeGraph* parent = nullptr;    // for groups: where GROUP_INPUT values come from
	const Json* groupNode = nullptr;
	std::map<std::string, Field> memo;
	int depth = 0;

	const Json* node(const std::string& n) const { const Json& j = (*nodes)[n.c_str()]; return j.t == Json::OBJ ? &j : nullptr; }

	static const Json* input(const Json& node, const char* id)
	{
		for (auto& i : node["inputs"].a) if (i["id"].str() == id) return &i;
		for (auto& i : node["inputs"].a) if (i["name"].str() == id && !(i["enabled"].t == Json::BOOL && !i["enabled"].b)) return &i;
		return nullptr;
	}

	Field defaultOf(const Json* in, int n) const
	{
		if (!in) return Broadcast(Field::C1(0), n);
		const Json& v = (*in)["value"];
		if (v.t == Json::NUM) return Broadcast(Field::C1((float)v.n), n);
		if (v.t == Json::ARR && v.size() >= 3) return Broadcast(Field::C3((float)v[(size_t)0].n, (float)v[1].n, (float)v[2].n), n);
		return Broadcast(Field::C1(0), n);
	}

	// value of an input socket (follows the link), with n channels
	Field in(const Json& nd, const char* id, int n)
	{
		const Json* s = input(nd, id);
		if (!s) return Broadcast(Field::C1(0), n);
		const Json& lk = (*s)["link"];
		if (lk.t == Json::ARR && lk.size() == 2) return Broadcast(out(lk[(size_t)0].str(), lk[1].str()), n);
		// unlinked vector inputs of textures default to UV
		return defaultOf(s, n);
	}
	bool linked(const Json& nd, const char* id) const { const Json* s = input(nd, id); return s && (*s)["link"].t == Json::ARR; }

	Field dirs()
	{
		// equirectangular grid (row 0 = bottom = straight down), directions in Blender world space
		Field f; f.n = 3; f.d.resize((size_t)ctx->W * ctx->H * 3);
		for (int y = 0; y < ctx->H; y++) for (int x = 0; x < ctx->W; x++) {
			size_t i = (size_t)y * ctx->W + x;
			float el = ((ctx->y0 + y + 0.5f) / ctx->FH() - 0.5f) * 3.14159265f, phi = (0.5f - (x + 0.5f) / ctx->W) * 2 * 3.14159265f;
			f.d[i * 3] = cosf(el) * cosf(phi); f.d[i * 3 + 1] = cosf(el) * sinf(phi); f.d[i * 3 + 2] = sinf(el);
		}
		return f;
	}
	Field uv()
	{
		if (ctx->worldMode) return dirs();
		Field f; f.n = 3; f.d.resize((size_t)ctx->W * ctx->H * 3);
		for (int y = 0; y < ctx->H; y++) for (int x = 0; x < ctx->W; x++) {
			size_t i = (size_t)y * ctx->W + x;
			f.d[i * 3] = (x + 0.5f) / ctx->W;
			f.d[i * 3 + 1] = (ctx->y0 + y + 0.5f) / ctx->FH();    // Blender UV: v up; grid row 0 = v near 0
			f.d[i * 3 + 2] = 0;
		}
		return f;
	}

	Field out(const std::string& nodeName, const std::string& sock)
	{
		std::string key = nodeName + "|" + sock;
		auto it = memo.find(key);
		if (it != memo.end()) return it->second;
		if (++depth > 200) { ctx->warn("node graph too deep"); depth--; return Field::C1(0); }
		Field r = eval(nodeName, sock);
		depth--;
		memo[key] = r;
		return r;
	}

	Field eval(const std::string& name, const std::string& sock);
};

static Field SampleImage(NodeCtx& ctx, const BakeImage& img, const Field& vec, const std::string& ext, bool closest, bool alpha)
{
	return Map(ctx, alpha ? 1 : 3, { &vec }, [&](size_t i, float* o) {
		float u = vec.at(i, 0), v = vec.at(i, 1);
		float fx = u * img.w - 0.5f, fy = (1.0f - v) * img.h - 0.5f;
		auto wrap = [&](int c, int n) {
			if (ext == "EXTEND" || ext == "CLIP") return c < 0 ? 0 : c >= n ? n - 1 : c;
			if (ext == "MIRROR") { int p = ((c % (2 * n)) + 2 * n) % (2 * n); return p < n ? p : 2 * n - 1 - p; }
			return ((c % n) + n) % n;
		};
		if (ext == "CLIP" && (u < 0 || u > 1 || v < 0 || v > 1)) { for (int c = 0; c < (alpha ? 1 : 3); c++) o[c] = 0; return; }
		if (closest) {
			int x = wrap((int)floorf(fx + 0.5f), img.w), y = wrap((int)floorf(fy + 0.5f), img.h);
			const float* p = &img.px[((size_t)y * img.w + x) * 4];
			if (alpha) o[0] = p[3]; else { o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; }
			return;
		}
		int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
		float tx = fx - x0, ty = fy - y0;
		int xa = wrap(x0, img.w), xb = wrap(x0 + 1, img.w), ya = wrap(y0, img.h), yb = wrap(y0 + 1, img.h);
		const float* p00 = &img.px[((size_t)ya * img.w + xa) * 4], * p10 = &img.px[((size_t)ya * img.w + xb) * 4];
		const float* p01 = &img.px[((size_t)yb * img.w + xa) * 4], * p11 = &img.px[((size_t)yb * img.w + xb) * 4];
		auto lerp = [&](int c) { return (p00[c] * (1 - tx) + p10[c] * tx) * (1 - ty) + (p01[c] * (1 - tx) + p11[c] * tx) * ty; };
		if (alpha) o[0] = lerp(3); else { o[0] = lerp(0); o[1] = lerp(1); o[2] = lerp(2); }
	});
}

static void Rotate(const float* r, float* v)   // Blender XYZ euler
{
	float x = v[0], y = v[1], z = v[2];
	float cx = cosf(r[0]), sx = sinf(r[0]), cy = cosf(r[1]), sy = sinf(r[1]), cz = cosf(r[2]), sz = sinf(r[2]);
	float y1 = y * cx - z * sx, z1 = y * sx + z * cx; y = y1; z = z1;
	float x2 = x * cy + z * sy, z2 = -x * sy + z * cy; x = x2; z = z2;
	float x3 = x * cz - y * sz, y3 = x * sz + y * cz; x = x3; y = y3;
	v[0] = x; v[1] = y; v[2] = z;
}

Field NodeGraph::eval(const std::string& name, const std::string& sock)
{
	NodeCtx& C = *ctx;
	const Json* np = node(name);
	if (!np) return Field::C1(0);
	const Json& n = *np;
	std::string type = n["type"].str();
	const Json& P = n["props"];
	auto prop = [&](const char* k, const char* d) { return P[k].str(d); };

	if (type == "REROUTE") {
		const Json* s = input(n, "Input");
		if (s && (*s)["link"].t == Json::ARR) return out((*s)["link"][(size_t)0].str(), (*s)["link"][1].str());
		return Field::C1(0);
	}
	if (type == "VALUE") { for (auto& o : n["outputs"].a) if (o["value"].t == Json::NUM) return Field::C1((float)o["value"].n); return Field::C1(0); }
	if (type == "RGB") { for (auto& o : n["outputs"].a) if (o["value"].t == Json::ARR) return Field::C3((float)o["value"][(size_t)0].n, (float)o["value"][1].n, (float)o["value"][2].n); return Field::C3(1, 1, 1); }
	if (type == "NEW_GEOMETRY") {
		if (sock == "Backfacing") return Field::C1(0);
		if (sock == "Pointiness" || sock == "Random Per Island") return Field::C1(0.5f);
		if (sock == "Position") return uv();
		return C.worldMode ? dirs() : Field::C3(0, 0, 1);
	}
	if (type == "TEX_ENVIRONMENT") {
		std::string file = prop("image", "");
		auto img = file.empty() ? nullptr : C.loadImage(file, prop("colorspace", "sRGB") == std::string("sRGB"));
		if (!img) return Field::C3(0.5f, 0.5f, 0.5f);
		Field d = linked(n, "Vector") ? in(n, "Vector", 3) : (C.worldMode ? dirs() : uv());
		Field uvf = Map(C, 3, { &d }, [&](size_t i, float* o) {
			float x = d.at(i, 0), y = d.at(i, 1), z = d.at(i, 2), l = sqrtf(x * x + y * y + z * z); if (l < 1e-8f) l = 1;
			o[0] = -atan2f(y / l, x / l) / (2 * 3.14159265f) + 0.5f; o[1] = atan2f(z / l, hypotf(x / l, y / l)) / 3.14159265f + 0.5f; o[2] = 0;
		});
		return SampleImage(C, *img, uvf, "REPEAT", false, false);
	}
	if (type == "TEX_COORD" || type == "UVMAP") {
		if (C.worldMode) return dirs();
		if (sock == "UV" || sock == "Generated" || sock == "Object" || sock == "Window" || sock == "Reflection" || sock == "Camera") return uv();
		if (sock == "Normal") return Field::C3(0, 0, 1);
		return uv();
	}
	if (type == "TEX_IMAGE") {
		std::string file = prop("image", "");
		if (file.empty()) return sock == "Alpha" ? Field::C1(1) : Field::C3(0, 0, 0);
		bool lin = prop("colorspace", "sRGB") == std::string("sRGB");
		auto img = C.loadImage(file, lin);
		if (!img) return sock == "Alpha" ? Field::C1(1) : Field::C3(1, 0, 1);
		Field vec = linked(n, "Vector") ? in(n, "Vector", 3) : uv();
		return SampleImage(C, *img, vec, prop("extension", "REPEAT"), prop("interpolation", "Linear") == std::string("Closest"), sock == "Alpha");
	}
	if (type == "MAPPING") {
		std::string vt = prop("vector_type", "POINT");
		Field v = in(n, "Vector", 3), loc = in(n, "Location", 3), rot = in(n, "Rotation", 3), sc = in(n, "Scale", 3);
		return Map(C, 3, { &v, &loc, &rot, &sc }, [&](size_t i, float* o) {
			float p[3] = { v.at(i, 0), v.at(i, 1), v.at(i, 2) }, L[3] = { loc.at(i, 0), loc.at(i, 1), loc.at(i, 2) };
			float R[3] = { rot.at(i, 0), rot.at(i, 1), rot.at(i, 2) }, S[3] = { sc.at(i, 0), sc.at(i, 1), sc.at(i, 2) };
			if (vt == "TEXTURE") { for (int c = 0; c < 3; c++) p[c] -= L[c]; float nr[3] = { -R[0], -R[1], -R[2] }; (void)nr; Rotate(nr, p); for (int c = 0; c < 3; c++) p[c] = S[c] != 0 ? p[c] / S[c] : 0; }
			else { for (int c = 0; c < 3; c++) p[c] *= S[c]; Rotate(R, p); if (vt == "POINT") for (int c = 0; c < 3; c++) p[c] += L[c]; }
			o[0] = p[0]; o[1] = p[1]; o[2] = p[2];
		});
	}
	if (type == "MATH") {
		std::string op = prop("operation", "ADD");
		bool clamp = P["use_clamp"].t == Json::BOOL && P["use_clamp"].b;
		Field a = in(n, "Value", 1), b = in(n, "Value_001", 1), c = in(n, "Value_002", 1);
		return Map(C, 1, { &a, &b, &c }, [&](size_t i, float* o) { float r = MathOp(op, a.at(i, 0), b.at(i, 0), c.at(i, 0)); o[0] = clamp ? Clamp01(r) : r; });
	}
	if (type == "VECT_MATH") {
		std::string op = prop("operation", "ADD");
		Field a = in(n, "Vector", 3), b = in(n, "Vector_001", 3), c = in(n, "Vector_002", 3), s = in(n, "Scale", 1);
		bool scalar = sock == "Value";
		return Map(C, scalar ? 1 : 3, { &a, &b, &c, &s }, [&](size_t i, float* o) {
			float A[3] = { a.at(i, 0), a.at(i, 1), a.at(i, 2) }, B[3] = { b.at(i, 0), b.at(i, 1), b.at(i, 2) }, Cc[3] = { c.at(i, 0), c.at(i, 1), c.at(i, 2) };
			float r[3] = { 0, 0, 0 }, val = 0;
			auto dot = [](const float* x, const float* y) { return x[0] * y[0] + x[1] * y[1] + x[2] * y[2]; };
			if (op == "ADD") for (int k = 0; k < 3; k++) r[k] = A[k] + B[k];
			else if (op == "SUBTRACT") for (int k = 0; k < 3; k++) r[k] = A[k] - B[k];
			else if (op == "MULTIPLY") for (int k = 0; k < 3; k++) r[k] = A[k] * B[k];
			else if (op == "DIVIDE") for (int k = 0; k < 3; k++) r[k] = B[k] != 0 ? A[k] / B[k] : 0;
			else if (op == "MULTIPLY_ADD") for (int k = 0; k < 3; k++) r[k] = A[k] * B[k] + Cc[k];
			else if (op == "CROSS_PRODUCT") { r[0] = A[1] * B[2] - A[2] * B[1]; r[1] = A[2] * B[0] - A[0] * B[2]; r[2] = A[0] * B[1] - A[1] * B[0]; }
			else if (op == "DOT_PRODUCT") val = dot(A, B);
			else if (op == "DISTANCE") { float d[3] = { A[0] - B[0], A[1] - B[1], A[2] - B[2] }; val = sqrtf(dot(d, d)); }
			else if (op == "LENGTH") val = sqrtf(dot(A, A));
			else if (op == "SCALE") for (int k = 0; k < 3; k++) r[k] = A[k] * s.at(i, 0);
			else if (op == "NORMALIZE") { float l = sqrtf(dot(A, A)); for (int k = 0; k < 3; k++) r[k] = l > 0 ? A[k] / l : 0; }
			else if (op == "ABSOLUTE") for (int k = 0; k < 3; k++) r[k] = fabsf(A[k]);
			else if (op == "MINIMUM") for (int k = 0; k < 3; k++) r[k] = std::min(A[k], B[k]);
			else if (op == "MAXIMUM") for (int k = 0; k < 3; k++) r[k] = std::max(A[k], B[k]);
			else if (op == "FLOOR") for (int k = 0; k < 3; k++) r[k] = floorf(A[k]);
			else if (op == "CEIL") for (int k = 0; k < 3; k++) r[k] = ceilf(A[k]);
			else if (op == "FRACTION") for (int k = 0; k < 3; k++) r[k] = A[k] - floorf(A[k]);
			else if (op == "MODULO") for (int k = 0; k < 3; k++) r[k] = B[k] != 0 ? fmodf(A[k], B[k]) : 0;
			else if (op == "SNAP") for (int k = 0; k < 3; k++) r[k] = B[k] != 0 ? floorf(A[k] / B[k]) * B[k] : 0;
			else if (op == "SINE") for (int k = 0; k < 3; k++) r[k] = sinf(A[k]);
			else if (op == "COSINE") for (int k = 0; k < 3; k++) r[k] = cosf(A[k]);
			else if (op == "PROJECT") { float bb = dot(B, B); float t = bb > 0 ? dot(A, B) / bb : 0; for (int k = 0; k < 3; k++) r[k] = B[k] * t; }
			else if (op == "REFLECT") { float l = sqrtf(dot(B, B)); float nn[3] = { B[0] / (l > 0 ? l : 1), B[1] / (l > 0 ? l : 1), B[2] / (l > 0 ? l : 1) }; float d2 = 2 * dot(A, nn); for (int k = 0; k < 3; k++) r[k] = A[k] - d2 * nn[k]; }
			else for (int k = 0; k < 3; k++) r[k] = A[k];
			if (scalar) o[0] = (op == "DOT_PRODUCT" || op == "DISTANCE" || op == "LENGTH") ? val : sqrtf(dot(r, r));
			else { o[0] = r[0]; o[1] = r[1]; o[2] = r[2]; }
		});
	}
	if (type == "MIX") {
		std::string dt = prop("data_type", "FLOAT"), bt = prop("blend_type", "MIX"), fm = prop("factor_mode", "UNIFORM");
		bool cf = !(P["clamp_factor"].t == Json::BOOL && !P["clamp_factor"].b);
		bool cr = P["clamp_result"].t == Json::BOOL && P["clamp_result"].b;
		if (dt == "FLOAT") {
			Field f = in(n, "Factor_Float", 1), a = in(n, "A_Float", 1), b = in(n, "B_Float", 1);
			return Map(C, 1, { &f, &a, &b }, [&](size_t i, float* o) { float t = cf ? Clamp01(f.at(i, 0)) : f.at(i, 0); o[0] = a.at(i, 0) * (1 - t) + b.at(i, 0) * t; });
		}
		if (dt == "VECTOR") {
			bool nu = fm == "NON_UNIFORM";
			Field f = nu ? in(n, "Factor_Vector", 3) : in(n, "Factor_Float", 1), a = in(n, "A_Vector", 3), b = in(n, "B_Vector", 3);
			return Map(C, 3, { &f, &a, &b }, [&](size_t i, float* o) {
				for (int k = 0; k < 3; k++) { float t = f.at(i, nu ? k : 0); if (cf) t = Clamp01(t); o[k] = a.at(i, k) * (1 - t) + b.at(i, k) * t; }
			});
		}
		Field f = in(n, "Factor_Float", 1), a = in(n, "A_Color", 3), b = in(n, "B_Color", 3);
		return Map(C, 3, { &f, &a, &b }, [&](size_t i, float* o) {
			float t = cf ? Clamp01(f.at(i, 0)) : f.at(i, 0);
			float A[3] = { a.at(i, 0), a.at(i, 1), a.at(i, 2) }, B[3] = { b.at(i, 0), b.at(i, 1), b.at(i, 2) };
			Blend(bt, t, A, B, o);
			if (cr) for (int k = 0; k < 3; k++) o[k] = Clamp01(o[k]);
		});
	}
	if (type == "MIX_RGB") {
		std::string bt = prop("blend_type", "MIX");
		bool cr = P["use_clamp"].t == Json::BOOL && P["use_clamp"].b;
		Field f = in(n, "Fac", 1), a = in(n, "Color1", 3), b = in(n, "Color2", 3);
		return Map(C, 3, { &f, &a, &b }, [&](size_t i, float* o) {
			float A[3] = { a.at(i, 0), a.at(i, 1), a.at(i, 2) }, B[3] = { b.at(i, 0), b.at(i, 1), b.at(i, 2) };
			Blend(bt, Clamp01(f.at(i, 0)), A, B, o);
			if (cr) for (int k = 0; k < 3; k++) o[k] = Clamp01(o[k]);
		});
	}
	if (type == "CLAMP") {
		bool range = prop("clamp_type", "MINMAX") == std::string("RANGE");
		Field v = in(n, "Value", 1), lo = in(n, "Min", 1), hi = in(n, "Max", 1);
		return Map(C, 1, { &v, &lo, &hi }, [&](size_t i, float* o) {
			float a = lo.at(i, 0), b = hi.at(i, 0);
			if (range && a > b) std::swap(a, b);
			o[0] = std::min(std::max(v.at(i, 0), a), b);
		});
	}
	if (type == "MAP_RANGE") {
		std::string it = prop("interpolation_type", "LINEAR");
		bool clamp = !(P["clamp"].t == Json::BOOL && !P["clamp"].b);
		Field v = in(n, "Value", 1), a = in(n, "From Min", 1), b = in(n, "From Max", 1), c = in(n, "To Min", 1), d = in(n, "To Max", 1), st = in(n, "Steps", 1);
		if (!input(n, "From Min")) { a = in(n, "From_Min", 1); b = in(n, "From_Max", 1); c = in(n, "To_Min", 1); d = in(n, "To_Max", 1); }
		return Map(C, 1, { &v, &a, &b, &c, &d, &st }, [&](size_t i, float* o) {
			float fa = a.at(i, 0), fb = b.at(i, 0), x = v.at(i, 0);
			float t = fb != fa ? (x - fa) / (fb - fa) : 0;
			if (it == "STEPPED") { float s = st.at(i, 0); t = s > 0 ? floorf(t * (s + 1)) / s : t; }
			else if (it == "SMOOTHSTEP") { t = Clamp01(t); t = t * t * (3 - 2 * t); }
			else if (it == "SMOOTHERSTEP") { t = Clamp01(t); t = t * t * t * (t * (t * 6 - 15) + 10); }
			float r = c.at(i, 0) + t * (d.at(i, 0) - c.at(i, 0));
			if (clamp && it != "SMOOTHSTEP" && it != "SMOOTHERSTEP") r = std::min(std::max(r, std::min(c.at(i, 0), d.at(i, 0))), std::max(c.at(i, 0), d.at(i, 0)));
			o[0] = r;
		});
	}
	if (type == "SEPARATE_COLOR" || type == "SEPRGB" || type == "SEPXYZ" || type == "SEPARATE_RGB" || type == "SEPARATE_XYZ" || type == "SEPHSV") {
		std::string mode = prop("mode", type == "SEPHSV" ? "HSV" : "RGB");
		Field c = in(n, type == "SEPXYZ" || type == "SEPARATE_XYZ" ? "Vector" : (input(n, "Color") ? "Color" : "Image"), 3);
		int ch = (sock == "Red" || sock == "R" || sock == "X" || sock == "H") ? 0 : (sock == "Green" || sock == "G" || sock == "Y" || sock == "S") ? 1 : 2;
		return Map(C, 1, { &c }, [&](size_t i, float* o) {
			float v[3] = { c.at(i, 0), c.at(i, 1), c.at(i, 2) };
			if (mode == "HSV" || mode == "HSL") { float h[3]; RGB2HSV(v, h); o[0] = h[ch]; }
			else o[0] = v[ch];
		});
	}
	if (type == "COMBINE_COLOR" || type == "COMBRGB" || type == "COMBXYZ" || type == "COMBINE_RGB" || type == "COMBINE_XYZ" || type == "COMBHSV") {
		const char* ids[3] = { "Red", "Green", "Blue" };
		if (type == "COMBRGB" || type == "COMBINE_RGB") { ids[0] = "R"; ids[1] = "G"; ids[2] = "B"; }
		if (type == "COMBXYZ" || type == "COMBINE_XYZ") { ids[0] = "X"; ids[1] = "Y"; ids[2] = "Z"; }
		std::string mode = prop("mode", "RGB");
		Field a = in(n, ids[0], 1), b = in(n, ids[1], 1), c = in(n, ids[2], 1);
		return Map(C, 3, { &a, &b, &c }, [&](size_t i, float* o) {
			float v[3] = { a.at(i, 0), b.at(i, 0), c.at(i, 0) };
			if (mode == "HSV" || mode == "HSL") HSV2RGB(v, o); else { o[0] = v[0]; o[1] = v[1]; o[2] = v[2]; }
		});
	}
	if (type == "INVERT") {
		Field f = in(n, "Fac", 1), c = in(n, "Color", 3);
		return Map(C, 3, { &f, &c }, [&](size_t i, float* o) { float t = f.at(i, 0); for (int k = 0; k < 3; k++) o[k] = c.at(i, k) * (1 - t) + (1 - c.at(i, k)) * t; });
	}
	if (type == "GAMMA") {
		Field c = in(n, "Color", 3), g = in(n, "Gamma", 1);
		return Map(C, 3, { &c, &g }, [&](size_t i, float* o) { for (int k = 0; k < 3; k++) o[k] = c.at(i, k) > 0 ? powf(c.at(i, k), g.at(i, 0)) : c.at(i, k); });
	}
	if (type == "BRIGHTCONTRAST") {
		Field c = in(n, "Color", 3), br = in(n, "Bright", 1), co = in(n, "Contrast", 1);
		return Map(C, 3, { &c, &br, &co }, [&](size_t i, float* o) {
			float a = 1 + co.at(i, 0), b = br.at(i, 0) - co.at(i, 0) * 0.5f;
			for (int k = 0; k < 3; k++) o[k] = std::max(a * c.at(i, k) + b, 0.0f);
		});
	}
	if (type == "HUE_SAT") {
		Field h = in(n, "Hue", 1), s = in(n, "Saturation", 1), v = in(n, "Value", 1), f = in(n, "Fac", 1), c = in(n, "Color", 3);
		return Map(C, 3, { &h, &s, &v, &f, &c }, [&](size_t i, float* o) {
			float col[3] = { c.at(i, 0), c.at(i, 1), c.at(i, 2) }, hsv[3];
			RGB2HSV(col, hsv);
			hsv[0] = hsv[0] + h.at(i, 0) + 0.5f; hsv[0] -= floorf(hsv[0]);
			hsv[1] = Clamp01(hsv[1] * s.at(i, 0)); hsv[2] = hsv[2] * v.at(i, 0);
			float r[3]; HSV2RGB(hsv, r);
			float t = f.at(i, 0);
			for (int k = 0; k < 3; k++) o[k] = col[k] * (1 - t) + r[k] * t;
		});
	}
	if (type == "RGBTOBW") {
		Field c = in(n, "Color", 3);
		return Map(C, 1, { &c }, [&](size_t i, float* o) { o[0] = 0.2126f * c.at(i, 0) + 0.7152f * c.at(i, 1) + 0.0722f * c.at(i, 2); });
	}
	if (type == "VALTORGB") {
		const Json& ramp = P["ramp"];
		std::string interp = ramp["interpolation"].str("LINEAR");
		std::vector<std::array<float, 5>> el;
		for (auto& e : ramp["elements"].a) el.push_back({ (float)e[(size_t)0].n, (float)e[1].n, (float)e[2].n, (float)e[3].n, (float)e[4].num(1) });
		std::sort(el.begin(), el.end(), [](const std::array<float, 5>& a, const std::array<float, 5>& b) { return a[0] < b[0]; });
		Field f = in(n, "Fac", 1);
		bool alpha = sock == "Alpha";
		return Map(C, alpha ? 1 : 3, { &f }, [&](size_t i, float* o) {
			float x = f.at(i, 0);
			float col[4] = { 0, 0, 0, 1 };
			if (el.empty()) {}
			else if (x <= el.front()[0]) for (int k = 0; k < 4; k++) col[k] = el.front()[k + 1];
			else if (x >= el.back()[0]) for (int k = 0; k < 4; k++) col[k] = el.back()[k + 1];
			else for (size_t j = 0; j + 1 < el.size(); j++) if (x >= el[j][0] && x <= el[j + 1][0]) {
				float t = el[j + 1][0] > el[j][0] ? (x - el[j][0]) / (el[j + 1][0] - el[j][0]) : 0;
				if (interp == "CONSTANT") t = 0;
				else if (interp == "EASE") t = t * t * (3 - 2 * t);
				for (int k = 0; k < 4; k++) col[k] = el[j][k + 1] * (1 - t) + el[j + 1][k + 1] * t;
				break;
			}
			if (alpha) o[0] = col[3]; else { o[0] = col[0]; o[1] = col[1]; o[2] = col[2]; }
		});
	}
	if (type == "CURVE_RGB" || type == "CURVE_FLOAT") {
		const Json& curves = P["curves"];
		auto evalCurve = [&](int ci, float x) {
			const Json& c = curves[(size_t)ci];
			if (c.t != Json::ARR || c.size() == 0) return x;
			if (x <= c[(size_t)0][(size_t)0].n) return (float)c[(size_t)0][1].n;
			for (size_t j = 0; j + 1 < c.size(); j++) {
				float x0 = (float)c[j][(size_t)0].n, x1 = (float)c[j + 1][(size_t)0].n;
				if (x <= x1) { float t = x1 > x0 ? (x - x0) / (x1 - x0) : 0; return (float)(c[j][1].n * (1 - t) + c[j + 1][1].n * t); }
			}
			return (float)c[c.size() - 1][1].n;
		};
		if (type == "CURVE_FLOAT") {
			Field f = in(n, "Factor", 1), v = in(n, "Value", 1);
			return Map(C, 1, { &f, &v }, [&](size_t i, float* o) { float t = f.at(i, 0); o[0] = v.at(i, 0) * (1 - t) + evalCurve(0, v.at(i, 0)) * t; });
		}
		Field f = in(n, "Fac", 1), c = in(n, "Color", 3);
		return Map(C, 3, { &f, &c }, [&](size_t i, float* o) {
			float t = f.at(i, 0);
			for (int k = 0; k < 3; k++) { float v = evalCurve(3, evalCurve(k, c.at(i, k))); o[k] = c.at(i, k) * (1 - t) + v * t; }
		});
	}
	if (type == "NORMAL_MAP") {
		// tangent-space normal (what the GBuffer wants), not Blender's world-space output
		Field s = in(n, "Strength", 1), c = in(n, "Color", 3);
		return Map(C, 3, { &s, &c }, [&](size_t i, float* o) {
			float v[3] = { c.at(i, 0) * 2 - 1, c.at(i, 1) * 2 - 1, c.at(i, 2) * 2 - 1 }, t = s.at(i, 0);
			for (int k = 0; k < 3; k++) v[k] = (k == 2 ? 1.0f : 0.0f) * (1 - t) + v[k] * t;
			float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (l < 1e-6f) { v[2] = 1; l = 1; }
			o[0] = v[0] / l; o[1] = v[1] / l; o[2] = v[2] / l;
		});
	}
	if (type == "BUMP") {
		Field h = in(n, "Height", 1), s = in(n, "Strength", 1), dist = in(n, "Distance", 1);
		Field base = linked(n, "Normal") ? in(n, "Normal", 3) : Field::C3(0, 0, 1);
		bool inv = P["invert"].t == Json::BOOL && P["invert"].b;
		if (h.constant()) return base;
		int W = C.W, H = C.H, FH = C.FH();
		bool tiled = FH != H;
		return Map(C, 3, { &h, &s, &base }, [&](size_t i, float* o) {
			int x = (int)(i % W), y = (int)(i / W);
			auto H_ = [&](int xx, int yy) { xx = (xx % W + W) % W; yy = tiled ? std::min(std::max(yy, 0), H - 1) : (yy % H + H) % H; return h.at((size_t)yy * W + xx, 0); };
			float k = s.at(i, 0) * dist.at(0, 0) * (inv ? -1.0f : 1.0f) * 2.0f;
			float dx = (H_(x + 1, y) - H_(x - 1, y)) * 0.5f * W * k * 0.01f, dy = (H_(x, y + 1) - H_(x, y - 1)) * 0.5f * FH * k * 0.01f;
			float nb[3] = { base.at(i, 0) - dx, base.at(i, 1) - dy, base.at(i, 2) };
			float l = sqrtf(nb[0] * nb[0] + nb[1] * nb[1] + nb[2] * nb[2]); if (l < 1e-6f) { nb[2] = 1; l = 1; }
			o[0] = nb[0] / l; o[1] = nb[1] / l; o[2] = nb[2] / l;
		});
	}
	if (type == "GROUP") {
		std::string tree = P["tree"].str();
		const Json* gt = C.groups ? &(*C.groups)[tree.c_str()] : nullptr;
		if (!gt || gt->t != Json::OBJ) { C.warn("missing node group " + tree); return Field::C1(0); }
		NodeGraph sub;
		sub.nodes = &(*gt)["nodes"];
		sub.ctx = ctx; sub.parent = this; sub.groupNode = &n; sub.depth = depth;
		for (auto& kv : sub.nodes->o) {
			if (kv.second["type"].str() != "GROUP_OUTPUT") continue;
			if (!(kv.second["props"]["is_active_output"].t == Json::BOOL ? kv.second["props"]["is_active_output"].b : true)) continue;
			for (auto& s : kv.second["inputs"].a) if (s["id"].str() == sock || s["name"].str() == sock) {
				if (s["link"].t == Json::ARR) return sub.out(s["link"][(size_t)0].str(), s["link"][1].str());
				return sub.defaultOf(&s, 3);
			}
		}
		return Field::C1(0);
	}
	if (type == "GROUP_INPUT") {
		if (!parent || !groupNode) return Field::C1(0);
		NodeGraph* p = const_cast<NodeGraph*>(parent);
		// the group node's input with the same identifier
		for (auto& s : (*groupNode)["inputs"].a) if (s["id"].str() == sock || s["name"].str() == sock) {
			if (s["link"].t == Json::ARR) return p->out(s["link"][(size_t)0].str(), s["link"][1].str());
			const Json& v = s["value"];
			if (v.t == Json::NUM) return Field::C1((float)v.n);
			if (v.t == Json::ARR && v.size() >= 3) return Field::C3((float)v[(size_t)0].n, (float)v[1].n, (float)v[2].n);
			return Field::C1(0);
		}
		return Field::C1(0);
	}
	if (type == "ATTRIBUTE" || type == "VERTEX_COLOR") { C.warn("vertex colour / attribute node treated as white"); return sock == "Fac" || sock == "Alpha" ? Field::C1(1) : Field::C3(1, 1, 1); }
	if (type == "AMBIENT_OCCLUSION") return sock == "AO" ? Field::C1(1) : in(n, "Color", 3);
	if (type == "FRESNEL") return Field::C1(0.08f);
	if (type == "LAYER_WEIGHT") return Field::C1(sock == "Fresnel" ? 0.08f : 0.1f);
	if (type == "LIGHT_PATH") return Field::C1(sock == "Is Camera Ray" ? C.cameraRay : 0.0f);
	if (type == "OBJECT_INFO") return sock == "Random" ? Field::C1(0.5f) : sock == "Color" ? Field::C3(1, 1, 1) : Field::C1(0);
	if (type == "TEX_CHECKER") {
		Field v = linked(n, "Vector") ? in(n, "Vector", 3) : uv(), a = in(n, "Color1", 3), b = in(n, "Color2", 3), sc = in(n, "Scale", 1);
		bool fac = sock == "Fac";
		return Map(C, fac ? 1 : 3, { &v, &a, &b, &sc }, [&](size_t i, float* o) {
			float s = sc.at(i, 0);
			int k = ((int)floorf(v.at(i, 0) * s) + (int)floorf(v.at(i, 1) * s) + (int)floorf(v.at(i, 2) * s)) & 1;
			if (fac) o[0] = (float)k; else for (int c = 0; c < 3; c++) o[c] = k ? a.at(i, c) : b.at(i, c);
		});
	}
	if (type.rfind("TEX_", 0) == 0) { C.warn("procedural " + type + " approximated"); return sock == "Fac" || sock == "Distance" ? Field::C1(0.5f) : Field::C3(0.5f, 0.5f, 0.5f); }
	C.warn("unsupported node " + type);
	return Field::C1(0);
}

// ------------------------------------------------------------------------------------------------ surface
struct Surface {
	Field base = Field::C3(0.8f, 0.8f, 0.8f), alpha = Field::C1(1), metal = Field::C1(0), rough = Field::C1(0.5f);
	Field normal = Field::C3(0, 0, 1), emission = Field::C3(0, 0, 0);
	bool transparentBlend = false;
};

static Field Lerp(NodeCtx& C, const Field& a, const Field& b, const Field& t, int n)
{
	Field A = Broadcast(a, n), B = Broadcast(b, n), T = Broadcast(t, 1);
	return Map(C, n, { &A, &B, &T }, [&](size_t i, float* o) { float f = Clamp01(T.at(i, 0)); for (int k = 0; k < n; k++) o[k] = A.at(i, k) * (1 - f) + B.at(i, k) * f; });
}

static Surface EvalShader(NodeGraph& g, const std::string& name, int depth = 0)
{
	Surface s;
	const Json* np = g.node(name);
	if (!np || depth > 16) return s;
	const Json& n = *np;
	std::string type = n["type"].str();
	NodeCtx& C = *g.ctx;
	if (type == "BSDF_PRINCIPLED") {
		s.base = g.in(n, "Base Color", 3);
		s.metal = g.in(n, "Metallic", 1);
		s.rough = g.in(n, "Roughness", 1);
		s.alpha = g.in(n, "Alpha", 1);
		if (g.linked(n, "Normal")) s.normal = g.in(n, "Normal", 3);
		Field ec = g.in(n, "Emission Color", 3), es = g.in(n, "Emission Strength", 1);
		s.emission = Map(C, 3, { &ec, &es }, [&](size_t i, float* o) { for (int k = 0; k < 3; k++) o[k] = ec.at(i, k) * es.at(i, 0); });
		return s;
	}
	if (type == "BSDF_DIFFUSE" || type == "BSDF_GLOSSY" || type == "BSDF_ANISOTROPIC" || type == "BSDF_TOON" || type == "BSDF_SHEEN" || type == "SUBSURFACE_SCATTERING") {
		s.base = g.in(n, "Color", 3);
		s.rough = g.in(n, "Roughness", 1);
		if (type == "BSDF_GLOSSY") s.metal = Field::C1(1);
		if (g.linked(n, "Normal")) s.normal = g.in(n, "Normal", 3);
		return s;
	}
	if (type == "BSDF_TRANSLUCENT") {   // foliage: light through leaves; shaded as a diffuse surface here
		s.base = g.in(n, "Color", 3);
		if (g.linked(n, "Normal")) s.normal = g.in(n, "Normal", 3);
		return s;
	}
	if (type == "BSDF_GLASS" || type == "BSDF_REFRACTION") {
		s.base = g.in(n, "Color", 3); s.rough = Field::C1(0.05f); s.alpha = Field::C1(0.3f); s.transparentBlend = true;
		return s;
	}
	if (type == "EMISSION") {
		Field c = g.in(n, "Color", 3), st = g.in(n, "Strength", 1);
		s.base = Field::C3(0, 0, 0);
		s.emission = Map(C, 3, { &c, &st }, [&](size_t i, float* o) { for (int k = 0; k < 3; k++) o[k] = c.at(i, k) * st.at(i, 0); });
		return s;
	}
	if (type == "BSDF_TRANSPARENT" || type == "HOLDOUT") { s.base = Field::C3(0, 0, 0); s.alpha = Field::C1(0); return s; }
	if (type == "MIX_SHADER" || type == "ADD_SHADER") {
		const Json* a = NodeGraph::input(n, "Shader"), * b = NodeGraph::input(n, "Shader_001");
		auto sub = [&](const Json* in) -> Surface {
			if (in && (*in)["link"].t == Json::ARR) return EvalShader(g, (*in)["link"][(size_t)0].str(), depth + 1);
			Surface e; e.base = Field::C3(0, 0, 0); e.alpha = Field::C1(0); return e;
		};
		Surface A = sub(a), B = sub(b);
		if (type == "ADD_SHADER") {
			A.emission = Map(C, 3, { &A.emission, &B.emission }, [&](size_t i, float* o) { for (int k = 0; k < 3; k++) o[k] = A.emission.at(i, k) + B.emission.at(i, k); });
			return A;
		}
		Field f = g.in(n, "Fac", 1);
		Surface r;
		r.base = Lerp(C, A.base, B.base, f, 3);
		r.alpha = Lerp(C, A.alpha, B.alpha, f, 1);
		r.metal = Lerp(C, A.metal, B.metal, f, 1);
		r.rough = Lerp(C, A.rough, B.rough, f, 1);
		r.normal = Lerp(C, A.normal, B.normal, f, 3);
		r.emission = Lerp(C, A.emission, B.emission, f, 3);
		r.transparentBlend = A.transparentBlend || B.transparentBlend;
		return r;
	}
	if (type == "GROUP") {
		// a group that outputs a shader: evaluate inside
		std::string tree = n["props"]["tree"].str();
		const Json* gt = C.groups ? &(*C.groups)[tree.c_str()] : nullptr;
		if (gt && gt->t == Json::OBJ) {
			NodeGraph sub; sub.nodes = &(*gt)["nodes"]; sub.ctx = g.ctx; sub.parent = &g; sub.groupNode = &n;
			for (auto& kv : sub.nodes->o) if (kv.second["type"].str() == "GROUP_OUTPUT")
				for (auto& si : kv.second["inputs"].a) if (si["type"].str() == "SHADER" && si["link"].t == Json::ARR)
					return EvalShader(sub, si["link"][(size_t)0].str(), depth + 1);
		}
		return s;
	}
	if (type == "REROUTE") {
		const Json* in = NodeGraph::input(n, "Input");
		if (in && (*in)["link"].t == Json::ARR) return EvalShader(g, (*in)["link"][(size_t)0].str(), depth + 1);
	}
	C.warn("shader " + type + " approximated");
	return s;
}

// images referenced by a graph (for picking the bake resolution)
static void CollectImages(const Json& nodes, const Json* groups, std::vector<std::string>& out, int depth = 0)
{
	if (depth > 8) return;
	for (auto& kv : nodes.o) {
		const Json& n = kv.second;
		if (n["type"].str() == "TEX_IMAGE" && n["props"]["image"].t == Json::STR) out.push_back(n["props"]["image"].s);
		if (n["type"].str() == "GROUP" && groups) {
			const Json& gt = (*groups)[n["props"]["tree"].str().c_str()];
			if (gt.t == Json::OBJ) CollectImages(gt["nodes"], groups, out, depth + 1);
		}
	}
}
