// MATCH REPLAY VIEWER (--replay <file.bfrp>, replay.h).
//
// The recorded match isn't put back into the game's own entities (a
// recompiled game's soldiers and vehicles are driven by its network and
// physics code; making them follow a file would mean faking a server). The
// level is loaded as it is (hosted with no bots: the player spawns alone)
// and the recording is drawn over it: each soldier a marker in its team's
// colour where it stood (feet to head, a tick where it looked, its name),
// vehicles as boxes with their kind, kills as crosses, flags in their
// owner's colour; the free camera (free_camera.h) flies around it, or
// follows a player. The markers are drawn over the picture (not hidden by
// walls or hills), projected with the free camera's own view.
//
// Keys: P / K pause.  Left / Right: 10 s back / on (Shift: 60 s).
//   Up / Down: faster / slower (1/4x ... 16x).  Home: the start.
//   N: follow the next player alive, B: the previous; F: fly freely again.
//   (The free camera: mouse, W A S D, Space / E up, Ctrl / C / Q down, Shift.)
#include "free_camera.h"
#include "kernel.h"
#include "pc_settings.h"
#include "replay.h"
#include "xenia_bridge.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <Xinput.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rt::replay {
std::string g_replay_file;

#ifndef _WIN32
void StartViewer() { RT_WARN("replay: the viewer is Windows-only for now"); }
#else
namespace {
constexpr float kPi = 3.14159265358979f;
constexpr int kW = 1280, kH = 720;  // the overlay (stretched to the window)

Recording g_rec;

struct State {
  SoldierState s;
  bool valid = false;
};

// The recording at time t (ms): soldiers and vehicles between the frames around it.
void At(double t, std::vector<SoldierState>& soldiers, std::vector<VehicleState>& vehicles) {
  soldiers.clear(), vehicles.clear();
  const auto& fr = g_rec.frames;
  auto it = std::upper_bound(fr.begin(), fr.end(), t, [](double v, const Frame& f) { return v < f.t; });
  const Frame& b = it == fr.end() ? fr.back() : *it;
  const Frame& a = it == fr.begin() ? fr.front() : *(it - 1);
  double k = b.t > a.t ? std::clamp((t - a.t) / double(b.t - a.t), 0.0, 1.0) : 0.0;
  auto lerp_angle = [](float x, float y, double k) {
    float d = y - x;
    while (d > kPi) d -= 2 * kPi;
    while (d < -kPi) d += 2 * kPi;
    return float(x + d * k);
  };
  for (const SoldierState& s : a.soldiers) {
    SoldierState o = s;
    for (const SoldierState& n : b.soldiers)
      if (n.player == s.player) {
        float jump = std::hypot(n.pos[0] - s.pos[0], n.pos[2] - s.pos[2]);
        if (jump < 30.0f) {  // (not a respawn)
          for (int i = 0; i < 3; ++i) o.pos[i] = float(s.pos[i] + (n.pos[i] - s.pos[i]) * k);
          o.yaw = lerp_angle(s.yaw, n.yaw, k), o.pitch = float(s.pitch + (n.pitch - s.pitch) * k);
        }
      }
    soldiers.push_back(o);
  }
  for (const VehicleState& v : a.vehicles) {
    VehicleState o = v;
    for (const VehicleState& n : b.vehicles)
      if (n.id == v.id) {
        for (int i = 0; i < 3; ++i) o.pos[i] = float(v.pos[i] + (n.pos[i] - v.pos[i]) * k);
        o.yaw = lerp_angle(v.yaw, n.yaw, k);
      }
    vehicles.push_back(o);
  }
}

// World -> overlay pixel with the free camera's view; false behind it.
struct Projector {
  freecam::View v;
  float ty = 0, tx = 0;
  bool ok = false;
  void Set(const freecam::View& view) {
    v = view;
    ty = std::tan(v.fov_y * 0.5f);
    tx = ty * (16.0f / 9.0f);
    ok = true;
  }
  bool Project(const float p[3], float& x, float& y, float& depth) const {
    if (!ok) return false;
    float d[3] = {p[0] - v.eye[0], p[1] - v.eye[1], p[2] - v.eye[2]};
    float z = d[0] * v.forward[0] + d[1] * v.forward[1] + d[2] * v.forward[2];
    if (z < 0.5f) return false;
    float right = -(d[0] * v.left[0] + d[1] * v.left[1] + d[2] * v.left[2]);
    float up = d[0] * v.up[0] + d[1] * v.up[1] + d[2] * v.up[2];
    x = (0.5f + 0.5f * right / (z * tx)) * kW;
    y = (0.5f - 0.5f * up / (z * ty)) * kH;
    depth = z;
    return x > -200 && x < kW + 200 && y > -200 && y < kH + 200;
  }
};

COLORREF TeamColor(int team, bool bright = true) {
  if (team == 1) return bright ? RGB(80, 160, 255) : RGB(40, 90, 170);  // US
  if (team == 2) return bright ? RGB(255, 70, 60) : RGB(170, 40, 35);   // Japan
  return RGB(230, 230, 230);
}

std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }
std::wstring Clock(double ms) {
  int s = int(ms / 1000);
  wchar_t b[32];
  swprintf(b, 32, L"%d:%02d", s / 60, s % 60);
  return b;
}

