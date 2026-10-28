/*
JK BUILD: Cinematic Studio - combat replay, game side (Phase 5).

Captures the events the cgame handles (effects, sounds, hits, blood, sparks, voices, footsteps...) so a
recorded fight can play them again, fires them again on playback, and shows recorded force powers on
the actors that play takes back (only while the frame is drawn, so replayed lightning can't hurt).
*/
#include "cg_headers.h"
#include "cs_studio.h"

#include "../game/g_local.h"

#include <algorithm>
#include <map>
#include <vector>

namespace cs
{
	namespace
	{
		constexpr int kFiredFlag = 0x40000000;	// spawnflags of the temp entities the studio fires (not captured again)
		bool g_capOn = false;
		std::vector<CapturedEvent> g_cap;
		std::vector<int> g_skip;				// entities whose events are the studio's playback

		bool Character(const int n)
		{
			return n >= 0 && n < ENTITYNUM_WORLD && g_entities[n].inuse && g_entities[n].client;
		}

		// events that ride on a character and only show / sound something
		bool CharacterEvent(const int ev)
		{
			return (ev >= EV_FOOTSTEP && ev <= EV_ROLL) || (ev >= EV_WATER_TOUCH && ev <= EV_LAVA_UNDER) ||
				ev == EV_FIRE_WEAPON || ev == EV_ALT_FIRE || ev == EV_GENERAL_SOUND || ev == EV_GLOBAL_SOUND ||
				(ev >= EV_PAIN && ev <= EV_DEATH3) || (ev >= EV_ANGER1 && ev <= EV_GIVEUP4);
		}

		std::string Config(const int base, const int index, const int max)
		{
			if (index <= 0 || index >= max) return "";
			const char* s = CG_ConfigString(base + index);
			return s ? s : "";
		}
	}

	void SetEventCapture(const bool on)
	{
		g_capOn = on;
		if (!on) g_cap.clear();
	}

	bool EventCaptureOn() { return g_capOn; }

	std::vector<CapturedEvent> TakeCapturedEvents()
	{
		std::vector<CapturedEvent> out;
		out.swap(g_cap);
		return out;
	}

	void PruneCapturedEvents(const int64_t beforeUs)
	{
		g_cap.erase(std::remove_if(g_cap.begin(), g_cap.end(), [&](const CapturedEvent& c) { return c.absUs < beforeUs; }), g_cap.end());
	}

	void SetCaptureSkip(const std::vector<int>& ents) { g_skip = ents; }

	int64_t CaptureNowUs() { return static_cast<int64_t>(cg.time) * 1000; }

	void CaptureEvent(centity_t* cent)
	{
		if (!g_capOn || !cent) return;
		const entityState_t& es = cent->currentState;
		const int ev = es.event & ~EV_EVENT_BITS;
		if (!ev) return;
		CapturedEvent c;
		WorldEvent& w = c.ev;
		w.event = ev;
		w.temp = es.eType > ET_EVENTS;
		c.absUs = CaptureNowUs();
		if (w.temp)
		{
			const gentity_t* g = cent->gent;
			if (g && (g->spawnflags & kFiredFlag)) return;		// the studio's own playback
			if (es.boltInfo != 0 || ev == EV_STOP_EFFECT) return;	// bolted onto a model: can't be moved to an actor
			if (g) { VectorCopy(g->pos3, w.pos3); VectorCopy(g->pos4, w.pos4); }
			if (es.otherentityNum > 0 && Character(es.otherentityNum)) c.actorEnt = es.otherentityNum;
			if (es.otherentityNum2 > 0 && Character(es.otherentityNum2)) c.other2Ent = es.otherentityNum2;
		}
		else
		{
			if (!CharacterEvent(ev) || !Character(es.number)) return;
			if (std::find(g_skip.begin(), g_skip.end(), es.number) != g_skip.end()) return;
			c.actorEnt = es.number;
		}
		VectorCopy(cent->lerpOrigin, w.origin);
		VectorCopy(es.angles, w.angles);
		VectorCopy(es.angles2, w.angles2);
		VectorCopy(es.origin2, w.origin2);
		w.parm = es.eventParm;
		w.weapon = es.weapon;
		w.loopSound = es.loopSound;
		w.modelindex = es.modelindex;
		w.time = es.time;
		w.time2 = es.time2;
		// indexes differ between levels and sessions: keep the names
		if (ev == EV_PLAY_EFFECT || ev == EV_PLAY_MUZZLE_EFFECT) w.fx = Config(CS_EFFECTS, es.eventParm, MAX_FX);
		if (ev == EV_GENERAL_SOUND || ev == EV_GLOBAL_SOUND) w.sound = Config(CS_SOUNDS, es.eventParm, MAX_SOUNDS);
		if (g_cap.size() < 200000) g_cap.push_back(c);
	}

