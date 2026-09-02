/**
 * @file src/semantic_input.h
 * @brief Narrow typed facade over retained Host input endpoints.
 */
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace input {
  enum class semantic_mouse_button_e {
    left,
    right,
    middle,
    back,
    forward,
  };

  enum class semantic_pen_tool_e {
    tip,
    eraser,
  };

  constexpr std::uint32_t semantic_pen_button_tip = 0x00000001U;
  constexpr std::uint32_t semantic_pen_button_barrel_1 = 0x00000002U;
  constexpr std::uint32_t semantic_pen_button_barrel_2 = 0x00000004U;
  constexpr std::uint32_t semantic_pen_button_barrel_3 = 0x00000008U;

  struct semantic_pen_t {
    semantic_pen_tool_e tool;
    bool in_proximity;
    std::uint32_t buttons;
    float normalized_x;
    float normalized_y;
    float pressure;
    float distance;
    float tilt_x_degrees;
    float tilt_y_degrees;
    float rotation_degrees;
  };

  struct semantic_feedback_t {
    std::uint64_t device_generation;
    std::vector<std::uint8_t> frame;
  };

  enum class semantic_feedback_result_e {
    ready,
    again,
    unavailable,
  };

  enum class semantic_close_disposition_e {
    suspend_and_retain,
    destroy_retained_identity,
  };

  /**
   * @brief Direct typed facade over the retained Linux virtual-HID endpoints.
   *
   * This path deliberately bypasses the inherited serialized packet parser.
   * One object owns one connection lease for a stable client instance.
   */
  class semantic_session_t {
  public:
    ~semantic_session_t();
    semantic_session_t(semantic_session_t &&) noexcept;
    semantic_session_t &operator=(semantic_session_t &&) noexcept;
    semantic_session_t(const semantic_session_t &) = delete;
    semantic_session_t &operator=(const semantic_session_t &) = delete;

    bool keyboard(std::uint16_t scan_code_set1, bool pressed);
    bool absolute_mouse(std::int32_t x, std::int32_t y);
    bool mouse_button(semantic_mouse_button_e button, bool pressed);
    bool wheel(std::int32_t horizontal_delta_120, std::int32_t vertical_delta_120);
    bool normalized_pen(const semantic_pen_t &pen);
    bool raw_hid(std::uint64_t device_generation, const std::uint8_t *frame, std::size_t frame_size);
    semantic_feedback_result_e next_feedback(
      std::chrono::milliseconds timeout,
      semantic_feedback_t &feedback
    );
    bool cancel_all() noexcept;
    bool close(semantic_close_disposition_e disposition) noexcept;

#ifdef SUNSHINE_TESTS
    std::array<float, 4> testing_last_pen_axes() const;
    int testing_last_pen_transition() const;
#endif

  private:
    struct impl_t;
    explicit semantic_session_t(std::unique_ptr<impl_t> impl) noexcept;
    std::unique_ptr<impl_t> impl_;

    friend std::unique_ptr<semantic_session_t> open_semantic_session(
      std::string client_instance_id,
      std::int32_t desktop_width,
      std::int32_t desktop_height
    );
  };

  bool semantic_input_available();

  std::unique_ptr<semantic_session_t> open_semantic_session(
    std::string client_instance_id,
    std::int32_t desktop_width,
    std::int32_t desktop_height
  );
}  // namespace input
