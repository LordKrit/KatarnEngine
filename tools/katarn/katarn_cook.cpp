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
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif
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
static float COLL_ERROR = 4.0f;
static float COLL_PAD = 8192.0f;
static bool COLL_ONLY = false;
static float COLL_WALL_MUL = 4.0f;  // walls / props: this many times the floor error     // --collision-only: rebuild the .map from an existing .kworld      // collision margin around the playable core
static size_t COLL_BUDGET = 250000;
// objects left out (name contains): fog volumes, particle FX emitters and cards (smoke, fire, mist, birds - their
// look comes from per-particle attributes the bake cannot see), battle wreckage
static std::vector<std::string> SKIP = { "HeightFog", "Sky_Cottonball", "FX_", "fxsrc_", "DroidWreck", "AATWreck", "AATDebris",
	"RubblePile", "BD_FogBillboard", "InvalidateLightPlane", "WindowEnlightenPlane" };
// materials left out (name contains): scattered wreckage and ground clutter too small to see past a few metres
static std::vector<std::string> SKIPMAT = { "DroidWreck", "Twig", "StreetDebris" };
// scattered instances thinned (material name contains, fraction kept)
static std::vector<std::pair<std::string, float>> THIN = { { "GroundLeaves", 0.5f } };
// lights left out (name contains)
static std::vector<std::string> SKIPLIGHT;
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
struct TexOut { KwTexture k; std::vector<uint8_t> data; uint64_t spillAt = 0, bytes = 0; std::string name; };
static std::vector<TexOut> textures;
static std::mutex texMutex;
static std::map<uint64_t, int> texByHash;

// alpha-tested textures: each mip's alpha is scaled so the same share of texels passes the 0.5 test as in the full
// image (plain averaging thins leaves out with every mip until trees and hedges vanish in the distance)
static float AlphaCoverage(const BakeImage& m, float scale)
{
	size_t n = (size_t)m.w * m.h, pass = 0;
	for (size_t i = 0; i < n; i++) if (m.px[i * 4 + 3] * scale >= 0.5f) pass++;
	return n ? (float)pass / n : 0.0f;
}
static void PreserveCoverage(BakeImage& m, float target)
{
	float lo = 0.25f, hi = 16.0f;
	for (int it = 0; it < 14; it++) { float mid = sqrtf(lo * hi); if (AlphaCoverage(m, mid) < target) lo = mid; else hi = mid; }
	for (size_t i = 3; i < m.px.size(); i += 4) m.px[i] = std::min(1.0f, m.px[i] * hi);
}

