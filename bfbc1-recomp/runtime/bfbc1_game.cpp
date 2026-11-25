// Battlefield: Bad Company: the game-specific half of the shared runtime
// (bf1943-recomp/runtime/game_patches.cpp and bots.cpp there). No guest patches
// yet; the settings keep their defaults and bots are not supported.
#include "kernel.h"
#include "admin_server.h"
#include "lan.h"

#ifdef _WIN32
#include <windows.h>
#include <Xinput.h>
#else
#include "posix_input.h"
#endif
#include "bc_compat.h"
#include "bc_mp_menu.h"
#include "pc_settings.h"
#include "pc_menu.h"
#include "xenia_bridge.h"

#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <set>
#include <string>
#include <vector>

#if defined(_WIN32) || defined(BF1943_CLIENT)  // (a window: not the Linux server's)
extern "C" const char* g_window_title;  // xenia_shims/bridge.cc
static const bool g_title_set = (g_window_title = "Battlefield: Bad Company", true);
#endif

namespace rt {
uint32_t g_max_fps = 120;    // --max-fps (0 = no limit), applied in VdSwap
uint32_t g_tick_rate = 75;   // --tickrate: the simulation rate (30, 60, 75, 100...)
float g_wind_cli = -1.0f;    // --wind (shared runtime; Bad Company's plants)
bool g_lan_host = false;     // --host
// Bad Company's ports are BF1943's + 500 (1500 VDP, 1503 server), so both games
// can host on one PC; BF1943's bot copies use offsets up to 150.
static const bool g_bc_ports = (g_base_port_offset = g_port_offset = 500, true);
bool g_vsync = false;        // --vsync
std::string g_host_map;      // --host-map
bool g_log_ui = false;       // --log-ui
std::string g_ui_call;       // --ui-call
std::string g_log_settings;  // --log-settings
uint32_t g_shadow_size = 0;  // the game's own
float g_shadow_distance = 0;
std::string g_worldrender;
std::string g_overgrowth;
float g_lod = 1.0f;
bool g_auto_start = false;  // set by main.cpp for dedicated/bot copies (bf1943)
std::string g_aspect_override, g_wide_hud_override;  // --aspect / --wide-hud (kernel.h)
std::mutex g_host_map_mutex;  // (taken to change g_host_map in a level's run: the replay and the bots read it)
std::atomic<int> g_server_generation{0};

std::atomic<bool> g_host_request{false};  // HostFromMainLoop starts hosting g_host_map
void RequestHost() { g_host_request = true; }

std::string StartBotDirector(int) { return {}; }
void SetBotSlot(const std::string&, int) {}
bool BotLookAxis(int, float&) { return false; }
// Sticks held by the test automation (BF_AUTO_MP "walk", "turn" and "look" steps).
static std::atomic<int16_t> g_auto_ly{0}, g_auto_rx{0}, g_auto_ry{0}, g_auto_lx{0};
static std::atomic<uint8_t> g_auto_rt{0};
// The host's autopilot (bc_bots.cpp, BF_BOT_HOST_PILOT): sticks and buttons.
std::atomic<int16_t> g_pilot_lx{0}, g_pilot_ly{0}, g_pilot_rx{0}, g_pilot_ry{0};
std::atomic<uint16_t> g_pilot_buttons{0};
std::atomic<uint8_t> g_pilot_rt{0}, g_pilot_lt{0};
// PC SETTINGS was chosen on the main menu (the menu callback hook below): the
// page to open (on the input thread, where it runs), and the DEV page the game
// went to, to leave once it is closed.
std::atomic<bool> g_open_pc_settings{false}, g_back_out_of_dev{false};
// The pad, every poll: our Multiplayer menu (bc_mp_menu.cpp) takes it while open.
bool AddBotInput(XINPUT_GAMEPAD& pad) {
  // SKIP INTRO: the startup movies (the EA and Dolby logos, the intro) end on
  // a press, so until the title screen A is pressed for 0.1 s every 0.4 s.
  // (Refusing their files instead stops the startup.)
  if (g_skip_intro && !g_title_passed.load(std::memory_order_relaxed)) {
    static const auto start = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    if (ms > 1500 && ms % 400 < 100) pad.wButtons |= XINPUT_GAMEPAD_A;
  }
  if (int16_t v = g_auto_ly.load()) pad.sThumbLY = v;
  if (int16_t v = g_auto_rx.load()) pad.sThumbRX = v;
  if (int16_t v = g_auto_ry.load()) pad.sThumbRY = v;
  if (int16_t v = g_auto_lx.load()) pad.sThumbLX = v;
  if (uint8_t v = g_auto_rt.load()) pad.bRightTrigger = v;
  if (int16_t v = g_pilot_lx.load()) pad.sThumbLX = v;
  if (int16_t v = g_pilot_ly.load()) pad.sThumbLY = v;
  if (int16_t v = g_pilot_rx.load()) pad.sThumbRX = v;
  if (int16_t v = g_pilot_ry.load()) pad.sThumbRY = v;
  pad.wButtons |= g_pilot_buttons.load();
  if (uint8_t v = g_pilot_rt.load()) pad.bRightTrigger = v;
  if (uint8_t v = g_pilot_lt.load()) pad.bLeftTrigger = v;
  if (bcmp::Update(pad)) pad = {};
  if (g_open_pc_settings.exchange(false) && !pcset::IsOpen()) pcset::Open();
  // The PC settings page, opened by PC SETTINGS, is closed and has let go of
  // the pad: B for 0.15 s takes the game from the DEV page (where its A went)
  // back to the main menu.
  {
    static uint64_t back_until = 0;
    uint64_t now = GetTickCount64();
    if (g_back_out_of_dev && !pcset::IsOpen() && !pcset::ControlsPad()) {
      g_back_out_of_dev = false, back_until = now + 150;
      RT_INFO("menu: PC settings closed, B to the main menu");
    }
    if (now < back_until) pad.wButtons |= XINPUT_GAMEPAD_B;
  }
  return false;
}
bool CustomMenus() { return false; }
// PC settings rows that do nothing here (pc_settings.cpp hides them): PLANT
// WIND sways BF1943's plant vertex shaders (a vec4 mad with c213.w, see
// spirv_shader_translator.h IsWindConstant); none of Bad Company's has it.
// On foot the mouse's pitch goes through the right stick's up/down read, as the
// controller's does (xam.cpp MouseLookAxis): the aim setter hook (bc_bots.cpp)
// then turns by the mouse only. Looking up and down did not move with the
// setter's pitch in gameplay (it works in the cutscenes, where this path is used).
static const bool g_bc_mouse_pitch = (g_mouse_pitch_by_stick = true, true);
// The scene effects cannot draw Bad Company's world sun shadows (the ray
// tracer's mesh capture uses BF1943's shader patterns): the game's own shadow
// map draws them, so the effects keep it (pc_settings.cpp: own_shadows=0).
static const bool g_bc_own_shadows = (g_scene_fx_own_shadows = false, true);
static const bool g_bc_settings_hidden = (g_setting_hidden = [](const char* key) {
  return !std::strcmp(key, "wind");
}, true);
// Key binding names in Bad Company: LB (the GRENADE key) takes out the kit's
// "G" group, the grenade and the gadgets (auto injector, motion mine...), as
// the kit screen shows; RB (SWITCH WEAPON) the "1" group, the main weapon and
// its launcher. (Seen in a running game with the pad presses: Y is the knife.)
static const bool g_bc_action_names = (g_action_name = [](int action) -> const wchar_t* {
  return action == pcset::kGrenade ? L"GRENADE / GADGET" : action == pcset::kNextWeapon ? L"SWITCH WEAPON / LAUNCHER"
                                                                                         : nullptr;
}, true);
// The PC settings page in Bad Company's menu look (pc_menu.cpp).
#if defined(_WIN32) || defined(BF1943_CLIENT)  // (the server has no PC menus)
static const bool g_bc_menu_look = (pcmenu::SetBadCompanyLook(), true);
#endif
// --blood (gpu.cpp): the server soldier (ServerSoldierEntity, its feet at
// +0x200) and the client soldier (feet at +0x370); their health parts found
// with BF_SOLDIER_DUMP (bc_bots.cpp). The blows' impulses are not known here.
const SoldierLayout g_blood_server = {0x820118D0u, 0x820118A4u, 0x200, 0, 0};
const SoldierLayout g_blood_client = {0x82008D48u, 0x82008D20u, 0x370, 0, 0};
// PC wording. The OPTIONS screen's storage device prompt (Xbox only) opens
// the PC settings page instead (changeStorageDevice, the menu callback hook below).
const char* const g_game_pc_text[] = {
    "CHANGE STORAGE DEVICE", "PC SETTINGS",
    "STORAGE DEVICE", "PC SETTINGS",
    "When you see this icon, the game is autosaving. Do not turn off the Xbox 360 console or remove the Xbox 360 "
    "Memory Unit while the game is autosaving.",
    "When you see this icon, the game is autosaving. Do not close the game or turn off your computer while the "
    "game is autosaving.",
    "Deleting player profile data. Please do not turn off your Xbox 360 console.",
    "Deleting player profile data. Please do not close the game.",
    "Loading player profile data. Please do not turn off your Xbox 360 console.",
    "Loading player profile data. Please do not close the game.",
    "Saving player profile data. Please do not turn off your Xbox 360 console.",
    "Saving player profile data. Please do not close the game.",
    "Please reconnect the Xbox 360 Controller", "Please reconnect the controller",
    "READING XBOX 360 MEMORY CARD UNIT", "READING SAVE DATA",
    "You are not signed in with an active gamer profile. You will not be able to save or load files to your Xbox "
    "360 console without an active gamer profile.",
    "You are not signed in with an active profile. You will not be able to save or load files without an active "
    "profile.",
    "You did not choose a gamer profile. Singleplayer progress saving and playing on Xbox LIVE will not be "
    "available.",
    "You did not choose a profile. Singleplayer progress saving and playing online will not be available.",
    "Your Gamer Profile does not have the correct permissions to access Xbox LIVE functionality.",
    "Your profile does not have the correct permissions to access online functionality.",
    "Connection to Xbox LIVE has been lost", "Connection to the server was lost",
    "Failed to connect to Xbox LIVE", "Failed to connect to server",
    "You were disconnected from Xbox LIVE", "Disconnected from the server",
    "The Xbox LIVE service is busy.", "The online service is busy.",
    "XBOX LIVE", "ONLINE",
    nullptr};
bool g_dedicated = false;
std::vector<std::string> g_map_rotation;
std::string g_emitter;  // --emitter: the particle settings (ApplyEmitter below)
float g_fov = 0.0f;  // --fov, the FIELD OF VIEW setting (the camera hooks below)
std::atomic<bool> g_aiming{false};  // the infantry camera zoomed in (sights), for AIM SENSITIVITY
// The game's dialogs up now (its menus' numDialogs(n) callback, the hook
// below; -1 before the first): a dedicated server presses A on them (xam.cpp).
std::atomic<int> g_game_dialogs{-1};
int GameDialogs() { return g_game_dialogs.load(); }
}  // namespace rt


