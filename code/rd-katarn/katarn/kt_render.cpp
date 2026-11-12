// KatarnEngine - the frame.
//
// Per 3D world view:
//   GBuffer      HD world meshes + legacy opaque surfaces (characters) -> albedo / normal+roughness+metal / emissive
//   Shadows      4 stabilised sun cascades (HD world + characters)
//   Sky          single-scattering atmosphere LUT + SH irradiance, every frame (the sun can move)
//   AO           horizon-based SSAO + depth-aware blur
//   Lighting     compute: GGX/Smith/Schlick sun + local lights, sky ambient + reflections, height fog
//   Forward      legacy blended content (sabers, effects, glass, HUD-in-world) into the HDR scene
//   Camera       histogram auto exposure, bloom, ACES filmic, FXAA
// 2D (menus, HUD, console) goes to the screen target, which is copied to the swap chain with r_gamma.
#include "kt_render.h"
#include "kt_world.h"
#include "kt_shaders_gen.h"
#include "../tr_local.h"

// SP and MP renderers share this module; MP's Cvar_Get takes a description
cvar_t* KtCvar(const char* name, const char* value, int flags)
{
#ifdef KT_MP
	return ri.Cvar_Get(name, value, flags, "");
#else
	return ri.Cvar_Get(name, value, flags);
#endif
}
#include <math.h>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <string>

namespace kt {

void GL_ShutdownTextures();
void GL_ResetState();

// ------------------------------------------------------------------------------------------------ cvars
static cvar_t* kt_debug, * kt_ssao, * kt_shadows, * kt_shadowRes, * kt_shadowDist, * kt_bloom, * kt_exposure, * kt_manualExposure;
static cvar_t* kt_fxaa, * kt_glowBoost, * kt_debugView, * kt_shaderDir, * kt_legacyDeferred, * kt_sun, * kt_fog, * kt_aoRadius, * kt_aoIntensity;
static cvar_t* kt_tonemap = nullptr;
static cvar_t* kt_fogScale = nullptr;
static cvar_t* kt_dynGlow, * kt_contactShadows, * kt_lightShadows;
static cvar_t* kt_probes = nullptr, * kt_probeScale = nullptr, * kt_parallax = nullptr, * kt_wind = nullptr;
static cvar_t* kt_vfog = nullptr, * kt_vfogDensity = nullptr, * kt_vfogLights = nullptr, * kt_vfogSun = nullptr, * kt_vfogFar = nullptr,
	* kt_vfogHeight = nullptr, * kt_vfogAmbient = nullptr, * kt_vfogColor = nullptr;
static cvar_t* kt_sss = nullptr, * kt_dof = nullptr, * kt_dofFocus = nullptr, * kt_dofAperture = nullptr, * kt_motionBlur = nullptr;
static cvar_t* kt_look = nullptr, * kt_sunScale = nullptr, * kt_skyLight = nullptr, * kt_sharpen = nullptr;
static cvar_t* kt_taa = nullptr, * kt_taaSharpen = nullptr, * kt_gi = nullptr, * kt_giRadius = nullptr;
static cvar_t* kt_bspSun = nullptr, * kt_bspSunAmbient = nullptr, * kt_bspSunDist = nullptr;
static cvar_t* kt_glowLights = nullptr;
static cvar_t* kt_bf3Rim = nullptr, * kt_bf3Spec = nullptr, * kt_bf3Refl = nullptr, * kt_filmGrain = nullptr, * kt_vignette = nullptr;
static std::vector<float> s_glowLights;
static cvar_t* kt_lights, * kt_dlightScale, * kt_speeds, * kt_ssr, * kt_volumetric, * kt_saberLight, * kt_saberGlow, * kt_saberDraw;

// ------------------------------------------------------------------------------------------------ rend2 colour grade
// rend2's tr_grade: r_grade* cvars, maps/<map>.grade files ("<cvar> <value>" lines, "preset <name>"), the "grade"
// command (save / load / reset / preset). Same names and files as rend2, so a map's grade carries over.
struct GradeVar { const char* name; const char* def; cvar_t* cv; };
static GradeVar s_grade[] = {
	{ "r_grade", "0", nullptr }, { "r_gradeCurve", "1", nullptr }, { "r_gradeExposure", "0", nullptr }, { "r_gradeContrast", "1.12", nullptr },
	{ "r_gradeSaturation", "0.92", nullptr }, { "r_gradeTemperature", "0", nullptr }, { "r_gradeTint", "0", nullptr },
	{ "r_gradeShadows", "1 1 1", nullptr }, { "r_gradeHighlights", "1 1 1", nullptr }, { "r_gradeBlack", "0", nullptr },
	{ "r_gradeVignette", "0.25", nullptr },
	{ "r_ssrStrength", "1", nullptr }, { "r_ssrGloss", "1", nullptr }, { "r_ssrRough", "1", nullptr },
	{ "r_atmosDensity", "0", nullptr }, { "r_atmosFalloff", "0.0012", nullptr }, { "r_atmosStart", "256", nullptr },
	{ "r_atmosColor", "0.55 0.6 0.7", nullptr }, { "r_atmosShafts", "0.6", nullptr },
};
static const int kNumGrade = (int)(sizeof(s_grade) / sizeof(s_grade[0]));
static cvar_t* r_atmosCv = nullptr;
struct GradePreset { const char* name; const char* v[10]; };
static const GradePreset s_gradePresets[] = {
	{ "default",  { "1", "0",    "1.12", "0.92", "0",     "0",     "1 1 1",          "1 1 1",          "0",    "0.25" } },
	{ "interior", { "1", "-0.1", "1.22", "0.78", "-0.25", "0",     "0.92 0.97 1.08", "1.02 1.01 0.98", "0.02", "0.30" } },
	{ "sunset",   { "2", "0.1",  "1.15", "1.05", "0.55",  "0.05",  "0.95 0.92 1.05", "1.10 1.00 0.86", "0.01", "0.30" } },
	{ "daylight", { "2", "0",    "1.10", "0.95", "0.15",  "-0.05", "0.96 1.00 1.04", "1.04 1.02 0.96", "0.01", "0.20" } },
	{ "space",    { "1", "0",    "1.25", "0.90", "0.20",  "0",     "0.90 0.95 1.10", "1.08 1.03 0.92", "0.03", "0.35" } },
	{ "noir",     { "1", "0",    "1.30", "0.15", "0",     "0",     "1 1 1",          "1 1 1",          "0.03", "0.45" } },
};

static void GradeVec3(const char* s, float* o)
{
	o[0] = o[1] = o[2] = 1.0f;
	if (sscanf(s, "%f %f %f", &o[0], &o[1], &o[2]) == 1) o[1] = o[2] = o[0];
}

static void GradeReset() { for (auto& g : s_grade) ri.Cvar_Set(g.name, g.def); }
static void GradePreset(const GradePreset& p) { for (int i = 0; i < 10; i++) ri.Cvar_Set(s_grade[i + 1].name, p.v[i]); }

static void GradePath(char* out, int size)
{
	char base[MAX_QPATH];
	Q_strncpyz(base, COM_SkipPath(tr.world ? tr.world->name : (char*)"nomap"), sizeof(base));
	COM_StripExtension(base, base, sizeof(base));
	Com_sprintf(out, size, "maps/%s.grade", base);
}

static void GradeLoadForMap()
{
	if (!s_grade[0].cv) return;
	char path[MAX_QPATH];
	GradePath(path, sizeof(path));
	GradeReset();
	char* buf = nullptr;
	if (ri.FS_ReadFile(path, (void**)&buf) <= 0 || !buf) return;
	const char* p = buf;
	int set = 0;
	while (*p) {
		char line[256]; int n = 0;
		while (*p && *p != '\n' && n < 255) line[n++] = *p++;
		line[n] = 0;
		if (*p) p++;
		if (char* cm = strstr(line, "//")) *cm = 0;
		char key[64], val[192];
		if (sscanf(line, " %63s %191[^\r\n]", key, val) != 2) continue;
		char* v = val; while (*v == ' ' || *v == '"') v++;
		for (char* e = v + strlen(v) - 1; e >= v && (*e == ' ' || *e == '"'); e--) *e = 0;
		if (!Q_stricmp(key, "preset")) { for (const auto& pr : s_gradePresets) if (!Q_stricmp(v, pr.name)) { GradePreset(pr); set++; } continue; }
		for (auto& g : s_grade) if (!Q_stricmp(key, g.name)) { ri.Cvar_Set(g.name, v); set++; }
	}
	ri.FS_FreeFile(buf);
	ri.Printf(PRINT_ALL, "grade: loaded %s (%d settings)\n", path, set);
}

static void Grade_f()
{
	const char* cmd = ri.Cmd_Argc() > 1 ? ri.Cmd_Argv(1) : "";
	char path[MAX_QPATH];
	GradePath(path, sizeof(path));
	if (!Q_stricmp(cmd, "save")) {
		char out[2048]; int len = 0;
		len += Com_sprintf(out + len, sizeof(out) - len, "// colour grade for %s (written by \"grade save\")\n", path);
		for (int i = 1; i < kNumGrade; i++) len += Com_sprintf(out + len, sizeof(out) - len, "%s \"%s\"\n", s_grade[i].name, s_grade[i].cv->string);
		ri.FS_WriteFile(path, out, len);
		ri.Printf(PRINT_ALL, "grade: saved %s\n", path);
	}
	else if (!Q_stricmp(cmd, "reset")) { GradeReset(); ri.Printf(PRINT_ALL, "grade: defaults restored\n"); }
	else if (!Q_stricmp(cmd, "load")) GradeLoadForMap();
	else if (!Q_stricmp(cmd, "preset")) {
		const char* n = ri.Cmd_Argc() > 2 ? ri.Cmd_Argv(2) : "";
		for (const auto& pr : s_gradePresets) if (!Q_stricmp(n, pr.name)) { GradePreset(pr); ri.Printf(PRINT_ALL, "grade: preset %s\n", pr.name); return; }
		ri.Printf(PRINT_ALL, "grade preset <name>:");
		for (const auto& pr : s_gradePresets) ri.Printf(PRINT_ALL, " %s", pr.name);
		ri.Printf(PRINT_ALL, "\n");
	}
	else {
		ri.Printf(PRINT_ALL, "grade save | load | reset | preset <name>   (map file: %s)\n", path);
		for (auto& g : s_grade) ri.Printf(PRINT_ALL, "  %-20s %s\n", g.name, g.cv->string);
	}
}

static void RegisterCvars()
{
	for (auto& gv : s_grade) gv.cv = KtCvar(gv.name, gv.def, CVAR_ARCHIVE);
	r_atmosCv = KtCvar("r_atmos", "1", CVAR_ARCHIVE);
#ifdef KT_MP
	ri.Cmd_AddCommand("grade", Grade_f, "colour grade: save | load | reset | preset <name>");
#else
	ri.Cmd_AddCommand("grade", Grade_f);
#endif
	kt_debug = KtCvar("kt_debug", "0", CVAR_LATCH);
	kt_ssao = KtCvar("kt_ssao", "1", CVAR_ARCHIVE);
	kt_shadows = KtCvar("kt_shadows", "1", CVAR_ARCHIVE);
	kt_shadowRes = KtCvar("kt_shadowRes", "2048", CVAR_ARCHIVE | CVAR_LATCH);
	kt_shadowDist = KtCvar("kt_shadowDist", "6000", CVAR_ARCHIVE);
	kt_bloom = KtCvar("kt_bloom", "0.06", CVAR_ARCHIVE);
	kt_exposure = KtCvar("kt_exposure", "0", CVAR_ARCHIVE);
	kt_tonemap = KtCvar("kt_tonemap", "1", CVAR_ARCHIVE);           // HD worlds: 1 AgX (Blender's look), 0 ACES
	kt_fogScale = KtCvar("kt_fogScale", "0.25", CVAR_ARCHIVE);      // HD world fog density (Blender renders have none)
	kt_manualExposure = KtCvar("kt_manualExposure", "0", CVAR_ARCHIVE);
	kt_fxaa = KtCvar("kt_fxaa", "1", CVAR_ARCHIVE);
	kt_glowBoost = KtCvar("kt_glowBoost", "3", CVAR_ARCHIVE);
	kt_debugView = KtCvar("kt_debugView", "0", 0);
	kt_shaderDir = KtCvar("kt_shaderDir", "", CVAR_ARCHIVE | CVAR_LATCH);
	kt_legacyDeferred = KtCvar("kt_legacyDeferred", "1", CVAR_ARCHIVE);
	kt_sun = KtCvar("kt_sun", "1", CVAR_ARCHIVE);
	kt_fog = KtCvar("kt_fog", "1", CVAR_ARCHIVE);
	kt_aoRadius = KtCvar("kt_aoRadius", "40", CVAR_ARCHIVE);
	kt_aoIntensity = KtCvar("kt_aoIntensity", "1.0", CVAR_ARCHIVE);
	kt_lights = KtCvar("kt_lights", "1", CVAR_ARCHIVE);
	kt_dlightScale = KtCvar("kt_dlightScale", "1.0", CVAR_ARCHIVE);
	kt_speeds = KtCvar("kt_speeds", "0", 0);
	kt_contactShadows = KtCvar("kt_contactShadows", "1", CVAR_ARCHIVE); // screen-space contact shadows (sun)
	kt_lightShadows = KtCvar("kt_lightShadows", "1", CVAR_ARCHIVE);     // screen-space shadows for sabers / dynamic lights
	kt_dynGlow = KtCvar("kt_dynamicGlow", "1", CVAR_ARCHIVE);   // rend2-style dynamic glow strength (glow stages blurred into the bloom)
	kt_ssr = KtCvar("kt_ssr", "1", CVAR_ARCHIVE);                 // screen-space reflections (HD worlds)
	kt_volumetric = KtCvar("kt_volumetric", "1", CVAR_ARCHIVE);   // sun shafts in the fog
	kt_saberLight = KtCvar("kt_saberLight", "1", CVAR_ARCHIVE);   // lightsaber blades as line lights (scale)
	kt_saberGlow = KtCvar("kt_saberGlowScale", "1", CVAR_ARCHIVE);   // saber glow brightness in the HDR scene (1 = as authored, like rend2)
	kt_saberDraw = KtCvar("kt_saberDraw2", "0", CVAR_ARCHIVE);     // 0: MD's own saber textures and trails (rend2 look), 1: Katarn's procedural blades
	// HD worlds: the Frostbite / Unreal style picture - a punchier tone curve, a stronger sun against darker, bluer shade
	kt_look = KtCvar("kt_look", "1", CVAR_ARCHIVE);               // 0 = Blender-neutral AgX, 1 = full, in between blends
	kt_sunScale = KtCvar("kt_sunScale", "1.4", CVAR_ARCHIVE);     // sun brightness (HD worlds)
	kt_skyLight = KtCvar("kt_skyLight", "0.65", CVAR_ARCHIVE);    // light from the sky in the shade (HD worlds)
	kt_sharpen = KtCvar("kt_sharpen", "0.3", CVAR_ARCHIVE);       // image sharpening (0 = off)
	kt_probes = KtCvar("kt_probes", "0", CVAR_ARCHIVE);           // reflection probes (cubemaps captured in game)
	kt_probeScale = KtCvar("kt_probeScale", "1", CVAR_ARCHIVE);   // their reflection strength
	kt_parallax = KtCvar("kt_parallax", "1", CVAR_ARCHIVE);       // parallax mapping (rend2 normal + height maps)
	kt_wind = KtCvar("kt_wind", "1", CVAR_ARCHIVE);
	kt_bf3Rim = KtCvar("kt_bf3Rim", "0.6", CVAR_ARCHIVE);     // BF3 Legacy shader patch outline light on characters (strength, 0 = off)
	kt_bf3Spec = KtCvar("kt_bf3Spec", "1", CVAR_ARCHIVE);
	kt_bf3Refl = KtCvar("kt_bf3Refl", "0.5", CVAR_ARCHIVE);   // BF3 Legacy fresnel reflection on characters (strength, 0 = off)
	kt_filmGrain = KtCvar("kt_filmGrain", "0.03", CVAR_ARCHIVE);   // BF3 Legacy film grain (0 = off)
	kt_vignette = KtCvar("kt_vignette", "0.25", CVAR_ARCHIVE);     // BF3 Legacy vignette (0 = off)     // BF3 Legacy specular: tinted by the surface colour, x2 (0 = off, 1 = full)
	kt_glowLights = KtCvar("kt_glowLights", "0", CVAR_ARCHIVE);   // glowing textures on old maps light their surroundings (strength, 0 = off)               // HD worlds: leaves move in the wind (0 = still)
	// old BSP maps: rend2's forced sun (r_forceSun 1, r_sunlightMode 1): the map's light is kept where the sun reaches
	// and scaled by kt_bspSunAmbient in its shadow / on surfaces facing away from it
	kt_bspSun = KtCvar("kt_bspSun", "1", CVAR_ARCHIVE);
	kt_taa = KtCvar("kt_taa", "0", CVAR_ARCHIVE);
	// volumetric fog (froxels): light from the sun, lamps and lightsabers scattered in the air
	kt_vfog = KtCvar("kt_vfog", "1", CVAR_ARCHIVE);
	kt_vfogDensity = KtCvar("kt_vfogDensity", "0.000015", CVAR_ARCHIVE);   // per unit (0.000015: 94% clear at 4000 units)
	kt_vfogLights = KtCvar("kt_vfogLights", "12", CVAR_ARCHIVE);         // how strongly lamps and sabers light the air
	kt_vfogSun = KtCvar("kt_vfogSun", "1", CVAR_ARCHIVE);
	kt_vfogFar = KtCvar("kt_vfogFar", "8000", CVAR_ARCHIVE);
	kt_vfogHeight = KtCvar("kt_vfogHeight", "0", CVAR_ARCHIVE);          // thinner with height (per unit above the floor)
	kt_vfogAmbient = KtCvar("kt_vfogAmbient", "0.08", CVAR_ARCHIVE);     // old maps: light already in the air
	kt_vfogColor = KtCvar("kt_vfogColor", "1 1 1", CVAR_ARCHIVE);
	kt_gi = KtCvar("kt_gi", "0", CVAR_ARCHIVE);
	kt_sss = KtCvar("kt_sss", "0.3", CVAR_ARCHIVE);               // skin subsurface scattering radius (units, 0 = off)
	// camera (for cinematics; off in play)
	kt_dof = KtCvar("kt_dof", "0", CVAR_ARCHIVE);                 // depth of field
	kt_dofFocus = KtCvar("kt_dofFocus", "0", 0);                  // focus distance in units, 0 = what is in the middle of the screen
	kt_dofAperture = KtCvar("kt_dofAperture", "1", CVAR_ARCHIVE); // blur strength
	kt_motionBlur = KtCvar("kt_motionBlur", "0", CVAR_ARCHIVE);   // camera motion blur (0.5 = half-open shutter)                   // HD worlds: bounced light (screen-space GI)
	kt_giRadius = KtCvar("kt_giRadius", "400", CVAR_ARCHIVE);     // how far bounced light travels (units)                 // temporal anti-aliasing (replaces FXAA when on)
	kt_taaSharpen = KtCvar("kt_taaSharpen", "0.35", CVAR_ARCHIVE);   // sharpening after TAA (0 = off)
	kt_bspSunAmbient = KtCvar("kt_bspSunAmbient", "0.5", CVAR_ARCHIVE);
	kt_bspSunDist = KtCvar("kt_bspSunDist", "3072", CVAR_ARCHIVE);   // 1: Katarn draws its own procedural blades instead of the game's

	// settings saved by older builds (tuning experiments) are put back to the current defaults once
	cvar_t* ver = KtCvar("kt_configVersion", "0", CVAR_ARCHIVE);
	if (ver->integer < 2) {
		const char* reset[][2] = { { "kt_bloom", "0.06" }, { "kt_dynamicGlow", "1" }, { "kt_glowBoost", "3" }, { "kt_saberGlowScale", "1" },
			{ "kt_saberLight", "1" }, { "kt_saberDraw2", "0" }, { "kt_exposure", "0" }, { "kt_manualExposure", "0" } };
		for (auto& r : reset) ri.Cvar_Set(r[0], r[1]);
		ri.Cvar_Set("kt_configVersion", "2");
		ri.Printf(PRINT_ALL, "KatarnEngine: bloom, glow and saber settings reset to the defaults\n");
	}
	if (ver->integer < 3) {
		// r11's first settings were too heavy (soft TAA, costly bounced light, thick fog, strong skin blur)
		const char* reset[][2] = { { "kt_taa", "0" }, { "kt_gi", "0" }, { "kt_vfogDensity", "0.000015" }, { "kt_vfogLights", "12" },
			{ "kt_vfogAmbient", "0.08" }, { "kt_sss", "0.3" }, { "kt_saberLight", "1" } };
		for (auto& r : reset) ri.Cvar_Set(r[0], r[1]);
		ri.Cvar_Set("kt_configVersion", "3");
	}
	if (ver->integer < 4) {
		ri.Cvar_Set("kt_saberDraw2", "1"); ri.Cvar_Set("kt_saberGlowScale", "1"); ri.Cvar_Set("kt_saberLight", "1");
		ri.Cvar_Set("kt_configVersion", "4");
	}
	if (ver->integer < 5) {
		ri.Cvar_Set("kt_saberDraw2", "0");   // back to MD's saber textures and trails
		ri.Cvar_Set("kt_configVersion", "5");
	}
	if (ver->integer < 6) {
		// rend2 look on old maps: no extra haze in the air, no sharpening (rend2 has neither)
		ri.Cvar_Set("kt_vfogAmbient", "0"); ri.Cvar_Set("kt_sharpen", "0");
		ri.Cvar_Set("kt_configVersion", "6");
	}
	if (ver->integer < 7) {
		ri.Cvar_Set("kt_glowLights", "0.35");   // r24: glowing panels lit nearby characters too strongly
		ri.Cvar_Set("kt_configVersion", "7");
	}
	if (ver->integer < 8) {
		ri.Cvar_Set("kt_probes", "0");   // r28: reflection probes off by default (performance)
		ri.Cvar_Set("kt_configVersion", "8");
	}
	if (ver->integer < 9) {
		ri.Cvar_Set("kt_glowLights", "0");   // r29: lights from glowing panels off (performance)
		ri.Cvar_Set("kt_configVersion", "9");
	}
}

// ------------------------------------------------------------------------------------------------ math
static void MatMul(float* out, const float* a, const float* b)
{
	float r[16];
	for (int c = 0; c < 4; c++)
		for (int rr = 0; rr < 4; rr++)
			r[c * 4 + rr] = a[0 * 4 + rr] * b[c * 4 + 0] + a[1 * 4 + rr] * b[c * 4 + 1] + a[2 * 4 + rr] * b[c * 4 + 2] + a[3 * 4 + rr] * b[c * 4 + 3];
	memcpy(out, r, 64);
}
static bool MatInverse(const float* m, float* out)
{
	float inv[16];
	inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
	inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
	inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
	inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
	inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
	inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
	inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
	inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
	inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
	inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
	inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
	inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
	inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
	inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
	inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
	inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
	float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
	if (fabsf(det) < 1e-30f) return false;
	det = 1.0f / det;
	for (int i = 0; i < 16; i++) out[i] = inv[i] * det;
	return true;
}
static inline float Dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static inline void Cross3(const float* a, const float* b, float* o) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }
static inline void Norm3(float* a) { float l = sqrtf(Dot3(a, a)); if (l > 0) { a[0] /= l; a[1] /= l; a[2] /= l; } }

