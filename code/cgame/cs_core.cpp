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
		switch (t)
		{
		case ObjType::Camera: return "camera";
		case ObjType::Events: return "events";
		case ObjType::Prop: return "prop";
		case ObjType::Audio: return "audio";
		default: return "actor";
		}
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

	std::string Object::Value(const std::string& key) const
	{
		for (auto& p : kv) if (p.first == key) return p.second;
		return "";
	}

	bool Scene::Spawn(Object& o)
	{
		if (o.type == ObjType::Prop)
		{
			o.entNum = host_->SpawnProp(o.classname, o.kv, o.origin, o.angles);
			if (o.entNum < 0) { host_->Log(1, "Couldn't spawn prop '" + o.name + "' (" + o.classname + ")"); return false; }
			return true;
		}
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

	int Scene::AddEvents(const std::string& name, const int64_t startUs, std::vector<WorldEvent> events)
	{
		if (events.empty()) return 0;
		std::stable_sort(events.begin(), events.end(), [](const WorldEvent& a, const WorldEvent& b) { return a.tUs < b.tUs; });
		Object o;
		o.id = nextId_++;
		o.type = ObjType::Events;
		o.name = NewName(name);
		o.eventsStartUs = std::max<int64_t>(0, startUs);
		o.events = std::make_shared<EventData>();
		o.events->events = std::move(events);
		objects.push_back(o);
		Op op; op.kind = Op::Add; op.after = o; op.timeMs = host_->NowMs();
		Push(op);
		return o.id;
	}

	int Scene::AddProp(const std::string& classname, const std::vector<std::pair<std::string, std::string>>& kv, const float origin[3], const float angles[3])
	{
		if (classname.empty()) return 0;
		Object o;
		o.id = nextId_++;
		o.type = ObjType::Prop;
		o.classname = classname;
		o.kv = kv;
		std::string base = classname;
		if (classname == "cs_model")
		{
			base = o.Value("model");
			const size_t sl = base.find_last_of('/'), dot = base.find_last_of('.');
			base = base.substr(sl == std::string::npos ? 0 : sl + 1, dot == std::string::npos || dot < sl + 1 ? std::string::npos : dot - (sl == std::string::npos ? 0 : sl + 1));
			if (base.empty()) base = "model";
		}
		o.name = NewName(base);
		std::copy(origin, origin + 3, o.origin);
		std::copy(angles, angles + 3, o.angles);
		if (!Spawn(o)) return 0;
		objects.push_back(o);
		Op op; op.kind = Op::Add; op.after = o; op.timeMs = host_->NowMs();
		Push(op);
		selected = o.id;
		return o.id;
	}

	bool Scene::SetPropKey(const int id, const std::string& key, const std::string& value)
	{
		const Object* o = Find(id);
		if (!o || o->type != ObjType::Prop || key.empty() || key == "classname") return false;
		Object after = *o;
		bool found = false;
		for (auto& p : after.kv) if (p.first == key) { p.second = value; found = true; }
		if (!found && !value.empty()) after.kv.emplace_back(key, value);
		after.kv.erase(std::remove_if(after.kv.begin(), after.kv.end(), [](const std::pair<std::string, std::string>& p) { return p.second.empty(); }), after.kv.end());
		if (after.kv == o->kv) return false;
		Op op; op.kind = Op::Change; op.before = *o; op.timeMs = host_->NowMs();
		Apply(after, true);
		const Object* n = Find(id);
		if (!n || n->entNum < 0)
		{
			Apply(op.before, true);	// the new keys don't spawn: keep the old ones
			host_->Log(1, "'" + key + "' = '" + value + "' doesn't spawn; kept the old keys");
			return false;
		}
		op.after = *n;
		op.after.entNum = -1;
		Push(op);
		return true;
	}

	bool Scene::SetAttach(const int id, const int actorId, const std::string& bolt, const float offset[3], const float angles[3], const bool coalesce)
	{
		const Object* o = Find(id);
		if (!o || o->type != ObjType::Prop) return false;
		const Object* a = actorId ? Find(actorId) : nullptr;
		if (actorId && (!a || a->type != ObjType::Actor)) return false;
		Object after = *o;
		after.attachTo = actorId;
		after.bolt = actorId ? bolt : "";
		std::copy(offset, offset + 3, after.attachOffset);
		std::copy(angles, angles + 3, after.attachAngles);
		return Change(after, coalesce, "attach");
	}

	int Scene::AddAudio(const std::string& name, const bool music)
	{
		Object o;
		o.id = nextId_++;
		o.type = ObjType::Audio;
		o.name = NewName(name.empty() ? (music ? "Music" : "Sounds") : name);
		o.music = music;
		objects.push_back(o);
		Op op; op.kind = Op::Add; op.after = o; op.timeMs = host_->NowMs();
		Push(op);
		selected = o.id;
		return o.id;
	}

	bool Scene::Edit(Object after, const bool coalesce, const std::string& tag)
	{
		const Object* o = Find(after.id);
		if (!o) return false;
		const auto byStart = [](const auto& a, const auto& b) { return a.startUs < b.startUs; };
		const auto byT = [](const auto& a, const auto& b) { return a.tUs < b.tUs; };
		std::stable_sort(after.sounds.begin(), after.sounds.end(), byStart);
		std::stable_sort(after.faceClips.begin(), after.faceClips.end(), byStart);
		std::stable_sort(after.idleKeys.begin(), after.idleKeys.end(), byT);
		std::stable_sort(after.eyeKeys.begin(), after.eyeKeys.end(), byT);
		for (auto& c : after.sounds) { c.startUs = std::max<int64_t>(0, c.startUs); c.lengthUs = std::max<int64_t>(50'000, c.lengthUs); }
		for (auto& c : after.faceClips)
		{
			c.startUs = std::max<int64_t>(0, c.startUs);
			c.lengthUs = std::max<int64_t>(50'000, c.lengthUs);
			c.weight = std::max(0.0f, std::min(1.0f, c.weight));
			c.blendMs = std::max(0, std::min(2000, c.blendMs));
		}
		after.entNum = o->entNum;
		return Change(after, coalesce, tag);
	}

	const FaceClip* ActiveFaceClip(const std::vector<FaceClip>& clips, const int64_t t)
	{
		const FaceClip* best = nullptr;
		for (auto& c : clips)
			if (c.startUs <= t && t < c.startUs + c.lengthUs && (!best || c.startUs >= best->startUs)) best = &c;
		return best;
	}

	const IdleKey* IdleAt(const std::vector<IdleKey>& keys, const int64_t t)
	{
		const IdleKey* best = nullptr;
		for (auto& k : keys) if (k.tUs <= t) best = &k;
		return best;
	}

	bool EyesAt(const std::vector<EyeKey>& keys, const int64_t t, EyeKey& out)
	{
		size_t i = keys.size();
		for (size_t k = 0; k < keys.size(); k++) if (keys[k].tUs <= t) i = k;
		if (i == keys.size()) return false;
		out = keys[i];
		if (out.mode == EyeKey::Aim && i + 1 < keys.size() && keys[i + 1].mode == EyeKey::Aim && keys[i + 1].tUs > keys[i].tUs)
		{
			const float f = static_cast<float>(static_cast<double>(t - keys[i].tUs) / static_cast<double>(keys[i + 1].tUs - keys[i].tUs));
			out.pitch += (keys[i + 1].pitch - out.pitch) * f;
			out.yaw += (keys[i + 1].yaw - out.yaw) * f;
		}
		out.tUs = t;
		return true;
	}

	bool Scene::SetEventsStart(const int id, const int64_t startUs, const bool coalesce)
	{
		const Object* o = Find(id);
		if (!o || o->type != ObjType::Events) return false;
		Object after = *o;
		after.eventsStartUs = std::max<int64_t>(0, startUs);
		return after.eventsStartUs == o->eventsStartUs || Change(after, coalesce, "events start");
	}

	bool Scene::SetVisible(const int id, const bool on)
	{
		const Object* o = Find(id);
		if (!o || o->visible == on) return false;
		Object after = *o;
		after.visible = on;
		return Change(after, false);
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
		if ((o.type == ObjType::Actor || o.type == ObjType::Prop) && !Spawn(o)) return 0;
		objects.push_back(o);
		Op op; op.kind = Op::Add; op.after = o; op.timeMs = host_->NowMs();
		Push(op);
		selected = o.id;
		return o.id;
	}

	// ------------------------------------------------------------------------------------------
	// Takes (Phase 3)
	// ------------------------------------------------------------------------------------------

	const char* const kTakeFieldNames =
		"tUs x y z vx vy vz yaw pitch legs legsFrame legsSpeed torso torsoFrame torsoSpeed flags weapon force";

	bool WorldEvent::operator==(const WorldEvent& o) const
	{
		for (int i = 0; i < 3; i++)
			if (origin[i] != o.origin[i] || pos3[i] != o.pos3[i] || pos4[i] != o.pos4[i] || angles[i] != o.angles[i] ||
				angles2[i] != o.angles2[i] || origin2[i] != o.origin2[i]) return false;
		return tUs == o.tUs && temp == o.temp && event == o.event && parm == o.parm && actor == o.actor && other2 == o.other2 &&
			weapon == o.weapon && loopSound == o.loopSound && modelindex == o.modelindex && time == o.time && time2 == o.time2 &&
			fx == o.fx && sound == o.sound;
	}

	bool TakeSample::operator==(const TakeSample& o) const
	{
		for (int i = 0; i < 3; i++) if (origin[i] != o.origin[i] || velocity[i] != o.velocity[i]) return false;
		return tUs == o.tUs && legsSpeed == o.legsSpeed && torsoSpeed == o.torsoSpeed &&
			yaw == o.yaw && pitch == o.pitch && legs == o.legs && torso == o.torso && legsFrame == o.legsFrame &&
			torsoFrame == o.torsoFrame && flags == o.flags && weapon == o.weapon && forceActive == o.forceActive;
	}

	bool Take::operator==(const Take& o) const
	{
		return id == o.id && name == o.name && startUs == o.startUs && profile == o.profile && performer == o.performer &&
			trimInUs == o.trimInUs && trimOutUs == o.trimOutUs && offset[0] == o.offset[0] && offset[1] == o.offset[1] &&
			offset[2] == o.offset[2] && yawOffset == o.yawOffset && Anims() == o.Anims() && Samples() == o.Samples();
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

	bool Take::Evaluate(const int64_t localUs, TakeSample& out) const
	{
		const int64_t local = std::max<int64_t>(0, std::min(localUs, LengthUs()));
		if (!EvaluateRaw(local + trimInUs, out)) return false;
		out.tUs = localUs;
		if (offset[0] != 0 || offset[1] != 0 || offset[2] != 0 || yawOffset != 0)
		{
			// turned about where the recording starts, then moved
			const float* pivot = data->samples.front().origin;
			const double r = static_cast<double>(yawOffset) * 3.14159265358979323846 / 180.0;
			const float c = static_cast<float>(std::cos(r)), sn = static_cast<float>(std::sin(r));
			const float dx = out.origin[0] - pivot[0], dy = out.origin[1] - pivot[1];
			out.origin[0] = pivot[0] + dx * c - dy * sn + offset[0];
			out.origin[1] = pivot[1] + dx * sn + dy * c + offset[1];
			out.origin[2] += offset[2];
			const float vx = out.velocity[0], vy = out.velocity[1];
			out.velocity[0] = vx * c - vy * sn;
			out.velocity[1] = vx * sn + vy * c;
			out.yaw += yawOffset;
		}
		return true;
	}

	bool Take::EvaluateRaw(int64_t t, TakeSample& out) const
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

	// ------------------------------------------------------------------------------------------
	// Phase 4: take / clip editing, keys
	// ------------------------------------------------------------------------------------------

	const char* InterpName(const Interp i)
	{
		switch (i)
		{
		case Interp::Step: return "step";
		case Interp::Linear: return "linear";
		case Interp::CatmullRom: return "curve";
		default: return "smooth";
		}
	}

	static Interp InterpFromName(const std::string& n)
	{
		if (n == "step") return Interp::Step;
		if (n == "linear") return Interp::Linear;
		if (n == "curve") return Interp::CatmullRom;
		return Interp::Smooth;
	}

	static float Unwrap(const float from, float to)
	{
		while (to - from > 180.0f) to -= 360.0f;
		while (to - from < -180.0f) to += 360.0f;
		return to;
	}

	// channels of a key: x y z pitch yaw roll fov (3-5 are angles)
	static float Ch(const TransformKey& k, const int c)
	{
		switch (c)
		{
		case 0: case 1: case 2: return k.origin[c];
		case 3: return k.pitch;
		case 4: return k.yaw;
		case 5: return k.roll;
		default: return k.fov;
		}
	}

	// segment i -> i+1 at t (0..1), in the interpolation of key i; out: the 7 channels
	static void KeyChannels(const std::vector<TransformKey>& k, const int i, const float t, float out[7])
	{
		const int n = static_cast<int>(k.size());
		const TransformKey& a = k[static_cast<size_t>(i)];
		const TransformKey& b = k[static_cast<size_t>(std::min(i + 1, n - 1))];
		const TransformKey& p0 = k[static_cast<size_t>(std::max(i - 1, 0))];
		const TransformKey& p3 = k[static_cast<size_t>(std::min(i + 2, n - 1))];
		float f = t;
		if (a.interp == Interp::Step) f = 0;
		else if (a.interp == Interp::Smooth) f = t * t * (3 - 2 * t);
		const float t2 = t * t, t3 = t2 * t;
		for (int c = 0; c < 7; c++)
		{
			const bool ang = c >= 3 && c <= 5;
			const float va = Ch(a, c);
			const float vb = ang ? Unwrap(va, Ch(b, c)) : Ch(b, c);
			if (a.interp == Interp::CatmullRom)
			{
				const float v0 = ang ? Unwrap(va, Ch(p0, c)) : Ch(p0, c);
				const float v3 = ang ? Unwrap(vb, Ch(p3, c)) : Ch(p3, c);
				out[c] = 0.5f * (2 * va + (-v0 + vb) * t + (2 * v0 - 5 * va + 4 * vb - v3) * t2 + (-v0 + 3 * va - 3 * vb + v3) * t3);
			}
			else out[c] = va + (vb - va) * f;
		}
	}

	static void KeyPos(const std::vector<TransformKey>& k, const int i, const float t, float out[3], float& yaw)
	{
		float c[7];
		KeyChannels(k, i, t, c);
		for (int j = 0; j < 3; j++) out[j] = c[j];
		yaw = c[4];
	}

	bool EvaluateCameraKeys(const std::vector<TransformKey>& k, const int64_t t, float origin[3], float angles[3], float& fov)
	{
		if (k.empty()) return false;
		float c[7];
		if (t <= k.front().tUs || k.size() == 1) for (int j = 0; j < 7; j++) c[j] = Ch(k.front(), j);
		else if (t >= k.back().tUs) for (int j = 0; j < 7; j++) c[j] = Ch(k.back(), j);
		else
		{
			size_t i = 0;
			while (i + 1 < k.size() && k[i + 1].tUs <= t) i++;
			KeyChannels(k, static_cast<int>(i), static_cast<float>(static_cast<double>(t - k[i].tUs) / static_cast<double>(k[i + 1].tUs - k[i].tUs)), c);
		}
		for (int j = 0; j < 3; j++) { origin[j] = c[j]; angles[j] = c[3 + j]; }
		fov = c[6];
		return true;
	}

	int Scene::ActiveCamera(const int64_t t) const
	{
		int best = 0;
		int64_t at = -1;
		for (auto& o : objects)
			if (o.type == ObjType::Camera)
				for (const int64_t c : o.cuts)
					if (c <= t && c > at) { at = c; best = o.id; }
		return best;
	}

	std::vector<std::pair<int64_t, int>> Scene::Cuts() const
	{
		std::vector<std::pair<int64_t, int>> out;
		for (auto& o : objects)
			if (o.type == ObjType::Camera) for (const int64_t c : o.cuts) out.emplace_back(c, o.id);
		std::sort(out.begin(), out.end());
		return out;
	}

	bool Scene::AddCut(const int camId, int64_t t)
	{
		t = std::max<int64_t>(0, t);
		// one cut per time: a cut to another camera at the same time moves here
		for (auto& o : objects)
			if (o.type == ObjType::Camera && o.id != camId && std::find(o.cuts.begin(), o.cuts.end(), t) != o.cuts.end())
				RemoveCut(o.id, t);
		const Object* o = Find(camId);
		if (!o || o->type != ObjType::Camera || std::find(o->cuts.begin(), o->cuts.end(), t) != o->cuts.end()) return false;
		Object after = *o;
		after.cuts.push_back(t);
		std::sort(after.cuts.begin(), after.cuts.end());
		return Change(after, false);
	}

	bool Scene::RemoveCut(const int camId, const int64_t t)
	{
		const Object* o = Find(camId);
		if (!o) return false;
		Object after = *o;
		const auto it = std::find(after.cuts.begin(), after.cuts.end(), t);
		if (it == after.cuts.end()) return false;
		after.cuts.erase(it);
		return Change(after, false);
	}

	bool Scene::MoveCut(const int camId, const int64_t from, int64_t to, const bool coalesce)
	{
		to = std::max<int64_t>(0, to);
		const Object* o = Find(camId);
		if (!o) return false;
		for (auto& c : objects)
			if (c.type == ObjType::Camera && std::find(c.cuts.begin(), c.cuts.end(), to) != c.cuts.end()) return false;
		Object after = *o;
		const auto it = std::find(after.cuts.begin(), after.cuts.end(), from);
		if (it == after.cuts.end()) return false;
		*it = to;
		std::sort(after.cuts.begin(), after.cuts.end());
		return Change(after, coalesce, "cut move");
	}

	bool Scene::SetCameraLook(const int camId, const int targetId)
	{
		const Object* o = Find(camId);
		if (!o || o->type != ObjType::Camera || o->lookAt == targetId) return false;
		Object after = *o;
		after.lookAt = targetId;
		return Change(after, false);
	}

	bool Scene::SetCameraShake(const int camId, const float amount, const bool coalesce)
	{
		const Object* o = Find(camId);
		if (!o || o->type != ObjType::Camera) return false;
		Object after = *o;
		after.shake = std::max(0.0f, std::min(10.0f, amount));
		return after.shake == o->shake || Change(after, coalesce, "shake");
	}

	bool Scene::SetCameraFov(const int camId, const float fov, const bool coalesce)
	{
		const Object* o = Find(camId);
		if (!o || o->type != ObjType::Camera) return false;
		Object after = *o;
		after.fov = std::max(5.0f, std::min(150.0f, fov));
		return after.fov == o->fov || Change(after, coalesce, "fov");
	}

	bool EvaluateKeys(const std::vector<TransformKey>& k, const int64_t t, float origin[3], float& yaw, float velocity[3])
	{
		for (int j = 0; j < 3; j++) velocity[j] = 0;
		if (k.empty()) return false;
		if (t <= k.front().tUs) { std::copy(k.front().origin, k.front().origin + 3, origin); yaw = k.front().yaw; return true; }
		if (t >= k.back().tUs) { std::copy(k.back().origin, k.back().origin + 3, origin); yaw = k.back().yaw; return true; }
		size_t i = 0;
		while (i + 1 < k.size() && k[i + 1].tUs <= t) i++;
		const double span = static_cast<double>(k[i + 1].tUs - k[i].tUs);
		const float f = static_cast<float>(static_cast<double>(t - k[i].tUs) / span);
		KeyPos(k, static_cast<int>(i), f, origin, yaw);
		// velocity from a small step (the game swings the legs from it)
		const float df = static_cast<float>(std::min(0.02, 10'000.0 / span));
		float o2[3], y2;
		KeyPos(k, static_cast<int>(i), std::min(1.0f, f + df), o2, y2);
		const float dt = static_cast<float>(span / 1e6) * (std::min(1.0f, f + df) - f);
		if (dt > 0) for (int j = 0; j < 3; j++) velocity[j] = (o2[j] - origin[j]) / dt;
		return true;
	}

	bool Scene::SetTake(const int id, const Take& edited, const bool coalesce)
	{
		const Object* o = Find(id);
		if (!o) return false;
		Object after = *o;
		Take* t = FindTake(after, edited.id);
		if (!t) return false;
		Take e = edited;
		e.startUs = std::max<int64_t>(0, e.startUs);
		e.trimInUs = std::max<int64_t>(0, std::min(e.trimInUs, e.RecordedUs() - 1000));
		e.trimOutUs = std::max<int64_t>(0, std::min(e.trimOutUs, e.RecordedUs() - e.trimInUs - 1000));
		if (e == *t) return true;
		*t = e;
		return Change(after, coalesce, "take " + std::to_string(e.id));
	}

	int Scene::SplitTake(const int id, const int takeId, const int64_t atUs)
	{
		const Object* o = Find(id);
		if (!o) return 0;
		Object after = *o;
		Take* t = FindTake(after, takeId);
		if (!t || atUs <= t->startUs + 1000 || atUs >= t->EndUs() - 1000) return 0;
		Take second = *t;		// shares the recording
		const int64_t cut = atUs - t->startUs;
		t->trimOutUs += t->LengthUs() - cut;
		second.trimInUs += cut;
		second.startUs = atUs;
		int top = 0;
		for (auto& k : after.takes) top = std::max(top, k.id);
		second.id = top + 1;
		second.name = t->name + " b";
		after.takes.push_back(second);
		Change(after, false);
		selectedTake = second.id;
		return second.id;
	}

	int Scene::SplitClip(const int id, const int index, const int64_t atUs)
	{
		const Object* o = Find(id);
		if (!o || index < 0 || index >= static_cast<int>(o->clips.size())) return -1;
		Object after = *o;
		AnimClip& c = after.clips[static_cast<size_t>(index)];
		if (atUs <= c.startUs + 1000 || atUs >= c.startUs + c.lengthUs - 1000) return -1;
		AnimClip second = c;
		const int64_t cut = atUs - c.startUs;
		c.lengthUs = cut;
		second.startUs = atUs;
		second.lengthUs -= cut;
		second.animOffsetUs += cut;
		second.blendMs = 0;		// it carries straight on
		after.clips.push_back(second);
		std::stable_sort(after.clips.begin(), after.clips.end(), [](const AnimClip& a, const AnimClip& b) { return a.startUs < b.startUs; });
		Change(after, false);
		const Object* n = Find(id);
		for (size_t i = 0; n && i < n->clips.size(); i++)
			if (n->clips[i] == second) { selectedClip = static_cast<int>(i); return selectedClip; }
		return -1;
	}

	static void SortKeys(std::vector<TransformKey>& k)
	{
		std::stable_sort(k.begin(), k.end(), [](const TransformKey& a, const TransformKey& b) { return a.tUs < b.tUs; });
	}

	bool Scene::SetKey(const int id, const TransformKey& key, const bool coalesce)
	{
		const Object* o = Find(id);
		if (!o || (o->type != ObjType::Actor && o->type != ObjType::Camera && o->type != ObjType::Prop)) return false;
		Object after = *o;
		TransformKey k = key;
		k.tUs = std::max<int64_t>(0, k.tUs);
		bool replaced = false;
		for (auto& e : after.keys) if (e.tUs == k.tUs) { e = k; replaced = true; }
		if (!replaced) after.keys.push_back(k);
		SortKeys(after.keys);
		if (after.type == ObjType::Actor) after.prop = true;	// keys move a prop
		return Change(after, coalesce, "key " + std::to_string(k.tUs));
	}

	bool Scene::MoveKey(const int id, const int64_t fromUs, int64_t toUs, const bool coalesce)
	{
		const Object* o = Find(id);
		if (!o) return false;
		toUs = std::max<int64_t>(0, toUs);
		Object after = *o;
		TransformKey* k = nullptr;
		for (auto& e : after.keys) { if (e.tUs == fromUs) k = &e; else if (e.tUs == toUs) return false; }	// one key per time
		if (!k) return false;
		k->tUs = toUs;
		SortKeys(after.keys);
		return Change(after, coalesce, "key move");
	}

	bool Scene::RemoveKey(const int id, const int64_t tUs)
	{
		const Object* o = Find(id);
		if (!o) return false;
		Object after = *o;
		const size_t n = after.keys.size();
		after.keys.erase(std::remove_if(after.keys.begin(), after.keys.end(), [&](const TransformKey& k) { return k.tUs == tUs; }), after.keys.end());
		return after.keys.size() != n && Change(after, false);
	}

	bool Scene::ReplaceKeys(const int id, const int64_t fromUs, const int64_t toUs, const std::vector<TransformKey>& keys)
	{
		const Object* o = Find(id);
		if (!o || (o->type != ObjType::Actor && o->type != ObjType::Camera && o->type != ObjType::Prop)) return false;
		Object after = *o;
		after.keys.erase(std::remove_if(after.keys.begin(), after.keys.end(), [&](const TransformKey& k) { return k.tUs >= fromUs && k.tUs <= toUs; }), after.keys.end());
		for (auto k : keys)
		{
			k.tUs = std::max<int64_t>(0, k.tUs);
			bool dup = false;
			for (auto& e : after.keys) if (e.tUs == k.tUs) { e = k; dup = true; }
			if (!dup) after.keys.push_back(k);
		}
		SortKeys(after.keys);
		if (after.keys == o->keys) return false;
		if (after.type == ObjType::Actor && !after.keys.empty()) after.prop = true;
		return Change(after, false);
	}

	bool Scene::SetKeyInterp(const int id, const int64_t tUs, const Interp interp)
	{
		const Object* o = Find(id);
		if (!o) return false;
		Object after = *o;
		for (auto& k : after.keys)
			if (k.tUs == tUs) { if (k.interp == interp) return true; k.interp = interp; return Change(after, false); }
		return false;
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
			if (cur->type == ObjType::Prop && cur->entNum >= 0) host_->RemoveActor(cur->entNum);
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
			if (o.type == ObjType::Prop) Spawn(o);
			else if (o.type == ObjType::Actor && !(o.adopted && host_->ActorAlive(o.entNum)))
			{
				o.adopted = false;
				Spawn(o);
			}
			objects.push_back(o);
			nextId_ = std::max(nextId_, o.id + 1);
			return;
		}
		if (cur->type == ObjType::Prop)
		{
			const bool respawn = cur->classname != state.classname || cur->kv != state.kv || cur->entNum < 0;
			const int ent = cur->entNum;
			*cur = state;
			cur->entNum = ent;
			if (respawn)
			{
				if (ent >= 0) host_->RemoveActor(ent);
				cur->entNum = -1;
				Spawn(*cur);
			}
			else host_->SetPropTransform(cur->entNum, cur->origin, cur->angles);
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
			if ((o.type == ObjType::Actor && !o.adopted) || o.type == ObjType::Prop) { if (o.entNum >= 0) host_->RemoveActor(o.entNum); }
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

	static Json SoundsToJson(const std::vector<SoundClip>& sounds)
	{
		Json arr = Json::MakeArray();
		for (auto& c : sounds)
		{
			Json k = Json::MakeObject();
			k.set("startUs", static_cast<double>(c.startUs));
			k.set("lengthUs", static_cast<double>(c.lengthUs));
			k.set("path", c.path);
			arr.push(k);
		}
		return arr;
	}

	static void SoundsFromJson(const Json& j, std::vector<SoundClip>& out)
	{
		for (size_t i = 0; i < j.size(); i++)
		{
			SoundClip c;
			c.startUs = static_cast<int64_t>(j[i].get("startUs").asNumber(0));
			c.lengthUs = static_cast<int64_t>(j[i].get("lengthUs").asNumber(2e6));
			c.path = j[i].get("path").asString();
			if (!c.path.empty()) out.push_back(c);
		}
		std::stable_sort(out.begin(), out.end(), [](const SoundClip& a, const SoundClip& b) { return a.startUs < b.startUs; });
	}

	static Json KeysToJson(const std::vector<TransformKey>& keys)
	{
		Json arr = Json::MakeArray();
		for (auto& k : keys)
		{
			Json kj = Json::MakeObject();
			kj.set("tUs", static_cast<double>(k.tUs));
			kj.set("origin", Vec(k.origin));
			kj.set("yaw", static_cast<double>(k.yaw));
			if (k.pitch != 0) kj.set("pitch", static_cast<double>(k.pitch));
			if (k.roll != 0) kj.set("roll", static_cast<double>(k.roll));
			if (k.fov != 0) kj.set("fov", static_cast<double>(k.fov));
			kj.set("interp", InterpName(k.interp));
			arr.push(kj);
		}
		return arr;
	}

	static void KeysFromJson(const Json& keys, std::vector<TransformKey>& out)
	{
		for (size_t k = 0; k < keys.size(); k++)
		{
			TransformKey key;
			key.tUs = static_cast<int64_t>(keys[k].get("tUs").asNumber(0));
			ReadVec(keys[k].get("origin"), key.origin);
			key.yaw = static_cast<float>(keys[k].get("yaw").asNumber(0));
			key.pitch = static_cast<float>(keys[k].get("pitch").asNumber(0));
			key.roll = static_cast<float>(keys[k].get("roll").asNumber(0));
			key.fov = static_cast<float>(keys[k].get("fov").asNumber(0));
			key.interp = InterpFromName(keys[k].get("interp").asString());
			bool dup = false;
			for (auto& e : out) dup = dup || e.tUs == key.tUs;
			if (!dup) out.push_back(key);
		}
		SortKeys(out);
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
					k.set("blendMs", c.blendMs);
					if (c.animOffsetUs) k.set("animOffsetUs", static_cast<double>(c.animOffsetUs));
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
					if (t.trimInUs) k.set("trimInUs", static_cast<double>(t.trimInUs));
					if (t.trimOutUs) k.set("trimOutUs", static_cast<double>(t.trimOutUs));
					if (t.offset[0] != 0 || t.offset[1] != 0 || t.offset[2] != 0) k.set("offset", Vec(t.offset));
					if (t.yawOffset != 0) k.set("yawOffset", static_cast<double>(t.yawOffset));
					Json an = Json::MakeArray();
					for (auto& a : t.Anims()) an.push(a);
					k.set("anims", an);
					// samples flattened, in the order of sampleFields
					Json sm = Json::MakeArray();
					for (auto& x : t.Samples())
					{
						const double v[] = { static_cast<double>(x.tUs), x.origin[0], x.origin[1], x.origin[2],
							x.velocity[0], x.velocity[1], x.velocity[2], x.yaw, x.pitch,
							static_cast<double>(x.legs), x.legsFrame, x.legsSpeed, static_cast<double>(x.torso), x.torsoFrame, x.torsoSpeed,
							static_cast<double>(x.flags), static_cast<double>(x.weapon), static_cast<double>(x.forceActive) };
						for (const double d : v) sm.push(Json(d));
					}
					k.set("sampleFields", kTakeFieldNames);
					k.set("samples", sm);
					takes.push(k);
				}
				if (!o.takes.empty()) j.set("takes", takes);
				if (!o.sounds.empty()) j.set("voice", SoundsToJson(o.sounds));
				if (!o.faceClips.empty())
				{
					Json fc = Json::MakeArray();
					for (auto& c : o.faceClips)
					{
						Json k = Json::MakeObject();
						k.set("startUs", static_cast<double>(c.startUs));
						k.set("lengthUs", static_cast<double>(c.lengthUs));
						k.set("pose", c.pose);
						k.set("weight", static_cast<double>(c.weight));
						k.set("blendMs", c.blendMs);
						fc.push(k);
					}
					j.set("faceClips", fc);
				}
				if (!o.idleKeys.empty())
				{
					Json ik = Json::MakeArray();
					for (auto& k : o.idleKeys)
					{
						Json q = Json::MakeObject();
						q.set("tUs", static_cast<double>(k.tUs));
						q.set("on", k.on);
						if (!k.profile.empty()) q.set("profile", k.profile);
						ik.push(q);
					}
					j.set("faceIdle", ik);
				}
				if (!o.eyeKeys.empty())
				{
					Json ek = Json::MakeArray();
					for (auto& k : o.eyeKeys)
					{
						Json q = Json::MakeObject();
						q.set("tUs", static_cast<double>(k.tUs));
						q.set("mode", k.mode == EyeKey::LookAt ? "lookat" : (k.mode == EyeKey::Aim ? "aim" : "free"));
						if (k.target) q.set("target", k.target);
						if (k.mode == EyeKey::Aim) { q.set("pitch", static_cast<double>(k.pitch)); q.set("yaw", static_cast<double>(k.yaw)); }
						ek.push(q);
					}
					j.set("eyes", ek);
				}
				if (!o.keys.empty()) j.set("keys", KeysToJson(o.keys));
			}
			else if (o.type == ObjType::Audio)
			{
				j.set("music", o.music);
				j.set("sounds", SoundsToJson(o.sounds));
			}
			else if (o.type == ObjType::Prop)
			{
				j.set("classname", o.classname);
				Json kv = Json::MakeObject();
				for (auto& p : o.kv) kv.set(p.first, p.second);
				j.set("keys_kv", kv);
				if (!o.keys.empty()) j.set("keys", KeysToJson(o.keys));
				if (o.attachTo)
				{
					j.set("attachTo", o.attachTo);
					j.set("bolt", o.bolt);
					j.set("attachOffset", Vec(o.attachOffset));
					j.set("attachAngles", Vec(o.attachAngles));
				}
			}
			else if (o.type == ObjType::Events)
			{
				j.set("startUs", static_cast<double>(o.eventsStartUs));
				Json ev = Json::MakeArray();
				for (auto& e : o.Events())
				{
					Json k = Json::MakeObject();
					k.set("t", static_cast<double>(e.tUs));
					k.set("temp", e.temp);
					k.set("ev", e.event);
					if (e.parm) k.set("parm", e.parm);
					if (e.actor) k.set("actor", e.actor);
					if (e.other2) k.set("other2", e.other2);
					k.set("o", Vec(e.origin));
					const auto nz = [](const float* v) { return v[0] != 0 || v[1] != 0 || v[2] != 0; };
					if (nz(e.pos3)) k.set("pos3", Vec(e.pos3));
					if (nz(e.pos4)) k.set("pos4", Vec(e.pos4));
					if (nz(e.angles)) k.set("ang", Vec(e.angles));
					if (nz(e.angles2)) k.set("ang2", Vec(e.angles2));
					if (nz(e.origin2)) k.set("o2", Vec(e.origin2));
					if (e.weapon) k.set("weapon", e.weapon);
					if (e.loopSound) k.set("loop", e.loopSound);
					if (e.modelindex) k.set("model", e.modelindex);
					if (e.time) k.set("time", e.time);
					if (e.time2) k.set("time2", e.time2);
					if (!e.fx.empty()) k.set("fx", e.fx);
					if (!e.sound.empty()) k.set("sound", e.sound);
					ev.push(k);
				}
				j.set("events", ev);
			}
			else
			{
				j.set("fov", static_cast<double>(o.fov));
				if (!o.keys.empty()) j.set("keys", KeysToJson(o.keys));
				if (!o.cuts.empty())
				{
					Json cuts = Json::MakeArray();
					for (const int64_t c : o.cuts) cuts.push(Json(static_cast<double>(c)));
					j.set("cuts", cuts);
				}
				if (o.lookAt) j.set("lookAt", o.lookAt);
				if (o.shake != 0) j.set("shake", static_cast<double>(o.shake));
			}
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
		for (auto& o : objects)
			if (o.type == ObjType::Prop && !o.Value("model").empty() && o.Value("model")[0] != '*')
			{
				bool seen = false;
				for (size_t i = 0; i < assets.size(); i++) seen = seen || assets[i].asString() == "model:" + o.Value("model");
				if (!seen) assets.push("model:" + o.Value("model"));
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
			const std::string ty = j.get("type").asString();
			o.type = ty == "camera" ? ObjType::Camera : (ty == "events" ? ObjType::Events : (ty == "prop" ? ObjType::Prop : (ty == "audio" ? ObjType::Audio : ObjType::Actor)));
			o.name = j.get("name").asString(NewName(ObjTypeName(o.type)));
			ReadVec(j.get("origin"), o.origin);
			ReadVec(j.get("angles"), o.angles);
			o.visible = j.get("visible").asBool(true);
			if (o.type == ObjType::Camera)
			{
				o.fov = static_cast<float>(j.get("fov").asNumber(80));
				KeysFromJson(j.get("keys"), o.keys);
				const Json& cuts = j.get("cuts");
				for (size_t k = 0; k < cuts.size(); k++) o.cuts.push_back(static_cast<int64_t>(cuts[k].asNumber(0)));
				std::sort(o.cuts.begin(), o.cuts.end());
				o.cuts.erase(std::unique(o.cuts.begin(), o.cuts.end()), o.cuts.end());
				o.lookAt = j.get("lookAt").asInt(0);
				o.shake = static_cast<float>(j.get("shake").asNumber(0));
			}
			else if (o.type == ObjType::Audio)
			{
				o.music = j.get("music").asBool(false);
				SoundsFromJson(j.get("sounds"), o.sounds);
			}
			else if (o.type == ObjType::Prop)
			{
				o.classname = j.get("classname").asString();
				const Json& kv = j.get("keys_kv");
				for (auto& k : kv.keys()) o.kv.emplace_back(k, kv.get(k).asString());
				KeysFromJson(j.get("keys"), o.keys);
				o.attachTo = j.get("attachTo").asInt(0);
				o.bolt = j.get("bolt").asString();
				ReadVec(j.get("attachOffset"), o.attachOffset);
				ReadVec(j.get("attachAngles"), o.attachAngles);
				if (o.classname.empty()) { warnings.push_back(o.name + ": prop without a class"); continue; }
				if (!Spawn(o)) warnings.push_back(o.name + ": '" + o.classname + "' didn't spawn (missing model / pk3?)");
			}
			else if (o.type == ObjType::Events)
			{
				o.eventsStartUs = static_cast<int64_t>(j.get("startUs").asNumber(0));
				o.events = std::make_shared<EventData>();
				const Json& ev = j.get("events");
				for (size_t k = 0; k < ev.size(); k++)
				{
					const Json& e = ev[k];
					WorldEvent w;
					w.tUs = static_cast<int64_t>(e.get("t").asNumber(0));
					w.temp = e.get("temp").asBool(true);
					w.event = e.get("ev").asInt(0);
					w.parm = e.get("parm").asInt(0);
					w.actor = e.get("actor").asInt(0);
					w.other2 = e.get("other2").asInt(0);
					ReadVec(e.get("o"), w.origin);
					ReadVec(e.get("pos3"), w.pos3);
					ReadVec(e.get("pos4"), w.pos4);
					ReadVec(e.get("ang"), w.angles);
					ReadVec(e.get("ang2"), w.angles2);
					ReadVec(e.get("o2"), w.origin2);
					w.weapon = e.get("weapon").asInt(0);
					w.loopSound = e.get("loop").asInt(0);
					w.modelindex = e.get("model").asInt(0);
					w.time = e.get("time").asInt(0);
					w.time2 = e.get("time2").asInt(0);
					w.fx = e.get("fx").asString();
					w.sound = e.get("sound").asString();
					if (w.event) o.events->events.push_back(w);
				}
				std::stable_sort(o.events->events.begin(), o.events->events.end(), [](const WorldEvent& a, const WorldEvent& b) { return a.tUs < b.tUs; });
			}
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
					c.blendMs = cj.get("blendMs").asInt(100);
					c.animOffsetUs = static_cast<int64_t>(cj.get("animOffsetUs").asNumber(0));
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
					t.trimInUs = static_cast<int64_t>(tj.get("trimInUs").asNumber(0));
					t.trimOutUs = static_cast<int64_t>(tj.get("trimOutUs").asNumber(0));
					ReadVec(tj.get("offset"), t.offset);
					t.yawOffset = static_cast<float>(tj.get("yawOffset").asNumber(0));
					const Json& an = tj.get("anims");
					for (size_t q = 0; q < an.size(); q++) t.AnimIndex(an[q].asString());
					const Json& sm = tj.get("samples");
					// fields by name: files from earlier builds have fewer (no velocity, speeds or force)
					std::vector<std::string> fields;
					{
						std::string names = tj.get("sampleFields").asString("tUs x y z yaw pitch legs legsFrame torso torsoFrame flags weapon");
						size_t p0 = 0;
						while (p0 < names.size())
						{
							size_t p1 = names.find(' ', p0);
							if (p1 == std::string::npos) p1 = names.size();
							if (p1 > p0) fields.push_back(names.substr(p0, p1 - p0));
							p0 = p1 + 1;
						}
					}
					const auto col = [&](const char* n) { for (size_t i = 0; i < fields.size(); i++) if (fields[i] == n) return static_cast<int>(i); return -1; };
					const int cT = col("tUs"), cX = col("x"), cY = col("y"), cZ = col("z"), cVx = col("vx"), cVy = col("vy"), cVz = col("vz");
					const int cYaw = col("yaw"), cPitch = col("pitch"), cL = col("legs"), cLf = col("legsFrame"), cLs = col("legsSpeed");
					const int cTo = col("torso"), cTf = col("torsoFrame"), cTs = col("torsoSpeed"), cFl = col("flags"), cW = col("weapon"), cF = col("force");
					const bool old12 = cVx < 0;
					const size_t kF = fields.size();
					const int na = static_cast<int>(t.Anims().size());
					auto& out = t.Edit().samples;
					for (size_t b = 0; kF && cT >= 0 && b + kF <= sm.size(); b += kF)
					{
						TakeSample x;
						const auto n = [&](const int c, const double d) { return c < 0 ? d : sm[b + static_cast<size_t>(c)].asNumber(d); };
						x.tUs = static_cast<int64_t>(n(cT, 0));
						x.origin[0] = static_cast<float>(n(cX, 0)); x.origin[1] = static_cast<float>(n(cY, 0)); x.origin[2] = static_cast<float>(n(cZ, 0));
						x.velocity[0] = static_cast<float>(n(cVx, 0)); x.velocity[1] = static_cast<float>(n(cVy, 0)); x.velocity[2] = static_cast<float>(n(cVz, 0));
						x.yaw = static_cast<float>(n(cYaw, 0));
						x.pitch = static_cast<float>(n(cPitch, 0));
						x.legs = static_cast<int>(n(cL, -1));
						x.legsFrame = static_cast<float>(n(cLf, 0));
						x.legsSpeed = static_cast<float>(n(cLs, 1));
						x.torso = static_cast<int>(n(cTo, -1));
						x.torsoFrame = static_cast<float>(n(cTf, 0));
						x.torsoSpeed = static_cast<float>(n(cTs, 1));
						x.flags = static_cast<int>(n(cFl, 0));
						x.weapon = static_cast<int>(n(cW, 0));
						x.forceActive = static_cast<int>(n(cF, 0));
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
				SoundsFromJson(j.get("voice"), o.sounds);
				{
					const Json& fc = j.get("faceClips");
					for (size_t k = 0; k < fc.size(); k++)
					{
						FaceClip c;
						c.startUs = static_cast<int64_t>(fc[k].get("startUs").asNumber(0));
						c.lengthUs = static_cast<int64_t>(fc[k].get("lengthUs").asNumber(1e6));
						c.pose = fc[k].get("pose").asString();
						c.weight = static_cast<float>(fc[k].get("weight").asNumber(1));
						c.blendMs = fc[k].get("blendMs").asInt(250);
						if (!c.pose.empty()) o.faceClips.push_back(c);
					}
					const Json& ik = j.get("faceIdle");
					for (size_t k = 0; k < ik.size(); k++)
					{
						IdleKey q;
						q.tUs = static_cast<int64_t>(ik[k].get("tUs").asNumber(0));
						q.on = ik[k].get("on").asBool(true);
						q.profile = ik[k].get("profile").asString();
						o.idleKeys.push_back(q);
					}
					const Json& ek = j.get("eyes");
					for (size_t k = 0; k < ek.size(); k++)
					{
						EyeKey q;
						q.tUs = static_cast<int64_t>(ek[k].get("tUs").asNumber(0));
						const std::string m = ek[k].get("mode").asString();
						q.mode = m == "lookat" ? EyeKey::LookAt : (m == "aim" ? EyeKey::Aim : EyeKey::Free);
						q.target = ek[k].get("target").asInt(0);
						q.pitch = static_cast<float>(ek[k].get("pitch").asNumber(0));
						q.yaw = static_cast<float>(ek[k].get("yaw").asNumber(0));
						o.eyeKeys.push_back(q);
					}
					const auto byStart = [](const auto& a, const auto& b) { return a.startUs < b.startUs; };
					const auto byT = [](const auto& a, const auto& b) { return a.tUs < b.tUs; };
					std::stable_sort(o.faceClips.begin(), o.faceClips.end(), byStart);
					std::stable_sort(o.idleKeys.begin(), o.idleKeys.end(), byT);
					std::stable_sort(o.eyeKeys.begin(), o.eyeKeys.end(), byT);
				}
				KeysFromJson(j.get("keys"), o.keys);
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
