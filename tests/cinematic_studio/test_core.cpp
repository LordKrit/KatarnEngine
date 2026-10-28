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
	struct Ent { std::string npc, name; float o[3], a[3]; bool ai = false; bool alive = true; std::string profile; bool prop = false; };
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
	void SetActorProp(int ent, bool on) override { ents[ent].prop = on; }
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

static Take MakeTake()
{
	Take t;
	t.performer = "player";
	const int stand = t.AnimIndex("BOTH_STAND1"), walk = t.AnimIndex("BOTH_WALK1");
	CHECK(t.AnimIndex("BOTH_STAND1") == stand && t.Anims().size() == 2);
	for (int i = 0; i <= 10; i++)
	{
		TakeSample x;
		x.tUs = i * 100'000;
		x.origin[0] = static_cast<float>(i * 10);
		x.yaw = i < 5 ? 170.0f : -170.0f;
		x.legs = i < 6 ? stand : walk;
		x.legsFrame = static_cast<float>(i < 6 ? i * 2 : (i - 6) * 3);
		x.torso = stand;
		x.torsoFrame = static_cast<float>(i == 8 ? 0 : i);	// loops back at 8
		x.flags = i >= 3 ? TAKE_SABER1 : 0;
		x.velocity[0] = 100;
		x.velocity[1] = static_cast<float>(i * 10);
		x.legsSpeed = 0.5f;
		t.Add(x);
	}
	return t;
}

