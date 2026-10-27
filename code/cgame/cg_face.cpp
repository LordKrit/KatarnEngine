/*
===========================================================================
Facial animation for Ghoul2 models with face bones: lipsync, expressions, blinking.

A model opts in with ext_data/faces/<model folder>.face (written by tools/face, see
docs/FACE_SYSTEM.md). The file lists the face bones with their base pose and a set of
named poses, each a small local rotation (axis * angle, radians) and translation per bone:

    face 1
    bone <name> <12 floats: base pose 3x4>
    pose <name>
    <bone> rx ry rz tx ty tz
    end

Mouth shapes use Rhubarb Lip Sync's letters: A (M B P), B (most consonants), C (E), D (A),
E (O), F (OO W), G (F V), H (L), X (rest). Other poses are expressions (angry, smirk, ...)
plus "blink".

Lipsync: when a voice sound starts on an entity (any CHAN_VOICE* channel), the file
"<sound path without extension>.lip" is loaded if it exists (Rhubarb TSV: "seconds<TAB>shape").
Without one, the mouth follows the voice volume instead.

Console / cutscene commands:
    faceexpr <who> <pose> [weight 1] [seconds 0 = hold] [blend ms 250]   ("faceexpr <who> none" clears)
    facelip  <who> <sound path>      play a voice line on someone with lipsync
    facelook <who> <who2|off>        point someone's eyes and head at someone
<who> is "player", an entity number, or a targetname / NPC_targetname / script_targetname.
===========================================================================
*/

#include "cg_headers.h"
#include "cg_media.h"
#include "../game/ghoul2_shared.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <cmath>
#include <cstdlib>

namespace
{
	struct FaceBone
	{
		std::string name;
		float base[3][4];
		float inv[3][4];
	};
	struct FacePoseKey
	{
		int bone;
		float r[3];
		float t[3];
	};
	struct FaceDef
	{
		bool valid = false;
		std::vector<FaceBone> bones;
		std::unordered_map<std::string, std::vector<FacePoseKey>> poses;
	};

	constexpr int NUM_VISEMES = 9;
	const char* const k_visemes[NUM_VISEMES] = { "A", "B", "C", "D", "E", "F", "G", "H", "X" };

	struct LipKey
	{
		int ms;
		int shape;
	};
	struct FaceState
	{
		std::vector<LipKey> lip;
		int lipStart = 0;
		int lipEnd = 0;
		float vis[NUM_VISEMES] = { 0, 0, 0, 0, 0, 0, 0, 0, 1 };
		std::string expr;
		float exprW = 0, exprTarget = 0, exprRate = 4;
		int exprUntil = 0;
		int nextBlink = 0;
		int blinkStart = -1000;
		int lastTime = 0;
		std::string model;
		std::string glm;
		std::vector<int> boneIndex;
		int boneModel = -1;

		// --- face life (idle behaviour)
		bool lifeInit = false;
		bool idleOff = false;
		std::string profileName;			// forced profile ("faceidle <who> <profile>"), else by model
		unsigned rng = 1;
		int blinkLen = 150;
		int secondBlink = 0;				// time of a queued double blink
		float blinkDepth = 1.0f;
		std::string mood, moodPrev;
		float moodW = 0, moodPrevW = 0, moodTarget = 0;
		int moodUntil = 0;
		float phase[4] = { 0, 0, 0, 0 };	// micro-motion noise phases
		float tension = 0;
		float eyeOff[2] = { 0, 0 };			// idle eye offset (pitch, yaw), degrees
		float eyeGoal[2] = { 0, 0 };
		int nextSaccade = 0, glanceUntil = 0;
		// emphasis while talking
		float emphasis = 0;
		int lastViseme = 8;
		int lastVol = 0;
		// expression cues inside the .lip file ("e <sec> <pose> [weight] [blend ms]")
		struct Cue { int ms; std::string pose; float w; int blend; };
		std::vector<Cue> cues;
		size_t nextCue = 0;
	};

	struct FaceProfile
	{
		std::vector<std::pair<std::string, float>> moods;	// pose, relative frequency ("neutral" = none)
		float moodStrength = 0.35f;
		float moodHold[2] = { 4, 10 };
		float blinkGap[2] = { 2.0f, 6.0f };
		float doubleBlink = 0.12f;
		float halfBlink = 0.10f;
		float saccadeGap[2] = { 0.6f, 2.5f };
		float saccadeSize = 3.0f;		// degrees, small idle eye drift
		float glance = 0.12f;			// chance a saccade is a bigger glance
		float glanceSize = 14.0f;
		float micro = 0.12f;			// brow / lid noise amount
		std::string combatPose = "focus";
		float combatMax = 0.6f;
		float combatRise = 0.6f, combatFall = 0.12f;	// per second
		float talkEmphasis = 0.5f;		// brow lift on stressed syllables
	};
	std::unordered_map<std::string, FaceProfile> g_profiles;
	bool g_profilesLoaded = false;