struct Painter {
  HDC dc = nullptr;
  HBITMAP bmp = nullptr;
  uint8_t* bits = nullptr;
  HFONT font = nullptr, small = nullptr;
  std::vector<RECT> panels;  // dark see-through boxes (under text)
  Painter() {
    dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), kW, -kH, 1, 32, BI_RGB};
    void* p = nullptr;
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
    bits = static_cast<uint8_t*>(p);
    SelectObject(dc, bmp);
    font = CreateFontW(-20, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       NONANTIALIASED_QUALITY, DEFAULT_PITCH, L"Bahnschrift SemiBold Condensed");
    small = CreateFontW(-15, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        NONANTIALIASED_QUALITY, DEFAULT_PITCH, L"Bahnschrift SemiBold Condensed");
    SetBkMode(dc, TRANSPARENT);
  }
  void Clear() {
    memset(bits, 0, size_t(kW) * kH * 4);
    panels.clear();
  }
  void Line(float x0, float y0, float x1, float y1, COLORREF c, int w = 2) {
    HPEN pen = CreatePen(PS_SOLID, w, c);
    HGDIOBJ old = SelectObject(dc, pen);
    MoveToEx(dc, int(x0), int(y0), nullptr);
    LineTo(dc, int(x1), int(y1));
    SelectObject(dc, old);
    DeleteObject(pen);
  }
  void Box(float x0, float y0, float x1, float y1, COLORREF c, bool fill) {
    HPEN pen = CreatePen(PS_SOLID, 2, c);
    HBRUSH brush = fill ? CreateSolidBrush(c) : (HBRUSH)GetStockObject(NULL_BRUSH);
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, brush);
    Rectangle(dc, int(x0), int(y0), int(x1), int(y1));
    SelectObject(dc, op), SelectObject(dc, ob);
    DeleteObject(pen);
    if (fill) DeleteObject(brush);
  }
  void Dot(float x, float y, float r, COLORREF c) {
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    HBRUSH brush = CreateSolidBrush(c);
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, brush);
    Ellipse(dc, int(x - r), int(y - r), int(x + r + 1), int(y + r + 1));
    SelectObject(dc, op), SelectObject(dc, ob);
    DeleteObject(pen), DeleteObject(brush);
  }
  // align: 0 left, 1 centre, 2 right of x.
  void Text(float x, float y, const std::wstring& s, COLORREF c, bool big = false, int align = 0) {
    SelectObject(dc, big ? font : small);
    SetTextColor(dc, c);
    SetTextAlign(dc, align == 1 ? TA_CENTER : align == 2 ? TA_RIGHT : TA_LEFT);
    TextOutW(dc, int(x), int(y), s.c_str(), int(s.size()));
  }
  void Panel(int x0, int y0, int x1, int y1) { panels.push_back({x0, y0, x1, y1}); }
  // GDI's drawing (on black) to straight-alpha RGBA: what was drawn is opaque.
  void Publish() {
    GdiFlush();
    std::vector<uint8_t> rgba(size_t(kW) * kH * 4);
    for (size_t i = 0; i < size_t(kW) * kH; ++i) {
      uint8_t b = bits[i * 4], g = bits[i * 4 + 1], r = bits[i * 4 + 2];
      rgba[i * 4] = r, rgba[i * 4 + 1] = g, rgba[i * 4 + 2] = b;
      rgba[i * 4 + 3] = (r | g | b) ? 255 : 0;
    }
    for (const RECT& p : panels)
      for (int y = std::max(0, int(p.top)); y < std::min(kH, int(p.bottom)); ++y)
        for (int x = std::max(0, int(p.left)); x < std::min(kW, int(p.right)); ++x) {
          uint8_t* q = &rgba[(size_t(y) * kW + x) * 4];
          if (!q[3]) q[0] = q[1] = q[2] = 10, q[3] = 150;
        }
    xgpu::SetOverlay(rgba.data(), kW, kH);
  }
};