using namespace rt;

#define GAME_PATCH(addr)                    extern "C" PPC_FUNC(__imp__sub_##addr);   PPC_FUNC(sub_##addr)

// Pad axis read (same function as bf1943's 821C7FD8): r3 = pad, r4 = input; returns
// 0..1 in f1. Inputs 4/5 = right stick right/left, 6/7 = up/down. Pad +8 is the
// user index + 1. Mouse look is added here, unclamped (see MouseLookAxis).
GAME_PATCH(82181390) {
  uint32_t input = ctx.r4.u32, pad = ctx.r3.u32;
  __imp__sub_82181390(ctx, base);
  if (input < 4 || input > 7 || Ld32(pad + 8) != 1) return;
  float v = MouseLookAxis(input >= 6);
  if (input & 1) v = -v;
  if (v > 0) ctx.f1.f64 = std::max(ctx.f1.f64, double(v));
}

// D3D queues a flip (bf1943's 82160160): r3 = front buffer | interval << 8 | ...
// The flip waits for last flip + interval vblanks (the game asks for 2 = 30 fps
// at 60 Hz). Interval 0 flips at once; VdSwap (gpu.cpp) caps at --max-fps.
GAME_PATCH(82160148) {
  if (!g_vsync) ctx.r3.u64 &= ~uint64_t(0xF00);
  __imp__sub_82160148(ctx, base);
}

// --host-map <level> (mp_harbour_day1...) or --ui-call <callback>[:<args>]: two seconds after the main menu is up,
// call the menu's hostMultiplayer callback (hostMultiplayerLocal without
// --online-server) on the menu thread, as the Host Game screen would.
// Set by the menu callback dispatcher hook (sub_8242F698) below.
static std::atomic<uint32_t> g_menu_manager{0};
static std::atomic<DWORD> g_menu_thread{0};
static std::atomic<int64_t> g_menu_up_ms{0};
static void CallMenu(PPCContext& ctx, uint8_t* base, const std::string& cb, const std::string& arg);
// Online (--online-server) the game logs in first (presenceRequestOnline, as
// opening the Multiplayer menu does) and hosts once the Theater login answered.
// RequestHost (our Multiplayer menu, lan.cpp) starts it again later.
static std::atomic<bool> g_internal_call{false};  // CallMenu from here, not the player
// The host's /map (admin_server.cpp) and a joiner sent along with it, or one
// kicked: the level is left (the menu's doExitLevel callback, as the pause
// menu's exit does), then, back in the main menu, the map is hosted (or the
// host joined again once it says the map is up). A host with bots, a
// dedicated server and any Windows host starts the game again on the map
// instead (admin::RestartOnMap): the bots' made-up connections do not go down
// with a level, and a second level in one run has no bots.
static std::mutex g_admin_mutex;
static std::string g_admin_map;           // the map to host (or join) after leaving; empty: stay in the menus
static std::atomic<int> g_admin_exit{0};  // 1 leave the level now, 2 left: waiting for the main menu
static bool g_rejoin = false;             // a joiner: join the host again on g_admin_map
static std::atomic<bool> g_rejoin_ready{false};
static double g_rejoin_since = 0;         // (seconds; it joins anyway after 90 s without the host's word)
static bool g_admin_map_up = false;       // host: tell the joiners once the new level is up
static std::atomic<bool> g_host_called{false};  // the host callback has run (a level is being made)
static double SteadySeconds() {
  return double(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
                    .count()) / 1000.0;
}
void rt::RequestMapChange(const std::string& map) {
  std::lock_guard lock(g_admin_mutex);
  g_admin_map = map, g_rejoin = false, g_admin_exit = 1;
}
void rt::RequestRejoin(const std::string& map) {
  std::lock_guard lock(g_admin_mutex);
  g_admin_map = map, g_rejoin = true, g_rejoin_ready = false, g_admin_exit = 1;
  g_rejoin_since = SteadySeconds();
}
void rt::RejoinReady() { g_rejoin_ready = true; }
// The maps /map takes: a short name or the level's own.
const char* const* rt::admin::MapAliases() {
  static const char* const kAliases[] = {
      "valley", "mp_harbour_day1",    "valleyrun", "mp_harbour_day1", "harbour1", "mp_harbour_day1",
      "decon", "mp_harbour_day2",     "deconstruction", "mp_harbour_day2", "harbour2", "mp_harbour_day2",
      "oasis", "mp_ignition_day1",    "ignition1", "mp_ignition_day1",
      "ignition", "mp_ignition_day2", "finalignition", "mp_ignition_day2", "ignition2", "mp_ignition_day2",
      "endoftheline", "mp_monastery_day1", "monastery", "mp_monastery_day1", "monastery1", "mp_monastery_day1",
      "ascension", "mp_monastery_day2", "monastery2", "mp_monastery_day2",
      "harvest", "mp_beachhead_day1", "harvestday", "mp_beachhead_day1", "beachhead", "mp_beachhead_day1",
      "beachhead1", "mp_beachhead_day1",
      "overandout", "mp_beachhead_day2", "over", "mp_beachhead_day2", "beachhead2", "mp_beachhead_day2",
      "mp_harbour_day1", "mp_harbour_day1", "mp_harbour_day2", "mp_harbour_day2",
      "mp_ignition_day1", "mp_ignition_day1", "mp_ignition_day2", "mp_ignition_day2",
      "mp_monastery_day1", "mp_monastery_day1", "mp_monastery_day2", "mp_monastery_day2",
      "mp_beachhead_day1", "mp_beachhead_day1", "mp_beachhead_day2", "mp_beachhead_day2",
      nullptr};
  return kAliases;
}
const char* rt::admin::MapChoices() { return "valley|decon|oasis|ignition|monastery|ascension|harvest|over"; }