	std::unordered_map<std::string, FaceDef> g_faceDefs;
	std::unordered_map<int, std::string> g_soundNames;
	FaceState g_face[MAX_GENTITIES];

	void InvertBone(const float m[3][4], float out[3][4])
	{
		// general 3x3 inverse (the base pose can carry a uniform scale)
		const float a = m[0][0], b = m[0][1], c = m[0][2];
		const float d = m[1][0], e = m[1][1], f = m[1][2];
		const float g = m[2][0], h = m[2][1], i = m[2][2];
		const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
		float det = a * A + b * B + c * C;
		if (std::fabs(det) < 1e-12f) det = 1e-12f;
		const float id = 1.0f / det;
		float r[3][3] = {
			{ A * id, -(b * i - c * h) * id, (b * f - c * e) * id },
			{ B * id, (a * i - c * g) * id, -(a * f - c * d) * id },
			{ C * id, -(a * h - b * g) * id, (a * e - b * d) * id } };
		for (int y = 0; y < 3; y++)
		{
			for (int x = 0; x < 3; x++) out[y][x] = r[y][x];
			out[y][3] = -(r[y][0] * m[0][3] + r[y][1] * m[1][3] + r[y][2] * m[2][3]);
		}
	}

	void Mul34(const float a[3][4], const float b[3][4], float o[3][4])
	{
		for (int y = 0; y < 3; y++)
		{
			for (int x = 0; x < 4; x++)
			{
				o[y][x] = a[y][0] * b[0][x] + a[y][1] * b[1][x] + a[y][2] * b[2][x] + (x == 3 ? a[y][3] : 0.0f);
			}
		}
	}

