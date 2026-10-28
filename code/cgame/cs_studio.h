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
		void SetActorProp(int ent, bool on) override;
		int SpawnProp(const std::string& classname, const std::vector<std::pair<std::string, std::string>>& kv,
			const float origin[3], const float angles[3]) override;
		void SetPropTransform(int ent, const float origin[3], const float angles[3]) override;
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

	// ---- Recorder (Phase 3) ----
	void SetHidden(int ent, bool hide);			// not drawn, not solid (an actor being re-recorded)
	void SetGhost(int ent, float alpha);		// drawn see-through (1 = solid)
	void SetSabers(int ent, int takeFlags);		// TAKE_SABER1/2 on or off
	bool SampleEntity(int ent, Take& take, TakeSample& out);	// a character's state now (adds animation names to the take)
	bool CharacterAlive(int ent);
	void SetTakeBody(int ent, const float origin[3], const float velocity[3], float yaw, float pitch);
	// speed = engine speed; startFrame / endFrame: a range of the animation (relative, -1 = to its end)
	bool PlayAnimAt(int ent, int anim, int parts, float frame, float speed, bool loop, int blendMs, int startFrame = 0, int endFrame = -1);
	bool LiveFrame(int ent, int parts, int anim, float& rel, int startFrame = 0, int endFrame = -1);
	void SetSolid(int ent, bool solid);
	// saber clashes between take-driven actors and any other blade (sparks, sound, flash); bounceLive:
	// knock back the attacks of characters the game drives (you, while recording). Returns how many.
	int DetectClashes(const std::vector<int>& takeEnts, bool bounceLive);

	// ---- Combat replay (cs_replay.cpp, Phase 5) ----
	struct CapturedEvent { WorldEvent ev; int actorEnt = -1, other2Ent = -1; int64_t absUs = 0; };
	void SetEventCapture(bool on);				// off also forgets what was captured
	bool EventCaptureOn();
	std::vector<CapturedEvent> TakeCapturedEvents();
	void PruneCapturedEvents(int64_t beforeUs);
	void SetCaptureSkip(const std::vector<int>& ents);	// the studio's playback actors (their events aren't captured)
	int64_t CaptureNowUs();						// the clock captured events and replay samples use
	void FireEvent(const WorldEvent& ev, int actorEnt, int other2Ent);
	void SetForceView(int ent, int forceBits, int takeFlags);	// until the frame is drawn

	// ---- Props (cs_props.cpp, Phase 7) ----
	std::vector<std::string> ListSpawnClasses();		// every class the game spawns + cs_model
	std::vector<std::string> ListModels(const std::string& dir);	// .md3 / .glm under dir (and one level down)
	bool PropBounds(int ent, float mins[3], float maxs[3]);
	bool PropAlive(int ent);
	bool AttachPose(int actorEnt, const std::string& bolt, const float offset[3], const float angOff[3], float org[3], float ang[3]);
	void SetPropRenderPose(int ent, const float org[3], const float ang[3]);	// drawn there this frame
	void ClearPropRenderPoses();
	void ForceState(int ent, int& forceBits, int& takeFlags);
	void SetLookTarget(int ent, int target);	// -1 = none			// a prop your saber can hit (still can't be hurt)
}

// face rig (cg_face.cpp), driven by the studio's face tracks (Phase 8)
bool CS_FaceHasRig(int ent);
void CS_FacePoses(int ent, std::vector<std::string>& out);
void CS_FaceExpr(int ent, const char* pose, float weight, int blendMs);	// pose "" / nullptr = none
void CS_FaceIdle(int ent, bool on, const char* profile);
void CS_FaceEyes(int ent, bool force, float pitch, float yaw);

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
void CS_DrawOverlay();					// over the HUD while it's closed (recording)
struct centity_s;
void CS_CaptureEvent(centity_s* cent);	// CG_CheckEvents / player state events, before CG_EntityEvent
void CS_EndFrame();						// after the frame is drawn
void CS_PropRenderPose(centity_s* cent);	// CG_CalcEntityLerpPositions: attached props
