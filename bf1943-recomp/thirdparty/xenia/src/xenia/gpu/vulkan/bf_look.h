// bf1943 looks for the final frame (shaders/bf_look.xesli), applied in the
// swap's gamma pass: set by the runtime (--look; --blood is bf_gore.h).

#ifndef XENIA_GPU_VULKAN_BF_LOOK_H_
#define XENIA_GPU_VULKAN_BF_LOOK_H_

#include <atomic>

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

}  // namespace bflook
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_BF_LOOK_H_
