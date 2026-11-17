// LIVE SPECTATE MODE (spectate.h).
//
// A thread of the host process: every 100 ms it reads the server's soldiers
// and vehicles from guest memory (the layout the match recorder, replay.cpp,
// reads: a player's spawner 8201A990 holds its soldier now at +120; a soldier's
// position at +180, health +3C, team +48, its aim object at +130 with yaw / pitch
// at +134 / +138; a vehicle's body transform) and keeps the last three samples
// of each player. The camera (free_camera.h's driver hook, once per game
// frame) draws the followed player between two samples, 150 ms in the past, so
// that it moves smoothly instead of ten times a second.
#include "spectate.h"

#include "bot_players.h"
#include "admin.h"
#include "free_camera.h"
#include "guest.h"
#include "kernel.h"
#include "pc_settings.h"
#include "xenia_bridge.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rt::spectate {
bool g_flag = false;
bool g_on_death = true;

#ifndef _WIN32
void Start() {}
bool Active() { return false; }
bool DeathWatch() { return false; }
bool MenuValue(bool show) { return show; }
void Killed(uint32_t, uint32_t) {}
void FreeKey() {}
#else
namespace {
constexpr uint32_t kSpawnObject = 0x8201A990;  // a player's soldier spawner
constexpr uint32_t kSoldier = 0x82013A40;      // ServerSoldierEntity
constexpr uint32_t kVehicle = 0x82013068;      // ServerVehicleEntity
constexpr float kPi = 3.14159265358979f;
constexpr double kDelay = 0.15;  // s: the camera draws this far in the past

double Now() {
  static const auto t0 = std::chrono::steady_clock::now();
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}
bool Readable(uint32_t a) { return a >= 0x40000000 && a < 0xC0000000 && IsCommitted(a) && IsCommitted(a + 0x400); }
float LdF(uint32_t a) {
  uint32_t u = Ld32(a);
  float f;
  memcpy(&f, &u, 4);
  return f;
}
std::vector<uint32_t> Scan(uint32_t vtable) {
  std::vector<uint32_t> out;
  uint32_t want = __builtin_bswap32(vtable);
  for (uint64_t page = 0x40000000; page < 0xC0000000; page += 0x1000) {
    if (page >= 0x80000000 && page < 0xA0000000) continue;
    if (!IsCommitted(uint32_t(page))) continue;
    const uint32_t* p = reinterpret_cast<const uint32_t*>(g_base + page);
    for (int i = 0; i < 1024; ++i)
      if (p[i] == want) out.push_back(uint32_t(page) + i * 4);
  }
  return out;
}

struct Pose {
  float p[3] = {};
  float yaw = 0, pitch = 0;
  bool veh = false;
};
struct Player {
  std::string name;
  int team = 0;
  bool bot = false;
  int kills = 0, deaths = 0;
  float health = 0;       // 0..100
  Pose pose[3];           // the last samples, oldest first (n of them)
  double at[3] = {};
  int n = 0;
  uint32_t round = 0;     // the sampling round it was last alive in
  uint32_t soldier = 0;
};

std::mutex g_mu;
std::map<uint32_t, Player> g_players;               // by spawner
std::map<uint32_t, uint32_t> g_soldier_key;         // soldier now -> spawner
uint32_t g_round = 0;
int g_humans_named = 0;
struct KillNote {
  uint32_t victim = 0, killer = 0;  // spawners
  double t = -100;
} g_last_kill;

std::atomic<bool> g_user{false};       // spectating by choice
std::atomic<bool> g_death{false};      // following after a death
std::atomic<bool> g_pending{false};    // --spectate: from the level's first players
std::atomic<bool> g_free{false};       // the free camera instead of a player
std::atomic<float> g_dist{6.0f};       // chase distance (< 1: first person)
uint32_t g_follow = 0;                 // spawner followed (under g_mu)
std::atomic<bool> g_real_menu{false};  // the game's own in-level menu is up
std::atomic<bool> g_arm_death{false};
bool g_killer_follow = false;          // (the followed one is the killer: the overlay says so)

bool Alive(const Player& p) { return p.round == g_round && p.n > 0; }

void Lerp(const Pose& a, const Pose& b, double k, Pose& o) {
  o = b;
  float jump = std::hypot(b.p[0] - a.p[0], b.p[2] - a.p[2]);
  if (jump > 30.0f) return;  // (a respawn)
  for (int i = 0; i < 3; ++i) o.p[i] = float(a.p[i] + (b.p[i] - a.p[i]) * k);
  float d = b.yaw - a.yaw;
  while (d > kPi) d -= 2 * kPi;
  while (d < -kPi) d += 2 * kPi;
  o.yaw = float(a.yaw + d * k);
  o.pitch = float(a.pitch + (b.pitch - a.pitch) * k);
}
Pose PoseAt(const Player& p, double t) {
  if (p.n <= 1 || t <= p.at[0]) return p.pose[0];
  for (int i = 0; i + 1 < p.n; ++i)
    if (t <= p.at[i + 1]) {
      double k = p.at[i + 1] > p.at[i] ? (t - p.at[i]) / (p.at[i + 1] - p.at[i]) : 1.0;
      Pose o;
      Lerp(p.pose[i], p.pose[i + 1], std::clamp(k, 0.0, 1.0), o);
      return o;
    }
  return p.pose[p.n - 1];
}

// ---- reading the server -------------------------------------------------------

struct Veh {
  float p[3];
  float yaw, pitch;
};
std::vector<uint32_t> g_spawners, g_vehicles;

void ReadVehicles(std::vector<Veh>& out) {
  for (uint32_t o : g_vehicles) {
    if (!Readable(o) || Ld32(o) != kVehicle) continue;
    uint32_t data = Ld32(o + 0x14), body = Ld32(o + 0xF0);
    if (!Readable(data) || !Readable(body)) continue;
    Veh v{};
    v.p[0] = LdF(body + 0xF0), v.p[1] = LdF(body + 0xF4), v.p[2] = LdF(body + 0xF8);
    uint32_t rows = body + 0xC0;
    if (v.p[0] == 0 && v.p[1] == 0 && v.p[2] == 0) {
      uint32_t made = Ld32(o + 0xEC);
      if (Readable(made)) {
        v.p[0] = LdF(made + 0x310), v.p[1] = LdF(made + 0x314), v.p[2] = LdF(made + 0x318);
        rows = made + 0x2E0;
      }
    }
    if ((v.p[0] == 0 && v.p[2] == 0) || !std::isfinite(v.p[0]) || std::fabs(v.p[0]) > 1e5f) continue;
    float f[3] = {LdF(rows + 0x20), LdF(rows + 0x24), LdF(rows + 0x28)};
    float n = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (n > 0.9f && n < 1.1f) v.yaw = std::atan2(f[0], f[2]), v.pitch = std::asin(std::clamp(f[1] / n, -1.0f, 1.0f));
    out.push_back(v);
  }
}

void SampleServer(bool rescan) {
  if (rescan) g_spawners = Scan(kSpawnObject), g_vehicles = Scan(kVehicle);
  std::vector<uint32_t> bot_soldiers;
  for (int i = 0; i < BotPlayerCount(); ++i) bot_soldiers.push_back(BotPlayerSoldier(i));
  std::vector<Veh> vehicles;
  ReadVehicles(vehicles);
  const double t = Now();
  std::lock_guard l(g_mu);
  ++g_round;
  g_soldier_key.clear();
  for (uint32_t sp : g_spawners) {
    if (!Readable(sp) || Ld32(sp) != kSpawnObject) continue;
    uint32_t so = Ld32(sp + 0x120);
    if (!Readable(so) || Ld32(so) != kSoldier) continue;
    uint32_t aim = Ld32(so + 0x130);
    if (!Readable(aim)) continue;
    int team = int(Ld32(so + 0x48));
    Pose po;
    po.p[0] = LdF(so + 0x180), po.p[1] = LdF(so + 0x184), po.p[2] = LdF(so + 0x188);
    po.yaw = LdF(aim + 0x134), po.pitch = LdF(aim + 0x138);
    float hp = LdF(so + 0x3C);
    if (!std::isfinite(po.p[0]) || !std::isfinite(po.yaw) || !(hp > 0) || (team != 1 && team != 2)) continue;
    if (!std::isfinite(po.pitch)) po.pitch = 0;
    if ((Ld32(so + 0x34C) >> 24) == 0) {  // seated: the vehicle's place
      float best = 30.0f;
      for (const Veh& v : vehicles) {
        float d = std::sqrt((v.p[0] - po.p[0]) * (v.p[0] - po.p[0]) + (v.p[1] - po.p[1]) * (v.p[1] - po.p[1]) +
                            (v.p[2] - po.p[2]) * (v.p[2] - po.p[2]));
        if (d < best) {
          best = d;
          po.veh = true;
          for (int i = 0; i < 3; ++i) po.p[i] = v.p[i];
          po.yaw = v.yaw, po.pitch = v.pitch;
        }
      }
    }
    auto it = g_players.find(sp);
    if (it == g_players.end()) {
      Player p;
      p.team = team;
      auto bot = std::find(bot_soldiers.begin(), bot_soldiers.end(), so);
      p.bot = bot != bot_soldiers.end();
      char name[24];
      if (p.bot) snprintf(name, sizeof name, "Bot %d", int(bot - bot_soldiers.begin()) + 1);
      else snprintf(name, sizeof name, "Player %d", ++g_humans_named);
      p.name = name;
      it = g_players.emplace(sp, p).first;
      RT_INFO("spectate: %s (team %d, spawner %08X)", name, team, sp);
    }
    Player& p = it->second;
    if (auto bot = std::find(bot_soldiers.begin(), bot_soldiers.end(), so); bot != bot_soldiers.end() && so && !p.bot) {
      p.bot = true;  // (the bots' table knew its soldier only after the first sight)
      p.name = "Bot " + std::to_string(int(bot - bot_soldiers.begin()) + 1);
    }
    p.team = team, p.health = hp, p.soldier = so, p.round = g_round;
    // (a new life far from the last samples: they are of another soldier)
    if (p.n > 0 && std::hypot(po.p[0] - p.pose[p.n - 1].p[0], po.p[2] - p.pose[p.n - 1].p[2]) > 30.0f) p.n = 0;
    if (p.n == 3) {
      for (int i = 0; i < 2; ++i) p.pose[i] = p.pose[i + 1], p.at[i] = p.at[i + 1];
      p.n = 2;
    }
    p.pose[p.n] = po, p.at[p.n] = t, ++p.n;
    g_soldier_key[so] = sp;
  }
}

// ---- following ------------------------------------------------------------------

// The followed player alive: the next alive one in order when it isn't. under g_mu.
// dir 0: keep (or the first), +1 / -1: step.
bool Pick(int dir) {
  std::vector<uint32_t> alive;
  for (auto& [k, p] : g_players)
    if (Alive(p)) alive.push_back(k);
  if (alive.empty()) return false;
  auto it = std::find(alive.begin(), alive.end(), g_follow);
  if (it != alive.end() && dir == 0) return true;
  if (it == alive.end()) {
    // (gone: the next in order after it)
    auto after = std::upper_bound(alive.begin(), alive.end(), g_follow);
    g_follow = dir < 0 ? (after == alive.begin() ? alive.back() : *(after - 1))
                       : (after == alive.end() ? alive.front() : *after);
  } else {
    int i = int(it - alive.begin()) + (dir ? dir : 0);
    g_follow = alive[(i + int(alive.size())) % int(alive.size())];
  }
  g_killer_follow = false;
  return true;
}

bool PlaceFollow() {
  float eye[3], yaw, pitch;
  {
    std::lock_guard l(g_mu);
    if (!Pick(0)) return false;
    const Player& p = g_players[g_follow];
    Pose po = PoseAt(p, Now() - kDelay);
    const float dist = g_dist.load();
    const bool fp = dist < 1.0f;
    const float height = po.veh ? 2.4f : 1.65f;
    float head[3] = {po.p[0], po.p[1] + height, po.p[2]};
    yaw = po.yaw;
    if (fp) {
      pitch = std::clamp(po.pitch, -1.4f, 1.4f);
      for (int i = 0; i < 3; ++i) eye[i] = head[i];
    } else {
      const float d = dist * (po.veh ? 2.5f : 1.0f);
      pitch = std::clamp(po.pitch, -0.8f, 0.8f);
      float f[3] = {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
      eye[0] = head[0] - f[0] * d, eye[1] = head[1] - f[1] * d + 0.4f + d * 0.06f, eye[2] = head[2] - f[2] * d;
    }
  }
  freecam::PlaceAt(eye, yaw, pitch);
  return true;
}

void Drive() {  // each camera frame (free_camera.h)
  if ((g_user || g_death) && !g_free) PlaceFollow();
}
bool Wheel(long w) {
  if (!(g_user || g_death) || g_free) return false;
  float d = g_dist.load();
  if (w > 0) d = d * 0.8f < 1.0f ? 0.5f : d * 0.8f;  // (in: down to first person)
  else d = d < 1.0f ? 1.5f : std::min(60.0f, d * 1.25f);
  g_dist = d;
  return true;
}

void SetFree(bool free) {
  g_free = free;
  freecam::SetDriven(!free);
}

// ---- the overlay ------------------------------------------------------------------

constexpr int kW = 1280, kH = 720;
struct Painter {
  HDC dc = nullptr;
  HBITMAP bmp = nullptr;
  uint8_t* bits = nullptr;
  HFONT big = nullptr, small = nullptr;
  Painter() {
    dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), kW, -kH, 1, 32, BI_RGB};
    void* p = nullptr;
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
    bits = static_cast<uint8_t*>(p);
    SelectObject(dc, bmp);
    big = CreateFontW(-24, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                      NONANTIALIASED_QUALITY, DEFAULT_PITCH, L"Bahnschrift SemiBold Condensed");
    small = CreateFontW(-16, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        NONANTIALIASED_QUALITY, DEFAULT_PITCH, L"Bahnschrift SemiBold Condensed");
    SetBkMode(dc, TRANSPARENT);
    SetTextAlign(dc, TA_CENTER);
  }
  // Two centred lines on a dark panel at the top.
  void Publish(const std::wstring& l1, COLORREF c1, const std::wstring& l2) {
    memset(bits, 0, size_t(kW) * kH * 4);
    SelectObject(dc, big);
    SetTextColor(dc, c1);
    TextOutW(dc, kW / 2, 18, l1.c_str(), int(l1.size()));
    SelectObject(dc, small);
    SetTextColor(dc, RGB(215, 215, 215));
    TextOutW(dc, kW / 2, 50, l2.c_str(), int(l2.size()));
    GdiFlush();
    std::vector<uint8_t> rgba(size_t(kW) * kH * 4);
    for (size_t i = 0; i < size_t(kW) * kH; ++i) {
      uint8_t b = bits[i * 4], g = bits[i * 4 + 1], r = bits[i * 4 + 2];
      rgba[i * 4] = r, rgba[i * 4 + 1] = g, rgba[i * 4 + 2] = b;
      rgba[i * 4 + 3] = (r | g | b) ? 255 : 0;
    }
    for (int y = 8; y < 78; ++y)
      for (int x = kW / 2 - 440; x < kW / 2 + 440; ++x) {
        uint8_t* q = &rgba[(size_t(y) * kW + x) * 4];
        if (!q[3]) q[0] = q[1] = q[2] = 10, q[3] = 150;
      }
    xgpu::SetOverlay(rgba.data(), kW, kH);
  }
};

