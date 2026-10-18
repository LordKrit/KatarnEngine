/*
===========================================================================
Mesh cloth - see tr_meshcloth.h.

Per surface (shared by every character using that model):
  particles  vertices welded by position, so UV seams can't tear apart
  freedom    0 = follows the animation exactly, 1 = fully free
  links      triangle edges (structural) and the opposite corners of
             neighbouring triangles (bend), both lengths taken from the
             animated pose each step, so the modelled folds are kept

Per character instance:
  positions/previous positions of the particles (Verlet), fixed 16 ms steps.
===========================================================================
*/

#include "../qcommon/q_shared.h"
#include "../rd-common/mdx_format.h"
#include "tr_meshcloth.h"

#include <vector>
#include <unordered_map>
#include <string>
#include <cmath>
#include <cstring>
#include <cstdlib>

namespace {

const int	MC_STEP_MS		= 16;
const float	MC_DT			= 0.016f;
const int	MC_MAX_STEPS	= 4;
const int	MC_STALE_MS		= 250;
const float	MC_TELEPORT		= 120.0f;
const int	MC_FORGET_MS	= 10000;
const float	MC_MARGIN		= 0.6f;

enum mcRuleType_t { MC_RULE_ABOVE, MC_RULE_NEAR, MC_RULE_SEGMENT };

struct mcDef_t {
	std::string		model, surface;
	mcRuleType_t	rule;
	float			a[8];
	float			tether;
	float			shape;
};

struct mcMesh_t {
	const mcDef_t		*def;
	int					numVerts;
	int					numParticles;
	std::vector<int>	vertToParticle;
	std::vector<int>	particleVert;		// one representative vertex per particle
	std::vector<float>	freedom;			// per particle
	std::vector<int>	linkA, linkB;
	std::vector<char>	linkBend;
	std::vector<int>	tris;				// particle indices, 3 per triangle
	std::vector<int>	vtris;				// vertex indices, 3 per triangle (normals)
};

struct mcInstance_t {
	const mdxmSurface_t	*surface;
	bool				active;
	int					lastTime;
	int					lastSeen;
	int					accum;
	std::vector<float>	p, prev, target, targetPrev;	// 3 floats per particle, world space
	std::vector<float>	outLocal;						// 3 per particle, model space
	std::vector<float>	nrmLocal;						// 3 per particle, model space
	std::vector<float>	scratchTarget, scratchFrom, scratchTo, scratchN;	// reused every frame
	int					outTime;
};

std::vector<mcDef_t>										g_defs;
struct mcMeshEntry_t {
	mcMesh_t	*mesh;					// nullptr = not cloth
	int			numVerts, numTris, index;	// to notice a reused address after a model reload
};
std::unordered_map<const mdxmSurface_t *, mcMeshEntry_t>		g_meshes;
std::unordered_map<unsigned long long, mcInstance_t *>		g_instances;
int															g_lastPrune;

// Local inlined vector helpers: the engine's VectorLength/VectorSubtract/...
// are out-of-line calls in this codebase, and the inner loops below run them
// hundreds of thousands of times per frame.
inline float mcDot( const float *a, const float *b ) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline void  mcSub( const float *a, const float *b, float *o ) { o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2]; }
inline void  mcAdd( const float *a, const float *b, float *o ) { o[0] = a[0] + b[0]; o[1] = a[1] + b[1]; o[2] = a[2] + b[2]; }
inline void  mcMA( const float *a, float s, const float *b, float *o ) { o[0] = a[0] + s * b[0]; o[1] = a[1] + s * b[1]; o[2] = a[2] + s * b[2]; }
inline void  mcScale( const float *a, float s, float *o ) { o[0] = a[0] * s; o[1] = a[1] * s; o[2] = a[2] * s; }
inline void  mcCopy( const float *a, float *o ) { o[0] = a[0]; o[1] = a[1]; o[2] = a[2]; }
inline float mcLen( const float *a ) { return sqrtf( mcDot( a, a ) ); }
inline void  mcCross( const float *a, const float *b, float *o ) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }
inline float mcNorm( float *a ) { float l = mcLen( a ); if ( l > 1e-12f ) { float i = 1.0f / l; a[0] *= i; a[1] *= i; a[2] *= i; } return l; }

