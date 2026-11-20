// Controllers (pad.h): XInput for Xbox pads on Windows, SDL3's gamepad API for
// every other pad there and for all pads on Linux and macOS.
#include "kernel.h"  // (before <windows.h>: its macros)
#include "pad.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace rt::pad {

const wchar_t* ButtonLabel(Kind kind, const char* b) {
  struct Entry { const char* name; const wchar_t *xbox, *ps3, *ps5, *deck; };
  static const Entry kEntries[] = {
      {"a", L"A", L"X", L"X", L"A"},          {"b", L"B", L"O", L"O", L"B"},
      {"x", L"X", L"[]", L"[]", L"X"},        {"y", L"Y", L"/\\", L"/\\", L"Y"},
      {"lb", L"LB", L"L1", L"L1", L"L1"},     {"rb", L"RB", L"R1", L"R1", L"R1"},
      {"lt", L"LT", L"L2", L"L2", L"L2"},     {"rt", L"RT", L"R2", L"R2", L"R2"},
      {"start", L"START", L"START", L"OPTIONS", L"MENU"}, {"back", L"BACK", L"SELECT", L"CREATE", L"VIEW"}};
  for (const Entry& e : kEntries) {
    if (std::strcmp(e.name, b)) continue;
    return kind == Kind::kPs5 ? e.ps5 : kind == Kind::kPs3 ? e.ps3 : kind == Kind::kSteamDeck ? e.deck : e.xbox;
  }
  return L"";
}

Kind PromptKind(int style) {
  switch (style) {
    case 2: return Kind::kXbox360;
    case 3: return Kind::kXboxSeries;
    case 4: return Kind::kPs3;
    case 5: return Kind::kPs5;
    case 6: return Kind::kSteamDeck;
    default: break;
  }
  Kind k = Kind::kNone;
  if (style != 1) k = UsedKind() != Kind::kNone ? UsedKind() : ActiveKind();
  return k == Kind::kNone || k == Kind::kGeneric ? Kind::kXbox360 : k;
}

// ---- the device last used (BUTTON PROMPTS = AUTO) -------------------------
// Only real use counts: a press (a button, a trigger pulled past a quarter,
// a stick pushed past a dead zone) that was not held at the last read, so a
// ghost or virtual pad (Steam Input, ViGEm leftovers) that is connected but
// never pressed, or one that reports a stuck value, never takes the prompts.
namespace {
std::atomic<int> g_used{-1};  // -1 the keyboard and mouse (also before any input), else a Kind
std::mutex g_used_mutex;
struct Held {
  bool seen = false;
  uint32_t bits = 0;
} g_held[4];

// The pad's inputs as press bits: the buttons, the triggers, the sticks' four directions.
uint32_t PressBits(const XINPUT_GAMEPAD& g) {
  constexpr int kTrigger = 64, kStick = 12000;  // (XInput's own dead zones are 7849 and 8689)
  uint32_t bits = g.wButtons;
  if (g.bLeftTrigger > kTrigger) bits |= 1u << 16;
  if (g.bRightTrigger > kTrigger) bits |= 1u << 17;
  const SHORT axes[4] = {g.sThumbLX, g.sThumbLY, g.sThumbRX, g.sThumbRY};
  for (int i = 0; i < 4; ++i) {
    if (axes[i] > kStick) bits |= 1u << (18 + 2 * i);
    if (axes[i] < -kStick) bits |= 1u << (19 + 2 * i);
  }
  return bits;
}

void SetUsed(int used) {
  if (g_used.exchange(used) == used) return;
  RT_INFO("input: %s used (button prompts AUTO follow it)", used < 0 ? "keyboard and mouse" : "controller");
}
}  // namespace

void NotePad(DWORD user, const XINPUT_GAMEPAD& g, Kind kind) {
  if (user >= 4) return;
  const uint32_t bits = PressBits(g);
  bool pressed;
  {
    std::lock_guard l(g_used_mutex);
    Held& h = g_held[user];
    pressed = h.seen && (bits & ~h.bits) != 0;  // (the first read only records what is held)
    h.seen = true, h.bits = bits;
  }
  if (pressed) SetUsed(int(kind == Kind::kNone ? Kind::kGeneric : kind));
}
void NoteKeyboardMouse() { SetUsed(-1); }
Kind UsedKind() {
  const int used = g_used.load();
  return used < 0 ? Kind::kNone : Kind(used);
}

}  // namespace rt::pad