static void AdminFromMainLoop(PPCContext& ctx, uint8_t* base) {
  if (g_admin_exit == 1 && g_menu_manager) {
    std::string map;
    bool rejoin;
    {
      std::lock_guard lock(g_admin_mutex);
      map = g_admin_map, rejoin = g_rejoin;
    }
    if (!rejoin && !map.empty() && rt::admin::RestartOnMap(map)) return;
    if (!g_in_menus) {
      g_leaving_level = true;
      RT_INFO("admin: leaving the level (doExitLevel)");
      CallMenu(ctx, base, "doExitLevel", "");
    }
    g_admin_exit = 2;
  }
  // Back in the main menu, and settled there (no notice up) for 4 s: hosting
  // at once, while the game still went through its exit, was dropped.
  static double settled_since = 0;
  const double now_s = SteadySeconds();
  if (g_admin_exit != 2 || !g_in_menus || g_game_dialogs.load() > 0) settled_since = 0;
  else if (!settled_since) settled_since = now_s;
  if (g_admin_exit == 2 && settled_since && now_s - settled_since >= 4.0) {
    std::string map;
    bool rejoin;
    {
      std::lock_guard lock(g_admin_mutex);
      map = g_admin_map, rejoin = g_rejoin;
    }
    if (rejoin) {
      // A joiner: once the host says its map is up (or after 90 s), join it.
      if (!g_rejoin_ready && now_s - g_rejoin_since < 90) return;
      g_admin_exit = 0;
      // (both in network order: the address's bytes as they are)
      const uint8_t* ip = reinterpret_cast<const uint8_t*>(&g_join_address);
      const uint8_t* port = reinterpret_cast<const uint8_t*>(&g_join_port);
      char target_text[64];
      snprintf(target_text, sizeof(target_text), "%u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3], unsigned(port[0] << 8 | port[1]));
      RT_INFO("admin: joining the host again (%s, %s)", target_text, map.c_str());
      if (!g_join_address || !rt::lan::Join(target_text, map, g_join_password)) RT_WARN("admin: could not join the host again");
      return;
    }
    g_admin_exit = 0;
    if (map.empty()) return;
    {
      std::lock_guard lock(g_host_map_mutex);
      g_host_map = map;
    }
    RT_INFO("admin: hosting %s", map.c_str());
    g_admin_map_up = true;
    g_host_called = false;
    g_host_request = true;
  }
  // The new level is up: the joiners sent away by /map come back.
  if (g_admin_map_up && g_host_called && !g_in_menus) {
    g_admin_map_up = false;
    std::string map;
    {
      std::lock_guard lock(g_host_map_mutex);
      map = g_host_map;
    }
    std::thread([map] {
      for (int i = 0; i < 3; ++i) {  // (UDP: said three times; a joiner acts on the first)
        std::this_thread::sleep_for(std::chrono::seconds(i ? 5 : 3));
        rt::admin::MapReady(map);
      }
    }).detach();
  }
}

static void HostFromMainLoop(PPCContext& ctx, uint8_t* base) {
  if (g_menu_up_ms && GetCurrentThreadId() == g_menu_thread) AdminFromMainLoop(ctx, base);
  static int state = -1;  // -1 idle, 0 waiting for the menu, 1 logging in
  static bool startup = !g_host_map.empty() || !g_ui_call.empty();  // --host-map, --ui-call
  if (!g_menu_up_ms || GetCurrentThreadId() != g_menu_thread) return;
  if (startup) startup = false, state = 0;
  if (g_host_request.exchange(false)) state = 0;
  if (state < 0 || g_host_map.empty() && g_ui_call.empty()) return;
  if (int64_t(GetTickCount64()) - g_menu_up_ms < 2000) return;
  if (state == 0 && !g_online_server.empty()) {
    state = 1;
    RT_INFO("host: logging in");
    CallMenu(ctx, base, "presenceRequestOnline", "1");
    return;
  }
  if (state == 1 && !g_online_ready) return;
  state = -1;
  std::string cb = g_online_server.empty() ? "hostMultiplayerLocal" : "hostMultiplayer", arg = g_host_map;
  if (!g_ui_call.empty()) {  // --ui-call name:args (testing)
    size_t colon = g_ui_call.find(':');
    cb = g_ui_call.substr(0, colon);
    arg = colon == std::string::npos ? "" : g_ui_call.substr(colon + 1);
  }
  RT_INFO("host: %s(%s)", cb.c_str(), arg.c_str());
  g_host_called = true;
  CallMenu(ctx, base, cb, arg);
}

GAME_PATCH(8229B708) {
  static uint32_t logged_modes = 0;  // bit per mode seen (two timers: 0 and 3)
  if (uint32_t mode = Ld32(ctx.r3.u32 + 40); mode < 32 && !(logged_modes & (1u << mode))) {
    logged_modes |= 1u << mode;
    RT_INFO("frame timer: mode %u, tick rate %u", mode, Ld32(ctx.r3.u32 + 44));
  }
  // Frame timer update (bf1943's 822C7388): r3 = timer, +40 mode, +44 tick rate,
  // +48 ticks. Game.MaxFps is read straight from its setting (pointer cached at
  // 82C24C68 on the first call), 30 by default.
  // With a limit, the timer counts Game.MaxFps ticks per second (+48), but every
  // tick still lasts 1/rate (+44), so the limit and the rate must agree. The
  // simulation timers (mode 1) run at --tickrate (75 by default; 30 restores
  // the original game's rate) and are created with that rate in
  // 82295A20 below. Others get 0 (no limit) and count their own rate.
  // The server's timer (mode 1, only while hosting: one tick per loop, paced by
  // sleeping 1/Game.MaxFps) uses the same rate, else it spins. The setting is
  // shared and read at the start of each call, so it's set per call.
  if (uint32_t max_fps = Ld32(0x82C24C68)) {
    float f = Ld32(ctx.r3.u32 + 40) == 1 ? float(std::clamp(g_tick_rate, 30u, 120u)) : 0.0f;
    uint32_t bits;
    memcpy(&bits, &f, 4);
    St32(max_fps, bits);
  }
  HostFromMainLoop(ctx, base);
  __imp__sub_8229B708(ctx, base);
}

// Simulation timer creation (bf1943's 822C7270): r3 = timer, r4 = tick rate
// (the game asks for 30), r5 = mode. The mode 1 timers (the client's and the
// server's) get --tickrate, so every tick lasts 1/rate and the game's time is
// right; the other timers keep what the game asks for. The first timer is the
// client's (made on the main thread at boot), and its thread is the one that
// simulates the local player.
static uint32_t g_client_thread;
GAME_PATCH(82295A20) {
  if (!g_client_thread) g_client_thread = CurrentThreadId();
  if (ctx.r5.u32 == 1) ctx.r4.u64 = std::clamp(g_tick_rate, 30u, 120u);
  __imp__sub_82295A20(ctx, base);
}
uint32_t rt::ClientThreadId() { return g_client_thread; }

// Sets the frame limiter's mode (bf1943's 822BBCF8; r4: 2 = vblank-synced with
// Game.VsyncEnable, else 0/1 = timed), called from the main loop at 82343F24.
GAME_PATCH(8228CCD8) {
  if (!g_vsync) ctx.r4.u64 = 0;
  __imp__sub_8228CCD8(ctx, base);
}


// Test automation (BF_AUTO_MP=1): presses Start at the title until the profile
// is read, then waits for the main menu file. After that it watches the file
// BF_AUTO_MP_FILE (default auto_steps.txt in the run folder): each new line is one
// button (up, down, left, right, a, b, start), pressed 1.5 s apart, so the menus
// can be driven one step at a time while looking at screenshots.
#include <thread>
#include <set>
#include <string>
#include <fstream>
#include <cstdio>
static const bool g_auto_mp_started = [] {
  if (!getenv("BF_AUTO_MP")) return false;
  const char* env_file = getenv("BF_AUTO_MP_FILE");
  std::string path = env_file ? env_file : "auto_steps.txt";
  std::thread([path] {
    auto press = [](uint16_t buttons, int ms) {
      g_auto_buttons = buttons;
      std::this_thread::sleep_for(std::chrono::milliseconds(ms));
      g_auto_buttons = 0;
    };
    while (!g_title_passed) {
      press(XINPUT_GAMEPAD_START, 200);
      std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    RT_INFO("auto mp: title passed, waiting for the main menu");
    while (!g_mainmenu_open) std::this_thread::sleep_for(std::chrono::milliseconds(250));
    RT_INFO("auto mp: main menu file open; reading %s", path.c_str());
    size_t done = 0;
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      std::ifstream in(path);
      std::vector<std::string> lines;
      for (std::string line; std::getline(in, line);) lines.push_back(line);
      while (done < lines.size()) {
        const std::string& name = lines[done++];
        uint16_t b = name == "down" ? XINPUT_GAMEPAD_DPAD_DOWN : name == "up" ? XINPUT_GAMEPAD_DPAD_UP
                   : name == "right" ? XINPUT_GAMEPAD_DPAD_RIGHT : name == "left" ? XINPUT_GAMEPAD_DPAD_LEFT
                   : name == "a" ? XINPUT_GAMEPAD_A : name == "b" ? XINPUT_GAMEPAD_B
                   : name == "start" ? XINPUT_GAMEPAD_START : 0;
        if (name == "wait") std::this_thread::sleep_for(std::chrono::seconds(3));
        if (name.rfind("hold:", 0) == 0) {  // hold:<a|b|x|y|lb|rb|ls|rs|back>[:ms] (1.5 s)
          std::string btn = name.substr(5);
          int ms = 1500;
          if (size_t c = btn.find(':'); c != std::string::npos) ms = std::atoi(btn.c_str() + c + 1), btn.resize(c);
          uint16_t bits = btn == "a" ? XINPUT_GAMEPAD_A : btn == "b" ? XINPUT_GAMEPAD_B
                        : btn == "x" ? XINPUT_GAMEPAD_X : btn == "y" ? XINPUT_GAMEPAD_Y
                        : btn == "lb" ? XINPUT_GAMEPAD_LEFT_SHOULDER : btn == "rb" ? XINPUT_GAMEPAD_RIGHT_SHOULDER
                        : btn == "ls" ? XINPUT_GAMEPAD_LEFT_THUMB : btn == "rs" ? XINPUT_GAMEPAD_RIGHT_THUMB
                        : btn == "back" ? XINPUT_GAMEPAD_BACK : 0;
          press(bits, ms);
          RT_INFO("auto mp: %s (step %zu)", name.c_str(), done);
          continue;
        }
        if (name == "strafe" || name == "fire") {  // left stick right / right trigger, 1 s
          if (name == "strafe") g_auto_lx = 32000; else g_auto_rt = 255;
          std::this_thread::sleep_for(std::chrono::seconds(1));
          g_auto_lx = 0, g_auto_rt = 0;
          RT_INFO("auto mp: %s (step %zu)", name.c_str(), done);
          continue;
        }
        if (name == "walklong") {  // left stick forward 8 s
          g_auto_ly = 32000;
          std::this_thread::sleep_for(std::chrono::seconds(8));
          g_auto_ly = 0;
          RT_INFO("auto mp: walklong (step %zu)", done);
          continue;
        }
        if (name == "walk" || name == "turn" || name == "look") {
          // left stick forward 2 s / right stick right or up 0.5 s
          auto& stick = name == "walk" ? g_auto_ly : name == "turn" ? g_auto_rx : g_auto_ry;
          stick = 32000;
          std::this_thread::sleep_for(std::chrono::milliseconds(name == "walk" ? 2000 : 500));
          stick = 0;
          RT_INFO("auto mp: %s (step %zu)", name.c_str(), done);
          continue;
        }
        if (!b) continue;
        press(b, 150);  // long enough for the menu to see it at ~40 fps (two instances)
        RT_INFO("auto mp: pressed %s (step %zu)", name.c_str(), done);
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
      }
    }
  }).detach();
  return true;
}();

// Replay (BF_REPLAY=<file>): plays back a recording made with BF_RECORD. "cp title" and
// "cp menu" lines wait until the game reaches that point; the press lines after a
// checkpoint are timed from it (see RecordPad in xam.cpp).
static const bool g_replay_started = [] {
  const char* path = getenv("BF_REPLAY");
  if (!path) return false;
  std::string file = path;
  std::thread([file] {
    int64_t zero = 0;
    RT_INFO("replay: thread started, reading %s", file.c_str());
    while ((zero = g_pad_t0_ns.load()) == 0) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    RT_INFO("replay: first pad read seen, starting");
    std::ifstream in(file);
    int64_t base = zero;
    for (std::string line; std::getline(in, line);) {
      if (line.rfind("cp ", 0) == 0) {
        // The menu checkpoint is set during boot in some runs, so only the title waits.
        if (line == "cp title") {
          RT_INFO("replay: waiting for the title");
          // Start is pressed again every 2 s, so a press that lands during the intro is not lost.
          while (!g_title_passed.load()) {
            g_auto_buttons = XINPUT_GAMEPAD_START;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            g_auto_buttons = 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(1800));
          }
        }
        base = std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
        continue;
      }
      double ms;
      unsigned buttons;
      if (std::sscanf(line.c_str(), "%lf %u", &ms, &buttons) != 2) continue;
      std::this_thread::sleep_until(std::chrono::steady_clock::time_point(
          std::chrono::nanoseconds(base + static_cast<int64_t>(ms * 1e6))));
      g_auto_buttons = static_cast<uint16_t>(buttons);
    }
    g_auto_buttons = 0;
    RT_INFO("replay: done");
  }).detach();
  return true;
}();

