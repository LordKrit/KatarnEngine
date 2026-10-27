/*
JK BUILD: Cinematic Studio - scene, clock, undo/redo and project files (see cs_core.h).
*/
#include "cs_core.h"

#include <algorithm>
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
		host_->SetActorAI(o.entNum, o.ai);
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
			now - undo_.back().timeMs < 1000)
		{
			undo_.back().after = after;
			undo_.back().timeMs = now;
			redo_.clear();
			dirty = true;
		}
		else
		{
			Op op; op.kind = Op::Change; op.before = *o; op.after = after; op.timeMs = now;
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
		const bool aiChanged = cur->ai != state.ai;
		*cur = state;
		cur->entNum = ent;		// the live entity, not the one in the snapshot
		if (cur->type == ObjType::Actor && cur->entNum >= 0 && host_->ActorAlive(cur->entNum))
		{
			host_->SetActorTransform(cur->entNum, cur->origin, cur->angles);
			if (aiChanged) host_->SetActorAI(cur->entNum, cur->ai);
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

	void Scene::Clear()
	{
		for (auto& o : objects)
			if (o.type == ObjType::Actor && o.entNum >= 0 && !o.adopted) host_->RemoveActor(o.entNum);
		objects.clear();
		undo_.clear();
		redo_.clear();
		selected = 0;
		nextId_ = 1;
		dirty = false;
	}

	void Scene::Forget()
	{
		objects.clear();
		undo_.clear();
		redo_.clear();
		selected = 0;
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
			if (o.ai) host_->GetActorTransform(o.entNum, o.origin, o.angles);
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
