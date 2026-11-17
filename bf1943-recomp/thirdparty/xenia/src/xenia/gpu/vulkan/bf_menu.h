// bf1943: the front end's menu rows as drawn, for mouse hover (runtime
// xam.cpp). The game's menus (EA APT movies) only take pad input; their solid
// 2D boxes (vertex shaders that transform FMT_32_32_FLOAT positions by c0-c3,
// colour in c8) are collected each frame, and the input code steps the
// highlight (D-pad) to the row under the cursor.

#ifndef XENIA_GPU_VULKAN_BF_MENU_H_
#define XENIA_GPU_VULKAN_BF_MENU_H_

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace xe {
namespace gpu {
namespace bfmenu {

struct Box {
  float x0, y0, x1, y1;  // 0-1 of the frame, y down
  float r, g, b, a;      // the draw's colour (c8)
};

// The Back prompt's key icon: its cell in a UI atlas (registered by the
// runtime when it redraws the atlas, ui pc_ui.cpp), so the prompt can be
// clicked.
struct Icon {
  uint32_t address, width, height;
  float u0, v0, u1, v1;  // texels
};

// Hardcore mode (runtime xgpu::SetHideUi): the game's 2D UI draws skipped.
inline std::atomic<bool> g_hide_ui{false};
inline std::atomic<bool> g_hide_ui_screen{false};  // (with it: Bad Company's screen-space UI shaders too)

// Ultrawide (runtime xgpu::SetWide; a window wider than 16:9). The game still
// renders its 16:9-sized frame, but its 3D views are built at the window's
// aspect (game_patches.cpp, the views' update) and the presenter stretches
// the frame over the whole window, so the 3D picture is undistorted and
// wider. The 2D UI draws (HUD, menus) are squeezed back to their own
// proportions by g_ui_scale (16:9 over the window's aspect, about the
// centre); in the HUD layout "edges" the bottom corners' elements (minimap,
// ammo) are also moved out by g_ui_shift (NDC) to the edges of a HUD area at
// most 21:9 wide (vulkan_command_processor.cc BfUiWideTransform).
inline std::atomic<bool> g_wide{false};      // the mode is on (any window)
inline std::atomic<int> g_wide_hud{1};       // 0: 16:9 in the centre, 1: edges
inline std::atomic<float> g_ui_scale{1.0f};  // 1: not wide right now
inline std::atomic<float> g_ui_shift{0.0f};
// A menu is up (front end, spawn screen, pause, scoreboard): its draws stay
// together in the centre.
inline std::atomic<bool> g_ui_menu{true};
// The window's aspect for the 3D views (16:9 or wider).
inline std::atomic<float> g_view_aspect{16.0f / 9.0f};
inline void UpdateWide(uint32_t width, uint32_t height) {
  // (A minimised window, 0 x 0, keeps the last size's.)
  static std::atomic<uint32_t> last_width{0}, last_height{0};
  if (width && height) last_width = width, last_height = height;
  width = last_width, height = last_height;
  float aspect = width && height ? float(width) / float(height) : 16.0f / 9.0f;
  if (!g_wide || !(aspect > 16.0f / 9.0f + 0.01f)) aspect = 16.0f / 9.0f;
  g_view_aspect = aspect;
  const float scale = (16.0f / 9.0f) / aspect;
  g_ui_scale = scale;
  // The HUD area: 21:9 (64:27) at most; its edges at +-hud/aspect in NDC.
  const float hud = std::min(aspect, 64.0f / 27.0f);
  g_ui_shift = g_wide_hud ? hud / aspect - scale : 0.0f;
}

inline std::mutex g_mutex;
inline std::vector<Box> g_building, g_done;
inline std::vector<Icon> g_back_icons;
inline std::vector<Box> g_back_building, g_back_done;
inline void AddBackIcon(const Icon& icon) {
  std::lock_guard lock(g_mutex);
  g_back_icons.push_back(icon);
}
// A drawn Back prompt icon: its screen box, the prompt's label to its right
// included.
inline void AddBack(const Box& box) {
  std::lock_guard lock(g_mutex);
  if (g_back_building.size() < 16) g_back_building.push_back(box);
}
inline std::vector<Box> BackBoxes() {
  std::lock_guard lock(g_mutex);
  return g_back_done;
}
// Filled by the command processor during a frame, published at the swap.
inline void AddBox(const Box& box) {
  std::lock_guard lock(g_mutex);
  if (g_building.size() < 512) g_building.push_back(box);
}
inline void EndFrame() {
  std::lock_guard lock(g_mutex);
  g_done.swap(g_building);
  g_building.clear();
  g_back_done.swap(g_back_building);
  g_back_building.clear();
}
// The last finished frame's boxes.
inline std::vector<Box> Boxes() {
  std::lock_guard lock(g_mutex);
  return g_done;
}

}  // namespace bfmenu
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_BF_MENU_H_
