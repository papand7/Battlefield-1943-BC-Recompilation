// FREE CAMERA (free_camera.h).
//   F3: on / off.  Mouse: look.  W A S D: fly (where the camera looks).
//   Space / E: up.  Ctrl / C / Q: down.  Shift: 4x faster.  Alt: 1/4 speed.
//   Mouse wheel: the base speed (2...200 m/s).
#include "free_camera.h"

#include "kernel.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include "posix_input.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace rt::freecam {
namespace {
constexpr float kPi = 3.14159265358979f;

std::mutex g_mutex;
std::atomic<bool> g_active{false};
bool g_placed = false;            // the camera has a place (from the game's or PlaceAt)
float g_pos[3] = {};
float g_yaw = 0, g_pitch = 0;     // forward = (sin yaw cos pitch, sin pitch, cos yaw cos pitch)
// The game's first matrix row is the view's RIGHT: minus our left. (A +1 here, as it was
// for a camera placed before the game's own was seen, mirrors the view: the
// ground and sea, one-sided, are culled and only the trees and props show.)
float g_left_sign = -1.0f;
float g_speed = 12.0f;            // m/s
float g_fov = 0.96f;              // vertical radians, the last handed to the view
long g_mouse_dx = 0, g_mouse_dy = 0;
std::chrono::steady_clock::time_point g_last_step{};
// Keys as polled (xam.cpp's thread), applied by FrameStep (the game's).
struct Keys {
  bool w, a, s, d, up, down, fast, slow;
} g_keys{};

bool Focused() {
#ifdef _WIN32
  HWND w = GetForegroundWindow();
  DWORD pid = 0;
  GetWindowThreadProcessId(w, &pid);
  return pid == GetCurrentProcessId();
#else
  return true;
#endif
}
bool Held(int vk) {
#ifdef _WIN32
  if (Focused() && (GetAsyncKeyState(vk) & 0x8000)) return true;
#endif
  return rt::SimulatedKeyDown(vk);
}

void Forward(float yaw, float pitch, float f[3]) {
  f[0] = std::sin(yaw) * std::cos(pitch);
  f[1] = std::sin(pitch);
  f[2] = std::cos(yaw) * std::cos(pitch);
}
// left = up x forward (the world's y up), as the game's rows.
void Basis(float yaw, float pitch, float f[3], float l[3], float u[3]) {
  Forward(yaw, pitch, f);
  l[0] = std::cos(yaw), l[1] = 0, l[2] = -std::sin(yaw);  // normalize((0,1,0) x f)
  u[0] = f[1] * l[2] - f[2] * l[1];                      // forward x left
  u[1] = f[2] * l[0] - f[0] * l[2];
  u[2] = f[0] * l[1] - f[1] * l[0];
}
}  // namespace

bool Active() { return g_active.load(std::memory_order_relaxed); }

void SetActive(bool on) {
  std::lock_guard l(g_mutex);
  if (on == g_active) return;
  g_active = on;
  if (!on) g_placed = false;  // the next time: from wherever the game's camera is then
  g_mouse_dx = g_mouse_dy = 0;
  g_last_step = {};
  RT_INFO("free camera: %s", on ? "on (F3 gives the view back)" : "off");
}
void Toggle() { SetActive(!Active()); }

void PollKeys(bool menus) {
  static bool f3_was = false;
  bool f3 = Held(VK_F3);
  if (f3 && !f3_was) Toggle();
  f3_was = f3;
  Keys k{};
  if (Active() && !menus) {
    k.w = Held('W'), k.a = Held('A'), k.s = Held('S'), k.d = Held('D');
    k.up = Held(VK_SPACE) || Held('E');
    k.down = Held(VK_CONTROL) || Held('C') || Held('Q');
    k.fast = Held(VK_SHIFT);
    k.slow = Held(VK_MENU);
  }
  std::lock_guard l(g_mutex);
  g_keys = k;
}

void AddMouse(long dx, long dy, long wheel) {
  static const bool trace = std::getenv("BF_FREECAM_TRACE") != nullptr;
  if (trace && (dx || dy)) RT_INFO("free camera: mouse %ld %ld", dx, dy);
  std::lock_guard l(g_mutex);
  g_mouse_dx += dx, g_mouse_dy += dy;
  if (wheel) g_speed = std::clamp(g_speed * (wheel > 0 ? 1.25f : 0.8f), 2.0f, 200.0f);
}

