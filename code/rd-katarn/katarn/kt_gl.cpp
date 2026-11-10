// KatarnEngine - fixed-function OpenGL 1.x translation for legacy content.
//
// The vanilla front end (menus, HUD, BSP maps, Ghoul2 characters, effects) still issues GL 1.x calls. They are
// recorded here as LegacyDraw entries (state snapshot + vertices in the upload ring) and replayed by kt_render.cpp
// into the 2D screen, a 3D view's HDR scene, or after Katarn's own deferred passes.
#include "kt_render.h"
#include "../qgl.h"
#include <math.h>
#include <string.h>
#include <unordered_map>
#include <vector>

namespace kt {

// ------------------------------------------------------------------------------------------------ textures
struct GLLevel { uint32_t w = 0, h = 0; std::vector<uint8_t> rgba; };
struct GLTexture {
	uint32_t name = 0;
	Texture tex;
	std::vector<GLLevel> levels;     // pending CPU data (level 0..n)
	bool dirty = false;               // levels need a (re)create
	struct Sub { uint32_t x, y, w, h; std::vector<uint8_t> rgba; };
	std::vector<Sub> subs;            // pending sub-image updates
	bool clamp = false, point = false;
	uint32_t w = 0, h = 0;
};
static std::unordered_map<uint32_t, GLTexture*> textures;

GLTexture* R_GLTexture(uint32_t name, bool create)
{
	auto it = textures.find(name);
	if (it != textures.end()) return it->second;
	if (!create) return nullptr;
	GLTexture* t = new GLTexture();
	t->name = name;
	textures[name] = t;
	return t;
}

void R_ResolveTexture(GLTexture* t)
{
	if (!t) return;
	if (t->dirty && !t->levels.empty() && t->levels[0].w) {
		// count the complete mip chain that was uploaded
		uint32_t mips = 1;
		while (mips < t->levels.size() && t->levels[mips].w) mips++;
		const GLLevel& l0 = t->levels[0];
		bool same = t->tex.res && t->tex.width == l0.w && t->tex.height == l0.h && t->tex.mips == mips;
		if (!same) {
			if (t->tex.res) DestroyTexture(t->tex);
			CreateTexture(t->tex, l0.w, l0.h, mips, DXGI_FORMAT_R8G8B8A8_UNORM, TEX_SRGB_VIEW, 1, nullptr, "gl");
		}
		for (uint32_t m = 0; m < mips; m++)
			UploadTexture(t->tex, m, 0, t->levels[m].rgba.data(), t->levels[m].w * 4);
		t->w = l0.w; t->h = l0.h;
		t->levels.clear();
		t->levels.shrink_to_fit();
		t->dirty = false;
	}
	if (!t->subs.empty() && t->tex.res) {
		for (auto& s : t->subs) UploadTexture(t->tex, 0, 0, s.rgba.data(), s.w * 4, s.x, s.y, s.w, s.h);
		t->subs.clear();
	}
}

uint32_t R_TextureSrv(GLTexture* t, bool srgb)
{
	if (!t || !t->tex.res) return kNullSrv;
	return srgb ? t->tex.srvSrgb : t->tex.srv;
}

static void DeleteTexture(uint32_t name)
{
	auto it = textures.find(name);
	if (it == textures.end()) return;
	DestroyTexture(it->second->tex);
	delete it->second;
	textures.erase(it);
}

void GL_ShutdownTextures()
{
	for (auto& kv : textures) { DestroyTexture(kv.second->tex); delete kv.second; }
	textures.clear();
}

// ------------------------------------------------------------------------------------------------ state
struct Mat { float m[16]; };
static void MatIdentity(float* m) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1; }
static void MatMul(float* out, const float* a, const float* b)   // out = a * b (column-major)
{
	float r[16];
	for (int c = 0; c < 4; c++)
		for (int rr = 0; rr < 4; rr++)
			r[c * 4 + rr] = a[0 * 4 + rr] * b[c * 4 + 0] + a[1 * 4 + rr] * b[c * 4 + 1] + a[2 * 4 + rr] * b[c * 4 + 2] + a[3 * 4 + rr] * b[c * 4 + 3];
	memcpy(out, r, 64);
}

struct ClientArray { bool on = false; int size = 0; GLenum type = 0; int stride = 0; const void* ptr = nullptr; };
struct IVert { float pos[3]; float uv0[2]; float uv1[2]; uint32_t color; };
static_assert(sizeof(IVert) == 32, "vertex layout");

