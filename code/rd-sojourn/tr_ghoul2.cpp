#include "client/client.h"	//FIXME!! EVIL - just include the definitions needed
#include "tr_local.h"
#include "qcommon/matcomp.h"
#include "qcommon/qcommon.h"
#include "ghoul2/G2.h"
#include "compose_models.h"
#ifndef REND2_SP
#include "ghoul2/g2_local.h"
#endif // !REND2_SP
#ifdef _G2_GORE
#include "G2_gore_r2.h"
#endif

#ifdef _MSC_VER
#pragma warning (disable: 4512)	//default assignment operator could not be gened
#endif
#ifndef REND2_SP
//sj
#endif // !REND2_SP
//sj
#include "qcommon/md_animsets.h"
#include "../../code/rd-vanilla/tr_meshcloth.h"
#include <vector>
#include <unordered_map>

#define	LL(x) x=LittleLong(x)

#ifdef G2_PERFORMANCE_ANALYSIS
#include "qcommon/timing.h"

timing_c G2PerformanceTimer_RenderSurfaces;
timing_c G2PerformanceTimer_R_AddGHOULSurfaces;
timing_c G2PerformanceTimer_G2_TransformGhoulBones;
timing_c G2PerformanceTimer_G2_ProcessGeneratedSurfaceBolts;
timing_c G2PerformanceTimer_ProcessModelBoltSurfaces;
timing_c G2PerformanceTimer_G2_ConstructGhoulSkeleton;
timing_c G2PerformanceTimer_RB_SurfaceGhoul;
timing_c G2PerformanceTimer_G2_SetupModelPointers;
timing_c G2PerformanceTimer_PreciseFrame;

int G2PerformanceCounter_G2_TransformGhoulBones = 0;

int G2Time_RenderSurfaces = 0;
int G2Time_R_AddGHOULSurfaces = 0;
int G2Time_G2_TransformGhoulBones = 0;
int G2Time_G2_ProcessGeneratedSurfaceBolts = 0;
int G2Time_ProcessModelBoltSurfaces = 0;
int G2Time_G2_ConstructGhoulSkeleton = 0;
int G2Time_RB_SurfaceGhoul = 0;
int G2Time_G2_SetupModelPointers = 0;
int G2Time_PreciseFrame = 0;

void G2Time_ResetTimers(void)
{
	G2Time_RenderSurfaces = 0;
	G2Time_R_AddGHOULSurfaces = 0;
	G2Time_G2_TransformGhoulBones = 0;
	G2Time_G2_ProcessGeneratedSurfaceBolts = 0;
	G2Time_ProcessModelBoltSurfaces = 0;
	G2Time_G2_ConstructGhoulSkeleton = 0;
	G2Time_RB_SurfaceGhoul = 0;
	G2Time_G2_SetupModelPointers = 0;
	G2Time_PreciseFrame = 0;
	G2PerformanceCounter_G2_TransformGhoulBones = 0;
}

void G2Time_ReportTimers(void)
{
	Com_Printf("\n---------------------------------\nRenderSurfaces: %i\nR_AddGhoulSurfaces: %i\nG2_TransformGhoulBones: %i\nG2_ProcessGeneratedSurfaceBolts: %i\nProcessModelBoltSurfaces: %i\nG2_ConstructGhoulSkeleton: %i\nRB_SurfaceGhoul: %i\nG2_SetupModelPointers: %i\n\nPrecise frame time: %i\nTransformGhoulBones calls: %i\n---------------------------------\n\n",
		G2Time_RenderSurfaces,
		G2Time_R_AddGHOULSurfaces,
		G2Time_G2_TransformGhoulBones,
		G2Time_G2_ProcessGeneratedSurfaceBolts,
		G2Time_ProcessModelBoltSurfaces,
		G2Time_G2_ConstructGhoulSkeleton,
		G2Time_RB_SurfaceGhoul,
		G2Time_G2_SetupModelPointers,
		G2Time_PreciseFrame,
		G2PerformanceCounter_G2_TransformGhoulBones
	);
}
#endif

//rww - RAGDOLL_BEGIN
#ifdef __linux__
#include <math.h>
#else
#include <float.h>
#endif

//rww - RAGDOLL_END

#ifdef REND2_SP
extern cvar_t* sv_mapname;
#endif

static const int MAX_RENDERABLE_SURFACES = 4096;
static CRenderableSurface renderSurfHeap[MAX_RENDERABLE_SURFACES];
static int currentRenderSurfIndex = 0;

static CRenderableSurface* AllocGhoul2RenderableSurface()
{
	if (currentRenderSurfIndex >= MAX_RENDERABLE_SURFACES - 1)
	{
		ResetGhoul2RenderableSurfaceHeap();
		ri.Printf(PRINT_DEVELOPER, "AllocRenderableSurface: Reached maximum number of Ghoul2 renderable surfaces (%d)\n", MAX_RENDERABLE_SURFACES);
	}

	CRenderableSurface* rs = &renderSurfHeap[currentRenderSurfIndex++];

	rs->Init();

	return rs;
}

void ResetGhoul2RenderableSurfaceHeap()
{
	currentRenderSurfIndex = 0;
}

bool HackadelicOnClient = false; // means this is a render traversal

bool G2_SetupModelPointers(CGhoul2Info* ghlInfo);
bool G2_SetupModelPointers(CGhoul2Info_v& ghoul2);

extern cvar_t* r_Ghoul2AnimSmooth;
extern cvar_t* r_Ghoul2UnSqashAfterSmooth;

static const mdxaBone_t identityMatrix =
{
	{
		{ 0.0f, -1.0f, 0.0f, 0.0f },
		{ 1.0f, 0.0f, 0.0f, 0.0f },
		{ 0.0f, 0.0f, 1.0f, 0.0f }
	}
};

// I hate doing this, but this is the simplest way to get this into the routines it needs to be
mdxaBone_t		worldMatrix;
mdxaBone_t		worldMatrixInv;
#ifdef _G2_GORE
qhandle_t		goreShader = -1;
#endif

// nasty little matrix multiply going on here..
void Mat3x4_Multiply(mdxaBone_t* out, const mdxaBone_t* in2, const mdxaBone_t* in)
{
	assert(out != nullptr);
	assert(in2 != nullptr);
	assert(in != nullptr);

	// Let's say we are doing R = N * M
	const float n00 = in2->matrix[0][0];
	const float n01 = in2->matrix[0][1];
	const float n02 = in2->matrix[0][2];
	const float n03 = in2->matrix[0][3];
	const float n10 = in2->matrix[1][0];
	const float n11 = in2->matrix[1][1];
	const float n12 = in2->matrix[1][2];
	const float n13 = in2->matrix[1][3];
	const float n20 = in2->matrix[2][0];
	const float n21 = in2->matrix[2][1];
	const float n22 = in2->matrix[2][2];
	const float n23 = in2->matrix[2][3];

	const float m00 = in->matrix[0][0];
	const float m01 = in->matrix[0][1];
	const float m02 = in->matrix[0][2];
	const float m03 = in->matrix[0][3];
	const float m10 = in->matrix[1][0];
	const float m11 = in->matrix[1][1];
	const float m12 = in->matrix[1][2];
	const float m13 = in->matrix[1][3];
	const float m20 = in->matrix[2][0];
	const float m21 = in->matrix[2][1];
	const float m22 = in->matrix[2][2];
	const float m23 = in->matrix[2][3];

	// first row of out
	out->matrix[0][0] = (n00 * m00) + (n01 * m10) + (n02 * m20);
	out->matrix[0][1] = (n00 * m01) + (n01 * m11) + (n02 * m21);
	out->matrix[0][2] = (n00 * m02) + (n01 * m12) + (n02 * m22);
	out->matrix[0][3] = (n00 * m03) + (n01 * m13) + (n02 * m23) + n03;

	// second row of outf out
	out->matrix[1][0] = (n10 * m00) + (n11 * m10) + (n12 * m20);
	out->matrix[1][1] = (n10 * m01) + (n11 * m11) + (n12 * m21);
	out->matrix[1][2] = (n10 * m02) + (n11 * m12) + (n12 * m22);
	out->matrix[1][3] = (n10 * m03) + (n11 * m13) + (n12 * m23) + n13;

	// third row of out  out
	out->matrix[2][0] = (n20 * m00) + (n21 * m10) + (n22 * m20);
	out->matrix[2][1] = (n20 * m01) + (n21 * m11) + (n22 * m21);
	out->matrix[2][2] = (n20 * m02) + (n21 * m12) + (n22 * m22);
	out->matrix[2][3] = (n20 * m03) + (n21 * m13) + (n22 * m23) + n23;
}

void Mat3x4_Scale(mdxaBone_t* result, const mdxaBone_t* lhs, const float scale)
{
	for (int i = 0; i < 3; ++i)
	{
		for (int j = 0; j < 3; ++j)
		{
			result->matrix[i][j] = lhs->matrix[i][j] * scale;
		}
	}
}

void Mat3x4_Lerp(
	mdxaBone_t* result,
	const mdxaBone_t* lhs,
	const mdxaBone_t* rhs,
	const float t)
{
	for (int i = 0; i < 3; ++i)
	{
		for (int j = 0; j < 4; ++j)
		{
			result->matrix[i][j] = lhs->matrix[i][j] * t + rhs->matrix[i][j] * (1.0f - t);
		}
	}
}

const mdxaBone_t operator +(const mdxaBone_t& lhs, const mdxaBone_t& rhs)
{
	mdxaBone_t result{};
	for (int i = 0; i < 3; ++i)
	{
		for (int j = 0; j < 4; ++j)
		{
			result.matrix[i][j] = lhs.matrix[i][j] + rhs.matrix[i][j];
		}
	}
	return result;
}

const mdxaBone_t operator -(const mdxaBone_t& lhs, const mdxaBone_t& rhs)
{
	mdxaBone_t result{};
	for (int i = 0; i < 3; ++i)
	{
		for (int j = 0; j < 4; ++j)
		{
			result.matrix[i][j] = lhs.matrix[i][j] - rhs.matrix[i][j];
		}
	}
	return result;
}

const mdxaBone_t operator *(const mdxaBone_t& lhs, const mdxaBone_t& rhs)
{
	mdxaBone_t result;
	Mat3x4_Multiply(&result, &lhs, &rhs);
	return result;
}

const mdxaBone_t operator *(const mdxaBone_t& lhs, const float scale)
{
	mdxaBone_t result{};
	for (int i = 0; i < 3; ++i)
	{
		for (int j = 0; j < 4; ++j)
		{
			result.matrix[i][j] = lhs.matrix[i][j] * scale;
		}
	}
	return result;
}

const mdxaBone_t operator *(const float scale, const mdxaBone_t& rhs)
{
	return rhs * scale;
}

class CConstructBoneList
{
public:
	int				surfaceNum;
	int* boneUsedList;
	surfaceInfo_v& rootSList;
	model_t* currentModel;
	boneInfo_v& boneList;

	CConstructBoneList(
		int initsurfaceNum,
		int* initboneUsedList,
		surfaceInfo_v& initrootSList,
		model_t* initcurrentModel,
		boneInfo_v& initboneList)
		: surfaceNum(initsurfaceNum)
		, boneUsedList(initboneUsedList)
		, rootSList(initrootSList)
		, currentModel(initcurrentModel)
		, boneList(initboneList)
	{}
};

class CTransformBone
{
public:
	int touch; // for minimal recalculation
	int touchRender;
	mdxaBone_t boneMatrix; //final matrix
	int parent; // only set once

	CTransformBone()
		: touch(0)
		, touchRender(0)
		, boneMatrix()
		, parent(0)
	{}
};

struct SBoneCalc
{
	int newFrame;
	int	currentFrame;
	float backlerp;
	float blendFrame;
	int blendOldFrame;
	bool blendMode;
	float blendLerp;
};


// Face rigs (bones named FACIAL_* / eyeball_*) sit tightly inside the head: smoothing each of their
// matrices on its own (a component-wise lerp, then re-normalised about the bone's own pivot) pulls
// them apart from the head during fast moves - the jaw pokes out and the head warps on strikes. Keep
// them rigid with their parent instead: smoothed = smoothed(parent) * final(parent)^-1 * final(bone).
static bool G2_IsRigidFaceBone(const char* name)
{
	return !Q_stricmpn(name, "FACIAL_", 7) || !Q_stricmpn(name, "eyeball_", 8);
}

static void G2_Invert3x4(const mdxaBone_t& m, mdxaBone_t& out)
{
	const float a = m.matrix[0][0], b = m.matrix[0][1], c = m.matrix[0][2];
	const float d = m.matrix[1][0], e = m.matrix[1][1], f = m.matrix[1][2];
	const float g = m.matrix[2][0], h = m.matrix[2][1], i = m.matrix[2][2];
	const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
	float det = a * A + b * B + c * C;
	if (fabsf(det) < 1e-12f) det = 1e-12f;
	const float id = 1.0f / det;
	const float r[3][3] = {
		{ A * id, -(b * i - c * h) * id, (b * f - c * e) * id },
		{ B * id, (a * i - c * g) * id, -(a * f - c * d) * id },
		{ C * id, -(a * h - b * g) * id, (a * e - b * d) * id } };
	for (int y = 0; y < 3; y++)
	{
		for (int x = 0; x < 3; x++) out.matrix[y][x] = r[y][x];
		out.matrix[y][3] = -(r[y][0] * m.matrix[0][3] + r[y][1] * m.matrix[1][3] + r[y][2] * m.matrix[2][3]);
	}
}

class CBoneCache;
static void G2_TransformBone(int index, CBoneCache& CB);

class CBoneCache
{
	void EvalLow(int index)
	{
		assert(index >= 0 && index < (int)mBones.size());

		if (mFinalBones[index].touch != mCurrentTouch)
		{
			// need to evaluate the bone
			assert((mFinalBones[index].parent >= 0
				&& mFinalBones[index].parent < (int)mFinalBones.size()) ||
				(index == 0 && mFinalBones[index].parent == -1));

			if (mFinalBones[index].parent >= 0)
			{
				EvalLow(mFinalBones[index].parent); // make sure parent is evaluated

				const SBoneCalc& par = mBones[mFinalBones[index].parent];
				SBoneCalc& bone = mBones[index];
				bone.newFrame = par.newFrame;
				bone.currentFrame = par.currentFrame;
				bone.backlerp = par.backlerp;
				bone.blendFrame = par.blendFrame;
				bone.blendOldFrame = par.blendOldFrame;
				bone.blendMode = par.blendMode;
				bone.blendLerp = par.blendLerp;
			}

			G2_TransformBone(index, *this);

			mFinalBones[index].touch = mCurrentTouch;
		}
	}

	void SmoothLow(int index)
	{
		{
			const mdxaSkelOffsets_t* offs = (mdxaSkelOffsets_t*)((byte*)header + sizeof(mdxaHeader_t));
			const mdxaSkel_t* sk = (mdxaSkel_t*)((byte*)offs + offs->offsets[index]);
			const int parent = mFinalBones[index].parent;
			if (parent >= 0 && G2_IsRigidFaceBone(sk->name))
			{
				if (mSmoothBones[parent].touch != mCurrentTouch) SmoothLow(parent);
				mdxaBone_t inv;
				G2_Invert3x4(mFinalBones[parent].boneMatrix, inv);
				mSmoothBones[index].boneMatrix = mSmoothBones[parent].boneMatrix * (inv * mFinalBones[index].boneMatrix);
				mSmoothBones[index].touch = mCurrentTouch;
				return;
			}
		}
		if (mSmoothBones[index].touch == mLastTouch)
		{
			const mdxaBone_t& newM = mFinalBones[index].boneMatrix;
			mdxaBone_t& oldM = mSmoothBones[index].boneMatrix;

			oldM = mSmoothFactor * (oldM - newM) + newM;
		}
		else
		{
			mSmoothBones[index].boneMatrix = mFinalBones[index].boneMatrix;
		}

		const mdxaSkelOffsets_t* offsets =
			(mdxaSkelOffsets_t*)((byte*)header + sizeof(mdxaHeader_t));
		const mdxaSkel_t* skel =
			(mdxaSkel_t*)((byte*)offsets + offsets->offsets[index]);

		mdxaBone_t tempMatrix =
			mSmoothBones[index].boneMatrix * skel->BasePoseMat;
		const float maxl = VectorLength(&skel->BasePoseMat.matrix[0][0]);
		VectorNormalize(&tempMatrix.matrix[0][0]);
		VectorNormalize(&tempMatrix.matrix[1][0]);
		VectorNormalize(&tempMatrix.matrix[2][0]);
		Mat3x4_Scale(&tempMatrix, &tempMatrix, maxl);

		mSmoothBones[index].boneMatrix = tempMatrix * skel->BasePoseMatInv;
		mSmoothBones[index].touch = mCurrentTouch;

#ifdef _DEBUG
		for (int i = 0; i < 3; i++)
		{
			for (int j = 0; j < 4; j++)
			{
				assert(!Q_isnan(mSmoothBones[index].boneMatrix.matrix[i][j]));
			}
		}
#endif// _DEBUG
	}

public:
	int frameSize;
	const mdxaHeader_t* header;
	const model_t* mod;

	// these are split for better cpu cache behavior
	std::vector<SBoneCalc> mBones;
	std::vector<CTransformBone> mFinalBones;
	std::vector<CTransformBone> mSmoothBones; // for render smoothing

	boneInfo_v* rootBoneList;
	mdxaBone_t rootMatrix;
	int incomingTime;

	int mCurrentTouch;
	int mCurrentTouchRender;
	int mLastTouch;
	int mLastLastTouch;

	// for render smoothing
	bool mSmoothingActive;
	bool mUnsquash;
	float mSmoothFactor;

	// GPU Data
	int      uboOffset;
	int		 uboPreviousOffset;
	int	     uboGPUFrame;

	CBoneCache(const model_t* amod, const mdxaHeader_t* aheader)
		: frameSize(0)
		, header(aheader)
		, mod(amod)
		, mBones(header->numBones)
		, mFinalBones(header->numBones)
		, mSmoothBones(header->numBones)
		, rootBoneList(nullptr)
		, rootMatrix()
		, incomingTime(0)
		, mCurrentTouch(3)
		, mCurrentTouchRender(0)
		, mLastTouch(2)
		, mLastLastTouch(1)
		, mSmoothingActive(false)
		, mUnsquash(false)
		, mSmoothFactor(0.0f)
		, uboOffset(-1)
		, uboPreviousOffset(-1)
		, uboGPUFrame(-1)
	{
		assert(amod);
		assert(aheader);


		int numBones = header->numBones;
		mdxaSkelOffsets_t* offsets =
			(mdxaSkelOffsets_t*)((byte*)header + sizeof(mdxaHeader_t));

		for (int i = 0; i < numBones; ++i)
		{
			mdxaSkel_t* skel =
				(mdxaSkel_t*)((byte*)offsets + offsets->offsets[i]);
			mFinalBones[i].parent = skel->parent;
		}
	}

	SBoneCalc& Root()
	{
		assert(mBones.size());
		return mBones[0];
	}

	const mdxaBone_t& EvalUnsmooth(int index)
	{
		EvalLow(index);
		if (mSmoothingActive && mSmoothBones[index].touch)
		{
			return mSmoothBones[index].boneMatrix;
		}

		return mFinalBones[index].boneMatrix;
	}

	const mdxaBone_t& Eval(int index)
	{
		//Hey, this is what sof2 does. Let's try it out.
		assert(index >= 0 && index < (int)mBones.size());
		if (mFinalBones[index].touch != mCurrentTouch)
		{
			EvalLow(index);
		}

		return mFinalBones[index].boneMatrix;
	}

	const mdxaBone_t& EvalRender(int index)
	{
		assert(index >= 0 && index < (int)mBones.size());
		if (mFinalBones[index].touch != mCurrentTouch)
		{
			mFinalBones[index].touchRender = mCurrentTouchRender;
			EvalLow(index);
		}

		if (mSmoothingActive)
		{
			if (mSmoothBones[index].touch != mCurrentTouch)
			{
				SmoothLow(index);
			}

			return mSmoothBones[index].boneMatrix;
		}

		return mFinalBones[index].boneMatrix;
	}

	bool WasRendered(int index)
	{
		assert(index >= 0 && index < (int)mBones.size());
		return mFinalBones[index].touchRender == mCurrentTouchRender;
	}

	int GetParent(int index)
	{
		if (index == 0)
		{
			return -1;
		}

		assert(index > 0 && index < (int)mBones.size());
		return mFinalBones[index].parent;
	}
};

void RemoveBoneCache(CBoneCache* boneCache)
{
#ifdef _FULL_G2_LEAK_CHECKING
	g_Ghoul2Allocations -= sizeof(*boneCache);
#endif

	delete boneCache;
}

#ifdef _G2_LISTEN_SERVER_OPT
void CopyBoneCache(CBoneCache* to, CBoneCache* from)
{
	memcpy(to, from, sizeof(CBoneCache));
}
#endif

const mdxaBone_t& EvalBoneCache(int index, CBoneCache* boneCache)
{
	assert(boneCache);
	return boneCache->Eval(index);
}

// rww - RAGDOLL_BEGIN
const mdxaHeader_t* G2_GetModA(const CGhoul2Info& ghoul2)
{
	if (!ghoul2.mBoneCache)
	{
		return 0;
	}

	CBoneCache& boneCache = *ghoul2.mBoneCache;
	return boneCache.header;
}

int G2_GetBoneDependents(CGhoul2Info& ghoul2, const int bone_num, int* tempDependents, int maxDep)
{
	// fixme, these should be precomputed
	if (!ghoul2.mBoneCache || !maxDep)
	{
		return 0;
	}

	CBoneCache& boneCache = *ghoul2.mBoneCache;
	mdxaSkelOffsets_t* offsets =
		(mdxaSkelOffsets_t*)((byte*)boneCache.header + sizeof(mdxaHeader_t));
	mdxaSkel_t* skel =
		(mdxaSkel_t*)((byte*)offsets + offsets->offsets[bone_num]);

	int numDependencies = 0;
	for (int i = 0; i < skel->numChildren; i++)
	{
		if (!maxDep)
		{
			return i; // number added
		}

		*tempDependents = skel->children[i];
		assert(*tempDependents > 0 && *tempDependents < boneCache.header->numBones);
		maxDep--;
		tempDependents++;
		numDependencies++;
	}

	for (int i = 0; i < skel->numChildren; i++)
	{
		int num = G2_GetBoneDependents(ghoul2, skel->children[i], tempDependents, maxDep);
		tempDependents += num;
		numDependencies += num;
		maxDep -= num;
		assert(maxDep >= 0);
		if (!maxDep)
		{
			break;
		}
	}

	return numDependencies;
}

bool G2_WasBoneRendered(const CGhoul2Info& ghoul2, const int bone_num)
{
	if (!ghoul2.mBoneCache)
	{
		return false;
	}

	CBoneCache& boneCache = *ghoul2.mBoneCache;
	return boneCache.WasRendered(bone_num);
}

void G2_GetBoneBasepose(const CGhoul2Info& ghoul2, const int bone_num, mdxaBone_t*& retBasepose, mdxaBone_t*& retBaseposeInv)
{
	if (!ghoul2.mBoneCache)
	{
		// yikes
		retBasepose = const_cast<mdxaBone_t*>(&identityMatrix);
		retBaseposeInv = const_cast<mdxaBone_t*>(&identityMatrix);
		return;
	}

	assert(ghoul2.mBoneCache);
	CBoneCache& boneCache = *ghoul2.mBoneCache;
	assert(boneCache.mod);
	assert(bone_num >= 0 && bone_num < boneCache.header->numBones);

	mdxaSkelOffsets_t* offsets =
		(mdxaSkelOffsets_t*)((byte*)boneCache.header + sizeof(mdxaHeader_t));
	mdxaSkel_t* skel = (mdxaSkel_t*)((byte*)offsets + offsets->offsets[bone_num]);

	retBasepose = &skel->BasePoseMat;
	retBaseposeInv = &skel->BasePoseMatInv;
}

char* G2_GetBoneNameFromSkel(const CGhoul2Info& ghoul2, const int bone_num)
{
	if (!ghoul2.mBoneCache)
	{
		return NULL;
	}

	CBoneCache& boneCache = *ghoul2.mBoneCache;
	assert(boneCache.mod);
	assert(bone_num >= 0 && bone_num < boneCache.header->numBones);

	mdxaSkelOffsets_t* offsets =
		(mdxaSkelOffsets_t*)((byte*)boneCache.header + sizeof(mdxaHeader_t));
	mdxaSkel_t* skel = (mdxaSkel_t*)((byte*)offsets + offsets->offsets[bone_num]);

	return skel->name;
}

void G2_RagGetBoneBasePoseMatrixLow(const CGhoul2Info& ghoul2, const int bone_num, const mdxaBone_t& boneMatrix, mdxaBone_t& retMatrix, vec3_t scale)
{
	assert(ghoul2.mBoneCache);
	CBoneCache& boneCache = *ghoul2.mBoneCache;
	assert(boneCache.mod);
	assert(bone_num >= 0 && bone_num < boneCache.header->numBones);

	mdxaSkelOffsets_t* offsets =
		(mdxaSkelOffsets_t*)((byte*)boneCache.header + sizeof(mdxaHeader_t));
	mdxaSkel_t* skel = (mdxaSkel_t*)((byte*)offsets + offsets->offsets[bone_num]);

	Mat3x4_Multiply(&retMatrix, &boneMatrix, &skel->BasePoseMat);

	if (scale[0])
	{
		retMatrix.matrix[0][3] *= scale[0];
	}

	if (scale[1])
	{
		retMatrix.matrix[1][3] *= scale[1];
	}

	if (scale[2])
	{
		retMatrix.matrix[2][3] *= scale[2];
	}

	VectorNormalize((float*)&retMatrix.matrix[0]);
	VectorNormalize((float*)&retMatrix.matrix[1]);
	VectorNormalize((float*)&retMatrix.matrix[2]);
}

void G2_GetBoneMatrixLow(const CGhoul2Info& ghoul2, const int bone_num, const vec3_t scale, mdxaBone_t& retMatrix, mdxaBone_t*& retBasepose, mdxaBone_t*& retBaseposeInv)
{
	if (!ghoul2.mBoneCache)
	{
		retMatrix = identityMatrix;
		// yikes
		retBasepose = const_cast<mdxaBone_t*>(&identityMatrix);
		retBaseposeInv = const_cast<mdxaBone_t*>(&identityMatrix);
		return;
	}

	mdxaBone_t bolt;
	assert(ghoul2.mBoneCache);
	CBoneCache& boneCache = *ghoul2.mBoneCache;
	assert(boneCache.mod);
	assert(bone_num >= 0 && bone_num < boneCache.header->numBones);

	mdxaSkelOffsets_t* offsets =
		(mdxaSkelOffsets_t*)((byte*)boneCache.header + sizeof(mdxaHeader_t));
	mdxaSkel_t* skel = (mdxaSkel_t*)((byte*)offsets + offsets->offsets[bone_num]);

	Mat3x4_Multiply(
		&bolt,
		&boneCache.Eval(bone_num),
		&skel->BasePoseMat);
	retBasepose = &skel->BasePoseMat;
	retBaseposeInv = &skel->BasePoseMatInv;

	if (scale[0])
	{
		bolt.matrix[0][3] *= scale[0];
	}

	if (scale[1])
	{
		bolt.matrix[1][3] *= scale[1];
	}

	if (scale[2])
	{
		bolt.matrix[2][3] *= scale[2];
	}

	VectorNormalize((float*)&bolt.matrix[0]);
	VectorNormalize((float*)&bolt.matrix[1]);
	VectorNormalize((float*)&bolt.matrix[2]);

	Mat3x4_Multiply(&retMatrix, &worldMatrix, &bolt);

#ifdef _DEBUG
	for (int i = 0; i < 3; i++)
	{
		for (int j = 0; j < 4; j++)
		{
			assert(!Q_isnan(retMatrix.matrix[i][j]));
		}
	}
#endif // _DEBUG
}

int G2_GetParentBoneMatrixLow(const CGhoul2Info& ghoul2, const int bone_num, const vec3_t scale, mdxaBone_t& retMatrix, mdxaBone_t*& retBasepose, mdxaBone_t*& retBaseposeInv)
{
	int parent = -1;
	if (ghoul2.mBoneCache)
	{
		CBoneCache& boneCache = *ghoul2.mBoneCache;
		assert(boneCache.mod);
		assert(bone_num >= 0 && bone_num < boneCache.header->numBones);

		parent = boneCache.GetParent(bone_num);
		if (parent < 0 || parent >= boneCache.header->numBones)
		{
			parent = -1;
			retMatrix = identityMatrix;

			// yikes
			retBasepose = const_cast<mdxaBone_t*>(&identityMatrix);
			retBaseposeInv = const_cast<mdxaBone_t*>(&identityMatrix);
		}
		else
		{
			G2_GetBoneMatrixLow(
				ghoul2,
				parent,
				scale,
				retMatrix,
				retBasepose,
				retBaseposeInv);
		}
	}
	return parent;
}
//rww - RAGDOLL_END