	const char* NextToken(const char*& p)
	{
		static char tok[256];
		while (*p == ' ' || *p == '\t' || *p == '\r') p++;
		int n = 0;
		while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && n < 255) tok[n++] = *p++;
		tok[n] = 0;
		return tok;
	}

	void NextLine(const char*& p)
	{
		while (*p && *p != '\n') p++;
		if (*p) p++;
	}

	const FaceDef& LoadDef(const std::string& model)
	{
		auto it = g_faceDefs.find(model);
		if (it != g_faceDefs.end()) return it->second;
		FaceDef& def = g_faceDefs[model];
		char* buf = nullptr;
		const std::string path = "ext_data/faces/" + model + ".face";
		if (gi.FS_ReadFile(path.c_str(), reinterpret_cast<void**>(&buf)) <= 0 || !buf)
		{
			Com_Printf("face: no %s\n", path.c_str());
			return def;
		}
		std::unordered_map<std::string, int> byName;
		std::vector<FacePoseKey>* pose = nullptr;
		const char* p = buf;
		while (*p)
		{
			const char* t = NextToken(p);
			if (!t[0] || t[0] == '#' || (t[0] == '/' && t[1] == '/'))
			{
				NextLine(p);
				continue;
			}
			std::string w = t;
			if (w == "bone")
			{
				FaceBone b;
				b.name = NextToken(p);
				for (int y = 0; y < 3; y++)
					for (int x = 0; x < 4; x++) b.base[y][x] = static_cast<float>(atof(NextToken(p)));
				InvertBone(b.base, b.inv);
				byName[b.name] = static_cast<int>(def.bones.size());
				def.bones.push_back(b);
			}
			else if (w == "pose")
			{
				pose = &def.poses[NextToken(p)];
			}
			else if (w == "end")
			{
				pose = nullptr;
			}
			else if (w != "face" && pose)
			{
				auto bi = byName.find(w);
				FacePoseKey k{};
				k.bone = bi == byName.end() ? -1 : bi->second;
				for (int j = 0; j < 3; j++) k.r[j] = static_cast<float>(atof(NextToken(p)));
				for (int j = 0; j < 3; j++) k.t[j] = static_cast<float>(atof(NextToken(p)));
				if (k.bone >= 0) pose->push_back(k);
			}
			NextLine(p);
		}
		gi.FS_FreeFile(buf);
		def.valid = !def.bones.empty();
		if (!def.valid) Com_Printf("face: %s has no bones\n", path.c_str());
		if (def.valid)
		{
			Com_Printf("face: %s, %d bones, %d poses\n", model.c_str(), static_cast<int>(def.bones.size()),
				static_cast<int>(def.poses.size()));
		}
		return def;
	}

	std::string ModelFolder(const CGhoul2Info& g2)
	{
		// models/players/<folder>/model.glm
		std::string f = g2.mFileName;
		const size_t e = f.find_last_of('/');
		if (e == std::string::npos) return f;
		const size_t s = f.find_last_of('/', e - 1);
		return f.substr(s == std::string::npos ? 0 : s + 1, e - (s == std::string::npos ? 0 : s + 1));
	}

	int VisemeIndex(const char c)
	{
		for (int i = 0; i < NUM_VISEMES; i++)
			if (k_visemes[i][0] == c) return i;
		return NUM_VISEMES - 1;
	}

	bool LoadLip(const std::string& soundPath, std::vector<LipKey>& out, int& lengthMs, std::vector<FaceState::Cue>* cues = nullptr)
	{
		std::string base = soundPath;
		const size_t dot = base.find_last_of('.');
		const size_t slash = base.find_last_of('/');
		if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) base = base.substr(0, dot);
		char* buf = nullptr;
		if (gi.FS_ReadFile((base + ".lip").c_str(), reinterpret_cast<void**>(&buf)) <= 0 || !buf) return false;
		out.clear();
		if (cues) cues->clear();
		const char* p = buf;
		while (*p)
		{
			const char* t = NextToken(p);
			if (t[0] == 'e' && t[1] == 0 && cues)
			{
				FaceState::Cue c;
				c.ms = static_cast<int>(atof(NextToken(p)) * 1000.0f);
				c.pose = NextToken(p);
				const char* w = NextToken(p);
				c.w = w[0] ? static_cast<float>(atof(w)) : 1.0f;
				const char* b = NextToken(p);
				c.blend = b[0] ? atoi(b) : 300;
				cues->push_back(c);
			}
			else if (t[0] && t[0] != '#')
			{
				const float sec = static_cast<float>(atof(t));
				const char* s = NextToken(p);
				out.push_back({ static_cast<int>(sec * 1000.0f), VisemeIndex(s[0]) });
			}
			NextLine(p);
		}
		gi.FS_FreeFile(buf);
		lengthMs = out.empty() ? 0 : out.back().ms;
		return !out.empty();
	}

	gentity_t* FindWho(const char* who)
	{
		if (!Q_stricmp(who, "player")) return &g_entities[0];
		if (who[0] >= '0' && who[0] <= '9')
		{
			const int n = atoi(who);
			return n >= 0 && n < MAX_GENTITIES && g_entities[n].inuse ? &g_entities[n] : nullptr;
		}
		for (int i = 0; i < MAX_GENTITIES; i++)
		{
			gentity_t* e = &g_entities[i];
			if (!e->inuse) continue;
			if ((e->targetname && !Q_stricmp(e->targetname, who)) || (e->NPC_targetname && !Q_stricmp(e->NPC_targetname, who))
				|| (e->script_targetname && !Q_stricmp(e->script_targetname, who)))
				return e;
		}
		return nullptr;
	}

	void Rodrigues(const float r[3], float m[3][3])
	{
		const float a = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
		if (a < 1e-7f)
		{
			for (int y = 0; y < 3; y++)
				for (int x = 0; x < 3; x++) m[y][x] = x == y ? 1.0f : 0.0f;
			return;
		}
		const float k[3] = { r[0] / a, r[1] / a, r[2] / a };
		const float s = std::sin(a), c = std::cos(a), v = 1 - c;
		m[0][0] = c + k[0] * k[0] * v; m[0][1] = k[0] * k[1] * v - k[2] * s; m[0][2] = k[0] * k[2] * v + k[1] * s;
		m[1][0] = k[1] * k[0] * v + k[2] * s; m[1][1] = c + k[1] * k[1] * v; m[1][2] = k[1] * k[2] * v - k[0] * s;
		m[2][0] = k[2] * k[0] * v - k[1] * s; m[2][1] = k[2] * k[1] * v + k[0] * s; m[2][2] = c + k[2] * k[2] * v;
	}
}

namespace
{
	float Rand01(unsigned& s)
	{
		s ^= s << 13; s ^= s >> 17; s ^= s << 5;
		return (s & 0xffffff) / 16777216.0f;
	}
	float RandRange(unsigned& s, const float* r) { return r[0] + (r[1] - r[0]) * Rand01(s); }