// Server message handler (same code as bf1943's 82413BC0: r4 = message, r5 = sub id,
// r6 = server object; 39/16 creates the server, +44 backend 1 = multiplayer UDP).
// A multiplayer server is listed on Arcadia (net.cpp RegisterHostedGame).
GAME_PATCH(823D1568) {
  bool create = ctx.r4.u32 == 39 && ctx.r5.u32 == 16;
  uint32_t server = ctx.r6.u32;
  // --host / --join (lan.cpp): a multiplayer server (+44 = 1, real UDP) that
  // isn't bound to 127.0.0.1 only (+41), as bf1943's 82413BC0 patch does.
  if (create) RT_INFO("server: create, backend %u, localhost only %u", Ld32(server + 44), *G<uint8_t>(server + 41));
  if (create && g_lan_host) {
    St32(server + 44, 1);
    St8(server + 41, 0);
  }
  __imp__sub_823D1568(ctx, base);
  if (g_join_address) return;  // a joiner's own server isn't listed
  // Listed with the VDP port (1000): a joiner first sends its session handshake
  // there (to the listed port, from its own VDP socket), not to the server's 1003.
  if (create && Ld32(server + 44) == 1)
    RegisterHostedGame(uint16_t(1000 + g_port_offset), g_host_map.empty() ? std::string("bfbc1") : g_host_map);
}

// Server setup (r3 = server): copies Server.LoadingTimeout to +556 and
// Server.IngameTimeout to +560 (seconds). With --host / --join a player who
// hasn't pressed Ready on the loading screen, or idles, isn't sent back to the
// menu (which also closes a host's server), so both are made very long.
GAME_PATCH(8236DED8) {
  uint32_t server = ctx.r3.u32;
  __imp__sub_8236DED8(ctx, base);
  if (!g_lan_host) return;
  float loading, ingame, never = 1e7f;
  uint32_t v = Ld32(server + 556), w = Ld32(server + 560), n;
  memcpy(&loading, &v, 4), memcpy(&ingame, &w, 4), memcpy(&n, &never, 4);
  RT_INFO("server: loading timeout %.0f s, ingame timeout %.0f s -> off", loading, ingame);
  St32(server + 556, n);
  St32(server + 560, n);
}

// Menu callback dispatcher: r3 = menu manager, r4 = callback name, r5 = its
// arguments as one string ("Movies/UIMAIntro", "0"; several are comma separated).
// startBackgroundMovie is called once the main menu is up.

GAME_PATCH(8242F698) {
  g_menu_manager = ctx.r3.u32;
  g_menu_thread = GetCurrentThreadId();
  static const bool trace = getenv("BF_MENU_TRACE") != nullptr;
  if (trace) RT_INFO("menu: callback %s(%s)", G<char>(ctx.r4.u32), ctx.r5.u32 ? G<char>(ctx.r5.u32) : "");
  if (!strcmp(G<char>(ctx.r4.u32), "numDialogs") && ctx.r5.u32) g_game_dialogs = atoi(G<char>(ctx.r5.u32));
  if (!strcmp(G<char>(ctx.r4.u32), "startBackgroundMovie")) {
    RT_INFO("menu: main menu up");
    if (!g_menu_up_ms) g_menu_up_ms = GetTickCount64();
  }
  // MULTIPLAYER on the main menu logs in to EA's servers (gone) and opens the
  // game's online menu: ours (bc_mp_menu.cpp, LAN host/join) opens instead.
  // The game also asks by itself some minutes after a match, which only opens
  // ours when the player has just pressed A (or Enter, or clicked).
  // OPTIONS (main menu and pause menus): its storage device prompt reads PC
  // SETTINGS (g_game_pc_text) and opens that page.
  if (!g_internal_call && !strcmp(G<char>(ctx.r4.u32), "changeStorageDevice")) {
    RT_INFO("menu: PC SETTINGS (options)");
    g_open_pc_settings = true;  // opened on the input thread (AddBotInput), where the page runs
    return;
  }
  // PC SETTINGS, the main menu's fifth row (tools/bc_menu_patch.py: the
  // retail-hidden DEV row), sends presenceRequestOnline("0") when chosen: the
  // PC settings page opens over the DEV page the game goes to (closed again
  // with B once the PC page is, AddBotInput). The menu also sends it when it
  // comes up (its row number not set yet): only just after an A press counts.
  // Only with the patched menu (else "0" is the game's own sign-out) and
  // offline (signed in, it stays the game's).
  static const bool patched_menu = std::filesystem::is_regular_file(
      g_user_root / "override" / "dist" / "xenon" / "menu" / "main" / "mainmenu-00.fbrb");
  if (!g_internal_call && patched_menu && g_online_server.empty() &&
      !strcmp(G<char>(ctx.r4.u32), "presenceRequestOnline") && ctx.r5.u32 && !strcmp(G<char>(ctx.r5.u32), "0")) {
    int64_t accepted = int64_t(g_last_accept_ms.load());
    int64_t now = int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    if (now - accepted < 1000 && !pcset::IsOpen()) {
      RT_INFO("menu: PC SETTINGS");
      g_open_pc_settings = true;
      g_back_out_of_dev = true;
    }
    return;
  }
  if (!g_internal_call && g_online_server.empty() && !strcmp(G<char>(ctx.r4.u32), "presenceRequestOnline")) {
    int64_t accepted = int64_t(g_last_accept_ms.load());
    int64_t now = int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    if (now - accepted < 3000) bcmp::Open();
    else RT_INFO("mp menu: not opened (the game asked, no A press)");
    return;
  }
  __imp__sub_8242F698(ctx, base);
}