std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

void DrawOverlay(Painter& painter) {
  std::wstring l1, l2;
  COLORREF col = RGB(240, 240, 240);
  {
    std::lock_guard l(g_mu);
    int alive = 0;
    for (auto& [k, p] : g_players) alive += Alive(p);
    const char* mode = g_free ? "FREE CAMERA" : g_dist.load() < 1.0f ? "FIRST PERSON" : "CHASE VIEW";
    if (g_free) {
      l1 = L"SPECTATING  -  FREE CAMERA";
    } else if (auto it = g_players.find(g_follow); it != g_players.end() && Alive(it->second)) {
      const Player& p = it->second;
      col = p.team == 1 ? RGB(110, 175, 255) : RGB(255, 100, 90);
      wchar_t b[200];
      swprintf(b, 200, L"%ls%ls  -  %ls  -  HEALTH %d  -  KILLS %d  DEATHS %d", g_death ? L"YOU DIED: " : L"SPECTATING: ",
               Wide(p.name).c_str(), p.team == 1 ? L"US" : L"JAPAN", int(p.health + 0.5f), p.kills, p.deaths);
      l1 = b;
      if (g_killer_follow) l1 += L"  (your killer)";
    } else {
      l1 = L"SPECTATING  -  waiting for players";
    }
    wchar_t b[200];
    swprintf(b, 200, L"%ls   %d alive   N / B player   SPACE free / follow   WHEEL zoom   F4 %ls", Wide(mode).c_str(),
             alive, g_death ? L"stay dead" : L"stop");
    l2 = b;
  }
  painter.Publish(l1, col, l2);
}