	// ext_data/faces/profiles.txt:
	//   profile <name>            (name = model folder, e.g. dooku_bf2, or "default")
	//   mood <pose|neutral> <relative frequency>
	//   moodStrength <0..1>   moodHold <min s> <max s>
	//   blink <min s> <max s>   doubleBlink <chance>   halfBlink <chance>
	//   saccade <min s> <max s> <degrees>   glance <chance> <degrees>
	//   micro <amount>   combat <pose> <max weight> [rise/s fall/s]   talkEmphasis <amount>
	//   end
	void LoadProfiles()
	{
		if (g_profilesLoaded) return;
		g_profilesLoaded = true;
		g_profiles["default"].moods = { { "neutral", 4 }, { "thoughtful", 1 }, { "lid_soft", 1 } };
		char* buf = nullptr;
		if (gi.FS_ReadFile("ext_data/faces/profiles.txt", reinterpret_cast<void**>(&buf)) <= 0 || !buf) return;
		FaceProfile* pr = nullptr;
		bool clearedMoods = false;
		const char* p = buf;
		while (*p)
		{
			const std::string w = NextToken(p);
			if (w.empty() || w[0] == '#' || w.rfind("//", 0) == 0) { NextLine(p); continue; }
			auto f = [&]() { return static_cast<float>(atof(NextToken(p))); };
			if (w == "profile") { pr = &g_profiles[NextToken(p)]; clearedMoods = false; }
			else if (w == "end") pr = nullptr;
			else if (pr)
			{
				if (w == "mood")
				{
					if (!clearedMoods) { pr->moods.clear(); clearedMoods = true; }
					std::string pose = NextToken(p);
					pr->moods.emplace_back(pose, f());
				}
				else if (w == "moodStrength") pr->moodStrength = f();
				else if (w == "moodHold") { pr->moodHold[0] = f(); pr->moodHold[1] = f(); }
				else if (w == "blink") { pr->blinkGap[0] = f(); pr->blinkGap[1] = f(); }
				else if (w == "doubleBlink") pr->doubleBlink = f();
				else if (w == "halfBlink") pr->halfBlink = f();
				else if (w == "saccade") { pr->saccadeGap[0] = f(); pr->saccadeGap[1] = f(); pr->saccadeSize = f(); }
				else if (w == "glance") { pr->glance = f(); pr->glanceSize = f(); }
				else if (w == "micro") pr->micro = f();
				else if (w == "combat")
				{
					pr->combatPose = NextToken(p); pr->combatMax = f();
					const char* t = NextToken(p);
					if (t[0]) { pr->combatRise = static_cast<float>(atof(t)); pr->combatFall = f(); }
				}
				else if (w == "talkEmphasis") pr->talkEmphasis = f();
			}
			NextLine(p);
		}
		gi.FS_FreeFile(buf);
		Com_Printf("face: %d profiles\n", static_cast<int>(g_profiles.size()));
	}

	const FaceProfile& ProfileFor(const FaceState& st)
	{
		LoadProfiles();
		auto it = g_profiles.find(st.profileName.empty() ? st.model : st.profileName);
		if (it == g_profiles.end()) it = g_profiles.find("default");
		return it->second;
	}

	bool InCombat(const gentity_t* g)
	{
		if (!g->client) return false;
		const playerState_t& ps = g->client->ps;
		if (ps.weaponTime > 0 || ps.saberMove > 1 || ps.saberBlocked) return true;
		if (g->painDebounceTime > level.time - 1500) return true;
		if (g->enemy && g->enemy->health > 0 && g->enemy->client)
		{
			vec3_t d;
			VectorSubtract(g->enemy->currentOrigin, g->currentOrigin, d);
			if (VectorLengthSquared(d) < 300.0f * 300.0f) return true;
		}
		return false;
	}

	bool MouthBone(const std::string& n)
	{
		static const char* const keys[] = { "Lip", "Jaw", "Chin", "Tongue", "Mouth", "Teeth" };
		for (const char* k : keys)
			if (n.find(k) != std::string::npos) return true;
		return false;
	}
}

// eye offset from the idle behaviour, added in CG_G2EyeAngles (degrees: pitch, yaw)
void CG_FaceEyeOffset(const int entityNum, float out[2])
{
	out[0] = out[1] = 0;
	if (entityNum < 0 || entityNum >= MAX_GENTITIES) return;
	const FaceState& st = g_face[entityNum];
	if (!st.lifeInit || st.idleOff) return;
	out[0] = st.eyeOff[0];
	out[1] = st.eyeOff[1];
}

// called for every sound registration so voice lines can be matched to their lipsync file
void CG_FaceRememberSound(const int handle, const char* name)
{
	if (handle > 0 && name) g_soundNames[handle] = name;
}

// a voice sound starts on an entity
void CG_FaceOnVoice(const int entityNum, const int sfx)
{
	if (entityNum < 0 || entityNum >= MAX_GENTITIES) return;
	const auto it = g_soundNames.find(sfx);
	if (it == g_soundNames.end())
	{
		if (cg_developer.integer) Com_Printf("face: voice sfx %d on %d has no name\n", sfx, entityNum);
		return;
	}
	FaceState& st = g_face[entityNum];
	int len = 0;
	const bool have = LoadLip(it->second, st.lip, len, &st.cues);
	st.nextCue = 0;
	if (cg_developer.integer) Com_Printf("face: voice %s on %d, lipsync %s\n", it->second.c_str(), entityNum, have ? "yes" : "no (volume)");
	if (have)
	{
		st.lipStart = cg.time;
		st.lipEnd = cg.time + len + 100;
	}
	else
	{
		st.lip.clear();
		st.lipEnd = 0;
	}
}

