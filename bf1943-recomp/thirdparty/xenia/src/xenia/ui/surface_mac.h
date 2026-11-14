/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_SURFACE_MAC_H_
#define XENIA_UI_SURFACE_MAC_H_

#include "xenia/ui/surface.h"

namespace xe {
namespace ui {

// A CAMetalLayer (as void*: no Objective-C here) and the window it belongs
// to, whose code gives the size in pixels (GetSizeImpl).
class MetalLayerSurface final : public Surface {
 public:
  explicit MetalLayerSurface(const void* layer, void* window)
      : layer_(layer), window_(window) {}
  TypeIndex GetType() const override { return kTypeIndex_MetalLayer; }
  const void* layer() const { return layer_; }
  void* window() const { return window_; }

 protected:
  bool GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const override;

 private:
  const void* layer_;
  void* window_;
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_SURFACE_MAC_H_