#if defined(BF1943_CLIENT) || defined(BF_SDL_PADS)

#include <SDL3/SDL.h>

#include <thread>

namespace rt::pad {
namespace {

struct Pad {
  SDL_Gamepad* handle = nullptr;
  SDL_JoystickID id = 0;
  Kind kind = Kind::kGeneric;
  std::string name;
  XINPUT_GAMEPAD last{};
  DWORD packet = 1;
  uint16_t rumble_low = 0, rumble_high = 0;
};

std::mutex g_mutex;
std::vector<Pad> g_pads;  // SDL's, in the order they came (at most four)
bool g_xinput[4] = {};     // Windows: which users XInput serves (as last polled)
DWORD g_xinput_packet[4] = {};
Kind g_last_kind = Kind::kNone;
SDL_JoystickID g_last_id = 0;  // 0: an XInput pad

Kind KindOf(SDL_Gamepad* pad, SDL_GamepadType type) {
  const char* name = pad ? SDL_GetGamepadName(pad) : nullptr;
  const Uint16 vendor = pad ? SDL_GetGamepadVendor(pad) : 0, product = pad ? SDL_GetGamepadProduct(pad) : 0;
  if ((name && std::strstr(name, "Steam Deck")) || (vendor == 0x28DE && product == 0x1205)) return Kind::kSteamDeck;
  switch (type) {
    case SDL_GAMEPAD_TYPE_PS3: return Kind::kPs3;
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5: return Kind::kPs5;
    case SDL_GAMEPAD_TYPE_XBOX360: return Kind::kXbox360;
    case SDL_GAMEPAD_TYPE_XBOXONE: return Kind::kXboxSeries;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return Kind::kSwitch;
    default: return Kind::kGeneric;
  }
}

void Added(SDL_JoystickID id) {
#ifdef _WIN32
  // Xbox pads are XInput's: not a second time.
  SDL_GamepadType t = SDL_GetGamepadTypeForID(id);
  if (t == SDL_GAMEPAD_TYPE_XBOX360 || t == SDL_GAMEPAD_TYPE_XBOXONE) return;
#endif
  std::lock_guard l(g_mutex);
  for (const Pad& p : g_pads)
    if (p.id == id) return;
  if (g_pads.size() >= 4) return;
  SDL_Gamepad* handle = SDL_OpenGamepad(id);
  if (!handle) return;
  Pad p;
  p.handle = handle, p.id = id;
  p.kind = KindOf(handle, SDL_GetGamepadType(handle));
  const char* name = SDL_GetGamepadName(handle);
  p.name = name ? name : "controller";
  RT_INFO("input: controller %s (%s)", p.name.c_str(),
          p.kind == Kind::kPs3 ? "PlayStation 3 prompts" : p.kind == Kind::kPs5 ? "PlayStation prompts"
          : p.kind == Kind::kSteamDeck ? "Steam Deck prompts" : p.kind == Kind::kSwitch ? "Switch pad"
          : p.kind == Kind::kXbox360 || p.kind == Kind::kXboxSeries ? "Xbox prompts" : "generic pad");
  g_pads.push_back(std::move(p));
}

void Removed(SDL_JoystickID id) {
  std::lock_guard l(g_mutex);
  for (size_t i = 0; i < g_pads.size(); ++i) {
    if (g_pads[i].id != id) continue;
    RT_INFO("input: controller %s removed", g_pads[i].name.c_str());
    SDL_CloseGamepad(g_pads[i].handle);
    g_pads.erase(g_pads.begin() + long(i));
    break;
  }
}

// The SDL pad that is this user's: the users XInput does not serve, in order.
Pad* SdlPadFor(DWORD user) {
  size_t index = 0;
  for (DWORD u = 0; u < user && u < 4; ++u)
    if (!g_xinput[u]) ++index;
  return index < g_pads.size() ? &g_pads[index] : nullptr;
}

void Read(Pad& pad, XINPUT_GAMEPAD& g) {
  g = {};
  struct Map { SDL_GamepadButton b; WORD w; };
  static const Map kButtons[] = {
      {SDL_GAMEPAD_BUTTON_SOUTH, XINPUT_GAMEPAD_A},         {SDL_GAMEPAD_BUTTON_EAST, XINPUT_GAMEPAD_B},
      {SDL_GAMEPAD_BUTTON_WEST, XINPUT_GAMEPAD_X},          {SDL_GAMEPAD_BUTTON_NORTH, XINPUT_GAMEPAD_Y},
      {SDL_GAMEPAD_BUTTON_BACK, XINPUT_GAMEPAD_BACK},       {SDL_GAMEPAD_BUTTON_START, XINPUT_GAMEPAD_START},
      {SDL_GAMEPAD_BUTTON_LEFT_STICK, XINPUT_GAMEPAD_LEFT_THUMB},
      {SDL_GAMEPAD_BUTTON_RIGHT_STICK, XINPUT_GAMEPAD_RIGHT_THUMB},
      {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, XINPUT_GAMEPAD_LEFT_SHOULDER},
      {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER},
      {SDL_GAMEPAD_BUTTON_DPAD_UP, XINPUT_GAMEPAD_DPAD_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_DOWN},
      {SDL_GAMEPAD_BUTTON_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_LEFT},
      {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT}};
  for (const Map& m : kButtons)
    if (SDL_GetGamepadButton(pad.handle, m.b)) g.wButtons |= m.w;
  auto axis = [&](SDL_GamepadAxis a) { return int(SDL_GetGamepadAxis(pad.handle, a)); };
  // SDL: down is positive; XInput's Y is up. Sticks inside a small dead
  // zone read centred (worn or noisy sticks); the game applies its own
  // dead zone on top, as for an Xbox pad.
  auto stick = [](int x, int y, SHORT& ox, SHORT& oy) {
    constexpr int kInner = 3000;
    if (long(x) * x + long(y) * y < long(kInner) * kInner) x = y = 0;
    ox = SHORT(std::clamp(x, -32768, 32767));
    oy = SHORT(std::clamp(-y, -32768, 32767));
  };
  stick(axis(SDL_GAMEPAD_AXIS_LEFTX), axis(SDL_GAMEPAD_AXIS_LEFTY), g.sThumbLX, g.sThumbLY);
  stick(axis(SDL_GAMEPAD_AXIS_RIGHTX), axis(SDL_GAMEPAD_AXIS_RIGHTY), g.sThumbRX, g.sThumbRY);
  g.bLeftTrigger = BYTE(std::max(0, axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER)) * 255 / 32767);
  g.bRightTrigger = BYTE(std::max(0, axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) * 255 / 32767);
}

#ifdef _WIN32
std::atomic<bool> g_started{false}, g_ready{false};
void Pump() {
  // No window of ours for SDL: pads count in the background too (the game
  // decides what is in front), and none of SDL's process-wide extras.
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
  SDL_SetHint(SDL_HINT_TIMER_RESOLUTION, "0");
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX, "0");  // (XInput has those)
  if (!SDL_Init(SDL_INIT_GAMEPAD)) {
    RT_INFO("input: SDL gamepads unavailable (%s): Xbox pads only", SDL_GetError());
    g_ready = true;
    return;
  }
  for (bool first = true;; first = false) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) OnSdlEvent(e);
    if (first) g_ready = true;
    SDL_Delay(8);
  }
}
#endif

}  // namespace