// every frame, after the head and eye angles, for each Ghoul2 character
void CG_FaceUpdate(const centity_t* cent)
{
	gentity_t* gent = cent->gent;
	if (!gent || !gent->client || gent->playerModel < 0 || gent->ghoul2.size() <= gent->playerModel) return;
	CGhoul2Info& g2 = gent->ghoul2[gent->playerModel];
	FaceState& st = g_face[gent->s.number];
	// re-check the model whenever the character's model file changes (playermodel, NPC spawn reuse)
	if (st.glm != g2.mFileName)
	{
		st.glm = g2.mFileName;
		st.model = ModelFolder(g2);
		st.boneIndex.clear();
	}
	const FaceDef& def = LoadDef(st.model);
	if (!def.valid) return;
	if (st.boneIndex.size() != def.bones.size())
	{
		st.boneIndex.resize(def.bones.size());
		for (size_t i = 0; i < def.bones.size(); i++)
			st.boneIndex[i] = gi.G2API_GetBoneIndex(&g2, def.bones[i].name.c_str(), qtrue);
	}

	const int now = cg.time;
	float dt = (now - st.lastTime) * 0.001f;
	if (dt < 0 || dt > 0.25f) dt = 0.016f;
	st.lastTime = now;

	// --- mouth: lipsync file, else voice volume, else rest
	float target[NUM_VISEMES] = { 0 };
	if (!st.lip.empty() && now < st.lipEnd)
	{
		const int t = now - st.lipStart;
		size_t k = 0;
		while (k + 1 < st.lip.size() && st.lip[k + 1].ms <= t) k++;
		target[st.lip[k].shape] = 1.0f;
	}
	else
	{
		const int vol = gi.VoiceVolume[gent->s.number];
		if (vol > 0)
		{
			// rough: louder opens wider; vary shape a little so it doesn't look mechanical
			const float v = vol > 10 ? 1.0f : vol / 10.0f;
			const int pick = (now / 90 + gent->s.number) % 3;
			target[pick == 0 ? 2 : pick == 1 ? 3 : 1] = 0.35f + 0.65f * v;
			target[8] = 1.0f - target[pick == 0 ? 2 : pick == 1 ? 3 : 1];
		}
		else
		{
			target[8] = 1.0f;
		}
	}
	const float kv = 1.0f - std::exp(-dt / 0.045f);
	for (int i = 0; i < NUM_VISEMES; i++) st.vis[i] += (target[i] - st.vis[i]) * kv;

	// --- expression
	if (st.exprUntil && now > st.exprUntil)
	{
		st.exprTarget = 0;
		st.exprUntil = 0;
	}
	const float step = st.exprRate * dt;
	if (st.exprW < st.exprTarget) st.exprW = st.exprW + step > st.exprTarget ? st.exprTarget : st.exprW + step;
	else if (st.exprW > st.exprTarget) st.exprW = st.exprW - step < st.exprTarget ? st.exprTarget : st.exprW - step;

	// --- face life: blinks, gaze, moods, micro motion, combat tension, talk emphasis
	const FaceProfile& pr = ProfileFor(st);
	if (!st.lifeInit)
	{
		st.lifeInit = true;
		st.rng = 0x9E3779B9u ^ (gent->s.number * 2654435761u) ^ static_cast<unsigned>(now);
		if (!st.rng) st.rng = 1;
		// staggered start so actors don't blink or change expression together
		st.nextBlink = now + static_cast<int>(Rand01(st.rng) * pr.blinkGap[1] * 1000.0f);
		st.moodUntil = now + static_cast<int>(Rand01(st.rng) * pr.moodHold[1] * 1000.0f);
		st.nextSaccade = now + static_cast<int>(Rand01(st.rng) * 1500.0f);
		for (float& ph : st.phase) ph = Rand01(st.rng) * 100.0f;
	}
	const bool speaking = (!st.lip.empty() && now < st.lipEnd) || gi.VoiceVolume[gent->s.number] > 0;
	const bool life = !st.idleOff && gent->health > 0;
	// a manual expression (faceexpr / cue) overrides the idle moods
	const float idleScale = life ? 1.0f - (st.exprW > 1 ? 1 : st.exprW) : 0.0f;

	// combat tension builds while fighting and fades afterwards
	if (life && InCombat(gent)) st.tension += pr.combatRise * dt;
	else st.tension -= pr.combatFall * dt;
	st.tension = st.tension < 0 ? 0 : (st.tension > 1 ? 1 : st.tension);

	// blinks: irregular, sometimes double or half; fewer while focused in a fight
	if (now >= st.nextBlink)
	{
		st.blinkStart = now;
		st.blinkLen = 120 + static_cast<int>(Rand01(st.rng) * 70);
		st.blinkDepth = Rand01(st.rng) < pr.halfBlink ? 0.55f + 0.2f * Rand01(st.rng) : 1.0f;
		float gap = RandRange(st.rng, pr.blinkGap) * (1.0f + st.tension * 0.8f);
		if (Rand01(st.rng) < 0.15f) gap *= 0.4f;					// clusters
		st.nextBlink = now + static_cast<int>(gap * 1000.0f);
		st.secondBlink = Rand01(st.rng) < pr.doubleBlink ? now + st.blinkLen + 70 + static_cast<int>(Rand01(st.rng) * 80) : 0;
	}
	if (st.secondBlink && now >= st.secondBlink)
	{
		st.blinkStart = now;
		st.blinkDepth = 1.0f;
		st.secondBlink = 0;
	}
	float blink = 0;
	{
		// fast close, slower open
		const float b = (now - st.blinkStart) / static_cast<float>(st.blinkLen);
		if (b >= 0 && b < 1) blink = st.blinkDepth * (b < 0.35f ? b / 0.35f : 1.0f - (b - 0.35f) / 0.65f);
	}
	if (gent->health <= 0) blink = 1.0f;

	// gaze: small idle saccades and brief glances; tiny ones only when looking at someone on purpose
	if (now >= st.nextSaccade)
	{
		const bool deliberate = gent->client->renderInfo.lookTarget >= 0 && gent->client->renderInfo.lookTarget < ENTITYNUM_WORLD;
		float size = pr.saccadeSize * (deliberate ? 0.35f : 1.0f) * (1.0f - st.tension * 0.6f);
		const bool glance = !deliberate && st.tension < 0.3f && Rand01(st.rng) < pr.glance;
		if (glance)
		{
			size = pr.glanceSize;
			st.glanceUntil = now + 400 + static_cast<int>(Rand01(st.rng) * 900);
		}
		st.eyeGoal[0] = (Rand01(st.rng) * 2 - 1) * size * 0.5f;
		st.eyeGoal[1] = (Rand01(st.rng) * 2 - 1) * size;
		st.nextSaccade = now + static_cast<int>(RandRange(st.rng, pr.saccadeGap) * 1000.0f);
		if (glance) st.nextSaccade = st.glanceUntil;
		if (glance && Rand01(st.rng) < 0.5f && !st.secondBlink) st.nextBlink = now + 60;	// people often blink on a big eye move
	}
	else if (st.glanceUntil && now >= st.glanceUntil)
	{
		st.glanceUntil = 0;
		st.eyeGoal[0] *= 0.15f;
		st.eyeGoal[1] *= 0.15f;
	}
	if (life)
	{
		// saccades are near-instant; CG_G2EyeAngles adds its own short smoothing
		const float ke = 1.0f - std::exp(-dt / 0.025f);
		for (int i = 0; i < 2; i++) st.eyeOff[i] += (st.eyeGoal[i] - st.eyeOff[i]) * ke;
	}

	// moods: drift between the profile's expressions, crossfading slowly
	if (life && now >= st.moodUntil && !pr.moods.empty())
	{
		float total = 0;
		for (const auto& m : pr.moods) total += m.second;
		float r = Rand01(st.rng) * total;
		std::string pick = pr.moods.back().first;
		for (const auto& m : pr.moods)
		{
			if (r < m.second) { pick = m.first; break; }
			r -= m.second;
		}
		if (pick == "neutral") pick.clear();
		if (pick != st.mood)
		{
			st.moodPrev = st.mood;
			st.moodPrevW = st.moodW;
			st.mood = pick;
			st.moodW = 0;
		}
		st.moodTarget = pr.moodStrength * (0.6f + 0.4f * Rand01(st.rng));
		st.moodUntil = now + static_cast<int>(RandRange(st.rng, pr.moodHold) * 1000.0f);
	}
	{
		const float km = 1.0f - std::exp(-dt / 1.2f);				// ~1 s crossfade
		st.moodW += (st.moodTarget - st.moodW) * km;
		st.moodPrevW += (0.0f - st.moodPrevW) * km;
	}

	// talking: brows lift on stressed syllables (a wide-open mouth after a closed one, or a volume jump)
	if (speaking && life)
	{
		int vis = 8;
		for (int i = 0; i < NUM_VISEMES; i++) if (target[i] > 0.5f) vis = i;
		const int vol = gi.VoiceVolume[gent->s.number];
		const bool stress = ((vis == 3 || vis == 2) && (st.lastViseme == 0 || st.lastViseme == 8 || st.lastViseme == 1))
			|| vol - st.lastVol > 4;
		if (stress && Rand01(st.rng) < 0.6f) st.emphasis = 0.6f + 0.4f * Rand01(st.rng);
		st.lastViseme = vis;
		st.lastVol = vol;
	}
	st.emphasis *= std::exp(-dt / 0.35f);

	// expression cues from the line's .lip file drive the manual expression
	if (!st.cues.empty() && now < st.lipEnd)
	{
		const int t = now - st.lipStart;
		while (st.nextCue < st.cues.size() && st.cues[st.nextCue].ms <= t)
		{
			const FaceState::Cue& c = st.cues[st.nextCue++];
			st.exprRate = 1000.0f / (c.blend > 1 ? c.blend : 1);
			if (c.pose == "none") { st.exprTarget = 0; }
			else
			{
				if (st.expr != c.pose) st.exprW = 0;
				st.expr = c.pose;
				st.exprTarget = c.w;
			}
			st.exprUntil = st.lipEnd + 600;		// relax after the line
		}
	}

	// --- blend poses into per-bone local rotation / translation
	const size_t nb = def.bones.size();
	std::vector<float> acc(nb * 6, 0.0f);
	auto add = [&](const char* name, const float w)
		{
			if (w <= 0.001f) return;
			const auto it = def.poses.find(name);
			if (it == def.poses.end()) return;
			for (const FacePoseKey& k : it->second)
				for (int j = 0; j < 3; j++)
				{
					acc[k.bone * 6 + j] += w * k.r[j];
					acc[k.bone * 6 + 3 + j] += w * k.t[j];
				}
		};
	for (int i = 0; i < NUM_VISEMES; i++) add(k_visemes[i], st.vis[i]);
	if (!st.expr.empty()) add(st.expr.c_str(), st.exprW);
	add("blink", blink);

	// idle layer: lipsync owns the mouth while talking
	auto addIdle = [&](const char* name, const float w)
		{
			if (w <= 0.001f || !name[0]) return;
			const auto it = def.poses.find(name);
			if (it == def.poses.end()) return;
			for (const FacePoseKey& k : it->second)
			{
				const float m = speaking && MouthBone(def.bones[k.bone].name) ? 0.15f : 1.0f;
				for (int j = 0; j < 3; j++)
				{
					acc[k.bone * 6 + j] += w * m * k.r[j];
					acc[k.bone * 6 + 3 + j] += w * m * k.t[j];
				}
			}
		};
	if (idleScale > 0)
	{
		const float calm = 1.0f - st.tension;
		addIdle(st.mood.c_str(), st.moodW * idleScale * calm);
		addIdle(st.moodPrev.c_str(), st.moodPrevW * idleScale * calm);
		addIdle(pr.combatPose.c_str(), st.tension * pr.combatMax * idleScale);
		// micro motion: slow, incommensurate drift so the face is never frozen
		const float tt = now * 0.001f;
		const float n1 = 0.5f + 0.5f * std::sin(tt * 0.37f + st.phase[0]) * std::sin(tt * 0.91f + st.phase[1]);
		const float n2 = 0.5f + 0.5f * std::sin(tt * 0.53f + st.phase[2]) * std::sin(tt * 1.27f + st.phase[3]);
		addIdle("brow_up", (n1 * pr.micro + st.emphasis * pr.talkEmphasis) * idleScale);
		addIdle("brow_in", n2 * pr.micro * 0.7f * idleScale);
		addIdle("lid_soft", (1.0f - n1) * pr.micro * idleScale);
	}

	for (size_t i = 0; i < nb; i++)
	{
		if (st.boneIndex[i] < 0) continue;
		const FaceBone& b = def.bones[i];
		float L[3][4];
		float R[3][3];
		Rodrigues(&acc[i * 6], R);
		for (int y = 0; y < 3; y++)
		{
			for (int x = 0; x < 3; x++) L[y][x] = R[y][x];
			L[y][3] = acc[i * 6 + 3 + y];
		}
		// the override is applied after the bone's own animation: base * local * base^-1
		float tmp[3][4];
		mdxaBone_t m;
		Mul34(L, b.inv, tmp);
		Mul34(b.base, tmp, m.matrix);
		gi.G2API_SetBoneAnglesMatrixIndex(&g2, st.boneIndex[i], m, BONE_ANGLES_POSTMULT, nullptr, 0, now);
	}
}

