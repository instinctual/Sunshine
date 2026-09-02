/**
 * @file src/platform/display_info.h
 * @brief Lightweight display-output metadata shared by topology consumers.
 */
#pragma once

#include <string>

namespace platf {
  /**
   * @brief Stable output metadata used by PLANK topology negotiation.
   */
  struct display_info_t {
    std::string id;  ///< Opaque stable protocol identifier.
    std::string name;  ///< User-facing connector or display name.
    std::string capture_name;  ///< Backend name accepted by display().
    int x {};  ///< Output origin in desktop coordinates.
    int y {};  ///< Output origin in desktop coordinates.
    int width {};  ///< Active pixel width.
    int height {};  ///< Active pixel height.
    int rotation {};  ///< Clockwise rotation in degrees.
    int refresh_millihz {};  ///< Active refresh rate in millihertz, or zero when unknown.
    bool primary {};  ///< Whether the window system marks this output primary.
  };
}  // namespace platf
