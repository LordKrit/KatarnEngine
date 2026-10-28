/*
JK BUILD: Cinematic Studio - props, game side (Phase 7).

Props are real map entities of any class the game can spawn (items, weapons, breakables, effect
runners, turrets, NPC spawners, movers with a model of the level...), spawned from key / values like
the level loader does, plus "cs_model": just a model (.md3, or .glm through misc_model_ghoul). The
studio moves them (keys) or holds them on an actor's bone (attachments); attached props are placed
again when each frame is drawn, so they follow the hand exactly.
*/
#include "cg_headers.h"
#include "cs_studio.h"

#include "../game/g_local.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

extern gentity_t* CS_SpawnMapEntity(const char* const* keys, const char* const* values, int n);
extern int CS_ListSpawnClasses(const char** out, int max);

namespace cs
{
	namespace
	{
		gentity_t* Ent(const int n)
		{
			if (n <= 0 || n >= ENTITYNUM_WORLD) return nullptr;
			gentity_t* e = &g_entities[n];
			return e->inuse ? e : nullptr;
		}

		bool EndsWith(const std::string& s, const char* ext)
		{
			const size_t n = strlen(ext);
			return s.size() >= n && !Q_stricmp(s.c_str() + s.size() - n, ext);
		}

		std::string Vec3(const float v[3])
		{
			char b[96];
			Com_sprintf(b, sizeof b, "%g %g %g", v[0], v[1], v[2]);
			return b;
		}

		// where the studio draws a prop this frame (attachments), per entity
		struct RenderPose { bool on = false; vec3_t origin, angles; };
		RenderPose g_pose[MAX_GENTITIES];
	}

	int GameHost::SpawnProp(const std::string& classname, const std::vector<std::pair<std::string, std::string>>& kv,
		const float origin[3], const float angles[3])
	{
		std::string cls = classname, model;
		std::vector<std::pair<std::string, std::string>> keys;
		for (auto& p : kv)
		{
			if (!Q_stricmp(p.first.c_str(), "origin") || !Q_stricmp(p.first.c_str(), "angles") || !Q_stricmp(p.first.c_str(), "angle") ||
				!Q_stricmp(p.first.c_str(), "classname")) continue;
			if (!Q_stricmp(p.first.c_str(), "model")) model = p.second;
			keys.push_back(p);
		}
		if (cls == "cs_model")
		{
			if (model.empty()) return -1;
			if (EndsWith(model, ".glm")) cls = "misc_model_ghoul";	// Ghoul2 models: the game's own entity for them
			else
			{
				// a plain model: drawn by the cgame as a general entity, not solid
				gentity_t* e = G_Spawn();
				if (!e) return -1;
				e->classname = "cs_model";
				e->s.eType = ET_GENERAL;
				e->s.modelindex = G_ModelIndex(model.c_str());
				float scale = 1;
				for (auto& p : keys) if (!Q_stricmp(p.first.c_str(), "modelscale")) scale = static_cast<float>(atof(p.second.c_str()));
				if (scale <= 0) scale = 1;
				VectorSet(e->s.modelScale, scale, scale, scale);
				VectorSet(e->mins, -16 * scale, -16 * scale, -16 * scale);
				VectorSet(e->maxs, 16 * scale, 16 * scale, 16 * scale);
				e->contents = 0;
				e->s.radius = static_cast<int>(64 * scale);
				vec3_t o = { origin[0], origin[1], origin[2] }, a = { angles[0], angles[1], angles[2] };
				G_SetOrigin(e, o);
				G_SetAngles(e, a);
				gi.linkentity(e);
				return e->s.number;
			}
		}
		std::vector<std::string> k = { "classname", "origin", "angles" }, v = { cls, Vec3(origin), Vec3(angles) };
		for (auto& p : keys) { k.push_back(p.first); v.push_back(p.second); }
		std::vector<const char*> kp, vp;
		for (size_t i = 0; i < k.size(); i++) { kp.push_back(k[i].c_str()); vp.push_back(v[i].c_str()); }
		gentity_t* e = CS_SpawnMapEntity(kp.data(), vp.data(), static_cast<int>(kp.size()));
		if (!e) return -1;
		g_pose[e->s.number].on = false;
		return e->s.number;
	}

	void GameHost::SetPropTransform(const int ent, const float origin[3], const float angles[3])
	{
		gentity_t* e = Ent(ent);
		if (!e || e->client) return;
		vec3_t o = { origin[0], origin[1], origin[2] }, a = { angles[0], angles[1], angles[2] };
		G_SetOrigin(e, o);
		G_SetAngles(e, a);
		VectorCopy(o, e->pos1);		// movers (doors, platforms) go back to here
		gi.linkentity(e);
	}

	void GameHost::SetEntityScale(const int ent, const float scale)
	{
		if (ent <= 0 || ent >= ENTITYNUM_WORLD || !g_entities[ent].inuse) return;
		gentity_t* e = &g_entities[ent];
		const float sc = scale > 0 ? scale : 1.0f;
		VectorSet(e->s.modelScale, sc, sc, sc);
		gi.linkentity(e);
	}

	std::vector<std::string> ListSpawnClasses()
	{
		static std::vector<std::string> out;
		if (!out.empty()) return out;
		const char* names[2048];
		const int n = CS_ListSpawnClasses(names, 2048);
		for (int i = 0; i < n; i++)
		{
			if (!names[i] || !names[i][0]) continue;
			if (!Q_strncmp(names[i], "info_player", 11) || !Q_stricmp(names[i], "worldspawn")) continue;
			out.emplace_back(names[i]);
		}
		std::sort(out.begin(), out.end());
		out.erase(std::unique(out.begin(), out.end()), out.end());
		out.insert(out.begin(), "cs_model");
		return out;
	}

