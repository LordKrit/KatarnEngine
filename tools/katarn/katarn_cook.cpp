// KatarnEngine cooker: Blender export (tools/katarn_export.py) -> maps/<name>.kworld + a collision .map for the BSP
//
//   katarn_cook <export folder> <output folder> <map name> [--units 40] [--maxtex 2048] [--quality 0..2] [--threads N]
//                [--collision-error 6] [--collision-min 24]
//
// .kworld: welded meshes with tangents, instances in Quake units, PBR materials, BC7/BC5 textures with mips, lights,
//          sun and fog (see code/rd-katarn/katarn/kworld_format.h)
// .map:    sky shell + simplified collision prisms (nodraw) + info_player_start, for q3map2 / WzMap -> .bsp
#define _CRT_SECURE_NO_WARNINGS
#include "../../code/rd-katarn/katarn/kworld_format.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_GIF
#define STBI_NO_PIC
#define STBI_NO_PNM
#include "../../../ext/stb/stb_image.h"
#include "../../../ext/bc7enc/bc7enc.h"
#define RGBCX_IMPLEMENTATION
#include "../../../ext/bc7enc/rgbcx.h"
#include "../../../ext/meshoptimizer/src/meshoptimizer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <thread>
#include <atomic>
#include <algorithm>
#include <memory>
#include <functional>

// ================================================================================================ tiny JSON
struct Json {
	enum T { NUL, BOOL, NUM, STR, ARR, OBJ } t = NUL;
	double n = 0; bool b = false; std::string s;
	std::vector<Json> a;
	std::vector<std::pair<std::string, Json>> o;
	const Json& operator[](const char* k) const { static Json none; for (auto& p : o) if (p.first == k) return p.second; return none; }
	const Json& operator[](size_t i) const { static Json none; return i < a.size() ? a[i] : none; }
	bool has(const char* k) const { for (auto& p : o) if (p.first == k) return true; return false; }
	double num(double d = 0) const { return t == NUM ? n : t == BOOL ? (b ? 1 : 0) : d; }
	std::string str(const char* d = "") const { return t == STR ? s : d; }
	size_t size() const { return t == ARR ? a.size() : o.size(); }
};
struct JsonParser {
	const char* p;
	void ws() { while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++; }
	std::string strv() {
		std::string r; p++;
		while (*p && *p != '"') {
			if (*p == '\\') {
				p++;
				switch (*p) {
				case 'n': r += '\n'; break; case 't': r += '\t'; break; case 'r': r += '\r'; break; case 'b': r += '\b'; break; case 'f': r += '\f'; break;
				case 'u': { unsigned c = (unsigned)strtoul(std::string(p + 1, 4).c_str(), nullptr, 16); p += 4;
					if (c < 0x80) r += (char)c; else if (c < 0x800) { r += (char)(0xC0 | (c >> 6)); r += (char)(0x80 | (c & 63)); }
					else { r += (char)(0xE0 | (c >> 12)); r += (char)(0x80 | ((c >> 6) & 63)); r += (char)(0x80 | (c & 63)); } break; }
				default: r += *p;
				}
				p++;
			}
			else r += *p++;
		}
		if (*p) p++;
		return r;
	}
	Json val() {
		ws();
		Json j;
		if (*p == '{') {
			j.t = Json::OBJ; p++; ws();
			if (*p == '}') { p++; return j; }
			while (*p) { ws(); std::string k = strv(); ws(); if (*p == ':') p++; j.o.push_back({ k, val() }); ws(); if (*p == ',') { p++; continue; } if (*p == '}') { p++; break; } }
		}
		else if (*p == '[') {
			j.t = Json::ARR; p++; ws();
			if (*p == ']') { p++; return j; }
			while (*p) { j.a.push_back(val()); ws(); if (*p == ',') { p++; continue; } if (*p == ']') { p++; break; } }
		}
		else if (*p == '"') { j.t = Json::STR; j.s = strv(); }
		else if (!strncmp(p, "true", 4)) { j.t = Json::BOOL; j.b = true; p += 4; }
		else if (!strncmp(p, "false", 5)) { j.t = Json::BOOL; p += 5; }
		else if (!strncmp(p, "null", 4)) { p += 4; }
		else if (!strncmp(p, "NaN", 3)) { j.t = Json::NUM; p += 3; }
		else if (!strncmp(p, "Infinity", 8)) { j.t = Json::NUM; j.n = 1e30; p += 8; }
		else if (!strncmp(p, "-Infinity", 9)) { j.t = Json::NUM; j.n = -1e30; p += 9; }
		else { j.t = Json::NUM; char* e; j.n = strtod(p, &e); p = e; }
		return j;
	}
};

static std::string ReadFile(const std::string& path)
{
	FILE* f = fopen(path.c_str(), "rb");
	if (!f) return std::string();
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	std::string s(n, '\0');
	if (n > 0 && fread(&s[0], 1, n, f) != (size_t)n) s.clear();
	fclose(f);
	return s;
}

// ================================================================================================ options
static float UNITS = 40.0f;          // Quake units per Blender metre (player is 64 units tall)
static int MAXTEX = 2048;
static int QUALITY = 1;
static int THREADS = 0;
static float COLL_ERROR = 6.0f;      // collision simplification error, Quake units
static float COLL_MIN = 24.0f;       // objects smaller than this (largest side, units) get no collision

// ================================================================================================ textures
struct Image { int w = 0, h = 0; std::vector<float> px; };   // RGBA float, linear for colour data when decoded with srgb

static float SrgbToLin(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }
static float LinToSrgb(float c) { c = c < 0 ? 0 : c > 1 ? 1 : c; return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1 / 2.4f) - 0.055f; }

static std::map<std::string, std::shared_ptr<Image>> imageCache;
static std::shared_ptr<Image> LoadImage(const std::string& path)
{
	auto it = imageCache.find(path);
	if (it != imageCache.end()) return it->second;
	int w, h, n;
	std::shared_ptr<Image> img;
	if (stbi_is_hdr(path.c_str())) {
		float* d = stbi_loadf(path.c_str(), &w, &h, &n, 4);
		if (d) { img = std::make_shared<Image>(); img->w = w; img->h = h; img->px.assign(d, d + (size_t)w * h * 4); stbi_image_free(d); }
	}
	else {
		unsigned char* d = stbi_load(path.c_str(), &w, &h, &n, 4);
		if (d) {
			img = std::make_shared<Image>(); img->w = w; img->h = h; img->px.resize((size_t)w * h * 4);
			for (size_t i = 0; i < img->px.size(); i++) img->px[i] = d[i] / 255.0f;
			stbi_image_free(d);
		}
	}
	if (!img) printf("  ! cannot read %s (%s)\n", path.c_str(), stbi_failure_reason());
	imageCache[path] = img;
	return img;
}

