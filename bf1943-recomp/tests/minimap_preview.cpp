// Offline check of the bots' maps over the game's own level pictures
// (runtime/bot_minimap.cpp, runtime/bot_map.cpp), without the game: reads the
// minimap from the game files as the runtime does (and keeps it in --cache),
// optionally compares its decoding with a PNG decoded elsewhere (--ref, e.g.
// the HD texture pack's), and draws side-by-side pictures, the current map
// (left) and the one over the game's picture (right), from the level's terrain
// heightfield (tools/fbrb.py extract <terrain-00.fbrb> <dir> TerrainHeightfield;
// the -000 tile). The walkable cells are a stand-in (slopes under 35 degrees
// above --sea), and there are no structures (the game's collision): those come
// from the running game.
//
// Build (Windows, clang-cl; from bf1943-recomp):
//   clang-cl /nologo /O2 /EHsc -std:c++20 -DBOT_MINIMAP_OFFLINE -Iruntime tests/minimap_preview.cpp
//            runtime/bot_minimap.cpp runtime/bot_map.cpp /Fe:minimap_preview.exe
// Run:
//   minimap_preview --game <folder with dist/> --map Wake_island_s --hf <Heightfield-000.res> --out <folder>
//                   [--cache <folder>] [--ref <png>] [--sea <height>] [--bounds cx cz half]
//                   [--objectives x,z;x,z;...] [--advisor x,z[,yaw]]...
#include "bot_llm.h"
#include "bot_minimap.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#include "../thirdparty/xenia/third_party/stb/stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../thirdparty/xenia/third_party/stb/stb_image_write.h"

using namespace rt::botllm;

