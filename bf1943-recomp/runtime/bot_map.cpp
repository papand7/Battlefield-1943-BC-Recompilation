// The bots' top-down maps (bot_llm.h: DrawMap): the advisor's pictures and the
// training replay's whole level. Apart from the rest of the advisor so that it
// can be drawn without the game too (tests/minimap_preview.cpp).
#include "bot_llm.h"
#include "bot_minimap.h"

#ifndef BOT_MINIMAP_OFFLINE
#include "kernel.h"
#else
#include <cstdio>
#define RT_INFO(...) (std::printf(__VA_ARGS__), std::printf("\n"))
#endif

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../thirdparty/xenia/third_party/stb/stb_image_write.h"

namespace rt::botllm {
namespace {

struct Canvas {
  int n;  // pixels each way
  std::vector<uint8_t> px;
  explicit Canvas(int size) : n(size), px(size_t(size) * size * 3) {}
  void Put(int x, int y, uint32_t rgb, float a = 1.0f) {
    if (x < 0 || y < 0 || x >= n || y >= n) return;
    uint8_t* p = &px[(y * n + x) * 3];
    // (signed: a darker colour over a lighter pixel wrapped round as unsigned,
    // black or garbage where it should have shaded)
    for (int c = 0; c < 3; ++c) {
      int to = int(rgb >> (16 - 8 * c) & 255);
      p[c] = uint8_t(std::clamp(int(std::lround(p[c] + (to - int(p[c])) * a)), 0, 255));
    }
  }
  void Disc(float cx, float cy, float r, uint32_t rgb) {
    for (int y = int(cy - r - 1); y <= int(cy + r + 1); ++y)
      for (int x = int(cx - r - 1); x <= int(cx + r + 1); ++x) {
        float d = std::hypot(x + 0.5f - cx, y + 0.5f - cy);
        if (d < r) Put(x, y, rgb, std::min(1.0f, r - d));
      }
  }
  void Box(float cx, float cy, float r, uint32_t rgb) {
    for (int y = int(cy - r); y <= int(cy + r); ++y)
      for (int x = int(cx - r); x <= int(cx + r); ++x) Put(x, y, rgb);
  }
  void Line(float x0, float y0, float x1, float y1, float w, uint32_t rgb) {
    int n = int(std::hypot(x1 - x0, y1 - y0) * 2) + 1;
    for (int i = 0; i <= n; ++i) Disc(x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n, w, rgb);
  }
  void Digit(float cx, float cy, int d, uint32_t rgb) {  // 3x5, 2 px a dot
    static const char* k[10] = {"111101101101111", "010110010010111", "111001111100111", "111001111001111",
                                "101101111001001", "111100111001111", "111100111101111", "111001001001001",
                                "111101111101111", "111101111001111"};
    for (int r = 0; r < 5; ++r)
      for (int c = 0; c < 3; ++c)
        if (k[d % 10][r * 3 + c] == '1') Box(cx - 2.5f + c * 2, cy - 4.5f + r * 2, 0.5f, rgb);
  }
};

uint32_t Rgb(float r, float g, float b) {
  auto c = [](float v) { return uint32_t(std::clamp(v, 0.0f, 255.0f)); };
  return c(r) << 16 | c(g) << 8 | c(b);
}

}  // namespace

std::vector<uint8_t> DrawMap(const MapView& m, int n, const minimap::Image* picture) {
  Canvas cv(n);
  const float mpp = 2 * m.half / n;  // metres a pixel
  auto toX = [&](float x) { return (x - (m.cx - m.half)) / mpp; };
  auto toY = [&](float z) { return ((m.cz + m.half) - z) / mpp; };  // (north up)
  // Ground: height-tinted, lit from the north-west.
  std::vector<float> h(n * n, NAN);
  if (m.ground)
    for (int y = 0; y < n; ++y)
      for (int x = 0; x < n; ++x) h[y * n + x] = m.ground(m.cx - m.half + (x + 0.5f) * mpp, m.cz + m.half - (y + 0.5f) * mpp);
  // The game's own picture of the level (bot_minimap.h), where it reaches: the
  // ground's colours instead of the tint, with our relief over them the more,
  // the coarser it is here (2 m a pixel: the whole level's 0.8 m, the
  // advisor's 0.3 to 0.4 m).
  std::vector<float> base;  // RGB a pixel, NAN outside it
  float relief = 0;
  if (picture) {
    base.assign(size_t(n) * n * 3, NAN);
    for (int y = 0; y < n; ++y)
      for (int x = 0; x < n; ++x)
        picture->At(m.cx - m.half + (x + 0.5f) * mpp, m.cz + m.half - (y + 0.5f) * mpp, &base[(size_t(y) * n + x) * 3]);
    relief = std::clamp((picture->MetresPerPixel() / mpp - 1) / 3, 0.0f, 1.0f) * (picture->grey ? 0.35f : 0.6f);
  }
  auto base_at = [&](size_t i) -> const float* {
    return !base.empty() && std::isfinite(base[i * 3]) ? &base[i * 3] : nullptr;
  };
  // Where soldiers walk: cells of 4 x 4 pixels, each asked once.
  const int kCell = 4, kCells = n / kCell;
  std::vector<char> walk;
  float sea = -1e9f;  // the sea's level: as low as the ground soldiers walk on goes (none known: no water drawn)
  if (m.walkable) {
    walk.assign(size_t(kCells) * kCells, 0);
    std::vector<float> walked;
    for (int cy = 0; cy < kCells; ++cy)
      for (int cx = 0; cx < kCells; ++cx) {
        float wx = m.cx - m.half + (cx + 0.5f) * kCell * mpp, wz = m.cz + m.half - (cy + 0.5f) * kCell * mpp;
        float gy = h[(cy * kCell + kCell / 2) * n + cx * kCell + kCell / 2];
        bool ok = std::isfinite(gy) && gy > -1e8f;
        walk[size_t(cy) * kCells + cx] = m.walkable(wx, ok ? gy : m.y, wz);
        if (ok && walk[size_t(cy) * kCells + cx]) walked.push_back(gy);
      }
    // (a whole level's only: its beaches go down to the water; the bots' own
    // small maps stay as the advisor's prompt describes them)
    if (n >= 1024 && walked.size() > 200) {
      std::nth_element(walked.begin(), walked.begin() + walked.size() / 100, walked.end());
      sea = walked[walked.size() / 100];
    }
  }
  // The structures (below), and the sea's surface among them: a flat
  // collision plane over the sea bed, the commonest height of what lies over
  // the ground (over a tenth of the picture). Its level is the sea's.
  std::vector<float> top;
  const bool structures = m.tops && m.tops(m.cx - m.half, m.cz + m.half, mpp, n, top);
  std::vector<float> waters;  // the water surfaces' levels
  if (structures) {
    std::map<int, int> heights;  // (half metres)
    for (size_t i = 0; i < top.size(); ++i)
      if (std::isfinite(top[i]) && (!std::isfinite(h[i]) || top[i] > h[i] + 1.0f)) ++heights[int(std::lround(top[i] * 2))];
    auto most = std::max_element(heights.begin(), heights.end(), [](auto& a, auto& b) { return a.second < b.second; });
    if (most != heights.end() && most->second > int(top.size() / 10)) waters.push_back(most->first / 2.0f);
    // Less of the picture (a 200th each) for rivers, lakes and harbours (Bad
    // Company's levels; Valley Run has two: the reservoir above its dam, the
    // river below), up to three: no roof is that large and flat.
    else if (n >= 1024) {
      std::vector<std::pair<int, int>> by(heights.begin(), heights.end());
      std::sort(by.begin(), by.end(), [](auto& a, auto& b) { return a.second > b.second; });
      for (size_t i = 0; i < by.size() && waters.size() < 3; ++i)
        if (by[i].second > int(top.size() / 200)) waters.push_back(by[i].first / 2.0f);
    }
    if (!waters.empty()) sea = *std::min_element(waters.begin(), waters.end());
    if (n >= 1024) {  // (the whole level's: what it found)
      std::vector<std::pair<int, int>> by(heights.begin(), heights.end());
      std::sort(by.begin(), by.end(), [](auto& a, auto& b) { return a.second > b.second; });
      std::string common;
      for (size_t i = 0; i < by.size() && i < 4; ++i)
        common += " " + std::to_string(by[i].first / 2.0f).substr(0, 6) + " m (" + std::to_string(by[i].second) + ")";
      std::string found;
      for (float w : waters) found += " " + std::to_string(w).substr(0, 6) + " m";
      RT_INFO("bot llm: the level's map: water%s; the commonest levels over the ground:%s",
              found.empty() ? " none" : found.c_str(), common.c_str());
    }
  }
  // The water surface at pixel i (its level), or NAN.
  auto water_at = [&](size_t i) {
    if (std::isfinite(top[i]))
      for (float w : waters)
        if (std::fabs(top[i] - w) < 0.6f) return w;
    return NAN;
  };
  auto is_water = [&](size_t i) { return std::isfinite(water_at(i)); };
  // The game's picture's colour b at height tint t, our light lit over it. Bad
  // Company's grey one: its shading over our tint, its flat dark water blue.
  auto over = [&](const float* b, float t, float lit) {
    float k = 1 + (lit - 1) * relief;
    if (!picture->grey) return Rgb(b[0] * k, b[1] * k, b[2] * k);
    float lum = (b[0] + b[1] + b[2]) / 3, s = lum / picture->mid * k;
    float wet = std::clamp((minimap::Image::kGreyWater + 2 - lum) / 6, 0.0f, 1.0f);
    float r = (90 + 80 * t) * s, g = (120 + 60 * t) * s, bl = (60 + 40 * t) * s;
    return Rgb(r + (55 - r) * wet, g + (115 - g) * wet, bl + (150 - bl) * wet);
  };
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x) {
      const size_t i = size_t(y) * n + x;
      const float* b = base_at(i);
      float v = h[i];
      if (!std::isfinite(v) || v < -1e8f) {
        cv.Put(x, y, b ? over(b, 0.5f, 1.0f) : 0x5A6E78);
        continue;
      }
      float t = std::clamp((v - m.y) / 30.0f * 0.5f + 0.5f, 0.0f, 1.0f);  // (30 m below to 30 m above it)
      // Water: under a water surface, or (no surfaces found) below the sea's
      // level; lighter in the shallows. (Over the game's picture: its own sea;
      // Bad Company's grey one tinted blue.)
      const float w = structures ? water_at(i) : NAN;
      if ((std::isfinite(w) && v < w) || (waters.empty() && v < sea - 0.3f)) {
        float d = std::clamp(((std::isfinite(w) ? w : sea) - v) / 15.0f, 0.0f, 1.0f);
        uint32_t water = Rgb(70 - 40 * d, 140 - 70 * d, 170 - 60 * d);
        if (!b) cv.Put(x, y, water);
        else {
          cv.Put(x, y, over(b, t, 1.0f));
          if (picture->grey) cv.Put(x, y, water, 0.55f);
        }
        continue;
      }
      float gx = x > 0 && x + 1 < n ? h[y * n + x + 1] - h[y * n + x - 1] : 0;
      float gy = y > 0 && y + 1 < n ? h[(y + 1) * n + x] - h[(y - 1) * n + x] : 0;
      float lit = std::isfinite(gx + gy) ? std::clamp(1.0f + (-gx - gy) / (4 * mpp) * 0.6f, 0.6f, 1.4f) : 1.0f;
      cv.Put(x, y, b ? over(b, t, lit) : Rgb((90 + 80 * t) * lit, (120 + 60 * t) * lit, (60 + 40 * t) * lit));
    }
  // Structures (buildings, walls, bunkers, rocks, ships): what stands over a
  // metre above the ground, roofs lighter the taller, lit as the ground, their
  // outlines dark. (Not the sea's surface: drawn as water above.) Over the
  // game's picture, its own buildings show through a little.
  if (structures) {
    auto above = [&](int x, int y) {
      if (x < 0 || y < 0 || x >= n || y >= n) return -1.0f;
      float t = top[size_t(y) * n + x], g = h[size_t(y) * n + x];
      if (!std::isfinite(t) || is_water(size_t(y) * n + x)) return -1.0f;
      return std::isfinite(g) && g > -1e8f ? t - g : 99.0f;  // (off the terrain: ships, piers)
    };
    for (int y = 0; y < n; ++y)
      for (int x = 0; x < n; ++x) {
        float a = above(x, y);
        if (a < 1.0f) continue;
        const float* t = &top[size_t(y) * n + x];
        float gx = x > 0 && x + 1 < n ? t[1] - t[-1] : 0, gy = y > 0 && y + 1 < n ? t[n] - t[-n] : 0;
        float lit = std::isfinite(gx + gy) ? std::clamp(1.0f + (-gx - gy) / (4 * mpp) * 0.5f, 0.55f, 1.35f) : 1.0f;
        bool edge = false;
        for (auto [dx, dy] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
          float b = above(x + dx, y + dy);
          if (b < 1.0f || std::fabs(top[size_t(y + dy) * n + x + dx] - *t) > 2.5f) edge = true;
        }
        float k = std::clamp(a / 15.0f, 0.0f, 1.0f) * 50 + 150;
        float r = k * lit, g = (k - 8) * lit, b = (k - 22) * lit;
        if (edge) r *= 0.45f, g *= 0.45f, b *= 0.45f;
        cv.Put(x, y, Rgb(r, g, b), !picture ? 1.0f : edge ? 0.9f : 0.65f);
      }
  }
  // Where nobody walks, darker (on land: the water shows it; with the
  // structures drawn, lighter: they show it already; over the game's picture,
  // a little lighter: it shows through).
  if (!walk.empty()) {
    const float dark = structures ? 0.35f : picture ? 0.6f : 0.8f;
    for (int cy = 0; cy < kCells; ++cy)
      for (int cx = 0; cx < kCells; ++cx) {
        if (walk[size_t(cy) * kCells + cx]) continue;
        float gy = h[(cy * kCell + kCell / 2) * n + cx * kCell + kCell / 2];
        if (std::isfinite(gy) && gy < sea - 0.3f) continue;
        for (int y = 0; y < kCell; ++y)
          for (int x = 0; x < kCell; ++x) cv.Put(cx * kCell + x, cy * kCell + y, 0x2A2A2A, dark);
      }
  }
  // A 10 m scale bar, bottom left.
  cv.Line(6, n - 6, 6 + 10 / mpp, n - 6, 1, 0xFFFFFF);
  for (const Marker& k : m.markers) {
    float x = toX(k.x), y = toY(k.z);
    switch (k.kind) {
      case Marker::kFriend: cv.Disc(x, y, 3.5f, 0x103070), cv.Disc(x, y, 2.5f, 0x40A0FF); break;
      case Marker::kEnemy: cv.Disc(x, y, 4.5f, 0x400000), cv.Disc(x, y, 3.5f, 0xFF2020); break;
      case Marker::kObjective: cv.Box(x, y, 5, 0x000000), cv.Box(x, y, 4, 0xFFE020); break;
      case Marker::kObjectiveEnemy: cv.Box(x, y, 5, 0x000000), cv.Box(x, y, 4, 0xFF8000); break;
      case Marker::kVehicle: cv.Box(x, y, 4, 0x000000), cv.Box(x, y, 3, 0xA0A0A0); break;
      case Marker::kPlace: cv.Disc(x, y, 8.5f, 0x000000), cv.Disc(x, y, 7.5f, 0xFFFFFF), cv.Digit(x, y, k.label, 0x000000); break;
      case Marker::kSelf: break;
    }
  }
  for (const Marker& k : m.markers)  // (on top)
    if (k.kind == Marker::kSelf) {
      float x = toX(k.x), y = toY(k.z);
      cv.Line(x, y, x + 12 * std::sin(k.yaw), y - 12 * std::cos(k.yaw), 1.2f, 0xFFFFFF);
      cv.Disc(x, y, 5.5f, 0x000000), cv.Disc(x, y, 4.5f, 0xFFFFFF);
    }
  std::vector<uint8_t> png;
  stbi_write_png_to_func([](void* ctx, void* data, int size) {
    auto* v = static_cast<std::vector<uint8_t>*>(ctx);
    v->insert(v->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
  }, &png, n, n, 3, cv.px.data(), n * 3);
  return png;
}

}  // namespace rt::botllm
