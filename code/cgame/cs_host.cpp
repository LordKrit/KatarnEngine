/*
JK BUILD: Cinematic Studio - the game side of cs::Host (actors are real NPC entities, files go
through the game's filesystem), plus entity queries for the editor UI.
*/
#include "cg_headers.h"
#include "cs_studio.h"

#include "../game/g_local.h"
#include "../game/b_local.h"

#include <cstring>

extern gentity_t* CS_SpawnNPC(const char* npcType, const vec3_t origin, float yaw, const char* targetname);

namespace cs
{
	static gentity_t* Ent(const int n)
	{
		if (n < 0 || n >= ENTITYNUM_WORLD) return nullptr;
		gentity_t* e = &g_entities[n];
		return e->inuse ? e : nullptr;
	}

	int GameHost::SpawnActor(const std::string& npcType, const float origin[3], const float angles[3], const std::string& name)
	{
		std::string target = "cs_";
		for (const char c : name) target += (c == ' ' ? '_' : c);
		vec3_t o = { origin[0], origin[1], origin[2] };
		gentity_t* npc = CS_SpawnNPC(npcType.c_str(), o, angles[YAW], target.c_str());
		if (!npc) return -1;
		SetActorTransform(npc->s.number, origin, angles);
		return npc->s.number;
	}

	void GameHost::RemoveActor(const int ent)
	{
		gentity_t* e = Ent(ent);
		if (!e || e->s.number == 0) return;	// never the player
		G_FreeEntity(e);
	}

	bool GameHost::ActorAlive(const int ent)
	{
		const gentity_t* e = Ent(ent);
		return e && e->client;
	}

	bool GameHost::GetActorTransform(const int ent, float origin[3], float angles[3])
	{
		const gentity_t* e = Ent(ent);
		if (!e) return false;
		VectorCopy(e->currentOrigin, origin);
		if (e->client) { angles[PITCH] = 0; angles[YAW] = e->client->ps.viewangles[YAW]; angles[ROLL] = 0; }
		else VectorCopy(e->currentAngles, angles);
		return true;
	}

	void GameHost::SetActorTransform(const int ent, const float origin[3], const float angles[3])
	{
		gentity_t* e = Ent(ent);
		if (!e) return;
		vec3_t o = { origin[0], origin[1], origin[2] };
		vec3_t a = { 0, angles[YAW], 0 };	// actors turn about up only; pitch/roll come from animation
		G_SetOrigin(e, o);
		if (e->client)
		{
			VectorCopy(o, e->client->ps.origin);
			VectorClear(e->client->ps.velocity);
			SetClientViewAngle(e, a);
			if (e->NPC)
			{
				e->NPC->desiredYaw = a[YAW];
				e->NPC->desiredPitch = 0;
				e->NPC->lockedDesiredYaw = a[YAW];
			}
		}
		else G_SetAngles(e, a);
		gi.linkentity(e);
	}

	void GameHost::SetActorAI(const int ent, const bool on)
	{
		gentity_t* e = Ent(ent);
		if (!e || !e->NPC) return;
		e->NPC->behaviorState = on ? BS_DEFAULT : BS_CINEMATIC;
		e->NPC->defaultBehavior = on ? BS_DEFAULT : BS_CINEMATIC;
	}

	std::string GameHost::MapName()
	{
		const char* m = level.mapname;
		return m ? m : "";
	}

	bool GameHost::WriteFile(const std::string& path, const std::string& data)
	{
		fileHandle_t f = 0;
		gi.FS_FOpenFile(path.c_str(), &f, FS_WRITE);
		if (!f) return false;
		const int n = gi.FS_Write(data.data(), static_cast<int>(data.size()), f);
		gi.FS_FCloseFile(f);
		return n == static_cast<int>(data.size());
	}

	bool GameHost::ReadFile(const std::string& path, std::string& out)
	{
		void* buf = nullptr;
		const long len = gi.FS_ReadFile(path.c_str(), &buf);
		if (len < 0 || !buf) return false;
		out.assign(static_cast<const char*>(buf), static_cast<size_t>(len));
		gi.FS_FreeFile(buf);
		return true;
	}

	int64_t GameHost::NowMs()
	{
		return gi.Milliseconds();
	}

	void GameHost::Log(const int level, const std::string& msg)
	{
		const char* col = level >= 2 ? S_COLOR_RED : (level == 1 ? S_COLOR_YELLOW : S_COLOR_CYAN);
		Com_Printf("%sStudio:%s %s\n", col, S_COLOR_WHITE, msg.c_str());
		UI_Message(level, msg);
	}

	// ------------------------------------------------------------------------------------------
	// entity queries for the editor
	// ------------------------------------------------------------------------------------------

	bool EntityInfo(const int ent, EntInfo& out)
	{
		const gentity_t* e = Ent(ent);
		if (!e || !e->client) return false;
		out = EntInfo();
		out.ent = ent;
		out.isPlayer = ent == 0;
		out.npcType = e->NPC_type ? e->NPC_type : (ent == 0 ? "player" : "");
		out.targetname = e->targetname ? e->targetname : "";
		out.health = e->health;
		out.ai = e->NPC ? e->NPC->behaviorState != BS_CINEMATIC : true;
		VectorCopy(e->currentOrigin, out.origin);
		VectorCopy(e->absmin, out.mins);
		VectorCopy(e->absmax, out.maxs);
		out.yaw = e->client->ps.viewangles[YAW];
		const int legs = e->client->ps.legsAnim, torso = e->client->ps.torsoAnim;
		out.legsAnim = (legs >= 0 && legs < MAX_ANIMATIONS && animTable[legs].name) ? animTable[legs].name : "";
		out.torsoAnim = (torso >= 0 && torso < MAX_ANIMATIONS && animTable[torso].name) ? animTable[torso].name : "";
		const int af = e->client->clientInfo.animFileIndex;
		out.animProfile = (af >= 0 && af < level.numKnownAnimFileSets) ? level.knownAnimFileSets[af].filename : "";
		if (e->ghoul2.size() && e->playerModel >= 0 && e->playerModel < e->ghoul2.size())
		{
			const char* fn = e->ghoul2[e->playerModel].mFileName;
			out.model = fn ? fn : "";
		}
		return true;
	}

	int ListCharacters(int* out, const int max)
	{
		int n = 0;
		for (int i = 0; i < ENTITYNUM_WORLD && n < max; i++)
		{
			const gentity_t* e = &g_entities[i];
			if (e->inuse && e->client && (i == 0 || e->NPC) && !(e->s.eFlags & EF_NODRAW)) out[n++] = i;
		}
		return n;
	}

	bool TraceFromView(const float start[3], const float dir[3], const float dist, float hit[3])
	{
		trace_t tr;
		vec3_t s = { start[0], start[1], start[2] }, end;
		VectorMA(s, dist, dir, end);
		gi.trace(&tr, s, nullptr, nullptr, end, ENTITYNUM_NONE, MASK_SOLID, static_cast<EG2_Collision>(0), 0);
		VectorCopy(tr.endpos, hit);
		return tr.fraction < 1.0f;
	}
}
