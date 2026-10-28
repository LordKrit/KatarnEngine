/*
JK BUILD: Cinematic Studio - scene, clock, undo/redo and project files (see cs_core.h).
*/
#include "cs_core.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace cs
{
	const char* ObjTypeName(const ObjType t)
	{
		return t == ObjType::Camera ? "camera" : "actor";
	}

	// ------------------------------------------------------------------------------------------
	// Clock
	// ------------------------------------------------------------------------------------------

	void Clock::Advance(const int64_t realUs)
	{
		if (!playing) return;
		timeUs += static_cast<int64_t>(static_cast<double>(realUs) * speed);
		if (timeUs >= lengthUs)
		{
			if (loop && lengthUs > 0) timeUs %= lengthUs;
			else { timeUs = lengthUs; playing = false; }
		}
		if (timeUs < 0) { timeUs = 0; playing = false; }
	}

	int64_t Clock::Frame() const { return timeUs * fps / 1'000'000; }
	int64_t Clock::FrameCount() const { return lengthUs * fps / 1'000'000; }

	void Clock::SetFrame(int64_t frame)
	{
		frame = std::max<int64_t>(0, std::min(frame, FrameCount()));
		// first microsecond of the frame (rounded up, so Frame() gives back exactly `frame`)
		timeUs = std::min(lengthUs, (frame * 1'000'000 + fps - 1) / fps);
	}

	void Clock::Step(const int frames)
	{
		playing = false;
		SetFrame(Frame() + frames);
	}

	void Clock::SetFps(const int f)
	{
		if (f != 24 && f != 30 && f != 60) return;
		fps = f;	// time stays in microseconds, so changing fps never moves the playhead
	}

	std::string Clock::Timecode() const
	{
		const int64_t totalFrames = Frame();
		const int64_t ff = totalFrames % fps;
		const int64_t s = timeUs / 1'000'000;
		char b[32];
		snprintf(b, sizeof b, "%02d:%02d:%02d:%02d", static_cast<int>(s / 3600), static_cast<int>(s / 60 % 60),
			static_cast<int>(s % 60), static_cast<int>(ff));
		return b;
	}

	// ------------------------------------------------------------------------------------------
	// Scene editing
	// ------------------------------------------------------------------------------------------

	Object* Scene::Find(const int id)
	{
		for (auto& o : objects) if (o.id == id) return &o;
		return nullptr;
	}

	const Object* Scene::Find(const int id) const
	{
		for (auto& o : objects) if (o.id == id) return &o;
		return nullptr;
	}

	Object* Scene::FindByEntity(const int ent)
	{
		if (ent < 0) return nullptr;
		for (auto& o : objects) if (o.type == ObjType::Actor && o.entNum == ent) return &o;
		return nullptr;
	}

	std::string Scene::NewName(const std::string& base) const
	{
		for (int n = 1;; n++)
		{
			char b[160];
			snprintf(b, sizeof b, "%s %02d", base.c_str(), n);
			bool used = false;
			for (auto& o : objects) used = used || o.name == b;
			if (!used) return b;
		}
	}

	bool Scene::Spawn(Object& o)
	{
		o.entNum = host_->SpawnActor(o.npcType, o.origin, o.angles, o.name);
		if (o.entNum < 0)
		{
			host_->Log(1, "Couldn't spawn actor '" + o.name + "' (NPC type '" + o.npcType + "' missing?)");
			return false;
		}
		host_->SetActorAI(o.entNum, o.ai && !o.prop);
		if (o.prop) host_->SetActorProp(o.entNum, true);
		if (!o.profile.empty())
		{
			std::string why;
			if (!host_->SetActorProfile(o.entNum, o.profile, why, o.profileForced))
			{
				host_->Log(1, o.name + ": animation profile " + o.profile + " not applied (" + why + ")");
				o.profile.clear();
			}
		}
		return true;
	}

	void Scene::Push(const Op& op)
	{
		undo_.push_back(op);
		if (undo_.size() > 256) undo_.erase(undo_.begin());
		redo_.clear();
		dirty = true;
	}

	int Scene::AddActor(const std::string& npcType, const float origin[3], const float angles[3])
	{
		Object o;
		o.id = nextId_++;
		o.type = ObjType::Actor;
		o.npcType = npcType;
		o.name = NewName(npcType);
		std::copy(origin, origin + 3, o.origin);
		std::copy(angles, angles + 3, o.angles);
		if (!Spawn(o)) return 0;
		objects.push_back(o);
		Op op; op.kind = Op::Add; op.after = o; op.timeMs = host_->NowMs();
		Push(op);
		selected = o.id;
		return o.id;
	}

	int Scene::AdoptActor(const int ent, const std::string& npcType, const std::string& name)
	{
		if (FindByEntity(ent)) return FindByEntity(ent)->id;
		Object o;
		o.id = nextId_++;
		o.type = ObjType::Actor;
		o.npcType = npcType;
		o.name = name.empty() ? NewName(npcType) : name;
		o.entNum = ent;
		o.adopted = true;
		o.ai = true;
		host_->GetActorTransform(ent, o.origin, o.angles);
		objects.push_back(o);
		Op op; op.kind = Op::Add; op.after = o; op.timeMs = host_->NowMs();
		Push(op);
		selected = o.id;
		return o.id;
	}

	int Scene::AddCamera(const float origin[3], const float angles[3], const float fov)
	{
		Object o;
		o.id = nextId_++;
		o.type = ObjType::Camera;
		o.name = NewName("Camera");
		o.fov = fov;
		std::copy(origin, origin + 3, o.origin);
		std::copy(angles, angles + 3, o.angles);
		objects.push_back(o);
		Op op; op.kind = Op::Add; op.after = o; op.timeMs = host_->NowMs();
		Push(op);
		selected = o.id;
		return o.id;
	}

	bool Scene::Remove(const int id)
	{
		Object* o = Find(id);
		if (!o) return false;
		Op op; op.kind = Op::Remove; op.before = *o; op.timeMs = host_->NowMs();
		Apply(*o, false);
		Push(op);
		if (selected == id) selected = 0;
		return true;
	}

	bool Scene::SetTransform(const int id, const float origin[3], const float angles[3], const bool coalesce)
	{
		Object* o = Find(id);
		if (!o) return false;
		Object after = *o;
		std::copy(origin, origin + 3, after.origin);
		std::copy(angles, angles + 3, after.angles);
		const int64_t now = host_->NowMs();
		if (coalesce && !undo_.empty() && undo_.back().kind == Op::Change && undo_.back().after.id == id &&
			undo_.back().tag == "transform" && now - undo_.back().timeMs < 1000)
		{
			undo_.back().after = after;
			undo_.back().timeMs = now;
			redo_.clear();
			dirty = true;
		}
		else
		{
			Op op; op.kind = Op::Change; op.before = *o; op.after = after; op.timeMs = now; op.tag = "transform";
			Push(op);
		}
		Apply(after, true);
		return true;
	}

	bool Scene::SetAI(const int id, const bool on)
	{
		Object* o = Find(id);
		if (!o || o->type != ObjType::Actor) return false;
		Object after = *o;
		after.ai = on;
		Op op; op.kind = Op::Change; op.before = *o; op.after = after; op.timeMs = host_->NowMs();
		Push(op);
		Apply(after, true);
		return true;
	}

	bool Scene::Rename(const int id, const std::string& name)
	{
		Object* o = Find(id);
		if (!o || name.empty()) return false;
		Object after = *o;
		after.name = name;
		Op op; op.kind = Op::Change; op.before = *o; op.after = after; op.timeMs = host_->NowMs();
		Push(op);
		Apply(after, true);
		return true;
	}

	bool Scene::SetProfile(const int id, const std::string& profile, const bool force)
	{
		Object* o = Find(id);
		if (!o || o->type != ObjType::Actor) return false;
		if (o->profile == profile && o->profileForced == force) return true;
		if (o->entNum >= 0)
		{
			std::string why;
			if (!host_->SetActorProfile(o->entNum, profile, why, force))
			{
				host_->Log(2, "Can't use " + (profile.empty() ? std::string("the model's own profile") : profile) + " on " + o->name + ": " + why);
				return false;
			}
			if (!why.empty()) host_->Log(1, o->name + ": " + why);
		}
		Op op; op.kind = Op::Change; op.before = *o; op.timeMs = host_->NowMs();
		o->profile = profile;
		o->profileForced = force && !profile.empty();
		op.after = *o;
		Push(op);
		return true;
	}

	bool Scene::SetNpcType(const int id, const std::string& npcType)
	{
		Object* o = Find(id);
		if (!o || o->type != ObjType::Actor || npcType.empty() || o->npcType == npcType) return false;
		if (o->adopted)
		{
			host_->Log(1, o->name + " is a level character: duplicate it to make a studio actor of another type");
			return false;
		}
		Object after = *o;
		after.npcType = npcType;
		Op op; op.kind = Op::Change; op.before = *o; op.timeMs = host_->NowMs();
		Apply(after, true);
		o = Find(id);
		if (!o || o->entNum < 0)
		{
			// couldn't spawn the new type: go back to the old one
			if (o) Apply(op.before, true);
			return false;
		}
		op.after = *o;
		op.after.entNum = -1;
		Push(op);
		return true;
	}

	int Scene::Duplicate(const int id)
	{
		const Object* src = Find(id);
		if (!src) return 0;
		Object o = *src;
		o.id = nextId_++;
		std::string base = src->name;
		const size_t sp = base.find_last_of(' ');
		if (sp != std::string::npos && sp + 1 < base.size() && base.find_first_not_of("0123456789", sp + 1) == std::string::npos) base = base.substr(0, sp);
		o.name = NewName(base);
		o.origin[1] += 40.0f;
		o.entNum = -1;
		o.adopted = false;
		if (o.type == ObjType::Actor && o.npcType.empty()) return 0;
		if (o.type == ObjType::Actor && !Spawn(o)) return 0;
		objects.push_back(o);
		Op op; op.kind = Op::Add; op.after = o; op.timeMs = host_->NowMs();
		Push(op);
		selected = o.id;
		return o.id;
	}

	// ------------------------------------------------------------------------------------------
	// Takes (Phase 3)
	// ------------------------------------------------------------------------------------------

	bool TakeSample::operator==(const TakeSample& o) const
	{
		for (int i = 0; i < 3; i++) if (origin[i] != o.origin[i] || velocity[i] != o.velocity[i]) return false;
		return tUs == o.tUs && legsSpeed == o.legsSpeed && torsoSpeed == o.torsoSpeed &&
			yaw == o.yaw && pitch == o.pitch && legs == o.legs && torso == o.torso && legsFrame == o.legsFrame &&
			torsoFrame == o.torsoFrame && flags == o.flags && weapon == o.weapon;
	}

	bool Take::operator==(const Take& o) const
	{
		return id == o.id && name == o.name && startUs == o.startUs && profile == o.profile && performer == o.performer &&
			Anims() == o.Anims() && Samples() == o.Samples();
	}

	TakeData& Take::Edit()
	{
		if (data.use_count() > 1) data = std::make_shared<TakeData>(*data);
		return *data;
	}

	int Take::AnimIndex(const std::string& n)
	{
		if (n.empty()) return -1;
		const auto& an = data->anims;
		for (size_t i = 0; i < an.size(); i++) if (an[i] == n) return static_cast<int>(i);
		Edit().anims.push_back(n);
		return static_cast<int>(data->anims.size()) - 1;
	}

	static float LerpAngle(const float a, const float b, const float f)
	{
		float d = std::fmod(b - a, 360.0f);
		if (d > 180.0f) d -= 360.0f;
		if (d < -180.0f) d += 360.0f;
		return a + d * f;
	}

	bool Take::Evaluate(int64_t t, TakeSample& out) const
	{
		const auto& samples = data->samples;
		if (samples.empty()) return false;
		if (t <= samples.front().tUs) { out = samples.front(); out.tUs = t; return true; }
		if (t >= samples.back().tUs) { out = samples.back(); out.tUs = t; return true; }
		// last sample at or before t
		auto it = std::upper_bound(samples.begin(), samples.end(), t, [](const int64_t v, const TakeSample& s) { return v < s.tUs; });
		const TakeSample& a = *(it - 1);
		const TakeSample& b = *it;
		const float f = b.tUs > a.tUs ? static_cast<float>(static_cast<double>(t - a.tUs) / static_cast<double>(b.tUs - a.tUs)) : 0.0f;
		out = a;
		out.tUs = t;
		for (int i = 0; i < 3; i++)
		{
			out.origin[i] = a.origin[i] + (b.origin[i] - a.origin[i]) * f;
			out.velocity[i] = a.velocity[i] + (b.velocity[i] - a.velocity[i]) * f;
		}
		out.yaw = LerpAngle(a.yaw, b.yaw, f);
		out.pitch = LerpAngle(a.pitch, b.pitch, f);
		// the same animation playing forward: in between frames; a new animation or a loop: the earlier sample's
		if (a.legs == b.legs && b.legsFrame >= a.legsFrame) out.legsFrame = a.legsFrame + (b.legsFrame - a.legsFrame) * f;
		if (a.torso == b.torso && b.torsoFrame >= a.torsoFrame) out.torsoFrame = a.torsoFrame + (b.torsoFrame - a.torsoFrame) * f;
		return true;
	}

	const Take* Scene::ActiveTake(const Object& o)
	{
		if (!o.activeTake) return nullptr;
		for (auto& t : o.takes) if (t.id == o.activeTake) return &t;
		return nullptr;
	}

	Take* Scene::FindTake(Object& o, const int takeId)
	{
		for (auto& t : o.takes) if (t.id == takeId) return &t;
		return nullptr;
	}

	bool Scene::Change(const Object& after, const bool coalesce, const std::string& tag)
	{
		Object* o = Find(after.id);
		if (!o) return false;
		const int64_t now = host_->NowMs();
		if (coalesce && !undo_.empty() && undo_.back().kind == Op::Change && undo_.back().after.id == after.id &&
			undo_.back().tag == tag && now - undo_.back().timeMs < 1000)
		{
			undo_.back().after = after;
			undo_.back().timeMs = now;
			redo_.clear();
			dirty = true;
		}
		else
		{
			Op op; op.kind = Op::Change; op.before = *o; op.after = after; op.timeMs = now; op.tag = tag;
			Push(op);
		}
		Apply(after, true);
		return true;
	}

	int Scene::AddTake(const int id, Take take, const bool makeActive)
	{
		const Object* o = Find(id);
		if (!o || o->type != ObjType::Actor || take.Samples().size() < 2) return 0;
		Object after = *o;
		int top = 0;
		for (auto& t : after.takes) top = std::max(top, t.id);
		take.id = top + 1;
		if (take.name.empty()) { char b[32]; snprintf(b, sizeof b, "Take %d", take.id); take.name = b; }
		after.takes.push_back(take);
		if (makeActive || !after.activeTake) after.activeTake = take.id;
		after.prop = true;		// a performance is played back as an animated prop
		Change(after, false);
		selectedTake = take.id;
		return take.id;
	}

	bool Scene::SetTakeStart(const int id, const int takeId, const int64_t startUs, const bool coalesce)
	{
		const Object* o = Find(id);
		if (!o) return false;
		Object after = *o;
		Take* t = FindTake(after, takeId);
		if (!t) return false;
		t->startUs = std::max<int64_t>(0, startUs);
		return Change(after, coalesce, "take start " + std::to_string(takeId));
	}

	bool Scene::RenameTake(const int id, const int takeId, const std::string& name)
	{
		const Object* o = Find(id);
		if (!o || name.empty()) return false;
		Object after = *o;
		Take* t = FindTake(after, takeId);
		if (!t) return false;
		t->name = name;
		return Change(after, false);
	}

	bool Scene::SetActiveTake(const int id, const int takeId)
	{
		const Object* o = Find(id);
		if (!o) return false;
		Object after = *o;
		if (takeId && !FindTake(after, takeId)) return false;
		if (after.activeTake == takeId) return true;
		after.activeTake = takeId;
		return Change(after, false);
	}

	bool Scene::RemoveTake(const int id, const int takeId)
	{
		const Object* o = Find(id);
		if (!o) return false;
		Object after = *o;
		const size_t n = after.takes.size();
		after.takes.erase(std::remove_if(after.takes.begin(), after.takes.end(), [&](const Take& t) { return t.id == takeId; }), after.takes.end());
		if (after.takes.size() == n) return false;
		if (after.activeTake == takeId) after.activeTake = after.takes.empty() ? 0 : after.takes.back().id;
		if (selectedTake == takeId) selectedTake = 0;
		return Change(after, false);
	}

	bool Scene::SetProp(const int id, const bool on)
	{
		const Object* o = Find(id);
		if (!o || o->type != ObjType::Actor || o->prop == on) return false;
		Object after = *o;
		after.prop = on;
		return Change(after, false);
	}

	bool Scene::SetGhost(const int id, const bool on)
	{
		const Object* o = Find(id);
		if (!o || o->type != ObjType::Actor || o->ghost == on) return false;
		Object after = *o;
		after.ghost = on;
		return Change(after, false);
	}

	static void SortClips(std::vector<AnimClip>& c)
	{
		std::stable_sort(c.begin(), c.end(), [](const AnimClip& a, const AnimClip& b) { return a.startUs < b.startUs; });
	}

	static int IndexOf(const std::vector<AnimClip>& c, const AnimClip& k)
	{
		for (size_t i = 0; i < c.size(); i++) if (c[i] == k) return static_cast<int>(i);
		return -1;
	}

	int Scene::AddClip(const int id, const AnimClip& clip)
	{
		Object* o = Find(id);
		if (!o || o->type != ObjType::Actor || clip.anim.empty()) return -1;
		Op op; op.kind = Op::Change; op.before = *o; op.timeMs = host_->NowMs();
		AnimClip c = clip;
		c.startUs = std::max<int64_t>(0, c.startUs);
		c.lengthUs = std::max<int64_t>(1000, c.lengthUs);
		o->clips.push_back(c);
		SortClips(o->clips);
		op.after = *o;
		Push(op);
		selectedClip = IndexOf(o->clips, c);
		return selectedClip;
	}

	bool Scene::SetClip(const int id, const int index, const AnimClip& clip, const bool coalesce)
	{
		Object* o = Find(id);
		if (!o || index < 0 || index >= static_cast<int>(o->clips.size())) return false;
		AnimClip c = clip;
		c.startUs = std::max<int64_t>(0, c.startUs);
		c.lengthUs = std::max<int64_t>(1000, c.lengthUs);
		const int64_t now = host_->NowMs();
		const bool merge = coalesce && !undo_.empty() && undo_.back().kind == Op::Change && undo_.back().after.id == id &&
			undo_.back().tag == "clip" && now - undo_.back().timeMs < 1000;
		Op op; op.kind = Op::Change; op.before = *o; op.timeMs = now; op.tag = "clip";
		o->clips[static_cast<size_t>(index)] = c;
		SortClips(o->clips);
		if (merge) { undo_.back().after = *o; undo_.back().timeMs = now; redo_.clear(); dirty = true; }
		else { op.after = *o; Push(op); }
		selectedClip = IndexOf(o->clips, c);
		return true;
	}

	bool Scene::RemoveClip(const int id, const int index)
	{
		Object* o = Find(id);
		if (!o || index < 0 || index >= static_cast<int>(o->clips.size())) return false;
		Op op; op.kind = Op::Change; op.before = *o; op.timeMs = host_->NowMs();
		o->clips.erase(o->clips.begin() + index);
		op.after = *o;
		Push(op);
		selectedClip = -1;
		return true;
	}

	const AnimClip* Scene::ActiveClip(const Object& o, const int64_t t, int64_t& localUs, bool& holding)
	{
		const AnimClip* best = nullptr;
		for (auto& c : o.clips)
			if (c.startUs <= t && (!best || c.startUs >= best->startUs)) best = &c;
		holding = false;
		localUs = 0;
		if (!best) return nullptr;
		localUs = t - best->startUs;
		if (!best->loop && localUs >= best->lengthUs) { holding = true; localUs = best->lengthUs; }
		return best;
	}

	// Make the scene (and the world) hold `state` (exists) or not hold that object (!exists).
	void Scene::Apply(const Object& state, const bool exists)
	{
		Object* cur = Find(state.id);
		if (!exists)
		{
			if (!cur) return;
			if (cur->type == ObjType::Actor && cur->entNum >= 0)
			{
				// adopted level NPCs are only released (kept in the level), studio actors are removed
				if (!cur->adopted) host_->RemoveActor(cur->entNum);
			}
			objects.erase(std::remove_if(objects.begin(), objects.end(), [&](const Object& o) { return o.id == state.id; }), objects.end());
			return;
		}
		if (!cur)
		{
			Object o = state;
			if (o.type == ObjType::Actor && !(o.adopted && host_->ActorAlive(o.entNum)))
			{
				o.adopted = false;
				Spawn(o);
			}
			objects.push_back(o);
			nextId_ = std::max(nextId_, o.id + 1);
			return;
		}
		const int ent = cur->entNum;
		const bool aiChanged = cur->ai != state.ai || cur->prop != state.prop;
		const bool propChanged = cur->prop != state.prop;
		const bool typeChanged = cur->type == ObjType::Actor && cur->npcType != state.npcType;
		const bool profileChanged = cur->profile != state.profile || cur->profileForced != state.profileForced;
		*cur = state;
		cur->entNum = ent;		// the live entity, not the one in the snapshot
		if (cur->type != ObjType::Actor) return;
		if (typeChanged)
		{
			if (ent >= 0 && !cur->adopted) host_->RemoveActor(ent);
			cur->adopted = false;
			Spawn(*cur);		// also applies AI and the profile
			return;
		}
		if (cur->entNum >= 0 && host_->ActorAlive(cur->entNum))
		{
			host_->SetActorTransform(cur->entNum, cur->origin, cur->angles);
			if (propChanged) host_->SetActorProp(cur->entNum, cur->prop);
			if (aiChanged) host_->SetActorAI(cur->entNum, cur->ai && !cur->prop);
			if (profileChanged)
			{
				std::string why;
				if (!host_->SetActorProfile(cur->entNum, cur->profile, why, cur->profileForced))
					host_->Log(1, cur->name + ": animation profile not applied (" + why + ")");
			}
		}
	}

	bool Scene::Undo()
	{
		if (undo_.empty()) return false;
		const Op op = undo_.back();
		undo_.pop_back();
		switch (op.kind)
		{
		case Op::Add: Apply(op.after, false); break;
		case Op::Remove: Apply(op.before, true); break;
		case Op::Change: Apply(op.before, true); break;
		}
		redo_.push_back(op);
		dirty = true;
		return true;
	}

	bool Scene::Redo()
	{
		if (redo_.empty()) return false;
		const Op op = redo_.back();
		redo_.pop_back();
		switch (op.kind)
		{
		case Op::Add: Apply(op.after, true); break;
		case Op::Remove: Apply(op.before, false); break;
		case Op::Change: Apply(op.after, true); break;
		}
		undo_.push_back(op);
		dirty = true;
		return true;
	}

	bool Scene::Respawn(const int id)
	{
		Object* o = Find(id);
		if (!o || o->type != ObjType::Actor) return false;
		if (o->entNum >= 0 && !o->adopted) host_->RemoveActor(o->entNum);
		o->entNum = -1;
		o->adopted = false;
		return Spawn(*o);
	}

	void Scene::Clear()
	{
		for (auto& o : objects)
			if (o.type == ObjType::Actor && o.entNum >= 0 && !o.adopted) host_->RemoveActor(o.entNum);
		objects.clear();
		undo_.clear();
		redo_.clear();
		selected = 0;
		selectedClip = -1;
		selectedTake = 0;
		nextId_ = 1;
		dirty = false;
	}

	void Scene::Forget()
	{
		objects.clear();
		undo_.clear();
		redo_.clear();
		selected = 0;
		selectedClip = -1;
		nextId_ = 1;
		dirty = false;
		project.clear();
		clock.Stop();
	}

	void Scene::SyncFromWorld()
	{
		for (auto& o : objects)
		{
			if (o.type != ObjType::Actor || o.entNum < 0) continue;
			if (!host_->ActorAlive(o.entNum)) { o.entNum = -1; continue; }
			if (o.ai && !o.prop) host_->GetActorTransform(o.entNum, o.origin, o.angles);
		}
	}

	// ------------------------------------------------------------------------------------------
	// Projects
	// ------------------------------------------------------------------------------------------

	bool Scene::ValidName(const std::string& name)
	{
		if (name.empty() || name.size() > 48) return false;
		for (const char c : name)
		{
			const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
			if (!ok) return false;
		}
		return true;
	}

	static Json Vec(const float v[3])
	{
		Json a = Json::MakeArray();
		for (int i = 0; i < 3; i++) a.push(Json(static_cast<double>(v[i])));
		return a;
	}

	static void ReadVec(const Json& j, float v[3])
	{
		for (int i = 0; i < 3; i++) v[i] = static_cast<float>(j[static_cast<size_t>(i)].asNumber(v[i]));
	}

	Json Scene::ToJson() const
	{
		Json s = Json::MakeObject();
		s.set("schema", "jkbuild.cinematic.scene");
		s.set("version", kProjectSchema);
		s.set("fps", clock.fps);
		s.set("lengthUs", static_cast<double>(clock.lengthUs));
		s.set("loop", clock.loop);
		Json objs = Json::MakeArray();
		for (auto& o : objects)
		{
			Json j = Json::MakeObject();
			j.set("id", o.id);
			j.set("type", ObjTypeName(o.type));
			j.set("name", o.name);
			j.set("origin", Vec(o.origin));
			j.set("angles", Vec(o.angles));
			j.set("visible", o.visible);
			if (o.type == ObjType::Actor)
			{
				j.set("npc", o.npcType);
				j.set("ai", o.ai);
				j.set("adopted", o.adopted);
				if (!o.profile.empty()) j.set("profile", o.profile);
				if (o.profileForced) j.set("profileForced", true);
				Json clips = Json::MakeArray();
				for (auto& c : o.clips)
				{
					Json k = Json::MakeObject();
					k.set("startUs", static_cast<double>(c.startUs));
					k.set("lengthUs", static_cast<double>(c.lengthUs));
					k.set("anim", c.anim);
					k.set("parts", c.parts);
					k.set("speed", static_cast<double>(c.speed));
					k.set("loop", c.loop);
					k.set("startFrame", c.startFrame);
					k.set("endFrame", c.endFrame);
					clips.push(k);
				}
				j.set("clips", clips);
				if (o.prop) j.set("prop", true);
				if (o.ghost) j.set("ghost", true);
				if (o.activeTake) j.set("activeTake", o.activeTake);
				Json takes = Json::MakeArray();
				for (auto& t : o.takes)
				{
					Json k = Json::MakeObject();
					k.set("id", t.id);
					k.set("name", t.name);
					k.set("startUs", static_cast<double>(t.startUs));
					k.set("profile", t.profile);
					k.set("performer", t.performer);
					Json an = Json::MakeArray();
					for (auto& a : t.Anims()) an.push(a);
					k.set("anims", an);
					// samples flattened, kTakeFields numbers each, in the order of sampleFields
					Json sm = Json::MakeArray();
					for (auto& x : t.Samples())
					{
						const double v[kTakeFields] = { static_cast<double>(x.tUs), x.origin[0], x.origin[1], x.origin[2],
							x.velocity[0], x.velocity[1], x.velocity[2], x.yaw, x.pitch,
							static_cast<double>(x.legs), x.legsFrame, x.legsSpeed, static_cast<double>(x.torso), x.torsoFrame, x.torsoSpeed,
							static_cast<double>(x.flags), static_cast<double>(x.weapon) };
						for (const double d : v) sm.push(Json(d));
					}
					k.set("sampleFields", "tUs x y z vx vy vz yaw pitch legs legsFrame legsSpeed torso torsoFrame torsoSpeed flags weapon");
					k.set("samples", sm);
					takes.push(k);
				}
				if (!o.takes.empty()) j.set("takes", takes);
			}
			else j.set("fov", static_cast<double>(o.fov));
			objs.push(j);
		}
		s.set("objects", objs);
		return s;
	}

	bool Scene::Save(const std::string& name)
	{
		if (!ValidName(name))
		{
			host_->Log(2, "Project names may use letters, digits, '_' and '-' (up to 48 characters)");
			return false;
		}
		Json p = Json::MakeObject();
		p.set("schema", "jkbuild.cinematic.project");
		p.set("version", kProjectSchema);
		p.set("name", name);
		p.set("map", host_->MapName());
		Json scenes = Json::MakeArray();
		scenes.push("scenes/scene_01.json");
		p.set("scenes", scenes);
		Json assets = Json::MakeArray();		// what this project needs installed
		for (auto& o : objects)
			if (o.type == ObjType::Actor)
			{
				bool seen = false;
				for (size_t i = 0; i < assets.size(); i++) seen = seen || assets[i].asString() == "npc:" + o.npcType;
				if (!seen) assets.push("npc:" + o.npcType);
			}
		p.set("requires", assets);
		const std::string dir = ProjectDir(name);
		if (!host_->WriteFile(dir + "/project.json", p.dump()) || !host_->WriteFile(dir + "/scenes/scene_01.json", ToJson().dump()))
		{
			host_->Log(2, "Couldn't write project files in " + dir);
			return false;
		}
		project = name;
		dirty = false;
		host_->Log(0, "Saved project " + name);
		return true;
	}

	bool Scene::Autosave()
	{
		if (!dirty || project.empty()) return false;
		const std::string dir = ProjectDir(project) + "/autosave";
		return host_->WriteFile(dir + "/scene_01.json", ToJson().dump());	// dirty stays set: autosave isn't a save
	}

	bool Scene::Load(const std::string& name, const bool force, const bool fromAutosave, std::vector<std::string>& warnings)
	{
		if (!ValidName(name)) { host_->Log(2, "Bad project name '" + name + "'"); return false; }
		const std::string dir = ProjectDir(name);
		std::string text, err;
		Json p, s;
		if (!host_->ReadFile(dir + "/project.json", text)) { host_->Log(2, "No project called " + name); return false; }
		if (!Json::parse(text, p, &err)) { host_->Log(2, "project.json: " + err); return false; }
		if (p.get("schema").asString() != "jkbuild.cinematic.project" || p.get("version").asInt() > kProjectSchema)
		{
			host_->Log(2, name + " was made by a newer or different version of Cinematic Studio");
			return false;
		}
		const std::string map = p.get("map").asString();
		if (!force && !map.empty() && map != host_->MapName())
		{
			host_->Log(2, "Project " + name + " was made on map '" + map + "' (this is '" + host_->MapName() +
				"'). Load that map first, or use 'studio load " + name + " force'.");
			return false;
		}
		const std::string scenePath = fromAutosave ? dir + "/autosave/scene_01.json" : dir + "/" + p.get("scenes")[0].asString("scenes/scene_01.json");
		text.clear();
		if (!host_->ReadFile(scenePath, text)) { host_->Log(2, "Missing " + scenePath); return false; }
		if (!Json::parse(text, s, &err)) { host_->Log(2, scenePath + ": " + err); return false; }

		Clear();
		clock.Stop();
		clock.SetFps(s.get("fps").asInt(30));
		clock.lengthUs = static_cast<int64_t>(s.get("lengthUs").asNumber(10'000'000));
		clock.loop = s.get("loop").asBool(false);
		const Json& objs = s.get("objects");
		for (size_t i = 0; i < objs.size(); i++)
		{
			const Json& j = objs[i];
			Object o;
			o.id = j.get("id").asInt(nextId_);
			if (Find(o.id)) o.id = nextId_;
			o.type = j.get("type").asString() == "camera" ? ObjType::Camera : ObjType::Actor;
			o.name = j.get("name").asString(NewName(ObjTypeName(o.type)));
			ReadVec(j.get("origin"), o.origin);
			ReadVec(j.get("angles"), o.angles);
			o.visible = j.get("visible").asBool(true);
			if (o.type == ObjType::Camera) o.fov = static_cast<float>(j.get("fov").asNumber(80));
			else
			{
				o.npcType = j.get("npc").asString();
				o.ai = j.get("ai").asBool(false);
				o.profile = j.get("profile").asString();
				o.profileForced = j.get("profileForced").asBool(false);
				const Json& clips = j.get("clips");
				for (size_t k = 0; k < clips.size(); k++)
				{
					const Json& cj = clips[k];
					AnimClip c;
					c.startUs = static_cast<int64_t>(cj.get("startUs").asNumber(0));
					c.lengthUs = static_cast<int64_t>(cj.get("lengthUs").asNumber(1e6));
					c.anim = cj.get("anim").asString();
					c.parts = cj.get("parts").asInt(PARTS_BOTH);
					c.speed = static_cast<float>(cj.get("speed").asNumber(1));
					c.loop = cj.get("loop").asBool(false);
					c.startFrame = cj.get("startFrame").asInt(0);
					c.endFrame = cj.get("endFrame").asInt(-1);
					if (!c.anim.empty()) o.clips.push_back(c);
				}
				SortClips(o.clips);
				o.prop = j.get("prop").asBool(false);
				o.ghost = j.get("ghost").asBool(false);
				const Json& takes = j.get("takes");
				for (size_t k = 0; k < takes.size(); k++)
				{
					const Json& tj = takes[k];
					Take t;
					t.id = tj.get("id").asInt(static_cast<int>(k) + 1);
					t.name = tj.get("name").asString();
					t.startUs = static_cast<int64_t>(tj.get("startUs").asNumber(0));
					t.profile = tj.get("profile").asString();
					t.performer = tj.get("performer").asString();
					const Json& an = tj.get("anims");
					for (size_t q = 0; q < an.size(); q++) t.AnimIndex(an[q].asString());
					const Json& sm = tj.get("samples");
					// 12 fields: the first Phase 3 build (no velocity or speeds)
					const bool old12 = tj.get("sampleFields").asString() == "tUs x y z yaw pitch legs legsFrame torso torsoFrame flags weapon";
					const size_t kF = old12 ? 12 : kTakeFields;
					const int na = static_cast<int>(t.Anims().size());
					auto& out = t.Edit().samples;
					for (size_t b = 0; b + kF <= sm.size(); b += kF)
					{
						TakeSample x;
						const auto n = [&](const size_t i) { return static_cast<float>(sm[b + i].asNumber()); };
						x.tUs = static_cast<int64_t>(sm[b].asNumber());
						for (int i = 0; i < 3; i++) x.origin[i] = n(1 + i);
						size_t i = 4;
						if (!old12) { for (int v = 0; v < 3; v++) x.velocity[v] = n(i++); }
						x.yaw = n(i++);
						x.pitch = n(i++);
						x.legs = sm[b + i++].asInt(-1);
						x.legsFrame = n(i++);
						if (!old12) x.legsSpeed = n(i++);
						x.torso = sm[b + i++].asInt(-1);
						x.torsoFrame = n(i++);
						if (!old12) x.torsoSpeed = n(i++);
						x.flags = sm[b + i++].asInt(0);
						x.weapon = sm[b + i++].asInt(0);
						if (x.legs >= na) x.legs = -1;
						if (x.torso >= na) x.torso = -1;
						if (!out.empty() && x.tUs < out.back().tUs) continue;	// keep them sorted
						out.push_back(x);
					}
					if (old12)	// estimate the velocity from the positions
						for (size_t q = 0; q + 1 < out.size(); q++)
						{
							const double dt = static_cast<double>(out[q + 1].tUs - out[q].tUs) / 1e6;
							for (int v = 0; v < 3 && dt > 0; v++) out[q].velocity[v] = static_cast<float>((out[q + 1].origin[v] - out[q].origin[v]) / dt);
						}
					bool dup = false;
					for (auto& u : o.takes) dup = dup || u.id == t.id;
					if (t.Samples().size() >= 2 && !dup) o.takes.push_back(t);
					else warnings.push_back(o.name + ": take '" + t.name + "' is empty or damaged, skipped");
				}
				o.activeTake = j.get("activeTake").asInt(0);
				if (!ActiveTake(o)) o.activeTake = 0;
				// level NPCs that were adopted aren't in a fresh level by name: the studio respawns them
				o.adopted = false;
				if (o.npcType.empty()) { warnings.push_back(o.name + ": no NPC type"); continue; }
				if (!Spawn(o)) warnings.push_back(o.name + ": NPC type '" + o.npcType + "' not found (missing pk3?)");
			}
			objects.push_back(o);
			nextId_ = std::max(nextId_, o.id + 1);
		}
		project = name;
		dirty = fromAutosave;
		host_->Log(warnings.empty() ? 0 : 1, "Loaded project " + name + (fromAutosave ? " (autosave)" : "") +
			(warnings.empty() ? "" : " with " + std::to_string(warnings.size()) + " warning(s)"));
		for (auto& w : warnings) host_->Log(1, "  " + w);
		return true;
	}
}