// ------------------------------------------------------------------------------------------------ GPU structs
struct ViewConstants {
	float viewProj[16], invViewProj[16], worldToEye[16], prevViewProj[16];
	float camPos[4], viewport[4], invTargetSize[4], projParams[4], sunDir[4], sunColor[4], skyParams[4], fogParams[4], fogColor[4];
	float cascadeViewProj[4][16];
	float cascadeSplits[4], shadowParams[4];
	uint32_t indices0[4], indices1[4], indices2[4];
	float postParams[4];
	float ambientSH[9][4];
	float extraParams[4];      // x overbright scale (2^r_overBrightBits, applied when the frame is presented)
	float viewProjNoJitter[16];   // this frame's view-projection without the TAA jitter
	uint32_t probeParams[4];      // x reflection probe buffer, y probe count, z strength (float)
	float bspSun[4];              // old BSP maps: x 1 = sun shadows modulate the baked light, y light kept in shadow
	float grade0[4], grade1[4], grade2[4], grade3[4];   // rend2 colour grade (r_grade*), grade3.w 1 = on
	float atmos0[4], atmos1[4], atmos2[4];             // rend2 atmosphere (r_atmos*): haze rgb + density; sun uv, visible, shafts; sun rgb + falloff
	float atmos3[4];                                    // x haze start, y 1 = on
	float taaParams[4];           // x, y this frame's jitter (pixels), z sharpen, w 1 = TAA on
	float vfog0[4];               // volumetric fog: density, height falloff, -, far
	float vfog1[4];               // sun scattering, light scattering, ambient (BSP maps), near
	float vfogColor[4];
	uint32_t vfogIdx[4];          // x on, y grid w | h << 16, w depth slices
};

struct LVert { float pos[3]; float normal[3]; float uv[2]; float lmuv[2]; uint32_t color; };
static_assert(sizeof(LVert) == 44, "LVert");

struct DeferredRec {
	D3D12_GPU_VIRTUAL_ADDRESS vb, ib, cb;
	uint32_t vbSize, indexCount;
	int cull; bool alpha; bool castShadow;
};

// ------------------------------------------------------------------------------------------------ state
static struct RState {
	bool ready = false;
	uint32_t w = 0, h = 0;
	ID3D12GraphicsCommandList* cl = nullptr;
	// targets
	Texture screen, screenDepth, ldr;
	Texture hdr, depth, gb0, gb1, gb2, ao, aoTmp, shadow, skyLut;
	Texture overlay;                 // lightsaber blades in display space: added after the tone map (rend2's look)
	Texture saberGlow;               // their glow stages in display space (rend2's dynamic glow buffer)
	Texture saberGlowLvl[5];         // rend2's glowScaled chain: full size, then halves
	Texture glow;                    // glow stages (saber blades, lights) drawn again here; blurred into the bloom (rend2 dynamic glow)
	Texture hdrPrev;                 // last frame's lit scene (screen-space reflections)
	bool hdrPrevValid = false;
	std::vector<GpuLight> tubes;     // lightsaber blades this view (line lights)
	struct Blade { float a[3], glowR, b[3], coreR, color[3], intensity; uint32_t tex, mode, alpha, pad; float vcol[3], pad2; };
	std::vector<Blade> blades;       // lightsaber blades to draw
	ID3D12PipelineState* bladePSO = nullptr;
	Texture bloomDown[6], bloomUp[6];
	int bloomLevels = 0;
	Buffer exposure, sh;
	uint32_t lightSrv[kFrames] = {};
	uint32_t shadowRes = 2048;
	// pipelines
	Shader legacyVS, legacyPS, fsVS, presentPS, tonemapPS, fxaaPS, lgVS, lgPS, lgShadowPS;
	std::unordered_map<uint64_t, ID3D12PipelineState*> psos;
	ID3D12PipelineState* presentPSO = nullptr, * tonemapPSO = nullptr, * fxaaPSO = nullptr;
	ID3D12PipelineState* lgPSO[3] = {}, * lgShadowPSO = nullptr, * lgShadowAlphaPSO = nullptr;
	ID3D12PipelineState* csSkyLut = nullptr, * csSkySh = nullptr, * csLighting = nullptr, * csAo = nullptr, * csAoBlur = nullptr;
	ID3D12PipelineState* csDGlowDown = nullptr, * csDGlowUp = nullptr;
	ID3D12PipelineState* csHist = nullptr, * csExposure = nullptr, * csBloomDown = nullptr, * csBloomUp = nullptr;
	// frame
	std::vector<LegacyDraw> stream;
	std::vector<DeferredRec> deferred;
	bool inView = false, viewWorld = false, reversedZ = false;
	bool portalThisFrame = false;
	ViewDesc view;
	float proj[16] = {}, viewProj[16] = {}, prevViewProj[16] = {};
	float vpNoJitter[16] = {}, lastVpNoJitter[16] = {};   // TAA: unjittered, this and the last full-screen world view
	D3D12_GPU_VIRTUAL_ADDRESS viewCB = 0, defaultCB = 0;
	ViewConstants vc;
	int lastMsec = 0;
	float dt = 0.016f;
	bool firstFrame = true;
	float cascadeTexel[4] = {};
	int lastSpeeds = 0;
	// reflection probes
	struct Probe { float origin[3]; float radius; int cluster; Texture cube; uint32_t mipSrv[8]; uint8_t faces; bool valid; };
	std::vector<Probe> probes;
	uint32_t probeSrv[kFrames] = {};
	ID3D12PipelineState* csProbe = nullptr;
	// TAA
	ID3D12PipelineState* csTaa = nullptr;
	// bounced light
	ID3D12PipelineState* csGiTrace = nullptr, * csGiTemporal = nullptr, * csGiSpatial = nullptr;
	Texture giRaw, giAcc[2], gi;
	int giIdx = 0;
	bool giValid = false;
	uint32_t giFrame = 0;
	// volumetric fog
	ID3D12PipelineState* csFogInject = nullptr, * csFogIntegrate = nullptr, * csFogApply = nullptr;
	Texture froxIn, froxOut;
	bool fogTypedLoad = false;
	// skin
	ID3D12PipelineState* csSss = nullptr;
	ID3D12PipelineState* csFocus = nullptr, * csDof = nullptr, * csMotionBlur = nullptr;
	Texture sssTmp;
	Texture taa[2];
	int taaIdx = 0;
	bool taaValid = false, taaThisView = false;
	float taaJitter[2] = {};
	uint32_t taaFrame = 0;
	float taaLastOrg[3] = {}, taaLastFwd[3] = {};
	bool capture = false;            // this view is a probe capture
	int capProbe = 0, capFace = 0;
	float capSavedVP[16] = {};
	// old BSP maps: static sun-shadow caster mesh
	Buffer bspVB, bspIB, bspCB;
	uint32_t bspIndexCount = 0, bspVertCount = 0;
} R;
static constexpr uint32_t kProbeSize = 128, kProbeMips = 8, kMaxProbes = 64;

static std::string ShaderSource(const char* file)
{
	std::string dir = kt_shaderDir ? kt_shaderDir->string : "";
	if (!dir.empty()) {
		auto readAll = [](const std::string& p, std::string& out) {
			FILE* f = fopen(p.c_str(), "rb");
			if (!f) return false;
			fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
			out.resize(n);
			bool ok = fread(&out[0], 1, n, f) == (size_t)n;
			fclose(f);
			return ok;
		};
		std::string src, common;
		if (readAll(dir + "/" + file + ".hlsl", src) && readAll(dir + "/common.hlsli", common)) {
			size_t at = src.find("#include \"common.hlsli\"");
			if (at != std::string::npos) src.replace(at, strlen("#include \"common.hlsli\""), common);
			return src;
		}
	}
	struct E { const char* name; const char* const* parts; };
	static const E table[] = {
		{ "legacy", kt_src_legacy_parts }, { "present", kt_src_present_parts }, { "world", kt_src_world_parts },
		{ "legacy_gbuffer", kt_src_legacy_gbuffer_parts }, { "sky", kt_src_sky_parts }, { "lighting", kt_src_lighting_parts },
		{ "ssao", kt_src_ssao_parts }, { "post", kt_src_post_parts }, { "saber", kt_src_saber_parts },
		{ "probes", kt_src_probes_parts }, { "taa", kt_src_taa_parts }, { "gi", kt_src_gi_parts }, { "fog", kt_src_fog_parts }, { "sss", kt_src_sss_parts }, { "camera", kt_src_camera_parts },
	};
	for (auto& e : table) if (!strcmp(e.name, file)) { std::string s; for (int i = 0; e.parts[i]; i++) s += e.parts[i]; return s; }
	return std::string();
}

Shader R_Shader(const char* file, const char* entry, const char* target)
{
	std::string src = ShaderSource(file);
	return CompileShader(file, src.c_str(), entry, target);
}

static D3D12_SHADER_BYTECODE BC(const Shader& s)
{
	D3D12_SHADER_BYTECODE b = {};
	if (s.ok) { b.pShaderBytecode = s.blob->GetBufferPointer(); b.BytecodeLength = s.blob->GetBufferSize(); }
	return b;
}

static D3D12_GRAPHICS_PIPELINE_STATE_DESC BasePso()
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
	d.SampleMask = 0xffffffff;
	d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	d.RasterizerState.FrontCounterClockwise = TRUE;
	d.RasterizerState.DepthClipEnable = TRUE;
	for (auto& rt : d.BlendState.RenderTarget) rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL, rt.SrcBlend = D3D12_BLEND_ONE,
		rt.DestBlend = D3D12_BLEND_ZERO, rt.BlendOp = D3D12_BLEND_OP_ADD, rt.SrcBlendAlpha = D3D12_BLEND_ONE, rt.DestBlendAlpha = D3D12_BLEND_ZERO,
		rt.BlendOpAlpha = D3D12_BLEND_OP_ADD, rt.LogicOp = D3D12_LOGIC_OP_NOOP;
	d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	d.SampleDesc.Count = 1;
	return d;
}