inline float Clamp01( float v ) { return v < 0.0f ? 0.0f : ( v > 1.0f ? 1.0f : v ); }

const char *ModelFolder( const char *modelName, char *buf, int bufSize ) {
	// "models/players/palpatine/model" -> "palpatine"
	const char *end = strrchr( modelName, '/' );
	if ( !end ) { Q_strncpyz( buf, modelName, bufSize ); return buf; }
	const char *start = end - 1;
	while ( start > modelName && start[-1] != '/' ) start--;
	int n = (int)( end - start );
	if ( n >= bufSize ) n = bufSize - 1;
	memcpy( buf, start, n );
	buf[n] = 0;
	return buf;
}

float DistToSegment( const float *p, const float *a, const float *b ) {
	vec3_t ab, ap, c;
	mcSub( b, a, ab );
	mcSub( p, a, ap );
	float L = mcDot( ab, ab );
	float t = ( L > 1e-6f ) ? mcDot( ap, ab ) / L : 0.0f;
	t = Clamp01( t );
	mcMA( a, t, ab, c );
	mcSub( p, c, c );
	return mcLen( c );
}

float RuleFreedom( const mcDef_t *d, const float *v ) {
	switch ( d->rule ) {
	case MC_RULE_ABOVE: {
		float span = d->a[0] - d->a[1];
		if ( fabsf( span ) < 0.001f ) return v[2] < d->a[0] ? 1.0f : 0.0f;
		return Clamp01( ( d->a[0] - v[2] ) / span );
	}
	case MC_RULE_NEAR: {
		vec3_t c = { d->a[0], d->a[1], d->a[2] }, t;
		mcSub( v, c, t );
		float r = mcLen( t ), span = d->a[4] - d->a[3];
		return Clamp01( ( r - d->a[3] ) / ( span > 0.001f ? span : 0.001f ) );
	}
	case MC_RULE_SEGMENT: {
		float r = DistToSegment( v, &d->a[0], &d->a[3] ), span = d->a[7] - d->a[6];
		return Clamp01( ( r - d->a[6] ) / ( span > 0.001f ? span : 0.001f ) );
	}
	}
	return 0.0f;
}

const mcDef_t *FindDef( const char *modelName, const char *surfaceName ) {
	char folder[MAX_QPATH];
	ModelFolder( modelName, folder, sizeof( folder ) );
	for ( const mcDef_t &d : g_defs ) {
		if ( !Q_stricmp( d.model.c_str(), folder ) && !Q_stricmp( d.surface.c_str(), surfaceName ) ) return &d;
	}
	return nullptr;
}

const mdxmVertex_t *Verts( const mdxmSurface_t *s ) {
	return reinterpret_cast<const mdxmVertex_t *>( reinterpret_cast<const byte *>( s ) + s->ofsVerts );
}