void OnSdlEvent(const SDL_Event& e) {
  if (e.type == SDL_EVENT_GAMEPAD_ADDED) Added(e.gdevice.which);
  else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) Removed(e.gdevice.which);
}

void Init() {
#ifdef _WIN32
  if (g_started.exchange(true)) return;
  std::thread(Pump).detach();
  // The pads plugged in at start are known before the first read (the prompts' AUTO).
  for (int i = 0; i < 100 && !g_ready; ++i) SDL_Delay(5);
#endif
}

DWORD GetState(DWORD user, XINPUT_STATE* state) {
  if (user >= 4) return 1167;
  Init();
#ifdef _WIN32
  {
    DWORD r = ::XInputGetState(user, state);
    std::lock_guard l(g_mutex);
    g_xinput[user] = r == ERROR_SUCCESS;
    if (r == ERROR_SUCCESS) {
      if (state->dwPacketNumber != g_xinput_packet[user]) g_xinput_packet[user] = state->dwPacketNumber, g_last_kind = Kind::kXbox360, g_last_id = 0;
      NotePad(user, state->Gamepad, Kind::kXbox360);
      return r;
    }
    g_xinput_packet[user] = 0;
  }
#endif
  std::lock_guard l(g_mutex);
  Pad* pad = SdlPadFor(user);
  if (!pad) return 1167;  // ERROR_DEVICE_NOT_CONNECTED
  *state = {};
  Read(*pad, state->Gamepad);
  // A new number only when something changed (as XInput: the game takes a
  // new one with a button held as another press).
  if (std::memcmp(&pad->last, &state->Gamepad, sizeof(XINPUT_GAMEPAD)) != 0) {
    pad->last = state->Gamepad, ++pad->packet;
    g_last_kind = pad->kind, g_last_id = pad->id;
  }
  state->dwPacketNumber = pad->packet;
  NotePad(user, state->Gamepad, pad->kind);
  return 0;
}

