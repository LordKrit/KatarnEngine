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
#include <map>
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

	enum Field { FIELD_NONE, FIELD_NPC, FIELD_PROJECT, FIELD_RENAME, FIELD_ANIMSEARCH, FIELD_NPCTYPE,
		FIELD_PROPSEARCH, FIELD_PROPDIR, FIELD_PROPKV, FIELD_PROPSET, FIELD_BOLT,
		FIELD_VOICE, FIELD_MUSIC };

	// configurable hotkeys (CinematicStudio/hotkeys.cfg)
	enum Action
	{
		ACT_CLOSE, ACT_PANELS, ACT_PLAY, ACT_STOP, ACT_STEP_BACK, ACT_STEP_FWD, ACT_UNDO, ACT_REDO, ACT_SAVE,
		ACT_DELETE, ACT_FOCUS, ACT_FREEZE, ACT_LEFT, ACT_RIGHT, ACT_FWD, ACT_BACK, ACT_UP, ACT_DOWN, ACT_TURN_L, ACT_TURN_R,
		ACT_PLAYER_VIEW, ACT_DUPLICATE, ACT_SPLIT, ACT_KEY, ACT_SNAP, ACT_DIRECTOR, ACT_COUNT
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
		{ "duplicate", A_LOW_D, true }, { "split", A_LOW_B, true }, { "set_key", A_LOW_K, false }, { "snap", A_LOW_N, false }, { "director", A_LOW_C, false },
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

		// ---- Phase 3: recorder ----
		enum { REC_IDLE, REC_COUNTDOWN, REC_RECORDING } rec = REC_IDLE;
		int recTarget = 0;			// actor getting the take (0 = a new actor)
		int recCountdownEnd = 0;	// ms
		int64_t recPunchUs = 0;		// scene time the take starts
		int64_t recLastUs = -1;
		struct Performer { int ent = -1; int objId = 0; std::string npcType; Take take; bool gone = false; };
		std::vector<Performer> recPerf;	// [0] = the player
		int recNpcs = 2;			// also record NPCs: 0 no, 1 scene actors with AI on, 2 every NPC nearby with AI
		bool recSolidTakes = true;	// earlier takes can be hit while recording over them
		std::vector<int> solidEnts;
		int takeBlendMs = 100;		// blend into each new animation of a take during playback
		std::map<int, std::pair<int, int>> liveAnims;
		std::vector<int> takeEnts;	// actors a take drives this frame (for saber clashes)	// per entity: legs, torso animation the engine is playing for a take
		double recOldSpeed = 1.0;
		bool recOldLoop = false;
		int recCountdownSec = 3;
		bool recGhostOthers = true;	// show the other takes see-through while recording
		std::vector<int> hiddenEnts;	// hidden while recording
		std::vector<int> ghostEnts;		// entities the studio made see-through

		// take dragging on the timeline
		int takeDragObj = 0, takeDragId = 0;
		int64_t takeDragGrabUs = 0;

		// ---- Phase 4 ----
		bool snap = true;			// timeline drags snap to the playhead and other edges
		int64_t viewStartUs = 0, viewLenUs = 0;	// visible part of the timeline (0 = all of it)
		int keyObj = 0;				// selected position key: object and time (keyObj 0 = none)
		int64_t keyUs = 0;
		bool keyDrag = false;
		bool clashes = true;		// saber clashes with takes
		// ---- Phase 5: combat replay ----
		int64_t recCgStartUs = 0;	// capture clock when recording began
		int64_t lastEvalUs = -1;	// scene time events were played up to
		bool irOn = false;			// instant replay: keep the last irSeconds of play
		int irSeconds = 30;
		int64_t irLastUs = -1, irLastPruneUs = 0;
		struct IrTrack { int ent = -1; std::string npcType; Take take; int64_t firstAbsUs = 0; bool open = true; };
		std::vector<IrTrack> irTracks;
		int evDragObj = 0;
		int64_t evDragGrabUs = 0;
		// ---- Phase 8: audio and faces ----
		std::string voicePath = "sound/cine/", soundPath = "music/";
		std::vector<std::string> facePoses;
		int facePosesEnt = -1;
		int exprPick = 0;
		float exprWeight = 1.0f;
		int64_t exprLenUs = 1'000'000;
		int exprBlendMs = 250;
		float eyePitch = 0, eyeYaw = 0;
		int eyeTarget = 0;
		int audioSelObj = 0, audioSelIdx = -1;	// selected sound clip (object, index); -1 = none
		int faceSelIdx = -1;					// selected expression clip of the selected actor
		std::map<int, std::pair<std::string, float>> faceApplied;	// per entity: expression the studio set
		std::map<int, int> idleApplied, eyeApplied;	// per entity: idle on (1) / off (0); eyes mode + 1
		bool soundsStarted = false;				// the studio started sounds (stopped when playback stops)
		bool musicStarted = false;
		// ---- Phase 7: props ----
		bool propModels = false;		// the PROPS tab lists model files instead of entity classes
		std::string propSearch, propDir = "models/map_objects", propClass = "misc_model_breakable", propModel;
		std::vector<std::string> propClasses, propFiles;
		std::string propFilesDir;
		float propScroll = 0;
		std::vector<std::pair<std::string, std::string>> propKv;	// extra keys for the next spawn
		// ---- Phase 6: camera director ----
		bool director = false;		// the view follows the cut track
		bool camRec = false;		// record the piloted camera's moves as keys while the scene plays
		std::vector<TransformKey> camRecKeys;
		int camRecId = 0;
		int64_t camRecLastUs = -1;
		int cutCam = 0;				// selected cut (camera id, time); cutCam 0 = none
		int64_t cutUs = 0;
		bool cutDrag = false;
		bool showPaths = true;
		// viewport drag of an actor a take or keys drive
		float dragTakeOff[3] = { 0, 0, 0 }, dragTakeYaw = 0, dragOrgStart[3] = { 0, 0, 0 };
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
	void StartRecording(int target);
	bool PropPose(const Object& o, int64_t t, float org[3], float ang[3]);
	bool CamPose(const Object& cam, int64_t t, float org[3], float ang[3], float& fov, bool effects);
	void CameraRecordFrame();
	void SetInstantReplay(bool on);
	void SetKeyAtPlayhead();

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
		s.director = false;
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

	// where a scene camera is at a time: its keys (or its own pose), aimed at its look-at actor, shaken
	bool CamPose(const Object& cam, const int64_t t, float org[3], float ang[3], float& fov, const bool effects)
	{
		Studio& s = S();
		if (cam.type != ObjType::Camera) return false;
		fov = cam.fov;
		float kf = 0;
		if (!cam.keys.empty() && EvaluateCameraKeys(cam.keys, t, org, ang, kf)) { if (kf > 0) fov = kf; }
		else { VectorCopy(cam.origin, org); VectorCopy(cam.angles, ang); }
		if (!effects) return true;
		if (cam.lookAt)
		{
			const Object* a = s.scene.Find(cam.lookAt);
			EntInfo info;
			if (a && a->entNum >= 0 && EntityInfo(a->entNum, info))
			{
				// aim at the chest / head
				const vec3_t at = { info.origin[0], info.origin[1], info.mins[2] + (info.maxs[2] - info.mins[2]) * 0.8f };
				vec3_t d, a2;
				VectorSubtract(at, org, d);
				if (VectorLength(d) > 1)
				{
					vectoangles(d, a2);
					ang[PITCH] = AngleNormalize180(a2[PITCH]);
					ang[YAW] = a2[YAW];
				}
			}
		}
		if (cam.shake > 0)
		{
			// handheld: a few slow sines, the same every time the scene plays
			const double ts = static_cast<double>(t) / 1e6;
			const float k = cam.shake * 0.25f;
			ang[PITCH] += k * static_cast<float>(sin(ts * 1.7) * 0.6 + sin(ts * 4.3 + 0.5) * 0.3 + sin(ts * 11.0 + 2.0) * 0.1);
			ang[YAW] += k * static_cast<float>(sin(ts * 1.3 + 1.0) * 0.6 + sin(ts * 3.7 + 2.0) * 0.3 + sin(ts * 9.0) * 0.1);
			ang[ROLL] += k * 0.5f * static_cast<float>(sin(ts * 2.1 + 3.0));
		}
		return true;
	}

	void LookThrough(const int id, const bool pilot)
	{
		Studio& s = S();
		const Object* o = s.scene.Find(id);
		if (!o || o->type != ObjType::Camera) return;
		s.lookId = id;
		s.pilotId = pilot ? id : 0;
		s.camActive = true;
		CamPose(*o, s.scene.clock.timeUs, s.camOrg, s.camAng, s.camFov, !pilot);
		s.director = false;	// your pick; the cut track would take the view away
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
		if (f == FIELD_PROPSEARCH) { s.propSearch = t; s.propScroll = 0; return; }
		if (f == FIELD_VOICE) { s.voicePath = t; return; }
		if (f == FIELD_MUSIC) { s.soundPath = t; return; }
		if (f == FIELD_PROPDIR) { s.propDir = t.empty() ? "models" : t; s.propFilesDir.clear(); s.propScroll = 0; return; }
		if (f == FIELD_PROPKV || f == FIELD_PROPSET)
		{
			// "key value" (value may have spaces); "key" alone removes it
			const size_t sp = t.find(' ');
			const std::string k = t.substr(0, sp), v = sp == std::string::npos ? "" : t.substr(sp + 1);
			if (k.empty()) return;
			if (f == FIELD_PROPSET) { s.scene.SetPropKey(s.scene.selected, k, v); return; }
			s.propKv.erase(std::remove_if(s.propKv.begin(), s.propKv.end(), [&](const std::pair<std::string, std::string>& p) { return p.first == k; }), s.propKv.end());
			if (!v.empty()) s.propKv.emplace_back(k, v);
			return;
		}
		if (f == FIELD_BOLT)
		{
			const Object* o = s.scene.Find(s.scene.selected);
			if (o && o->type == ObjType::Prop && o->attachTo) s.scene.SetAttach(o->id, o->attachTo, t, o->attachOffset, o->attachAngles, false);
			return;
		}
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
			const bool cam = o.type == ObjType::Camera, evt = o.type == ObjType::Events, prp = o.type == ObjType::Prop;
			const vec4_t kPropCol = { 0.55f, 0.85f, 0.45f, 1 };
			const bool aud = o.type == ObjType::Audio;
			const vec4_t kAudCol = { 0.30f, 0.85f, 0.85f, 1 };
			Text(x + 4, ry + 1, cam ? "C" : (evt ? "E" : (prp ? "P" : (aud ? "S" : "A"))), cam ? kCam : (evt ? kWarn : (prp ? kPropCol : (aud ? kAudCol : kAccent))));
			const bool missing = (o.type == ObjType::Actor || o.type == ObjType::Prop) && o.entNum < 0;
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
			if (Button(x, y, kRightW - 60, 11, "ANIMATIONS...")) s.tab = TAB_ANIMS;
			if (Button(x + kRightW - 58, y, 46, 11, "KEY (K)")) SetKeyAtPlayhead();
			y += 13;
			if (Button(x, y, bw, 11, o->prop ? "PROP: ON" : "PROP: OFF", o->prop, true)) s.scene.SetProp(o->id, !o->prop);
			if (Button(x + bw + 2, y, bw, 11, "GHOST", o->ghost)) s.scene.SetGhost(o->id, !o->ghost);
			if (Button(x + 2 * (bw + 2), y, bw, 11, "RECORD TAKE", false, s.rec == Studio::REC_IDLE)) { StartRecording(o->id); return; }
			y += 13;
			Text(x, y, Fmt("TAKES (%d)", static_cast<int>(o->takes.size())), kAccent);
			if (!o->takes.empty() && Button(x + kRightW - 52, y - 1, 40, 9, "NONE", !o->activeTake)) s.scene.SetActiveTake(o->id, 0);
			y += 10;
			int shown = 0;
			for (auto& t : o->takes)
			{
				if (shown++ >= 4) { Text(x, y, Fmt("+%d more (timeline)", static_cast<int>(o->takes.size()) - 4), kDim); y += kRow; break; }
				const bool act = t.id == o->activeTake, sel = t.id == s.scene.selectedTake;
				if (sel) Rect(x - 5, y, kRightW - 2, kRow, kSel);
				TextClip(x, y + 1, Fmt("%s %s  %.1fs", act ? ">" : " ", t.name.c_str(), static_cast<double>(t.LengthUs()) / 1e6), kRightW - 56, act ? kAccent : kTextCol);
				if (Button(x + kRightW - 52, y, 40, 9, act ? "ACTIVE" : "USE", act)) s.scene.SetActiveTake(o->id, act ? 0 : t.id);
				else if (TakeClick(x - 5, y, kRightW - 52, kRow)) { s.scene.selectedTake = t.id; s.scene.selectedClip = -1; }
				y += kRow;
			}
			if (o->takes.empty()) { Text(x, y, "RECORD TAKE: perform it yourself", kDim); y += kRow; }
			else if (!o->prop) { Text(x, y, "PROP OFF: the game drives it (AI ON to fight)", kDim, 0.36f); y += kRow; }
			y += 2;
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
		const float w = kRightW - 12;
		Label(x, y, "name", o->name); y += kRow;
		if (Button(x + kRightW - 50, y - kRow, 38, 9, "RENAME")) BeginField(FIELD_RENAME, o->name);
		if (s.field == FIELD_RENAME) { DrawField(x, y, kRightW - 12, FIELD_RENAME, o->name); y += 13; }
		float org[3], ang[3], fov;
		CamPose(*o, s.scene.clock.timeUs, org, ang, fov, false);
		Label(x, y, "position", Fmt("%.0f %.0f %.0f", org[0], org[1], org[2])); y += kRow;
		Label(x, y, "angles", Fmt("%.0f %.0f %.0f", ang[0], ang[1], ang[2])); y += kRow;
		Label(x, y, "fov", Fmt("%.0f%s", fov, o->keys.empty() ? "" : "  (keys)"));
		if (o->keys.empty())
		{
			if (Button(x + 90, y, 18, 9, "-")) { s.scene.SetCameraFov(o->id, o->fov - 5, true); if (s.lookId == o->id) s.camFov = std::max(5.0f, s.camFov - 5); }
			if (Button(x + 110, y, 18, 9, "+")) { s.scene.SetCameraFov(o->id, o->fov + 5, true); if (s.lookId == o->id) s.camFov = std::min(150.0f, s.camFov + 5); }
		}
		y += kRow;
		Label(x, y, "keys / cuts", Fmt("%d / %d", static_cast<int>(o->keys.size()), static_cast<int>(o->cuts.size()))); y += kRow + 3;
		const float bw = (w - 4) / 3;
		if (Button(x, y, bw, 11, "LOOK THROUGH", s.lookId == o->id && s.pilotId != o->id)) LookThrough(o->id, false);
		if (Button(x + bw + 2, y, bw, 11, "PILOT", s.pilotId == o->id)) LookThrough(o->id, true);
		if (Button(x + 2 * (bw + 2), y, bw, 11, "PLAYER VIEW")) { s.director = false; PlayerView(); }
		y += 13;
		if (Button(x, y, bw, 11, "KEY (K)")) SetKeyAtPlayhead();
		if (Button(x + bw + 2, y, bw, 11, "CUT HERE")) { if (s.scene.AddCut(o->id, s.scene.clock.timeUs)) s.host.Log(0, "Cut to " + o->name + " at " + s.scene.clock.Timecode()); }
		if (Button(x + 2 * (bw + 2), y, bw, 11, "REC MOVE", s.camRec)) s.camRec = !s.camRec;
		y += 13;
		if (Button(x, y, bw, 11, "SET FROM VIEW", false, o->keys.empty()))
			s.scene.SetTransform(o->id, cg.refdef.vieworg, cg.refdefViewAngles, false), s.scene.SetCameraFov(o->id, cg.refdef.fov_x, false);
		if (Button(x + bw + 2, y, bw, 11, "CLEAR KEYS", false, !o->keys.empty())) s.scene.ReplaceKeys(o->id, 0, INT64_MAX, {});
		if (Button(x + 2 * (bw + 2), y, bw, 11, "DIRECTOR", s.director)) { s.director = !s.director; if (s.director) { s.pilotId = 0; s.camActive = true; } }
		y += 14;
		{
			const Object* t = o->lookAt ? s.scene.Find(o->lookAt) : nullptr;
			Label(x, y, "look at", t ? t->name : "(its own angles)", w - 30);
			if (Button(x + w - 28, y, 28, 9, "NEXT"))
			{
				// cycle: none -> each actor -> none
				int next = 0;
				bool after = o->lookAt == 0;
				for (auto& a : s.scene.objects)
				{
					if (a.type != ObjType::Actor) continue;
					if (after) { next = a.id; break; }
					if (a.id == o->lookAt) after = true;
				}
				s.scene.SetCameraLook(o->id, next);
			}
			y += kRow;
		}
		Label(x, y, "shake", Fmt("%.1f", o->shake), w);
		if (Button(x + w - 28, y, 12, 9, "-")) s.scene.SetCameraShake(o->id, o->shake - 0.5f, true);
		if (Button(x + w - 14, y, 12, 9, "+")) s.scene.SetCameraShake(o->id, o->shake + 0.5f, true);
		y += kRow + 2;
		Text(x, y, s.camRec ? "REC MOVE: PILOT + SPACE keys your flight" : "PILOT: right mouse + WASD, wheel = fov", s.camRec ? kRed : kDim);
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
		s.liveAnims.clear();
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
		float f = static_cast<float>(static_cast<double>(localUs + c.animOffsetUs) / 1000.0 * c.speed / ai.frameLerp);
		if (c.loop) f = fmodf(f, frames);
		else f = std::min(f, frames - 1);
		return static_cast<float>(sf) + std::max(0.0f, f);
	}

	void Unhold(const int ent)
	{
		Studio& s = S();
		for (size_t i = 0; i < s.heldEnts.size(); i++)
			if (s.heldEnts[i] == ent) { ReleaseAnim(ent); s.heldEnts.erase(s.heldEnts.begin() + static_cast<long>(i)); s.liveAnims.erase(ent); return; }
	}

	// Plays an animation on the legs / torso / both for the studio. While the scene plays, the engine
	// plays (and blends into) the animation at `speed` and the studio only corrects it when it drifts,
	// so movement flows like in the game. Paused or scrubbing, the frame is posed exactly.
	void DriveAnim(const int ent, const AnimInfo& ai, const int parts, const float frame, const float speed, const bool loop,
		const int blendMs, const int sf = 0, const int ef = -1)
	{
		Studio& s = S();
		const Clock& c = s.scene.clock;
		const bool live = c.playing && c.speed > 0 && !s.scrubbing;
		auto& state = s.liveAnims[ent];
		int& cur = parts == PARTS_TORSO ? state.second : state.first;
		const float f = std::min(frame, static_cast<float>(ai.numFrames - 1));
		const auto mark = [&](const int v) { cur = v; if (parts == PARTS_BOTH) state.first = state.second = v; };
		if (!live)
		{
			PlayAnimAt(ent, ai.anim, parts, f, 0.0f, false, 0, sf, ef);
			mark(-1);
			return;
		}
		const float sp = speed * static_cast<float>(c.speed);
		float rel = 0;
		const bool same = cur == ai.anim && LiveFrame(ent, parts == PARTS_BOTH ? PARTS_LEGS : parts, ai.anim, rel, sf, ef);
		if (same)
		{
			float d = fabsf(rel - f);
			const int last = ef < 0 ? ai.numFrames - 1 : std::min(ef, ai.numFrames - 1);
			if (loop) d = std::min(d, fabsf(static_cast<float>(last - sf + 1) - d));	// across the loop point
			if (d <= 2.5f) return;
			PlayAnimAt(ent, ai.anim, parts, f, sp, loop, 0, sf, ef);	// drifted: put it back
		}
		else PlayAnimAt(ent, ai.anim, parts, f, sp, loop, blendMs, sf, ef);	// a new animation: blend into it
		mark(ai.anim);
	}

	// a take's animations and sabers at a time (its body is placed separately)
	void DriveTakeAnim(const Object& o, const TakeSample& x, const Take& tk)
	{
		Studio& s = S();
		const auto drive = [&](const int idx, const float frame, const float speed, const bool loop, const int parts)
		{
			if (idx < 0 || idx >= static_cast<int>(tk.Anims().size())) return;
			AnimInfo ai;
			if (!AnimInfoFor(o.entNum, AnimByName(tk.Anims()[static_cast<size_t>(idx)]), ai)) return;	// skeleton lacks it
			DriveAnim(o.entNum, ai, parts, frame, speed, loop, s.takeBlendMs);
		};
		if (x.legs == x.torso && fabsf(x.legsFrame - x.torsoFrame) < 0.01f)
			drive(x.legs, x.legsFrame, x.legsSpeed, (x.flags & TAKE_LEGS_LOOP) != 0, PARTS_BOTH);
		else
		{
			drive(x.legs, x.legsFrame, x.legsSpeed, (x.flags & TAKE_LEGS_LOOP) != 0, PARTS_LEGS);
			drive(x.torso, x.torsoFrame, x.torsoSpeed, (x.flags & TAKE_TORSO_LOOP) != 0, PARTS_TORSO);
		}
		SetSabers(o.entNum, x.flags);
		if (x.forceActive || (x.flags & (TAKE_SHOCKED | TAKE_STUNNED | TAKE_DRAINED))) SetForceView(o.entNum, x.forceActive, x.flags);
	}

	bool Contains(const std::vector<int>& v, const int e) { return std::find(v.begin(), v.end(), e) != v.end(); }

	// Takes, keys and timeline clips drive their actors while the studio is open or recording.
	// Body (position, facing): the active take; else the position keys. Animation: a clip that is playing;
	// else the active take; else a clip holding its last frame; else the game. Takes and keys only drive
	// props (PROP OFF hands the actor to the game).
	void EvaluateScene()
	{
		Studio& s = S();
		const int64_t t = s.scene.clock.timeUs;
		std::vector<int> ghosts;
		s.takeEnts.clear();
		for (auto& o : s.scene.objects)
		{
			if (o.type != ObjType::Actor || o.entNum < 0) continue;
			const bool recordingOver = s.rec != Studio::REC_IDLE && o.id == s.recTarget;
			const Take* tk = o.prop ? Scene::ActiveTake(o) : nullptr;
			const bool ghost = o.ghost || (s.rec != Studio::REC_IDLE && s.recGhostOthers && tk && !recordingOver);
			if (ghost) { SetGhost(o.entNum, 0.4f); ghosts.push_back(o.entNum); }
			if (o.entNum == s.previewEnt || recordingOver) continue;

			// body
			TakeSample x;
			const bool haveTake = tk && tk->Evaluate(t - tk->startUs, x);
			if (haveTake)
			{
				SetTakeBody(o.entNum, x.origin, x.velocity, x.yaw, x.pitch);
				s.takeEnts.push_back(o.entNum);
			}
			else if (o.prop && !o.keys.empty() && !(s.dragging && o.id == s.scene.selected))
			{
				float org[3], yaw, vel[3];
				EvaluateKeys(o.keys, t, org, yaw, vel);
				const bool live = s.scene.clock.playing && !s.scrubbing;
				const float still[3] = { 0, 0, 0 };
				SetTakeBody(o.entNum, org, live ? vel : still, yaw, 0);
			}

			// animation
			int64_t local = 0;
			bool holding = false;
			const AnimClip* c = o.clips.empty() ? nullptr : Scene::ActiveClip(o, t, local, holding);
			if (haveTake && (!c || holding)) { DriveTakeAnim(o, x, *tk); Hold(o.entNum); continue; }
			if (!c) { Unhold(o.entNum); continue; }	// before the first clip the game animates the actor
			AnimInfo ai;
			if (!AnimInfoFor(o.entNum, AnimByName(c->anim), ai)) continue;	// this skeleton lacks the animation
			int sf, ef;
			const float f = ClipFrame(*c, ai, local, holding, sf, ef);
			const float engineSpeed = 50.0f / static_cast<float>(std::max(1, ai.frameLerp)) * c->speed;
			if (holding) DriveAnim(o.entNum, ai, c->parts, f, 0.0f, false, 0, sf, ef);
			else DriveAnim(o.entNum, ai, c->parts, f, engineSpeed, c->loop, c->blendMs, sf, ef);
			Hold(o.entNum);
		}
		for (const int e : s.ghostEnts) if (!Contains(ghosts, e)) SetGhost(e, 1.0f);
		s.ghostEnts = ghosts;
	}

	void ClearGhosts()
	{
		Studio& s = S();
		for (const int e : s.ghostEnts) SetGhost(e, 1.0f);
		s.ghostEnts.clear();
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
	void SplitAtPlayhead();
	void SetKeyAtPlayhead();
	bool DrawTakeEditor(const float x, float y)
	{
		Studio& s = S();
		Object* o = s.scene.Find(s.scene.selected);
		Take* t = o ? Scene::FindTake(*o, s.scene.selectedTake) : nullptr;
		if (!t) return false;
		const float w = kRightW - 12;
		Text(x, y, "TAKE", kAccent);
		if (Button(x + w - 40, y - 1, 40, 10, "CLOSE")) { s.scene.selectedTake = 0; return false; }
		y += 11;
		Label(x, y, "name", t->name, w); y += kRow;
		Label(x, y, "actor", o->name, w); y += kRow;
		Label(x, y, "performer", t->performer + (t->profile.empty() ? "" : "  (" + t->profile + ")"), w); y += kRow;
		const int64_t frameUs = 1'000'000 / s.scene.clock.fps;
		Label(x, y, "start", Fmt("%.2fs", static_cast<double>(t->startUs) / 1e6), w);
		const int id = o->id, tid = t->id;
		const int64_t step = s.shift ? 1'000'000 : frameUs;
		if (Button(x + w - 28, y, 12, 9, "-")) { s.scene.SetTakeStart(id, tid, t->startUs - step, true); return true; }
		if (Button(x + w - 14, y, 12, 9, "+")) { s.scene.SetTakeStart(id, tid, t->startUs + step, true); return true; }
		y += kRow;
		Label(x, y, "length", Fmt("%.2fs of %.2fs", static_cast<double>(t->LengthUs()) / 1e6, static_cast<double>(t->RecordedUs()) / 1e6), w); y += kRow;
		{
			Take e = *t;
			bool ch = false;
			Label(x, y, "trim in", Fmt("%.2fs", static_cast<double>(t->trimInUs) / 1e6), w);
			if (Button(x + w - 28, y, 12, 9, "-")) { e.trimInUs -= step; e.startUs -= step; ch = true; }
			if (Button(x + w - 14, y, 12, 9, "+")) { e.trimInUs += step; e.startUs += step; ch = true; }	// the rest stays put
			y += kRow;
			Label(x, y, "trim out", Fmt("%.2fs", static_cast<double>(t->trimOutUs) / 1e6), w);
			if (Button(x + w - 28, y, 12, 9, "-")) { e.trimOutUs -= step; ch = true; }
			if (Button(x + w - 14, y, 12, 9, "+")) { e.trimOutUs += step; ch = true; }
			y += kRow;
			Label(x, y, "offset", Fmt("%.0f %.0f %.0f  turn %.0f", t->offset[0], t->offset[1], t->offset[2], t->yawOffset), w);
			y += kRow;
			const float bw4 = (w - 6) / 4;
			if (Button(x, y, bw4, 9, "UP")) { e.offset[2] += s.shift ? 16.0f : 1.0f; ch = true; }
			if (Button(x + bw4 + 2, y, bw4, 9, "DOWN")) { e.offset[2] -= s.shift ? 16.0f : 1.0f; ch = true; }
			if (Button(x + 2 * (bw4 + 2), y, bw4, 9, "RESET")) { e.offset[0] = e.offset[1] = e.offset[2] = 0; e.yawOffset = 0; ch = true; }
			if (Button(x + 3 * (bw4 + 2), y, bw4, 9, "SPLIT")) { SplitAtPlayhead(); return true; }
			y += kRow + 1;
			if (ch)
			{
				if (e.trimInUs < 0) { e.startUs -= e.trimInUs; e.trimInUs = 0; }
				s.scene.SetTake(id, e, true);
				return true;
			}
		}
		Label(x, y, "samples", Fmt("%d  (%d animations)", static_cast<int>(t->Samples().size()), static_cast<int>(t->Anims().size())), w); y += kRow + 1;
		const bool act = o->activeTake == tid;
		if (Button(x, y, 50, 10, act ? "ACTIVE" : "USE", act)) { s.scene.SetActiveTake(id, act ? 0 : tid); return true; }
		if (Button(x + 52, y, 50, 10, "RE-RECORD", false, s.rec == Studio::REC_IDLE))
		{
			s.scene.clock.timeUs = t->startUs;	// a new take from the same point; this one stays
			StartRecording(id);
			return true;
		}
		if (Button(x + w - 48, y, 48, 10, "DELETE")) { s.scene.RemoveTake(id, tid); return true; }
		y += 13;
		Text(x, y, "drag on timeline; drag actor: move take", kDim); y += kRow;
		Text(x, y, "CTRL+drag actor: turn take  SHIFT = 1s", kDim);
		return true;
	}

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
		y += kRow;
		Label(x, y, "blend in", Fmt("%d ms", c.blendMs), w);
		if (Button(x + w - 28, y, 12, 9, "-")) { c.blendMs = std::max(0, c.blendMs - 50); changed = true; }
		if (Button(x + w - 14, y, 12, 9, "+")) { c.blendMs = std::min(1000, c.blendMs + 50); changed = true; }
		y += kRow + 1;
		const char* pn = c.parts == PARTS_LEGS ? "LEGS" : (c.parts == PARTS_TORSO ? "TORSO" : "BOTH");
		if (Button(x, y, 50, 10, pn)) { c.parts = c.parts == PARTS_BOTH ? PARTS_LEGS : (c.parts == PARTS_LEGS ? PARTS_TORSO : PARTS_BOTH); changed = true; }
		if (Button(x + 52, y, 40, 10, "LOOP", c.loop)) { c.loop = !c.loop; changed = true; }
		if (Button(x + 94, y, 30, 10, "SPLIT")) { SplitAtPlayhead(); return true; }
		if (Button(x + w - 48, y, 48, 10, "DELETE")) { s.scene.RemoveClip(o->id, idx); return true; }
		y += 13;
		Text(x, y, "drag on timeline; CTRL+B split; SHIFT=1s", kDim);
		if (changed) s.scene.SetClip(o->id, idx, c, true);
		return true;
	}

	// ---- Phase 4 helpers ----
	// where the actor is now (a take or keys may be moving it)
	bool ShownTransform(const Object& o, float org[3], float& yaw)
	{
		Studio& s = S();
		if (o.type == ObjType::Prop) { float ang[3]; PropPose(o, s.scene.clock.timeUs, org, ang); yaw = ang[YAW]; return true; }
		const int64_t t = s.scene.clock.timeUs;
		const Take* tk = o.prop ? Scene::ActiveTake(o) : nullptr;
		TakeSample x;
		if (tk && tk->Evaluate(t - tk->startUs, x)) { VectorCopy(x.origin, org); yaw = x.yaw; return true; }
		float vel[3];
		if (o.prop && EvaluateKeys(o.keys, t, org, yaw, vel)) return true;
		VectorCopy(o.origin, org);
		yaw = o.angles[YAW];
		return true;
	}

	void SetKeyAtPlayhead()
	{
		Studio& s = S();
		const Object* o = s.scene.Find(s.scene.selected);
		if (o && o->type == ObjType::Camera)
		{
			TransformKey k;
			const int64_t frameUs0 = 1'000'000 / s.scene.clock.fps;
			k.tUs = s.scene.clock.timeUs / frameUs0 * frameUs0;
			float ang[3], fov;
			if (s.pilotId == o->id || s.lookId == o->id) { VectorCopy(s.camOrg, k.origin); VectorCopy(s.camAng, ang); fov = s.camFov; }
			else CamPose(*o, k.tUs, k.origin, ang, fov, false);
			k.pitch = ang[PITCH]; k.yaw = ang[YAW]; k.roll = ang[ROLL]; k.fov = fov;
			for (auto& e : o->keys) if (e.tUs == k.tUs) k.interp = e.interp;
			const int cid = o->id;
			if (s.scene.SetKey(cid, k)) { s.keyObj = cid; s.keyUs = k.tUs; s.host.Log(0, o->name + ": camera key at " + s.scene.clock.Timecode()); }
			return;
		}
		if (o && o->type == ObjType::Prop)
		{
			if (o->attachTo) { s.host.Log(1, o->name + " is held by an actor (NEXT to nobody to key it)"); return; }
			TransformKey k;
			const int64_t frameUs1 = 1'000'000 / s.scene.clock.fps;
			k.tUs = s.scene.clock.timeUs / frameUs1 * frameUs1;
			float ang[3];
			PropPose(*o, k.tUs, k.origin, ang);
			k.pitch = ang[PITCH]; k.yaw = ang[YAW]; k.roll = ang[ROLL];
			for (auto& e : o->keys) if (e.tUs == k.tUs) k.interp = e.interp;
			const int pid = o->id;
			if (s.scene.SetKey(pid, k)) { s.keyObj = pid; s.keyUs = k.tUs; s.host.Log(0, o->name + ": key at " + s.scene.clock.Timecode()); }
			return;
		}
		if (!o || o->type != ObjType::Actor) { s.host.Log(1, "Select an actor, camera or prop to key"); return; }
		if (o->prop && Scene::ActiveTake(*o)) { s.host.Log(1, o->name + ": a take moves it (USE NONE on its takes to key it instead)"); return; }
		TransformKey k;
		const int64_t frameUs = 1'000'000 / s.scene.clock.fps;
		k.tUs = s.scene.clock.timeUs / frameUs * frameUs;
		ShownTransform(*o, k.origin, k.yaw);
		for (auto& e : o->keys) if (e.tUs == k.tUs) k.interp = e.interp;
		const int id = o->id;
		if (s.scene.SetKey(id, k)) { s.keyObj = id; s.keyUs = k.tUs; s.host.Log(0, o->name + ": key at " + s.scene.clock.Timecode()); }
	}

	void SplitAtPlayhead()
	{
		Studio& s = S();
		const int id = s.scene.selected;
		const int64_t t = s.scene.clock.timeUs;
		if (s.scene.selectedClip >= 0) { if (s.scene.SplitClip(id, s.scene.selectedClip, t) < 0) s.host.Log(1, "Put the playhead inside the clip to split it"); return; }
		if (s.scene.selectedTake) { if (!s.scene.SplitTake(id, s.scene.selectedTake, t)) s.host.Log(1, "Put the playhead inside the take to split it"); return; }
		s.host.Log(1, "Select a clip or a take to split (CTRL+B)");
	}

	bool DrawKeyEditor(const float x, float y)
	{
		Studio& s = S();
		Object* o = s.scene.Find(s.keyObj);
		const TransformKey* k = nullptr;
		if (o && o->id == s.scene.selected) for (auto& e : o->keys) if (e.tUs == s.keyUs) k = &e;
		if (!k) { s.keyObj = 0; return false; }
		const float w = kRightW - 12;
		Text(x, y, "POSITION KEY", kAccent);
		if (Button(x + w - 40, y - 1, 40, 10, "CLOSE")) { s.keyObj = 0; return false; }
		y += 11;
		Label(x, y, "actor", o->name, w); y += kRow;
		Label(x, y, "time", Fmt("%.3fs", static_cast<double>(k->tUs) / 1e6), w); y += kRow;
		Label(x, y, "position", Fmt("%.0f %.0f %.0f", k->origin[0], k->origin[1], k->origin[2]), w); y += kRow;
		if (o->type == ObjType::Camera) { Label(x, y, "angles", Fmt("%.0f %.0f %.0f  fov %.0f", k->pitch, k->yaw, k->roll, k->fov), w); y += kRow + 1; }
		else { Label(x, y, "yaw", Fmt("%.0f", k->yaw), w); y += kRow + 1; }
		const int id = o->id;
		const int64_t at = k->tUs;
		static const Interp kOrder[] = { Interp::Smooth, Interp::Linear, Interp::CatmullRom, Interp::Step };
		int cur = 0;
		for (int i = 0; i < 4; i++) if (kOrder[i] == k->interp) cur = i;
		if (Button(x, y, 70, 10, std::string("TO NEXT: ") + InterpName(k->interp))) { s.scene.SetKeyInterp(id, at, kOrder[(cur + 1) % 4]); return true; }
		if (Button(x + 72, y, 46, 10, "GO TO")) { s.scene.clock.Pause(); s.scene.clock.timeUs = at; }
		if (Button(x + w - 40, y, 40, 10, "DELETE")) { s.scene.RemoveKey(id, at); s.keyObj = 0; return true; }
		y += 12;
		if (o->type == ObjType::Camera)
		{
			if (Button(x, y, w, 10, "SET FROM THE VIEW"))
			{
				TransformKey nk = *k;
				VectorCopy(cg.refdef.vieworg, nk.origin);
				nk.pitch = cg.refdefViewAngles[PITCH]; nk.yaw = cg.refdefViewAngles[YAW]; nk.roll = cg.refdefViewAngles[ROLL];
				nk.fov = cg.refdef.fov_x;
				s.scene.SetKey(id, nk);
				return true;
			}
		}
		else if (Button(x, y, w, 10, "SET FROM WHERE IT IS NOW"))
		{
			TransformKey nk = *k;
			EntInfo info;
			if (EntityInfo(o->entNum, info)) { VectorCopy(info.origin, nk.origin); nk.yaw = info.yaw; s.scene.SetKey(id, nk); }
			return true;
		}
		y += 13;
		Text(x, y, "K: key at playhead  drag keys in time", kDim); y += kRow;
		Text(x, y, "drag the actor at a key's time to move it", kDim);
		return true;
	}

	// ---- Phase 8: audio and faces ----
	void StartSound(const Object& o, const SoundClip& c)
	{
		Studio& s = S();
		const sfxHandle_t h = cgi_S_RegisterSound(c.path.c_str());
		if (!h) { s.host.Log(1, "Can't load sound " + c.path); return; }
		if (o.type == ObjType::Actor)
		{
			if (o.entNum < 0) return;
			cgi_S_StartSound(nullptr, o.entNum, CHAN_VOICE_GLOBAL, h);	// a voice line: lip sync on a face rig
		}
		else if (o.music)
		{
			cgi_S_StartBackgroundTrack(c.path.c_str(), c.path.c_str(), qtrue);
			s.musicStarted = true;
		}
		else cgi_S_StartLocalSound(h, CHAN_AUTO);
		s.soundsStarted = true;
	}

	// sounds starting between two scene times (from, to] play
	void PlaySceneSounds(const int64_t from, const int64_t to)
	{
		Studio& s = S();
		for (auto& o : s.scene.objects)
		{
			if ((o.type != ObjType::Actor && o.type != ObjType::Audio) || o.sounds.empty() || !o.visible) continue;
			for (auto& c : o.sounds)
				if (c.startUs > from && c.startUs <= to) StartSound(o, c);
		}
	}

	void StopStudioSounds()
	{
		Studio& s = S();
		if (s.musicStarted) cgi_S_StartBackgroundTrack("", "", qfalse);
		if (s.soundsStarted) cgi_S_StopSounds();
		s.soundsStarted = s.musicStarted = false;
	}

	// face tracks: expressions, idle on / off, eyes; every frame, also while scrubbing
	void EvaluateFaces()
	{
		Studio& s = S();
		const int64_t t = s.scene.clock.timeUs;
		for (auto& o : s.scene.objects)
		{
			if (o.type != ObjType::Actor || o.entNum < 0) continue;
			const int e = o.entNum;
			// expression
			if (!o.faceClips.empty() || s.faceApplied.count(e))
			{
				const FaceClip* c = ActiveFaceClip(o.faceClips, t);
				const std::string pose = c ? c->pose : "";
				const float w = c ? c->weight : 0.0f;
				auto it = s.faceApplied.find(e);
				if (it == s.faceApplied.end() || it->second.first != pose || it->second.second != w)
				{
					int blend = c ? c->blendMs : 250;
					if (!c && it != s.faceApplied.end())
						for (auto& fc : o.faceClips) if (fc.pose == it->second.first) blend = fc.blendMs;	// out as it came in
					CS_FaceExpr(e, pose.c_str(), w, blend);
					if (c || !o.faceClips.empty()) s.faceApplied[e] = { pose, w }; else s.faceApplied.erase(e);
				}
			}
			// idle
			if (!o.idleKeys.empty())
			{
				const IdleKey* k = IdleAt(o.idleKeys, t);
				const int want = k ? (k->on ? 1 : 0) : 1;
				auto it = s.idleApplied.find(e);
				if (it == s.idleApplied.end() || it->second != want) { CS_FaceIdle(e, want != 0, k ? k->profile.c_str() : ""); s.idleApplied[e] = want; }
			}
			// eyes
			if (!o.eyeKeys.empty() || s.eyeApplied.count(e))
			{
				EyeKey x;
				const bool have = EyesAt(o.eyeKeys, t, x);
				const int mode = have ? static_cast<int>(x.mode) : -1;
				if (mode == EyeKey::Aim) CS_FaceEyes(e, true, x.pitch, x.yaw);
				else CS_FaceEyes(e, false, 0, 0);
				if (mode == EyeKey::LookAt)
				{
					const Object* tg = s.scene.Find(x.target);
					SetLookTarget(e, tg ? tg->entNum : -1);
				}
				else if (s.eyeApplied.count(e) && s.eyeApplied[e] == EyeKey::LookAt + 1) SetLookTarget(e, -1);
				if (have) s.eyeApplied[e] = mode + 1; else s.eyeApplied.erase(e);
			}
		}
	}

	void RefreshFacePoses(const int ent)
	{
		Studio& s = S();
		if (s.facePosesEnt == ent) return;
		s.facePosesEnt = ent;
		s.facePoses.clear();
		if (ent >= 0) CS_FacePoses(ent, s.facePoses);
		if (s.facePoses.empty()) s.facePoses = { "angry", "smile", "smirk", "sad", "surprised", "blink" };
		s.exprPick = 0;
	}

	void DrawSoundList(const float x, float& y, Object* o, const float w, const int maxRows)
	{
		Studio& s = S();
		int shown = 0;
		for (size_t i = 0; i < o->sounds.size(); i++)
		{
			if (shown++ >= maxRows) { Text(x, y, Fmt("+%d more (timeline)", static_cast<int>(o->sounds.size()) - maxRows), kDim); y += kRow; break; }
			const SoundClip& c = o->sounds[i];
			const bool sel = s.audioSelObj == o->id && s.audioSelIdx == static_cast<int>(i);
			if (sel) Rect(x - 5, y, kRightW - 2, kRow, kSel);
			std::string nm = c.path;
			const size_t sl = nm.find_last_of('/');
			if (sl != std::string::npos) nm = nm.substr(sl + 1);
			TextClip(x, y + 1, Fmt("%.2fs  ", static_cast<double>(c.startUs) / 1e6) + nm, w - 16, kTextCol);
			if (Button(x + w - 12, y, 12, 9, "x")) { Object e = *o; e.sounds.erase(e.sounds.begin() + static_cast<long>(i)); s.scene.Edit(e, false); s.audioSelIdx = -1; return; }
			if (TakeClick(x - 5, y, w - 14, kRow)) { s.audioSelObj = o->id; s.audioSelIdx = static_cast<int>(i); }
			y += kRow;
		}
		// the selected one: start / length nudges
		if (s.audioSelObj == o->id && s.audioSelIdx >= 0 && s.audioSelIdx < static_cast<int>(o->sounds.size()))
		{
			Object e = *o;
			SoundClip& c = e.sounds[static_cast<size_t>(s.audioSelIdx)];
			const int64_t step = s.shift ? 1'000'000 : 1'000'000 / s.scene.clock.fps;
			bool ch = false;
			Label(x, y, "start", Fmt("%.2fs", static_cast<double>(c.startUs) / 1e6), w);
			if (Button(x + w - 28, y, 12, 9, "-")) { c.startUs -= step; ch = true; }
			if (Button(x + w - 14, y, 12, 9, "+")) { c.startUs += step; ch = true; }
			y += kRow;
			Label(x, y, "length", Fmt("%.2fs (on the timeline)", static_cast<double>(c.lengthUs) / 1e6), w);
			if (Button(x + w - 28, y, 12, 9, "-")) { c.lengthUs -= step * 5; ch = true; }
			if (Button(x + w - 14, y, 12, 9, "+")) { c.lengthUs += step * 5; ch = true; }
			y += kRow;
			if (Button(x, y, 50, 10, "PLAY NOW")) StartSound(*o, c);
			if (Button(x + 52, y, 50, 10, "STOP")) StopStudioSounds();
			y += 12;
			if (ch) { const SoundClip keep = c; s.scene.Edit(e, true, "sound"); const Object* n = s.scene.Find(e.id); for (size_t i = 0; n && i < n->sounds.size(); i++) if (n->sounds[i] == keep) s.audioSelIdx = static_cast<int>(i); }
		}
	}

	void DrawAudioPanel(const float x, const float y0, const float h)
	{
		Studio& s = S();
		const float ix = x + 6, w = kRightW - 12;
		float y = y0 + 2;
		Object* o = s.scene.Find(s.scene.selected);
		const int64_t t = s.scene.clock.timeUs;
		if (o && o->type == ObjType::Actor)
		{
			// ---- voice lines
			Text(ix, y, "VOICE LINES: " + o->name, kAccent); y += 11;
			DrawField(ix, y, w, FIELD_VOICE, s.voicePath); y += 13;
			if (Button(ix, y, w, 10, "ADD LINE AT PLAYHEAD (lip sync)"))
			{
				std::string path = s.voicePath;
				if (path.find('/') == std::string::npos) path = "sound/cine/" + path;
				if (path.empty() || path.back() == '/') s.host.Log(1, "Type the sound file first (e.g. sound/cine/line1.mp3)");
				else
				{
					Object e = *o;
					SoundClip c;
					c.startUs = t;
					c.path = path;
					e.sounds.push_back(c);
					s.scene.Edit(e, false);
				}
			}
			y += 12;
			DrawSoundList(ix, y, o, w, 3);
			y += 3;
			// ---- face
			const bool rig = CS_FaceHasRig(o->entNum);
			Text(ix, y, rig ? "FACE" : "FACE (no face rig on this model)", rig ? kAccent : kDim); y += 11;
			if (!rig)
			{
				Text(ix, y, "Expression / eye tracks need a model with", kDim); y += kRow;
				Text(ix, y, "an ext_data/faces/<model>.face file.", kDim); y += kRow;
				Text(ix, y, "Eyes LOOK AT still turn the head.", kDim); y += kRow + 2;
			}
			else
			{
				RefreshFacePoses(o->entNum);
				const std::string pose = s.facePoses[static_cast<size_t>(s.exprPick) % s.facePoses.size()];
				if (Button(ix, y, 12, 10, "<")) s.exprPick = (s.exprPick + static_cast<int>(s.facePoses.size()) - 1) % static_cast<int>(s.facePoses.size());
				TextClip(ix + 15, y + 1, pose, w - 32, kTextCol);
				if (Button(ix + w - 12, y, 12, 10, ">")) s.exprPick = (s.exprPick + 1) % static_cast<int>(s.facePoses.size());
				y += 12;
				const float qw = (w - 6) / 3;
				Label(ix, y, "weight", Fmt("%.2f", s.exprWeight), qw * 2);
				if (Button(ix + w - 28, y, 12, 9, "-")) s.exprWeight = std::max(0.1f, s.exprWeight - 0.1f);
				if (Button(ix + w - 14, y, 12, 9, "+")) s.exprWeight = std::min(1.0f, s.exprWeight + 0.1f);
				y += kRow;
				Label(ix, y, "length / blend", Fmt("%.1fs / %d ms", static_cast<double>(s.exprLenUs) / 1e6, s.exprBlendMs), w - 60);
				if (Button(ix + w - 56, y, 12, 9, "-")) s.exprLenUs = std::max<int64_t>(100'000, s.exprLenUs - (s.shift ? 1'000'000 : 100'000));
				if (Button(ix + w - 42, y, 12, 9, "+")) s.exprLenUs += s.shift ? 1'000'000 : 100'000;
				if (Button(ix + w - 28, y, 12, 9, "-")) s.exprBlendMs = std::max(0, s.exprBlendMs - 50);
				if (Button(ix + w - 14, y, 12, 9, "+")) s.exprBlendMs = std::min(2000, s.exprBlendMs + 50);
				y += kRow;
				if (Button(ix, y, w, 10, "ADD EXPRESSION AT PLAYHEAD"))
				{
					Object e = *o;
					FaceClip c;
					c.startUs = t; c.lengthUs = s.exprLenUs; c.pose = pose; c.weight = s.exprWeight; c.blendMs = s.exprBlendMs;
					e.faceClips.push_back(c);
					s.scene.Edit(e, false);
				}
				y += 12;
				// the clips here
				int n = 0;
				for (size_t i = 0; i < o->faceClips.size() && n < 2; i++)
				{
					const FaceClip& c = o->faceClips[i];
					if (t < c.startUs - 3'000'000 || t > c.startUs + c.lengthUs + 3'000'000) continue;	// near the playhead
					n++;
					TextClip(ix, y + 1, Fmt("%.1fs %s %.1f (%.1fs)", static_cast<double>(c.startUs) / 1e6, c.pose.c_str(), c.weight, static_cast<double>(c.lengthUs) / 1e6), w - 16,
						ActiveFaceClip(o->faceClips, t) == &c ? kAccent : kTextCol);
					if (Button(ix + w - 12, y, 12, 9, "x")) { Object e = *o; e.faceClips.erase(e.faceClips.begin() + static_cast<long>(i)); s.scene.Edit(e, false); return; }
					y += kRow;
				}
				// idle
				const IdleKey* ik = IdleAt(o->idleKeys, t);
				Label(ix, y, "idle", ik ? (ik->on ? "on" : "off") : "on (no keys)", w - 70);
				const bool onB = Button(ix + w - 68, y, 32, 9, "ON HERE");
				const bool offB = Button(ix + w - 34, y, 34, 9, "OFF HERE");
				if (onB || offB)
				{
					const bool on = onB;
					Object e = *o;
					IdleKey k; k.tUs = t; k.on = on;
					e.idleKeys.erase(std::remove_if(e.idleKeys.begin(), e.idleKeys.end(), [&](const IdleKey& q) { return q.tUs == t; }), e.idleKeys.end());
					e.idleKeys.push_back(k);
					s.scene.Edit(e, false);
				}
				y += kRow + 2;
			}
			// ---- eyes
			Text(ix, y, "EYES", kAccent); y += 11;
			EyeKey cur;
			const bool haveEyes = EyesAt(o->eyeKeys, t, cur);
			const Object* tg = haveEyes && cur.mode == EyeKey::LookAt ? s.scene.Find(cur.target) : nullptr;
			Label(ix, y, "now", !haveEyes ? std::string("free (no keys)") : cur.mode == EyeKey::Free ? std::string("free") :
				cur.mode == EyeKey::LookAt ? "look at " + (tg ? tg->name : std::string("?")) : Fmt("aim %.0f %.0f", cur.pitch, cur.yaw), w);
			y += kRow;
			{
				const Object* pick = s.eyeTarget ? s.scene.Find(s.eyeTarget) : nullptr;
				if (!pick || pick->id == o->id || pick->type != ObjType::Actor)
				{
					s.eyeTarget = 0;
					for (auto& c : s.scene.objects) if (c.type == ObjType::Actor && c.id != o->id) { s.eyeTarget = c.id; break; }
					pick = s.eyeTarget ? s.scene.Find(s.eyeTarget) : nullptr;
				}
				Label(ix, y, "target", pick ? pick->name : std::string("(no other actor)"), w - 30);
				if (Button(ix + w - 28, y, 28, 9, "NEXT"))
				{
					bool after = false;
					int next = 0;
					for (auto& c : s.scene.objects)
					{
						if (c.type != ObjType::Actor || c.id == o->id) continue;
						if (!next) next = c.id;
						if (after) { next = c.id; break; }
						if (c.id == s.eyeTarget) after = true;
					}
					s.eyeTarget = next;
				}
				y += kRow;
			}
			Label(ix, y, "aim", Fmt("%.0f %.0f", s.eyePitch, s.eyeYaw), w - 60);
			if (Button(ix + w - 56, y, 12, 9, "^")) s.eyePitch = std::max(-40.0f, s.eyePitch - 5);
			if (Button(ix + w - 42, y, 12, 9, "v")) s.eyePitch = std::min(40.0f, s.eyePitch + 5);
			if (Button(ix + w - 28, y, 12, 9, "<")) s.eyeYaw = std::min(60.0f, s.eyeYaw + 5);
			if (Button(ix + w - 14, y, 12, 9, ">")) s.eyeYaw = std::max(-60.0f, s.eyeYaw - 5);
			y += kRow + 1;
			const float bw = (w - 4) / 3;
			int addMode = -1;
			if (Button(ix, y, bw, 10, "LOOK AT HERE", false, s.eyeTarget != 0)) addMode = EyeKey::LookAt;
			if (Button(ix + bw + 2, y, bw, 10, "AIM HERE", false, rig)) addMode = EyeKey::Aim;
			if (Button(ix + 2 * (bw + 2), y, bw, 10, "FREE HERE")) addMode = EyeKey::Free;
			if (addMode >= 0)
			{
				Object e = *o;
				EyeKey k;
				k.tUs = t;
				k.mode = static_cast<EyeKey::Mode>(addMode);
				k.target = addMode == EyeKey::LookAt ? s.eyeTarget : 0;
				k.pitch = s.eyePitch; k.yaw = s.eyeYaw;
				e.eyeKeys.erase(std::remove_if(e.eyeKeys.begin(), e.eyeKeys.end(), [&](const EyeKey& q) { return q.tUs == t; }), e.eyeKeys.end());
				e.eyeKeys.push_back(k);
				s.scene.Edit(e, false);
			}
			y += 12;
			if (!o->eyeKeys.empty() && Button(ix, y, w, 9, Fmt("CLEAR %d EYE KEY(S)", static_cast<int>(o->eyeKeys.size()))))
			{
				Object e = *o;
				e.eyeKeys.clear();
				s.scene.Edit(e, false);
			}
			return;
		}
		if (o && o->type == ObjType::Audio)
		{
			Text(ix, y, (o->music ? "MUSIC: " : "SOUNDS: ") + o->name, kAccent); y += 11;
			DrawField(ix, y, w, FIELD_MUSIC, s.soundPath); y += 13;
			if (Button(ix, y, w, 10, o->music ? "ADD MUSIC AT PLAYHEAD" : "ADD SOUND AT PLAYHEAD"))
			{
				if (s.soundPath.empty() || s.soundPath.back() == '/') s.host.Log(1, "Type the file first (e.g. music/kor_lite/korrib_action.mp3)");
				else
				{
					Object e = *o;
					SoundClip c;
					c.startUs = t;
					c.path = s.soundPath;
					if (o->music) c.lengthUs = 30'000'000;
					e.sounds.push_back(c);
					s.scene.Edit(e, false);
				}
			}
			y += 12;
			if (Button(ix, y, 66, 10, o->visible ? "PLAYS: ON" : "PLAYS: OFF", o->visible)) s.scene.SetVisible(o->id, !o->visible);
			y += 13;
			DrawSoundList(ix, y, o, w, 10);
			y += 4;
			Text(ix, y, o->music ? "music: one track at a time, plays on" : "sounds play where the scene starts them", kDim, 0.34f);
			return;
		}
		Text(ix, y, "AUDIO", kAccent); y += 12;
		Text(ix, y, "Select an actor for voice lines and", kDim); y += kRow;
		Text(ix, y, "face tracks, or add a track:", kDim); y += kRow + 3;
		if (Button(ix, y, w, 11, "ADD MUSIC TRACK")) s.scene.AddAudio("", true);
		y += 13;
		if (Button(ix, y, w, 11, "ADD SOUND TRACK")) s.scene.AddAudio("", false);
		y += 16;
		Text(ix, y, "Sounds start when the scene plays", kDim); y += kRow;
		Text(ix, y, "through their start (not when scrubbing).", kDim);
	}

	// ---- Phase 7: props ----
	void SpawnPropInFront()
	{
		Studio& s = S();
		float org[3], yaw;
		SpotInFront(org, yaw);
		const float ang[3] = { 0, yaw + 180.0f, 0 };	// facing the camera
		std::vector<std::pair<std::string, std::string>> kv = s.propKv;
		std::string cls = s.propModels ? "cs_model" : s.propClass;
		if (cls == "cs_model")
		{
			const std::string m = s.propModels ? s.propModel : "";
			bool have = false;
			for (auto& p : kv) have = have || p.first == "model";
			if (!have && !m.empty()) kv.emplace_back("model", m);
			if (!have && m.empty()) { s.host.Log(1, "Pick a model (MODELS list) or add a 'model' key"); return; }
		}
		const int id = s.scene.AddProp(cls, kv, org, ang);
		if (id) { s.tab = TAB_SCENE; s.host.Log(0, "Spawned " + s.scene.Find(id)->name + " (" + cls + ")"); }
	}

	// where a prop is now: on its actor's bone, on its keys, or where it was put
	bool PropPose(const Object& o, const int64_t t, float org[3], float ang[3])
	{
		Studio& s = S();
		if (o.attachTo)
		{
			const Object* a = s.scene.Find(o.attachTo);
			if (a && a->entNum >= 0 && AttachPose(a->entNum, o.bolt, o.attachOffset, o.attachAngles, org, ang)) return true;
		}
		float fov;
		if (!o.keys.empty() && EvaluateCameraKeys(o.keys, t, org, ang, fov)) return true;
		VectorCopy(o.origin, org);
		VectorCopy(o.angles, ang);
		return true;
	}

	// props a key or an actor moves, every frame
	void EvaluateProps()
	{
		Studio& s = S();
		ClearPropRenderPoses();
		const int64_t t = s.scene.clock.timeUs;
		for (auto& o : s.scene.objects)
		{
			if (o.type != ObjType::Prop || o.entNum < 0) continue;
			if (!PropAlive(o.entNum)) { o.entNum = -1; continue; }		// broken, picked up, used up
			if (!o.attachTo && o.keys.empty()) continue;
			if (s.dragging && o.id == s.scene.selected && !o.attachTo) continue;
			float org[3], ang[3];
			PropPose(o, t, org, ang);
			s.host.SetPropTransform(o.entNum, org, ang);
			if (o.attachTo) SetPropRenderPose(o.entNum, org, ang);
		}
	}

	void DrawPropInspector(const float x, float y, Object* o)
	{
		Studio& s = S();
		const float w = kRightW - 12;
		Label(x, y, "name", o->name); y += kRow;
		if (Button(x + kRightW - 50, y - kRow, 38, 9, "RENAME")) BeginField(FIELD_RENAME, o->name);
		if (s.field == FIELD_RENAME) { DrawField(x, y, w, FIELD_RENAME, o->name); y += 13; }
		Label(x, y, "class", o->classname); y += kRow;
		Label(x, y, "entity", o->entNum >= 0 ? Fmt("%d", o->entNum) : std::string("gone (broken / picked up)")); y += kRow;
		float org[3], ang[3];
		PropPose(*o, s.scene.clock.timeUs, org, ang);
		Label(x, y, "position", Fmt("%.0f %.0f %.0f", org[0], org[1], org[2])); y += kRow;
		Label(x, y, "angles", Fmt("%.0f %.0f %.0f", ang[0], ang[1], ang[2])); y += kRow + 2;
		// its keys
		Text(x, y, "KEYS (key value)", kAccent); y += 10;
		int shown = 0;
		for (auto& p : o->kv)
		{
			if (shown++ >= 5) { Text(x, y, Fmt("+%d more", static_cast<int>(o->kv.size()) - 5), kDim); y += kRow; break; }
			TextClip(x, y + 1, p.first + "  " + p.second, w - 14, kTextCol);
			if (Button(x + w - 12, y, 12, 9, "x")) { s.scene.SetPropKey(o->id, p.first, ""); return; }
			y += kRow;
		}
		if (s.field == FIELD_PROPSET) { DrawField(x, y, w, FIELD_PROPSET, ""); y += 13; }
		else if (Button(x, y, w, 10, "SET KEY (e.g. health 100)")) BeginField(FIELD_PROPSET, ""), y += 13;
		else y += 12;
		const float bw = (w - 4) / 3;
		if (Button(x, y, bw, 11, "KEY (K)")) SetKeyAtPlayhead();
		if (Button(x + bw + 2, y, bw, 11, "RESPAWN")) { Object a = *o; s.scene.SetPropKey(o->id, "cs_respawn", "1"); s.scene.SetPropKey(o->id, "cs_respawn", ""); }
		if (Button(x + 2 * (bw + 2), y, bw, 11, "DUPLICATE")) s.scene.Duplicate(o->id);
		y += 14;
		// held by an actor
		{
			const Object* a = o->attachTo ? s.scene.Find(o->attachTo) : nullptr;
			Label(x, y, "held by", a ? a->name : "(nobody)", w - 30);
			if (Button(x + w - 28, y, 28, 9, "NEXT"))
			{
				int next = 0;
				bool after = o->attachTo == 0;
				for (auto& c : s.scene.objects)
				{
					if (c.type != ObjType::Actor) continue;
					if (after) { next = c.id; break; }
					if (c.id == o->attachTo) after = true;
				}
				const float zero[3] = { 0, 0, 0 };
				s.scene.SetAttach(o->id, next, o->bolt.empty() ? "*r_hand" : o->bolt, next ? o->attachOffset : zero, next ? o->attachAngles : zero, false);
				return;
			}
			y += kRow;
			if (o->attachTo)
			{
				static const char* kBolts[] = { "*r_hand", "*l_hand", "*head_top", "*head_front", "*back", "*chestg", "*hip_r", "*hip_l", "*r_hand_cap_r_arm", "*l_hand_cap_l_arm" };
				Label(x, y, "bone", o->bolt, w - 58);
				if (Button(x + w - 56, y, 26, 9, "NEXT"))
				{
					int i = 0;
					while (i < 10 && o->bolt != kBolts[i]) i++;
					s.scene.SetAttach(o->id, o->attachTo, kBolts[(i + 1) % 10], o->attachOffset, o->attachAngles, false);
					return;
				}
				if (Button(x + w - 28, y, 28, 9, "TYPE")) BeginField(FIELD_BOLT, o->bolt);
				y += kRow;
				if (s.field == FIELD_BOLT) { DrawField(x, y, w, FIELD_BOLT, o->bolt); y += 13; }
				float off[3] = { o->attachOffset[0], o->attachOffset[1], o->attachOffset[2] };
				float aoff[3] = { o->attachAngles[0], o->attachAngles[1], o->attachAngles[2] };
				const float st = s.shift ? 8.0f : 1.0f, as = s.shift ? 45.0f : 15.0f;
				bool ch = false;
				Label(x, y, "offset", Fmt("%.0f %.0f %.0f", off[0], off[1], off[2]), w); y += kRow;
				const char* axn[3] = { "X", "Y", "Z" };
				for (int i = 0; i < 3; i++)
				{
					const float bx = x + i * (w / 3);
					if (Button(bx, y, 12, 9, "-")) { off[i] -= st; ch = true; }
					Text(bx + 14, y + 0.5f, axn[i], kDim);
					if (Button(bx + 22, y, 12, 9, "+")) { off[i] += st; ch = true; }
				}
				y += kRow;
				Label(x, y, "turn", Fmt("%.0f %.0f %.0f", aoff[0], aoff[1], aoff[2]), w); y += kRow;
				const char* an[3] = { "P", "Y", "R" };
				for (int i = 0; i < 3; i++)
				{
					const float bx = x + i * (w / 3);
					if (Button(bx, y, 12, 9, "-")) { aoff[i] -= as; ch = true; }
					Text(bx + 14, y + 0.5f, an[i], kDim);
					if (Button(bx + 22, y, 12, 9, "+")) { aoff[i] += as; ch = true; }
				}
				y += kRow + 1;
				if (ch) s.scene.SetAttach(o->id, o->attachTo, o->bolt, off, aoff, true);
				if (!AttachPose(a ? a->entNum : -1, o->bolt, off, aoff, org, ang)) Text(x, y, "that bone isn't on this model", kWarn);
			}
		}
	}

	void DrawPropsPanel(const float x, const float y0, const float h)
	{
		Studio& s = S();
		const float ix = x + 6, w = kRightW - 12;
		float y = y0 + 2;
		Text(ix, y, "PROPS: MAP OBJECTS", kAccent); y += 11;
		if (Button(ix, y, w / 2 - 1, 10, "ENTITY CLASSES", !s.propModels)) { s.propModels = false; s.propScroll = 0; }
		if (Button(ix + w / 2 + 1, y, w / 2 - 1, 10, "MODELS", s.propModels)) { s.propModels = true; s.propScroll = 0; }
		y += 12;
		std::vector<std::string> list;
		if (!s.propModels)
		{
			if (s.propClasses.empty()) s.propClasses = ListSpawnClasses();
			DrawField(ix, y, w, FIELD_PROPSEARCH, s.propSearch); y += 13;
			for (auto& c : s.propClasses)
				if (s.propSearch.empty() || Q_stristr(c.c_str(), s.propSearch.c_str())) list.push_back(c);
		}
		else
		{
			DrawField(ix, y, w, FIELD_PROPDIR, s.propDir); y += 13;
			if (s.propFilesDir != s.propDir) { s.propFiles = ListModels(s.propDir); s.propFilesDir = s.propDir; }
			list = s.propFiles;
		}
		// the list
		const float listH = h - 118;
		const int rows = std::max(1, static_cast<int>(listH / kRow));
		if (Inside(s.cx, s.cy, x, y, kRightW, listH) && s.wheel) { s.propScroll -= static_cast<float>(s.wheel) * 3; s.wheel = 0; }
		s.propScroll = std::max(0.0f, std::min(s.propScroll, static_cast<float>(std::max(0, static_cast<int>(list.size()) - rows))));
		Rect(x + 2, y, kRightW - 4, rows * kRow, kPanelDark);
		for (int i = 0; i < rows && static_cast<size_t>(i) + static_cast<size_t>(s.propScroll) < list.size(); i++)
		{
			const std::string& it = list[static_cast<size_t>(i) + static_cast<size_t>(s.propScroll)];
			const float ry = y + i * kRow;
			const bool sel = s.propModels ? it == s.propModel : it == s.propClass;
			if (sel) Rect(x + 2, ry, kRightW - 4, kRow, kSel);
			std::string shown = it;
			if (s.propModels && shown.size() > s.propDir.size() + 1) shown = shown.substr(s.propDir.size() + 1);
			TextClip(ix, ry + 0.5f, shown, w, it == "cs_model" ? kAccent : kTextCol);
			if (TakeClick(x, ry, kRightW, kRow))
			{
				if (s.propModels) s.propModel = it; else s.propClass = it;
				if (s.doubleClick) SpawnPropInFront();
			}
		}
		if (list.empty()) Text(ix, y + 1, s.propModels ? "no .md3 / .glm files here" : "nothing matches", kDim);
		y += rows * kRow + 3;
		Label(ix, y, "spawn", s.propModels ? (s.propModel.empty() ? std::string("(pick a model)") : s.propModel) : s.propClass, w); y += kRow;
		// extra keys for the spawn
		std::string kvs;
		for (auto& p : s.propKv) kvs += (kvs.empty() ? "" : ", ") + p.first + " " + p.second;
		Label(ix, y, "with", kvs.empty() ? std::string("(no extra keys)") : kvs, w); y += kRow;
		if (s.field == FIELD_PROPKV) { DrawField(ix, y, w, FIELD_PROPKV, ""); y += 13; }
		else
		{
			if (Button(ix, y, w / 2 - 1, 10, "ADD KEY")) BeginField(FIELD_PROPKV, "");
			if (Button(ix + w / 2 + 1, y, w / 2 - 1, 10, "CLEAR KEYS", false, !s.propKv.empty())) s.propKv.clear();
			y += 12;
		}
		if (Button(ix, y, w, 12, "SPAWN IN FRONT OF VIEW")) SpawnPropInFront();
		y += 14;
		Text(ix, y, "double-click spawns; keys like 'model',", kDim, 0.34f); y += 9;
		Text(ix, y, "'health', 'fxFile', 'spawnflags', 'target'", kDim, 0.34f);
	}

	// ---- moving actors with the mouse ----
	bool SelectedActorTransform(float org[3], float& yaw, int& id)
	{
		Studio& s = S();
		const Object* o = s.scene.Find(s.scene.selected);
		if (!o || (o->type != ObjType::Actor && o->type != ObjType::Prop) || o->entNum < 0) return false;
		if (o->type == ObjType::Prop && o->attachTo) return false;	// held by an actor: offsets move it
		ShownTransform(*o, org, yaw);
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
		if (!SelectedActorTransform(org, yaw, id)) return false;
		if (s.scene.Find(id)->type == ObjType::Prop)
		{
			info = EntInfo();
			if (!PropBounds(s.scene.Find(id)->entNum, info.mins, info.maxs)) return false;
		}
		else if (!EntityInfo(s.scene.Find(id)->entNum, info)) return false;
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
		VectorCopy(org, s.dragOrgStart);
		{
			Object* o = s.scene.Find(id);
			const Take* tk = o && o->prop ? Scene::ActiveTake(*o) : nullptr;
			if (tk) { VectorCopy(tk->offset, s.dragTakeOff); s.dragTakeYaw = tk->yawOffset; }
		}
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
		Object* o = s.scene.Find(id);
		if (o->type == ObjType::Prop)
		{
			float full[3] = { o->angles[0], ang[YAW], o->angles[2] };
			if (!o->keys.empty())
			{
				float kp[3], ka[3];
				PropPose(*o, s.scene.clock.timeUs, kp, ka);
				const int64_t frameUs = 1'000'000 / s.scene.clock.fps;
				TransformKey k;
				k.tUs = s.scene.clock.timeUs / frameUs * frameUs;
				for (auto& e : o->keys) if (e.tUs == k.tUs) k = e;
				VectorCopy(org, k.origin);
				k.origin[2] = s.dragZ;
				k.pitch = ka[PITCH]; k.yaw = ang[YAW]; k.roll = ka[ROLL];
				s.scene.SetKey(id, k, true);
				return;
			}
			if (fabsf(org[0] - o->origin[0]) > 0.01f || fabsf(org[1] - o->origin[1]) > 0.01f || fabsf(ang[YAW] - o->angles[YAW]) > 0.01f)
				s.scene.SetTransform(id, org, full, true);
			return;
		}
		const Take* tk = o->prop ? Scene::ActiveTake(*o) : nullptr;
		if (tk)
		{
			// a take moves it: move / turn the whole take
			Take e = *tk;
			if (s.dragTurn) e.yawOffset = AngleNormalize180(s.dragTakeYaw + (ang[YAW] - s.dragStartYaw));
			else for (int i = 0; i < 2; i++) e.offset[i] = s.dragTakeOff[i] + (org[i] - s.dragOrgStart[i]);
			s.scene.SetTake(id, e, true);
			return;
		}
		if (o->prop && !o->keys.empty())
		{
			// keys move it: key it where it's dropped, at the playhead
			const int64_t frameUs = 1'000'000 / s.scene.clock.fps;
			TransformKey k;
			k.tUs = s.scene.clock.timeUs / frameUs * frameUs;
			for (auto& e : o->keys) if (e.tUs == k.tUs) k = e;
			VectorCopy(org, k.origin);
			k.origin[2] = s.dragZ;
			k.yaw = ang[YAW];
			s.scene.SetKey(id, k, true);
			s.host.SetActorTransform(o->entNum, k.origin, ang);
			return;
		}
		if (fabsf(org[0] - o->origin[0]) > 0.01f || fabsf(org[1] - o->origin[1]) > 0.01f || fabsf(ang[YAW] - yaw) > 0.01f)
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
			if (DrawTakeEditor(ix, y)) break;
			if (DrawKeyEditor(ix, y)) break;
			Text(ix, y, "INSPECTOR", kAccent); y += 12;
			if (sel && sel->type == ObjType::Camera) DrawCameraInspector(ix, y, sel);
			else if (sel && sel->type == ObjType::Prop) DrawPropInspector(ix, y, sel);
			else if (sel && sel->type == ObjType::Audio) DrawAudioPanel(x, y0, h);
			else if (sel && sel->type == ObjType::Events)
			{
				Label(ix, y, "name", sel->name); y += kRow;
				Label(ix, y, "events", Fmt("%d", static_cast<int>(sel->Events().size()))); y += kRow;
				Label(ix, y, "starts", Fmt("%.2fs", static_cast<double>(sel->eventsStartUs) / 1e6)); y += kRow;
				if (!sel->Events().empty()) { Label(ix, y, "length", Fmt("%.2fs", static_cast<double>(sel->Events().back().tUs) / 1e6)); y += kRow; }
				int fx = 0, snd = 0, chr = 0;
				for (auto& e : sel->Events()) { if (!e.fx.empty()) fx++; else if (!e.sound.empty()) snd++; if (!e.temp) chr++; }
				Label(ix, y, "kinds", Fmt("%d effects, %d sounds, %d on characters", fx, snd, chr)); y += kRow + 2;
				if (Button(ix, y, 66, 11, sel->visible ? "PLAYS: ON" : "PLAYS: OFF", sel->visible)) s.scene.SetVisible(sel->id, !sel->visible);
				y += 14;
				Text(ix, y, "effects, sounds, hits, voices of the", kDim); y += kRow;
				Text(ix, y, "fight; they play while the scene plays", kDim); y += kRow;
				Text(ix, y, "drag on the timeline to retime; DEL", kDim);
			}
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
			y += 13;
			if (Button(ix, y, kRightW - 12, 11, "RECORD NEW PERFORMER (YOU)", false, s.rec == Studio::REC_IDLE)) { s.field = FIELD_NONE; StartRecording(0); break; }
			y += 11;
			Text(ix, y, Fmt("plays back as your NPC, or %s", s.npcType.c_str()), kDim, 0.36f);
			y += 9;
			if (Button(ix, y, 80, 10, Fmt("COUNTDOWN %ds", s.recCountdownSec))) s.recCountdownSec = (s.recCountdownSec + 1) % 6;
			if (Button(ix + 82, y, kRightW - 94, 10, "GHOST OTHERS", s.recGhostOthers)) s.recGhostOthers = !s.recGhostOthers;
			y += 12;
			{
				static const char* kNpcModes[] = { "RECORD NPCS: NO", "RECORD NPCS: SCENE AI", "RECORD NPCS: ALL NEARBY" };
				if (Button(ix, y, 98, 10, kNpcModes[s.recNpcs], s.recNpcs > 0)) s.recNpcs = (s.recNpcs + 1) % 3;
				if (Button(ix + 100, y, kRightW - 112, 10, "SOLID TAKES", s.recSolidTakes)) s.recSolidTakes = !s.recSolidTakes;
				y += 12;
				if (Button(ix, y, 98, 10, Fmt("TAKE BLEND %d ms", s.takeBlendMs))) s.takeBlendMs = s.takeBlendMs >= 200 ? 0 : s.takeBlendMs + 50;
				y += 12;
				if (Button(ix, y, 98, 10, s.irOn ? "INSTANT REPLAY: ON" : "INSTANT REPLAY: OFF", s.irOn)) SetInstantReplay(!s.irOn);
				if (Button(ix + 100, y, kRightW - 112, 10, Fmt("KEEP %ds", s.irSeconds))) s.irSeconds = s.irSeconds >= 60 ? 10 : s.irSeconds + 10;
				y += 11;
				Text(ix, y, "play; F9 puts the last seconds here", kDim, 0.34f);
				y += 13;
			}
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
			Text(ix, y, "K: key the camera   CUT HERE: cut to it", kDim); y += kRow;
		Text(ix, y, "C: director view (follows the cuts)", kDim); y += kRow;
		if (Button(ix, y, 66, 11, "PATHS", s.showPaths)) s.showPaths = !s.showPaths;
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
			DrawAudioPanel(x, y0, h);
			break;
		case TAB_PROPS:
			DrawPropsPanel(x, y0, h);
			break;
		}
	}

	// the visible part of the timeline
	void ViewRange(int64_t& start, int64_t& len)
	{
		Studio& s = S();
		const int64_t total = std::max<int64_t>(1, s.scene.clock.lengthUs);
		len = s.viewLenUs > 0 ? std::min(s.viewLenUs, total) : total;
		start = std::max<int64_t>(0, std::min(s.viewStartUs, total - len));
	}

	float TimeToX(const int64_t us, const float x0, const float w)
	{
		int64_t vs, vl;
		ViewRange(vs, vl);
		return x0 + static_cast<float>(static_cast<double>(us - vs) / static_cast<double>(vl)) * w;
	}

	int64_t XToTime(const float x, const float x0, const float w)
	{
		int64_t vs, vl;
		ViewRange(vs, vl);
		return vs + static_cast<int64_t>(static_cast<double>((x - x0) / w) * static_cast<double>(vl));
	}

	// snap a time to the playhead and the edges of clips, takes and keys within 5 pixels (skip: the dragged item)
	int64_t Snap(int64_t t, const float lw, const int skipObj, const int skipClip, const int skipTake, const int64_t skipKey = -1)
	{
		Studio& s = S();
		if (!s.snap || s.shift) return t;	// SHIFT: free
		int64_t vs, vl;
		ViewRange(vs, vl);
		const int64_t range = static_cast<int64_t>(5.0 / lw * static_cast<double>(vl));
		int64_t best = t, bestD = range + 1;
		const auto tryT = [&](const int64_t c) { const int64_t d = c > t ? c - t : t - c; if (d < bestD) { bestD = d; best = c; } };
		tryT(s.scene.clock.timeUs);
		tryT(0);
		for (auto& o : s.scene.objects)
		{
			for (size_t i = 0; i < o.clips.size(); i++)
				if (!(o.id == skipObj && static_cast<int>(i) == skipClip)) { tryT(o.clips[i].startUs); tryT(o.clips[i].startUs + o.clips[i].lengthUs); }
			for (auto& tk : o.takes)
				if (!(o.id == skipObj && tk.id == skipTake)) { tryT(tk.startUs); tryT(tk.EndUs()); }
			for (auto& k : o.keys)
				if (!(o.id == skipObj && k.tUs == skipKey)) tryT(k.tUs);
		}
		return bestD <= range ? best : t;
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
		if (B(30, "SNAP", s.snap)) s.snap = !s.snap;
		if (B(36, "CLASH", s.clashes)) s.clashes = !s.clashes;
		x += 4;
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
		int64_t vs, vl;
		ViewRange(vs, vl);
		const double secs = static_cast<double>(vl) / 1e6;
		const double every = secs <= 2 ? 0.25 : (secs <= 5 ? 0.5 : (secs <= 20 ? 1 : (secs <= 60 ? 5 : (secs <= 300 ? 15 : 60))));
		for (double sec = std::floor(static_cast<double>(vs) / 1e6 / every) * every; sec <= static_cast<double>(vs + vl) / 1e6; sec += every)
		{
			if (sec < 0) continue;
			const float tx = TimeToX(static_cast<int64_t>(sec * 1e6), lx, lw);
			Rect(tx, y0 + 6, 0.6f, kRulerH - 6, kLine);
			Text(tx + 1.5f, y0 + 1, every < 1 ? Fmt("%.2f", sec) : Fmt("%d", static_cast<int>(sec)), kDim, 0.34f);
		}
		const double framePx = lw / std::max(1.0, static_cast<double>(vl) * c.fps / 1e6);
		if (framePx >= 3)
			for (int64_t f = vs * c.fps / 1'000'000; f * 1'000'000 / c.fps <= vs + vl; f++)
				Rect(TimeToX(f * 1'000'000 / c.fps, lx, lw), y0 + kRulerH - 3, 0.4f, 3, kLine);
		if (s.viewLenUs > 0) Text(lx + lw - 70, y0 + 1, Fmt("zoom %.1fs  CTRL+wheel", static_cast<double>(vl) / 1e6), kAccent, 0.32f);

		// dragging a clip along its lane (whole frames)
		if (s.clipDragIndex >= 0)
		{
			Object* o = s.scene.Find(s.clipDragObj);
			if (!s.lmb || !o || s.clipDragIndex >= static_cast<int>(o->clips.size())) s.clipDragIndex = -1;
			else
			{
				const int64_t frameUs = 1000000 / c.fps;
				int64_t start = XToTime(s.cx, lx, lw) - s.clipDragGrabUs;
				start = std::max<int64_t>(0, (start + frameUs / 2) / frameUs * frameUs);
				{
					const AnimClip& dc = o->clips[static_cast<size_t>(s.clipDragIndex)];
					const int64_t a = Snap(start, lw, o->id, s.clipDragIndex, 0), b = Snap(start + dc.lengthUs, lw, o->id, s.clipDragIndex, 0) - dc.lengthUs;
					start = std::max<int64_t>(0, a != start ? a : b);	// either edge may snap
				}
				if (start != o->clips[static_cast<size_t>(s.clipDragIndex)].startUs)
				{
					AnimClip moved = o->clips[static_cast<size_t>(s.clipDragIndex)];
					moved.startUs = start;
					s.scene.SetClip(o->id, s.clipDragIndex, moved, true);
					s.clipDragIndex = s.scene.selectedClip;
				}
			}
		}

		// dragging an events track
		if (s.evDragObj)
		{
			const Object* o = s.scene.Find(s.evDragObj);
			if (!s.lmb || !o) s.evDragObj = 0;
			else
			{
				const int64_t frameUs = 1000000 / c.fps;
				int64_t start = XToTime(s.cx, lx, lw) - s.evDragGrabUs;
				start = std::max<int64_t>(0, (start + frameUs / 2) / frameUs * frameUs);
				start = Snap(start, lw, o->id, -1, 0);
				if (start != o->eventsStartUs) s.scene.SetEventsStart(o->id, start, true);
			}
		}

		// dragging a position key (whole frames)
		if (s.keyDrag)
		{
			if (!s.lmb || !s.scene.Find(s.keyObj)) s.keyDrag = false;
			else
			{
				const int64_t frameUs = 1000000 / c.fps;
				int64_t at = std::max<int64_t>(0, (XToTime(s.cx, lx, lw) + frameUs / 2) / frameUs * frameUs);
				at = Snap(at, lw, s.keyObj, -1, 0, s.keyUs);
				if (at != s.keyUs && s.scene.MoveKey(s.keyObj, s.keyUs, at, true)) s.keyUs = at;
			}
		}

		// dragging a take along its lane (whole frames)
		if (s.takeDragId)
		{
			Object* o = s.scene.Find(s.takeDragObj);
			Take* tk = o ? Scene::FindTake(*o, s.takeDragId) : nullptr;
			if (!s.lmb || !tk) s.takeDragId = 0;
			else
			{
				const int64_t frameUs = 1000000 / c.fps;
				int64_t start = XToTime(s.cx, lx, lw) - s.takeDragGrabUs;
				start = std::max<int64_t>(0, (start + frameUs / 2) / frameUs * frameUs);
				{
					const int64_t a = Snap(start, lw, o->id, -1, tk->id), b = Snap(start + tk->LengthUs(), lw, o->id, -1, tk->id) - tk->LengthUs();
					start = std::max<int64_t>(0, a != start ? a : b);
				}
				if (start != tk->startUs) s.scene.SetTakeStart(o->id, tk->id, start, true);
			}
		}

		// scrub: click or drag on the ruler / lanes
		const bool overTime = Inside(s.cx, s.cy, lx, y0, lw, h);
		if (s.clicked && Inside(s.clickX, s.clickY, lx, y0, lw, h)) { s.clicked = false; s.scrubbing = true; }
		if (!s.lmb) s.scrubbing = false;
		if (s.scrubbing)
		{
			const int64_t t = std::max<int64_t>(0, std::min(c.lengthUs, XToTime(s.cx, lx, lw)));
			c.playing = false;
			c.SetFrame((t * c.fps + 500'000) / 1'000'000);
		}

		// tracks
		float ry = y0 + kRulerH + 1;
		Text(4, ry + 1, "TRACKS", kAccent);
		// the cut track: which camera the director shows when (first row)
		{
			const float cy0 = ry + 1;
			Text(60, cy0 + 0.5f, "CUTS", kCam, 0.36f);
			if (Button(84, cy0, 40, 8, "DIRECTOR", s.director)) { s.director = !s.director; if (s.director) { s.pilotId = 0; s.camActive = true; } }
			Rect(lx, cy0, lw, kRow - 2, kLane);
			const auto cuts = s.scene.Cuts();
			for (size_t i = 0; i < cuts.size(); i++)
			{
				const int64_t a0 = cuts[i].first, a1 = i + 1 < cuts.size() ? cuts[i + 1].first : c.lengthUs;
				const Object* cam = s.scene.Find(cuts[i].second);
				const float x0 = TimeToX(a0, lx, lw), x1 = TimeToX(a1, lx, lw);
				if (x1 < lx || x0 > lx + lw) continue;
				const float shade = 0.35f + 0.25f * static_cast<float>(i % 2);
				const vec4_t seg = { kCam[0] * shade, kCam[1] * shade, kCam[2] * shade, 0.85f };
				const float sx0 = std::max(lx, x0), sx1 = std::min(lx + lw, x1);
				Rect(sx0, cy0, sx1 - sx0, kRow - 2, seg);
				const bool csel = s.cutCam == cuts[i].second && s.cutUs == a0;
				Rect(x0 - 0.6f, cy0 - 1, 1.6f, kRow, csel ? kAccent : kTextCol);
				TextClip(sx0 + 3, cy0 + 0.5f, cam ? cam->name : "?", std::max(0.0f, sx1 - sx0 - 4), kTextCol);
				if (s.clicked && Inside(s.clickX, s.clickY, x0 - 3, cy0 - 1, 6, kRow))
				{
					s.clicked = false;
					s.cutCam = cuts[i].second;
					s.cutUs = a0;
					s.cutDrag = true;
					s.scene.selected = cuts[i].second;
					s.lastSel = s.scene.selected;
					s.keyObj = 0;
				}
				else if (s.clicked && Inside(s.clickX, s.clickY, sx0, cy0, sx1 - sx0, kRow - 2))
				{
					s.clicked = false;
					s.scene.selected = cuts[i].second;
					s.lastSel = s.scene.selected;
					s.cutCam = 0;
				}
			}
			if (cuts.empty()) Text(lx + 3, cy0 + 0.5f, "no cuts: select a camera > CUT HERE (at the playhead)", kDim, 0.34f);
			// drag a cut in time
			if (s.cutDrag)
			{
				if (!s.lmb || !s.scene.Find(s.cutCam)) s.cutDrag = false;
				else
				{
					const int64_t frameUs = 1000000 / c.fps;
					int64_t at = std::max<int64_t>(0, (XToTime(s.cx, lx, lw) + frameUs / 2) / frameUs * frameUs);
					at = Snap(at, lw, 0, -1, 0);
					if (at != s.cutUs && s.scene.MoveCut(s.cutCam, s.cutUs, at, true)) s.cutUs = at;
				}
			}
		}
		ry += kRow;
		const int maxRows = static_cast<int>((h - kRulerH - 2) / kRow) - 1;	// the cut track takes a row
		if ((overTime || Inside(s.cx, s.cy, 0, y0, kW, h)) && s.wheel)
		{
			int64_t vs2, vl2;
			ViewRange(vs2, vl2);
			if (s.ctrl && overTime)
			{
				// zoom about the time under the cursor
				const int64_t at = XToTime(s.cx, lx, lw);
				const int64_t nl = std::max<int64_t>(500'000, std::min(c.lengthUs, static_cast<int64_t>(static_cast<double>(vl2) * (s.wheel > 0 ? 0.7 : 1.43))));
				s.viewStartUs = std::max<int64_t>(0, at - static_cast<int64_t>(static_cast<double>(at - vs2) * static_cast<double>(nl) / static_cast<double>(vl2)));
				s.viewLenUs = nl >= c.lengthUs ? 0 : nl;
			}
			else if (s.shift) s.viewStartUs = std::max<int64_t>(0, vs2 - static_cast<int64_t>(s.wheel) * vl2 / 8);	// pan
			else s.timelineScroll -= static_cast<float>(s.wheel);
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
			TextClip(14, yy + 1, o.name, kLeftW - 18, o.type == ObjType::Camera ? kCam : (o.type == ObjType::Events ? kWarn : kTextCol));
			Rect(lx, yy + 1, lw, kRow - 2, kLane);
			if (o.type == ObjType::Actor)
				for (auto& tk : o.takes)
				{
					const bool act = tk.id == o.activeTake;
					if (!act && !(o.id == s.scene.selected && tk.id == s.scene.selectedTake)) continue;	// only the active (or selected) take
					const float tx0 = TimeToX(tk.startUs, lx, lw), tx1 = TimeToX(tk.EndUs(), lx, lw);
					const bool tsel = o.id == s.scene.selected && tk.id == s.scene.selectedTake;
					const vec4_t body = { 0.62f, 0.36f, 0.85f, act ? (tsel ? 0.85f : 0.6f) : 0.3f };
					Rect(tx0, yy + 1, std::max(2.0f, tx1 - tx0), kRow - 2, body);
					if (tsel) Frame(tx0, yy + 1, std::max(2.0f, tx1 - tx0), kRow - 2, kAccent);
					if (o.clips.empty()) TextClip(tx0 + 2, yy + 1.5f, "REC " + tk.name, std::max(0.0f, tx1 - tx0 - 3), kTextCol);
					// clips are drawn on top and take the click first; the take takes what's left
					bool onClip = false;
					for (auto& c2 : o.clips)
					{
						const float a0 = TimeToX(c2.startUs, lx, lw), a1 = TimeToX(c2.startUs + c2.lengthUs, lx, lw);
						onClip = onClip || Inside(s.clickX, s.clickY, a0, yy, std::max(4.0f, a1 - a0), kRow);
					}
					if (s.clicked && !onClip && Inside(s.clickX, s.clickY, tx0, yy, std::max(4.0f, tx1 - tx0), kRow))
					{
						s.clicked = false;
						s.scene.selected = o.id;
						s.lastSel = o.id;
						s.levelSel = -1;
						s.scene.selectedClip = -1;
						s.scene.selectedTake = tk.id;
						s.tab = TAB_SCENE;
						s.takeDragObj = o.id;
						s.takeDragId = tk.id;
						s.takeDragGrabUs = XToTime(s.clickX, lx, lw) - tk.startUs;
						s.keyObj = 0;
					}
				}
			// voice lines (teal, top) and expressions (pink, bottom) on actors; sound clips on audio tracks
			{
				const vec4_t vc = { 0.30f, 0.85f, 0.85f, 0.9f }, fc = { 0.95f, 0.45f, 0.75f, 0.85f };
				const bool audio = o.type == ObjType::Audio;
				for (size_t si = 0; si < o.sounds.size(); si++)
				{
					const SoundClip& c = o.sounds[si];
					const float a0 = TimeToX(c.startUs, lx, lw), a1 = std::max(a0 + 2, TimeToX(c.startUs + c.lengthUs, lx, lw));
					if (a1 < lx || a0 > lx + lw) continue;
					const bool ssel = s.audioSelObj == o.id && s.audioSelIdx == static_cast<int>(si);
					if (audio)
					{
						const vec4_t ac = { 0.30f, 0.70f, 0.75f, o.visible ? 0.75f : 0.25f };
						Rect(a0, yy + 1, a1 - a0, kRow - 2, ac);
						std::string nm = c.path;
						const size_t sl = nm.find_last_of('/');
						if (sl != std::string::npos) nm = nm.substr(sl + 1);
						TextClip(a0 + 2, yy + 1.5f, nm, std::max(0.0f, a1 - a0 - 3), kTextCol);
					}
					else Rect(a0, yy + 1, a1 - a0, 2, vc);
					if (ssel) Frame(a0, yy + 1, a1 - a0, audio ? kRow - 2 : 2, kAccent);
					if (s.clicked && Inside(s.clickX, s.clickY, a0, yy, a1 - a0, audio ? kRow : 3))
					{
						s.clicked = false;
						s.scene.selected = o.id; s.lastSel = o.id; s.levelSel = -1;
						s.audioSelObj = o.id; s.audioSelIdx = static_cast<int>(si);
						s.tab = TAB_AUDIO;
					}
				}
				for (auto& c : o.faceClips)
				{
					const float a0 = TimeToX(c.startUs, lx, lw), a1 = std::max(a0 + 2, TimeToX(c.startUs + c.lengthUs, lx, lw));
					if (a1 < lx || a0 > lx + lw) continue;
					Rect(a0, yy + kRow - 3, a1 - a0, 2, fc);
				}
				for (auto& k : o.eyeKeys)
				{
					const float kx = TimeToX(k.tUs, lx, lw);
					if (kx >= lx && kx <= lx + lw) Rect(kx - 0.5f, yy + 1, 1, 3, fc);
				}
				for (auto& k : o.idleKeys)
				{
					const float kx = TimeToX(k.tUs, lx, lw);
					if (kx >= lx && kx <= lx + lw) Rect(kx - 0.5f, yy + kRow - 5, 1, 4, k.on ? vc : kRed);
				}
			}
			// world events: a bar from the first to the last, a tick per event
			if (o.type == ObjType::Events && !o.Events().empty())
			{
				const int64_t e0 = o.eventsStartUs + o.Events().front().tUs, e1 = o.eventsStartUs + o.Events().back().tUs;
				const float bx0 = TimeToX(e0, lx, lw), bx1 = std::max(bx0 + 2, TimeToX(e1, lx, lw));
				const bool esel = o.id == s.scene.selected;
				const vec4_t bar = { 0.85f, 0.45f, 0.20f, o.visible ? (esel ? 0.55f : 0.35f) : 0.12f };
				const vec4_t tick = { 1.0f, 0.75f, 0.35f, o.visible ? 0.9f : 0.3f };
				Rect(bx0, yy + 2, bx1 - bx0, kRow - 4, bar);
				int64_t vs3, vl3;
				ViewRange(vs3, vl3);
				float lastX = -10;
				for (auto& e : o.Events())
				{
					const int64_t at = o.eventsStartUs + e.tUs;
					if (at < vs3 || at > vs3 + vl3) continue;
					const float ex = TimeToX(at, lx, lw);
					if (ex - lastX < 1.0f) continue;	// one tick per pixel
					lastX = ex;
					Rect(ex, yy + 2, 0.6f, kRow - 4, tick);
				}
				if (esel) Frame(bx0, yy + 1, bx1 - bx0, kRow - 2, kAccent);
				if (s.clicked && Inside(s.clickX, s.clickY, bx0, yy, bx1 - bx0, kRow))
				{
					s.clicked = false;
					s.scene.selected = o.id;
					s.lastSel = o.id;
					s.levelSel = -1;
					s.evDragObj = o.id;
					s.evDragGrabUs = XToTime(s.clickX, lx, lw) - o.eventsStartUs;
					s.tab = TAB_SCENE;
				}
			}
			// position keys: diamonds along the bottom of the lane
			if (o.type == ObjType::Actor || o.type == ObjType::Camera || o.type == ObjType::Prop)
				for (auto& k : o.keys)
				{
					const float kx = TimeToX(k.tUs, lx, lw);
					if (kx < lx - 3 || kx > lx + lw + 3) continue;
					const bool ksel = s.keyObj == o.id && s.keyUs == k.tUs;
					const vec4_t kc = { 1.0f, 0.85f, 0.25f, ksel ? 1.0f : 0.8f };
					Rect(kx - 2, yy + kRow - 5, 4, 4, ksel ? kAccent : kc);
					if (s.clicked && Inside(s.clickX, s.clickY, kx - 3, yy + kRow - 6, 6, 6))
					{
						s.clicked = false;
						s.scene.selected = o.id;
						s.lastSel = o.id;
						s.levelSel = -1;
						s.scene.selectedClip = -1;
						s.scene.selectedTake = 0;
						s.keyObj = o.id;
						s.keyUs = k.tUs;
						s.keyDrag = true;
						s.tab = TAB_SCENE;
					}
				}
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
						s.scene.selectedTake = 0;
						s.tab = TAB_SCENE;
						s.clipDragObj = o.id;
						s.clipDragIndex = static_cast<int>(ci);
						s.clipDragGrabUs = XToTime(s.clickX, lx, lw) - c.startUs;
						s.keyObj = 0;
					}
				}
			}
			else if (o.type == ObjType::Actor && o.takes.empty()) Text(lx + 3, yy + 1.5f, "no clips: ANIMATIONS tab > ADD CLIP AT PLAYHEAD, or RECORD TAKE", kDim, 0.34f);
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
				"right mouse + WASD fly   F focus   P freeze   K key   CTRL+B split   N snap", kDim, 0.36f);
	}

	void DrawViewportOverlay()
	{
		Studio& s = S();
		// scene cameras
		const int64_t now = s.scene.clock.timeUs;
		for (auto& o : s.scene.objects)
		{
			if (o.type != ObjType::Camera) continue;
			const bool sel = o.id == s.scene.selected;
			// its path: dots between its first and last key, keys as squares
			if (s.showPaths && o.keys.size() >= 2 && (sel || o.id != s.lookId))
			{
				const int64_t k0 = o.keys.front().tUs, k1 = o.keys.back().tUs;
				const int64_t step = std::max<int64_t>(50'000, (k1 - k0) / 200);
				const vec4_t dot = { kCam[0], kCam[1], kCam[2], sel ? 0.8f : 0.35f };
				for (int64_t t = k0; t <= k1; t += step)
				{
					float p[3], a[3], f, sx, sy;
					if (!EvaluateCameraKeys(o.keys, t, p, a, f) || !Project(p, sx, sy)) continue;
					Rect(sx - 0.6f, sy - 0.6f, 1.2f, 1.2f, dot);
				}
				for (auto& k : o.keys)
				{
					float sx, sy;
					if (!Project(k.origin, sx, sy)) continue;
					const bool ks = s.keyObj == o.id && s.keyUs == k.tUs;
					Rect(sx - 1.5f, sy - 1.5f, 3, 3, ks ? kAccent : kCam);
				}
			}
			if (o.id == s.lookId) continue;
			float p[3], a[3], f, sx, sy;
			CamPose(o, now, p, a, f, true);
			if (!Project(p, sx, sy)) continue;
			Frame(sx - 4, sy - 3, 8, 6, sel ? kAccent : kCam);
			// which way it looks
			vec3_t fwd, tip;
			AngleVectors(a, fwd, nullptr, nullptr);
			VectorMA(p, 24, fwd, tip);
			float tx, ty;
			if (Project(tip, tx, ty))
				for (int i = 1; i <= 4; i++) Rect(sx + (tx - sx) * i / 4.0f - 0.5f, sy + (ty - sy) * i / 4.0f - 0.5f, 1, 1, sel ? kAccent : kCam);
			const bool live = s.scene.ActiveCamera(now) == o.id;
			Text(sx + 6, sy - 4, o.name + (live ? "  [LIVE]" : ""), sel ? kAccent : (live ? kRed : kCam), 0.36f);
		}
		// props: a small diamond and their name
		for (auto& o : s.scene.objects)
		{
			if (o.type != ObjType::Prop || o.entNum < 0) continue;
			float p[3], a[3], sx, sy;
			PropPose(o, now, p, a);
			if (!Project(p, sx, sy)) continue;
			const bool sel = o.id == s.scene.selected;
			const vec4_t pc = { 0.55f, 0.85f, 0.45f, sel ? 1.0f : 0.7f };
			Rect(sx - 1.5f, sy - 1.5f, 3, 3, sel ? kAccent : pc);
			if (sel || Inside(s.cx, s.cy, sx - 20, sy - 10, 40, 20)) Text(sx + 4, sy - 4, o.name, sel ? kAccent : pc, 0.34f);
			if (sel)
			{
				float mn[3], mx[3];
				if (PropBounds(o.entNum, mn, mx))
				{
					float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
					int nn = 0;
					for (int c = 0; c < 8; c++)
					{
						const vec3_t q = { (c & 1) ? mx[0] : mn[0], (c & 2) ? mx[1] : mn[1], (c & 4) ? mx[2] : mn[2] };
						float qx, qy;
						if (!Project(q, qx, qy)) continue;
						x0 = std::min(x0, qx); y0 = std::min(y0, qy); x1 = std::max(x1, qx); y1 = std::max(y1, qy);
						nn++;
					}
					if (nn) Frame(x0, y0, x1 - x0, y1 - y0, kAccent);
				}
			}
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
			const std::string mode = s.director && !s.pilotId ? "DIRECTOR: " + (look ? look->name : std::string("no cut yet")) :
				look ? (s.pilotId ? (s.camRec && s.scene.clock.playing ? "REC MOVE " : "PILOTING ") : "VIEWING ") + look->name : "FREE CAMERA";
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
			float sx, sy, pp[3], pa[3], pf;
			CamPose(o, s.scene.clock.timeUs, pp, pa, pf, true);
			if (!Project(pp, sx, sy)) continue;
			const float d = hypotf(sx - s.clickX, sy - s.clickY);
			if (d < best) { best = d; bestId = o.id; bestEnt = -1; }
		}
		for (auto& o : s.scene.objects)
		{
			if (o.type != ObjType::Prop || o.entNum < 0) continue;
			float pp[3], pa[3], sx, sy;
			PropPose(o, s.scene.clock.timeUs, pp, pa);
			if (!Project(pp, sx, sy)) continue;
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

	// ------------------------------------------------------------------------------------------
	// Phase 3: Ghost Performance Recorder
	// ------------------------------------------------------------------------------------------

	// ---- Phase 6: recording a camera move ----
	void CommitCameraRecording()
	{
		Studio& s = S();
		if (s.camRecKeys.size() >= 2 && s.scene.Find(s.camRecId))
		{
			s.scene.ReplaceKeys(s.camRecId, s.camRecKeys.front().tUs, s.camRecKeys.back().tUs, s.camRecKeys);
			s.host.Log(0, Fmt("Camera move recorded: %d keys, %.1f s", static_cast<int>(s.camRecKeys.size()),
				static_cast<double>(s.camRecKeys.back().tUs - s.camRecKeys.front().tUs) / 1e6));
		}
		s.camRecKeys.clear();
		s.camRecId = 0;
		s.camRecLastUs = -1;
	}

	// while REC MOVE is on, flying a camera with the scene playing keys it 10 times a second
	void CameraRecordFrame()
	{
		Studio& s = S();
		const bool recording = s.open && s.camRec && s.pilotId && s.scene.clock.playing;
		if (!recording) { if (!s.camRecKeys.empty()) CommitCameraRecording(); return; }
		if (s.camRecId != s.pilotId) { CommitCameraRecording(); s.camRecId = s.pilotId; }
		const int64_t t = s.scene.clock.timeUs;
		if (s.camRecLastUs >= 0 && t < s.camRecLastUs) { CommitCameraRecording(); s.camRecId = s.pilotId; }	// looped
		if (s.camRecLastUs >= 0 && t - s.camRecLastUs < 100'000) return;
		s.camRecLastUs = t;
		TransformKey k;
		k.tUs = t;
		VectorCopy(s.camOrg, k.origin);
		k.pitch = s.camAng[PITCH];
		k.yaw = s.camAng[YAW];
		k.roll = s.camAng[ROLL];
		k.fov = s.camFov;
		k.interp = Interp::CatmullRom;
		s.camRecKeys.push_back(k);
	}

	void Open();

	// Record the player into a take for `target` (0 = a new actor), starting at the playhead, and
	// (recNpcs) every NPC fighting you, each into a take of its own.
	void StartRecording(const int target)
	{
		Studio& s = S();
		if (s.rec != Studio::REC_IDLE) return;
		const Object* o = target ? s.scene.Find(target) : nullptr;
		if (target && (!o || o->type != ObjType::Actor)) { s.host.Log(1, "Select an actor to record a take for"); return; }
		Clock& c = s.scene.clock;
		if (c.timeUs >= c.lengthUs) c.timeUs = 0;
		ReleasePreview();
		s.recTarget = target;
		s.recPunchUs = c.timeUs;
		s.recLastUs = -1;
		s.recOldSpeed = c.speed;
		s.recOldLoop = c.loop;
		c.Pause();
		s.recPerf.clear();
		Studio::Performer me;
		me.ent = 0;
		me.objId = target;
		s.recPerf.push_back(me);
		if (o && o->entNum >= 0) { SetHidden(o->entNum, true); Unhold(o->entNum); s.hiddenEnts.push_back(o->entNum); }

		// NPCs that fight: anything with AI on that isn't played back by a take
		EntInfo pi;
		EntityInfo(0, pi);
		int ents[64];
		const int n = s.recNpcs ? ListCharacters(ents, 64) : 0;
		for (int i = 0; i < n; i++)
		{
			const int e = ents[i];
			EntInfo info;
			if (e == 0 || !EntityInfo(e, info) || !CharacterAlive(e) || !info.ai) continue;
			const Object* so = s.scene.FindByEntity(e);
			if (so && (so->prop || so->id == target)) continue;
			if (!so && s.recNpcs < 2) continue;
			const float dx = info.origin[0] - pi.origin[0], dy = info.origin[1] - pi.origin[1], dz = info.origin[2] - pi.origin[2];
			if (!so && dx * dx + dy * dy + dz * dz > 2000.0f * 2000.0f) continue;	// level NPCs only nearby
			Studio::Performer p;
			p.ent = e;
			p.objId = so ? so->id : 0;
			p.npcType = info.npcType;
			s.recPerf.push_back(p);
		}
		// earlier takes can be hit while you fight them
		if (s.recSolidTakes)
			for (auto& so : s.scene.objects)
				if (so.type == ObjType::Actor && so.entNum >= 0 && so.prop && Scene::ActiveTake(so) && so.id != target)
					SetSolid(so.entNum, true), s.solidEnts.push_back(so.entNum);

		Close();			// the game gets the keyboard and mouse back; the scene keeps playing
		s.rec = Studio::REC_COUNTDOWN;
		s.recCountdownEnd = NowMs() + s.recCountdownSec * 1000;
		const int npcs = static_cast<int>(s.recPerf.size()) - 1;
		s.host.Log(0, "Recording " + (o ? "a take for " + o->name : std::string("you")) +
			(npcs ? " and " + std::to_string(npcs) + " NPC(s)" : std::string()) + " from " + c.Timecode() +
			" in " + std::to_string(s.recCountdownSec) + " s. F10 stops.");
	}

	void SampleNow()
	{
		Studio& s = S();
		const int64_t t = s.scene.clock.timeUs - s.recPunchUs;
		if (t < 0) return;
		for (auto& p : s.recPerf)
		{
			if (p.gone) continue;
			TakeSample x;
			if (!SampleEntity(p.ent, p.take, x)) { p.gone = !p.take.Samples().empty(); continue; }	// removed from the level
			if (!p.take.Samples().empty() && t <= p.take.Samples().back().tUs) continue;
			x.tUs = p.take.Samples().empty() ? 0 : t;		// a take starts at 0
			p.take.Add(x);
		}
		s.recLastUs = t;
	}

	void EndRecordingState()
	{
		Studio& s = S();
		Clock& c = s.scene.clock;
		s.rec = Studio::REC_IDLE;
		c.Pause();
		c.speed = s.recOldSpeed;
		c.loop = s.recOldLoop;
		for (const int e : s.hiddenEnts) SetHidden(e, false);
		for (const int e : s.solidEnts) SetSolid(e, false);
		s.hiddenEnts.clear();
		s.solidEnts.clear();
		ClearGhosts();
	}

	// put one performer's take on its actor; returns the actor id
	int KeepPerformance(Studio::Performer& p, const int64_t startUs)
	{
		Studio& s = S();
		Take& tk = p.take;
		tk.startUs = startUs;
		const TakeSample& first = tk.Samples()[0];
		const float ang[3] = { 0, first.yaw, 0 };
		int id = p.objId && s.scene.Find(p.objId) ? p.objId : 0;
		if (p.ent == 0)
		{
			// you: the chosen actor, or a new one of your NPC type where you started
			if (!id)
			{
				const std::string type = tk.performer.empty() || tk.performer == "player" ? s.npcType : tk.performer;
				id = s.scene.AddActor(type, first.origin, ang);
				if (!id) { s.host.Log(2, "Couldn't spawn '" + type + "' for your take (set the NPC type in ACTORS)"); return 0; }
			}
		}
		else if (id)
		{
			// a scene actor: if it died in the fight, a fresh one plays the take (death and all)
			Object* o = s.scene.Find(id);
			if (o->entNum < 0 || !CharacterAlive(o->entNum)) s.scene.Respawn(id);
		}
		else if (CharacterAlive(p.ent))
			id = s.scene.AdoptActor(p.ent, p.npcType, "");		// a level NPC joins the scene
		else
		{
			id = s.scene.AddActor(p.npcType, first.origin, ang);	// it died: a new one plays it
			if (!id) { s.host.Log(1, "Couldn't spawn '" + p.npcType + "' for its take"); return 0; }
		}
		return s.scene.AddTake(id, tk, true) ? id : 0;
	}

	void StopRecording(const bool keep)
	{
		Studio& s = S();
		if (s.rec == Studio::REC_IDLE) return;
		Clock& c = s.scene.clock;
		const bool wasRecording = s.rec == Studio::REC_RECORDING;
		if (wasRecording) SampleNow();
		EndRecordingState();
		Open();
		std::vector<Studio::Performer> perf;
		perf.swap(s.recPerf);
		c.timeUs = s.recPunchUs;
		if (!keep || !wasRecording || perf.empty() || perf[0].take.Samples().size() < 2)
		{
			s.host.Log(1, keep ? "Nothing recorded (stopped before the countdown ended)" : "Recording discarded");
			SetEventCapture(s.irOn);
			return;
		}
		int64_t end = 0;
		int kept = 0, mine = 0;
		std::string names;
		std::map<int, int> entToObj;
		for (auto& p : perf)
		{
			if (p.take.Samples().size() < 2) continue;
			const int id = KeepPerformance(p, s.recPunchUs);
			if (p.ent >= 0) entToObj[p.ent] = id;
			if (!id) continue;
			if (p.ent == 0) mine = id;
			kept++;
			end = std::max(end, s.recPunchUs + p.take.LengthUs());
			if (const Object* o = s.scene.Find(id)) names += (names.empty() ? "" : ", ") + o->name;
		}
		// what happened in the fight (effects, sounds, hits, voices) on a world-events track
		{
			std::vector<WorldEvent> ev;
			for (auto& ce : TakeCapturedEvents())
			{
				if (ce.absUs < s.recCgStartUs) continue;
				WorldEvent w = ce.ev;
				w.tUs = ce.absUs - s.recCgStartUs;
				w.actor = entToObj.count(ce.actorEnt) ? entToObj[ce.actorEnt] : 0;
				w.other2 = entToObj.count(ce.other2Ent) ? entToObj[ce.other2Ent] : 0;
				if (!w.temp && !w.actor) continue;	// on a character that wasn't recorded
				ev.push_back(w);
			}
			if (!ev.empty()) s.scene.AddEvents("Fight events", s.recPunchUs, ev);
			SetEventCapture(s.irOn);
		}
		if (end > c.lengthUs) c.lengthUs = (end + 999'999) / 1'000'000 * 1'000'000;
		if (mine)
		{
			Object* o = s.scene.Find(mine);
			s.scene.selected = mine;
			s.lastSel = mine;
			s.scene.selectedClip = -1;
			s.scene.selectedTake = o ? o->activeTake : 0;
		}
		s.tab = TAB_SCENE;
		s.host.Log(0, Fmt("%d take(s) recorded, %.1f s: %s", kept, static_cast<double>(perf[0].take.LengthUs()) / 1e6, names.c_str()));
	}

	// runs every frame from CS_Frame
	void RecorderFrame()
	{
		Studio& s = S();
		Clock& c = s.scene.clock;
		if (s.rec == Studio::REC_COUNTDOWN && NowMs() >= s.recCountdownEnd)
		{
			s.rec = Studio::REC_RECORDING;
			SetEventCapture(false);	// fresh
			SetEventCapture(true);
			s.recCgStartUs = CaptureNowUs();
			c.speed = 1.0;		// performances are recorded in real time
			c.loop = false;
			c.Play();
			SampleNow();
		}
		if (s.rec != Studio::REC_RECORDING) return;
		// recording past the end of the scene makes the scene longer
		if (c.timeUs > c.lengthUs - 500'000) c.lengthUs += 5'000'000;
		if (!c.playing) c.Play();
		const int64_t t = c.timeUs - s.recPunchUs;
		if (s.recLastUs < 0 || t - s.recLastUs >= 1'000'000 / 30) SampleNow();	// 30 samples a second (playback blends between)
	}

	// ---- Phase 5: instant replay ----
	// While you play (studio closed), the last irSeconds of every character near you and every event
	// are kept; "studio replay" (F9) turns them into takes and a world-events track at the playhead.

	void InstantReplayFrame()
	{
		Studio& s = S();
		if (!s.irOn || s.open || s.rec != Studio::REC_IDLE) return;
		const int64_t now = CaptureNowUs();
		if (s.irLastUs >= 0 && now - s.irLastUs < 1'000'000 / 30 && now >= s.irLastUs) return;
		s.irLastUs = now;
		if (!EventCaptureOn()) SetEventCapture(true);
		EntInfo pi;
		if (!EntityInfo(0, pi)) return;
		for (auto& t : s.irTracks) t.open = false;
		int ents[64];
		const int n = ListCharacters(ents, 64);
		for (int i = 0; i < n; i++)
		{
			const int e = ents[i];
			EntInfo info;
			if (!EntityInfo(e, info)) continue;
			const Object* so = s.scene.FindByEntity(e);
			if (so && so->prop) continue;	// the studio's own playback
			const float dx = info.origin[0] - pi.origin[0], dy = info.origin[1] - pi.origin[1];
			if (e != 0 && dx * dx + dy * dy > 3000.0f * 3000.0f) continue;
			Studio::IrTrack* tr = nullptr;
			for (auto& t : s.irTracks) if (t.ent == e && t.npcType == info.npcType && !t.take.Samples().empty() &&
				now - (t.firstAbsUs + t.take.Samples().back().tUs) < 500'000) tr = &t;
			if (!tr)
			{
				s.irTracks.emplace_back();
				tr = &s.irTracks.back();
				tr->ent = e;
				tr->npcType = info.npcType;
				tr->firstAbsUs = now;
			}
			TakeSample x;
			if (!SampleEntity(e, tr->take, x)) continue;
			x.tUs = now - tr->firstAbsUs;
			if (!tr->take.Samples().empty() && x.tUs <= tr->take.Samples().back().tUs) continue;
			tr->take.Add(x);
			tr->open = true;
		}
		// forget what's older than the window (every 2 s)
		if (now - s.irLastPruneUs > 2'000'000 || now < s.irLastPruneUs)
		{
			s.irLastPruneUs = now;
			const int64_t cut = now - static_cast<int64_t>(s.irSeconds) * 1'000'000;
			PruneCapturedEvents(cut);
			for (auto& t : s.irTracks)
			{
				auto& sm = t.take.Edit().samples;
				size_t k = 0;
				while (k < sm.size() && t.firstAbsUs + sm[k].tUs < cut) k++;
				if (!k) continue;
				sm.erase(sm.begin(), sm.begin() + static_cast<long>(k));
				if (sm.empty()) continue;
				const int64_t shift = sm.front().tUs;
				for (auto& q : sm) q.tUs -= shift;
				t.firstAbsUs += shift;
			}
			s.irTracks.erase(std::remove_if(s.irTracks.begin(), s.irTracks.end(), [](const Studio::IrTrack& t) { return t.take.Samples().size() < 2 && !t.open; }), s.irTracks.end());
		}
	}

	void SetInstantReplay(const bool on)
	{
		Studio& s = S();
		s.irOn = on;
		s.irTracks.clear();
		s.irLastUs = -1;
		if (s.rec == Studio::REC_IDLE) SetEventCapture(on);
		s.host.Log(0, on ? Fmt("Instant replay on: F9 keeps the last %d s", s.irSeconds) : "Instant replay off");
	}

	void SaveInstantReplay()
	{
		Studio& s = S();
		if (!s.irOn) { s.host.Log(1, "Instant replay is off (ACTORS tab: INSTANT REPLAY, or 'studio replay on')"); return; }
		if (s.rec != Studio::REC_IDLE) return;
		const int64_t now = CaptureNowUs();
		const int64_t w0 = now - static_cast<int64_t>(s.irSeconds) * 1'000'000;
		const int64_t punch = s.scene.clock.timeUs >= s.scene.clock.lengthUs ? 0 : s.scene.clock.timeUs;
		int64_t first = now;
		for (auto& t : s.irTracks)
			for (auto& q : t.take.Samples()) if (t.firstAbsUs + q.tUs >= w0) { first = std::min(first, t.firstAbsUs + q.tUs); break; }
		if (first >= now) { s.host.Log(1, "Nothing to replay yet"); return; }
		struct Kept { int ent; int64_t a0, a1; int id; };
		std::vector<Kept> kept;
		int mine = 0, count = 0;
		ReleasePreview();
		for (auto& t : s.irTracks)
		{
			Studio::Performer p;
			p.npcType = t.npcType;
			// a track that ended (died, removed) or an entity used again: a new actor plays it
			p.ent = t.open ? t.ent : (t.ent == 0 ? 0 : -1);
			if (p.ent > 0) { const Object* so = s.scene.FindByEntity(p.ent); p.objId = so ? so->id : 0; }
			for (auto& a : t.take.Anims()) p.take.AnimIndex(a);
			int64_t a0 = -1, a1 = 0;
			for (auto q : t.take.Samples())
			{
				const int64_t abs = t.firstAbsUs + q.tUs;
				if (abs < w0) continue;
				if (a0 < 0) a0 = abs;
				q.tUs = abs - a0;
				p.take.Add(q);
				a1 = abs;
			}
			if (p.take.Samples().size() < 2) continue;
			p.take.profile = t.take.profile;
			p.take.performer = t.take.performer;
			p.take.name = "Replay";
			const int id = KeepPerformance(p, punch + (a0 - first));
			if (!id) continue;
			if (t.ent == 0) mine = id;
			kept.push_back({ t.ent, a0, a1, id });
			count++;
		}
		std::vector<WorldEvent> ev;
		for (auto& ce : TakeCapturedEvents())
		{
			if (ce.absUs < first) continue;
			WorldEvent w = ce.ev;
			w.tUs = ce.absUs - first;
			const auto who = [&](const int ent) { for (auto& k : kept) if (k.ent == ent && ce.absUs >= k.a0 - 100'000 && ce.absUs <= k.a1 + 100'000) return k.id; return 0; };
			w.actor = ce.actorEnt >= 0 ? who(ce.actorEnt) : 0;
			w.other2 = ce.other2Ent >= 0 ? who(ce.other2Ent) : 0;
			if (!w.temp && !w.actor) continue;
			ev.push_back(w);
		}
		if (!ev.empty()) s.scene.AddEvents("Replay events", punch, ev);
		s.irTracks.clear();
		s.irLastUs = -1;
		Open();
		Clock& c = s.scene.clock;
		c.Pause();
		const int64_t end = punch + (now - first);
		if (end > c.lengthUs) c.lengthUs = (end + 999'999) / 1'000'000 * 1'000'000;
		c.timeUs = punch;
		if (mine) { s.scene.selected = mine; s.lastSel = mine; }
		s.tab = TAB_SCENE;
		s.host.Log(0, Fmt("Instant replay: %d character(s), %d event(s), %.1f s", count, static_cast<int>(ev.size()), static_cast<double>(now - first) / 1e6));
	}

	// world events between two scene times (from, to] play again
	void PlaySceneEvents(const int64_t from, const int64_t to)
	{
		Studio& s = S();
		for (auto& o : s.scene.objects)
		{
			if (o.type != ObjType::Events || !o.visible) continue;
			const auto& ev = o.Events();
			auto it = std::upper_bound(ev.begin(), ev.end(), from - o.eventsStartUs, [](const int64_t v, const WorldEvent& e) { return v < e.tUs; });
			for (; it != ev.end() && o.eventsStartUs + it->tUs <= to; ++it)
			{
				const Object* a = it->actor ? s.scene.Find(it->actor) : nullptr;
				const Object* b = it->other2 ? s.scene.Find(it->other2) : nullptr;
				if (!it->temp && (!a || a->entNum < 0)) continue;
				FireEvent(*it, a ? a->entNum : -1, b ? b->entNum : -1);
			}
		}
	}

	void DrawRecordingOverlay()
	{
		Studio& s = S();
		if (s.irOn && s.rec == Studio::REC_IDLE)
		{
			Text(4, 470, Fmt("instant replay: F9 keeps the last %d s", s.irSeconds), kDim, 0.32f);
			return;
		}
		if (s.rec == Studio::REC_IDLE) return;
		if (s.rec == Studio::REC_COUNTDOWN)
		{
			const int left = std::max(0, (s.recCountdownEnd - NowMs() + 999) / 1000);
			Text(310, 190, Fmt("%d", left), kRed, 2.0f);
			Text(232, 240, "get ready - recording starts", kTextCol, 0.5f);
		}
		else
		{
			if ((NowMs() / 500) % 2 == 0) Rect(8, 8, 8, 8, kRed);
			Text(20, 6, "REC  " + s.scene.clock.Timecode(), kRed, 0.55f);
			Text(20, 18, "F10: stop and keep    console 'studio rec cancel': discard", kDim, 0.38f);
		}
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
		s.takeDragId = 0;
		s.keyDrag = false;
		s.evDragObj = 0;
		s.cutDrag = false;
		s.camRec = false;
		CameraRecordFrame();	// keeps what was recorded
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
	s.rec = Studio::REC_IDLE;	// a level change ends a recording (it's lost)
	s.hiddenEnts.clear();
	s.ghostEnts.clear();
	s.solidEnts.clear();
	s.recPerf.clear();
	s.faceApplied.clear();
	s.idleApplied.clear();
	s.eyeApplied.clear();
	s.facePosesEnt = -1;
	s.liveAnims.clear();
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
	if (g_firstRun.integer < 2)
	{
		cgi_SendConsoleCommand("bind F9 \"studio replay\"\n");	// Phase 5: keep the last seconds of play
		cgi_Cvar_Set("cs_firstrun", "2");
	}
	s.irTracks.clear();
	s.lastEvalUs = -1;
	SetEventCapture(s.irOn);
}

void CS_Shutdown()
{
	if (g_s && g_s->open)
	{
		g_s->open = false;
		cgi_Cvar_Set("cs_active", "0");
	}
}

void CS_DrawOverlay()
{
	if (g_s && !g_s->open) DrawRecordingOverlay();
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
	if (!sub[0] || !Q_stricmp(sub, "toggle"))
	{
		if (s.rec != Studio::REC_IDLE) StopRecording(true);	// F10 while recording: stop and keep
		else s.open ? Close() : Open();
		return;
	}
	if (!Q_stricmp(sub, "rec"))
	{
		if (!Q_stricmp(a2.c_str(), "stop")) StopRecording(true);
		else if (!Q_stricmp(a2.c_str(), "cancel")) StopRecording(false);
		else if (s.rec != Studio::REC_IDLE) StopRecording(true);
		else if (!Q_stricmp(a2.c_str(), "new")) StartRecording(0);
		else
		{
			const Object* o = s.scene.Find(s.scene.selected);
			StartRecording(o && o->type == ObjType::Actor ? o->id : 0);
		}
		return;
	}
	if (!Q_stricmp(sub, "replay"))
	{
		if (!Q_stricmp(a2.c_str(), "on")) SetInstantReplay(true);
		else if (!Q_stricmp(a2.c_str(), "off")) SetInstantReplay(false);
		else if (!a2.empty() && atoi(a2.c_str()) > 0) { s.irSeconds = std::max(5, std::min(120, atoi(a2.c_str()))); s.host.Log(0, Fmt("Instant replay keeps %d s", s.irSeconds)); }
		else SaveInstantReplay();
		return;
	}
	if (!Q_stricmp(sub, "countdown")) { s.recCountdownSec = std::max(0, std::min(10, atoi(a2.c_str()))); return; }
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
		"       spawn <npc>  camera  play  pause  stop  fps <24|30|60>  undo  redo  freeze  list\n"
		"       rec [new|stop|cancel]  countdown <seconds>  replay [on|off|<seconds>]  (replay: keep the last seconds)\n");
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
		if (s.cutCam && s.cutCam == s.scene.selected) { s.scene.RemoveCut(s.cutCam, s.cutUs); s.cutCam = 0; }
		else if (s.scene.selectedClip >= 0) s.scene.RemoveClip(s.scene.selected, s.scene.selectedClip);
		else s.scene.Remove(s.scene.selected);
	}
	else if (is(ACT_DUPLICATE)) s.scene.Duplicate(s.scene.selected);
	else if (is(ACT_SPLIT)) SplitAtPlayhead();
	else if (is(ACT_KEY)) SetKeyAtPlayhead();
	else if (is(ACT_DIRECTOR)) { s.director = !s.director; if (s.director) { s.pilotId = 0; s.camActive = true; } s.host.Log(0, s.director ? "Director view: the cut track picks the camera" : "Director view off"); }
	else if (is(ACT_SNAP)) { s.snap = !s.snap; s.host.Log(0, s.snap ? "Snapping on" : "Snapping off"); }
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
	}
	RecorderFrame();
	InstantReplayFrame();
	if (s.open || s.rec != Studio::REC_IDLE)
	{
		EvaluateScene();
		EvaluateProps();
		EvaluateFaces();
		// saber clashes with takes while the scene plays (recording: your attacks bounce off them)
		if (s.clashes && s.scene.clock.playing && !s.takeEnts.empty()) DetectClashes(s.takeEnts, s.rec == Studio::REC_RECORDING);
		SetCaptureSkip(s.takeEnts);
		// world events play while the scene plays forward (not when scrubbing or jumping)
		const int64_t t = s.scene.clock.timeUs;
		if (s.scene.clock.playing && !s.scrubbing && s.lastEvalUs >= 0 && t > s.lastEvalUs && t - s.lastEvalUs < 400'000)
		{
			PlaySceneEvents(s.lastEvalUs, t);
			PlaySceneSounds(s.lastEvalUs, t);
		}
		if (!s.scene.clock.playing && (s.soundsStarted || s.musicStarted) && s.rec == Studio::REC_IDLE) StopStudioSounds();
		s.lastEvalUs = t;
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
		if (s.lookId && !s.pilotId) { s.lookId = 0; s.director = false; }	// flying away from a camera you were only viewing
		if (s.pilotId)
		{
			Object* o = s.scene.Find(s.pilotId);
			if (o && o->keys.empty() && !s.camRec)	// a keyed camera keeps its path: K keys where you flew
			{
				const bool moved = !VectorCompare(o->origin, s.camOrg) || !VectorCompare(o->angles, s.camAng) || o->fov != s.camFov;
				if (moved) { s.scene.SetTransform(o->id, s.camOrg, s.camAng, true); o->fov = s.camFov; }
			}
		}
	}

	CameraRecordFrame();

	if (now - s.lastAutosaveMs > 120000)
	{
		s.lastAutosaveMs = now;
		if (s.scene.Autosave()) s.host.Log(0, "Autosaved " + s.scene.project);
	}
}

bool CS_ApplyView()
{
	if (g_s && g_s->open && g_s->director && !g_s->camActive && g_s->scene.ActiveCamera(g_s->scene.clock.timeUs)) g_s->camActive = true;
	if (!g_s || !g_s->open || !g_s->camActive) return false;
	Studio& s = S();
	const int64_t t = s.scene.clock.timeUs;
	if (s.director && !s.pilotId)
	{
		// the cut track picks the camera
		const Object* o = s.scene.Find(s.scene.ActiveCamera(t));
		if (o) { CamPose(*o, t, s.camOrg, s.camAng, s.camFov, true); s.lookId = o->id; }
	}
	else if (s.lookId && !s.pilotId)
	{
		const Object* o = s.scene.Find(s.lookId);
		if (!o) { PlayerView(); return false; }
		CamPose(*o, t, s.camOrg, s.camAng, s.camFov, true);
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
	if (s.scene.selected != s.lastSel) { s.scene.selectedClip = -1; s.scene.selectedTake = 0; s.keyObj = 0; s.lastSel = s.scene.selected; }
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