mcMesh_t *BuildMesh( const mdxmSurface_t *s, const mcDef_t *def ) {
	mcMesh_t *m = new mcMesh_t;
	m->def = def;
	m->numVerts = s->numVerts;
	const mdxmVertex_t *v = Verts( s );

	// weld by bind-pose position
	m->vertToParticle.assign( s->numVerts, -1 );
	std::unordered_map<long long, std::vector<int>> grid;
	auto key = []( const float *p ) {
		long long x = (long long)floorf( p[0] * 100.0f ), y = (long long)floorf( p[1] * 100.0f ), z = (long long)floorf( p[2] * 100.0f );
		return ( x * 73856093LL ) ^ ( y * 19349663LL ) ^ ( z * 83492791LL );
	};
	for ( int i = 0; i < s->numVerts; i++ ) {
		long long k = key( v[i].vertCoords );
		int found = -1;
		for ( int j : grid[k] ) {
			vec3_t d;
			mcSub( v[i].vertCoords, v[j].vertCoords, d );
			if ( mcDot( d, d ) < 1e-6f ) { found = m->vertToParticle[j]; break; }
		}
		if ( found < 0 ) {
			found = (int)m->particleVert.size();
			m->particleVert.push_back( i );
		}
		m->vertToParticle[i] = found;
		grid[k].push_back( i );
	}
	m->numParticles = (int)m->particleVert.size();

	m->freedom.resize( m->numParticles );
	for ( int p = 0; p < m->numParticles; p++ ) {
		m->freedom[p] = RuleFreedom( def, v[m->particleVert[p]].vertCoords );
	}

	// triangles -> structural links, plus bend links across shared edges
	const int *tri = reinterpret_cast<const int *>( reinterpret_cast<const byte *>( s ) + s->ofsTriangles );
	std::unordered_map<unsigned long long, int> edgeOther;	// edge -> opposite particle of first triangle
	std::unordered_map<unsigned long long, char> haveLink;
	auto ekey = []( int a, int b ) { if ( a > b ) std::swap( a, b ); return ( (unsigned long long)a << 32 ) | (unsigned)b; };
	auto addLink = [&]( int a, int b, char bend ) {
		if ( a == b ) return;
		unsigned long long k = ekey( a, b );
		if ( haveLink.count( k ) ) return;
		haveLink[k] = 1;
		m->linkA.push_back( a ); m->linkB.push_back( b ); m->linkBend.push_back( bend );
	};
	for ( int t = 0; t < s->numTriangles; t++ ) {
		int vi[3] = { tri[t * 3], tri[t * 3 + 1], tri[t * 3 + 2] };
		int pi[3];
		for ( int k = 0; k < 3; k++ ) {
			m->vtris.push_back( vi[k] );
			pi[k] = m->vertToParticle[vi[k]];
			m->tris.push_back( pi[k] );
		}
		for ( int k = 0; k < 3; k++ ) {
			int a = pi[k], b = pi[( k + 1 ) % 3], o = pi[( k + 2 ) % 3];
			addLink( a, b, 0 );
			unsigned long long ek = ekey( a, b );
			auto it = edgeOther.find( ek );
			if ( it == edgeOther.end() ) edgeOther[ek] = o;
			else addLink( it->second, o, 1 );
		}
	}
	return m;
}

inline void ToWorld( const float *l, const vec3_t origin, const vec3_t axis[3], float *w ) {
	for ( int k = 0; k < 3; k++ ) w[k] = origin[k] + axis[0][k] * l[0] + axis[1][k] * l[1] + axis[2][k] * l[2];
}

inline void ToLocal( const float *w, const vec3_t origin, const vec3_t axis[3], float *l ) {
	vec3_t d;
	mcSub( w, origin, d );
	for ( int k = 0; k < 3; k++ ) {
		float L2 = mcDot( axis[k], axis[k] );
		l[k] = mcDot( d, axis[k] ) / ( L2 > 1e-8f ? L2 : 1.0f );
	}
}

inline void DirToLocal( const float *w, const vec3_t axis[3], float *l ) {
	for ( int k = 0; k < 3; k++ ) {
		float L2 = mcDot( axis[k], axis[k] );
		l[k] = mcDot( w, axis[k] ) / ( L2 > 1e-8f ? L2 : 1.0f );
	}
}

