// rd-sojourn: Movie Duels SP <-> Warzone renderer glue (types and helpers the Warzone renderer took from its MP engine)
#pragma once
#include <cctype>
#include <cstdlib>

// --- Warzone's material table (renderer-only meaning; BSP surface flags 0..30 agree with JKA's)
#undef MATERIAL_NONE
#define MATERIAL_NONE							0			// for when the artist hasn't set anything up =)
#undef MATERIAL_SOLIDWOOD
#define MATERIAL_SOLIDWOOD						1			// freshly cut timber
#undef MATERIAL_HOLLOWWOOD
#define MATERIAL_HOLLOWWOOD						2			// termite infested creaky wood
#undef MATERIAL_SOLIDMETAL
#define MATERIAL_SOLIDMETAL						3			// solid girders
#undef MATERIAL_HOLLOWMETAL
#define MATERIAL_HOLLOWMETAL					4			// hollow metal machines
#undef MATERIAL_SHORTGRASS
#define MATERIAL_SHORTGRASS						5			// manicured lawn
#undef MATERIAL_LONGGRASS
#define MATERIAL_LONGGRASS						6			// long jungle grass
#undef MATERIAL_DIRT
#define MATERIAL_DIRT							7			// hard mud
#undef MATERIAL_SAND
#define MATERIAL_SAND							8			// sandy beach
#undef MATERIAL_GRAVEL
#define MATERIAL_GRAVEL							9			// lots of small stones
#undef MATERIAL_GLASS
#define MATERIAL_GLASS							10			//
#undef MATERIAL_CONCRETE
#define MATERIAL_CONCRETE						11			// hardened concrete pavement
#undef MATERIAL_MARBLE
#define MATERIAL_MARBLE							12			// marble floors
#undef MATERIAL_WATER
#define MATERIAL_WATER							13			// light covering of water on a surface
#undef MATERIAL_SNOW
#define MATERIAL_SNOW							14			// freshly laid snow
#undef MATERIAL_ICE
#define MATERIAL_ICE							15			// packed snow/solid ice
#undef MATERIAL_FLESH
#define MATERIAL_FLESH							16			// hung meat, corpses in the world
#undef MATERIAL_MUD
#define MATERIAL_MUD							17			// wet soil
#undef MATERIAL_BPGLASS
#define MATERIAL_BPGLASS						18			// bulletproof glass
#undef MATERIAL_DRYLEAVES
#define MATERIAL_DRYLEAVES						19			// dried up leaves on the floor
#undef MATERIAL_GREENLEAVES
#define MATERIAL_GREENLEAVES					20			// fresh leaves still on a tree
#undef MATERIAL_FABRIC
#define MATERIAL_FABRIC							21			// Cotton sheets
#undef MATERIAL_CANVAS
#define MATERIAL_CANVAS							22			// tent material
#undef MATERIAL_ROCK
#define MATERIAL_ROCK							23			//
#undef MATERIAL_RUBBER
#define MATERIAL_RUBBER							24			// hard tire like rubber
#undef MATERIAL_PLASTIC
#define MATERIAL_PLASTIC						25			//
#undef MATERIAL_TILES
#define MATERIAL_TILES							26			// tiled floor
#undef MATERIAL_CARPET
#define MATERIAL_CARPET							27			// lush carpet
#undef MATERIAL_PLASTER
#define MATERIAL_PLASTER						28			// drywall style plaster
#undef MATERIAL_SHATTERGLASS
#define MATERIAL_SHATTERGLASS					29			// glass with the Crisis Zone style shattering
#undef MATERIAL_ARMOR
#define MATERIAL_ARMOR							30			// body armor
#undef MATERIAL_COMPUTER
#define MATERIAL_COMPUTER						31			// computers/electronic equipment
#undef MATERIAL_PUDDLE
#define MATERIAL_PUDDLE							32			// shallow puddle on floor surface, or water that should not use GLSL.
#undef MATERIAL_POLISHEDWOOD
#define MATERIAL_POLISHEDWOOD					33			// shiny polished wood
#undef MATERIAL_LAVA
#define MATERIAL_LAVA							34			// lava
#undef MATERIAL_TREEBARK
#define MATERIAL_TREEBARK						35			// tree bark
#undef MATERIAL_STONE
#define MATERIAL_STONE							36			// stone (rock but no vines)
#undef MATERIAL_EFX
#define MATERIAL_EFX							37			// any efx surfaces
#undef MATERIAL_BLASTERBOLT
#define MATERIAL_BLASTERBOLT					38			// blaster bolts, sabers, etc
#undef MATERIAL_FIRE
#define MATERIAL_FIRE							39			// fire
#undef MATERIAL_SMOKE
#define MATERIAL_SMOKE							40			// smoke
#undef MATERIAL_FIREFLIES
#define MATERIAL_FIREFLIES						41			// fire flies fx
#undef MATERIAL_MAGIC_PARTICLES_TREE
#define MATERIAL_MAGIC_PARTICLES_TREE			42			// magic tree particle fx
#undef MATERIAL_MAGIC_PARTICLES
#define MATERIAL_MAGIC_PARTICLES				43			// magic particles (small particle fx)
#undef MATERIAL_PORTAL
#define MATERIAL_PORTAL							44			// area transition portal
#undef MATERIAL_MENU_BACKGROUND
#define MATERIAL_MENU_BACKGROUND				45			// main menu background glsl
#undef MATERIAL_SKYSCRAPER
#define MATERIAL_SKYSCRAPER						46			// warzone sky scrapers
#undef MATERIAL_DISTORTEDGLASS
#define MATERIAL_DISTORTEDGLASS					47			// warzone distorted glass
#undef MATERIAL_DISTORTEDPUSH
#define MATERIAL_DISTORTEDPUSH					48			// warzone distorted push
#undef MATERIAL_DISTORTEDPULL
#define MATERIAL_DISTORTEDPULL					49			// warzone distorted pull
#undef MATERIAL_CLOAK
#define MATERIAL_CLOAK							50			// warzone cloak
#undef MATERIAL_FORCEFIELD
#define MATERIAL_FORCEFIELD						51			// warzone force fields
#undef MATERIAL_PROCEDURALFOLIAGE
#define MATERIAL_PROCEDURALFOLIAGE				52			// warzone GLSL foliages
#undef MATERIAL_BIRD
#define MATERIAL_BIRD							53			// warzone flying birds (wing flapping)
#undef MATERIAL_LAST
#define MATERIAL_LAST							54			// number of materials
#undef MATERIALS
#define MATERIALS						\
	"none",								\
	"solidwood",						\
	"hollowwood",						\
	"solidmetal",						\
	"hollowmetal",						\
	"shortgrass",						\
	"longgrass",						\
	"dirt",	   							\
	"sand",	   							\
	"gravel",							\
	"glass",							\
	"concrete",							\
	"marble",							\
	"water",							\
	"snow",	   							\
	"ice",								\
	"flesh",							\
	"mud",								\
	"bpglass",							\
	"dryleaves",						\
	"greenleaves",						\
	"fabric",							\
	"canvas",							\
	"rock",								\
	"rubber",							\
	"plastic",							\
	"tiles",							\
	"carpet",							\
	"plaster",							\
	"shatterglass",						\
	"armor",							\
	"computer",							\
	"puddle",							\
	"polishedwood",						\
	"lava",								\
	"treebark",							\
	"stone",							\
	"efx",								\
	"blasterbolt",						\
	"fire",								\
	"smoke",							\
	"fireflies",						\
	"magicparticlestree",				\
	"magicparticles",					\
	"portal",							\
	"menubackground",					\
	"skyscraper",						\
	"distortedglass",					\
	"distortedpush",					\
	"distortedpull",					\
	"cloak",							\
	"forcefield",						\
	"proceduralfoliage"					\
	"bird"