// ---- the thread -----------------------------------------------------------------------

std::atomic<int64_t> g_level_seen{-1};

void Stop(bool restore_menu) {
  const bool was = g_user || g_death;
  g_user = false, g_death = false, g_pending = false;
  if (!was) return;
  freecam::SetDriven(false);
  freecam::SetActive(false);
  g_free = false;
  xgpu::SetOverlay(nullptr, 0, 0);
  if (restore_menu) g_level_menu = g_real_menu.load();  // (the spawn screen's input is the game's again)
  RT_INFO("spectate: off");
}

bool StartFollow(bool death) {
  {
    std::lock_guard l(g_mu);
    if (!Pick(0)) return false;
  }
  SetFree(false);
  if (!PlaceFollow()) return false;
  if (death) g_death = true;
  else g_user = true, g_pending = false;
  if (!freecam::Active()) freecam::SetActive(true);
  freecam::SetDriven(true);
  if (!death) g_level_menu = false;
  RT_INFO("spectate: %s", death ? "following after the death" : "on");
  return true;
}

// The soldier nearest the game's own camera while it is the player's: ours.
uint32_t g_mine = 0;
double g_mine_seen = 0;
uint32_t g_dead_key = 0;
double g_death_at = 0;

void DeathLogic() {
  if (g_death) {
    bool end;
    {
      std::lock_guard l(g_mu);
      auto it = g_players.find(g_dead_key);
      const bool back = it != g_players.end() && Alive(it->second) && Now() - g_death_at > 1.0;
      end = g_real_menu || back || Now() - g_death_at > 45.0 || !freecam::Active();  // (F3 gave the view back)
    }
    if (end) Stop(false);
    return;
  }
  if (!g_on_death || g_user || freecam::Active() || g_real_menu) {
    g_mine = 0;
    return;
  }
  float eye[3];
  const bool have_eye = xgpu::CameraEye(eye);
  std::lock_guard l(g_mu);
  const auto mine = g_players.find(g_mine);
  if (g_mine && mine != g_players.end() && Alive(mine->second)) {
    g_mine_seen = Now();  // (kept: no other soldier near the camera takes its place)
  } else if (have_eye && !g_mine) {
    uint32_t best_key = 0;
    float best = 8.0f;
    for (auto& [k, p] : g_players) {
      if (!Alive(p) || p.bot) continue;
      const Pose& po = p.pose[p.n - 1];
      float d = std::sqrt((po.p[0] - eye[0]) * (po.p[0] - eye[0]) + (po.p[1] - eye[1]) * (po.p[1] - eye[1]) +
                          (po.p[2] - eye[2]) * (po.p[2] - eye[2]));
      if (d < best) best = d, best_key = k;
    }
    if (best_key) g_mine = best_key, g_mine_seen = Now();
  }
  if (!g_mine) return;
  auto it = g_players.find(g_mine);
  if (it != g_players.end() && Alive(it->second)) return;
  if (Now() - g_mine_seen > 4.0) {  // (gone for long: the level ended, not a death)
    g_mine = 0;
    return;
  }
  // Dead: its killer, or a teammate, or anyone.
  const uint32_t dead = g_mine;
  const int team = it != g_players.end() ? it->second.team : 0;
  uint32_t target = 0;
  g_mine = 0;
  g_killer_follow = false;
  if (g_last_kill.victim == dead && Now() - g_last_kill.t < 3.0) {
    auto k = g_players.find(g_last_kill.killer);
    if (k != g_players.end() && Alive(k->second)) target = g_last_kill.killer, g_killer_follow = true;
  }
  if (!target)
    for (auto& [k, p] : g_players)
      if (Alive(p) && p.team == team && k != dead) target = k;
  if (!target)
    for (auto& [k, p] : g_players)
      if (Alive(p) && k != dead) target = k;
  if (!target) return;
  g_dead_key = dead, g_death_at = Now(), g_follow = target;
  g_arm_death = true;
}

