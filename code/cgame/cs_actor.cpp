/*
JK BUILD: Cinematic Studio - Actor Manager, game side (Phase 2).

- Animations of an actor come from its animation set (the animation.cfg of its skeleton).
- The studio poses or plays an animation through the same Ghoul2 calls the game uses
  (PM_SetAnimFinal): legs on the root bone, torso on lower_lumbar (+ the motion bone), with the
  animation's gla index. While the studio holds an actor, its legs/torso animation timers are held
  so the game's own animation code doesn't take over; releasing hands it back.
- Animation profiles (_humanoid_X): an actor can play another skeleton's animation set only if
  both .gla files have exactly the same bones in the same order (Ghoul2 addresses bones by index)
  and every animation's frames exist in the target .gla. The switch offsets the model's animation
  index to the target .gla (g_csGlaOffset), which the game's animation code adds to each
  animation's own gla index.
*/
#include "cg_headers.h"
#include "cs_studio.h"

#include "../game/g_local.h"
#include "../game/b_local.h"

#include "../rd-common/mdx_format.h"

#include <algorithm>
#include <cstring>
#include <map>

extern int G_ParseAnimFileSet(const char* skeletonName, const char* model_name = nullptr);

int g_csGlaOffset[MAX_GENTITIES];	// per entity: target .gla handle - model's own .gla handle
static int g_csOrigSet[MAX_GENTITIES];	// per entity: its own animation set + 1 (0 = never switched)

int CS_GlaOffset(const gentity_t* ent)
{
	return (ent && ent->s.number >= 0 && ent->s.number < MAX_GENTITIES) ? g_csGlaOffset[ent->s.number] : 0;
}

void CS_ClearEntity(const int entNum)
{
	if (entNum >= 0 && entNum < MAX_GENTITIES) g_csGlaOffset[entNum] = 0, g_csOrigSet[entNum] = 0;
}

void CS_ClearAllEntities()
{
	memset(g_csGlaOffset, 0, sizeof g_csGlaOffset);
	memset(g_csOrigSet, 0, sizeof g_csOrigSet);
}

namespace cs
{
	namespace
	{
		gentity_t* Actor(const int n)
		{
			if (n < 0 || n >= ENTITYNUM_WORLD) return nullptr;
			gentity_t* e = &g_entities[n];
			if (!e->inuse || !e->client || !e->ghoul2.size() || e->playerModel < 0 || e->playerModel >= e->ghoul2.size()) return nullptr;
			return e;
		}

		const animFileSet_t* Set(const gentity_t* e)
		{
			const int af = e->client->clientInfo.animFileIndex;
			return (af >= 0 && af < level.numKnownAnimFileSets) ? &level.knownAnimFileSets[af] : nullptr;
		}

		int AnimTime() { return cg.time ? cg.time : level.time; }

		// .gla bone names and frame count (cached by path)
		struct GlaInfo { bool ok = false; int numFrames = 0; std::vector<std::string> bones; };

		const GlaInfo& ReadGla(const std::string& path)
		{
			static std::map<std::string, GlaInfo> cache;
			auto it = cache.find(path);
			if (it != cache.end()) return it->second;
			GlaInfo info;
			void* buf = nullptr;
			const long len = gi.FS_ReadFile(path.c_str(), &buf);
			if (len > static_cast<long>(sizeof(mdxaHeader_t)) && buf)
			{
				const auto* h = static_cast<const mdxaHeader_t*>(buf);
				const auto* base = static_cast<const byte*>(buf);
				if (h->ident == MDXA_IDENT && h->numBones > 0 && h->numBones < 1024 && h->ofsSkel > 0 && h->ofsSkel < len)
				{
					const auto* offs = reinterpret_cast<const mdxaSkelOffsets_t*>(base + sizeof(mdxaHeader_t));
					info.ok = true;
					info.numFrames = h->numFrames;
					for (int i = 0; i < h->numBones && info.ok; i++)
					{
						const long o = static_cast<long>(sizeof(mdxaHeader_t)) + offs->offsets[i];
						if (o <= 0 || o + static_cast<long>(sizeof(mdxaSkel_t)) > len) { info.ok = false; break; }
						const auto* sk = reinterpret_cast<const mdxaSkel_t*>(base + o);
						info.bones.emplace_back(sk->name, strnlen(sk->name, sizeof sk->name));
					}
				}
			}
			if (buf) gi.FS_FreeFile(buf);
			return cache.emplace(path, info).first->second;
		}