static void TestPhase3()
{
	FakeHost h;
	Scene s(&h);
	const float o[3] = { 0, 0, 0 }, a[3] = { 0, 0, 0 };

	// evaluation: interpolation, clamping, animation changes and loops
	Take t = MakeTake();
	CHECK(t.LengthUs() == 1'000'000);
	TakeSample x;
	CHECK(t.Evaluate(150'000, x) && std::fabs(x.origin[0] - 15) < 1e-4 && std::fabs(x.legsFrame - 3) < 1e-4 && x.legs == 0);
	CHECK(t.Evaluate(-5, x) && x.origin[0] == 0 && t.Evaluate(5'000'000, x) && x.origin[0] == 100);	// clamped
	CHECK(t.Evaluate(450'000, x) && std::fabs(std::fabs(x.yaw) - 180) < 1e-3);		// shortest way across 180
	CHECK(t.Evaluate(550'000, x) && x.legs == 0 && std::fabs(x.legsFrame - 10) < 1e-4);	// new animation next: hold
	CHECK(t.Evaluate(750'000, x) && std::fabs(x.torsoFrame - 7) < 1e-4);					// looping back: hold
	CHECK(t.Evaluate(350'000, x) && x.flags == TAKE_SABER1);
	CHECK(t.Evaluate(150'000, x) && std::fabs(x.velocity[1] - 15) < 1e-4 && x.velocity[0] == 100 && x.legsSpeed == 0.5f);
	Take empty;
	CHECK(!empty.Evaluate(0, x));
	// copies share the recorded data until one is changed
	Take c1 = t;
	CHECK(&c1.Samples() == &t.Samples());
	c1.Add(TakeSample());
	CHECK(&c1.Samples() != &t.Samples() && t.Samples().size() == 11 && c1.Samples().size() == 12);

	// prop mode
	const int v = s.AddActor("md_vader", o, a);
	const int ent = s.Find(v)->entNum;
	CHECK(s.SetProp(v, true) && h.ents[ent].prop && !h.ents[ent].ai);
	CHECK(!s.SetProp(v, true));
	CHECK(s.SetAI(v, true) && !h.ents[ent].ai);		// AI stays off while it's a prop
	CHECK(s.SetProp(v, false) && !h.ents[ent].prop && h.ents[ent].ai);
	CHECK(s.Undo() && h.ents[ent].prop && s.Undo() && !s.Find(v)->ai && s.Undo() && !h.ents[ent].prop);

	// takes: add, activate, move, rename, remove, undo
	const int t1 = s.AddTake(v, t, true);
	CHECK(t1 == 1 && s.Find(v)->activeTake == 1 && s.Find(v)->prop && h.ents[ent].prop && s.selectedTake == 1);
	CHECK(s.Find(v)->takes[0].name == "Take 1");
	Take tooShort; tooShort.Add(TakeSample());
	CHECK(!s.AddTake(v, tooShort, true));
	const int t2 = s.AddTake(v, t, false);
	CHECK(t2 == 2 && s.Find(v)->activeTake == 1);
	CHECK(s.SetActiveTake(v, t2) && Scene::ActiveTake(*s.Find(v))->id == 2);
	CHECK(!s.SetActiveTake(v, 99));
	CHECK(s.SetTakeStart(v, t2, 500'000, true) && s.SetTakeStart(v, t2, 700'000, true));
	CHECK(Scene::FindTake(*s.Find(v), t2)->startUs == 700'000);
	CHECK(s.Undo() && Scene::FindTake(*s.Find(v), t2)->startUs == 0);		// coalesced drag = one undo
	CHECK(s.RenameTake(v, t2, "Duel B") && Scene::FindTake(*s.Find(v), t2)->name == "Duel B");
	CHECK(s.RemoveTake(v, t2) && s.Find(v)->activeTake == 1 && s.Find(v)->takes.size() == 1);
	CHECK(s.Undo() && s.Find(v)->takes.size() == 2 && s.Find(v)->activeTake == 2);
	CHECK(s.SetGhost(v, true) && s.Find(v)->ghost);
	CHECK(s.SetActiveTake(v, 0) && !Scene::ActiveTake(*s.Find(v)) && s.Undo());

	// saved and loaded, sample for sample
	CHECK(s.Save("P3"));
	Scene l(&h);
	std::vector<std::string> warn;
	CHECK(l.Load("P3", false, false, warn) && warn.empty());
	const Object* lo = l.Find(v);
	CHECK(lo && lo->prop && lo->ghost && lo->activeTake == 2 && lo->takes.size() == 2);
	CHECK(lo->takes[0] == s.Find(v)->takes[0] && lo->takes[1] == s.Find(v)->takes[1]);
	CHECK(h.ents[lo->entNum].prop);

	// a damaged take is skipped with a warning; an older (version 1) project still loads
	std::string txt = h.files["CinematicStudio/Projects/P3/scenes/scene_01.json"];
	const size_t at = txt.find("\"samples\": [");
	CHECK(at != std::string::npos);
	h.files["CinematicStudio/Projects/P3/scenes/scene_01.json"] = txt.substr(0, at) + "\"samples\":[1,2,3]" +
		txt.substr(txt.find(']', at) + 1);
	Scene d(&h);
	warn.clear();
	CHECK(d.Load("P3", false, false, warn) && warn.size() == 1 && d.Find(v)->takes.size() == 1);
	std::string pj = h.files["CinematicStudio/Projects/P3/project.json"];
	const size_t vp = pj.find("\"version\": 2");
	CHECK(vp != std::string::npos);
	h.files["CinematicStudio/Projects/P3/project.json"] = pj.replace(vp, 12, "\"version\": 1");
	Scene e(&h);
	warn.clear();
	CHECK(e.Load("P3", false, false, warn));

	// a take saved by the first Phase 3 build (12 numbers per sample, no velocity) still loads
	std::string sj = h.files["CinematicStudio/Projects/P3/scenes/scene_01.json"];
	const std::string oldTake = "{\"id\": 7, \"name\": \"Old\", \"startUs\": 0, \"anims\": [\"BOTH_STAND1\"], "
		"\"sampleFields\": \"tUs x y z yaw pitch legs legsFrame torso torsoFrame flags weapon\", "
		"\"samples\": [0, 0, 0, 0, 90, 0, 0, 1, 0, 1, 1, 2,  500000, 50, 0, 0, 90, 0, 0, 3, 0, 3, 1, 2]}";
	const size_t tk = sj.find("\"takes\": [");
	CHECK(tk != std::string::npos);
	sj.insert(tk + 10, oldTake + ",");
	h.files["CinematicStudio/Projects/P3/scenes/scene_01.json"] = sj;
	Scene f(&h);
	warn.clear();
	CHECK(f.Load("P3", true, false, warn));
	Object* fo = f.Find(v);
	const Take* old = fo ? Scene::FindTake(*fo, 7) : nullptr;
	CHECK(old && old->Samples().size() == 2 && old->Samples()[1].legsFrame == 3 && old->Samples()[1].flags == 1 && old->Samples()[1].weapon == 2);
	CHECK(old && std::fabs(old->Samples()[0].velocity[0] - 100) < 1e-3 && old->Samples()[0].yaw == 90);	// estimated from positions

	// respawn (an actor that died in a recorded fight)
	const int was = fo->entNum;
	CHECK(f.Respawn(v) && fo->entNum >= 0 && fo->entNum != was && !h.ents.count(was) && h.ents[fo->entNum].prop);
}

int main()
{
	TestPhase3();
	TestPhase2();
	TestJson();
	TestClock();
	TestUndo();
	TestProject();
	printf("%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