void Run() {
  Painter painter;
  bool f4_was = false, n_was = false, b_was = false, space_was = false, joiner_told = false;
  auto last_sample = std::chrono::steady_clock::time_point{}, last_scan = last_sample, last_draw = last_sample;
  bool overlay_up = false;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(33));
    const auto now = std::chrono::steady_clock::now();
    const bool host = (g_lan_host || g_dedicated) && !g_join_address;
    const bool in_level = !g_in_menus.load() && g_level_open_ms.load() > 0;
    const int64_t level = in_level ? g_level_open_ms.load() : -1;
    if (level != g_level_seen) {  // (a level opened or closed)
      if (g_user || g_death) Stop(false);
      {
        std::lock_guard l(g_mu);
        g_players.clear(), g_soldier_key.clear(), g_follow = 0, g_mine = 0, g_humans_named = 0;
      }
      g_level_seen = level;
      g_pending = in_level && g_flag && host;
      overlay_up = false;
    }
    if (!in_level) continue;
    const bool typing = admin::Typing();
    bool f4 = !typing && freecam::KeyHeld(VK_F4);
    if (f4 && !f4_was) {
      if (!host) {
        if (!joiner_told) {
          joiner_told = true;
          RT_WARN("spectate: only a host (or a game hosted with bots) can spectate: a joining client's game has no "
                  "server soldiers to read");
        }
      } else if (g_user || g_death) {
        Stop(true);
      } else if (g_pending) {
        g_pending = false;
        g_level_menu = g_real_menu.load();  // (the spawn screen's input is the game's again)
      } else {
        g_pending = true;  // (as soon as someone is there to follow)
      }
    }
    f4_was = f4;
    if (!host) continue;
    if (now - last_sample >= std::chrono::milliseconds(100)) {
      // Spectating (or about to): the whole guest memory is scanned every 2 s for new
      // players; only waiting for a death it is every 15 s (a known player's soldier
      // is read through its spawner at every sample, so a death is seen all the same).
      const bool busy = g_pending || g_user || g_death;
      const bool rescan = now - last_scan > (busy ? std::chrono::seconds(2) : std::chrono::seconds(15));
      if (rescan) last_scan = now;
      last_sample = now;
      if (g_pending || g_user || g_death || g_on_death) SampleServer(rescan);
    }
    if (g_pending && !g_user) StartFollow(false);
    DeathLogic();
    if (g_arm_death) {
      g_arm_death = false;
      if (!StartFollow(true)) g_death = false;
    }
    if (g_user || g_death) {
      auto edge = [&](int vk, bool& was) {
        bool d = !typing && freecam::KeyHeld(vk);
        bool e = d && !was;
        was = d;
        return e;
      };
      if (edge('N', n_was)) {
        std::lock_guard l(g_mu);
        Pick(1);
      }
      if (edge('B', b_was)) {
        std::lock_guard l(g_mu);
        Pick(-1);
      }
      if (edge(VK_SPACE, space_was)) FreeKey();
      if (now - last_draw >= std::chrono::milliseconds(100) && !pcset::IsOpen() && !g_level_menu) {
        last_draw = now;
        DrawOverlay(painter);
        overlay_up = true;
      }
    } else if (overlay_up) {
      overlay_up = false;
    }
  }
}
}  // namespace