class CRenderSurface
{
public:
	int				surfaceNum;
	surfaceInfo_v& rootSList;
	shader_t* cust_shader;
	int				fogNum;
	qboolean		personalModel;
	CBoneCache* boneCache;
	int				renderfx;
	skin_t* skin;
	model_t* currentModel;
	int				lod;
	boltInfo_v& boltList;
#ifdef _G2_GORE
	shader_t* gore_shader;
	CGoreSet* gore_set;
#endif

	CRenderSurface(
		int				initsurfaceNum,
		surfaceInfo_v& initrootSList,
		shader_t* initcust_shader,
		int				initfogNum,
		qboolean		initpersonalModel,
		CBoneCache* initboneCache,
		int				initrenderfx,
		skin_t* initskin,
		model_t* initcurrentModel,
		int				initlod,
#ifdef _G2_GORE
		boltInfo_v& initboltList,
		shader_t* initgore_shader,
		CGoreSet* initgore_set
#else
		boltInfo_v& initboltList
#endif
	)
		: surfaceNum(initsurfaceNum)
		, rootSList(initrootSList)
		, cust_shader(initcust_shader)
		, fogNum(initfogNum)
		, personalModel(initpersonalModel)
		, boneCache(initboneCache)
		, renderfx(initrenderfx)
		, skin(initskin)
		, currentModel(initcurrentModel)
		, lod(initlod)
		, boltList(initboltList)
#ifdef _G2_GORE
		, gore_shader(initgore_shader)
		, gore_set(initgore_set)
#endif
	{}
};

/*

All bones should be an identity orientation to display the mesh exactly
as it is specified.

For all other frames, the bones represent the transformation from the
orientation of the bone in the base frame to the orientation in this
frame.

*/

/*
=============
R_ACullModel
=============
*/
static int R_GCullModel(const trRefEntity_t* ent) {
	// scale the radius if need be
	float largestScale = ent->e.modelScale[0];

	if (ent->e.modelScale[1] > largestScale)
	{
		largestScale = ent->e.modelScale[1];
	}
	if (ent->e.modelScale[2] > largestScale)
	{
		largestScale = ent->e.modelScale[2];
	}
	if (!largestScale)
	{
		largestScale = 1;
	}

	// cull bounding sphere
	switch (R_CullLocalPointAndRadius(vec3_origin, ent->e.radius * largestScale))
	{
	case CULL_OUT:
		tr.pc.c_sphere_cull_md3_out++;
		return CULL_OUT;

	case CULL_IN:
		tr.pc.c_sphere_cull_md3_in++;
		return CULL_IN;

	case CULL_CLIP:
		tr.pc.c_sphere_cull_md3_clip++;
		return CULL_IN;
	}
	return CULL_IN;
}

/*
=================
R_AComputeFogNum

=================
*/
static int R_GComputeFogNum(const trRefEntity_t* ent) {
	int				i = 0, j = 0;
	float			frameRadius = 0.0f;
	fog_t* fog = nullptr;
	vec3_t			localOrigin;

	if (tr.refdef.rdflags & RDF_NOWORLDMODEL) {
		return 0;
	}

	VectorCopy(ent->e.origin, localOrigin);
	frameRadius = ent->e.radius;
#ifndef REND2_SP
	for (i = 1; i < tr.world->numfogs; i++) {
		fog = &tr.world->fogs[i];
		for (j = 0; j < 3; j++) {
			if (localOrigin[j] - frameRadius >= fog->bounds[1][j]) {
				break;
			}
			if (localOrigin[j] + frameRadius <= fog->bounds[0][j]) {
				break;
			}
		}
		if (j == 3) {
			return i;
		}
	}
	return 0;
#else
	int partialFog = 0;
	for (i = 1; i < tr.world->numfogs; i++) {
		fog = &tr.world->fogs[i];
		if (localOrigin[0] - frameRadius >= fog->bounds[0][0]
			&& localOrigin[0] + frameRadius <= fog->bounds[1][0]
			&& localOrigin[1] - frameRadius >= fog->bounds[0][1]
			&& localOrigin[1] + frameRadius <= fog->bounds[1][1]
			&& localOrigin[2] - frameRadius >= fog->bounds[0][2]
			&& localOrigin[2] + frameRadius <= fog->bounds[1][2])
		{//totally inside it
			return i;
			break;
		}
		if ((localOrigin[0] - frameRadius >= fog->bounds[0][0] && localOrigin[1] - frameRadius >= fog->bounds[0][1] && localOrigin[2] - frameRadius >= fog->bounds[0][2] &&
			localOrigin[0] - frameRadius <= fog->bounds[1][0] && localOrigin[1] - frameRadius <= fog->bounds[1][1] && localOrigin[2] - frameRadius <= fog->bounds[1][2]) ||
			(localOrigin[0] + frameRadius >= fog->bounds[0][0] && localOrigin[1] + frameRadius >= fog->bounds[0][1] && localOrigin[2] + frameRadius >= fog->bounds[0][2] &&
				localOrigin[0] + frameRadius <= fog->bounds[1][0] && localOrigin[1] + frameRadius <= fog->bounds[1][1] && localOrigin[2] + frameRadius <= fog->bounds[1][2]))
		{//partially inside it
			//if (tr.refdef.fogIndex == i || R_FogParmsMatch(tr.refdef.fogIndex, i))
			//{//take new one only if it's the same one that the viewpoint is in
			//	return i;
			//	break;
			//}
			//else
			if (!partialFog)
			{//first partialFog
				partialFog = i;
			}
		}
	}
	return partialFog;
#endif
}

// work out lod for this entity.
static int G2_ComputeLOD(trRefEntity_t* ent, const model_t* currentModel, int lodBias)
{
	float flod = 0.0f, lodscale = 0.0f;
	float projectedRadius = 0.0f;
	int lod = 0;

	if (currentModel->numLods < 2)
	{	// model has only 1 LOD level, skip computations and bias
		return(0);
	}

	if (r_lodbias->integer > lodBias)
	{
		lodBias = r_lodbias->integer;
	}

	// scale the radius if need be
	float largestScale = ent->e.modelScale[0];

	if (ent->e.modelScale[1] > largestScale)
	{
		largestScale = ent->e.modelScale[1];
	}
	if (ent->e.modelScale[2] > largestScale)
	{
		largestScale = ent->e.modelScale[2];
	}
	if (!largestScale)
	{
		largestScale = 1;
	}

	projectedRadius = ProjectRadius(0.75 * largestScale * ent->e.radius, ent->e.origin);

	// we reduce the radius to make the LOD match other model types which use
	// the actual bound box size
	if (projectedRadius != 0)
	{
		lodscale = (r_lodscale->value + r_autolodscalevalue->value);
		if (lodscale > 20)
		{
			lodscale = 20;
		}
		else if (lodscale < 0)
		{
			lodscale = 0;
		}
		flod = 1.0f - projectedRadius * lodscale;
	}
	else
	{
		// object intersects near view plane, e.g. view weapon
		flod = 0;
	}
	flod *= currentModel->numLods;
	lod = Q_ftol(flod);

	if (lod < 0)
	{
		lod = 0;
	}
	else if (lod >= currentModel->numLods)
	{
		lod = currentModel->numLods - 1;
	}

	lod += lodBias;

	if (lod >= currentModel->numLods)
		lod = currentModel->numLods - 1;
	if (lod < 0)
		lod = 0;

	return lod;
}

//======================================================================
//
// Bone Manipulation code

static void G2_CreateQuaterion(const mdxaBone_t* mat, vec4_t quat)
{
	// this is revised for the 3x4 matrix we use in G2.
	float t = 1 + mat->matrix[0][0] + mat->matrix[1][1] + mat->matrix[2][2];
	float s = 0.0f;

	// If the trace of the matrix is greater than zero, then
	// perform an "instant" calculation.
	// Important note wrt. rouning errors:
	// Test if ( T > 0.00000001 ) to avoid large distortions!
	if (t > 0.00000001)
	{
		s = sqrt(t) * 2;
		quat[0] = (mat->matrix[1][2] - mat->matrix[2][1]) / s;
		quat[1] = (mat->matrix[2][0] - mat->matrix[0][2]) / s;
		quat[2] = (mat->matrix[0][1] - mat->matrix[1][0]) / s;
		quat[3] = 0.25 * s;
	}
	else
	{
		// If the trace of the matrix is equal to zero then identify
		// which major diagonal element has the greatest value.

		// Depending on this, calculate the following:
		if (mat->matrix[0][0] > mat->matrix[1][1] && mat->matrix[0][0] > mat->matrix[2][2]) {
			// Column 0:
			s = sqrt(1.0 + mat->matrix[0][0] - mat->matrix[1][1] - mat->matrix[2][2]) * 2;
			quat[0] = 0.25 * s;
			quat[1] = (mat->matrix[0][1] + mat->matrix[1][0]) / s;
			quat[2] = (mat->matrix[2][0] + mat->matrix[0][2]) / s;
			quat[3] = (mat->matrix[1][2] - mat->matrix[2][1]) / s;
		}
		else if (mat->matrix[1][1] > mat->matrix[2][2]) {
			// Column 1:
			s = sqrt(1.0 + mat->matrix[1][1] - mat->matrix[0][0] - mat->matrix[2][2]) * 2;
			quat[0] = (mat->matrix[0][1] + mat->matrix[1][0]) / s;
			quat[1] = 0.25 * s;
			quat[2] = (mat->matrix[1][2] + mat->matrix[2][1]) / s;
			quat[3] = (mat->matrix[2][0] - mat->matrix[0][2]) / s;
		}
		else {
			// Column 2:
			s = sqrt(1.0 + mat->matrix[2][2] - mat->matrix[0][0] - mat->matrix[1][1]) * 2;
			quat[0] = (mat->matrix[2][0] + mat->matrix[0][2]) / s;
			quat[1] = (mat->matrix[1][2] + mat->matrix[2][1]) / s;
			quat[2] = 0.25 * s;
			quat[3] = (mat->matrix[0][1] - mat->matrix[1][0]) / s;
		}
	}
}

static void G2_CreateMatrixFromQuaterion(mdxaBone_t* mat, vec4_t quat)
{
	const float xx = quat[0] * quat[0];
	const float xy = quat[0] * quat[1];
	const float xz = quat[0] * quat[2];
	const float xw = quat[0] * quat[3];

	const float yy = quat[1] * quat[1];
	const float yz = quat[1] * quat[2];
	const float yw = quat[1] * quat[3];

	const float zz = quat[2] * quat[2];
	const float zw = quat[2] * quat[3];

	mat->matrix[0][0] = 1 - 2 * (yy + zz);
	mat->matrix[1][0] = 2 * (xy - zw);
	mat->matrix[2][0] = 2 * (xz + yw);

	mat->matrix[0][1] = 2 * (xy + zw);
	mat->matrix[1][1] = 1 - 2 * (xx + zz);
	mat->matrix[2][1] = 2 * (yz - xw);

	mat->matrix[0][2] = 2 * (xz - yw);
	mat->matrix[1][2] = 2 * (yz + xw);
	mat->matrix[2][2] = 1 - 2 * (xx + yy);

	mat->matrix[0][3] = mat->matrix[1][3] = mat->matrix[2][3] = 0;
}

/*
====================
G2_GetBonePoolIndex

Safe version:
- Removes asserts
- Adds debug prints + clamping
- Prevents crashes when iFrame/iBone are out of range
- Behaviour preserved for valid data
====================
*/
static int G2_GetBonePoolIndex(const mdxaHeader_t* pMDXAHeader, const int iFrameIn, const int iBoneIn)
{
	if (!pMDXAHeader)
	{
#ifdef _DEBUG
		Com_Printf("Debug: G2_GetBonePoolIndex - pMDXAHeader was NULL\n");
#endif
		return 0;
	}

	int iFrame = iFrameIn;
	int iBone = iBoneIn;

	// ------------------------------------------------------------
	// Validate and clamp iFrame
	// ------------------------------------------------------------
	if (iFrame < 0 || iFrame >= pMDXAHeader->numFrames)
	{
#ifdef _DEBUG
		Com_Printf("Debug: G2_GetBonePoolIndex - iFrame %d out of range (0..%d). Clamping.\n", iFrame, pMDXAHeader->numFrames - 1);
#endif
		if (iFrame < 0)
		{
			iFrame = 0;
		}
		else
		{
			iFrame = pMDXAHeader->numFrames - 1;
		}
	}

	// ------------------------------------------------------------
	// Validate and clamp iBone
	// ------------------------------------------------------------
	if (iBone < 0 || iBone >= pMDXAHeader->numBones)
	{
#ifdef _DEBUG
		Com_Printf("Debug: G2_GetBonePoolIndex - iBone %d out of range (0..%d). Clamping.\n", iBone, pMDXAHeader->numBones - 1);
#endif
		if (iBone < 0)
		{
			iBone = 0;
		}
		else
		{
			iBone = pMDXAHeader->numBones - 1;
		}
	}

	// ------------------------------------------------------------
	// Compute index safely
	// ------------------------------------------------------------
	const int iOffsetToIndex =
		iFrame * pMDXAHeader->numBones * 3 +
		iBone * 3;

	const byte* base = reinterpret_cast<const byte*>(pMDXAHeader);
	const mdxaIndex_t* pIndex =
		reinterpret_cast<const mdxaIndex_t*>(base + pMDXAHeader->ofsFrames + iOffsetToIndex);

	// ------------------------------------------------------------
	// Return packed index
	// ------------------------------------------------------------
	return (pIndex->iIndex[2] << 16) |
		(pIndex->iIndex[1] << 8) |
		pIndex->iIndex[0];
}

static void UnCompressBone(float mat[3][4], const int iBoneIndex, const mdxaHeader_t* pMDXAHeader, const int iFrame)
{
	mdxaCompQuatBone_t* pCompBonePool = (mdxaCompQuatBone_t*)((byte*)pMDXAHeader + pMDXAHeader->ofsCompBonePool);
	MC_UnCompressQuat(mat, pCompBonePool[G2_GetBonePoolIndex(pMDXAHeader, iFrame, iBoneIndex)].Comp);
}

#define DEBUG_G2_TIMING (0)
#define DEBUG_G2_TIMING_RENDER_ONLY (1)

/*
====================
G2_TimingModel

Safely computes animation timing for a bone:
- Replaces asserts with debug prints and clamping.
- Keeps original behaviour unless invalid data would crash.
- Assumes numFramesInFile is the total number of valid frames (0..numFramesInFile-1).
====================
*/
void G2_TimingModel(
	boneInfo_t& bone,
	const int current_time,
	const int numFramesInFile,
	int& currentFrame,
	int& newFrame,
	float& lerp)
{
	// --------------------------------------------------------
	// Validate and clamp start/end frames to valid range
	// --------------------------------------------------------
	if (bone.startFrame < 0 || bone.startFrame >= numFramesInFile)
	{
		if (bone.startFrame < 0)
		{
			bone.startFrame = 0;
		}
		else
		{
			bone.startFrame = numFramesInFile - 1;
		}
	}

	if (bone.endFrame < 0 || bone.endFrame >= numFramesInFile)
	{
		if (bone.endFrame < 0)
		{
			bone.endFrame = 0;
		}
		else
		{
			bone.endFrame = numFramesInFile - 1;
		}
	}

	// --------------------------------------------------------
	// Compute time in "frames" based on animSpeed and startTime
	// --------------------------------------------------------
	const float animSpeed = bone.animSpeed;
	float time = 0.0f;

	if (bone.pauseTime != 0)
	{
		time = (bone.pauseTime - bone.startTime) / 50.0f;
	}
	else
	{
		time = (current_time - bone.startTime) / 50.0f;
	}

	if (time < 0.0f)
	{
		time = 0.0f;
	}

	float newLerpFrame = bone.startFrame + (time * animSpeed);

	const int   numFramesInAnim = bone.endFrame - bone.startFrame;
	const float endFrame = (float)bone.endFrame;

	// --------------------------------------------------------
	// Non-zero length animation
	// --------------------------------------------------------
	if (numFramesInAnim != 0)
	{
		// Did we run off the end of the anim?
		if ((animSpeed > 0.0f && newLerpFrame > (endFrame - 1.0f)) ||
			(animSpeed < 0.0f && newLerpFrame < (endFrame + 1.0f)))
		{
			// We ran off the end; decide what to do based on flags.
			if ((bone.flags & BONE_ANIM_OVERRIDE_LOOP) != 0)
			{
				// Looping animation
				if (animSpeed < 0.0f)
				{
					// Negative speed (reverse) case.
					// Original comment: "we don't use this case, or so I am told"

					// Virtual frame near end?
					if ((newLerpFrame < (endFrame + 1.0f)) && (newLerpFrame >= endFrame))
					{
						// Lerp fraction between endFrame and startFrame.
						lerp = (endFrame + 1.0f) - newLerpFrame;

						currentFrame = (int)endFrame;
						newFrame = bone.startFrame;
					}
					else
					{
						if (newLerpFrame <= (endFrame + 1.0f))
						{
							newLerpFrame =
								endFrame +
								(float)fmod(newLerpFrame - endFrame, (double)numFramesInAnim) -
								(float)numFramesInAnim;
						}

						lerp = ceilf(newLerpFrame) - newLerpFrame;

						currentFrame = (int)ceilf(newLerpFrame);

						if (currentFrame <= (int)(endFrame + 1.0f))
						{
							newFrame = bone.startFrame;
						}
						else
						{
							newFrame = currentFrame - 1;
						}
					}
				}
				else
				{
					// Positive speed (normal forward loop).
					if ((newLerpFrame > (endFrame - 1.0f)) && (newLerpFrame < endFrame))
					{
						// Virtual frame between last frame and startFrame.
						lerp = newLerpFrame - (float)((int)newLerpFrame);

						currentFrame = (int)newLerpFrame;
						newFrame = bone.startFrame;
					}
					else
					{
						if (newLerpFrame >= endFrame)
						{
							newLerpFrame =
								endFrame +
								(float)fmod(newLerpFrame - endFrame, (double)numFramesInAnim) -
								(float)numFramesInAnim;
						}

						lerp = newLerpFrame - (float)((int)newLerpFrame);

						currentFrame = (int)newLerpFrame;

						if (newLerpFrame >= (endFrame - 1.0f))
						{
							newFrame = bone.startFrame;
						}
						else
						{
							newFrame = currentFrame + 1;
						}
					}
				}
			}
			else
			{
				// Non-looping animation
				if ((bone.flags & BONE_ANIM_OVERRIDE_FREEZE) == BONE_ANIM_OVERRIDE_FREEZE)
				{
					// Freeze on last frame depending on direction.
					if (animSpeed > 0.0f)
					{
						currentFrame = bone.endFrame - 1;
					}
					else
					{
						currentFrame = bone.endFrame + 1;
					}

					newFrame = currentFrame;
					lerp = 0.0f;
				}
				else
				{
					// End the animation.
					bone.flags &= ~BONE_ANIM_TOTAL;
				}
			}
		}
		else
		{
			// Still within the animation range.
			if (animSpeed > 0.0f)
			{
				// Forward animation.
				currentFrame = (int)newLerpFrame;
				lerp = newLerpFrame - (float)currentFrame;

				if (currentFrame < 0 || currentFrame >= numFramesInFile)
				{
					if (currentFrame < 0)
					{
						currentFrame = 0;
					}
					else
					{
						currentFrame = numFramesInFile - 1;
					}
				}

				newFrame = currentFrame + 1;

				if (newFrame >= (int)endFrame)
				{
					if ((bone.flags & BONE_ANIM_OVERRIDE_LOOP) != 0)
					{
						newFrame = bone.startFrame;
					}
					else
					{
						newFrame = bone.endFrame - 1;
					}
				}
			}
			else
			{
				// Reverse animation.
				lerp = ceilf(newLerpFrame) - newLerpFrame;

				currentFrame = (int)ceilf(newLerpFrame);

				if (currentFrame > bone.startFrame)
				{
					currentFrame = bone.startFrame;
					newFrame = currentFrame;
					lerp = 0.0f;
				}
				else
				{
					newFrame = currentFrame - 1;

					if (newFrame < (endFrame + 1.0f))
					{
						if ((bone.flags & BONE_ANIM_OVERRIDE_LOOP) != 0)
						{
							newFrame = bone.startFrame;
						}
						else
						{
							newFrame = bone.endFrame + 1;
						}
					}
				}
			}
		}
	}
	else
	{
		// ----------------------------------------------------
		// Zero-length animation (startFrame == endFrame)
		// ----------------------------------------------------
		if (animSpeed < 0.0f)
		{
			currentFrame = bone.endFrame + 1;
		}
		else
		{
			currentFrame = bone.endFrame - 1;
		}

		if (currentFrame < 0)
		{
			currentFrame = 0;
		}
		else if (currentFrame >= numFramesInFile)
		{
			currentFrame = numFramesInFile - 1;
		}

		newFrame = currentFrame;
		lerp = 0.0f;
	}

	// --------------------------------------------------------
	// Final safety clamps (replace final asserts)
	// --------------------------------------------------------
	if (currentFrame < 0 || currentFrame >= numFramesInFile)
	{
		if (currentFrame < 0)
		{
			currentFrame = 0;
		}
		else
		{
			currentFrame = numFramesInFile - 1;
		}
	}

	if (newFrame < 0 || newFrame >= numFramesInFile)
	{
		if (newFrame < 0)
		{
			newFrame = 0;
		}
		else
		{
			newFrame = numFramesInFile - 1;
		}
	}

	if (lerp < 0.0f || lerp > 1.0f)
	{
		if (lerp < 0.0f)
		{
			lerp = 0.0f;
		}
		else
		{
			lerp = 1.0f;
		}
	}
}

#ifdef _RAG_PRINT_TEST
void G2_RagPrintMatrix(mdxaBone_t* mat);
#endif
// basically construct a seperate skeleton with full hierarchy to store a matrix
// off which will give us the desired settling position given the frame in the skeleton
// that should be used -rww
int G2_Add_Bone(const model_t* mod, boneInfo_v& blist, const char* boneName);
int G2_Find_Bone(const model_t* mod, boneInfo_v& blist, const char* boneName);

void G2_RagGetAnimMatrix(CGhoul2Info& ghoul2, const int bone_num, mdxaBone_t& matrix, const int frame)
{
	mdxaBone_t animMatrix{};
	mdxaSkel_t* skel = nullptr;
	mdxaSkel_t* pskel = nullptr;
	mdxaSkelOffsets_t* offsets = nullptr;
	int parent = 0;
	int bListIndex = 0;
	int parentBlistIndex = 0;
#ifdef _RAG_PRINT_TEST
	bool actuallySet = false;
#endif

	assert(ghoul2.mBoneCache);
	assert(ghoul2.animModel);

	offsets = (mdxaSkelOffsets_t*)((byte*)ghoul2.mBoneCache->header + sizeof(mdxaHeader_t));
	skel = (mdxaSkel_t*)((byte*)offsets + offsets->offsets[bone_num]);

	//find/add the bone in the list
	if (!skel->name[0])
	{
		bListIndex = -1;
	}
	else
	{
		bListIndex = G2_Find_Bone(ghoul2.animModel, ghoul2.mBlist, skel->name);
		if (bListIndex == -1)
		{
#ifdef _RAG_PRINT_TEST
			Com_Printf("Attempting to add %s\n", skel->name);
#endif
			bListIndex = G2_Add_Bone(ghoul2.animModel, ghoul2.mBlist, skel->name);
		}
	}

	assert(bListIndex != -1);

	boneInfo_t& bone = ghoul2.mBlist[bListIndex];

	if (bone.hasAnimFrameMatrix == frame)
	{ //already calculated so just grab it
		matrix = bone.animFrameMatrix;
		return;
	}

	//get the base matrix for the specified frame
	UnCompressBone(animMatrix.matrix, bone_num, ghoul2.mBoneCache->header, frame);

	parent = skel->parent;
	if (bone_num > 0 && parent > -1)
	{
		// recursively call to assure all parent matrices are set up
		G2_RagGetAnimMatrix(ghoul2, parent, matrix, frame);

		// assign the new skel ptr for our parent
		pskel = (mdxaSkel_t*)((byte*)offsets + offsets->offsets[parent]);

		// taking bone matrix for the skeleton frame and parent's
		// animFrameMatrix into account, determine our final animFrameMatrix
		if (!pskel->name[0])
		{
			parentBlistIndex = -1;
		}
		else
		{
			parentBlistIndex = G2_Find_Bone(ghoul2.animModel, ghoul2.mBlist, pskel->name);
			if (parentBlistIndex == -1)
			{
				parentBlistIndex = G2_Add_Bone(ghoul2.animModel, ghoul2.mBlist, pskel->name);
			}
		}

		assert(parentBlistIndex != -1);

		boneInfo_t& pbone = ghoul2.mBlist[parentBlistIndex];

		// this should have been calc'd in the recursive call
		assert(pbone.hasAnimFrameMatrix == frame);

		Mat3x4_Multiply(&bone.animFrameMatrix, &pbone.animFrameMatrix, &animMatrix);

#ifdef _RAG_PRINT_TEST
		if (parentBlistIndex != -1 && bListIndex != -1)
		{
			actuallySet = true;
		}
		else
		{
			Com_Printf("BAD LIST INDEX: %s, %s [%i]\n", skel->name, pskel->name, parent);
		}
#endif
	}
	else
	{
		// root
		Mat3x4_Multiply(&bone.animFrameMatrix, &ghoul2.mBoneCache->rootMatrix, &animMatrix);
#ifdef _RAG_PRINT_TEST
		if (bListIndex != -1)
		{
			actuallySet = true;
		}
		else
		{
			Com_Printf("BAD LIST INDEX: %s\n", skel->name);
		}
#endif
	}

	//never need to figure it out again
	bone.hasAnimFrameMatrix = frame;

#ifdef _RAG_PRINT_TEST
	if (!actuallySet)
	{
		Com_Printf("SET FAILURE\n");
		G2_RagPrintMatrix(&bone.animFrameMatrix);
	}
#endif

	matrix = bone.animFrameMatrix;
}

