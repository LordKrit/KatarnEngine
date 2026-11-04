// rd-sojourn: SP glue implementations
#include "tr_local.h"

// Mat3x4_Multiply lives in tr_ghoul2.cpp (Movie Duels rend2 front end)


void SJ_BoxTrace(trace_t* results, const vec3_t start, const vec3_t end, const vec3_t mins, const vec3_t maxs, int, int brushmask, int)
{
	ri.SV_Trace(results, start, mins, maxs, end, ENTITYNUM_NONE, brushmask, G2_NOCOLLIDE, 0);
}

// --- window / GL context: the Movie Duels SP engine creates the window (SDL); Warzone's win32 GLimp_Init did it itself
static window_t sj_window;
void SJ_LoadQGL(void);

void GLimp_Init(void)
{
	windowDesc_t desc = {};
	desc.api = GRAPHICS_API_OPENGL;
	desc.gl.majorVersion = 4;
	desc.gl.minorVersion = 3;
	desc.gl.profile = GLPROFILE_COMPATIBILITY;	// Warzone still uses GL 1.x calls next to its GLSL 4 shaders
	sj_window = ri.WIN_Init(&desc, &glConfig);
	SJ_LoadQGL();
	glConfig.vendor_string = (const char*)qglGetString(GL_VENDOR);
	glConfig.renderer_string = (const char*)qglGetString(GL_RENDERER);
	glConfig.version_string = (const char*)qglGetString(GL_VERSION);
	glConfig.extensions_string = (const char*)qglGetString(GL_EXTENSIONS);
	if (!glConfig.extensions_string) glConfig.extensions_string = "";
	// Warzone's GLimp_HaveExtension searches this one (its sdl/win32 GLimp_Init set it)
	glConfigExt.glConfig = &glConfig;
	glConfigExt.originalExtensionString = glConfig.extensions_string;
	qglGetIntegerv(GL_MAX_TEXTURE_SIZE, &glConfig.maxTextureSize);
	if (glConfig.maxTextureSize <= 0) glConfig.maxTextureSize = 0;
	// the rest of what Warzone's GLimp_InitExtensions did (everything else is loaded in tr_extensions.cpp)
	glConfig.maxActiveTextures = 0;
	qglGetIntegerv(0x84E2 /*GL_MAX_TEXTURE_UNITS*/, &glConfig.maxActiveTextures);
	if (glConfig.maxActiveTextures < 2) glConfig.maxActiveTextures = 8;
	glConfig.textureCompression = TC_NONE;
	glConfig.textureEnvAddAvailable = qtrue;
	glConfig.clampToEdgeAvailable = qtrue;
	glConfig.maxTextureFilterAnisotropy = 0;
	if (strstr(glConfig.extensions_string, "GL_EXT_texture_filter_anisotropic"))
		qglGetFloatv(0x84FF /*GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT*/, &glConfig.maxTextureFilterAnisotropy);
	ri.Printf(PRINT_ALL, "GL_VENDOR: %s\nGL_RENDERER: %s\nGL_VERSION: %s\n", glConfig.vendor_string, glConfig.renderer_string, glConfig.version_string);
}
qboolean sj_menuIsOpen = qfalse;

// ---- Sojourn: SP window / platform glue (Warzone had these in win32 / sdl) ----
void GLimp_Shutdown(void) { ri.WIN_Shutdown(); }
void GLimp_EndFrame(void) { ri.WIN_Present(&sj_window); }
void GLimp_LogComment(char *comment) { }
void GLimp_Minimize(void) { }
void GLimp_SetGamma(unsigned char red[256], unsigned char green[256], unsigned char blue[256]) { ri.WIN_SetGamma(&glConfig, red, green, blue); }

// Warzone's in-game editor GUIs (nuklear / imgui) are not part of the SP port
void GUI_Init(void) { }
void GUI_Shutdown(void) { }
int GUI_GetMouseCursor(void) { return 0; }
uint64_t GUI_GetMouseCursorBindless(void) { return 0; }
void NuklearUI_Main(void) { }
void RE_RenderImGui(void) { }

static cvar_t sj_buildScript = {};
cvar_t *com_buildScript = &sj_buildScript;

qboolean SkipBracedSection_Depth(const char **program, int depth)
{
	char *token;
	do {
		token = COM_ParseExt(program, qtrue);
		if (token[1] == 0) {
			if (token[0] == '{') depth++;
			else if (token[0] == '}') depth--;
		}
	} while (depth && *program);
	return (qboolean)(depth == 0);
}
