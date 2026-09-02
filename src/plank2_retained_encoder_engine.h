/**
 * @file src/plank2_retained_encoder_engine.h
 * @brief Narrow retained-Host facade over Sunshine's encoder implementation.
 */
/* SPDX-License-Identifier: GPL-3.0-only */

#pragma once

#include "video.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace video {
  enum class retained_encoder_result_e {
    ok,
    again,
    unsupported,
    unavailable,
    failed,
  };

  struct retained_encoder_packet_t {
    std::shared_ptr<std::vector<std::uint8_t>> bytes;
    std::uint64_t frame_sequence {};
    std::uint64_t monotonic_timestamp_ns {};
    bool key_frame {};
    bool codec_config {};
    bool end_of_frame {};
  };

  /**
   * One retained encoder instance. Every method, including destruction, must
   * be called on the same thread that created the instance.
   */
  class retained_encoder_engine_t {
  public:
    virtual ~retained_encoder_engine_t() = default;

    /**
     * Convert and encode one borrowed capture allocation synchronously.
     * Implementations must not retain `image` after this call returns.
     */
    virtual retained_encoder_result_e submit(
      platf::img_t &image,
      std::uint64_t frame_sequence,
      std::uint64_t monotonic_timestamp_ns
    ) = 0;
    virtual retained_encoder_result_e next(retained_encoder_packet_t &packet) = 0;
    virtual retained_encoder_result_e set_target_bitrate(
      std::uint32_t target_bitrate_kbps
    ) = 0;
    virtual retained_encoder_result_e flush() = 0;
    virtual retained_encoder_result_e request_idr() = 0;
    virtual retained_encoder_result_e invalidate_reference_frames(
      std::uint64_t first_frame, std::uint64_t last_frame
    ) = 0;
  };

  /**
   * Factory whose capability answers come from the retained real-frame probe.
   * The returned engine uses the supplied original capture display; it must
   * never open a replacement display internally.
   */
  class retained_encoder_factory_t {
  public:
    virtual ~retained_encoder_factory_t() = default;
    virtual bool available() const = 0;
    virtual bool qualifies(std::string_view encoding_mode) const = 0;
    virtual std::unique_ptr<retained_encoder_engine_t> open(
      std::shared_ptr<platf::display_t> display,
      const config_t &config
    ) = 0;
  };

  std::shared_ptr<retained_encoder_factory_t>
  create_retained_encoder_factory();
}  // namespace video