static struct {
	GLenum matrixMode = GL_MODELVIEW;
	std::vector<Mat> stack[2];         // 0 modelview, 1 projection
	int vp[4] = {}, sc[4] = {};
	bool scissor = false;
	float depthNear = 0, depthFar = 1;
	float color[4] = { 1, 1, 1, 1 };
	float tc[2][2] = {};
	ClientArray vertex, colorArr, texcoord[2];
	int activeUnit = 0, clientUnit = 0;
	uint32_t bound[2] = {};
	bool texEnabled[2] = { true, false };
	GLenum env[2] = { GL_MODULATE, GL_MODULATE };
	bool blend = false; GLenum src = GL_ONE, dst = GL_ZERO;
	bool depthTest = false, depthWrite = true; GLenum depthFunc = GL_LEQUAL;
	bool alphaTest = false; GLenum alphaFunc = GL_ALWAYS; float alphaRef = 0;
	bool cull = false; GLenum cullFace = GL_BACK;
	bool polyOffset = false;
	bool colorMask[4] = { true, true, true, true };
	bool stencilTest = false;
	bool wire = false;
	float clearColor[4] = { 0, 0, 0, 1 };
	float clearDepth = 1;
	// immediate mode
	bool inBegin = false; GLenum prim = 0;
	std::vector<IVert> imm;
	const void* stage = nullptr;
	bool glow = false;
} s;

static inline void CurMat(float* out)
{
	auto& st = s.stack[s.matrixMode == GL_PROJECTION ? 1 : 0];
	memcpy(out, st.back().m, 64);
}
static inline float* Top() { return s.stack[s.matrixMode == GL_PROJECTION ? 1 : 0].back().m; }

void GL_ResetState()
{
	s.stack[0].assign(1, Mat()); MatIdentity(s.stack[0][0].m);
	s.stack[1].assign(1, Mat()); MatIdentity(s.stack[1][0].m);
	s.matrixMode = GL_MODELVIEW;
}
void GL_SetStage(const void* stage, bool glow) { s.stage = stage; s.glow = glow; }

// PSO bits (kt_render.cpp decodes these)
enum : uint32_t {
	PSO_SRC_SHIFT = 0, PSO_DST_SHIFT = 4, PSO_BLEND = 1u << 8,
	PSO_DEPTHTEST = 1u << 9, PSO_DEPTHWRITE = 1u << 10, PSO_DFUNC_SHIFT = 11,
	PSO_CULL_SHIFT = 14, PSO_OFFSET = 1u << 16, PSO_MASK_SHIFT = 17, PSO_LINES = 1u << 21, PSO_WIRE = 1u << 22,
};
static uint32_t BlendCode(GLenum b)
{
	switch (b) {
	case GL_ZERO: return 0; case GL_ONE: return 1; case GL_SRC_COLOR: return 2; case GL_ONE_MINUS_SRC_COLOR: return 3;
	case GL_SRC_ALPHA: return 4; case GL_ONE_MINUS_SRC_ALPHA: return 5; case GL_DST_ALPHA: return 6; case GL_ONE_MINUS_DST_ALPHA: return 7;
	case GL_DST_COLOR: return 8; case GL_ONE_MINUS_DST_COLOR: return 9; case GL_SRC_ALPHA_SATURATE: return 10;
	default: return 1;
	}
}
static uint32_t DepthCode(GLenum f)   // 0 never 1 less 2 equal 3 lequal 4 greater 5 notequal 6 gequal 7 always
{
	switch (f) {
	case GL_NEVER: return 0; case GL_LESS: return 1; case GL_EQUAL: return 2; case GL_LEQUAL: return 3;
	case GL_GREATER: return 4; case GL_NOTEQUAL: return 5; case GL_GEQUAL: return 6; default: return 7;
	}
}