// Push p out of the body capsules - but never further out than where the
// animation itself puts this vertex (target). Robes are often modelled a
// little inside the capsule shapes; forcing those parts out fights their
// attached neighbours and stretches the mesh.
void Collide( float *p, const float *target, const mcCapsule_t *caps, int n ) {
	for ( int c = 0; c < n; c++ ) {
		vec3_t ab, ap, cl, out;
		mcSub( caps[c].b, caps[c].a, ab );
		float L = mcDot( ab, ab );
		mcSub( p, caps[c].a, ap );
		float t = ( L > 1e-6f ) ? Clamp01( mcDot( ap, ab ) / L ) : 0.0f;
		mcMA( caps[c].a, t, ab, cl );
		mcSub( p, cl, out );
		float d = mcLen( out ), minD = caps[c].r + MC_MARGIN;
		if ( d >= minD || d < 1e-4f ) continue;
		// how deep the animated position already is
		vec3_t at, ac, ao;
		mcSub( target, caps[c].a, at );
		float tt = ( L > 1e-6f ) ? Clamp01( mcDot( at, ab ) / L ) : 0.0f;
		mcMA( caps[c].a, tt, ab, ac );
		mcSub( target, ac, ao );
		float dT = mcLen( ao );
		if ( dT < minD ) minD = dT;
		if ( d < minD ) mcMA( cl, minD / d, out, p );
	}
}

void ParticleNormals( const mcMesh_t *m, const std::vector<float> &pos, std::vector<float> &n ) {
	n.assign( (size_t)m->numParticles * 3, 0.0f );
	for ( size_t t = 0; t + 2 < m->tris.size(); t += 3 ) {
		const float *a = &pos[m->tris[t] * 3], *b = &pos[m->tris[t + 1] * 3], *c = &pos[m->tris[t + 2] * 3];
		vec3_t e1, e2, fn;
		mcSub( b, a, e1 );
		mcSub( c, a, e2 );
		mcCross( e1, e2, fn );
		for ( int k = 0; k < 3; k++ ) {
			float *d = &n[m->tris[t + k] * 3];
			mcAdd( d, fn, d );
		}
	}
	for ( int p = 0; p < m->numParticles; p++ ) mcNorm( &n[p * 3] );
}

void Constrain( const mcMesh_t *m, mcInstance_t *in, const mcCapsule_t *caps, int ncaps, int iters ) {
	const float tether = m->def->tether;
	for ( int it = 0; it < iters; it++ ) {
		for ( size_t l = 0; l < m->linkA.size(); l++ ) {
			int a = m->linkA[l], b = m->linkB[l];
			float wa = m->freedom[a] > 0.0f ? 1.0f : 0.0f, wb = m->freedom[b] > 0.0f ? 1.0f : 0.0f;
			if ( wa + wb == 0.0f ) continue;
			float *pa = &in->p[a * 3], *pb = &in->p[b * 3];
			vec3_t dT, d;
			mcSub( &in->target[b * 3], &in->target[a * 3], dT );
			float rest = mcLen( dT );
			mcSub( pb, pa, d );
			float len = mcLen( d );
			if ( len < 1e-5f ) continue;
			if ( m->linkBend[l] && len <= rest ) continue;	// bend links only resist stretching
			float k = ( len - rest ) / ( len * ( wa + wb ) );
			if ( m->linkBend[l] ) k *= 0.5f;
			mcMA( pa, k * wa, d, pa );
			mcMA( pb, -k * wb, d, pb );
		}
		for ( int p = 0; p < m->numParticles; p++ ) {
			float f = m->freedom[p], *pp = &in->p[p * 3];
			const float *t = &in->target[p * 3];
			if ( f <= 0.0f ) { mcCopy( t, pp ); continue; }
			Collide( pp, t, caps, ncaps );
			// tether: never further from the animated position than freedom * tether
			vec3_t d;
			mcSub( pp, t, d );
			float len = mcLen( d ), maxLen = f * tether;
			if ( len > maxLen && len > 1e-5f ) mcMA( t, maxLen / len, d, pp );
		}
	}
}