// ------------------------------------------------------------------------------------------------ legacy pipelines
static const D3D12_INPUT_ELEMENT_DESC kLegacyLayout[] = {
	{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
};
static const D3D12_INPUT_ELEMENT_DESC kLgLayout[] = {
	{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
};

enum : uint32_t {   // must match kt_gl.cpp
	PSO_SRC_SHIFT = 0, PSO_DST_SHIFT = 4, PSO_BLEND = 1u << 8,
	PSO_DEPTHTEST = 1u << 9, PSO_DEPTHWRITE = 1u << 10, PSO_DFUNC_SHIFT = 11,
	PSO_CULL_SHIFT = 14, PSO_OFFSET = 1u << 16, PSO_MASK_SHIFT = 17, PSO_LINES = 1u << 21, PSO_WIRE = 1u << 22,
};

static D3D12_BLEND BlendOf(uint32_t c, bool alpha)
{
	static const D3D12_BLEND col[] = { D3D12_BLEND_ZERO, D3D12_BLEND_ONE, D3D12_BLEND_SRC_COLOR, D3D12_BLEND_INV_SRC_COLOR, D3D12_BLEND_SRC_ALPHA,
		D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_DEST_ALPHA, D3D12_BLEND_INV_DEST_ALPHA, D3D12_BLEND_DEST_COLOR, D3D12_BLEND_INV_DEST_COLOR, D3D12_BLEND_SRC_ALPHA_SAT };
	static const D3D12_BLEND alp[] = { D3D12_BLEND_ZERO, D3D12_BLEND_ONE, D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_SRC_ALPHA,
		D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_DEST_ALPHA, D3D12_BLEND_INV_DEST_ALPHA, D3D12_BLEND_DEST_ALPHA, D3D12_BLEND_INV_DEST_ALPHA, D3D12_BLEND_SRC_ALPHA_SAT };
	if (c > 10) c = 1;
	return alpha ? alp[c] : col[c];
}

static ID3D12PipelineState* LegacyPSO(uint32_t bits, int target)   // target 0 screen (RGBA8), 1 HDR (RGBA16F)
{
	uint64_t key = (uint64_t)bits | ((uint64_t)target << 32) | ((uint64_t)(R.reversedZ ? 1 : 0) << 34);
	auto it = R.psos.find(key);
	if (it != R.psos.end()) return it->second;
	D3D12_GRAPHICS_PIPELINE_STATE_DESC d = BasePso();
	d.VS = BC(R.legacyVS); d.PS = BC(R.legacyPS);
	d.InputLayout = { kLegacyLayout, 4 };
	auto& rt = d.BlendState.RenderTarget[0];
	if (bits & PSO_BLEND) {
		rt.BlendEnable = TRUE;
		uint32_t s = (bits >> PSO_SRC_SHIFT) & 15, ds = (bits >> PSO_DST_SHIFT) & 15;
		rt.SrcBlend = BlendOf(s, false); rt.DestBlend = BlendOf(ds, false);
		rt.SrcBlendAlpha = BlendOf(s, true); rt.DestBlendAlpha = BlendOf(ds, true);
	}
	rt.RenderTargetWriteMask = (UINT8)((bits >> PSO_MASK_SHIFT) & 15);
	if (bits & PSO_DEPTHTEST) {
		d.DepthStencilState.DepthEnable = TRUE;
		static const D3D12_COMPARISON_FUNC f[] = { D3D12_COMPARISON_FUNC_NEVER, D3D12_COMPARISON_FUNC_LESS, D3D12_COMPARISON_FUNC_EQUAL,
			D3D12_COMPARISON_FUNC_LESS_EQUAL, D3D12_COMPARISON_FUNC_GREATER, D3D12_COMPARISON_FUNC_NOT_EQUAL, D3D12_COMPARISON_FUNC_GREATER_EQUAL,
			D3D12_COMPARISON_FUNC_ALWAYS };
		d.DepthStencilState.DepthFunc = f[(bits >> PSO_DFUNC_SHIFT) & 7];
		d.DepthStencilState.DepthWriteMask = (bits & PSO_DEPTHWRITE) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
	}
	uint32_t cull = (bits >> PSO_CULL_SHIFT) & 3;
	d.RasterizerState.CullMode = cull == 1 ? D3D12_CULL_MODE_FRONT : cull == 2 ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
	if (cull == 3) d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;   // front and back: GL draws nothing; rare
	if (bits & PSO_OFFSET) {
		d.RasterizerState.DepthBias = R.reversedZ ? 2 : -2;
		d.RasterizerState.SlopeScaledDepthBias = R.reversedZ ? 1.0f : -1.0f;
		d.RasterizerState.DepthBiasClamp = 0.0f;
	}
	if (bits & PSO_WIRE) d.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
	d.PrimitiveTopologyType = (bits & PSO_LINES) ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	d.NumRenderTargets = target == 1 ? 4 : 1;   // HDR: + the glow target, the saber overlay and the saber glow (same blend)
	d.RTVFormats[0] = target == 1 ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
	if (target == 1) d.RTVFormats[1] = d.RTVFormats[2] = d.RTVFormats[3] = DXGI_FORMAT_R16G16B16A16_FLOAT;
	d.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	ID3D12PipelineState* p = CreateGraphicsPSO(d, "legacy");
	R.psos[key] = p;
	return p;
}

static ID3D12PipelineState* FullscreenPSO(const Shader& ps, DXGI_FORMAT fmt, const char* name)
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC d = BasePso();
	d.VS = BC(R.fsVS); d.PS = BC(ps);
	d.NumRenderTargets = 1;
	d.RTVFormats[0] = fmt;
	return CreateGraphicsPSO(d, name);
}

static bool CreatePipelines()
{
	R.legacyVS = R_Shader("legacy", "VS", "vs_5_1");
	R.legacyPS = R_Shader("legacy", "PS", "ps_5_1");
	R.fsVS = R_Shader("present", "FullscreenVS", "vs_5_1");
	R.presentPS = R_Shader("present", "PS", "ps_5_1");
	R.tonemapPS = R_Shader("post", "TonemapPS", "ps_5_1");
	R.fxaaPS = R_Shader("post", "FxaaPS", "ps_5_1");
	R.lgVS = R_Shader("legacy_gbuffer", "VS", "vs_5_1");
	R.lgPS = R_Shader("legacy_gbuffer", "PS", "ps_5_1");
	R.lgShadowPS = R_Shader("legacy_gbuffer", "ShadowPS", "ps_5_1");
	{
		// compile everything once up front so one run reports every shader error in katarn_log.txt
		const char* all[][3] = { { "world", "VS", "vs_5_1" }, { "world", "PS", "ps_5_1" }, { "world", "ShadowPS", "ps_5_1" },
			{ "sky", "LutCS", "cs_5_1" }, { "sky", "ShCS", "cs_5_1" }, { "lighting", "CS", "cs_5_1" }, { "ssao", "AoCS", "cs_5_1" },
			{ "ssao", "BlurCS", "cs_5_1" }, { "post", "HistogramCS", "cs_5_1" }, { "post", "ExposureCS", "cs_5_1" },
			{ "post", "BloomDownCS", "cs_5_1" }, { "post", "BloomUpCS", "cs_5_1" } };
		int bad = 0;
		for (auto& e : all) { Shader s = R_Shader(e[0], e[1], e[2]); if (!s.ok) bad++; if (s.blob) s.blob->Release(); }
		Shader* mine[] = { &R.legacyVS, &R.legacyPS, &R.fsVS, &R.presentPS, &R.tonemapPS, &R.fxaaPS, &R.lgVS, &R.lgPS, &R.lgShadowPS };
		for (auto* s : mine) if (!s->ok) bad++;
		if (bad) { Log("^1KatarnEngine: %d shaders failed to compile\n", bad); return false; }
	}
	R.presentPSO = FullscreenPSO(R.presentPS, DXGI_FORMAT_R8G8B8A8_UNORM, "present");
	R.tonemapPSO = FullscreenPSO(R.tonemapPS, DXGI_FORMAT_R8G8B8A8_UNORM, "tonemap");
	{
		Shader bvs = R_Shader("saber", "VS", "vs_5_1"), bps = R_Shader("saber", "PS", "ps_5_1");
		if (bvs.ok && bps.ok) {
			D3D12_GRAPHICS_PIPELINE_STATE_DESC d = BasePso();
			d.VS = BC(bvs); d.PS = BC(bps);
			auto& rt = d.BlendState.RenderTarget[0];
			// premultiplied: additive parts write a = 0, alpha-blended cores (dark blades) a = coverage
			rt.BlendEnable = TRUE; rt.SrcBlend = D3D12_BLEND_ONE; rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
			rt.SrcBlendAlpha = D3D12_BLEND_ZERO; rt.DestBlendAlpha = D3D12_BLEND_ONE;
			d.DepthStencilState.DepthEnable = FALSE;
			d.NumRenderTargets = 1;
			d.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
			R.bladePSO = CreateGraphicsPSO(d, "saber blades");
		}
	}
	R.fxaaPSO = FullscreenPSO(R.fxaaPS, DXGI_FORMAT_R8G8B8A8_UNORM, "fxaa");

	// legacy -> GBuffer, one per cull mode
	for (int c = 0; c < 3; c++) {
		D3D12_GRAPHICS_PIPELINE_STATE_DESC d = BasePso();
		d.VS = BC(R.lgVS); d.PS = BC(R.lgPS);
		d.InputLayout = { kLgLayout, 5 };
		d.RasterizerState.CullMode = c == 1 ? D3D12_CULL_MODE_FRONT : c == 2 ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
		d.DepthStencilState.DepthEnable = TRUE;
		d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
		d.NumRenderTargets = 3;
		d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; d.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT; d.RTVFormats[2] = DXGI_FORMAT_R16G16B16A16_FLOAT;
		d.DSVFormat = DXGI_FORMAT_D32_FLOAT;
		R.lgPSO[c] = CreateGraphicsPSO(d, "legacy gbuffer");
	}
	for (int a = 0; a < 2; a++) {
		D3D12_GRAPHICS_PIPELINE_STATE_DESC d = BasePso();
		d.VS = BC(R.lgVS);
		if (a) d.PS = BC(R.lgShadowPS);
		d.InputLayout = { kLgLayout, 5 };
		d.DepthStencilState.DepthEnable = TRUE;
		d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		d.RasterizerState.DepthBias = 1000; d.RasterizerState.SlopeScaledDepthBias = 2.0f; d.RasterizerState.DepthBiasClamp = 0.01f;
		d.NumRenderTargets = 0;
		d.DSVFormat = DXGI_FORMAT_D32_FLOAT;
		(a ? R.lgShadowAlphaPSO : R.lgShadowPSO) = CreateGraphicsPSO(d, "legacy shadow");
	}
	R.csSkyLut = CreateComputePSO(R_Shader("sky", "LutCS", "cs_5_1"), "sky lut");
	R.csSkySh = CreateComputePSO(R_Shader("sky", "ShCS", "cs_5_1"), "sky sh");
	R.csLighting = CreateComputePSO(R_Shader("lighting", "CS", "cs_5_1"), "lighting");
	R.csAo = CreateComputePSO(R_Shader("ssao", "AoCS", "cs_5_1"), "ssao");
	R.csAoBlur = CreateComputePSO(R_Shader("ssao", "BlurCS", "cs_5_1"), "ssao blur");
	R.csHist = CreateComputePSO(R_Shader("post", "HistogramCS", "cs_5_1"), "histogram");
	R.csExposure = CreateComputePSO(R_Shader("post", "ExposureCS", "cs_5_1"), "exposure");
	R.csBloomDown = CreateComputePSO(R_Shader("post", "BloomDownCS", "cs_5_1"), "bloom down");
	R.csBloomUp = CreateComputePSO(R_Shader("post", "BloomUpCS", "cs_5_1"), "bloom up");
	R.csDGlowDown = CreateComputePSO(R_Shader("post", "DGlowDownCS", "cs_5_1"), "saber glow down");
	R.csDGlowUp = CreateComputePSO(R_Shader("post", "DGlowUpCS", "cs_5_1"), "saber glow up");
	{
		Shader ps = R_Shader("probes", "FilterCS", "cs_5_1");   // optional: without it there are no probes
		if (ps.ok) R.csProbe = CreateComputePSO(ps, "probe filter");
		if (ps.blob) ps.blob->Release();
	}
	{
		Shader ts = R_Shader("taa", "ResolveCS", "cs_5_1");
		if (ts.ok) R.csTaa = CreateComputePSO(ts, "taa");
		if (ts.blob) ts.blob->Release();
	}
	{
		Shader fi = R_Shader("lighting", "FogInjectCS", "cs_5_1"), fg = R_Shader("fog", "IntegrateCS", "cs_5_1"), fa = R_Shader("fog", "ApplyCS", "cs_5_1");
		if (fi.ok) R.csFogInject = CreateComputePSO(fi, "fog inject");
		if (fg.ok) R.csFogIntegrate = CreateComputePSO(fg, "fog integrate");
		if (fa.ok) R.csFogApply = CreateComputePSO(fa, "fog apply");
		for (Shader* s : { &fi, &fg, &fa }) if (s->blob) s->blob->Release();
		// applying the fog reads the HDR scene as a UAV (typed load of RGBA16F)
		D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = { DXGI_FORMAT_R16G16B16A16_FLOAT };
		R.fogTypedLoad = SUCCEEDED(g.dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))) && (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD);
		{
			Shader a = R_Shader("camera", "FocusCS", "cs_5_1"), b = R_Shader("camera", "DofCS", "cs_5_1"), m = R_Shader("camera", "MotionBlurCS", "cs_5_1");
			if (a.ok) R.csFocus = CreateComputePSO(a, "dof focus");
			if (b.ok) R.csDof = CreateComputePSO(b, "dof");
			if (m.ok) R.csMotionBlur = CreateComputePSO(m, "motion blur");
			for (Shader* s : { &a, &b, &m }) if (s->blob) s->blob->Release();
		}
		Shader ss = R_Shader("sss", "BlurCS", "cs_5_1");
		if (ss.ok) R.csSss = CreateComputePSO(ss, "skin");
		if (ss.blob) ss.blob->Release();
		const char* e[3] = { "TraceCS", "TemporalCS", "SpatialCS" };
		ID3D12PipelineState** o[3] = { &R.csGiTrace, &R.csGiTemporal, &R.csGiSpatial };
		for (int i = 0; i < 3; i++) {
			Shader s = R_Shader("gi", e[i], "cs_5_1");
			if (s.ok) *o[i] = CreateComputePSO(s, "gi");
			if (s.blob) s.blob->Release();
		}
	}
	return R.presentPSO != nullptr;
}

// ------------------------------------------------------------------------------------------------ targets
static void DestroyTargets()
{
	Texture* all[] = { &R.screen, &R.screenDepth, &R.ldr, &R.hdr, &R.glow, &R.overlay, &R.saberGlow, &R.hdrPrev, &R.depth, &R.gb0, &R.gb1, &R.gb2, &R.ao, &R.aoTmp, &R.shadow, &R.skyLut };
	for (Texture* t : all) DestroyTexture(*t);
	for (auto& t : R.bloomDown) DestroyTexture(t);
	for (auto& t : R.saberGlowLvl) DestroyTexture(t);
	for (auto& t : R.bloomUp) DestroyTexture(t);
}

static bool CreateTargets(uint32_t w, uint32_t h)
{
	const float black[4] = { 0, 0, 0, 0 };
	const float zero[1] = { 0.0f };
	const float one[1] = { 1.0f };
	bool ok = true;
	ok &= CreateTexture(R.screen, w, h, 1, DXGI_FORMAT_R8G8B8A8_UNORM, TEX_RT, 1, black, "screen");
	ok &= CreateTexture(R.screenDepth, w, h, 1, DXGI_FORMAT_D32_FLOAT, TEX_DEPTH, 1, zero, "screen depth");
	ok &= CreateTexture(R.ldr, w, h, 1, DXGI_FORMAT_R8G8B8A8_UNORM, TEX_RT, 1, black, "ldr");
	ok &= CreateTexture(R.hdr, w, h, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, TEX_RT | TEX_UAV, 1, black, "hdr");
	ok &= CreateTexture(R.glow, w, h, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, TEX_RT, 1, black, "glow");
	ok &= CreateTexture(R.overlay, w, h, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, TEX_RT, 1, black, "saber overlay");
	ok &= CreateTexture(R.saberGlow, w, h, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, TEX_RT, 1, black, "saber glow");
	for (int i = 0; i < 5; i++)
		ok &= CreateTexture(R.saberGlowLvl[i], Q_max(1u, w >> i), Q_max(1u, h >> i), 1, DXGI_FORMAT_R16G16B16A16_FLOAT, TEX_UAV, 1, nullptr, "saber glow chain");
	ok &= CreateTexture(R.hdrPrev, w, h, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 1, nullptr, "hdr previous");
	R.hdrPrevValid = false;
	for (int i = 0; i < 2; i++) ok &= CreateTexture(R.taa[i], w, h, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, TEX_UAV, 1, nullptr, "taa history");
	R.taaValid = false;
	{
		uint32_t hw = Q_max(1u, w / 2), hh = Q_max(1u, h / 2);
		ok &= CreateTexture(R.giRaw, hw, hh, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, TEX_UAV, 1, nullptr, "gi raw");
		ok &= CreateTexture(R.gi, hw, hh, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, TEX_UAV, 1, nullptr, "gi");
		for (int i = 0; i < 2; i++) ok &= CreateTexture(R.giAcc[i], hw, hh, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, TEX_UAV, 1, nullptr, "gi history");
		R.giValid = false;
	}
	{
		// froxels: 1/16 of the screen per cell, 64 depth slices
		uint32_t fx = Q_max(16u, (w + 15) / 16), fy = Q_max(9u, (h + 15) / 16);
		ok &= CreateTexture(R.froxIn, fx, fy, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, TEX_UAV | TEX_3D, 64, nullptr, "fog froxels");
		ok &= CreateTexture(R.sssTmp, w, h, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, TEX_UAV, 1, nullptr, "skin blur");
		ok &= CreateTexture(R.froxOut, fx, fy, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, TEX_UAV | TEX_3D, 64, nullptr, "fog integrated");
	}
	ok &= CreateTexture(R.depth, w, h, 1, DXGI_FORMAT_D32_FLOAT, TEX_DEPTH, 1, zero, "depth");
	ok &= CreateTexture(R.gb0, w, h, 1, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, TEX_RT, 1, black, "gbuffer albedo");
	ok &= CreateTexture(R.gb1, w, h, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, TEX_RT, 1, black, "gbuffer normal");
	ok &= CreateTexture(R.gb2, w, h, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, TEX_RT, 1, black, "gbuffer emissive");
	ok &= CreateTexture(R.ao, w, h, 1, DXGI_FORMAT_R16_FLOAT, TEX_UAV, 1, nullptr, "ao");
	ok &= CreateTexture(R.aoTmp, w, h, 1, DXGI_FORMAT_R16_FLOAT, TEX_UAV, 1, nullptr, "ao tmp");
	R.shadowRes = (uint32_t)Q_max(512, Q_min(8192, kt_shadowRes->integer));
	ok &= CreateTexture(R.shadow, R.shadowRes, R.shadowRes, 1, DXGI_FORMAT_D32_FLOAT, TEX_DEPTH | TEX_ARRAY, 4, one, "sun shadows");
	ok &= CreateTexture(R.skyLut, 128, 64, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, TEX_UAV, 1, nullptr, "sky lut");
	R.bloomLevels = 0;
	uint32_t bw = w / 2, bh = h / 2;
	for (int i = 0; i < 6 && bw >= 4 && bh >= 4; i++, bw /= 2, bh /= 2) {
		ok &= CreateTexture(R.bloomDown[i], bw, bh, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, TEX_UAV, 1, nullptr, "bloom down");
		ok &= CreateTexture(R.bloomUp[i], bw, bh, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, TEX_UAV, 1, nullptr, "bloom up");
		R.bloomLevels++;
	}
	R.w = w; R.h = h;
	return ok;
}

static void ProbesDestroy();
static uint32_t ProbeList(const float* org);
// r27: sphere vs the current view frustum (lights / probes off screen cost every pixel nothing but still looped)
static float s_fr[6][4]; static bool s_frOk = false;
static void FrustumFromViewProj(const float* m, const float* org, const float* fwd)
{
	float M[4][4];
	float p[4] = { org[0] + fwd[0] * 100, org[1] + fwd[1] * 100, org[2] + fwd[2] * 100, 1 };
	int best = 0; float bestErr = 1e30f;
	for (int t = 0; t < 2; t++) {
		float c[4];
		for (int r = 0; r < 4; r++) { c[r] = 0; for (int k = 0; k < 4; k++) c[r] += (t ? m[k * 4 + r] : m[r * 4 + k]) * p[k]; }
		float e = c[3] > 1e-3f ? fabsf(c[0] / c[3]) + fabsf(c[1] / c[3]) : 1e30f;
		if (e < bestErr) { bestErr = e; best = t; }
	}
	for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) M[r][k] = best ? m[k * 4 + r] : m[r * 4 + k];
	const int sg[4][2] = { { 0, 1 }, { 0, -1 }, { 1, 1 }, { 1, -1 } };
	for (int i = 0; i < 4; i++)
		for (int k = 0; k < 4; k++) s_fr[i][k] = M[3][k] + sg[i][1] * M[sg[i][0]][k];
	for (int k = 0; k < 4; k++) s_fr[4][k] = M[3][k] - 0.0f * M[2][k];   // in front of the camera (w > 0)
	for (int i = 0; i < 5; i++) {
		float l = sqrtf(s_fr[i][0] * s_fr[i][0] + s_fr[i][1] * s_fr[i][1] + s_fr[i][2] * s_fr[i][2]);
		if (l > 1e-6f) for (int k = 0; k < 4; k++) s_fr[i][k] /= l;
	}
	s_frOk = bestErr < 0.5f;
}
static bool SphereVisible(const float* c, float r)
{
	if (!s_frOk) return true;
	for (int i = 0; i < 5; i++)
		if (s_fr[i][0] * c[0] + s_fr[i][1] * c[1] + s_fr[i][2] * c[2] + s_fr[i][3] < -r) return false;
	return true;
}

// ------------------------------------------------------------------------------------------------ init
void InstallCrashHandler();
bool R_Init(void* hwnd, int width, int height)
{
	InstallCrashHandler();
	RegisterCvars();
	Config cfg;
	cvar_t* swap = KtCvar("r_swapInterval", "0", CVAR_ARCHIVE);
	cfg.vsync = swap->integer != 0;
	cfg.debug = kt_debug->integer != 0;
	std::string err;
	if (!Init((HWND)hwnd, width, height, cfg, err)) {
		Log("^1KatarnEngine: Direct3D 12 start failed: %s\n", err.c_str());
		return false;
	}
	char cacheDir[1024];
	Com_sprintf(cacheDir, sizeof(cacheDir), "%s/katarn_shadercache", ri.Cvar_VariableString("fs_homepath"));
	CreateDirectoryA(cacheDir, nullptr);
	SetShaderCacheDir(cacheDir);
	if (!CreatePipelines()) { Log("^1KatarnEngine: shaders failed to build - see katarn_log.txt\n"); return false; }
	if (!CreateTargets(width, height)) { Log("^1KatarnEngine: render targets failed\n"); return false; }
	CreateBuffer(R.exposure, 512, true, "exposure");
	{
		float init[128] = {};
		init[0] = 1.0f; init[1] = 0.18f;
		UploadBuffer(R.exposure, 0, init, sizeof(init));
	}
	CreateBuffer(R.sh, 256, true, "sky sh");
	for (int i = 0; i < kFrames; i++) R.lightSrv[i] = AllocDescriptor();
	for (int i = 0; i < kFrames; i++) R.probeSrv[i] = AllocDescriptor();
	R.ready = true;
	R.firstFrame = true;
	GL_ResetState();
	Log("KatarnEngine: %dx%d, shadows %u, ready\n", width, height, R.shadowRes);
	return true;
}

