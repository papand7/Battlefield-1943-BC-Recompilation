// The game's own picture of each level (the in-game minimap's texture), as the
// ground under the bots' maps (bot_llm.cpp: the advisor's pictures and the
// training replay's map.png). Read from the player's own game files the first
// time a map needs it (its level archive's texture, decoded here) and kept in
// <user>/minimaps/<map>.png; nothing of the game is shipped. Where on the
// level each picture lies (numbers only, fitted against the level's terrain)
// is in bot_minimap.cpp. The game's own minimap is not touched.
//
//   Battlefield 1943: Textures/UI/Minimap/beach/<map>.res (1024 x 1024 DXT1, an
//     aerial picture: sand, grass, roads, buildings, the sea), in the level's
//     reduced-level-00.fbrb.
//   Bad Company: Terrains/<map>/Textures/<map>_minimap.res (1024 x 1024 DXT1,
//     the terrain's grey shading, rivers dark), in the level's level-00.fbrb.
// Both cover the level's detailed terrain tile (-1024..1024 m), the first with
// x running the other way (east on the left), the second with z (north at the
// bottom).
#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace rt::botllm::minimap {

struct Image {
  int w = 0, h = 0;
  std::vector<uint8_t> rgb;  // w x h, rows as the texture stores them
  // The world square it shows: centre and half its width (metres), and which
  // way its columns and rows run (mirror_x: x falls to the right; mirror_z: z
  // rises downwards).
  float cx = 0, cz = 0, half = 1024;
  bool mirror_x = false, mirror_z = false;
  // Bad Company's: shading only, no colour; its water flat dark (under
  // kGreyWater), the ground's middle brightness `mid`.
  bool grey = false;
  float mid = 128;
  static constexpr float kGreyWater = 54;
  float MetresPerPixel() const { return 2 * half / float(w); }
  // Its colour at (x, z) (0..255 each, bilinear); false outside it.
  bool At(float x, float z, float rgb_out[3]) const;
};

// Starts making the map's picture ready on a thread of its own (from the cache,
// else the game's files), once per map; nothing for a map without one.
void Prepare(const std::string& map);
// The map's picture; null for a map without one (or that failed), or (wait
// false) while it is being made.
std::shared_ptr<const Image> Get(const std::string& map, bool wait);

// The work itself, without the game (also for tools): the picture of `map`
// from the game files in game_root (the folder with dist/), kept in cache_dir
// (empty: not kept, nor looked for there). Null and why when there is none.
std::shared_ptr<Image> Load(const std::filesystem::path& game_root, const std::filesystem::path& cache_dir,
                            const std::string& map, std::string* why);

}  // namespace rt::botllm::minimap
