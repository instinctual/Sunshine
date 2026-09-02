/**
 * @file src/video_topology.h
 * @brief Lightweight declarations for PLANK output-topology consumers.
 */
#pragma once

#include "platform/display_info.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace video {
  /** Enumerate outputs visible to the active capture backend. */
  std::vector<platf::display_info_t> output_topology();

  /** Build a stable generation identifier for an output topology. */
  std::string output_topology_generation(
    const std::vector<platf::display_info_t> &outputs
  );

  /** Resolve one opaque output ID against a topology snapshot. */
  std::optional<std::string> resolve_output_capture_name(
    const std::vector<platf::display_info_t> &outputs,
    std::string_view output_id
  );
}  // namespace video