struct Viewer {
  double t = 0;       // ms into the recording
  double speed = 1;
  bool paused = false;
  int follow = -1;    // player followed (-1: none)
  bool started = false;
  std::map<int, bool> was;  // keys' last state
  std::vector<SoldierState> soldiers;
  std::vector<VehicleState> vehicles;

  bool Pressed(int vk) {
    bool now = freecam::KeyHeld(vk);
    bool before = was[vk];
    was[vk] = now;
    return now && !before;
  }
  std::wstring Name(int id) const {
    if (id < 0 || id >= int(g_rec.players.size()) || g_rec.players[id].name.empty()) {
      wchar_t b[24];
      swprintf(b, 24, L"#%d", id + 1);
      return b;
    }
    return Wide(g_rec.players[id].name);
  }
  int Team(int id) const { return id >= 0 && id < int(g_rec.players.size()) ? g_rec.players[id].team : 0; }

  void Keys(double dt_ms) {
    if (Pressed('P') || Pressed('K')) paused = !paused;
    const bool shift = freecam::KeyHeld(VK_SHIFT);
    if (Pressed(VK_LEFT)) t -= shift ? 60000 : 10000;
    if (Pressed(VK_RIGHT)) t += shift ? 60000 : 10000;
    if (Pressed(VK_UP)) speed = std::min(16.0, speed * 2);
    if (Pressed(VK_DOWN)) speed = std::max(0.25, speed / 2);
    if (Pressed(VK_HOME)) t = 0;
    auto step_follow = [&](int dir) {
      if (soldiers.empty()) return;
      std::vector<int> ids;
      for (const auto& s : soldiers) ids.push_back(s.player);
      std::sort(ids.begin(), ids.end());
      auto it = std::find(ids.begin(), ids.end(), follow);
      if (it == ids.end()) follow = dir > 0 ? ids.front() : ids.back();
      else {
        int i = int(it - ids.begin()) + dir;
        follow = ids[(i + int(ids.size())) % int(ids.size())];
      }
      RT_INFO("replay: following %ls", Name(follow).c_str());
    };
    if (Pressed('N')) step_follow(1);
    if (Pressed('B')) step_follow(-1);
    if (Pressed('F')) follow = -1;
    if (!paused) t += dt_ms * speed;
    t = std::clamp(t, 0.0, double(g_rec.length));
  }

  void Follow() {
    if (follow < 0) return;
    for (const SoldierState& s : soldiers)
      if (s.player == follow) {
        // Behind and above it, looking where it looks (a little down).
        float fx = std::sin(s.yaw), fz = std::cos(s.yaw);
        float eye[3] = {s.pos[0] - fx * 9.0f, s.pos[1] + 4.0f, s.pos[2] - fz * 9.0f};
        freecam::PlaceAt(eye, s.yaw, -0.3f + s.pitch * 0.4f);
        return;
      }
    // (dead: the camera stays until it is back)
  }

