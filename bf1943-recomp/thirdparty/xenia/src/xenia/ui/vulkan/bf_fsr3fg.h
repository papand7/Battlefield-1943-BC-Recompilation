// bf1943: AMD FSR 3.1 frame generation (thirdparty/fidelityfx_fg, the FSR SDK
// 2.4 FFX API; amd_fidelityfx_loader_vk.dll and
// amd_fidelityfx_framegeneration_vk.dll next to the exe, loaded at run time, so
// the game runs without them: the option then just stays off).
//
// The presenter's VkSwapchainKHR is replaced by the SDK's frame generation
// swapchain (the presenter calls the wrappers below instead of the
// vkCreate/Destroy/GetImages/Acquire/Present entry points; with the option off
// they are the plain Vulkan calls). The scene's depth and motion vectors and
// the upscaled frame without the HUD come from the command processor
// (gpu/vulkan/bf_fsr3.cc, Publish), once per game frame; at each paint of a new
// game frame the presenter records the frame generation "prepare" pass from
// them, and the swapchain then interpolates one frame between the previous
// and this one inside the present call and paces both presents.

#ifndef XENIA_UI_VULKAN_BF_FSR3FG_H_
#define XENIA_UI_VULKAN_BF_FSR3FG_H_

#include <atomic>
#include <cstdint>

#include "xenia/ui/vulkan/vulkan_api.h"

namespace xe {
namespace ui {
namespace vulkan {

class VulkanDevice;

namespace bffg {

// The option (--frame-gen, the FRAME GENERATION setting), set before the
// Vulkan device is made: the device then gets the queues and the timeline
// semaphore feature the swapchain needs.
inline std::atomic<bool> requested{false};
// Queues the swapchain wants besides the presenter's own (present, async
// compute, image acquire), in the graphics queue family.
constexpr uint32_t kExtraQueues = 3;

// Presented frames (real and generated), for the window title.
inline std::atomic<uint64_t> presented{0};
// The swapchain is the frame generation one now (the option is on and it
// started).
inline std::atomic<bool> active{false};

// One game frame's inputs, images in VK_IMAGE_LAYOUT_GENERAL (the command
// processor owns them: a ring, so they stay valid for the next few frames).
struct Snapshot {
  uint64_t seq = 0;  // 0: none yet
  VkImage depth = VK_NULL_HANDLE;    // R32_SFLOAT, reversed, render size
  VkImage motion = VK_NULL_HANDLE;   // R16G16_SFLOAT, render pixels
  VkImage hudless = VK_NULL_HANDLE;  // R8G8B8A8_UNORM, display size: no HUD
  uint32_t render_width = 0, render_height = 0;
  uint32_t display_width = 0, display_height = 0;
  float jitter_x = 0.0f, jitter_y = 0.0f;  // render pixels
  float frame_ms = 16.7f;
  float near_plane = 0.1f, far_plane = 10000.0f, fov_y = 1.0f;
  bool reset = false;
};

// The DLLs are there and load (once).
bool Available();
// From the command processor, once per game frame (thread-safe).
void Publish(const Snapshot& snapshot);

// The presenter's swapchain entry points. CreateSwapchain destroys old_swapchain
// itself when it makes the frame generation one (DestroySwapchain of that
// handle afterwards does nothing); where frame generation is off or cannot
// start it is vkCreateSwapchainKHR with old_swapchain.
VkResult CreateSwapchain(const VulkanDevice* device,
                         const VkSwapchainCreateInfoKHR& create_info,
                         VkSwapchainKHR old_swapchain,
                         VkSwapchainKHR* swapchain_out);
void DestroySwapchain(const VulkanDevice* device, VkSwapchainKHR swapchain);
VkResult GetSwapchainImages(const VulkanDevice* device, VkSwapchainKHR swapchain,
                            uint32_t* count, VkImage* images);
VkResult AcquireNextImage(const VulkanDevice* device, VkSwapchainKHR swapchain,
                          uint64_t timeout, VkSemaphore semaphore,
                          uint32_t* image_index);
VkResult QueuePresent(const VulkanDevice* device, VkQueue queue,
                      const VkPresentInfoKHR& present_info);

// At the start of the presenter's draw command buffer (before its render
// pass): configures the swapchain for this present and records the prepare
// pass when a new game frame is to be interpolated.
void BeginPresent(const VulkanDevice* device, VkSwapchainKHR swapchain,
                  VkCommandBuffer command_buffer, uint32_t width,
                  uint32_t height);

void Shutdown();

}  // namespace bffg
}  // namespace vulkan
}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_VULKAN_BF_FSR3FG_H_
