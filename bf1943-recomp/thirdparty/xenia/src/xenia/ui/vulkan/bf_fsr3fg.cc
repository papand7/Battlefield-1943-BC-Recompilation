// bf1943: AMD FSR 3.1 frame generation (bf_fsr3fg.h).

#include "xenia/ui/vulkan/bf_fsr3fg.h"

#include <algorithm>
#include <cstring>
#include <mutex>

#include "xenia/base/logging.h"
#include "xenia/ui/vulkan/vulkan_device.h"
#include "xenia/ui/vulkan/vulkan_instance.h"

// Windows, with the SDK fetched (tools/fetch_fsr3fg.py); otherwise the plain
// Vulkan swapchain calls and no frame generation.
#if defined(_WIN32) && __has_include("framegeneration/include/ffx_framegeneration.h")
#define BF_FSR3FG 1
#define _WINDOWS
#include "api/include/ffx_api.h"
#include "api/include/ffx_api_loader.h"
#include "api/include/vk/ffx_api_vk.h"
#include "framegeneration/include/ffx_framegeneration.h"
#include "framegeneration/include/vk/ffx_api_framegeneration_vk.h"
#endif

namespace xe {
namespace ui {
namespace vulkan {
namespace bffg {

namespace {
std::mutex snapshot_mutex;
Snapshot latest_snapshot;
}  // namespace

void Publish(const Snapshot& snapshot) {
  std::lock_guard<std::mutex> lock(snapshot_mutex);
  latest_snapshot = snapshot;
}

#ifdef BF_FSR3FG

namespace {

struct State {
  bool tried = false, loaded = false, failed = false;
  HMODULE module = nullptr;
  ffxFunctions ffx = {};
  // The swapchain (its context, the replacement entry points) and the frame
  // generation context that dispatches the interpolation.
  ffxContext swapchain_context = nullptr, fg_context = nullptr;
  ffxQueryDescSwapchainReplacementFunctionsVK fn = {};
  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  // Handed back by the caller after CreateSwapchain destroyed it already.
  VkSwapchainKHR retired = VK_NULL_HANDLE;
  uint32_t family = 0;
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t width = 0, height = 0;
  uint64_t frame_id = 0, last_seq = 0, generated_frames = 0;
  bool first_after_reset = true;
} g;

const VulkanDevice* g_device = nullptr;

void Message(uint32_t type, const wchar_t* message) {
  char text[512];
  size_t n = 0;
  for (; message[n] && n + 1 < sizeof(text); ++n) text[n] = char(message[n] < 128 ? message[n] : '?');
  text[n] = 0;
  XELOGI("fsr3fg: {}{}", type == FFX_API_MESSAGE_TYPE_ERROR ? "error: " : "", text);
}

// Submits the SDK makes on a queue that may also be used by others: the game
// queue is the presenter's (Xenia locks it), the others get a lock each.
VkResult GameSubmit(uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
  const auto queue = g_device->AcquireQueue(g.family, 0);
  return g_device->functions().vkQueueSubmit(queue.queue(), count, submits, fence);
}
template <uint32_t kIndex>
VkResult OwnSubmit(uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
  const auto queue = g_device->AcquireQueue(g.family, kIndex);
  return g_device->functions().vkQueueSubmit(queue.queue(), count, submits, fence);
}

FfxApiResource Image(VkImage image, VkFormat format, uint32_t width, uint32_t height) {
  VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = {width, height, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  // All of them are kept in VK_IMAGE_LAYOUT_GENERAL.
  return ffxApiGetResourceVK(reinterpret_cast<void*>(image),
                             ffxApiGetImageResourceDescriptionVK(image, info, 0),
                             FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
}

bool Load() {
  if (g.tried) return g.loaded;
  g.tried = true;
  g.module = LoadLibraryW(L"amd_fidelityfx_loader_vk.dll");
  if (g.module) {
    ffxLoadFunctions(&g.ffx, g.module);
    if (!g.ffx.CreateContext || !g.ffx.Dispatch || !g.ffx.Query || !g.ffx.Configure || !g.ffx.DestroyContext) {
      FreeLibrary(g.module);
      g.module = nullptr;
    }
  }
  if (!g.module) {
    XELOGW("fsr3fg: amd_fidelityfx_loader_vk.dll not found or incomplete");
    return false;
  }
  // The loader finds the frame generation DLL next to itself.
  wchar_t path[MAX_PATH];
  DWORD n = GetModuleFileNameW(g.module, path, MAX_PATH);
  while (n && path[n - 1] != L'\\' && path[n - 1] != L'/') --n;
  static const wchar_t kName[] = L"amd_fidelityfx_framegeneration_vk.dll";
  if (n && n + sizeof(kName) / sizeof(wchar_t) < MAX_PATH) {
    std::memcpy(path + n, kName, sizeof(kName));
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
      XELOGW("fsr3fg: amd_fidelityfx_framegeneration_vk.dll not found next to the loader");
      return false;
    }
  }
  g.loaded = true;
  XELOGI("fsr3fg: loaded");
  return true;
}

void Teardown(const VulkanDevice* device) {
  if (!g.swapchain_context && !g.fg_context && g.swapchain == VK_NULL_HANDLE) return;
  const auto& dfn = device->functions();
  if (g.swapchain_context) {
    // The presents the swapchain still has in flight.
    ffxDispatchDescFrameGenerationSwapChainWaitForPresentsVK wait = {};
    wait.header.type = FFX_API_DISPATCH_DESC_TYPE_FGSWAPCHAIN_WAIT_FOR_PRESENTS_VK;
    g.ffx.Dispatch(&g.swapchain_context, &wait.header);
  }
  {
    const auto queue = device->AcquireQueue(device->queue_family_graphics_compute(), 0);
    dfn.vkQueueWaitIdle(queue.queue());
  }
  if (g.fg_context) g.ffx.DestroyContext(&g.fg_context, nullptr);
  g.fg_context = nullptr;
  if (g.swapchain_context) g.ffx.DestroyContext(&g.swapchain_context, nullptr);
  g.swapchain_context = nullptr;
  if (g.swapchain != VK_NULL_HANDLE && g.fn.pOutDestroySwapchainFFXAPI) {
    g.fn.pOutDestroySwapchainFFXAPI(device->device(), g.swapchain, nullptr, nullptr);
  }
  g.retired = g.swapchain;
  g.swapchain = VK_NULL_HANDLE;
  g.fn = {};
  active = false;
}

// The frame generation swapchain in place of vkCreateSwapchainKHR. False (and
// nothing left behind) if it cannot be made.
bool TryCreate(const VulkanDevice* device, const VkSwapchainCreateInfoKHR& create_info,
               VkSwapchainKHR old_swapchain, VkSwapchainKHR* out) {
  const auto& queue_families = device->queue_families();
  const uint32_t family = device->queue_family_graphics_compute();
  if (queue_families[family].queues.size() < 1 + kExtraQueues) {
    XELOGW("fsr3fg: the graphics queue family has {} queue(s), {} needed: frame generation off",
           queue_families[family].queues.size(), 1 + kExtraQueues);
    return false;
  }
  if (create_info.imageSharingMode != VK_SHARING_MODE_EXCLUSIVE) {
    XELOGW("fsr3fg: the presentation queue family differs from the graphics one: frame generation off");
    return false;
  }
  g_device = device;
  g.family = family;
  VkSwapchainCreateInfoKHR info = create_info;
  // The swapchain the game's passes see is the SDK's; it copies and composes
  // into the real one.
  info.imageUsage |= VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  VkSwapchainKHR swapchain = old_swapchain;
  info.oldSwapchain = old_swapchain;

  ffxCreateContextDescFrameGenerationSwapChainVersionVK version = {};
  version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_VK;
  version.version = FFX_FRAMEGENERATION_SWAPCHAIN_VK_VERSION;
  ffxCreateContextDescFrameGenerationSwapChainModeVK mode = {};
  mode.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_MODE_VK;
  mode.composeOnPresentQueue = false;
  version.header.pNext = &mode.header;
  ffxCreateContextDescFrameGenerationSwapChainVK desc = {};
  desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
  desc.header.pNext = &version.header;
  desc.physicalDevice = device->physical_device();
  desc.device = device->device();
  desc.swapchain = &swapchain;
  desc.allocator = nullptr;
  desc.createInfo = info;
  desc.gameQueue = {queue_families[family].queues[0]->queue, family, GameSubmit};
  desc.presentQueue = {queue_families[family].queues[1]->queue, family, OwnSubmit<1>};
  desc.asyncComputeQueue = {queue_families[family].queues[2]->queue, family, OwnSubmit<2>};
  desc.imageAcquireQueue = {queue_families[family].queues[3]->queue, family, OwnSubmit<3>};
  ffxReturnCode_t r = g.ffx.CreateContext(&g.swapchain_context, &desc.header, nullptr);
  if (r != FFX_API_RETURN_OK || swapchain == VK_NULL_HANDLE) {
    XELOGE("fsr3fg: no frame generation swapchain ({})", uint32_t(r));
    g.swapchain_context = nullptr;
    return false;
  }
  g.swapchain = swapchain;
  g.fn = {};
  g.fn.header.type = FFX_API_QUERY_DESC_TYPE_FGSWAPCHAIN_FUNCTIONS_VK;
  if (g.ffx.Query(&g.swapchain_context, &g.fn.header) != FFX_API_RETURN_OK || !g.fn.pOutQueuePresentKHR ||
      !g.fn.pOutAcquireNextImageKHR || !g.fn.pOutGetSwapchainImagesKHR || !g.fn.pOutDestroySwapchainFFXAPI) {
    XELOGE("fsr3fg: the swapchain's entry points are missing");
    Teardown(device);
    return false;
  }

  // The frame generation context that interpolates in the swapchain's presents.
  ffxCreateBackendVKDesc backend = {};
  backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
  backend.deviceContext.vkDevice = device->device();
  backend.deviceContext.vkPhysicalDevice = device->physical_device();
  backend.deviceContext.vkDeviceProcAddr = device->vulkan_instance()->functions().vkGetDeviceProcAddr;
  ffxCreateContextDescFrameGenerationVersion fg_version = {};
  fg_version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
  fg_version.version = FFX_FRAMEGENERATION_VERSION;
  ffxCreateContextDescFrameGenerationHudless hudless = {};
  hudless.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS;
  hudless.hudlessBackBufferFormat = ffxApiGetSurfaceFormatVK(VK_FORMAT_R8G8B8A8_UNORM);
  hudless.header.pNext = &fg_version.header;
  backend.header.pNext = &hudless.header;
  ffxCreateContextDescFrameGeneration fg = {};
  fg.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
  fg.header.pNext = &backend.header;
  // The game's depth is reversed; its motion vectors are in render pixels.
  fg.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
  fg.displaySize = {create_info.imageExtent.width, create_info.imageExtent.height};
  fg.maxRenderSize = fg.displaySize;
  fg.backBufferFormat = ffxApiGetSurfaceFormatVK(create_info.imageFormat);
  r = g.ffx.CreateContext(&g.fg_context, &fg.header, nullptr);
  if (r != FFX_API_RETURN_OK) {
    XELOGE("fsr3fg: no frame generation context ({})", uint32_t(r));
    g.fg_context = nullptr;
    Teardown(device);
    return false;
  }
  g.format = create_info.imageFormat;
  g.width = create_info.imageExtent.width;
  g.height = create_info.imageExtent.height;
  g.frame_id = 0;
  g.last_seq = 0;
  g.first_after_reset = true;
  *out = g.swapchain;
  active = true;
  XELOGI("fsr3fg: frame generation swapchain {}x{}", g.width, g.height);
  return true;
}

ffxReturnCode_t Interpolate(ffxDispatchDescFrameGeneration* params, void* context) {
  return g.ffx.Dispatch(reinterpret_cast<ffxContext*>(context), &params->header);
}

}  // namespace

bool Available() { return Load(); }

VkResult CreateSwapchain(const VulkanDevice* device, const VkSwapchainCreateInfoKHR& create_info,
                         VkSwapchainKHR old_swapchain, VkSwapchainKHR* swapchain_out) {
  if (requested && !g.failed && Load()) {
    VkSwapchainKHR old = old_swapchain;
    if (old != VK_NULL_HANDLE && old == g.swapchain) {
      Teardown(device);  // (the caller's later destroy of old is then a no-op)
      old = VK_NULL_HANDLE;
    } else if (g.swapchain != VK_NULL_HANDLE) {
      Teardown(device);
    }
    if (TryCreate(device, create_info, old, swapchain_out)) {
      if (old != VK_NULL_HANDLE) g.retired = old_swapchain;  // the SDK destroyed it
      return VK_SUCCESS;
    }
    // Not available here (or not working): stay on the plain swapchain.
    g.failed = true;
    old_swapchain = old;
  }
  VkSwapchainCreateInfoKHR info = create_info;
  info.oldSwapchain = old_swapchain;
  return device->functions().vkCreateSwapchainKHR(device->device(), &info, nullptr, swapchain_out);
}

void DestroySwapchain(const VulkanDevice* device, VkSwapchainKHR swapchain) {
  if (swapchain == VK_NULL_HANDLE) return;
  if (swapchain == g.retired) {
    g.retired = VK_NULL_HANDLE;
    return;
  }
  if (swapchain == g.swapchain) {
    Teardown(device);
    g.retired = VK_NULL_HANDLE;
    return;
  }
  device->functions().vkDestroySwapchainKHR(device->device(), swapchain, nullptr);
}

VkResult GetSwapchainImages(const VulkanDevice* device, VkSwapchainKHR swapchain, uint32_t* count,
                            VkImage* images) {
  if (swapchain != VK_NULL_HANDLE && swapchain == g.swapchain) {
    return g.fn.pOutGetSwapchainImagesKHR(device->device(), swapchain, count, images);
  }
  return device->functions().vkGetSwapchainImagesKHR(device->device(), swapchain, count, images);
}

VkResult AcquireNextImage(const VulkanDevice* device, VkSwapchainKHR swapchain, uint64_t timeout,
                          VkSemaphore semaphore, uint32_t* image_index) {
  if (swapchain != VK_NULL_HANDLE && swapchain == g.swapchain) {
    return g.fn.pOutAcquireNextImageKHR(device->device(), swapchain, timeout, semaphore, VK_NULL_HANDLE,
                                        image_index);
  }
  return device->functions().vkAcquireNextImageKHR(device->device(), swapchain, timeout, semaphore,
                                                   VK_NULL_HANDLE, image_index);
}

VkResult QueuePresent(const VulkanDevice* device, VkQueue queue, const VkPresentInfoKHR& present_info) {
  if (present_info.swapchainCount == 1 && present_info.pSwapchains[0] != VK_NULL_HANDLE &&
      present_info.pSwapchains[0] == g.swapchain) {
    VkResult result = g.fn.pOutQueuePresentKHR(queue, &present_info);
    if (g.fn.pOutGetLastPresentCountFFXAPI) presented = g.fn.pOutGetLastPresentCountFFXAPI(g.swapchain);
    return result;
  }
  presented.fetch_add(1);
  return device->functions().vkQueuePresentKHR(queue, &present_info);
}

void BeginPresent(const VulkanDevice* device, VkSwapchainKHR swapchain, VkCommandBuffer command_buffer,
                  uint32_t width, uint32_t height) {
  if (swapchain == VK_NULL_HANDLE || swapchain != g.swapchain || !g.fg_context) return;
  Snapshot s;
  {
    std::lock_guard<std::mutex> lock(snapshot_mutex);
    s = latest_snapshot;
  }
  // A new game frame: it and the one before it get a generated frame between
  // them; a repaint of the same game frame is only presented.
  const bool usable = s.seq && s.depth && s.motion && s.render_width && s.render_height &&
                      width == g.width && height == g.height;
  const bool generate = usable && s.seq != g.last_seq;
  const bool hudless = generate && s.hudless && s.display_width == width && s.display_height == height;
  ++g.frame_id;

  ffxConfigureDescFrameGeneration config = {};
  config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
  config.swapChain = reinterpret_cast<void*>(swapchain);
  config.presentCallback = nullptr;
  config.presentCallbackUserContext = nullptr;
  config.frameGenerationCallback = Interpolate;
  config.frameGenerationCallbackUserContext = &g.fg_context;
  config.frameGenerationEnabled = generate;
  config.allowAsyncWorkloads = false;
  if (hudless) config.HUDLessColor = Image(s.hudless, VK_FORMAT_R8G8B8A8_UNORM, width, height);
  config.flags = 0;
  config.onlyPresentGenerated = false;
  config.generationRect = {0, 0, int32_t(width), int32_t(height)};
  config.frameID = g.frame_id;
  ffxReturnCode_t r = g.ffx.Configure(&g.fg_context, &config.header);
  if (r != FFX_API_RETURN_OK) {
    static int logs = 0;
    if (logs++ < 5) XELOGE("fsr3fg: configure failed ({})", uint32_t(r));
    return;
  }
  if (!generate) return;

  // What the game frame wrote (the command processor's copies) to compute.
  const VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_MEMORY_WRITE_BIT,
                                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
  device->functions().vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0,
                                           nullptr);
  ffxDispatchDescFrameGenerationPrepareV2 prepare = {};
  prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
  prepare.frameID = g.frame_id;
  prepare.flags = 0;
  prepare.commandList = command_buffer;
  prepare.renderSize = {s.render_width, s.render_height};
  prepare.jitterOffset = {s.jitter_x, s.jitter_y};
  prepare.motionVectorScale = {1.0f, 1.0f};  // already in render pixels
  prepare.frameTimeDelta = s.frame_ms;
  prepare.reset = s.reset || g.first_after_reset;
  prepare.cameraNear = s.near_plane;
  prepare.cameraFar = s.far_plane;
  prepare.cameraFovAngleVertical = s.fov_y;
  prepare.viewSpaceToMetersFactor = 0.0f;
  prepare.depth = Image(s.depth, VK_FORMAT_R32_SFLOAT, s.render_width, s.render_height);
  prepare.motionVectors = Image(s.motion, VK_FORMAT_R16G16_SFLOAT, s.render_width, s.render_height);
  prepare.cameraUp[1] = 1.0f;
  prepare.cameraRight[0] = 1.0f;
  prepare.cameraForward[2] = -1.0f;
  r = g.ffx.Dispatch(&g.fg_context, &prepare.header);
  if (r != FFX_API_RETURN_OK) {
    // Nothing prepared for this frame: it is only presented.
    static int logs = 0;
    if (logs++ < 5) XELOGE("fsr3fg: prepare failed ({})", uint32_t(r));
    ffxConfigureDescFrameGeneration off = config;
    off.frameGenerationEnabled = false;
    off.HUDLessColor = {};
    g.ffx.Configure(&g.fg_context, &off.header);
    return;
  }
  g.last_seq = s.seq;
  g.first_after_reset = false;
  ++g.generated_frames;
}

void Shutdown() {
  if (g_device) Teardown(g_device);
}

#else  // !BF_FSR3FG

bool Available() { return false; }

VkResult CreateSwapchain(const VulkanDevice* device, const VkSwapchainCreateInfoKHR& create_info,
                         VkSwapchainKHR old_swapchain, VkSwapchainKHR* swapchain_out) {
  VkSwapchainCreateInfoKHR info = create_info;
  info.oldSwapchain = old_swapchain;
  return device->functions().vkCreateSwapchainKHR(device->device(), &info, nullptr, swapchain_out);
}
void DestroySwapchain(const VulkanDevice* device, VkSwapchainKHR swapchain) {
  device->functions().vkDestroySwapchainKHR(device->device(), swapchain, nullptr);
}
VkResult GetSwapchainImages(const VulkanDevice* device, VkSwapchainKHR swapchain, uint32_t* count,
                            VkImage* images) {
  return device->functions().vkGetSwapchainImagesKHR(device->device(), swapchain, count, images);
}
VkResult AcquireNextImage(const VulkanDevice* device, VkSwapchainKHR swapchain, uint64_t timeout,
                          VkSemaphore semaphore, uint32_t* image_index) {
  return device->functions().vkAcquireNextImageKHR(device->device(), swapchain, timeout, semaphore,
                                                   VK_NULL_HANDLE, image_index);
}
VkResult QueuePresent(const VulkanDevice* device, VkQueue queue, const VkPresentInfoKHR& present_info) {
  presented.fetch_add(1);
  return device->functions().vkQueuePresentKHR(queue, &present_info);
}
void BeginPresent(const VulkanDevice*, VkSwapchainKHR, VkCommandBuffer, uint32_t, uint32_t) {}
void Shutdown() {}

#endif  // BF_FSR3FG

}  // namespace bffg
}  // namespace vulkan
}  // namespace ui
}  // namespace xe
