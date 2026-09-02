/**
 * @file src/plank2_encoder_source.cpp
 * @brief PLANK2 source adapter for the retained Host encoder engine.
 */
/* SPDX-License-Identifier: GPL-3.0-only */

#include "plank2_encoder_source.h"

#include "encoder_source_v1.hpp"

#include <memory>
#include <utility>

namespace plank::platform::linux_backend {
  namespace {
    std::uint16_t only_memory_kind(std::uint32_t memory_kinds) {
      if (memory_kinds == 0U ||
          (memory_kinds & (memory_kinds - 1U)) != 0U) {
        return 0U;
      }
      for (std::uint16_t kind = PLANK_MEDIA_MEMORY_CPU_V1;
           kind <= PLANK_MEDIA_MEMORY_CORE_VIDEO_V1; ++kind) {
        if (memory_kinds == (UINT32_C(1) << (kind - 1U))) return kind;
      }
      return 0U;
    }

    class retained_encoder_stream_t final: public encoder_stream_t {
    public:
      explicit retained_encoder_stream_t(
          std::weak_ptr<IPlankRetainedEncoderTarget> target,
          std::shared_ptr<PlankRetainedHostMediaSessionContext> media_session):
          target_ {std::move(target)},
          media_session_ {std::move(media_session)} {
      }

      ~retained_encoder_stream_t() override {
        if (const auto target = target_.lock()) target->close();
      }

      PlankBackendOperationResultV1 submit(
          const PlankMediaFrameLeaseV1 &frame) override {
        const auto target = target_.lock();
        if (!target || !target->available()) {
          return PLANK_BACKEND_OPERATION_UNAVAILABLE_V1;
        }
        PlankRetainedEncoderFrame retained {
          frame.profile_id, frame.pixel_layout, frame.memory_kind,
          frame.plane_count, frame.width, frame.height, frame.frame_sequence,
          frame.monotonic_timestamp_ns, frame.lease_id,
          frame.topology_generation, {}, frame.backend_frame_handle,
        };
        for (std::uint16_t index = 0U; index < frame.plane_count; ++index) {
          retained.planes[index] = frame.planes[index];
        }
        return target->submit(retained);
      }

      PlankBackendOperationResultV1 next(
          std::uint32_t timeout_ms, encoder_packet_t &packet) override {
        const auto target = target_.lock();
        if (!target || !target->available()) {
          return PLANK_BACKEND_OPERATION_UNAVAILABLE_V1;
        }
        PlankRetainedEncodedPacket retained;
        const auto result = target->next(timeout_ms, retained);
        if (result != PLANK_BACKEND_OPERATION_OK_V1) return result;
        packet = {
          retained.profile_id, retained.flags, retained.frame_sequence,
          retained.presentation_timestamp_ns, retained.decode_timestamp_ns,
          retained.data, retained.size, std::move(retained.owner),
        };
        return PLANK_BACKEND_OPERATION_OK_V1;
      }

      PlankBackendOperationResultV1 set_target_bitrate(
          std::uint64_t target_bitrate_bps) override {
        const auto target = target_.lock();
        if (!target || !target->available()) {
          return PLANK_BACKEND_OPERATION_UNAVAILABLE_V1;
        }
        return target->set_target_bitrate(target_bitrate_bps);
      }

      PlankBackendOperationResultV1 recover(
          const PlankEncoderRecoveryRequestV1 &request) override {
        const auto target = target_.lock();
        if (!target || !target->available()) {
          return PLANK_BACKEND_OPERATION_UNAVAILABLE_V1;
        }
        return target->recover(request);
      }

      PlankBackendOperationResultV1 flush() override {
        const auto target = target_.lock();
        if (!target || !target->available()) {
          return PLANK_BACKEND_OPERATION_UNAVAILABLE_V1;
        }
        return target->flush();
      }

