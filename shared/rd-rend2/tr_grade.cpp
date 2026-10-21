/*
tr_grade.cpp - filmic tone curve and per-map colour grading for rend2.

Every setting is an r_grade* cvar, so the look can be tuned live from the console.
When a map loads, maps/<mapname>.grade is read if it exists (lines of "<cvar> <value>",
// comments allowed); otherwise the defaults below are restored. "grade save" writes the
current values to maps/<mapname>.grade under the home path, "grade reset" restores the
defaults, "grade preset <name>" applies a named look and "grade" prints the current values.
*/

#include "tr_local.h"

struct gradeVar_t { const char* name; const char* def; cvar_t* cv; };

static gradeVar_t gradeVars[] = {
	{ "r_grade",            "0"     , nullptr }, // 0 = stock rend2 tonemap
	{ "r_gradeCurve",       "1"     , nullptr }, // 0 = stock filmic, 1 = ACES, 2 = Uchimura (GT, soft highlights)
	{ "r_gradeExposure",    "0"     , nullptr }, // stops
	{ "r_gradeContrast",    "1.12"  , nullptr }, // around 18% grey, in log space
	{ "r_gradeSaturation",  "0.92"  , nullptr },
	{ "r_gradeTemperature", "0"     , nullptr }, // -1 cold .. +1 warm
	{ "r_gradeTint",        "0"     , nullptr }, // -1 green .. +1 magenta
	{ "r_gradeShadows",     "1 1 1" , nullptr }, // multiplier for the dark end (lift colour)
	{ "r_gradeHighlights",  "1 1 1" , nullptr }, // multiplier for the bright end (gain colour)
	{ "r_gradeBlack",       "0"     , nullptr }, // black crush (0..0.1)
	{ "r_gradeVignette",    "0.25"  , nullptr }, // 0..1
	// reflections and atmosphere can also be set per map in the same file
	{ "r_ssrStrength",      "1"      , nullptr },
	{ "r_ssrGloss",         "1"      , nullptr },
	{ "r_ssrRough",         "1"      , nullptr },
	{ "r_atmosDensity",     "0.00012", nullptr },
	{ "r_atmosFalloff",     "0.0012" , nullptr },
	{ "r_atmosStart",       "256"    , nullptr },
	{ "r_atmosColor",       "0.55 0.6 0.7", nullptr },
	{ "r_atmosShafts",      "0.6"    , nullptr },
};
static const int numGradeVars = ARRAY_LEN(gradeVars);

static cvar_t* GV(int i) { return gradeVars[i].cv; }

static void ParseVec3(const char* s, vec3_t out)
{
	out[0] = out[1] = out[2] = 1.0f;
	int n = sscanf(s, "%f %f %f", &out[0], &out[1], &out[2]);
	if (n == 1) out[1] = out[2] = out[0];
}

void R_GradeRegisterCvars(void)
{
	for (int i = 0; i < numGradeVars; i++)
		gradeVars[i].cv = ri.Cvar_Get(gradeVars[i].name, gradeVars[i].def, CVAR_ARCHIVE_ND);
}

qboolean R_GradeEnabled(void)
{
	return (qboolean)(GV(0) && GV(0)->integer);
}

// u_Grade0 = exposure mul, contrast, saturation, vignette
// u_Grade1 = shadow rgb, curve
// u_Grade2 = highlight rgb, black crush
// u_Grade3 = white balance rgb, unused
void R_GradeUniforms(vec4_t g0, vec4_t g1, vec4_t g2, vec4_t g3)
{
	vec3_t sh, hi;
	ParseVec3(GV(7)->string, sh);
	ParseVec3(GV(8)->string, hi);

	VectorSet4(g0, powf(2.0f, GV(2)->value), GV(3)->value, GV(4)->value, Com_Clamp(0.0f, 1.0f, GV(10)->value));
	VectorSet4(g1, sh[0], sh[1], sh[2], (float)GV(1)->integer);
	VectorSet4(g2, hi[0], hi[1], hi[2], Com_Clamp(0.0f, 0.2f, GV(9)->value));

	// simple white balance: temperature pushes red/blue apart, tint pushes green against magenta
	float t = Com_Clamp(-1.0f, 1.0f, GV(5)->value), m = Com_Clamp(-1.0f, 1.0f, GV(6)->value);
	VectorSet4(g3, 1.0f + 0.18f * t + 0.06f * m, 1.0f - 0.12f * m, 1.0f - 0.18f * t + 0.06f * m, 0.0f);
	float lum = 0.2126f * g3[0] + 0.7152f * g3[1] + 0.0722f * g3[2];
	g3[0] /= lum; g3[1] /= lum; g3[2] /= lum;
}

