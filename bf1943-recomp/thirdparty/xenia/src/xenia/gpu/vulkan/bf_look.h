// bf1943 looks for the final frame (shaders/bf_look.xesli), applied in the
// swap's gamma pass: set by the runtime (--look; --blood is bf_gore.h).

#ifndef XENIA_GPU_VULKAN_BF_LOOK_H_
#define XENIA_GPU_VULKAN_BF_LOOK_H_

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>

namespace xe {
namespace gpu {
namespace bflook {

// 0 the game's own, 1 toon, 2 noir, 3 ww2, 4 vivid.
inline std::atomic<int> look{0};
// The picture's BRIGHTNESS (-0.3...0.3, 0 the game's) and GAMMA (0.7...1.3,
// 1 the game's; above 1 brighter midtones), on the final frame.
inline std::atomic<float> brightness{0.0f}, gamma{1.0f};
// The enhanced look's colour grade (0 off, 1 the default; the stock preset
// has none): contrast, vibrance, warm highlights, local contrast.
inline std::atomic<float> grade{0.0f};
// The plants' extra gust sway, metres at a plant's top (0 off). The game's own
// wind is in the shaders; this adds a gust that varies across the plant.
inline std::atomic<float> wind{0.4f};
// The wind's clock (seconds), shared by the vertex push constants and the
// ray-traced shadows' refit.
inline float WindTime() {
  static const auto start = std::chrono::steady_clock::now();
  return std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
}
// The gust at an object-space position: the CPU twin of the translator's
// WindOffset (spirv_shader_translator_alu.cc); keep both the same.
inline void WindGust(const float p[3], float time, float amp, float out[3]) {
  float s1 = std::sin(time * 1.3f + p[0] * 0.21f + p[2] * 0.17f);
  float s2 = std::sin(time * 2.7f + p[0] * -0.31f + p[2] * 0.43f);
  float s3 = std::sin(time * 0.6f + p[0] * 0.07f + p[2] * 0.05f);
  float gust = s1 * 0.5f + (s2 * 0.3f + s3 * 0.4f);
  float weight = amp * std::clamp((p[1] - 0.3f) * 0.45f, 0.0f, 1.0f);
  out[0] = weight * gust;
  out[1] = weight * (s2 * 0.25f);
  out[2] = weight * (gust * 0.6f);
}

}  // namespace bflook
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_BF_LOOK_H_
