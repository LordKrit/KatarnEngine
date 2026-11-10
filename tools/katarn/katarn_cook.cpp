// KatarnEngine cooker: Blender export (tools/katarn_export.py) -> maps/<name>.kworld + a collision .map for the BSP
//
//   katarn_cook <export folder> <output folder> <map name> [options]
//     --units 40            Quake units per Blender metre
//     --maxtex 2048         largest baked texture
//     --texbudget 2500      MB of texture memory for the whole world (resolutions are shared out by surface area)
//     --quality 0..2        BC7 encoder effort
//     --cell 2048           cluster cell size (units)
//     --lod0-error 0.25     LOD0 simplification error (units; removes invisible density)
//     --collision-error 8   collision geometry error (units)
//     --collision-budget 150000   max collision triangles
//     --skip "a,b"          object names containing these are left out (default: HeightFog,Sky_Cottonball)
//     --threads N
//
// .kworld: world-space clusters with LOD chains, per-vertex material ids, baked PBR materials (the Blender node graphs
//          are evaluated over UV space), BC7/BC5 textures, lights, sun, sky texture + SH ambient
// .map:    sky shell + collision prisms (nodraw) + info_player_start, for q3map2 / WzMap -> .bsp
#define _CRT_SECURE_NO_WARNINGS
#include "../../code/rd-katarn/katarn/kworld_format.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_GIF
#define STBI_NO_PIC
#define STBI_NO_PNM
#include "../../../ext/stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../../ext/stb/stb_image_write.h"
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
#include <mutex>
#include <algorithm>
#include <memory>
#include <functional>
#include <array>
#include <list>
#include <chrono>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <process.h>
#include <io.h>
#define kt_fseek _fseeki64
#else
#include <unistd.h>
#define kt_fseek fseeko
#endif

#include <stdarg.h>
static FILE* g_cookLog = nullptr;
static int CookPrintf(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int r = vprintf(fmt, ap);
	va_end(ap);
	fflush(stdout);
	if (g_cookLog && !strchr(fmt, '\r')) { va_start(ap, fmt); vfprintf(g_cookLog, fmt, ap); va_end(ap); fflush(g_cookLog); }
	return r;
}
#define printf CookPrintf

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

#include "node_eval.h"

// ================================================================================================ options
static float UNITS = 40.0f;
static int MAXTEX = 2048;
static float TEXBUDGET_MB = 2500.0f;
static int QUALITY = 1;
static int THREADS = 0;
static float CELL = 2048.0f;
static float LOD0_ERROR = 0.25f;
static float COLL_ERROR = 8.0f;
static size_t COLL_BUDGET = 150000;
static std::vector<std::string> SKIP = { "HeightFog", "Sky_Cottonball" };
static std::string exportDir;

// default: half the cores, so the PC stays usable (and cooler) during a long cook
static int NumThreads() { return THREADS > 0 ? THREADS : (int)std::max(1u, std::thread::hardware_concurrency() / 2); }

// ================================================================================================ memory
static int BANDS = 0;          // 0 = from the export size and the memory limit
static size_t MAXMEM_MB = 0;      // 0 = auto (60% of physical RAM)
static size_t ProcessMB()
{
#ifdef _WIN32
	PROCESS_MEMORY_COUNTERS pmc = {};
	if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return pmc.PagefileUsage >> 20;
	return 0;
#else
	FILE* f = fopen("/proc/self/statm", "r");
	unsigned long a = 0, r = 0;
	if (f) { if (fscanf(f, "%lu %lu", &a, &r) != 2) r = 0; fclose(f); }
	return (size_t)r * 4096 >> 20;
#endif
}
static size_t PhysicalMB()
{
#ifdef _WIN32
	MEMORYSTATUSEX ms = {}; ms.dwLength = sizeof(ms);
	if (GlobalMemoryStatusEx(&ms)) return (size_t)(ms.ullTotalPhys >> 20);
	return 8192;
#else
	long pages = sysconf(_SC_PHYS_PAGES), ps = sysconf(_SC_PAGE_SIZE);
	return pages > 0 ? (size_t)((uint64_t)pages * ps >> 20) : 8192;
#endif
}
static std::atomic<size_t> g_peakMB(0);
static std::atomic<bool> g_cookDone(false);
// stops the cook cleanly (with a message) before it can take the whole PC down
static void MemoryWatchdog()
{
	while (!g_cookDone) {
		size_t mb = ProcessMB();
		if (mb > g_peakMB) g_peakMB = mb;
		if (mb > MAXMEM_MB) {
			printf("\n\n  ! STOPPED: the cook is using %zu MB, over the %zu MB limit.\n"
				"    Try --threads 2, a smaller --texbudget, or raise the limit with --maxmem <MB>.\n", mb, MAXMEM_MB);
			fflush(stdout);
			_exit(3);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
	}
}

// ================================================================================================ spill file
// cooked geometry and texture data go to a temporary file as soon as they are finished, so the cook never holds
// the whole world in memory; the .kworld is assembled from it at the end.
static FILE* g_spill = nullptr;
static uint64_t g_spillPos = 0;
static std::mutex g_spillMutex;
static uint64_t SpillWrite(const void* data, size_t bytes)
{
	std::lock_guard<std::mutex> lk(g_spillMutex);
	uint64_t at = g_spillPos;
	kt_fseek(g_spill, (int64_t)at, SEEK_SET);
	if (bytes && fwrite(data, 1, bytes, g_spill) != bytes) { printf("\n  ! cannot write the spill file (disk full?)\n"); _exit(4); }
	g_spillPos += bytes;
	return at;
}
static void SpillRead(uint64_t at, void* data, size_t bytes)
{
	std::lock_guard<std::mutex> lk(g_spillMutex);
	kt_fseek(g_spill, (int64_t)at, SEEK_SET);
	if (bytes && fread(data, 1, bytes, g_spill) != bytes) { printf("\n  ! cannot read the spill file\n"); _exit(4); }
}
static void SpillCopy(uint64_t at, uint64_t bytes, FILE* out)
{
	std::vector<uint8_t> buf(16u << 20);
	while (bytes) {
		size_t n = (size_t)std::min<uint64_t>(buf.size(), bytes);
		SpillRead(at, buf.data(), n);
		fwrite(buf.data(), 1, n, out);
		at += n; bytes -= n;
	}
}
static double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

static void ParallelFor(size_t n, const std::function<void(size_t)>& fn, int threads = 0)
{
	int nt = threads > 0 ? threads : NumThreads();
	if (nt <= 1 || n <= 1) { for (size_t i = 0; i < n; i++) fn(i); return; }
	std::atomic<size_t> next(0);
	std::vector<std::thread> pool;
	for (int t = 0; t < nt; t++) pool.emplace_back([&]() { for (size_t i; (i = next++) < n;) fn(i); });
	for (auto& th : pool) th.join();
}

// ================================================================================================ images
static float SrgbToLin(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }
static float LinToSrgb(float c) { c = c < 0 ? 0 : c > 1 ? 1 : c; return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1 / 2.4f) - 0.055f; }

static BakeImage Resample(const BakeImage& s, int w, int h)
{
	BakeImage d; d.w = w; d.h = h; d.px.assign((size_t)w * h * 4, 0);
	for (int y = 0; y < h; y++) {
		float y0 = (float)y * s.h / h, y1 = (float)(y + 1) * s.h / h;
		for (int x = 0; x < w; x++) {
			float x0 = (float)x * s.w / w, x1 = (float)(x + 1) * s.w / w;
			float acc[4] = { 0, 0, 0, 0 }, wsum = 0;
			for (int sy = (int)y0; sy < (int)ceilf(y1) && sy < s.h; sy++) {
				float wy = fminf(y1, sy + 1.0f) - fmaxf(y0, (float)sy);
				for (int sx = (int)x0; sx < (int)ceilf(x1) && sx < s.w; sx++) {
					float wx = fminf(x1, sx + 1.0f) - fmaxf(x0, (float)sx);
					const float* p = &s.px[((size_t)sy * s.w + sx) * 4];
					for (int c = 0; c < 4; c++) acc[c] += p[c] * wx * wy;
					wsum += wx * wy;
				}
			}
			float* q = &d.px[((size_t)y * w + x) * 4];
			for (int c = 0; c < 4; c++) q[c] = wsum > 0 ? acc[c] / wsum : 0;
		}
	}
	return d;
}

// decoded source images (float RGBA, linearised for sRGB colour data), capped at 2048 and kept in an LRU cache
static std::mutex imgMutex;
static std::list<std::pair<std::string, std::shared_ptr<BakeImage>>> imgLru;
static size_t imgBytes = 0;
static std::map<std::string, std::pair<int, int>> imgDims;

static std::shared_ptr<BakeImage> LoadSource(const std::string& file, bool linearize)
{
	std::string key = file + (linearize ? "|lin" : "|raw");
	{
		std::lock_guard<std::mutex> lk(imgMutex);
		for (auto it = imgLru.begin(); it != imgLru.end(); ++it)
			if (it->first == key) { imgLru.splice(imgLru.begin(), imgLru, it); return it->second; }
	}
	std::string path = exportDir + "/textures/" + file;
	int w, h, n;
	auto img = std::make_shared<BakeImage>();
	if (stbi_is_hdr(path.c_str())) {
		float* d = stbi_loadf(path.c_str(), &w, &h, &n, 4);
		if (!d) return nullptr;
		img->w = w; img->h = h; img->px.assign(d, d + (size_t)w * h * 4);
		stbi_image_free(d);
	}
	else {
		unsigned char* d = stbi_load(path.c_str(), &w, &h, &n, 4);
		if (!d) { printf("  ! cannot read %s (%s)\n", file.c_str(), stbi_failure_reason()); return nullptr; }
		static float lut[256], lin[256];
		static bool init = false;
		if (!init) { for (int i = 0; i < 256; i++) { lut[i] = i / 255.0f; lin[i] = SrgbToLin(i / 255.0f); } init = true; }
		img->w = w; img->h = h; img->px.resize((size_t)w * h * 4);
		for (size_t i = 0; i < img->px.size(); i++) img->px[i] = (linearize && (i & 3) != 3) ? lin[d[i]] : lut[d[i]];
		stbi_image_free(d);
	}
	if (img->w > 2048 || img->h > 2048) {
		int nw = std::min(img->w, 2048), nh = std::max(1, img->h * nw / img->w);
		if (nh > 2048) { nh = 2048; nw = std::max(1, img->w * nh / img->h); }
		*img = Resample(*img, nw, nh);
	}
	std::lock_guard<std::mutex> lk(imgMutex);
	imgLru.push_front({ key, img });
	imgBytes += img->px.size() * 4;
	while (imgBytes > std::min<size_t>(3ull << 29, (MAXMEM_MB << 20) / 6) && imgLru.size() > 1) { imgBytes -= imgLru.back().second->px.size() * 4; imgLru.pop_back(); }
	return img;
}