// --- commands
void CG_FaceExpr_f()
{
	if (cgi_Argc() < 3)
	{
		Com_Printf("usage: faceexpr <who> <pose|none> [weight] [seconds] [blend ms]\n");
		return;
	}
	gentity_t* e = FindWho(CG_Argv(1));
	if (!e)
	{
		Com_Printf("faceexpr: nobody called %s\n", CG_Argv(1));
		return;
	}
	FaceState& st = g_face[e->s.number];
	const char* pose = CG_Argv(2);
	const float w = cgi_Argc() > 3 ? static_cast<float>(atof(CG_Argv(3))) : 1.0f;
	const float sec = cgi_Argc() > 4 ? static_cast<float>(atof(CG_Argv(4))) : 0.0f;
	const float blend = cgi_Argc() > 5 ? static_cast<float>(atof(CG_Argv(5))) : 250.0f;
	st.exprRate = 1000.0f / (blend > 1 ? blend : 1);
	if (!Q_stricmp(pose, "none"))
	{
		st.exprTarget = 0;
		st.exprUntil = 0;
		return;
	}
	if (st.expr != pose) st.exprW = 0;
	st.expr = pose;
	st.exprTarget = w;
	st.exprUntil = sec > 0 ? cg.time + static_cast<int>(sec * 1000) : 0;
}

