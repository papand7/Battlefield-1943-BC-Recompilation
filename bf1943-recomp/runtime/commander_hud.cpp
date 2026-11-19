// The commander's on-screen markers (commander_hud.h): the zones' rings on the
// ground and their panel, the order's marker and label. Drawn by the UI's
// overlay (bridge.cc) each frame from the main view's matrix (scene_fx's
// ViewProj) and the eye (CameraEye).
#include "commander_hud.h"

#include "commander.h"
#include "xenia_bridge.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace rt::commander::hud {
namespace {
using Clock = std::chrono::steady_clock;
constexpr float kRingRadius = 15.0f;    // a supply zone's reach (commander_world.cpp)
constexpr float kStrikeRadius = 12.0f;  // the airstrike's blast
constexpr float kEyeToGround = 1.7f;  // the eye's height over the ground, when the ground isn't known
constexpr int kPanelTexture = 1, kLabelTexture = 2;

struct Stored {
  Snapshot s;
  Clock::time_point at;
};
std::mutex g_mu;
Stored g_state[3];  // by team

// What the textures showed last time (a text is redrawn only when it changes).
std::string g_panel_key, g_label_key;
bool g_panel_shown = false, g_label_shown = false;

// --- the packet ---------------------------------------------------------------
// "z<kind>,<x>,<y or n>,<z>,<left>;...|o<kind>,<x>,<y or n>,<z>,<left>,<flag>"
void PutY(char* b, size_t n, bool has, float y) {
  if (has) snprintf(b, n, "%.2f", y);
  else snprintf(b, n, "n");
}

bool ParseY(const std::string& t, float* y, bool* has) {
  *has = t != "n";
  if (!*has) return true;
  char* end = nullptr;
  *y = strtof(t.c_str(), &end);
  return end && !*end && std::isfinite(*y);
}

std::vector<std::string> Split(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t at = 0;
  for (;;) {
    size_t e = s.find(sep, at);
    out.push_back(s.substr(at, e == std::string::npos ? std::string::npos : e - at));
    if (e == std::string::npos) break;
    at = e + 1;
  }
  return out;
}

}  // namespace

std::string Encode(const Snapshot& s) {
  std::string out;
  char b[160], y[24];
  for (const Zone& z : s.zones) {
    PutY(y, sizeof y, z.has_y, z.y);
    snprintf(b, sizeof b, "z%d,%.2f,%s,%.2f,%.1f;", z.kind, z.x, y, z.z, z.left);
    out += b;
  }
  out += "|";
  if (s.order.kind) {
    PutY(y, sizeof y, s.order.has_y, s.order.y);
    snprintf(b, sizeof b, "o%d,%.2f,%s,%.2f,%.1f,%d,", s.order.kind, s.order.x, y, s.order.z, s.order.left, s.order.flag);
    out += b;
    for (char c : s.order.name.substr(0, 40))  // (the separators can't be in it)
      out += (c == ',' || c == ';' || c == '|' || c < 32) ? ' ' : c;
  }
  out += "|";
  if (s.strike.left > 0) {
    PutY(y, sizeof y, s.strike.has_y, s.strike.y);
    snprintf(b, sizeof b, "s%.2f,%s,%.2f,%.1f", s.strike.x, y, s.strike.z, s.strike.left);
    out += b;
  }
  return out;
}