DWORD SetState(DWORD user, XINPUT_VIBRATION* v) {
  if (user >= 4) return 1167;
#ifdef _WIN32
  bool xi;
  {
    std::lock_guard l(g_mutex);
    xi = g_xinput[user];
  }
  if (xi) return ::XInputSetState(user, v);
#endif
  std::lock_guard l(g_mutex);
  Pad* pad = SdlPadFor(user);
  if (!pad) return 1167;
  if (pad->rumble_low != v->wLeftMotorSpeed || pad->rumble_high != v->wRightMotorSpeed) {
    pad->rumble_low = v->wLeftMotorSpeed, pad->rumble_high = v->wRightMotorSpeed;
    SDL_RumbleGamepad(pad->handle, pad->rumble_low, pad->rumble_high, SDL_MAX_UINT32);  // (until the game changes it)
  }
  return 0;
}

DWORD GetCapabilities(DWORD user, DWORD flags, XINPUT_CAPABILITIES* caps) {
  if (user >= 4) return 1167;
  Init();
#ifdef _WIN32
  if (DWORD r = ::XInputGetCapabilities(user, flags, caps); r == ERROR_SUCCESS) return r;
#endif
  (void)flags;
  std::lock_guard l(g_mutex);
  if (!SdlPadFor(user)) return 1167;
  *caps = {};
  caps->Type = 1, caps->SubType = 1;  // a gamepad
  return 0;
}

Kind ActiveKind() {
  Init();
  std::lock_guard l(g_mutex);
  bool xinput = false;
  for (bool x : g_xinput) xinput |= x;
  if (!xinput && g_pads.empty()) return Kind::kNone;
  if (g_last_kind != Kind::kNone) {
    bool alive = g_last_id == 0 ? xinput : false;
    for (const Pad& p : g_pads) alive |= p.id == g_last_id;
    if (alive) return g_last_kind;
  }
  return g_pads.empty() ? Kind::kXbox360 : g_pads[0].kind;
}

const char* ActiveName() {
  std::lock_guard l(g_mutex);
  for (const Pad& p : g_pads)
    if (p.id == g_last_id) return p.name.c_str();
  return g_pads.empty() ? "Xbox controller" : g_pads[0].name.c_str();
}

}  // namespace rt::pad

#else  // no SDL: XInput alone on Windows, no pads on the Linux server

namespace rt::pad {

void Init() {}
DWORD GetState(DWORD user, XINPUT_STATE* state) {
#ifdef _WIN32
  DWORD r = ::XInputGetState(user, state);
  if (r == ERROR_SUCCESS) NotePad(user, state->Gamepad, Kind::kXbox360);
  return r;
#else
  (void)user, (void)state;
  return 1167;
#endif
}
DWORD SetState(DWORD user, XINPUT_VIBRATION* v) {
#ifdef _WIN32
  return ::XInputSetState(user, v);
#else
  (void)user, (void)v;
  return 1167;
#endif
}
DWORD GetCapabilities(DWORD user, DWORD flags, XINPUT_CAPABILITIES* caps) {
#ifdef _WIN32
  return ::XInputGetCapabilities(user, flags, caps);
#else
  (void)user, (void)flags, (void)caps;
  return 1167;
#endif
}
Kind ActiveKind() {
#ifdef _WIN32
  XINPUT_STATE s;
  for (DWORD i = 0; i < 4; ++i)
    if (::XInputGetState(i, &s) == ERROR_SUCCESS) return Kind::kXbox360;
#endif
  return Kind::kNone;
}
const char* ActiveName() { return "Xbox controller"; }

}  // namespace rt::pad

#endif