static void EncodeImage(const BakeImage& img, TexKind kind, uint32_t format, std::vector<uint8_t>& out, uint32_t& mips, bool parallel, bool alphaTest = false)
{
	std::vector<BakeImage> chain;
	chain.push_back(img);
	const float coverage = alphaTest ? AlphaCoverage(img, 1.0f) : 0.0f;
	while (chain.back().w > 1 || chain.back().h > 1) {
		const BakeImage& p = chain.back();
		BakeImage n = Resample(p, std::max(1, p.w / 2), std::max(1, p.h / 2));
		if (kind == TK_NORMAL)
			for (size_t i = 0; i < n.px.size(); i += 4) {
				float x = n.px[i] * 2 - 1, y = n.px[i + 1] * 2 - 1, z = n.px[i + 2] * 2 - 1;
				float l = sqrtf(x * x + y * y + z * z); if (l < 1e-6f) l = 1;
				n.px[i] = x / l * 0.5f + 0.5f; n.px[i + 1] = y / l * 0.5f + 0.5f; n.px[i + 2] = z / l * 0.5f + 0.5f;
			}
		if (alphaTest && coverage > 0.0f && n.w * n.h >= 4) PreserveCoverage(n, coverage);
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
static int AddTexture(const BakeImage& img, TexKind kind, bool parallel, bool alphaTest = false)
{
	uint64_t h = HashImage(img, kind) ^ (alphaTest ? 0x9e3779b97f4a7c15ull : 0);
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
	EncodeImage(img, kind, fmt, t.data, t.k.mips, parallel, alphaTest);
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
	bool fxCard = false, skipped = false;
	double area = 0;            // world area using it (units^2)
	int srcRes = 0;             // largest source image side
	int res = 0;                // bake resolution
	std::vector<std::string> images;
	bool planar = false;        // textures projected from world X / Y: UVs are generated, one tile is baked
	float period[2] = { 1, 1 }; // tile size, Blender metres
	float repeats = 1;          // how many times the most repeated source image fits in a tile
};
static std::vector<MatInfo> mats;
static std::map<std::string, uint32_t> matIndex;
static std::vector<KwMaterial> kmats;

static const Json* g_groups = nullptr;
static const char* OutputShaderNode(const Json& nodes);

// what the material's shader tree is made of: a Transparent BSDF (cut-out leaves, railings, decals), a Translucent
// BSDF (foliage), or a Principled BSDF whose Alpha is driven / below 1. Blender's alphaMode only knows the last.
struct ShaderTraits { bool transparent = false, translucent = false; };
static void WalkShader(const Json& nodes, const std::string& name, ShaderTraits& t, int depth)
{
	if (depth > 24 || !nodes.has(name.c_str())) return;
	const Json& n = nodes[name.c_str()];
	std::string type = n["type"].str();
	auto follow = [&](const Json& in) { if (in["link"].t == Json::ARR) WalkShader(nodes, in["link"][(size_t)0].str(), t, depth + 1); };
	if (type == "BSDF_TRANSPARENT") { t.transparent = true; return; }
	if (type == "BSDF_TRANSLUCENT") { t.translucent = true; return; }
	if (type == "BSDF_PRINCIPLED") {
		for (auto& in : n["inputs"].a) {
			std::string id = in["id"].str();
			if (id == "Alpha" && (in["link"].t == Json::ARR || (in["value"].t == Json::NUM && in["value"].n < 0.999))) t.transparent = true;
		}
		return;
	}
	if (type == "MIX_SHADER" || type == "ADD_SHADER" || type == "REROUTE") {
		for (auto& in : n["inputs"].a) if (in["type"].str() == "SHADER" || type == "REROUTE") follow(in);
		return;
	}
	if (type == "GROUP" && g_groups) {
		const Json& gt = (*g_groups)[n["props"]["tree"].str().c_str()];
		if (gt.t != Json::OBJ) return;
		const Json& gn = gt["nodes"];
		for (auto& kv : gn.o) if (kv.second["type"].str() == "GROUP_OUTPUT")
			for (auto& si : kv.second["inputs"].a) if (si["type"].str() == "SHADER" && si["link"].t == Json::ARR)
				WalkShader(gn, si["link"][(size_t)0].str(), t, depth + 1);
	}
}
static ShaderTraits GetShaderTraits(const Json& m)
{
	ShaderTraits t;
	if (m["graph"].t != Json::OBJ) return t;
	const char* out = OutputShaderNode(m["graph"]["nodes"]);
	if (out) WalkShader(m["graph"]["nodes"], out, t, 0);
	return t;
}

static uint32_t MaterialFlags(const Json& m)
{
	uint32_t f = 0;
	std::string mode = m["alphaMode"].str("OPAQUE");
	if (mode == "MASK" || mode == "BLEND") f |= KWM_MASK;
	if (mode == "BLEND") f |= KWM_BLEND;
	if (m["twoSided"].t == Json::BOOL && m["twoSided"].b) f |= KWM_TWOSIDED;
	ShaderTraits st = GetShaderTraits(m);
	if (st.transparent) f |= KWM_MASK;            // classified before baking: masked clusters get alpha-tested shadows
	if (st.translucent) f |= KWM_FOLIAGE | KWM_TWOSIDED;
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

// ================================================================================================ splat (terrain) materials
// Terrain materials in the export blend tiled texture layers by world-position masks (Geometry > Position >
// Mapping > Image). A bake over UV space cannot represent that, so they are recognised here and turned into a
// splat record the shader evaluates per pixel (see KwSplatHeader).
static std::vector<uint8_t> splatBlob;
static std::mutex splatMutex;

struct GraphView {
	const Json& nodes;
	const Json* node(const std::string& n) const { const Json& j = nodes[n.c_str()]; return j.t == Json::OBJ ? &j : nullptr; }
	static const Json* input(const Json& nd, const char* id)
	{
		for (auto& i : nd["inputs"].a) if (i["id"].str() == id) return &i;
		for (auto& i : nd["inputs"].a) if (i["name"].str() == id && !(i["enabled"].t == Json::BOOL && !i["enabled"].b)) return &i;
		return nullptr;
	}
	// the (node, socket) feeding an input, or false
	bool link(const Json& nd, const char* id, std::string& from, std::string& sock) const
	{
		const Json* in = input(nd, id);
		if (!in) return false;
		const Json& l = (*in)["link"];
		if (l.t != Json::ARR || l.size() != 2) return false;
		from = l[(size_t)0].str(); sock = l[1].str();
		return true;
	}
	float value(const Json& nd, const char* id, int c = 0, float d = 0) const
	{
		const Json* in = input(nd, id);
		if (!in) return d;
		const Json& v = (*in)["value"];
		if (v.t == Json::NUM) return (float)v.n;
		if (v.t == Json::ARR && (size_t)c < v.size()) return (float)v[(size_t)c].num(d);
		return d;
	}
	// a MAPPING (or nothing) between Geometry > Position and an image: scale + location, rotation must be 0
	bool positionMapping(const Json& tex, float xf[4]) const
	{
		std::string from, sock;
		if (!link(tex, "Vector", from, sock)) return false;
		const Json* m = node(from);
		if (!m) return false;
		std::string t = (*m)["type"].str();
		if (t == "NEW_GEOMETRY") { if (sock != "Position") return false; xf[0] = xf[1] = 1; xf[2] = xf[3] = 0; return true; }
		if (t != "MAPPING") return false;
		std::string f2, s2;
		if (!link(*m, "Vector", f2, s2)) return false;
		const Json* g = node(f2);
		if (!g || (*g)["type"].str() != "NEW_GEOMETRY" || s2 != "Position") return false;
		if (fabsf(value(*m, "Rotation", 2)) > 1e-4f || fabsf(value(*m, "Rotation", 0)) > 1e-4f || fabsf(value(*m, "Rotation", 1)) > 1e-4f) return false;
		xf[0] = value(*m, "Scale", 0, 1); xf[1] = value(*m, "Scale", 1, 1);
		xf[2] = value(*m, "Location", 0); xf[3] = value(*m, "Location", 1);
		return true;
	}
};

struct SplatLayerSrc { std::string color, noh; float scale = 1; std::string mask; int chan = 0; float smin = 0, smax = 1; std::string mapping; bool normalOnly = false; };


// ================================================================================================ planar projected materials
// "Top projected" materials texture by world X / Y (Geometry > Position > Mapping or Multiply-Add > Image). When every
// such image repeats a whole number of times over one period, one tile can be baked and the cooker generates UVs
// from the world position instead (the sampler repeats the tile).
static bool LinearAxis(const GraphView& g, const std::string& from, const std::string& sock, int& axis, float& a, float& b)
{
	const Json* n = g.node(from);
	if (!n) return false;
	std::string t = (*n)["type"].str();
	if (t == "SEPXYZ") {
		std::string f, s2;
		if (!g.link(*n, "Vector", f, s2) || s2 != "Position") return false;
		const Json* geo = g.node(f);
		if (!geo || (*geo)["type"].str() != "NEW_GEOMETRY") return false;
		axis = sock == "X" ? 0 : sock == "Y" ? 1 : 2;
		a = 1; b = 0;
		return axis < 2;
	}
	if (t == "MATH") {
		std::string op = (*n)["props"]["operation"].str();
		std::string f, s2;
		if (!g.link(*n, "Value", f, s2)) return false;
		if (!LinearAxis(g, f, s2, axis, a, b)) return false;
		std::string dummy, d2;
		if (g.link(*n, "Value_001", dummy, d2) || g.link(*n, "Value_002", dummy, d2)) return false;
		float v1 = g.value(*n, "Value_001"), v2 = g.value(*n, "Value_002");
		if (op == "MULTIPLY_ADD") { a *= v1; b = b * v1 + v2; return true; }
		if (op == "MULTIPLY") { a *= v1; b *= v1; return true; }
		if (op == "ADD") { b += v1; return true; }
		return false;
	}
	return false;
}
// the image's coordinate as u = a[0] x + b[0], v = a[1] y + b[1]
static bool PlanarCoords(const GraphView& g, const Json& tex, float a[2], float b[2])
{
	float xf[4];
	if (g.positionMapping(tex, xf)) { a[0] = xf[0]; a[1] = xf[1]; b[0] = xf[2]; b[1] = xf[3]; return true; }
	std::string f, s2;
	if (!g.link(tex, "Vector", f, s2)) return false;
	const Json* c = g.node(f);
	if (!c || (*c)["type"].str() != "COMBXYZ") return false;
	std::string xf2, xs, yf, ys;
	if (!g.link(*c, "X", xf2, xs) || !g.link(*c, "Y", yf, ys)) return false;
	int ax0, ax1; float a0, b0, a1, b1;
	if (!LinearAxis(g, xf2, xs, ax0, a0, b0) || !LinearAxis(g, yf, ys, ax1, a1, b1) || ax0 != 0 || ax1 != 1) return false;
	a[0] = a0; b[0] = b0; a[1] = a1; b[1] = b1;
	return true;
}
static bool DetectPlanar(const Json& m, float period[2], float& repeats)
{
	const Json& graph = m["graph"];
	if (graph.t != Json::OBJ) return false;
	GraphView g{ graph["nodes"] };
	bool usesPos = false;
	for (auto& kv : graph["nodes"].o)
		for (auto& in : kv.second["inputs"].a) {
			const Json& l = in["link"];
			if (l.t != Json::ARR || l.size() != 2) continue;
			const Json* src = g.node(l[(size_t)0].str());
			if (src && (*src)["type"].str() == "NEW_GEOMETRY") {
				if (l[1].str() != "Position") return false;    // normal / pointiness etc.: not a plain projection
				usesPos = true;
			}
			if (src && (*src)["type"].str() == "SEPXYZ" && l[1].str() == "Z") {
				std::string f, s2;
				if (g.link(*src, "Vector", f, s2) && s2 == "Position") return false;   // height dependent
			}
		}
	if (!usesPos) return false;
	std::vector<std::array<float, 2>> as;
	for (auto& kv : graph["nodes"].o) {
		const Json& n = kv.second;
		if (n["type"].str() != "TEX_IMAGE") continue;
		std::string f, s2;
		if (!g.link(n, "Vector", f, s2)) continue;   // UV mapped image: fine (but then the tile would not match)
		float a[2], b[2];
		if (!PlanarCoords(g, n, a, b)) return false;
		if (n["props"]["extension"].str("REPEAT") != "REPEAT") return false;
		if (fabsf(a[0]) < 1e-5f || fabsf(a[1]) < 1e-5f) return false;
		as.push_back({ fabsf(a[0]), fabsf(a[1]) });
	}
	if (as.empty()) return false;
	repeats = 1;
	for (int ax = 0; ax < 2; ax++) {
		float base = 1.0f / as[0][ax], best = 0;
		for (int k = 1; k <= 24 && !best; k++) {
			float P = base * k;
			if (P > 64.0f) break;
			bool ok = true;
			for (auto& a : as) { float r = a[ax] * P; if (fabsf(r - roundf(r)) > 0.02f * std::max(1.0f, r)) { ok = false; break; } }
			if (ok) best = P;
		}
		if (!best) return false;
		period[ax] = best;
		for (auto& a : as) repeats = std::max(repeats, a[ax] * best);
	}
	return true;
}
static int SplatTexture(const std::string& file, TexKind kind)
{
	auto img = LoadSource(file, kind == TK_COLOR);
	if (!img) return -1;
	BakeImage b = *img;
	if (b.w > MAXTEX || b.h > MAXTEX) b = Resample(b, std::min(b.w, MAXTEX), std::min(b.h, MAXTEX));
	return AddTexture(b, kind, true);
}

// returns true and fills k when the material is a splat terrain
static bool TrySplat(const Json& m, KwMaterial& k, std::vector<std::string>& warnings, bool dry = false)
{
	const Json& graph = m["graph"];
	if (graph.t != Json::OBJ) return false;
	GraphView g{ graph["nodes"] };
	const Json* bsdf = nullptr;
	for (auto& kv : graph["nodes"].o) if (kv.second["type"].str() == "BSDF_PRINCIPLED") { bsdf = &kv.second; break; }
	if (!bsdf) return false;
	std::string from, sock;
	if (!g.link(*bsdf, "Base Color", from, sock)) return false;
	KwSplatHeader h = {};
	h.tintTex = -1; h.tintMul = 1; h.normalStrength = 1; h.invUnits = 1.0f / UNITS; h.roughness = g.value(*bsdf, "Roughness", 0, 0.5f);
	std::string tintFile;
	// optional tint: MULTIPLY(chain, SCALE(image))
	const Json* top = g.node(from);
	if (top && (*top)["type"].str() == "MIX" && (*top)["props"]["blend_type"].str() == "MULTIPLY") {
		std::string bf, bs, af, as;
		if (g.link(*top, "B_Color", bf, bs) && g.link(*top, "A_Color", af, as)) {
			const Json* vm = g.node(bf);
			std::string tf, ts;
			if (vm && (*vm)["type"].str() == "VECT_MATH" && g.link(*vm, "Vector", tf, ts)) {
				const Json* ti = g.node(tf);
				if (ti && (*ti)["type"].str() == "TEX_IMAGE" && g.positionMapping(*ti, h.tintXf)) {
					tintFile = (*ti)["props"]["image"].str();
					h.tintMul = g.value(*vm, "Scale", 0, 1);
				}
			}
			from = af; sock = as;
		}
	}
	// walk a MIX chain down to its base image: returns layers base first
	auto walk = [&](std::string n, std::vector<SplatLayerSrc>& out) -> bool {
		std::vector<SplatLayerSrc> rev;
		for (int guard = 0; guard < 64; guard++) {
			const Json* nd = g.node(n);
			if (!nd) return false;
			std::string t = (*nd)["type"].str();
			if (t == "TEX_IMAGE") {
				SplatLayerSrc L; L.color = (*nd)["props"]["image"].str();
				float xf[4];
				if (!g.positionMapping(*nd, xf)) return false;
				L.scale = xf[0];
				std::string vf, vs; g.link(*nd, "Vector", vf, vs); L.mapping = vf;
				rev.push_back(L);
				break;
			}
			if (t != "MIX" || (*nd)["props"]["blend_type"].str() != "MIX") return false;
			std::string ff, fs, bf, bs, af, as;
			bool hasF = g.link(*nd, "Factor_Float", ff, fs) || g.link(*nd, "Factor", ff, fs);
			bool hasB = g.link(*nd, "B_Color", bf, bs) || g.link(*nd, "B_Float", bf, bs) || g.link(*nd, "B", bf, bs);
			bool hasA = g.link(*nd, "A_Color", af, as) || g.link(*nd, "A_Float", af, as) || g.link(*nd, "A", af, as);
			if (!hasF || !hasB || !hasA) return false;
			SplatLayerSrc L;
			const Json* bi = g.node(bf);
			if (!bi || (*bi)["type"].str() != "TEX_IMAGE") return false;
			float xf[4];
			if (!g.positionMapping(*bi, xf)) return false;
			L.color = (*bi)["props"]["image"].str(); L.scale = xf[0];
			std::string vf, vs; g.link(*bi, "Vector", vf, vs); L.mapping = vf;
			// weight
			const Json* fn = g.node(ff);
			if (!fn) return false;
			std::string ft = (*fn)["type"].str();
			const Json* maskImg = nullptr;
			if (ft == "SEPARATE_COLOR") {
				L.chan = fs == "Red" ? 0 : fs == "Green" ? 1 : fs == "Blue" ? 2 : 3;
				std::string mf, ms;
				if (!g.link(*fn, "Color", mf, ms)) return false;
				maskImg = g.node(mf);
			}
			else if (ft == "TEX_IMAGE") { L.chan = fs == "Alpha" ? 3 : 0; maskImg = fn; }
			else if (ft == "MAP_RANGE") {
				std::string vf2, vs2;
				if (!g.link(*fn, "Value", vf2, vs2)) return false;
				const Json* sx = g.node(vf2);
				std::string gf, gs;
				if (!sx || (*sx)["type"].str() != "SEPXYZ" || vs2 != "Z" || !g.link(*sx, "Vector", gf, gs) || gs != "Normal") return false;
				float f0 = g.value(*fn, "From Min"), f1 = g.value(*fn, "From Max", 0, 1), t0 = g.value(*fn, "To Min"), t1 = g.value(*fn, "To Max", 0, 1);
				if (t0 > t1) { L.smin = f0; L.smax = f1; } else { L.smin = f1; L.smax = f0; }   // weight = 1 at smin, 0 at smax
				L.chan = 4;
			}
			else return false;
			if (maskImg) {
				if ((*maskImg)["type"].str() != "TEX_IMAGE") return false;
				float mxf[4];
				if (!g.positionMapping(*maskImg, mxf)) return false;
				L.mask = (*maskImg)["props"]["image"].str();
				memcpy(h.maskXf, mxf, 16);
			}
			rev.push_back(L);
			n = af;
		}
		if (rev.empty()) return false;
		out.assign(rev.rbegin(), rev.rend());
		return true;
	};
	std::vector<SplatLayerSrc> col;
	if (!walk(from, col) || col.size() < 2) return false;
	// normal chain: Normal Map <- (Combine Color <- Separate Color <-) chain
	std::vector<SplatLayerSrc> nrm;
	std::string nf, ns;
	if (g.link(*bsdf, "Normal", nf, ns)) {
		const Json* nm = g.node(nf);
		if (nm && (*nm)["type"].str() == "NORMAL_MAP") {
			h.normalStrength = g.value(*nm, "Strength", 0, 1);
			std::string cf, cs;
			if (g.link(*nm, "Color", cf, cs)) {
				const Json* cc = g.node(cf);
				std::string rf, rs;
				if (cc && (*cc)["type"].str() == "COMBINE_COLOR" && g.link(*cc, "Red", rf, rs)) {
					const Json* sc = g.node(rf);
					std::string sf, ss;
					if (sc && (*sc)["type"].str() == "SEPARATE_COLOR" && g.link(*sc, "Color", sf, ss)) walk(sf, nrm);
				}
			}
		}
	}
	// roughness: from the layers' alpha (smoothness) when it is a chain
	std::string rf, rs;
	if (g.link(*bsdf, "Roughness", rf, rs)) h.roughness = -1;
	// merge: the normal chain sets the order (it may hold normal-only layers); its images are the colour layers' NOH
	std::vector<SplatLayerSrc> layers;
	size_t ci = 0;
	for (auto& n : nrm) {
		if (ci < col.size() && n.mapping == col[ci].mapping) { SplatLayerSrc L = col[ci++]; L.noh = n.color; layers.push_back(L); }
		else { SplatLayerSrc L = n; L.noh = n.color; L.color.clear(); L.normalOnly = true; layers.push_back(L); }
	}
	for (; ci < col.size(); ci++) layers.push_back(col[ci]);
	if (layers.empty() || layers[0].normalOnly) return false;
	if (getenv("KT_SPLAT_DEBUG")) {
		printf("splat %s: tint %s x%.2f, normal %.2f, rough %.2f, mask xf %g %g %g %g\n", m["name"].str("?").c_str(), tintFile.c_str(), h.tintMul, h.normalStrength, h.roughness, h.maskXf[0], h.maskXf[1], h.maskXf[2], h.maskXf[3]);
		for (auto& L : layers) printf("   %-40s noh %-40s scale %.3f mask %s.%d slope %.2f-%.2f%s\n", L.color.c_str(), L.noh.c_str(), L.scale, L.mask.c_str(), L.chan, L.smin, L.smax, L.normalOnly ? " NORMAL-ONLY" : "");
	}
	if (dry) return true;
	// textures
	std::map<std::string, int> cache;
	auto tex = [&](const std::string& f, TexKind kind) -> int {
		if (f.empty()) return -1;
		std::string key = f + "|" + std::to_string((int)kind);
		auto it = cache.find(key);
		if (it != cache.end()) return it->second;
		int t = SplatTexture(f, kind);
		if (t < 0) warnings.push_back("splat: cannot read " + f);
		return cache[key] = t;
	};
	h.numLayers = (uint32_t)layers.size();
	h.tintTex = tex(tintFile, TK_DATA);
	std::vector<KwSplatLayer> out;
	for (auto& L : layers) {
		KwSplatLayer o = {};
		o.albedoTex = L.normalOnly ? -1 : tex(L.color, TK_COLOR);
		o.nohTex = tex(L.noh, TK_DATA);
		o.scale = L.scale;
		o.maskTex = L.chan == 4 ? -1 : tex(L.mask, TK_DATA);
		o.maskChan = (uint32_t)L.chan;
		o.slopeMin = L.smin; o.slopeMax = L.smax;
		o.flags = L.normalOnly ? KWS_NORMAL_ONLY : 0;
		out.push_back(o);
	}
	std::lock_guard<std::mutex> lk(splatMutex);
	k.albedoTex = (int32_t)splatBlob.size();
	k.flags |= KWM_SPLAT;
	k.baseColor[0] = k.baseColor[1] = k.baseColor[2] = k.baseColor[3] = 1;
	k.roughness = h.roughness < 0 ? 1.0f : h.roughness;
	const uint8_t* hp = (const uint8_t*)&h;
	splatBlob.insert(splatBlob.end(), hp, hp + sizeof(h));
	const uint8_t* lp = (const uint8_t*)out.data();
	splatBlob.insert(splatBlob.end(), lp, lp + out.size() * sizeof(KwSplatLayer));
	char buf[160];
	snprintf(buf, sizeof(buf), "splat terrain: %zu layers (evaluated in the shader)", out.size());
	warnings.push_back(buf);
	return true;
}


// ================================================================================================ water
// Still water (rivers, fountains): a volume-absorption material or one named like water. Rendered by the FX pass
// (animated waves, sky reflection, depth absorption), so only its parameters are kept.
static bool IsWaterMaterial(const std::string& name, const Json& m)
{
	std::string lo = name;
	for (auto& c : lo) c = (char)tolower((unsigned char)c);
	if (lo.find("waterfall") != std::string::npos || lo.find("_fal") != std::string::npos) return false;
	const Json& graph = m["graph"];
	if (graph.t == Json::OBJ)
		for (auto& kv : graph["nodes"].o) if (kv.second["type"].str() == "VOLUME_ABSORPTION") return true;
	if (lo.find("water") != std::string::npos) return true;
	size_t w = lo.find("_wat");
	return w != std::string::npos && (w + 4 == lo.size() || lo[w + 4] == '.' || lo[w + 4] == '_');
}
static void WaterParams(const Json& m, KwMaterial& k)
{
	k.baseColor[0] = 0.02f; k.baseColor[1] = 0.05f; k.baseColor[2] = 0.05f; k.baseColor[3] = 1;
	k.emissive[0] = 0.25f; k.emissive[1] = 0.55f; k.emissive[2] = 0.5f;
	k.roughness = 0.03f; k.normalScale = 0.25f; k.metallic = 0;
	const Json& graph = m["graph"];
	if (graph.t == Json::OBJ) {
		GraphView g{ graph["nodes"] };
		for (auto& kv : graph["nodes"].o) {
			const Json& n = kv.second;
			std::string t = n["type"].str();
			if (t == "BSDF_PRINCIPLED") {
				std::string f, s2;
				if (!g.link(n, "Base Color", f, s2)) for (int c = 0; c < 3; c++) k.baseColor[c] = g.value(n, "Base Color", c, k.baseColor[c]);
				if (!g.link(n, "Roughness", f, s2)) k.roughness = g.value(n, "Roughness", 0, k.roughness);
			}
			if (t == "VOLUME_ABSORPTION") for (int c = 0; c < 3; c++) k.emissive[c] = g.value(n, "Color", c, k.emissive[c]);
			if (t == "BUMP") k.normalScale = g.value(n, "Strength", 0, k.normalScale);
		}
	}
	else {
		const Json& bc = m["baseColor"];
		if (bc.t == Json::ARR) for (int c = 0; c < 3; c++) k.baseColor[c] = (float)bc[(size_t)c].num(k.baseColor[c]);
	}
	k.flags = (k.flags & ~(uint32_t)(KWM_MASK | KWM_BLEND)) | KWM_WATER | KWM_TWOSIDED;
}

// ================================================================================================ flowing water
// Waterfall sheets: alpha built from scrolling texture channels (Mapping Location <- Vector Math Scale <- Value,
// i.e. animated by the frame). Parsed into a flow record the FX pass animates.
static bool TryFlow(const Json& m, KwMaterial& k, std::vector<std::string>& warnings, bool dry = false)
{
	if (m["alphaMode"].str() != "BLEND") return false;
	const Json& graph = m["graph"];
	if (graph.t != Json::OBJ) return false;
	GraphView g{ graph["nodes"] };
	const Json* bsdf = nullptr;
	for (auto& kv : graph["nodes"].o) if (kv.second["type"].str() == "BSDF_PRINCIPLED") { bsdf = &kv.second; break; }
	if (!bsdf) return false;
	std::string af, as;
	if (!g.link(*bsdf, "Alpha", af, as)) return false;
	struct Term { std::string img; int chan; float scale[2], speed[2]; int op; };
	std::vector<Term> terms;
	float mul = 1;
	// a leaf: Separate Color channel of a scrolling image (or a vertex colour, taken as 1)
	auto leaf = [&](const std::string& n, const std::string& sock, Term& t) -> int {   // 1 term, 0 constant one, -1 no
		const Json* nd = g.node(n);
		if (!nd) return -1;
		std::string ty = (*nd)["type"].str();
		if (ty == "SEPARATE_COLOR") {
			std::string f, s2;
			if (!g.link(*nd, "Color", f, s2)) return -1;
			const Json* src = g.node(f);
			if (!src) return -1;
			if ((*src)["type"].str() == "VERTEX_COLOR") return 0;
			if ((*src)["type"].str() != "TEX_IMAGE") return -1;
			t.img = (*src)["props"]["image"].str();
			t.chan = sock == "Red" ? 0 : sock == "Green" ? 1 : sock == "Blue" ? 2 : 3;
			std::string mf, ms;
			if (!g.link(*src, "Vector", mf, ms)) { t.scale[0] = t.scale[1] = 1; t.speed[0] = t.speed[1] = 0; return 1; }
			const Json* map = g.node(mf);
			if (!map || (*map)["type"].str() != "MAPPING") return -1;
			t.scale[0] = g.value(*map, "Scale", 0, 1); t.scale[1] = g.value(*map, "Scale", 1, 1);
			t.speed[0] = t.speed[1] = 0;
			std::string lf, ls;
			if (g.link(*map, "Location", lf, ls)) {
				const Json* vm = g.node(lf);
				if (!vm || (*vm)["type"].str() != "VECT_MATH") return -1;
				t.speed[0] = g.value(*vm, "Vector", 0); t.speed[1] = g.value(*vm, "Vector", 1);
			}
			else { t.speed[0] = 0; t.speed[1] = 0; }
			return 1;
		}
		if (ty == "VERTEX_COLOR") return 0;
		return -1;
	};
	// left-deep expression of MATH ADD / MULTIPLY
	std::function<bool(const std::string&, const std::string&)> expr = [&](const std::string& n, const std::string& sock) -> bool {
		Term t = {};
		int lf = leaf(n, sock, t);
		if (lf == 1) { t.op = terms.empty() ? 0 : 1; terms.push_back(t); return true; }
		const Json* nd = g.node(n);
		if (!nd || (*nd)["type"].str() != "MATH") return false;
		std::string op = (*nd)["props"]["operation"].str();
		if (op != "ADD" && op != "MULTIPLY") return false;
		std::string f0, s0, f1, s1;
		bool l0 = g.link(*nd, "Value", f0, s0), l1 = g.link(*nd, "Value_001", f1, s1);
		if (!l0) return false;
		if (!expr(f0, s0)) return false;
		if (!l1) { float v = g.value(*nd, "Value_001"); if (op == "MULTIPLY") mul *= v; else return false; return true; }
		Term r = {};
		int rl = leaf(f1, s1, r);
		if (rl == 0) return true;            // * vertex colour (1)
		if (rl != 1) return false;
		r.op = op == "ADD" ? 1 : 2;
		terms.push_back(r);
		return true;
	};
	if (!expr(af, as) || terms.empty() || terms.size() > 8) return false;
	if (dry) return true;
	KwSplatHeader h = {};
	h.kind = 1; h.numLayers = (uint32_t)terms.size(); h.tintTex = -1; h.tintMul = mul; h.invUnits = 1.0f / UNITS;
	std::vector<KwFlowLayer> L;
	for (auto& t : terms) {
		KwFlowLayer f = {};
		f.tex = SplatTexture(t.img, TK_DATA);
		f.chan = (uint32_t)t.chan; f.scale[0] = t.scale[0]; f.scale[1] = t.scale[1]; f.speed[0] = t.speed[0]; f.speed[1] = t.speed[1];
		f.op = (uint32_t)t.op;
		L.push_back(f);
	}
	const Json& bc = m["baseColor"];
	k.baseColor[0] = 0.85f; k.baseColor[1] = 0.92f; k.baseColor[2] = 0.95f; k.baseColor[3] = 1;
	if (bc.t == Json::ARR) for (int c = 0; c < 3; c++) k.baseColor[c] = (float)bc[(size_t)c].num(k.baseColor[c]);
	const Json& em = m["emission"];
	if (em.t == Json::ARR) for (int c = 0; c < 3; c++) k.emissive[c] = (float)em[(size_t)c].num(0);
	k.roughness = (float)m["roughness"].num(0.15);
	k.flags = (k.flags & ~(uint32_t)(KWM_MASK | KWM_BLEND)) | KWM_FLOW | KWM_TWOSIDED;
	std::lock_guard<std::mutex> lk(splatMutex);
	k.albedoTex = (int32_t)splatBlob.size();
	const uint8_t* hp = (const uint8_t*)&h;
	splatBlob.insert(splatBlob.end(), hp, hp + sizeof(h));
	const uint8_t* lp = (const uint8_t*)L.data();
	splatBlob.insert(splatBlob.end(), lp, lp + L.size() * sizeof(KwFlowLayer));
	warnings.push_back("flowing water (animated in the FX pass)");
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
	if (mi.flags & KWM_WATER) { WaterParams(m, k); return; }
	if ((mi.flags & KWM_FLOW) && TryFlow(m, k, warnings)) return;
	if (out && TrySplat(m, k, warnings)) return;
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
		if (mi.planar) { ctx.posScale[0] = mi.period[0]; ctx.posScale[1] = mi.period[1]; }
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
	if (alphaVaries || alpha0 < 0.99f) {
		if (!(k.flags & KWM_MASK)) k.flags |= KWM_MASK | KWM_TWOSIDED;
	}
	if (!oBase.varies) { for (int c = 0; c < 4; c++) k.baseColor[c] = oBase.c[c]; }
	else {
		k.baseColor[0] = k.baseColor[1] = k.baseColor[2] = k.baseColor[3] = 1;
		k.albedoTex = AddTexture(image(oBase), TK_COLOR, false, (k.flags & KWM_MASK) && !(k.flags & KWM_BLEND));
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
		KwMaterial dk; std::vector<std::string> dw;
		if ((m.flags & (KWM_WATER | KWM_FLOW)) || (m.json && TrySplat(*m.json, dk, dw, true))) { m.srcRes = 0; m.area = 0; continue; }   // shader-evaluated
		m.srcRes = 0;
		for (auto& f : m.images) { auto d = SourceDims(f); m.srcRes = std::max(m.srcRes, std::max(d.first, d.second)); }
		m.srcRes = std::min(m.srcRes, MAXTEX);
		if (m.planar) {
			int r = 64;
			while (r * 2 <= MAXTEX && r * 2 <= m.srcRes * m.repeats) r *= 2;
			m.srcRes = r;
			m.area = 1e12;   // always the full tile resolution
		}
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
	// alpha-tested foliage: cards are separate quads, and pruning / permissive collapses would delete whole leaves
	// and branches (trees go bald with distance); simplify them only where the cards stay intact
	const bool cards = b.cls == KWC_MASKED;
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
						cards ? (unsigned)meshopt_SimplifyLockBorder : (meshopt_SimplifyLockBorder | meshopt_SimplifyPermissive | (L >= 3 ? meshopt_SimplifyPrune : 0)), &e);
				dst.resize(n);
				meshopt_optimizeVertexCache(dst.data(), dst.data(), dst.size(), b.v.size());
			}
			else dst = src;
			maxErr = std::max(maxErr, e * scale);
			total += dst.size();
			cur2[gi] = std::move(dst);
		}
		if (getenv("KT_LODLOG")) printf("    lod cls %d L%d %zu tris err %.2f\n", b.cls, L, total / 3, maxErr);
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
	for (auto& s : SKIP) {   // "^Name" = the name starts with it
		if (s.size() > 1 && s[0] == '^') { if (name.compare(0, s.size() - 1, s, 1, std::string::npos) == 0) return true; }
		else if (!s.empty() && name.find(s) != std::string::npos) return true;
	}
	return false;
}

static void Xform(const float* m, const float* p, float* o)   // 3-row matrix
{
	for (int r = 0; r < 3; r++) o[r] = m[r * 4] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2] + m[r * 4 + 3];
}