    private:
      std::weak_ptr<IPlankRetainedEncoderTarget> target_;
      std::shared_ptr<PlankRetainedHostMediaSessionContext> media_session_;
    };

    class retained_encoder_source_t final: public encoder_source_t {
    public:
      explicit retained_encoder_source_t(
          std::weak_ptr<IPlankRetainedEncoderTarget> target,
          std::shared_ptr<PlankRetainedHostMediaSessionSlot> media_session_slot,
          uid_t account_uid):
          target_ {std::move(target)},
          media_session_slot_ {std::move(media_session_slot)},
          account_uid_ {account_uid} {
      }

      bool available() override {
        const auto target = target_.lock();
        return target && target->available();
      }

      encoder_qualify_result_t qualify(
          const PlankMediaProfileCapabilityV1 &capability) override {
        const auto target = target_.lock();
        if (!target || !target->available()) {
          return encoder_qualify_result_t::unavailable;
        }
        const auto memory_kind = only_memory_kind(capability.memory_kinds);
        if (memory_kind == 0U) return encoder_qualify_result_t::unsupported;
        return target->qualifies(capability.profile_id,
                                 capability.pixel_layout, memory_kind) ?
          encoder_qualify_result_t::available :
          encoder_qualify_result_t::unsupported;
      }

      PlankBackendOperationResultV1 open(
          const PlankEncoderOpenRequestV1 &request,
          const PlankMediaProfileCapabilityV1 &capability,
          std::unique_ptr<encoder_stream_t> &stream) override {
        stream.reset();
        const auto target = target_.lock();
        if (!target || !target->available()) {
          return PLANK_BACKEND_OPERATION_UNAVAILABLE_V1;
        }
        const auto memory_kind = only_memory_kind(capability.memory_kinds);
        if (capability.profile_id != request.profile_id || memory_kind == 0U ||
            !target->qualifies(capability.profile_id,
                               capability.pixel_layout, memory_kind)) {
          return PLANK_BACKEND_OPERATION_UNSUPPORTED_V1;
        }
        const PlankRetainedHostMediaSessionIdentity identity {
          account_uid_, capability.profile_id, capability.pixel_layout,
          memory_kind, request.source_width, request.source_height,
          request.refresh_millihz, request.topology_generation,
        };
        std::shared_ptr<PlankRetainedHostMediaSessionContext> media_session;
        if (!media_session_slot_ ||
            media_session_slot_->acquire(identity, media_session) !=
              PlankRetainedHostMediaSessionResult::ok) {
          return PLANK_BACKEND_OPERATION_UNAVAILABLE_V1;
        }
        const PlankRetainedEncoderOpenRequest retained {
          request.profile_id, capability.pixel_layout, memory_kind,
          request.source_width, request.source_height,
          request.encoded_width, request.encoded_height,
          request.refresh_millihz,
          request.target_bitrate_bps, request.topology_generation,
          media_session,
        };
        const auto result = target->open(retained);
        if (result != PLANK_BACKEND_OPERATION_OK_V1) return result;
        try {
          stream = std::make_unique<retained_encoder_stream_t>(
            target_, std::move(media_session)
          );
        } catch (...) {
          target->close();
          throw;
        }
        return PLANK_BACKEND_OPERATION_OK_V1;
      }

    private:
      std::weak_ptr<IPlankRetainedEncoderTarget> target_;
      std::shared_ptr<PlankRetainedHostMediaSessionSlot> media_session_slot_;
      uid_t account_uid_ {};
    };
  }

  std::shared_ptr<encoder_source_t> create_retained_host_encoder_source_v1(
      std::weak_ptr<IPlankRetainedEncoderTarget> target,
      std::shared_ptr<PlankRetainedHostMediaSessionSlot> media_session_slot,
      uid_t account_uid) {
    if (!media_session_slot) return {};
    return std::make_shared<retained_encoder_source_t>(
      std::move(target), std::move(media_session_slot), account_uid
    );
  }
}