// --- MP renderer types missing from the SP headers
#ifndef MAX_POLYVERTS
#ifndef MAX_POLYS
#define MAX_POLYS 8192
#endif
#define MAX_POLYVERTS 3000
#endif
#ifndef MAX_KEYS
#define MAX_KEYS 512
#endif
typedef struct WinVars_s { void* hWnd; void* hInstance; } WinVars_t;

typedef struct _skinSurface_s {
	char name[MAX_QPATH];
	void* shader;
} _skinSurface_t;
typedef struct skin_s {
	char name[MAX_QPATH];
	int numSurfaces;
	_skinSurface_t* surfaces[128];
} skin_t;

inline qboolean StringContainsWord(const char* haystack, const char* needle)
{
	if (!*needle) return qfalse;
	for (; *haystack; ++haystack)
	{
		if (toupper(*haystack) == toupper(*needle))
		{
			const char *h, *n;
			for (h = haystack, n = needle; *h && *n; ++h, ++n)
				if (toupper(*h) != toupper(*n)) break;
			if (!*n) return qtrue;
		}
	}
	return qfalse;
}
#ifndef random
#define random() ((rand() & 0x7fff) / ((float)0x7fff))
#endif

// --- SP has no renderer hunk: allocate from the zone (as rend2 SP does)
typedef enum { h_high, h_low, h_dontcare } ha_pref;
void* Hunk_Alloc(const int size, const ha_pref preference);
void* Hunk_AllocateTempMemory(const int size);
void Hunk_FreeTempMemory(void* buf);
int Hunk_MemoryRemaining(void);
void* R_Malloc(const int iSize, const memtag_t eTag);
void* R_Malloc(const int iSize, const memtag_t eTag, const qboolean bZeroit);
void* R_Malloc(const int iSize, const memtag_t eTag, const qboolean bZeroit, const int iUnusedAlign);
void R_Free(void* pvAddress);
void QDECL Com_OPrintf(const char* msg, ...);
void SJ_BoxTrace(trace_t* results, const vec3_t start, const vec3_t end, const vec3_t mins, const vec3_t maxs, int model, int brushmask, int capsule);

