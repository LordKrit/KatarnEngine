/*
===========================================================================
Mesh cloth - see tr_meshcloth.h.

Per model LOD ("group", shared by every character using that model):
  surfaces   every cloth surface of the model, simulated together
  particles  vertices welded by bind position across all those surfaces,
             so UV seams and stitched pieces can't tear apart
  freedom    0 = follows the animation exactly, 1 = fully free; vertices
             touching a non-cloth surface are forced to 0 (closed seams)
  links      triangle edges (structural) and the opposite corners of
             neighbouring triangles (bend); lengths come from the animated
             pose each step, so the modelled folds are kept

Per character instance:
  particle positions (Verlet), fixed 16 ms steps, results per vertex.
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
#include <algorithm>

namespace {

const int	MC_STEP_MS		= 16;
const float	MC_DT			= 0.016f;
const int	MC_MAX_STEPS	= 4;
const int	MC_STALE_MS		= 250;
const float	MC_TELEPORT		= 120.0f;
const int	MC_FORGET_MS	= 10000;
const float	MC_MARGIN		= 0.6f;
const float	MC_WELD			= 0.01f;	// bind-pose distance that counts as the same point

enum mcRuleType_t { MC_RULE_ABOVE, MC_RULE_NEAR, MC_RULE_SEGMENT };

struct mcDef_t {
	std::string		model, surface;
	mcRuleType_t	rule;
	float			a[8];
	float			tether;
	float			shape;
	float			bend;		// 0..1 bending stiffness (0 = limp silk, 1 = stiff leather)
	float			inertia;	// 0..1, or -1 to use r_meshClothInertia
};

struct mcGroup_t {
	std::vector<const mdxmSurface_t *>	surfs;
	std::vector<const mcDef_t *>		defs;
	std::vector<int>					vertBase;		// first global vertex of each surface
	int									numVerts;
	int									numParticles;
	std::vector<int>					vertToParticle;	// global vertex -> particle
	std::vector<int>					partSurf, partVert;	// representative surface / local vertex
	std::vector<float>					freedom, tether, shape, bend, inertia;	// per particle
	std::vector<int>					lraAnchor;		// nearest attached particle along the cloth (-1 none)
	std::vector<float>					lraLen;			// bind-pose distance to it along the cloth
	float								bindLinkSum;	// total bind length of the structural links
	std::vector<int>					linkA, linkB;
	std::vector<char>					linkBend;
	std::vector<int>					tris;			// particle indices
};

struct mcSurfEntry_t {
	mcGroup_t	*group;			// nullptr = not cloth
	int			surfIndex;		// index into group->surfs
	int			numVerts, numTris, index;	// to notice a reused address after a model reload
};

struct mcInstance_t {
	const mcGroup_t		*group;
	bool				active;
	int					lastTime, lastSeen, accum, outTime;
	float				linkScale;		// animated / bind size of the cloth (model scale)
	vec3_t				lastOrigin;		// character origin at the previous simulation
	std::vector<float>	p, prev, target, targetPrev;	// 3 per particle, world
	std::vector<float>	outLocal, nrmLocal;				// 3 per particle, model space
	std::vector<float>	skinNrm;						// 3 per global vertex, model space
	std::vector<float>	scratchTarget, scratchFrom, scratchTo, scratchN;
};

std::vector<mcDef_t>										g_defs;
std::unordered_map<const mdxmSurface_t *, mcSurfEntry_t>	g_surfs;
std::vector<mcGroup_t *>									g_groups;
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
	float t = ( L > 1e-6f ) ? Clamp01( mcDot( ap, ab ) / L ) : 0.0f;
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

const mcDef_t *FindDef( const char *folder, const char *surfaceName ) {
	for ( const mcDef_t &d : g_defs ) {
		if ( !Q_stricmp( d.model.c_str(), folder ) && !Q_stricmp( d.surface.c_str(), surfaceName ) ) return &d;
	}
	return nullptr;
}

const mdxmVertex_t *Verts( const mdxmSurface_t *s ) {
	return reinterpret_cast<const mdxmVertex_t *>( reinterpret_cast<const byte *>( s ) + s->ofsVerts );
}

const mdxmHeader_t *HeaderOf( const mdxmSurface_t *s ) {
	return reinterpret_cast<const mdxmHeader_t *>( reinterpret_cast<const byte *>( s ) + s->ofsHeader );
}

const char *SurfaceName( const mdxmHeader_t *h, int index ) {
	const auto ho = reinterpret_cast<const mdxmHierarchyOffsets_t *>( reinterpret_cast<const byte *>( h ) + sizeof( mdxmHeader_t ) );
	const auto sh = reinterpret_cast<const mdxmSurfHierarchy_t *>( reinterpret_cast<const byte *>( ho ) + ho->offsets[index] );
	return sh->name;
}

// all surfaces of the LOD that contains s
std::vector<const mdxmSurface_t *> LodSurfaces( const mdxmSurface_t *s ) {
	std::vector<const mdxmSurface_t *> out;
	const mdxmHeader_t *h = HeaderOf( s );
	const byte *lod = reinterpret_cast<const byte *>( h ) + h->ofsLODs;
	for ( int l = 0; l < h->numLODs; l++ ) {
		const auto lh = reinterpret_cast<const mdxmLOD_t *>( lod );
		const auto lso = reinterpret_cast<const mdxmLODSurfOffset_t *>( lod + sizeof( mdxmLOD_t ) );
		const byte *end = lod + lh->ofsEnd;
		if ( reinterpret_cast<const byte *>( s ) >= lod && reinterpret_cast<const byte *>( s ) < end ) {
			for ( int i = 0; i < h->numSurfaces; i++ ) {
				out.push_back( reinterpret_cast<const mdxmSurface_t *>( reinterpret_cast<const byte *>( lso ) + lso->offsets[i] ) );
			}
			return out;
		}
		lod = end;
	}
	out.push_back( s );
	return out;
}

struct mcWeldGrid_t {
	std::unordered_map<long long, std::vector<int>> cells;
	std::vector<const float *> pts;
	static long long Key( long long x, long long y, long long z ) { return ( x * 73856093LL ) ^ ( y * 19349663LL ) ^ ( z * 83492791LL ); }
	static long long Cell( float v ) { return (long long)floorf( v * 10.0f ); }	// 0.1 unit cells
	void Add( const float *p, int id ) {
		if ( (int)pts.size() <= id ) pts.resize( id + 1 );
		pts[id] = p;
		cells[Key( Cell( p[0] ), Cell( p[1] ), Cell( p[2] ) )].push_back( id );
	}
	int Find( const float *p ) const {
		long long cx = Cell( p[0] ), cy = Cell( p[1] ), cz = Cell( p[2] );
		for ( long long dx = -1; dx <= 1; dx++ ) for ( long long dy = -1; dy <= 1; dy++ ) for ( long long dz = -1; dz <= 1; dz++ ) {
			auto it = cells.find( Key( cx + dx, cy + dy, cz + dz ) );
			if ( it == cells.end() ) continue;
			for ( int id : it->second ) {
				vec3_t d;
				mcSub( p, pts[id], d );
				if ( mcDot( d, d ) < MC_WELD * MC_WELD ) return id;
			}
		}
		return -1;
	}
};

mcGroup_t *BuildGroup( const mdxmSurface_t *seed, const char *folder ) {
	const mdxmHeader_t *h = HeaderOf( seed );
	std::vector<const mdxmSurface_t *> all = LodSurfaces( seed );
	mcGroup_t *g = new mcGroup_t;
	std::vector<const mdxmSurface_t *> rigid;
	for ( const mdxmSurface_t *s : all ) {
		const char *name = SurfaceName( h, s->thisSurfaceIndex );
		const mcDef_t *def = FindDef( folder, name );
		if ( def && s->numVerts > 0 && s->numTriangles > 0 ) {
			g->surfs.push_back( s );
			g->defs.push_back( def );
		} else if ( name[0] != '*' && !strstr( name, "_off" ) && s->numVerts > 0 ) {
			rigid.push_back( s );	// tags (*name) and dismemberment caps (_off) don't count as seams
		}
	}

	// rigid seam points
	mcWeldGrid_t rigidGrid;
	int rid = 0;
	for ( const mdxmSurface_t *s : rigid ) {
		const mdxmVertex_t *v = Verts( s );
		for ( int i = 0; i < s->numVerts; i++ ) rigidGrid.Add( v[i].vertCoords, rid++ );
	}

	// weld cloth vertices across surfaces
	mcWeldGrid_t grid;
	g->numVerts = 0;
	for ( size_t si = 0; si < g->surfs.size(); si++ ) {
		g->vertBase.push_back( g->numVerts );
		g->numVerts += g->surfs[si]->numVerts;
	}
	g->vertToParticle.assign( g->numVerts, -1 );
	for ( size_t si = 0; si < g->surfs.size(); si++ ) {
		const mdxmSurface_t *s = g->surfs[si];
		const mdxmVertex_t *v = Verts( s );
		for ( int i = 0; i < s->numVerts; i++ ) {
			int p = grid.Find( v[i].vertCoords );
			float f = RuleFreedom( g->defs[si], v[i].vertCoords );
			if ( rigidGrid.Find( v[i].vertCoords ) >= 0 ) f = 0.0f;	// seam with a non-cloth surface
			if ( p < 0 ) {
				p = (int)g->partSurf.size();
				g->partSurf.push_back( (int)si );
				g->partVert.push_back( i );
				g->freedom.push_back( f );
				g->tether.push_back( g->defs[si]->tether );
				g->shape.push_back( g->defs[si]->shape );
				g->bend.push_back( g->defs[si]->bend );
				g->inertia.push_back( g->defs[si]->inertia );
				grid.Add( v[i].vertCoords, p );
			} else {
				// shared by two pieces: the more attached and the shorter tether win
				if ( f < g->freedom[p] ) g->freedom[p] = f;
				if ( g->defs[si]->tether < g->tether[p] ) g->tether[p] = g->defs[si]->tether;
				if ( g->defs[si]->shape > g->shape[p] ) g->shape[p] = g->defs[si]->shape;
				if ( g->defs[si]->bend > g->bend[p] ) g->bend[p] = g->defs[si]->bend;
			}
			g->vertToParticle[g->vertBase[si] + i] = p;
		}
	}
	g->numParticles = (int)g->partSurf.size();
	// almost-attached vertices are attached: the solver has no partial springs
	for ( int p = 0; p < g->numParticles; p++ ) if ( g->freedom[p] < 0.02f ) g->freedom[p] = 0.0f;

	// links from every cloth triangle
	std::unordered_map<unsigned long long, int> edgeOther;
	std::unordered_map<unsigned long long, char> haveLink;
	auto ekey = []( int a, int b ) { if ( a > b ) { int t = a; a = b; b = t; } return ( (unsigned long long)a << 32 ) | (unsigned)b; };
	auto addLink = [&]( int a, int b, char bend ) {
		if ( a == b ) return;
		unsigned long long k = ekey( a, b );
		if ( haveLink.count( k ) ) return;
		haveLink[k] = 1;
		g->linkA.push_back( a ); g->linkB.push_back( b ); g->linkBend.push_back( bend );
	};
	for ( size_t si = 0; si < g->surfs.size(); si++ ) {
		const mdxmSurface_t *s = g->surfs[si];
		const int *tri = reinterpret_cast<const int *>( reinterpret_cast<const byte *>( s ) + s->ofsTriangles );
		for ( int t = 0; t < s->numTriangles; t++ ) {
			int pi[3];
			for ( int k = 0; k < 3; k++ ) {
				pi[k] = g->vertToParticle[g->vertBase[si] + tri[t * 3 + k]];
				g->tris.push_back( pi[k] );
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
	}

	// long range attachments (Kim, Chentanez & Mueller 2012): each free
	// particle's nearest attached particle measured along the cloth, so the
	// solver can forbid it from ever getting further away than that
	std::vector<const float *> bindPos( g->numParticles );
	for ( int p = 0; p < g->numParticles; p++ ) bindPos[p] = Verts( g->surfs[g->partSurf[p]] )[g->partVert[p]].vertCoords;
	std::vector<std::vector<std::pair<int, float>>> adj( g->numParticles );
	g->bindLinkSum = 0.0f;
	for ( size_t l = 0; l < g->linkA.size(); l++ ) {
		if ( g->linkBend[l] ) continue;
		vec3_t d;
		mcSub( bindPos[g->linkA[l]], bindPos[g->linkB[l]], d );
		const float len = mcLen( d );
		g->bindLinkSum += len;
		adj[g->linkA[l]].push_back( { g->linkB[l], len } );
		adj[g->linkB[l]].push_back( { g->linkA[l], len } );
	}
	g->lraAnchor.assign( g->numParticles, -1 );
	g->lraLen.assign( g->numParticles, 1e9f );
	std::vector<std::pair<float, int>> heap;	// min-heap by distance
	auto cmp = []( const std::pair<float, int> &x, const std::pair<float, int> &y ) { return x.first > y.first; };
	for ( int p = 0; p < g->numParticles; p++ ) {
		if ( g->freedom[p] > 0.0f ) continue;
		g->lraAnchor[p] = p;
		g->lraLen[p] = 0.0f;
		heap.push_back( { 0.0f, p } );
	}
	std::make_heap( heap.begin(), heap.end(), cmp );
	while ( !heap.empty() ) {
		std::pop_heap( heap.begin(), heap.end(), cmp );
		const std::pair<float, int> top = heap.back();
		heap.pop_back();
		if ( top.first > g->lraLen[top.second] ) continue;
		for ( const auto &e : adj[top.second] ) {
			const float nd = top.first + e.second;
			if ( nd < g->lraLen[e.first] ) {
				g->lraLen[e.first] = nd;
				g->lraAnchor[e.first] = g->lraAnchor[top.second];
				heap.push_back( { nd, e.first } );
				std::push_heap( heap.begin(), heap.end(), cmp );
			}
		}
	}
	for ( int p = 0; p < g->numParticles; p++ ) if ( g->lraAnchor[p] < 0 ) g->lraLen[p] = 0.0f;
	return g;
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

// Skin one vertex exactly like RB_SurfaceGhoul.
void SkinVertex( const mdxmSurface_t *s, int i, mcBoneFn_t boneFn, void *ctx, float *outPos, float *outNrm ) {
	const mdxmVertex_t *v = &Verts( s )[i];
	const int *refs = reinterpret_cast<const int *>( reinterpret_cast<const byte *>( s ) + s->ofsBoneReferences );
	const int nw = G2_GetVertWeights( v );
	float tot = 0.0f;
	outPos[0] = outPos[1] = outPos[2] = 0.0f;
	outNrm[0] = outNrm[1] = outNrm[2] = 0.0f;
	for ( int k = 0; k < nw; k++ ) {
		const float w = G2_GetVertBoneWeight( v, k, tot, nw );
		const float (*m)[4] = boneFn( ctx, refs[G2_GetVertBoneIndex( v, k )] );
		for ( int r = 0; r < 3; r++ ) {
			outPos[r] += w * ( m[r][0] * v->vertCoords[0] + m[r][1] * v->vertCoords[1] + m[r][2] * v->vertCoords[2] + m[r][3] );
			outNrm[r] += w * ( m[r][0] * v->normal[0] + m[r][1] * v->normal[1] + m[r][2] * v->normal[2] );
		}
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

void ParticleNormals( const mcGroup_t *g, const std::vector<float> &pos, std::vector<float> &n ) {
	n.assign( (size_t)g->numParticles * 3, 0.0f );
	for ( size_t t = 0; t + 2 < g->tris.size(); t += 3 ) {
		const float *a = &pos[g->tris[t] * 3], *b = &pos[g->tris[t + 1] * 3], *c = &pos[g->tris[t + 2] * 3];
		vec3_t e1, e2, fn;
		mcSub( b, a, e1 );
		mcSub( c, a, e2 );
		mcCross( e1, e2, fn );
		for ( int k = 0; k < 3; k++ ) {
			float *d = &n[g->tris[t + k] * 3];
			mcAdd( d, fn, d );
		}
	}
	for ( int p = 0; p < g->numParticles; p++ ) mcNorm( &n[p * 3] );
}

// Only the capsules that can reach the cloth: bounds of the animated free
// positions grown by the tether, against each capsule's bounds.
int CullCapsules( const mcGroup_t *g, const std::vector<float> &target, const mcCapsule_t *caps, int ncaps,
				  mcCapsule_t *out, int maxOut ) {
	vec3_t mins = { 1e9f, 1e9f, 1e9f }, maxs = { -1e9f, -1e9f, -1e9f };
	float grow = MC_MARGIN;
	for ( int p = 0; p < g->numParticles; p++ ) {
		if ( g->freedom[p] <= 0.0f ) continue;
		if ( g->tether[p] + MC_MARGIN > grow ) grow = g->tether[p] + MC_MARGIN;
		for ( int k = 0; k < 3; k++ ) {
			float v = target[p * 3 + k];
			if ( v < mins[k] ) mins[k] = v;
			if ( v > maxs[k] ) maxs[k] = v;
		}
	}
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

// One pass over every constraint, Gauss-Seidel. bendK is the per-substep
// fraction of the bend error removed (stiffness made independent of the
// substep count by the caller).
void SolveConstraints( const mcGroup_t *g, mcInstance_t *in, const mcCapsule_t *caps, int ncaps, float bendScale ) {
	// stretch: the fabric keeps the edge lengths the animated mesh has now
	// (rigidly - real cloth barely stretches); bend links resist folding
	for ( size_t l = 0; l < g->linkA.size(); l++ ) {
		const int a = g->linkA[l], b = g->linkB[l];
		const float wa = g->freedom[a] > 0.0f ? 1.0f : 0.0f, wb = g->freedom[b] > 0.0f ? 1.0f : 0.0f;
		if ( wa + wb == 0.0f ) continue;
		float *pa = &in->p[a * 3], *pb = &in->p[b * 3];
		vec3_t dT, d;
		mcSub( &in->target[b * 3], &in->target[a * 3], dT );
		const float rest = mcLen( dT );
		mcSub( pb, pa, d );
		const float len = mcLen( d );
		if ( len < 1e-5f ) continue;
		float k = ( len - rest ) / ( len * ( wa + wb ) );
		if ( g->linkBend[l] ) {
			const float bs = 0.5f * ( g->bend[a] + g->bend[b] );
			k *= bs * bendScale;
			if ( k == 0.0f ) continue;
		}
		mcMA( pa, k * wa, d, pa );
		mcMA( pb, -k * wb, d, pb );
	}
	for ( int p = 0; p < g->numParticles; p++ ) {
		const float f = g->freedom[p];
		float *pp = &in->p[p * 3];
		const float *t = &in->target[p * 3];
		if ( f <= 0.0f ) { mcCopy( t, pp ); continue; }
		// long range attachment: never further from the attached edge than
		// the cloth itself is long
		const int an = g->lraAnchor[p];
		if ( an >= 0 ) {
			vec3_t d;
			mcSub( pp, &in->p[an * 3], d );
			// skinning can stretch the animated mesh past its bind size
			// (raised shoulders): never limit tighter than the pose itself
			vec3_t dt;
			mcSub( t, &in->target[an * 3], dt );
			float maxLen = g->lraLen[p] * in->linkScale;
			const float animLen = mcLen( dt );
			if ( animLen > maxLen ) maxLen = animLen;
			maxLen *= 1.03f;
			const float len = mcLen( d );
			if ( len > maxLen && len > 1e-5f ) mcMA( &in->p[an * 3], maxLen / len, d, pp );
		}
		Collide( pp, t, caps, ncaps );
		// max distance from the animated position (a safety net, as in game
		// cloth tools - not a spring)
		vec3_t d;
		mcSub( pp, t, d );
		const float len = mcLen( d ), maxLen = f * g->tether[p];
		if ( len > maxLen && len > 1e-5f ) mcMA( t, maxLen / len, d, pp );
	}
}

void Reset( mcInstance_t *in, int time ) {
	in->p = in->target;
	in->prev = in->target;
	in->targetPrev = in->target;
	in->accum = 0;
	in->lastTime = time;
	in->active = true;
	in->linkScale = 1.0f;
	in->lastOrigin[0] = in->lastOrigin[1] = in->lastOrigin[2] = 0.0f;
}

// One 16 ms step, split into substeps ("Small Steps in Physics
// Simulation", Macklin et al. 2019): many short steps with a single
// constraint pass each keep the fabric from stretching or wobbling far
// better than one long step with many passes.
void Step( const mcGroup_t *g, mcInstance_t *in, const std::vector<float> &tgtFrom, const std::vector<float> &tgtTo,
		   const mcCapsule_t *caps, int ncaps, const mcParams_t *prm ) {
	const int sub = prm->iterations < 1 ? 1 : ( prm->iterations > 16 ? 16 : prm->iterations );
	const float h = MC_DT / sub;
	const float damp = powf( 0.99f, 1.0f / sub );			// 1% velocity loss per 16 ms
	// bend stiffness per substep so the total doesn't depend on the count
	const float bendScale = 1.0f - powf( 0.5f, 1.0f / sub );
	std::vector<float> &n = in->scratchN;

	// the cloth's current size against its bind pose (model scale)
	float animSum = 0.0f;
	for ( size_t l = 0; l < g->linkA.size(); l++ ) {
		if ( g->linkBend[l] ) continue;
		vec3_t d;
		mcSub( &tgtTo[g->linkB[l] * 3], &tgtTo[g->linkA[l] * 3], d );
		animSum += mcLen( d );
	}
	in->linkScale = g->bindLinkSum > 1e-3f ? animSum / g->bindLinkSum : 1.0f;

	in->target.resize( tgtTo.size() );
	for ( int s = 0; s < sub; s++ ) {
		const float t1 = (float)( s + 1 ) / sub;
		for ( size_t i = 0; i < tgtTo.size(); i++ ) in->target[i] = tgtFrom[i] + ( tgtTo[i] - tgtFrom[i] ) * t1;
		ParticleNormals( g, in->p, n );
		for ( int p = 0; p < g->numParticles; p++ ) {
			const float f = g->freedom[p];
			float *pp = &in->p[p * 3], *pv = &in->prev[p * 3];
			if ( f <= 0.0f ) {
				mcCopy( pp, pv );
				mcCopy( &in->target[p * 3], pp );
				continue;
			}
			vec3_t v;
			mcSub( pp, pv, v );
			mcScale( v, damp, v );
			// air resistance: the part of the motion straight through the
			// cloth is slowed, sliding along it isn't (why capes billow)
			const float *nn = &n[p * 3];
			const float vn = mcDot( v, nn );
			float dr = prm->drag * h;
			if ( dr > 1.0f ) dr = 1.0f;
			mcMA( v, -vn * dr, nn, v );
			mcCopy( pp, pv );
			mcAdd( pp, v, pp );
			pp[2] -= prm->gravity * h * h;
			// optional pull toward the modelled shape (off unless configured)
			const float sh = g->shape[p] * prm->shapeScale;
			if ( sh > 0.0f ) {
				const float k = 1.0f - powf( 1.0f - Clamp01( sh ), 1.0f / sub );
				vec3_t d;
				mcSub( &in->target[p * 3], pp, d );
				mcMA( pp, k, d, pp );
			}
		}
		SolveConstraints( g, in, caps, ncaps, bendScale );
	}
}

void Simulate( const mcGroup_t *g, mcInstance_t *in, int timeMs, const vec3_t origin, const vec3_t axis[3],
			   const mcCapsule_t *caps, int ncaps, const mcParams_t *prm, mcBoneFn_t boneFn, void *boneCtx ) {
	// animated (target) positions of every particle, and skinned normals of every vertex
	std::vector<float> &target = in->scratchTarget;
	target.resize( (size_t)g->numParticles * 3 );
	in->skinNrm.resize( (size_t)g->numVerts * 3 );
	std::vector<char> done( g->numParticles, 0 );
	for ( size_t si = 0; si < g->surfs.size(); si++ ) {
		const mdxmSurface_t *s = g->surfs[si];
		for ( int i = 0; i < s->numVerts; i++ ) {
			const int gv = g->vertBase[si] + i, p = g->vertToParticle[gv];
			vec3_t pos;
			SkinVertex( s, i, boneFn, boneCtx, pos, &in->skinNrm[gv * 3] );
			if ( !done[p] ) { ToWorld( pos, origin, axis, &target[p * 3] ); done[p] = 1; }
		}
	}

	if ( in->active ) {
		const int dt = timeMs - in->lastTime;
		vec3_t d;
		mcSub( &target[0], &in->targetPrev[0], d );
		if ( dt < 0 || dt > MC_STALE_MS || mcLen( d ) > MC_TELEPORT ) in->active = false;
	}
	mcCapsule_t nearCaps[32];
	const int nnear = CullCapsules( g, target, caps, ncaps, nearCaps, 32 );
	if ( !in->active ) {
		in->target.assign( target.begin(), target.end() );
		Reset( in, timeMs );
		mcCopy( origin, in->lastOrigin );
	} else {
		// inertia scale (as in game cloth tools): only part of the
		// character's travel through the world reaches the cloth, so a
		// running character's robe trails instead of being left behind
		vec3_t dO;
		mcSub( origin, in->lastOrigin, dO );
		if ( mcDot( dO, dO ) < MC_TELEPORT * MC_TELEPORT ) {
			for ( int p = 0; p < g->numParticles; p++ ) {
				if ( g->freedom[p] <= 0.0f ) continue;
				const float carry = 1.0f - Clamp01( g->inertia[p] >= 0.0f ? g->inertia[p] : prm->inertia );
				if ( carry <= 0.0f ) continue;
				mcMA( &in->p[p * 3], carry, dO, &in->p[p * 3] );
				mcMA( &in->prev[p * 3], carry, dO, &in->prev[p * 3] );
			}
		}
		mcCopy( origin, in->lastOrigin );
		const int dt = timeMs - in->lastTime;
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
				const float t0 = (float)s / steps, t1 = (float)( s + 1 ) / steps;
				for ( size_t i = 0; i < target.size(); i++ ) {
					const float d0 = target[i] - in->targetPrev[i];
					from[i] = in->targetPrev[i] + d0 * t0;
					to[i] = in->targetPrev[i] + d0 * t1;
				}
				Step( g, in, from, to, nearCaps, nnear, prm );
			}
			in->targetPrev.assign( target.begin(), target.end() );
			in->lastTime = timeMs;
		}
		// frames without a full step still follow the current pose, so a
		// fast limb never ends up inside stale cloth
		if ( steps == 0 ) {
			in->target.assign( target.begin(), target.end() );
			SolveConstraints( g, in, nearCaps, nnear, 0.0f );
		}
	}

	// results back into model space, with normals from the moving triangles
	std::vector<float> &nw = in->scratchN;
	ParticleNormals( g, in->p, nw );
	in->outLocal.resize( (size_t)g->numParticles * 3 );
	in->nrmLocal.resize( (size_t)g->numParticles * 3 );
	for ( int p = 0; p < g->numParticles; p++ ) {
		ToLocal( &in->p[p * 3], origin, axis, &in->outLocal[p * 3] );
		DirToLocal( &nw[p * 3], axis, &in->nrmLocal[p * 3] );
		mcNorm( &in->nrmLocal[p * 3] );
	}
	in->outTime = timeMs;
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
		d.shape = 0.0f;
		d.bend = 0.3f;
		d.inertia = -1.0f;
		while ( 1 ) {
			tok = COM_ParseExt( &p, qfalse );
			if ( !tok[0] ) break;
			if ( !Q_stricmp( tok, "tether" ) ) d.tether = (float)atof( COM_ParseExt( &p, qfalse ) );
			else if ( !Q_stricmp( tok, "shape" ) ) d.shape = (float)atof( COM_ParseExt( &p, qfalse ) );
			else if ( !Q_stricmp( tok, "inertia" ) ) d.inertia = Clamp01( (float)atof( COM_ParseExt( &p, qfalse ) ) );
			else if ( !Q_stricmp( tok, "bend" ) ) d.bend = Clamp01( (float)atof( COM_ParseExt( &p, qfalse ) ) );
		}
		if ( d.tether < 0.0f ) d.tether = 0.0f;
		g_defs.push_back( d );
	}
	return (int)g_defs.size();
}

void MC_Clear( void ) {
	for ( mcGroup_t *g : g_groups ) delete g;
	g_groups.clear();
	g_surfs.clear();
	for ( auto &kv : g_instances ) delete kv.second;
	g_instances.clear();
}

bool MC_IsClothSurface( const void *surfacePtr, const char *modelName, const char *surfaceName ) {
	const mdxmSurface_t *surface = static_cast<const mdxmSurface_t *>( surfacePtr );
	auto it = g_surfs.find( surface );
	if ( it != g_surfs.end() ) {
		const mcSurfEntry_t &e = it->second;
		if ( e.numVerts == surface->numVerts && e.numTris == surface->numTriangles && e.index == surface->thisSurfaceIndex ) {
			return e.group != nullptr;
		}
		// same address, different surface: models were reloaded - start over
		std::vector<mcDef_t> keep = g_defs;
		MC_Clear();
		g_defs = keep;
	}

	char folder[MAX_QPATH];
	ModelFolder( modelName, folder, sizeof( folder ) );
	mcSurfEntry_t e;
	e.group = nullptr;
	e.surfIndex = -1;
	e.numVerts = surface->numVerts;
	e.numTris = surface->numTriangles;
	e.index = surface->thisSurfaceIndex;
	if ( FindDef( folder, surfaceName ) ) {
		mcGroup_t *g = BuildGroup( surface, folder );
		g_groups.push_back( g );
		for ( size_t si = 0; si < g->surfs.size(); si++ ) {
			mcSurfEntry_t ge;
			ge.group = g;
			ge.surfIndex = (int)si;
			ge.numVerts = g->surfs[si]->numVerts;
			ge.numTris = g->surfs[si]->numTriangles;
			ge.index = g->surfs[si]->thisSurfaceIndex;
			g_surfs[g->surfs[si]] = ge;
		}
		auto gi = g_surfs.find( surface );
		if ( gi != g_surfs.end() ) return gi->second.group != nullptr;
	}
	g_surfs[surface] = e;
	return false;
}

void MC_Apply( const void *surfacePtr, const void *instanceKey, int timeMs,
			   const vec3_t origin, const vec3_t axis[3],
			   const mcCapsule_t *caps, int ncaps, const mcParams_t *prm,
			   mcBoneFn_t boneFn, void *boneCtx,
			   float (*xyz)[4], float (*normal)[4] ) {
	const mdxmSurface_t *surface = static_cast<const mdxmSurface_t *>( surfacePtr );
	auto sit = g_surfs.find( surface );
	if ( sit == g_surfs.end() || !sit->second.group ) return;
	const mcGroup_t *g = sit->second.group;
	const int si = sit->second.surfIndex;
	if ( g->surfs[si]->numVerts != surface->numVerts ) return;

	const unsigned long long key = (unsigned long long)(size_t)instanceKey * 31ULL ^ (unsigned long long)(size_t)g;
	mcInstance_t *in;
	auto iit = g_instances.find( key );
	if ( iit == g_instances.end() ) {
		in = new mcInstance_t;
		in->group = g;
		in->active = false;
		in->outTime = -1;
		g_instances[key] = in;
	} else {
		in = iit->second;
	}
	in->lastSeen = timeMs;

	// the first cloth surface drawn this frame simulates the whole model;
	// the others (and shadow/mirror passes) reuse the result
	if ( in->outTime != timeMs || !in->active ) {
		Simulate( g, in, timeMs, origin, axis, caps, ncaps, prm, boneFn, boneCtx );
	}

	const int base = g->vertBase[si];
	for ( int i = 0; i < surface->numVerts; i++ ) {
		const int p = g->vertToParticle[base + i];
		const float f = g->freedom[p];
		if ( f <= 0.0f ) continue;
		mcCopy( &in->outLocal[p * 3], xyz[i] );
		// keep this vertex's side of the surface (two-sided robes, seams)
		const float *cn = &in->nrmLocal[p * 3];
		const float *sn = &in->skinNrm[( base + i ) * 3];
		const float sgn = mcDot( cn, sn ) < 0.0f ? -1.0f : 1.0f;
		const float b = f < 0.25f ? f * 4.0f : 1.0f;		// fade in near the attached edge
		for ( int k = 0; k < 3; k++ ) normal[i][k] = normal[i][k] * ( 1.0f - b ) + cn[k] * sgn * b;
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