// Only the capsules that can reach this surface: bounds of the animated
// positions grown by the tether, against each capsule's bounds.
int CullCapsules( const mcMesh_t *m, const std::vector<float> &target, const mcCapsule_t *caps, int ncaps,
				  mcCapsule_t *out, int maxOut ) {
	vec3_t mins = { 1e9f, 1e9f, 1e9f }, maxs = { -1e9f, -1e9f, -1e9f };
	for ( int p = 0; p < m->numParticles; p++ ) {
		if ( m->freedom[p] <= 0.0f ) continue;
		for ( int k = 0; k < 3; k++ ) {
			float v = target[p * 3 + k];
			if ( v < mins[k] ) mins[k] = v;
			if ( v > maxs[k] ) maxs[k] = v;
		}
	}
	float grow = m->def->tether + MC_MARGIN;
	int n = 0;
	for ( int c = 0; c < ncaps && n < maxOut; c++ ) {
		bool overlap = true;
		for ( int k = 0; k < 3 && overlap; k++ ) {
			float lo = ( caps[c].a[k] < caps[c].b[k] ? caps[c].a[k] : caps[c].b[k] ) - caps[c].r;
			float hi = ( caps[c].a[k] > caps[c].b[k] ? caps[c].a[k] : caps[c].b[k] ) + caps[c].r;
			if ( hi < mins[k] - grow || lo > maxs[k] + grow ) overlap = false;
		}
		if ( overlap ) out[n++] = caps[c];
	}
	return n;
}

void Reset( const mcMesh_t *m, mcInstance_t *in, int time ) {
	in->p = in->target;
	in->prev = in->target;
	in->targetPrev = in->target;
	in->accum = 0;
	in->lastTime = time;
	in->active = true;
}

void Step( const mcMesh_t *m, mcInstance_t *in, const std::vector<float> &tgtFrom, const std::vector<float> &tgtTo,
		   const mcCapsule_t *caps, int ncaps, const mcParams_t *prm ) {
	std::vector<float> &n = in->scratchN;
	ParticleNormals( m, in->p, n );
	const float shape = Clamp01( m->def->shape * prm->shapeScale );
	in->target.assign( tgtTo.begin(), tgtTo.end() );
	for ( int p = 0; p < m->numParticles; p++ ) {
		float f = m->freedom[p], *pp = &in->p[p * 3], *pv = &in->prev[p * 3];
		if ( f <= 0.0f ) {
			mcCopy( &tgtTo[p * 3], pp );
			mcCopy( &tgtFrom[p * 3], pv );
			continue;
		}
		vec3_t v, acc;
		mcSub( pp, pv, v );
		float vn = mcDot( v, &n[p * 3] ) / MC_DT;
		mcScale( &n[p * 3], -prm->drag * vn, acc );
		acc[2] -= prm->gravity;
		mcCopy( pp, pv );
		mcMA( pp, 0.985f, v, pp );
		mcMA( pp, MC_DT * MC_DT, acc, pp );
		// keep the modelled shape: pull a little toward the animated position,
		// more strongly where the vertex is less free
		float s = shape + ( 1.0f - f ) * 0.15f;
		vec3_t d;
		mcSub( &tgtTo[p * 3], pp, d );
		mcMA( pp, s, d, pp );
	}
	Constrain( m, in, caps, ncaps, prm->iterations );
}

} // namespace

