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
		bool SetActorProfile(int ent, const std::string& profile, std::string& why, bool force) override;
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

	// ---- Actor Manager (cs_actor.cpp) ----
	struct AnimInfo
	{
		int anim = -1;			// animNumber_t
		std::string name;		// BOTH_STAND1...
		int numFrames = 0;
		int frameLerp = 50;		// ms per frame
		bool loops = false;
		bool extra = false;		// named in the profile's animation.cfg but not in the game's animTable
	};
	int ListAnims(int ent, std::vector<AnimInfo>& out);	// animations the actor's skeleton has
	int AnimByName(const std::string& name);
	bool AnimInfoFor(int ent, int anim, AnimInfo& out);
	// Play (speed > 0) or pose (speed 0, at `frame`) an animation; frame < 0 = from the start of the range.
	// The studio holds the actor until ReleaseAnim.
	bool PoseAnim(int ent, int anim, int parts, float frame, float speed, bool loop, int startFrame, int endFrame, int blendMs);
	void ReleaseAnim(int ent);
	bool CurrentFrame(int ent, int& anim, float& relFrame);	// legs animation and its frame
	std::vector<std::string> ListProfiles();				// _humanoid* folders with a .gla and animation.cfg
	std::string DefaultProfile(int ent);
	std::string CurrentProfile(int ent);
	bool CheckProfile(int ent, const std::string& profile, std::string& why);
	bool SetProfile(int ent, const std::string& profile, std::string& why, bool force = false);
	bool ViewRay(float sx, float sy, float dir[3]);			// virtual screen point -> world direction
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