		std::string ModelGla(const gentity_t* e)
		{
			const char* n = gi.G2API_GetAnimFileNameIndex(e->ghoul2[e->playerModel].mModelindex);
			if (!n || !n[0]) return "";
			std::string s = n;
			if (s.size() < 4 || Q_stricmp(s.c_str() + s.size() - 4, ".gla")) s += ".gla";
			return s;
		}

		// "models/players/_humanoid_vader/_humanoid.gla" -> "_humanoid_vader"
		std::string FolderOf(const std::string& glaPath)
		{
			const size_t e = glaPath.find_last_of('/');
			if (e == std::string::npos) return "";
			const size_t b = glaPath.find_last_of('/', e - 1);
			return glaPath.substr(b == std::string::npos ? 0 : b + 1, e - (b == std::string::npos ? 0 : b + 1));
		}

		// the .gla of a profile folder: _humanoid.gla, or the folder's only/first .gla
		std::string ProfileGla(const std::string& profile)
		{
			static std::map<std::string, std::string> cache;
			auto it = cache.find(profile);
			if (it != cache.end()) return it->second;
			const std::string dir = "models/players/" + profile;
			std::string found = dir + "/_humanoid.gla";
			if (gi.FS_ReadFile(found.c_str(), nullptr) <= 0)
			{
				char list[2048];
				if (gi.FS_GetFileList(dir.c_str(), ".gla", list, sizeof list) > 0 && list[0]) found = dir + "/" + list;
			}
			return cache.emplace(profile, found).first->second;
		}
	}

	int ListAnims(const int ent, std::vector<AnimInfo>& out)
	{
		out.clear();
		const gentity_t* e = Actor(ent);
		const animFileSet_t* set = e ? Set(e) : nullptr;
		if (!set) return 0;
		for (int i = 0; i < MAX_ANIMATIONS; i++)
		{
			const animation_t& a = set->animations[i];
			if (a.numFrames <= 0 || !animTable[i].name) continue;
			AnimInfo ai;
			ai.anim = i;
			ai.name = animTable[i].name;
			ai.numFrames = a.numFrames;
			ai.frameLerp = a.frameLerp ? abs(a.frameLerp) : 50;
			ai.loops = a.loopFrames != -1;
			out.push_back(ai);
		}
		return static_cast<int>(out.size());
	}

	int AnimByName(const std::string& name)
	{
		for (int i = 0; i < MAX_ANIMATIONS; i++)
			if (animTable[i].name && !Q_stricmp(animTable[i].name, name.c_str())) return i;
		return -1;
	}

	bool AnimInfoFor(const int ent, const int anim, AnimInfo& out)
	{
		const gentity_t* e = Actor(ent);
		const animFileSet_t* set = e ? Set(e) : nullptr;
		if (!set || anim < 0 || anim >= MAX_ANIMATIONS || set->animations[anim].numFrames <= 0) return false;
		const animation_t& a = set->animations[anim];
		out.anim = anim;
		out.name = animTable[anim].name ? animTable[anim].name : "";
		out.numFrames = a.numFrames;
		out.frameLerp = a.frameLerp ? abs(a.frameLerp) : 50;
		out.loops = a.loopFrames != -1;
		return true;
	}