void R_Shutdown()
{
	if (!g.dev) return;
	ri.Cmd_RemoveCommand("grade");
	if (R.cl) { SubmitList(R.cl); R.cl = nullptr; }
	WaitIdle();
	World_Shutdown();
	GL_ShutdownTextures();
	DestroyTargets();
	ProbesDestroy();
	if (R.bspVB.res) DestroyBuffer(R.bspVB);
	if (R.bspIB.res) DestroyBuffer(R.bspIB);
	if (R.bspCB.res) DestroyBuffer(R.bspCB);
	DestroyBuffer(R.exposure);
	DestroyBuffer(R.sh);
	for (auto& kv : R.psos) if (kv.second) kv.second->Release();
	R.psos.clear();
	ID3D12PipelineState* ps[] = { R.presentPSO, R.tonemapPSO, R.fxaaPSO, R.lgPSO[0], R.lgPSO[1], R.lgPSO[2], R.lgShadowPSO, R.lgShadowAlphaPSO,
		R.csSkyLut, R.csSkySh, R.csLighting, R.csAo, R.csAoBlur, R.csHist, R.csExposure, R.csBloomDown, R.csBloomUp, R.csDGlowDown, R.csDGlowUp, R.csProbe, R.csTaa, R.csGiTrace, R.csGiTemporal, R.csGiSpatial, R.csFogInject, R.csFogIntegrate, R.csFogApply, R.csSss, R.csFocus, R.csDof, R.csMotionBlur };
	for (auto* p : ps) if (p) p->Release();
	Shader* sh[] = { &R.legacyVS, &R.legacyPS, &R.fsVS, &R.presentPS, &R.tonemapPS, &R.fxaaPS, &R.lgVS, &R.lgPS, &R.lgShadowPS };
	for (auto* s : sh) if (s->blob) s->blob->Release();
	WaitIdle();
	R = RState();
	Shutdown();
}