void Start() {
  static std::once_flag once;
  std::call_once(once, [] {
    if (g_dedicated || !g_present) {
      if (g_flag) RT_WARN("spectate: a dedicated server has no window to watch in");
      g_flag = false;
      return;
    }
    freecam::SetDriver(Drive, Wheel);
    std::thread(Run).detach();
  });
}
bool Active() { return g_user.load() || (g_flag && g_pending.load()); }
bool DeathWatch() { return g_death.load(); }
bool MenuValue(bool show) {
  g_real_menu = show;
  return show && !Active();
}
void Killed(uint32_t victim, uint32_t killer) {
  std::lock_guard l(g_mu);
  auto v = g_soldier_key.find(victim);
  if (v == g_soldier_key.end()) return;
  auto pv = g_players.find(v->second);
  if (pv != g_players.end()) ++pv->second.deaths;
  auto k = g_soldier_key.find(killer);
  if (killer && k != g_soldier_key.end()) {
    if (k->second != v->second)
      if (auto pk = g_players.find(k->second); pk != g_players.end()) ++pk->second.kills;
    g_last_kill = {v->second, k->second, Now()};
  } else {
    g_last_kill = {v->second, 0, Now()};
  }
}
void FreeKey() {
  if (!(g_user || g_death)) return;
  SetFree(!g_free);
  RT_INFO("spectate: %s", g_free ? "free camera" : "following");
}
#endif
}  // namespace rt::spectate
