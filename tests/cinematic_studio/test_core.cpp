// Unit tests for the Cinematic Studio core (no engine). Build: see CMakeLists.txt / run.sh here.
#include "../../code/cgame/cs_core.h"

#include <cmath>
#include <cstdio>
#include <map>

using namespace cs;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

class FakeHost : public Host
{
public:
	struct Ent { std::string npc, name; float o[3], a[3]; bool ai = false; bool alive = true; std::string profile; };
	std::map<int, Ent> ents;
	std::map<std::string, std::string> files;
	std::string map = "md_test";
	int64_t now = 0;
	int nextEnt = 10;
	std::vector<std::string> log;

	int SpawnActor(const std::string& npc, const float o[3], const float a[3], const std::string& name) override
	{
		if (npc == "missing") return -1;
		Ent e; e.npc = npc; e.name = name;
		for (int i = 0; i < 3; i++) { e.o[i] = o[i]; e.a[i] = a[i]; }
		ents[nextEnt] = e;
		return nextEnt++;
	}
	void RemoveActor(int ent) override { ents.erase(ent); }
	bool ActorAlive(int ent) override { return ents.count(ent) && ents[ent].alive; }
	bool GetActorTransform(int ent, float o[3], float a[3]) override
	{
		if (!ents.count(ent)) return false;
		for (int i = 0; i < 3; i++) { o[i] = ents[ent].o[i]; a[i] = ents[ent].a[i]; }
		return true;
	}
	void SetActorTransform(int ent, const float o[3], const float a[3]) override
	{
		for (int i = 0; i < 3; i++) { ents[ent].o[i] = o[i]; ents[ent].a[i] = a[i]; }
	}
	void SetActorAI(int ent, bool on) override { ents[ent].ai = on; }
	bool SetActorProfile(int ent, const std::string& p, std::string& why, bool force) override
	{
		if (p == "_humanoid_alien" && !force) { why = "different bones"; return false; }
		ents[ent].profile = p;
		return true;
	}
	std::string MapName() override { return map; }
	bool WriteFile(const std::string& p, const std::string& d) override { files[p] = d; return true; }
	bool ReadFile(const std::string& p, std::string& out) override
	{
		if (!files.count(p)) return false;
		out = files[p];
		return true;
	}
	int64_t NowMs() override { return now; }
	void Log(int, const std::string& m) override { log.push_back(m); }
};

static void TestJson()
{
	Json j;
	CHECK(Json::parse(R"({"a":1,"b":[1,2.5,-3e2],"c":{"d":"x\"yé"},"e":true,"f":null})", j));
	CHECK(j.get("a").asInt() == 1);
	CHECK(j.get("b").size() == 3 && std::fabs(j.get("b")[1].asNumber() - 2.5) < 1e-9 && j.get("b")[2].asNumber() == -300);
	CHECK(j.get("c").get("d").asString() == "x\"y\xc3\xa9");
	CHECK(j.get("e").asBool() && j.get("f").isNull());
	Json k;
	CHECK(Json::parse(j.dump(), k));		// round trip
	CHECK(k.get("c").get("d").asString() == "x\"y\xc3\xa9" && k.keys().size() == 5 && k.keys()[0] == "a");
	std::string err;
	CHECK(!Json::parse("{\"a\":1,}", k, &err) && !err.empty());
	CHECK(!Json::parse("[1,2", k));
	CHECK(!Json::parse("{} x", k));
	CHECK(j.get("missing").get("deeper")[3].asInt(7) == 7);	// safe chained reads
}