// box resample to w x h
static Image Resample(const Image& s, int w, int h)
{
	Image d; d.w = w; d.h = h; d.px.assign((size_t)w * h * 4, 0);
	for (int y = 0; y < h; y++) {
		float y0 = (float)y * s.h / h, y1 = (float)(y + 1) * s.h / h;
		for (int x = 0; x < w; x++) {
			float x0 = (float)x * s.w / w, x1 = (float)(x + 1) * s.w / w;
			float acc[4] = { 0, 0, 0, 0 }, wsum = 0;
			for (int sy = (int)y0; sy < (int)ceilf(y1) && sy < s.h; sy++) {
				float wy = fminf(y1, sy + 1.0f) - fmaxf(y0, (float)sy);
				for (int sx = (int)x0; sx < (int)ceilf(x1) && sx < s.w; sx++) {
					float wx = fminf(x1, sx + 1.0f) - fmaxf(x0, (float)sx);
					float ww = wx * wy;
					const float* p = &s.px[((size_t)sy * s.w + sx) * 4];
					for (int c = 0; c < 4; c++) acc[c] += p[c] * ww;
					wsum += ww;
				}
			}
			float* q = &d.px[((size_t)y * w + x) * 4];
			for (int c = 0; c < 4; c++) q[c] = wsum > 0 ? acc[c] / wsum : 0;
		}
	}
	return d;
}

enum TexKind { TK_COLOR, TK_NORMAL, TK_DATA };

static void EncodeImage(const Image& img, TexKind kind, uint32_t format, std::vector<uint8_t>& out, uint32_t& mips)
{
	// mip chain (the image is linear for colour, raw for data/normals)
	std::vector<Image> chain;
	chain.push_back(img);
	while (chain.back().w > 1 || chain.back().h > 1) {
		const Image& p = chain.back();
		Image n = Resample(p, std::max(1, p.w / 2), std::max(1, p.h / 2));
		if (kind == TK_NORMAL)
			for (size_t i = 0; i < n.px.size(); i += 4) {
				float x = n.px[i] * 2 - 1, y = n.px[i + 1] * 2 - 1, z = n.px[i + 2] * 2 - 1;
				float l = sqrtf(x * x + y * y + z * z); if (l < 1e-6f) l = 1;
				n.px[i] = x / l * 0.5f + 0.5f; n.px[i + 1] = y / l * 0.5f + 0.5f; n.px[i + 2] = z / l * 0.5f + 0.5f;
			}
		chain.push_back(std::move(n));
	}
	mips = (uint32_t)chain.size();
	bc7enc_compress_block_params bp;
	bc7enc_compress_block_params_init(&bp);
	bp.m_uber_level = QUALITY >= 2 ? 2 : 0;
	bp.m_max_partitions_mode = QUALITY == 0 ? 0 : QUALITY == 1 ? 16 : BC7ENC_MAX_PARTITIONS1;
	bp.m_try_least_squares = QUALITY >= 1;
	if (kind != TK_COLOR) bc7enc_compress_block_params_init_linear_weights(&bp);
	for (const Image& m : chain) {
		int bw = (m.w + 3) / 4, bh = (m.h + 3) / 4;
		size_t blockBytes = format == 28 ? 0 : 16;
		size_t base = out.size();
		if (format == 28) {   // RGBA8
			out.resize(base + (size_t)m.w * m.h * 4);
			for (size_t i = 0; i < (size_t)m.w * m.h * 4; i++) {
				float v = m.px[i];
				if (kind == TK_COLOR && (i & 3) != 3) v = LinToSrgb(v);
				out[base + i] = (uint8_t)(std::min(1.0f, std::max(0.0f, v)) * 255.0f + 0.5f);
			}
			continue;
		}
		out.resize(base + (size_t)bw * bh * blockBytes);
		std::atomic<int> nextRow(0);
		int nt = THREADS > 0 ? THREADS : std::max(1u, std::thread::hardware_concurrency());
		std::vector<std::thread> pool;
		for (int t = 0; t < nt; t++) pool.emplace_back([&]() {
			uint8_t blk[64];
			for (int by; (by = nextRow++) < bh;) {
				for (int bx = 0; bx < bw; bx++) {
					for (int py = 0; py < 4; py++) for (int pxx = 0; pxx < 4; pxx++) {
						int sx = std::min(m.w - 1, bx * 4 + pxx), sy = std::min(m.h - 1, by * 4 + py);
						const float* s = &m.px[((size_t)sy * m.w + sx) * 4];
						uint8_t* d = &blk[(py * 4 + pxx) * 4];
						for (int c = 0; c < 4; c++) {
							float v = s[c];
							if (kind == TK_COLOR && c < 3) v = LinToSrgb(v);
							d[c] = (uint8_t)(std::min(1.0f, std::max(0.0f, v)) * 255.0f + 0.5f);
						}
					}
					uint8_t* dst = &out[base + ((size_t)by * bw + bx) * 16];
					if (format == 83) rgbcx::encode_bc5(dst, blk, 0, 1, 4);
					else bc7enc_compress_block(dst, blk, &bp);
				}
			}
		});
		for (auto& th : pool) th.join();
	}
}

struct TexOut { KwTexture k; std::vector<uint8_t> data; };
static std::vector<TexOut> textures;
static std::map<std::string, int> texIndex;
static std::string exportDir;

static int FitSize(int s) { int r = std::min(s, MAXTEX); r = std::max(4, (r + 3) & ~3); return r; }

// a texture made from one or more channels of source images
struct ChanSrc { std::string file; int chan = -1; bool alphaOut = false; };   // chan -1 = luminance / rgb
static int MakeTexture(const std::string& key, TexKind kind, const std::function<void(Image&)>& build, int w, int h)
{
	auto it = texIndex.find(key);
	if (it != texIndex.end()) return it->second;
	Image img; img.w = w; img.h = h; img.px.assign((size_t)w * h * 4, 0);
	build(img);
	TexOut t = {};
	uint32_t fmt = kind == TK_NORMAL ? 83 : 98;
	EncodeImage(img, kind, fmt, t.data, t.k.mips);
	t.k.width = w; t.k.height = h; t.k.format = fmt; t.k.srgb = kind == TK_COLOR ? 1 : 0;
	int idx = (int)textures.size();
	textures.push_back(std::move(t));
	texIndex[key] = idx;
	return idx;
}