	bool PoseAnim(const int ent, const int anim, const int parts, float frame, const float speed, const bool loop,
		const int startFrame, int endFrame, const int blendMs)
	{
		gentity_t* e = Actor(ent);
		const animFileSet_t* set = e ? Set(e) : nullptr;
		if (!set || anim < 0 || anim >= MAX_ANIMATIONS) return false;
		const animation_t& a = set->animations[anim];
		if (a.numFrames <= 0) return false;
		if (endFrame < 0 || endFrame >= a.numFrames) endFrame = a.numFrames - 1;
		const int s0 = a.firstFrame + std::max(0, std::min(startFrame, endFrame));
		const int s1 = a.firstFrame + endFrame + 1;	// G2 end frame is exclusive
		// game speed: 50 ms per frame is 1.0 (frameLerp < 0 plays backwards)
		const float base = 50.0f / static_cast<float>(a.frameLerp ? abs(a.frameLerp) : 50);
		const float animSpeed = speed == 0.0f ? 0.0f : base * speed;
		int flags = loop ? BONE_ANIM_OVERRIDE_LOOP : BONE_ANIM_OVERRIDE_FREEZE;
		if (blendMs > 0) flags |= BONE_ANIM_BLEND;
		const float setFrame = frame < 0 ? -1.0f : std::max(static_cast<float>(s0), std::min(static_cast<float>(s1) - 0.001f, a.firstFrame + frame));
		const int t = AnimTime();
		CGhoul2Info* g = &e->ghoul2[e->playerModel];

		gi.G2API_SetAnimIndex(g, a.glaIndex + CS_GlaOffset(e));
		if ((parts & PARTS_TORSO) && e->lowerLumbarBone != -1)
		{
			gi.G2API_SetBoneAnimIndex(g, e->lowerLumbarBone, s0, s1, flags, animSpeed, t, setFrame, blendMs);
			if (e->motionBone != -1) gi.G2API_SetBoneAnimIndex(g, e->motionBone, s0, s1, flags, animSpeed, t, setFrame, blendMs);
			e->client->ps.torsoAnim = anim;
			e->client->ps.torsoAnimTimer = 100000000;	// held by the studio
		}
		if ((parts & PARTS_LEGS) && e->rootBone != -1)
		{
			gi.G2API_SetBoneAnimIndex(g, e->rootBone, s0, s1, flags, animSpeed, t, setFrame, blendMs);
			e->client->ps.legsAnim = anim;
			e->client->ps.legsAnimTimer = 100000000;
		}
		return true;
	}

	void ReleaseAnim(const int ent)
	{
		gentity_t* e = Actor(ent);
		if (!e) return;
		e->client->ps.legsAnimTimer = 0;
		e->client->ps.torsoAnimTimer = 0;
	}

	bool CurrentFrame(const int ent, int& anim, float& relFrame)
	{
		const gentity_t* e = Actor(ent);
		const animFileSet_t* set = e ? Set(e) : nullptr;
		if (!set || e->rootBone == -1) return false;
		anim = e->client->ps.legsAnim;
		if (anim < 0 || anim >= MAX_ANIMATIONS) return false;
		float cur = 0;
		int sf = 0, ef = 0, fl = 0;
		float sp = 0;
		int ml = 0;
		auto* g = const_cast<CGhoul2Info*>(&e->ghoul2[e->playerModel]);
		if (!gi.G2API_GetBoneAnimIndex(g, e->rootBone, AnimTime(), &cur, &sf, &ef, &fl, &sp, &ml)) return false;
		relFrame = cur - static_cast<float>(set->animations[anim].firstFrame);
		return true;
	}

	std::vector<std::string> ListProfiles()
	{
		std::vector<std::string> out;
		static char buf[16384];
		const int n = gi.FS_GetFileList("models/players", "/", buf, sizeof buf);
		const char* p = buf;
		for (int i = 0; i < n && p < buf + sizeof buf; i++)
		{
			const std::string d = p;
			p += d.size() + 1;
			if (d.size() < 9 || Q_stricmpn(d.c_str(), "_humanoid", 9)) continue;
			if (gi.FS_ReadFile(ProfileGla(d).c_str(), nullptr) <= 0) continue;
			if (gi.FS_ReadFile(("models/players/" + d + "/animation.cfg").c_str(), nullptr) <= 0) continue;
			bool dup = false;
			for (auto& o : out) dup = dup || !Q_stricmp(o.c_str(), d.c_str());
			if (!dup) out.push_back(d);
		}
		std::sort(out.begin(), out.end());
		return out;
	}

	std::string DefaultProfile(const int ent)
	{
		const gentity_t* e = Actor(ent);
		return e ? FolderOf(ModelGla(e)) : "";
	}

	std::string CurrentProfile(const int ent)
	{
		const gentity_t* e = Actor(ent);
		const animFileSet_t* set = e ? Set(e) : nullptr;
		return set ? set->filename : "";
	}