static void Snapshot(LegacyDraw& d)
{
	memset(&d, 0, sizeof(d));
	bool rev = R_ReversedZ() && R_InView();
	uint32_t p = 0;
	if (s.blend) p |= PSO_BLEND | (BlendCode(s.src) << PSO_SRC_SHIFT) | (BlendCode(s.dst) << PSO_DST_SHIFT);
	if (s.depthTest) {
		uint32_t f = DepthCode(s.depthFunc);
		if (rev) { static const uint32_t flip[8] = { 0, 4, 2, 6, 1, 5, 3, 7 }; f = flip[f]; }
		p |= PSO_DEPTHTEST | (f << PSO_DFUNC_SHIFT);
	}
	if (s.depthWrite && s.depthTest) p |= PSO_DEPTHWRITE;
	if (s.cull) p |= (s.cullFace == GL_FRONT ? 1u : s.cullFace == GL_BACK ? 2u : 3u) << PSO_CULL_SHIFT;
	if (s.polyOffset) p |= PSO_OFFSET;
	p |= ((s.colorMask[0] ? 1u : 0) | (s.colorMask[1] ? 2u : 0) | (s.colorMask[2] ? 4u : 0) | (s.colorMask[3] ? 8u : 0)) << PSO_MASK_SHIFT;
	if (s.wire) p |= PSO_WIRE;
	d.pso = p;

	uint32_t f = 0;
	for (int u = 0; u < 2; u++) {
		if (!s.texEnabled[u] || !s.bound[u]) continue;
		GLTexture* t = R_GLTexture(s.bound[u], false);
		if (!t) continue;
		f |= u == 0 ? LF_TEX0 : LF_TEX1;
		if (t->clamp) f |= u == 0 ? LF_CLAMP0 : LF_CLAMP1;
		if (t->point) f |= u == 0 ? LF_POINT0 : LF_POINT1;
		(u == 0 ? d.tex0 : d.tex1) = t->name;
	}
	auto envCode = [](GLenum e) -> uint32_t { return e == GL_ADD ? 1 : e == GL_REPLACE ? 2 : e == GL_DECAL ? 3 : 0; };
	f |= envCode(s.env[1]) << LF_ENV_SHIFT;
	f |= envCode(s.env[0]) << LF_ENV0_SHIFT;
	if (s.alphaTest) {
		uint32_t a = 0;
		switch (s.alphaFunc) {
		case GL_GREATER: a = 1; break; case GL_LESS: a = 2; break; case GL_GEQUAL: a = 3; break;
		case GL_LEQUAL: a = 4; break; case GL_EQUAL: a = 5; break; case GL_NOTEQUAL: a = 6; break;
		case GL_NEVER: a = 7; break; default: a = 0;
		}
		f |= a << LF_ATEST_SHIFT;
		d.alphaRef = s.alphaRef;
	}
	if (s.glow) f |= LF_GLOW;
	d.flags = f;

	// matrices
	float mv[16], pr[16];
	memcpy(mv, s.stack[0].back().m, 64);
	if (R_InView()) memcpy(pr, R_ViewProjection(), 64);
	else {
		// GL clip z [-w, w] -> D3D [0, w]
		static const float fix[16] = { 1,0,0,0, 0,1,0,0, 0,0,0.5f,0, 0,0,0.5f,1 };
		MatMul(pr, fix, s.stack[1].back().m);
	}
	MatMul(d.mvp, pr, mv);

	memcpy(d.vp, s.vp, sizeof(d.vp));
	if (s.scissor) memcpy(d.sc, s.sc, sizeof(d.sc));
	else memcpy(d.sc, s.vp, sizeof(d.sc));
	if (rev) {
		d.depthMin = 1.0f - s.depthFar; d.depthMax = 1.0f - s.depthNear;
		// sky (depthRange 1,1): just in front of the cleared far plane, so the sky counts as drawn
		if (d.depthMax < 1e-7f) { d.depthMin = 1e-7f; d.depthMax = 1e-7f; }
	}
	else { d.depthMin = s.depthNear; d.depthMax = s.depthFar; }
	if (d.depthMin > d.depthMax) { float t = d.depthMin; d.depthMin = d.depthMax; d.depthMax = t; }
	d.stage = s.stage;
}

static bool Skip()
{
	if (s.stencilTest) return true;                 // stencil shadows / distortion masks: Katarn has its own
	if (!s.colorMask[0] && !s.colorMask[1] && !s.colorMask[2] && !s.colorMask[3] && !(s.depthWrite && s.depthTest)) return true;
	return false;
}

static inline uint32_t PackColor(const float* c)
{
	auto b = [](float v) -> uint32_t { int i = (int)(v * 255.0f + 0.5f); return (uint32_t)(i < 0 ? 0 : i > 255 ? 255 : i); };
	return b(c[0]) | (b(c[1]) << 8) | (b(c[2]) << 16) | (b(c[3]) << 24);
}

static void FetchVertex(int i, IVert& v)
{
	const ClientArray& a = s.vertex;
	const uint8_t* p = (const uint8_t*)a.ptr + (size_t)i * (a.stride ? a.stride : a.size * 4);
	const float* f = (const float*)p;
	v.pos[0] = f[0]; v.pos[1] = f[1]; v.pos[2] = a.size >= 3 ? f[2] : 0.0f;
	if (s.colorArr.on && s.colorArr.ptr) {
		const ClientArray& c = s.colorArr;
		if (c.type == GL_UNSIGNED_BYTE) {
			const uint8_t* q = (const uint8_t*)c.ptr + (size_t)i * (c.stride ? c.stride : c.size);
			v.color = q[0] | (q[1] << 8) | (q[2] << 16) | ((c.size == 4 ? q[3] : 255u) << 24);
		}
		else {
			const float* q = (const float*)((const uint8_t*)c.ptr + (size_t)i * (c.stride ? c.stride : c.size * 4));
			float cc[4] = { q[0], q[1], q[2], c.size == 4 ? q[3] : 1.0f };
			v.color = PackColor(cc);
		}
	}
	else v.color = PackColor(s.color);
	for (int u = 0; u < 2; u++) {
		float* out = u == 0 ? v.uv0 : v.uv1;
		const ClientArray& t = s.texcoord[u];
		if (t.on && t.ptr) {
			const float* q = (const float*)((const uint8_t*)t.ptr + (size_t)i * (t.stride ? t.stride : t.size * 4));
			out[0] = q[0]; out[1] = q[1];
		}
		else { out[0] = s.tc[u][0]; out[1] = s.tc[u][1]; }
	}
}