int MC_LoadConfig( const char *text ) {
	g_defs.clear();
	MC_Clear();
	COM_ParseSession session;	// the SP parser refuses to run outside a session
	const char *p = text;
	while ( p ) {
		const char *tok = COM_ParseExt( &p, qtrue );
		if ( !tok[0] ) break;
		mcDef_t d;
		d.model = tok;
		tok = COM_ParseExt( &p, qfalse ); if ( !tok[0] ) break;
		d.surface = tok;
		tok = COM_ParseExt( &p, qfalse );
		int need;
		if ( !Q_stricmp( tok, "above" ) ) { d.rule = MC_RULE_ABOVE; need = 2; }
		else if ( !Q_stricmp( tok, "near" ) ) { d.rule = MC_RULE_NEAR; need = 5; }
		else if ( !Q_stricmp( tok, "segment" ) ) { d.rule = MC_RULE_SEGMENT; need = 8; }
		else { while ( COM_ParseExt( &p, qfalse )[0] ) {} continue; }	// unknown rule: skip line
		int k;
		for ( k = 0; k < need; k++ ) {
			tok = COM_ParseExt( &p, qfalse );
			if ( !tok[0] ) break;
			d.a[k] = (float)atof( tok );
		}
		if ( k < need ) continue;
		d.tether = 12.0f;
		d.shape = 0.03f;
		while ( 1 ) {
			tok = COM_ParseExt( &p, qfalse );
			if ( !tok[0] ) break;
			if ( !Q_stricmp( tok, "tether" ) ) d.tether = (float)atof( COM_ParseExt( &p, qfalse ) );
			else if ( !Q_stricmp( tok, "shape" ) ) d.shape = (float)atof( COM_ParseExt( &p, qfalse ) );
		}
		if ( d.tether < 0.0f ) d.tether = 0.0f;
		g_defs.push_back( d );
	}
	return (int)g_defs.size();
}

void MC_Clear( void ) {
	for ( auto &kv : g_meshes ) delete kv.second.mesh;
	g_meshes.clear();
	for ( auto &kv : g_instances ) delete kv.second;
	g_instances.clear();
}

bool MC_IsClothSurface( const void *surfacePtr, const char *modelName, const char *surfaceName ) {
	const mdxmSurface_t *surface = static_cast<const mdxmSurface_t *>( surfacePtr );
	auto it = g_meshes.find( surface );
	if ( it != g_meshes.end() ) {
		const mcMeshEntry_t &e = it->second;
		if ( e.numVerts == surface->numVerts && e.numTris == surface->numTriangles && e.index == surface->thisSurfaceIndex ) {
			return e.mesh != nullptr;
		}
		// same address, different surface: the model was reloaded
		delete e.mesh;
		for ( auto ii = g_instances.begin(); ii != g_instances.end(); ) {
			if ( ii->second->surface == surface ) { delete ii->second; ii = g_instances.erase( ii ); }
			else ++ii;
		}
	}
	const mcDef_t *def = FindDef( modelName, surfaceName );
	mcMeshEntry_t e;
	e.mesh = ( def && surface->numVerts > 0 && surface->numTriangles > 0 ) ? BuildMesh( surface, def ) : nullptr;
	e.numVerts = surface->numVerts;
	e.numTris = surface->numTriangles;
	e.index = surface->thisSurfaceIndex;
	g_meshes[surface] = e;
	return e.mesh != nullptr;
}