  void Draw(Painter& p) {
    p.Clear();
    Projector pr;
    freecam::View v;
    if (freecam::CurrentView(v)) pr.Set(v);
    // Flags: each point's owner at t.
    std::map<int, const Event*> flags;
    for (const Event& e : g_rec.events)
      if (e.type == 5 && e.t <= t) flags[e.a] = &e;
    for (auto& [id, e] : flags) {
      float x, y, d, top[3] = {e->pos[0], e->pos[1] + 12.0f, e->pos[2]};
      float x2, y2, d2;
      if (pr.Project(e->pos, x, y, d) && pr.Project(top, x2, y2, d2)) {
        COLORREF c = e->b == 1 || e->b == 2 ? TeamColor(e->b) : RGB(230, 230, 230);
        p.Line(x, y, x2, y2, RGB(220, 220, 220), 2);
        p.Box(x2, y2, x2 + 22, y2 + 14, c, true);
      }
    }
    // Vehicles: a box on the ground, its kind.
    for (const VehicleState& ve : vehicles) {
      float x, y, d;
      float c[3] = {ve.pos[0], ve.pos[1] + 1.2f, ve.pos[2]};
      if (!pr.Project(c, x, y, d)) continue;
      float r = std::clamp(900.0f / d, 5.0f, 60.0f);
      p.Box(x - r, y - r * 0.6f, x + r, y + r * 0.6f, RGB(250, 210, 60), false);
      if (d < 400) {
        std::string kind = ve.id < g_rec.vehicles.size() ? g_rec.vehicles[ve.id] : "?";
        p.Text(x, y + r * 0.6f + 2, Wide(kind), RGB(250, 210, 60), false, 1);
      }
    }
    // Soldiers: feet to head in the team's colour, a tick where it looks.
    for (const SoldierState& s : soldiers) {
      float head[3] = {s.pos[0], s.pos[1] + 1.8f, s.pos[2]};
      float look[3] = {head[0] + std::sin(s.yaw) * 1.5f, head[1] + std::sin(s.pitch) * 1.5f,
                       head[2] + std::cos(s.yaw) * 1.5f};
      float fx, fy, fd, hx, hy, hd, lx, ly, ld;
      if (!pr.Project(s.pos, fx, fy, fd) || !pr.Project(head, hx, hy, hd)) continue;
      const int team = Team(s.player);
      const COLORREF c = TeamColor(team, !(s.flags & 1));
      float h = std::max(6.0f, fy - hy);
      float w = std::max(3.0f, h * 0.22f);
      if (s.player == follow) {
        // (the one followed: an outline, so the scene behind it shows)
        p.Box(hx - w, hy - 2 * w, hx + w, fy, c, false);
      } else {
        p.Box(hx - w, hy, hx + w, fy, c, true);
        p.Dot(hx, hy - w, w, c);
        if (pr.Project(look, lx, ly, ld)) p.Line(hx, hy, lx, ly, RGB(255, 255, 255), 2);
      }
      if (fd < 250 || s.player == follow) {
        std::wstring label = Name(s.player);
        if (s.flags & 1) label += L" (vehicle)";
        p.Text(hx, hy - 2 * w - 20, label, c, false, 1);
      }
    }
    // Kills of the last 5 s: a cross where the soldier fell.
    std::vector<const Event*> feed;
    for (const Event& e : g_rec.events) {
      if (e.type != 4 || e.t > t) continue;
      if (t - e.t < 8000) feed.push_back(&e);
      if (t - e.t > 5000) continue;
      float x, y, d;
      if (!pr.Project(e.pos, x, y, d)) continue;
      float r = std::clamp(600.0f / d, 5.0f, 18.0f);
      p.Line(x - r, y - r, x + r, y + r, RGB(255, 40, 40), 3);
      p.Line(x - r, y + r, x + r, y - r, RGB(255, 40, 40), 3);
    }
    // The kill feed (top right).
    int fy0 = 60;
    for (size_t i = feed.size() > 5 ? feed.size() - 5 : 0; i < feed.size(); ++i) {
      const Event& e = *feed[i];
      std::wstring killer = e.b == 0xFFFF ? L"?" : Name(e.b);
      std::wstring line = killer + L"  killed  " + Name(e.a);
      p.Panel(kW - 330, fy0 - 2, kW - 10, fy0 + 20);
      p.Text(kW - 320, fy0, line, RGB(240, 240, 240));
      fy0 += 24;
    }
    // The timeline (bottom).
    const int y0 = kH - 58;
    p.Panel(20, y0 - 6, kW - 20, kH - 10);
    double len = std::max<double>(1.0, g_rec.length);
    const float bx0 = 200, bx1 = kW - 40, by = float(y0 + 28);
    p.Line(bx0, by, bx1, by, RGB(140, 140, 140), 4);
    float at = float(bx0 + (bx1 - bx0) * (t / len));
    p.Line(bx0, by, at, by, RGB(250, 210, 60), 4);
    for (const Event& e : g_rec.events)
      if (e.type == 4) {
        float ex = float(bx0 + (bx1 - bx0) * (e.t / len));
        p.Line(ex, by - 6, ex, by - 1, RGB(255, 60, 60), 1);
      }
    p.Dot(at, by, 6, RGB(255, 255, 255));
    wchar_t buf[160];
    swprintf(buf, 160, L"REPLAY  %ls / %ls   %gx%ls", Clock(t).c_str(), Clock(len).c_str(), speed,
             paused ? L"  PAUSED" : L"");
    p.Text(32, float(y0 - 2), buf, RGB(255, 255, 255), true);
    swprintf(buf, 160, L"%ls   %d alive", Wide(g_rec.map).c_str(), int(soldiers.size()));
    p.Text(32, float(y0 + 22), buf, RGB(200, 200, 200));
    std::wstring help = L"P pause   \x2190 \x2192 10 s   \x2191 \x2193 speed   N / B follow   F free   F3 camera";
    if (follow >= 0) help = L"following " + Name(follow) + L"     " + help;
    p.Text(float(kW - 32), float(y0 - 2), help, RGB(200, 200, 200), false, 2);
    // (right-aligned)
    p.Publish();
  }
};