void FrameStep() {
  if (!Active()) return;
  std::lock_guard l(g_mutex);
  auto now = std::chrono::steady_clock::now();
  float dt = g_last_step == std::chrono::steady_clock::time_point{}
                 ? 0.0f
                 : float(std::chrono::duration<double>(now - g_last_step).count());
  g_last_step = now;
  dt = std::clamp(dt, 0.0f, 0.1f);
  if (!g_placed) return;  // (placed by the first Override)
  // Look: 0.0022 radians a count at sensitivity 1 (about the game's own feel).
  const float k = 0.0022f * (g_mouse_sensitivity > 0 ? g_mouse_sensitivity : 1.0f);
  g_yaw -= float(g_mouse_dx) * k;  // right lowers yaw
  g_pitch += float(g_mouse_invert ? g_mouse_dy : -g_mouse_dy) * k;
  g_mouse_dx = g_mouse_dy = 0;
  g_pitch = std::clamp(g_pitch, -1.55f, 1.55f);
  if (g_yaw > kPi) g_yaw -= 2 * kPi;
  if (g_yaw < -kPi) g_yaw += 2 * kPi;
  float f[3], lft[3], u[3];
  Basis(g_yaw, g_pitch, f, lft, u);
  float move[3] = {};
  auto add = [&](const float* v, float s) {
    for (int i = 0; i < 3; ++i) move[i] += v[i] * s;
  };
  if (g_keys.w) add(f, 1);
  if (g_keys.s) add(f, -1);
  if (g_keys.a) add(lft, 1);
  if (g_keys.d) add(lft, -1);
  const float world_up[3] = {0, 1, 0};
  if (g_keys.up) add(world_up, 1);
  if (g_keys.down) add(world_up, -1);
  float len = std::sqrt(move[0] * move[0] + move[1] * move[1] + move[2] * move[2]);
  if (len > 0) {
    float speed = g_speed * (g_keys.fast ? 4.0f : 1.0f) * (g_keys.slow ? 0.25f : 1.0f);
    for (int i = 0; i < 3; ++i) g_pos[i] += move[i] / len * speed * dt;
  }
}

bool Override(const float game[16], float out[16]) {
  if (!Active()) return false;
  std::lock_guard l(g_mutex);
  if (!g_placed) {
    // Start from the game's camera. It looks along minus the third row (seen:
    // mouse down with +row 3 as the look pitched the view up, and W moved it
    // backward), so the rows are right, up, back.
    const float f[3] = {-game[8], -game[9], -game[10]};
    float n = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (!(n > 0.5f && n < 2.0f) || !std::isfinite(game[12])) return false;
    g_pos[0] = game[12], g_pos[1] = game[13], g_pos[2] = game[14];
    g_yaw = std::atan2(f[0] / n, f[2] / n);
    g_pitch = std::asin(std::clamp(f[1] / n, -1.0f, 1.0f));
    float fw[3], lft[3], u[3];
    Basis(g_yaw, g_pitch, fw, lft, u);
    g_left_sign = game[0] * lft[0] + game[1] * lft[1] + game[2] * lft[2] < 0 ? -1.0f : 1.0f;  // (always -1: checked)
    g_placed = true;
    // BF_FREECAM_AT="x,y,z,yaw,pitch" (testing): start there instead.
    if (const char* at = std::getenv("BF_FREECAM_AT")) {
      float v[5] = {};
      if (std::sscanf(at, "%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4]) == 5)
        g_pos[0] = v[0], g_pos[1] = v[1], g_pos[2] = v[2], g_yaw = v[3], g_pitch = v[4];
    }
    RT_INFO("free camera: from %.1f, %.1f, %.1f (yaw %.2f, pitch %.2f; rows: right %.2f %.2f %.2f, up %.2f %.2f %.2f, "
            "back %.2f %.2f %.2f; first row %s)",
            g_pos[0], g_pos[1], g_pos[2], g_yaw, g_pitch, game[0], game[1], game[2], game[4], game[5], game[6], game[8],
            game[9], game[10], g_left_sign > 0 ? "left (unexpected)" : "right");
  }
  float f[3], lft[3], u[3];
  Basis(g_yaw, g_pitch, f, lft, u);
  static const bool log = std::getenv("BF_TERRAIN_LOG") != nullptr;
  if (log) {
    static auto next = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (now >= next)
      next = now + std::chrono::seconds(3),
      RT_INFO("free camera: the game's own camera is at %.1f %.1f %.1f, ours %.1f %.1f %.1f", game[12], game[13],
              game[14], g_pos[0], g_pos[1], g_pos[2]);
  }
  for (int i = 0; i < 3; ++i) {
    out[i] = lft[i] * g_left_sign;
    out[4 + i] = u[i];
    out[8 + i] = -f[i];
    out[12 + i] = g_pos[i];
  }
  out[3] = game[3], out[7] = game[7], out[11] = game[11], out[15] = game[15];
  return true;
}

void NoteFov(float vertical_radians) {
  if (vertical_radians > 0.05f && vertical_radians < 3.0f) g_fov = vertical_radians;
}

bool CurrentView(View& v) {
  if (!Active()) return false;
  std::lock_guard l(g_mutex);
  if (!g_placed) return false;
  Basis(g_yaw, g_pitch, v.forward, v.left, v.up);
  for (int i = 0; i < 3; ++i) v.eye[i] = g_pos[i];
  v.fov_y = g_fov;
  return true;
}

bool KeyHeld(int vk) { return Held(vk); }

void PlaceAt(const float eye[3], float yaw, float pitch) {
  std::lock_guard l(g_mutex);
  for (int i = 0; i < 3; ++i) g_pos[i] = eye[i];
  g_yaw = yaw, g_pitch = std::clamp(pitch, -1.55f, 1.55f);
  g_placed = true;
}
}  // namespace rt::freecam