void CG_FaceLip_f()
{
	if (cgi_Argc() < 3)
	{
		Com_Printf("usage: facelip <who> <sound path>\n");
		return;
	}
	gentity_t* e = FindWho(CG_Argv(1));
	if (!e)
	{
		Com_Printf("facelip: nobody called %s\n", CG_Argv(1));
		return;
	}
	const sfxHandle_t h = cgi_S_RegisterSound(CG_Argv(2));
	cgi_S_StartSound(nullptr, e->s.number, CHAN_VOICE, h);
}

void CG_FaceLook_f()
{
	if (cgi_Argc() < 3)
	{
		Com_Printf("usage: facelook <who> <who2|off>\n");
		return;
	}
	gentity_t* e = FindWho(CG_Argv(1));
	if (!e || !e->client)
	{
		Com_Printf("facelook: nobody called %s\n", CG_Argv(1));
		return;
	}
	if (!Q_stricmp(CG_Argv(2), "off"))
	{
		e->client->renderInfo.lookTarget = ENTITYNUM_NONE;
		return;
	}
	const gentity_t* t = FindWho(CG_Argv(2));
	if (t) e->client->renderInfo.lookTarget = t->s.number;
}

void CG_FaceInfo_f()
{
	gentity_t* e = FindWho(cgi_Argc() > 1 ? CG_Argv(1) : "player");
	if (!e || !e->client || e->playerModel < 0 || e->ghoul2.size() <= e->playerModel)
	{
		Com_Printf("faceinfo: no Ghoul2 character\n");
		return;
	}
	CGhoul2Info& g2 = e->ghoul2[e->playerModel];
	const std::string model = ModelFolder(g2);
	const FaceDef& def = LoadDef(model);
	const FaceState& st = g_face[e->s.number];
	int found = 0;
	for (const int b : st.boneIndex) found += b >= 0;
	Com_Printf("faceinfo: entity %d, glm %s, model '%s', face file %s (%d bones, %d poses), bones found %d/%d\n",
		e->s.number, g2.mFileName, model.c_str(), def.valid ? "loaded" : "MISSING",
		static_cast<int>(def.bones.size()), static_cast<int>(def.poses.size()), found, static_cast<int>(st.boneIndex.size()));
	Com_Printf("  expression '%s' %.2f -> %.2f, lipsync %s, voice volume %d, update ran %d ms ago\n", st.expr.c_str(),
		st.exprW, st.exprTarget, !st.lip.empty() && cg.time < st.lipEnd ? "playing" : "none",
		gi.VoiceVolume[e->s.number], cg.time - st.lastTime);
}