static std::pair<int, int> SourceDims(const std::string& file)
{
	{
		std::lock_guard<std::mutex> lk(imgMutex);
		auto it = imgDims.find(file);
		if (it != imgDims.end()) return it->second;
	}
	int w = 0, h = 0, n = 0;
	stbi_info((exportDir + "/textures/" + file).c_str(), &w, &h, &n);
	std::lock_guard<std::mutex> lk(imgMutex);
	imgDims[file] = { w, h };
	return { w, h };
}

// ================================================================================================ texture encoding
enum TexKind { TK_COLOR, TK_NORMAL, TK_DATA };
struct TexOut { KwTexture k; std::vector<uint8_t> data; uint64_t spillAt = 0, bytes = 0; };
static std::vector<TexOut> textures;
static std::mutex texMutex;
static std::map<uint64_t, int> texByHash;

static void EncodeImage(const BakeImage& img, TexKind kind, uint32_t format, std::vector<uint8_t>& out, uint32_t& mips, bool parallel)
{
	std::vector<BakeImage> chain;
	chain.push_back(img);
	while (chain.back().w > 1 || chain.back().h > 1) {
		const BakeImage& p = chain.back();
		BakeImage n = Resample(p, std::max(1, p.w / 2), std::max(1, p.h / 2));
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
	for (const BakeImage& m : chain) {
		int bw = (m.w + 3) / 4, bh = (m.h + 3) / 4;
		size_t base = out.size();
		out.resize(base + (size_t)bw * bh * 16);
		auto row = [&](size_t by) {
			uint8_t blk[64];
			for (int bx = 0; bx < bw; bx++) {
				for (int py = 0; py < 4; py++) for (int pxx = 0; pxx < 4; pxx++) {
					int sx = std::min(m.w - 1, bx * 4 + pxx), sy = std::min(m.h - 1, (int)by * 4 + py);
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
		};
		if (parallel) ParallelFor(bh, row); else for (int by = 0; by < bh; by++) row(by);
	}
}

static uint64_t HashImage(const BakeImage& img, TexKind kind)
{
	uint64_t h = 1469598103934665603ull ^ (uint64_t)kind;
	auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ull; };
	mix((uint64_t)img.w); mix((uint64_t)img.h);
	for (float f : img.px) mix((uint64_t)(int)(f * 1023.0f + 0.5f));
	return h;
}

// encodes (or reuses an identical) texture; returns its index
static int AddTexture(const BakeImage& img, TexKind kind, bool parallel)
{
	uint64_t h = HashImage(img, kind);
	{
		std::lock_guard<std::mutex> lk(texMutex);
		auto it = texByHash.find(h);
		if (it != texByHash.end()) return it->second;
	}
	if (const char* dump = getenv("KT_DUMP")) {   // debugging: the baked images as PNG
		std::vector<uint8_t> px((size_t)img.w * img.h * 4);
		for (size_t i = 0; i < px.size(); i++) { float v = img.px[i]; if (kind == TK_COLOR && (i & 3) != 3) v = LinToSrgb(v); px[i] = (uint8_t)(std::min(1.0f, std::max(0.0f, v)) * 255 + 0.5f); }
		char name[512]; snprintf(name, sizeof(name), "%s/%016llx_%d.png", dump, (unsigned long long)h, (int)kind);
		stbi_write_png(name, img.w, img.h, 4, px.data(), img.w * 4);
	}
	TexOut t = {};
	uint32_t fmt = kind == TK_NORMAL ? 83 : 98;
	EncodeImage(img, kind, fmt, t.data, t.k.mips, parallel);
	t.k.width = img.w; t.k.height = img.h; t.k.format = fmt; t.k.srgb = kind == TK_COLOR ? 1 : 0;
	std::lock_guard<std::mutex> lk(texMutex);
	auto it = texByHash.find(h);
	if (it != texByHash.end()) return it->second;
	int idx = (int)textures.size();
	t.bytes = t.data.size();
	t.spillAt = SpillWrite(t.data.data(), t.data.size());
	std::vector<uint8_t>().swap(t.data);
	textures.push_back(std::move(t));
	texByHash[h] = idx;
	return idx;
}

// ================================================================================================ materials
struct MatInfo {
	std::string name;
	const Json* json = nullptr;
	uint32_t flags = 0;
	bool invisible = false;     // volume-only / no surface: its geometry is left out
	double area = 0;            // world area using it (units^2)
	int srcRes = 0;             // largest source image side
	int res = 0;                // bake resolution
	std::vector<std::string> images;
};
static std::vector<MatInfo> mats;
static std::map<std::string, uint32_t> matIndex;
static std::vector<KwMaterial> kmats;

static uint32_t MaterialFlags(const Json& m)
{
	uint32_t f = 0;
	std::string mode = m["alphaMode"].str("OPAQUE");
	if (mode == "MASK" || mode == "BLEND") f |= KWM_MASK;
	if (mode == "BLEND") f |= KWM_BLEND;
	if (m["twoSided"].t == Json::BOOL && m["twoSided"].b) f |= KWM_TWOSIDED;
	if (f & KWM_MASK) f |= KWM_TWOSIDED;
	return f;
}

static const char* OutputShaderNode(const Json& nodes)
{
	const char* best = nullptr;
	for (auto& kv : nodes.o) {
		const Json& n = kv.second;
		if (n["type"].str() != "OUTPUT_MATERIAL") continue;
		bool active = n["props"]["active"].t == Json::BOOL && n["props"]["active"].b;
		std::string target = n["props"]["target"].str("ALL");
		for (auto& in : n["inputs"].a)
			if (in["id"].str() == "Surface" && in["link"].t == Json::ARR) {
				if (active || !best) best = in["link"][(size_t)0].s.c_str();
				if (active && target != "CYCLES") return best;
			}
	}
	return best;
}

static bool FieldIs(const Field& f, const float* v, int n, float eps = 1e-4f)
{
	if (!f.constant()) return false;
	for (int c = 0; c < n; c++) if (fabsf(f.d[c] - v[c]) > eps) return false;
	return true;
}

static void BakeMaterial(MatInfo& mi, KwMaterial& k, std::vector<std::string>& warnings, const Json* groups)
{
	const Json& m = *mi.json;
	k = KwMaterial();
	k.baseColor[0] = k.baseColor[1] = k.baseColor[2] = 0.8f; k.baseColor[3] = 1;
	k.roughness = (float)m["roughness"].num(0.5); k.metallic = (float)m["metallic"].num(0); k.normalScale = 1; k.alphaCutoff = 0.5f;
	k.albedoTex = k.normalTex = k.ormTex = k.emissiveTex = -1;
	k.flags = mi.flags;
	const Json& graph = m["graph"];
	const char* out = graph.t == Json::OBJ ? OutputShaderNode(graph["nodes"]) : nullptr;
	if (!out) {
		const Json& bc = m["baseColor"];
		if (bc.t == Json::ARR) for (int i = 0; i < 4; i++) k.baseColor[i] = (float)bc[(size_t)i].num(i == 3 ? 1 : 0.8);
		return;
	}
	// the graph is evaluated in bands of rows (every node keeps its output while the graph runs, so a whole
	// 2048 x 2048 bake at once could need gigabytes); results go into the four output images
	const int W = std::max(4, mi.res), H = W;
	int bandRows = std::max(1, std::min(H, (int)((256u * 1024u) / (unsigned)W)));
	if (const char* br = getenv("KT_BAKE_ROWS")) bandRows = std::max(1, std::min(H, atoi(br)));
	struct Out { std::vector<float> px; bool varies = false; float c[4] = {}; bool any = false; };
	Out oBase, oNorm, oOrm, oEmis;
	bool blend = false, alphaVaries = false, alphaAny = false;
	float alpha0 = 1;
	float emisMax = 1e-6f;
	std::vector<std::string> cw;
	auto put = [&](Out& o, const Field* f, int y0, int rows, int ch, const std::function<void(const Field*, size_t, float*)>& fn) {
		// constant over this band and equal to what we had so far: keep it as a constant
		float v[4];
		if (f->constant()) {
			fn(f, 0, v);
			if (!o.any) { memcpy(o.c, v, 16); o.any = true; }
			else if (!o.varies && memcmp(o.c, v, 16) != 0) o.varies = true;
		}
		else o.varies = true;
		if (!o.varies) return;                 // still one constant: no image needed (yet)
		if (o.px.empty()) {                    // first band that varies: earlier bands were the constant
			o.px.resize((size_t)W * H * 4);
			for (int y = 0; y < y0; y++) for (int x = 0; x < W; x++) memcpy(&o.px[((size_t)(H - 1 - y) * W + x) * 4], o.c, 16);
		}
		for (int y = 0; y < rows; y++) for (int x = 0; x < W; x++) {
			size_t i = (size_t)y * W + x;                                          // field: row 0 = v near 0 (bottom)
			fn(f, f->constant() ? 0 : i, &o.px[((size_t)(H - 1 - (y0 + y)) * W + x) * 4]);   // image: row 0 = top
		}
		(void)ch;
	};
	for (int y0 = 0; y0 < H; y0 += bandRows) {
		int rows = std::min(bandRows, H - y0);
		NodeCtx ctx;
		ctx.groups = groups;
		ctx.W = W; ctx.H = rows; ctx.y0 = y0; ctx.fullH = H;
		ctx.loadImage = [](const std::string& f, bool lin) { return LoadSource(f, lin); };
		NodeGraph g;
		g.nodes = &graph["nodes"];
		g.ctx = &ctx;
		Surface s = EvalShader(g, out);
		blend |= s.transparentBlend;
		if (!s.alpha.constant()) alphaVaries = true;
		else if (!alphaAny) { alpha0 = s.alpha.d[0]; alphaAny = true; }
		else if (s.alpha.d[0] != alpha0) alphaVaries = true;
		// base + alpha share one output: a band varies if either does
		Field ba; ba.n = 4;
		if (s.base.constant() && s.alpha.constant()) ba.d = { s.base.d[0], s.base.d[1], s.base.d[2], s.alpha.d[0] };
		else {
			ba.d.resize((size_t)W * rows * 4);
			for (size_t i = 0; i < (size_t)W * rows; i++) { ba.d[i * 4] = s.base.at(i, 0); ba.d[i * 4 + 1] = s.base.at(i, 1); ba.d[i * 4 + 2] = s.base.at(i, 2); ba.d[i * 4 + 3] = s.alpha.at(i, 0); }
		}
		put(oBase, &ba, y0, rows, 4, [](const Field* f, size_t i, float* o) { o[0] = f->at(i, 0); o[1] = f->at(i, 1); o[2] = f->at(i, 2); o[3] = Clamp01(f->at(i, 3)); });
		put(oNorm, &s.normal, y0, rows, 3, [](const Field* f, size_t i, float* o) {
			float x = f->at(i, 0), y = f->at(i, 1), z = f->at(i, 2), l = sqrtf(x * x + y * y + z * z);
			if (l < 1e-6f) { x = 0; y = 0; z = 1; l = 1; }
			o[0] = x / l * 0.5f + 0.5f; o[1] = y / l * 0.5f + 0.5f; o[2] = z / l * 0.5f + 0.5f; o[3] = 1;
		});
		Field rm; rm.n = 2;
		if (s.rough.constant() && s.metal.constant()) rm.d = { s.rough.d[0], s.metal.d[0] };
		else { rm.d.resize((size_t)W * rows * 2); for (size_t i = 0; i < (size_t)W * rows; i++) { rm.d[i * 2] = s.rough.at(i, 0); rm.d[i * 2 + 1] = s.metal.at(i, 0); } }
		put(oOrm, &rm, y0, rows, 2, [](const Field* f, size_t i, float* o) { o[0] = 1; o[1] = Clamp01(f->at(i, 0)); o[2] = Clamp01(f->at(i, 1)); o[3] = 1; });
		for (float v : s.emission.d) emisMax = std::max(emisMax, v);
		put(oEmis, &s.emission, y0, rows, 3, [](const Field* f, size_t i, float* o) { o[0] = f->at(i, 0); o[1] = f->at(i, 1); o[2] = f->at(i, 2); o[3] = 1; });
		for (auto& w : ctx.warnings) if (std::find(cw.begin(), cw.end(), w) == cw.end()) cw.push_back(w);
	}
	auto image = [&](Out& o) {
		BakeImage img; img.w = W; img.h = H;
		if (o.px.empty()) { img.px.resize((size_t)W * H * 4); for (size_t i = 0; i < img.px.size(); i += 4) memcpy(&img.px[i], o.c, 16); }
		else img.px = std::move(o.px);
		return img;
	};
	// albedo + alpha
	if (!oBase.varies) { for (int c = 0; c < 4; c++) k.baseColor[c] = oBase.c[c]; }
	else {
		k.baseColor[0] = k.baseColor[1] = k.baseColor[2] = k.baseColor[3] = 1;
		k.albedoTex = AddTexture(image(oBase), TK_COLOR, false);
	}
	if (alphaVaries || alpha0 < 0.99f) {
		if (!(k.flags & KWM_MASK)) k.flags |= KWM_MASK | KWM_TWOSIDED;
	}
	if (blend) k.flags |= KWM_BLEND;
	// normal
	bool flatN = !oNorm.varies && fabsf(oNorm.c[0] - 0.5f) < 5e-4f && fabsf(oNorm.c[1] - 0.5f) < 5e-4f;
	if (!flatN) k.normalTex = AddTexture(image(oNorm), TK_NORMAL, false);
	// roughness / metallic
	if (!oOrm.varies) { k.roughness = oOrm.c[1]; k.metallic = oOrm.c[2]; }
	else { k.roughness = 1; k.metallic = 1; k.ormTex = AddTexture(image(oOrm), TK_DATA, false); }
	// emission (Blender strength 1 ~ a white diffuse surface under 1 W/m2; scaled to Katarn's sun units)
	if (!oEmis.varies) { for (int c = 0; c < 3; c++) k.emissive[c] = oEmis.c[c] * 2.0f; }
	else {
		k.emissive[0] = k.emissive[1] = k.emissive[2] = emisMax * 2.0f;
		BakeImage e = image(oEmis);
		for (size_t i = 0; i < e.px.size(); i++) if ((i & 3) != 3) e.px[i] /= emisMax;
		k.emissiveTex = AddTexture(e, TK_COLOR, false);
	}
	for (auto& w : cw) warnings.push_back(w);
}

// texture resolutions: big surfaces get more texels, within the memory budget
static void PlanResolutions()
{
	for (auto& m : mats) {
		m.srcRes = 0;
		for (auto& f : m.images) { auto d = SourceDims(f); m.srcRes = std::max(m.srcRes, std::max(d.first, d.second)); }
		m.srcRes = std::min(m.srcRes, MAXTEX);
	}
	auto resFor = [&](const MatInfo& m, double s) {
		if (m.srcRes <= 0) return 4;
		double want = sqrt(std::max(m.area, 1.0)) * s;
		int r = 64;
		while (r * 2 <= m.srcRes && r * 2 <= want) r *= 2;
		return std::min(r, m.srcRes);
	};
	auto cost = [&](double s) {
		double b = 0;
		for (auto& m : mats) { int r = resFor(m, s); if (r > 4) b += 3.0 * r * r * 1.34; }
		return b / 1048576.0;
	};
	double lo = 1e-6, hi = 1e3;
	for (int it = 0; it < 60; it++) { double mid = sqrt(lo * hi); if (cost(mid) > TEXBUDGET_MB) hi = mid; else lo = mid; }
	for (auto& m : mats) m.res = resFor(m, lo);
	printf("  texture plan: %.0f MB (budget %.0f MB)\n", cost(lo), TEXBUDGET_MB);
}

// ================================================================================================ geometry
struct InVert { float p[3], n[3], uv[2]; };

static void OctEncode(const float* v, int16_t* o)
{
	float l = fabsf(v[0]) + fabsf(v[1]) + fabsf(v[2]);
	float x = l > 0 ? v[0] / l : 0, y = l > 0 ? v[1] / l : 0;
	if (v[2] < 0) { float tx = (1 - fabsf(y)) * (x >= 0 ? 1.0f : -1.0f), ty = (1 - fabsf(x)) * (y >= 0 ? 1.0f : -1.0f); x = tx; y = ty; }
	o[0] = (int16_t)lroundf(std::max(-1.0f, std::min(1.0f, x)) * 32767.0f);
	o[1] = (int16_t)lroundf(std::max(-1.0f, std::min(1.0f, y)) * 32767.0f);
}

static void ObjectTangents(const std::vector<InVert>& v, const std::vector<uint32_t>& idx, std::vector<float>& tan, std::vector<float>& sign)
{
	size_t nv = v.size();
	std::vector<float> t(nv * 3, 0), b(nv * 3, 0);
	for (size_t i = 0; i + 2 < idx.size(); i += 3) {
		const InVert& A = v[idx[i]], & B = v[idx[i + 1]], & C = v[idx[i + 2]];
		float e1[3] = { B.p[0] - A.p[0], B.p[1] - A.p[1], B.p[2] - A.p[2] }, e2[3] = { C.p[0] - A.p[0], C.p[1] - A.p[1], C.p[2] - A.p[2] };
		float du1 = B.uv[0] - A.uv[0], dv1 = B.uv[1] - A.uv[1], du2 = C.uv[0] - A.uv[0], dv2 = C.uv[1] - A.uv[1];
		float r = du1 * dv2 - du2 * dv1;
		if (fabsf(r) < 1e-12f) continue;
		r = 1.0f / r;
		for (int k = 0; k < 3; k++) {
			float tt = (e1[k] * dv2 - e2[k] * dv1) * r, bb = (e2[k] * du1 - e1[k] * du2) * r;
			for (int j = 0; j < 3; j++) { t[idx[i + j] * 3 + k] += tt; b[idx[i + j] * 3 + k] += bb; }
		}
	}
	tan.assign(nv * 3, 0); sign.assign(nv, 1);
	for (size_t i = 0; i < nv; i++) {
		const float* n = v[i].n;
		float* tt = &t[i * 3];
		float d = tt[0] * n[0] + tt[1] * n[1] + tt[2] * n[2];
		float o[3] = { tt[0] - n[0] * d, tt[1] - n[1] * d, tt[2] - n[2] * d };
		float l = sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
		if (l < 1e-8f) { float a[3] = { fabsf(n[0]) < 0.9f ? 1.0f : 0.0f, fabsf(n[0]) < 0.9f ? 0.0f : 1.0f, 0 };
			o[0] = a[1] * n[2] - a[2] * n[1]; o[1] = a[2] * n[0] - a[0] * n[2]; o[2] = a[0] * n[1] - a[1] * n[0]; l = sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]) + 1e-12f; }
		for (int k = 0; k < 3; k++) tan[i * 3 + k] = o[k] / l;
		float cx = n[1] * tan[i * 3 + 2] - n[2] * tan[i * 3 + 1], cy = n[2] * tan[i * 3] - n[0] * tan[i * 3 + 2], cz = n[0] * tan[i * 3 + 1] - n[1] * tan[i * 3];
		const float* bb = &b[i * 3];
		sign[i] = (cx * bb[0] + cy * bb[1] + cz * bb[2]) < 0 ? -1.0f : 1.0f;
	}
}