static void CallMenu(PPCContext& ctx, uint8_t* base, const std::string& cb, const std::string& arg) {
  static uint32_t strs = g_system_heap->Alloc(512, 16);
  snprintf(G<char>(strs), 64, "%s", cb.c_str());
  snprintf(G<char>(strs + 64), 448, "%s", arg.c_str());
  PPCContext c = ctx;
  c.r3.u64 = g_menu_manager;
  c.r4.u64 = strs;
  c.r5.u64 = strs + 64;
  g_internal_call = true;
  sub_8242F698(c, base);
  g_internal_call = false;
}

// The camera, as bf1943's (game_patches.cpp; the same code and offsets here).
// FIELD OF VIEW (g_fov, horizontal degrees at 16:9; the game's own 85.6): the
// camera manager's frame sub_8226DB00(manager) calls the view's SetFov
// (sub_827F2A20, f1 = vertical radians) first for the camera (0x8226DD50),
// then maybe an override (0x8226DD98); sub_8226D8B8 hands the angle to another
// view (sub_82482970). Each is widened by one ratio of half-angle tangents, so
// a sight's zoom narrows as much as before. The manager's FOV controller
// (+0xE0: +0x28 the camera's own angle, +0x30 the angle now) tells aiming.
namespace {
double WiderFov(double radians) {
  const float h = rt::g_fov;
  if (h <= 0.0f || !(radians > 0.0 && radians < 3.0)) return radians;
  static const double kGame = std::tan(85.6 * 0.5 * 3.14159265358979 / 180.0);
  double k = std::tan(double(std::clamp(h, 60.0f, 120.0f)) * 0.5 * 3.14159265358979 / 180.0) / kGame;
  return std::min(2.0 * std::atan(std::tan(radians * 0.5) * k), 2.6);
}
float GuestFloat(uint32_t a) {
  uint32_t u = Ld32(a);
  float f;
  memcpy(&f, &u, 4);
  return f;
}
void SetGuestFloat(uint32_t a, float f) {
  uint32_t u;
  memcpy(&u, &f, 4);
  St32(a, u);
}
thread_local uint32_t t_camera_manager = 0;
thread_local int t_camera_setfov = 0;
thread_local bool t_second_view = false;
thread_local bool t_view_effects = false;
}  // namespace
GAME_PATCH(8226DB00) {
  const uint32_t manager = ctx.r3.u32, before = t_camera_manager;
  const int calls = t_camera_setfov;
  t_camera_manager = manager, t_camera_setfov = 0;
  __imp__sub_8226DB00(ctx, base);
  t_camera_manager = before, t_camera_setfov = calls;
}
GAME_PATCH(8226D8B8) {
  const bool before = t_second_view;
  t_second_view = true;
  __imp__sub_8226D8B8(ctx, base);
  t_second_view = before;
}
GAME_PATCH(827F2A20) {
  if (t_camera_manager && t_camera_setfov++ == 0) {
    const uint32_t controller = Ld32(t_camera_manager + 0xE0);
    if (controller) {
      float now = GuestFloat(controller + 0x30), own = GuestFloat(controller + 0x28);
      rt::g_aiming = own > 1.0f && now < own - 0.5f;
    }
    ctx.f1.f64 = WiderFov(ctx.f1.f64);
  }
  __imp__sub_827F2A20(ctx, base);
}
// ULTRAWIDE (xgpu::SetWide; bf1943's 82583430 / 82623DB8): a view's update,
// sub_827F2C08(view), builds its culling frustum (sub_82482970) from its
// vertical angle (+12) and aspect (+32, width / height; +16 = +12 * +32).
// Every 16:9 view (camera, weapon, shadow cascades) gets the window's aspect:
// the same vertical angle, more to the sides; the presenter stretches the 16:9
// frame over the window, which undoes the squeeze. Square views stay.
namespace {
std::mutex g_wide_mutex;
std::vector<float> g_wide_applied;  // aspects given to views so far (a resize)
float WideAspect(float a) {
  const float want = xgpu::ViewAspect();
  if (std::abs(a - want) < 1e-4f) return a;
  bool ours = std::abs(a - 16.0f / 9.0f) < 0.002f;
  std::lock_guard lock(g_wide_mutex);
  for (float applied : g_wide_applied) ours |= std::abs(a - applied) < 1e-4f;
  if (!ours) return a;
  if (std::find(g_wide_applied.begin(), g_wide_applied.end(), want) == g_wide_applied.end() &&
      g_wide_applied.size() < 64)
    g_wide_applied.push_back(want);
  return want;
}
}  // namespace
GAME_PATCH(827F2C08) {
  const uint32_t view = ctx.r3.u32;
  const float a = GuestFloat(view + 32), wide = WideAspect(a);
  if (wide != a) {
    SetGuestFloat(view + 32, wide);
    SetGuestFloat(view + 16, GuestFloat(view + 12) * wide);
    St32(view, Ld32(view) | 10);
  }
  __imp__sub_827F2C08(ctx, base);
}
GAME_PATCH(82482970) {
  if (t_second_view) ctx.f1.f64 = WiderFov(ctx.f1.f64);
  // ULTRAWIDE: views built without their update (sub_8226D8B8's) the same.
  if (float wide = WideAspect(float(ctx.f2.f64)); wide != float(ctx.f2.f64)) ctx.f2.f64 = wide;
  __imp__sub_82482970(ctx, base);
}

// CAMERA SHAKE: three curves (yaw, roll, pitch) at shaker+0x50/+0x54/+0x58,
// each with its value now at curve+8, turned into this frame's rotation by
// sub_82232DF8(shaker); scaled for that call (g_camera_shake), then set back.
GAME_PATCH(82232DF8) {
  const float scale = rt::g_camera_shake;
  const uint32_t shaker = ctx.r3.u32;
  if (scale == 1.0f || !shaker) {
    __imp__sub_82232DF8(ctx, base);
    return;
  }
  uint32_t curves[3];
  float kept[3];
  for (int i = 0; i < 3; ++i) {
    curves[i] = Ld32(shaker + 0x50 + 4 * i);
    if (curves[i]) kept[i] = GuestFloat(curves[i] + 8), SetGuestFloat(curves[i] + 8, kept[i] * scale);
  }
  __imp__sub_82232DF8(ctx, base);
  for (int i = 0; i < 3; ++i)
    if (curves[i]) SetGuestFloat(curves[i] + 8, kept[i]);
}

// MOTION BLUR off: the view's effects are gathered by sub_82269688(manager,
// camera), with the first-person camera's blur from sub_82263350 and the
// manager's own (+0x100, used above 0); sub_82812AB8 is the blur pass (the
// renderer's vtable slot 31). Off, the blur reads 0 there and the pass is skipped.
GAME_PATCH(82269688) {
  const uint32_t manager = ctx.r3.u32;
  if (rt::g_motion_blur || !manager) {
    __imp__sub_82269688(ctx, base);
    return;
  }
  const float own = GuestFloat(manager + 0x100);
  SetGuestFloat(manager + 0x100, 0.0f);
  t_view_effects = true;
  __imp__sub_82269688(ctx, base);
  t_view_effects = false;
  SetGuestFloat(manager + 0x100, own);
}
GAME_PATCH(82263350) {
  __imp__sub_82263350(ctx, base);
  if (t_view_effects) ctx.f1.f64 = 0.0;
}
GAME_PATCH(82812AB8) {
  if (rt::g_motion_blur) __imp__sub_82812AB8(ctx, base);
}

// Hardcore (kernel.h g_hardcore_hud; bf1943's game_patches.cpp has the same): the
// HUD and crosshair are the game's 2D UI movie draws, as its menus are; they are
// skipped on the GPU (xgpu::SetHideUi) while in a level with no menu over it
// (the spawn screen and pause menu: g_level_menu) and the scoreboard key not
// held (xam.cpp g_scoreboard_open), so the menus and scoreboard still show.
// Called from the UI's per-frame step below.
namespace {
struct HardcoreSupported {
  HardcoreSupported() { rt::g_hardcore_supported = true; }
} g_hardcore_supported_install;
void HardcoreHud() {
  static int last = -1;
  // BF_HIDE_UI_TEST=1 (testing): hidden everywhere, menus too (to see what the draw detection catches).
  static const bool test = getenv("BF_HIDE_UI_TEST") != nullptr;
  const bool hide = test || (rt::g_hardcore_hud && !rt::g_in_menus && !rt::g_level_menu && !rt::g_scoreboard_open);
  if (int(hide) == last) return;
  last = int(hide);
  xgpu::SetHideUi(hide, true);
  static int said = 0;
  if (rt::g_hardcore_hud && said++ < 20) RT_INFO("hardcore: the HUD %s", hide ? "hidden" : "shown (a menu or the scoreboard)");
}
}  // namespace