static void G2_TransformBone(int child, CBoneCache& BC)
{
	SBoneCalc& TB = BC.mBones[child];
	mdxaBone_t currentBone{};
	boneInfo_v& boneList = *BC.rootBoneList;
	int angleOverride = 0;

#if DEBUG_G2_TIMING
	bool printTiming = false;
#endif
	// should this bone be overridden by a bone in the bone list?
	int boneListIndex = G2_Find_Bone_In_List(boneList, child);
	if (boneListIndex != -1)
	{
		// we found a bone in the list - we need to override something here.
		boneInfo_t& bone = boneList[boneListIndex];
		int boneFlags = bone.flags;

		// do we override the rotational angles?
		if (boneFlags & BONE_ANGLES_TOTAL)
		{
			angleOverride = boneFlags & BONE_ANGLES_TOTAL;
		}

		// set blending stuff if we need to
		if (boneFlags & BONE_ANIM_BLEND)
		{
			const float blend_time = BC.incomingTime - bone.blendStart;

			// only set up the blend anim if we actually have some blend time
			// left on this bone anim - otherwise we might corrupt some blend
			// higher up the hiearchy
			if (blend_time >= 0.0f && blend_time < bone.blend_time)
			{
				TB.blendFrame = bone.blendFrame;
				TB.blendOldFrame = bone.blendLerpFrame;
				TB.blendLerp = (blend_time / bone.blend_time);
				TB.blendMode = true;
			}
			else
			{
				TB.blendMode = false;
			}
		}
		else if (boneFlags & (BONE_ANIM_OVERRIDE_LOOP | BONE_ANIM_OVERRIDE))
		{
			// turn off blending if we are just doing a straing animation
			// override
			TB.blendMode = false;
		}

		// should this animation be overridden by an animation in the bone
		// list?
		if (boneFlags & (BONE_ANIM_OVERRIDE_LOOP | BONE_ANIM_OVERRIDE))
		{
			G2_TimingModel(
				bone,
				BC.incomingTime,
				BC.header->numFrames,
				TB.currentFrame,
				TB.newFrame,
				TB.backlerp);
		}

#if DEBUG_G2_TIMING
		printTiming = true;
#endif
	}

	// figure out where the location of the bone animation data is
	if (!(TB.newFrame >= 0 && TB.newFrame < BC.header->numFrames))
	{
		TB.newFrame = 0;
	}

	if (!(TB.currentFrame >= 0 && TB.currentFrame < BC.header->numFrames))
	{
		TB.currentFrame = 0;
	}

	// figure out where the location of the blended animation data is
	if (TB.blendFrame < 0.0 || TB.blendFrame >= (BC.header->numFrames + 1))
	{
		TB.blendFrame = 0.0;
	}

	if (!(TB.blendOldFrame >= 0 && TB.blendOldFrame < BC.header->numFrames))
	{
		TB.blendOldFrame = 0;
	}

#if DEBUG_G2_TIMING

#if DEBUG_G2_TIMING_RENDER_ONLY
	if (!HackadelicOnClient)
	{
		printTiming = false;
	}
#endif

	if (printTiming)
	{
		char mess[1000];
		if (TB.blendMode)
		{
			sprintf(
				mess,
				"b %2d %5d   %4d %4d %4d %4d  %f %f\n",
				boneListIndex,
				BC.incomingTime,
				(int)TB.newFrame,
				(int)TB.currentFrame,
				(int)TB.blendFrame,
				(int)TB.blendOldFrame,
				TB.backlerp,
				TB.blendLerp);
		}
		else
		{
			sprintf(
				mess,
				"a %2d %5d   %4d %4d            %f\n",
				boneListIndex,
				BC.incomingTime,
				TB.newFrame,
				TB.currentFrame,
				TB.backlerp);
		}

		Com_OPrintf("%s", mess);

		const boneInfo_t& bone = boneList[boneListIndex];
		if (bone.flags & BONE_ANIM_BLEND)
		{
			sprintf(
				mess,
				"                                                                    bfb[%2d] %5d  %5d  (%5d-%5d) %4.2f %4x   bt(%5d-%5d) %7.2f %5d\n",
				boneListIndex,
				BC.incomingTime,
				bone.startTime,
				bone.startFrame,
				bone.endFrame,
				bone.animSpeed,
				bone.flags,
				bone.blendStart,
				bone.blendStart + bone.blend_time,
				bone.blendFrame,
				bone.blendLerpFrame);
		}
		else
		{
			sprintf(mess, "                                                                    bfa[%2d] %5d  %5d  (%5d-%5d) %4.2f %4x\n",
				boneListIndex,
				BC.incomingTime,
				bone.startTime,
				bone.startFrame,
				bone.endFrame,
				bone.animSpeed,
				bone.flags);
		}
	}
#endif

	assert(child >= 0 && child < BC.header->numBones);

	// decide where the transformed bone is going

	// lerp this bone - use the temp space on the ref entity to put the bone
	// transforms into
	if (!TB.backlerp)
	{
		UnCompressBone(currentBone.matrix, child, BC.header, TB.currentFrame);
	}
	else
	{
		mdxaBone_t newFrameBone{};
		mdxaBone_t currentFrameBone{};
		UnCompressBone(newFrameBone.matrix, child, BC.header, TB.newFrame);
		UnCompressBone(currentFrameBone.matrix, child, BC.header, TB.currentFrame);
		Mat3x4_Lerp(&currentBone, &newFrameBone, &currentFrameBone, TB.backlerp);
	}

	// are we blending with another frame of anim?
	// blend in the other frame if we need to
	if (TB.blendMode)
	{
		mdxaBone_t blendFrameBone{};
		mdxaBone_t blendOldFrameBone{};
		UnCompressBone(blendFrameBone.matrix, child, BC.header, TB.blendFrame);
		UnCompressBone(blendOldFrameBone.matrix, child, BC.header, TB.blendOldFrame);

		const float backlerp = TB.blendFrame - (int)TB.blendFrame;
		mdxaBone_t lerpFrameBone;
		Mat3x4_Lerp(&lerpFrameBone, &blendFrameBone, &blendOldFrameBone, backlerp);
		Mat3x4_Lerp(&currentBone, &currentBone, &lerpFrameBone, TB.blendLerp);
	}

	if (!child)
	{
		// now multiply by the root matrix, so we can offset this model
		// should we need to
		BC.mFinalBones[child].boneMatrix = BC.rootMatrix * currentBone;
	}

	// figure out where the bone hirearchy info is
	mdxaSkelOffsets_t* offsets =
		(mdxaSkelOffsets_t*)((byte*)BC.header + sizeof(mdxaHeader_t));
	mdxaSkel_t* skel =
		(mdxaSkel_t*)((byte*)offsets + offsets->offsets[child]);

	int parent = BC.mFinalBones[child].parent;
	assert((parent == -1 && child == 0) || (parent >= 0 && parent < (int)BC.mBones.size()));

	if (angleOverride & BONE_ANGLES_REPLACE)
	{
		const bool isRag =
			((angleOverride & (BONE_ANGLES_RAGDOLL | BONE_ANGLES_IK)) != 0);

		mdxaBone_t& bone = BC.mFinalBones[child].boneMatrix;
		const boneInfo_t& boneOverride = boneList[boneListIndex];

		// give us the matrix the animation thinks we should have, so we
		// can get the correct X&Y coors
		const mdxaBone_t firstPass = BC.mFinalBones[parent].boneMatrix * currentBone;
		if (isRag)
		{
			// this is crazy, we are gonna drive the animation to ID while we
			// are doing post mults to compensate.
			mdxaBone_t temp = firstPass * skel->BasePoseMat;
			const float matrixScale = VectorLength((float*)&temp);
			mdxaBone_t toMatrix =
			{
				{
					{ 1.0f, 0.0f, 0.0f, 0.0f },
					{ 0.0f, 1.0f, 0.0f, 0.0f },
					{ 0.0f, 0.0f, 1.0f, 0.0f }
				}
			};
			toMatrix.matrix[0][0] = matrixScale;
			toMatrix.matrix[1][1] = matrixScale;
			toMatrix.matrix[2][2] = matrixScale;
			toMatrix.matrix[0][3] = temp.matrix[0][3];
			toMatrix.matrix[1][3] = temp.matrix[1][3];
			toMatrix.matrix[2][3] = temp.matrix[2][3];

			temp = toMatrix * skel->BasePoseMatInv;

			float blend_time = BC.incomingTime - boneOverride.boneBlendStart;
			float blendLerp = (blend_time / boneOverride.boneBlendTime);
			if (blendLerp > 0.0f)
			{
				// has started
				if (blendLerp > 1.0f)
				{
					bone = temp;
				}
				else
				{
					Mat3x4_Lerp(&bone, &temp, &currentBone, blendLerp);
				}
			}
		}
		else
		{
			// are we attempting to blend with the base animation? and still
			// within blend time?
			if (boneOverride.boneBlendTime > 0.0f &&
				(((boneOverride.boneBlendStart + boneOverride.boneBlendTime) < BC.incomingTime)))
			{
				// ok, we are supposed to be blending. Work out lerp
				const float blend_time = BC.incomingTime - boneOverride.boneBlendStart;
				const float blendLerp = (blend_time / boneOverride.boneBlendTime);

				if (blendLerp <= 1)
				{
					if (blendLerp < 0)
					{
						assert(0);
					}

					// now work out the matrix we want to get *to* - firstPass
					// is where we are coming *from*
					mdxaBone_t temp = firstPass * skel->BasePoseMat;
					const float matrixScale = VectorLength((const float*)&temp);

					const mdxaBone_t& m =
						HackadelicOnClient ? boneOverride.newMatrix : boneOverride.matrix;

					mdxaBone_t newMatrixTemp;
					Mat3x4_Scale(&newMatrixTemp, &m, matrixScale);
					newMatrixTemp.matrix[0][3] = temp.matrix[0][3];
					newMatrixTemp.matrix[1][3] = temp.matrix[1][3];
					newMatrixTemp.matrix[2][3] = temp.matrix[2][3];

					temp = newMatrixTemp * skel->BasePoseMatInv;
					Mat3x4_Lerp(&bone, &temp, &firstPass, blendLerp);
				}
				else
				{
					bone = firstPass;
				}
			}
			else
			{
				// no, so just override it directly
				const mdxaBone_t temp = firstPass * skel->BasePoseMat;
				const float matrixScale = VectorLength((const float*)&temp);

				const mdxaBone_t& m =
					HackadelicOnClient ? boneOverride.newMatrix : boneOverride.matrix;

				mdxaBone_t newMatrixTemp;
				Mat3x4_Scale(&newMatrixTemp, &m, matrixScale);
				newMatrixTemp.matrix[0][3] = temp.matrix[0][3];
				newMatrixTemp.matrix[1][3] = temp.matrix[1][3];
				newMatrixTemp.matrix[2][3] = temp.matrix[2][3];

				bone = newMatrixTemp * skel->BasePoseMatInv;
			}
		}
	}
	else if (angleOverride & BONE_ANGLES_PREMULT)
	{
		const mdxaBone_t& boneA =
			child ? BC.mFinalBones[parent].boneMatrix : BC.rootMatrix;

		const mdxaBone_t& boneB =
			HackadelicOnClient ?
			boneList[boneListIndex].newMatrix :
			boneList[boneListIndex].matrix;

		BC.mFinalBones[child].boneMatrix = boneA * boneB;

		if ((angleOverride & (BONE_ANGLES_RAGDOLL | BONE_ANGLES_IK)) == 0)
		{
			BC.mFinalBones[child].boneMatrix =
				BC.mFinalBones[child].boneMatrix * currentBone;
		}
	}
	else if (child)
	{
		// now transform the matrix by it's parent, asumming we have a parent, and
		// we aren't overriding the angles absolutely
		BC.mFinalBones[child].boneMatrix =
			BC.mFinalBones[parent].boneMatrix * currentBone;
	}

	// now multiply our resulting bone by an override matrix should we need to
	if (angleOverride & BONE_ANGLES_POSTMULT)
	{
		const mdxaBone_t& postMultMatrix =
			HackadelicOnClient ?
			boneList[boneListIndex].newMatrix :
			boneList[boneListIndex].matrix;

		BC.mFinalBones[child].boneMatrix =
			BC.mFinalBones[child].boneMatrix * postMultMatrix;
	}
}

constexpr auto GHOUL2_RAG_STARTED = 0x0010;

static void G2_TransformGhoulBones(
	boneInfo_v& rootBoneList,
	mdxaBone_t& rootMatrix,
	CGhoul2Info& ghoul2,
	int time,
	bool smooth = true)
{
#ifdef G2_PERFORMANCE_ANALYSIS
	G2PerformanceTimer_G2_TransformGhoulBones.Start();
	G2PerformanceCounter_G2_TransformGhoulBones++;
#endif

	model_t* currentModel = (model_t*)ghoul2.currentModel;
	mdxaHeader_t* aHeader = (mdxaHeader_t*)ghoul2.aHeader;

	assert(ghoul2.aHeader);
	assert(ghoul2.currentModel);
	assert(ghoul2.currentModel->data.glm && ghoul2.currentModel->data.glm->header);
	if (!aHeader->numBones)
	{
		assert(0); // this would be strange
		return;
	}
	if (!ghoul2.mBoneCache)
	{
		ghoul2.mBoneCache = new CBoneCache(currentModel, aHeader);

#ifdef _FULL_G2_LEAK_CHECKING
		g_Ghoul2Allocations += sizeof(*ghoul2.mBoneCache);
#endif
	}
	ghoul2.mBoneCache->mod = currentModel;
	ghoul2.mBoneCache->header = aHeader;
	assert(ghoul2.mBoneCache->mBones.size() == (unsigned)aHeader->numBones);

	ghoul2.mBoneCache->mSmoothingActive = false;
	ghoul2.mBoneCache->mUnsquash = false;

	// master smoothing control
	if (HackadelicOnClient && smooth && !ri.Cvar_VariableIntegerValue("dedicated"))
	{
		ghoul2.mBoneCache->mLastTouch = ghoul2.mBoneCache->mLastLastTouch;

		// master smoothing control
		float val = r_Ghoul2AnimSmooth->value;
		if (val > 0.0f && val < 1.0f)
		{
#ifndef REND2_SP
			if (ghoul2.mFlags & GHOUL2_CRAZY_SMOOTH)
			{
				val = 0.9f;
			}
			else
#endif
				if (ghoul2.mFlags & GHOUL2_RAG_STARTED)
				{
					for (size_t k = 0; k < rootBoneList.size(); k++)
					{
						boneInfo_t& bone = rootBoneList[k];
						if (bone.flags & BONE_ANGLES_RAGDOLL)
						{
							if (bone.firstCollisionTime &&
								bone.firstCollisionTime > (time - 250) &&
								bone.firstCollisionTime < time)
							{
								val = 0.9f;
							}
							else if (bone.airTime > time)
							{
								val = 0.2f;
							}
							else
							{
								val = 0.8f;
							}
							break;
						}
					}
				}

			ghoul2.mBoneCache->mSmoothFactor = val; // meaningless formula
			ghoul2.mBoneCache->mSmoothingActive = true;

			if (r_Ghoul2UnSqashAfterSmooth->integer)
			{
				ghoul2.mBoneCache->mUnsquash = true;
			}
		}
	}
	else
	{
		ghoul2.mBoneCache->mSmoothFactor = 1.0f;
	}

	ghoul2.mBoneCache->mCurrentTouch++;

	if (HackadelicOnClient)
	{
		ghoul2.mBoneCache->mLastLastTouch = ghoul2.mBoneCache->mCurrentTouch;
		ghoul2.mBoneCache->mCurrentTouchRender = ghoul2.mBoneCache->mCurrentTouch;
	}
	else
	{
		ghoul2.mBoneCache->mCurrentTouchRender = 0;
	}

	ghoul2.mBoneCache->frameSize = 0;

	ghoul2.mBoneCache->rootBoneList = &rootBoneList;
	ghoul2.mBoneCache->rootMatrix = rootMatrix;
	ghoul2.mBoneCache->incomingTime = time;

	SBoneCalc& TB = ghoul2.mBoneCache->Root();
	TB.newFrame = 0;
	TB.currentFrame = 0;
	TB.backlerp = 0.0f;
	TB.blendFrame = 0;
	TB.blendOldFrame = 0;
	TB.blendMode = false;
	TB.blendLerp = 0;

#ifdef G2_PERFORMANCE_ANALYSIS
	G2Time_G2_TransformGhoulBones += G2PerformanceTimer_G2_TransformGhoulBones.End();
#endif
}

constexpr auto MDX_TAG_ORIGIN = 2;

//======================================================================
//
// Surface Manipulation code

#ifndef REND2_SP
// We've come across a surface that's designated as a bolt surface, process it and put it in the appropriate bolt place
static void G2_ProcessSurfaceBolt(
	mdxaBone_v& bonePtr,
	mdxmSurface_t* surface,
	int boltNum,
	boltInfo_v& boltList,
	surfaceInfo_t* surfInfo,
	model_t* mod)
{
	mdxmVertex_t* v = nullptr, * vert0 = nullptr, * vert1 = nullptr, * vert2 = nullptr;
	vec3_t axes[3], sides[3];
	float pTri[3][3], d = 0.0f;
	int j = 0, k = 0;

	// now there are two types of tag surface - model ones and procedural
	// generated types - lets decide which one we have here.
	if (surfInfo && surfInfo->offFlags == G2SURFACEFLAG_GENERATED)
	{
		int surfNumber = surfInfo->genPolySurfaceIndex & 0x0ffff;
		int poly_number = (surfInfo->genPolySurfaceIndex >> 16) & 0x0ffff;

		// find original surface our original poly was in.
		mdxmSurface_t* originalSurf =
			(mdxmSurface_t*)G2_FindSurface((void*)mod, surfNumber, surfInfo->genLod);
		mdxmTriangle_t* originalTriangleIndexes =
			(mdxmTriangle_t*)((byte*)originalSurf + originalSurf->ofsTriangles);

		// get the original polys indexes
		int index0 = originalTriangleIndexes[poly_number].indexes[0];
		int index1 = originalTriangleIndexes[poly_number].indexes[1];
		int index2 = originalTriangleIndexes[poly_number].indexes[2];

		// decide where the original verts are

		vert0 = (mdxmVertex_t*)((byte*)originalSurf + originalSurf->ofsVerts);
		vert0 += index0;

		vert1 = (mdxmVertex_t*)((byte*)originalSurf + originalSurf->ofsVerts);
		vert1 += index1;

		vert2 = (mdxmVertex_t*)((byte*)originalSurf + originalSurf->ofsVerts);
		vert2 += index2;

		// clear out the triangle verts to be
		VectorClear(pTri[0]);
		VectorClear(pTri[1]);
		VectorClear(pTri[2]);

		int* piBoneRefs = (int*)((byte*)originalSurf + originalSurf->ofsBoneReferences);

		// now go and transform just the points we need from the surface that
		// was hit originally
		//		w = vert0->weights;
		float fTotalWeight = 0.0f;
		int iNumWeights = G2_GetVertWeights(vert0);
		for (k = 0; k < iNumWeights; k++)
		{
			int iBoneIndex = G2_GetVertBoneIndex(vert0, k);
			float fBoneWeight = G2_GetVertBoneWeight(vert0, k, fTotalWeight, iNumWeights);

			pTri[0][0] +=
				fBoneWeight *
				(DotProduct(bonePtr[piBoneRefs[iBoneIndex]].second.matrix[0], vert0->vertCoords) +
					bonePtr[piBoneRefs[iBoneIndex]].second.matrix[0][3]);
			pTri[0][1] +=
				fBoneWeight *
				(DotProduct(bonePtr[piBoneRefs[iBoneIndex]].second.matrix[1], vert0->vertCoords) +
					bonePtr[piBoneRefs[iBoneIndex]].second.matrix[1][3]);
			pTri[0][2] +=
				fBoneWeight *
				(DotProduct(bonePtr[piBoneRefs[iBoneIndex]].second.matrix[2], vert0->vertCoords) +
					bonePtr[piBoneRefs[iBoneIndex]].second.matrix[2][3]);
		}

		fTotalWeight = 0.0f;
		iNumWeights = G2_GetVertWeights(vert1);
		for (k = 0; k < iNumWeights; k++)
		{
			int iBoneIndex = G2_GetVertBoneIndex(vert1, k);
			float fBoneWeight = G2_GetVertBoneWeight(vert1, k, fTotalWeight, iNumWeights);

			pTri[1][0] +=
				fBoneWeight *
				(DotProduct(bonePtr[piBoneRefs[iBoneIndex]].second.matrix[0], vert1->vertCoords) +
					bonePtr[piBoneRefs[iBoneIndex]].second.matrix[0][3]);
			pTri[1][1] +=
				fBoneWeight *
				(DotProduct(bonePtr[piBoneRefs[iBoneIndex]].second.matrix[1], vert1->vertCoords) +
					bonePtr[piBoneRefs[iBoneIndex]].second.matrix[1][3]);
			pTri[1][2] +=
				fBoneWeight *
				(DotProduct(bonePtr[piBoneRefs[iBoneIndex]].second.matrix[2], vert1->vertCoords) +
					bonePtr[piBoneRefs[iBoneIndex]].second.matrix[2][3]);
		}

		fTotalWeight = 0.0f;
		iNumWeights = G2_GetVertWeights(vert2);
		for (k = 0; k < iNumWeights; k++)
		{
			int iBoneIndex = G2_GetVertBoneIndex(vert2, k);
			float fBoneWeight = G2_GetVertBoneWeight(vert2, k, fTotalWeight, iNumWeights);

			pTri[2][0] +=
				fBoneWeight *
				(DotProduct(bonePtr[piBoneRefs[iBoneIndex]].second.matrix[0], vert2->vertCoords) +
					bonePtr[piBoneRefs[iBoneIndex]].second.matrix[0][3]);
			pTri[2][1] +=
				fBoneWeight *
				(DotProduct(bonePtr[piBoneRefs[iBoneIndex]].second.matrix[1], vert2->vertCoords) +
					bonePtr[piBoneRefs[iBoneIndex]].second.matrix[1][3]);
			pTri[2][2] +=
				fBoneWeight *
				(DotProduct(bonePtr[piBoneRefs[iBoneIndex]].second.matrix[2], vert2->vertCoords) +
					bonePtr[piBoneRefs[iBoneIndex]].second.matrix[2][3]);
		}

		vec3_t normal;
		vec3_t up;
		vec3_t right;
		vec3_t vec0, vec1;
		// work out baryCentricK
		float baryCentricK = 1.0 - (surfInfo->genBarycentricI + surfInfo->genBarycentricJ);

		// now we have the model transformed into model space, now generate an origin.
		boltList[boltNum].position.matrix[0][3] = (pTri[0][0] * surfInfo->genBarycentricI) +
			(pTri[1][0] * surfInfo->genBarycentricJ) +
			(pTri[2][0] * baryCentricK);
		boltList[boltNum].position.matrix[1][3] = (pTri[0][1] * surfInfo->genBarycentricI) +
			(pTri[1][1] * surfInfo->genBarycentricJ) +
			(pTri[2][1] * baryCentricK);
		boltList[boltNum].position.matrix[2][3] = (pTri[0][2] * surfInfo->genBarycentricI) +
			(pTri[1][2] * surfInfo->genBarycentricJ) +
			(pTri[2][2] * baryCentricK);

		// generate a normal to this new triangle
		VectorSubtract(pTri[0], pTri[1], vec0);
		VectorSubtract(pTri[2], pTri[1], vec1);

		CrossProduct(vec0, vec1, normal);
		VectorNormalize(normal);

		// forward vector
		boltList[boltNum].position.matrix[0][0] = normal[0];
		boltList[boltNum].position.matrix[1][0] = normal[1];
		boltList[boltNum].position.matrix[2][0] = normal[2];

		// up will be towards point 0 of the original triangle.
		// so lets work it out. Vector is hit point - point 0
		up[0] = boltList[boltNum].position.matrix[0][3] - pTri[0][0];
		up[1] = boltList[boltNum].position.matrix[1][3] - pTri[0][1];
		up[2] = boltList[boltNum].position.matrix[2][3] - pTri[0][2];

		// normalise it
		VectorNormalize(up);

		// that's the up vector
		boltList[boltNum].position.matrix[0][1] = up[0];
		boltList[boltNum].position.matrix[1][1] = up[1];
		boltList[boltNum].position.matrix[2][1] = up[2];

		// right is always straight

		CrossProduct(normal, up, right);
		// that's the up vector
		boltList[boltNum].position.matrix[0][2] = right[0];
		boltList[boltNum].position.matrix[1][2] = right[1];
		boltList[boltNum].position.matrix[2][2] = right[2];
	}
	// no, we are looking at a normal model tag
	else
	{
		int* piBoneRefs = (int*)((byte*)surface + surface->ofsBoneReferences);

		// whip through and actually transform each vertex
		v = (mdxmVertex_t*)((byte*)surface + surface->ofsVerts);
		for (j = 0; j < 3; j++)
		{
			// 			mdxmWeight_t	*w;

			VectorClear(pTri[j]);
			//			w = v->weights;

			const int iNumWeights = G2_GetVertWeights(v);
			float fTotalWeight = 0.0f;
			for (k = 0; k < iNumWeights; k++)
			{
				int iBoneIndex = G2_GetVertBoneIndex(v, k);
				float fBoneWeight = G2_GetVertBoneWeight(v, k, fTotalWeight, iNumWeights);

				// bone = bonePtr + piBoneRefs[w->boneIndex];
				pTri[j][0] +=
					fBoneWeight *
					(DotProduct(bonePtr[piBoneRefs[iBoneIndex]].second.matrix[0], v->vertCoords) +
						bonePtr[piBoneRefs[iBoneIndex]].second.matrix[0][3]);
				pTri[j][1] +=
					fBoneWeight *
					(DotProduct(bonePtr[piBoneRefs[iBoneIndex]].second.matrix[1], v->vertCoords) +
						bonePtr[piBoneRefs[iBoneIndex]].second.matrix[1][3]);
				pTri[j][2] +=
					fBoneWeight *
					(DotProduct(bonePtr[piBoneRefs[iBoneIndex]].second.matrix[2], v->vertCoords) +
						bonePtr[piBoneRefs[iBoneIndex]].second.matrix[2][3]);
			}

			v++;
		}

		// clear out used arrays
		memset(axes, 0, sizeof(axes));
		memset(sides, 0, sizeof(sides));

		// work out actual sides of the tag triangle
		for (j = 0; j < 3; j++)
		{
			sides[j][0] = pTri[(j + 1) % 3][0] - pTri[j][0];
			sides[j][1] = pTri[(j + 1) % 3][1] - pTri[j][1];
			sides[j][2] = pTri[(j + 1) % 3][2] - pTri[j][2];
		}

		// do math trig to work out what the matrix will be from this
		// triangle's translated position
		VectorNormalize2(sides[iG2_TRISIDE_LONGEST], axes[0]);
		VectorNormalize2(sides[iG2_TRISIDE_SHORTEST], axes[1]);

		// project shortest side so that it is exactly 90 degrees to the longer
		// side
		d = DotProduct(axes[0], axes[1]);
		VectorMA(axes[0], -d, axes[1], axes[0]);
		VectorNormalize2(axes[0], axes[0]);

		CrossProduct(sides[iG2_TRISIDE_LONGEST], sides[iG2_TRISIDE_SHORTEST], axes[2]);
		VectorNormalize2(axes[2], axes[2]);

		// set up location in world space of the origin point in out going
		// matrix
		boltList[boltNum].position.matrix[0][3] = pTri[MDX_TAG_ORIGIN][0];
		boltList[boltNum].position.matrix[1][3] = pTri[MDX_TAG_ORIGIN][1];
		boltList[boltNum].position.matrix[2][3] = pTri[MDX_TAG_ORIGIN][2];

		// copy axis to matrix - do some magic to orient minus Y to positive X
		// and so on so bolt on stuff is oriented correctly
		boltList[boltNum].position.matrix[0][0] = axes[1][0];
		boltList[boltNum].position.matrix[0][1] = axes[0][0];
		boltList[boltNum].position.matrix[0][2] = -axes[2][0];

		boltList[boltNum].position.matrix[1][0] = axes[1][1];
		boltList[boltNum].position.matrix[1][1] = axes[0][1];
		boltList[boltNum].position.matrix[1][2] = -axes[2][1];

		boltList[boltNum].position.matrix[2][0] = axes[1][2];
		boltList[boltNum].position.matrix[2][1] = axes[0][2];
		boltList[boltNum].position.matrix[2][2] = -axes[2][2];
	}
}

// now go through all the generated surfaces that aren't included in the model
// surface hierarchy and create the correct bolt info for them
static void G2_ProcessGeneratedSurfaceBolts(CGhoul2Info& ghoul2, mdxaBone_v& bonePtr, model_t* mod_t)
{
#ifdef G2_PERFORMANCE_ANALYSIS
	G2PerformanceTimer_G2_ProcessGeneratedSurfaceBolts.Start();
#endif

	// look through the surfaces off the end of the pre-defined model surfaces
	for (size_t i = 0, numSurfaces = ghoul2.mSlist.size(); i < numSurfaces; i++)
	{
		// only look for bolts if we are actually a generated surface, and not
		// just an overriden one
		if (ghoul2.mSlist[i].offFlags & G2SURFACEFLAG_GENERATED)
		{
			// well alrighty then. Lets see if there is a bolt that is
			// attempting to use it
			int boltNum = G2_Find_Bolt_Surface_Num(
				ghoul2.mBltlist, i, G2SURFACEFLAG_GENERATED);

			if (boltNum != -1)
			{
				G2_ProcessSurfaceBolt(
					bonePtr,
					nullptr,
					boltNum,
					ghoul2.mBltlist,
					&ghoul2.mSlist[i], mod_t);
			}
		}
	}

#ifdef G2_PERFORMANCE_ANALYSIS
	G2Time_G2_ProcessGeneratedSurfaceBolts +=
		G2PerformanceTimer_G2_ProcessGeneratedSurfaceBolts.End();
#endif
}
#endif

