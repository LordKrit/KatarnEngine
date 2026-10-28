/*
JK BUILD: Cinematic Studio - engine-independent core (Scene Manager + Project Manager).

Holds the scene (actors, cameras), the cinematic clock, selection and the undo/redo stack, and
reads/writes projects. Everything that touches the game goes through cs::Host, so this file
builds and is unit-tested outside the engine (tests/cinematic_studio).
*/
#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "cs_json.h"

namespace cs
{
	constexpr int kProjectSchema = 2;	// 2: takes, prop, ghost (Phase 3)
	constexpr int kProjectSchemaOld = 1;	// bump when the file layout changes; readers accept <= this
	constexpr const char* kProjectRoot = "CinematicStudio/Projects";

	enum class ObjType { Actor, Camera, Events, Prop, Audio };

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
		int blendMs = 100;			// blend into this clip from what played before (Phase 4)
		int64_t animOffsetUs = 0;	// starts this far into its animation (the second half of a split clip)
		bool operator==(const AnimClip& o) const
		{
			return startUs == o.startUs && lengthUs == o.lengthUs && anim == o.anim && parts == o.parts &&
				speed == o.speed && loop == o.loop && startFrame == o.startFrame && endFrame == o.endFrame &&
				blendMs == o.blendMs && animOffsetUs == o.animOffsetUs;
		}
	};
	const char* ObjTypeName(ObjType t);

	// ---- Phase 3: recorded performances ----
	// One sample of a performer: where it was, how it moved and looked, and the exact frame and speed of
	// the legs and torso animations (named through the take's anims, so a take plays on any skeleton
	// that has them).
	struct TakeSample
	{
		int64_t tUs = 0;			// time into the take
		float origin[3] = { 0, 0, 0 };
		float velocity[3] = { 0, 0, 0 };	// the game turns and swings the legs from this
		float yaw = 0, pitch = 0;	// view angles
		int legs = -1, torso = -1;	// index into the take's anims (-1 = none)
		float legsFrame = 0, torsoFrame = 0;	// relative frames
		float legsSpeed = 1, torsoSpeed = 1;	// the engine's playback speed of each (frames per 50 ms)
		int flags = 0;				// TakeFlags
		int weapon = 0;
		int forceActive = 0;		// force powers in use (bits), shown on playback (Phase 5)
		bool operator==(const TakeSample& o) const;
	};
	enum TakeFlags
	{
		TAKE_SABER1 = 1, TAKE_SABER2 = 2, TAKE_LEGS_LOOP = 4, TAKE_TORSO_LOOP = 8,
		TAKE_SHOCKED = 16, TAKE_STUNNED = 32, TAKE_DRAINED = 64,	// on the receiving end of force lightning / stun / drain
	};
	// the numbers per sample in a project file, by name (files say which ones they have, in "sampleFields")
	extern const char* const kTakeFieldNames;

	// the recorded data; shared between copies of a take (undo snapshots copy takes a lot)
	struct TakeData
	{
		std::vector<std::string> anims;
		std::vector<TakeSample> samples;	// sorted by tUs, first at 0
	};

	struct Take
	{
		int id = 0;
		std::string name;
		int64_t startUs = 0;		// scene time the take starts
		std::string profile;		// animation set the performer had (for information)
		std::string performer;		// NPC type / "player"
		// Phase 4: trimmed off the start / end of the recording, and moved / turned as a whole
		// (turned about where the recording starts)
		int64_t trimInUs = 0, trimOutUs = 0;
		float offset[3] = { 0, 0, 0 };
		float yawOffset = 0;

		const std::vector<TakeSample>& Samples() const { return data->samples; }
		const std::vector<std::string>& Anims() const { return data->anims; }
		TakeData& Edit();				// the data, copied first if another take shares it
		void Add(const TakeSample& s) { Edit().samples.push_back(s); }
		int64_t RecordedUs() const { return data->samples.empty() ? 0 : data->samples.back().tUs; }
		int64_t LengthUs() const { return std::max<int64_t>(0, RecordedUs() - trimInUs - trimOutUs); }	// on the timeline
		int64_t EndUs() const { return startUs + LengthUs(); }
		int AnimIndex(const std::string& name);	// adds it if new
		// the performance at a time into the take (clamped to the take): positions, velocity and angles are
		// interpolated; an animation frame is interpolated while the same animation keeps playing forward
		bool Evaluate(int64_t localUs, TakeSample& out) const;	// localUs: time into the (trimmed) take; with offsets
		bool EvaluateRaw(int64_t recordedUs, TakeSample& out) const;	// the recording itself
		bool operator==(const Take& o) const;
	private:
		std::shared_ptr<TakeData> data = std::make_shared<TakeData>();
	};

	// ---- Phase 5: world events (effects, sounds, hits, voices...) recorded with a fight ----
	struct WorldEvent
	{
		int64_t tUs = 0;
		bool temp = true;			// a temporary event entity (effect, sound at a spot...); else an event on a character
		int event = 0, parm = 0;
		int actor = 0;				// object id of the character it rides on / refers to (0 = none)
		int other2 = 0;				// object id of a second character it refers to (0 = none)
		float origin[3] = { 0, 0, 0 }, pos3[3] = { 0, 0, 0 }, pos4[3] = { 0, 0, 0 };
		float angles[3] = { 0, 0, 0 }, angles2[3] = { 0, 0, 0 }, origin2[3] = { 0, 0, 0 };
		int weapon = 0, loopSound = 0, modelindex = 0, time = 0, time2 = 0;
		std::string fx, sound;		// effect / sound name when the event plays one (indexes differ between levels)
		bool operator==(const WorldEvent& o) const;
	};
	struct EventData { std::vector<WorldEvent> events; };	// sorted by tUs; shared like take data

	// ---- Phase 8: audio and faces ----
	// a sound on the timeline: on an actor, a voice line (with lip sync); on an audio track, music or a sound
	struct SoundClip
	{
		int64_t startUs = 0;
		int64_t lengthUs = 2'000'000;	// for the timeline (the sound plays to its end)
		std::string path;
		bool operator==(const SoundClip& o) const { return startUs == o.startUs && lengthUs == o.lengthUs && path == o.path; }
	};
	// an expression held for a while on an actor with a face rig
	struct FaceClip
	{
		int64_t startUs = 0;
		int64_t lengthUs = 1'000'000;
		std::string pose;			// a pose of the model's .face file (angry, smirk, blink...)
		float weight = 1.0f;
		int blendMs = 250;			// in and out
		bool operator==(const FaceClip& o) const
		{
			return startUs == o.startUs && lengthUs == o.lengthUs && pose == o.pose && weight == o.weight && blendMs == o.blendMs;
		}
	};
	// face idle (blinks, glances, moods) on or off from a time on
	struct IdleKey
	{
		int64_t tUs = 0;
		bool on = true;
		std::string profile;		// "" = the model's own
		bool operator==(const IdleKey& o) const { return tUs == o.tUs && on == o.on && profile == o.profile; }
	};
	// where the eyes (and head) look from a time on
	struct EyeKey
	{
		enum Mode { Free, LookAt, Aim } mode = Free;
		int64_t tUs = 0;
		int target = 0;				// LookAt: object id
		float pitch = 0, yaw = 0;	// Aim: eye angles (degrees), blended from the previous Aim key
		bool operator==(const EyeKey& o) const { return mode == o.mode && tUs == o.tUs && target == o.target && pitch == o.pitch && yaw == o.yaw; }
	};
	const FaceClip* ActiveFaceClip(const std::vector<FaceClip>& clips, int64_t t);	// started, not ended (latest)
	const IdleKey* IdleAt(const std::vector<IdleKey>& keys, int64_t t);				// latest at or before t
	// the eyes at t: mode / target from the latest key; Aim angles move linearly to the next Aim key
	bool EyesAt(const std::vector<EyeKey>& keys, int64_t t, EyeKey& out);

	// ---- Phase 4: keyframed position ----
	enum class Interp { Step, Linear, Smooth, CatmullRom };
	const char* InterpName(Interp i);
	struct TransformKey
	{
		int64_t tUs = 0;
		float origin[3] = { 0, 0, 0 };
		float yaw = 0;
		float pitch = 0, roll = 0, fov = 0;	// cameras (Phase 6)
		Interp interp = Interp::Smooth;	// how it moves from this key to the next
		// Smooth: the move flows through keys at a steady speed; ease slows it into (in) / out of (out)
		// this key, 1 = comes to a stop here. The first and last keys always start / end at rest.
		float easeIn = 0, easeOut = 0;
		bool operator==(const TransformKey& o) const
		{
			return tUs == o.tUs && origin[0] == o.origin[0] && origin[1] == o.origin[1] && origin[2] == o.origin[2] &&
				yaw == o.yaw && pitch == o.pitch && roll == o.roll && fov == o.fov && interp == o.interp &&
				easeIn == o.easeIn && easeOut == o.easeOut;
		}
	};
	// position and facing from keys at a time (holds the first / last key outside them); velocity in units/s
	bool EvaluateKeys(const std::vector<TransformKey>& keys, int64_t t, float origin[3], float& yaw, float velocity[3]);
	// a camera's keys: position, angles (pitch yaw roll) and fov
	bool EvaluateCameraKeys(const std::vector<TransformKey>& keys, int64_t t, float origin[3], float angles[3], float& fov);

	struct Object
	{
		int id = 0;
		ObjType type = ObjType::Actor;
		std::string name;
		float origin[3] = { 0, 0, 0 };
		float angles[3] = { 0, 0, 0 };	// pitch yaw roll
		bool visible = true;
		float scale = 1.0f;		// actors and props: drawn this much bigger / smaller

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
		std::vector<TransformKey> keys;	// sorted by tUs; move a prop when no take drives it

		// camera
		float fov = 80.0f;
		std::vector<int64_t> cuts;	// scene times the director cuts to this camera (sorted)
		int lookAt = 0;			// actor it aims at (0 = its own angles)
		float shake = 0;		// handheld shake, 0-10

		// audio / face (Phase 8): actors: voice lines + face tracks; audio objects: sounds
		std::vector<SoundClip> sounds;
		std::vector<FaceClip> faceClips;
		std::vector<IdleKey> idleKeys;
		std::vector<EyeKey> eyeKeys;
		bool music = false;			// audio object: its sounds are music (one at a time, the background track)

		// prop (ObjType::Prop, Phase 7): any map entity class with its key / values ("cs_model" = just a model)
		std::string classname;
		std::vector<std::pair<std::string, std::string>> kv;	// spawn keys (origin / angles come from the object)
		std::string Value(const std::string& key) const;
		int attachTo = 0;			// actor id it's held by / attached to (0 = free)
		std::string bolt;			// bone or tag on that actor ("*r_hand"...)
		float attachOffset[3] = { 0, 0, 0 }, attachAngles[3] = { 0, 0, 0 };

		// events track (ObjType::Events): starts at eventsStartUs on the scene clock
		int64_t eventsStartUs = 0;
		std::shared_ptr<EventData> events = std::make_shared<EventData>();
		const std::vector<WorldEvent>& Events() const { return events->events; }
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
		// map entities (Phase 7): returns the entity number or -1; moved with all three angles
		virtual int SpawnProp(const std::string& classname, const std::vector<std::pair<std::string, std::string>>& kv,
			const float origin[3], const float angles[3]) = 0;
		virtual void SetPropTransform(int ent, const float origin[3], const float angles[3]) = 0;
		virtual void SetEntityScale(int ent, float scale) = 0;	// how big it's drawn (1 = as made)
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
		int AddEvents(const std::string& name, int64_t startUs, std::vector<WorldEvent> events);	// a world-events track
		int AddAudio(const std::string& name, bool music);		// a music / sound track
		// any change to one object, undoable (timed lists are sorted first); tag: edits of the same kind coalesce
		bool Edit(Object after, bool coalesce, const std::string& tag = "");
		// props (undoable): a map entity of any class; changing its class or keys spawns it again
		int AddProp(const std::string& classname, const std::vector<std::pair<std::string, std::string>>& kv, const float origin[3], const float angles[3]);
		bool SetPropKey(int id, const std::string& key, const std::string& value);	// "" removes the key
		bool SetAttach(int id, int actorId, const std::string& bolt, const float offset[3], const float angles[3], bool coalesce);
		bool SetEventsStart(int id, int64_t startUs, bool coalesce);
		bool SetVisible(int id, bool on);	// events tracks: off = not played
		bool SetScale(int id, float scale, bool coalesce);	// actors and props, 0.05 - 20
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
		bool SetTake(int id, const Take& edited, bool coalesce);	// trims, offsets, name (same take id)
		int SplitTake(int id, int takeId, int64_t atUs);		// returns the second part's id
		int SplitClip(int id, int index, int64_t atUs);			// returns the second part's index
		// keys (undoable): one key per time; setting a key where one is replaces it
		bool SetKey(int id, const TransformKey& key, bool coalesce = false);
		bool MoveKey(int id, int64_t fromUs, int64_t toUs, bool coalesce);
		bool RemoveKey(int id, int64_t tUs);
		bool SetKeyInterp(int id, int64_t tUs, Interp interp);
		// replaces the keys from..to (inclusive) with `keys`, one undo step (a recorded camera move; empty = delete them)
		bool ReplaceKeys(int id, int64_t fromUs, int64_t toUs, const std::vector<TransformKey>& keys);
		static const Take* ActiveTake(const Object& o);
		// cameras (Phase 6): cuts, look-at, shake, fov (undoable)
		int ActiveCamera(int64_t t) const;					// the camera cut to at t (0 = none)
		std::vector<std::pair<int64_t, int>> Cuts() const;	// (time, camera id), sorted
		bool AddCut(int camId, int64_t t);					// one cut per time
		bool RemoveCut(int camId, int64_t t);
		bool MoveCut(int camId, int64_t from, int64_t to, bool coalesce);
		bool SetCameraLook(int camId, int targetId);
		bool SetCameraShake(int camId, float amount, bool coalesce);
		bool SetCameraFov(int camId, float fov, bool coalesce);
		static Take* FindTake(Object& o, int takeId);
		int selectedTake = 0;	// take id in the selected object (0 = none)

		// The clip that drives an actor at scene time t: the latest one that started at or before t.
		// local = time into the clip; holding = past the end of a non-looping clip (show its last frame).
		static const AnimClip* ActiveClip(const Object& o, int64_t t, int64_t& localUs, bool& holding);
		bool Respawn(int id);		// spawns the actor again (e.g. it died in a recorded fight); not undoable
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
