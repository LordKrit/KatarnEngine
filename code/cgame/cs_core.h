/*
JK BUILD: Cinematic Studio - engine-independent core (Scene Manager + Project Manager).

Holds the scene (actors, cameras), the cinematic clock, selection and the undo/redo stack, and
reads/writes projects. Everything that touches the game goes through cs::Host, so this file
builds and is unit-tested outside the engine (tests/cinematic_studio).
*/
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cs_json.h"

namespace cs
{
	constexpr int kProjectSchema = 2;	// 2: takes, prop, ghost (Phase 3)
	constexpr int kProjectSchemaOld = 1;	// bump when the file layout changes; readers accept <= this
	constexpr const char* kProjectRoot = "CinematicStudio/Projects";

	enum class ObjType { Actor, Camera };

	enum AnimParts { PARTS_LEGS = 1, PARTS_TORSO = 2, PARTS_BOTH = 3 };

	// An animation placed on an actor's timeline. The animation is named (BOTH_STAND1...), so a clip
	// survives a change of animation profile as long as the new skeleton has that animation.
	struct AnimClip
	{
		int64_t startUs = 0;		// scene time the clip starts
		int64_t lengthUs = 1'000'000;
		std::string anim;
		int parts = PARTS_BOTH;
		float speed = 1.0f;
		bool loop = false;			// loops until the next clip (else holds its last frame)
		int startFrame = 0;			// range inside the animation (relative frames)
		int endFrame = -1;			// -1 = last frame
		bool operator==(const AnimClip& o) const
		{
			return startUs == o.startUs && lengthUs == o.lengthUs && anim == o.anim && parts == o.parts &&
				speed == o.speed && loop == o.loop && startFrame == o.startFrame && endFrame == o.endFrame;
		}
	};
	const char* ObjTypeName(ObjType t);

	// ---- Phase 3: recorded performances ----
	// One sample of a performer: where it was, where it looked, and the exact frame of the legs and
	// torso animations (named through Take::anims, so a take plays on any skeleton that has them).
	struct TakeSample
	{
		int64_t tUs = 0;			// time into the take
		float origin[3] = { 0, 0, 0 };
		float yaw = 0, pitch = 0;	// view angles
		int legs = -1, torso = -1;	// index into Take::anims (-1 = none)
		float legsFrame = 0, torsoFrame = 0;	// relative frames
		int flags = 0;				// TAKE_SABER1 | TAKE_SABER2
		int weapon = 0;
		bool operator==(const TakeSample& o) const;
	};
	enum TakeFlags { TAKE_SABER1 = 1, TAKE_SABER2 = 2 };

	struct Take
	{
		int id = 0;
		std::string name;
		int64_t startUs = 0;		// scene time the take starts
		std::string profile;		// animation set the performer had (for information)
		std::string performer;		// NPC type / "player"
		std::vector<std::string> anims;
		std::vector<TakeSample> samples;	// sorted by tUs, first at 0

		int64_t LengthUs() const { return samples.empty() ? 0 : samples.back().tUs; }
		int AnimIndex(const std::string& name);	// adds it if new
		// the performance at a time into the take (clamped to the take): positions and angles are
		// interpolated; an animation frame is interpolated while the same animation keeps playing forward
		bool Evaluate(int64_t localUs, TakeSample& out) const;
		bool operator==(const Take& o) const;
	};

	struct Object
	{
		int id = 0;
		ObjType type = ObjType::Actor;
		std::string name;
		float origin[3] = { 0, 0, 0 };
		float angles[3] = { 0, 0, 0 };	// pitch yaw roll
		bool visible = true;

		// actor
		std::string npcType;	// NPC type from ext_data/npcs (e.g. md_vader)
		int entNum = -1;		// live entity, -1 = not in the world
		bool ai = false;		// false = cinematic (stands still), true = normal NPC AI
		bool adopted = false;	// an NPC that was already in the level (not spawned by the studio)
		std::string profile;	// animation profile (_humanoid_X); "" = the model's own
		bool profileForced = false;	// applied without the skeleton checks
		std::vector<AnimClip> clips;	// sorted by startUs
		bool prop = false;		// animated prop: no AI, no collision, no gravity; only the studio moves it
		bool ghost = false;		// drawn see-through (a guide while recording over it)
		std::vector<Take> takes;	// recorded performances
		int activeTake = 0;		// id of the take that drives the actor (0 = none)

		// camera
		float fov = 80.0f;
	};

	// Engine services the core needs. The game implements it (cs_host_game.cpp); the tests fake it.
	class Host
	{
	public:
		virtual ~Host() = default;
		// actors: returns the entity number or -1
		virtual int SpawnActor(const std::string& npcType, const float origin[3], const float angles[3], const std::string& name) = 0;
		virtual void RemoveActor(int ent) = 0;
		virtual bool ActorAlive(int ent) = 0;
		virtual bool GetActorTransform(int ent, float origin[3], float angles[3]) = 0;
		virtual void SetActorTransform(int ent, const float origin[3], const float angles[3]) = 0;
		virtual void SetActorAI(int ent, bool on) = 0;
		// animation profile ("" = the model's own); false + why if the skeletons don't match
		// force: skip the skeleton checks
		virtual bool SetActorProfile(int ent, const std::string& profile, std::string& why, bool force) = 0;
		virtual void SetActorProp(int ent, bool on) = 0;	// animated prop mode (see Object::prop)
		virtual std::string MapName() = 0;
		// files, relative to the game's write directory
		virtual bool WriteFile(const std::string& path, const std::string& data) = 0;
		virtual bool ReadFile(const std::string& path, std::string& out) = 0;
		virtual int64_t NowMs() = 0;
		virtual void Log(int level, const std::string& msg) = 0;	// 0 info, 1 warning, 2 error
	};