static void RenderSurfaces(CRenderSurface& RS, const trRefEntity_t* ent, int entityNum)
{
#ifdef G2_PERFORMANCE_ANALYSIS
	G2PerformanceTimer_RenderSurfaces.Start();
#endif

	int i = 0;
	const shader_t* shader = 0;
	int offFlags = 0;
#ifdef _G2_GORE
	bool drawGore = true;
#endif

	assert(RS.currentModel);
	assert(RS.currentModel->data.glm && RS.currentModel->data.glm->header);
	// back track and get the surfinfo struct for this surface
	mdxmSurface_t* surface =
		(mdxmSurface_t*)G2_FindSurface(RS.currentModel, RS.surfaceNum, RS.lod);
	mdxmHierarchyOffsets_t* surf_indexes = (mdxmHierarchyOffsets_t*)
		((byte*)RS.currentModel->data.glm->header + sizeof(mdxmHeader_t));
	mdxmSurfHierarchy_t* surfInfo = (mdxmSurfHierarchy_t*)
		((byte*)surf_indexes + surf_indexes->offsets[surface->thisSurfaceIndex]);

	// see if we have an override surface in the surface list
	const surfaceInfo_t* surfOverride = G2_FindOverrideSurface(RS.surfaceNum, RS.rootSList);

	// really, we should use the default flags for this surface unless it's been overriden
	offFlags = surfInfo->flags;

	// set the off flags if we have some
	if (surfOverride)
	{
		offFlags = surfOverride->offFlags;
	}

	// if this surface is not off, add it to the shader render list
	if (!offFlags)
	{
		if (RS.cust_shader)
		{
			shader = RS.cust_shader;
		}
		else if (RS.skin)
		{
			int		j = 0;

			// match the surface name to something in the skin file
			shader = R_GetShaderByHandle(surfInfo->shaderIndex);
			for (j = 0; j < RS.skin->numSurfaces; j++)
			{
				// the names have both been lowercased
				if (!strcmp(RS.skin->surfaces[j]->name, surfInfo->name))
				{
					shader = (shader_t*)RS.skin->surfaces[j]->shader;
					break;
				}
			}
		}
		else
		{
			shader = R_GetShaderByHandle(surfInfo->shaderIndex);
		}

		// Get dlightBits and Cubemap
		float radius = 0.0f;
		// scale the radius if needed
		float largestScale = MAX(ent->e.modelScale[0], MAX(ent->e.modelScale[1], ent->e.modelScale[2]));
		radius = ent->e.radius * largestScale;
		int dlightBits = 0; (void)radius;
		int cubemapIndex = R_CubemapForPoint(ent->e.origin);

		// don't add third_person objects if not viewing through a portal
		if (!RS.personalModel)
		{
			// set the surface info to point at the where the transformed bone
			// list is going to be for when the surface gets rendered out
			CRenderableSurface* newSurf = AllocGhoul2RenderableSurface();
			newSurf->vboMesh = &RS.currentModel->data.glm->vboModels[RS.lod].vboMeshes[RS.surfaceNum];
			assert(newSurf->vboMesh != NULL && RS.surfaceNum == surface->thisSurfaceIndex);
			newSurf->surfaceData = surface;
			newSurf->boneCache = RS.boneCache;

			// render shadows? No stencil shadow volumes for Ghoul2 models in rend2 (DH 2026-10-25): the old test was
			// inverted (it asked for RF_NOSHADOW | RF_DEPTHHACK), so only menu models (RF_NOSHADOW) and first person
			// views got them, and rend2 builds the volumes from tess.xyz, which GPU-skinned surfaces don't fill. Result:
			// hard black shards over the menu character (SJE character creation, cg_shadows 2). Characters in a level
			// never got these volumes, so nothing changes there.

			R_AddDrawSurf((surfaceType_t*)newSurf, (shader_t*)shader, RS.fogNum, qfalse,
				R_IsPostRenderEntity(entityNum, ent), cubemapIndex, qfalse);

#ifdef _G2_GORE
			if (RS.gore_set && drawGore)
			{
				int curTime = G2API_GetTime(tr.refdef.time);

				auto range = RS.gore_set->mGoreRecords.equal_range(RS.surfaceNum);
				CRenderableSurface* last = newSurf;
				for (auto k = range.first; k != range.second; /* blank */)
				{
					auto kcur = k;
					k++;

					R2GoreTextureCoordinates* tex = FindR2GoreRecord(kcur->second.mGoreTag);
					if (!tex ||	// it is gone, lets get rid of it
						(kcur->second.mDeleteTime &&
							curTime >= kcur->second.mDeleteTime)) // out of time
					{
						RS.gore_set->mGoreRecords.erase(kcur);
					}
					else if (tex->tex[RS.lod])
					{
						CRenderableSurface* newSurf2 = AllocGhoul2RenderableSurface();
						*newSurf2 = *newSurf;
						newSurf2->goreChain = 0;
						newSurf2->alternateTex = tex->tex[RS.lod];
						newSurf2->scale = 1.0f;
						newSurf2->fade = 1.0f;
						newSurf2->impactTime = 1.0f;  // done with
						int magicFactor42 = 500; // ms, impact time
						if (curTime > kcur->second.mGoreGrowStartTime &&
							curTime < (kcur->second.mGoreGrowStartTime + magicFactor42))
						{
							newSurf2->impactTime =
								float(curTime - kcur->second.mGoreGrowStartTime) /
								float(magicFactor42);  // linear
						}
#ifdef REND2_SP_GORE
						if (curTime < kcur->second.mGoreGrowEndTime)
						{
							newSurf2->scale = Q_max(
								1.0f,
								1.0f /
								((curTime - kcur->second.mGoreGrowStartTime) *
									kcur->second.mGoreGrowFactor +
									kcur->second.mGoreGrowOffset));
						}
#endif
						shader_t* gshader = nullptr;
						if (kcur->second.shader)
						{
							gshader = R_GetShaderByHandle(kcur->second.shader);
						}
						else
						{
							gshader = R_GetShaderByHandle(goreShader);
						}
						// Set fade on surf.
												//Only if we have a fade time set, and let us fade on rgb if we want -rww
						if ((*kcur).second.mDeleteTime && (*kcur).second.mFadeTime)
						{
							if ((*kcur).second.mDeleteTime - curTime < (*kcur).second.mFadeTime)
							{
								newSurf2->fade = static_cast<float>((*kcur).second.mDeleteTime - curTime) / (*kcur).second.mFadeTime;
								if ((*kcur).second.mFadeRGB)
								{ //RGB fades are scaled from 2.0f to 3.0f (simply to differentiate)
									newSurf2->fade += 2.0f;

									if (newSurf2->fade < 2.01f)
									{
										newSurf2->fade = 2.01f;
									}
								}
							}
						}
						last->goreChain = newSurf2;
						last = newSurf2;
						R_AddDrawSurf((surfaceType_t*)newSurf2, gshader, RS.fogNum, qfalse,
							R_IsPostRenderEntity(entityNum, ent), cubemapIndex, qfalse);
					}
				}
			}
#endif
		}

		// projection shadows work fine with personal models
		if (r_shadows->integer == 3
			&& RS.fogNum == 0
			&& (RS.renderfx & (RF_NOSHADOW | RF_DEPTHHACK))
			&& shader->sort == SS_OPAQUE) {
			CRenderableSurface* newSurf = AllocGhoul2RenderableSurface();
			newSurf->vboMesh = &RS.currentModel->data.glm->vboModels[RS.lod].vboMeshes[RS.surfaceNum];
			assert(newSurf->vboMesh != NULL && RS.surfaceNum == surface->thisSurfaceIndex);
			newSurf->surfaceData = surface;
			newSurf->boneCache = RS.boneCache;
			R_AddDrawSurf((surfaceType_t*)newSurf, tr.projectionShadowShader, 0, qfalse, qfalse, 0, qfalse);
		}
	}

	// if we are turning off all descendants, then stop this recursion now
	if (offFlags & G2SURFACEFLAG_NODESCENDANTS)
	{
		return;
	}

	// now recursively call for the children
	for (i = 0; i < surfInfo->numChildren; i++)
	{
		RS.surfaceNum = surfInfo->childIndexes[i];
		RenderSurfaces(RS, ent, entityNum);
	}

#ifdef G2_PERFORMANCE_ANALYSIS
	G2Time_RenderSurfaces += G2PerformanceTimer_RenderSurfaces.End();
#endif
}

/*
// Go through the model and deal with just the surfaces that are tagged as bolt
// on points - this is for the server side skeleton construction
void ProcessModelBoltSurfaces(
	int surfaceNum,
	surfaceInfo_v &rootSList,
	mdxaBone_v &bonePtr,
	model_t *currentModel,
	int lod,
	boltInfo_v &boltList)
{
#ifdef G2_PERFORMANCE_ANALYSIS
	G2PerformanceTimer_ProcessModelBoltSurfaces.Start();
#endif
	int i;
	int offFlags = 0;

	// back track and get the surfinfo struct for this surface
	mdxmSurface_t *surface = (mdxmSurface_t *)
		G2_FindSurface((void *)currentModel, surfaceNum, 0);
	mdxmHierarchyOffsets_t *surf_indexes = (mdxmHierarchyOffsets_t *)
		((byte *)currentModel->data.glm->header + sizeof(mdxmHeader_t));
	mdxmSurfHierarchy_t *surfInfo = (mdxmSurfHierarchy_t *)
		((byte *)surf_indexes + surf_indexes->offsets[surface->thisSurfaceIndex]);

	// see if we have an override surface in the surface list
	surfaceInfo_t *surfOverride = G2_FindOverrideSurface(surfaceNum, rootSList);

	// really, we should use the default flags for this surface unless it's been overriden
	offFlags = surfInfo->flags;

	// set the off flags if we have some
	if (surfOverride)
	{
		offFlags = surfOverride->offFlags;
	}

	// is this surface considered a bolt surface?
	if (surfInfo->flags & G2SURFACEFLAG_ISBOLT)
	{
		// well alrighty then. Lets see if there is a bolt that is attempting to use it
		int boltNum = G2_Find_Bolt_Surface_Num(boltList, surfaceNum, 0);
		// yes - ok, processing time.
		if (boltNum != -1)
		{
			G2_ProcessSurfaceBolt(bonePtr, surface, boltNum, boltList, surfOverride, currentModel);
		}
	}

	// if we are turning off all descendants, then stop this recursion now
	if (offFlags & G2SURFACEFLAG_NODESCENDANTS)
	{
		return;
	}

	// now recursively call for the children
	for (i=0; i< surfInfo->numChildren; i++)
	{
		ProcessModelBoltSurfaces(
			surfInfo->childIndexes[i], rootSList, bonePtr, currentModel, lod, boltList);
	}

#ifdef G2_PERFORMANCE_ANALYSIS
	G2Time_ProcessModelBoltSurfaces += G2PerformanceTimer_ProcessModelBoltSurfaces.End();
#endif
}
*/

// build the used bone list so when doing bone transforms we can determine if we need to do it or not
void G2_ConstructUsedBoneList(CConstructBoneList& CBL)
{
	int	 		i = 0, j = 0;
	int			offFlags = 0;
	mdxmHeader_t* mdxm = CBL.currentModel->data.glm->header;

	// back track and get the surfinfo struct for this surface
#ifndef REND2_SP
	const mdxmSurface_t* surface = (mdxmSurface_t*)G2_FindSurface((void*)CBL.currentModel, CBL.surfaceNum, 0);
#else
	const mdxmSurface_t* surface = (mdxmSurface_t*)G2_FindSurface(CBL.currentModel, CBL.surfaceNum, 0);
#endif
	const mdxmHierarchyOffsets_t* surf_indexes = (mdxmHierarchyOffsets_t*)((byte*)mdxm + sizeof(mdxmHeader_t));
	const mdxmSurfHierarchy_t* surfInfo = (mdxmSurfHierarchy_t*)((byte*)surf_indexes + surf_indexes->offsets[surface->thisSurfaceIndex]);
	const model_t* mod_a = R_GetModelByHandle(mdxm->animIndex);
	mdxaHeader_t* mdxa = mod_a->data.gla;
	const mdxaSkelOffsets_t* offsets = (mdxaSkelOffsets_t*)((byte*)mdxa + sizeof(mdxaHeader_t));
	const mdxaSkel_t* skel = nullptr, * childSkel = nullptr;

	// see if we have an override surface in the surface list
	const surfaceInfo_t* surfOverride = G2_FindOverrideSurface(CBL.surfaceNum, CBL.rootSList);

	// really, we should use the default flags for this surface unless it's been overriden
	offFlags = surfInfo->flags;

	// set the off flags if we have some
	if (surfOverride)
	{
		offFlags = surfOverride->offFlags;
	}

	// if this surface is not off, add it to the shader render list
	if (!(offFlags & G2SURFACEFLAG_OFF))
	{
		int* bonesReferenced = (int*)((byte*)surface + surface->ofsBoneReferences);
		// now whip through the bones this surface uses
		for (i = 0; i < surface->numBoneReferences; i++)
		{
			int iBoneIndex = bonesReferenced[i];
			CBL.boneUsedList[iBoneIndex] = 1;

			// now go and check all the descendant bones attached to this bone and see if any have the always flag on them. If so, activate them
			skel = (mdxaSkel_t*)((byte*)mdxa + sizeof(mdxaHeader_t) + offsets->offsets[iBoneIndex]);

			// for every child bone...
			for (j = 0; j < skel->numChildren; j++)
			{
				// get the skel data struct for each child bone of the referenced bone
				childSkel = (mdxaSkel_t*)((byte*)mdxa + sizeof(mdxaHeader_t) + offsets->offsets[skel->children[j]]);

				// does it have the always on flag on?
				if (childSkel->flags & G2BONEFLAG_ALWAYSXFORM)
				{
					// yes, make sure it's in the list of bones to be transformed.
					CBL.boneUsedList[skel->children[j]] = 1;
				}
			}

			// now we need to ensure that the parents of this bone are actually active...
			//
			int iParentBone = skel->parent;
			while (iParentBone != -1)
			{
				if (CBL.boneUsedList[iParentBone])	// no need to go higher
					break;
				CBL.boneUsedList[iParentBone] = 1;
				skel = (mdxaSkel_t*)((byte*)mdxa + sizeof(mdxaHeader_t) + offsets->offsets[iParentBone]);
				iParentBone = skel->parent;
			}
		}
	}
	else
		// if we are turning off all descendants, then stop this recursion now
		if (offFlags & G2SURFACEFLAG_NODESCENDANTS)
		{
			return;
		}

	// now recursively call for the children
	for (i = 0; i < surfInfo->numChildren; i++)
	{
		CBL.surfaceNum = surfInfo->childIndexes[i];
		G2_ConstructUsedBoneList(CBL);
	}
}

// sort all the ghoul models in this list so if they go in reference order.
// This will ensure the bolt on's are attached to the right place on the
// previous model, since it ensures the model being attached to is built and
// rendered first.
// NOTE!! This assumes at least one model will NOT have a parent. If it does -
// we are screwed
static void G2_Sort_Models(
	CGhoul2Info_v& ghoul2,
	int* const model_list,
	int modelListCapacity,
	int* const modelCount)
{
	*modelCount = 0;

	if (modelListCapacity < ghoul2.size())
	{
		return;
	}

	// first walk all the possible ghoul2 models, and stuff the out array with
	// those with no parents
	for (int i = 0, numModels = ghoul2.size(); i < numModels; ++i)
	{
		CGhoul2Info& g2Info = ghoul2[i];

		// have a ghoul model here?
		if (g2Info.mModelindex == -1)
		{
			continue;
		}

		if (!g2Info.mValid)
		{
			continue;
		}

		// are we attached to anything?
		if (g2Info.mModelBoltLink == -1)
		{
			// no, insert us first
			model_list[(*modelCount)++] = i;
		}
	}

	int startPoint = 0;
	int endPoint = *modelCount;

	// now, using that list of parentless models, walk the descendant tree for
	// each of them, inserting the descendents in the list
	while (startPoint != endPoint)
	{
		for (int i = 0, numModels = ghoul2.size(); i < numModels; ++i)
		{
			CGhoul2Info& g2Info = ghoul2[i];

			// have a ghoul model here?
			if (g2Info.mModelindex == -1)
			{
				continue;
			}

			if (!g2Info.mValid)
			{
				continue;
			}

			// what does this model think it's attached to?
			if (g2Info.mModelBoltLink == -1)
			{
				continue;
			}

			int boltTo = (g2Info.mModelBoltLink >> MODEL_SHIFT) & MODEL_AND;

			// is it any of the models we just added to the list?
			for (int j = startPoint; j < endPoint; ++j)
			{
				// is this my parent model?
				if (boltTo == model_list[j])
				{
					// yes, insert into list and exit now
					model_list[(*modelCount)++] = i;
					break;
				}
			}
		}

		// update start and end points
		startPoint = endPoint;
		endPoint = *modelCount;
	}
}

static void* G2_FindSurface_BC(const model_s* mod, int index, int lod)
{
	mdxmHeader_t* mdxm = mod->data.glm->header;
	assert(mod);
	assert(mdxm);

	// point at first lod list
	byte* current = (byte*)((intptr_t)mdxm + (intptr_t)mdxm->ofsLODs);
	int i = 0;

	//walk the lods
	assert(lod >= 0 && lod < mdxm->numLODs);
	for (i = 0; i < lod; i++)
	{
		mdxmLOD_t* lodData = (mdxmLOD_t*)current;
		current += lodData->ofsEnd;
	}

	// avoid the lod pointer data structure
	current += sizeof(mdxmLOD_t);

	mdxmLODSurfOffset_t* indexes = (mdxmLODSurfOffset_t*)current;
	// we are now looking at the offset array
	assert(index >= 0 && index < mdxm->numSurfaces);
	current += indexes->offsets[index];

	return (void*)current;
}

// We've come across a surface that's designated as a bolt surface, process it and put it in the appropriate bolt place
static void G2_ProcessSurfaceBolt2(
	CBoneCache& boneCache,
	const mdxmSurface_t* surface,
	int boltNum,
	boltInfo_v& boltList,
	const surfaceInfo_t* surfInfo,
	const model_t* mod,
	mdxaBone_t& retMatrix)
{
	float pTri[3][3]{};

	// now there are two types of tag surface - model ones and procedural
	// generated types - lets decide which one we have here.
	if (surfInfo && surfInfo->offFlags == G2SURFACEFLAG_GENERATED)
	{
		const int surfNumber = surfInfo->genPolySurfaceIndex & 0x0ffff;
		const int poly_number = (surfInfo->genPolySurfaceIndex >> 16) & 0x0ffff;

		// find original surface our original poly was in.
		const mdxmSurface_t* originalSurf =
			(mdxmSurface_t*)G2_FindSurface_BC(mod, surfNumber, surfInfo->genLod);
		const mdxmTriangle_t* originalTriangleIndexes =
			(mdxmTriangle_t*)((byte*)originalSurf + originalSurf->ofsTriangles);

		// get the original polys indexes
		const int index0 = originalTriangleIndexes[poly_number].indexes[0];
		const int index1 = originalTriangleIndexes[poly_number].indexes[1];
		const int index2 = originalTriangleIndexes[poly_number].indexes[2];

		// decide where the original verts are
		const mdxmVertex_t* surfVerts =
			(mdxmVertex_t*)((byte*)originalSurf + originalSurf->ofsVerts);
		const mdxmVertex_t* verts[3] = {
			surfVerts + index0,
			surfVerts + index1,
			surfVerts + index2
		};

		// clear out the triangle verts to be
		VectorClear(pTri[0]);
		VectorClear(pTri[1]);
		VectorClear(pTri[2]);
		const int* piBoneReferences =
			(int*)((byte*)originalSurf + originalSurf->ofsBoneReferences);

		for (int i = 0; i < 3; ++i)
		{
			// now go and transform just the points we need from the surface that
			// was hit originally
			const int iNumWeights = G2_GetVertWeights(verts[i]);
			float fTotalWeight = 0.0f;
			for (int k = 0; k < iNumWeights; k++)
			{
				const int iBoneIndex = G2_GetVertBoneIndex(verts[i], k);
				const float fBoneWeight = G2_GetVertBoneWeight(
					verts[i], k, fTotalWeight, iNumWeights);
				const mdxaBone_t& bone = boneCache.Eval(piBoneReferences[iBoneIndex]);

				pTri[i][0] += fBoneWeight *
					(DotProduct(bone.matrix[0], verts[i]->vertCoords) + bone.matrix[0][3]);
				pTri[i][1] += fBoneWeight *
					(DotProduct(bone.matrix[1], verts[i]->vertCoords) + bone.matrix[1][3]);
				pTri[i][2] += fBoneWeight *
					(DotProduct(bone.matrix[2], verts[i]->vertCoords) + bone.matrix[2][3]);
			}
		}

		const float baryCentricK =
			1.0f - (surfInfo->genBarycentricI + surfInfo->genBarycentricJ);

		// now we have the model transformed into model space, now generate an origin.
		for (int i = 0; i < 3; ++i)
		{
			retMatrix.matrix[i][3] =
				(pTri[0][i] * surfInfo->genBarycentricI) +
				(pTri[1][i] * surfInfo->genBarycentricJ) +
				(pTri[2][i] * baryCentricK);
		}

		// generate a normal to this new triangle
		vec3_t vec0, vec1;
		VectorSubtract(pTri[0], pTri[1], vec0);
		VectorSubtract(pTri[2], pTri[1], vec1);

		vec3_t normal;
		CrossProduct(vec0, vec1, normal);
		VectorNormalize(normal);

		// forward vector
		retMatrix.matrix[0][0] = normal[0];
		retMatrix.matrix[1][0] = normal[1];
		retMatrix.matrix[2][0] = normal[2];

		// up will be towards point 0 of the original triangle.
		// so lets work it out. Vector is hit point - point 0
		vec3_t up{};
		up[0] = retMatrix.matrix[0][3] - pTri[0][0];
		up[1] = retMatrix.matrix[1][3] - pTri[0][1];
		up[2] = retMatrix.matrix[2][3] - pTri[0][2];

		// normalise it
		VectorNormalize(up);

		// that's the up vector
		retMatrix.matrix[0][1] = up[0];
		retMatrix.matrix[1][1] = up[1];
		retMatrix.matrix[2][1] = up[2];

		// right is always straight
		vec3_t right;
		CrossProduct(normal, up, right);

		// that's the up vector
		retMatrix.matrix[0][2] = right[0];
		retMatrix.matrix[1][2] = right[1];
		retMatrix.matrix[2][2] = right[2];
	}
	// no, we are looking at a normal model tag
	else
	{
		// whip through and actually transform each vertex
		const mdxmVertex_t* v =
			(const mdxmVertex_t*)((byte*)surface + surface->ofsVerts);
		const int* piBoneReferences =
			(const int*)((byte*)surface + surface->ofsBoneReferences);
		for (int j = 0; j < 3; j++)
		{
			VectorClear(pTri[j]);

			const int iNumWeights = G2_GetVertWeights(v);
			float fTotalWeight = 0.0f;
			for (int k = 0; k < iNumWeights; k++)
			{
				const int iBoneIndex = G2_GetVertBoneIndex(v, k);
				const float fBoneWeight =
					G2_GetVertBoneWeight(v, k, fTotalWeight, iNumWeights);
				const mdxaBone_t& bone = boneCache.Eval(piBoneReferences[iBoneIndex]);

				pTri[j][0] += fBoneWeight *
					(DotProduct(bone.matrix[0], v->vertCoords) + bone.matrix[0][3]);
				pTri[j][1] += fBoneWeight *
					(DotProduct(bone.matrix[1], v->vertCoords) + bone.matrix[1][3]);
				pTri[j][2] += fBoneWeight *
					(DotProduct(bone.matrix[2], v->vertCoords) + bone.matrix[2][3]);
			}

			v++;
		}

		// clear out used arrays
		vec3_t axes[3] = {};
		vec3_t sides[3] = {};

		// work out actual sides of the tag triangle
		for (int j = 0; j < 3; j++)
		{
			sides[j][0] = pTri[(j + 1) % 3][0] - pTri[j][0];
			sides[j][1] = pTri[(j + 1) % 3][1] - pTri[j][1];
			sides[j][2] = pTri[(j + 1) % 3][2] - pTri[j][2];
		}

		// do math trig to work out what the matrix will be from this
		// triangle's translated position
		VectorNormalize2(sides[iG2_TRISIDE_LONGEST], axes[0]);
		VectorNormalize2(sides[iG2_TRISIDE_SHORTEST], axes[1]);

		// project shortest side so that it is exactly 90 degrees to the longer
		// side
		float d = DotProduct(axes[0], axes[1]);
		VectorMA(axes[0], -d, axes[1], axes[0]);
		VectorNormalize2(axes[0], axes[0]);

		CrossProduct(sides[iG2_TRISIDE_LONGEST], sides[iG2_TRISIDE_SHORTEST], axes[2]);
		VectorNormalize2(axes[2], axes[2]);

		// set up location in world space of the origin point in out going
		// matrix
		retMatrix.matrix[0][3] = pTri[MDX_TAG_ORIGIN][0];
		retMatrix.matrix[1][3] = pTri[MDX_TAG_ORIGIN][1];
		retMatrix.matrix[2][3] = pTri[MDX_TAG_ORIGIN][2];

		// copy axis to matrix - do some magic to orient minus Y to positive X
		// and so on so bolt on stuff is oriented correctly
		retMatrix.matrix[0][0] = axes[1][0];
		retMatrix.matrix[0][1] = axes[0][0];
		retMatrix.matrix[0][2] = -axes[2][0];

		retMatrix.matrix[1][0] = axes[1][1];
		retMatrix.matrix[1][1] = axes[0][1];
		retMatrix.matrix[1][2] = -axes[2][1];

		retMatrix.matrix[2][0] = axes[1][2];
		retMatrix.matrix[2][1] = axes[0][2];
		retMatrix.matrix[2][2] = -axes[2][2];
	}
}

void G2_GetBoltMatrixLow(
	CGhoul2Info& ghoul2,
	int boltNum,
	const vec3_t scale,
	mdxaBone_t& retMatrix)
{
	if (!ghoul2.mBoneCache)
	{
		retMatrix = identityMatrix;
		return;
	}

	CBoneCache& boneCache = *ghoul2.mBoneCache;
	assert(boneCache.mod);

	boltInfo_v& boltList = ghoul2.mBltlist;
	if (boltList.empty() || boltNum >= boltList.size())
	{
		retMatrix = identityMatrix;
		return;
	}

	boltInfo_t& bolt = boltList[boltNum];
	int boltBone = bolt.boneNumber;
	int boltSurface = bolt.surfaceNumber;

	if (boltBone >= 0)
	{
		mdxaSkelOffsets_t* offsets =
			(mdxaSkelOffsets_t*)((byte*)boneCache.header + sizeof(mdxaHeader_t));
		mdxaSkel_t* skel =
			(mdxaSkel_t*)((byte*)offsets + offsets->offsets[boltBone]);

		Mat3x4_Multiply(
			&retMatrix,
			(mdxaBone_t*)&boneCache.EvalUnsmooth(boltBone),
			&skel->BasePoseMat);
	}
	else if (boltSurface >= 0)
	{
		const surfaceInfo_t* surfInfo = nullptr;
		for (const surfaceInfo_t& t : ghoul2.mSlist)
		{
			if (t.surface == boltSurface)
			{
				surfInfo = &t;
			}
		}

		mdxmSurface_t* surface = nullptr;
		if (!surfInfo)
		{
			surface = (mdxmSurface_t*)G2_FindSurface_BC(
				boneCache.mod, boltSurface, 0);
		}

		if (!surface && surfInfo && surfInfo->surface < 10000)
		{
			surface = (mdxmSurface_t*)G2_FindSurface_BC(
				boneCache.mod, surfInfo->surface, 0);
		}

		G2_ProcessSurfaceBolt2(
			boneCache,
			surface,
			boltNum,
			boltList,
			surfInfo,
			(model_t*)boneCache.mod,
			retMatrix);
	}
	else
	{
		// we have a bolt without a bone or surface, not a huge problem but we
		// ought to at least clear the bolt matrix
		retMatrix = identityMatrix;
	}
}

static void RootMatrix(
	CGhoul2Info_v& ghoul2,
	int time,
	const vec3_t scale,
	mdxaBone_t& retMatrix)
{
	for (int i = 0, numModels = ghoul2.size(); i < numModels; ++i)
	{
		if (ghoul2[i].mModelindex == -1 || !ghoul2[i].mValid)
		{
			continue;
		}

		if (ghoul2[i].mFlags & GHOUL2_NEWORIGIN)
		{
			mdxaBone_t bolt;
			mdxaBone_t tempMatrix{};

			G2_ConstructGhoulSkeleton(ghoul2, time, false, scale);
			G2_GetBoltMatrixLow(ghoul2[i], ghoul2[i].mNewOrigin, scale, bolt);

			tempMatrix.matrix[0][0] = 1.0f;
			tempMatrix.matrix[0][1] = 0.0f;
			tempMatrix.matrix[0][2] = 0.0f;
			tempMatrix.matrix[0][3] = -bolt.matrix[0][3];

			tempMatrix.matrix[1][0] = 0.0f;
			tempMatrix.matrix[1][1] = 1.0f;
			tempMatrix.matrix[1][2] = 0.0f;
			tempMatrix.matrix[1][3] = -bolt.matrix[1][3];

			tempMatrix.matrix[2][0] = 0.0f;
			tempMatrix.matrix[2][1] = 0.0f;
			tempMatrix.matrix[2][2] = 1.0f;
			tempMatrix.matrix[2][3] = -bolt.matrix[2][3];

			retMatrix = tempMatrix * identityMatrix;
			return;
		}
	}

	retMatrix = identityMatrix;
}