namespace {
Snapshot Decode(const std::string& body) {
  Snapshot s;
  const auto parts = Split(body, '|');
  if (parts.size() < 2) return s;
  for (const std::string& part : Split(parts[0], ';')) {
    if (part.size() < 2 || part[0] != 'z') continue;
    const auto f = Split(part.substr(1), ',');
    if (f.size() != 5) continue;
    Zone z;
    z.kind = atoi(f[0].c_str()), z.x = float(atof(f[1].c_str())), z.z = float(atof(f[3].c_str()));
    z.left = float(atof(f[4].c_str()));
    if (!ParseY(f[2], &z.y, &z.has_y) || !std::isfinite(z.x) || !std::isfinite(z.z)) continue;
    if (z.kind >= 1 && z.kind <= 2 && z.left > 0) s.zones.push_back(z);
  }
  const std::string& o = parts[1];
  if (o.size() > 1 && o[0] == 'o') {
    const auto f = Split(o.substr(1), ',');
    if (f.size() == 6 || f.size() == 7) {
      Order d;
      d.kind = atoi(f[0].c_str()), d.x = float(atof(f[1].c_str())), d.z = float(atof(f[3].c_str()));
      d.left = float(atof(f[4].c_str())), d.flag = atoi(f[5].c_str());
      if (f.size() == 7) d.name = f[6].substr(0, 40);
      if (ParseY(f[2], &d.y, &d.has_y) && (d.kind == 1 || d.kind == 2) && d.left > 0 && std::isfinite(d.x) && std::isfinite(d.z))
        s.order = d;
    }
  }
  if (parts.size() >= 3 && parts[2].size() > 1 && parts[2][0] == 's') {
    const auto f = Split(parts[2].substr(1), ',');
    if (f.size() == 4) {
      Strike k;
      k.x = float(atof(f[0].c_str())), k.z = float(atof(f[2].c_str())), k.left = float(atof(f[3].c_str()));
      if (ParseY(f[1], &k.y, &k.has_y) && k.left > 0 && std::isfinite(k.x) && std::isfinite(k.z)) s.strike = k;
    }
  }
  return s;
}

// --- projection ---------------------------------------------------------------
// clip = M * (x, y, z, 1) with M column-major. False when the point is at the eye's plane.
bool Project(const float m[16], float x, float y, float z, float w, float h, float* sx, float* sy, float* cw) {
  const float v[4] = {x, y, z, 1.0f};
  float c[4];
  for (int r = 0; r < 4; ++r) {
    c[r] = 0;
    for (int k = 0; k < 4; ++k) c[r] += m[k * 4 + r] * v[k];
  }
  if (std::fabs(c[3]) < 1e-4f) return false;
  *cw = c[3];
  const float nx = c[0] / c[3], ny = c[1] / c[3];
  *sx = (nx * 0.5f + 0.5f) * w;
  *sy = (0.5f - ny * 0.5f) * h;  // (clip y up: checked in game, nearer ground points lower on screen)
  return true;
}

// Where a point is from the view: 0 ahead, 1 left, 2 right, 3 behind.
int Way(const float m[16], float x, float y, float z) {
  float sx, sy, cw;
  if (!Project(m, x, y, z, 2.0f, 2.0f, &sx, &sy, &cw)) return 3;
  if (cw < 0) return 3;
  const float nx = (sx / 2.0f) * 2.0f - 1.0f;
  if (std::fabs(nx) <= 1.0f && std::fabs(sy / 1.0f - 1.0f) <= 1.0f) return 0;
  return nx < 0 ? 1 : 2;
}

const char* WayWord(int w) {
  switch (w) {
    case 0: return "ahead";
    case 1: return "left";
    case 2: return "right";
    default: return "behind";
  }
}

// --- text ---------------------------------------------------------------------
struct Text {
  std::vector<uint8_t> rgba;
  int w = 0, h = 0;
};

#ifdef _WIN32
Text RenderLines(const std::vector<std::wstring>& lines) {
  Text t;
  const int pad = 10, line_h = 30;
  HDC dc = CreateCompatibleDC(nullptr);
  HFONT font = CreateFontW(-24, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Bahnschrift SemiCondensed");
  HGDIOBJ old_font = SelectObject(dc, font);
  int text_w = 0;
  for (const std::wstring& l : lines) {
    SIZE ext{};
    GetTextExtentPoint32W(dc, l.c_str(), int(l.size()), &ext);
    text_w = std::max(text_w, int(ext.cx));
  }
  t.w = text_w + 2 * pad;
  t.h = int(lines.size()) * line_h + pad;
  BITMAPINFO bi{};
  bi.bmiHeader = {sizeof(BITMAPINFOHEADER), t.w, -t.h, 1, 32, BI_RGB};
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  HGDIOBJ old_bmp = SelectObject(dc, bmp);
  memset(bits, 0, size_t(t.w) * t.h * 4);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(245, 245, 245));
  for (size_t i = 0; i < lines.size(); ++i)
    TextOutW(dc, pad, pad / 2 + int(i) * line_h, lines[i].c_str(), int(lines[i].size()));
  GdiFlush();
  // The text's coverage over a near-black panel, see-through like the chat's.
  t.rgba.resize(size_t(t.w) * t.h * 4);
  const auto* px = static_cast<const uint8_t*>(bits);
  for (size_t i = 0; i < size_t(t.w) * t.h; ++i) {
    const double c = std::max({px[i * 4], px[i * 4 + 1], px[i * 4 + 2]}) / 255.0;
    const uint8_t v = uint8_t(std::lround((0.02 + 0.98 * c) * 255));
    t.rgba[i * 4] = t.rgba[i * 4 + 1] = t.rgba[i * 4 + 2] = v;
    t.rgba[i * 4 + 3] = uint8_t(std::lround((0.70 + 0.30 * c) * 255));
  }
  SelectObject(dc, old_bmp);
  DeleteObject(bmp);
  SelectObject(dc, old_font);
  DeleteObject(font);
  DeleteDC(dc);
  return t;
}
#else
Text RenderLines(const std::vector<std::wstring>&) { return {}; }  // (Windows only: the live map too)
#endif

std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

std::string Clock2(float secs) {
  char b[16];
  const int s = std::max(0, int(std::ceil(secs)));
  snprintf(b, sizeof b, "%d:%02d", s / 60, s % 60);
  return b;
}

// --- quads --------------------------------------------------------------------
void Segment(std::vector<Quad>& out, float x0, float y0, float x1, float y1, float t, uint32_t color) {
  const float dx = x1 - x0, dy = y1 - y0, len = std::hypot(dx, dy);
  if (len < 1e-3f) return;
  const float nx = -dy / len * t * 0.5f, ny = dx / len * t * 0.5f;
  Quad q;
  q.x[0] = x0 + nx, q.y[0] = y0 + ny;
  q.x[1] = x1 + nx, q.y[1] = y1 + ny;
  q.x[2] = x1 - nx, q.y[2] = y1 - ny;
  q.x[3] = x0 - nx, q.y[3] = y0 - ny;
  q.color = color;
  out.push_back(q);
}

void Textured(std::vector<Quad>& out, int tex, float cx, float top, float tw, float th, float k) {
  Quad q;
  const float x0 = cx - tw * k / 2, x1 = cx + tw * k / 2, y0 = top, y1 = top + th * k;
  q.x[0] = x0, q.y[0] = y0;
  q.x[1] = x1, q.y[1] = y0;
  q.x[2] = x1, q.y[2] = y1;
  q.x[3] = x0, q.y[3] = y1;
  q.tex = tex;
  out.push_back(q);
}

uint32_t ZoneColor(int kind) { return kind == 2 ? 0x40B0E0u : 0x60E060u; }  // BGR: amber ammo, green heal

// A zone on the ground: a see-through disc with a soft wide edge and a crisp
// inner one. The circle (48 sides at the ground's height) is clipped in clip
// space at the near plane (standing inside one: the part behind the eye goes)
// and on screen at the window's edges, then filled as a fan (it stays convex).
// rgb: 0xBBGGRR. dist: the eye to the centre, for the fade (a far zone fainter).
struct ClipV {
  float v[4];
  bool edge;  // the side from this vertex to the next is the circle's
};
template <typename F>
std::vector<ClipV> ClipBy(const std::vector<ClipV>& in, F side) {
  std::vector<ClipV> out;
  for (size_t i = 0; i < in.size(); ++i) {
    const ClipV& a = in[i];
    const ClipV& b = in[(i + 1) % in.size()];
    const float fa = side(a), fb = side(b);
    if (fa >= 0) out.push_back(a);
    if ((fa >= 0) != (fb >= 0)) {
      const float t = fa / (fa - fb);
      ClipV c;
      for (int j = 0; j < 4; ++j) c.v[j] = a.v[j] + (b.v[j] - a.v[j]) * t;
      c.edge = fa >= 0 ? false : a.edge;  // (leaving: along the cut; entering: the rest of a's side)
      out.push_back(c);
    }
  }
  return out;
}
void Disc(std::vector<Quad>& out, const float m[16], float W, float H, float k, float cx, float cy, float cz, float r,
          uint32_t rgb, float dist) {
  const int n = 48;
  std::vector<ClipV> poly;
  for (int i = 0; i < n; ++i) {
    const float a = float(i) * 6.2831853f / n;
    const float p[4] = {cx + r * std::cos(a), cy, cz + r * std::sin(a), 1.0f};
    ClipV c;
    for (int row = 0; row < 4; ++row) {
      c.v[row] = 0;
      for (int col = 0; col < 4; ++col) c.v[row] += m[col * 4 + row] * p[col];
    }
    c.edge = true;
    poly.push_back(c);
  }
  poly = ClipBy(poly, [](const ClipV& c) { return c.v[3] - 0.05f; });
  if (poly.size() < 3) return;
  for (ClipV& c : poly) {  // to window pixels (x, y in v[0], v[1])
    const float x = (c.v[0] / c.v[3] * 0.5f + 0.5f) * W, y = (0.5f - c.v[1] / c.v[3] * 0.5f) * H;
    c.v[0] = x, c.v[1] = y, c.v[2] = 0, c.v[3] = 1;
  }
  const float pad = 16.0f * k;
  poly = ClipBy(poly, [&](const ClipV& c) { return c.v[0] + pad; });
  poly = ClipBy(poly, [&](const ClipV& c) { return W + pad - c.v[0]; });
  poly = ClipBy(poly, [&](const ClipV& c) { return c.v[1] + pad; });
  poly = ClipBy(poly, [&](const ClipV& c) { return H + pad - c.v[1]; });
  if (poly.size() < 3) return;
  // Inside: fainter (it covers the view), else full up to 80 m, then fading to a third at 400 m.
  const bool inside = dist <= r;
  const float edge_d = std::max(0.0f, dist - r);
  const float fade = std::clamp(1.0f - (edge_d - 80.0f) / 480.0f, 0.35f, 1.0f);
  auto with = [&](float a) { return (uint32_t(std::clamp(a * fade, 0.0f, 1.0f) * 255.0f + 0.5f) << 24) | rgb; };
  const uint32_t fill = with(inside ? 0.14f : 0.22f);
  for (size_t i = 1; i + 1 < poly.size(); ++i) {
    Quad q;
    q.x[0] = poly[0].v[0], q.y[0] = poly[0].v[1];
    q.x[1] = poly[i].v[0], q.y[1] = poly[i].v[1];
    q.x[2] = poly[i + 1].v[0], q.y[2] = poly[i + 1].v[1];
    q.x[3] = q.x[2], q.y[3] = q.y[2];
    q.color = fill;
    out.push_back(q);
  }
  for (size_t i = 0; i < poly.size(); ++i) {
    if (!poly[i].edge) continue;
    const ClipV& a = poly[i];
    const ClipV& b = poly[(i + 1) % poly.size()];
    Segment(out, a.v[0], a.v[1], b.v[0], b.v[1], 12.0f * k, with(0.22f));  // (the glow)
    Segment(out, a.v[0], a.v[1], b.v[0], b.v[1], 4.0f * k, with(0.90f));
  }
}
uint32_t OrderColor(int kind) { return kind == 1 ? 0xE04040E0u : 0xE0F09050u; }  // attack red, defend blue

// Keeps a texture's content: returns true (and builds it) when the key changed.
bool Refresh(const std::string& key, std::string& last, bool& shown, int id, const std::vector<std::wstring>& lines,
             std::vector<Texture>& textures, Text& text) {
  if (lines.empty()) {
    if (shown) textures.push_back({id, 0, 0, {}}), shown = false;
    last.clear();
    return false;
  }
  if (key == last && shown) return false;
  last = key;
  text = RenderLines(lines);
  textures.push_back({id, uint32_t(text.w), uint32_t(text.h), text.rgba});
  shown = true;
  return true;
}
}  // namespace

