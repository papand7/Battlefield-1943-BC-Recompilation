/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_DRAW_EXTENT_ESTIMATOR_H_
#define XENIA_GPU_DRAW_EXTENT_ESTIMATOR_H_

#include <cstdint>
#include <optional>

#include "xenia/gpu/register_file.h"
#include "xenia/gpu/shader.h"
#include "xenia/gpu/shader_interpreter.h"
#include "xenia/gpu/trace_writer.h"
#include "xenia/memory.h"

namespace xe {
namespace gpu {

class DrawExtentEstimator {
 public:
  DrawExtentEstimator(const RegisterFile& register_file, const Memory& memory,
                      TraceWriter* trace_writer)
      : register_file_(register_file),
        memory_(memory),
        trace_writer_(trace_writer),
        shader_interpreter_(register_file, memory) {
    shader_interpreter_.SetTraceWriter(trace_writer);
  }

  // The shader must have its ucode analyzed.
  uint32_t EstimateVertexMaxY(const Shader& vertex_shader,
                              bool window_offset_in_edram);
  uint32_t EstimateMaxY(bool try_to_estimate_vertex_max_y,
                        const Shader& vertex_shader,
                        bool window_offset_in_edram);

  // bf: for a single axis-aligned rectangle (a kRectangleList draw of 3
  // vertices, as Direct3D 9 draws its clears), its bounds in host pixel
  // coordinates (the guest's after the viewport and the pixel center offset):
  // x0, y0, x1, y1. False if it isn't one or the vertices can't be worked out.
  bool EstimateRectangle(const Shader& vertex_shader, float bounds_out[4]);

 private:
  class PositionYExportSink : public ShaderInterpreter::ExportSink {
   public:
    void Export(ucode::ExportRegister export_register, const float* value,
                uint32_t value_mask) override;

    void Reset() {
      position_y_.reset();
      position_w_.reset();
      point_size_.reset();
      vertex_kill_.reset();
    }

    const std::optional<float>& position_y() const { return position_y_; }
    const std::optional<float>& position_w() const { return position_w_; }
    const std::optional<float>& point_size() const { return point_size_; }
    const std::optional<uint32_t>& vertex_kill() const { return vertex_kill_; }

   private:
    std::optional<float> position_y_;
    std::optional<float> position_w_;
    std::optional<float> point_size_;
    std::optional<uint32_t> vertex_kill_;
  };

  class PositionExportSink : public ShaderInterpreter::ExportSink {
   public:
    void Export(ucode::ExportRegister export_register, const float* value,
                uint32_t value_mask) override;
    void Reset() {
      position_mask = 0;
      kill = false;
    }
    float position[4] = {};
    uint32_t position_mask = 0;
    bool kill = false;
  };

  const RegisterFile& register_file_;
  const Memory& memory_;
  TraceWriter* trace_writer_;

  ShaderInterpreter shader_interpreter_;
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_DRAW_EXTENT_ESTIMATOR_H_