// ================================================================================================ main
// ================================================================================================ collision
// The BSP is collision only: clusters near the playable area become thin detail brushes (one per triangle), at a
// LOD whose error grows with the distance from the core, coarsened until the whole set fits the budget.
// vAt[ci]: file offset of the cluster's vertices; iBase[ci] + firstIndex * 4: file offset of a LOD's indices
// (both in g_spill, which is the spill file during a cook and the .kworld itself with --collision-only).
static std::string g_skyName = "katarn/sky";
static void WriteCollision(const std::vector<KwCluster>& clusters, const std::vector<KwLod>& lods, const std::vector<uint64_t>& vAt,
	const std::vector<uint64_t>& iBase, const float* cmin, const float* cmax, const Json& scene, const std::string& outDir, const std::string& mapName)
{
	// ---- collision .map: opaque clusters inside the playable core, at the LOD that fits the budget
	// the collision region: the playable core plus a wide margin (surrounding hills, docks, the valley below)
	const float T = 64.0f;
	float lo[3] = { floorf(cmin[0] - COLL_PAD), floorf(cmin[1] - COLL_PAD), floorf(cmin[2] - COLL_PAD) };
	float hi[3] = { ceilf(cmax[0] + COLL_PAD), ceilf(cmax[1] + COLL_PAD), ceilf(cmax[2] + 4096.0f) };
	auto inCore = [&](const float* p) { return p[0] > lo[0] && p[0] < hi[0] && p[1] > lo[1] && p[1] < hi[1] && p[2] > lo[2] && p[2] < hi[2]; };
	// materials you walk through: leaves, flowers, vines, cut-out cards, water (same order as the cook numbers them)
	std::vector<bool> passThrough;
	for (auto& kv : scene["materials"].o) {
		uint32_t f = MaterialFlags(kv.second);
		passThrough.push_back((f & (KWM_FOLIAGE | KWM_MASK)) != 0 || IsWaterMaterial(kv.first, kv.second));
	}
	size_t passSkipped = 0;
	// detail falls off with the distance from the core: full detail where you play, coarser further out
	std::vector<float> errScale(clusters.size(), 1.0f);
	std::vector<bool> nearCore(clusters.size(), false);
	std::vector<int> collLod(clusters.size(), -1);
	for (size_t ci = 0; ci < clusters.size(); ci++) {
		const KwCluster& c = clusters[ci];
		if (c.cls == KWC_MASKED || c.cls == KWC_FX) continue;
		if (!(c.boundsMin[2] < hi[2] && c.boundsMax[2] > lo[2] && c.boundsMax[0] > lo[0] && c.boundsMin[0] < hi[0] && c.boundsMax[1] > lo[1] && c.boundsMin[1] < hi[1])) continue;
		float d2 = 0;
		for (int q = 0; q < 3; q++) {
			float g = std::max(0.0f, std::max(cmin[q] - c.boundsMax[q], c.boundsMin[q] - cmax[q]));
			d2 += g * g;
		}
		float d = sqrtf(d2);
		nearCore[ci] = d <= 0.0f;
		errScale[ci] = 1.0f + d / 1024.0f;
		int pick = 0;
		for (uint32_t l = 0; l < c.numLods; l++) { pick = (int)l; if (lods[c.firstLod + l].error >= COLL_ERROR * errScale[ci]) break; }
		collLod[ci] = pick;
	}
	// collision triangles per cluster, split in two: walkable (floors, streets, stair treads: kept accurate, the player
	// stands on them) and everything else (walls, carvings, props: much coarser, they only have to block).
	// Then everything is coarsened until the whole set fits the budget, the surroundings first.
	const size_t NS = clusters.size() * 2;
	std::vector<std::vector<float>> collPos(NS);
	std::vector<std::vector<uint32_t>> collTri(NS);
	std::vector<float> slotErr(NS, COLL_ERROR);
	std::vector<bool> slotNear(NS, false);
	size_t collTotal = 0;
	auto simplifySlot = [&](size_t k, float err, bool prune) {
		std::vector<uint32_t>& I = collTri[k];
		if (I.size() < 3) return;
		float sc = meshopt_simplifyScale(collPos[k].data(), collPos[k].size() / 3, 12);
		std::vector<uint32_t> d(I.size());
		size_t n = meshopt_simplify(d.data(), I.data(), I.size(), collPos[k].data(), collPos[k].size() / 3, 12, 0, err / std::max(sc, 1.0f),
			prune ? (unsigned)meshopt_SimplifyPrune : 0u, nullptr);
		d.resize(n);
		I.swap(d);
	};
	for (size_t ci = 0; ci < clusters.size(); ci++) {
		if (collLod[ci] < 0) continue;
		const KwCluster& c = clusters[ci];
		const KwLod& l = lods[c.firstLod + collLod[ci]];
		std::vector<float> P((size_t)c.numVertices * 3);
		std::vector<KwVertex> BV(c.numVertices);
		std::vector<uint32_t> BI(l.indexCount);
		SpillRead(vAt[ci], BV.data(), BV.size() * sizeof(KwVertex));
		SpillRead(iBase[ci] + (uint64_t)l.firstIndex * 4, BI.data(), BI.size() * 4);
		for (uint32_t k = 0; k < c.numVertices; k++) memcpy(&P[k * 3], BV[k].pos, 12);
		for (uint32_t t2 = 0; t2 + 2 < l.indexCount; t2 += 3) {
			uint16_t tm = BV[BI[t2]].material;
			if (tm < passThrough.size() && passThrough[tm]) { passSkipped++; continue; }
			const float* a3 = &P[BI[t2] * 3], * b3 = &P[BI[t2 + 1] * 3], * c3 = &P[BI[t2 + 2] * 3];
			float ctr[3] = { (a3[0] + b3[0] + c3[0]) / 3, (a3[1] + b3[1] + c3[1]) / 3, (a3[2] + b3[2] + c3[2]) / 3 };
			if (!inCore(ctr)) continue;
			float e1[3] = { b3[0] - a3[0], b3[1] - a3[1], b3[2] - a3[2] }, e2[3] = { c3[0] - a3[0], c3[1] - a3[1], c3[2] - a3[2] };
			float nx = e1[1] * e2[2] - e1[2] * e2[1], ny = e1[2] * e2[0] - e1[0] * e2[2], nz = e1[0] * e2[1] - e1[1] * e2[0];
			float nl = sqrtf(nx * nx + ny * ny + nz * nz);
			int kind = (nl > 0 && nz / nl > 0.7f) ? 0 : 1;
			std::vector<uint32_t>& I = collTri[ci * 2 + kind];
			I.push_back(BI[t2]); I.push_back(BI[t2 + 1]); I.push_back(BI[t2 + 2]);
		}
		for (int kind = 0; kind < 2; kind++) {
			size_t k = ci * 2 + kind;
			std::vector<uint32_t>& I = collTri[k];
			slotNear[k] = nearCore[ci];
			slotErr[k] = COLL_ERROR * errScale[ci] * (kind ? COLL_WALL_MUL : 1.0f);
			if (I.empty()) continue;
			// weld by position (UV / material seams would stop the simplifier)
			std::vector<unsigned int> remap(c.numVertices);
			size_t nu = meshopt_generateVertexRemap(remap.data(), I.data(), I.size(), P.data(), c.numVertices, 12);
			collPos[k].resize(nu * 3);
			meshopt_remapVertexBuffer(collPos[k].data(), P.data(), c.numVertices, 12, remap.data());
			meshopt_remapIndexBuffer(I.data(), I.data(), I.size(), remap.data());
			// small loose pieces (pots, debris) only block nothing worth a brush outside the core; walls everywhere
			simplifySlot(k, slotErr[k], kind == 1 || !nearCore[ci]);
			collTotal += I.size() / 3;
		}
	}
	{
		size_t fl = 0, flNear = 0, wl = 0, wlNear = 0;
		for (size_t k = 0; k < NS; k++) { size_t n = collTri[k].size() / 3; if (k & 1) { wl += n; if (slotNear[k]) wlNear += n; } else { fl += n; if (slotNear[k]) flNear += n; } }
		printf("  collision: %zu triangles: floors %zu (%zu in the core) at %.0f units, walls %zu (%zu in the core) at %.0f units (%zu foliage / water triangles left out)\n",
			collTotal, fl, flNear, COLL_ERROR, wl, wlNear, COLL_ERROR * COLL_WALL_MUL, passSkipped);
	}
	// phases: the surroundings, then walls in the core, then floors in the core too
	// (walls are only for blocking: they go to 16x before the floors you walk on lose any detail)
	static const char* phaseName[3] = { "surroundings", "core walls", "everything" };
	for (int phase = 0; phase < 3 && collTotal > COLL_BUDGET; phase++) {
		for (float m = 2; collTotal > COLL_BUDGET && m <= (phase == 1 ? 16.0f : 256.0f); m *= 2) {
			collTotal = 0;
			for (size_t k = 0; k < NS; k++) {
				bool take = phase == 0 ? !slotNear[k] : phase == 1 ? (!slotNear[k] || (k & 1)) : true;
				if (take) simplifySlot(k, slotErr[k] * (phase == 2 && (k & 1) ? std::max(m, 16.0f) : m), !slotNear[k] || (k & 1));
				collTotal += collTri[k].size() / 3;
			}
			printf("  collision: %zu triangles (%s x%.0f)\n", collTotal, phaseName[phase], m);
		}
	}
	if (collTotal > COLL_BUDGET) {   // still too many: sloppy (vertex clustering) simplification to the budget
		double keep = (double)COLL_BUDGET / collTotal;
		collTotal = 0;
		for (size_t k = 0; k < NS; k++) {
			std::vector<uint32_t>& I = collTri[k];
			if (I.size() < 3) continue;
			std::vector<uint32_t> d(I.size());
			size_t n = meshopt_simplifySloppy(d.data(), I.data(), I.size(), collPos[k].data(), collPos[k].size() / 3, 12, (size_t)(I.size() / 3 * keep) * 3, 1.0f, nullptr);
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
	const char* sky = g_skyName.c_str();
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
	for (size_t ci = 0; ci < collTri.size(); ci++) {
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
}

// --collision-only: the clusters come from the .kworld already cooked, so only the .map is rebuilt (minutes, not hours)
static int CollisionOnly(const Json& scene, const std::string& outDir, const std::string& mapName, double t0)
{
	std::string kpath = outDir + "/" + mapName + ".kworld";
	FILE* kf = fopen(kpath.c_str(), "rb");
	if (!kf) { printf("cannot open %s (cook the world first)\n", kpath.c_str()); return 1; }
	KwHeader h = {};
	if (fread(&h, sizeof(h), 1, kf) != 1 || memcmp(h.magic, KW_MAGIC, 8)) { printf("%s is not a KWORLD02 file\n", kpath.c_str()); return 1; }
	std::vector<KwSection> secs(h.numSections);
	fread(secs.data(), sizeof(KwSection), secs.size(), kf);
	auto find = [&](uint32_t t) -> const KwSection* { for (auto& x : secs) if (x.type == t) return &x; return nullptr; };
	const KwSection* sv = find(KW_VERT), * si = find(KW_INDX), * sc = find(KW_CLUS), * sl = find(KW_LODS), * se = find(KW_ENVT);
	if (!sv || !si || !sc || !sl) { printf("%s: sections missing\n", kpath.c_str()); return 1; }
	std::vector<KwCluster> clusters(sc->size / sizeof(KwCluster));
	std::vector<KwLod> lods(sl->size / sizeof(KwLod));
	kt_fseek(kf, (int64_t)sc->offset, SEEK_SET); fread(clusters.data(), sizeof(KwCluster), clusters.size(), kf);
	kt_fseek(kf, (int64_t)sl->offset, SEEK_SET); fread(lods.data(), sizeof(KwLod), lods.size(), kf);
	std::vector<uint64_t> vAt(clusters.size()), iBase(clusters.size(), si->offset);
	for (size_t ci = 0; ci < clusters.size(); ci++) vAt[ci] = sv->offset + (uint64_t)clusters[ci].baseVertex * sizeof(KwVertex);
	// the playable core: stored by newer cooks, else worked out from the export's object bounds
	float cmin[3] = { 1e30f, 1e30f, 1e30f }, cmax[3] = { -1e30f, -1e30f, -1e30f };
	bool haveCore = false;
	if (se && se->size) {
		std::string env(se->size, '\0');
		kt_fseek(kf, (int64_t)se->offset, SEEK_SET); fread(&env[0], 1, env.size(), kf);
		const char* a = strstr(env.c_str(), "coreMin "), * b = strstr(env.c_str(), "coreMax ");
		if (a && b && sscanf(a, "coreMin %f %f %f", &cmin[0], &cmin[1], &cmin[2]) == 3 && sscanf(b, "coreMax %f %f %f", &cmax[0], &cmax[1], &cmax[2]) == 3) haveCore = true;
	}
	if (!haveCore) {
		const Json& meshes = scene["meshes"];
		const Json& insts = scene["instances"];
		std::vector<std::array<float, 4>> samples;
		for (size_t ii = 0; ii < insts.size(); ii++) {
			const Json& in = insts[ii];
			if (Skip(in["object"].str())) continue;
			size_t mi = (size_t)in["mesh"].num();
			if (mi >= meshes.size()) continue;
			const Json& m = meshes[mi];
			const Json& bd = m["bounds"];
			if (bd.t != Json::ARR || bd.size() < 2) continue;
			float M[12];
			const Json& J = in["matrix"];
			for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) M[r * 4 + c] = (float)J[(size_t)(r * 4 + c)].num() * UNITS;
			float omin[3] = { 1e30f, 1e30f, 1e30f }, omax[3] = { -1e30f, -1e30f, -1e30f };
			for (int k = 0; k < 8; k++) {
				float p[3] = { (float)bd[(size_t)(k & 1)][(size_t)0].num(), (float)bd[(size_t)((k >> 1) & 1)][(size_t)1].num(), (float)bd[(size_t)((k >> 2) & 1)][(size_t)2].num() }, w[3];
				Xform(M, p, w);
				for (int q = 0; q < 3; q++) { omin[q] = std::min(omin[q], w[q]); omax[q] = std::max(omax[q], w[q]); }
			}
			float ext = std::max(omax[0] - omin[0], std::max(omax[1] - omin[1], omax[2] - omin[2]));
			if (ext < 100 * UNITS) samples.push_back({ (omin[0] + omax[0]) * 0.5f, (omin[1] + omax[1]) * 0.5f, (omin[2] + omax[2]) * 0.5f, (float)(m["indexCount"].num() / 3) });
		}
		double total = 0;
		for (auto& x : samples) total += x[3];
		for (int q = 0; q < 3 && !samples.empty(); q++) {
			std::sort(samples.begin(), samples.end(), [q](const std::array<float, 4>& a, const std::array<float, 4>& b) { return a[q] < b[q]; });
			double acc = 0; bool lo = false;
			for (auto& x : samples) {
				acc += x[3];
				if (!lo && acc >= total * 0.02) { cmin[q] = x[q]; lo = true; }
				if (acc >= total * 0.98) { cmax[q] = x[q]; break; }
			}
		}
		if (cmin[0] > cmax[0]) { memcpy(cmin, h.boundsMin, 12); memcpy(cmax, h.boundsMax, 12); }
	}
	printf("  collision only, from %s: %zu clusters, playable core %.0f x %.0f x %.0f units\n", kpath.c_str(), clusters.size(),
		cmax[0] - cmin[0], cmax[1] - cmin[1], cmax[2] - cmin[2]);
	fclose(g_spill);
	remove((outDir + "/" + mapName + ".kworld.tmp").c_str());
	g_spill = kf;
	WriteCollision(clusters, lods, vAt, iBase, cmin, cmax, scene, outDir, mapName);
	printf("done in %.0fs\n", Now() - t0);
	fflush(stdout);
	_exit(0);   // (the watchdog thread is still running)
}


// ================================================================================================ kworld v3 export
std::vector<std::string> g_v3TexNames, g_v3MatNames;
static bool FORMAT_V3 = true;
static void MakeDirs(const std::string& p)
{
	std::string cur;
	for (size_t i = 0; i <= p.size(); i++) {
		if (i == p.size() || p[i] == '/' || p[i] == '\\') {
			if (!cur.empty() && cur != "." && cur != ".." && !(cur.size() == 2 && cur[1] == ':')) {
#ifdef _WIN32
				_mkdir(cur.c_str());
#else
				mkdir(cur.c_str(), 0755);
#endif
			}
		}
		if (i < p.size()) cur += p[i];
	}
}
#include "bake_light.h"
static float g_sunDirG[3] = { 0.35f, 0.25f, 0.9f }, g_sunColG[3] = { 1, 1, 1 };

// Textures leave the .kworld: each one is a DDS file (BC7 / BC5 with its mips) at textures/kw_<map>/<name>.dds, found
// through the game filesystem (pk3s, overrides, shared between maps). Materials become a normal .shader file
// (shaders/kw_<map>.shader) with rend2's stage keywords, so any renderer that parses shaders can draw the world.
static std::string SafeName(std::string n)
{
	for (char& c : n) { c = (char)tolower((unsigned char)c); if (!isalnum((unsigned char)c) && c != '_') c = '_'; }
	while (n.find("__") != std::string::npos) n.replace(n.find("__"), 2, "_");
	if (n.size() > 30) { uint32_t h = 2166136261u; for (char c : n) { h ^= (uint8_t)c; h *= 16777619u; } char b[16]; snprintf(b, sizeof(b), "_%06x", h & 0xffffff); n = n.substr(0, 23) + b; }
	return n.empty() ? std::string("m") : n;
}
static void WriteDDS(const std::string& path, const TexOut& t)
{
	FILE* f = fopen(path.c_str(), "wb");
	if (!f) { printf("  ! cannot write %s\n", path.c_str()); return; }
	uint32_t hdr[32] = {};
	hdr[0] = 0x20534444;                         // "DDS "
	hdr[1] = 124;
	hdr[2] = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000;
	hdr[3] = t.k.height; hdr[4] = t.k.width;
	hdr[5] = (uint32_t)(((t.k.width + 3) / 4) * ((t.k.height + 3) / 4) * 16);
	hdr[7] = t.k.mips;
	hdr[19] = 32; hdr[20] = 0x4; hdr[21] = 0x30315844;   // pixel format: FOURCC "DX10"
	hdr[27] = 0x1000 | 0x400000 | 0x8;
	uint32_t fmt = t.k.format == 98 && t.k.srgb ? 99u : t.k.format;
	uint32_t dx10[5] = { fmt, 3, 0, 1, 0 };
	fwrite(hdr, 4, 32, f); fwrite(dx10, 4, 5, f);
	SpillCopy(t.spillAt, t.bytes, f);
	fclose(f);
}
static std::string SunLine()
{
	// rend2 / q3map2 sun: colour, intensity, yaw and elevation in degrees (where the light comes from)
	float l = sqrtf(g_sunDirG[0] * g_sunDirG[0] + g_sunDirG[1] * g_sunDirG[1] + g_sunDirG[2] * g_sunDirG[2]);
	float yaw = atan2f(g_sunDirG[1], g_sunDirG[0]) * 57.29578f, el = asinf(std::max(-1.0f, std::min(1.0f, g_sunDirG[2] / std::max(1e-6f, l)))) * 57.29578f;
	char b[160];
	snprintf(b, sizeof(b), "\tq3map_sun %.3f %.3f %.3f 100 %.1f %.1f\n", g_sunColG[0], g_sunColG[1], g_sunColG[2], yaw, el);
	return b;
}
static void ExportV3(const std::string& outDir, const std::string& mapName, const std::vector<uint8_t>& splat)
{
	std::string root = outDir + "/..";                // home\MD (maps live in home\MD\maps)
	std::string texDir = "textures/kw_" + mapName;
	MakeDirs(root + "/" + texDir); MakeDirs(root + "/shaders");
	std::vector<std::string> mname(kmats.size());
	std::map<std::string, int> used;
	for (size_t i = 0; i < kmats.size() && i < mats.size(); i++) {
		std::string n = SafeName(mats[i].name.substr(0, mats[i].name.find('|')));
		if (used[n]++) n += "_" + std::to_string(used[n] - 1);
		mname[i] = n;
		const KwMaterial& k = kmats[i];
		auto nameTex = [&](int ti, const char* suf) { if (ti >= 0 && ti < (int)textures.size() && textures[ti].name.empty()) textures[ti].name = n + suf; };
		if (!(k.flags & (KWM_SPLAT | KWM_FLOW))) { nameTex(k.albedoTex, "_d"); nameTex(k.normalTex, "_n"); nameTex(k.ormTex, "_orm"); nameTex(k.emissiveTex, "_e"); }
	}
	// flat-colour materials: a 4x4 texture of the colour (rend2 multiplies the texture by the baked vertex light)
	std::vector<int> flatTex(kmats.size(), -1);
	for (size_t i = 0; i < kmats.size(); i++) {
		const KwMaterial& k = kmats[i];
		if (k.albedoTex >= 0 || (k.flags & (KWM_SPLAT | KWM_FLOW))) continue;
		BakeImage b; b.w = b.h = 4; b.px.resize(64);
		for (int p = 0; p < 16; p++) for (int c = 0; c < 4; c++) b.px[p * 4 + c] = c < 3 ? std::min(1.0f, k.baseColor[c]) : k.baseColor[3];
		flatTex[i] = AddTexture(b, TK_COLOR, false);
		if (textures[flatTex[i]].name.empty()) textures[flatTex[i]].name = mname[i] + "_c";
	}
	for (size_t t = 0; t < textures.size(); t++) {
		if (textures[t].name.empty()) { char b[32]; snprintf(b, sizeof(b), "t%04zu", t); textures[t].name = b; }
		WriteDDS(root + "/" + texDir + "/" + textures[t].name + ".dds", textures[t]);
	}
	auto tp = [&](int ti) { return texDir + "/" + textures[ti].name; };
	std::string sh = "// " + mapName + " - KatarnEngine HD world materials (kworld v3). rend2 stage keywords; edit freely.\n\n";
	for (size_t i = 0; i < kmats.size(); i++) {
		const KwMaterial& k = kmats[i];
		std::string name = texDir + "/" + mname[i];
		char buf[256];
		sh += name + "\n{\n";
		if (k.flags & KWM_TWOSIDED) sh += "\tcull none\n";
		if (k.flags & (KWM_WATER | KWM_FLOW)) sh += "\tsurfaceparm water\n\tsurfaceparm trans\n";
		if (k.flags & KWM_FOLIAGE) sh += "\tsurfaceparm trans\n\tdeformVertexes wave 194 sin 0 0.6 0 0.25\n";
		sh += "\tsurfaceparm nolightmap\n";
		int base = (k.albedoTex >= 0 && !(k.flags & (KWM_SPLAT | KWM_FLOW))) ? k.albedoTex : flatTex[i];
		const KwSplatHeader* sp = nullptr;
		if ((k.flags & KWM_SPLAT) && k.albedoTex >= 0 && (size_t)k.albedoTex + sizeof(KwSplatHeader) + sizeof(KwSplatLayer) <= splat.size())
			sp = (const KwSplatHeader*)&splat[k.albedoTex];
		if (sp && sp->kind == 0 && sp->numLayers > 0) {
			// splat terrain: its base layer, tiled from the world position (tcGen vector), as KatarnEngine does
			const KwSplatLayer* L0 = (const KwSplatLayer*)(sp + 1);
			float sc = L0->scale * sp->invUnits;
			sh += "\tqer_editorimage " + tp(L0->albedoTex) + "\n\t{\n\t\tmap " + tp(L0->albedoTex) + "\n";
			snprintf(buf, sizeof(buf), "\t\ttcGen vector ( %g 0 0 ) ( 0 %g 0 )\n", sc, -sc); sh += buf;
			if (L0->nohTex >= 0) sh += "\t\tnormalMap " + tp(L0->nohTex) + "\n";
		}
		else if (base >= 0) {
			sh += "\tqer_editorimage " + tp(base) + "\n\t{\n\t\tmap " + tp(base) + "\n";
			if (k.flags & KWM_BLEND) sh += "\t\tblendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA\n";
			else if (k.flags & KWM_MASK) sh += "\t\talphaFunc GE128\n\t\tdepthWrite\n";
		}
		else {
			// water / flowing water: a tinted see-through sheet in rend2 (KatarnEngine evaluates the full material)
			sh += "\t{\n\t\tmap $whiteimage\n";
			if (k.flags & (KWM_WATER | KWM_FLOW | KWM_BLEND)) { snprintf(buf, sizeof(buf), "\t\talphaGen const %.2f\n\t\tblendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA\n", (k.flags & KWM_FLOW) ? 0.5f : 0.7f); sh += buf; }
		}
		if (!(k.flags & (KWM_SPLAT | KWM_FLOW | KWM_WATER)) && !sp) {
			if (k.normalTex >= 0) sh += "\t\tnormalMap " + tp(k.normalTex) + "\n";
			if (k.ormTex >= 0) sh += "\t\tormMap " + tp(k.ormTex) + "\n";
			else { snprintf(buf, sizeof(buf), "\t\tspecularScale %.3f %.3f\n", k.metallic, 1.0f - k.roughness); sh += buf; }
		}
		sh += "\t\trgbGen vertex\n\t}\n";   // vertex colour = baked light (kworld v3 vertices)
		if (k.emissiveTex >= 0 || k.emissive[0] + k.emissive[1] + k.emissive[2] > 0.01f) {
			sh += "\t{\n\t\tmap " + (k.emissiveTex >= 0 ? tp(k.emissiveTex) : std::string("$whiteimage")) + "\n\t\tblendFunc GL_ONE GL_ONE\n\t\tglow\n";
			snprintf(buf, sizeof(buf), "\t\trgbGen const ( %.3f %.3f %.3f )\n", std::min(1.0f, k.emissive[0]), std::min(1.0f, k.emissive[1]), std::min(1.0f, k.emissive[2])); sh += buf;
			sh += "\t}\n";
		}
		sh += "}\n\n";
	}
	if (!g_envImg.px.empty()) {
		WriteSkyBox(root + "/" + texDir, "env");
		sh += texDir + "/sky\n{\n\tqer_editorimage " + texDir + "/env_ft\n\tsurfaceparm sky\n\tsurfaceparm noimpact\n\tsurfaceparm nolightmap\n\tsurfaceparm nomarks\n\tnopicmip\n\tnomipmaps\n\tskyparms " + texDir + "/env - -\n" + SunLine() + "}\n\n";
	}
	else sh += texDir + "/sky\n{\n\tsurfaceparm sky\n\tsurfaceparm noimpact\n\tsurfaceparm nolightmap\n\tsurfaceparm nomarks\n\tskyparms - - -\n}\n\n";
	FILE* f = fopen((root + "/shaders/kw_" + mapName + ".shader").c_str(), "wb");
	if (f) { fwrite(sh.data(), 1, sh.size(), f); fclose(f); }
	printf("  v3: %zu textures as DDS in %s, %zu materials in shaders/kw_%s.shader\n", textures.size(), texDir.c_str(), kmats.size(), mapName.c_str());
	// names for the kworld (TNAM / MNAM)
	g_v3TexNames.clear(); g_v3MatNames.clear();
	for (auto& t : textures) g_v3TexNames.push_back(texDir + "/" + t.name);
	for (size_t i = 0; i < kmats.size(); i++) g_v3MatNames.push_back(texDir + "/" + mname[i]);
}

int main(int argc, char** argv)
{
	if (argc < 4) {
		printf("usage: katarn_cook <export folder> <output folder> <map name> [--units 40] [--maxtex 2048] [--texbudget 2500]\n"
			"       [--quality 0-2] [--cell 2048] [--lod0-error 0.25] [--collision-error 8] [--collision-budget 250000] [--collision-walls 4] [--collision-pad 8192] [--collision-only]\n"
			"       [--skip \"HeightFog,Sky_Cottonball\"] [--threads N] [--maxmem MB] [--bands N]\n");
		return 1;
	}
	double t0 = Now();
	exportDir = argv[1];
	std::string outDir = argv[2], mapName = argv[3];
	for (int i = 4; i < argc; i++) if (!strcmp(argv[i], "--collision-only")) { COLL_ONLY = true; for (int j = i; j + 1 < argc; j++) argv[j] = argv[j + 1]; argc--; break; }
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
		else if (a == "--collision-walls") COLL_WALL_MUL = (float)atof(v.c_str());
		else if (a == "--collision-pad") COLL_PAD = (float)atof(v.c_str());
		else if (a == "--collision-budget") COLL_BUDGET = (size_t)atoll(v.c_str());
		else if (a == "--skipmat") { SKIPMAT.clear(); size_t s = 0; while (s <= v.size()) { size_t e = v.find(',', s); if (e == std::string::npos) e = v.size(); SKIPMAT.push_back(v.substr(s, e - s)); s = e + 1; } }
		else if (a == "--thin") {   // pattern=fraction,pattern=fraction
			THIN.clear(); size_t s = 0;
			while (s < v.size()) {
				size_t e = v.find(',', s); if (e == std::string::npos) e = v.size();
				std::string t = v.substr(s, e - s); size_t q = t.find('=');
				if (q != std::string::npos) THIN.push_back({ t.substr(0, q), (float)atof(t.c_str() + q + 1) });
				s = e + 1;
			}
		}
		else if (a == "--format") FORMAT_V3 = atoi(v.c_str()) >= 3;
		else if (a == "--bake") BAKE_OFF = atoi(v.c_str()) == 0;
		else if (a == "--bake-rays") BAKE_SKY_RAYS = std::max(0, atoi(v.c_str()));
		else if (a == "--skiplight") { SKIPLIGHT.clear(); size_t s = 0; while (s <= v.size()) { size_t e = v.find(',', s); if (e == std::string::npos) e = v.size(); SKIPLIGHT.push_back(v.substr(s, e - s)); s = e + 1; } }
		else if (a == "--skip") { SKIP.clear(); size_t s = 0; while (s <= v.size()) { size_t e = v.find(',', s); if (e == std::string::npos) e = v.size(); SKIP.push_back(v.substr(s, e - s)); s = e + 1; } }
	}
	if (FORMAT_V3) g_skyName = "kw_" + mapName + "/sky";
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
	g_groups = groups;
	if (COLL_ONLY) return CollisionOnly(scene, outDir, mapName, t0);

	// ---- materials (flags now, baked after we know how much surface each covers)
	const Json& jm = scene["materials"];
	for (auto& kv : jm.o) {
		MatInfo mi;
		mi.name = kv.first; mi.json = &kv.second; mi.flags = MaterialFlags(kv.second);
		if (kv.second["graph"].t == Json::OBJ) {
			CollectImages(kv.second["graph"]["nodes"], groups, mi.images);
			mi.invisible = OutputShaderNode(kv.second["graph"]["nodes"]) == nullptr;
			// particle FX cards: colour / alpha / flipbook frame come from the instancer (geometry nodes attributes)
			for (auto& nd : kv.second["graph"]["nodes"].o)
				if (nd.second["type"].str() == "ATTRIBUTE" && nd.second["props"]["attribute_type"].str() == "INSTANCER") { mi.invisible = true; mi.fxCard = true; break; }
		}
		for (auto& s : SKIPMAT) if (!s.empty() && mi.name.find(s) != std::string::npos) { mi.invisible = true; mi.skipped = true; }
		std::sort(mi.images.begin(), mi.images.end());
		mi.images.erase(std::unique(mi.images.begin(), mi.images.end()), mi.images.end());
		mi.planar = DetectPlanar(kv.second, mi.period, mi.repeats);
		if (!mi.invisible && IsWaterMaterial(mi.name, kv.second)) { mi.flags |= KWM_WATER; mi.planar = false; }
		else { KwMaterial dk; std::vector<std::string> dw; if (TryFlow(kv.second, dk, dw, true)) mi.flags |= KWM_FLOW; }
		matIndex[mi.name] = (uint32_t)mats.size();
		mats.push_back(mi);
	}
	if (mats.empty()) { MatInfo d; d.name = "__default"; d.json = &jm; mats.push_back(d); }
	{
		size_t nfx = 0, nsk = 0;
		for (auto& mi : mats) { if (mi.fxCard) nfx++; if (mi.skipped) nsk++; }
		if (nfx || nsk) printf("  left out: %zu particle FX card materials, %zu materials by name\n", nfx, nsk);
		size_t np = 0;
		for (auto& mi : mats) if (mi.planar) np++;
		if (np) printf("  %zu materials projected from world position (tiled, UVs generated)\n", np);
		size_t nw = 0, nf = 0;
		for (auto& mi : mats) { if (mi.flags & KWM_WATER) nw++; if (mi.flags & KWM_FLOW) nf++; }
		if (nw || nf) printf("  water: %zu still water materials, %zu flowing (waterfalls)\n", nw, nf);
	}

	if (getenv("KT_SPLAT_DEBUG")) {
		for (auto& mi : mats) { KwMaterial dk; std::vector<std::string> dw; TrySplat(*mi.json, dk, dw, true); }
		for (auto& mi : mats) if (mi.planar) printf("planar %-50s period %.2f x %.2f m, repeats %.1f\n", mi.name.c_str(), mi.period[0], mi.period[1], mi.repeats);
		fflush(stdout); _exit(0);
	}
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
		{
			// all of it left out, or a thinned scatter (keep a fixed fraction of its instances)
			bool anyVisible = false; float keep = 1.0f;
			for (size_t si = 0; si < m["submeshes"].size(); si++) {
				const std::string& smat = m["submeshes"][si]["material"].str();
				auto it = matIndex.find(smat);
				if (it == matIndex.end() || !mats[it->second].invisible) anyVisible = true;
				for (auto& th : THIN) if (!th.first.empty() && smat.find(th.first) != std::string::npos) keep = std::min(keep, th.second);
			}
			if (!anyVisible) { if (band == 0) skipped++; continue; }
			if (keep < 1.0f) {
				uint32_t hsh = (uint32_t)ii * 2654435761u; hsh ^= hsh >> 15;
				if ((hsh & 0xffff) >= (uint32_t)(keep * 65536.0f)) { if (band == 0) skipped++; continue; }
			}
		}
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
			int cls = (mats[mat].flags & (KWM_WATER | KWM_FLOW)) ? KWC_FX : (mats[mat].flags & KWM_MASK) ? KWC_MASKED : (mats[mat].flags & KWM_TWOSIDED) ? KWC_TWOSIDED : KWC_OPAQUE;
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
				if (mats[mat].planar) {   // world-projected: UV from the world position, one baked tile repeats
					kv.uv[0] = kv.pos[0] / UNITS / mats[mat].period[0];
					kv.uv[1] = 1.0f - kv.pos[1] / UNITS / mats[mat].period[1];
				}
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
		{ std::string ln = l["name"].str(); bool sk = false; for (auto& s : SKIPLIGHT) if (!s.empty() && ln.find(s) != std::string::npos) sk = true; if (sk) continue; }
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
			memcpy(g_sh, sh, sizeof(g_sh)); g_haveSH = true; g_envImg = e; g_envStrength = sc;
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
	memcpy(g_sunDirG, sunDir, 12); memcpy(g_sunColG, sunColor, 12);
	{
		char cb[160];
		snprintf(cb, sizeof(cb), "coreMin %f %f %f\ncoreMax %f %f %f\n", cmin[0], cmin[1], cmin[2], cmax[0], cmax[1], cmax[2]);
		envText += cb;
	}

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
		c.flags = b->cls == KWC_FX ? 1u : 0u;   // water casts no shadow
		uint32_t ib = (uint32_t)iCount;
		for (KwLod l : b->lods) { l.firstIndex += ib; lods.push_back(l); }
		iCount += b->ni;
		vCount += b->nv;
		clusters.push_back(c);
		cbucket.push_back(b);
	}
	std::vector<char> tnam, mnam;
	if (FORMAT_V3) {
		ExportV3(outDir, mapName, splatBlob);
		tnam.resize(g_v3TexNames.size() * 64); mnam.resize(g_v3MatNames.size() * 64);
		for (size_t i = 0; i < g_v3TexNames.size(); i++) strncpy(&tnam[i * 64], g_v3TexNames[i].c_str(), 63);
		for (size_t i = 0; i < g_v3MatNames.size(); i++) strncpy(&mnam[i * 64], g_v3MatNames[i].c_str(), 63);
	}
	BakeOut bake;
	std::vector<uint8_t> lgrd;
	char skyn[64] = {};
	if (FORMAT_V3) {
		snprintf(skyn, sizeof(skyn), "textures/%s", g_skyName.c_str());
		if (!BAKE_OFF) {
			BakeLighting(clusters, lods, cbucket, lights, sunDir, sunColor, std::max(0.5f, sunStrength), cmin, cmax, vCount, bake);
			lgrd.resize(sizeof(KwGridHeader) + bake.points.size() * sizeof(KwGridPoint));
			memcpy(lgrd.data(), &bake.grid, sizeof(KwGridHeader));
			if (!bake.points.empty()) memcpy(lgrd.data() + sizeof(KwGridHeader), bake.points.data(), bake.points.size() * sizeof(KwGridPoint));
		}
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
		{ KW_LITE, lights.data(), lights.size() * sizeof(KwLight) }, { KW_ENVT, envText.data(), envText.size() }, { KW_SPLT, splatBlob.data(), splatBlob.size() }, { KW_TDAT, nullptr, FORMAT_V3 ? 0 : tdatSize },
		{ KW_TNAM, tnam.data(), tnam.size() }, { KW_MNAM, mnam.data(), mnam.size() },
		{ KW_VCOL, bake.vcol.data(), bake.vcol.size() * 4 }, { KW_LGRD, lgrd.data(), lgrd.size() }, { KW_SKYN, skyn, FORMAT_V3 ? sizeof(skyn) : 0 },
	};
	KwHeader h = {};
	memcpy(h.magic, KW_MAGIC, 8);
	h.version = FORMAT_V3 ? 3 : 2;
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
		else if (secs[i].type == KW_TDAT && !FORMAT_V3) {
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

	{
		std::vector<uint64_t> vAt(clusters.size()), iBase(clusters.size());
		for (size_t ci = 0; ci < clusters.size(); ci++) {
			const Bucket* B = cbucket[ci];
			vAt[ci] = B->vAt;
			iBase[ci] = B->iAt - (uint64_t)lods[clusters[ci].firstLod].firstIndex * 4;   // (wraps; only sums are used)
		}
		WriteCollision(clusters, lods, vAt, iBase, cmin, cmax, scene, outDir, mapName);
	}
	printf("done in %.0fs\n", Now() - t0);
	g_cookDone = true;
	watchdog.join();
	fclose(g_spill);
	remove(spillPath.c_str());
	printf("  peak memory %zu MB\n", (size_t)g_peakMB);
	return 0;
}
