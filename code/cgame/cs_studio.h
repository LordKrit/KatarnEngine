/*
JK BUILD: Cinematic Studio - engine glue declarations (cgame side).
See docs/cinematic_studio/ARCHITECTURE.md.
*/
#pragma once

#include "cs_core.h"

namespace cs
{
	class GameHost final : public Host
	{
	public:
		int SpawnActor(const std::string& npcType, const float origin[3], const float angles[3], const std::string& name) override;
		void RemoveActor(int ent) override;
		bool ActorAlive(int ent) override;
		bool GetActorTransform(int ent, float origin[3], float angles[3]) override;
		void SetActorTransform(int ent, const float origin[3], const float angles[3]) override;
		void SetActorAI(int ent, bool on) override;
		std::string MapName() override;
		bool WriteFile(const std::string& path, const std::string& data) override;
		bool ReadFile(const std::string& path, std::string& out) override;
		int64_t NowMs() override;
		void Log(int level, const std::string& msg) override;
	};

	// what the inspector shows about a character entity
	struct EntInfo
	{
		int ent = -1;
		bool isPlayer = false;
		std::string npcType, targetname, model, animProfile, legsAnim, torsoAnim;
		int health = 0;
		bool ai = true;
		float origin[3] = { 0, 0, 0 }, mins[3] = { 0, 0, 0 }, maxs[3] = { 0, 0, 0 };
		float yaw = 0;
	};
	bool EntityInfo(int ent, EntInfo& out);
	int ListCharacters(int* out, int max);	// player + NPCs in the level
	bool TraceFromView(const float start[3], const float dir[3], float dist, float hit[3]);

	void UI_Message(int level, const std::string& msg);	// status line
}

// called from the cgame
void CS_Init();							// CG_Init (new level)
void CS_Shutdown();						// CG_Shutdown
void CS_Command_f();					// "studio ..." console command
int CS_KeyEvent(int key, int down);		// vmMain CG_KEY_EVENT
int CS_MouseEvent(int dx, int dy);		// vmMain CG_MOUSE_EVENT
void CS_Frame();						// every CG_DrawActiveFrame, before the view is built
bool CS_ApplyView();					// after the view is built: the studio camera, if active
bool CS_IsOpen();
void CS_Draw2D();						// instead of the HUD while the studio is open