// looks matched to the BF2 Shaderpatch reference shots
// order: curve exposure contrast saturation temperature tint shadows highlights black vignette
struct gradePreset_t { const char* name; const char* v[10]; };
static const gradePreset_t gradePresets[] = {
	{ "default",  { "1", "0",    "1.12", "0.92", "0",     "0",     "1 1 1",          "1 1 1",          "0",    "0.25" } },
	{ "interior", { "1", "-0.1", "1.22", "0.78", "-0.25", "0",     "0.92 0.97 1.08", "1.02 1.01 0.98", "0.02", "0.30" } }, // cold metal halls
	{ "sunset",   { "2", "0.1",  "1.15", "1.05", "0.55",  "0.05",  "0.95 0.92 1.05", "1.10 1.00 0.86", "0.01", "0.30" } }, // Bespin / Geonosis dusk
	{ "daylight", { "2", "0",    "1.10", "0.95", "0.15",  "-0.05", "0.96 1.00 1.04", "1.04 1.02 0.96", "0.01", "0.20" } }, // forest / exterior
	{ "space",    { "1", "0",    "1.25", "0.90", "0.20",  "0",     "0.90 0.95 1.10", "1.08 1.03 0.92", "0.03", "0.35" } },
	{ "noir",     { "1", "0",    "1.30", "0.15", "0",     "0",     "1 1 1",          "1 1 1",          "0.03", "0.45" } },
};

static void GradeApplyPreset(const gradePreset_t& p)
{
	for (int i = 0; i < 10; i++)
		ri.Cvar_Set(gradeVars[i + 1].name, p.v[i]);
}

static void GradeReset(void)
{
	for (int i = 0; i < numGradeVars; i++)
		ri.Cvar_Set(gradeVars[i].name, gradeVars[i].def);
}

static void GradeMapPath(char* out, int size)
{
	char base[MAX_QPATH];
	Q_strncpyz(base, COM_SkipPath(tr.world ? tr.world->name : (char*)"nomap"), sizeof(base));
	COM_StripExtension(base, base, sizeof(base));
	Com_sprintf(out, size, "maps/%s.grade", base);
}

void R_GradeLoadForMap(void)
{
	char path[MAX_QPATH];
	GradeMapPath(path, sizeof(path));
	GradeReset();

	char* buf = nullptr;
	if (ri.FS_ReadFile(path, (void**)&buf) <= 0 || !buf)
		return;

	const char* p = buf;
	int set = 0;
	while (*p)
	{
		char line[256]; int n = 0;
		while (*p && *p != '\n' && n < 255) line[n++] = *p++;
		line[n] = 0;
		if (*p) p++;
		char* c = strstr(line, "//"); if (c) *c = 0;
		char key[64], val[192];
		if (sscanf(line, " %63s %191[^\r\n]", key, val) != 2) continue;
		char* v = val; while (*v == ' ' || *v == '"') v++;
		for (char* e = v + strlen(v) - 1; e >= v && (*e == ' ' || *e == '"'); e--) *e = 0;
		if (!Q_stricmp(key, "preset"))
		{
			for (const auto& pr : gradePresets)
				if (!Q_stricmp(v, pr.name)) { GradeApplyPreset(pr); set++; }
			continue;
		}
		for (int i = 0; i < numGradeVars; i++)
			if (!Q_stricmp(key, gradeVars[i].name)) { ri.Cvar_Set(gradeVars[i].name, v); set++; }
	}
	ri.FS_FreeFile(buf);
	ri.Printf(PRINT_ALL, "grade: loaded %s (%d settings)\n", path, set);
}

void R_Grade_f(void)
{
	const char* cmd = ri.Cmd_Argc() > 1 ? ri.Cmd_Argv(1) : "";
	char path[MAX_QPATH];
	GradeMapPath(path, sizeof(path));

	if (!Q_stricmp(cmd, "save"))
	{
		char out[2048]; int len = 0;
		len += Com_sprintf(out + len, sizeof(out) - len, "// colour grade for %s (written by \"grade save\")\n", path);
		for (int i = 1; i < numGradeVars; i++)
			len += Com_sprintf(out + len, sizeof(out) - len, "%s \"%s\"\n", gradeVars[i].name, gradeVars[i].cv->string);
		ri.FS_WriteFile(path, out, len);
		ri.Printf(PRINT_ALL, "grade: saved %s\n", path);
	}
	else if (!Q_stricmp(cmd, "reset"))
	{
		GradeReset();
		ri.Printf(PRINT_ALL, "grade: defaults restored\n");
	}
	else if (!Q_stricmp(cmd, "preset"))
	{
		const char* n = ri.Cmd_Argc() > 2 ? ri.Cmd_Argv(2) : "";
		for (const auto& p : gradePresets)
			if (!Q_stricmp(n, p.name)) { GradeApplyPreset(p); ri.Printf(PRINT_ALL, "grade: preset %s\n", p.name); return; }
		ri.Printf(PRINT_ALL, "grade preset <name>:");
		for (const auto& p : gradePresets) ri.Printf(PRINT_ALL, " %s", p.name);
		ri.Printf(PRINT_ALL, "\n");
	}
	else if (!Q_stricmp(cmd, "load"))
	{
		R_GradeLoadForMap();
	}
	else
	{
		ri.Printf(PRINT_ALL, "grade save | load | reset | preset <name>   (map file: %s)\n", path);
		for (int i = 0; i < numGradeVars; i++)
			ri.Printf(PRINT_ALL, "  %-20s %s\n", gradeVars[i].name, gradeVars[i].cv->string);
	}
}