static void Emit(const IVert* verts, uint32_t nv, const uint32_t* idx, uint32_t ni, bool lines)
{
	if (!nv || !ni || Skip()) return;
	LegacyDraw d;
	Snapshot(d);
	if (lines) d.pso |= PSO_LINES;
	UploadAlloc vb = Upload((uint64_t)nv * sizeof(IVert), 32);
	UploadAlloc ib = Upload((uint64_t)ni * 4, 4);
	if (!vb.cpu || !ib.cpu) return;
	memcpy(vb.cpu, verts, (size_t)nv * sizeof(IVert));
	memcpy(ib.cpu, idx, (size_t)ni * 4);
	d.vb = vb.gpu; d.vbSize = nv * sizeof(IVert);
	d.ib = ib.gpu; d.indexCount = ni;
	R_AppendLegacy(d);
}

static std::vector<IVert> scratchV;
static std::vector<uint32_t> scratchI;

static void DrawIndexed(GLenum mode, const uint32_t* idx, uint32_t count)
{
	if (!s.vertex.ptr || !count) return;
	uint32_t lo = 0xffffffffu, hi = 0;
	for (uint32_t i = 0; i < count; i++) { if (idx[i] < lo) lo = idx[i]; if (idx[i] > hi) hi = idx[i]; }
	uint32_t nv = hi - lo + 1;
	scratchV.resize(nv);
	for (uint32_t i = 0; i < nv; i++) FetchVertex(lo + i, scratchV[i]);
	scratchI.clear();
	bool lines = false;
	switch (mode) {
	case GL_TRIANGLES:
		for (uint32_t i = 0; i + 2 < count; i += 3) { scratchI.push_back(idx[i] - lo); scratchI.push_back(idx[i + 1] - lo); scratchI.push_back(idx[i + 2] - lo); }
		break;
	case GL_TRIANGLE_STRIP:
		for (uint32_t i = 2; i < count; i++) {
			if (i & 1) { scratchI.push_back(idx[i - 1] - lo); scratchI.push_back(idx[i - 2] - lo); }
			else { scratchI.push_back(idx[i - 2] - lo); scratchI.push_back(idx[i - 1] - lo); }
			scratchI.push_back(idx[i] - lo);
		}
		break;
	case GL_TRIANGLE_FAN: case GL_POLYGON:
		for (uint32_t i = 2; i < count; i++) { scratchI.push_back(idx[0] - lo); scratchI.push_back(idx[i - 1] - lo); scratchI.push_back(idx[i] - lo); }
		break;
	case GL_QUADS:
		for (uint32_t i = 0; i + 3 < count; i += 4) {
			uint32_t a = idx[i] - lo, b = idx[i + 1] - lo, c = idx[i + 2] - lo, e = idx[i + 3] - lo;
			scratchI.insert(scratchI.end(), { a, b, c, a, c, e });
		}
		break;
	case GL_LINES:
		lines = true;
		for (uint32_t i = 0; i + 1 < count; i += 2) { scratchI.push_back(idx[i] - lo); scratchI.push_back(idx[i + 1] - lo); }
		break;
	case GL_LINE_STRIP: case GL_LINE_LOOP:
		lines = true;
		for (uint32_t i = 1; i < count; i++) { scratchI.push_back(idx[i - 1] - lo); scratchI.push_back(idx[i] - lo); }
		if (mode == GL_LINE_LOOP && count > 2) { scratchI.push_back(idx[count - 1] - lo); scratchI.push_back(idx[0] - lo); }
		break;
	default:
		return;   // points
	}
	Emit(scratchV.data(), nv, scratchI.data(), (uint32_t)scratchI.size(), lines);
}

} // namespace kt

using namespace kt;