// HUD and menu animations above 30 fps (bf1943's 82954388): the UI's update
// (sub_826C54F8) steps its Flash player one movie frame a call,
// sub_8278ED20(player, 33, 1), once a game frame, and the movies are made for
// 30 frames a second: the player is stepped as many times as a 30 Hz clock has
// ticked since the last step (none on most frames, two at most after a hitch).
GAME_PATCH(8278ED20) {
  HardcoreHud();
  // ULTRAWIDE (bf_menu.h): under a menu (the loading screen too: a level before
  // its first spawn screen) the UI's draws stay together in the centre.
  if (ctx.r4.u32 == 33) {
    static bool spawn_seen = false;
    if (rt::g_in_menus) spawn_seen = false;
    if (rt::g_level_menu) spawn_seen = true;
    xgpu::SetUiMenu(rt::g_in_menus || rt::g_level_menu || rt::g_scoreboard_open || !spawn_seen);
  }
  if (ctx.r4.u32 != 33 || rt::g_max_fps == 30) {
    __imp__sub_8278ED20(ctx, base);
    return;
  }
  static const auto start = std::chrono::steady_clock::now();
  static int64_t stepped = -1;  // movie frames played (on the 30 Hz clock)
  const int64_t due = int64_t(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() * 30.0);
  if (stepped < 0 || due - stepped > 2) stepped = due - (stepped < 0 ? 1 : 2);
  const PPCContext call = ctx;
  for (; stepped < due; ++stepped) {
    ctx = call;
    __imp__sub_8278ED20(ctx, base);
  }
}

// Physics effects (debris, falling trees; bf1943's 8253EDF8): sub_824BBB10(world,
// f1, f2) gets the tick's time in f1 and the frame's in f2, which only a second,
// smoothed physics system inside uses; with ticks and frames apart it ran at the
// wrong speed, so it gets the tick's.
GAME_PATCH(824BBB10) {
  if (ctx.f1.f64 > 0.0) ctx.f2.f64 = ctx.f1.f64;
  __imp__sub_824BBB10(ctx, base);
}

// CRT startup step (bf1943's 827E44F8) run just before the entry point checks
// the "parse the command line" flag at 82E5E760, which retail never sets: set
// when --args gave the game a command line (ExLoadedCommandLine).
GAME_PATCH(8292C5B8) {
  __imp__sub_8292C5B8(ctx, base);
  if (!g_guest_args.empty()) St32(0x82E5E760, 1);
}

// An explosion entity's destructor (vtables 82007F8C / 82007EB8; bf1943's
// 82205B20): its world position (+0x10), where the scene effects add smoke,
// dust and debris.
GAME_PATCH(821D9E28) {
  // Read before the destructor runs (the object is freed after).
  float pos[3];
  for (int i = 0; i < 3; ++i) pos[i] = GuestFloat(ctx.r3.u32 + 0x10 + 4 * i);
  __imp__sub_821D9E28(ctx, base);
  if (std::isfinite(pos[0]) && std::isfinite(pos[1]) && std::isfinite(pos[2]) &&
      (std::abs(pos[0]) > 1.0f || std::abs(pos[2]) > 1.0f) && std::abs(pos[0]) < 5000 && std::abs(pos[2]) < 5000)
    xgpu::ExplosionAt(pos);
}

// The game's own music and voice volumes (PC settings; bf1943's AudioTick):
// the sound system (the object at *82C7C034, vtable 8204BA68) keeps them at
// +740 and +744 with a changed flag at +749 (its setters 8267A1B0 / 8267A1C0,
// vtable slots 30 and 31, called by name, MusicVolume and DialogueVolume, from
// 822538E0). 100 % is the game's own level (read when the object is first
// seen); set again every few seconds (a level may reset them).
namespace {
void AudioTick(PPCContext&) {
  static uint32_t system_seen = 0;
  static float game_music = 1.0f, game_voice = 1.0f, music = -1.0f, voice = -1.0f;
  static int ticks = 0;
  uint32_t system = Ld32(0x82C7C034);
  if (!system) return;
  if (system != system_seen) {
    system_seen = system;
    float m = GuestFloat(system + 740), v = GuestFloat(system + 744);
    game_music = m >= 0.0f && m <= 4.0f ? m : 1.0f;
    game_voice = v >= 0.0f && v <= 4.0f ? v : 1.0f;
    RT_INFO("sound: the game's music %.2f, voice %.2f (sound system %08X)", game_music, game_voice, system);
    ticks = 500;
  }
  if (++ticks < 500 && music == rt::g_music_volume && voice == rt::g_voice_volume) return;  // ~2.7 s
  ticks = 0;
  music = rt::g_music_volume, voice = rt::g_voice_volume;
  SetGuestFloat(system + 740, game_music * music);
  SetGuestFloat(system + 744, game_voice * voice);
  *G<uint8_t>(system + 749) = 1;
}
struct AudioTickInstall {
  AudioTickInstall() { rt::g_audio_tick = AudioTick; }
} g_audio_tick_install;
}  // namespace

// A lost connection (bf1943's 8237CE40): sub_823259F0(connection, reason), the
// network client's vtable slot 30, closes the game's connection to its server.
// The reason is kept for the main menu's notice (pc_settings.cpp).
GAME_PATCH(823259F0) {
  const uint32_t reason = ctx.r4.u32;
  RT_INFO("net: the connection to the server closed (reason %u)", reason);
  rt::g_disconnect_reason = int(reason);
  rt::g_disconnect_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
  __imp__sub_823259F0(ctx, base);
}

// In-level menus (the pause menu, the spawn screen; bf1943's 823CD3E8): r3 =
// the client's menu handler, r4 = show; it pushes or pops the "Menu" input
// context (r3+60). The mouse works the menu way while it is set (xam.cpp).
GAME_PATCH(82381890) {
  g_level_menu = (ctx.r4.u32 & 0xFF) != 0;
  if (g_log_ui) RT_INFO("ui: level menu %s", g_level_menu ? "shown" : "hidden");
  __imp__sub_82381890(ctx, base);
}