struct Bucket {
	int cls = 0;
	int cell[3] = {};
	std::vector<KwVertex> v;
	std::vector<uint32_t> idx;
	std::vector<uint16_t> triMat;
	std::unordered_map<uint64_t, uint32_t> remap;
	int64_t remapOwner = -1;
	bool noShadow = false;
	// output
	std::vector<KwLod> lods;
	std::vector<uint32_t> lodIdx;   // all LODs, concatenated
	uint64_t vAt = 0, iAt = 0;      // in the spill file once the LODs are built
	uint32_t nv = 0, ni = 0;
	float bmin[3] = { 1e30f, 1e30f, 1e30f }, bmax[3] = { -1e30f, -1e30f, -1e30f };
};
static std::map<std::array<int, 4>, Bucket*> buckets;

static void BuildLods(Bucket& b)
{
	size_t nt = b.idx.size() / 3;
	if (!nt) return;
	// group triangles by material
	std::vector<uint32_t> order(nt);
	for (size_t i = 0; i < nt; i++) order[i] = (uint32_t)i;
	std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t c) { return b.triMat[a] < b.triMat[c]; });
	std::vector<std::vector<uint32_t>> groups;
	uint16_t cur = 0xffff;
	for (uint32_t t : order) {
		if (b.triMat[t] != cur) { groups.emplace_back(); cur = b.triMat[t]; }
		auto& gg = groups.back();
		gg.push_back(b.idx[t * 3]); gg.push_back(b.idx[t * 3 + 1]); gg.push_back(b.idx[t * 3 + 2]);
	}
	float scale = meshopt_simplifyScale(&b.v[0].pos[0], b.v.size(), sizeof(KwVertex));
	if (scale <= 0) scale = 1;
	// LOD0 keeps the shading: normals take part in its error
	std::vector<float> nrm(b.v.size() * 3);
	for (size_t i = 0; i < b.v.size(); i++) {
		float x = b.v[i].normal[0] / 32767.0f, y = b.v[i].normal[1] / 32767.0f, z = 1 - fabsf(x) - fabsf(y);
		float tt = std::max(-z, 0.0f); x += x >= 0 ? -tt : tt; y += y >= 0 ? -tt : tt;
		float l = sqrtf(x * x + y * y + z * z); if (l < 1e-6f) l = 1;
		nrm[i * 3] = x / l; nrm[i * 3 + 1] = y / l; nrm[i * 3 + 2] = z / l;
	}
	const float nw[3] = { 0.5f, 0.5f, 0.5f };
	const float ratios[] = { 1.0f, 0.5f, 0.25f, 0.1f, 0.035f };
	const float errs[] = { LOD0_ERROR, 2.0f, 8.0f, 32.0f, 128.0f };
	std::vector<std::vector<uint32_t>> prev = groups;
	size_t prevCount = 0;
	for (auto& gg : groups) prevCount += gg.size();
	for (int L = 0; L < 5; L++) {
		std::vector<std::vector<uint32_t>> cur2(groups.size());
		float maxErr = 0;
		size_t total = 0;
		for (size_t gi = 0; gi < groups.size(); gi++) {
			const std::vector<uint32_t>& src = L == 0 ? groups[gi] : prev[gi];
			std::vector<uint32_t> dst(src.size());
			float e = 0;
			size_t target = L == 0 ? 0 : (size_t)(groups[gi].size() / 3 * ratios[L]) * 3;
			size_t n = src.size();
			if (src.size() >= 3) {
				if (L == 0)
					n = meshopt_simplifyWithAttributes(dst.data(), src.data(), src.size(), &b.v[0].pos[0], b.v.size(), sizeof(KwVertex),
						nrm.data(), 12, nw, 3, nullptr, 0, errs[L] / scale, meshopt_SimplifyLockBorder, &e);
				else
					n = meshopt_simplify(dst.data(), src.data(), src.size(), &b.v[0].pos[0], b.v.size(), sizeof(KwVertex),
						std::min(target, src.size()), errs[L] / scale,
						meshopt_SimplifyLockBorder | meshopt_SimplifyPermissive | (L >= 3 ? meshopt_SimplifyPrune : 0), &e);
				dst.resize(n);
				meshopt_optimizeVertexCache(dst.data(), dst.data(), dst.size(), b.v.size());
			}
			else dst = src;
			maxErr = std::max(maxErr, e * scale);
			total += dst.size();
			cur2[gi] = std::move(dst);
		}
		if (L > 0 && total > prevCount * 0.8) break;   // not worth another level
		KwLod lod = {};
		lod.firstIndex = (uint32_t)b.lodIdx.size();
		for (auto& gg : cur2) b.lodIdx.insert(b.lodIdx.end(), gg.begin(), gg.end());
		lod.indexCount = (uint32_t)total;
		lod.error = L == 0 ? 0.0f : std::max(maxErr, b.lods.empty() ? 0.0f : b.lods.back().error);
		b.lods.push_back(lod);
		prev = std::move(cur2);
		prevCount = total;
		if (total < 64) break;
	}
	std::vector<uint32_t>().swap(b.idx);
	std::vector<uint16_t>().swap(b.triMat);
	b.remap.clear();
}