// ================================================================================================ GL entry points
void APIENTRY ktgl_Enable(GLenum cap)
{
	switch (cap) {
	case GL_BLEND: s.blend = true; break;
	case GL_DEPTH_TEST: s.depthTest = true; break;
	case GL_ALPHA_TEST: s.alphaTest = true; break;
	case GL_CULL_FACE: s.cull = true; break;
	case GL_POLYGON_OFFSET_FILL: s.polyOffset = true; break;
	case GL_SCISSOR_TEST: s.scissor = true; break;
	case GL_STENCIL_TEST: s.stencilTest = true; break;
	case GL_TEXTURE_2D: s.texEnabled[s.activeUnit] = true; break;
	default: break;
	}
}
void APIENTRY ktgl_Disable(GLenum cap)
{
	switch (cap) {
	case GL_BLEND: s.blend = false; break;
	case GL_DEPTH_TEST: s.depthTest = false; break;
	case GL_ALPHA_TEST: s.alphaTest = false; break;
	case GL_CULL_FACE: s.cull = false; break;
	case GL_POLYGON_OFFSET_FILL: s.polyOffset = false; break;
	case GL_SCISSOR_TEST: s.scissor = false; break;
	case GL_STENCIL_TEST: s.stencilTest = false; break;
	case GL_TEXTURE_2D: s.texEnabled[s.activeUnit] = false; break;
	default: break;
	}
}
GLboolean APIENTRY ktgl_IsEnabled(GLenum cap)
{
	switch (cap) {
	case GL_BLEND: return s.blend; case GL_DEPTH_TEST: return s.depthTest; case GL_ALPHA_TEST: return s.alphaTest;
	case GL_CULL_FACE: return s.cull; case GL_SCISSOR_TEST: return s.scissor; case GL_STENCIL_TEST: return s.stencilTest;
	case GL_TEXTURE_2D: return s.texEnabled[s.activeUnit];
	default: return GL_FALSE;
	}
}
void APIENTRY ktgl_BlendFunc(GLenum sf, GLenum df) { s.src = sf; s.dst = df; }
void APIENTRY ktgl_DepthFunc(GLenum f) { s.depthFunc = f; }
void APIENTRY ktgl_DepthMask(GLboolean f) { s.depthWrite = f != 0; }
void APIENTRY ktgl_AlphaFunc(GLenum f, GLclampf ref) { s.alphaFunc = f; s.alphaRef = ref; }
void APIENTRY ktgl_CullFace(GLenum m) { s.cullFace = m; }
void APIENTRY ktgl_PolygonOffset(GLfloat, GLfloat) {}
void APIENTRY ktgl_PolygonMode(GLenum, GLenum mode) { s.wire = mode == GL_LINE; }
void APIENTRY ktgl_ColorMask(GLboolean r, GLboolean g_, GLboolean b, GLboolean a) { s.colorMask[0] = r; s.colorMask[1] = g_; s.colorMask[2] = b; s.colorMask[3] = a; }
void APIENTRY ktgl_ShadeModel(GLenum) {}
void APIENTRY ktgl_LineWidth(GLfloat) {}
void APIENTRY ktgl_StencilFunc(GLenum, GLint, GLuint) {}
void APIENTRY ktgl_StencilMask(GLuint) {}
void APIENTRY ktgl_StencilOp(GLenum, GLenum, GLenum) {}
void APIENTRY ktgl_ClearStencil(GLint) {}
void APIENTRY ktgl_ClipPlane(GLenum, const GLdouble*) {}
void APIENTRY ktgl_Fogf(GLenum, GLfloat) {}
void APIENTRY ktgl_Fogfv(GLenum, const GLfloat*) {}
void APIENTRY ktgl_Fogi(GLenum, GLint) {}
void APIENTRY ktgl_PixelStorei(GLenum, GLint) {}
void APIENTRY ktgl_DrawBuffer(GLenum) {}
void APIENTRY ktgl_Finish(void) {}
void APIENTRY ktgl_PushAttrib(GLbitfield) {}
void APIENTRY ktgl_PopAttrib(void) {}
void APIENTRY ktgl_CallList(GLuint) {}
void APIENTRY ktgl_DeleteLists(GLuint, GLsizei) {}
GLenum APIENTRY ktgl_GetError(void) { return GL_NO_ERROR; }

void APIENTRY ktgl_Viewport(GLint x, GLint y, GLsizei w, GLsizei h) { s.vp[0] = x; s.vp[1] = y; s.vp[2] = w; s.vp[3] = h; }
void APIENTRY ktgl_Scissor(GLint x, GLint y, GLsizei w, GLsizei h) { s.sc[0] = x; s.sc[1] = y; s.sc[2] = w; s.sc[3] = h; }
void APIENTRY ktgl_DepthRange(GLclampd n, GLclampd f) { s.depthNear = (float)n; s.depthFar = (float)f; }
void APIENTRY ktgl_ClearColor(GLclampf r, GLclampf g_, GLclampf b, GLclampf a) { s.clearColor[0] = r; s.clearColor[1] = g_; s.clearColor[2] = b; s.clearColor[3] = a; }
void APIENTRY ktgl_ClearDepth(GLclampd d) { s.clearDepth = (float)d; }
void APIENTRY ktgl_Clear(GLbitfield mask)
{
	if (R_InView()) return;      // Katarn clears its own view targets
	LegacyDraw d;
	Snapshot(d);
	d.clear = ((mask & GL_COLOR_BUFFER_BIT) ? 1 : 0) | ((mask & GL_DEPTH_BUFFER_BIT) ? 2 : 0);
	if (!d.clear) return;
	memcpy(d.clearColor, s.clearColor, 16);
	d.clearDepth = s.clearDepth;
	R_AppendLegacy(d);
}