// ------------------------------------------------------------------------------------------------ frame
static void EnsureList()
{
	if (!R.cl) {
		R.cl = OpenList();
		Transition(R.cl, R.screen, D3D12_RESOURCE_STATE_RENDER_TARGET);
		Transition(R.cl, R.screenDepth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		if (R.firstFrame) {
			const float c[4] = { 0, 0, 0, 1 };
			R.cl->ClearRenderTargetView(R.screen.rtv[0], c, 0, nullptr);
			R.firstFrame = false;
		}
		// a default view constant buffer so every shader that declares it reads something valid
		UploadAlloc a = Upload(sizeof(ViewConstants), 256);
		memset(a.cpu, 0, sizeof(ViewConstants));
		ViewConstants* v = (ViewConstants*)a.cpu;
		v->indices1[3] = R.exposure.srv;
		R.defaultCB = a.gpu;
		R.cl->SetGraphicsRootConstantBufferView(RP_VIEW_CBV, R.defaultCB);
		R.cl->SetGraphicsRootConstantBufferView(RP_EXTRA_CBV, R.defaultCB);
	}
}

static void SetViewportGL(ID3D12GraphicsCommandList* cl, const int* vp, const int* sc, float dmin, float dmax, uint32_t th)
{
	D3D12_VIEWPORT v = { (float)vp[0], (float)(int)(th - (vp[1] + vp[3])), (float)vp[2], (float)vp[3], dmin, dmax };
	cl->RSSetViewports(1, &v);
	LONG l = Q_max(sc[0], vp[0]), r = Q_min(sc[0] + sc[2], vp[0] + vp[2]);
	LONG b = Q_max(sc[1], vp[1]), t = Q_min(sc[1] + sc[3], vp[1] + vp[3]);
	if (r < l) r = l;
	if (t < b) t = b;
	D3D12_RECT rc = { l, (LONG)th - t, r, (LONG)th - b };
	if (rc.top < 0) rc.top = 0;
	cl->RSSetScissorRects(1, &rc);
}

// replays the recorded legacy draws into target 0 (screen) or 1 (HDR scene)
static void FlushStream(int target)
{
	if (R.stream.empty()) return;
	EnsureList();
	ID3D12GraphicsCommandList* cl = R.cl;
	Texture& rt = target == 1 ? R.hdr : R.screen;
	Texture& ds = target == 1 ? R.depth : R.screenDepth;
	Transition(cl, rt, D3D12_RESOURCE_STATE_RENDER_TARGET);
	Transition(cl, ds, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	if (target == 1) {
		Transition(cl, R.glow, D3D12_RESOURCE_STATE_RENDER_TARGET);
		Transition(cl, R.overlay, D3D12_RESOURCE_STATE_RENDER_TARGET);
		Transition(cl, R.saberGlow, D3D12_RESOURCE_STATE_RENDER_TARGET);
		D3D12_CPU_DESCRIPTOR_HANDLE four[4] = { rt.rtv[0], R.glow.rtv[0], R.overlay.rtv[0], R.saberGlow.rtv[0] };
		cl->OMSetRenderTargets(4, four, FALSE, &ds.dsv[0]);
	}
	else cl->OMSetRenderTargets(1, &rt.rtv[0], FALSE, &ds.dsv[0]);
	if (target == 1) cl->SetGraphicsRootConstantBufferView(RP_VIEW_CBV, R.viewCB);
	else cl->SetGraphicsRootConstantBufferView(RP_VIEW_CBV, R.defaultCB);
	ID3D12PipelineState* last = nullptr;
	D3D_PRIMITIVE_TOPOLOGY lastTopo = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
	float glow = kt_glowBoost->value;
	for (const LegacyDraw& d : R.stream) {
		if (d.clear) {
			int y0 = (int)rt.height - (d.sc[1] + d.sc[3]);
			D3D12_RECT rc = { d.sc[0], Q_max(0, y0), d.sc[0] + d.sc[2], y0 + d.sc[3] };
			if (d.clear & 1) cl->ClearRenderTargetView(rt.rtv[0], d.clearColor, 1, &rc);
			if (d.clear & 2) cl->ClearDepthStencilView(ds.dsv[0], D3D12_CLEAR_FLAG_DEPTH, R.reversedZ ? 1.0f - d.clearDepth : d.clearDepth, 0, 1, &rc);
			continue;
		}
		ID3D12PipelineState* p = LegacyPSO(d.pso, target);
		if (!p) continue;
		if (p != last) { cl->SetPipelineState(p); last = p; }
		D3D_PRIMITIVE_TOPOLOGY topo = (d.pso & PSO_LINES) ? D3D_PRIMITIVE_TOPOLOGY_LINELIST : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		if (topo != lastTopo) { cl->IASetPrimitiveTopology(topo); lastTopo = topo; }
		SetViewportGL(cl, d.vp, d.sc, d.depthMin, d.depthMax, rt.height);
		uint32_t c[32] = {};
		memcpy(c, d.mvp, 64);
		GLTexture* t0 = (d.flags & LF_TEX0) ? R_GLTexture(d.tex0, false) : nullptr;
		GLTexture* t1 = (d.flags & LF_TEX1) ? R_GLTexture(d.tex1, false) : nullptr;
		R_ResolveTexture(t0); R_ResolveTexture(t1);
		uint32_t flags = d.flags;
		c[16] = R_TextureSrv(t0, target == 1);
		c[17] = R_TextureSrv(t1, target == 1);
		if (!c[16]) flags &= ~LF_TEX0;
		if (!c[17]) flags &= ~LF_TEX1;
		c[18] = flags;
		memcpy(&c[19], &d.alphaRef, 4);
		c[20] = (uint32_t)target;
		memcpy(&c[21], &glow, 4);
		{ float sc = d.scale > 0 ? d.scale : 1.0f; memcpy(&c[22], &sc, 4); }
		cl->SetGraphicsRoot32BitConstants(RP_CONSTANTS, 24, c, 0);
		D3D12_VERTEX_BUFFER_VIEW vbv = { d.vb, d.vbSize, 32 };
		D3D12_INDEX_BUFFER_VIEW ibv = { d.ib, d.indexCount * 4, DXGI_FORMAT_R32_UINT };
		cl->IASetVertexBuffers(0, 1, &vbv);
		cl->IASetIndexBuffer(&ibv);
		cl->DrawIndexedInstanced(d.indexCount, 1, 0, 0, 0);
	}
	R.stream.clear();
}

void R_AppendLegacy(const LegacyDraw& d)
{
	if (!R.ready) return;
	EnsureList();
	R.stream.push_back(d);
	if (!R.inView && R.stream.size() > 4096) FlushStream(0);
}

void R_BeginFrame()
{
	if (!R.ready) return;
	EnsureList();
	R.portalThisFrame = false;
	int now = ri.Milliseconds();
	R.dt = R.lastMsec ? Q_min(0.25f, Q_max(0.0005f, (now - R.lastMsec) * 0.001f)) : 0.016f;
	R.lastMsec = now;
}

void R_EndFrame()
{
	if (!R.ready) return;
	EnsureList();
	FlushStream(0);
	ID3D12GraphicsCommandList* cl = R.cl;
	Texture& bb = CurrentBackbuffer();
	Transition(cl, R.screen, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	Transition(cl, bb, D3D12_RESOURCE_STATE_RENDER_TARGET);
	cl->OMSetRenderTargets(1, &bb.rtv[0], FALSE, nullptr);
	D3D12_VIEWPORT v = { 0, 0, (float)g.width, (float)g.height, 0, 1 };
	D3D12_RECT rc = { 0, 0, (LONG)g.width, (LONG)g.height };
	cl->RSSetViewports(1, &v);
	cl->RSSetScissorRects(1, &rc);
	cl->SetPipelineState(R.presentPSO);
	cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	float gamma = r_gamma ? Q_max(0.5f, Q_min(3.0f, r_gamma->value)) : 1.0f;
	float bright = 1.0f;
	uint32_t c[4] = { R.screen.srv, 0, 0, 0 };
	memcpy(&c[1], &gamma, 4);
	memcpy(&c[2], &bright, 4);
	cl->SetGraphicsRoot32BitConstants(RP_CONSTANTS, 4, c, 0);
	cl->DrawInstanced(3, 1, 0, 0);
	Transition(cl, bb, D3D12_RESOURCE_STATE_PRESENT);
	Transition(cl, R.screen, D3D12_RESOURCE_STATE_RENDER_TARGET);
	SubmitList(cl);
	R.cl = nullptr;
	Present();
}

bool R_InView() { return R.inView; }
bool R_ViewIsWorld() { return R.inView && R.viewWorld; }
bool R_ReversedZ() { return R.reversedZ; }
const float* R_ViewProjection() { return R.proj; }

const float* R_LegacyMVP(const float* modelView)
{
	static float m[16];
	MatMul(m, R.proj, modelView);
	return m;
}

void R_Begin2D()
{
	R.reversedZ = false;
}

void R_BeginView(const ViewDesc& v)
{
	if (!R.ready) return;
	EnsureList();
	FlushStream(0);
	R.view = v;
	R.inView = true;
	R.capture = (v.rdflags & RDF_KT_PROBE) != 0 && v.world;
	if (R.capture) {
		R.capProbe = (v.rdflags >> 20) & 63; R.capFace = (v.rdflags >> 26) & 7;
		memcpy(R.capSavedVP, R.viewProj, 64);        // a capture leaves the camera history (SSR reprojection) alone
	}
	R.viewWorld = v.world;
	R.reversedZ = true;
	// reversed-Z infinite perspective in GL eye space
	float sx = 1.0f / tanf(v.fovX * (float)M_PI / 360.0f);
	float sy = 1.0f / tanf(v.fovY * (float)M_PI / 360.0f);
	memset(R.proj, 0, 64);
	R.proj[0] = sx; R.proj[5] = sy; R.proj[11] = -1.0f; R.proj[14] = v.zNear;
	MatMul(R.vpNoJitter, R.proj, v.worldToEye);
	// TAA: the game's own world view only (not probe captures or the sky portal), a Halton (2,3) sub-pixel jitter
	R.taaThisView = v.world && !R.capture && !v.skyPortal && kt_taa->integer && R.csTaa && v.w >= 64 && v.h >= 64;
	R.taaJitter[0] = R.taaJitter[1] = 0;
	if (R.taaThisView) {
		auto halton = [](uint32_t i, uint32_t b) { float f = 1, r = 0; while (i) { f /= b; r += f * (i % b); i /= b; } return r; };
		uint32_t n = (R.taaFrame++ % 8) + 1;
		R.taaJitter[0] = halton(n, 2) - 0.5f; R.taaJitter[1] = halton(n, 3) - 0.5f;
		R.proj[8] = R.taaJitter[0] * 2.0f / v.w;
		R.proj[9] = -R.taaJitter[1] * 2.0f / v.h;
		// a camera cut or teleport: start the history again
		float fwd[3] = { -v.worldToEye[2], -v.worldToEye[6], -v.worldToEye[10] };
		float d[3] = { v.origin[0] - R.taaLastOrg[0], v.origin[1] - R.taaLastOrg[1], v.origin[2] - R.taaLastOrg[2] };
		if (Dot3(d, d) > 256.0f * 256.0f || Dot3(fwd, R.taaLastFwd) < 0.7f) R.taaValid = false;
		for (int k = 0; k < 3; k++) { R.taaLastOrg[k] = v.origin[k]; R.taaLastFwd[k] = fwd[k]; }
	}
	memcpy(R.prevViewProj, R.viewProj, 64);
	MatMul(R.viewProj, R.proj, v.worldToEye);
	R.deferred.clear();
	R.tubes.clear();
	R.blades.clear();

	// view constants
	ViewConstants& c = R.vc;
	memset(&c, 0, sizeof(c));
	memcpy(c.viewProj, R.viewProj, 64);
	FrustumFromViewProj(R.viewProj, v.origin, backEnd.viewParms.ori.axis[0]);
	if (R.capture) s_frOk = false;
	MatInverse(R.viewProj, c.invViewProj);
	memcpy(c.worldToEye, v.worldToEye, 64);
	memcpy(c.prevViewProj, R.prevViewProj, 64);
	if (R.taaThisView && R.taaValid) memcpy(c.prevViewProj, R.lastVpNoJitter, 64);   // history is resolved: no jitter in it
	memcpy(c.viewProjNoJitter, R.vpNoJitter, 64);
	c.taaParams[0] = R.taaJitter[0]; c.taaParams[1] = R.taaJitter[1];
	c.taaParams[2] = Q_max(0.0f, Q_min(1.0f, R.taaThisView ? Q_max(kt_taaSharpen->value, kt_sharpen->value) : (v.world && !R.capture ? kt_sharpen->value : 0.0f)));
	c.taaParams[3] = R.taaThisView ? 1.0f : 0.0f;
	if (v.world && !R.capture && !v.skyPortal && kt_vfog->integer && (World_Active() || kt_vfog->integer >= 2) && kt_vfogDensity->value > 0 && R.csFogInject && R.csFogIntegrate
		&& R.csFogApply && R.fogTypedLoad && R.froxIn.res) {
		c.vfog0[0] = kt_vfogDensity->value; c.vfog0[1] = Q_max(0.0f, kt_vfogHeight->value); c.vfog0[3] = Q_max(1000.0f, kt_vfogFar->value);
		c.vfog1[0] = Q_max(0.0f, kt_vfogSun->value); c.vfog1[1] = Q_max(0.0f, kt_vfogLights->value); c.vfog1[2] = Q_max(0.0f, kt_vfogAmbient->value);
		c.vfog1[3] = 16.0f;
		float fc[3] = { 1, 1, 1 };
		sscanf(kt_vfogColor->string, "%f %f %f", &fc[0], &fc[1], &fc[2]);
		c.vfogColor[0] = fc[0]; c.vfogColor[1] = fc[1]; c.vfogColor[2] = fc[2];
		c.vfogIdx[0] = 1; c.vfogIdx[1] = R.froxIn.width | (R.froxIn.height << 16); c.vfogIdx[3] = R.froxIn.layers;
	}
	c.camPos[0] = v.origin[0]; c.camPos[1] = v.origin[1]; c.camPos[2] = v.origin[2]; c.camPos[3] = v.zNear;
	c.viewport[0] = (float)v.x; c.viewport[1] = (float)((int)R.h - (v.y + v.h)); c.viewport[2] = (float)v.w; c.viewport[3] = (float)v.h;
	c.invTargetSize[0] = 1.0f / R.w; c.invTargetSize[1] = 1.0f / R.h; c.invTargetSize[2] = (float)R.w; c.invTargetSize[3] = (float)R.h;
	c.projParams[0] = sx; c.projParams[1] = sy; c.projParams[2] = v.zNear; c.projParams[3] = v.time;
	const WorldEnv& env = World_Env();
	float sd[3] = { env.sunDir[0], env.sunDir[1], env.sunDir[2] };
	Norm3(sd);
	c.sunDir[0] = sd[0]; c.sunDir[1] = sd[1]; c.sunDir[2] = sd[2]; c.sunDir[3] = Q_max(0.004f, env.sunAngle);
	c.sunColor[0] = env.sunColor[0]; c.sunColor[1] = env.sunColor[1]; c.sunColor[2] = env.sunColor[2];
	c.sunColor[3] = (kt_sun->integer && World_Active()) ? env.sunIntensity * Q_max(0.0f, kt_sunScale->value) : 0.0f;   // old BSP maps: their own baked light only
	c.skyParams[0] = env.haze; c.skyParams[1] = env.skyIntensity; c.skyParams[2] = env.ambient * Q_max(0.0f, kt_skyLight->value); c.skyParams[3] = World_Active() ? 1.0f : 0.0f;
	if (env.envSrv) { c.indices2[0] = env.envSrv; c.skyParams[1] = env.envStrength; float rs = env.envLight * Q_max(0.0f, kt_skyLight->value) / Q_max(1e-4f, env.envStrength); memcpy(&c.indices2[2], &rs, 4); }
	if (env.hasSH) { c.indices2[1] = 1; for (int k = 0; k < 9; k++) for (int q = 0; q < 3; q++) c.ambientSH[k][q] = env.sh[k][q]; }
	if (kt_fog->integer) {
		c.fogParams[0] = env.fogDensity * Q_max(0.0f, kt_fogScale->value); c.fogParams[1] = env.fogFalloff; c.fogParams[2] = env.fogHeight; c.fogParams[3] = env.fogStart;
	}
	c.fogColor[0] = env.fogColor[0]; c.fogColor[1] = env.fogColor[1]; c.fogColor[2] = env.fogColor[2]; c.fogColor[3] = env.fogMax;
	c.indices0[0] = R.gb0.srv; c.indices0[1] = R.gb1.srv; c.indices0[2] = R.gb2.srv; c.indices0[3] = R.depth.srv;
	c.indices1[0] = R.shadow.srv; c.indices1[1] = R.ao.srv; c.indices1[2] = R.skyLut.srv; c.indices1[3] = R.exposure.srv;
	c.indices2[3] = (uint32_t)g.frameNumber;
	c.postParams[0] = kt_exposure->value + env.exposureEV;
	c.postParams[1] = kt_bloom->value;
	c.postParams[2] = kt_manualExposure->value;
	c.postParams[3] = kt_glowBoost->value;
	c.extraParams[0] = (float)(1 << tr.overbrightBits);
	c.extraParams[1] = kt_tonemap->integer ? 1.0f + Q_max(0.0f, Q_min(1.5f, kt_look->value)) : 0.0f;   // 0 ACES, 1 AgX, 1..2.5 AgX + look
	// sky portal maps: the portal view is drawn first; the main view keeps it where it drew nothing (its sky)
	if (v.skyPortal) R.portalThisFrame = true;
	c.extraParams[2] = (!v.skyPortal && R.portalThisFrame && !R.capture) ? 1.0f : 0.0f;
	c.extraParams[3] = Q_max(0.0f, kt_wind->value) * 1.5f;   // HD world foliage sway, in units
	if (v.world && !R.capture && kt_probes->integer) {
		c.probeParams[0] = R.probeSrv[g.frameIndex];
		c.probeParams[1] = ProbeList(v.origin);
		float ps = Q_max(0.0f, kt_probeScale->value);
		memcpy(&c.probeParams[2], &ps, 4);
	}
	if (!World_Active()) c.postParams[2] = 1.0f;   // legacy maps: fixed exposure (their content is display-referred)

	// old BSP maps: rend2's forced sun - the sky shader's q3map_sun direction (or rend2's default), shadows only
	bool bspSun = v.world && !World_Active() && kt_bspSun->integer && kt_shadows->integer && R.bspIndexCount && tr.world;
	if (bspSun) {
		sd[0] = tr.sunDirection[0]; sd[1] = tr.sunDirection[1]; sd[2] = tr.sunDirection[2];
		Norm3(sd);
		if (sd[2] < 0.05f) bspSun = false;
		c.sunDir[0] = sd[0]; c.sunDir[1] = sd[1]; c.sunDir[2] = sd[2];
		c.bspSun[0] = bspSun ? 1.0f : 0.0f;
		c.bspSun[1] = Q_max(0.0f, Q_min(1.0f, kt_bspSunAmbient->value));
	}

	// rend2 colour grade (any map) and atmosphere (old BSP maps: haze + screen-space sun shafts)
	if (s_grade[0].cv && s_grade[0].cv->integer && !R.capture) {
		float sh[3], hi[3];
		GradeVec3(s_grade[7].cv->string, sh); GradeVec3(s_grade[8].cv->string, hi);
		c.grade0[0] = powf(2.0f, s_grade[2].cv->value); c.grade0[1] = s_grade[3].cv->value; c.grade0[2] = s_grade[4].cv->value;
		c.grade0[3] = Q_max(0.0f, Q_min(1.0f, s_grade[10].cv->value));
		c.grade1[0] = sh[0]; c.grade1[1] = sh[1]; c.grade1[2] = sh[2]; c.grade1[3] = (float)s_grade[1].cv->integer;
		c.grade2[0] = hi[0]; c.grade2[1] = hi[1]; c.grade2[2] = hi[2]; c.grade2[3] = Q_max(0.0f, Q_min(0.2f, s_grade[9].cv->value));
		float t = Q_max(-1.0f, Q_min(1.0f, s_grade[5].cv->value)), m = Q_max(-1.0f, Q_min(1.0f, s_grade[6].cv->value));
		c.grade3[0] = 1.0f + 0.18f * t + 0.06f * m; c.grade3[1] = 1.0f - 0.12f * m; c.grade3[2] = 1.0f - 0.18f * t + 0.06f * m;
		float lum = 0.2126f * c.grade3[0] + 0.7152f * c.grade3[1] + 0.0722f * c.grade3[2];
		c.grade3[0] /= lum; c.grade3[1] /= lum; c.grade3[2] /= lum;
		c.grade3[3] = 1.0f;
	}
	if (v.world && !World_Active() && !R.capture && !v.skyPortal && r_atmosCv && r_atmosCv->integer && tr.world
		&& (s_grade[14].cv->value > 0 || s_grade[18].cv->value > 0)) {
		float col[3] = { 0.55f, 0.6f, 0.7f };
		sscanf(s_grade[17].cv->string, "%f %f %f", &col[0], &col[1], &col[2]);
		float sun[3] = { tr.sunDirection[0], tr.sunDirection[1], tr.sunDirection[2] };
		Norm3(sun);
		// the sun on screen
		float sunPt[4] = { v.origin[0] + sun[0] * 1e5f, v.origin[1] + sun[1] * 1e5f, v.origin[2] + sun[2] * 1e5f, 1 }, cp[4];
		for (int r = 0; r < 4; r++) cp[r] = R.viewProj[r] * sunPt[0] + R.viewProj[4 + r] * sunPt[1] + R.viewProj[8 + r] * sunPt[2] + R.viewProj[12 + r];
		float vis = 0, su = 0.5f, sv = 0.5f;
		if (cp[3] > 0) {
			su = cp[0] / cp[3] * 0.5f + 0.5f; sv = -cp[1] / cp[3] * 0.5f + 0.5f;
			float off = Q_max(fabsf(su - 0.5f), fabsf(sv - 0.5f));
			vis = Q_max(0.0f, Q_min(1.0f, (1.2f - off) / 0.5f));
		}
		float smax = Q_max(tr.sunLight[0], Q_max(tr.sunLight[1], tr.sunLight[2]));
		float scol[3] = { 1, 0.95f, 0.85f };
		if (smax > 0) for (int k = 0; k < 3; k++) scol[k] = tr.sunLight[k] / smax;
		c.atmos0[0] = col[0]; c.atmos0[1] = col[1]; c.atmos0[2] = col[2]; c.atmos0[3] = Q_max(0.0f, s_grade[14].cv->value);
		c.atmos1[0] = su; c.atmos1[1] = sv; c.atmos1[2] = vis; c.atmos1[3] = Q_max(0.0f, s_grade[18].cv->value);
		c.atmos2[0] = scol[0]; c.atmos2[1] = scol[1]; c.atmos2[2] = scol[2]; c.atmos2[3] = Q_max(1e-5f, s_grade[15].cv->value);
		c.atmos3[0] = s_grade[16].cv->value; c.atmos3[1] = 1.0f;
		c.sunDir[0] = sun[0]; c.sunDir[1] = sun[1]; c.sunDir[2] = sun[2];
	}
	// BF3 Legacy post (shader patch postprocess): film grain and vignette
	c.atmos3[2] = R.capture ? 0.0f : Q_max(0.0f, kt_filmGrain->value);
	c.atmos3[3] = R.capture ? 0.0f : Q_max(0.0f, Q_min(1.0f, kt_vignette->value));

	// sun cascades
	float shadowDist = bspSun ? Q_max(500.0f, kt_bspSunDist->value) : Q_max(500.0f, kt_shadowDist->value);
	bool shadows = v.world && kt_shadows->integer && ((World_Active() && c.sunColor[3] > 0) || bspSun) && sd[2] > -0.05f;
	if (shadows) {
		float n = Q_max(4.0f, v.zNear), f = shadowDist;
		float splits[5] = { n };
		for (int i = 1; i <= 4; i++) {
			float t = i / 4.0f;
			splits[i] = 0.82f * n * powf(f / n, t) + 0.18f * (n + (f - n) * t);
		}
		if (R.capture) splits[1] = splits[2] = splits[3] = n;   // probe capture: one cascade over the whole view
		float tx = tanf(v.fovX * (float)M_PI / 360.0f), ty = tanf(v.fovY * (float)M_PI / 360.0f);
		float fwd[3] = { v.axis[0][0], v.axis[0][1], v.axis[0][2] };
		float lft[3] = { v.axis[1][0], v.axis[1][1], v.axis[1][2] };
		float up[3] = { v.axis[2][0], v.axis[2][1], v.axis[2][2] };
		float L[3] = { -sd[0], -sd[1], -sd[2] };        // direction light travels
		float ref[3] = { 0, 0, 1 };
		if (fabsf(L[2]) > 0.99f) { ref[0] = 1; ref[2] = 0; }
		float rgt[3], upl[3];
		Cross3(L, ref, rgt); Norm3(rgt);
		Cross3(rgt, L, upl); Norm3(upl);
		for (int ci = 0; ci < 4; ci++) {
			float d0 = splits[ci], d1 = splits[ci + 1];
			// bounding sphere of the frustum slice (stable under rotation)
			float corners[8][3];
			int k = 0;
			for (int s = 0; s < 2; s++) {
				float d = s ? d1 : d0;
				for (int yy = -1; yy <= 1; yy += 2)
					for (int xx = -1; xx <= 1; xx += 2, k++)
						for (int a = 0; a < 3; a++)
							corners[k][a] = v.origin[a] + fwd[a] * d - lft[a] * (xx * tx * d) + up[a] * (yy * ty * d);
			}
			float ctr[3] = { 0, 0, 0 };
			for (int i = 0; i < 8; i++) for (int a = 0; a < 3; a++) ctr[a] += corners[i][a] / 8.0f;
			float rad = 0;
			for (int i = 0; i < 8; i++) {
				float dd[3] = { corners[i][0] - ctr[0], corners[i][1] - ctr[1], corners[i][2] - ctr[2] };
				rad = Q_max(rad, sqrtf(Dot3(dd, dd)));
			}
			rad = ceilf(rad / 16.0f) * 16.0f;
			float texel = 2.0f * rad / R.shadowRes;
			R.cascadeTexel[ci] = texel;
			// snap the centre to whole texels in light space
			float cx = Dot3(ctr, rgt), cy = Dot3(ctr, upl), cz = Dot3(ctr, L);
			cx = floorf(cx / texel) * texel; cy = floorf(cy / texel) * texel;
			float back = 30000.0f;                // casters up to this far towards the sun
			float zr = rad + back + rad;
			float* m = c.cascadeViewProj[ci];
			memset(m, 0, 64);
			// rows: x = (p.rgt - cx)/rad, y = (p.upl - cy)/rad, z = (p.L - (cz - rad - back)) / zr
			m[0] = rgt[0] / rad; m[4] = rgt[1] / rad; m[8] = rgt[2] / rad; m[12] = -cx / rad;
			m[1] = upl[0] / rad; m[5] = upl[1] / rad; m[9] = upl[2] / rad; m[13] = -cy / rad;
			m[2] = L[0] / zr; m[6] = L[1] / zr; m[10] = L[2] / zr; m[14] = -(cz - rad - back) / zr;
			m[15] = 1.0f;
			c.cascadeSplits[ci] = d1;
			if (ci == 0) {
				c.shadowParams[0] = 1.0f / R.shadowRes;
				c.shadowParams[1] = 0.0004f;
				c.shadowParams[2] = texel * 1.2f;
			}
		}
		c.shadowParams[3] = 1.0f;
	}
	UploadAlloc a = Upload(sizeof(ViewConstants), 256);
	memcpy(a.cpu, &c, sizeof(c));
	R.viewCB = a.gpu;
}

void R_AppendDeferred(const DeferredDraw& d)
{
	if (!R.inView || !d.numIndexes || !d.numVerts) return;
	UploadAlloc vb = Upload((uint64_t)d.numVerts * sizeof(LVert), 16);
	UploadAlloc ib = Upload((uint64_t)d.numIndexes * 4, 4);
	UploadAlloc cb = Upload(256, 256);
	if (!vb.cpu || !ib.cpu || !cb.cpu) return;
	LVert* v = (LVert*)vb.cpu;
	for (int i = 0; i < d.numVerts; i++) {
		const float* p = (const float*)((const uint8_t*)d.xyz + (size_t)i * d.xyzStride);
		const float* n = (const float*)((const uint8_t*)d.normal + (size_t)i * d.normalStride);
		LVert& o = v[i];
		o.pos[0] = p[0]; o.pos[1] = p[1]; o.pos[2] = p[2];
		o.normal[0] = n[0]; o.normal[1] = n[1]; o.normal[2] = n[2];
		o.uv[0] = d.uv[i * 2]; o.uv[1] = d.uv[i * 2 + 1];
		if (d.lmuv) { const float* l = (const float*)((const uint8_t*)d.lmuv + (size_t)i * d.lmuvStride); o.lmuv[0] = l[0]; o.lmuv[1] = l[1]; }
		else o.lmuv[0] = o.lmuv[1] = 0;
		o.color = d.colors ? (d.colors[i * 4] | (d.colors[i * 4 + 1] << 8) | (d.colors[i * 4 + 2] << 16) | ((uint32_t)d.colors[i * 4 + 3] << 24)) : 0xffffffffu;
	}
	memcpy(ib.cpu, d.indexes, (size_t)d.numIndexes * 4);
	// per-draw constants (b2): mvp, object rows, textures, material
	uint8_t* c = (uint8_t*)cb.cpu;
	memcpy(c, d.mvp, 64);
	memcpy(c + 64, d.objToWorld, 48);
	GLTexture* ta = d.albedoTex ? R_GLTexture(d.albedoTex, false) : nullptr;
	GLTexture* tl = d.lightmapTex ? R_GLTexture(d.lightmapTex, false) : nullptr;
	GLTexture* ts = d.specTex ? R_GLTexture(d.specTex, false) : nullptr;
	R_ResolveTexture(ta); R_ResolveTexture(tl); R_ResolveTexture(ts);
	uint32_t u[4] = { R_TextureSrv(ta, true), R_TextureSrv(tl, true), d.flags, 0 };
	memcpy(&u[3], &d.alphaRef, 4);
	memcpy(c + 112, u, 16);
	float f[4] = { d.roughness, d.metallic, d.bakedScale, 0 };
	uint32_t spec = R_TextureSrv(ts, false);
	memcpy(c + 128, f, 12);
	memcpy(c + 140, &spec, 4);
	memcpy(c + 144, d.tint, 16);
	{
		GLTexture* tn = d.normalTex ? R_GLTexture(d.normalTex, false) : nullptr;
		GLTexture* tr2 = d.rmoTex ? R_GLTexture(d.rmoTex, false) : nullptr;
		R_ResolveTexture(tn); R_ResolveTexture(tr2);
		uint32_t pbr[4] = { R_TextureSrv(tn, false), R_TextureSrv(tr2, false), (uint32_t)d.rmoOrder, 0 };
		memcpy(&pbr[3], &d.skin, 4);
		memcpy(c + 160, pbr, 16);
		float grid[12] = { d.gridAmbient[0], d.gridAmbient[1], d.gridAmbient[2], kt_bf3Rim ? kt_bf3Rim->value : 0.0f, d.gridDirected[0], d.gridDirected[1], d.gridDirected[2], kt_bf3Spec ? kt_bf3Spec->value : 0.0f,
			d.gridDir[0], d.gridDir[1], d.gridDir[2], kt_bf3Refl ? kt_bf3Refl->value : 0.0f };
		memcpy(c + 176, grid, 48);
		GLTexture* tdx = d.deluxeTex ? R_GLTexture(d.deluxeTex, false) : nullptr;
		R_ResolveTexture(tdx);
		float px = (kt_parallax && kt_parallax->integer && d.normalTex) ? d.parallax : 0.0f;
		uint32_t ex[4] = { R_TextureSrv(tdx, false), 0, 0, 0 };
		memcpy(&ex[1], &px, 4);
		memcpy(c + 224, ex, 16);
	}
	DeferredRec r;
	r.vb = vb.gpu; r.ib = ib.gpu; r.cb = cb.gpu;
	r.vbSize = d.numVerts * sizeof(LVert);
	r.indexCount = d.numIndexes;
	r.cull = d.cull;
	r.alpha = (d.flags & 1) != 0;
	r.castShadow = d.castShadow;
	R.deferred.push_back(r);
}

static void DrawDeferredList(ID3D12GraphicsCommandList* cl, bool shadow, int cascade)
{
	cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	uint32_t rc[2] = { (uint32_t)cascade, shadow ? 1u : 0u };
	cl->SetGraphicsRoot32BitConstants(RP_CONSTANTS, 2, rc, 0);
	ID3D12PipelineState* last = nullptr;
	for (const DeferredRec& r : R.deferred) {
		if (shadow && !r.castShadow) continue;
		ID3D12PipelineState* p = shadow ? (r.alpha ? R.lgShadowAlphaPSO : R.lgShadowPSO) : R.lgPSO[r.cull];
		if (!p) continue;
		if (p != last) { cl->SetPipelineState(p); last = p; }
		cl->SetGraphicsRootConstantBufferView(RP_EXTRA_CBV, r.cb);
		D3D12_VERTEX_BUFFER_VIEW vbv = { r.vb, r.vbSize, sizeof(LVert) };
		D3D12_INDEX_BUFFER_VIEW ibv = { r.ib, r.indexCount * 4, DXGI_FORMAT_R32_UINT };
		cl->IASetVertexBuffers(0, 1, &vbv);
		cl->IASetIndexBuffer(&ibv);
		cl->DrawIndexedInstanced(r.indexCount, 1, 0, 0, 0);
	}
}

static void Dispatch(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* p, const uint32_t* consts, int n, uint32_t gx, uint32_t gy)
{
	if (!p) return;
	cl->SetPipelineState(p);
	cl->SetComputeRoot32BitConstants(RP_CONSTANTS, n, consts, 0);
	cl->Dispatch(gx, gy, 1);
}

static void FullscreenPass(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* p, Texture& rt, const uint32_t* consts, int n)
{
	cl->OMSetRenderTargets(1, &rt.rtv[0], FALSE, nullptr);
	D3D12_VIEWPORT vp = { R.vc.viewport[0], R.vc.viewport[1], R.vc.viewport[2], R.vc.viewport[3], 0, 1 };
	D3D12_RECT rc = { (LONG)vp.TopLeftX, (LONG)vp.TopLeftY, (LONG)(vp.TopLeftX + vp.Width), (LONG)(vp.TopLeftY + vp.Height) };
	cl->RSSetViewports(1, &vp);
	cl->RSSetScissorRects(1, &rc);
	cl->SetPipelineState(p);
	cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cl->SetGraphicsRoot32BitConstants(RP_CONSTANTS, n, consts, 0);
	cl->DrawInstanced(3, 1, 0, 0);
}

// old BSP maps: the static world into one sun cascade (the legacy shadow pipeline, an identity object matrix)
static void DrawBspShadow(ID3D12GraphicsCommandList* cl, int cascade)
{
	if (!R.bspIndexCount || !R.lgShadowPSO) return;
	cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	uint32_t rc[2] = { (uint32_t)cascade, 1u };
	cl->SetGraphicsRoot32BitConstants(RP_CONSTANTS, 2, rc, 0);
	cl->SetPipelineState(R.lgShadowPSO);
	cl->SetGraphicsRootConstantBufferView(RP_EXTRA_CBV, R.bspCB.res->GetGPUVirtualAddress());
	D3D12_VERTEX_BUFFER_VIEW vbv = { R.bspVB.res->GetGPUVirtualAddress(), R.bspVertCount * (UINT)sizeof(LVert), sizeof(LVert) };
	D3D12_INDEX_BUFFER_VIEW ibv = { R.bspIB.res->GetGPUVirtualAddress(), R.bspIndexCount * 4, DXGI_FORMAT_R32_UINT };
	cl->IASetVertexBuffers(0, 1, &vbv);
	cl->IASetIndexBuffer(&ibv);
	cl->DrawIndexedInstanced(R.bspIndexCount, 1, 0, 0, 0);
	cl->SetGraphicsRootConstantBufferView(RP_EXTRA_CBV, R.viewCB);
}

bool R_BspShadowMesh() { return R.bspIndexCount > 0; }

void R_SetBspShadowMesh(const float* pos, uint32_t numVerts, const uint32_t* idx, uint32_t numIdx)
{
	if (R.bspVB.res) DestroyBuffer(R.bspVB);
	if (R.bspIB.res) DestroyBuffer(R.bspIB);
	R.bspIndexCount = R.bspVertCount = 0;
	if (!R.ready || !numVerts || !numIdx) return;
	std::vector<LVert> v(numVerts);
	for (uint32_t i = 0; i < numVerts; i++) {
		memset(&v[i], 0, sizeof(LVert));
		v[i].pos[0] = pos[i * 3]; v[i].pos[1] = pos[i * 3 + 1]; v[i].pos[2] = pos[i * 3 + 2];
		v[i].normal[2] = 1.0f;
	}
	if (!CreateBuffer(R.bspVB, (uint64_t)numVerts * sizeof(LVert), false, "bsp shadow vb") || !CreateBuffer(R.bspIB, (uint64_t)numIdx * 4, false, "bsp shadow ib"))
		return;
	UploadBuffer(R.bspVB, 0, v.data(), (uint64_t)numVerts * sizeof(LVert));
	UploadBuffer(R.bspIB, 0, idx, (uint64_t)numIdx * 4);
	if (!R.bspCB.res && CreateBuffer(R.bspCB, 256, false, "bsp shadow cb")) {
		float cb[64] = {};
		cb[16] = 1; cb[21] = 1; cb[26] = 1;   // objRow0..2 = identity (after the 4x4 gMvp)
		UploadBuffer(R.bspCB, 0, cb, 256);
	}
	R.bspVertCount = numVerts;
	R.bspIndexCount = numIdx;
	Log("KatarnEngine: sun shadow mesh %u triangles\n", numIdx / 3);
}

static void ProbeCaptureFace(ID3D12GraphicsCommandList* cl);
static void RenderWorldView()
{
	ID3D12GraphicsCommandList* cl = R.cl;
	const ViewDesc& v = R.view;
	const ViewConstants& c = R.vc;
	bool hd = World_Active();
	D3D12_RECT rect = { (LONG)c.viewport[0], (LONG)c.viewport[1], (LONG)(c.viewport[0] + c.viewport[2]), (LONG)(c.viewport[1] + c.viewport[3]) };
	D3D12_VIEWPORT vp = { c.viewport[0], c.viewport[1], c.viewport[2], c.viewport[3], 0, 1 };
	uint32_t gx = ((uint32_t)v.w + 7) / 8, gy = ((uint32_t)v.h + 7) / 8;

	cl->SetGraphicsRootConstantBufferView(RP_VIEW_CBV, R.viewCB);
	cl->SetComputeRootSignature(g.rootSig);
	BindRoot(cl, true);
	BindRoot(cl, false);
	cl->SetGraphicsRootConstantBufferView(RP_VIEW_CBV, R.viewCB);
	cl->SetGraphicsRootConstantBufferView(RP_EXTRA_CBV, R.viewCB);
	cl->SetComputeRootConstantBufferView(RP_VIEW_CBV, R.viewCB);
	cl->SetComputeRootConstantBufferView(RP_EXTRA_CBV, R.viewCB);

	// ---- GBuffer
	Transition(cl, R.gb0, D3D12_RESOURCE_STATE_RENDER_TARGET);
	Transition(cl, R.gb1, D3D12_RESOURCE_STATE_RENDER_TARGET);
	Transition(cl, R.gb2, D3D12_RESOURCE_STATE_RENDER_TARGET);
	Transition(cl, R.depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	const float zero4[4] = { 0, 0, 0, 0 };
	cl->ClearRenderTargetView(R.gb0.rtv[0], zero4, 1, &rect);
	cl->ClearRenderTargetView(R.gb1.rtv[0], zero4, 1, &rect);
	cl->ClearRenderTargetView(R.gb2.rtv[0], zero4, 1, &rect);
	cl->ClearDepthStencilView(R.depth.dsv[0], D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 1, &rect);
	D3D12_CPU_DESCRIPTOR_HANDLE gbs[3] = { R.gb0.rtv[0], R.gb1.rtv[0], R.gb2.rtv[0] };
	cl->OMSetRenderTargets(3, gbs, FALSE, &R.depth.dsv[0]);
	cl->RSSetViewports(1, &vp);
	cl->RSSetScissorRects(1, &rect);
	if (hd) {
		WorldView wv;
		wv.viewProj = R.viewProj;
		wv.cam[0] = v.origin[0]; wv.cam[1] = v.origin[1]; wv.cam[2] = v.origin[2];
		wv.pixelScale = v.h * 0.5f * c.projParams[1];
		wv.lodError = 1.0f;
		World_DrawGBuffer(cl, wv);
	}
	if (!R.deferred.empty()) DrawDeferredList(cl, false, 0);

	// ---- sun shadows
	bool shadows = c.shadowParams[3] > 0;
	if (shadows) {
		Transition(cl, R.shadow, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		D3D12_VIEWPORT svp = { 0, 0, (float)R.shadowRes, (float)R.shadowRes, 0, 1 };
		D3D12_RECT src = { 0, 0, (LONG)R.shadowRes, (LONG)R.shadowRes };
		for (int ci = 0; ci < 4; ci++) {
			cl->ClearDepthStencilView(R.shadow.dsv[ci], D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
			cl->OMSetRenderTargets(0, nullptr, FALSE, &R.shadow.dsv[ci]);
			cl->RSSetViewports(1, &svp);
			cl->RSSetScissorRects(1, &src);
			if (R.capture && ci < 3) continue;   // probe capture: only the last cascade is used
			World_DrawShadow(cl, ci, c.cascadeViewProj[ci], R.cascadeTexel[ci]);
			if (!hd && c.bspSun[0] > 0) DrawBspShadow(cl, ci);
			if (!R.deferred.empty()) DrawDeferredList(cl, true, ci);
		}
	}
	Transition(cl, R.shadow, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

	// ---- GBuffer -> readable
	D3D12_RESOURCE_STATES srv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	Transition(cl, R.gb0, srv); Transition(cl, R.gb1, srv); Transition(cl, R.gb2, srv);
	Transition(cl, R.depth, D3D12_RESOURCE_STATE_DEPTH_READ | srv);

	// ---- sky LUT + SH
	{
		Transition(cl, R.skyLut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		Transition(cl, R.sh, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		float ga = 0.3f;
		uint32_t k[8] = { R.skyLut.uav[0], R.skyLut.srv, R.sh.uav, 128, 64, 0, 0, 0 };
		memcpy(&k[5], &ga, 4);
		if (!c.indices2[0]) Dispatch(cl, R.csSkyLut, k, 8, 16, 8);
		Transition(cl, R.skyLut, srv);
		if (!c.indices2[1]) Dispatch(cl, R.csSkySh, k, 8, 1, 1);
		Transition(cl, R.sh, srv);
	}

	// ---- AO
	bool ao = kt_ssao->integer && R.csAo && R.csAoBlur;
	if (ao) {
		Transition(cl, R.ao, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		float rad = kt_aoRadius->value, inten = kt_aoIntensity->value;
		uint32_t k[8] = { R.ao.uav[0], 0, 0, 0, 0, 0, 0, 0 };
		memcpy(&k[2], &rad, 4); memcpy(&k[3], &inten, 4);
		Dispatch(cl, R.csAo, k, 8, gx, gy);
		Transition(cl, R.ao, srv);
		Transition(cl, R.aoTmp, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		uint32_t b1[8] = { R.aoTmp.uav[0], R.ao.srv, 0, 0, 1, 0, 0, 0 };
		Dispatch(cl, R.csAoBlur, b1, 8, gx, gy);
		Transition(cl, R.aoTmp, srv);
		Transition(cl, R.ao, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		uint32_t b2[8] = { R.ao.uav[0], R.aoTmp.srv, 0, 0, 0, 1, 0, 0 };
		Dispatch(cl, R.csAoBlur, b2, 8, gx, gy);
		Transition(cl, R.ao, srv);
	}

	// ---- lights (HD world lights + game dynamic lights)
	std::vector<GpuLight> lights;
	if (kt_lights->integer) {
		World_AppendLights(lights, v.origin);
		for (int i = 0; i < backEnd.refdef.num_dlights && lights.size() < 1024; i++) {
			const dlight_t& dl = backEnd.refdef.dlights[i];
			GpuLight l = {};
			l.pos[0] = dl.origin[0]; l.pos[1] = dl.origin[1]; l.pos[2] = dl.origin[2];
			l.radius = dl.radius;
			float s = kt_dlightScale->value * (hd ? 4.0f : 1.0f);
			l.color[0] = dl.color[0] * s; l.color[1] = dl.color[1] * s; l.color[2] = dl.color[2] * s;
			l.type = 0;
			l.invSquare = 0;
			lights.push_back(l);
		}
	}
	// r19: glowing map surfaces as lights (the nearest 48 within 3000 units)
	if (kt_lights->integer && kt_glowLights->value > 0 && !s_glowLights.empty() && !hd) {
		const float* o = v.origin;
		std::vector<std::pair<float, int>> near_;
		for (int i = 0; i < (int)s_glowLights.size() / 7; i++) {
			const float* g = &s_glowLights[i * 7];
			float d2 = (g[0] - o[0]) * (g[0] - o[0]) + (g[1] - o[1]) * (g[1] - o[1]) + (g[2] - o[2]) * (g[2] - o[2]);
			if (d2 < 9000.0f * 9000.0f) near_.push_back({ d2, i });   // r25: the whole level's glow lights, always on
		}
		std::sort(near_.begin(), near_.end());
		for (size_t k = 0; k < near_.size() && k < 256 && lights.size() < 1024; k++) {
			const float* g = &s_glowLights[near_[k].second * 7];
			GpuLight l = {};
			l.pos[0] = g[0]; l.pos[1] = g[1]; l.pos[2] = g[2];
			// r24: fade in with distance (1800..3000) and out at the end of the list, so lights never pop on as you walk up
			const float fade = 1.0f;   // r25: no distance fade - the lights are part of the level from the moment it loads
			for (int q = 0; q < 3; q++) l.color[q] = g[3 + q] * kt_glowLights->value * fade;
			l.radius = Q_min(g[6], 300.0f);
			l.pad[1] = 1.0f;   // lighting.hlsl Light.lp.y: no volumetric fog from these
			l.type = 0; l.invSquare = 0;
			lights.push_back(l);
		}
	}
	if (kt_lights->integer && kt_saberLight->value > 0)
		for (const GpuLight& t : R.tubes) if (lights.size() < 1024) {
			GpuLight l = t;
			for (int q = 0; q < 3; q++) l.color[q] *= kt_saberLight->value;
			lights.push_back(l);
		}
	// nearest first: the screen-space shadows go to the closest lights, and the fog only takes the first few
	{
		const float* o = v.origin;
		auto key = [o](const GpuLight& l) {
			float p[3] = { l.pos[0], l.pos[1], l.pos[2] };
			if (l.type == 2.0f) for (int q = 0; q < 3; q++) p[q] += l.dir[q] * 0.5f;
			float d = sqrtf((p[0] - o[0]) * (p[0] - o[0]) + (p[1] - o[1]) * (p[1] - o[1]) + (p[2] - o[2]) * (p[2] - o[2]));
			return d - l.radius * 0.5f;
		};
		std::stable_sort(lights.begin(), lights.end(), [&](const GpuLight& a, const GpuLight& b) { return key(a) < key(b); });
	}
	if (s_frOk) {
		size_t w = 0;
		for (size_t i = 0; i < lights.size(); i++) {
			const GpuLight& l = lights[i];
			float c[3] = { l.pos[0], l.pos[1], l.pos[2] }, r = l.radius;
			if (l.type == 2.0f) { for (int q = 0; q < 3; q++) c[q] += l.dir[q] * 0.5f; r += 0.5f * sqrtf(l.dir[0] * l.dir[0] + l.dir[1] * l.dir[1] + l.dir[2] * l.dir[2]); }
			if (SphereVisible(c, r)) lights[w++] = l;
		}
		lights.resize(w);
	}
	uint32_t nl = (uint32_t)lights.size();
	if (!nl) { GpuLight dummy = {}; lights.push_back(dummy); }
	UploadAlloc la = Upload(lights.size() * sizeof(GpuLight), 256);
	memcpy(la.cpu, lights.data(), lights.size() * sizeof(GpuLight));
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
		sd.Format = DXGI_FORMAT_R32_TYPELESS;
		sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		sd.Buffer.FirstElement = la.offset / 4;
		sd.Buffer.NumElements = (UINT)(lights.size() * sizeof(GpuLight) / 4);
		sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
		g.dev->CreateShaderResourceView(la.res, &sd, CpuHandle(R.lightSrv[g.frameIndex]));
	}
	// exposure buffer must be readable by the lighting debug views and the forward pass
	Transition(cl, R.exposure, srv);

	// ---- bounced light (HD worlds): rays over the depth buffer into last frame's lit scene, half resolution
	uint32_t giSrv = 0;
	if (hd && kt_gi->integer && R.hdrPrevValid && !R.capture && !v.skyPortal && R.csGiTrace && R.csGiTemporal && R.csGiSpatial) {
		uint32_t hw = Q_min((uint32_t)(v.w + 1) / 2, (uint32_t)R.gi.width), hh = Q_min((uint32_t)(v.h + 1) / 2, (uint32_t)R.gi.height);
		uint32_t hgx = (hw + 7) / 8, hgy = (hh + 7) / 8;
		float rad = Q_max(64.0f, kt_giRadius->value);
		Transition(cl, R.hdrPrev, srv);
		Transition(cl, R.giRaw, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		uint32_t kt[8] = { R.hdrPrev.srv, R.giRaw.uav[0], 0, hw, hh, R.giFrame, 0, 0 };
		memcpy(&kt[7], &rad, 4);
		Dispatch(cl, R.csGiTrace, kt, 8, hgx, hgy);
		Transition(cl, R.giRaw, srv);
		Texture& gh = R.giAcc[R.giIdx];
		Texture& go = R.giAcc[R.giIdx ^ 1];
		Transition(cl, gh, srv);
		Transition(cl, go, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		uint32_t kte[8] = { R.giRaw.srv, go.uav[0], gh.srv, hw, hh, R.giFrame, R.giValid ? 1u : 0u, 0 };
		Dispatch(cl, R.csGiTemporal, kte, 8, hgx, hgy);
		Transition(cl, go, srv);
		Transition(cl, R.gi, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		uint32_t ks[8] = { go.srv, R.gi.uav[0], 0, hw, hh, R.giFrame, 0, 0 };
		Dispatch(cl, R.csGiSpatial, ks, 8, hgx, hgy);
		Transition(cl, R.gi, srv);
		R.giIdx ^= 1; R.giValid = true; R.giFrame++;
		giSrv = R.gi.srv;
	}
	else if (!R.capture && !v.skyPortal) R.giValid = false;

	// ---- lighting
	Transition(cl, R.hdr, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	{
		uint32_t k[9] = { R.hdr.uav[0], R.sh.srv, R.lightSrv[g.frameIndex], nl, R.ao.srv,
			(ao ? 1u : 0u) | (shadows ? 2u : 0u) | ((kt_volumetric->integer && !c.vfogIdx[0]) ? 4u : 0u) | (kt_contactShadows->integer ? 8u : 0u) | (kt_lightShadows->integer ? 16u : 0u), (uint32_t)Q_max(0, kt_debugView->integer),
			(kt_ssr->integer && R.hdrPrevValid && !R.capture) ? R.hdrPrev.srv : 0u, giSrv };
		Dispatch(cl, R.csLighting, k, 9, gx, gy);
	}
	// ---- skin: subsurface blur of the lit skin (two passes through a temporary)
	if (kt_sss->value > 0 && R.csSss && !R.capture) {
		float wdt = kt_sss->value;
		Transition(cl, R.hdr, srv);
		Transition(cl, R.sssTmp, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		uint32_t k1[4] = { R.hdr.srv, R.sssTmp.uav[0], 0, 0 };
		memcpy(&k1[3], &wdt, 4);
		Dispatch(cl, R.csSss, k1, 4, gx, gy);
		Transition(cl, R.sssTmp, srv);
		Transition(cl, R.hdr, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		uint32_t k2[4] = { R.sssTmp.srv, R.hdr.uav[0], 1, 0 };
		memcpy(&k2[3], &wdt, 4);
		Dispatch(cl, R.csSss, k2, 4, gx, gy);
		UavBarrier(cl, R.hdr.res);
	}
	// ---- volumetric fog: light the froxels and integrate them (applied after the forward pass)
	if (c.vfogIdx[0]) {
		Transition(cl, R.froxIn, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		uint32_t k[8] = { R.froxIn.uav[0], R.sh.srv, R.lightSrv[g.frameIndex], Q_min(nl, 16u), 0, (shadows ? 2u : 0u), 0, 0 };
		Dispatch(cl, R.csFogInject, k, 8, (R.froxIn.width + 7) / 8, (R.froxIn.height + 7) / 8);
		Transition(cl, R.froxIn, srv);
		Transition(cl, R.froxOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		uint32_t ki[4] = { R.froxIn.srv, R.froxOut.uav[0], R.froxIn.width | (R.froxIn.height << 16), R.froxIn.layers };
		Dispatch(cl, R.csFogIntegrate, ki, 4, (R.froxIn.width + 7) / 8, (R.froxIn.height + 7) / 8);
		Transition(cl, R.froxOut, srv);
	}

	// ---- water / flowing water (reads the scene depth, blends into the lit scene)
	Transition(cl, R.hdr, D3D12_RESOURCE_STATE_RENDER_TARGET);
	if (hd) {
		cl->OMSetRenderTargets(1, &R.hdr.rtv[0], FALSE, nullptr);
		cl->RSSetViewports(1, &vp);
		cl->RSSetScissorRects(1, &rect);
		cl->SetGraphicsRootConstantBufferView(RP_VIEW_CBV, R.viewCB);
		WorldView wv;
		wv.viewProj = R.viewProj;
		wv.cam[0] = v.origin[0]; wv.cam[1] = v.origin[1]; wv.cam[2] = v.origin[2];
		wv.pixelScale = v.h * 0.5f * c.projParams[1];
		wv.lodError = 1.0f;
		World_DrawFX(cl, wv);
	}

	// ---- forward legacy content into the HDR scene
	Transition(cl, R.depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	{
		const float zero4g[4] = { 0, 0, 0, 0 };
		Transition(cl, R.glow, D3D12_RESOURCE_STATE_RENDER_TARGET);
		cl->ClearRenderTargetView(R.glow.rtv[0], zero4g, 0, nullptr);
		Transition(cl, R.overlay, D3D12_RESOURCE_STATE_RENDER_TARGET);
		cl->ClearRenderTargetView(R.overlay.rtv[0], zero4g, 0, nullptr);
		Transition(cl, R.saberGlow, D3D12_RESOURCE_STATE_RENDER_TARGET);
		cl->ClearRenderTargetView(R.saberGlow.rtv[0], zero4g, 0, nullptr);
	}
	FlushStream(1);
	Transition(cl, R.glow, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	Transition(cl, R.overlay, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	Transition(cl, R.saberGlow, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

	// ---- camera: exposure
	D3D12_RESOURCE_STATES srvAll = srv;
	// lightsaber blades (after the legacy content, so the depth holds characters and BSP surfaces)
	if (!R.blades.empty() && R.bladePSO) {
		Transition(cl, R.depth, D3D12_RESOURCE_STATE_DEPTH_READ | srv);
		Transition(cl, R.hdr, D3D12_RESOURCE_STATE_RENDER_TARGET);
		cl->OMSetRenderTargets(1, &R.hdr.rtv[0], FALSE, nullptr);
		cl->RSSetViewports(1, &vp);
		cl->RSSetScissorRects(1, &rect);
		cl->SetGraphicsRootConstantBufferView(RP_VIEW_CBV, R.viewCB);
		cl->SetPipelineState(R.bladePSO);
		cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		// first a soft halo into the HDR scene (bloom, and the dark core of a darksaber), then the blade itself into
		// the display overlay: it skips the tone curve, so the edge keeps its colour and the core stays white
		for (int tgt = 0; tgt < 2; tgt++) {
			if (tgt == 1) {
				Transition(cl, R.overlay, D3D12_RESOURCE_STATE_RENDER_TARGET);
				cl->OMSetRenderTargets(1, &R.overlay.rtv[0], FALSE, nullptr);
			}
			for (int pass = 0; pass < 2; pass++)            // glows, then cores on top
				for (const auto& bl0 : R.blades) {
					if ((int)bl0.mode != pass) continue;
					RState::Blade bl = bl0;
					GLTexture* t = bl.tex ? R_GLTexture(bl.tex, false) : nullptr;
					R_ResolveTexture(t);
					bl.tex = t ? R_TextureSrv(t, true) : 0;
					bl.pad = (uint32_t)tgt;
					cl->SetGraphicsRoot32BitConstants(RP_CONSTANTS, 20, &bl, 0);
					cl->DrawInstanced(6, 1, 0, 0);
				}
		}
		Transition(cl, R.overlay, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	}
	// ---- volumetric fog over everything drawn so far (before TAA, which smooths its noise)
	if (c.vfogIdx[0]) {
		Transition(cl, R.depth, D3D12_RESOURCE_STATE_DEPTH_READ | srv);
		Transition(cl, R.hdr, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		uint32_t k[4] = { R.froxOut.srv, R.hdr.uav[0], R.froxIn.width | (R.froxIn.height << 16), R.froxIn.layers };
		Dispatch(cl, R.csFogApply, k, 4, gx, gy);
		UavBarrier(cl, R.hdr.res);
	}
	// ---- TAA resolve (history clipped to this frame's neighbourhood), written back into the HDR scene
	if (R.taaThisView) {
		Transition(cl, R.depth, D3D12_RESOURCE_STATE_DEPTH_READ | srv);
		Texture& hist = R.taa[R.taaIdx];
		Texture& out = R.taa[R.taaIdx ^ 1];
		Transition(cl, R.hdr, srvAll);
		Transition(cl, hist, srvAll);
		Transition(cl, out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		float fmin = 0.08f, fmax = 0.3f;
		uint32_t k[8] = { R.hdr.srv, hist.srv, out.uav[0], R.depth.srv, R.taaValid ? 1u : 0u, 0, 0, 0 };
		memcpy(&k[5], &fmin, 4); memcpy(&k[6], &fmax, 4);
		Dispatch(cl, R.csTaa, k, 8, gx, gy);
		Transition(cl, out, D3D12_RESOURCE_STATE_COPY_SOURCE);
		Transition(cl, R.hdr, D3D12_RESOURCE_STATE_COPY_DEST);
		cl->CopyResource(R.hdr.res, out.res);
		Transition(cl, out, srvAll);
		R.taaIdx ^= 1;
		R.taaValid = true;
		memcpy(R.lastVpNoJitter, R.vpNoJitter, 64);
	}
	// keep this frame's lit scene for next frame's screen-space reflections
	if (R.capture) {
		// probe capture: the lit HDR view goes into the probe's cube face; no camera effects
		ProbeCaptureFace(cl);
		cl->SetGraphicsRootConstantBufferView(RP_VIEW_CBV, R.defaultCB);
		return;
	}
	if (kt_ssr->integer || (hd && kt_gi->integer)) {   // also the light the bounced-light rays pick up
		Transition(cl, R.hdr, D3D12_RESOURCE_STATE_COPY_SOURCE);
		Transition(cl, R.hdrPrev, D3D12_RESOURCE_STATE_COPY_DEST);
		cl->CopyResource(R.hdrPrev.res, R.hdr.res);
		Transition(cl, R.hdrPrev, srvAll);
		R.hdrPrevValid = true;
	}
	Transition(cl, R.hdr, srvAll);
	Transition(cl, R.exposure, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	{
		uint32_t k[8] = { R.hdr.srv, 0, R.exposure.uav, 0, 0, 0, 0, 0 };
		memcpy(&k[6], &R.dt, 4);
		Dispatch(cl, R.csHist, k, 8, ((uint32_t)v.w / 2 + 15) / 16, ((uint32_t)v.h / 2 + 15) / 16);
		UavBarrier(cl, R.exposure.res);
		Dispatch(cl, R.csExposure, k, 8, 1, 1);
	}
	bool dof = kt_dof->integer && R.csFocus && R.csDof && !v.skyPortal;
	bool mblur = kt_motionBlur->value > 0 && R.csMotionBlur && !v.skyPortal && R.taaThisView;
	float camK[4] = { kt_dofFocus->value, Q_max(0.0f, kt_dofAperture->value) * 6.0f, Q_max(0.0f, Q_min(1.0f, kt_motionBlur->value)), R.dt };
	if (dof && kt_dofFocus->value <= 0) {
		Transition(cl, R.depth, D3D12_RESOURCE_STATE_DEPTH_READ | srv);
		UavBarrier(cl, R.exposure.res);
		uint32_t k[8] = { 0, 0, R.exposure.uav, 1, 0, 0, 0, 0 };
		memcpy(&k[4], camK, 16);
		Dispatch(cl, R.csFocus, k, 8, 1, 1);
	}
	Transition(cl, R.exposure, srvAll);
	// ---- camera: motion blur, then depth of field (each into a temporary and back)
	for (int pass = 0; pass < 2; pass++) {
		if (pass == 0 ? !mblur : !dof) continue;
		Transition(cl, R.depth, D3D12_RESOURCE_STATE_DEPTH_READ | srv);
		Transition(cl, R.hdr, srvAll);
		Transition(cl, R.sssTmp, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		uint32_t k[8] = { R.hdr.srv, R.sssTmp.uav[0], 0, kt_dofFocus->value <= 0 ? 1u : 0u, 0, 0, 0, 0 };
		memcpy(&k[4], camK, 16);
		Dispatch(cl, pass == 0 ? R.csMotionBlur : R.csDof, k, 8, gx, gy);
		Transition(cl, R.sssTmp, D3D12_RESOURCE_STATE_COPY_SOURCE);
		Transition(cl, R.hdr, D3D12_RESOURCE_STATE_COPY_DEST);
		cl->CopyResource(R.hdr.res, R.sssTmp.res);
		Transition(cl, R.hdr, srvAll);
	}

	// ---- bloom
	uint32_t bloomSrv = 0;
	if (kt_bloom->value > 0 && R.bloomLevels >= 2 && R.csBloomDown && R.csBloomUp) {
		uint32_t src = R.hdr.srv;
		for (int i = 0; i < R.bloomLevels; i++) {
			Texture& d = R.bloomDown[i];
			Transition(cl, d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			uint32_t dw = i == 0 ? Q_max(1, v.w / 2) : d.width, dh = i == 0 ? Q_max(1, v.h / 2) : d.height;
			if (i > 0) { dw = Q_max(1u, (uint32_t)(R.bloomDown[i - 1].width / 2)); dh = Q_max(1u, (uint32_t)(R.bloomDown[i - 1].height / 2)); }
			dw = Q_min(dw, d.width); dh = Q_min(dh, d.height);
			uint32_t k[8] = { src, d.uav[0], 0, 0, d.width, d.height, 0, i == 0 ? 0u : 1u };
			if (i == 0) { k[2] = R.glow.srv; float gs = kt_dynGlow->value / (Q_max(1e-3f, kt_bloom->value) * (float)R.bloomLevels); memcpy(&k[6], &gs, 4); }
			Dispatch(cl, R.csBloomDown, k, 8, (d.width + 7) / 8, (d.height + 7) / 8);
			Transition(cl, d, srvAll);
			src = d.srv;
		}
		// up: up[n-1] = down[n-1]; up[i] = down[i] + tent(up[i+1])
		uint32_t prev = R.bloomDown[R.bloomLevels - 1].srv;
		for (int i = R.bloomLevels - 2; i >= 0; i--) {
			Texture& u = R.bloomUp[i];
			Transition(cl, u, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			uint32_t k[8] = { R.bloomDown[i].srv, u.uav[0], 0, prev, u.width, u.height, 0, 0 };
			Dispatch(cl, R.csBloomUp, k, 8, (u.width + 7) / 8, (u.height + 7) / 8);
			Transition(cl, u, srvAll);
			prev = u.srv;
		}
		bloomSrv = prev;
	}

	// ---- rend2 dynamic glow for the saber glow stages: the glow buffer downsampled into glowScaled[0..4] (CoD:AW
	// 13-tap filter, glowScaled[0] is full size), then upsampled back level by level; the tone map pass screen-blends
	// glowScaled[0] x r_dynamicGlowIntensity (1.13) over the picture, exactly as rend2's RB_PostProcess does
	uint32_t saberGlowSrv = 0;
	if (R.csDGlowDown && R.csDGlowUp && kt_dynGlow->value > 0) {
		uint32_t src = R.saberGlow.srv;
		for (int i = 0; i < 5; i++) {
			Texture& d = R.saberGlowLvl[i];
			Transition(cl, d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			uint32_t k[8] = { src, d.uav[0], 0, 0, d.width, d.height, 0, 0 };
			Dispatch(cl, R.csDGlowDown, k, 8, (d.width + 7) / 8, (d.height + 7) / 8);
			Transition(cl, d, srvAll);
			src = d.srv;
		}
		for (int i = 3; i >= 0; i--) {
			Texture& d = R.saberGlowLvl[i];
			Transition(cl, d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			uint32_t k[8] = { R.saberGlowLvl[i + 1].srv, d.uav[0], 0, 0, d.width, d.height, 0, 0 };
			Dispatch(cl, R.csDGlowUp, k, 8, (d.width + 7) / 8, (d.height + 7) / 8);
			Transition(cl, d, srvAll);
		}
		saberGlowSrv = R.saberGlowLvl[0].srv;
	}

	// ---- tone map (+ FXAA) into the screen
	cl->SetGraphicsRootConstantBufferView(RP_VIEW_CBV, R.viewCB);
	bool fxaa = kt_fxaa->integer && R.fxaaPSO && !R.taaThisView;   // TAA does the anti-aliasing
	Texture& tmTarget = fxaa ? R.ldr : R.screen;
	Transition(cl, R.depth, D3D12_RESOURCE_STATE_DEPTH_READ | srv);
	Transition(cl, tmTarget, D3D12_RESOURCE_STATE_RENDER_TARGET);
	{
		uint32_t k[8] = { R.hdr.srv, R.overlay.srv, saberGlowSrv, bloomSrv, R.w, R.h, 0, 0 };
		float gi = 1.13f * Q_max(0.0f, kt_dynGlow->value);   // r_dynamicGlowIntensity
		memcpy(&k[6], &gi, 4);
		FullscreenPass(cl, R.tonemapPSO, tmTarget, k, 8);
	}
	if (fxaa) {
		Transition(cl, R.ldr, srvAll);
		Transition(cl, R.screen, D3D12_RESOURCE_STATE_RENDER_TARGET);
		uint32_t k[8] = { R.ldr.srv, 0, 0, 0, R.w, R.h, 0, 0 };
		FullscreenPass(cl, R.fxaaPSO, R.screen, k, 8);
	}
	Transition(cl, R.screen, D3D12_RESOURCE_STATE_RENDER_TARGET);
	cl->SetGraphicsRootConstantBufferView(RP_VIEW_CBV, R.defaultCB);
}

// a lightsaber blade (from the glow surface): a line light from the hilt to the tip
void R_AddSaberLight(const float* start, const float* dir, float length, const uint8_t* rgba)
{
	if (!R.inView || length < 1.0f || R.tubes.size() >= 64) return;
	GpuLight l = {};
	for (int q = 0; q < 3; q++) { l.pos[q] = start[q]; l.dir[q] = dir[q] * length; }
	l.radius = 170.0f + length * 0.25f;   // r19: a short reach - lights who holds it and what is near, not the room
	// Battlefront II (2017) style: the blade colour with a little of the white-hot core in it, a soft-cored inverse
	// square from the nearest point of the blade (bright pools on floors and walls nearby, not a hard hot spot on
	// whatever touches it) and a faint hum flicker
	float c[3] = { rgba[0] / 255.0f, rgba[1] / 255.0f, rgba[2] / 255.0f };
	float mx = Q_max(c[0], Q_max(c[1], c[2]));
	if (mx < 0.01f) { c[0] = c[1] = c[2] = 1; mx = 1; }
	const float t = ri.Milliseconds() * 0.001f;
	const float hum = 1.0f + 0.04f * sinf(t * 47.0f + start[0] * 0.05f) + 0.03f * sinf(t * 113.0f);
	for (int q = 0; q < 3; q++) l.color[q] = (powf(c[q] / mx, 2.2f) * 0.85f + 0.15f) * hum;
	l.type = 2;
	l.invSquare = 1000.0f;               // r23: strong on the wielder without burning faces / helmets out
	l.pad[0] = 22.0f * 22.0f;   // soft radius squared (lighting.hlsl Light.lp.x)
	R.tubes.push_back(l);
}
float SaberGlowBoost() { return kt_saberGlow ? kt_saberGlow->value : 2.0f; }
bool SaberDraw() { return kt_saberDraw && kt_saberDraw->integer != 0; }
void R_AddSaberBlade(const float* start, const float* dir, float length, float radius, const uint8_t* rgba, uint32_t glowTex, const uint8_t* vertexRgba, bool coreOnly)
{
	if (!R.inView || length < 0.5f || R.blades.size() >= 128) return;
	RState::Blade b = {};
	float c[3] = { rgba[0] / 255.0f, rgba[1] / 255.0f, rgba[2] / 255.0f };
	float mx = Q_max(c[0], Q_max(c[1], c[2]));
	const bool dark = mx < 0.01f;   // darksaber style: black core, white rim
	for (int q = 0; q < 3; q++) { b.a[q] = start[q]; b.b[q] = start[q] + dir[q] * length; b.color[q] = dark ? 1.0f : powf(c[q] / mx, 2.2f); }
	b.glowR = Q_max(1.0f, radius);
	b.coreR = Q_max(0.6f, radius * 0.22f);
	b.intensity = kt_saberGlow ? kt_saberGlow->value * 0.5f : 1.0f;
	b.tex = 0; b.mode = 0; b.alpha = dark ? 1 : 0;   // procedural (BF2-style) blade
	if (coreOnly) { if (dark) return; b.alpha = 2; b.coreR = Q_max(0.9f, radius * 0.3f); }
	// the glow texture is tinted by the entity colour (rgbGen vertex): use that, linearised
	for (int q = 0; q < 3; q++) b.vcol[q] = powf(vertexRgba[q] / 255.0f, 2.2f);
	R.blades.push_back(b);
}
void R_AddSaberCore(const float* start, const float* end, float radius, const uint8_t* rgba, uint32_t tex, bool alphaBlend)
{
	if (!R.inView || R.blades.size() >= 128) return;
	RState::Blade b = {};
	for (int q = 0; q < 3; q++) { b.a[q] = start[q]; b.b[q] = end[q]; b.color[q] = powf(rgba[q] / 255.0f, 2.2f); b.vcol[q] = b.color[q]; }
	b.glowR = Q_max(0.3f, radius);
	b.coreR = b.glowR;
	b.intensity = kt_saberGlow ? kt_saberGlow->value * 0.5f : 1.0f;
	b.tex = tex; b.mode = 1; b.alpha = alphaBlend ? 1 : 0;
	R.blades.push_back(b);
}

void R_EndView()
{
	if (R.ready && R.inView && R.viewWorld && kt_speeds && kt_speeds->integer && World_Active()) {
		int now = ri.Milliseconds();
		if (now - R.lastSpeeds > 1000) {
			R.lastSpeeds = now;
			uint32_t cl; uint64_t tr;
			World_Stats(cl, tr);
			ri.Printf(PRINT_ALL, "katarn: %.1f ms, %u clusters, %.2fM triangles (gbuffer), %d characters/models\n", R.dt * 1000.0f, cl, tr / 1e6, (int)R.deferred.size());
		}
	}
	if (!R.ready || !R.inView) return;
	EnsureList();
	ID3D12GraphicsCommandList* cl = R.cl;
	bool world = R.viewWorld && (World_Active() || kt_legacyDeferred->integer >= 0);
	if (world && R.csLighting && R.tonemapPSO) {
		RenderWorldView();
	}
	else {
		// UI model view (or no world): forward into the screen with its own depth
		const ViewConstants& c = R.vc;
		D3D12_RECT rect = { (LONG)c.viewport[0], (LONG)c.viewport[1], (LONG)(c.viewport[0] + c.viewport[2]), (LONG)(c.viewport[1] + c.viewport[3]) };
		Transition(cl, R.screenDepth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		cl->ClearDepthStencilView(R.screenDepth.dsv[0], D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 1, &rect);
		cl->SetGraphicsRootConstantBufferView(RP_VIEW_CBV, R.viewCB);
		FlushStream(0);
	}
	if (R.capture) { memcpy(R.viewProj, R.capSavedVP, 64); R.capture = false; }
	R.stream.clear();
	R.deferred.clear();
	R.inView = false;
	R.reversedZ = false;
}

bool R_ReadScreen(int x, int y, int w, int h, uint8_t* out, bool rgba)
{
	if (!R.ready || w <= 0 || h <= 0) return false;
	EnsureList();
	FlushStream(0);
	ID3D12GraphicsCommandList* cl = R.cl;
	uint32_t pitch = ((uint32_t)w * 4 + 255) & ~255u;
	D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
	D3D12_RESOURCE_DESC rd = {};
	rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = (uint64_t)pitch * h; rd.Height = 1; rd.DepthOrArraySize = 1;
	rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	ID3D12Resource* rb = nullptr;
	if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)))) return false;
	Transition(cl, R.screen, D3D12_RESOURCE_STATE_COPY_SOURCE);
	D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
	dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, (UINT)w, (UINT)h, 1, pitch };
	src.pResource = R.screen.res; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	int top = (int)R.h - (y + h);
	D3D12_BOX box = { (UINT)x, (UINT)Q_max(0, top), 0, (UINT)(x + w), (UINT)(Q_max(0, top) + h), 1 };
	cl->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
	Transition(cl, R.screen, D3D12_RESOURCE_STATE_RENDER_TARGET);
	SubmitList(cl);
	WaitIdle();
	R.cl = OpenList();
	R.cl->SetGraphicsRootConstantBufferView(RP_VIEW_CBV, R.defaultCB);
	R.cl->SetGraphicsRootConstantBufferView(RP_EXTRA_CBV, R.defaultCB);
	uint8_t* p = nullptr;
	D3D12_RANGE range = { 0, (SIZE_T)rd.Width };
	if (FAILED(rb->Map(0, &range, (void**)&p))) { rb->Release(); return false; }
	// GL order: bottom row first
	for (int r = 0; r < h; r++) {
		const uint8_t* s = p + (size_t)(h - 1 - r) * pitch;
		uint8_t* d = out + (size_t)r * w * (rgba ? 4 : 3);
		for (int i = 0; i < w; i++) {
			d[0] = s[i * 4]; d[1] = s[i * 4 + 1]; d[2] = s[i * 4 + 2];
			if (rgba) { d[3] = 255; d += 4; }
			else d += 3;
		}
	}
	D3D12_RANGE none = { 0, 0 };
	rb->Unmap(0, &none);
	rb->Release();
	return true;
}

void R_GfxInfo()
{
	ri.Printf(PRINT_ALL, "KatarnEngine (Direct3D 12): %s, %u MB VRAM\n", g.adapterName.c_str(), (unsigned)(g.vram >> 20));
	ri.Printf(PRINT_ALL, "  %ux%u, vsync %s, shadows %ux%u x4, HD world: %s\n", R.w, R.h, g.cfg.vsync ? "on" : "off", R.shadowRes, R.shadowRes,
		World_Active() ? "loaded" : "none");
}

// ------------------------------------------------------------------------------------------------ reflection probes
// rend2-style cubemaps for old maps and HD worlds. Positions come from the map's entities (misc_cubemap when the map
// has them, else where players and NPCs go: spawn points, waypoints, items). The renderer captures them itself once
// the game draws the world: 128x128 views queued at the start of a frame (all six faces per frame on BSP maps, two
// on HD worlds), each lit like the game view, copied into the probe's cube and filtered into a mip chain whose
// levels are rougher reflections.

// the BSP leaf cluster of a point (-1 = in solid / outside the map)
static int PointCluster(const float* p)
{
	if (!tr.world || !tr.world->nodes) return -1;
	mnode_t* n = tr.world->nodes;
	while (n->contents == -1) {
		const cplane_t* pl = n->plane;
		n = (DotProduct(p, pl->normal) - pl->dist > 0) ? n->children[0] : n->children[1];
	}
	return n->cluster;
}

static struct ProbeFront {
	std::vector<RState::Probe> list;   // positions (textures live in R.probes)
	std::vector<uint8_t> done;
	int scenes = 0, lastSceneFrame = -100, time = 0;
	float viewOrg[3] = {};
	int current = -1, face = 0;
	bool logged = false;
} PF;

static void ProbesDestroy()
{
	for (auto& p : R.probes) {
		if (p.cube.res) DestroyTexture(p.cube);
		for (uint32_t& s : p.mipSrv) if (s) { FreeDescriptor(s); s = 0; }
	}
	R.probes.clear();
}

// the back end's probes follow the front end's list (a new map, or the renderer restarted)
static void ProbesSync()
{
	ProbesDestroy();
	R.probes = PF.list;
	for (auto& p : R.probes) { p.cube = Texture(); memset(p.mipSrv, 0, sizeof(p.mipSrv)); p.faces = 0; p.valid = false; }
	PF.done.assign(PF.list.size(), 0);
	PF.current = -1; PF.face = 0;
}

void GlowLights_Set(const float* data, int count) { s_glowLights.assign(data, data + count * 7); }
void Probes_SetEntities(const char* text)
{
	PF.list.clear();
	PF.scenes = 0; PF.lastSceneFrame = -100; PF.logged = false;
	struct Cand { float o[3]; int pri; };
	std::vector<Cand> cands;
	const char* p = text ? text : "";
	auto token = [&](std::string& out) -> bool {
		while (*p && (unsigned char)*p <= ' ') p++;
		if (!*p) return false;
		if (*p == '{' || *p == '}') { out.assign(p, 1); p++; return true; }
		if (*p == '"') {
			const char* s0 = ++p;
			while (*p && *p != '"') p++;
			out.assign(s0, p - s0);
			if (*p) p++;
			return true;
		}
		const char* s0 = p;
		while (*p && (unsigned char)*p > ' ') p++;
		out.assign(s0, p - s0);
		return true;
	};
	std::string t, key, val;
	while (token(t)) {
		if (t != "{") continue;
		std::string cls, origin;
		bool brush = false;
		while (token(key) && key != "}") {
			if (!token(val)) break;
			if (!Q_stricmp(key.c_str(), "classname")) cls = val;
			else if (!Q_stricmp(key.c_str(), "origin")) origin = val;
			else if (!Q_stricmp(key.c_str(), "model") && val[0] == '*') brush = true;
		}
		if (origin.empty() || brush || cls.empty()) continue;
		Cand c;
		if (sscanf(origin.c_str(), "%f %f %f", &c.o[0], &c.o[1], &c.o[2]) != 3) continue;
		const char* n = cls.c_str();
		if (!Q_stricmp(n, "misc_cubemap")) c.pri = 0;
		else if (!Q_stricmpn(n, "info_player", 11)) c.pri = 1;
		else if (!Q_stricmpn(n, "NPC_", 4) || !Q_stricmp(n, "waypoint") || !Q_stricmpn(n, "waypoint_", 9) || !Q_stricmpn(n, "item_", 5)
			|| !Q_stricmpn(n, "weapon_", 7) || !Q_stricmpn(n, "ammo_", 5)) c.pri = 2;
		else if (!Q_stricmp(n, "light") || !Q_stricmpn(n, "misc_", 5) || !Q_stricmpn(n, "fx_", 3)) c.pri = 3;
		else continue;   // targets, triggers, path corners, info_null...: often outside the playable space
		cands.push_back(c);
	}
	bool authored = false;
	for (auto& c : cands) if (c.pri == 0) authored = true;
	std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.pri < b.pri; });
	// usable spots: in the open (a valid cluster), about eye height above what stands on the ground
	struct Spot { float o[3]; int pri, cluster; };
	std::vector<Spot> pool;
	for (auto& c : cands) {
		if (authored && c.pri != 0) continue;
		Spot s = { { c.o[0], c.o[1], c.o[2] }, c.pri, -1 };
		if (c.pri) s.o[2] += 40;
		s.cluster = PointCluster(s.o);
		if (s.cluster < 0 && c.pri) { s.o[2] -= 40; s.cluster = PointCluster(s.o); }
		if (s.cluster < 0 && c.pri) continue;
		float minSpace = c.pri ? 384.0f : 64.0f;
		bool close = false;
		for (auto& q : pool) {
			float d[3] = { q.o[0] - s.o[0], q.o[1] - s.o[1], q.o[2] - s.o[2] };
			if (Dot3(d, d) < minSpace * minSpace) { close = true; break; }
		}
		if (!close) pool.push_back(s);
	}
	// at most kMaxProbes: player starts first, then the spot farthest from those already chosen (even coverage)
	std::vector<Spot> chosen;
	std::vector<float> dmin(pool.size(), 1e30f);
	std::vector<uint8_t> used(pool.size(), 0);
	auto take = [&](size_t i) {
		used[i] = 1; chosen.push_back(pool[i]);
		for (size_t k = 0; k < pool.size(); k++) {
			float d[3] = { pool[k].o[0] - pool[i].o[0], pool[k].o[1] - pool[i].o[1], pool[k].o[2] - pool[i].o[2] };
			dmin[k] = Q_min(dmin[k], Dot3(d, d));
		}
	};
	for (size_t i = 0; i < pool.size() && chosen.size() < kMaxProbes; i++) if (pool[i].pri <= 1) take(i);
	while (chosen.size() < kMaxProbes) {
		size_t best = (size_t)-1; float bd = -1;
		for (size_t i = 0; i < pool.size(); i++) if (!used[i] && dmin[i] > bd) { bd = dmin[i]; best = i; }
		if (best == (size_t)-1) break;
		take(best);
	}
	for (size_t i = 0; i < chosen.size(); i++) {
		RState::Probe pr = {};
		pr.origin[0] = chosen[i].o[0]; pr.origin[1] = chosen[i].o[1]; pr.origin[2] = chosen[i].o[2];
		pr.cluster = chosen[i].cluster;
		// parallax sphere: about the distance to the next probe (the size of the space it stands in)
		float nd = 1e30f;
		for (size_t k = 0; k < chosen.size(); k++) if (k != i) {
			float d[3] = { chosen[k].o[0] - pr.origin[0], chosen[k].o[1] - pr.origin[1], chosen[k].o[2] - pr.origin[2] };
			nd = Q_min(nd, sqrtf(Dot3(d, d)));
		}
		pr.radius = nd > 1e29f ? 1024.0f : Q_max(512.0f, Q_min(1536.0f, nd));
		PF.list.push_back(pr);
	}
	ProbesSync();
	GradeLoadForMap();   // rend2: maps/<map>.grade
	ri.Printf(PRINT_DEVELOPER, "KatarnEngine: %d reflection probes (%s)\n", (int)PF.list.size(), authored ? "misc_cubemap" : "from entities");
}

void Probes_NoteScene(const void* refdef)
{
	const refdef_t* fd = (const refdef_t*)refdef;
	if (!fd || (fd->rdflags & (RDF_NOWORLDMODEL | RDF_SKYBOXPORTAL | RDF_KT_PROBE))) return;
	PF.scenes++;
	PF.lastSceneFrame = tr.frameCount;
	PF.time = fd->time;
	VectorCopy(fd->vieworg, PF.viewOrg);
}

void Probes_BeginFrame()
{
	if (!R.ready || !tr.world || PF.list.empty() || !kt_probes || !kt_probes->integer || !R.csProbe) return;
	if (R.probes.size() != PF.list.size()) ProbesSync();
	// only once the game is drawing the world (its textures and shaders are in), and only while it keeps doing so
	if (PF.scenes < 30 || tr.frameCount - PF.lastSceneFrame > 3) return;
	if ((uint32_t)glConfig.vidWidth < kProbeSize || (uint32_t)glConfig.vidHeight < kProbeSize) return;
	// cube faces as seen from Quake space: forward, right, up (D3D cube convention with the sample direction
	// (x, z, y), see lighting.hlsl)
	static const float face[6][3][3] = {
		{ { 1, 0, 0 }, { 0, -1, 0 }, { 0, 0, 1 } }, { { -1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } },
		{ { 0, 0, 1 }, { 1, 0, 0 }, { 0, -1, 0 } }, { { 0, 0, -1 }, { 1, 0, 0 }, { 0, 1, 0 } },
		{ { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } }, { { 0, -1, 0 }, { -1, 0, 0 }, { 0, 0, 1 } } };
	int faces = World_Active() ? 1 : 6;
	while (faces-- > 0) {
		if (PF.current < 0) {
			// next: the probe nearest the camera
			float best = 1e30f;
			for (size_t i = 0; i < PF.list.size(); i++) if (!PF.done[i]) {
				float d[3] = { PF.list[i].origin[0] - PF.viewOrg[0], PF.list[i].origin[1] - PF.viewOrg[1], PF.list[i].origin[2] - PF.viewOrg[2] };
				float d2 = Dot3(d, d);
				if (d2 < best) { best = d2; PF.current = (int)i; }
			}
			if (PF.current < 0) {
				if (!PF.logged) { PF.logged = true; ri.Printf(PRINT_DEVELOPER, "KatarnEngine: reflection probes captured\n"); }
				return;
			}
			PF.face = 0;
		}
		refdef_t rd;
		memset(&rd, 0, sizeof(rd));
		rd.x = 0; rd.y = 0; rd.width = (int)kProbeSize; rd.height = (int)kProbeSize;
		rd.fov_x = rd.fov_y = 90.0f;
		VectorCopy(PF.list[PF.current].origin, rd.vieworg);
		for (int a = 0; a < 3; a++) {
			rd.viewaxis[0][a] = face[PF.face][0][a];
			rd.viewaxis[1][a] = -face[PF.face][1][a];   // Quake axis 1 is left
			rd.viewaxis[2][a] = face[PF.face][2][a];
		}
		rd.time = PF.time;
		rd.rdflags = RDF_KT_PROBE | (PF.current << 20) | (PF.face << 26);
		RE_ClearScene();
		RE_RenderScene(&rd);
		RE_ClearScene();
		if (++PF.face == 6) { PF.done[PF.current] = 1; PF.current = -1; }
	}
}

// probes for a view: the ones in clusters the camera can see (less light leaking through walls), else all
static uint32_t ProbeList(const float* org)
{
	std::vector<uint32_t> pick;
	const byte* vis = nullptr;
	if (tr.world && tr.world->vis) {
		int cl = PointCluster(org);
		if (cl >= 0 && cl < tr.world->numClusters) vis = tr.world->vis + cl * tr.world->clusterBytes;
	}
	for (int pass = 0; pass < 2 && pick.empty(); pass++)
		for (uint32_t i = 0; i < R.probes.size(); i++) {
			const RState::Probe& p = R.probes[i];
			if (!p.valid) continue;
			if (!SphereVisible(p.origin, p.radius)) continue;   // r27: off-screen probes reflect into nothing on screen
			(void)vis;   // r26: every probe, always - picking them by what the camera can see switched reflections on as you walked
			pick.push_back(i);
		}
	if (pick.empty()) return 0;
	struct G { float pos[3], radius; uint32_t srv, mips, pad[2]; };
	UploadAlloc a = Upload(pick.size() * sizeof(G), 256);
	G* gp = (G*)a.cpu;
	for (size_t k = 0; k < pick.size(); k++) {
		const RState::Probe& p = R.probes[pick[k]];
		G e = { { p.origin[0], p.origin[1], p.origin[2] }, p.radius, p.cube.srv, p.cube.mips, { 0, 0 } };
		gp[k] = e;
	}
	D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
	sd.Format = DXGI_FORMAT_R32_TYPELESS;
	sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	sd.Buffer.FirstElement = a.offset / 4;
	sd.Buffer.NumElements = (UINT)(pick.size() * sizeof(G) / 4);
	sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
	g.dev->CreateShaderResourceView(a.res, &sd, CpuHandle(R.probeSrv[g.frameIndex]));
	return (uint32_t)pick.size();
}

static void SubresourceBarrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, uint32_t mip, uint32_t mips, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
	D3D12_RESOURCE_BARRIER br[6] = {};
	for (uint32_t f = 0; f < 6; f++) {
		br[f].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		br[f].Transition.pResource = r;
		br[f].Transition.Subresource = mip + f * mips;
		br[f].Transition.StateBefore = a;
		br[f].Transition.StateAfter = b;
	}
	cl->ResourceBarrier(6, br);
}

static void ProbeCaptureFace(ID3D12GraphicsCommandList* cl)
{
	if (R.capProbe < 0 || R.capProbe >= (int)R.probes.size() || R.capFace > 5) return;
	RState::Probe& p = R.probes[R.capProbe];
	if (!p.cube.res) {
		if (!CreateTexture(p.cube, kProbeSize, kProbeSize, kProbeMips, DXGI_FORMAT_R16G16B16A16_FLOAT, TEX_CUBE | TEX_UAV, 1, nullptr, "reflection probe"))
			return;
		for (uint32_t m = 0; m < kProbeMips; m++) {
			D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
			sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
			sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
			sd.Texture2DArray.MostDetailedMip = m; sd.Texture2DArray.MipLevels = 1;
			sd.Texture2DArray.FirstArraySlice = 0; sd.Texture2DArray.ArraySize = 6;
			p.mipSrv[m] = AllocDescriptor();
			g.dev->CreateShaderResourceView(p.cube.res, &sd, CpuHandle(p.mipSrv[m]));
		}
	}
	const ViewConstants& c = R.vc;
	Transition(cl, R.hdr, D3D12_RESOURCE_STATE_COPY_SOURCE);
	Transition(cl, p.cube, D3D12_RESOURCE_STATE_COPY_DEST);
	D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
	dst.pResource = p.cube.res; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = (UINT)R.capFace * p.cube.mips;
	src.pResource = R.hdr.res; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
	D3D12_BOX box = { (UINT)c.viewport[0], (UINT)c.viewport[1], 0, (UINT)c.viewport[0] + kProbeSize, (UINT)c.viewport[1] + kProbeSize, 1 };
	cl->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
	p.faces |= (uint8_t)(1u << R.capFace);
	if (p.faces != 63) return;

	// all six faces: filter the mip chain, each level a wider cone of the one above it
	const D3D12_RESOURCE_STATES rd = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	Transition(cl, p.cube, rd);
	cl->SetPipelineState(R.csProbe);
	for (uint32_t m = 1; m < p.cube.mips; m++) {
		uint32_t size = Q_max(1u, kProbeSize >> m);
		SubresourceBarrier(cl, p.cube.res, m, p.cube.mips, rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		uint32_t k[4] = { p.mipSrv[m - 1], p.cube.uav[m], size, m };
		cl->SetComputeRoot32BitConstants(RP_CONSTANTS, 4, k, 0);
		cl->Dispatch((size + 7) / 8, (size + 7) / 8, 6);
		SubresourceBarrier(cl, p.cube.res, m, p.cube.mips, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, rd);
	}
	Transition(cl, p.cube, rd | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	p.valid = true;
}

} // namespace kt
