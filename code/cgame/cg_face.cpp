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
		std::vector<int> boneIndex;
		int boneModel = -1;
	};

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

	bool LoadLip(const std::string& soundPath, std::vector<LipKey>& out, int& lengthMs)
	{
		std::string base = soundPath;
		const size_t dot = base.find_last_of('.');
		const size_t slash = base.find_last_of('/');
		if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) base = base.substr(0, dot);
		char* buf = nullptr;
		if (gi.FS_ReadFile((base + ".lip").c_str(), reinterpret_cast<void**>(&buf)) <= 0 || !buf) return false;
		out.clear();
		const char* p = buf;
		while (*p)
		{
			const char* t = NextToken(p);
			if (t[0] && t[0] != '#')
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
	if (it == g_soundNames.end()) return;
	FaceState& st = g_face[entityNum];
	int len = 0;
	if (LoadLip(it->second, st.lip, len))
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
	if (st.boneModel != g2.mModelindex)
	{
		st.model = ModelFolder(g2);
		st.boneIndex.clear();
		st.boneModel = g2.mModelindex;
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

	// --- blink every 2-6 s, 160 ms
	if (now >= st.nextBlink)
	{
		st.blinkStart = now;
		st.nextBlink = now + 2000 + rand() % 4000;
	}
	float blink = 0;
	{
		const float b = (now - st.blinkStart) / 160.0f;
		if (b >= 0 && b < 1) blink = 1.0f - std::fabs(b * 2 - 1);
	}
	if (gent->health <= 0) blink = 1.0f;

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