static std::shared_ptr<Image> LoadFitted(const std::string& file, bool srgb, int& w, int& h)
{
	auto src = LoadImage(exportDir + "/textures/" + file);
	if (!src) return nullptr;
	w = FitSize(src->w); h = FitSize(src->h);
	auto key = file + (srgb ? "#lin" : "#raw") + std::to_string(w) + "x" + std::to_string(h);
	auto it = imageCache.find(key);
	if (it != imageCache.end()) return it->second;
	Image conv = *src;
	if (srgb) for (size_t i = 0; i < conv.px.size(); i++) if ((i & 3) != 3) conv.px[i] = SrgbToLin(conv.px[i]);
	auto r = std::make_shared<Image>((w == conv.w && h == conv.h) ? conv : Resample(conv, w, h));
	imageCache[key] = r;
	return r;
}

// ================================================================================================ materials
static float Num(const Json& j, float d) { return j.t == Json::NUM ? (float)j.n : j.t == Json::ARR && j.size() ? (float)j[(size_t)0].num(d) : d; }
static std::string TexFile(const Json& t) { return t.t == Json::OBJ ? t["file"].str() : std::string(); }
static int TexChan(const Json& t)
{
	if (t.t != Json::OBJ) return -1;
	if (t["chan"].t == Json::NUM) return (int)t["chan"].n;
	if (t["out"].str() == "Alpha") return 3;
	return -1;
}
static float Pick(const float* p, int chan) { return chan >= 0 && chan < 4 ? p[chan] : (p[0] * 0.2126f + p[1] * 0.7152f + p[2] * 0.0722f); }

static std::vector<KwMaterial> materials;
static std::map<std::string, uint32_t> materialIndex;

static void CookMaterial(const std::string& name, const Json& m)
{
	KwMaterial k = {};
	const Json& bc = m["baseColor"];
	for (int i = 0; i < 4; i++) k.baseColor[i] = bc.t == Json::ARR && bc.size() > (size_t)i ? (float)bc[i].num(1) : (i == 3 ? 1.0f : 0.8f);
	k.roughness = Num(m["roughness"], 0.5f);
	k.metallic = Num(m["metallic"], 0.0f);
	k.normalScale = Num(m["normalStrength"], 1.0f);
	k.alphaCutoff = 0.5f;
	k.albedoTex = k.normalTex = k.ormTex = k.emissiveTex = -1;
	std::string mode = m["alphaMode"].str("OPAQUE");
	if (mode == "MASK") k.flags |= KWM_MASK;
	if (mode == "BLEND") k.flags |= KWM_MASK | KWM_BLEND;      // drawn as cut-out for now
	if (m["twoSided"].t == Json::BOOL && m["twoSided"].b) k.flags |= KWM_TWOSIDED;
	if (k.flags & KWM_MASK) k.flags |= KWM_TWOSIDED;

	// albedo (+ alpha from the albedo texture's alpha, or a separate alpha texture)
	std::string af = TexFile(m["baseColorTex"]);
	std::string alf = TexFile(m["alphaTex"]);
	int alc = TexChan(m["alphaTex"]);
	if (!af.empty()) {
		int w, h;
		auto img = LoadFitted(af, true, w, h);
		if (img) {
			std::shared_ptr<Image> aimg;
			if (!alf.empty() && alf != af) { int aw, ah; aimg = LoadFitted(alf, false, aw, ah); if (aimg && (aw != w || ah != h)) aimg = std::make_shared<Image>(Resample(*aimg, w, h)); }
			k.albedoTex = MakeTexture("albedo:" + af + "|" + alf, TK_COLOR, [&](Image& o) {
				for (size_t i = 0; i < o.px.size(); i += 4) {
					o.px[i] = img->px[i]; o.px[i + 1] = img->px[i + 1]; o.px[i + 2] = img->px[i + 2];
					o.px[i + 3] = aimg ? Pick(&aimg->px[i], alc) : (alf == af ? img->px[i + 3] : (k.flags & KWM_MASK) ? img->px[i + 3] : 1.0f);
				}
			}, w, h);
			k.baseColor[0] = k.baseColor[1] = k.baseColor[2] = 1.0f;
		}
	}
	if (m["alpha"].t == Json::NUM) k.baseColor[3] = (float)m["alpha"].n;
	// normal (or bump -> normal)
	std::string nf = TexFile(m["normalTex"]);
	if (!nf.empty()) {
		int w, h;
		auto img = LoadFitted(nf, false, w, h);
		if (img) k.normalTex = MakeTexture("normal:" + nf, TK_NORMAL, [&](Image& o) { o.px = img->px; }, w, h);
	}
	else if (!TexFile(m["bumpTex"]).empty()) {
		std::string bf = TexFile(m["bumpTex"]);
		int w, h;
		auto img = LoadFitted(bf, false, w, h);
		float s = Num(m["bumpStrength"], 1.0f) * 4.0f;
		if (img) k.normalTex = MakeTexture("bump:" + bf, TK_NORMAL, [&](Image& o) {
			auto H = [&](int x, int y) { x = (x % w + w) % w; y = (y % h + h) % h; return Pick(&img->px[((size_t)y * w + x) * 4], -1); };
			for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
				float dx = (H(x + 1, y) - H(x - 1, y)) * s, dy = (H(x, y + 1) - H(x, y - 1)) * s;
				float nx = -dx, ny = dy, nz = 1, l = sqrtf(nx * nx + ny * ny + nz * nz);
				float* q = &o.px[((size_t)y * w + x) * 4];
				q[0] = nx / l * 0.5f + 0.5f; q[1] = ny / l * 0.5f + 0.5f; q[2] = nz / l * 0.5f + 0.5f; q[3] = 1;
			}
		}, w, h);
	}
	// ORM: R ao (1), G roughness, B metallic
	std::string rf = TexFile(m["roughnessTex"]), mf = TexFile(m["metallicTex"]);
	if (!rf.empty() || !mf.empty()) {
		int w = 4, h = 4, rw, rh, mw, mh;
		std::shared_ptr<Image> ri, mi;
		if (!rf.empty()) { ri = LoadFitted(rf, false, rw, rh); if (ri) { w = rw; h = rh; } }
		if (!mf.empty()) { mi = LoadFitted(mf, false, mw, mh); if (mi && !ri) { w = mw; h = mh; } }
		if (mi && (mi->w != w || mi->h != h)) mi = std::make_shared<Image>(Resample(*mi, w, h));
		int rc = TexChan(m["roughnessTex"]), mc = TexChan(m["metallicTex"]);
		if (ri || mi) {
			k.ormTex = MakeTexture("orm:" + rf + "#" + std::to_string(rc) + "|" + mf + "#" + std::to_string(mc), TK_DATA, [&](Image& o) {
				for (size_t i = 0; i < o.px.size(); i += 4) {
					o.px[i] = 1.0f;
					o.px[i + 1] = ri ? Pick(&ri->px[i], rc) : 1.0f;
					o.px[i + 2] = mi ? Pick(&mi->px[i], mc) : 1.0f;
					o.px[i + 3] = 1.0f;
				}
			}, w, h);
			if (ri) k.roughness = 1.0f;
			if (mi) k.metallic = 1.0f;
		}
	}
	// emission
	float es = Num(m["emissionStrength"], 0.0f);
	const Json& ec = m["emission"];
	float e[3] = { 0, 0, 0 };
	if (ec.t == Json::ARR) for (int i = 0; i < 3; i++) e[i] = (float)ec[i].num(0) * es;
	std::string ef = TexFile(m["emissionTex"]);
	if (!ef.empty() && es > 0) {
		int w, h;
		auto img = LoadFitted(ef, true, w, h);
		if (img) k.emissiveTex = MakeTexture("emissive:" + ef, TK_COLOR, [&](Image& o) { o.px = img->px; }, w, h);
		if (ec.t != Json::ARR) e[0] = e[1] = e[2] = es;
	}
	// Blender emission strength 1 = "as bright as a white diffuse surface lit by 1 W/m2" -> scale to the sun
	for (int i = 0; i < 3; i++) k.emissive[i] = e[i] * 2.0f;
	if (m["shader"].str() == "EMISSION") { k.baseColor[0] = k.baseColor[1] = k.baseColor[2] = 0; }
	materialIndex[name] = (uint32_t)materials.size();
	materials.push_back(k);
}