// Into the level: Start and the dialogs (the dedicated server's automation,
// xam.cpp), then A on the spawn screen until the player is in.
void Run() {
  Viewer v;
  Painter painter;
  using Clk = std::chrono::steady_clock;
  auto last = Clk::now();
  auto in_since = Clk::time_point{};
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(33));
    auto now = Clk::now();
    double dt = std::chrono::duration<double, std::milli>(now - last).count();
    last = now;
    const bool in_level = !g_in_menus && g_level_open_ms > 0;
    if (!v.started) {
      // The spawn screen: deploy (A pulses).
      bool pulse = std::fmod(std::chrono::duration<double>(now.time_since_epoch()).count(), 1.5) < 0.15;
      g_auto_buttons = in_level && g_level_menu && pulse ? XINPUT_GAMEPAD_A : 0;
      if (in_level && !g_level_menu) {
        if (in_since == Clk::time_point{}) in_since = now;
      } else {
        in_since = {};
      }
      if (in_since == Clk::time_point{} || now - in_since < std::chrono::seconds(3)) continue;
      g_auto_buttons = 0;
      v.started = true;
      // Above the players at the start, looking down at them.
      float c[3] = {};
      const Frame& f = g_rec.frames.front();
      for (const auto& s : f.soldiers)
        for (int i = 0; i < 3; ++i) c[i] += s.pos[i] / float(f.soldiers.size());
      if (f.soldiers.empty()) c[1] = 100;
      float eye[3] = {c[0] - 70.0f, c[1] + 60.0f, c[2] - 70.0f};
      freecam::PlaceAt(eye, std::atan2(c[0] - eye[0], c[2] - eye[2]), -std::atan2(60.0f, 99.0f));
      freecam::SetActive(true);
      freecam::PlaceAt(eye, std::atan2(c[0] - eye[0], c[2] - eye[2]), -std::atan2(60.0f, 99.0f));
      RT_INFO("replay: playing %s (%s, %.0f s, %zu frames, %zu players, %zu events)", g_replay_file.c_str(),
              g_rec.map.c_str(), g_rec.length / 1000.0, g_rec.frames.size(), g_rec.players.size(), g_rec.events.size());
      continue;
    }
    if (!in_level) {
      xgpu::SetOverlay(nullptr, 0, 0);
      continue;
    }
    if (!g_level_menu && !pcset::IsOpen()) v.Keys(dt);
    At(v.t, v.soldiers, v.vehicles);
    v.Follow();
    if (g_level_menu || pcset::IsOpen()) continue;  // (the game's menus over the level: theirs)
    v.Draw(painter);
    static int logged = -1;
    int sec = int(v.t / 10000);
    if (sec != logged) {
      logged = sec;
      RT_INFO("replay: %.0f s, %zu soldiers, %zu vehicles%s", v.t / 1000.0, v.soldiers.size(), v.vehicles.size(),
              v.paused ? " (paused)" : "");
    }
  }
}
}  // namespace

void StartViewer() {
  std::string error;
  if (!Load(g_replay_file, g_rec, &error)) {
    RT_WARN("replay: %s: %s", g_replay_file.c_str(), error.c_str());
    g_replay_file.clear();
    return;
  }
  // The recorded level, hosted alone (no bots, nobody joins it).
  g_lan_host = true;
  {
    std::string safe;  // (the name comes from the file: map-name characters only)
    for (char c : g_rec.map)
      if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') safe += c;
    g_host_map = safe;
  }
  g_bot_count = 0;
  g_auto_start = true;
  RT_INFO("replay: %s: %s, %.0f s, %zu players", g_replay_file.c_str(), g_rec.map.c_str(), g_rec.length / 1000.0,
          g_rec.players.size());
  std::thread(Run).detach();
}
#endif
}  // namespace rt::replay