// Shadow, WorldRender, vegetation and particle settings (bf1943's
// game_patches.cpp, the same fields at the same offsets here): after a level
// starts loading, every copy of the WorldRender settings (vtable 820632A0:
// shadowmap width +A8, height +78, slices +84, view distance +AC) found in
// guest memory gets --shadowmap, --shadow-distance and --worldrender, a few
// times while the level's objects are created; OvergrowthSettings --lod and
// --overgrowth; EmitterSystemSettings --emitter.
namespace {
// WorldRender fields (as bf1943's: the same names and offsets): offset and type (f = float,
// i = int, b = bool).
struct WorldRenderField { const char* name; uint32_t offset; char type; };
const WorldRenderField kWorldRenderFields[] = {
    {"ShadowMinScreenArea", 0x98, 'f'},          {"ShadowViewportScale", 0x8C, 'f'},
    {"ShadowmapMinFov", 0x90, 'f'},              {"ShadowmapFixedMovementEnable", 0xD8, 'b'},
    {"ShadowmapFixedDepthEnable", 0xD9, 'b'},    {"ShadowmapSizeZScale", 0x9C, 'f'},
    {"ShadowmapWidth", 0xA8, 'i'},               {"ShadowmapHeight", 0x78, 'i'},
    {"ShadowmapSliceCount", 0x84, 'i'},          {"ShadowmapSliceSchemeWeight", 0x80, 'f'},
    {"ShadowmapSliceOverlapFactor", 0xB4, 'f'},  {"ShadowmapFirstSliceScale", 0xB0, 'f'},
    {"ShadowmapViewDistance", 0xAC, 'f'},        {"ShadowmapExtrusionLength", 0xA0, 'f'},
    {"ShadowmapMinScreenArea", 0xA4, 'f'},       {"CascadedBoxShadowmapEnable", 0xDA, 'b'},
    {"CascadedBoxShadowmapClipMethodEnable", 0xDD, 'b'},
    {"ColoredShadowmapSlicesEnable", 0xCF, 'b'}, {"MinScreenArea", 0x88, 'f'},
};
// OvergrowthSettings (vegetation; vtable 8204DB4C, field table at 82E82538).
const WorldRenderField kOvergrowthFields[] = {
    {"GlobalLodScale", 0x10, 'f'},           {"HiLodEnable", 0x68, 'b'},
    {"AdaptiveLodEnable", 0x6D, 'b'},        {"AdaptiveLodMVertexBudget", 0x0C, 'f'},
    {"AdaptiveLodMinModifier", 0x34, 'f'},   {"AdaptiveLodMaxModifier", 0x2C, 'f'},
    {"AdaptiveLodMargin", 0x48, 'f'},        {"BillboardsEnable", 0x6F, 'b'},
    {"OnlyBillboardsEnable", 0x67, 'b'},     {"AlmostOnlyBillboardsEnable", 0x66, 'b'},
    {"MaxBillboardCount", 0x18, 'i'},        {"BillboardScale", 0x1C, 'f'},
    {"UseMaxShadowLOD", 0x64, 'b'},
};
template <size_t N>
const WorldRenderField* FindField(const WorldRenderField (&fields)[N], const std::string& name) {
  for (const auto& f : fields)
    if (name == f.name) return &f;
  return nullptr;
}
const WorldRenderField* FindWorldRenderField(const std::string& name) {
  return FindField(kWorldRenderFields, name);
}
std::string WorldRenderValue(uint32_t o, const WorldRenderField& f) {
  char buf[48];
  if (f.type == 'b') snprintf(buf, sizeof(buf), "%u", *G<uint8_t>(o + f.offset));
  else if (f.type == 'i') snprintf(buf, sizeof(buf), "%d", int(Ld32(o + f.offset)));
  else {
    uint32_t u = Ld32(o + f.offset);
    float v;
    memcpy(&v, &u, 4);
    snprintf(buf, sizeof(buf), "%g", v);
  }
  return buf;
}
void SetWorldRenderValue(uint32_t o, const WorldRenderField& f, const std::string& value) {
  if (f.type == 'b') *G<uint8_t>(o + f.offset) = (value == "1" || value == "true") ? 1 : 0;
  else if (f.type == 'i') St32(o + f.offset, uint32_t(atoi(value.c_str())));
  else {
    float v = float(atof(value.c_str()));
    uint32_t u;
    memcpy(&u, &v, 4);
    St32(o + f.offset, u);
  }
}
}  // namespace
// EmitterSystemSettings (vtable 82042014; its fields from the reflection
// table, found through the type info at 82EA7BB8): the game's particle
// renderer. The 360 draws particles at half resolution and simply shaded.
const WorldRenderField kEmitterFields[] = {
    {"QuadEnableRendering", 0x63, 'b'},       {"QuadSimpleRenderingEnable", 0x65, 'b'},
    {"QuadEnableOverdrawRendering", 0x6A, 'b'}, {"QuadEnableSorting", 0x6F, 'b'},
    {"QuadEnableProjectionScaleCulling", 0x71, 'b'}, {"QuadEnableZOcclusion", 0x6C, 'b'},
    {"QuadHalfResEnable", 0x6E, 'b'},         {"QuadHalfResSlice1ThresholdHigh", 0x0C, 'f'},
    {"QuadHalfResSlice2ThresholdHigh", 0x1C, 'f'}, {"QuadGroupsJoinNone", 0x59, 'b'},
    {"QuadTechnique", 0x2C, 'i'},             {"QuadKeptParticlesAlphaBoost", 0x20, 'f'},
    {"QuadProjectionScaleRampEnd", 0x30, 'f'}, {"MeshDrawCountLimit", 0x44, 'i'},
};
// Every EmitterSystemSettings copy gets --emitter (logged before and after).
void ApplyEmitter(int pass) {
  const std::string& settings = rt::g_emitter;
  if (settings.empty()) return;
  int found = 0;
  for (uint32_t page = 0x40000000; page < 0xC0000000; page += 0x1000) {
    if (!IsCommitted(page)) continue;
    for (uint32_t o = page; o + 0x80 <= page + 0x1000; o += 4) {
      if (Ld32(o) != 0x82042014) continue;
      uint8_t half = *G<uint8_t>(o + 0x6E), simple = *G<uint8_t>(o + 0x65);
      if (half > 1 || simple > 1) continue;
      ++found;
      if (pass == 0 || pass == 29) {
        std::string all;
        for (const auto& f : kEmitterFields) all += std::string(" ") + f.name + "=" + WorldRenderValue(o, f);
        RT_INFO("emitter %08X:%s", o, all.c_str());
      }
      for (size_t p = 0; p < settings.size();) {
        size_t comma = settings.find(',', p);
        if (comma == std::string::npos) comma = settings.size();
        std::string item = settings.substr(p, comma - p);
        p = comma + 1;
        size_t eq = item.find('=');
        if (item.empty()) continue;
        const WorldRenderField* f = eq == std::string::npos ? nullptr : FindField(kEmitterFields, item.substr(0, eq));
        if (f) SetWorldRenderValue(o, *f, item.substr(eq + 1));
        else if (pass == 0) RT_WARN("emitter: unknown setting %s", item.c_str());
      }
    }
  }
  if (pass == 0 || pass == 29) RT_INFO("emitter: %d copies set", found);
}
void rt::ApplyEmitterNow() {
  if (g_in_menus) return;  // the level's settings object is made with the level
  std::thread([] { ApplyEmitter(1); }).detach();
}
// GlobalPostProcessSettings (vtable 820671E0, getType 82B76FC0 -> type
// 82EB52B8, registered by 82B776E8 with the name record 82EB5288 and the
// field table 82E8B0B8 that 82B77074 fills; 0x110 bytes): FILM GRAIN
// (pc_settings.cpp) is its FilmGrainEnable, a bool at +100, as the PC mod's
// "PostProcess.FilmGrainEnable false". OFF writes 0 to every copy; ON puts
// back the game's value in the copies it changed.
const WorldRenderField kPostProcessFields[] = {
    {"FilmGrainEnable", 0x100, 'b'},          {"FilmGrainLinearFilteringEnable", 0xFB, 'b'},
    {"FilmGrainTextureScale", 0x90, 'f'},     {"FilmGrainColorScale", 0x10, 'f'},
    {"HdrBloomEnable", 0xF7, 'b'},            {"HdrBlurEnable", 0xF8, 'b'},
};
namespace {
std::mutex g_grain_mutex;
std::map<uint32_t, uint8_t> g_grain_game;  // copy -> the game's FilmGrainEnable
}  // namespace
void ApplyPostProcess(int pass) {
  std::lock_guard<std::mutex> lock(g_grain_mutex);
  // (the game's own and nothing changed: only looked at once, for the log)
  if (rt::g_film_grain && g_grain_game.empty() && pass != 0) return;
  int found = 0;
  for (uint32_t page = 0x40000000; page < 0xC0000000; page += 0x1000) {
    if (!IsCommitted(page)) continue;
    for (uint32_t o = page; o + 0x110 <= page + 0x1000; o += 4) {
      if (Ld32(o) != 0x820671E0) continue;
      bool bools = true;  // (its bools, +F4..+103: 0 or 1 in a real copy)
      for (uint32_t b = 0xF4; b < 0x104 && bools; ++b) bools = *G<uint8_t>(o + b) <= 1;
      if (!bools) continue;
      ++found;
      uint8_t* grain = G<uint8_t>(o + 0x100);
      if (pass == 0 || pass == 29 || pass < 0) {
        std::string all;
        for (const auto& f : kPostProcessFields) all += std::string(" ") + f.name + "=" + WorldRenderValue(o, f);
        RT_INFO("postprocess %08X:%s", o, all.c_str());
      }
      if (!rt::g_film_grain) {
        g_grain_game.emplace(o, *grain);  // (the first value seen: the game's)
        *grain = 0;
      } else if (auto it = g_grain_game.find(o); it != g_grain_game.end()) {
        *grain = it->second;
      }
    }
  }
  if (rt::g_film_grain) g_grain_game.clear();
  if (pass == 0 || pass == 29 || pass < 0)
    RT_INFO("postprocess: %d copies, film grain %s", found, rt::g_film_grain ? "the game's" : "off");
}
// FILM GRAIN changed (pc_settings.cpp): now, in the level's copies.
static const bool g_bc_film_grain = (g_apply_film_grain = [] {
  if (g_in_menus) return;
  std::thread([] { ApplyPostProcess(-1); }).detach();
}, true);
// Every OvergrowthSettings copy gets --overgrowth (logged before and after).
void ApplyOvergrowth(int pass) {
  // The engine's GlobalLodScale shrinks the vegetation LOD distances: 1 at
  // bf1943's Wake Island, 0.25 shows the far palms and bushes at no measured cost,
  // 0.1 costs a third of the frame rate for little more, 4 removes them.
  std::string settings;
  if (rt::g_lod > 0 && rt::g_lod != 1.0f) settings = "GlobalLodScale=" + std::to_string(1.0f / rt::g_lod) + ",";
  settings += rt::g_overgrowth;
  if (settings.empty()) return;
  int found = 0;
  for (uint32_t page = 0x40000000; page < 0xC0000000; page += 0x1000) {
    if (!IsCommitted(page)) continue;
    for (uint32_t o = page; o + 0x74 <= page + 0x1000; o += 4) {
      if (Ld32(o) != 0x8204DB4C) continue;
      uint32_t u = Ld32(o + 0x10);
      float scale;
      memcpy(&scale, &u, 4);
      if (!(scale > 0.001f && scale < 1000.0f)) continue;
      ++found;
      if (pass == 0 || pass == 29) {
        std::string all;
        for (const auto& f : kOvergrowthFields) all += std::string(" ") + f.name + "=" + WorldRenderValue(o, f);
        RT_INFO("overgrowth %08X:%s", o, all.c_str());
      }
      for (size_t p = 0; p < settings.size();) {
        size_t comma = settings.find(',', p);
        if (comma == std::string::npos) comma = settings.size();
        std::string item = settings.substr(p, comma - p);
        p = comma + 1;
        size_t eq = item.find('=');
        if (item.empty()) continue;
        const WorldRenderField* f = eq == std::string::npos ? nullptr : FindField(kOvergrowthFields, item.substr(0, eq));
        if (f) SetWorldRenderValue(o, *f, item.substr(eq + 1));
        else if (pass == 0) RT_WARN("overgrowth: unknown setting %s", item.c_str());
      }
    }
  }
  if (pass == 0 || pass == 29) RT_INFO("overgrowth: %d copies set", found);
}
void rt::ApplyShadowSettings() {
  std::thread([] {
    const std::string& settings = g_worldrender;
    for (int pass = 0; pass < 30 && !g_in_menus; ++pass) {
      std::this_thread::sleep_for(std::chrono::seconds(2));
      int found = 0;
      for (uint32_t page = 0x40000000; page < 0xC0000000; page += 0x1000) {  // heaps, image, physical
        if (!IsCommitted(page)) continue;
        for (uint32_t o = page; o + 0xE0 <= page + 0x1000; o += 4) {
          if (Ld32(o) != 0x820632A0) continue;
          uint32_t w = Ld32(o + 0xA8), slices = Ld32(o + 0x84);
          if (w < 256 || w > 4096 || slices < 1 || slices > 8) continue;
          ++found;
          if (pass == 0 || pass == 29) {
            std::string all;
            for (const auto& f : kWorldRenderFields) all += std::string(" ") + f.name + "=" + WorldRenderValue(o, f);
            RT_INFO("worldrender %08X:%s", o, all.c_str());
          }
          for (size_t p = 0; p < settings.size();) {
            size_t comma = settings.find(',', p);
            if (comma == std::string::npos) comma = settings.size();
            std::string item = settings.substr(p, comma - p);
            p = comma + 1;
            size_t eq = item.find('=');
            const WorldRenderField* f = eq == std::string::npos ? nullptr : FindWorldRenderField(item.substr(0, eq));
            if (item.empty()) continue;
            if (f) SetWorldRenderValue(o, *f, item.substr(eq + 1));
            else if (pass == 0) RT_WARN("worldrender: unknown setting %s", item.c_str());
          }
          if (g_shadow_size) St32(o + 0xA8, g_shadow_size), St32(o + 0x78, g_shadow_size);
          if (g_shadow_distance > 0) {
            // Bad Company's shadow budgets overflow past 200 m (its own map is
            // about 100 m): further than that and the shadows drop out.
            float distance = std::min(g_shadow_distance, 200.0f);
            uint32_t d;
            memcpy(&d, &distance, 4);
            St32(o + 0xAC, d);
          }
        }
      }
      if (pass == 0 || pass == 29) RT_INFO("shadows: %d WorldRender settings copies set", found);
      ApplyOvergrowth(pass);
      ApplyEmitter(pass);
      ApplyPostProcess(pass);
    }
  }).detach();
}

