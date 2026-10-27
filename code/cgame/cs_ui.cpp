/*
JK BUILD: Cinematic Studio - in-game editor: panels, input, studio camera, viewport picking.

Drawn by the cgame over the live 3D view (the game keeps running unless "Freeze world" is on).
Layout in the 640x480 virtual screen:

  title bar / tabs
  outliner (left) | live viewport | tab panel / inspector (right)
  transport
  timeline
*/
#include "cg_headers.h"
#include "cg_media.h"
#include "cs_studio.h"
#include "../client/keycodes.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

extern qboolean CG_CalcFOVFromX(float fov_x);
void CS_ClearAllEntities();

using namespace cs;

namespace
{
	// ------------------------------------------------------------------------------------------
	// layout (virtual 640x480)
	// ------------------------------------------------------------------------------------------
	constexpr float kW = 640, kH = 480;
	constexpr float kTitleH = 13, kTabsY = 13, kTabsH = 12, kPanelY = 26;
	constexpr float kLeftW = 128, kRightW = 172, kRightX = kW - kRightW;
	constexpr float kTransportY = 330, kTransportH = 15;
	constexpr float kTimelineY = kTransportY + kTransportH, kTimelineH = kH - kTimelineY;
	constexpr float kRow = 10, kRulerH = 12, kStatusH = 10;
	constexpr float kText = 0.42f;			// font scale for panel text

	const vec4_t kPanel = { 0.04f, 0.05f, 0.07f, 0.80f };
	const vec4_t kPanelDark = { 0.02f, 0.02f, 0.03f, 0.88f };
	const vec4_t kLine = { 0.30f, 0.34f, 0.40f, 0.9f };
	const vec4_t kTextCol = { 0.88f, 0.90f, 0.93f, 1 };
	const vec4_t kDim = { 0.55f, 0.58f, 0.62f, 1 };
	const vec4_t kAccent = { 1.00f, 0.62f, 0.15f, 1 };
	const vec4_t kSel = { 0.95f, 0.55f, 0.10f, 0.35f };
	const vec4_t kHover = { 1, 1, 1, 0.08f };
	const vec4_t kButton = { 0.16f, 0.18f, 0.22f, 0.95f };
	const vec4_t kButtonOn = { 0.70f, 0.38f, 0.06f, 0.95f };
	const vec4_t kRed = { 0.95f, 0.25f, 0.20f, 1 };
	const vec4_t kWarn = { 1.0f, 0.85f, 0.3f, 1 };
	const vec4_t kCam = { 0.35f, 0.75f, 1.0f, 1 };
	const vec4_t kLane = { 1, 1, 1, 0.04f };
	const vec4_t kBar = { 0.35f, 0.55f, 0.80f, 0.45f };

	enum Tab { TAB_SCENE, TAB_ACTORS, TAB_ANIMS, TAB_CAMERA, TAB_AUDIO, TAB_PROPS, TAB_PROJECT, TAB_COUNT };
	const char* kTabNames[TAB_COUNT] = { "SCENE", "ACTORS", "ANIMATIONS", "CAMERA", "AUDIO", "PROPS", "PROJECT" };

	enum Field { FIELD_NONE, FIELD_NPC, FIELD_PROJECT, FIELD_RENAME, FIELD_ANIMSEARCH, FIELD_NPCTYPE };

	// configurable hotkeys (CinematicStudio/hotkeys.cfg)
	enum Action
	{
		ACT_CLOSE, ACT_PANELS, ACT_PLAY, ACT_STOP, ACT_STEP_BACK, ACT_STEP_FWD, ACT_UNDO, ACT_REDO, ACT_SAVE,
		ACT_DELETE, ACT_FOCUS, ACT_FREEZE, ACT_LEFT, ACT_RIGHT, ACT_FWD, ACT_BACK, ACT_UP, ACT_DOWN, ACT_TURN_L, ACT_TURN_R,
		ACT_PLAYER_VIEW, ACT_DUPLICATE, ACT_COUNT
	};
	struct Hotkey { const char* name; int key; bool ctrl; };
	Hotkey g_keys[ACT_COUNT] = {
		{ "close_studio", A_F10, false }, { "toggle_panels", A_TAB, false }, { "play_pause", A_SPACE, false },
		{ "stop", A_HOME, false }, { "step_back", A_COMMA, false }, { "step_forward", A_PERIOD, false },
		{ "undo", A_LOW_Z, true }, { "redo", A_LOW_Y, true }, { "save", A_LOW_S, true }, { "delete", A_DELETE, false },
		{ "focus", A_LOW_F, false }, { "freeze_world", A_LOW_P, false },
		{ "nudge_left", A_CURSOR_LEFT, false }, { "nudge_right", A_CURSOR_RIGHT, false },
		{ "nudge_forward", A_CURSOR_UP, false }, { "nudge_back", A_CURSOR_DOWN, false },
		{ "nudge_up", A_PAGE_UP, false }, { "nudge_down", A_PAGE_DOWN, false },
		{ "turn_left", A_LOW_Q, false }, { "turn_right", A_LOW_E, false }, { "player_view", A_LOW_V, false },
		{ "duplicate", A_LOW_D, true },
	};

	struct KeyName { const char* n; int k; };
	const KeyName kKeyNames[] = {
		{ "TAB", A_TAB }, { "SPACE", A_SPACE }, { "ENTER", A_ENTER }, { "HOME", A_HOME }, { "END", A_END },
		{ "DEL", A_DELETE }, { "INS", A_INSERT }, { "PGUP", A_PAGE_UP }, { "PGDN", A_PAGE_DOWN },
		{ "LEFT", A_CURSOR_LEFT }, { "RIGHT", A_CURSOR_RIGHT }, { "UP", A_CURSOR_UP }, { "DOWN", A_CURSOR_DOWN },
		{ "COMMA", A_COMMA }, { "PERIOD", A_PERIOD }, { "MINUS", A_MINUS }, { "PLUS", A_PLUS },
		{ "F1", A_F1 }, { "F2", A_F2 }, { "F3", A_F3 }, { "F4", A_F4 }, { "F5", A_F5 }, { "F6", A_F6 }, { "F7", A_F7 },
		{ "F8", A_F8 }, { "F9", A_F9 }, { "F10", A_F10 }, { "F11", A_F11 }, { "F12", A_F12 },
	};

	std::string KeyToName(const int k)
	{
		for (const auto& kn : kKeyNames) if (kn.k == k) return kn.n;
		if (k >= A_LOW_A && k <= A_LOW_Z) return std::string(1, static_cast<char>('A' + (k - A_LOW_A)));
		if (k >= '0' && k <= '9') return std::string(1, static_cast<char>(k));
		return "?";
	}

	int NameToKey(std::string n)
	{
		for (auto& c : n) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
		for (const auto& kn : kKeyNames) if (n == kn.n) return kn.k;
		if (n.size() == 1 && n[0] >= 'A' && n[0] <= 'Z') return A_LOW_A + (n[0] - 'A');
		if (n.size() == 1 && n[0] >= '0' && n[0] <= '9') return n[0];
		return -1;
	}

	struct Studio
	{
		GameHost host;
		Scene scene{ &host };

		bool open = false, panels = true;
		int tab = TAB_SCENE;

		// cursor and buttons
		float cx = 320, cy = 240;
		bool lmb = false, rmb = false, ctrl = false, shift = false;
		bool clicked = false;			// a left click waiting for a widget
		float clickX = 0, clickY = 0;
		int lastClickMs = 0;
		bool doubleClick = false;
		bool scrubbing = false;
		int wheel = 0;

		// fly keys (held while the right button is down)
		bool kFwd = false, kBack = false, kLeft = false, kRight = false, kUp = false, kDown = false;

		// studio camera: free camera, a scene camera seen through, or a scene camera being piloted
		bool camActive = false;
		vec3_t camOrg = { 0, 0, 0 }, camAng = { 0, 0, 0 };
		float camFov = 80;
		int lookId = 0;			// scene camera shown in the viewport (0 = free camera / player)
		int pilotId = 0;		// scene camera moved by the free camera

		// text entry
		int field = FIELD_NONE;
		std::string fieldText;
		std::string npcType = "md_vader";
		std::string projectName;

		int levelSel = -1;		// selected character that isn't in the scene
		int confirmNewMs = 0;	// "New project" needs a second click
		int lastMs = 0, lastAutosaveMs = 0;
		float timelineScroll = 0;

		std::deque<std::pair<int, std::string>> msgs;
		int msgMs = 0;
		std::vector<std::string> recent;
		std::string initMap;

		// ---- Phase 2: animation browser ----
		std::string animSearch;
		float animScroll = 0;
		int browseAnim = -1;		// selected animation (animNumber_t)
		int parts = PARTS_BOTH;
		float animSpeed = 1.0f;
		bool animLoop = true;
		int blendMs = 150;
		int rangeStart = 0, rangeEnd = -1;
		int previewEnt = -1;		// actor the browser is driving (clips leave it alone meanwhile)
		bool previewFrozen = false;
		float previewFrame = 0;
		bool scrubAnim = false;
		std::vector<AnimInfo> anims;	// of animsEnt
		int animsEnt = -1;
		std::string animsProfile;
		std::vector<std::string> profiles;
		int profilePick = -1;		// index into profiles shown in the picker
		std::string profileWhy;		// validation of the picked profile ("" = ok)
		bool profileChecked = false;

		// viewport transform dragging
		bool dragging = false, dragTurn = false;
		float dragZ = 0, dragOff[2] = { 0, 0 }, dragStartX = 0, dragStartYaw = 0;

		// timeline clip dragging
		int clipDragObj = 0, clipDragIndex = -1;
		int64_t clipDragGrabUs = 0;
		std::vector<int> heldEnts;	// actors whose animation the studio holds (released on close)
		int lastSel = 0;
	};

	Studio* g_s = nullptr;
	Studio& S()
	{
		if (!g_s) g_s = new Studio();
		return *g_s;
	}

	int NowMs() { return cgi_Milliseconds(); }
	void Close();
	void ReleasePreview();

	// ------------------------------------------------------------------------------------------
	// drawing helpers
	// ------------------------------------------------------------------------------------------

	float TextW(const std::string& s, const float sc = kText)
	{
		return static_cast<float>(cgi_R_Font_StrLenPixels(s.c_str(), cgs.media.qhFontSmall, sc));
	}

	void Text(const float x, const float y, const std::string& s, const float* col, const float sc = kText)
	{
		cgi_R_Font_DrawString(static_cast<int>(x), static_cast<int>(y), s.c_str(), col, cgs.media.qhFontSmall, -1, sc);
	}

	// text clipped to a width (with "..")
	void TextClip(const float x, const float y, std::string s, const float w, const float* col)
	{
		if (TextW(s) > w)
		{
			while (!s.empty() && TextW(s + "..") > w) s.pop_back();
			s += "..";
		}
		Text(x, y, s, col);
	}

	void Rect(const float x, const float y, const float w, const float h, const float* c) { CG_FillRect(x, y, w, h, c); }
	void Frame(const float x, const float y, const float w, const float h, const float* c) { CG_DrawRect(x, y, w, h, 0.5f, c); }

	bool Inside(const float px, const float py, const float x, const float y, const float w, const float h)
	{
		return px >= x && px < x + w && py >= y && py < y + h;
	}

	// takes the pending click if it landed in the rectangle
	bool TakeClick(const float x, const float y, const float w, const float h)
	{
		Studio& s = S();
		if (s.clicked && Inside(s.clickX, s.clickY, x, y, w, h)) { s.clicked = false; return true; }
		return false;
	}

