/**
 * @file src/session/display_state.h
 * @brief One canonical interpretation of the supervisor-owned display state.
 */
#pragma once

#include "session_context.h"
#include "../platform/display_info.h"

#include <string>
#include <string_view>
#include <vector>

namespace plank::session {
  struct live_display_layout_t {
    std::string startup_kind;
    std::string kind;
    bool virtual_layout {};
    std::vector<std::string> virtual_modes;
    bool temporary_physical_lease {};
    uid_t lease_uid {};
  };

  inline live_display_layout_t describe_live_display_layout(
    const std::vector<platf::display_info_t> &outputs,
    startup_layout_t startup_layout,
    const std::optional<runtime_display_state_t> &runtime
  ) {
    live_display_layout_t result;
    result.startup_kind = startup_layout == startup_layout_t::virtual_display ?
      "single" : "physical";
    if (runtime) {
      result.kind = runtime->layout;
      result.virtual_layout = true;
      result.virtual_modes = {runtime->mode_1};
      if (!runtime->mode_2.empty()) {
        result.virtual_modes.push_back(runtime->mode_2);
      }
      result.temporary_physical_lease = true;
      result.lease_uid = runtime->lease_uid;
      return result;
    }
    result.virtual_layout = result.startup_kind != "physical";
    result.kind = !result.virtual_layout ? "physical" :
      outputs.size() == 1U ? "single" :
      outputs.size() == 2U ? "dual-horizontal" : "unhealthy";
    if (result.virtual_layout) {
      for (const auto &output : outputs) {
        result.virtual_modes.push_back(
          std::to_string(output.width) + "x" + std::to_string(output.height)
        );
      }
    }
    return result;
  }

  inline live_display_layout_t live_display_layout(
    const std::vector<platf::display_info_t> &outputs,
    startup_layout_t startup_layout,
    std::string_view runtime_state_path
  ) {
    return describe_live_display_layout(
      outputs, startup_layout, read_runtime_display_state(runtime_state_path)
    );
  }
}  // namespace plank::session