// ================================================================================================ geometry
struct InVert { float p[3], n[3], uv[2]; };

static void Tangents(const std::vector<InVert>& v, const std::vector<uint32_t>& idx, std::vector<KwVertex>& out)
{
	size_t nv = v.size();
	std::vector<float> tan(nv * 3, 0), bit(nv * 3, 0);
	for (size_t i = 0; i + 2 < idx.size(); i += 3) {
		const InVert& a = v[idx[i]], & b = v[idx[i + 1]], & c = v[idx[i + 2]];
		float e1[3] = { b.p[0] - a.p[0], b.p[1] - a.p[1], b.p[2] - a.p[2] }, e2[3] = { c.p[0] - a.p[0], c.p[1] - a.p[1], c.p[2] - a.p[2] };
		float du1 = b.uv[0] - a.uv[0], dv1 = b.uv[1] - a.uv[1], du2 = c.uv[0] - a.uv[0], dv2 = c.uv[1] - a.uv[1];
		float r = du1 * dv2 - du2 * dv1;
		if (fabsf(r) < 1e-12f) continue;
		r = 1.0f / r;
		float t[3], bb[3];
		for (int k = 0; k < 3; k++) { t[k] = (e1[k] * dv2 - e2[k] * dv1) * r; bb[k] = (e2[k] * du1 - e1[k] * du2) * r; }
		for (int j = 0; j < 3; j++) for (int k = 0; k < 3; k++) { tan[idx[i + j] * 3 + k] += t[k]; bit[idx[i + j] * 3 + k] += bb[k]; }
	}
	out.resize(nv);
	for (size_t i = 0; i < nv; i++) {
		KwVertex& o = out[i];
		memcpy(o.pos, v[i].p, 12);
		float n[3] = { v[i].n[0], v[i].n[1], v[i].n[2] };
		float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]); if (l < 1e-8f) { n[0] = 0; n[1] = 0; n[2] = 1; l = 1; }
		for (int k = 0; k < 3; k++) n[k] /= l;
		memcpy(o.normal, n, 12);
		float* t = &tan[i * 3];
		float d = t[0] * n[0] + t[1] * n[1] + t[2] * n[2];
		float tt[3] = { t[0] - n[0] * d, t[1] - n[1] * d, t[2] - n[2] * d };
		float tl = sqrtf(tt[0] * tt[0] + tt[1] * tt[1] + tt[2] * tt[2]);
		if (tl < 1e-8f) {   // any perpendicular
			float a[3] = { fabsf(n[0]) < 0.9f ? 1.0f : 0.0f, fabsf(n[0]) < 0.9f ? 0.0f : 1.0f, 0 };
			tt[0] = a[1] * n[2] - a[2] * n[1]; tt[1] = a[2] * n[0] - a[0] * n[2]; tt[2] = a[0] * n[1] - a[1] * n[0];
			tl = sqrtf(tt[0] * tt[0] + tt[1] * tt[1] + tt[2] * tt[2]);
		}
		for (int k = 0; k < 3; k++) o.tangent[k] = tt[k] / tl;
		float* b = &bit[i * 3];
		float cx = n[1] * o.tangent[2] - n[2] * o.tangent[1], cy = n[2] * o.tangent[0] - n[0] * o.tangent[2], cz = n[0] * o.tangent[1] - n[1] * o.tangent[0];
		o.tangent[3] = (cx * b[0] + cy * b[1] + cz * b[2]) < 0 ? -1.0f : 1.0f;
		// Blender UV origin is bottom-left, D3D top-left
		o.uv[0] = v[i].uv[0]; o.uv[1] = 1.0f - v[i].uv[1];
	}
}

static void Xform(const float* m, const float* p, float* o)   // 3-row matrix
{
	for (int r = 0; r < 3; r++) o[r] = m[r * 4] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2] + m[r * 4 + 3];
}