namespace {

struct Heightfield {
  float x0, y0, z0, x1, y1, z1;
  int n = 0;
  std::vector<uint16_t> s;
  bool Read(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (d.size() < 0x2E) return false;
    auto be = [&](size_t o) { return uint32_t(d[o]) << 24 | uint32_t(d[o + 1]) << 16 | uint32_t(d[o + 2]) << 8 | d[o + 3]; };
    float f[6];
    for (int i = 0; i < 6; ++i) {
      uint32_t u = be(4 * i);
      memcpy(&f[i], &u, 4);
    }
    x0 = f[0], y0 = f[1], z0 = f[2], x1 = f[3], y1 = f[4], z1 = f[5];
    n = int(be(24));
    if (d.size() < 0x2E + size_t(n) * n * 2) return false;
    s.resize(size_t(n) * n);
    for (size_t i = 0; i < s.size(); ++i) s[i] = uint16_t(d[0x2E + 2 * i] | d[0x2E + 2 * i + 1] << 8);
    return true;
  }
  float At(float x, float z) const {
    float cell = (x1 - x0) / n;
    if (x < x0 || z < z0 || x >= x1 || z >= z1) return NAN;
    int c = std::clamp(int((x - x0) / cell), 0, n - 1), r = std::clamp(int((z - z0) / cell), 0, n - 1);
    return y0 + s[size_t(r) * n + c] / 65535.0f * (y1 - y0);
  }
};

struct Pixels {
  int w = 0, h = 0;
  std::vector<uint8_t> rgb;
};
Pixels FromPng(const std::vector<uint8_t>& png) {
  Pixels p;
  int n;
  uint8_t* d = stbi_load_from_memory(png.data(), int(png.size()), &p.w, &p.h, &n, 3);
  if (d) p.rgb.assign(d, d + size_t(p.w) * p.h * 3), stbi_image_free(d);
  return p;
}
// Two pictures side by side, 8 px apart.
void SideBySide(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, const std::string& path) {
  Pixels l = FromPng(a), r = FromPng(b);
  int w = l.w + 8 + r.w, h = std::max(l.h, r.h);
  std::vector<uint8_t> out(size_t(w) * h * 3, 24);
  for (int y = 0; y < l.h; ++y) memcpy(&out[size_t(y) * w * 3], &l.rgb[size_t(y) * l.w * 3], size_t(l.w) * 3);
  for (int y = 0; y < r.h; ++y) memcpy(&out[(size_t(y) * w + l.w + 8) * 3], &r.rgb[size_t(y) * r.w * 3], size_t(r.w) * 3);
  stbi_write_png(path.c_str(), w, h, 3, out.data(), w * 3);
  printf("wrote %s\n", path.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  std::string game, map, hf_path, out = ".", cache, ref, objectives;
  float sea = -1e9f, bounds[3] = {NAN, NAN, NAN};
  std::vector<std::array<float, 3>> advisors;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
    if (a == "--game") game = next();
    else if (a == "--map") map = next();
    else if (a == "--hf") hf_path = next();
    else if (a == "--out") out = next();
    else if (a == "--cache") cache = next();
    else if (a == "--ref") ref = next();
    else if (a == "--sea") sea = std::stof(next());
    else if (a == "--objectives") objectives = next();
    else if (a == "--bounds") bounds[0] = std::stof(next()), bounds[1] = std::stof(next()), bounds[2] = std::stof(next());
    else if (a == "--advisor") {
      std::array<float, 3> p{0, 0, 0};
      sscanf(next().c_str(), "%f,%f,%f", &p[0], &p[1], &p[2]);
      advisors.push_back(p);
    }
  }
  if (game.empty() || map.empty() || hf_path.empty()) {
    fprintf(stderr, "usage: see the top of tests/minimap_preview.cpp\n");
    return 2;
  }
  auto t0 = std::chrono::steady_clock::now();
  std::string why;
  std::shared_ptr<minimap::Image> pic = minimap::Load(game, cache, map, &why);
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  if (!pic) {
    fprintf(stderr, "no minimap: %s\n", why.c_str());
    return 1;
  }
  printf("%s: minimap %dx%d (%s), %.2f m a pixel, square centre %.2f %.2f half %.2f, mirror x %d z %d, in %.2f s\n",
         map.c_str(), pic->w, pic->h, pic->grey ? "grey" : "colour", pic->MetresPerPixel(), pic->cx, pic->cz, pic->half,
         pic->mirror_x, pic->mirror_z, secs);
  if (!ref.empty()) {  // (the same decoding as the texture pack's?)
    int w, h, n;
    if (uint8_t* d = stbi_load(ref.c_str(), &w, &h, &n, 3)) {
      if (w != pic->w || h != pic->h) printf("ref: size %dx%d differs\n", w, h);
      else {
        int worst = 0;
        double sum = 0;
        for (size_t i = 0; i < pic->rgb.size(); ++i) {
          int e = std::abs(int(d[i]) - int(pic->rgb[i]));
          worst = std::max(worst, e), sum += e;
        }
        printf("ref: largest difference %d, mean %.4f (of 255)\n", worst, sum / pic->rgb.size());
      }
      stbi_image_free(d);
    }
  }
  Heightfield hf;
  if (!hf.Read(hf_path)) {
    fprintf(stderr, "cannot read %s\n", hf_path.c_str());
    return 1;
  }
  MapView level;
  std::vector<std::pair<float, float>> flags;
  for (size_t p = 0; p < objectives.size();) {
    float x, z;
    if (sscanf(objectives.c_str() + p, "%f,%f", &x, &z) == 2) flags.push_back({x, z});
    size_t semi = objectives.find(';', p);
    if (semi == std::string::npos) break;
    p = semi + 1;
  }
  float ys = 0;
  int yn = 0;
  for (auto [x, z] : flags)
    if (std::isfinite(hf.At(x, z))) ys += hf.At(x, z), ++yn;
  level.y = yn ? ys / yn : (hf.y0 + hf.y1) / 2;
  level.ground = [&](float x, float z) { return hf.At(x, z); };
  level.walkable = [&](float x, float, float z) {  // (a stand-in for the navigation mesh)
    float c = hf.At(x, z), e = hf.At(x + 1, z), s = hf.At(x, z + 1);
    return std::isfinite(c + e + s) && c > sea && std::hypot(e - c, s - c) < 0.7f;
  };
  if (std::isfinite(bounds[0])) level.cx = bounds[0], level.cz = bounds[1], level.half = bounds[2];
  else if (!flags.empty()) {  // (as the games do: the flags' extent and 350 m round)
    float x0 = 1e9f, x1 = -1e9f, z0 = 1e9f, z1 = -1e9f;
    for (auto [x, z] : flags) x0 = std::min(x0, x), x1 = std::max(x1, x), z0 = std::min(z0, z), z1 = std::max(z1, z);
    level.cx = (x0 + x1) / 2, level.cz = (z0 + z1) / 2, level.half = std::max(x1 - x0, z1 - z0) / 2 + 350.0f;
  } else {
    level.cx = (hf.x0 + hf.x1) / 2, level.cz = (hf.z0 + hf.z1) / 2, level.half = (hf.x1 - hf.x0) / 2;
  }
  for (auto [x, z] : flags) level.markers.push_back({Marker::kObjective, x, z});
  printf("the level's map: centre %.1f %.1f, half %.1f\n", level.cx, level.cz, level.half);
  t0 = std::chrono::steady_clock::now();
  std::vector<uint8_t> before = DrawMap(level, 2048, nullptr);
  std::vector<uint8_t> after = DrawMap(level, 2048, pic.get());
  printf("drawn (2048 x 2) in %.1f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
  std::string base = out + "/" + map;
  std::ofstream(base + "_map_current.png", std::ios::binary).write(reinterpret_cast<const char*>(before.data()), std::streamsize(before.size()));
  std::ofstream(base + "_map_minimap.png", std::ios::binary).write(reinterpret_cast<const char*>(after.data()), std::streamsize(after.size()));
  SideBySide(before, after, base + "_map_side_by_side.png");
  // The advisor's pictures (256 px, 80 m across): a bot, teammates, enemies,
  // its objective and numbered places, as Ask draws them.
  int k = 0;
  for (auto [x, z, yaw] : advisors) {
    MapView m = level;
    m.cx = x, m.cz = z, m.half = 40, m.y = hf.At(x, z);
    m.markers.clear();
    m.markers.push_back({Marker::kSelf, x, z, yaw});
    m.markers.push_back({Marker::kFriend, x - 8, z + 5});
    m.markers.push_back({Marker::kEnemy, x + 22, z + 18});
    m.markers.push_back({Marker::kEnemy, x + 26, z + 10});
    if (!flags.empty()) {  // (the nearest flag, if it is on the picture)
      auto f = *std::min_element(flags.begin(), flags.end(), [&](auto& a, auto& b) {
        return std::hypot(a.first - x, a.second - z) < std::hypot(b.first - x, b.second - z);
      });
      m.markers.push_back({Marker::kObjective, f.first, f.second});
    }
    const float off[5][2] = {{-15, 10}, {12, -14}, {-20, -12}, {6, 20}, {18, 2}};
    for (int p = 0; p < 5; ++p) {
      Marker place{Marker::kPlace, x + off[p][0], z + off[p][1]};
      place.label = p + 1;
      m.markers.push_back(place);
    }
    std::vector<uint8_t> a = DrawMap(m, 256, nullptr), b = DrawMap(m, 256, pic.get());
    SideBySide(a, b, base + "_advisor" + std::to_string(++k) + "_side_by_side.png");
  }
  return 0;
}
