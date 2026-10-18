/*
===========================================================================
Mesh cloth: simulates the real vertices of chosen Ghoul2 surfaces (robes,
sleeves, hoods) instead of drawing extra cloth. Engine-neutral: the
renderer glue in tr_ghoul2.cpp supplies bone matrices, the entity
transform and body capsules; this module moves the free vertices.

All cloth surfaces of one model are simulated together as one cloth, so
pieces stitched to each other (robe + leg panels) can't separate. Any
cloth vertex that touches a non-cloth surface (torso robe, body) stays
attached, so those seams stay closed too.

Which surfaces are cloth, and which of their vertices stay attached, comes
from a config text (ext_data/meshcloth.cfg), one line per surface:

  model  surface  rule...  [tether T]  [shape S]

  model    model folder name (models/players/<model>/model.glm)
  surface  surface name in the .glm, e.g. hips_robe
  rule     where the attached part is, in the model's bind pose (the
           coordinates you see in the modelling tool: z up, feet at 0):
             above   Zpin Zfree
                     attached at/above Zpin, fully free at/below Zfree
             near    x y z  Rpin Rfree
                     attached within Rpin of the point, free beyond Rfree
             segment x0 y0 z0  x1 y1 z1  Rpin Rfree
                     same, measured from a line (e.g. shoulder to wrist)
  tether   how far (game units) a fully free vertex may move away from
           where the animation puts it. Default 12.
  bend     0..1 bending stiffness: 0 folds like silk, 1 holds its shape
           like leather. Default 0.3.
  inertia  0..1 how much of the character's running and turning this
           cloth feels (1 = all of it, a cape that flies out behind).
           Default: the r_meshClothInertia console value.
  self     (no value) self-collision, so a single-layer cape can't fold
           through itself. Leave off for layered robes and sleeves.
  smooth   0..1 smooths small creases out of the drawn cloth (visual only).
           Default 0.5.
  shape    0..1 optional pull back toward the modelled shape. Default 0
           (real cloth: only the attached part follows the animation).

The solver is position-based dynamics with substeps (r_meshClothIter
substeps per 16 ms), rigid stretch constraints, bending, long range
attachments, body capsules and an inertia scale (r_meshClothInertia).
===========================================================================
*/
#pragma once

struct mcCapsule_t {
	vec3_t	a, b;
	float	r;
};

struct mcParams_t {
	float	gravity;		// units / s^2
	float	drag;			// air resistance along the cloth normal
	int		iterations;		// constraint passes per step
	float	shapeScale;		// multiplies each surface's shape value
	float	inertia;		// 0..1 how much of the character's own travel the cloth feels
};

// Returns the 3x4 skinning matrix (model space) for a skeleton bone index.
typedef const float ( *( *mcBoneFn_t )( void *ctx, int boneIndex ) )[4];

// Load (or reload) the surface list. Returns the number of entries.
int  MC_LoadConfig( const char *text );

// Forget all cached meshes and cloth state (call when models are freed).
void MC_Clear( void );

// True if this surface is configured as cloth (cached per surface).
// surface is a const mdxmSurface_t* (passed opaquely: the .glm structs are
// unnamed types, which can't cross translation units in C++ signatures).
// modelName is the .glm's internal name, e.g. "models/players/palpatine/model".
bool MC_IsClothSurface( const void *surface, const char *modelName, const char *surfaceName );

// Replace the free vertices of an already-skinned surface with simulated
// positions. xyz/normal point at the surface's first vertex in model space
// (stride 4 floats). origin/axis convert model space to world space.
// instanceKey identifies one character's copy of the model across frames.
// The first call for a model in a frame simulates all of its cloth
// surfaces (skinning the others through boneFn); later calls reuse it.
void MC_Apply( const void *surface, const void *instanceKey, int timeMs,
			   const vec3_t origin, const vec3_t axis[3],
			   const mcCapsule_t *capsulesWorld, int numCapsules,
			   const mcParams_t *params,
			   mcBoneFn_t boneFn, void *boneCtx,
			   float (*xyz)[4], float (*normal)[4] );

// Drop instances not drawn for a while. Call about once a frame.
void MC_Prune( int timeMs );