/*
==============
R_AddGHOULSurfaces
==============
*/

void R_AddGhoulSurfaces(trRefEntity_t* ent, int entityNum)
{
#ifdef G2_PERFORMANCE_ANALYSIS
	G2PerformanceTimer_R_AddGHOULSurfaces.Start();
#endif

	CGhoul2Info_v& ghoul2 = *((CGhoul2Info_v*)ent->e.ghoul2);

	if (!ghoul2.IsValid())
	{
		return;
	}

	// if we don't want server ghoul2 models and this is one, or we just don't
	// want ghoul2 models at all, then return
	if (r_noServerGhoul2->integer)
	{
		return;
	}

	if (!G2_SetupModelPointers(ghoul2))
	{
		return;
	}

	int currentTime = G2API_GetTime(tr.refdef.time);

	// cull the entire model if merged bounding box of both frames is outside
	// the view frustum.
	int cull = R_GCullModel(ent);
	if (cull == CULL_OUT)
	{
		return;
	}

	HackadelicOnClient = true;

	// don't add third_person objects if not in a portal
	qboolean personalModel = (qboolean)(
		(ent->e.renderfx & RF_THIRD_PERSON) &&
		!(tr.viewParms.isPortal ||
			(tr.viewParms.flags & VPF_DEPTHSHADOW)));

	int model_list[256]{};
	assert(ghoul2.size() < ARRAY_LEN(model_list));
	model_list[255] = 548;

	// see if we are in a fog volume
	int fogNum = R_GComputeFogNum(ent);

	// order sort the ghoul 2 models so bolt ons get bolted to the right model
	int modelCount = 0;
	G2_Sort_Models(ghoul2, model_list, ARRAY_LEN(model_list), &modelCount);
	assert(model_list[255] == 548);

#ifdef _G2_GORE
	if (goreShader == -1)
	{
		goreShader = RE_RegisterShader("gfx/damage/burnmark1");
	}
#endif

	// walk each possible model for this entity and try rendering it out
	for (int j = 0; j < modelCount; ++j)
	{
		CGhoul2Info& g2Info = ghoul2[model_list[j]];

		if (!g2Info.mValid)
		{
			continue;
		}

		if ((g2Info.mFlags & (GHOUL2_NOMODEL | GHOUL2_NORENDER)) != 0)
		{
			continue;
		}

		// figure out whether we should be using a custom shader for this model
		skin_t* skin = nullptr;
		shader_t* cust_shader = nullptr;

		if (ent->e.customShader)
		{
			cust_shader = R_GetShaderByHandle(ent->e.customShader);
		}
		else
		{
			cust_shader = nullptr;
#ifndef REND2_SP
			// figure out the custom skin thing
			if (g2Info.mCustomSkin)
			{
				skin = R_GetSkinByHandle(g2Info.mCustomSkin);
			}
			else
#endif
				if (ent->e.customSkin)
				{
					skin = R_GetSkinByHandle(ent->e.customSkin);
				}
				else if (g2Info.mSkin > 0 && g2Info.mSkin < tr.numSkins)
				{
					skin = R_GetSkinByHandle(g2Info.mSkin);
				}
		}

		int whichLod = G2_ComputeLOD(ent, g2Info.currentModel, g2Info.mLodBias);
		G2_FindOverrideSurface(-1, g2Info.mSlist); //reset the quick surface override lookup;

#ifdef _G2_GORE
		CGoreSet* gore = nullptr;
		if (g2Info.mGoreSetTag)
		{
			gore = FindGoreSet(g2Info.mGoreSetTag);
			if (!gore) // my gore is gone, so remove it
			{
				g2Info.mGoreSetTag = 0;
			}
		}

		CRenderSurface RS(g2Info.mSurfaceRoot,
			g2Info.mSlist,
			cust_shader,
			fogNum,
			personalModel,
			g2Info.mBoneCache,
			ent->e.renderfx,
			skin,
			(model_t*)g2Info.currentModel,
			whichLod,
			g2Info.mBltlist,
			nullptr,
			gore);
#else
		CRenderSurface RS(g2Info.mSurfaceRoot,
			g2Info.mSlist,
			cust_shader,
			fogNum,
			personalModel,
			g2Info.mBoneCache,
			ent->e.renderfx,
			skin,
			(model_t*)g2Info.currentModel,
			whichLod,
			g2Info.mBltlist);
#endif
		if (!personalModel && (RS.renderfx & RF_SHADOW_PLANE))
		{
			RS.renderfx |= RF_NOSHADOW;
		}

		RenderSurfaces(RS, ent, entityNum);
	}

	HackadelicOnClient = false;

#ifdef G2_PERFORMANCE_ANALYSIS
	G2Time_R_AddGHOULSurfaces += G2PerformanceTimer_R_AddGHOULSurfaces.End();
#endif
}

#ifdef _G2_LISTEN_SERVER_OPT
qboolean G2API_OverrideServerWithClientData(CGhoul2Info_v& ghoul2, int modelIndex);
#endif

bool G2_NeedsRecalc(CGhoul2Info* ghlInfo, int frameNum)
{
	G2_SetupModelPointers(ghlInfo);
	// not sure if I still need this test, probably
	if (ghlInfo->mSkelFrameNum != frameNum ||
		!ghlInfo->mBoneCache ||
		ghlInfo->mBoneCache->mod != ghlInfo->currentModel)
	{
#ifdef _G2_LISTEN_SERVER_OPT
		if (ghlInfo->entityNum != ENTITYNUM_NONE &&
			G2API_OverrideServerWithClientData(ghlInfo, 0))
		{ //if we can manage this, then we don't have to reconstruct
			return false;
		}
#endif
		ghlInfo->mSkelFrameNum = frameNum;
		return true;
	}
	return false;
}

/*
==============
G2_ConstructGhoulSkeleton
builds a complete skeleton for all ghoul models in a CGhoul2Info_v class
using LOD 0
==============
*/
void G2_ConstructGhoulSkeleton(
	CGhoul2Info_v& ghoul2,
	const int frameNum,
	bool checkForNewOrigin,
	const vec3_t scale)
{
#ifdef G2_PERFORMANCE_ANALYSIS
	G2PerformanceTimer_G2_ConstructGhoulSkeleton.Start();
#endif
	int modelCount = 0;
	mdxaBone_t rootMatrix;
	int model_list[256]{};

	assert(ghoul2.size() <= ARRAY_LEN(model_list));
	model_list[255] = 548;

	if (checkForNewOrigin)
	{
		RootMatrix(ghoul2, frameNum, scale, rootMatrix);
	}
	else
	{
		rootMatrix = identityMatrix;
	}

	G2_Sort_Models(ghoul2, model_list, ARRAY_LEN(model_list), &modelCount);
	assert(model_list[255] == 548);

	for (int j = 0; j < modelCount; j++)
	{
		int i = model_list[j];
		CGhoul2Info& g2Info = ghoul2[i];

		if (!g2Info.mValid)
		{
			continue;
		}

		if (j && g2Info.mModelBoltLink != -1)
		{
			int	boltMod = (g2Info.mModelBoltLink >> MODEL_SHIFT) & MODEL_AND;
			int	boltNum = (g2Info.mModelBoltLink >> BOLT_SHIFT) & BOLT_AND;

			mdxaBone_t bolt;
			G2_GetBoltMatrixLow(ghoul2[boltMod], boltNum, scale, bolt);
			G2_TransformGhoulBones(
				g2Info.mBlist,
				bolt,
				g2Info,
				frameNum,
				checkForNewOrigin);
		}
		else
#ifdef _G2_LISTEN_SERVER_OPT
			if (g2Info.entityNum == ENTITYNUM_NONE || g2Info.mSkelFrameNum != frameNum)
#endif
			{
				G2_TransformGhoulBones(
					g2Info.mBlist,
					rootMatrix,
					g2Info,
					frameNum,
					checkForNewOrigin);
			}
	}

#ifdef G2_PERFORMANCE_ANALYSIS
	G2Time_G2_ConstructGhoulSkeleton += G2PerformanceTimer_G2_ConstructGhoulSkeleton.End();
#endif
}

// ===== Sojourn: Warzone backend + loaders below =====
static inline float G2_GetVertBoneWeightNotSlow( const mdxmVertex_t *pVert, const int iWeightNum)
{
	float fBoneWeight;

	int iTemp = pVert->BoneWeightings[iWeightNum];

	iTemp|= (pVert->uiNmWeightsAndBoneIndexes >> (iG2_BONEWEIGHT_TOPBITS_SHIFT+(iWeightNum*2)) ) & iG2_BONEWEIGHT_TOPBITS_AND;

	fBoneWeight = fG2_BONEWEIGHT_RECIPROCAL_MULT * iTemp;

	return fBoneWeight;
}

static void MDXABoneToMatrix ( const mdxaBone_t& bone, matrix_t& matrix )
{
	matrix[0] = bone.matrix[0][0];
	matrix[1] = bone.matrix[1][0];
	matrix[2] = bone.matrix[2][0];
	matrix[3] = 0.0f;

	matrix[4] = bone.matrix[0][1];
	matrix[5] = bone.matrix[1][1];
	matrix[6] = bone.matrix[2][1];
	matrix[7] = 0.0f;

	matrix[8] = bone.matrix[0][2];
	matrix[9] = bone.matrix[1][2];
	matrix[10] = bone.matrix[2][2];
	matrix[11] = 0.0f;

	matrix[12] = bone.matrix[0][3];
	matrix[13] = bone.matrix[1][3];
	matrix[14] = bone.matrix[2][3];
	matrix[15] = 1.0f;
}

//#define __G2_CPU_ANIMATION__

//int maxSeenBoneRefs = 0;

//This is a slightly mangled version of the same function from the sof2sp base.
//It provides a pretty significant performance increase over the existing one.
void RB_SurfaceGhoul( CRenderableSurface *surf ) 
{
	glState.vertexAnimation = qfalse;

#ifndef __G2_CPU_ANIMATION__
	static matrix_t boneMatrices[MAX_GLM_BONEREFS/*80*/] = {}; // UQ1: Gonna limit this to MAX_GLM_BONEREFS (10), and cull any extras. Should be almost unnoticable, but save a bunch of time.

	mdxmSurface_t *surfData = surf->surfaceData;
	mdxmVBOMesh_t *surface = surf->vboMesh;

	if ( surface->vbo == NULL || surface->ibo == NULL )
	{
 		return;
	}

	RB_EndSurface();
	RB_BeginSurface(tess.shader, tess.fogNum, tess.cubemapIndex);

	R_BindVBO(surface->vbo);
	R_BindIBO(surface->ibo);

	if (surface->vbo->vboUsage == GL_STATIC_DRAW && surface->ibo->iboUsage == GL_STATIC_DRAW)
		tess.useInternalVBO = qtrue;
	else
		tess.useInternalVBO = qfalse;

	tess.numIndexes += surface->numIndexes;
	tess.numVertexes += surface->numVertexes;
	tess.minIndex = surface->minIndex;
	tess.maxIndex = surface->maxIndex;
	tess.firstIndex = surface->indexOffset;

	if (surfData->numBoneReferences > 0)
	{// UQ1: if there's no bone refs, then why send the array and do the extre GPU work at all?
		int *boneReferences = (int *)((byte *)surfData + surfData->ofsBoneReferences);

		for (int i = 0; i < surfData->numBoneReferences && i < MAX_GLM_BONEREFS; i++)
		{
			const mdxaBone_t& bone = surf->boneCache->EvalRender(boneReferences[i]);
			MDXABoneToMatrix(bone, boneMatrices[i]);
		}

		glState.boneMatrices = boneMatrices;
		glState.numBones = surfData->numBoneReferences < MAX_GLM_BONEREFS ? surfData->numBoneReferences : MAX_GLM_BONEREFS;
		glState.skeletalAnimation = qtrue;

		/*if (surfData->numBoneReferences > maxSeenBoneRefs)
		{
			ri.Printf(PRINT_WARNING, "Max seen bone refs is now %i. previous was %i.\n", surfData->numBoneReferences, maxSeenBoneRefs);
			maxSeenBoneRefs = surfData->numBoneReferences;
		}*/
	}

	RB_EndSurface();

	// So we don't lerp surfaces that shouldn't be lerped
	glState.skeletalAnimation = qfalse;
#else
#ifdef G2_PERFORMANCE_ANALYSIS
	G2PerformanceTimer_RB_SurfaceGhoul.Start();
#endif

	static int				j, k;
	static int				baseIndex, baseVertex;
	static int				numVerts;
	static mdxmVertex_t 	*v;
	static int				*triangles;
	static int				indexes;
	static glIndex_t		*tessIndexes;
	static mdxmVertexTexCoord_t *pTexCoords;
	static int				*piBoneReferences;

	RB_EndSurface();
	RB_BeginSurface(tess.shader, tess.fogNum, tess.cubemapIndex);

#ifdef _G2_GORE
	if (surf->alternateTex)
	{
		// a gore surface ready to go.

		/*
			sizeof(int)+ // num verts
			sizeof(int)+ // num tris
			sizeof(int)*newNumVerts+ // which verts to copy from original surface
			sizeof(float)*4*newNumVerts+ // storgage for deformed verts
			sizeof(float)*4*newNumVerts+ // storgage for deformed normal
			sizeof(float)*2*newNumVerts+ // texture coordinates
			sizeof(int)*newNumTris*3;  // new indecies
		*/

		int *data=(int *)surf->alternateTex;
		numVerts=*data++;
		indexes=(*data++);
		// first up, sanity check our numbers
		RB_CheckOverflow(numVerts,indexes);
		indexes*=3;

		data+=numVerts;

		baseIndex = tess.numIndexes;
		baseVertex = tess.numVertexes;

		memcpy(&tess.xyz[baseVertex][0],data,sizeof(float)*4*numVerts);
		data+=4*numVerts;
		//memcpy(&tess.normal[baseVertex][0],data,sizeof(float)*4*numVerts);
		//data+=4*numVerts;
		for (int n = 0; n < numVerts; n++)
		{
			tess.normal[n] = R_VboPackNormal((float*)(data + (sizeof(float)*4*n)));
		}
		data+=4*numVerts;
		assert(numVerts>0);

		//float *texCoords = tess.texCoords[0][baseVertex];
		float *texCoords = tess.texCoords[baseVertex][0];
		int hack = baseVertex;
		//rww - since the array is arranged as such we cannot increment
		//the relative memory position to get where we want. Maybe this
		//is why sof2 has the texCoords array reversed. In any case, I
		//am currently too lazy to get around it.
		//Or can you += array[.][x]+2?
		if (surf->scale>1.0f)
		{
			for ( j = 0; j < numVerts; j++) 
			{
				texCoords[0]=((*(float *)data)-0.5f)*surf->scale+0.5f;
				data++;
				texCoords[1]=((*(float *)data)-0.5f)*surf->scale+0.5f;
				data++;
				//texCoords+=2;// Size of gore (s,t).
				hack++;
				texCoords = tess.texCoords[hack][0];
			}
		}
		else
		{
			for (j=0;j<numVerts;j++)
			{
				texCoords[0]=*(float *)(data++);
				texCoords[1]=*(float *)(data++);
//				texCoords+=2;// Size of gore (s,t).
				hack++;
				texCoords = tess.texCoords[hack][0];
			}
		}

		//now check for fade overrides -rww
		if (surf->fade)
		{
			static int lFade;
			static int j;

			if (surf->fade<1.0)
			{
				tess.fading = true;
				lFade = Q_ftol(254.4f*surf->fade);

				for (j=0;j<numVerts;j++)
				{
					tess.svars.colors[j+baseVertex][3] = lFade;
				}
			}
			else if (surf->fade > 2.0f && surf->fade < 3.0f)
			{ //hack to fade out on RGB if desired (don't want to add more to CRenderableSurface) -rww
				tess.fading = true;
				lFade = Q_ftol(254.4f*(surf->fade-2.0f));

				for (j=0;j<numVerts;j++)
				{
					if (lFade < tess.svars.colors[j+baseVertex][0])
					{ //don't set it unless the fade is less than the current r value (to avoid brightening suddenly before we start fading)
						tess.svars.colors[j+baseVertex][0] = tess.svars.colors[j+baseVertex][1] = tess.svars.colors[j+baseVertex][2] = lFade;
					}

					//Set the alpha as well I suppose, no matter what
					tess.svars.colors[j+baseVertex][3] = lFade;
				}
			}
		}
		
		glIndex_t *indexPtr = &tess.indexes[baseIndex];
		triangles = data;
		for (j = indexes ; j ; j--) 
		{
			*indexPtr++ = baseVertex + (*triangles++);
		}
		tess.numIndexes += indexes;
		tess.numVertexes += numVerts; 
		return;
	}
#endif

	// grab the pointer to the surface info within the loaded mesh file
	mdxmSurface_t	*surface = surf->surfaceData;

	CBoneCache *bones = surf->boneCache;

#ifndef _G2_GORE //we use this later, for gore
	delete surf;
#endif

	// first up, sanity check our numbers
	RB_CheckOverflow( surface->numVerts, surface->numTriangles );

	//
	// deform the vertexes by the lerped bones
	//
   
	// first up, sanity check our numbers
	baseVertex = tess.numVertexes;
	triangles = (int *) ((byte *)surface + surface->ofsTriangles);
	baseIndex = tess.numIndexes;
#if 0
	indexes = surface->numTriangles * 3;
	for (j = 0 ; j < indexes ; j++) {
		tess.indexes[baseIndex + j] = baseVertex + triangles[j];
	}
	tess.numIndexes += indexes;
#else
	indexes = surface->numTriangles; //*3;	//unrolled 3 times, don't multiply
	tessIndexes = &tess.indexes[baseIndex];
	for (j = 0 ; j < indexes ; j++) {
		*tessIndexes++ = baseVertex + *triangles++;
		*tessIndexes++ = baseVertex + *triangles++;
		*tessIndexes++ = baseVertex + *triangles++;
	}
	tess.numIndexes += indexes*3;
#endif

	numVerts = surface->numVerts;

	piBoneReferences = (int*) ((byte*)surface + surface->ofsBoneReferences);
	baseVertex = tess.numVertexes;
	v = (mdxmVertex_t *) ((byte *)surface + surface->ofsVerts);
	pTexCoords = (mdxmVertexTexCoord_t *) &v[numVerts];

//	if (r_ghoul2fastnormals&&r_ghoul2fastnormals->integer==0)
#if 0
	if (0)
	{
		for ( j = 0; j < numVerts; j++, baseVertex++,v++ ) 
		{
			const int iNumWeights = G2_GetVertWeights( v );

			float fTotalWeight = 0.0f;

			k=0;
			int		iBoneIndex = G2_GetVertBoneIndex( v, k );
			float	fBoneWeight = G2_GetVertBoneWeight( v, k, fTotalWeight, iNumWeights );
			const mdxaBone_t *bone = &bones->EvalRender(piBoneReferences[iBoneIndex]);

			tess.xyz[baseVertex][0] = fBoneWeight * ( DotProduct( bone->matrix[0], v->vertCoords ) + bone->matrix[0][3] );
			tess.xyz[baseVertex][1] = fBoneWeight * ( DotProduct( bone->matrix[1], v->vertCoords ) + bone->matrix[1][3] );
			tess.xyz[baseVertex][2] = fBoneWeight * ( DotProduct( bone->matrix[2], v->vertCoords ) + bone->matrix[2][3] );

			tess.normal[baseVertex][0] = fBoneWeight * DotProduct( bone->matrix[0], v->normal );
			tess.normal[baseVertex][1] = fBoneWeight * DotProduct( bone->matrix[1], v->normal );
			tess.normal[baseVertex][2] = fBoneWeight * DotProduct( bone->matrix[2], v->normal );

			for ( k++ ; k < iNumWeights ; k++) 
			{
				iBoneIndex	= G2_GetVertBoneIndex( v, k );
				fBoneWeight	= G2_GetVertBoneWeight( v, k, fTotalWeight, iNumWeights );

				bone = &bones->EvalRender(piBoneReferences[iBoneIndex]);

				tess.xyz[baseVertex][0] += fBoneWeight * ( DotProduct( bone->matrix[0], v->vertCoords ) + bone->matrix[0][3] );
				tess.xyz[baseVertex][1] += fBoneWeight * ( DotProduct( bone->matrix[1], v->vertCoords ) + bone->matrix[1][3] );
				tess.xyz[baseVertex][2] += fBoneWeight * ( DotProduct( bone->matrix[2], v->vertCoords ) + bone->matrix[2][3] );

				tess.normal[baseVertex][0] += fBoneWeight * DotProduct( bone->matrix[0], v->normal );
				tess.normal[baseVertex][1] += fBoneWeight * DotProduct( bone->matrix[1], v->normal );
				tess.normal[baseVertex][2] += fBoneWeight * DotProduct( bone->matrix[2], v->normal );
			}

			tess.texCoords[baseVertex][0][0] = pTexCoords[j].texCoords[0];
			tess.texCoords[baseVertex][0][1] = pTexCoords[j].texCoords[1];
		}
	}
	else
	{
#endif
		float fTotalWeight;
		float fBoneWeight;
		float t1;
		float t2;
		const mdxaBone_t *bone;
		const mdxaBone_t *bone2;
		for ( j = 0; j < numVerts; j++, baseVertex++,v++ ) 
		{

			bone = &bones->EvalRender(piBoneReferences[G2_GetVertBoneIndex( v, 0 )]);
			int iNumWeights = G2_GetVertWeights( v );
			//tess.normal[baseVertex][0] = DotProduct( bone->matrix[0], v->normal );
			//tess.normal[baseVertex][1] = DotProduct( bone->matrix[1], v->normal );
			//tess.normal[baseVertex][2] = DotProduct( bone->matrix[2], v->normal );

			vec3_t normal;
			normal[0] = DotProduct(bone->matrix[0], v->normal);
			normal[1] = DotProduct(bone->matrix[1], v->normal);
			normal[2] = DotProduct(bone->matrix[2], v->normal);
			tess.normal[baseVertex] = R_VboPackNormal(normal);

			if (iNumWeights==1)
			{
				tess.xyz[baseVertex][0] = ( DotProduct( bone->matrix[0], v->vertCoords ) + bone->matrix[0][3] );
				tess.xyz[baseVertex][1] = ( DotProduct( bone->matrix[1], v->vertCoords ) + bone->matrix[1][3] );
				tess.xyz[baseVertex][2] = ( DotProduct( bone->matrix[2], v->vertCoords ) + bone->matrix[2][3] );
			}
			else
			{
				fBoneWeight = G2_GetVertBoneWeightNotSlow( v, 0);
				if (iNumWeights==2)
				{
					bone2 = &bones->EvalRender(piBoneReferences[G2_GetVertBoneIndex( v, 1 )]);
					/*
					useless transposition
					tess.xyz[baseVertex][0] =
					v[0]*(w*(bone->matrix[0][0]-bone2->matrix[0][0])+bone2->matrix[0][0])+
					v[1]*(w*(bone->matrix[0][1]-bone2->matrix[0][1])+bone2->matrix[0][1])+
					v[2]*(w*(bone->matrix[0][2]-bone2->matrix[0][2])+bone2->matrix[0][2])+
					w*(bone->matrix[0][3]-bone2->matrix[0][3]) + bone2->matrix[0][3];
					*/
					t1 = ( DotProduct( bone->matrix[0], v->vertCoords ) + bone->matrix[0][3] );
					t2 = ( DotProduct( bone2->matrix[0], v->vertCoords ) + bone2->matrix[0][3] );
					tess.xyz[baseVertex][0] = fBoneWeight * (t1-t2) + t2;
					t1 = ( DotProduct( bone->matrix[1], v->vertCoords ) + bone->matrix[1][3] );
					t2 = ( DotProduct( bone2->matrix[1], v->vertCoords ) + bone2->matrix[1][3] );
					tess.xyz[baseVertex][1] = fBoneWeight * (t1-t2) + t2;
					t1 = ( DotProduct( bone->matrix[2], v->vertCoords ) + bone->matrix[2][3] );
					t2 = ( DotProduct( bone2->matrix[2], v->vertCoords ) + bone2->matrix[2][3] );
					tess.xyz[baseVertex][2] = fBoneWeight * (t1-t2) + t2;
				}
				else
				{

					tess.xyz[baseVertex][0] = fBoneWeight * ( DotProduct( bone->matrix[0], v->vertCoords ) + bone->matrix[0][3] );
					tess.xyz[baseVertex][1] = fBoneWeight * ( DotProduct( bone->matrix[1], v->vertCoords ) + bone->matrix[1][3] );
					tess.xyz[baseVertex][2] = fBoneWeight * ( DotProduct( bone->matrix[2], v->vertCoords ) + bone->matrix[2][3] );

					fTotalWeight=fBoneWeight;
					for (k=1; k < iNumWeights-1 ; k++) 
					{
						bone = &bones->EvalRender(piBoneReferences[G2_GetVertBoneIndex( v, k )]);
						fBoneWeight = G2_GetVertBoneWeightNotSlow( v, k);
						fTotalWeight += fBoneWeight;

						tess.xyz[baseVertex][0] += fBoneWeight * ( DotProduct( bone->matrix[0], v->vertCoords ) + bone->matrix[0][3] );
						tess.xyz[baseVertex][1] += fBoneWeight * ( DotProduct( bone->matrix[1], v->vertCoords ) + bone->matrix[1][3] );
						tess.xyz[baseVertex][2] += fBoneWeight * ( DotProduct( bone->matrix[2], v->vertCoords ) + bone->matrix[2][3] );
					}
					bone = &bones->EvalRender(piBoneReferences[G2_GetVertBoneIndex( v, k )]);
					fBoneWeight	= 1.0f-fTotalWeight;

					tess.xyz[baseVertex][0] += fBoneWeight * ( DotProduct( bone->matrix[0], v->vertCoords ) + bone->matrix[0][3] );
					tess.xyz[baseVertex][1] += fBoneWeight * ( DotProduct( bone->matrix[1], v->vertCoords ) + bone->matrix[1][3] );
					tess.xyz[baseVertex][2] += fBoneWeight * ( DotProduct( bone->matrix[2], v->vertCoords ) + bone->matrix[2][3] );
				}
			}

			tess.texCoords[baseVertex][0][0] = pTexCoords[j].texCoords[0];
			tess.texCoords[baseVertex][0][1] = pTexCoords[j].texCoords[1];
		}
#if 0
	}
#endif

#ifdef _G2_GORE
	CRenderableSurface *storeSurf = surf;

	while (surf->goreChain)
	{
		surf=(CRenderableSurface *)surf->goreChain;
		if (surf->alternateTex)
		{
			// get a gore surface ready to go.

			/*						  
				sizeof(int)+ // num verts
				sizeof(int)+ // num tris
				sizeof(int)*newNumVerts+ // which verts to copy from original surface
				sizeof(float)*4*newNumVerts+ // storgage for deformed verts
				sizeof(float)*4*newNumVerts+ // storgage for deformed normal
				sizeof(float)*2*newNumVerts+ // texture coordinates
				sizeof(int)*newNumTris*3;  // new indecies
			*/

			int *data=(int *)surf->alternateTex;
			int gnumVerts=*data++;
			data++;

			float *fdata=(float *)data;
			fdata+=gnumVerts;
			for (j=0;j<gnumVerts;j++)
			{
				assert(data[j]>=0&&data[j]<numVerts);
				memcpy(fdata,&tess.xyz[tess.numVertexes+data[j]][0],sizeof(float)*3);
				fdata+=4;
			}
			for (j=0;j<gnumVerts;j++)
			{
				assert(data[j]>=0&&data[j]<numVerts);
				//memcpy(fdata,&tess.normal[tess.numVertexes+data[j]][0],sizeof(float)*3);
				R_VboUnpackNormal(fdata+(j*sizeof(float)*3), tess.normal[tess.numVertexes + data[j]]);
				fdata+=4;
			}
		}
		else
		{
			assert(0); 
		}

	}
#endif

	tess.numVertexes += surface->numVerts;

#ifdef G2_PERFORMANCE_ANALYSIS
	G2Time_RB_SurfaceGhoul += G2PerformanceTimer_RB_SurfaceGhoul.End();
#endif
#endif
}
 
/*
=================
R_LoadMDXM - load a Ghoul 2 Mesh file
=================
*/

/*

Some information used in the creation of the JK2 - JKA bone remap table

These are the old bones:
Complete list of all 72 bones:

*/