	bool CheckProfile(const int ent, const std::string& profile, std::string& why)
	{
		const gentity_t* e = Actor(ent);
		if (!e) { why = "not a character"; return false; }
		const std::string own = ModelGla(e);
		if (own.empty()) { why = "the model doesn't name its skeleton"; return false; }
		if (profile.empty() || !Q_stricmp(FolderOf(own).c_str(), profile.c_str())) return true;
		const GlaInfo& a = ReadGla(own);
		const GlaInfo& b = ReadGla(ProfileGla(profile));
		if (!a.ok) { why = "can't read " + own; return false; }
		if (!b.ok) { why = "can't read " + ProfileGla(profile); return false; }
		if (a.bones.size() != b.bones.size())
		{
			why = "different skeletons (" + std::to_string(a.bones.size()) + " vs " + std::to_string(b.bones.size()) + " bones)";
			return false;
		}
		for (size_t i = 0; i < a.bones.size(); i++)
			if (Q_stricmp(a.bones[i].c_str(), b.bones[i].c_str()))
			{
				why = "bone " + std::to_string(i) + " is " + a.bones[i] + " here but " + b.bones[i] + " in " + profile;
				return false;
			}
		return true;
	}

	bool SetProfile(const int ent, const std::string& profileIn, std::string& why)
	{
		gentity_t* e = Actor(ent);
		if (!e) { why = "not a character"; return false; }
		const std::string own = ModelGla(e);
		const std::string profile = profileIn.empty() ? FolderOf(own) : profileIn;
		if (!CheckProfile(ent, profile, why)) return false;

		const qhandle_t ownH = gi.G2API_PrecacheGhoul2Model(own.c_str());
		const qhandle_t newH = gi.G2API_PrecacheGhoul2Model(ProfileGla(profile).c_str());
		if (!ownH || (!newH && Q_stricmp(profile.c_str(), FolderOf(own).c_str())))
		{
			why = "can't load the skeleton";
			return false;
		}
		const int n = e->s.number;
		if (!g_csOrigSet[n]) g_csOrigSet[n] = e->client->clientInfo.animFileIndex + 1;
		const bool backHome = !Q_stricmp(profile.c_str(), FolderOf(own).c_str());
		const int setIndex = backHome ? g_csOrigSet[n] - 1 : G_ParseAnimFileSet(profile.c_str(), nullptr);
		if (setIndex < 0 || setIndex >= level.numKnownAnimFileSets) { why = "no animation.cfg for " + profile; return false; }

		// every animation must fit in the target skeleton's frames
		const GlaInfo& target = ReadGla(ProfileGla(profile));
		if (target.ok && !backHome)
		{
			const animFileSet_t& set = level.knownAnimFileSets[setIndex];
			for (int i = 0; i < MAX_ANIMATIONS; i++)
			{
				const animation_t& a = set.animations[i];
				if (a.numFrames > 0 && a.glaIndex == 0 && a.firstFrame + a.numFrames > target.numFrames)
				{
					why = std::string(animTable[i].name ? animTable[i].name : "an animation") + " needs frame " +
						std::to_string(a.firstFrame + a.numFrames) + " but " + profile + " has " + std::to_string(target.numFrames);
					return false;
				}
			}
		}

		g_csGlaOffset[e->s.number] = newH ? newH - ownH : 0;
		e->client->clientInfo.animFileIndex = setIndex;
		// restart what it was doing with the new animations
		const int legs = e->client->ps.legsAnim, torso = e->client->ps.torsoAnim;
		const animFileSet_t& set = level.knownAnimFileSets[setIndex];
		if (legs >= 0 && legs < MAX_ANIMATIONS && set.animations[legs].numFrames > 0) PoseAnim(ent, legs, PARTS_LEGS, -1, 1, true, 0, -1, 150);
		if (torso >= 0 && torso < MAX_ANIMATIONS && set.animations[torso].numFrames > 0) PoseAnim(ent, torso, PARTS_TORSO, -1, 1, true, 0, -1, 150);
		ReleaseAnim(ent);
		return true;
	}

	bool GameHost::SetActorProfile(const int ent, const std::string& profile, std::string& why)
	{
		return SetProfile(ent, profile, why);
	}

	bool ViewRay(const float sx, const float sy, float dir[3])
	{
		const float tx = tanf(cg.refdef.fov_x * static_cast<float>(M_PI) / 360.0f);
		const float ty = tanf(cg.refdef.fov_y * static_cast<float>(M_PI) / 360.0f);
		const float u = (1.0f - sx / 320.0f) * tx;	// left
		const float v = (1.0f - sy / 240.0f) * ty;	// up
		for (int i = 0; i < 3; i++) dir[i] = cg.refdef.viewaxis[0][i] + u * cg.refdef.viewaxis[1][i] + v * cg.refdef.viewaxis[2][i];
		VectorNormalize(dir);
		return true;
	}
}