#include <algorithm>
// mixed-type min / max like the old macros (Warzone's code relies on them)
// (by value: decltype(a < b ? a : b) is a reference for same-type arguments, which returned a dangling reference to the
//  parameter and gave garbage - 31 mip levels for an 8x8 texture, an endless log2 loop for 256x256)
#include <type_traits>
template <class A, class B> inline typename std::common_type<A, B>::type min(A a, B b) { return a < b ? a : b; }
template <class A, class B> inline typename std::common_type<A, B>::type max(A a, B b) { return a > b ? a : b; }

// --- Warzone engine extensions Movie Duels SP doesn't have (never set here)
#define RT_PLANT			(RT_MAX_REF_ENTITY_TYPE + 1)
#define RT_GRASS			(RT_MAX_REF_ENTITY_TYPE + 2)
#define RT_MODEL_INSTANCED	(RT_MAX_REF_ENTITY_TYPE + 3)
#ifndef RDF_BLUR
#define RDF_BLUR		0
#endif
#ifndef RDF_UNDERWATER
#define RDF_UNDERWATER	0
#endif
template <class T, class L, class H> inline T Q_clamp(L lo, T v, H hi) { return v < (T)lo ? (T)lo : (v > (T)hi ? (T)hi : v); }
void Mat3x4_Multiply(mdxaBone_t* out, const mdxaBone_t* in2, const mdxaBone_t* in);
void Create_Matrix(const float* angle, mdxaBone_t* matrix);

#ifndef RF_FORCEPOST
#define RF_FORCEPOST 0
#endif
#ifndef RDF_NOFOG
#define RDF_NOFOG 0
#endif
#ifndef MST_FOLIAGE
#define MST_FOLIAGE 0x7ffe		// Warzone map surface type; never in Movie Duels maps
#endif
#ifndef crandom
#define crandom() (2.0f * (random() - 0.5f))
#endif

#ifndef GHOUL2_CRAZY_SMOOTH
#define GHOUL2_CRAZY_SMOOTH 0x2000
#endif
#ifndef RDF_AUTOMAP
#define RDF_AUTOMAP 0
#endif
#ifndef MASK_PLAYERSOLID
#define MASK_PLAYERSOLID (CONTENTS_SOLID|CONTENTS_PLAYERCLIP|CONTENTS_BODY|CONTENTS_TERRAIN)
#endif
inline float DistanceVertical(const vec3_t p1, const vec3_t p2) { return fabsf(p1[2] - p2[2]); }
extern qboolean sj_menuIsOpen;	// Warzone kept this in glconfig_t
#include <vector>
#include <string>
#include <utility>
using std::vector;
using std::string;
using std::make_pair;
void G2_SetSurfaceOnOffFromSkin(CGhoul2Info* ghlInfo, const qhandle_t render_skin);