void Publish(int team, const Snapshot& s) {
  if (team < 1 || team > 2) return;
  std::lock_guard l(g_mu);
  g_state[team].s = s;
  g_state[team].at = Clock::now();
}

void OnPacket(const std::string& body) {
  const int team = MyTeam();
  if (!team) return;
  Publish(team, Decode(body));
}

void Frame(float W, float H, std::vector<Quad>& quads, std::vector<Texture>& textures) {
  static Text panel_text, label_text;
  const int team = MyTeam();
  if (!team || W < 16 || H < 16) {  // (nothing for this game: clear what was shown)
    std::vector<Texture> none;
    Refresh("", g_panel_key, g_panel_shown, kPanelTexture, {}, textures, panel_text);
    Refresh("", g_label_key, g_label_shown, kLabelTexture, {}, textures, label_text);
    return;
  }
  Snapshot s;
  Clock::time_point at;
  {
    std::lock_guard l(g_mu);
    s = g_state[team].s, at = g_state[team].at;
  }
  const float elapsed = float(std::chrono::duration<double>(Clock::now() - at).count());
  const float k = H / 1080.0f;
  float eye[3] = {};
  const bool has_eye = xgpu::CameraEye(eye);
  float vp[16] = {};
  const bool has_vp = xgpu::ViewProjection(vp);
  // The ground at a point: the server's terrain height there, but near the
  // player (60 m) and within 10 m of its own feet its own ground: the level's
  // terrain is not always the surface walked on (Guadalcanal's airfield apron
  // is lower: a ring there was drawn at eye height).
  auto ground = [&](bool has_y, float y, float x, float z) {
    if (!has_eye) return has_y ? y : 0.0f;
    const float mine = eye[1] - kEyeToGround;
    if (!has_y) return mine;
    if (std::hypot(x - eye[0], z - eye[2]) < 60.0f && std::fabs(y - mine) < 10.0f) return mine;
    return y;
  };
  auto dist = [&](float x, float z) { return has_eye ? std::hypot(x - eye[0], z - eye[2]) : 0.0f; };

  // Zones: a ring on the ground, and a line in the panel.
  std::vector<std::wstring> panel;
  std::string panel_key;
  for (const Zone& z : s.zones) {
    const float left = z.left - elapsed;
    if (left <= 0) continue;
    const char* name = z.kind == 2 ? "AMMO ZONE" : "HEAL ZONE";
    std::string line = std::string(name) + "  " + Clock2(left);
    if (has_eye) {
      char b[48];
      snprintf(b, sizeof b, "  %.0f m %s", dist(z.x, z.z), WayWord(Way(vp, z.x, ground(z.has_y, z.y, z.x, z.z), z.z)));
      line += b;
    }
    panel_key += line;
    panel.push_back(Wide(line));
    if (!has_vp) continue;
    Disc(quads, vp, W, H, k, z.x, ground(z.has_y, z.y, z.x, z.z), z.z, kRingRadius, ZoneColor(z.kind),
         has_eye ? dist(z.x, z.z) : 1e9f);
  }
  // The airstrike's warning: a red ring where the blast will be, and its seconds.
  const float strike_left = s.strike.left - elapsed;
  if (strike_left > 0) {
    std::string line = "AIRSTRIKE  " + Clock2(strike_left);
    if (has_eye) {
      char b[48];
      snprintf(b, sizeof b, "  %.0f m %s", dist(s.strike.x, s.strike.z),
               WayWord(has_vp ? Way(vp, s.strike.x, ground(s.strike.has_y, s.strike.y, s.strike.x, s.strike.z), s.strike.z) : 0));
      line += b;
    }
    panel_key += line;
    panel.push_back(Wide(line));
    if (has_vp)
      Disc(quads, vp, W, H, k, s.strike.x, ground(s.strike.has_y, s.strike.y, s.strike.x, s.strike.z), s.strike.z,
           kStrikeRadius, 0x3030E0u, has_eye ? dist(s.strike.x, s.strike.z) : 1e9f);
  }
  if (panel.empty()) panel_key.clear();
  {
    std::vector<std::wstring> lines = panel;
    if (!lines.empty()) lines.insert(lines.begin(), L"COMMANDER");
    std::string key = panel_key;
    if (!lines.empty()) {
      bool changed = Refresh(key, g_panel_key, g_panel_shown, kPanelTexture, lines, textures, panel_text);
      (void)changed;
    } else {
      Refresh("", g_panel_key, g_panel_shown, kPanelTexture, {}, textures, panel_text);
    }
    // (the top right: the game's tickets and flags are at the top centre, the chat at the top left)
    if (g_panel_shown)
      Textured(quads, kPanelTexture, W - 24.0f * k - panel_text.w * k / 2, 24.0f * k, float(panel_text.w), float(panel_text.h), k);
  }

  // The order: a square at its flag (clamped to the window's edge when it is
  // off screen), and its label under it.
  bool label_shown = false;
  const Order& o = s.order;
  const float oleft = o.left - elapsed;
  if (o.kind && oleft > 0) {
    const char* what = o.kind == 1 ? "ATTACK" : "DEFEND";
    char flag[32];
    snprintf(flag, sizeof flag, "flag %d", o.flag);
    std::string line = std::string(what) + " " + (o.name.empty() ? std::string(flag) : o.name) + "  " + Clock2(oleft);
    float cx = W / 2, cy = H / 2;
    bool onscreen = false;
    if (has_vp) {
      float sx = 0, sy = 0, cw = 0;
      const float gy = ground(o.has_y, o.y, o.x, o.z) + 1.5f;
      if (Project(vp, o.x, gy, o.z, W, H, &sx, &sy, &cw)) {
        onscreen = cw > 0.05f && sx >= 0 && sx <= W && sy >= 0 && sy <= H;
        const float m = 40.0f * k;
        if (cw <= 0.05f) {  // (behind: on the bottom edge, on its side)
          cx = std::clamp(W - sx, m, W - m), cy = H - 3 * m;
        } else {
          cx = std::clamp(sx, m, W - m), cy = std::clamp(sy, m, H - m);
        }
      }
    }
    if (has_eye) {
      char b[48];
      snprintf(b, sizeof b, "  %.0f m %s", dist(o.x, o.z), WayWord(has_vp ? Way(vp, o.x, ground(o.has_y, o.y, o.x, o.z) + 1.5f, o.z) : 0));
      line += b;
    }
    (void)onscreen;
    const float h = 14.0f * k, t = 3.0f * k;
    const uint32_t c = OrderColor(o.kind);
    Segment(quads, cx - h, cy - h, cx + h, cy - h, t, c), Segment(quads, cx + h, cy - h, cx + h, cy + h, t, c);
    Segment(quads, cx + h, cy + h, cx - h, cy + h, t, c), Segment(quads, cx - h, cy + h, cx - h, cy - h, t, c);
    const std::vector<std::wstring> lines = {Wide(line)};
    Refresh(line, g_label_key, g_label_shown, kLabelTexture, lines, textures, label_text);
    if (g_label_shown) {
      Textured(quads, kLabelTexture, cx, cy - h - 12.0f * k - label_text.h * k, float(label_text.w), float(label_text.h), k);
      label_shown = true;
    }
  } else {
    Refresh("", g_label_key, g_label_shown, kLabelTexture, {}, textures, label_text);
  }
}
}  // namespace rt::commander::hud