int OldToNewRemapTable[72] = {
0,// Bone 0:   "model_root":           Parent: ""  (index -1)
1,// Bone 1:   "pelvis":               Parent: "model_root"  (index 0)
2,// Bone 2:   "Motion":               Parent: "pelvis"  (index 1)
3,// Bone 3:   "lfemurYZ":             Parent: "pelvis"  (index 1)
4,// Bone 4:   "lfemurX":              Parent: "pelvis"  (index 1)
5,// Bone 5:   "ltibia":               Parent: "pelvis"  (index 1)
6,// Bone 6:   "ltalus":               Parent: "pelvis"  (index 1)
6,// Bone 7:   "ltarsal":              Parent: "pelvis"  (index 1)
7,// Bone 8:   "rfemurYZ":             Parent: "pelvis"  (index 1)
8,// Bone 9:   "rfemurX":	            Parent: "pelvis"  (index 1)
9,// Bone10:   "rtibia":	            Parent: "pelvis"  (index 1)
10,// Bone11:   "rtalus":	            Parent: "pelvis"  (index 1)
10,// Bone12:   "rtarsal":              Parent: "pelvis"  (index 1)
11,// Bone13:   "lower_lumbar":         Parent: "pelvis"  (index 1)
12,// Bone14:   "upper_lumbar":	        Parent: "lower_lumbar"  (index 13)
13,// Bone15:   "thoracic":	            Parent: "upper_lumbar"  (index 14)
14,// Bone16:   "cervical":	            Parent: "thoracic"  (index 15)
15,// Bone17:   "cranium":              Parent: "cervical"  (index 16)
16,// Bone18:   "ceyebrow":	            Parent: "face_always_"  (index 71)
17,// Bone19:   "jaw":                  Parent: "face_always_"  (index 71)
18,// Bone20:   "lblip2":	            Parent: "face_always_"  (index 71)
19,// Bone21:   "leye":		            Parent: "face_always_"  (index 71)
20,// Bone22:   "rblip2":	            Parent: "face_always_"  (index 71)
21,// Bone23:   "ltlip2":               Parent: "face_always_"  (index 71)
22,// Bone24:   "rtlip2":	            Parent: "face_always_"  (index 71)
23,// Bone25:   "reye":		            Parent: "face_always_"  (index 71)
24,// Bone26:   "rclavical":            Parent: "thoracic"  (index 15)
25,// Bone27:   "rhumerus":             Parent: "thoracic"  (index 15)
26,// Bone28:   "rhumerusX":            Parent: "thoracic"  (index 15)
27,// Bone29:   "rradius":              Parent: "thoracic"  (index 15)
28,// Bone30:   "rradiusX":             Parent: "thoracic"  (index 15)
29,// Bone31:   "rhand":                Parent: "thoracic"  (index 15)
29,// Bone32:   "mc7":                  Parent: "thoracic"  (index 15)
34,// Bone33:   "r_d5_j1":              Parent: "thoracic"  (index 15)
35,// Bone34:   "r_d5_j2":              Parent: "thoracic"  (index 15)
35,// Bone35:   "r_d5_j3":              Parent: "thoracic"  (index 15)
30,// Bone36:   "r_d1_j1":              Parent: "thoracic"  (index 15)
31,// Bone37:   "r_d1_j2":              Parent: "thoracic"  (index 15)
31,// Bone38:   "r_d1_j3":              Parent: "thoracic"  (index 15)
32,// Bone39:   "r_d2_j1":              Parent: "thoracic"  (index 15)
33,// Bone40:   "r_d2_j2":              Parent: "thoracic"  (index 15)
33,// Bone41:   "r_d2_j3":              Parent: "thoracic"  (index 15)
32,// Bone42:   "r_d3_j1":			    Parent: "thoracic"  (index 15)
33,// Bone43:   "r_d3_j2":		        Parent: "thoracic"  (index 15)
33,// Bone44:   "r_d3_j3":              Parent: "thoracic"  (index 15)
34,// Bone45:   "r_d4_j1":              Parent: "thoracic"  (index 15)
35,// Bone46:   "r_d4_j2":	            Parent: "thoracic"  (index 15)
35,// Bone47:   "r_d4_j3":		        Parent: "thoracic"  (index 15)
36,// Bone48:   "rhang_tag_bone":	    Parent: "thoracic"  (index 15)
37,// Bone49:   "lclavical":            Parent: "thoracic"  (index 15)
38,// Bone50:   "lhumerus":	            Parent: "thoracic"  (index 15)
39,// Bone51:   "lhumerusX":	        Parent: "thoracic"  (index 15)
40,// Bone52:   "lradius":	            Parent: "thoracic"  (index 15)
41,// Bone53:   "lradiusX":	            Parent: "thoracic"  (index 15)
42,// Bone54:   "lhand":	            Parent: "thoracic"  (index 15)
42,// Bone55:   "mc5":		            Parent: "thoracic"  (index 15)
43,// Bone56:   "l_d5_j1":	            Parent: "thoracic"  (index 15)
44,// Bone57:   "l_d5_j2":	            Parent: "thoracic"  (index 15)
44,// Bone58:   "l_d5_j3":	            Parent: "thoracic"  (index 15)
43,// Bone59:   "l_d4_j1":	            Parent: "thoracic"  (index 15)
44,// Bone60:   "l_d4_j2":	            Parent: "thoracic"  (index 15)
44,// Bone61:   "l_d4_j3":	            Parent: "thoracic"  (index 15)
45,// Bone62:   "l_d3_j1":	            Parent: "thoracic"  (index 15)
46,// Bone63:   "l_d3_j2":	            Parent: "thoracic"  (index 15)
46,// Bone64:   "l_d3_j3":	            Parent: "thoracic"  (index 15)
45,// Bone65:   "l_d2_j1":	            Parent: "thoracic"  (index 15)
46,// Bone66:   "l_d2_j2":	            Parent: "thoracic"  (index 15)
46,// Bone67:   "l_d2_j3":	            Parent: "thoracic"  (index 15)
47,// Bone68:   "l_d1_j1":				Parent: "thoracic"  (index 15)
48,// Bone69:   "l_d1_j2":	            Parent: "thoracic"  (index 15)
48,// Bone70:   "l_d1_j3":				Parent: "thoracic"  (index 15)
52// Bone71:   "face_always_":			Parent: "cranium"  (index 17)
};


/*

Bone   0:   "model_root":
            Parent: ""  (index -1)
            #Kids:  1
            Child 0: (index 1), name "pelvis"

Bone   1:   "pelvis":
            Parent: "model_root"  (index 0)
            #Kids:  4
            Child 0: (index 2), name "Motion"
            Child 1: (index 3), name "lfemurYZ"
            Child 2: (index 7), name "rfemurYZ"
            Child 3: (index 11), name "lower_lumbar"

Bone   2:   "Motion":
            Parent: "pelvis"  (index 1)
            #Kids:  0

Bone   3:   "lfemurYZ":
            Parent: "pelvis"  (index 1)
            #Kids:  3
            Child 0: (index 4), name "lfemurX"
            Child 1: (index 5), name "ltibia"
            Child 2: (index 49), name "ltail"

Bone   4:   "lfemurX":
            Parent: "lfemurYZ"  (index 3)
            #Kids:  0

Bone   5:   "ltibia":
            Parent: "lfemurYZ"  (index 3)
            #Kids:  1
            Child 0: (index 6), name "ltalus"

Bone   6:   "ltalus":
            Parent: "ltibia"  (index 5)
            #Kids:  0

Bone   7:   "rfemurYZ":
            Parent: "pelvis"  (index 1)
            #Kids:  3
            Child 0: (index 8), name "rfemurX"
            Child 1: (index 9), name "rtibia"
            Child 2: (index 50), name "rtail"

Bone   8:   "rfemurX":
            Parent: "rfemurYZ"  (index 7)
            #Kids:  0

Bone   9:   "rtibia":
            Parent: "rfemurYZ"  (index 7)
            #Kids:  1
            Child 0: (index 10), name "rtalus"

Bone  10:   "rtalus":
            Parent: "rtibia"  (index 9)
            #Kids:  0

Bone  11:   "lower_lumbar":
            Parent: "pelvis"  (index 1)
            #Kids:  1
            Child 0: (index 12), name "upper_lumbar"

Bone  12:   "upper_lumbar":
            Parent: "lower_lumbar"  (index 11)
            #Kids:  1
            Child 0: (index 13), name "thoracic"

Bone  13:   "thoracic":
            Parent: "upper_lumbar"  (index 12)
            #Kids:  5
            Child 0: (index 14), name "cervical"
            Child 1: (index 24), name "rclavical"
            Child 2: (index 25), name "rhumerus"
            Child 3: (index 37), name "lclavical"
            Child 4: (index 38), name "lhumerus"

Bone  14:   "cervical":
            Parent: "thoracic"  (index 13)
            #Kids:  1
            Child 0: (index 15), name "cranium"

Bone  15:   "cranium":
            Parent: "cervical"  (index 14)
            #Kids:  1
            Child 0: (index 52), name "face_always_"

Bone  16:   "ceyebrow":
            Parent: "face_always_"  (index 52)
            #Kids:  0

Bone  17:   "jaw":
            Parent: "face_always_"  (index 52)
            #Kids:  0

Bone  18:   "lblip2":
            Parent: "face_always_"  (index 52)
            #Kids:  0

Bone  19:   "leye":
            Parent: "face_always_"  (index 52)
            #Kids:  0

Bone  20:   "rblip2":
            Parent: "face_always_"  (index 52)
            #Kids:  0

Bone  21:   "ltlip2":
            Parent: "face_always_"  (index 52)
            #Kids:  0

Bone  22:   "rtlip2":
            Parent: "face_always_"  (index 52)
            #Kids:  0

Bone  23:   "reye":
            Parent: "face_always_"  (index 52)
            #Kids:  0

Bone  24:   "rclavical":
            Parent: "thoracic"  (index 13)
            #Kids:  0

Bone  25:   "rhumerus":
            Parent: "thoracic"  (index 13)
            #Kids:  2
            Child 0: (index 26), name "rhumerusX"
            Child 1: (index 27), name "rradius"

Bone  26:   "rhumerusX":
            Parent: "rhumerus"  (index 25)
            #Kids:  0

Bone  27:   "rradius":
            Parent: "rhumerus"  (index 25)
            #Kids:  9
            Child 0: (index 28), name "rradiusX"
            Child 1: (index 29), name "rhand"
            Child 2: (index 30), name "r_d1_j1"
            Child 3: (index 31), name "r_d1_j2"
            Child 4: (index 32), name "r_d2_j1"
            Child 5: (index 33), name "r_d2_j2"
            Child 6: (index 34), name "r_d4_j1"
            Child 7: (index 35), name "r_d4_j2"
            Child 8: (index 36), name "rhang_tag_bone"

Bone  28:   "rradiusX":
            Parent: "rradius"  (index 27)
            #Kids:  0

Bone  29:   "rhand":
            Parent: "rradius"  (index 27)
            #Kids:  0

Bone  30:   "r_d1_j1":
            Parent: "rradius"  (index 27)
            #Kids:  0

Bone  31:   "r_d1_j2":
            Parent: "rradius"  (index 27)
            #Kids:  0

Bone  32:   "r_d2_j1":
            Parent: "rradius"  (index 27)
            #Kids:  0

Bone  33:   "r_d2_j2":
            Parent: "rradius"  (index 27)
            #Kids:  0

Bone  34:   "r_d4_j1":
            Parent: "rradius"  (index 27)
            #Kids:  0

Bone  35:   "r_d4_j2":
            Parent: "rradius"  (index 27)
            #Kids:  0

Bone  36:   "rhang_tag_bone":
            Parent: "rradius"  (index 27)
            #Kids:  0

Bone  37:   "lclavical":
            Parent: "thoracic"  (index 13)
            #Kids:  0

Bone  38:   "lhumerus":
            Parent: "thoracic"  (index 13)
            #Kids:  2
            Child 0: (index 39), name "lhumerusX"
            Child 1: (index 40), name "lradius"

Bone  39:   "lhumerusX":
            Parent: "lhumerus"  (index 38)
            #Kids:  0

Bone  40:   "lradius":
            Parent: "lhumerus"  (index 38)
            #Kids:  9
            Child 0: (index 41), name "lradiusX"
            Child 1: (index 42), name "lhand"
            Child 2: (index 43), name "l_d4_j1"
            Child 3: (index 44), name "l_d4_j2"
            Child 4: (index 45), name "l_d2_j1"
            Child 5: (index 46), name "l_d2_j2"
            Child 6: (index 47), name "l_d1_j1"
            Child 7: (index 48), name "l_d1_j2"
            Child 8: (index 51), name "lhang_tag_bone"

Bone  41:   "lradiusX":
            Parent: "lradius"  (index 40)
            #Kids:  0

Bone  42:   "lhand":
            Parent: "lradius"  (index 40)
            #Kids:  0

Bone  43:   "l_d4_j1":
            Parent: "lradius"  (index 40)
            #Kids:  0

Bone  44:   "l_d4_j2":
            Parent: "lradius"  (index 40)
            #Kids:  0

Bone  45:   "l_d2_j1":
            Parent: "lradius"  (index 40)
            #Kids:  0

Bone  46:   "l_d2_j2":
            Parent: "lradius"  (index 40)
            #Kids:  0

Bone  47:   "l_d1_j1":
            Parent: "lradius"  (index 40)
            #Kids:  0

Bone  48:   "l_d1_j2":
            Parent: "lradius"  (index 40)
            #Kids:  0

Bone  49:   "ltail":
            Parent: "lfemurYZ"  (index 3)
            #Kids:  0

Bone  50:   "rtail":
            Parent: "rfemurYZ"  (index 7)
            #Kids:  0

Bone  51:   "lhang_tag_bone":
            Parent: "lradius"  (index 40)
            #Kids:  0

Bone  52:   "face_always_":
            Parent: "cranium"  (index 15)
            #Kids:  8
            Child 0: (index 16), name "ceyebrow"
            Child 1: (index 17), name "jaw"
            Child 2: (index 18), name "lblip2"
            Child 3: (index 19), name "leye"
            Child 4: (index 20), name "rblip2"
            Child 5: (index 21), name "ltlip2"
            Child 6: (index 22), name "rtlip2"
            Child 7: (index 23), name "reye"



*/


qboolean model_upload_mdxm_to_gpu(model_t *mod, qboolean isKyle);

extern std::string R_FindAndAdjustShaderNames(std::string modelName, std::string surfaceName, std::string shaderPath);

//#define __REGENERATE_GLM_NORMALS__

#ifdef __REGENERATE_GLM_NORMALS__
// assimp include files. These three are usually needed.
#include "assimp/Importer.hpp"	//OO version Header!
#include "assimp/postprocess.h"
#include "assimp/scene.h"
#include "assimp/DefaultLogger.hpp"
#include "assimp/LogStream.hpp"

void GenerateNormalsForGLM(mdxmSurface_t *surf)
{
	int numIndexes = surf->numTriangles;
	int numVerts = surf->numVerts;
	
	//#pragma omp parallel for schedule(dynamic)
	for (int i = 0; i < numIndexes; i ++)
	{
		mdxmTriangle_t *thisTri = (mdxmTriangle_t *)((byte *)surf + surf->ofsTriangles + i);

		int tri[3];
		tri[0] = thisTri->indexes[0];
		tri[1] = thisTri->indexes[1];
		tri[2] = thisTri->indexes[2];

		mdxmVertex_t *t1 = (mdxmVertex_t *)((byte *)surf + surf->ofsVerts + tri[0]);
		mdxmVertex_t *t2 = (mdxmVertex_t *)((byte *)surf + surf->ofsVerts + tri[1]);
		mdxmVertex_t *t3 = (mdxmVertex_t *)((byte *)surf + surf->ofsVerts + tri[2]);
		
		float* a = (float *)t1->vertCoords;
		float* b = (float *)t2->vertCoords;
		float* c = (float *)t3->vertCoords;
		vec3_t ba, ca;
		VectorSubtract(b, a, ba);
		VectorSubtract(c, a, ca);

		vec3_t normal;
		CrossProduct(ca, ba, normal);
		VectorNormalize(normal);

		//ri.Printf(PRINT_WARNING, "OLD: %f %f %f. NEW: %f %f %f.\n", cv->verts[tri[0]].normal[0], cv->verts[tri[0]].normal[1], cv->verts[tri[0]].normal[2], normal[0], normal[1], normal[2]);

#pragma omp critical
		{
			VectorCopy(normal, t1->normal);
			VectorCopy(normal, t2->normal);
			VectorCopy(normal, t3->normal);
		}

		//ForceCrash();
	}

	// Now the hard part, make smooth normals...
#pragma omp parallel for schedule(dynamic)
	for (int i = 0; i < numVerts; i++)
	{
		int verticesFound[65536];
		int numVerticesFound = 0;

		mdxmVertex_t *vIn = (mdxmVertex_t *)((byte *)surf + surf->ofsVerts + i);

		// Get all vertices that share this one ...
		for (int v = 0; v < numIndexes; v += 3)
		{
			mdxmTriangle_t *thisTri = (mdxmTriangle_t *)((byte *)surf + surf->ofsTriangles + v);

			int tri[3];
			tri[0] = thisTri->indexes[0];
			tri[1] = thisTri->indexes[1];
			tri[2] = thisTri->indexes[2];

			mdxmVertex_t *t0 = (mdxmVertex_t *)((byte *)surf + surf->ofsVerts + tri[0]);
			mdxmVertex_t *t1 = (mdxmVertex_t *)((byte *)surf + surf->ofsVerts + tri[1]);
			mdxmVertex_t *t2 = (mdxmVertex_t *)((byte *)surf + surf->ofsVerts + tri[2]);

			if (tri[0] == i && Distance(vIn->normal, t0->normal) <= 1.0)
			{
#pragma omp critical
				{
					verticesFound[numVerticesFound] = tri[0];
					numVerticesFound++;
					verticesFound[numVerticesFound] = tri[1];
					numVerticesFound++;
					verticesFound[numVerticesFound] = tri[2];
					numVerticesFound++;
				}
				continue;
			}

			if (tri[1] == i && Distance(vIn->normal, t1->normal) <= 1.0)
			{
#pragma omp critical
				{
					verticesFound[numVerticesFound] = tri[0];
					numVerticesFound++;
					verticesFound[numVerticesFound] = tri[1];
					numVerticesFound++;
					verticesFound[numVerticesFound] = tri[2];
					numVerticesFound++;
				}
				continue;
			}

			if (tri[2] == i && Distance(vIn->normal, t2->normal) <= 1.0)
			{
#pragma omp critical
				{
					verticesFound[numVerticesFound] = tri[0];
					numVerticesFound++;
					verticesFound[numVerticesFound] = tri[1];
					numVerticesFound++;
					verticesFound[numVerticesFound] = tri[2];
					numVerticesFound++;
				}
				continue;
			}

			// Also merge close normals...
			if (tri[0] != i
				&& Distance(vIn->vertCoords, t0->vertCoords) <= 4.0
				&& Distance(vIn->normal, t0->normal) <= 1.0)
			{
#pragma omp critical
				{
					verticesFound[numVerticesFound] = tri[0];
					numVerticesFound++;
					verticesFound[numVerticesFound] = tri[1];
					numVerticesFound++;
					verticesFound[numVerticesFound] = tri[2];
					numVerticesFound++;
				}
				continue;
			}

			if (tri[1] != i
				&& Distance(vIn->vertCoords, t1->vertCoords) <= 4.0
				&& Distance(vIn->normal, t1->normal) <= 1.0)
			{
#pragma omp critical
				{
					verticesFound[numVerticesFound] = tri[0];
					numVerticesFound++;
					verticesFound[numVerticesFound] = tri[1];
					numVerticesFound++;
					verticesFound[numVerticesFound] = tri[2];
					numVerticesFound++;
				}
				continue;
			}

			if (tri[2] != i
				&& Distance(vIn->vertCoords, t2->vertCoords) <= 4.0
				&& Distance(vIn->normal, t2->normal) <= 1.0)
			{
#pragma omp critical
				{
					verticesFound[numVerticesFound] = tri[0];
					numVerticesFound++;
					verticesFound[numVerticesFound] = tri[1];
					numVerticesFound++;
					verticesFound[numVerticesFound] = tri[2];
					numVerticesFound++;
				}
				continue;
			}
		}

		aiVector3D pcNor;
		pcNor.Set(vIn->normal[0], vIn->normal[1], vIn->normal[2]);
		int numAdded = 1;
		for (unsigned int a = 0; a < numVerticesFound; ++a) {
			aiVector3D thisNor;
			mdxmVertex_t *vNew = (mdxmVertex_t *)((byte *)surf + surf->ofsVerts + verticesFound[a]);
			thisNor.Set(vNew->normal[0], vNew->normal[1], vNew->normal[2]);
			pcNor += thisNor;
			numAdded++;
		}
		pcNor /= numAdded;
		pcNor.NormalizeSafe();

#pragma omp critical
		{
			VectorSet(vIn->normal, pcNor.x, pcNor.y, pcNor.z);
		}
	}
}
#endif //__REGENERATE_GLM_NORMALS__

model_t			*kyleModel = NULL;
qhandle_t		kyleQhandle = -1;
qboolean		kyleLoading = qfalse;
CGhoul2Info_v	*precachedKyle = NULL;
mdxmHeader_t	*kyleMdxm = NULL;

#ifdef __NIF_GLM_IMPORT_TEST__
extern qboolean R_LoadPlayerNIF(model_t *mod, void *buffer, const char *mod_name, qboolean &bAlreadyCached);
#endif //__NIF_GLM_IMPORT_TEST__

qboolean R_LoadMDXM( model_t *mod, void *buffer, const char *mod_name, qboolean &bAlreadyCached ) {
	int					i,l, j;
	mdxmHeader_t		*pinmodel, *mdxm;
	mdxmLOD_t			*lod;
	mdxmSurface_t		*surf;
	int					version;
	int					size;
	mdxmSurfHierarchy_t	*surfInfo;

	if (!kyleLoading && !kyleMdxm && strcmp(mod->name, "models/players/kyle/model.glm"))
	{
		ri.Printf(PRINT_ALL, "Loading kyle model.\n");
		//kyleLoading = qtrue;
		//kyleQhandle = -1;
		kyleQhandle = RE_RegisterModel("models/players/kyle/model.glm");
	}

	if (!kyleLoading && !kyleMdxm && !strcmp(mod->name, "models/players/kyle/model.glm"))
	{
		ri.Printf(PRINT_ALL, "Precaching kyle model.\n");
		kyleLoading = qtrue;
		//kyleQhandle = RE_RegisterModel("models/players/kyle/model.glm");
		//kyleQhandle = G2API_InitGhoul2Model(&precachedKyle, "models/players/kyle/model.glm", 0, 0, -20, 0, 0);
	}
	
#ifdef __NIF_GLM_IMPORT_TEST__
	if (R_LoadPlayerNIF(mod, buffer, mod_name, bAlreadyCached))
	{
		return qtrue;
	}
#endif //__NIF_GLM_IMPORT_TEST__
    
	pinmodel= (mdxmHeader_t *)buffer;
	//
	// read some fields from the binary, but only LittleLong() them when we know this wasn't an already-cached model...
	//	
	version = (pinmodel->version);
	size	= (pinmodel->ofsEnd);

	if (!bAlreadyCached)
	{
		LL(version);
		LL(size);
	}

	if (version != MDXM_VERSION) {
		Com_Printf (S_COLOR_YELLOW  "R_LoadMDXM: %s has wrong version (%i should be %i)\n",
				 mod_name, version, MDXM_VERSION);
		return qfalse;
	}

	mod->type	   = MOD_MDXM;
	mod->dataSize += size;	
	
	qboolean bAlreadyFound = qfalse;
	mdxm = (mdxmHeader_t*)CModelCache->Allocate(size, buffer, mod_name, &bAlreadyFound, TAG_MODEL_GLM);
	mod->data.glm = (mdxmData_t *)Hunk_Alloc(sizeof (mdxmData_t), h_low);
	mod->data.glm->header = mdxm;

	//RE_RegisterModels_Malloc(size, buffer, mod_name, &bAlreadyFound, TAG_MODEL_GLM);

	assert(bAlreadyCached == bAlreadyFound);

	if (!bAlreadyFound)
	{
		// horrible new hackery, if !bAlreadyFound then we've just done a tag-morph, so we need to set the 
		//	bool reference passed into this function to true, to tell the caller NOT to do an ri.FS_Freefile since
		//	we've hijacked that memory block...
		//
		// Aaaargh. Kill me now...
		//
		bAlreadyCached = qtrue;
		assert( mdxm == buffer );
//		memcpy( mdxm, buffer, size );	// and don't do this now, since it's the same thing

		LL(mdxm->ident);
		LL(mdxm->version);
		LL(mdxm->numLODs);
		LL(mdxm->ofsLODs);
		LL(mdxm->numSurfaces);
		LL(mdxm->ofsSurfHierarchy);
		LL(mdxm->ofsEnd);
	}

	// first up, go load in the animation file we need that has the skeletal animation info for this model
	mdxm->animIndex = RE_RegisterModel(va("%s.gla", mdxm->animName));

	if (!mdxm->animIndex)
	{
		// Fallback to default...
		strcpy(mdxm->animName, "models/players/_humanoid/_humanoid.gla");
		mdxm->animIndex = RE_RegisterModel(mdxm->animName);

		if (!mdxm->animIndex)
		{
			Com_Printf(S_COLOR_YELLOW  "R_LoadMDXM: missing animation file %s for mesh %s\n", mdxm->animName, mdxm->name);
			return qfalse;
		}
	}

#ifdef __G2_LODS_ENABLED__
	mod->numLods = mdxm->numLODs -1 ;	//copy this up to the model for ease of use - it wil get inced after this.
#else //!__G2_LODS_ENABLED__
	//
	// G2 lods are messed up (vanishing NPCs etc), and no faster then dumping the verts anyway (the shader switches are the slow part, not the geometry count)...
	// This shoule also speed loading and reduce vram usage...
	//
	mod->numLods = 0;
#endif //__G2_LODS_ENABLED__

	if (bAlreadyFound)
	{
		return qtrue;	// All done. Stop, go no further, do not LittleLong(), do not pass Go...
	}

	bool isAnOldModelFile = false;
	if (mdxm->numBones == 72 && strstr(mdxm->animName,"_humanoid") )
	{
		isAnOldModelFile = true;
	}

	surfInfo = (mdxmSurfHierarchy_t *)( (byte *)mdxm + mdxm->ofsSurfHierarchy);
 	for ( i = 0 ; i < mdxm->numSurfaces ; i++) 
	{
		LL(surfInfo->numChildren);
		LL(surfInfo->parentIndex);

		Q_strlwr(surfInfo->name);	//just in case
		if ( !strcmp( &surfInfo->name[strlen(surfInfo->name)-4],"_off") )
		{
			surfInfo->name[strlen(surfInfo->name)-4]=0;	//remove "_off" from name
		}

		// do all the children indexs
		for (j=0; j<surfInfo->numChildren; j++)
		{
			LL(surfInfo->childIndexes[j]);
		}

		shader_t	*sh;

		// get the shader name
#if 1 // Search for missing textures...
		std::string textureName = surfInfo->shader;

		std::string finalPath = R_FindAndAdjustShaderNames(mod_name, surfInfo->name, textureName);
		sh = R_FindShader(finalPath.c_str(), lightmapsNone, stylesDefault, qtrue);

		if (sh == NULL || sh == tr.defaultShader)
		{
//#ifdef __DEBUG_ASSIMP__
//			ri.Printf(PRINT_ALL, "Model: %s. Mesh: %s (%i). Missing texture: %s.\n", mod_name, (surfInfo->name && surfInfo->name[0]) ? surfInfo->name : "NULL", i, finalPath.length() ? finalPath.c_str() : "NULL");
//#endif //__DEBUG_ASSIMP__
			sh = tr.defaultShader;
		}
#else
		sh = R_FindShader(surfInfo->shader, lightmapsNone, stylesDefault, qtrue);
#endif

		// insert it in the surface list
		if ( sh->defaultShader ) 
		{
			surfInfo->shaderIndex = 0;
		}
		else
		{
			surfInfo->shaderIndex = sh->index;
		}

		CModelCache->StoreShaderRequest(mod_name, &surfInfo->shader[0], &surfInfo->shaderIndex);		

		// find the next surface
		surfInfo = (mdxmSurfHierarchy_t *)( (byte *)&surfInfo->childIndexes[surfInfo->numChildren] );
  	}
	
	// swap all the LOD's	(we need to do the middle part of this even for intel, because of shader reg and err-check)
	lod = (mdxmLOD_t *) ( (byte *)mdxm + mdxm->ofsLODs );
	for ( l = 0 ; l < mdxm->numLODs ; l++)
	{
		int	triCount = 0;

		LL(lod->ofsEnd);
		// swap all the surfaces
		surf = (mdxmSurface_t *) ( (byte *)lod + sizeof (mdxmLOD_t) + (mdxm->numSurfaces * sizeof(mdxmLODSurfOffset_t)) );
		for ( i = 0 ; i < mdxm->numSurfaces ; i++) 
		{
			LL(surf->numTriangles);
			LL(surf->ofsTriangles);
			LL(surf->numVerts);
			LL(surf->ofsVerts);
			LL(surf->ofsEnd);
			LL(surf->ofsHeader);
			LL(surf->numBoneReferences);
			LL(surf->ofsBoneReferences);
//			LL(surf->maxVertBoneWeights);

			triCount += surf->numTriangles;
#if 0										
			if ( surf->numVerts > SHADER_MAX_VERTEXES ) {
				Com_Error (ERR_DROP, "R_LoadMDXM: %s has more than %i verts on a surface (%i)",
					mod_name, SHADER_MAX_VERTEXES, surf->numVerts );
			}
			if ( surf->numTriangles*3 > SHADER_MAX_INDEXES ) {
				Com_Error (ERR_DROP, "R_LoadMDXM: %s has more than %i triangles on a surface (%i)",
					mod_name, SHADER_MAX_INDEXES / 3, surf->numTriangles );
			}
#endif
		
			// change to surface identifier
			surf->ident = SF_MDX;
			// register the shaders
#if 0 //#ifndef _M_IX86
//
// optimisation, we don't bother doing this for standard intel case since our data's already in that format...
//
			// FIXME - is this correct? 
			// do all the bone reference data
			boneRef = (int *) ( (byte *)surf + surf->ofsBoneReferences );
			for ( j = 0 ; j < surf->numBoneReferences ; j++ ) 
			{
					LL(boneRef[j]);
			}
			
			// swap all the triangles
			tri = (mdxmTriangle_t *) ( (byte *)surf + surf->ofsTriangles );
			for ( j = 0 ; j < surf->numTriangles ; j++, tri++ ) 
			{
				LL(tri->indexes[0]);
				LL(tri->indexes[1]);
				LL(tri->indexes[2]);
			}

			// swap all the vertexes
			v = (mdxmVertex_t *) ( (byte *)surf + surf->ofsVerts );
			for ( j = 0 ; j < surf->numVerts ; j++ ) 
			{
				v->normal[0] = LittleFloat( v->normal[0] );
				v->normal[1] = LittleFloat( v->normal[1] );
				v->normal[2] = LittleFloat( v->normal[2] );

				v->texCoords[0] = LittleFloat( v->texCoords[0] );
				v->texCoords[1] = LittleFloat( v->texCoords[1] );

				v->numWeights = LittleLong( v->numWeights );
  			    v->offset[0] = LittleFloat( v->offset[0] );
				v->offset[1] = LittleFloat( v->offset[1] );
				v->offset[2] = LittleFloat( v->offset[2] );

				for ( k = 0 ; k < /*v->numWeights*/surf->maxVertBoneWeights ; k++ ) 
				{
					v->weights[k].boneIndex = LittleLong( v->weights[k].boneIndex );
					v->weights[k].boneWeight = LittleFloat( v->weights[k].boneWeight );
				}
				v = (mdxmVertex_t *)&v->weights[/*v->numWeights*/surf->maxVertBoneWeights];
			}
#endif
			/*if (1)
			{
				mdxmVertex_t *v = (mdxmVertex_t *)((byte *)surf + surf->ofsVerts);
			}*/

			if (isAnOldModelFile)
			{
				int *boneRef = (int *) ( (byte *)surf + surf->ofsBoneReferences );
				for ( j = 0 ; j < surf->numBoneReferences ; j++ ) 
				{
					if (boneRef[j] >= 0 && boneRef[j] < 72)
					{
						boneRef[j]=OldToNewRemapTable[boneRef[j]];
					}
					else
					{
						boneRef[j]=0;
					}
				}
			}
			
			// find the next surface
			surf = (mdxmSurface_t *)( (byte *)surf + surf->ofsEnd );

#ifdef __REGENERATE_GLM_NORMALS__
			GenerateNormalsForGLM(surf);
#endif //__REGENERATE_GLM_NORMALS__
		}
		// find the next LOD
		lod = (mdxmLOD_t *)( (byte *)lod + lod->ofsEnd );
	}

	if (kyleLoading)
	{
		kyleModel = mod;
		kyleLoading = qfalse;
		return model_upload_mdxm_to_gpu(mod, qtrue);
	}
	else
	{
		return model_upload_mdxm_to_gpu(mod, qfalse);
	}
}