// matrices
void APIENTRY ktgl_MatrixMode(GLenum m) { s.matrixMode = m; }
void APIENTRY ktgl_LoadIdentity(void) { MatIdentity(Top()); }
void APIENTRY ktgl_LoadMatrixf(const GLfloat* m) { memcpy(Top(), m, 64); }
void APIENTRY ktgl_PushMatrix(void) { auto& st = s.stack[s.matrixMode == GL_PROJECTION ? 1 : 0]; st.push_back(st.back()); }
void APIENTRY ktgl_PopMatrix(void) { auto& st = s.stack[s.matrixMode == GL_PROJECTION ? 1 : 0]; if (st.size() > 1) st.pop_back(); }
void APIENTRY ktgl_Translatef(GLfloat x, GLfloat y, GLfloat z)
{
	float t[16]; MatIdentity(t); t[12] = x; t[13] = y; t[14] = z;
	MatMul(Top(), Top(), t);
}
void APIENTRY ktgl_Ortho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
	float o[16] = {};
	o[0] = (float)(2.0 / (r - l)); o[5] = (float)(2.0 / (t - b)); o[10] = (float)(-2.0 / (f - n));
	o[12] = (float)(-(r + l) / (r - l)); o[13] = (float)(-(t + b) / (t - b)); o[14] = (float)(-(f + n) / (f - n)); o[15] = 1;
	MatMul(Top(), Top(), o);
}
void APIENTRY ktgl_GetFloatv(GLenum pname, GLfloat* p)
{
	if (pname == GL_MODELVIEW_MATRIX) memcpy(p, s.stack[0].back().m, 64);
	else if (pname == GL_PROJECTION_MATRIX) memcpy(p, s.stack[1].back().m, 64);
	else if (pname == GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT) *p = 16.0f;
	else *p = 0;
}
void APIENTRY ktgl_GetDoublev(GLenum pname, GLdouble* p)
{
	float f[16] = {};
	ktgl_GetFloatv(pname, f);
	int n = (pname == GL_MODELVIEW_MATRIX || pname == GL_PROJECTION_MATRIX) ? 16 : 1;
	for (int i = 0; i < n; i++) p[i] = f[i];
}
void APIENTRY ktgl_GetIntegerv(GLenum pname, GLint* p)
{
	switch (pname) {
	case GL_MAX_TEXTURE_SIZE: *p = 16384; break;
	case GL_MAX_TEXTURE_UNITS_ARB: *p = 2; break;
	case GL_VIEWPORT: memcpy(p, s.vp, 16); break;
	case GL_STENCIL_BITS: *p = 0; break;
	case GL_DEPTH_BITS: *p = 32; break;
	default: *p = 0; break;
	}
}
const GLubyte* APIENTRY ktgl_GetString(GLenum name)
{
	switch (name) {
	case GL_VENDOR: return (const GLubyte*)"KatarnEngine";
	case GL_RENDERER: return (const GLubyte*)(g.adapterName.empty() ? "Direct3D 12" : g.adapterName.c_str());
	case GL_VERSION: return (const GLubyte*)"1.3 KatarnEngine (Direct3D 12)";
	case GL_EXTENSIONS: return (const GLubyte*)"GL_ARB_multitexture GL_EXT_texture_env_add GL_EXT_texture_edge_clamp GL_SGIS_texture_edge_clamp GL_EXT_compiled_vertex_array GL_EXT_texture_filter_anisotropic";
	default: return (const GLubyte*)"";
	}
}

