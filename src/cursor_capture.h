/**
 * @file src/cursor_capture.h
 * @brief Lightweight X11 cursor boundary for the PLANK 2 cursor adapter.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace cursor_capture {
  /** One premultiplied BGRA cursor image from XFixes. */
  struct image_t {
    std::uint64_t generation {};
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint32_t hotspot_x {};
    std::uint32_t hotspot_y {};
    bool visible {};
    std::vector<std::uint8_t> pixels;
  };

  /** Host-authoritative pointer hotspot in X11 root coordinates. */
  struct position_t {
    std::int32_t x {};
    std::int32_t y {};
    std::uint32_t desktop_width {};
    std::uint32_t desktop_height {};
  };

  /** Isolated X11 cursor source used by the PLANK 2 producer sidecar. */
  class capture_t {
  public:
    virtual ~capture_t() = default;
    virtual bool subscribe_shape_events() = 0;
    virtual bool capture_image(image_t &image) = 0;
    virtual bool query_position(position_t &position) = 0;
    virtual int consume_shape_change() = 0;
  };

  /** Open an independent connection to the active X11 display. */
  std::unique_ptr<capture_t> open_x11_capture();
}  // namespace cursor_capture