	// Cinematic clock: microseconds, independent of the render frame rate. Frames come from the project fps.
	class Clock
	{
	public:
		int64_t timeUs = 0;
		int64_t lengthUs = 10'000'000;	// scene length (playback stops or loops at the end)
		int fps = 30;					// 24, 30 or 60
		double speed = 1.0;
		bool playing = false;
		bool loop = false;

		void Advance(int64_t realUs);	// call every frame with the real elapsed time
		void Play() { if (timeUs >= lengthUs) timeUs = 0; playing = true; }
		void Pause() { playing = false; }
		void Stop() { playing = false; timeUs = 0; }
		void Step(int frames);			// pauses and moves by whole frames
		void SetFrame(int64_t frame);
		int64_t Frame() const;			// current frame number
		int64_t FrameCount() const;
		void SetFps(int f);
		std::string Timecode() const;	// HH:MM:SS:FF
	};

	struct Op
	{
		enum Kind { Add, Remove, Change } kind = Change;
		Object before, after;
		int64_t timeMs = 0;
		std::string tag;	// what kind of edit (only edits of the same kind coalesce)
	};

	class Scene
	{
	public:
		explicit Scene(Host* host) : host_(host) {}

		std::vector<Object> objects;
		Clock clock;
		int selected = 0;			// object id, 0 = none
		std::string project;		// current project name ("" = unsaved)
		bool dirty = false;

		Object* Find(int id);
		const Object* Find(int id) const;
		Object* FindByEntity(int ent);

		// editing (all undoable)
		int AddActor(const std::string& npcType, const float origin[3], const float angles[3]);
		int AdoptActor(int ent, const std::string& npcType, const std::string& name);
		int AddCamera(const float origin[3], const float angles[3], float fov);
		bool Remove(int id);
		// coalesce: merge into the previous change of the same object if it was < 1 s ago (nudging)
		bool SetTransform(int id, const float origin[3], const float angles[3], bool coalesce);
		bool SetAI(int id, bool on);
		bool Rename(int id, const std::string& name);
		bool SetProfile(int id, const std::string& profile, bool force = false);	// validated by the host unless forced
		bool SetNpcType(int id, const std::string& npcType);	// respawns the actor as another NPC
		int Duplicate(int id);									// copy beside it (actors and cameras)

		// animation clips (undoable); index into Object::clips, which stays sorted by start
		int AddClip(int id, const AnimClip& clip);
		bool SetClip(int id, int index, const AnimClip& clip, bool coalesce);	// returns the clip's new index via selectedClip
		bool RemoveClip(int id, int index);
		int selectedClip = -1;	// in the selected object

		// takes (undoable)
		int AddTake(int id, Take take, bool makeActive);	// returns the take id (0 = failed)
		bool SetTakeStart(int id, int takeId, int64_t startUs, bool coalesce);
		bool RenameTake(int id, int takeId, const std::string& name);
		bool SetActiveTake(int id, int takeId);				// 0 = none
		bool RemoveTake(int id, int takeId);
		bool SetProp(int id, bool on);
		bool SetGhost(int id, bool on);
		static const Take* ActiveTake(const Object& o);
		static Take* FindTake(Object& o, int takeId);
		int selectedTake = 0;	// take id in the selected object (0 = none)

		// The clip that drives an actor at scene time t: the latest one that started at or before t.
		// local = time into the clip; holding = past the end of a non-looping clip (show its last frame).
		static const AnimClip* ActiveClip(const Object& o, int64_t t, int64_t& localUs, bool& holding);
		void Clear();				// empties the scene (removes spawned actors), not undoable
		void Forget();				// empties the scene without touching the world (the level is gone)

		bool Undo();
		bool Redo();
		bool CanUndo() const { return !undo_.empty(); }
		bool CanRedo() const { return !redo_.empty(); }

		// pulls actor transforms from the world (actors with AI on move by themselves)
		void SyncFromWorld();

		// projects
		static bool ValidName(const std::string& name);
		std::string ProjectDir(const std::string& name) const { return std::string(kProjectRoot) + "/" + name; }
		bool Save(const std::string& name);
		bool Autosave();			// to <project>/autosave/ when dirty
		// force: load even if the project was made on another map
		bool Load(const std::string& name, bool force, bool fromAutosave, std::vector<std::string>& warnings);

		Json ToJson() const;
		std::string NewName(const std::string& base) const;

	private:
		void Push(const Op& op);
		bool Change(const Object& after, bool coalesce, const std::string& tag = "");	// undoable change of one object
		void Apply(const Object& state, bool exists);	// make the world + scene match a snapshot
		bool Spawn(Object& o);
		Host* host_;
		int nextId_ = 1;
		std::vector<Op> undo_, redo_;
	};
}