static void TestClock()
{
	Clock c;
	c.fps = 30; c.lengthUs = 2'000'000;
	c.Play(); c.Advance(500'000);
	CHECK(c.timeUs == 500'000 && c.Frame() == 15);
	c.speed = 0.5; c.Advance(1'000'000);
	CHECK(c.timeUs == 1'000'000);
	c.speed = 1; c.Advance(5'000'000);
	CHECK(!c.playing && c.timeUs == 2'000'000);		// stops at the end
	c.loop = true; c.timeUs = 1'900'000; c.Play(); c.Advance(200'000);
	CHECK(c.playing && c.timeUs == 100'000);
	c.Step(1);
	CHECK(!c.playing && c.Frame() == 4);			// 100 ms = frame 3, +1
	c.Step(-10);
	CHECK(c.Frame() == 0);
	for (int f : {24, 30, 60})
	{
		c.SetFps(f);
		for (int64_t fr = 0; fr <= c.FrameCount(); fr++) { c.SetFrame(fr); CHECK(c.Frame() == fr); }
	}
	c.SetFps(25);
	CHECK(c.fps == 60);								// unsupported rates are ignored
	c.SetFps(30); c.SetFrame(45);
	CHECK(c.Timecode() == "00:00:01:15");
	c.Stop();
	CHECK(c.timeUs == 0 && !c.playing);
}

static void TestUndo()
{
	FakeHost h;
	Scene s(&h);
	const float o[3] = { 10, 20, 30 }, a[3] = { 0, 90, 0 };
	const int vader = s.AddActor("md_vader", o, a);
	CHECK(vader && s.objects.size() == 1 && h.ents.size() == 1);
	const int ent = s.Find(vader)->entNum;
	CHECK(h.ents[ent].npc == "md_vader" && !h.ents[ent].ai);

	// nudges within a second merge into one undo step
	float o2[3] = { 14, 20, 30 };
	h.now = 100; s.SetTransform(vader, o2, a, true);
	o2[0] = 18; h.now = 300; s.SetTransform(vader, o2, a, true);
	CHECK(h.ents[ent].o[0] == 18);
	CHECK(s.Undo());
	CHECK(h.ents[ent].o[0] == 10 && s.Find(vader)->origin[0] == 10);
	CHECK(s.Redo());
	CHECK(h.ents[ent].o[0] == 18);

	// remove + undo respawns (as a new entity) with the same id, name and transform
	const std::string name = s.Find(vader)->name;
	CHECK(s.Remove(vader) && h.ents.empty() && s.objects.empty());
	CHECK(s.Undo());
	CHECK(s.Find(vader) && s.Find(vader)->name == name && h.ents.size() == 1);
	CHECK(h.ents[s.Find(vader)->entNum].o[0] == 18);
	CHECK(s.Undo());		// undo the move (2 ops back: remove was undone)
	CHECK(s.Find(vader)->origin[0] == 10);

	// redo stack cleared by a new edit
	CHECK(s.CanRedo());
	const float co[3] = { 0, 0, 64 };
	const int cam = s.AddCamera(co, a, 60);
	CHECK(cam && !s.CanRedo() && s.Find(cam)->name == "Camera 01");
	CHECK(s.Undo() && !s.Find(cam));

	// a missing NPC type doesn't add an object
	CHECK(s.AddActor("missing", o, a) == 0);

	// adopted level NPCs are released, not deleted, when removed from the scene
	h.ents[99] = FakeHost::Ent{ "stormtrooper", "st", { 1, 2, 3 }, { 0, 0, 0 }, true, true, "" };
	const int st = s.AdoptActor(99, "stormtrooper", "");
	CHECK(st && s.Find(st)->adopted && s.Find(st)->origin[2] == 3);
	CHECK(s.Remove(st) && h.ents.count(99));
	CHECK(s.Undo() && s.Find(st)->entNum == 99);

	// AI toggle reaches the entity and undoes
	CHECK(s.SetAI(vader, true) && h.ents[s.Find(vader)->entNum].ai);
	CHECK(s.Undo() && !h.ents[s.Find(vader)->entNum].ai);
}

static void TestProject()
{
	FakeHost h;
	Scene s(&h);
	const float o[3] = { 1.5f, -2, 3 }, a[3] = { 0, 45, 0 };
	s.AddActor("md_vader", o, a);
	s.AddActor("md_obiwan", o, a);
	const float co[3] = { 100, 0, 50 };
	s.AddCamera(co, a, 55);
	s.clock.SetFps(24);
	s.clock.lengthUs = 30'000'000;
	CHECK(!s.Save("bad name"));
	CHECK(s.Save("Vader_Duel") && !s.dirty);
	CHECK(h.files.count("CinematicStudio/Projects/Vader_Duel/project.json"));
	CHECK(h.files.count("CinematicStudio/Projects/Vader_Duel/scenes/scene_01.json"));

	Scene t(&h);
	std::vector<std::string> warn;
	h.map = "other_map";
	CHECK(!t.Load("Vader_Duel", false, false, warn));			// map mismatch refused
	CHECK(t.Load("Vader_Duel", true, false, warn) && warn.empty());
	CHECK(t.objects.size() == 3 && t.clock.fps == 24 && t.clock.lengthUs == 30'000'000);
	CHECK(t.objects[0].npcType == "md_vader" && std::fabs(t.objects[0].origin[0] - 1.5f) < 1e-6 && t.objects[0].entNum >= 0);
	CHECK(t.objects[2].type == ObjType::Camera && std::fabs(t.objects[2].fov - 55) < 1e-6);
	// ids survive and new objects don't collide
	const float z[3] = { 0, 0, 0 };
	const int nid = t.AddCamera(z, z, 80);
	for (size_t i = 0; i + 1 < t.objects.size(); i++) CHECK(t.objects[i].id != nid);

	// missing assets load with warnings instead of failing
	std::string scene = h.files["CinematicStudio/Projects/Vader_Duel/scenes/scene_01.json"];
	const std::string key = "\"npc\": \"md_obiwan\"";
	const size_t at = scene.find(key);
	CHECK(at != std::string::npos);
	scene.replace(at, key.size(), "\"npc\": \"missing\"");
	h.files["CinematicStudio/Projects/Vader_Duel/scenes/scene_01.json"] = scene;
	Scene u(&h);
	warn.clear();
	// the actor stays in the scene (unspawned) so saving again doesn't silently drop it
	CHECK(u.Load("Vader_Duel", true, false, warn) && warn.size() == 1 && u.objects.size() == 3 && u.objects[1].entNum == -1);

	// autosave only when dirty, and recover from it
	h.map = "md_test";
	Scene v(&h);
	v.AddActor("md_vader", o, a);
	CHECK(v.Save("Recover") && !v.Autosave());
	v.AddCamera(co, a, 70);
	CHECK(v.Autosave() && v.dirty);
	Scene w(&h);
	warn.clear();
	CHECK(w.Load("Recover", false, true, warn) && w.objects.size() == 2 && w.dirty);
	CHECK(w.Load("Recover", false, false, warn) && w.objects.size() == 1);	// the saved version

	// newer schema refused
	h.files["CinematicStudio/Projects/Future/project.json"] = R"({"schema":"jkbuild.cinematic.project","version":99})";
	CHECK(!w.Load("Future", true, false, warn));
}

static void TestPhase2()
{
	FakeHost h;
	Scene s(&h);
	const float o[3] = { 0, 0, 24 }, a[3] = { 0, 0, 0 };
	const int v = s.AddActor("md_vader", o, a);
	const int ent = s.Find(v)->entNum;

	// profiles: validated by the host, undoable
	CHECK(s.SetProfile(v, "_humanoid_vader") && h.ents[ent].profile == "_humanoid_vader");
	CHECK(!s.SetProfile(v, "_humanoid_alien") && s.Find(v)->profile == "_humanoid_vader");	// refused, unchanged
	CHECK(s.Undo() && h.ents[ent].profile.empty() && s.Find(v)->profile.empty());
	CHECK(s.Redo() && h.ents[ent].profile == "_humanoid_vader");
	// forced: no checks, remembered, saved
	CHECK(s.SetProfile(v, "_humanoid_alien", true) && h.ents[ent].profile == "_humanoid_alien" && s.Find(v)->profileForced);
	CHECK(s.Save("Forced"));
	{ Scene f(&h); std::vector<std::string> w; CHECK(f.Load("Forced", false, false, w) && f.objects[0].profileForced && h.ents[f.objects[0].entNum].profile == "_humanoid_alien"); f.Clear(); }
	CHECK(s.Undo() && h.ents[ent].profile == "_humanoid_vader" && !s.Find(v)->profileForced);

	// clips stay sorted, select the edited clip, undo
	AnimClip c; c.anim = "BOTH_STAND1"; c.startUs = 2'000'000; c.lengthUs = 1'000'000;
	CHECK(s.AddClip(v, c) == 0);
	c.anim = "BOTH_WALK1"; c.startUs = 500'000;
	CHECK(s.AddClip(v, c) == 0 && s.Find(v)->clips[1].anim == "BOTH_STAND1");
	AnimClip moved = s.Find(v)->clips[0];
	moved.startUs = 3'000'000;
	h.now = 5000;
	CHECK(s.SetClip(v, 0, moved, true) && s.selectedClip == 1 && s.Find(v)->clips[1].anim == "BOTH_WALK1");
	moved.startUs = 3'500'000; h.now = 5200;
	CHECK(s.SetClip(v, 1, moved, true));							// merged drag
	CHECK(s.Undo() && s.Find(v)->clips[0].anim == "BOTH_WALK1" && s.Find(v)->clips[0].startUs == 500'000);
	CHECK(s.RemoveClip(v, 0) && s.Find(v)->clips.size() == 1);
	CHECK(s.Undo() && s.Find(v)->clips.size() == 2);

	// which clip drives the actor
	int64_t local = 0; bool hold = false;
	const Object& ob = *s.Find(v);
	CHECK(Scene::ActiveClip(ob, 100'000, local, hold) == nullptr);				// before the first clip: game animates
	const AnimClip* ac = Scene::ActiveClip(ob, 1'000'000, local, hold);
	CHECK(ac && ac->anim == "BOTH_WALK1" && local == 500'000 && !hold);
	ac = Scene::ActiveClip(ob, 1'800'000, local, hold);
	CHECK(ac && ac->anim == "BOTH_WALK1" && hold && local == 1'000'000);		// held on its last frame
	ac = Scene::ActiveClip(ob, 2'100'000, local, hold);
	CHECK(ac && ac->anim == "BOTH_STAND1" && local == 100'000);

	// NPC type change respawns and undoes
	const int before = s.Find(v)->entNum;
	CHECK(s.SetNpcType(v, "md_obiwan") && h.ents[s.Find(v)->entNum].npc == "md_obiwan" && !h.ents.count(before));
	CHECK(h.ents[s.Find(v)->entNum].profile == "_humanoid_vader");				// profile carried over
	CHECK(!s.SetNpcType(v, "missing") && h.ents[s.Find(v)->entNum].npc == "md_obiwan");
	CHECK(s.Undo() && h.ents[s.Find(v)->entNum].npc == "md_vader");

	// duplicate copies profile and clips, beside the original
	const int d = s.Duplicate(v);
	CHECK(d && s.Find(d)->clips.size() == 2 && s.Find(d)->profile == "_humanoid_vader" && s.Find(d)->origin[1] == 40);
	CHECK(s.Find(d)->name == "md_vader 02" && h.ents[s.Find(d)->entNum].profile == "_humanoid_vader");
	CHECK(s.Undo() && !s.Find(d));

	// saved and loaded
	CHECK(s.Save("P2"));
	Scene t(&h);
	std::vector<std::string> warn;
	CHECK(t.Load("P2", false, false, warn));
	const Object* lo = t.Find(v);
	CHECK(lo && lo->profile == "_humanoid_vader" && lo->clips.size() == 2 && lo->clips[0] == s.Find(v)->clips[0]);
	CHECK(h.ents[lo->entNum].profile == "_humanoid_vader");
}

int main()
{
	TestPhase2();
	TestJson();
	TestClock();
	TestUndo();
	TestProject();
	printf("%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