// textures
void APIENTRY ktgl_GenTextures(GLsizei n, GLuint* out)
{
	static GLuint next = 0x40000;
	for (int i = 0; i < n; i++) out[i] = next++;
}
void APIENTRY ktgl_DeleteTextures(GLsizei n, const GLuint* names) { for (int i = 0; i < n; i++) DeleteTexture(names[i]); }
void APIENTRY ktgl_BindTexture(GLenum, GLuint name) { s.bound[s.activeUnit] = name; }
void APIENTRY ktgl_TexEnvf(GLenum, GLenum pname, GLfloat v) { if (pname == GL_TEXTURE_ENV_MODE) s.env[s.activeUnit] = (GLenum)v; }
static void TexParam(GLenum pname, GLint v)
{
	GLTexture* t = R_GLTexture(s.bound[s.activeUnit], true);
	if (pname == GL_TEXTURE_WRAP_S || pname == GL_TEXTURE_WRAP_T) t->clamp = (v != GL_REPEAT);
	else if (pname == GL_TEXTURE_MIN_FILTER) t->point = (v == GL_NEAREST || v == GL_NEAREST_MIPMAP_NEAREST);
}
void APIENTRY ktgl_TexParameterf(GLenum, GLenum pname, GLfloat v) { TexParam(pname, (GLint)v); }
void APIENTRY ktgl_TexParameteri(GLenum, GLenum pname, GLint v) { TexParam(pname, v); }
void APIENTRY ktgl_TexParameterfv(GLenum, GLenum pname, const GLfloat* v) { TexParam(pname, (GLint)v[0]); }
void APIENTRY ktgl_TexImage2D(GLenum target, GLint level, GLint, GLsizei w, GLsizei h, GLint, GLenum format, GLenum type, const GLvoid* data)
{
	if (target != GL_TEXTURE_2D || level < 0 || level > 15 || w <= 0 || h <= 0) return;
	GLTexture* t = R_GLTexture(s.bound[s.activeUnit], true);
	if ((uint32_t)level >= t->levels.size()) t->levels.resize(level + 1);
	if (level == 0) { for (auto& l : t->levels) l.w = 0; t->subs.clear(); }
	GLLevel& L = t->levels[level];
	L.w = w; L.h = h;
	L.rgba.assign((size_t)w * h * 4, 255);
	if (data && type == GL_UNSIGNED_BYTE) {
		if (format == GL_RGBA) memcpy(L.rgba.data(), data, (size_t)w * h * 4);
		else if (format == GL_RGB) {
			const uint8_t* p = (const uint8_t*)data;
			for (int i = 0; i < w * h; i++) { L.rgba[i * 4] = p[i * 3]; L.rgba[i * 4 + 1] = p[i * 3 + 1]; L.rgba[i * 4 + 2] = p[i * 3 + 2]; }
		}
	}
	t->dirty = true;
}
void APIENTRY ktgl_TexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const GLvoid* data)
{
	if (target != GL_TEXTURE_2D || level != 0 || !data || type != GL_UNSIGNED_BYTE || format != GL_RGBA) return;
	GLTexture* t = R_GLTexture(s.bound[s.activeUnit], true);
	if (t->dirty && !t->levels.empty() && t->levels[0].w) {
		// not on the GPU yet: patch the pending level
		GLLevel& L = t->levels[0];
		for (int r = 0; r < h; r++)
			if (y + r < (int)L.h) memcpy(&L.rgba[((size_t)(y + r) * L.w + x) * 4], (const uint8_t*)data + (size_t)r * w * 4, (size_t)w * 4);
		return;
	}
	GLTexture::Sub sub;
	sub.x = x; sub.y = y; sub.w = w; sub.h = h;
	sub.rgba.assign((const uint8_t*)data, (const uint8_t*)data + (size_t)w * h * 4);
	t->subs.push_back(std::move(sub));
}
void APIENTRY ktgl_CopyTexImage2D(GLenum, GLint, GLenum, GLint, GLint, GLsizei, GLsizei, GLint) {}
void APIENTRY ktgl_CopyTexSubImage2D(GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei) {}
void APIENTRY ktgl_ReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, GLvoid* p)
{
	if (!p) return;
	if (format == GL_DEPTH_COMPONENT) { if (type == GL_FLOAT) for (int i = 0; i < w * h; i++) ((float*)p)[i] = 0.0f; return; }
	if (format == GL_STENCIL_INDEX) { memset(p, 0, (size_t)w * h); return; }
	if (type != GL_UNSIGNED_BYTE || (format != GL_RGB && format != GL_RGBA)) return;
	if (!R_ReadScreen(x, y, w, h, (uint8_t*)p, format == GL_RGBA))
		memset(p, 0, (size_t)w * h * (format == GL_RGBA ? 4 : 3));
}

// client arrays
static ClientArray* ArrayFor(GLenum a)
{
	switch (a) {
	case GL_VERTEX_ARRAY: return &s.vertex;
	case GL_COLOR_ARRAY: return &s.colorArr;
	case GL_TEXTURE_COORD_ARRAY: return &s.texcoord[s.clientUnit];
	default: return nullptr;
	}
}
void APIENTRY ktgl_EnableClientState(GLenum a) { if (ClientArray* c = ArrayFor(a)) c->on = true; }
void APIENTRY ktgl_DisableClientState(GLenum a) { if (ClientArray* c = ArrayFor(a)) c->on = false; }
void APIENTRY ktgl_VertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* p) { s.vertex = { s.vertex.on, size, type, stride, p }; }
void APIENTRY ktgl_ColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* p) { s.colorArr = { s.colorArr.on, size, type, stride, p }; }
void APIENTRY ktgl_TexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* p)
{
	ClientArray& t = s.texcoord[s.clientUnit];
	t = { t.on, size, type, stride, p };
}
void APIENTRY ktgl_DrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid* indices)
{
	if (count <= 0 || !indices) return;
	if (type == GL_UNSIGNED_INT) DrawIndexed(mode, (const uint32_t*)indices, count);
	else {
		std::vector<uint32_t> tmp(count);
		for (int i = 0; i < count; i++) tmp[i] = type == GL_UNSIGNED_SHORT ? ((const uint16_t*)indices)[i] : ((const uint8_t*)indices)[i];
		DrawIndexed(mode, tmp.data(), count);
	}
}
void APIENTRY ktgl_DrawArrays(GLenum mode, GLint first, GLsizei count)
{
	if (count <= 0) return;
	std::vector<uint32_t> idx(count);
	for (int i = 0; i < count; i++) idx[i] = first + i;
	DrawIndexed(mode, idx.data(), count);
}