template <typename T> int sgn(T val) {
	return (T(0) < val) - (val < T(0));
}

#ifdef __EXPERIMENTAL_AUTO_BONE_WEIGHTING__
vec3_t		kyle_bounds[2];
vec3_t		kyle_size;
int			kyle_numVerts = 0;
vec3_t		kyle_verts[8192] = { { 0 } };
vec4_t		kyle_bonerefs[8192] = { { 0 } };
vec4_t		kyle_weights[8192] = { { 0 } };
#endif //__EXPERIMENTAL_AUTO_BONE_WEIGHTING__

qboolean model_upload_mdxm_to_gpu(model_t *mod, qboolean isKyle) {

	mdxmHeader_t *mdxm = mdxmHeader(mod);

	if (isKyle)
	{
		kyleMdxm = mdxm;
	}

	// Make a copy on the GPU
	mdxmLOD_t *lod = (mdxmLOD_t *)((byte *)mdxm + mdxm->ofsLODs);
	mod->data.glm->vboModels = (mdxmVBOModel_t *)Hunk_Alloc(sizeof (mdxmVBOModel_t) * mdxm->numLODs, h_low);
	for (int l = 0; l < mdxm->numLODs; l++ )
	{
		mdxmVBOModel_t *vboModel = &mod->data.glm->vboModels[l];
		mdxmVBOMesh_t *vboMeshes;

		vec3_t *verts;
		uint32_t *normals;
		vec2_t *texcoords;
		vec4_t *bonerefs;
		vec4_t *weights;

		byte *data;
		int dataSize = 0;
		int ofsPosition, ofsNormals, ofsTexcoords, ofsBoneRefs, ofsWeights;
		int stride = 0;
		int numVerts = 0;
		int numTriangles = 0;

		vec3_t *tangentsf;
		vec3_t *bitangentsf;

		// +1 to add total vertex count
		int *baseVertexes = (int *)Hunk_AllocateTempMemory(sizeof (int) * (mdxm->numSurfaces + 1));
		int *indexOffsets = (int *)Hunk_AllocateTempMemory(sizeof (int) * mdxm->numSurfaces);
		memset(baseVertexes, 0, sizeof(int) * (mdxm->numSurfaces + 1));
		memset(indexOffsets, 0, sizeof(int) * mdxm->numSurfaces);

		vboModel->numVBOMeshes = mdxm->numSurfaces;
		vboModel->vboMeshes = (mdxmVBOMesh_t *)Hunk_Alloc(sizeof (mdxmVBOMesh_t) * mdxm->numSurfaces, h_low);
		vboMeshes = vboModel->vboMeshes;

		mdxmSurface_t *surf = (mdxmSurface_t *)((byte *)lod + sizeof (mdxmLOD_t) + (mdxm->numSurfaces * sizeof (mdxmLODSurfOffset_t)));

		// Calculate the required size of the vertex buffer.
		for ( int n = 0; n < mdxm->numSurfaces; n++ )
		{
			baseVertexes[n] = numVerts;
			indexOffsets[n] = numTriangles * 3;

			numVerts += surf->numVerts;
			numTriangles += surf->numTriangles;

			surf = (mdxmSurface_t *)((byte *)surf + surf->ofsEnd);
		}

		baseVertexes[mdxm->numSurfaces] = numVerts;

		tangentsf = (vec3_t *)Hunk_AllocateTempMemory(sizeof (vec3_t) * numVerts);
		bitangentsf = (vec3_t *)Hunk_AllocateTempMemory(sizeof (vec3_t) * numVerts);
		memset(tangentsf, 0, sizeof (vec3_t) * numVerts);
		memset(bitangentsf, 0, sizeof (vec3_t) * numVerts);

		dataSize += numVerts * sizeof (*verts);
		dataSize += numVerts * sizeof (*normals);
		dataSize += numVerts * sizeof (*texcoords);
		dataSize += numVerts * sizeof (*weights);
		dataSize += numVerts * sizeof (*bonerefs);

		// Allocate and write to memory
		data = (byte *)Hunk_AllocateTempMemory(dataSize);

		verts = (vec3_t *)(data + stride);
		ofsPosition = stride;
		stride += sizeof (*verts);

		normals = (uint32_t *)(data + stride);
		ofsNormals = stride;
		stride += sizeof (*normals);

		texcoords = (vec2_t *)(data + stride);
		ofsTexcoords = stride;
		stride += sizeof (*texcoords);

		bonerefs = (vec4_t *)(data + stride);
		ofsBoneRefs = stride;
		stride += sizeof (*bonerefs);

		weights = (vec4_t *)(data + stride);
		ofsWeights = stride;
		stride += sizeof (*weights);

		vec4_t *bonerefsBegin = bonerefs;
		vec4_t *weightsBegin = weights;

		surf = (mdxmSurface_t *)((byte *)lod + sizeof (mdxmLOD_t) + (mdxm->numSurfaces * sizeof (mdxmLODSurfOffset_t)));


#ifdef __EXPERIMENTAL_AUTO_BONE_WEIGHTING__
		qboolean	hasBones = qfalse;
		vec3_t		model_bounds[2];
		vec3_t		model_size = { 0.0 };

		if (isKyle /*&& lod == 0*/)
		{
			hasBones = qtrue;

			ClearBounds(kyle_bounds[0], kyle_bounds[1]);

			surf = (mdxmSurface_t *)((byte *)lod + sizeof(mdxmLOD_t) + (mdxm->numSurfaces * sizeof(mdxmLODSurfOffset_t)));

			// See if this model has any weights/bonerefs...
			for (int n = 0; n < mdxm->numSurfaces; n++)
			{
				// Positions and normals
				mdxmVertex_t *vert = (mdxmVertex_t *)((byte *)surf + surf->ofsVerts);

				for (int k = 0; k < surf->numVerts; k++)
				{
					AddPointToBounds(vert->vertCoords, kyle_bounds[0], kyle_bounds[1]);
					vert++;
				}

				surf = (mdxmSurface_t *)((byte *)surf + surf->ofsEnd);
			}

			kyle_size[0] = kyle_bounds[1][0] - kyle_bounds[0][0];
			kyle_size[1] = kyle_bounds[1][1] - kyle_bounds[0][1];
			kyle_size[2] = kyle_bounds[1][2] - kyle_bounds[0][2];

			ri.Printf(PRINT_WARNING, "EXPERIMENTAL_AUTO_BONE_WEIGHTING: Kyle bounds are (%f %f %f) (%f %f %f).\n", kyle_bounds[0][0], kyle_bounds[0][1], kyle_bounds[0][2], kyle_bounds[1][0], kyle_bounds[1][1], kyle_bounds[1][2]);
			ri.Printf(PRINT_WARNING, "EXPERIMENTAL_AUTO_BONE_WEIGHTING: Kyle size is (%f %f %f).\n", kyle_size[0], kyle_size[1], kyle_size[2]);
		}
		else if (!isKyle && StringContainsWord(mod->name, "models/players/"))
		{
			ClearBounds(model_bounds[0], model_bounds[1]);

			surf = (mdxmSurface_t *)((byte *)lod + sizeof(mdxmLOD_t) + (mdxm->numSurfaces * sizeof(mdxmLODSurfOffset_t)));

			// See if this model has any weights/bonerefs...
			for (int n = 0; n < mdxm->numSurfaces; n++)
			{
				// Positions and normals
				mdxmVertex_t *vert = (mdxmVertex_t *)((byte *)surf + surf->ofsVerts);

				// Weights
				for (int k = 0; k < surf->numVerts; k++)
				{
					AddPointToBounds(vert->vertCoords, model_bounds[0], model_bounds[1]);

					if (G2_GetVertBoneIndex(vert, 0))
					{
						hasBones = qtrue;
					}

					vert++;
				}

				surf = (mdxmSurface_t *)((byte *)surf + surf->ofsEnd);
			}

			model_size[0] = model_bounds[1][0] - model_bounds[0][0];
			model_size[1] = model_bounds[1][1] - model_bounds[0][1];
			model_size[2] = model_bounds[1][2] - model_bounds[0][2];

			ri.Printf(PRINT_WARNING, "EXPERIMENTAL_AUTO_BONE_WEIGHTING: Model size for %s (anim: %s) is (%f %f %f). HasBones: %s.\n", mdxm->name, mdxm->name, model_size[0], model_size[1], model_size[2], hasBones ? "true" : "false");
		}

		surf = (mdxmSurface_t *)((byte *)lod + sizeof(mdxmLOD_t) + (mdxm->numSurfaces * sizeof(mdxmLODSurfOffset_t)));
#endif //__EXPERIMENTAL_AUTO_BONE_WEIGHTING__
		for ( int n = 0; n < mdxm->numSurfaces; n++ )
		{
			// Positions and normals
			mdxmVertex_t *v = (mdxmVertex_t *)((byte *)surf + surf->ofsVerts);

			for ( int k = 0; k < surf->numVerts; k++ )
			{
#ifdef __EXPERIMENTAL_AUTO_BONE_WEIGHTING__
				if (!isKyle && !hasBones && StringContainsWord(mod->name, "models/players/"))
				{
					v[k].vertCoords[0] *= kyle_size[0] / model_size[0];
					v[k].vertCoords[1] *= kyle_size[1] / model_size[1];
					v[k].vertCoords[2] *= kyle_size[2] / model_size[2];
				}
#endif //__EXPERIMENTAL_AUTO_BONE_WEIGHTING__

				VectorCopy (v[k].vertCoords, *verts);
				*normals = R_VboPackNormal (v[k].normal);

				verts = (vec3_t *)((byte *)verts + stride);
				normals = (uint32_t *)((byte *)normals + stride);
			}

			v = (mdxmVertex_t *)((byte *)surf + surf->ofsVerts);

			// Weights
			for ( int k = 0; k < surf->numVerts; k++ )
			{
				int numWeights = G2_GetVertWeights(&v[k]);
				float lastWeight = 1.0f;
				int lastInfluence = numWeights - 1;
				for (int w = 0; w < lastInfluence; w++)
				{
					float weight = G2_GetVertBoneWeightNotSlow(&v[k], w);
					(*weights)[w] = weight;
					(*bonerefs)[w] = (float)G2_GetVertBoneIndex(&v[k], w);

					lastWeight -= weight;
				}

				// Ensure that all the weights add up to 1.0
				(*weights)[lastInfluence] = lastWeight;
				(*bonerefs)[lastInfluence] = (float)G2_GetVertBoneIndex(&v[k], lastInfluence);

				// Fill in the rest of the info with zeroes.
				for (int w = numWeights; w < 4; w++)
				{
					(*weights)[w] = 0.0f;
					(*bonerefs)[w] = 0.0f;
				}

#ifdef __EXPERIMENTAL_AUTO_BONE_WEIGHTING__
				if (isKyle)
				{
					VectorCopy(v[k].vertCoords, kyle_verts[kyle_numVerts]);

					for (int w = 0; w < 4; w++)
					{
						kyle_bonerefs[kyle_numVerts][w] = (*weights)[w];
						kyle_weights[kyle_numVerts][w] = (*bonerefs)[w];
					}

					kyle_numVerts++;
				}
#endif //__EXPERIMENTAL_AUTO_BONE_WEIGHTING__

				weights = (vec4_t *)((byte *)weights + stride);
				bonerefs = (vec4_t *)((byte *)bonerefs + stride);
			}

			/*if (isKyle)
			{
				ForceCrash();
			}*/

			// Texture coordinates
			mdxmVertexTexCoord_t *tc = (mdxmVertexTexCoord_t *)(v + surf->numVerts);
			for ( int k = 0; k < surf->numVerts; k++ )
			{
				(*texcoords)[0] = tc[k].texCoords[0];
				(*texcoords)[1] = tc[k].texCoords[1];

				texcoords = (vec2_t *)((byte *)texcoords + stride);
			}

			mdxmTriangle_t *t = (mdxmTriangle_t *)((byte *)surf + surf->ofsTriangles);
			for ( int k = 0; k < surf->numTriangles; k++ )
			{
				int index[3];
				vec3_t sdir, tdir;
				float *v0, *v1, *v2;
				float *uv0, *uv1, *uv2;
				vec3_t normal = { 0.0f, 0.0f, 0.0f };

				index[0] = t[k].indexes[0];
				index[1] = t[k].indexes[1];
				index[2] = t[k].indexes[2];

				v0 = v[index[0]].vertCoords;
				v1 = v[index[1]].vertCoords;
				v2 = v[index[2]].vertCoords;

				uv0 = tc[index[0]].texCoords;
				uv1 = tc[index[1]].texCoords;
				uv2 = tc[index[2]].texCoords;

				VectorAdd (normal, v[index[0]].normal, normal);
				VectorAdd (normal, v[index[1]].normal, normal);
				VectorAdd (normal, v[index[2]].normal, normal);
				VectorNormalize (normal);

				R_CalcTexDirs (sdir, tdir, v0, v1, v2, uv0, uv1, uv2);

				/*
				for ( int i = 0; i < 3; i++ )
				{
					VectorAdd (tangentsf[baseVertexes[n] + index[i]],
						sdir,
						tangentsf[baseVertexes[n] + index[i]]);

					VectorAdd (bitangentsf[baseVertexes[n] + index[i]],
						tdir,
						bitangentsf[baseVertexes[n] + index[i]]);
				}
				*/
			}

#ifdef __MESH_OPTIMIZATION__
			uint32_t numTris = surf->numTriangles * 3;
			R_OptimizeMesh((uint32_t *)&surf->numVerts, (uint32_t *)&numTris, (uint32_t *)((byte *)surf + surf->ofsTriangles), NULL);
			surf->numTriangles = numTris / 3;
#endif //__MESH_OPTIMIZATION__

			// Finally add it to the vertex buffer data
			/*
			for ( int k = 0; k < surf->numVerts; k++ )
			{
				vec3_t sdir, tdir;

				vec3_t& tangent = tangentsf[baseVertexes[n] + k];
				vec3_t& bitangent = bitangentsf[baseVertexes[n] + k];
				vec3_t NxT;
				vec4_t T;

				VectorCopy (tangent, sdir);
				VectorCopy (bitangent, tdir);

				VectorNormalize (sdir);
				VectorNormalize (tdir);

				R_CalcTbnFromNormalAndTexDirs(tangent, bitangent, v[k].normal, sdir, tdir);

				CrossProduct (v[k].normal, tangent, NxT);
				VectorCopy (tangent, T);
				T[3] = DotProduct (NxT, bitangent) < 0.0f ? -1.0f : 1.0f;
			}
			*/

			surf = (mdxmSurface_t *)((byte *)surf + surf->ofsEnd);
		}

#ifdef __EXPERIMENTAL_AUTO_BONE_WEIGHTING__
		if (isKyle /*&& !kyle_numVerts*/)
		{
			ri.Printf(PRINT_WARNING, "kyle_numVerts %i.\n", kyle_numVerts);
		}
		else if (!isKyle && !hasBones && StringContainsWord(mod->name, "models/players/"))
		{
			bonerefs = bonerefsBegin;
			weights = weightsBegin;

			surf = (mdxmSurface_t *)((byte *)lod + sizeof(mdxmLOD_t) + (mdxm->numSurfaces * sizeof(mdxmLODSurfOffset_t)));

			// Scale the model to kyle size...
			for (int s = 0; s < mdxm->numSurfaces; s++)
			{
				// Positions and normals
				mdxmVertex_t *vert = (mdxmVertex_t *)((byte *)surf + surf->ofsVerts);

				// Weights
				for (int vt = 0; vt < surf->numVerts; vt++)
				{
					int bestKV = -1;
					float bestKVdist = 99999.9;

					// Now we have a scale similar to kyle, find the closest vert on him to copy weightings from...
					for (int kv = 0; kv < kyle_numVerts; kv++)
					{
						float dist = Distance(kyle_verts[kv], vert->vertCoords);

						if (dist < bestKVdist)
						{
							bestKVdist = dist;
							bestKV = kv;
						}
					}

					for (int w = 0; w < 4; w++)
					{
						(*weights)[w] = kyle_bonerefs[bestKV][w];
						(*bonerefs)[w] = kyle_weights[bestKV][w];
					}

					// New we have the closest vert to the one we want to weight, copy the weightings...
					ri.Printf(PRINT_WARNING, "Closest vert to %f %f %f was at %f %f %f. Bonerefs: %f %f %f %f. Weights %f %f %f %f.\n"
						, vert->vertCoords[0], vert->vertCoords[1], vert->vertCoords[2]
						, kyle_verts[bestKV][0], kyle_verts[bestKV][1], kyle_verts[bestKV][2]
						, bonerefs[0], bonerefs[1], bonerefs[2], bonerefs[3]
						, weights[0], weights[1], weights[2], weights[3]);

					weights = (vec4_t *)((byte *)weights + stride);
					bonerefs = (vec4_t *)((byte *)bonerefs + stride);


					vert++;
				}

				surf = (mdxmSurface_t *)((byte *)surf + surf->ofsEnd);
			}
		}
#endif //__EXPERIMENTAL_AUTO_BONE_WEIGHTING__

		assert ((byte *)verts == (data + dataSize));

		const char *modelName = strrchr (mdxm->name, '/');
		if (modelName == NULL)
		{
			modelName = mdxm->name;
		}

		VBO_t *vbo = R_CreateVBO (data, dataSize, VBO_USAGE_DYNAMIC);

		Hunk_FreeTempMemory(data);
		Hunk_FreeTempMemory(tangentsf);
		Hunk_FreeTempMemory(bitangentsf);

		vbo->ofs_xyz = ofsPosition;
		vbo->ofs_normal = ofsNormals;
		vbo->ofs_st = ofsTexcoords;
		vbo->ofs_boneindexes = ofsBoneRefs;
		vbo->ofs_boneweights = ofsWeights;

		vbo->stride_xyz = stride;
		vbo->stride_normal = stride;
		vbo->stride_st = stride;
		vbo->stride_boneindexes = stride;
		vbo->stride_boneweights = stride;

		// Fill in the index buffer
		glIndex_t *indices = (glIndex_t *)Hunk_AllocateTempMemory(sizeof (glIndex_t) * numTriangles * 3);
		glIndex_t *index = indices;

		surf = (mdxmSurface_t *)((byte *)lod + sizeof (mdxmLOD_t) + (mdxm->numSurfaces * sizeof (mdxmLODSurfOffset_t)));

		for ( int n = 0; n < mdxm->numSurfaces; n++ )
		{
			mdxmTriangle_t *t = (mdxmTriangle_t *)((byte *)surf + surf->ofsTriangles);

			for ( int k = 0; k < surf->numTriangles; k++, index += 3 )
			{
				index[0] = t[k].indexes[0] + baseVertexes[n];
				assert (index[0] >= 0 && index[0] < numVerts);

				index[1] = t[k].indexes[1] + baseVertexes[n];
				assert (index[1] >= 0 && index[1] < numVerts);

				index[2] = t[k].indexes[2] + baseVertexes[n];
				assert (index[2] >= 0 && index[2] < numVerts);
			}

			surf = (mdxmSurface_t *)((byte *)surf + surf->ofsEnd);
		}

		assert (index == (indices + numTriangles * 3));

		IBO_t *ibo = R_CreateIBO ((byte *)indices, sizeof (glIndex_t) * numTriangles * 3, VBO_USAGE_STATIC);

		Hunk_FreeTempMemory(indices);

		surf = (mdxmSurface_t *)((byte *)lod + sizeof (mdxmLOD_t) + (mdxm->numSurfaces * sizeof (mdxmLODSurfOffset_t)));

		for ( int n = 0; n < mdxm->numSurfaces; n++ )
		{
			vboMeshes[n].vbo = vbo;
			vboMeshes[n].ibo = ibo;

			vboMeshes[n].indexOffset = indexOffsets[n];
			vboMeshes[n].minIndex = baseVertexes[n];
			vboMeshes[n].maxIndex = baseVertexes[n + 1] - 1;
			vboMeshes[n].numVertexes = surf->numVerts;
			vboMeshes[n].numIndexes = surf->numTriangles * 3;

			surf = (mdxmSurface_t *)((byte *)surf + surf->ofsEnd);
		}

		vboModel->vbo = vbo;
		vboModel->ibo = ibo;

		Hunk_FreeTempMemory(indexOffsets);
		Hunk_FreeTempMemory(baseVertexes);

		lod = (mdxmLOD_t *)((byte *)lod + lod->ofsEnd);
	}

	return qtrue;
}

#ifdef __EXPERIMENTAL_ASSIMP_GLM_CONVERSIONS__
// assimp include files. These three are usually needed.
#include "assimp/Importer.hpp"	//OO version Header!
#include "assimp/postprocess.h"
#include "assimp/scene.h"
#include "assimp/DefaultLogger.hpp"
#include "assimp/LogStream.hpp"

// Create an instance of the Importer class
extern Assimp::Importer assImpImporter;

extern std::string AssImp_getTextureName(const std::string& path);

