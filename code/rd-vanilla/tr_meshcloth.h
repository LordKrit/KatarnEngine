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
  maxaccel units/s^2: the character's acceleration the cloth feels, at
           most (Battlefront II's MaxAcceleration; 20 m/s^2 is about 790).
           0 = no limit. Default: the r_meshClothMaxAccel console value.
  maxspin  degrees/s: how fast the attached edge may turn before the cloth
           is carried round with it instead of winding up (fast saber spins).
           0 = no limit (default).
  spread   0..1 sideways-only pull toward the animated shape: keeps a
           cape from rolling into a rope in saber spins without slowing
           its swing front to back. Default 0.
  stretch  0..1 XPBD stretch give: 0 = rigid fabric (default), 0.1-0.3 = a
           little elastic (knits, leather straps).
  friction 0..1 kinetic friction against the body and the floor. Default 0.3.
  sfriction static friction: a contact sliding slower than sfriction x how
           hard it is pressed in sticks (cloth rests on shoulders instead of
           buzzing). Default 0.5. 0 = off.
  lift     units a free point may rise above the attached edge it hangs
           from (stops capes flipping over the head on spawns and fast
           turns). Default 2. 'lift off' = no limit: capes can blow up and
           over the head (also needs a long enough tether).
  shape    0..1 optional pull back toward the modelled shape. Default 0
           (real cloth: only the attached part follows the animation).

The solver is XPBD (r_meshClothXPBD 1, default; compliance from bend/stretch,
r_meshClothXPBDIter iterations per substep) or plain position-based dynamics
(r_meshClothXPBD 0), with substeps (r_meshClothIter
substeps per 16 ms), rigid stretch constraints, bending, long range
attachments, body capsules and an inertia scale (r_meshClothInertia).
===========================================================================
*/
#pragma once

struct mcCapsule_t {
	vec3_t	a, b;
	float	r;
	int		hard;	// 1 = limbs/hands: always push cloth fully out, even where the animated cloth is inside;
					// 2 = the same, for the shoulders (used by grid-simulated cloth only, so older capes keep their tuning)
};

struct mcParams_t {
	float	gravity;		// units / s^2
	float	drag;			// air resistance along the cloth normal
	int		iterations;		// constraint passes per step
	float	shapeScale;		// multiplies each surface's shape value
	float	inertia;		// 0..1 how much of the character's own travel the cloth feels
	float	maxAccel;		// units/s^2 of character acceleration the cloth feels (0 = no limit)
	float	airDrag;		// 1/s drag on all motion relative to the air: capes trail when running and lift when falling
	float	wind[3];		// world wind velocity, units/s (still air = 0 0 0)
	float	gust;			// 0..1 how much the wind strength varies over time
	float	timeSec;		// current time for gusts (filled in by MC_Apply)
	int		xpbd;			// 1: XPBD constraints (compliance, substep independent); 0: old projection
	int		xpbdIters;		// XPBD solver iterations per substep
	float	frictionScale;
	float	liftLimit;		// overrides every surface's 'lift': -2 = use the config, -1 = no limit, N = units	// multiplies every surface's friction (0 = frictionless)
	int		floor;			// 1: floorPt / floorN hold the floor plane under the character (world)
	float	floorPt[3];
	float	floorN[3];
	int		numWalls;		// walls near the character (world planes the cloth stays in front of)
	float	wallPt[8][3];
	float	wallN[8][3];
	float	viewDist;		// distance from the camera (units), for LOD
	float	lodDist;		// beyond this: half the substeps, no self-collision (0 = off)
	float	cullDist;		// beyond this: no simulation, the animated pose is drawn (0 = off)
	int		sleep;			// 1: cloth at rest on a still character stops simulating until something moves
	int		lite;			// internal (set by the LOD)
};

// Counters since the last MC_TakeStats call (r_meshClothStats).
struct mcStats_t {
	int		instances = 0;		// characters with cloth state
	int		simulated = 0;		// cloth simulations run (one per character per frame)
	int		steps = 0;			// 16 ms steps taken
	int		sleepingSteps = 0;	// steps skipped because the cloth was asleep
	int		sleeping = 0;		// simulations of sleeping cloth
	int		lod = 0;			// simulations at reduced detail
	int		culled = 0;			// frames a character's cloth was too far to simulate
	int		usec = 0;			// time spent simulating
};
void MC_TakeStats( mcStats_t *out );

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

// Push all cloth near a point outward, like the blast wave of an explosion.
// strength: speed (units/s) given to cloth at the centre, fading to 0 at radius.
void MC_Blast( const float center[3], float strength, float radius );