// immediate mode
void APIENTRY ktgl_Begin(GLenum mode) { s.inBegin = true; s.prim = mode; s.imm.clear(); }
static void ImmVertex(float x, float y, float z)
{
	IVert v;
	v.pos[0] = x; v.pos[1] = y; v.pos[2] = z;
	v.uv0[0] = s.tc[0][0]; v.uv0[1] = s.tc[0][1];
	v.uv1[0] = s.tc[1][0]; v.uv1[1] = s.tc[1][1];
	v.color = PackColor(s.color);
	s.imm.push_back(v);
}
void APIENTRY ktgl_Vertex2f(GLfloat x, GLfloat y) { ImmVertex(x, y, 0); }
void APIENTRY ktgl_Vertex3f(GLfloat x, GLfloat y, GLfloat z) { ImmVertex(x, y, z); }
void APIENTRY ktgl_Vertex3fv(const GLfloat* v) { ImmVertex(v[0], v[1], v[2]); }
void APIENTRY ktgl_TexCoord2f(GLfloat u, GLfloat v) { s.tc[0][0] = u; s.tc[0][1] = v; }
void APIENTRY ktgl_TexCoord2fv(const GLfloat* v) { s.tc[0][0] = v[0]; s.tc[0][1] = v[1]; }
void APIENTRY ktgl_Color3f(GLfloat r, GLfloat g_, GLfloat b) { s.color[0] = r; s.color[1] = g_; s.color[2] = b; s.color[3] = 1; }
void APIENTRY ktgl_Color4f(GLfloat r, GLfloat g_, GLfloat b, GLfloat a) { s.color[0] = r; s.color[1] = g_; s.color[2] = b; s.color[3] = a; }
void APIENTRY ktgl_Color4ub(GLubyte r, GLubyte g_, GLubyte b, GLubyte a) { ktgl_Color4f(r / 255.0f, g_ / 255.0f, b / 255.0f, a / 255.0f); }
void APIENTRY ktgl_Color4ubv(const GLubyte* v) { ktgl_Color4ub(v[0], v[1], v[2], v[3]); }
void APIENTRY ktgl_ArrayElement(GLint i)
{
	IVert v;
	FetchVertex(i, v);
	s.imm.push_back(v);
}
void APIENTRY ktgl_End(void)
{
	s.inBegin = false;
	uint32_t n = (uint32_t)s.imm.size();
	if (!n) return;
	std::vector<uint32_t> idx;
	bool lines = false;
	switch (s.prim) {
	case GL_TRIANGLES: for (uint32_t i = 0; i + 2 < n; i += 3) idx.insert(idx.end(), { i, i + 1, i + 2 }); break;
	case GL_TRIANGLE_STRIP: for (uint32_t i = 2; i < n; i++) { if (i & 1) idx.insert(idx.end(), { i - 1, i - 2, i }); else idx.insert(idx.end(), { i - 2, i - 1, i }); } break;
	case GL_TRIANGLE_FAN: case GL_POLYGON: for (uint32_t i = 2; i < n; i++) idx.insert(idx.end(), { 0, i - 1, i }); break;
	case GL_QUADS: for (uint32_t i = 0; i + 3 < n; i += 4) idx.insert(idx.end(), { i, i + 1, i + 2, i, i + 2, i + 3 }); break;
	case GL_QUAD_STRIP: for (uint32_t i = 0; i + 3 < n; i += 2) idx.insert(idx.end(), { i, i + 1, i + 3, i, i + 3, i + 2 }); break;
	case GL_LINES: lines = true; for (uint32_t i = 0; i + 1 < n; i += 2) idx.insert(idx.end(), { i, i + 1 }); break;
	case GL_LINE_STRIP: case GL_LINE_LOOP:
		lines = true;
		for (uint32_t i = 1; i < n; i++) idx.insert(idx.end(), { i - 1, i });
		if (s.prim == GL_LINE_LOOP && n > 2) idx.insert(idx.end(), { n - 1, 0 });
		break;
	default: return;
	}
	Emit(s.imm.data(), n, idx.data(), (uint32_t)idx.size(), lines);
}

// multitexture extension entry points (assigned to the qgl*ARB pointers in tr_init.cpp)
void APIENTRY ktgl_ActiveTextureARB(GLenum unit) { s.activeUnit = (unit == GL_TEXTURE1_ARB) ? 1 : 0; }
void APIENTRY ktgl_ClientActiveTextureARB(GLenum unit) { s.clientUnit = (unit == GL_TEXTURE1_ARB) ? 1 : 0; }
void APIENTRY ktgl_MultiTexCoord2fARB(GLenum unit, GLfloat u, GLfloat v)
{
	int i = (unit == GL_TEXTURE1_ARB) ? 1 : 0;
	s.tc[i][0] = u; s.tc[i][1] = v;
}
void APIENTRY ktgl_LockArraysEXT(GLint, GLsizei) {}
void APIENTRY ktgl_UnlockArraysEXT(void) {}