// cineaudio play <file> [who]  - play any sound as someone's voice line, with lipsync
//   <file>: full path ("sound/chars/dooku/misc/taunt1.mp3") or a bare name, looked up in sound/cine/
//   [who]: player (default), entity number, or targetname / NPC_targetname / script_targetname
// cineaudio stop [who]          - stop that person's lipsync (the sound itself finishes)
void CG_CineAudio_f()
{
	if (cgi_Argc() < 2 || (Q_stricmp(CG_Argv(1), "play") && Q_stricmp(CG_Argv(1), "stop")))
	{
		Com_Printf("usage: cineaudio play <file> [who]   |   cineaudio stop [who]\n");
		return;
	}
	const bool play = !Q_stricmp(CG_Argv(1), "play");
	const char* whoName = cgi_Argc() > (play ? 3 : 2) ? CG_Argv(play ? 3 : 2) : "player";
	gentity_t* e = FindWho(whoName);
	if (!e)
	{
		Com_Printf("cineaudio: nobody called %s\n", whoName);
		return;
	}
	if (!play)
	{
		g_face[e->s.number].lip.clear();
		g_face[e->s.number].lipEnd = 0;
		return;
	}
	if (cgi_Argc() < 3)
	{
		Com_Printf("usage: cineaudio play <file> [who]\n");
		return;
	}
	std::string path = CG_Argv(2);
	if (path.find('/') == std::string::npos) path = "sound/cine/" + path;
	const sfxHandle_t h = cgi_S_RegisterSound(path.c_str());
	if (!h)
	{
		Com_Printf("cineaudio: can't load %s\n", path.c_str());
		return;
	}
	cgi_S_StartSound(nullptr, e->s.number, CHAN_VOICE_GLOBAL, h);
}

// faceidle <who> on|off|<profile>   idle behaviour (blinks, gaze, moods) per character
void CG_FaceIdle_f()
{
	if (cgi_Argc() < 3)
	{
		Com_Printf("usage: faceidle <who> on|off|<profile>\n");
		return;
	}
	const gentity_t* e = FindWho(CG_Argv(1));
	if (!e)
	{
		Com_Printf("faceidle: nobody called %s\n", CG_Argv(1));
		return;
	}
	FaceState& st = g_face[e->s.number];
	const char* a = CG_Argv(2);
	if (!Q_stricmp(a, "off")) st.idleOff = true;
	else if (!Q_stricmp(a, "on")) st.idleOff = false;
	else
	{
		LoadProfiles();
		st.profileName = a;
		st.idleOff = false;
		st.moodUntil = 0;
		if (!g_profiles.count(a)) Com_Printf("faceidle: no profile '%s' (using default)\n", a);
	}
}