	bool Button(const float x, const float y, const float w, const float h, const std::string& label,
		const bool on = false, const bool enabled = true)
	{
		Studio& s = S();
		const bool hover = enabled && Inside(s.cx, s.cy, x, y, w, h);
		Rect(x, y, w, h, on ? kButtonOn : kButton);
		if (hover) Rect(x, y, w, h, kHover);
		Frame(x, y, w, h, kLine);
		const float tw = TextW(label);
		Text(x + (w - tw) * 0.5f, y + 1.5f, label, enabled ? kTextCol : kDim);
		return enabled && TakeClick(x, y, w, h);
	}

	void Label(const float x, const float y, const std::string& k, const std::string& v, const float w = kRightW - 12)
	{
		Text(x, y, k, kDim);
		TextClip(x + 46, y, v, w - 46, kTextCol);
	}

	std::string Fmt(const char* f, ...)
	{
		char b[256];
		va_list ap;
		va_start(ap, f);
		vsnprintf(b, sizeof b, f, ap);
		va_end(ap);
		return b;
	}

	// world -> virtual screen through the view that was just rendered
	bool Project(const float* p, float& sx, float& sy)
	{
		vec3_t d;
		VectorSubtract(p, cg.refdef.vieworg, d);
		const float z = DotProduct(d, cg.refdef.viewaxis[0]);
		if (z < 1.0f) return false;
		const float tx = tanf(cg.refdef.fov_x * static_cast<float>(M_PI) / 360.0f);
		const float ty = tanf(cg.refdef.fov_y * static_cast<float>(M_PI) / 360.0f);
		sx = kW * 0.5f * (1.0f - DotProduct(d, cg.refdef.viewaxis[1]) / (z * tx));
		sy = kH * 0.5f * (1.0f - DotProduct(d, cg.refdef.viewaxis[2]) / (z * ty));
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// actions
	// ------------------------------------------------------------------------------------------

	void SaveRecent()
	{
		std::string t;
		for (auto& r : S().recent) t += r + "\n";
		S().host.WriteFile("CinematicStudio/recent.txt", t);
	}

	void TouchRecent(const std::string& name)
	{
		auto& r = S().recent;
		for (size_t i = 0; i < r.size(); i++) if (r[i] == name) { r.erase(r.begin() + static_cast<long>(i)); break; }
		r.insert(r.begin(), name);
		if (r.size() > 8) r.resize(8);
		SaveRecent();
	}

	void LoadHotkeys()
	{
		std::string text;
		if (!S().host.ReadFile("CinematicStudio/hotkeys.cfg", text))
		{
			std::string out = "// JK BUILD Cinematic Studio hotkeys: <action> <key>, CTRL+<key> for combinations.\n"
				"// Keys: A-Z 0-9 F1-F12 TAB SPACE ENTER HOME END DEL INS PGUP PGDN LEFT RIGHT UP DOWN COMMA PERIOD MINUS PLUS\n"
				"// Flying (hold the right mouse button): W A S D, E up, Q down, SHIFT faster, wheel = field of view.\n";
			for (auto& k : g_keys) out += Fmt("%-14s %s%s\n", k.name, k.ctrl ? "CTRL+" : "", KeyToName(k.key).c_str());
			S().host.WriteFile("CinematicStudio/hotkeys.cfg", out);
			return;
		}
		size_t p = 0;
		while (p < text.size())
		{
			size_t e = text.find('\n', p);
			if (e == std::string::npos) e = text.size();
			std::string line = text.substr(p, e - p);
			p = e + 1;
			if (line.size() < 3 || line[0] == '/' || line[0] == '#') continue;
			char a[64] = { 0 }, k[64] = { 0 };
			if (sscanf(line.c_str(), "%63s %63s", a, k) != 2) continue;
			std::string key = k;
			bool ctrl = false;
			if (key.size() > 5 && (key.compare(0, 5, "CTRL+") == 0 || key.compare(0, 5, "ctrl+") == 0)) { ctrl = true; key = key.substr(5); }
			const int code = NameToKey(key);
			for (auto& hk : g_keys)
				if (!Q_stricmp(hk.name, a) && code >= 0) { hk.key = code; hk.ctrl = ctrl; }
		}
	}

	void LoadRecent()
	{
		std::string t;
		S().recent.clear();
		if (!S().host.ReadFile("CinematicStudio/recent.txt", t)) return;
		size_t p = 0;
		while (p < t.size())
		{
			size_t e = t.find('\n', p);
			if (e == std::string::npos) e = t.size();
			const std::string n = t.substr(p, e - p);
			if (Scene::ValidName(n)) S().recent.push_back(n);
			p = e + 1;
		}
	}

	void SetFreeze(const bool on)
	{
		cgi_Cvar_Set("cl_paused", on ? "1" : "0");
	}

	vmCvar_t g_clPaused, g_firstRun;

	bool Frozen()
	{
		cgi_Cvar_Update(&g_clPaused);
		return g_clPaused.integer != 0;
	}

	void ViewVectors(float fwd[3], float right[3], float up[3])
	{
		AngleVectors(cg.refdefViewAngles, fwd, right, up);
	}

	// where a new actor goes: on the floor in front of the view
	void SpotInFront(float out[3], float& yaw)
	{
		vec3_t fwd, right, up, hit;
		ViewVectors(fwd, right, up);
		const bool hitWall = TraceFromView(cg.refdef.vieworg, fwd, 256, hit);
		if (hitWall) VectorMA(hit, -24, fwd, hit);
		const vec3_t down = { 0, 0, -1 };
		vec3_t floor;
		if (TraceFromView(hit, down, 512, floor)) VectorCopy(floor, hit);
		hit[2] += 25;	// NPC origin sits 24 above the feet
		VectorCopy(hit, out);
		yaw = AngleNormalize180(cg.refdefViewAngles[YAW] + 180.0f);	// facing the camera
	}

	void SpawnActor(const std::string& type)
	{
		Studio& s = S();
		float o[3], yaw;
		SpotInFront(o, yaw);
		const float a[3] = { 0, yaw, 0 };
		if (s.scene.AddActor(type, o, a)) s.levelSel = -1;
	}

	void AddCameraFromView()
	{
		Studio& s = S();
		s.scene.AddCamera(cg.refdef.vieworg, cg.refdefViewAngles, cg.refdef.fov_x);
	}

	void StopLooking()
	{
		Studio& s = S();
		s.lookId = 0;
		s.pilotId = 0;
	}

	void PlayerView()
	{
		Studio& s = S();
		StopLooking();
		s.camActive = false;
	}

	void StartFreeCam()
	{
		Studio& s = S();
		if (!s.camActive)
		{
			VectorCopy(cg.refdef.vieworg, s.camOrg);
			VectorCopy(cg.refdefViewAngles, s.camAng);
			s.camAng[ROLL] = 0;
			s.camFov = cg.refdef.fov_x;
		}
		s.camActive = true;
	}

	void LookThrough(const int id, const bool pilot)
	{
		Studio& s = S();
		const Object* o = s.scene.Find(id);
		if (!o || o->type != ObjType::Camera) return;
		s.lookId = id;
		s.pilotId = pilot ? id : 0;
		s.camActive = true;
		VectorCopy(o->origin, s.camOrg);
		VectorCopy(o->angles, s.camAng);
		s.camFov = o->fov;
	}

	void Focus()
	{
		Studio& s = S();
		const Object* o = s.scene.Find(s.scene.selected);
		vec3_t target;
		EntInfo info;
		if (o) VectorCopy(o->origin, target);
		else if (s.levelSel >= 0 && EntityInfo(s.levelSel, info)) VectorCopy(info.origin, target);
		else return;
		if (o && o->type == ObjType::Camera) { LookThrough(o->id, false); return; }
		StopLooking();
		StartFreeCam();
		// back off along the current view direction, slightly above
		vec3_t fwd, right, up;
		AngleVectors(s.camAng, fwd, right, up);
		VectorMA(target, -110, fwd, s.camOrg);
		s.camOrg[2] += 18;
		vec3_t d;
		VectorSubtract(target, s.camOrg, d);
		vectoangles(d, s.camAng);
	}

	void Nudge(const float dx, const float dy, const float dz, const float dyaw)
	{
		Studio& s = S();
		const Object* o = s.scene.Find(s.scene.selected);
		if (!o) return;
		const float step = s.shift ? 16.0f : (s.ctrl ? 1.0f : 4.0f);
		const float yaw = DEG2RAD(cg.refdefViewAngles[YAW]);
		const float fx = cosf(yaw), fy = sinf(yaw);		// view forward on the floor
		float org[3] = { o->origin[0], o->origin[1], o->origin[2] };
		float ang[3] = { o->angles[0], o->angles[1], o->angles[2] };
		org[0] += (fx * dy + fy * dx) * step;
		org[1] += (fy * dy - fx * dx) * step;
		org[2] += dz * step;
		ang[YAW] = AngleNormalize180(ang[YAW] + dyaw * (s.shift ? 15.0f : 5.0f));
		s.scene.SetTransform(o->id, org, ang, true);
		if (s.pilotId == o->id || s.lookId == o->id) { VectorCopy(org, s.camOrg); VectorCopy(ang, s.camAng); }
	}

	void Save(const std::string& name)
	{
		Studio& s = S();
		if (s.scene.Save(name)) { s.projectName = name; TouchRecent(name); }
	}

	void Load(const std::string& name, const bool force, const bool autosave)
	{
		Studio& s = S();
		std::vector<std::string> warn;
		PlayerView();
		s.levelSel = -1;
		if (s.scene.Load(name, force, autosave, warn)) { s.projectName = name; TouchRecent(name); }
	}

	void BeginField(const int f, const std::string& init)
	{
		S().field = f;
		S().fieldText = init;
	}

	void CommitField()
	{
		Studio& s = S();
		const int f = s.field;
		const std::string t = s.fieldText;
		s.field = FIELD_NONE;
		if (f == FIELD_ANIMSEARCH) { s.animSearch = t; s.animScroll = 0; return; }
		if (t.empty()) return;
		if (f == FIELD_NPCTYPE) { ReleasePreview(); s.scene.SetNpcType(s.scene.selected, t); s.animsEnt = -1; return; }
		if (f == FIELD_NPC) { s.npcType = t; SpawnActor(t); }
		else if (f == FIELD_PROJECT) s.projectName = t;
		else if (f == FIELD_RENAME) s.scene.Rename(s.scene.selected, t);
	}

	// ------------------------------------------------------------------------------------------
	// panels
	// ------------------------------------------------------------------------------------------

	void DrawField(const float x, const float y, const float w, const int which, const std::string& value)
	{
		Studio& s = S();
		const bool editing = s.field == which;
		Rect(x, y, w, 11, kPanelDark);
		Frame(x, y, w, 11, editing ? kAccent : kLine);
		std::string t = editing ? s.fieldText : value;
		if (editing && (NowMs() / 400) % 2) t += "_";
		TextClip(x + 2, y + 1.5f, t, w - 4, editing ? kAccent : kTextCol);
		if (TakeClick(x, y, w, 11)) BeginField(which, value);
	}

	void DrawTitle()
	{
		Studio& s = S();
		Rect(0, 0, kW, kTitleH, kPanelDark);
		Text(4, 2, "JK BUILD - CINEMATIC STUDIO", kAccent);
		const std::string proj = s.scene.project.empty() ? "unsaved scene" : s.scene.project;
		const std::string mid = proj + (s.scene.dirty ? " *" : "") + "    map: " + s.host.MapName();
		Text(200, 2, mid, kDim);
		if (Button(kW - 82, 1, 40, 11, "SAVE"))
		{
			if (!s.scene.project.empty()) Save(s.scene.project);
			else { s.tab = TAB_PROJECT; BeginField(FIELD_PROJECT, s.projectName); }
		}
		if (Button(kW - 40, 1, 38, 11, "X")) Close();

		Rect(0, kTabsY, kW, kTabsH, kPanel);
		float x = 4;
		for (int t = 0; t < TAB_COUNT; t++)
		{
			const float w = TextW(kTabNames[t]) + 12;
			if (Button(x, kTabsY + 1, w, kTabsH - 2, kTabNames[t], s.tab == t)) s.tab = t;
			x += w + 2;
		}
		const bool frozen = Frozen();
		if (Button(kW - 82, kTabsY + 1, 80, kTabsH - 2, frozen ? "WORLD FROZEN" : "FREEZE WORLD", frozen)) SetFreeze(!frozen);
	}

	void DrawOutliner()
	{
		Studio& s = S();
		const float x = 0, y = kPanelY, w = kLeftW, h = kTransportY - kPanelY;
		Rect(x, y, w, h, kPanel);
		Text(x + 4, y + 2, "SCENE OUTLINER", kAccent);
		float ry = y + 14;
		const float listBottom = y + h - 26;
		int shown = 0;
		for (auto& o : s.scene.objects)
		{
			if (ry + kRow > listBottom) break;
			const bool sel = s.scene.selected == o.id;
			if (sel) Rect(x + 1, ry, w - 2, kRow, kSel);
			else if (Inside(s.cx, s.cy, x, ry, w, kRow)) Rect(x + 1, ry, w - 2, kRow, kHover);
			const bool cam = o.type == ObjType::Camera;
			Text(x + 4, ry + 1, cam ? "C" : "A", cam ? kCam : kAccent);
			const bool missing = !cam && o.entNum < 0;
			TextClip(x + 14, ry + 1, o.name + (missing ? "  (missing)" : ""), w - 18, missing ? kWarn : kTextCol);
			if (TakeClick(x, ry, w, kRow))
			{
				s.scene.selected = o.id;
				s.levelSel = -1;
				if (s.doubleClick) Focus();
			}
			ry += kRow;
			shown++;
		}
		if (!shown) TextClip(x + 4, ry + 1, "empty: add actors and cameras", w - 8, kDim);
		const float by = y + h - 24;
		if (Button(x + 3, by, 39, 11, "+ACTOR")) { s.tab = TAB_ACTORS; BeginField(FIELD_NPC, s.npcType); }
		if (Button(x + 44, by, 40, 11, "+CAMERA")) AddCameraFromView();
		if (Button(x + 86, by, 39, 11, "DELETE", false, s.scene.selected != 0)) s.scene.Remove(s.scene.selected);
		if (Button(x + 3, by + 12, 60, 11, "UNDO", false, s.scene.CanUndo())) s.scene.Undo();
		if (Button(x + 65, by + 12, 60, 11, "REDO", false, s.scene.CanRedo())) s.scene.Redo();
	}

	void DrawActorInspector(const float x, float y, Object* o, const int ent)
	{
		Studio& s = S();
		EntInfo info;
		const bool live = EntityInfo(o ? o->entNum : ent, info);
		if (o)
		{
			Label(x, y, "name", o->name); y += kRow;
			if (Button(x + kRightW - 50, y - kRow, 38, 9, "RENAME")) BeginField(FIELD_RENAME, o->name);
			if (s.field == FIELD_RENAME) { DrawField(x, y, kRightW - 12, FIELD_RENAME, o->name); y += 13; }
		}
		Label(x, y, "npc", live ? info.npcType : (o ? o->npcType : "")); y += kRow;
		if (!live) { Text(x, y, "not in the world (NPC type missing?)", kWarn); return; }
		Label(x, y, "entity", Fmt("%d%s", info.ent, info.isPlayer ? " (player)" : "")); y += kRow;
		Label(x, y, "model", info.model); y += kRow;
		Label(x, y, "profile", info.animProfile); y += kRow;
		Label(x, y, "position", Fmt("%.0f %.0f %.0f", info.origin[0], info.origin[1], info.origin[2])); y += kRow;
		Label(x, y, "yaw", Fmt("%.0f", info.yaw)); y += kRow;
		Label(x, y, "health", Fmt("%d", info.health)); y += kRow;
		Label(x, y, "legs", info.legsAnim); y += kRow;
		Label(x, y, "torso", info.torsoAnim); y += kRow + 2;
		if (o)
		{
			const float bw = (kRightW - 16) / 3;
			if (Button(x, y, bw, 11, o->ai ? "AI: ON" : "AI: OFF", o->ai)) s.scene.SetAI(o->id, !o->ai);
			if (Button(x + bw + 2, y, bw, 11, "FOCUS")) Focus();
			if (Button(x + 2 * (bw + 2), y, bw, 11, "DUPLICATE")) s.scene.Duplicate(o->id);
			y += 13;
			if (s.field == FIELD_NPCTYPE) { DrawField(x, y, kRightW - 12, FIELD_NPCTYPE, o->npcType); y += 13; }
			else if (Button(x, y, kRightW - 12, 11, "CHANGE NPC TYPE", false, !o->adopted)) BeginField(FIELD_NPCTYPE, o->npcType), y += 13;
			else y += 13;
			if (Button(x, y, kRightW - 12, 11, "ANIMATIONS...")) s.tab = TAB_ANIMS;
			y += 14;
			Text(x, y, "drag in view: move  CTRL+drag: turn", kDim); y += kRow;
			Text(x, y, "arrows/PgUp/PgDn, Q/E (SHIFT/CTRL)", kDim);
		}
		else if (!info.isPlayer)
		{
			if (Button(x, y, kRightW - 12, 11, "ADD TO SCENE"))
				s.levelSel = -1, s.scene.AdoptActor(info.ent, info.npcType, "");
		}
	}

	void DrawCameraInspector(const float x, float y, Object* o)
	{
		Studio& s = S();
		Label(x, y, "name", o->name); y += kRow;
		if (Button(x + kRightW - 50, y - kRow, 38, 9, "RENAME")) BeginField(FIELD_RENAME, o->name);
		if (s.field == FIELD_RENAME) { DrawField(x, y, kRightW - 12, FIELD_RENAME, o->name); y += 13; }
		Label(x, y, "position", Fmt("%.0f %.0f %.0f", o->origin[0], o->origin[1], o->origin[2])); y += kRow;
		Label(x, y, "angles", Fmt("%.0f %.0f %.0f", o->angles[0], o->angles[1], o->angles[2])); y += kRow;
		Label(x, y, "fov", Fmt("%.0f", o->fov));
		if (Button(x + 90, y, 18, 9, "-")) { o->fov = std::max(10.0f, o->fov - 5); s.scene.dirty = true; if (s.lookId == o->id) s.camFov = o->fov; }
		if (Button(x + 110, y, 18, 9, "+")) { o->fov = std::min(130.0f, o->fov + 5); s.scene.dirty = true; if (s.lookId == o->id) s.camFov = o->fov; }
		y += kRow + 3;
		if (Button(x, y, 66, 11, "LOOK THROUGH", s.lookId == o->id && s.pilotId != o->id)) LookThrough(o->id, false);
		if (Button(x + 70, y, 66, 11, "PILOT", s.pilotId == o->id)) LookThrough(o->id, true);
		y += 13;
		if (Button(x, y, 66, 11, "SET FROM VIEW"))
			s.scene.SetTransform(o->id, cg.refdef.vieworg, cg.refdefViewAngles, false), o->fov = cg.refdef.fov_x;
		if (Button(x + 70, y, 66, 11, "PLAYER VIEW")) PlayerView();
		y += 14;
		Text(x, y, "PILOT: hold right mouse + WASD", kDim);
	}

	// ------------------------------------------------------------------------------------------
	// Phase 2: animations, clips, profiles, dragging
	// ------------------------------------------------------------------------------------------

	// the character the animation tools work on: the selected scene actor or level character
	int SelectedCharacter()
	{
		Studio& s = S();
		if (const Object* o = s.scene.Find(s.scene.selected)) return o->type == ObjType::Actor ? o->entNum : -1;
		return s.levelSel;
	}

	void Hold(const int ent)
	{
		auto& h = S().heldEnts;
		for (const int e : h) if (e == ent) return;
		h.push_back(ent);
	}

	void ReleaseAllHeld()
	{
		Studio& s = S();
		for (const int e : s.heldEnts) ReleaseAnim(e);
		s.heldEnts.clear();
		s.previewEnt = -1;
	}

	void RefreshAnims(const int ent)
	{
		Studio& s = S();
		const std::string prof = ent >= 0 ? CurrentProfile(ent) : "";
		if (ent == s.animsEnt && prof == s.animsProfile) return;
		s.animsEnt = ent;
		s.animsProfile = prof;
		s.anims.clear();
		if (ent >= 0) ListAnims(ent, s.anims);
		s.animScroll = 0;
		s.profileChecked = false;
	}

	const AnimInfo* BrowseInfo()
	{
		Studio& s = S();
		for (auto& a : s.anims) if (a.anim == s.browseAnim) return &a;
		return nullptr;
	}

	void StartPreview(const bool frozen)
	{
		Studio& s = S();
		const int ent = SelectedCharacter();
		const AnimInfo* ai = BrowseInfo();
		if (ent < 0 || !ai) return;
		s.scene.clock.Pause();
		const int last = s.rangeEnd < 0 ? ai->numFrames - 1 : std::min(s.rangeEnd, ai->numFrames - 1);
		s.previewFrame = std::max(static_cast<float>(s.rangeStart), std::min(static_cast<float>(last), s.previewFrame));
		PoseAnim(ent, ai->anim, s.parts, frozen ? s.previewFrame : -1.0f, frozen ? 0.0f : s.animSpeed, s.animLoop,
			s.rangeStart, s.rangeEnd, frozen ? 0 : s.blendMs);
		s.previewEnt = ent;
		s.previewFrozen = frozen;
		Hold(ent);
	}

	void ReleasePreview()
	{
		Studio& s = S();
		if (s.previewEnt >= 0) ReleaseAnim(s.previewEnt);
		s.previewEnt = -1;
	}

	void AddClipAtPlayhead()
	{
		Studio& s = S();
		const Object* o = s.scene.Find(s.scene.selected);
		const AnimInfo* ai = BrowseInfo();
		if (!o || o->type != ObjType::Actor) { s.host.Log(1, "Add the character to the scene first (ADD TO SCENE)"); return; }
		if (!ai) { s.host.Log(1, "Pick an animation first"); return; }
		AnimClip c;
		c.anim = ai->name;
		c.parts = s.parts;
		c.speed = s.animSpeed;
		c.loop = s.animLoop;
		c.startFrame = s.rangeStart;
		c.endFrame = s.rangeEnd;
		const int last = c.endFrame < 0 ? ai->numFrames - 1 : std::min(c.endFrame, ai->numFrames - 1);
		const double frames = std::max(1, last - c.startFrame + 1);
		c.lengthUs = static_cast<int64_t>(frames * ai->frameLerp * 1000.0 / std::max(0.05f, c.speed));
		c.startUs = s.scene.clock.timeUs;
		ReleasePreview();
		if (s.scene.AddClip(o->id, c) >= 0) s.host.Log(0, "Clip " + c.anim + " placed at " + s.scene.clock.Timecode());
	}

	// relative frame of a clip at a time into it
	float ClipFrame(const AnimClip& c, const AnimInfo& ai, const int64_t localUs, const bool holding, int& sf, int& ef)
	{
		sf = std::max(0, std::min(c.startFrame, ai.numFrames - 1));
		ef = c.endFrame < 0 ? ai.numFrames - 1 : std::max(sf, std::min(c.endFrame, ai.numFrames - 1));
		const float frames = static_cast<float>(ef - sf + 1);
		if (holding) return static_cast<float>(ef);
		float f = static_cast<float>(static_cast<double>(localUs) / 1000.0 * c.speed / ai.frameLerp);
		if (c.loop) f = fmodf(f, frames);
		else f = std::min(f, frames - 1);
		return static_cast<float>(sf) + std::max(0.0f, f);
	}

	// timeline clips drive their actors (exactly, frame by frame) while the studio is open
	void EvaluateClips()
	{
		Studio& s = S();
		const int64_t t = s.scene.clock.timeUs;
		for (auto& o : s.scene.objects)
		{
			if (o.type != ObjType::Actor || o.entNum < 0 || o.clips.empty() || o.entNum == s.previewEnt) continue;
			int64_t local = 0;
			bool holding = false;
			const AnimClip* c = Scene::ActiveClip(o, t, local, holding);
			if (!c)
			{
				// before the first clip the game animates the actor
				for (size_t i = 0; i < s.heldEnts.size(); i++)
					if (s.heldEnts[i] == o.entNum) { ReleaseAnim(o.entNum); s.heldEnts.erase(s.heldEnts.begin() + static_cast<long>(i)); break; }
				continue;
			}
			AnimInfo ai;
			if (!AnimInfoFor(o.entNum, AnimByName(c->anim), ai)) continue;	// this skeleton lacks the animation
			int sf, ef;
			const float f = ClipFrame(*c, ai, local, holding, sf, ef);
			PoseAnim(o.entNum, ai.anim, c->parts, f, 0.0f, false, sf, ef, 0);
			Hold(o.entNum);
		}
	}

	void LoadProfiles()
	{
		Studio& s = S();
		if (s.profiles.empty()) s.profiles = ListProfiles();
	}

	void DrawAnimPanel(const float x, float y, const float h)
	{
		Studio& s = S();
		const float ix = x + 6, w = kRightW - 12, yEnd = y + h;
		const int ent = SelectedCharacter();
		EntInfo info;
		if (ent < 0 || !EntityInfo(ent, info))
		{
			Text(ix, y + 2, "ANIMATIONS", kAccent);
			Text(ix, y + 14, "Select an actor first.", kDim);
			return;
		}
		RefreshAnims(ent);
		const Object* obj = s.scene.FindByEntity(ent);
		TextClip(ix, y + 2, "ANIMATIONS - " + (obj ? obj->name : info.npcType), w, kAccent);
		y += 13;

		// ---- profile ----
		LoadProfiles();
		const std::string cur = CurrentProfile(ent);
		Label(ix, y, "profile", cur, w); y += kRow;
		if (s.profilePick < 0 || s.profilePick >= static_cast<int>(s.profiles.size()))
		{
			s.profilePick = 0;
			for (size_t i = 0; i < s.profiles.size(); i++) if (!Q_stricmp(s.profiles[i].c_str(), cur.c_str())) s.profilePick = static_cast<int>(i);
			s.profileChecked = false;
		}
		if (!s.profiles.empty())
		{
			const std::string pick = s.profiles[static_cast<size_t>(s.profilePick)];
			if (Button(ix, y, 12, 10, "<")) { s.profilePick = (s.profilePick + static_cast<int>(s.profiles.size()) - 1) % static_cast<int>(s.profiles.size()); s.profileChecked = false; }
			TextClip(ix + 15, y + 1, pick, w - 92, kTextCol);
			if (Button(ix + w - 76, y, 12, 10, ">")) { s.profilePick = (s.profilePick + 1) % static_cast<int>(s.profiles.size()); s.profileChecked = false; }
			if (!s.profileChecked)
			{
				s.profileWhy.clear();
				if (!CheckProfile(ent, s.profiles[static_cast<size_t>(s.profilePick)], s.profileWhy) && s.profileWhy.empty()) s.profileWhy = "not compatible";
				s.profileChecked = true;
			}
			const bool ok = s.profileWhy.empty();
			if (Button(ix + w - 62, y, 32, 10, "APPLY", false, ok && Q_stricmp(pick.c_str(), cur.c_str()) && obj))
			{
				ReleasePreview();
				if (s.scene.SetProfile(obj->id, pick)) { s.host.Log(0, obj->name + " now uses " + pick); s.animsEnt = -1; }
			}
			if (Button(ix + w - 28, y, 28, 10, "OWN", false, obj && !obj->profile.empty()))
			{
				ReleasePreview();
				if (s.scene.SetProfile(obj->id, "")) s.animsEnt = -1;
			}
			y += 11;
			if (ok) TextClip(ix, y, "compatible skeleton", w, kDim);
			else
			{
				// force skips the checks: the actor may pose wrongly if the skeletons really differ
				TextClip(ix, y, s.profileWhy, w - 40, kRed);
				if (Button(ix + w - 38, y - 1, 38, 10, "FORCE", false, obj && Q_stricmp(pick.c_str(), cur.c_str())))
				{
					ReleasePreview();
					if (s.scene.SetProfile(obj->id, pick, true)) { s.host.Log(1, obj->name + " forced to " + pick + " (no skeleton checks)"); s.animsEnt = -1; }
				}
			}
			if (!obj) TextClip(ix + 90, y, "(add to scene to switch)", w - 90, kDim);
			y += kRow;
		}

		// ---- search + list ----
		DrawField(ix, y, w, FIELD_ANIMSEARCH, s.animSearch);
		if (s.field != FIELD_ANIMSEARCH && s.animSearch.empty()) Text(ix + 3, y + 1.5f, "search animations", kDim);
		y += 13;
		const std::string filter = s.field == FIELD_ANIMSEARCH ? s.fieldText : s.animSearch;
		std::vector<const AnimInfo*> shown;
		for (auto& a : s.anims)
		{
			if (!filter.empty())
			{
				std::string n = a.name, f = filter;
				for (auto& c : n) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
				for (auto& c : f) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
				if (n.find(f) == std::string::npos) continue;
			}
			shown.push_back(&a);
		}
		const float listH = 9 * 9.0f;
		const int rows = static_cast<int>(listH / 9);
		Rect(ix, y, w, listH, kPanelDark);
		if (Inside(s.cx, s.cy, ix, y, w, listH)) { s.animScroll -= static_cast<float>(s.wheel) * 3; s.wheel = 0; }
		s.animScroll = std::max(0.0f, std::min(s.animScroll, static_cast<float>(std::max(0, static_cast<int>(shown.size()) - rows))));
		for (int r = 0; r < rows; r++)
		{
			const int i = static_cast<int>(s.animScroll) + r;
			if (i >= static_cast<int>(shown.size())) break;
			const AnimInfo* a = shown[static_cast<size_t>(i)];
			const float ry = y + static_cast<float>(r) * 9;
			if (a->anim == s.browseAnim) Rect(ix, ry, w, 9, kSel);
			else if (Inside(s.cx, s.cy, ix, ry, w, 9)) Rect(ix, ry, w, 9, kHover);
			TextClip(ix + 2, ry + 0.5f, a->name + (a->extra ? "  *" : ""), w - 34, a->extra ? kAccent : kTextCol);
			Text(ix + w - 30, ry + 0.5f, Fmt("%d", a->numFrames), kDim, 0.36f);
			if (TakeClick(ix, ry, w, 9))
			{
				if (s.browseAnim != a->anim) { s.browseAnim = a->anim; s.rangeStart = 0; s.rangeEnd = -1; s.previewFrame = 0; }
				if (s.doubleClick) StartPreview(false);
			}
		}
		y += listH + 2;
		TextClip(ix, y, Fmt("%d of %d animations", static_cast<int>(shown.size()), static_cast<int>(s.anims.size())), w, kDim);
		y += kRow;

		const AnimInfo* ai = BrowseInfo();
		if (!ai) { Text(ix, y, "Pick an animation (double-click plays)", kDim); return; }
		const int last = s.rangeEnd < 0 ? ai->numFrames - 1 : std::min(s.rangeEnd, ai->numFrames - 1);
		TextClip(ix, y, Fmt("%d frames  %.0f fps  %.2fs%s", ai->numFrames, 1000.0f / static_cast<float>(ai->frameLerp),
			static_cast<float>(ai->numFrames * ai->frameLerp) / 1000.0f, ai->loops ? "  loops" : ""), w, kTextCol);
		y += kRow;

		// parts / loop
		if (Button(ix, y, 34, 10, "BOTH", s.parts == PARTS_BOTH)) s.parts = PARTS_BOTH;
		if (Button(ix + 36, y, 34, 10, "LEGS", s.parts == PARTS_LEGS)) s.parts = PARTS_LEGS;
		if (Button(ix + 72, y, 34, 10, "TORSO", s.parts == PARTS_TORSO)) s.parts = PARTS_TORSO;
		if (Button(ix + 108, y, w - 108, 10, "LOOP", s.animLoop)) s.animLoop = !s.animLoop;
		y += 12;
		// speed / blend
		Text(ix, y + 1, Fmt("speed %.2f", s.animSpeed), kTextCol);
		if (Button(ix + 50, y, 12, 10, "-")) s.animSpeed = std::max(0.05f, s.animSpeed - (s.animSpeed > 1.0f ? 0.25f : 0.05f));
		if (Button(ix + 64, y, 12, 10, "+")) s.animSpeed = std::min(4.0f, s.animSpeed + (s.animSpeed >= 1.0f ? 0.25f : 0.05f));
		Text(ix + 82, y + 1, Fmt("blend %d", s.blendMs), kTextCol);
		if (Button(ix + w - 26, y, 12, 10, "-")) s.blendMs = std::max(0, s.blendMs - 50);
		if (Button(ix + w - 12, y, 12, 10, "+")) s.blendMs = std::min(1000, s.blendMs + 50);
		y += 12;
		// range
		Text(ix, y + 1, Fmt("range %d-%d", s.rangeStart, last), kTextCol);
		if (Button(ix + 70, y, 16, 10, "<")) s.rangeStart = std::max(0, s.rangeStart - (s.shift ? 10 : 1));
		if (Button(ix + 88, y, 16, 10, ">")) s.rangeStart = std::min(last, s.rangeStart + (s.shift ? 10 : 1));
		if (Button(ix + w - 34, y, 16, 10, "<")) s.rangeEnd = std::max(s.rangeStart, last - (s.shift ? 10 : 1));
		if (Button(ix + w - 16, y, 16, 10, ">")) { const int ne = last + (s.shift ? 10 : 1); s.rangeEnd = ne >= ai->numFrames - 1 ? -1 : ne; }
		y += 12;
		// frame scrubber: click or drag poses the actor at that frame
		const float barH = 8;
		Rect(ix, y, w, barH, kPanelDark);
		Frame(ix, y, w, barH, kLine);
		const float span = static_cast<float>(std::max(1, ai->numFrames - 1));
		Rect(ix + w * static_cast<float>(s.rangeStart) / span, y, w * static_cast<float>(last - s.rangeStart) / span, barH, kBar);
		int curAnim = -1;
		float curFrame = 0;
		if (s.previewEnt == ent && CurrentFrame(ent, curAnim, curFrame) && curAnim == ai->anim)
			Rect(ix + w * std::max(0.0f, std::min(span, curFrame)) / span - 0.5f, y - 1, 1.5f, barH + 2, kRed);
		if (s.clicked && Inside(s.clickX, s.clickY, ix, y - 1, w, barH + 2)) { s.clicked = false; s.scrubAnim = true; }
		if (!s.lmb) s.scrubAnim = false;
		if (s.scrubAnim)
		{
			s.previewFrame = std::max(0.0f, std::min(span, (s.cx - ix) / w * span));
			s.previewFrame = floorf(s.previewFrame + 0.5f);
			StartPreview(true);
		}
		y += barH + 1;
		Text(ix, y, Fmt("frame %.0f", s.previewEnt == ent ? curFrame : s.previewFrame), kDim, 0.36f);
		y += 9;
		// actions
		const float bw = (w - 4) / 3;
		if (Button(ix, y, bw, 11, "PLAY", s.previewEnt == ent && !s.previewFrozen)) StartPreview(false);
		if (Button(ix + bw + 2, y, bw, 11, "FREEZE", s.previewEnt == ent && s.previewFrozen))
		{
			if (s.previewEnt == ent && CurrentFrame(ent, curAnim, curFrame)) s.previewFrame = floorf(curFrame + 0.5f);
			StartPreview(true);
		}
		if (Button(ix + 2 * (bw + 2), y, bw, 11, "RELEASE", false, s.previewEnt == ent)) ReleasePreview();
		y += 13;
		if (y + 11 <= yEnd && Button(ix, y, w, 11, "ADD CLIP AT PLAYHEAD", false, obj != nullptr)) AddClipAtPlayhead();
	}

	// the selected timeline clip, edited in the SCENE inspector
	bool DrawClipEditor(const float x, float y)
	{
		Studio& s = S();
		Object* o = s.scene.Find(s.scene.selected);
		if (!o || s.scene.selectedClip < 0 || s.scene.selectedClip >= static_cast<int>(o->clips.size())) return false;
		const int idx = s.scene.selectedClip;
		AnimClip c = o->clips[static_cast<size_t>(idx)];
		const float w = kRightW - 12;
		Text(x, y, "CLIP", kAccent);
		if (Button(x + w - 40, y - 1, 40, 10, "CLOSE")) { s.scene.selectedClip = -1; return false; }
		y += 11;
		Label(x, y, "anim", c.anim, w); y += kRow;
		Label(x, y, "actor", o->name, w); y += kRow;
		const double fps = s.scene.clock.fps;
		const int64_t frameUs = static_cast<int64_t>(1e6 / fps);
		bool changed = false;
		Label(x, y, "start", Fmt("%.2fs", static_cast<double>(c.startUs) / 1e6), w);
		if (Button(x + w - 28, y, 12, 9, "-")) { c.startUs -= s.shift ? frameUs * s.scene.clock.fps : frameUs; changed = true; }
		if (Button(x + w - 14, y, 12, 9, "+")) { c.startUs += s.shift ? frameUs * s.scene.clock.fps : frameUs; changed = true; }
		y += kRow;
		Label(x, y, "length", Fmt("%.2fs", static_cast<double>(c.lengthUs) / 1e6), w);
		if (Button(x + w - 28, y, 12, 9, "-")) { c.lengthUs -= s.shift ? frameUs * s.scene.clock.fps : frameUs; changed = true; }
		if (Button(x + w - 14, y, 12, 9, "+")) { c.lengthUs += s.shift ? frameUs * s.scene.clock.fps : frameUs; changed = true; }
		y += kRow;
		Label(x, y, "speed", Fmt("%.2f", c.speed), w);
		if (Button(x + w - 28, y, 12, 9, "-")) { c.speed = std::max(0.05f, c.speed - 0.05f); changed = true; }
		if (Button(x + w - 14, y, 12, 9, "+")) { c.speed = std::min(4.0f, c.speed + 0.05f); changed = true; }
		y += kRow;
		Label(x, y, "frames", Fmt("%d - %s", c.startFrame, c.endFrame < 0 ? "end" : std::to_string(c.endFrame).c_str()), w);
		y += kRow + 1;
		const char* pn = c.parts == PARTS_LEGS ? "LEGS" : (c.parts == PARTS_TORSO ? "TORSO" : "BOTH");
		if (Button(x, y, 50, 10, pn)) { c.parts = c.parts == PARTS_BOTH ? PARTS_LEGS : (c.parts == PARTS_LEGS ? PARTS_TORSO : PARTS_BOTH); changed = true; }
		if (Button(x + 52, y, 40, 10, "LOOP", c.loop)) { c.loop = !c.loop; changed = true; }
		if (Button(x + w - 48, y, 48, 10, "DELETE")) { s.scene.RemoveClip(o->id, idx); return true; }
		y += 13;
		Text(x, y, "drag clips on the timeline; SHIFT = 1s", kDim);
		if (changed) s.scene.SetClip(o->id, idx, c, true);
		return true;
	}

	// ---- moving actors with the mouse ----
	bool SelectedActorTransform(float org[3], float& yaw, int& id)
	{
		Studio& s = S();
		const Object* o = s.scene.Find(s.scene.selected);
		if (!o || o->type != ObjType::Actor || o->entNum < 0) return false;
		VectorCopy(o->origin, org);
		yaw = o->angles[YAW];
		id = o->id;
		return true;
	}

	bool RayToPlane(const float sx, const float sy, const float z, float hit[3])
	{
		vec3_t dir;
		ViewRay(sx, sy, dir);
		if (fabsf(dir[2]) < 1e-4f) return false;
		const float t = (z - cg.refdef.vieworg[2]) / dir[2];
		if (t <= 0 || t > 16384) return false;
		VectorMA(cg.refdef.vieworg, t, dir, hit);
		return true;
	}

	// a click on the selected actor (inside its bracket) starts a move; CTRL+drag turns it
	bool TryStartDrag()
	{
		Studio& s = S();
		float org[3], yaw;
		int id;
		EntInfo info;
		if (!SelectedActorTransform(org, yaw, id) || !EntityInfo(s.scene.Find(id)->entNum, info)) return false;
		float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
		int n = 0;
		for (int c = 0; c < 8; c++)
		{
			const vec3_t p = { (c & 1) ? info.maxs[0] : info.mins[0], (c & 2) ? info.maxs[1] : info.mins[1], (c & 4) ? info.maxs[2] : info.mins[2] };
			float sx, sy;
			if (!Project(p, sx, sy)) continue;
			x0 = std::min(x0, sx); y0 = std::min(y0, sy); x1 = std::max(x1, sx); y1 = std::max(y1, sy);
			n++;
		}
		if (!n || !Inside(s.clickX, s.clickY, x0 - 4, y0 - 4, x1 - x0 + 8, y1 - y0 + 8)) return false;
		s.dragging = true;
		s.dragTurn = s.ctrl;
		s.dragZ = org[2];
		s.dragStartX = s.clickX;
		s.dragStartYaw = yaw;
		float hit[3];
		if (RayToPlane(s.clickX, s.clickY, s.dragZ, hit)) { s.dragOff[0] = org[0] - hit[0]; s.dragOff[1] = org[1] - hit[1]; }
		else s.dragOff[0] = s.dragOff[1] = 0;
		return true;
	}

	void UpdateDrag()
	{
		Studio& s = S();
		if (!s.dragging) return;
		if (!s.lmb) { s.dragging = false; return; }
		float org[3], yaw;
		int id;
		if (!SelectedActorTransform(org, yaw, id)) { s.dragging = false; return; }
		float ang[3] = { 0, yaw, 0 };
		if (s.dragTurn) ang[YAW] = AngleNormalize180(s.dragStartYaw - (s.cx - s.dragStartX) * 1.5f);
		else
		{
			float hit[3];
			if (!RayToPlane(s.cx, s.cy, s.dragZ, hit)) return;
			org[0] = hit[0] + s.dragOff[0];
			org[1] = hit[1] + s.dragOff[1];
		}
		if (fabsf(org[0] - s.scene.Find(id)->origin[0]) > 0.01f || fabsf(org[1] - s.scene.Find(id)->origin[1]) > 0.01f ||
			fabsf(ang[YAW] - yaw) > 0.01f)
			s.scene.SetTransform(id, org, ang, true);
	}

	// ring at the selected actor's feet and its facing
	void DrawGizmo()
	{
		Studio& s = S();
		float org[3], yaw;
		int id;
		if (!SelectedActorTransform(org, yaw, id)) return;
		const vec4_t col = { kAccent[0], kAccent[1], kAccent[2], s.dragging ? 1.0f : 0.7f };
		const float z = org[2] - 23.0f, r = 20.0f;
		for (int i = 0; i < 32; i++)
		{
			const float a = static_cast<float>(i) * static_cast<float>(M_PI) * 2.0f / 32.0f;
			const vec3_t p = { org[0] + cosf(a) * r, org[1] + sinf(a) * r, z };
			float sx, sy;
			if (Project(p, sx, sy)) Rect(sx - 0.75f, sy - 0.75f, 1.5f, 1.5f, col);
		}
		const float yr = DEG2RAD(yaw);
		for (int i = 1; i <= 8; i++)
		{
			const float d = r + static_cast<float>(i) * 2.5f;
			const vec3_t p = { org[0] + cosf(yr) * d, org[1] + sinf(yr) * d, z };
			float sx, sy;
			if (Project(p, sx, sy)) Rect(sx - 1, sy - 1, 2, 2, col);
		}
	}

	void DrawRightPanel()
	{
		Studio& s = S();
		const float x = kRightX, y0 = kPanelY, h = kTransportY - kPanelY;
		Rect(x, y0, kRightW, h, kPanel);
		const float ix = x + 6;
		float y = y0 + 2;
		Object* sel = s.scene.Find(s.scene.selected);

		switch (s.tab)
		{
		case TAB_SCENE:
		default:
			if (DrawClipEditor(ix, y)) break;
			Text(ix, y, "INSPECTOR", kAccent); y += 12;
			if (sel && sel->type == ObjType::Camera) DrawCameraInspector(ix, y, sel);
			else if (sel) DrawActorInspector(ix, y, sel, -1);
			else if (s.levelSel >= 0) { Text(ix, y, "level character (not in scene)", kDim); DrawActorInspector(ix, y + 12, nullptr, s.levelSel); }
			else
			{
				Text(ix, y, "Click an actor or camera in the", kDim); y += kRow;
				Text(ix, y, "outliner or the viewport.", kDim); y += kRow * 2;
				Text(ix, y, "Right mouse + WASD: fly", kDim); y += kRow;
				Text(ix, y, "TAB: hide panels   F10: close", kDim);
			}
			break;

		case TAB_ACTORS:
		{
			Text(ix, y, "ADD ACTOR (NPC type)", kAccent); y += 12;
			DrawField(ix, y, kRightW - 12, FIELD_NPC, s.npcType); y += 13;
			if (Button(ix, y, kRightW - 12, 11, "SPAWN IN FRONT OF VIEW")) { s.field = FIELD_NONE; SpawnActor(s.npcType); }
			y += 16;
			Text(ix, y, "LEVEL CHARACTERS", kAccent); y += 12;
			int ents[64];
			const int n = ListCharacters(ents, 64);
			for (int i = 0; i < n && y < y0 + h - kRow; i++)
			{
				EntInfo info;
				if (!EntityInfo(ents[i], info)) continue;
				const Object* inScene = s.scene.FindByEntity(ents[i]);
				const bool sel2 = (inScene && inScene->id == s.scene.selected) || s.levelSel == ents[i];
				if (sel2) Rect(x + 1, y, kRightW - 2, kRow, kSel);
				TextClip(ix, y + 1, Fmt("%s%s", info.isPlayer ? "player" : info.npcType.c_str(), inScene ? "  (in scene)" : ""),
					kRightW - 12, inScene ? kDim : kTextCol);
				if (TakeClick(x, y, kRightW, kRow))
				{
					if (inScene) { s.scene.selected = inScene->id; s.levelSel = -1; }
					else { s.scene.selected = 0; s.levelSel = ents[i]; s.tab = TAB_SCENE; }
				}
				y += kRow;
			}
			break;
		}

		case TAB_CAMERA:
		{
			Text(ix, y, "CAMERAS", kAccent); y += 12;
			if (Button(ix, y, 66, 11, "ADD FROM VIEW")) AddCameraFromView();
			if (Button(ix + 70, y, 66, 11, "PLAYER VIEW", !s.camActive)) PlayerView();
			y += 14;
			if (Button(ix, y, 66, 11, "FREE CAMERA", s.camActive && !s.lookId)) { StopLooking(); StartFreeCam(); }
			y += 16;
			for (auto& o : s.scene.objects)
			{
				if (o.type != ObjType::Camera || y > y0 + h - kRow) continue;
				const bool sel2 = o.id == s.scene.selected;
				if (sel2) Rect(x + 1, y, kRightW - 2, kRow, kSel);
				TextClip(ix, y + 1, o.name + (s.lookId == o.id ? "  [viewing]" : ""), kRightW - 12, s.lookId == o.id ? kCam : kTextCol);
				if (TakeClick(x, y, kRightW, kRow)) { s.scene.selected = o.id; LookThrough(o.id, false); }
				y += kRow;
			}
			y += 6;
			Text(ix, y, "Camera paths and cuts: Phase 6", kDim);
			break;
		}

		case TAB_PROJECT:
		{
			Text(ix, y, "PROJECT", kAccent); y += 12;
			DrawField(ix, y, kRightW - 12, FIELD_PROJECT, s.projectName.empty() ? s.scene.project : s.projectName); y += 13;
			const std::string name = s.projectName.empty() ? s.scene.project : s.projectName;
			if (Button(ix, y, 44, 11, "SAVE", false, !name.empty())) Save(name);
			if (Button(ix + 46, y, 44, 11, "LOAD", false, !name.empty())) Load(name, false, false);
			if (Button(ix + 92, y, 44, 11, "RECOVER", false, !name.empty())) Load(name, false, true);
			y += 13;
			const bool confirm = NowMs() - s.confirmNewMs < 3000;
			if (Button(ix, y, 90, 11, confirm ? "CLICK TO CONFIRM" : "NEW (CLEAR SCENE)", confirm))
			{
				if (confirm) { s.scene.Clear(); s.scene.project.clear(); s.projectName.clear(); PlayerView(); s.confirmNewMs = 0; }
				else s.confirmNewMs = NowMs();
			}
			if (Button(ix + 92, y, 44, 11, "FORCE", false, !name.empty())) Load(name, true, false);
			y += 16;
			Text(ix, y, "RECENT", kAccent); y += 12;
			for (auto& r : s.recent)
			{
				if (y > y0 + h - 30) break;
				if (Inside(s.cx, s.cy, x, y, kRightW, kRow)) Rect(x + 1, y, kRightW - 2, kRow, kHover);
				TextClip(ix, y + 1, r, kRightW - 12, kTextCol);
				if (TakeClick(x, y, kRightW, kRow)) s.projectName = r;
				y += kRow;
			}
			Text(ix, y0 + h - 22, "Files: CinematicStudio/Projects/", kDim);
			Text(ix, y0 + h - 12, "Autosave every 2 min when changed", kDim);
			break;
		}

		case TAB_ANIMS:
			DrawAnimPanel(x, y0, h);
			break;
		case TAB_AUDIO:
			Text(ix, y, "AUDIO", kAccent); y += 12;
			Text(ix, y, "Dialogue tracks: Phase 8.", kDim); y += kRow;
			Text(ix, y, "Now: console 'cineaudio play", kDim); y += kRow;
			Text(ix, y, "<file> <who>' (lip sync).", kDim);
			break;
		case TAB_PROPS:
			Text(ix, y, "PROPS", kAccent); y += 12;
			Text(ix, y, "Prop placement: Phase 7.", kDim);
			break;
		}
	}

	float TimeToX(const int64_t us, const float x0, const float w)
	{
		const Clock& c = S().scene.clock;
		return x0 + (c.lengthUs > 0 ? static_cast<float>(static_cast<double>(us) / static_cast<double>(c.lengthUs)) : 0.0f) * w;
	}

	void DrawTransport()
	{
		Studio& s = S();
		Clock& c = s.scene.clock;
		const float y = kTransportY;
		Rect(0, y, kW, kTransportH, kPanelDark);
		Text(6, y + 3, c.Timecode(), kAccent, 0.5f);
		Text(78, y + 4, Fmt("frame %lld / %lld", static_cast<long long>(c.Frame()), static_cast<long long>(c.FrameCount())), kDim);
		float x = 170;
		auto B = [&](const float w, const std::string& l, const bool on = false) { const bool r = Button(x, y + 2, w, 11, l, on); x += w + 2; return r; };
		if (B(18, "|<")) c.SetFrame(0), c.Pause();
		if (B(14, "<")) c.Step(-1);
		if (B(40, c.playing ? "PAUSE" : "PLAY", c.playing)) c.playing ? c.Pause() : c.Play();
		if (B(14, ">")) c.Step(1);
		if (B(18, ">|")) c.SetFrame(c.FrameCount()), c.Pause();
		if (B(30, "STOP")) c.Stop();
		x += 8;
		if (B(40, Fmt("%d FPS", c.fps))) c.SetFps(c.fps == 24 ? 30 : (c.fps == 30 ? 60 : 24));
		static const double speeds[] = { 0.1, 0.25, 0.5, 1.0, 2.0 };
		if (B(36, Fmt("%.2fx", c.speed)))
		{
			int i = 0;
			while (i < 5 && speeds[i] <= c.speed + 1e-6) i++;
			c.speed = speeds[i % 5];
		}
		if (B(30, "LOOP", c.loop)) c.loop = !c.loop;
		x += 8;
		Text(x, y + 4, Fmt("length %ds", static_cast<int>(c.lengthUs / 1000000)), kDim);
		x += 52;
		if (B(12, "-") && c.lengthUs > 1000000) { c.lengthUs -= 1000000; if (c.timeUs > c.lengthUs) c.timeUs = c.lengthUs; s.scene.dirty = true; }
		if (B(12, "+") && c.lengthUs < 3600LL * 1000000) { c.lengthUs += 1000000; s.scene.dirty = true; }
	}

	void DrawTimeline()
	{
		Studio& s = S();
		Clock& c = s.scene.clock;
		const float y0 = kTimelineY, h = kTimelineH - kStatusH;
		Rect(0, y0, kW, kTimelineH, kPanel);
		const float lx = kLeftW, lw = kW - kLeftW - 6;

		// ruler
		Rect(lx, y0, lw, kRulerH, kPanelDark);
		const double secs = static_cast<double>(c.lengthUs) / 1e6;
		const int every = secs <= 20 ? 1 : (secs <= 60 ? 5 : (secs <= 300 ? 15 : 60));
		for (int sec = 0; sec <= static_cast<int>(secs); sec += every)
		{
			const float tx = TimeToX(static_cast<int64_t>(sec) * 1000000, lx, lw);
			Rect(tx, y0 + 6, 0.6f, kRulerH - 6, kLine);
			Text(tx + 1.5f, y0 + 1, Fmt("%d", sec), kDim, 0.34f);
		}
		const double framePx = lw / std::max(1.0, static_cast<double>(c.FrameCount()));
		if (framePx >= 3)
			for (int64_t f = 0; f <= c.FrameCount(); f++)
				Rect(lx + static_cast<float>(f * framePx), y0 + kRulerH - 3, 0.4f, 3, kLine);

		// dragging a clip along its lane (whole frames)
		if (s.clipDragIndex >= 0)
		{
			Object* o = s.scene.Find(s.clipDragObj);
			if (!s.lmb || !o || s.clipDragIndex >= static_cast<int>(o->clips.size())) s.clipDragIndex = -1;
			else
			{
				const double tAt = static_cast<double>((s.cx - lx) / lw) * static_cast<double>(c.lengthUs);
				const int64_t frameUs = 1000000 / c.fps;
				int64_t start = static_cast<int64_t>(tAt) - s.clipDragGrabUs;
				start = std::max<int64_t>(0, (start + frameUs / 2) / frameUs * frameUs);
				if (start != o->clips[static_cast<size_t>(s.clipDragIndex)].startUs)
				{
					AnimClip moved = o->clips[static_cast<size_t>(s.clipDragIndex)];
					moved.startUs = start;
					s.scene.SetClip(o->id, s.clipDragIndex, moved, true);
					s.clipDragIndex = s.scene.selectedClip;
				}
			}
		}

		// scrub: click or drag on the ruler / lanes
		const bool overTime = Inside(s.cx, s.cy, lx, y0, lw, h);
		if (s.clicked && Inside(s.clickX, s.clickY, lx, y0, lw, h)) { s.clicked = false; s.scrubbing = true; }
		if (!s.lmb) s.scrubbing = false;
		if (s.scrubbing)
		{
			const double t = std::max(0.0, std::min(1.0, static_cast<double>((s.cx - lx) / lw)));
			c.playing = false;
			c.SetFrame(static_cast<int64_t>(t * static_cast<double>(c.FrameCount()) + 0.5));
		}

		// tracks
		float ry = y0 + kRulerH + 1;
		Text(4, ry + 1, "TRACKS", kAccent);
		const int maxRows = static_cast<int>((h - kRulerH - 2) / kRow);
		if (overTime || Inside(s.cx, s.cy, 0, y0, kW, h))
		{
			s.timelineScroll -= static_cast<float>(s.wheel);
			s.wheel = 0;
		}
		const int count = static_cast<int>(s.scene.objects.size());
		s.timelineScroll = std::max(0.0f, std::min(s.timelineScroll, static_cast<float>(std::max(0, count - (maxRows - 1)))));
		int row = 0;
		for (int i = static_cast<int>(s.timelineScroll); i < count && row < maxRows - 1; i++, row++)
		{
			Object& o = s.scene.objects[static_cast<size_t>(i)];
			const float yy = ry + kRow * static_cast<float>(row + 1);
			if (o.id == s.scene.selected) Rect(0, yy, kW, kRow, kSel);
			TextClip(14, yy + 1, o.name, kLeftW - 18, o.type == ObjType::Camera ? kCam : kTextCol);
			Rect(lx, yy + 1, lw, kRow - 2, kLane);
			if (o.type == ObjType::Actor && !o.clips.empty())
			{
				for (size_t ci = 0; ci < o.clips.size(); ci++)
				{
					const AnimClip& c = o.clips[ci];
					const float cx0 = TimeToX(c.startUs, lx, lw);
					// a clip runs until the next one starts (looping or holding its last frame past its length)
					const int64_t nextUs = ci + 1 < o.clips.size() ? o.clips[ci + 1].startUs : c.lengthUs + c.startUs;
					const float cxl = TimeToX(c.startUs + c.lengthUs, lx, lw), cxn = TimeToX(std::max(nextUs, c.startUs + c.lengthUs), lx, lw);
					const bool csel = o.id == s.scene.selected && static_cast<int>(ci) == s.scene.selectedClip;
					const vec4_t body = { 0.30f, 0.62f, 0.38f, csel ? 0.95f : 0.75f };
					const vec4_t tail = { 0.30f, 0.62f, 0.38f, 0.25f };
					if (cxn > cxl) Rect(cxl, yy + 2, cxn - cxl, kRow - 4, tail);
					Rect(cx0, yy + 1, std::max(2.0f, cxl - cx0), kRow - 2, body);
					if (csel) Frame(cx0, yy + 1, std::max(2.0f, cxl - cx0), kRow - 2, kAccent);
					TextClip(cx0 + 2, yy + 1.5f, c.anim, std::max(0.0f, cxl - cx0 - 3), kTextCol);
					if (s.clicked && Inside(s.clickX, s.clickY, cx0, yy, std::max(4.0f, cxl - cx0), kRow))
					{
						s.clicked = false;
						s.scene.selected = o.id;
						s.lastSel = o.id;
						s.levelSel = -1;
						s.scene.selectedClip = static_cast<int>(ci);
						s.tab = TAB_SCENE;
						s.clipDragObj = o.id;
						s.clipDragIndex = static_cast<int>(ci);
						const double tAt = static_cast<double>((s.clickX - lx) / lw) * static_cast<double>(s.scene.clock.lengthUs);
						s.clipDragGrabUs = static_cast<int64_t>(tAt) - c.startUs;
					}
				}
			}
			else if (o.type == ObjType::Actor) Text(lx + 3, yy + 1.5f, "no clips: ANIMATIONS tab > ADD CLIP AT PLAYHEAD", kDim, 0.34f);
			if (TakeClick(0, yy, kLeftW, kRow)) { s.scene.selected = o.id; s.levelSel = -1; }
		}
		if (!count) Text(14, ry + kRow + 1, "no objects", kDim);

		// playhead
		const float px = TimeToX(c.timeUs, lx, lw);
		Rect(px - 0.5f, y0, 1.2f, h, kRed);

		// status line
		const float sy = kH - kStatusH;
		Rect(0, sy, kW, kStatusH, kPanelDark);
		if (!s.msgs.empty() && NowMs() - s.msgMs < 8000)
		{
			const auto& m = s.msgs.back();
			TextClip(4, sy + 1, m.second, kW - 8, m.first >= 2 ? kRed : (m.first == 1 ? kWarn : kTextCol));
		}
		else
			Text(4, sy + 1, "F10 close   TAB hide panels   SPACE play   , . step   CTRL+Z/Y undo/redo   CTRL+S save   "
				"right mouse + WASD fly   F focus   P freeze world", kDim, 0.36f);
	}

	void DrawViewportOverlay()
	{
		Studio& s = S();
		// scene cameras
		for (auto& o : s.scene.objects)
		{
			if (o.type != ObjType::Camera || o.id == s.lookId) continue;
			float sx, sy;
			if (!Project(o.origin, sx, sy)) continue;
			const bool sel = o.id == s.scene.selected;
			Frame(sx - 4, sy - 3, 8, 6, sel ? kAccent : kCam);
			Text(sx + 6, sy - 4, o.name, sel ? kAccent : kCam, 0.36f);
		}
		// selection bracket around the selected character
		int ent = -1;
		if (const Object* o = s.scene.Find(s.scene.selected)) ent = o->type == ObjType::Actor ? o->entNum : -1;
		else ent = s.levelSel;
		EntInfo info;
		if (ent >= 0 && EntityInfo(ent, info))
		{
			float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
			int n = 0;
			for (int c = 0; c < 8; c++)
			{
				const vec3_t p = { (c & 1) ? info.maxs[0] : info.mins[0], (c & 2) ? info.maxs[1] : info.mins[1], (c & 4) ? info.maxs[2] : info.mins[2] };
				float sx, sy;
				if (!Project(p, sx, sy)) continue;
				x0 = std::min(x0, sx); y0 = std::min(y0, sy); x1 = std::max(x1, sx); y1 = std::max(y1, sy);
				n++;
			}
			if (n)
			{
				Frame(x0, y0, x1 - x0, y1 - y0, kAccent);
				const Object* o = s.scene.FindByEntity(ent);
				Text(x0, y0 - 9, o ? o->name : info.npcType + " (level)", kAccent, 0.36f);
			}
		}
		if (s.camActive)
		{
			const Object* look = s.scene.Find(s.lookId);
			const std::string mode = look ? (s.pilotId ? "PILOTING " : "VIEWING ") + look->name : "FREE CAMERA";
			Text(kLeftW + 6, kPanelY + 3, mode + Fmt("   fov %.0f", s.camFov), kCam, 0.4f);
		}
	}

	// a click that no panel took: pick the nearest character or camera on screen
	void PickInViewport()
	{
		Studio& s = S();
		if (!s.clicked) return;
		s.clicked = false;
		float best = 22;
		int bestId = 0, bestEnt = -1;
		for (auto& o : s.scene.objects)
		{
			if (o.type != ObjType::Camera || o.id == s.lookId) continue;
			float sx, sy;
			if (!Project(o.origin, sx, sy)) continue;
			const float d = hypotf(sx - s.clickX, sy - s.clickY);
			if (d < best) { best = d; bestId = o.id; bestEnt = -1; }
		}
		int ents[128];
		const int n = ListCharacters(ents, 128);
		for (int i = 0; i < n; i++)
		{
			EntInfo info;
			if (!EntityInfo(ents[i], info) || (info.isPlayer && !s.camActive)) continue;
			const vec3_t mid = { info.origin[0], info.origin[1], (info.mins[2] + info.maxs[2]) * 0.5f };
			float sx, sy;
			if (!Project(mid, sx, sy)) continue;
			// generous vertical reach: characters are tall
			const float d = hypotf(sx - s.clickX, (sy - s.clickY) * 0.45f);
			if (d < best) { best = d; bestEnt = ents[i]; bestId = 0; }
		}
		if (bestId) { s.scene.selected = bestId; s.levelSel = -1; }
		else if (bestEnt >= 0)
		{
			const Object* o = s.scene.FindByEntity(bestEnt);
			if (o) { s.scene.selected = o->id; s.levelSel = -1; }
			else { s.scene.selected = 0; s.levelSel = bestEnt; s.tab = TAB_SCENE; }
		}
		else { s.scene.selected = 0; s.levelSel = -1; }
		if (s.doubleClick && (bestId || bestEnt >= 0)) Focus();
	}

	void DrawCursor()
	{
		Studio& s = S();
		const vec4_t black = { 0, 0, 0, 0.9f };
		Rect(s.cx - 0.5f, s.cy - 0.5f, 2.5f, 10.5f, black);
		Rect(s.cx - 0.5f, s.cy - 0.5f, 7.5f, 2.5f, black);
		Rect(s.cx, s.cy, 1.5f, 9, kTextCol);
		Rect(s.cx, s.cy, 6.5f, 1.5f, kTextCol);
	}

	void Open()
	{
		Studio& s = S();
		if (s.open) return;
		s.open = true;
		s.panels = true;
		cgi_Cvar_Set("cs_active", "1");
		LoadHotkeys();
		LoadRecent();
		s.lastMs = NowMs();
		s.host.Log(0, "Cinematic Studio open (F10 or 'studio' to close)");
	}

	void Close()
	{
		Studio& s = S();
		if (!s.open) return;
		s.open = false;
		s.field = FIELD_NONE;
		s.rmb = s.lmb = s.scrubbing = false;
		s.kFwd = s.kBack = s.kLeft = s.kRight = s.kUp = s.kDown = false;
		PlayerView();
		ReleaseAllHeld();
		s.dragging = false;
		s.clipDragIndex = -1;
		cgi_Cvar_Set("cs_active", "0");
		if (Frozen()) SetFreeze(false);	// back to regular gameplay
	}
}

namespace cs
{
	void UI_Message(const int level, const std::string& msg)
	{
		Studio& s = S();
		s.msgs.emplace_back(level, msg);
		if (s.msgs.size() > 20) s.msgs.pop_front();
		s.msgMs = NowMs();
	}
}

// ----------------------------------------------------------------------------------------------
// cgame entry points
// ----------------------------------------------------------------------------------------------

void CS_Init()
{
	Studio& s = S();
	// CG_Init also runs after vid_restart, when the level (and the actors) are still there
	const std::string map = s.host.MapName();
	if (map != s.initMap)
	{
		s.scene.Forget();
		CS_ClearAllEntities();
		s.heldEnts.clear();
		s.previewEnt = -1;
		s.animsEnt = -1;
		s.profiles.clear();
	}
	else
		for (auto& o : s.scene.objects)
		{
			if (o.type != ObjType::Actor || o.entNum < 0) continue;
			EntInfo info;
			std::string want = "cs_";
			for (const char c : o.name) want += (c == ' ' ? '_' : c);
			const bool same = EntityInfo(o.entNum, info) && (o.adopted || info.targetname == want);
			if (!same) o.entNum = -1;	// a loaded save game: the entity number means someone else now
		}
	s.initMap = map;
	s.open = false;
	s.levelSel = -1;
	PlayerView();
	cgi_Cvar_Set("cs_active", "0");
	// first run: a key to open the studio (the studio can't see keys while it's closed)
	cgi_Cvar_Register(&g_clPaused, "cl_paused", "0", 0);
	cgi_Cvar_Register(&g_firstRun, "cs_firstrun", "0", CVAR_ARCHIVE);
	if (!g_firstRun.integer)
	{
		cgi_SendConsoleCommand("bind F10 studio\n");
		cgi_Cvar_Set("cs_firstrun", "1");
	}
}

void CS_Shutdown()
{
	if (g_s && g_s->open)
	{
		g_s->open = false;
		cgi_Cvar_Set("cs_active", "0");
	}
}

bool CS_IsOpen()
{
	return g_s && g_s->open;
}

void CS_Command_f()
{
	Studio& s = S();
	const char* sub = cgi_Argc() > 1 ? CG_Argv(1) : "";
	const std::string a2 = cgi_Argc() > 2 ? CG_Argv(2) : "";
	const bool force = cgi_Argc() > 3 && !Q_stricmp(CG_Argv(3), "force");
	if (!sub[0] || !Q_stricmp(sub, "toggle")) { s.open ? Close() : Open(); return; }
	if (!Q_stricmp(sub, "open")) { Open(); return; }
	if (!Q_stricmp(sub, "close")) { Close(); return; }
	if (!Q_stricmp(sub, "save")) { Save(a2.empty() ? s.scene.project : a2); return; }
	if (!Q_stricmp(sub, "load")) { Load(a2, force, false); return; }
	if (!Q_stricmp(sub, "recover")) { Load(a2.empty() ? s.scene.project : a2, force, true); return; }
	if (!Q_stricmp(sub, "new")) { s.scene.Clear(); s.scene.project.clear(); s.projectName.clear(); PlayerView(); return; }
	if (!Q_stricmp(sub, "spawn")) { if (!a2.empty()) SpawnActor(a2); return; }
	if (!Q_stricmp(sub, "camera")) { AddCameraFromView(); return; }
	if (!Q_stricmp(sub, "play")) { s.scene.clock.Play(); return; }
	if (!Q_stricmp(sub, "pause")) { s.scene.clock.Pause(); return; }
	if (!Q_stricmp(sub, "stop")) { s.scene.clock.Stop(); return; }
	if (!Q_stricmp(sub, "fps")) { s.scene.clock.SetFps(atoi(a2.c_str())); return; }
	if (!Q_stricmp(sub, "undo")) { s.scene.Undo(); return; }
	if (!Q_stricmp(sub, "redo")) { s.scene.Redo(); return; }
	if (!Q_stricmp(sub, "freeze")) { SetFreeze(!Frozen()); return; }
	if (!Q_stricmp(sub, "list"))
	{
		for (auto& o : s.scene.objects)
			Com_Printf("%3d %-6s %-24s %s ent %d  (%.0f %.0f %.0f)\n", o.id, ObjTypeName(o.type), o.name.c_str(),
				o.npcType.c_str(), o.entNum, o.origin[0], o.origin[1], o.origin[2]);
		return;
	}
	Com_Printf("studio [open|close|toggle]  save [name]  load <name> [force]  recover [name]  new\n"
		"       spawn <npc>  camera  play  pause  stop  fps <24|30|60>  undo  redo  freeze  list\n");
}

int CS_KeyEvent(int key, const int down)
{
	if (!g_s || !g_s->open) return 0;
	Studio& s = S();

	// typed characters: only for an active text field
	if (key & K_CHAR_FLAG)
	{
		if (s.field == FIELD_NONE) return 1;	// swallowed: keys are commands here
		const int c = key & ~K_CHAR_FLAG;
		if (c >= 32 && c < 127 && s.fieldText.size() < 48) s.fieldText += static_cast<char>(c);
		return 1;
	}

	// letters arrive as their capital codes (A_CAP_A..Z); the studio's tables use the lowercase ones
	if (key >= A_CAP_A && key <= A_CAP_Z) key = A_LOW_A + (key - A_CAP_A);

	// held-state keys
	if (key == A_CTRL) { s.ctrl = down != 0; return 1; }
	if (key == A_SHIFT) { s.shift = down != 0; return 1; }
	if (key == A_MOUSE1)
	{
		s.lmb = down != 0;
		if (down)
		{
			if (s.field != FIELD_NONE) CommitField();	// clicking elsewhere commits typing
			const int now = NowMs();
			s.doubleClick = now - s.lastClickMs < 350 && fabsf(s.cx - s.clickX) < 4 && fabsf(s.cy - s.clickY) < 4;
			s.lastClickMs = now;
			s.clicked = true;
			s.clickX = s.cx;
			s.clickY = s.cy;
		}
		return 1;
	}
	if (key == A_MOUSE2)
	{
		s.rmb = down != 0;
		if (down) StartFreeCam();
		return 1;
	}
	if (key == A_MWHEELUP || key == A_MWHEELDOWN)
	{
		if (!down) return 1;
		const int dir = key == A_MWHEELUP ? 1 : -1;
		if (s.rmb) s.camFov = std::max(10.0f, std::min(130.0f, s.camFov - static_cast<float>(dir) * 3.0f));
		else s.wheel += dir;
		return 1;
	}
	// fly keys while the right button is held
	const bool flyKey = key == A_LOW_W || key == A_LOW_A || key == A_LOW_S || key == A_LOW_D || key == A_LOW_Q || key == A_LOW_E;
	if (flyKey && (s.rmb || !down))
	{
		bool* k = key == A_LOW_W ? &s.kFwd : key == A_LOW_S ? &s.kBack : key == A_LOW_A ? &s.kLeft : key == A_LOW_D ? &s.kRight :
			key == A_LOW_E ? &s.kUp : &s.kDown;
		*k = down != 0;
		if (s.rmb || !down) return 1;
	}
	if (!down) return 1;

	// text field editing
	if (s.field != FIELD_NONE)
	{
		if (key == A_ENTER) CommitField();
		else if (key == A_ESCAPE) s.field = FIELD_NONE;
		else if (key == A_BACKSPACE && !s.fieldText.empty()) s.fieldText.pop_back();
		return 1;
	}
	if (key == A_ESCAPE)
	{
		if (s.camActive) { PlayerView(); return 1; }
		Close();
		return 1;
	}

	auto is = [&](const Action a) { return g_keys[a].key == key && g_keys[a].ctrl == s.ctrl; };
	Clock& c = s.scene.clock;
	if (is(ACT_CLOSE)) Close();
	else if (is(ACT_PANELS)) s.panels = !s.panels;
	else if (is(ACT_PLAY)) c.playing ? c.Pause() : c.Play();
	else if (is(ACT_STOP)) c.Stop();
	else if (is(ACT_STEP_BACK)) c.Step(s.shift ? -c.fps : -1);
	else if (is(ACT_STEP_FWD)) c.Step(s.shift ? c.fps : 1);
	else if (is(ACT_UNDO)) s.scene.Undo();
	else if (is(ACT_REDO)) s.scene.Redo();
	else if (is(ACT_SAVE)) { if (!s.scene.project.empty()) Save(s.scene.project); else { s.tab = TAB_PROJECT; BeginField(FIELD_PROJECT, ""); } }
	else if (is(ACT_DELETE))
	{
		if (s.scene.selectedClip >= 0) s.scene.RemoveClip(s.scene.selected, s.scene.selectedClip);
		else s.scene.Remove(s.scene.selected);
	}
	else if (is(ACT_DUPLICATE)) s.scene.Duplicate(s.scene.selected);
	else if (is(ACT_FOCUS)) Focus();
	else if (is(ACT_FREEZE)) SetFreeze(!Frozen());
	else if (is(ACT_PLAYER_VIEW)) PlayerView();
	else if (g_keys[ACT_LEFT].key == key) Nudge(-1, 0, 0, 0);
	else if (g_keys[ACT_RIGHT].key == key) Nudge(1, 0, 0, 0);
	else if (g_keys[ACT_FWD].key == key) Nudge(0, 1, 0, 0);
	else if (g_keys[ACT_BACK].key == key) Nudge(0, -1, 0, 0);
	else if (g_keys[ACT_UP].key == key) Nudge(0, 0, 1, 0);
	else if (g_keys[ACT_DOWN].key == key) Nudge(0, 0, -1, 0);
	else if (g_keys[ACT_TURN_L].key == key) Nudge(0, 0, 0, 1);
	else if (g_keys[ACT_TURN_R].key == key) Nudge(0, 0, 0, -1);
	return 1;	// the studio owns the keyboard while open (the console key still works)
}

int CS_MouseEvent(const int dx, const int dy)
{
	if (!g_s || !g_s->open) return 0;
	Studio& s = S();
	if (s.rmb)
	{
		// mouse look for the studio camera
		const float sens = 0.15f;
		s.camAng[YAW] = AngleNormalize180(s.camAng[YAW] - static_cast<float>(dx) * sens);
		s.camAng[PITCH] = std::max(-89.0f, std::min(89.0f, s.camAng[PITCH] + static_cast<float>(dy) * sens));
		return 1;
	}
	const float sx = kW / std::max(1, cgs.glconfig.vidWidth), sy = kH / std::max(1, cgs.glconfig.vidHeight);
	s.cx = std::max(0.0f, std::min(kW - 1, s.cx + static_cast<float>(dx) * sx * 1.4f));
	s.cy = std::max(0.0f, std::min(kH - 1, s.cy + static_cast<float>(dy) * sy * 1.4f));
	return 1;
}

void CS_Frame()
{
	if (!g_s) return;
	Studio& s = S();
	const int now = NowMs();
	int dt = now - s.lastMs;
	s.lastMs = now;
	if (dt < 0 || dt > 250) dt = 0;	// hitches and alt-tab don't jump the clock

	s.scene.clock.Advance(static_cast<int64_t>(dt) * 1000);
	s.scene.SyncFromWorld();
	if (s.open)
	{
		if (s.scene.clock.playing && s.previewEnt >= 0) ReleasePreview();	// playback takes the actors back
		EvaluateClips();
	}

	if (s.open && s.camActive && s.rmb)
	{
		vec3_t fwd, right, up;
		AngleVectors(s.camAng, fwd, right, up);
		const float speed = (s.shift ? 600.0f : 200.0f) * static_cast<float>(dt) / 1000.0f;
		if (s.kFwd) VectorMA(s.camOrg, speed, fwd, s.camOrg);
		if (s.kBack) VectorMA(s.camOrg, -speed, fwd, s.camOrg);
		if (s.kRight) VectorMA(s.camOrg, speed, right, s.camOrg);
		if (s.kLeft) VectorMA(s.camOrg, -speed, right, s.camOrg);
		if (s.kUp) s.camOrg[2] += speed;
		if (s.kDown) s.camOrg[2] -= speed;
		if (s.lookId && !s.pilotId) s.lookId = 0;	// flying away from a camera you were only viewing
		if (s.pilotId)
		{
			Object* o = s.scene.Find(s.pilotId);
			if (o)
			{
				const bool moved = !VectorCompare(o->origin, s.camOrg) || !VectorCompare(o->angles, s.camAng) || o->fov != s.camFov;
				if (moved) { s.scene.SetTransform(o->id, s.camOrg, s.camAng, true); o->fov = s.camFov; }
			}
		}
	}

	if (now - s.lastAutosaveMs > 120000)
	{
		s.lastAutosaveMs = now;
		if (s.scene.Autosave()) s.host.Log(0, "Autosaved " + s.scene.project);
	}
}

bool CS_ApplyView()
{
	if (!g_s || !g_s->open || !g_s->camActive) return false;
	Studio& s = S();
	if (s.lookId && !s.pilotId)
	{
		const Object* o = s.scene.Find(s.lookId);
		if (!o) { PlayerView(); return false; }
		VectorCopy(o->origin, s.camOrg);
		VectorCopy(o->angles, s.camAng);
		s.camFov = o->fov;
	}
	VectorCopy(s.camOrg, cg.refdef.vieworg);
	VectorCopy(s.camAng, cg.refdefViewAngles);
	AnglesToAxis(cg.refdefViewAngles, cg.refdef.viewaxis);
	CG_CalcFOVFromX(s.camFov);
	cg.renderingThirdPerson = qtrue;	// the player's body is in shot
	return true;
}

void CS_Draw2D()
{
	if (!g_s || !g_s->open) return;
	Studio& s = S();
	if (s.scene.selected != s.lastSel) { s.scene.selectedClip = -1; s.lastSel = s.scene.selected; }
	UpdateDrag();
	DrawGizmo();
	if (s.panels)
	{
		DrawTitle();
		DrawOutliner();
		DrawRightPanel();
		DrawTransport();
		DrawTimeline();
		DrawViewportOverlay();
	}
	else
	{
		DrawViewportOverlay();
		Text(4, 4, "CINEMATIC STUDIO  (TAB: panels)", kAccent, 0.36f);
	}
	// clicks no panel took are viewport picks (only in the viewport area while panels are up)
	if (s.clicked)
	{
		const bool inViewport = !s.panels || Inside(s.clickX, s.clickY, kLeftW, kPanelY, kRightX - kLeftW, kTransportY - kPanelY);
		if (inViewport && !TryStartDrag()) PickInViewport();
		s.clicked = false;
	}
	s.doubleClick = false;
	if (!s.rmb) DrawCursor();
}