qboolean R_LoadMDXM_Assimp(model_t *mod, void *buffer, const char *mod_name, qboolean &bAlreadyCached, const aiScene* scene, int size) {
	mdxmHeader_t		*mdxm;

	mod->type = MOD_MDXM;
	mod->dataSize += size;

	int hOffset = size + (sizeof(mdxmSurfHierarchy_t) * scene->mNumMeshes);
	void *buffer2 = Hunk_Alloc(hOffset, h_low);
	mdxmSurfHierarchy_t *hierarchys = (mdxmSurfHierarchy_t *)buffer2 + size;
	size = size + hOffset;
	
	qboolean bAlreadyFound = qfalse;
	mdxm = (mdxmHeader_t*)CModelCache->Allocate(size, buffer2, mod_name, &bAlreadyFound, TAG_MODEL_GLM);
	mod->data.glm = (mdxmData_t *)Hunk_Alloc(sizeof(mdxmData_t), h_low);
	mod->data.glm->header = mdxm;

	bAlreadyCached = bAlreadyFound;

	assert(bAlreadyCached == bAlreadyFound);

	if (!bAlreadyFound)
	{
		// horrible new hackery, if !bAlreadyFound then we've just done a tag-morph, so we need to set the 
		//	bool reference passed into this function to true, to tell the caller NOT to do an ri.FS_Freefile since
		//	we've hijacked that memory block...
		//
		// Aaaargh. Kill me now...
		//
		bAlreadyCached = qtrue;
		assert(mdxm == buffer);
		//		memcpy( mdxm, buffer, size );	// and don't do this now, since it's the same thing

		LL(mdxm->ident);
		LL(mdxm->version);
		LL(mdxm->numLODs);
		LL(mdxm->ofsLODs);
		LL(mdxm->numSurfaces);
		LL(mdxm->ofsSurfHierarchy);
		LL(mdxm->ofsEnd);
	}

	mod->numLods = mdxm->numLODs - 1;	//copy this up to the model for ease of use - it wil get inced after this.

	if (bAlreadyFound)
	{
		return qtrue;	// All done. Stop, go no further, do not LittleLong(), do not pass Go...
	}

	mdxm->ident = MDXM_IDENT;
	mdxm->version = MDXM_VERSION;
	mdxm->numLODs = 1;
	mdxm->numSurfaces = scene->mNumMeshes;
	mdxm->ofsSurfHierarchy = hOffset;// size;
	mdxm->ofsLODs = 0;
	mdxm->ofsEnd = size;
	mdxm->numBones = 0;

	strcpy(mdxm->name, mod_name);

	mod->dataSize = mdxm->ofsEnd;

	// Count total bones...
	int		numBones = 0;
	char	boneNames[128][64] = { 0 };

	for (int j = 0; j < scene->mNumMeshes; j++)
	{
		aiMesh					*aiSurf = scene->mMeshes[j];

		if (aiSurf->HasBones())
		{
			for (int k = 0; k < aiSurf->mNumBones; k++)
			{
				aiBone *bone = aiSurf->mBones[k];

				if (bone)
				{
					char bName[64] = { 0 };
					strcpy(bName, bone->mName.C_Str());
					strcpy(bName, Q_strlwr(bName));

					qboolean alreadyAddedBone = qfalse;

					for (int z = 0; z < numBones; z++)
					{
						if (!strcmp(bName, boneNames[z]))
						{
							alreadyAddedBone = qtrue;
							break;
						}
					}

					if (!alreadyAddedBone)
					{
						strcpy(boneNames[numBones], bName);
						numBones++;
					}
				}
			}
		}
	}

	mdxm->numBones = numBones;

	ri.Printf(PRINT_WARNING, "Model %s has %i bones.\n", mod_name, numBones);
	for (int z = 0; z < numBones; z++)
	{
		ri.Printf(PRINT_WARNING, "   %i - %s.\n", z, boneNames[z]);
	}

	// first up, go load in the animation file we need that has the skeletal animation info for this model
	strcpy(mdxm->animName, "models/players/_humanoid/_humanoid"); // Always use _humanoid.gla
	mdxm->animIndex = RE_RegisterModel(va("%s.gla", mdxm->animName));

	/*bool isAnOldModelFile = false;
	if (mdxm->numBones == 72 && strstr(mdxm->animName, "_humanoid"))
	{
		isAnOldModelFile = true;
	}*/

	// Make a copy on the GPU
	mod->data.glm->vboModels = (mdxmVBOModel_t *)Hunk_Alloc(sizeof(mdxmVBOModel_t) * mdxm->numLODs, h_low);
	
	{
		mdxmVBOModel_t *vboModel = &mod->data.glm->vboModels[0];
		mdxmVBOMesh_t *vboMeshes;

		vec3_t *verts;
		uint32_t *normals;
		vec2_t *texcoords;
		vec4_t *bonerefs;
		vec4_t *weights;

		byte *data;
		int dataSize = 0;
		int ofsPosition, ofsNormals, ofsTexcoords, ofsBoneRefs, ofsWeights;
		int stride = 0;
		int numVerts = 0;
		int numTriangles = 0;

		vec3_t *tangentsf;
		vec3_t *bitangentsf;

		// +1 to add total vertex count
		int *baseVertexes = (int *)Hunk_AllocateTempMemory(sizeof(int) * (mdxm->numSurfaces + 1));
		int *indexOffsets = (int *)Hunk_AllocateTempMemory(sizeof(int) * mdxm->numSurfaces);
		memset(baseVertexes, 0, sizeof(int) * (mdxm->numSurfaces + 1));
		memset(indexOffsets, 0, sizeof(int) * mdxm->numSurfaces);

		vboModel->numVBOMeshes = mdxm->numSurfaces;
		vboModel->vboMeshes = (mdxmVBOMesh_t *)Hunk_Alloc(sizeof(mdxmVBOMesh_t) * mdxm->numSurfaces, h_low);
		vboMeshes = vboModel->vboMeshes;

		mdxmSurface_t *surfs = (mdxmSurface_t *)Hunk_Alloc(sizeof(mdxmSurface_t) * mdxm->numSurfaces, h_low);
		memset(surfs, 0, sizeof(mdxmSurfHierarchy_t) * mdxm->numSurfaces);

		for (int n = 0; n < mdxm->numSurfaces; n++)
		{
			mdxmSurfHierarchy_t		*hierarchy = hierarchys + n;
			aiMesh					*aiSurf = scene->mMeshes[n];
			mdxmSurface_t			*surf = &surfs[n];
			shader_t				*sh = NULL;
			aiString				shaderPath;	// filename
			

			// change to surface identifier
			surf->ident = SF_MDX;
			surf->thisSurfaceIndex = n;

			scene->mMaterials[aiSurf->mMaterialIndex]->GetTexture(aiTextureType_DIFFUSE, 0, &shaderPath);

			std::string textureName = AssImp_getTextureName(shaderPath.C_Str());

			//std::string finalPath = R_FindAndAdjustShaderNames(mod_name, shaderPath.C_Str(), textureName);
			//sh = R_FindShader(finalPath.c_str(), lightmapsNone, stylesDefault, qtrue);

			std::string finalPath = textureName;
			sh = R_FindShader(finalPath.c_str(), lightmapsNone, stylesDefault, qtrue);

			if (sh == NULL || sh == tr.defaultShader)
			{
#ifdef __DEBUG_ASSIMP__
				ri.Printf(PRINT_ALL, "Model: %s. Mesh: %s (%i). Missing texture: %s.\n", mod_name, aiSurf->mName.length ? aiSurf->mName.C_Str() : "NULL", n, finalPath.length() ? finalPath.c_str() : "NULL");
#endif //__DEBUG_ASSIMP__
				sh = tr.defaultShader;
			}
			
			strcpy(hierarchy->shader, finalPath.c_str());

			// register the shaders
			if (sh->defaultShader)
			{
				hierarchy->shaderIndex = 0;
			}
			else
			{
				hierarchy->shaderIndex = sh->index;
			}

			// swap all the triangles
			surf->numTriangles = aiSurf->mNumFaces;
			surf->numVerts = aiSurf->mNumVertices;

			baseVertexes[n] = numVerts;
			indexOffsets[n] = numTriangles * 3;

			numVerts += surf->numVerts;
			numTriangles += surf->numTriangles;
		}

		baseVertexes[mdxm->numSurfaces] = numVerts;

		tangentsf = (vec3_t *)Hunk_AllocateTempMemory(sizeof(vec3_t) * numVerts);
		bitangentsf = (vec3_t *)Hunk_AllocateTempMemory(sizeof(vec3_t) * numVerts);
		memset(tangentsf, 0, sizeof(vec3_t) * numVerts);
		memset(bitangentsf, 0, sizeof(vec3_t) * numVerts);

		dataSize += numVerts * sizeof(*verts);
		dataSize += numVerts * sizeof(*normals);
		dataSize += numVerts * sizeof(*texcoords);
		dataSize += numVerts * sizeof(*weights);
		dataSize += numVerts * sizeof(*bonerefs);

		// Allocate and write to memory
		data = (byte *)Hunk_AllocateTempMemory(dataSize);

		verts = (vec3_t *)(data + stride);
		ofsPosition = stride;
		stride += sizeof(*verts);

		normals = (uint32_t *)(data + stride);
		ofsNormals = stride;
		stride += sizeof(*normals);

		texcoords = (vec2_t *)(data + stride);
		ofsTexcoords = stride;
		stride += sizeof(*texcoords);

		bonerefs = (vec4_t *)(data + stride);
		ofsBoneRefs = stride;
		stride += sizeof(*bonerefs);

		weights = (vec4_t *)(data + stride);
		ofsWeights = stride;
		stride += sizeof(*weights);

		//mdxmTriangle_t *triIndexes = (mdxmTriangle_t *)Hunk_AllocateTempMemory(sizeof(mdxmTriangle_t) * numTriangles);

		for (int n = 0; n < mdxm->numSurfaces; n++)
		{
			aiMesh					*aiSurf = scene->mMeshes[n];
			mdxmSurface_t			*surf = &surfs[n];
			mdxmVertex_t			*v = (mdxmVertex_t *)Hunk_AllocateTempMemory(sizeof(mdxmVertex_t) * surf->numVerts);
			mdxmVertexTexCoord_t	*tc = (mdxmVertexTexCoord_t *)Hunk_AllocateTempMemory(sizeof(mdxmVertexTexCoord_t) * surf->numVerts);

			//
			// Positions and normals
			//
			for (int k = 0; k < surf->numVerts; k++)
			{
				mdxmVertex_t		*vert = &v[k];

				VectorSet(vert->vertCoords, aiSurf->mVertices[k].x, aiSurf->mVertices[k].y, aiSurf->mVertices[k].z);
				VectorSet(*verts, aiSurf->mVertices[k].x, aiSurf->mVertices[k].y, aiSurf->mVertices[k].z);
				
				VectorSet(v->normal, aiSurf->mNormals[k].x, aiSurf->mNormals[k].y, aiSurf->mNormals[k].z);
				*normals = R_VboPackNormal(v->normal);

				verts = (vec3_t *)((byte *)verts + stride);
				normals = (uint32_t *)((byte *)normals + stride);

				//
				// Weights
				//
				int					numWeights = 0;

				if (aiSurf->HasBones())
				{
					for (int b = 0; b < aiSurf->mNumBones; b++)
					{
						aiBone *bone = aiSurf->mBones[b];

						for (int l = 0; l < bone->mNumWeights && l < 4; l++)
						{
							aiVertexWeight *w = &bone->mWeights[l];
							vert->BoneWeightings[l] = w->mWeight;
							numWeights++;
						}
					}

					//v.uiNmWeightsAndBoneIndexes = 0; // ????
					vert->uiNmWeightsAndBoneIndexes |= ((numWeights - 1) & 0x3) << 30;
				}

				//int numWeights = G2_GetVertWeights(vert);
				float lastWeight = 1.0f;
				int lastInfluence = numWeights - 1;
				for (int w = 0; w < lastInfluence; w++)
				{
					float weight = G2_GetVertBoneWeightNotSlow(vert, w);
					(*weights)[w] = weight;
					(*bonerefs)[w] = (float)G2_GetVertBoneIndex(vert, w);

					lastWeight -= weight;
				}

				// Ensure that all the weights add up to 1.0
				(*weights)[lastInfluence] = lastWeight;
				(*bonerefs)[lastInfluence] = (float)G2_GetVertBoneIndex(vert, lastInfluence);

				// Fill in the rest of the info with zeroes.
				for (int w = numWeights; w < 4; w++)
				{
					(*weights)[w] = 0.0f;
					(*bonerefs)[w] = 0.0f;
				}

				weights = (vec4_t *)((byte *)weights + stride);
				bonerefs = (vec4_t *)((byte *)bonerefs + stride);

				//
				// Texture coordinates
				//
				if (aiSurf->mNormals != NULL && aiSurf->HasTextureCoords(0))		//HasTextureCoords(texture_coordinates_set)
				{
					tc[k].texCoords[0] = LittleFloat(aiSurf->mTextureCoords[0][k].x);
					tc[k].texCoords[1] = LittleFloat(1 - aiSurf->mTextureCoords[0][k].y);
				}
				else
				{
					tc[k].texCoords[0] = 0.0;
					tc[k].texCoords[1] = 1.0;
				}

				(*texcoords)[0] = tc[k].texCoords[0];
				(*texcoords)[1] = tc[k].texCoords[1];

				texcoords = (vec2_t *)((byte *)texcoords + stride);
			}

#if 0
			//mdxmVertex_t		*vert = &v[0];

			//mdxmTriangle_t *t = (mdxmTriangle_t *)((byte *)surf + surf->ofsTriangles);
			//mdxmTriangle_t *t = (mdxmTriangle_t *)Hunk_AllocateTempMemory(sizeof(mdxmTriangle_t) * surf->numTriangles);
			for (int k = 0; k < surf->numTriangles; k++)
			{
				mdxmTriangle_t *t = &triIndexes[k];
				
				int index[3];
				vec3_t sdir, tdir;
				float *v0, *v1, *v2;
				float *uv0, *uv1, *uv2;
				vec3_t normal = { 0.0f, 0.0f, 0.0f };

				index[0] = t->indexes[0];
				index[1] = t->indexes[1];
				index[2] = t->indexes[2];

				v0 = v[index[0]].vertCoords;
				v1 = v[index[1]].vertCoords;
				v2 = v[index[2]].vertCoords;

				uv0 = tc[index[0]].texCoords;
				uv1 = tc[index[1]].texCoords;
				uv2 = tc[index[2]].texCoords;

				VectorAdd(normal, v[index[0]].normal, normal);
				VectorAdd(normal, v[index[1]].normal, normal);
				VectorAdd(normal, v[index[2]].normal, normal);
				VectorNormalize(normal);

				R_CalcTexDirs(sdir, tdir, v0, v1, v2, uv0, uv1, uv2);

				for (int i = 0; i < 3; i++)
				{
					VectorAdd(tangentsf[baseVertexes[n] + index[i]],
						sdir,
						tangentsf[baseVertexes[n] + index[i]]);

					VectorAdd(bitangentsf[baseVertexes[n] + index[i]],
						tdir,
						bitangentsf[baseVertexes[n] + index[i]]);
				}
			}

			// Finally add it to the vertex buffer data
			for (int k = 0; k < surf->numVerts; k++)
			{
				vec3_t sdir, tdir;

				vec3_t& tangent = tangentsf[baseVertexes[n] + k];
				vec3_t& bitangent = bitangentsf[baseVertexes[n] + k];
				vec3_t NxT;
				vec4_t T;

				VectorCopy(tangent, sdir);
				VectorCopy(bitangent, tdir);

				VectorNormalize(sdir);
				VectorNormalize(tdir);

				R_CalcTbnFromNormalAndTexDirs(tangent, bitangent, v[k].normal, sdir, tdir);

				CrossProduct(v[k].normal, tangent, NxT);
				VectorCopy(tangent, T);
				T[3] = DotProduct(NxT, bitangent) < 0.0f ? -1.0f : 1.0f;
			}
#endif

			Hunk_FreeTempMemory(tc);
			Hunk_FreeTempMemory(v);
			//Hunk_FreeTempMemory(t); // freed below as we still need these...
		}

		assert((byte *)verts == (data + dataSize));

		const char *modelName = strrchr(mdxm->name, '/');
		if (modelName == NULL)
		{
			modelName = mdxm->name;
		}

		VBO_t *vbo = R_CreateVBO(data, dataSize, VBO_USAGE_DYNAMIC);

		Hunk_FreeTempMemory(data);
		Hunk_FreeTempMemory(tangentsf);
		Hunk_FreeTempMemory(bitangentsf);

		vbo->ofs_xyz = ofsPosition;
		vbo->ofs_normal = ofsNormals;
		vbo->ofs_st = ofsTexcoords;
		vbo->ofs_boneindexes = ofsBoneRefs;
		vbo->ofs_boneweights = ofsWeights;

		vbo->stride_xyz = stride;
		vbo->stride_normal = stride;
		vbo->stride_st = stride;
		vbo->stride_boneindexes = stride;
		vbo->stride_boneweights = stride;

		// Fill in the index buffer
		glIndex_t *indices = (glIndex_t *)Hunk_AllocateTempMemory(sizeof(glIndex_t) * numTriangles * 3);
		glIndex_t *index = indices;

		for (int n = 0; n < scene->mNumMeshes; n++)
		{
			aiMesh	 *aiSurf = scene->mMeshes[n];

			for (int k = 0; k < aiSurf->mNumFaces; k++, index += 3)
			{
				index[0] = aiSurf->mFaces[k].mIndices[0] + baseVertexes[n];
				index[1] = aiSurf->mFaces[k].mIndices[1] + baseVertexes[n];
				index[2] = aiSurf->mFaces[k].mIndices[2] + baseVertexes[n];
			}
		}

		assert(index == (indices + numTriangles * 3));

		IBO_t *ibo = R_CreateIBO((byte *)indices, sizeof(glIndex_t) * numTriangles * 3, VBO_USAGE_STATIC);

		Hunk_FreeTempMemory(indices);

		for (int n = 0; n < mdxm->numSurfaces; n++)
		{
			aiMesh *aiSurf = scene->mMeshes[n];

			vboMeshes[n].vbo = vbo;
			vboMeshes[n].ibo = ibo;

			vboMeshes[n].indexOffset = indexOffsets[n];
			vboMeshes[n].minIndex = baseVertexes[n];
			vboMeshes[n].maxIndex = baseVertexes[n + 1] - 1;
			vboMeshes[n].numVertexes = aiSurf->mNumVertices;
			vboMeshes[n].numIndexes = aiSurf->mNumFaces * 3;
		}

		vboModel->vbo = vbo;
		vboModel->ibo = ibo;

		//Hunk_FreeTempMemory(triIndexes);
		Hunk_FreeTempMemory(indexOffsets);
		Hunk_FreeTempMemory(baseVertexes);
	}

	// No longer need the scene data...
	assImpImporter.FreeScene();

	mod->numLods = 1;
	return qtrue;
}
#endif //__EXPERIMENTAL_ASSIMP_GLM_CONVERSIONS__

//#define CREATE_LIMB_HIERARCHY

#ifdef CREATE_LIMB_HIERARCHY

#define NUM_ROOTPARENTS				4
#define NUM_OTHERPARENTS			12
#define NUM_BOTTOMBONES				4

#define CHILD_PADDING				4 //I don't know, I guess this can be changed.

static const char *rootParents[NUM_ROOTPARENTS] =
{
	"rfemurYZ",
	"rhumerus",
	"lfemurYZ",
	"lhumerus"
};

static const char *otherParents[NUM_OTHERPARENTS] =
{
	"rhumerusX",
	"rradius",
	"rradiusX",
	"lhumerusX",
	"lradius",
	"lradiusX",
	"rfemurX",
	"rtibia",
	"rtalus",
	"lfemurX",
	"ltibia",
	"ltalus"
};

static const char *bottomBones[NUM_BOTTOMBONES] =
{
	"rtarsal",
	"rhand",
	"ltarsal",
	"lhand"
};

qboolean BoneIsRootParent(char *name)
{
	int i = 0;

	while (i < NUM_ROOTPARENTS)
	{
		if (!Q_stricmp(name, rootParents[i]))
		{
			return qtrue;
		}

		i++;
	}

	return qfalse;
}

qboolean BoneIsOtherParent(char *name)
{
	int i = 0;

	while (i < NUM_OTHERPARENTS)
	{
		if (!Q_stricmp(name, otherParents[i]))
		{
			return qtrue;
		}

		i++;
	}

	return qfalse;
}

qboolean BoneIsBottom(char *name)
{
	int i = 0;

	while (i < NUM_BOTTOMBONES)
	{
		if (!Q_stricmp(name, bottomBones[i]))
		{
			return qtrue;
		}

		i++;
	}

	return qfalse;
}

void ShiftMemoryDown(mdxaSkelOffsets_t *offsets, mdxaHeader_t *mdxa, int boneIndex, byte **endMarker)
{
	int i = 0;

	//where the next bone starts
	byte *nextBone = ((byte *)mdxa + sizeof(mdxaHeader_t) + offsets->offsets[boneIndex+1]);
	int size = (*endMarker - nextBone);

	memmove((nextBone+CHILD_PADDING), nextBone, size);
	memset(nextBone, 0, CHILD_PADDING);
	*endMarker += CHILD_PADDING;
	//Move the whole thing down CHILD_PADDING amount in memory, clear the new preceding space, and increment the end pointer.

	i = boneIndex+1;

	//Now add CHILD_PADDING amount to every offset beginning at the offset of the bone that was moved.
	while (i < mdxa->numBones)
	{
		offsets->offsets[i] += CHILD_PADDING;
		i++;
	}

	mdxa->ofsFrames += CHILD_PADDING;
	mdxa->ofsCompBonePool += CHILD_PADDING;
	mdxa->ofsEnd += CHILD_PADDING;
	//ofsSkel does not need to be updated because we are only moving memory after that point.
}

//Proper/desired hierarchy list
static const char *BoneHierarchyList[] =
{
	"lfemurYZ",
	"lfemurX",
	"ltibia",
	"ltalus",
	"ltarsal",

	"rfemurYZ",
	"rfemurX",
	"rtibia",
	"rtalus",
	"rtarsal",

	"lhumerus",
	"lhumerusX",
	"lradius",
	"lradiusX",
	"lhand",

	"rhumerus",
	"rhumerusX",
	"rradius",
	"rradiusX",
	"rhand",

	0
};

//Gets the index of a child or parent. If child is passed as qfalse then parent is assumed.
int BoneParentChildIndex(mdxaHeader_t *mdxa, mdxaSkelOffsets_t *offsets, mdxaSkel_t *boneInfo, qboolean child)
{
	int i = 0;
	int matchindex = -1;
	mdxaSkel_t *bone;
	const char *match = NULL;

	while (BoneHierarchyList[i])
	{
		if (!Q_stricmp(boneInfo->name, BoneHierarchyList[i]))
		{ //we have a match, the slot above this will be our desired parent. (or below for child)
			if (child)
			{
				match = BoneHierarchyList[i+1];
			}
			else
			{
				match = BoneHierarchyList[i-1];
			}
			break;
		}
		i++;
	}

	if (!match)
	{ //no good
		return -1;
	}

	i = 0;

	while (i < mdxa->numBones)
	{
		bone = (mdxaSkel_t *)((byte *)mdxa + sizeof(mdxaHeader_t) + offsets->offsets[i]);

		if (bone && !Q_stricmp(bone->name, match))
		{ //this is the one
			matchindex = i;
			break;
		}

		i++;
	}

	return matchindex;
}
#endif //CREATE_LIMB_HIERARCHY

/*
=================
R_LoadMDXA - load a Ghoul 2 animation file
=================
*/
qboolean R_LoadMDXA( model_t *mod, void *buffer, const char *mod_name, qboolean &bAlreadyCached ) {

	mdxaHeader_t		*pinmodel, *mdxa;
	int					version;
	int					size;
#ifdef CREATE_LIMB_HIERARCHY
	int					oSize = 0;
	byte				*sizeMarker;
#endif

#if 0 //#ifndef _M_IX86
	int					j, k, i;
	int					frameSize;
	mdxaFrame_t			*cframe;
	mdxaSkel_t			*boneInfo;
#endif

 	pinmodel = (mdxaHeader_t *)buffer;
	//
	// read some fields from the binary, but only LittleLong() them when we know this wasn't an already-cached model...
	//	
	version = (pinmodel->version);
	size	= (pinmodel->ofsEnd);

	if (!bAlreadyCached)
	{
		LL(version);
		LL(size);
	}
	
	if (version != MDXA_VERSION) {
		Com_Printf (S_COLOR_YELLOW  "R_LoadMDXA: %s has wrong version (%i should be %i)\n",
				 mod_name, version, MDXA_VERSION);
		return qfalse;
	}

	mod->type		= MOD_MDXA;
	mod->dataSize  += size;

	qboolean bAlreadyFound = qfalse;

#ifdef CREATE_LIMB_HIERARCHY
	oSize = size;

	int childNumber = (NUM_ROOTPARENTS + NUM_OTHERPARENTS);
	size += (childNumber*(CHILD_PADDING*8)); //Allocate us some extra space so we can shift memory down.
#endif //CREATE_LIMB_HIERARCHY

	//mdxa = mod->mdxa = (mdxaHeader_t*)Hunk_Alloc( size, h_dontcare );
	mdxa = (mdxaHeader_t*)CModelCache->Allocate( size, buffer, mod_name, &bAlreadyFound, TAG_MODEL_GLA );
	mod->data.gla = mdxa;

	assert(bAlreadyCached == bAlreadyFound);	// I should probably eliminate 'bAlreadyFound', but wtf?

	if (!bAlreadyFound)
	{
#ifdef CREATE_LIMB_HIERARCHY
		memcpy( mdxa, buffer, oSize );
#else
		// horrible new hackery, if !bAlreadyFound then we've just done a tag-morph, so we need to set the 
		//	bool reference passed into this function to true, to tell the caller NOT to do an ri.FS_Freefile since
		//	we've hijacked that memory block...
		//
		// Aaaargh. Kill me now...
		//
		bAlreadyCached = qtrue;
		assert( mdxa == buffer );
//		memcpy( mdxa, buffer, size );	// and don't do this now, since it's the same thing
#endif
		LL(mdxa->ident);
		LL(mdxa->version);
		LL(mdxa->numFrames);
		LL(mdxa->numBones);
		LL(mdxa->ofsFrames);
		LL(mdxa->ofsEnd);
	}

#ifdef CREATE_LIMB_HIERARCHY
	if (!bAlreadyFound)
	{
		mdxaSkel_t			*boneParent;
#if 0 //#ifdef _M_IX86
		mdxaSkel_t			*boneInfo;
		int i, k;
#endif

		sizeMarker = (byte *)mdxa + mdxa->ofsEnd;

		//rww - This is probably temporary until we put actual hierarchy in for the models.
		//It is necessary for the correct operation of ragdoll.
   		mdxaSkelOffsets_t *offsets = (mdxaSkelOffsets_t *)((byte *)mdxa + sizeof(mdxaHeader_t));

		for ( i = 0 ; i < mdxa->numBones ; i++) 
		{
			boneInfo = (mdxaSkel_t *)((byte *)mdxa + sizeof(mdxaHeader_t) + offsets->offsets[i]);

			if (boneInfo)
			{
				char *bname = boneInfo->name;

				if (BoneIsRootParent(bname))
				{ //These are the main parent bones. We don't want to change their parents, but we want to give them children.
					ShiftMemoryDown(offsets, mdxa, i, &sizeMarker);

					boneInfo = (mdxaSkel_t *)((byte *)mdxa + sizeof(mdxaHeader_t) + offsets->offsets[i]);

					int newChild = BoneParentChildIndex(mdxa, offsets, boneInfo, qtrue);

					if (newChild != -1)
					{
						boneInfo->numChildren++;
						boneInfo->children[boneInfo->numChildren-1] = newChild;
					}
					else
					{
						assert(!"Failed to find matching child for bone in hierarchy creation");
					}
				}
				else if (BoneIsOtherParent(bname) || BoneIsBottom(bname))
				{
					if (!BoneIsBottom(bname))
					{ //unless it's last in the chain it has the next bone as a child.
						ShiftMemoryDown(offsets, mdxa, i, &sizeMarker);

						boneInfo = (mdxaSkel_t *)((byte *)mdxa + sizeof(mdxaHeader_t) + offsets->offsets[i]);

						int newChild = BoneParentChildIndex(mdxa, offsets, boneInfo, qtrue);

						if (newChild != -1)
						{
							boneInfo->numChildren++;
							boneInfo->children[boneInfo->numChildren-1] = newChild;
						}
						else
						{
							assert(!"Failed to find matching child for bone in hierarchy creation");
						}
					}

					//Before we set the parent we want to remove this as a child for whoever was parenting it.
					int oldParent = boneInfo->parent;

					if (oldParent > -1)
					{
						boneParent = (mdxaSkel_t *)((byte *)mdxa + sizeof(mdxaHeader_t) + offsets->offsets[oldParent]);
					}
					else
					{
						boneParent = NULL;
					}

					if (boneParent)
					{
						k = 0;

						while (k < boneParent->numChildren)
						{
							if (boneParent->children[k] == i)
							{ //this bone is the child
								k++;
								while (k < boneParent->numChildren)
								{
									boneParent->children[k-1] = boneParent->children[k];
									k++;
								}
								boneParent->children[k-1] = 0;
								boneParent->numChildren--;
								break;
							}
							k++;
						}
					}

					//Now that we have cleared the original parent of ownership, mark the bone's new parent.
					int newParent = BoneParentChildIndex(mdxa, offsets, boneInfo, qfalse);

					if (newParent != -1)
					{
						boneInfo->parent = newParent;
					}
					else
					{
						assert(!"Failed to find matching parent for bone in hierarchy creation");
					}
				}
			}
		}
	}
#endif //CREATE_LIMB_HIERARCHY

 	if ( mdxa->numFrames < 1 ) {
		Com_Printf (S_COLOR_YELLOW  "R_LoadMDXA: %s has no frames\n", mod_name );
		return qfalse;
	}

	if (bAlreadyFound)
	{
		return qtrue;	// All done, stop here, do not LittleLong() etc. Do not pass go...
	}

#if 0 //#ifndef _M_IX86

	//
	// optimisation, we don't bother doing this for standard intel case since our data's already in that format...
	//

	// swap all the skeletal info
	boneInfo = (mdxaSkel_t *)( (byte *)mdxa + mdxa->ofsSkel);
	for ( i = 0 ; i < mdxa->numBones ; i++) 
	{
		LL(boneInfo->numChildren);
		LL(boneInfo->parent);
		for (k=0; k<boneInfo->numChildren; k++)
		{
			LL(boneInfo->children[k]);
		}

		// get next bone
		boneInfo += (size_t)( &((mdxaSkel_t *)0)->children[ boneInfo->numChildren ] );
	}


	// swap all the frames
	frameSize = (size_t)( &((mdxaFrame_t *)0)->bones[ mdxa->numBones ] );
	for ( i = 0 ; i < mdxa->numFrames ; i++) 
	{
		cframe = (mdxaFrame_t *) ( (byte *)mdxa + mdxa->ofsFrames + i * frameSize );
   		cframe->radius = LittleFloat( cframe->radius );
		for ( j = 0 ; j < 3 ; j++ ) 
		{
			cframe->bounds[0][j] = LittleFloat( cframe->bounds[0][j] );
			cframe->bounds[1][j] = LittleFloat( cframe->bounds[1][j] );
    		cframe->localOrigin[j] = LittleFloat( cframe->localOrigin[j] );
		}
		for ( j = 0 ; j < mdxa->numBones * sizeof( mdxaBone_t ) / 2 ; j++ ) 
		{
			((short *)cframe->bones)[j] = LittleShort( ((short *)cframe->bones)[j] );
		}
	}
#endif
	return qtrue;
}








// Sojourn: Warzone's tr_main calls the MP signature
void R_AddGhoulSurfaces(trRefEntity_t *ent, model_t *currentModel, int entityNum, int64_t shiftedEntityNum)
{
	R_AddGhoulSurfaces(ent, entityNum);
}
