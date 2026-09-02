/**
 * @file src/plank2_encoder_source.h
 * @brief Typed PLANK2 boundary for the retained Host encoder engine.
 */
/* SPDX-License-Identifier: GPL-3.0-only */

#pragma once

#include "plank/backend/operations_v1.h"
#include "plank/media/interfaces_v1.h"
#include "plank2_media_session.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace plank::platform::linux_backend {
  class encoder_source_t;
}

struct PlankRetainedEncoderOpenRequest {
  std::uint16_t profile_id {};
  std::uint16_t pixel_layout {};
  std::uint16_t memory_kind {};
  std::uint32_t width {};
  std::uint32_t height {};
  std::uint32_t refresh_millihz {};
  std::uint64_t target_bitrate_bps {};
  std::string topology_generation;
  std::shared_ptr<PlankRetainedHostMediaSessionContext> media_session;
};

struct PlankRetainedEncoderFrame {
  std::uint16_t profile_id {};
  std::uint16_t pixel_layout {};
  std::uint16_t memory_kind {};
  std::uint16_t plane_count {};
  std::uint32_t width {};
  std::uint32_t height {};
  std::uint64_t frame_sequence {};
  std::uint64_t monotonic_timestamp_ns {};
  std::uint64_t frame_lease_id {};
  std::string_view topology_generation;
  std::array<PlankMediaPlaneV1, PLANK_MEDIA_MAX_PLANES_V1> planes {};
  std::uintptr_t backend_frame_handle {};
};

struct PlankRetainedEncodedPacket {
  std::uint16_t profile_id {};
  std::uint32_t flags {};
  std::uint64_t frame_sequence {};
  std::uint64_t presentation_timestamp_ns {};
  std::uint64_t decode_timestamp_ns {};
  const std::uint8_t *data {};
  std::size_t size {};
  std::shared_ptr<void> owner;
};

/**
 * Narrow boundary to the retained x264-CUDA/direct-NVENC engine.
 *
 * A successful submit must finish every read, conversion, upload, and other
 * access to the producer-owned frame before returning. It must not retain the
 * frame lease, its plane pointers, or native handles. A successful next call
 * transfers an owner that keeps every returned packet byte valid until the
 * PLANK2 packet lease is released. Qualification requires a real frame to
 * prove the exact profile, input layout, memory kind, and encoder output.
 * Calls on an open stream are serialized but may arrive from different caller
 * threads; the target must own or marshal any thread-affine codec/CUDA state.
 * The open request carries the exact session context published by capture.
 * The target must use that context's original display, retain the context for
 * its complete open lifetime, and release it synchronously from close().
 */
class IPlankRetainedEncoderTarget {
public:
  virtual ~IPlankRetainedEncoderTarget() = default;

  virtual bool available() const = 0;
  virtual bool qualifies(std::uint16_t profile_id,
                         std::uint16_t pixel_layout,
                         std::uint16_t memory_kind) const = 0;
  virtual PlankBackendOperationResultV1 open(
    const PlankRetainedEncoderOpenRequest &request
  ) = 0;
  virtual PlankBackendOperationResultV1 submit(
    const PlankRetainedEncoderFrame &frame
  ) = 0;
  virtual PlankBackendOperationResultV1 next(
    std::uint32_t timeout_ms, PlankRetainedEncodedPacket &packet
  ) = 0;
  virtual PlankBackendOperationResultV1 set_target_bitrate(
    std::uint64_t target_bitrate_bps
  ) = 0;
  virtual PlankBackendOperationResultV1 flush() = 0;
  virtual void close() noexcept = 0;
};

namespace plank::platform::linux_backend {
  std::shared_ptr<encoder_source_t> create_retained_host_encoder_source_v1(
    std::weak_ptr<IPlankRetainedEncoderTarget> target,
    std::shared_ptr<PlankRetainedHostMediaSessionSlot> media_session_slot,
    uid_t account_uid
  );
}