	void FireEvent(const WorldEvent& w, const int actorEnt, const int other2Ent)
	{
		int parm = w.parm;
		if (!w.fx.empty()) parm = G_EffectIndex(w.fx.c_str());
		else if (!w.sound.empty()) parm = G_SoundIndex(w.sound.c_str());
		if (!w.temp)
		{
			if (!Character(actorEnt)) return;
			G_AddEvent(&g_entities[actorEnt], w.event, parm);
			return;
		}
		vec3_t o = { w.origin[0], w.origin[1], w.origin[2] };
		gentity_t* te = G_TempEntity(o, w.event);
		if (!te) return;
		te->spawnflags |= kFiredFlag;
		te->s.eventParm = parm;
		te->s.otherentityNum = Character(actorEnt) ? actorEnt : 0;
		te->s.otherentityNum2 = Character(other2Ent) ? other2Ent : 0;
		VectorCopy(w.angles, te->s.angles);
		VectorCopy(w.angles2, te->s.angles2);
		VectorCopy(w.origin2, te->s.origin2);
		te->s.weapon = w.weapon;
		te->s.loopSound = w.loopSound;
		te->s.modelindex = w.modelindex;
		te->s.time = w.time;
		te->s.time2 = w.time2;
		VectorCopy(w.pos3, te->pos3);
		VectorCopy(w.pos4, te->pos4);
		if (w.event == EV_PLAY_EFFECT)
		{
			VectorSet(te->maxs, 32, 32, 32);	// as G_PlayEffect
			VectorScale(te->maxs, -1, te->mins);
		}
		gi.linkentity(te);
	}

	// ---- force powers on playback actors: set for drawing, put back after ----
	namespace
	{
		struct ForceSave { int active; int shocked, stunned; bool drained; };
		std::map<int, ForceSave> g_force;
	}

	void SetForceView(const int ent, const int forceBits, const int takeFlags)
	{
		if (!Character(ent) || ent == 0) return;
		playerState_t& ps = g_entities[ent].client->ps;
		if (!g_force.count(ent))
			g_force[ent] = { ps.forcePowersActive, ps.powerups[PW_SHOCKED], ps.powerups[PW_STUNNED], (g_entities[ent].s.powerups & (1 << PW_DRAINED)) != 0 };
		ps.forcePowersActive = forceBits;
		if (takeFlags & TAKE_SHOCKED) ps.powerups[PW_SHOCKED] = cg.time + 200;
		if (takeFlags & TAKE_STUNNED) ps.powerups[PW_STUNNED] = cg.time + 200;
		if (takeFlags & TAKE_DRAINED) g_entities[ent].s.powerups |= 1 << PW_DRAINED;
	}

	void EndFrameForce()
	{
		for (auto& kv : g_force)
		{
			if (!Character(kv.first)) continue;
			gentity_t& e = g_entities[kv.first];
			e.client->ps.forcePowersActive = kv.second.active;
			e.client->ps.powerups[PW_SHOCKED] = kv.second.shocked;
			e.client->ps.powerups[PW_STUNNED] = kv.second.stunned;
			if (!kv.second.drained) e.s.powerups &= ~(1 << PW_DRAINED);
		}
		g_force.clear();
	}

	std::string CvarString(const char* name)
	{
		char buf[256] = { 0 };
		gi.Cvar_VariableStringBuffer(name, buf, sizeof buf);
		return buf;
	}

	// eyes and head toward another character (ENTITYNUM_NONE / -1 = their own business)
	void SetLookTarget(const int ent, const int target)
	{
		if (!Character(ent)) return;
		g_entities[ent].client->renderInfo.lookTarget = Character(target) ? target : ENTITYNUM_NONE;
		g_entities[ent].client->renderInfo.lookTargetClearTime = 0;
	}

	// what a character's force / receiving-end state is now (for samples)
	void ForceState(const int ent, int& forceBits, int& takeFlags)
	{
		forceBits = 0;
		if (!Character(ent)) return;
		const gentity_t& e = g_entities[ent];
		forceBits = e.client->ps.forcePowersActive;
		if (e.client->ps.powerups[PW_SHOCKED] > level.time) takeFlags |= TAKE_SHOCKED;
		if (e.client->ps.powerups[PW_STUNNED] > level.time) takeFlags |= TAKE_STUNNED;
		if (e.s.powerups & (1 << PW_DRAINED)) takeFlags |= TAKE_DRAINED;
	}
}

void CS_CaptureEvent(centity_t* cent)
{
	cs::CaptureEvent(cent);
}

void CS_EndFrame()
{
	cs::EndFrameForce();
}