void MC_Apply( const void *surfacePtr, const void *instanceKey, int timeMs,
			   const vec3_t origin, const vec3_t axis[3],
			   const mcCapsule_t *caps, int ncaps, const mcParams_t *prm,
			   float (*xyz)[4], float (*normal)[4] ) {
	const mdxmSurface_t *surface = static_cast<const mdxmSurface_t *>( surfacePtr );
	auto mit = g_meshes.find( surface );
	if ( mit == g_meshes.end() || !mit->second.mesh ) return;
	const mcMesh_t *m = mit->second.mesh;
	if ( m->numVerts != surface->numVerts ) return;

	unsigned long long key = (unsigned long long)(size_t)instanceKey * 31ULL ^ (unsigned long long)(size_t)surface;
	mcInstance_t *in;
	auto iit = g_instances.find( key );
	if ( iit == g_instances.end() ) {
		in = new mcInstance_t;
		in->surface = surface;
		in->active = false;
		in->outTime = -1;
		g_instances[key] = in;
	} else {
		in = iit->second;
	}
	in->lastSeen = timeMs;

	// the same surface can be drawn several times a frame (shadows, mirrors):
	// simulate once, then reuse
	if ( in->outTime != timeMs || !in->active ) {
		std::vector<float> &target = in->scratchTarget;
		target.resize( m->numParticles * 3 );
		for ( int p = 0; p < m->numParticles; p++ ) ToWorld( xyz[m->particleVert[p]], origin, axis, &target[p * 3] );

		if ( in->active ) {
			int dt = timeMs - in->lastTime;
			vec3_t d;
			mcSub( &target[0], &in->targetPrev[0], d );
			if ( dt < 0 || dt > MC_STALE_MS || mcLen( d ) > MC_TELEPORT ) in->active = false;
		}
		mcCapsule_t near[32];
		int nnear = CullCapsules( m, target, caps, ncaps, near, 32 );
		if ( !in->active ) {
			in->target.assign( target.begin(), target.end() );
			Reset( m, in, timeMs );
		} else {
			int dt = timeMs - in->lastTime;
			int steps = 0;
			if ( dt > 0 ) {
				in->accum += dt;
				steps = in->accum / MC_STEP_MS;
				if ( steps > MC_MAX_STEPS ) { steps = MC_MAX_STEPS; in->accum = 0; }
				else in->accum -= steps * MC_STEP_MS;
				std::vector<float> &from = in->scratchFrom, &to = in->scratchTo;
				from.resize( target.size() );
				to.resize( target.size() );
				for ( int s = 0; s < steps; s++ ) {
					float t0 = (float)s / steps, t1 = (float)( s + 1 ) / steps;
					for ( size_t i = 0; i < target.size(); i++ ) {
						float d0 = target[i] - in->targetPrev[i];
						from[i] = in->targetPrev[i] + d0 * t0;
						to[i] = in->targetPrev[i] + d0 * t1;
					}
					Step( m, in, from, to, near, nnear, prm );
				}
				in->targetPrev.assign( target.begin(), target.end() );
				in->lastTime = timeMs;
			}
			// frames without a full step still follow the current pose, so a
			// fast limb never ends up inside stale cloth
			if ( steps == 0 ) {
				in->target.assign( target.begin(), target.end() );
				Constrain( m, in, near, nnear, 1 );
			}
		}

		// results back into model space, with normals from the moving triangles
		std::vector<float> &nw = in->scratchN;
		ParticleNormals( m, in->p, nw );
		in->outLocal.resize( m->numParticles * 3 );
		in->nrmLocal.resize( m->numParticles * 3 );
		for ( int p = 0; p < m->numParticles; p++ ) {
			ToLocal( &in->p[p * 3], origin, axis, &in->outLocal[p * 3] );
			DirToLocal( &nw[p * 3], axis, &in->nrmLocal[p * 3] );
			mcNorm( &in->nrmLocal[p * 3] );
		}
		in->outTime = timeMs;
	}

	for ( int i = 0; i < m->numVerts; i++ ) {
		int p = m->vertToParticle[i];
		float f = m->freedom[p];
		if ( f <= 0.0f ) continue;
		mcCopy( &in->outLocal[p * 3], xyz[i] );
		// keep the model's normal direction convention (two-sided seams etc.)
		const float *cn = &in->nrmLocal[p * 3];
		float sgn = mcDot( cn, normal[i] ) < 0.0f ? -1.0f : 1.0f;
		vec3_t nn;
		mcScale( cn, sgn, nn );
		float b = f < 0.25f ? f * 4.0f : 1.0f;		// fade in near the attached edge
		for ( int k = 0; k < 3; k++ ) normal[i][k] = normal[i][k] * ( 1.0f - b ) + nn[k] * b;
		mcNorm( normal[i] );
	}
}

void MC_Prune( int timeMs ) {
	if ( timeMs - g_lastPrune < 1000 && timeMs >= g_lastPrune ) return;
	g_lastPrune = timeMs;
	for ( auto it = g_instances.begin(); it != g_instances.end(); ) {
		if ( timeMs - it->second->lastSeen > MC_FORGET_MS || timeMs < it->second->lastSeen ) {
			delete it->second;
			it = g_instances.erase( it );
		} else {
			++it;
		}
	}
}