// ================================================================================================ environment SH
static void ProjectSH(const BakeImage& env, float strength, float sh[9][3])
{
	memset(sh, 0, sizeof(float) * 27);
	// Blender equirect: u = atan2(dir.y, -dir.x) / 2pi + 0.5, v = atan2(dir.z, hypot(x, y)) / pi + 0.5 (image v up)
	int W = env.w, H = env.h;
	int step = std::max(1, std::max(W, H) / 512);
	for (int y = 0; y < H; y += step) {
		float v = 1.0f - (y + 0.5f) / H;
		float el = (v - 0.5f) * 3.14159265f;
		float w = cosf(el) * (2 * 3.14159265f * step / W) * (3.14159265f * step / H);
		for (int x = 0; x < W; x += step) {
			float u = (x + 0.5f) / W;
			float phi = (u - 0.5f) * 2 * 3.14159265f;
			float d[3] = { cosf(el) * cosf(phi), -cosf(el) * sinf(phi), sinf(el) };
			const float* p = &env.px[((size_t)y * W + x) * 4];
			float basis[9] = { 0.282095f, 0.488603f * d[1], 0.488603f * d[2], 0.488603f * d[0], 1.092548f * d[0] * d[1], 1.092548f * d[1] * d[2],
				0.315392f * (3 * d[2] * d[2] - 1), 1.092548f * d[0] * d[2], 0.546274f * (d[0] * d[0] - d[1] * d[1]) };
			for (int k = 0; k < 9; k++) for (int c = 0; c < 3; c++) sh[k][c] += p[c] * strength * basis[k] * w;
		}
	}
	const float A[9] = { 1, 2 / 3.f, 2 / 3.f, 2 / 3.f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f };
	for (int k = 0; k < 9; k++) for (int c = 0; c < 3; c++) sh[k][c] *= A[k];
}

static bool Skip(const std::string& name)
{
	for (auto& s : SKIP) if (!s.empty() && name.find(s) != std::string::npos) return true;
	return false;
}

static void Xform(const float* m, const float* p, float* o)   // 3-row matrix
{
	for (int r = 0; r < 3; r++) o[r] = m[r * 4] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2] + m[r * 4 + 3];
}