	// model files under a folder (and one level of sub-folders)
	std::vector<std::string> ListModels(const std::string& dir)
	{
		std::vector<std::string> out;
		static char buf[65536];
		for (const char* ext : { ".md3", ".glm" })
		{
			const int n = gi.FS_GetFileList(dir.c_str(), ext, buf, sizeof buf);
			const char* p = buf;
			for (int i = 0; i < n && p < buf + sizeof buf; i++) { out.push_back(dir + "/" + p); p += strlen(p) + 1; }
		}
		const int nd = gi.FS_GetFileList(dir.c_str(), "/", buf, sizeof buf);
		std::vector<std::string> subs;
		const char* p = buf;
		for (int i = 0; i < nd && p < buf + sizeof buf; i++) { if (p[0] && p[0] != '.') subs.emplace_back(p); p += strlen(p) + 1; }
		static char buf2[65536];
		for (auto& sd : subs)
			for (const char* ext : { ".md3", ".glm" })
			{
				const std::string d = dir + "/" + sd;
				const int n = gi.FS_GetFileList(d.c_str(), ext, buf2, sizeof buf2);
				const char* q = buf2;
				for (int i = 0; i < n && q < buf2 + sizeof buf2; i++) { out.push_back(d + "/" + q); q += strlen(q) + 1; }
			}
		std::sort(out.begin(), out.end());
		return out;
	}

	bool PropBounds(const int ent, float mins[3], float maxs[3])
	{
		const gentity_t* e = Ent(ent);
		if (!e) return false;
		VectorCopy(e->absmin, mins);
		VectorCopy(e->absmax, maxs);
		if (maxs[0] - mins[0] < 2 || maxs[2] - mins[2] < 2)
			for (int i = 0; i < 3; i++) { mins[i] = e->currentOrigin[i] - 8; maxs[i] = e->currentOrigin[i] + 8; }
		return true;
	}

	bool PropAlive(const int ent) { return Ent(ent) != nullptr; }

	// a bone / tag of an actor in the world now, plus an offset in its frame
	bool AttachPose(const int actorEnt, const std::string& bolt, const float offset[3], const float angOff[3], float org[3], float ang[3])
	{
		if (actorEnt < 0 || actorEnt >= ENTITYNUM_WORLD) return false;
		gentity_t* a = &g_entities[actorEnt];
		if (!a->inuse || !a->client || !a->ghoul2.size() || a->playerModel < 0 || a->playerModel >= a->ghoul2.size()) return false;
		const int b = gi.G2API_AddBolt(&a->ghoul2[a->playerModel], bolt.c_str());
		if (b < 0) return false;
		const centity_t& c = cg_entities[actorEnt];
		const vec3_t mAng = { 0, c.pe.legs.yawAngle, 0 };
		vec3_t mOrg;
		VectorCopy(c.lerpOrigin, mOrg);
		if (VectorCompare(mOrg, vec3_origin)) VectorCopy(a->currentOrigin, mOrg);
		mdxaBone_t m;
		if (!gi.G2API_GetBoltMatrix(a->ghoul2, a->playerModel, b, &m, mAng, mOrg, cg.time, nullptr, a->s.modelScale)) return false;
		vec3_t p, ax[3];
		gi.G2API_GiveMeVectorFromMatrix(m, ORIGIN, p);
		for (int i = 0; i < 3; i++) { ax[0][i] = m.matrix[i][0]; ax[1][i] = m.matrix[i][1]; ax[2][i] = m.matrix[i][2]; }
		// offset in the bolt's frame
		for (int i = 0; i < 3; i++) org[i] = p[i] + ax[0][i] * offset[0] + ax[1][i] * offset[1] + ax[2][i] * offset[2];
		// orientation: the bolt's axes, turned by the angle offset
		vec3_t off[3], full[3];
		const vec3_t ao = { angOff[0], angOff[1], angOff[2] };
		AnglesToAxis(ao, off);
		for (int r = 0; r < 3; r++)
			for (int col = 0; col < 3; col++)
				full[r][col] = off[r][0] * ax[0][col] + off[r][1] * ax[1][col] + off[r][2] * ax[2][col];
		vec3_t out;
		vectoangles(full[0], out);
		// roll from the up axis
		vec3_t right, up;
		AngleVectors(out, nullptr, right, up);
		out[ROLL] = atan2f(DotProduct(full[2], right) * -1.0f, DotProduct(full[2], up)) * 180.0f / static_cast<float>(M_PI);
		VectorCopy(out, ang);
		return true;
	}

	void SetPropRenderPose(const int ent, const float org[3], const float ang[3])
	{
		if (ent <= 0 || ent >= MAX_GENTITIES) return;
		g_pose[ent].on = true;
		VectorCopy(org, g_pose[ent].origin);
		VectorCopy(ang, g_pose[ent].angles);
	}

	void ClearPropRenderPoses()
	{
		for (auto& p : g_pose) p.on = false;
	}
}

// CG_CalcEntityLerpPositions: an attached prop is drawn exactly on its bone this frame
void CS_PropRenderPose(centity_s* cent)
{
	const int n = cent->currentState.number;
	if (n <= 0 || n >= MAX_GENTITIES || !cs::g_pose[n].on) return;
	VectorCopy(cs::g_pose[n].origin, cent->lerpOrigin);
	VectorCopy(cs::g_pose[n].angles, cent->lerpAngles);
}