// ================================================================================================ main
int main(int argc, char** argv)
{
	if (argc < 4) {
		printf("usage: katarn_cook <export folder> <output folder> <map name> [--units 40] [--maxtex 2048] [--quality 0-2] [--threads N]\n"
			"                    [--collision-error 6] [--collision-min 24]\n");
		return 1;
	}
	exportDir = argv[1];
	std::string outDir = argv[2], mapName = argv[3];
	for (int i = 4; i + 1 < argc; i += 2) {
		std::string a = argv[i];
		if (a == "--units") UNITS = (float)atof(argv[i + 1]);
		else if (a == "--maxtex") MAXTEX = atoi(argv[i + 1]);
		else if (a == "--quality") QUALITY = atoi(argv[i + 1]);
		else if (a == "--threads") THREADS = atoi(argv[i + 1]);
		else if (a == "--collision-error") COLL_ERROR = (float)atof(argv[i + 1]);
		else if (a == "--collision-min") COLL_MIN = (float)atof(argv[i + 1]);
	}
	std::string js = ReadFile(exportDir + "/scene.json");
	if (js.empty()) { printf("no %s/scene.json\n", exportDir.c_str()); return 1; }
	JsonParser jp{ js.c_str() };
	Json scene = jp.val();
	bc7enc_compress_block_init();
	rgbcx::init();
	printf("KatarnEngine cook: %s -> %s/%s.kworld (1 m = %g units, textures up to %d, quality %d)\n",
		exportDir.c_str(), outDir.c_str(), mapName.c_str(), UNITS, MAXTEX, QUALITY);

	// ---- materials
	const Json& mats = scene["materials"];
	for (size_t i = 0; i < mats.o.size(); i++) {
		printf("\r  material %zu/%zu  ", i + 1, mats.o.size()); fflush(stdout);
		CookMaterial(mats.o[i].first, mats.o[i].second);
	}
	printf("\n  %zu materials, %zu textures\n", materials.size(), textures.size());
	imageCache.clear();

	// ---- meshes
	const Json& meshes = scene["meshes"];
	std::vector<KwVertex> allV;
	std::vector<uint32_t> allI;
	std::vector<KwMesh> kmeshes;
	std::vector<KwSubmesh> ksubs;
	std::vector<std::vector<float>> collPos(meshes.size());          // simplified collision geometry per mesh (object space)
	std::vector<std::vector<uint32_t>> collIdx(meshes.size());
	std::map<std::string, FILE*> packs;
	for (size_t mi = 0; mi < meshes.size(); mi++) {
		const Json& m = meshes[mi];
		std::string pack = m["pack"].str();
		FILE*& pf = packs[pack];
		if (!pf) pf = fopen((exportDir + "/" + pack).c_str(), "rb");
		uint32_t nv = (uint32_t)m["vertexCount"].num(), ni = (uint32_t)m["indexCount"].num();
		std::vector<InVert> v(nv);
		std::vector<uint32_t> idx(ni);
		if (pf) {
#ifdef _WIN32
			_fseeki64(pf, (long long)m["offset"].num(), SEEK_SET);
#else
			fseeko(pf, (off_t)m["offset"].num(), SEEK_SET);
#endif
			if (fread(v.data(), sizeof(InVert), nv, pf) != nv || fread(idx.data(), 4, ni, pf) != ni) { printf("  ! short read in %s\n", pack.c_str()); continue; }
		}
		std::vector<KwVertex> kv;
		Tangents(v, idx, kv);
		KwMesh km = {};
		km.firstSubmesh = (uint32_t)ksubs.size();
		km.baseVertex = (uint32_t)allV.size();
		km.numVertices = nv;
		const Json& b = m["bounds"];
		for (int k = 0; k < 3; k++) { km.boundsMin[k] = (float)b[(size_t)0][k].num(); km.boundsMax[k] = (float)b[1][k].num(); }
		uint32_t base = (uint32_t)allI.size();
		// per submesh: vertex cache order
		for (size_t si = 0; si < m["submeshes"].size(); si++) {
			const Json& s = m["submeshes"][si];
			uint32_t st = (uint32_t)s["start"].num(), cnt = (uint32_t)s["count"].num();
			std::vector<uint32_t> opt(cnt);
			meshopt_optimizeVertexCache(opt.data(), idx.data() + st, cnt, nv);
			KwSubmesh ks = {};
			ks.firstIndex = (uint32_t)allI.size();
			ks.indexCount = cnt;
			auto it = materialIndex.find(s["material"].str());
			ks.material = it != materialIndex.end() ? it->second : 0;
			allI.insert(allI.end(), opt.begin(), opt.end());
			ksubs.push_back(ks);
			km.numSubmeshes++;
		}
		(void)base;
		allV.insert(allV.end(), kv.begin(), kv.end());
		kmeshes.push_back(km);
		// collision: simplified, solid materials only (no cut-out foliage)
		{
			std::vector<uint32_t> solid;
			for (size_t si = 0; si < m["submeshes"].size(); si++) {
				const Json& s = m["submeshes"][si];
				auto it = materialIndex.find(s["material"].str());
				if (it != materialIndex.end() && (materials[it->second].flags & KWM_MASK)) continue;
				uint32_t st = (uint32_t)s["start"].num(), cnt = (uint32_t)s["count"].num();
				solid.insert(solid.end(), idx.begin() + st, idx.begin() + st + cnt);
			}
			std::vector<float> pos(nv * 3);
			for (uint32_t i = 0; i < nv; i++) memcpy(&pos[i * 3], v[i].p, 12);
			if (!solid.empty()) {
				std::vector<uint32_t> simp(solid.size());
				float ext = 0;
				for (int k = 0; k < 3; k++) ext = std::max(ext, km.boundsMax[k] - km.boundsMin[k]);
				float relErr = ext > 0 ? (COLL_ERROR / UNITS) / ext : 0.01f;
				size_t n = meshopt_simplify(simp.data(), solid.data(), solid.size(), pos.data(), nv, 12, 0, relErr, meshopt_SimplifyLockBorder);
				if (n == 0 || n > solid.size()) n = 0;
				simp.resize(n);
				collIdx[mi] = simp;
				collPos[mi] = pos;
			}
		}
		if (mi % 50 == 0 || mi + 1 == meshes.size()) { printf("\r  mesh %zu/%zu  ", mi + 1, meshes.size()); fflush(stdout); }
	}
	for (auto& p : packs) if (p.second) fclose(p.second);
	printf("\n  %zu meshes, %zu vertices, %zu triangles\n", kmeshes.size(), allV.size(), allI.size() / 3);

	// ---- instances (Blender metres -> Quake units)
	const Json& insts = scene["instances"];
	std::vector<KwInstance> kinst;
	float wmin[3] = { 1e30f, 1e30f, 1e30f }, wmax[3] = { -1e30f, -1e30f, -1e30f };
	for (size_t i = 0; i < insts.size(); i++) {
		const Json& in = insts[i];
		KwInstance k = {};
		k.mesh = (uint32_t)in["mesh"].num();
		if (k.mesh >= kmeshes.size()) continue;
		const Json& M = in["matrix"];
		for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) k.m[r * 4 + c] = (float)M[(size_t)(r * 4 + c)].num() * UNITS;
		const KwMesh& km = kmeshes[k.mesh];
		for (int a = 0; a < 3; a++) { k.boundsMin[a] = 1e30f; k.boundsMax[a] = -1e30f; }
		for (int c = 0; c < 8; c++) {
			float p[3] = { (c & 1) ? km.boundsMax[0] : km.boundsMin[0], (c & 2) ? km.boundsMax[1] : km.boundsMin[1], (c & 4) ? km.boundsMax[2] : km.boundsMin[2] };
			float w[3]; Xform(k.m, p, w);
			for (int a = 0; a < 3; a++) { k.boundsMin[a] = std::min(k.boundsMin[a], w[a]); k.boundsMax[a] = std::max(k.boundsMax[a], w[a]); }
		}
		for (int a = 0; a < 3; a++) { wmin[a] = std::min(wmin[a], k.boundsMin[a]); wmax[a] = std::max(wmax[a], k.boundsMax[a]); }
		kinst.push_back(k);
	}
	printf("  %zu instances, world %.0f x %.0f x %.0f units\n", kinst.size(), wmax[0] - wmin[0], wmax[1] - wmin[1], wmax[2] - wmin[2]);

	// ---- lights + sun
	std::vector<KwLight> lights;
	float sunDir[3] = { 0.35f, 0.25f, 0.9f }, sunColor[3] = { 1.0f, 0.96f, 0.9f }, sunStrength = 5.0f, sunAngle = 0.0093f;
	bool haveSun = false;
	const Json& L = scene["lights"];
	for (size_t i = 0; i < L.size(); i++) {
		const Json& l = L[i];
		std::string type = l["type"].str();
		float col[3] = { (float)l["color"][(size_t)0].num(1), (float)l["color"][1].num(1), (float)l["color"][2].num(1) };
		float energy = (float)l["energy"].num(0);
		float dir[3] = { (float)l["direction"][(size_t)0].num(0), (float)l["direction"][1].num(0), (float)l["direction"][2].num(-1) };
		if (type == "SUN") {
			if (!haveSun || energy > sunStrength) {
				sunDir[0] = -dir[0]; sunDir[1] = -dir[1]; sunDir[2] = -dir[2];
				memcpy(sunColor, col, 12); sunStrength = energy; sunAngle = (float)l["angle"].num(0.0093) * 0.5f;
				haveSun = true;
			}
			continue;
		}
		if (energy <= 0) continue;
		KwLight k = {};
		for (int a = 0; a < 3; a++) k.pos[a] = (float)l["position"][(size_t)a].num() * UNITS;
		// Cycles: irradiance E = P / (4 pi d^2), d in metres -> d in units
		float I = energy / (4.0f * 3.14159265f) * UNITS * UNITS;
		if (type == "AREA") I *= 0.5f;    // one-sided
		k.invSquare = I;
		memcpy(k.color, col, 12);
		k.radius = std::min(sqrtf(I / 0.002f), 200.0f * UNITS);
		memcpy(k.dir, dir, 12);
		if (type == "SPOT") {
			float half = (float)l["spotSize"].num(0.785) * 0.5f;
			float blend = (float)l["spotBlend"].num(0.15);
			k.type = 1;
			k.spotCos = cosf(half);
			k.spotInnerCos = cosf(half * (1.0f - blend));
		}
		lights.push_back(k);
	}
	const Json& world = scene["world"];
	if (!haveSun && world["sky"].t == Json::OBJ) {
		float el = (float)world["sky"]["sun_elevation"].num(0.8), rot = (float)world["sky"]["sun_rotation"].num(0);
		sunDir[0] = cosf(el) * sinf(rot); sunDir[1] = -cosf(el) * cosf(rot); sunDir[2] = sinf(el);
		sunStrength = (float)world["sky"]["sun_intensity"].num(1.0) * 5.0f;
		haveSun = true;
	}
	printf("  %zu lights, sun %s (%.2f %.2f %.2f) strength %.2f\n", lights.size(), haveSun ? "from Blender" : "default", sunDir[0], sunDir[1], sunDir[2], sunStrength);

	// ---- environment
	float extent = std::max(wmax[0] - wmin[0], wmax[1] - wmin[1]);
	char env[2048];
	snprintf(env, sizeof(env),
		"sunDir %f %f %f\nsunColor %f %f %f\nsunIntensity %f\nsunAngle %f\nskyIntensity 1\nhaze 0.4\nambient 1\n"
		"fogDensity %g\nfogFalloff %g\nfogHeight %f\nfogStart %f\nfogColor 1 1 1\nfogMax 0.8\nexposureEV 0\n",
		sunDir[0], sunDir[1], sunDir[2], sunColor[0], sunColor[1], sunColor[2], std::max(0.5f, sunStrength), std::max(0.0047f, sunAngle),
		1.0f / (UNITS * 2500.0f), 1.0f / (UNITS * 120.0f), wmin[2], UNITS * 20.0f);

	// ---- write .kworld
	std::string kpath = outDir + "/" + mapName + ".kworld";
	FILE* f = fopen(kpath.c_str(), "wb");
	if (!f) { printf("cannot write %s\n", kpath.c_str()); return 1; }
	std::vector<KwTexture> ktex;
	uint64_t tdatSize = 0;
	for (auto& t : textures) { KwTexture k = t.k; k.offset = tdatSize; k.size = t.data.size(); tdatSize += (k.size + 255) & ~255ull; ktex.push_back(k); }
	struct Sec { uint32_t type; const void* data; uint64_t size; };
	std::vector<Sec> secs = {
		{ KW_VERT, allV.data(), allV.size() * sizeof(KwVertex) }, { KW_INDX, allI.data(), allI.size() * 4 },
		{ KW_MESH, kmeshes.data(), kmeshes.size() * sizeof(KwMesh) }, { KW_SUBM, ksubs.data(), ksubs.size() * sizeof(KwSubmesh) },
		{ KW_INST, kinst.data(), kinst.size() * sizeof(KwInstance) }, { KW_MATL, materials.data(), materials.size() * sizeof(KwMaterial) },
		{ KW_TEXT, ktex.data(), ktex.size() * sizeof(KwTexture) }, { KW_LITE, lights.data(), lights.size() * sizeof(KwLight) },
		{ KW_ENVT, env, strlen(env) }, { KW_TDAT, nullptr, tdatSize },
	};
	KwHeader h = {};
	memcpy(h.magic, KW_MAGIC, 8);
	h.version = KW_VERSION;
	h.numSections = (uint32_t)secs.size();
	memcpy(h.boundsMin, wmin, 12); memcpy(h.boundsMax, wmax, 12);
	std::vector<KwSection> table(secs.size());
	uint64_t off = sizeof(KwHeader) + sizeof(KwSection) * secs.size();
	for (size_t i = 0; i < secs.size(); i++) {
		off = (off + 255) & ~255ull;
		table[i].type = secs[i].type; table[i].offset = off; table[i].size = secs[i].size;
		off += secs[i].size;
	}
	fwrite(&h, sizeof(h), 1, f);
	fwrite(table.data(), sizeof(KwSection), table.size(), f);
	auto pad = [&](uint64_t to) { uint64_t cur = (uint64_t)ftell(f); static const char z[256] = {}; while (cur < to) { size_t n = (size_t)std::min<uint64_t>(256, to - cur); fwrite(z, 1, n, f); cur += n; } };
	for (size_t i = 0; i < secs.size(); i++) {
#ifdef _WIN32
		_fseeki64(f, 0, SEEK_END);
#endif
		pad(table[i].offset);
		if (secs[i].type == KW_TDAT) {
			for (auto& t : textures) {
				fwrite(t.data.data(), 1, t.data.size(), f);
				size_t p = t.data.size();
				static const char z[256] = {};
				size_t al = ((p + 255) & ~(size_t)255) - p;
				if (al) fwrite(z, 1, al, f);
			}
		}
		else if (secs[i].size) fwrite(secs[i].data, 1, secs[i].size, f);
	}
	long long total = ftell(f);
	fclose(f);
	printf("  wrote %s (%.1f MB, textures %.1f MB)\n", kpath.c_str(), total / 1048576.0, tdatSize / 1048576.0);

	// ---- collision .map
	std::string mpath = outDir + "/" + mapName + ".map";
	FILE* mf = fopen(mpath.c_str(), "w");
	if (!mf) { printf("cannot write %s\n", mpath.c_str()); return 1; }
	fprintf(mf, "// %s - collision and entities for the KatarnEngine HD world %s.kworld (visible geometry is not in the BSP)\n", mapName.c_str(), mapName.c_str());
	fprintf(mf, "{\n\"classname\" \"worldspawn\"\n\"message\" \"%s (KatarnEngine HD world)\"\n\"_katarn\" \"1\"\n", mapName.c_str());
	// sky shell
	float pad2 = 512.0f, T = 64.0f;
	float lo[3] = { floorf(wmin[0] - pad2), floorf(wmin[1] - pad2), floorf(wmin[2] - pad2) };
	float hi[3] = { ceilf(wmax[0] + pad2), ceilf(wmax[1] + pad2), ceilf(wmax[2] + pad2 * 4) };
	auto box = [&](float x0, float y0, float z0, float x1, float y1, float z1, const char* sh, unsigned contents) {
		fprintf(mf, "{\n");
		const char* t = sh;
		fprintf(mf, "( %g %g %g ) ( %g %g %g ) ( %g %g %g ) %s 0 0 0 0.5 0.5 %u 0 0\n", x0, y0, z1, x0, y1, z1, x1, y1, z1, t, contents);
		fprintf(mf, "( %g %g %g ) ( %g %g %g ) ( %g %g %g ) %s 0 0 0 0.5 0.5 %u 0 0\n", x0, y1, z0, x0, y0, z0, x1, y0, z0, t, contents);
		fprintf(mf, "( %g %g %g ) ( %g %g %g ) ( %g %g %g ) %s 0 0 0 0.5 0.5 %u 0 0\n", x0, y0, z0, x0, y1, z0, x0, y1, z1, t, contents);
		fprintf(mf, "( %g %g %g ) ( %g %g %g ) ( %g %g %g ) %s 0 0 0 0.5 0.5 %u 0 0\n", x1, y1, z0, x1, y0, z0, x1, y0, z1, t, contents);
		fprintf(mf, "( %g %g %g ) ( %g %g %g ) ( %g %g %g ) %s 0 0 0 0.5 0.5 %u 0 0\n", x1, y0, z0, x0, y0, z0, x0, y0, z1, t, contents);
		fprintf(mf, "( %g %g %g ) ( %g %g %g ) ( %g %g %g ) %s 0 0 0 0.5 0.5 %u 0 0\n", x0, y1, z0, x1, y1, z0, x1, y1, z1, t, contents);
		fprintf(mf, "}\n");
	};
	const char* sky = "katarn/sky";
	box(lo[0] - T, lo[1] - T, lo[2] - T, hi[0] + T, hi[1] + T, lo[2], sky, 0);
	box(lo[0] - T, lo[1] - T, hi[2], hi[0] + T, hi[1] + T, hi[2] + T, sky, 0);
	box(lo[0] - T, lo[1] - T, lo[2], lo[0], hi[1] + T, hi[2], sky, 0);
	box(hi[0], lo[1] - T, lo[2], hi[0] + T, hi[1] + T, hi[2], sky, 0);
	box(lo[0], lo[1] - T, lo[2], hi[0], lo[1], hi[2], sky, 0);
	box(lo[0], hi[1], lo[2], hi[0], hi[1] + T, hi[2], sky, 0);
	// collision prisms
	size_t brushes = 0, skipped = 0;
	std::vector<float> groundTris;   // for the spawn drop
	const float thick = 8.0f;
	const unsigned DETAIL = 0x8000000;
	for (const KwInstance& k : kinst) {
		float ext = 0;
		for (int a = 0; a < 3; a++) ext = std::max(ext, k.boundsMax[a] - k.boundsMin[a]);
		if (ext < COLL_MIN) { skipped++; continue; }
		const std::vector<uint32_t>& ci = collIdx[k.mesh];
		const std::vector<float>& cp = collPos[k.mesh];
		for (size_t t = 0; t + 2 < ci.size(); t += 3) {
			float a[3], b[3], c[3];
			Xform(k.m, &cp[ci[t] * 3], a); Xform(k.m, &cp[ci[t + 1] * 3], b); Xform(k.m, &cp[ci[t + 2] * 3], c);
			float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
			float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
			float area2 = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
			auto len = [](const float* p, const float* q) { return sqrtf((p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2])); };
			float lmax = std::max(len(a, b), std::max(len(b, c), len(c, a)));
			if (area2 < 4.0f || lmax < 2.0f || area2 / lmax < 1.0f) continue;   // degenerate / sliver
			for (int q = 0; q < 3; q++) n[q] /= area2;
			if (n[2] > 0.5f) { groundTris.insert(groundTris.end(), a, a + 3); groundTris.insert(groundTris.end(), b, b + 3); groundTris.insert(groundTris.end(), c, c + 3); }
			float a2[3], b2[3], c2[3], an[3], bn[3];
			for (int q = 0; q < 3; q++) { a2[q] = a[q] - n[q] * thick; b2[q] = b[q] - n[q] * thick; c2[q] = c[q] - n[q] * thick; an[q] = a[q] + n[q] * 16; bn[q] = b[q] + n[q] * 16; }
			float cn[3] = { c[0] + n[0] * 16, c[1] + n[1] * 16, c[2] + n[2] * 16 };
			const char* cs = "katarn/collision";
			fprintf(mf, "{\n");
			fprintf(mf, "( %.3f %.3f %.3f ) ( %.3f %.3f %.3f ) ( %.3f %.3f %.3f ) %s 0 0 0 0.5 0.5 %u 0 0\n", a[0], a[1], a[2], c[0], c[1], c[2], b[0], b[1], b[2], cs, DETAIL);
			fprintf(mf, "( %.3f %.3f %.3f ) ( %.3f %.3f %.3f ) ( %.3f %.3f %.3f ) %s 0 0 0 0.5 0.5 %u 0 0\n", a2[0], a2[1], a2[2], b2[0], b2[1], b2[2], c2[0], c2[1], c2[2], cs, DETAIL);
			fprintf(mf, "( %.3f %.3f %.3f ) ( %.3f %.3f %.3f ) ( %.3f %.3f %.3f ) %s 0 0 0 0.5 0.5 %u 0 0\n", a[0], a[1], a[2], an[0], an[1], an[2], b[0], b[1], b[2], cs, DETAIL);
			fprintf(mf, "( %.3f %.3f %.3f ) ( %.3f %.3f %.3f ) ( %.3f %.3f %.3f ) %s 0 0 0 0.5 0.5 %u 0 0\n", b[0], b[1], b[2], bn[0], bn[1], bn[2], c[0], c[1], c[2], cs, DETAIL);
			fprintf(mf, "( %.3f %.3f %.3f ) ( %.3f %.3f %.3f ) ( %.3f %.3f %.3f ) %s 0 0 0 0.5 0.5 %u 0 0\n", c[0], c[1], c[2], cn[0], cn[1], cn[2], a[0], a[1], a[2], cs, DETAIL);
			fprintf(mf, "}\n");
			brushes++;
		}
	}
	fprintf(mf, "}\n");
	// spawn: the Blender camera, dropped onto the ground below it
	float sp[3] = { (wmin[0] + wmax[0]) * 0.5f, (wmin[1] + wmax[1]) * 0.5f, wmax[2] };
	float yaw = 0;
	const Json& cam = scene["camera"];
	if (cam.t == Json::OBJ) {
		const Json& M = cam["matrix"];
		sp[0] = (float)M[(size_t)3].num() * UNITS; sp[1] = (float)M[(size_t)7].num() * UNITS; sp[2] = (float)M[(size_t)11].num() * UNITS;
		float fx = -(float)M[(size_t)2].num(), fy = -(float)M[(size_t)6].num();   // camera looks down -Z
		yaw = atan2f(fy, fx) * 57.2958f;
	}
	float best = -1e30f;
	for (size_t i = 0; i + 8 < groundTris.size(); i += 9) {
		const float* a = &groundTris[i], * b = &groundTris[i + 3], * c = &groundTris[i + 6];
		// point in triangle (xy) and its height there
		float d = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1]);
		if (fabsf(d) < 1e-6f) continue;
		float l1 = ((b[1] - c[1]) * (sp[0] - c[0]) + (c[0] - b[0]) * (sp[1] - c[1])) / d;
		float l2 = ((c[1] - a[1]) * (sp[0] - c[0]) + (a[0] - c[0]) * (sp[1] - c[1])) / d;
		float l3 = 1 - l1 - l2;
		if (l1 < 0 || l2 < 0 || l3 < 0) continue;
		float z = l1 * a[2] + l2 * b[2] + l3 * c[2];
		if (z <= sp[2] + 8 && z > best) best = z;
	}
	if (best > -1e29f) sp[2] = best + 40;
	fprintf(mf, "{\n\"classname\" \"info_player_start\"\n\"origin\" \"%.0f %.0f %.0f\"\n\"angle\" \"%.0f\"\n}\n", sp[0], sp[1], sp[2], yaw);
	fclose(mf);
	printf("  wrote %s: %zu collision brushes (%zu small objects without collision), spawn %.0f %.0f %.0f\n", mpath.c_str(), brushes, skipped, sp[0], sp[1], sp[2]);

	// ---- shader for the collision BSP
	std::string spath = outDir + "/katarn_hd.shader";
	FILE* sf = fopen(spath.c_str(), "w");
	if (sf) {
		fprintf(sf, "// KatarnEngine HD worlds: the BSP is collision only, the visible world is the .kworld\n"
			"textures/katarn/collision\n{\n\tqer_editorimage textures/common/clip\n\tsurfaceparm nodraw\n\tsurfaceparm nolightmap\n\tsurfaceparm nomarks\n\tsurfaceparm trans\n}\n\n"
			"textures/katarn/sky\n{\n\tqer_editorimage textures/common/sky\n\tsurfaceparm sky\n\tsurfaceparm noimpact\n\tsurfaceparm nolightmap\n\tsurfaceparm nomarks\n\tskyparms - - -\n}\n");
		fclose(sf);
	}
	return 0;
}