// ================================================================================================ main
int main(int argc, char** argv)
{
	if (argc < 4) {
		printf("usage: katarn_cook <export folder> <output folder> <map name> [--units 40] [--maxtex 2048] [--texbudget 2500]\n"
			"       [--quality 0-2] [--cell 2048] [--lod0-error 0.25] [--collision-error 8] [--collision-budget 150000]\n"
			"       [--skip \"HeightFog,Sky_Cottonball\"] [--threads N] [--maxmem MB] [--bands N]\n");
		return 1;
	}
	double t0 = Now();
	exportDir = argv[1];
	std::string outDir = argv[2], mapName = argv[3];
	for (int i = 4; i + 1 < argc; i += 2) {
		std::string a = argv[i], v = argv[i + 1];
		if (a == "--units") UNITS = (float)atof(v.c_str());
		else if (a == "--maxtex") MAXTEX = atoi(v.c_str());
		else if (a == "--texbudget") TEXBUDGET_MB = (float)atof(v.c_str());
		else if (a == "--quality") QUALITY = atoi(v.c_str());
		else if (a == "--threads") THREADS = atoi(v.c_str());
		else if (a == "--bands") BANDS = atoi(v.c_str());
		else if (a == "--maxmem") MAXMEM_MB = (size_t)atoll(v.c_str());
		else if (a == "--cell") CELL = (float)atof(v.c_str());
		else if (a == "--lod0-error") LOD0_ERROR = (float)atof(v.c_str());
		else if (a == "--collision-error") COLL_ERROR = (float)atof(v.c_str());
		else if (a == "--collision-budget") COLL_BUDGET = (size_t)atoll(v.c_str());
		else if (a == "--skip") { SKIP.clear(); size_t s = 0; while (s <= v.size()) { size_t e = v.find(',', s); if (e == std::string::npos) e = v.size(); SKIP.push_back(v.substr(s, e - s)); s = e + 1; } }
	}
	g_cookLog = fopen((outDir + "/" + mapName + "_cook_log.txt").c_str(), "w");
	std::string js;
	{
		FILE* f = fopen((exportDir + "/scene.json").c_str(), "rb");
		if (!f) { printf("no %s/scene.json\n", exportDir.c_str()); return 1; }
		fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
		js.resize(n); if (fread(&js[0], 1, n, f) != (size_t)n) js.clear(); fclose(f);
	}
	JsonParser jp{ js.c_str() };
	Json scene = jp.val();
	js.clear(); js.shrink_to_fit();
	bc7enc_compress_block_init();
	rgbcx::init();
	if (!MAXMEM_MB) MAXMEM_MB = PhysicalMB() * 6 / 10;
	printf("KatarnEngine cook: %s -> %s/%s.kworld\n  1 m = %g units, cells %g, textures up to %d within %g MB, %d threads, memory limit %zu MB\n",
		exportDir.c_str(), outDir.c_str(), mapName.c_str(), UNITS, CELL, MAXTEX, TEXBUDGET_MB, NumThreads(), MAXMEM_MB);
	std::string spillPath = outDir + "/" + mapName + ".kworld.tmp";
	g_spill = fopen(spillPath.c_str(), "w+b");
	if (!g_spill) { printf("cannot create %s\n", spillPath.c_str()); return 1; }
	std::thread watchdog(MemoryWatchdog);
	const Json* groups = scene.has("groups") ? &scene["groups"] : nullptr;

	// ---- materials (flags now, baked after we know how much surface each covers)
	const Json& jm = scene["materials"];
	for (auto& kv : jm.o) {
		MatInfo mi;
		mi.name = kv.first; mi.json = &kv.second; mi.flags = MaterialFlags(kv.second);
		if (kv.second["graph"].t == Json::OBJ) {
			CollectImages(kv.second["graph"]["nodes"], groups, mi.images);
			mi.invisible = OutputShaderNode(kv.second["graph"]["nodes"]) == nullptr;
		}
		std::sort(mi.images.begin(), mi.images.end());
		mi.images.erase(std::unique(mi.images.begin(), mi.images.end()), mi.images.end());
		matIndex[mi.name] = (uint32_t)mats.size();
		mats.push_back(mi);
	}
	if (mats.empty()) { MatInfo d; d.name = "__default"; d.json = &jm; mats.push_back(d); }

	// ---- geometry -> world-space buckets (cell x class)
	const Json& meshes = scene["meshes"];
	const Json& insts = scene["instances"];
	std::map<std::string, FILE*> packs;
	float wmin[3] = { 1e30f, 1e30f, 1e30f }, wmax[3] = { -1e30f, -1e30f, -1e30f };
	float cmin[3] = { 1e30f, 1e30f, 1e30f }, cmax[3] = { -1e30f, -1e30f, -1e30f };   // "core": objects under 100 m
	size_t inTris = 0, skipped = 0;
	std::vector<InVert> v;
	std::vector<uint32_t> idx;
	std::vector<float> tan, tsign;
	std::vector<std::array<float, 4>> coreSamples;
	// big worlds are gathered in interleaved bands of cells (cell x mod bands), so only one band's raw geometry is
	// in memory at a time; each band's clusters get their LODs and go to the spill file before the next band
	int nBands = BANDS;
	if (nBands <= 0) {
		double tris = 0;
		for (size_t ii = 0; ii < insts.size(); ii++) {
			size_t mi = (size_t)insts[ii]["mesh"].num();
			if (mi < meshes.size()) tris += meshes[mi]["indexCount"].num() / 3;
		}
		double need = tris * 140.0 / 1048576.0, budget = MAXMEM_MB * 0.4;
		nBands = std::max(1, std::min(64, (int)ceil(need / budget)));
		printf("  %.1fM triangles in the export (~%.0f MB to gather): %d band%s\n", tris / 1e6, need, nBands, nBands > 1 ? "s" : "");
	}
	std::vector<Bucket*> blist;
	std::atomic<size_t> done(0);
	double tLods = 0;
	for (int band = 0; band < nBands; band++) {
	for (size_t ii = 0; ii < insts.size(); ii++) {
		const Json& in = insts[ii];
		std::string oname = in["object"].str();
		if (Skip(oname)) { if (band == 0) skipped++; continue; }
		size_t mi = (size_t)in["mesh"].num();
		if (mi >= meshes.size()) continue;
		const Json& m = meshes[mi];
		std::string pack = m["pack"].str();
		FILE*& pf = packs[pack];
		if (!pf) pf = fopen((exportDir + "/" + pack).c_str(), "rb");
		if (!pf) { printf("  ! missing %s\n", pack.c_str()); continue; }
		uint32_t nv = (uint32_t)m["vertexCount"].num(), ni = (uint32_t)m["indexCount"].num();
		v.resize(nv); idx.resize(ni);
#ifdef _WIN32
		_fseeki64(pf, (long long)m["offset"].num(), SEEK_SET);
#else
		fseeko(pf, (off_t)m["offset"].num(), SEEK_SET);
#endif
		if (fread(v.data(), sizeof(InVert), nv, pf) != nv || fread(idx.data(), 4, ni, pf) != ni) { printf("  ! short read in %s\n", pack.c_str()); continue; }
		ObjectTangents(v, idx, tan, tsign);
		// matrix (Blender rows) -> Quake units
		float M[12];
		const Json& J = in["matrix"];
		for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) M[r * 4 + c] = (float)J[(size_t)(r * 4 + c)].num() * UNITS;
		float a = M[0], b = M[1], c = M[2], d = M[4], e = M[5], f = M[6], gg = M[8], h = M[9], i9 = M[10];
		float det = a * (e * i9 - f * h) - b * (d * i9 - f * gg) + c * (d * h - e * gg);
		bool mirror = det < 0;
		// normal matrix (cofactors)
		float N[9] = { e * i9 - f * h, -(d * i9 - f * gg), d * h - e * gg, -(b * i9 - c * h), a * i9 - c * gg, -(a * h - b * gg), b * f - c * e, -(a * f - c * d), a * e - b * d };
		// submesh material per triangle
		std::vector<uint16_t> triMat(ni / 3, 0);
		for (size_t si = 0; si < m["submeshes"].size(); si++) {
			const Json& s = m["submeshes"][si];
			auto it = matIndex.find(s["material"].str());
			uint16_t mid = it != matIndex.end() ? (uint16_t)it->second : 0;
			uint32_t st = (uint32_t)s["start"].num() / 3, cnt = (uint32_t)s["count"].num() / 3;
			for (uint32_t t = st; t < st + cnt && t < triMat.size(); t++) triMat[t] = mid;
		}
		// world positions once
		std::vector<float> wp(nv * 3);
		float omin[3] = { 1e30f, 1e30f, 1e30f }, omax[3] = { -1e30f, -1e30f, -1e30f };
		for (uint32_t k = 0; k < nv; k++) {
			Xform(M, v[k].p, &wp[k * 3]);
			for (int q = 0; q < 3; q++) { omin[q] = std::min(omin[q], wp[k * 3 + q]); omax[q] = std::max(omax[q], wp[k * 3 + q]); }
		}
		float oext = std::max(omax[0] - omin[0], std::max(omax[1] - omin[1], omax[2] - omin[2]));
		if (band == 0) for (int q = 0; q < 3; q++) { wmin[q] = std::min(wmin[q], omin[q]); wmax[q] = std::max(wmax[q], omax[q]); }
		if (band == 0 && oext < 100 * UNITS) {
			float ctr[3] = { (omin[0] + omax[0]) * 0.5f, (omin[1] + omax[1]) * 0.5f, (omin[2] + omax[2]) * 0.5f };
			coreSamples.push_back({ ctr[0], ctr[1], ctr[2], (float)(ni / 3) });
		}
		for (size_t t = 0; t + 2 < ni; t += 3) {
			uint32_t vi[3] = { idx[t], idx[t + 1], idx[t + 2] };
			if (mirror) std::swap(vi[1], vi[2]);
			const float* p0 = &wp[vi[0] * 3], * p1 = &wp[vi[1] * 3], * p2 = &wp[vi[2] * 3];
			uint16_t mat = triMat[t / 3];
			if (mats[mat].invisible) continue;
			int cls = (mats[mat].flags & KWM_MASK) ? KWC_MASKED : (mats[mat].flags & KWM_TWOSIDED) ? KWC_TWOSIDED : KWC_OPAQUE;
			int cx = (int)floorf((p0[0] + p1[0] + p2[0]) / (3 * CELL)), cy = (int)floorf((p0[1] + p1[1] + p2[1]) / (3 * CELL)), cz = (int)floorf((p0[2] + p1[2] + p2[2]) / (3 * CELL));
			if (((cx % nBands) + nBands) % nBands != band) continue;
			std::array<int, 4> key = { cx, cy, cz, cls };
			Bucket*& B = buckets[key];
			if (!B) { B = new Bucket(); B->cls = cls; B->cell[0] = cx; B->cell[1] = cy; B->cell[2] = cz; }
			if (B->remapOwner != (int64_t)ii) { std::unordered_map<uint64_t, uint32_t>().swap(B->remap); B->remapOwner = (int64_t)ii; }
			uint32_t out[3];
			for (int q = 0; q < 3; q++) {
				uint64_t rk = ((uint64_t)vi[q] << 16) | mat;
				auto it = B->remap.find(rk);
				if (it != B->remap.end()) { out[q] = it->second; continue; }
				KwVertex kv = {};
				const InVert& s = v[vi[q]];
				memcpy(kv.pos, &wp[vi[q] * 3], 12);
				float n[3] = { N[0] * s.n[0] + N[1] * s.n[1] + N[2] * s.n[2], N[3] * s.n[0] + N[4] * s.n[1] + N[5] * s.n[2], N[6] * s.n[0] + N[7] * s.n[1] + N[8] * s.n[2] };
				float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]); if (nl < 1e-12f) { n[2] = 1; nl = 1; }
				for (int k = 0; k < 3; k++) n[k] /= nl;
				const float* to = &tan[vi[q] * 3];
				float tw[3] = { M[0] * to[0] + M[1] * to[1] + M[2] * to[2], M[4] * to[0] + M[5] * to[1] + M[6] * to[2], M[8] * to[0] + M[9] * to[1] + M[10] * to[2] };
				float tl = sqrtf(tw[0] * tw[0] + tw[1] * tw[1] + tw[2] * tw[2]); if (tl < 1e-12f) { tw[0] = 1; tl = 1; }
				for (int k = 0; k < 3; k++) tw[k] /= tl;
				OctEncode(n, kv.normal);
				OctEncode(tw, kv.tangent);
				kv.tangentSign = (int16_t)((tsign[vi[q]] < 0) != mirror ? -1 : 1);
				kv.uv[0] = s.uv[0]; kv.uv[1] = 1.0f - s.uv[1];
				kv.material = mat;
				out[q] = (uint32_t)B->v.size();
				B->v.push_back(kv);
				for (int k = 0; k < 3; k++) { B->bmin[k] = std::min(B->bmin[k], kv.pos[k]); B->bmax[k] = std::max(B->bmax[k], kv.pos[k]); }
				B->remap[rk] = out[q];
			}
			B->idx.insert(B->idx.end(), out, out + 3);
			B->triMat.push_back(mat);
			// surface area per material, for the texture plan
			float e1[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] }, e2[3] = { p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2] };
			float cr[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
			mats[mat].area += 0.5 * sqrt((double)cr[0] * cr[0] + (double)cr[1] * cr[1] + (double)cr[2] * cr[2]);
			inTris++;
		}
		if (ii % 500 == 0) { printf("\r  geometry band %d/%d: %zu/%zu objects, %.1fM triangles, %zu clusters   ", band + 1, nBands, ii + 1, insts.size(), inTris / 1e6, buckets.size()); fflush(stdout); }
	}
	// this band's clusters: LODs, then out to the spill file
	std::vector<Bucket*> bandList;
	for (auto& kv : buckets) if (((kv.first[0] % nBands) + nBands) % nBands == band) {
		std::unordered_map<uint64_t, uint32_t>().swap(kv.second->remap);
		bandList.push_back(kv.second); blist.push_back(kv.second);
	}
	double tl0 = Now();
	ParallelFor(bandList.size(), [&](size_t i) {
		Bucket& b = *bandList[i];
		BuildLods(b);
		b.nv = (uint32_t)b.v.size(); b.ni = (uint32_t)b.lodIdx.size();
		b.vAt = SpillWrite(b.v.data(), b.v.size() * sizeof(KwVertex));
		b.iAt = SpillWrite(b.lodIdx.data(), b.lodIdx.size() * 4);
		std::vector<KwVertex>().swap(b.v);
		std::vector<uint32_t>().swap(b.lodIdx);
		size_t d = ++done;
		if (d % 50 == 0) { printf("\r  LODs %zu clusters (band %d/%d)   ", d, band + 1, nBands); fflush(stdout); }
	});
	tLods += Now() - tl0;
	}
	for (auto& p : packs) if (p.second) fclose(p.second);
	printf("\r  geometry: %.2fM triangles in %zu clusters (%zu objects skipped), world %.0f x %.0f x %.0f units, %.0fs\n",
		inTris / 1e6, buckets.size(), skipped, wmax[0] - wmin[0], wmax[1] - wmin[1], wmax[2] - wmin[2], Now() - t0);
	// the playable core: where most of the detailed objects are (2nd..98th percentile per axis)
	if (!coreSamples.empty()) {
		double total = 0;
		for (auto& s : coreSamples) total += s[3];
		for (int q = 0; q < 3; q++) {
			std::sort(coreSamples.begin(), coreSamples.end(), [q](const std::array<float, 4>& a, const std::array<float, 4>& b) { return a[q] < b[q]; });
			double acc = 0; bool lo = false;
			for (auto& s : coreSamples) {
				acc += s[3];
				if (!lo && acc >= total * 0.02) { cmin[q] = s[q]; lo = true; }
				if (acc >= total * 0.98) { cmax[q] = s[q]; break; }
			}
		}
	}
	if (cmin[0] > cmax[0]) { memcpy(cmin, wmin, 12); memcpy(cmax, wmax, 12); }
	printf("  playable core %.0f x %.0f x %.0f units around (%.0f %.0f %.0f)\n", cmax[0] - cmin[0], cmax[1] - cmin[1], cmax[2] - cmin[2],
		(cmin[0] + cmax[0]) * 0.5f, (cmin[1] + cmax[1]) * 0.5f, (cmin[2] + cmax[2]) * 0.5f);

	// ---- LODs (built per band above)
	size_t lod0 = 0, lodAll = 0, verts = 0;
	for (Bucket* b : blist) { if (!b->lods.empty()) lod0 += b->lods[0].indexCount / 3; lodAll += b->ni / 3; verts += b->nv; }
	printf("\r  LODs: LOD0 %.2fM triangles (from %.2fM), all levels %.2fM, %.2fM vertices (LODs took %.0fs), %.0fs\n", lod0 / 1e6, inTris / 1e6, lodAll / 1e6, verts / 1e6, tLods, Now() - t0);

	// ---- materials: bake the node graphs
	PlanResolutions();
	kmats.resize(mats.size());
	std::vector<std::vector<std::string>> warn(mats.size());
	done = 0;
	ParallelFor(mats.size(), [&](size_t i) {
		BakeMaterial(mats[i], kmats[i], warn[i], groups);
		size_t d = ++done;
		if (d % 10 == 0 || d == mats.size()) { printf("\r  materials %zu/%zu, %zu textures   ", d, mats.size(), textures.size()); fflush(stdout); }
	});
	std::map<std::string, int> wcount;
	for (auto& w : warn) for (auto& s : w) wcount[s]++;
	uint64_t texBytes = 0;
	for (auto& t : textures) texBytes += t.bytes;
	printf("\r  materials: %zu baked, %zu textures (%.0f MB), %.0fs\n", mats.size(), textures.size(), texBytes / 1048576.0, Now() - t0);
	for (auto& w : wcount) printf("    note: %s (%d materials)\n", w.first.c_str(), w.second);

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
		float I = energy / (4.0f * 3.14159265f) * UNITS * UNITS;
		if (type == "AREA") I *= 0.5f;
		k.invSquare = I;
		memcpy(k.color, col, 12);
		k.radius = std::min(sqrtf(I / 0.002f), 200.0f * UNITS);
		memcpy(k.dir, dir, 12);
		if (type == "SPOT") {
			float half = (float)l["spotSize"].num(0.785) * 0.5f, blend = (float)l["spotBlend"].num(0.15);
			k.type = 1; k.spotCos = cosf(half); k.spotInnerCos = cosf(half * (1.0f - blend));
		}
		lights.push_back(k);
	}
	printf("  %zu lights, sun %s (%.2f %.2f %.2f) strength %.2f\n", lights.size(), haveSun ? "from Blender" : "default", sunDir[0], sunDir[1], sunDir[2], sunStrength);

	// ---- sky: the world's environment texture (or Katarn's physical sky)
	std::string envLines;
	const Json& world = scene["world"];
	{
		const Json& wg = world["graph"];
		const char* bg = nullptr;
		if (wg.t == Json::OBJ)
			for (auto& kv : wg["nodes"].o)
				if (kv.second["type"].str() == "OUTPUT_WORLD")
					for (auto& in : kv.second["inputs"].a) if (in["id"].str() == "Surface" && in["link"].t == Json::ARR) bg = in["link"][(size_t)0].s.c_str();
		const Json* bgn = bg ? &wg["nodes"][bg] : nullptr;
		if (bgn && (*bgn)["type"].str() == "BACKGROUND") {
			NodeCtx ctx;
			ctx.groups = groups; ctx.worldMode = true; ctx.W = 2048; ctx.H = 1024;
			ctx.loadImage = [](const std::string& f, bool lin) { return LoadSource(f, lin); };
			NodeGraph g; g.nodes = &wg["nodes"]; g.ctx = &ctx;
			Field col = g.in(*bgn, "Color", 3);
			Field strCam = g.in(*bgn, "Strength", 1);
			NodeGraph g2; g2.nodes = &wg["nodes"]; g2.ctx = &ctx;
			ctx.cameraRay = 0;
			Field strLight = g2.in(*bgn, "Strength", 1);
			float sc = strCam.constant() ? strCam.d[0] : 1.0f, sl = strLight.constant() ? strLight.d[0] : sc;
			BakeImage e; e.w = ctx.W; e.h = ctx.H; e.px.resize((size_t)e.w * e.h * 4);
			for (int y = 0; y < e.h; y++) for (int x = 0; x < e.w; x++) {
				size_t i = (size_t)y * e.w + x;
				float* o = &e.px[((size_t)(e.h - 1 - y) * e.w + x) * 4];
				o[0] = col.at(i, 0); o[1] = col.at(i, 1); o[2] = col.at(i, 2); o[3] = 1;
			}
			float sh[9][3];
			ProjectSH(e, sl, sh);
			int ti = AddTexture(e, TK_COLOR, true);
			char buf[512];
			snprintf(buf, sizeof(buf), "envTex %d\nenvStrength %f\nenvLight %f\n", ti, sc, sl);
			envLines += buf;
			for (int k = 0; k < 9; k++) { snprintf(buf, sizeof(buf), "sh%d %f %f %f\n", k, sh[k][0], sh[k][1], sh[k][2]); envLines += buf; }
			printf("  sky: world graph baked (camera strength %.2f, lighting %.2f)\n", sc, sl);
			for (auto& w : ctx.warnings) printf("    note: sky: %s\n", w.c_str());
		}
	}
	char env[2048];
	snprintf(env, sizeof(env),
		"sunDir %f %f %f\nsunColor %f %f %f\nsunIntensity %f\nsunAngle %f\nskyIntensity 1\nhaze 0.4\nambient 1\n"
		"fogDensity %g\nfogFalloff %g\nfogHeight %f\nfogStart %f\nfogColor 1 1 1\nfogMax 0.8\nexposureEV 0\n",
		sunDir[0], sunDir[1], sunDir[2], sunColor[0], sunColor[1], sunColor[2], std::max(0.5f, sunStrength), std::max(0.0047f, sunAngle),
		1.0f / (UNITS * 2500.0f), 1.0f / (UNITS * 120.0f), cmin[2], UNITS * 20.0f);
	std::string envText = std::string(env) + envLines;

	// ---- write .kworld
	// clusters reference the buckets' own arrays (no second copy of the geometry in memory)
	std::vector<KwCluster> clusters;
	std::vector<KwLod> lods;
	std::vector<Bucket*> cbucket;
	uint64_t vCount = 0, iCount = 0;
	for (Bucket* b : blist) {
		if (b->lods.empty()) continue;
		KwCluster c = {};
		memcpy(c.boundsMin, b->bmin, 12); memcpy(c.boundsMax, b->bmax, 12);
		c.baseVertex = (uint32_t)vCount; c.numVertices = b->nv;
		c.firstLod = (uint32_t)lods.size(); c.numLods = (uint32_t)b->lods.size();
		c.cls = (uint32_t)b->cls;
		uint32_t ib = (uint32_t)iCount;
		for (KwLod l : b->lods) { l.firstIndex += ib; lods.push_back(l); }
		iCount += b->ni;
		vCount += b->nv;
		clusters.push_back(c);
		cbucket.push_back(b);
	}
	std::vector<KwTexture> ktex;
	uint64_t tdatSize = 0;
	for (auto& t : textures) { KwTexture k = t.k; k.offset = tdatSize; k.size = t.bytes; tdatSize += (k.size + 255) & ~255ull; ktex.push_back(k); }
	std::string kpath = outDir + "/" + mapName + ".kworld";
	FILE* f = fopen(kpath.c_str(), "wb");
	if (!f) { printf("cannot write %s\n", kpath.c_str()); fflush(stdout); _exit(1); }
	struct Sec { uint32_t type; const void* data; uint64_t size; };
	std::vector<Sec> secs = {
		{ KW_VERT, nullptr, vCount * sizeof(KwVertex) }, { KW_INDX, nullptr, iCount * 4 },
		{ KW_CLUS, clusters.data(), clusters.size() * sizeof(KwCluster) }, { KW_LODS, lods.data(), lods.size() * sizeof(KwLod) },
		{ KW_MATL, kmats.data(), kmats.size() * sizeof(KwMaterial) }, { KW_TEXT, ktex.data(), ktex.size() * sizeof(KwTexture) },
		{ KW_LITE, lights.data(), lights.size() * sizeof(KwLight) }, { KW_ENVT, envText.data(), envText.size() }, { KW_TDAT, nullptr, tdatSize },
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
	uint64_t pos = sizeof(h) + sizeof(KwSection) * table.size();
	static const char zeros[256] = {};
	auto padTo = [&](uint64_t to) { while (pos < to) { size_t n = (size_t)std::min<uint64_t>(256, to - pos); fwrite(zeros, 1, n, f); pos += n; } };
	for (size_t i = 0; i < secs.size(); i++) {
		padTo(table[i].offset);
		if (secs[i].type == KW_VERT || secs[i].type == KW_INDX) {
			for (Bucket* b : cbucket) {
				if (secs[i].type == KW_VERT) SpillCopy(b->vAt, (uint64_t)b->nv * sizeof(KwVertex), f);
				else SpillCopy(b->iAt, (uint64_t)b->ni * 4, f);
			}
			pos += secs[i].size;
		}
		else if (secs[i].type == KW_TDAT) {
			for (auto& t : textures) {
				SpillCopy(t.spillAt, t.bytes, f);
				pos += t.bytes;
				padTo((pos + 255) & ~255ull);
			}
		}
		else if (secs[i].size) {
			const uint8_t* p = (const uint8_t*)secs[i].data;
			for (uint64_t w = 0; w < secs[i].size; ) { size_t n = (size_t)std::min<uint64_t>(64ull << 20, secs[i].size - w); fwrite(p + w, 1, n, f); w += n; }
			pos += secs[i].size;
		}
	}
	fclose(f);
	printf("  wrote %s (%.0f MB: geometry %.0f MB, textures %.0f MB), %zu clusters\n", kpath.c_str(), pos / 1048576.0,
		(vCount * sizeof(KwVertex) + iCount * 4) / 1048576.0, tdatSize / 1048576.0, clusters.size());

	// ---- collision .map: opaque clusters inside the playable core, at the LOD that fits the budget
	float pad = 1024.0f, T = 64.0f;
	float lo[3] = { floorf(cmin[0] - pad), floorf(cmin[1] - pad), floorf(cmin[2] - pad) };
	float hi[3] = { ceilf(cmax[0] + pad), ceilf(cmax[1] + pad), ceilf(cmax[2] + pad * 4) };
	auto inCore = [&](const float* p) { return p[0] > lo[0] && p[0] < hi[0] && p[1] > lo[1] && p[1] < hi[1] && p[2] > lo[2] && p[2] < hi[2]; };
	std::vector<int> collLod(clusters.size(), -1);
	for (size_t ci = 0; ci < clusters.size(); ci++) {
		const KwCluster& c = clusters[ci];
		if (c.cls == KWC_MASKED) continue;
		float ctr[3] = { (c.boundsMin[0] + c.boundsMax[0]) * 0.5f, (c.boundsMin[1] + c.boundsMax[1]) * 0.5f, (c.boundsMin[2] + c.boundsMax[2]) * 0.5f };
		if (!inCore(ctr) && !(c.boundsMin[2] < hi[2] && c.boundsMax[2] > lo[2] && c.boundsMax[0] > lo[0] && c.boundsMin[0] < hi[0] && c.boundsMax[1] > lo[1] && c.boundsMin[1] < hi[1])) continue;
		int pick = 0;
		for (uint32_t l = 0; l < c.numLods; l++) { pick = (int)l; if (lods[c.firstLod + l].error >= COLL_ERROR) break; }
		collLod[ci] = pick;
	}
	// per cluster collision triangles (world positions), then coarsened until the whole set fits the budget
	std::vector<std::vector<float>> collPos(clusters.size());
	std::vector<std::vector<uint32_t>> collTri(clusters.size());
	size_t collTotal = 0;
	for (size_t ci = 0; ci < clusters.size(); ci++) {
		if (collLod[ci] < 0) continue;
		const KwCluster& c = clusters[ci];
		const KwLod& l = lods[c.firstLod + collLod[ci]];
		std::vector<float>& P = collPos[ci];
		P.resize((size_t)c.numVertices * 3);
		const Bucket* B = cbucket[ci];
		std::vector<KwVertex> BV(B->nv);
		std::vector<uint32_t> BI(l.indexCount);
		SpillRead(B->vAt, BV.data(), BV.size() * sizeof(KwVertex));
		SpillRead(B->iAt + (uint64_t)(l.firstIndex - lods[c.firstLod].firstIndex + B->lods[0].firstIndex) * 4, BI.data(), BI.size() * 4);
		const uint32_t* LI = BI.data();
		for (uint32_t k = 0; k < c.numVertices; k++) memcpy(&P[k * 3], BV[k].pos, 12);
		std::vector<uint32_t>& I = collTri[ci];
		for (uint32_t t2 = 0; t2 + 2 < l.indexCount; t2 += 3) {
			const float* a3 = &P[LI[t2] * 3], * b3 = &P[LI[t2 + 1] * 3], * c3 = &P[LI[t2 + 2] * 3];
			float ctr[3] = { (a3[0] + b3[0] + c3[0]) / 3, (a3[1] + b3[1] + c3[1]) / 3, (a3[2] + b3[2] + c3[2]) / 3 };
			if (!inCore(ctr)) continue;
			I.push_back(LI[t2]); I.push_back(LI[t2 + 1]); I.push_back(LI[t2 + 2]);
		}
		// weld by position (UV / material seams would stop the simplifier)
		if (!I.empty()) {
			std::vector<unsigned int> remap(c.numVertices);
			size_t nu = meshopt_generateVertexRemap(remap.data(), I.data(), I.size(), P.data(), c.numVertices, 12);
			std::vector<float> P2(nu * 3);
			meshopt_remapVertexBuffer(P2.data(), P.data(), c.numVertices, 12, remap.data());
			meshopt_remapIndexBuffer(I.data(), I.data(), I.size(), remap.data());
			P.swap(P2);
		}
		collTotal += I.size() / 3;
	}
	for (float err = COLL_ERROR * 2; collTotal > COLL_BUDGET && err < 4096; err *= 2) {
		collTotal = 0;
		for (size_t ci = 0; ci < clusters.size(); ci++) {
			std::vector<uint32_t>& I = collTri[ci];
			if (I.size() < 3) continue;
			float sc = meshopt_simplifyScale(collPos[ci].data(), collPos[ci].size() / 3, 12);
			std::vector<uint32_t> d(I.size());
			size_t n = meshopt_simplify(d.data(), I.data(), I.size(), collPos[ci].data(), collPos[ci].size() / 3, 12, 0, err / std::max(sc, 1.0f), meshopt_SimplifyPrune, nullptr);
			d.resize(n);
			I.swap(d);
			collTotal += I.size() / 3;
		}
		printf("  collision: %zu triangles at %.0f units error\n", collTotal, err);
	}
	if (collTotal > COLL_BUDGET) {   // still too many: sloppy (vertex clustering) simplification to the budget
		double keep = (double)COLL_BUDGET / collTotal;
		collTotal = 0;
		for (size_t ci = 0; ci < clusters.size(); ci++) {
			std::vector<uint32_t>& I = collTri[ci];
			if (I.size() < 3) continue;
			std::vector<uint32_t> d(I.size());
			size_t n = meshopt_simplifySloppy(d.data(), I.data(), I.size(), collPos[ci].data(), collPos[ci].size() / 3, 12, (size_t)(I.size() / 3 * keep) * 3, 1.0f, nullptr);
			d.resize(n);
			I.swap(d);
			collTotal += I.size() / 3;
		}
		printf("  collision: %zu triangles after clustering\n", collTotal);
	}
	std::string mpath = outDir + "/" + mapName + ".map";
	FILE* mf = fopen(mpath.c_str(), "w");
	if (!mf) { printf("cannot write %s\n", mpath.c_str()); fflush(stdout); _exit(1); }
	fprintf(mf, "// %s - collision and entities for the KatarnEngine HD world %s.kworld (the visible world is not in the BSP)\n", mapName.c_str(), mapName.c_str());
	fprintf(mf, "{\n\"classname\" \"worldspawn\"\n\"message\" \"%s (KatarnEngine HD world)\"\n\"_katarn\" \"1\"\n", mapName.c_str());
	auto box = [&](float x0, float y0, float z0, float x1, float y1, float z1, const char* sh) {
		fprintf(mf, "{\n");
		fprintf(mf, "( %g %g %g ) ( %g %g %g ) ( %g %g %g ) %s 0 0 0 0.5 0.5 0 0 0\n", x0, y0, z1, x0, y1, z1, x1, y1, z1, sh);
		fprintf(mf, "( %g %g %g ) ( %g %g %g ) ( %g %g %g ) %s 0 0 0 0.5 0.5 0 0 0\n", x0, y1, z0, x0, y0, z0, x1, y0, z0, sh);
		fprintf(mf, "( %g %g %g ) ( %g %g %g ) ( %g %g %g ) %s 0 0 0 0.5 0.5 0 0 0\n", x0, y0, z0, x0, y1, z0, x0, y1, z1, sh);
		fprintf(mf, "( %g %g %g ) ( %g %g %g ) ( %g %g %g ) %s 0 0 0 0.5 0.5 0 0 0\n", x1, y1, z0, x1, y0, z0, x1, y0, z1, sh);
		fprintf(mf, "( %g %g %g ) ( %g %g %g ) ( %g %g %g ) %s 0 0 0 0.5 0.5 0 0 0\n", x1, y0, z0, x0, y0, z0, x0, y0, z1, sh);
		fprintf(mf, "( %g %g %g ) ( %g %g %g ) ( %g %g %g ) %s 0 0 0 0.5 0.5 0 0 0\n", x0, y1, z0, x1, y1, z0, x1, y1, z1, sh);
		fprintf(mf, "}\n");
	};
	const char* sky = "katarn/sky";
	box(lo[0] - T, lo[1] - T, lo[2] - T, hi[0] + T, hi[1] + T, lo[2], sky);
	box(lo[0] - T, lo[1] - T, hi[2], hi[0] + T, hi[1] + T, hi[2] + T, sky);
	box(lo[0] - T, lo[1] - T, lo[2], lo[0], hi[1] + T, hi[2], sky);
	box(hi[0], lo[1] - T, lo[2], hi[0] + T, hi[1] + T, hi[2], sky);
	box(lo[0], lo[1] - T, lo[2], hi[0], lo[1], hi[2], sky);
	box(lo[0], hi[1], lo[2], hi[0], hi[1] + T, hi[2], sky);
	size_t brushes = 0;
	std::vector<float> groundTris;
	const float thick = 8.0f;
	const unsigned DETAIL = 0x8000000;
	for (size_t ci = 0; ci < clusters.size(); ci++) {
		const std::vector<uint32_t>& I = collTri[ci];
		const std::vector<float>& P = collPos[ci];
		for (size_t t = 0; t + 2 < I.size(); t += 3) {
			const float* a = &P[I[t] * 3];
			const float* b = &P[I[t + 1] * 3];
			const float* cc = &P[I[t + 2] * 3];
			float ctr[3] = { (a[0] + b[0] + cc[0]) / 3, (a[1] + b[1] + cc[1]) / 3, (a[2] + b[2] + cc[2]) / 3 };
			if (!inCore(ctr)) continue;
			float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { cc[0] - a[0], cc[1] - a[1], cc[2] - a[2] };
			float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
			float area2 = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
			auto len = [](const float* p, const float* q) { return sqrtf((p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2])); };
			float lmax = std::max(len(a, b), std::max(len(b, cc), len(cc, a)));
			if (area2 < 4.0f || lmax < 2.0f || area2 / lmax < 1.0f) continue;
			for (int q = 0; q < 3; q++) n[q] /= area2;
			if (n[2] > 0.5f) { groundTris.insert(groundTris.end(), a, a + 3); groundTris.insert(groundTris.end(), b, b + 3); groundTris.insert(groundTris.end(), cc, cc + 3); }
			float a2[3], b2[3], c2[3], an[3], bn[3], cn[3];
			for (int q = 0; q < 3; q++) { a2[q] = a[q] - n[q] * thick; b2[q] = b[q] - n[q] * thick; c2[q] = cc[q] - n[q] * thick; an[q] = a[q] + n[q] * 16; bn[q] = b[q] + n[q] * 16; cn[q] = cc[q] + n[q] * 16; }
			const char* cs = "katarn/collision";
			fprintf(mf, "{\n");
			fprintf(mf, "( %.3f %.3f %.3f ) ( %.3f %.3f %.3f ) ( %.3f %.3f %.3f ) %s 0 0 0 0.5 0.5 %u 0 0\n", a[0], a[1], a[2], cc[0], cc[1], cc[2], b[0], b[1], b[2], cs, DETAIL);
			fprintf(mf, "( %.3f %.3f %.3f ) ( %.3f %.3f %.3f ) ( %.3f %.3f %.3f ) %s 0 0 0 0.5 0.5 %u 0 0\n", a2[0], a2[1], a2[2], b2[0], b2[1], b2[2], c2[0], c2[1], c2[2], cs, DETAIL);
			fprintf(mf, "( %.3f %.3f %.3f ) ( %.3f %.3f %.3f ) ( %.3f %.3f %.3f ) %s 0 0 0 0.5 0.5 %u 0 0\n", a[0], a[1], a[2], an[0], an[1], an[2], b[0], b[1], b[2], cs, DETAIL);
			fprintf(mf, "( %.3f %.3f %.3f ) ( %.3f %.3f %.3f ) ( %.3f %.3f %.3f ) %s 0 0 0 0.5 0.5 %u 0 0\n", b[0], b[1], b[2], bn[0], bn[1], bn[2], cc[0], cc[1], cc[2], cs, DETAIL);
			fprintf(mf, "( %.3f %.3f %.3f ) ( %.3f %.3f %.3f ) ( %.3f %.3f %.3f ) %s 0 0 0 0.5 0.5 %u 0 0\n", cc[0], cc[1], cc[2], cn[0], cn[1], cn[2], a[0], a[1], a[2], cs, DETAIL);
			fprintf(mf, "}\n");
			brushes++;
		}
	}
	fprintf(mf, "}\n");
	// spawn: the Blender camera, or the middle of the core, dropped onto the ground
	float sp[3] = { (cmin[0] + cmax[0]) * 0.5f, (cmin[1] + cmax[1]) * 0.5f, cmax[2] };
	float yaw = 0;
	const Json& cam = scene["camera"];
	if (cam.t == Json::OBJ) {
		const Json& M = cam["matrix"];
		sp[0] = (float)M[(size_t)3].num() * UNITS; sp[1] = (float)M[(size_t)7].num() * UNITS; sp[2] = (float)M[(size_t)11].num() * UNITS;
		yaw = atan2f(-(float)M[(size_t)6].num(), -(float)M[(size_t)2].num()) * 57.2958f;
	}
	float best = -1e30f;
	for (size_t i = 0; i + 8 < groundTris.size(); i += 9) {
		const float* a = &groundTris[i], * b = &groundTris[i + 3], * c = &groundTris[i + 6];
		float d = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1]);
		if (fabsf(d) < 1e-6f) continue;
		float l1 = ((b[1] - c[1]) * (sp[0] - c[0]) + (c[0] - b[0]) * (sp[1] - c[1])) / d;
		float l2 = ((c[1] - a[1]) * (sp[0] - c[0]) + (a[0] - c[0]) * (sp[1] - c[1])) / d;
		float l3 = 1 - l1 - l2;
		if (l1 < 0 || l2 < 0 || l3 < 0) continue;
		float z = l1 * a[2] + l2 * b[2] + l3 * c[2];
		if (z <= sp[2] + 8 && z > best) best = z;
	}
	if (best <= -1e29f && !groundTris.empty()) {
		// nothing under the camera / centre: the nearest walkable triangle
		float bd = 1e30f;
		for (size_t i = 0; i + 8 < groundTris.size(); i += 9) {
			float cx = (groundTris[i] + groundTris[i + 3] + groundTris[i + 6]) / 3, cy = (groundTris[i + 1] + groundTris[i + 4] + groundTris[i + 7]) / 3;
			float cz = (groundTris[i + 2] + groundTris[i + 5] + groundTris[i + 8]) / 3;
			float d = (cx - sp[0]) * (cx - sp[0]) + (cy - sp[1]) * (cy - sp[1]);
			if (d < bd) { bd = d; sp[0] = cx; sp[1] = cy; best = cz; }
		}
	}
	if (best > -1e29f) sp[2] = best + 40;
	fprintf(mf, "{\n\"classname\" \"info_player_start\"\n\"origin\" \"%.0f %.0f %.0f\"\n\"angle\" \"%.0f\"\n}\n", sp[0], sp[1], sp[2], yaw);
	fclose(mf);
	printf("  wrote %s: %zu collision brushes, sky shell %.0f x %.0f x %.0f, spawn %.0f %.0f %.0f\n", mpath.c_str(), brushes,
		hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], sp[0], sp[1], sp[2]);
	std::string spath = outDir + "/katarn_hd.shader";
	FILE* sf = fopen(spath.c_str(), "w");
	if (sf) {
		fprintf(sf, "// KatarnEngine HD worlds: the BSP is collision only, the visible world is the .kworld\n"
			"textures/katarn/collision\n{\n\tqer_editorimage textures/common/clip\n\tsurfaceparm nodraw\n\tsurfaceparm nolightmap\n\tsurfaceparm nomarks\n\tsurfaceparm trans\n}\n\n"
			"textures/katarn/sky\n{\n\tqer_editorimage textures/common/sky\n\tsurfaceparm sky\n\tsurfaceparm noimpact\n\tsurfaceparm nolightmap\n\tsurfaceparm nomarks\n\tskyparms - - -\n}\n");
		fclose(sf);
	}
	printf("done in %.0fs\n", Now() - t0);
	g_cookDone = true;
	watchdog.join();
	fclose(g_spill);
	remove(spillPath.c_str());
	printf("  peak memory %zu MB\n", (size_t)g_peakMB);
	return 0;
}