// The local player's vehicle (bf1943's WatchVehicle, bots.cpp), for the
// vehicle keys (xam.cpp: W/S throttle and brake, A/D steer; arrows fly). Ours
// is the client soldier (vtable 82008D48, its feet at +370) nearest the
// camera's eye; its player is at +1D0, the entity it controls at player +114
// (a ClientVehicleEntity, vtable 820082E8, when in one) and its seat at player
// +118 (0 the driver's). The vehicle's data +14, its name at data +C: AH60
// flies; Howitzer and KORD are guns; a passenger's seat is a gunner's too.
// Read with SafeRead (bc_compat.h): a level can be unloaded under this thread.
namespace {
std::atomic<int> g_local_vehicle{rt::kOnFoot};
std::atomic<int64_t> g_vehicle_asked{0};
bool SafeLd32(uint32_t a, uint32_t& v) {
  uint32_t raw;
  if (!SafeRead(&raw, g_base + a, 4)) return false;
  v = _byteswap_ulong(raw);
  return true;
}
bool SafeLdF(uint32_t a, float& f) {
  uint32_t u;
  if (!SafeLd32(a, u)) return false;
  memcpy(&f, &u, 4);
  return true;
}
std::string SafeString(uint32_t a) {
  char buf[33] = {};
  size_t got = 0;
  while (got < 32 && SafeRead(buf + got, g_base + a + got, 1)) ++got;
  return std::string(buf, strnlen(buf, got));
}
std::vector<uint32_t> SafeScan(uint32_t vtable) {
  std::vector<uint32_t> out;
  uint32_t want = _byteswap_ulong(vtable);
  static uint32_t page_words[1024];
  for (uint64_t page = 0x40000000; page < 0xC0000000; page += 0x1000) {
    if (page >= 0x80000000 && page < 0xA0000000) continue;
    if (!IsCommitted(uint32_t(page))) continue;
    if (!SafeRead(page_words, g_base + page, 0x1000)) continue;
    for (int i = 0; i < 1024; ++i)
      if (page_words[i] == want) out.push_back(uint32_t(page) + i * 4);
  }
  return out;
}
int VehicleClass(const std::string& kind, uint32_t seat) {
  if (kind.find("AH60") != std::string::npos) return seat == 0 ? rt::kFlying : rt::kGunner;
  if (kind.rfind("KORD", 0) == 0 || kind.rfind("Howitzer", 0) == 0 || seat != 0) return rt::kGunner;
  return rt::kDriving;
}
void WatchVehicle() {
  constexpr uint32_t kClientSoldier = 0x82008D48, kClientVehicle = 0x820082E8;
  std::vector<uint32_t> soldiers;
  auto last_scan = std::chrono::steady_clock::time_point{};
  std::string last_kind;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    auto now = std::chrono::steady_clock::now();
    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    float eye[3];
    // No camera for a moment (some frames have none) keeps what was seen;
    // for over a second (or no one asking: out of a level), on foot.
    static int64_t eye_seen = 0;
    bool eye_ok = xgpu::CameraEye(eye);
    if (eye_ok) eye_seen = now_ms;
    if (now_ms - g_vehicle_asked.load() > 1000 || now_ms - eye_seen > 1000) {
      g_local_vehicle = rt::kOnFoot;
      soldiers.clear();
      continue;
    }
    if (!eye_ok) continue;
    bool stale = false;
    for (uint32_t c : soldiers) {
      uint32_t vt = 0;
      if (!SafeLd32(c, vt) || vt != kClientSoldier) stale = true;
    }
    auto since = now - last_scan;
    if ((stale && since > std::chrono::seconds(1)) || since > std::chrono::seconds(3) ||
        (soldiers.empty() && since > std::chrono::seconds(1))) {
      soldiers = SafeScan(kClientSoldier);
      last_scan = now;
    }
    // The one seen last stays ours while within 15 m of the eye (a vehicle's
    // camera sits behind it, where another soldier can be nearer).
    static uint32_t last_mine = 0;
    uint32_t mine = 0;
    float best = 60.0f;
    for (uint32_t c : soldiers) {
      float x, y, z;
      if (!SafeLdF(c + 0x370, x) || !SafeLdF(c + 0x374, y) || !SafeLdF(c + 0x378, z)) continue;
      float d = std::sqrt((x - eye[0]) * (x - eye[0]) + (y - eye[1]) * (y - eye[1]) + (z - eye[2]) * (z - eye[2]));
      if (c == last_mine && d < 15.0f) d = 0.0f;
      if (d < best) best = d, mine = c;
    }
    last_mine = mine;
    int kind_class = rt::kOnFoot;
    std::string kind;
    uint32_t player = 0, v = 0, vt = 0, seat = 0, data = 0, name = 0;
    if (mine && SafeLd32(mine + 0x1D0, player) && player && SafeLd32(player + 0x114, v) && v && SafeLd32(v, vt) &&
        vt == kClientVehicle) {
      SafeLd32(player + 0x118, seat);
      if (SafeLd32(v + 0x14, data) && data && SafeLd32(data + 0xC, name) && name) kind = SafeString(name);
      kind_class = VehicleClass(kind, seat);
    }
    std::string label = kind.empty() ? "" : kind + " seat " + std::to_string(seat);
    if (label != last_kind) {
      RT_INFO("vehicle keys: %s", label.empty() ? "on foot" : label.c_str());
      last_kind = label;
    }
    // Steady for 0.45 s before it counts (a frame's camera can be elsewhere).
    static int pending = -1, seen = 0;
    if (kind_class != pending) pending = kind_class, seen = 0;
    if (++seen >= 3) g_local_vehicle = kind_class;
  }
}
}  // namespace
int rt::LocalVehicle() {
  static const bool off = getenv("BF_NO_VEHICLE_KEYS") != nullptr;
  if (off) return kOnFoot;
  static std::once_flag started;
  std::call_once(started, [] { std::thread(WatchVehicle).detach(); });
  g_vehicle_asked = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
  return g_local_vehicle.load(std::memory_order_relaxed);
}
// The camera view in a vehicle: the right stick click in the tank, car, boat
// and gunner layouts (objects/vehicles/common/input/*vehicleinput.dbx; the
// artillery has none).
uint16_t rt::VehicleCameraButton() { return XINPUT_GAMEPAD_RIGHT_THUMB; }
