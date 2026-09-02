/**
 * @file src/plank2_encoder_target.cpp
 * @brief Concrete PLANK2 target for the retained Host encoder engine.
 */
/* SPDX-License-Identifier: GPL-3.0-only */

#include <utility>

#include "plank2_encoder_target.h"

#include "plank/media/profile_v1.h"
#include "plank/platform/linux/legacy_profile_adapter_v1.h"

#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>

namespace plank::platform::linux_backend {
  namespace {
    class encoder_worker_t final {
    public:
      encoder_worker_t():
          thread_ {[this]() { run(); }} {
      }

      ~encoder_worker_t() {
        stop();
      }

      encoder_worker_t(const encoder_worker_t &) = delete;
      encoder_worker_t &operator=(const encoder_worker_t &) = delete;

      template<class Function>
      auto invoke(Function &&function) -> std::invoke_result_t<Function> {
        using result_t = std::invoke_result_t<Function>;
        auto task = std::make_shared<std::packaged_task<result_t()>>(
          std::forward<Function>(function)
        );
        auto result = task->get_future();
        {
          std::lock_guard lock {mutex_};
          if (stopping_) {
            throw std::runtime_error("retained encoder worker is stopped");
          }
          tasks_.emplace_back([task]() { (*task)(); });
        }
        changed_.notify_one();
        return result.get();
      }

      void stop() noexcept {
        {
          std::lock_guard lock {mutex_};
          if (stopping_) {
            return;
          }
          stopping_ = true;
        }
        changed_.notify_all();
        if (thread_.joinable()) {
          thread_.join();
        }
      }

    private:
      void run() {
        for (;;) {
          std::function<void()> task;
          {
            std::unique_lock lock {mutex_};
            changed_.wait(lock, [this]() {
              return stopping_ || !tasks_.empty();
            });
            if (stopping_ && tasks_.empty()) {
              return;
            }
            task = std::move(tasks_.front());
            tasks_.pop_front();
          }
          task();
        }
      }

      std::mutex mutex_;
      std::condition_variable changed_;
      std::deque<std::function<void()>> tasks_;
      bool stopping_ {};
      std::thread thread_;
    };

    struct retained_profile_config_t {
      video::config_t config;
      std::string encoding_mode;
      std::uint16_t pixel_layout {};
      std::uint16_t memory_kind {};
    };

    std::optional<retained_profile_config_t> profile_config(
        std::uint16_t profile_id, std::uint32_t width,
        std::uint32_t height, std::uint32_t refresh_millihz,
        std::uint64_t target_bitrate_bps) {
      const auto *profile = plank_media_profile_find_v1(profile_id);
      if (profile == nullptr || width == 0U || height == 0U ||
          width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
          height > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
          refresh_millihz < 1000U || refresh_millihz % 10U != 0U ||
          target_bitrate_bps == 0U || target_bitrate_bps % 1000U != 0U ||
          target_bitrate_bps / 1000U >
            static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
      }

      PlankLinuxLegacyMediaProfileV1 mapping;
      if (plank_linux_legacy_media_profile_v1(profile, &mapping) !=
          PLANK_LINUX_LEGACY_PROFILE_OK_V1) {
        return std::nullopt;
      }

      retained_profile_config_t result;
      result.encoding_mode = mapping.encoding_mode;
      result.pixel_layout =
        profile->capture_source == PLANK_MEDIA_CAPTURE_NVFBC_8BIT_V1 ?
          PLANK_MEDIA_PIXEL_BGRX8_V1 :
          PLANK_MEDIA_PIXEL_XRGB2101010_LE_V1;
      result.memory_kind =
        profile->capture_source == PLANK_MEDIA_CAPTURE_NVFBC_8BIT_V1 ?
          PLANK_MEDIA_MEMORY_CUDA_V1 : PLANK_MEDIA_MEMORY_CPU_V1;

      auto &config = result.config;
      config.width = static_cast<int>(width);
      config.height = static_cast<int>(height);
      config.framerate = static_cast<int>(
        (refresh_millihz + 500U) / 1000U
      );
      config.framerateX100 = static_cast<int>(refresh_millihz / 10U);
      config.bitrate = static_cast<int>(target_bitrate_bps / 1000U);
      config.slicesPerFrame = 1;
      config.numRefFrames = 0;
      config.encoderCscMode = mapping.encoder_csc_mode;
      config.videoFormat = mapping.video_codec;
      config.dynamicRange = mapping.dynamic_range;
      config.chromaSamplingType = mapping.chroma_sampling;
      config.enableIntraRefresh = 0;
      config.span_desktop = false;
      config.encoder_backend = mapping.encoder_backend;
      if (std::strcmp(mapping.capture_source, "nvfbc") == 0) {
        config.capture_source = video::capture_source_e::nvfbc_8bit;
      } else if (std::strcmp(mapping.capture_source, "x11-native10") == 0) {
        config.capture_source = video::capture_source_e::x11_native10;
      } else {
        return std::nullopt;
      }
      return result;
    }

    PlankBackendOperationResultV1 operation_result(
        video::retained_encoder_result_e result) {
      switch (result) {
        case video::retained_encoder_result_e::ok:
          return PLANK_BACKEND_OPERATION_OK_V1;
        case video::retained_encoder_result_e::again:
          return PLANK_BACKEND_OPERATION_AGAIN_V1;
        case video::retained_encoder_result_e::unsupported:
          return PLANK_BACKEND_OPERATION_UNSUPPORTED_V1;
        case video::retained_encoder_result_e::unavailable:
          return PLANK_BACKEND_OPERATION_UNAVAILABLE_V1;
        case video::retained_encoder_result_e::failed:
        default:
          return PLANK_BACKEND_OPERATION_FAILED_V1;
      }
    }

    class retained_host_encoder_target_t final:
        public IPlankRetainedEncoderTarget {
    public:
      explicit retained_host_encoder_target_t(
          std::shared_ptr<video::retained_encoder_factory_t> factory):
          factory_ {std::move(factory)} {
      }

      ~retained_host_encoder_target_t() override {
        close();
        worker_.stop();
      }

      bool available() const override {
        try {
          return factory_ && factory_->available();
        } catch (...) {
          return false;
        }
      }

      bool qualifies(std::uint16_t profile_id, std::uint16_t pixel_layout,
                     std::uint16_t memory_kind) const override {
        const auto translated = profile_config(
          profile_id, 1U, 1U, 60000U, 10000000U
        );
        if (!translated || translated->pixel_layout != pixel_layout ||
            translated->memory_kind != memory_kind || !factory_) {
          return false;
        }
        try {
          return factory_->qualifies(translated->encoding_mode);
        } catch (...) {
          return false;
        }
      }

      PlankBackendOperationResultV1 open(
          const PlankRetainedEncoderOpenRequest &request) override {
        if (!request.media_session || !request.media_session->display()) {
          return PLANK_BACKEND_OPERATION_INVALID_ARGUMENT_V1;
        }
        const auto translated = profile_config(
          request.profile_id, request.encoded_width, request.encoded_height,
          request.refresh_millihz, request.target_bitrate_bps
        );
        const auto &identity = request.media_session->identity();
        if (!translated || translated->pixel_layout != request.pixel_layout ||
            translated->memory_kind != request.memory_kind ||
            identity.profile_id != request.profile_id ||
            identity.pixel_layout != request.pixel_layout ||
            identity.memory_kind != request.memory_kind ||
            identity.width != request.source_width ||
            identity.height != request.source_height ||
            identity.refresh_millihz != request.refresh_millihz ||
            identity.topology_generation != request.topology_generation ||
            !factory_) {
          return PLANK_BACKEND_OPERATION_UNSUPPORTED_V1;
        }

        try {
          if (!factory_->qualifies(translated->encoding_mode)) {
            return PLANK_BACKEND_OPERATION_UNSUPPORTED_V1;
          }
          return worker_.invoke([this, request, translated]() {
            if (engine_) {
              return PLANK_BACKEND_OPERATION_INVALID_ARGUMENT_V1;
            }
            auto engine = factory_->open(
              request.media_session->display(), translated->config
            );
            if (!engine) {
              return PLANK_BACKEND_OPERATION_UNAVAILABLE_V1;
            }
            engine_ = std::move(engine);
            profile_id_ = request.profile_id;
            media_session_ = request.media_session;
            return PLANK_BACKEND_OPERATION_OK_V1;
          });
        } catch (...) {
          return PLANK_BACKEND_OPERATION_FAILED_V1;
        }
      }

      PlankBackendOperationResultV1 submit(
          const PlankRetainedEncoderFrame &frame) override {
        if (frame.backend_frame_handle == 0U || frame.plane_count != 1U) {
          return PLANK_BACKEND_OPERATION_INVALID_ARGUMENT_V1;
        }
        auto *image = reinterpret_cast<platf::img_t *>(
          frame.backend_frame_handle
        );
        try {
          return worker_.invoke([this, image, frame]() {
            if (!engine_ || !media_session_) {
              return PLANK_BACKEND_OPERATION_UNAVAILABLE_V1;
            }
            const auto &identity = media_session_->identity();
            if (frame.profile_id != profile_id_ ||
                frame.profile_id != identity.profile_id ||
                frame.pixel_layout != identity.pixel_layout ||
                frame.memory_kind != identity.memory_kind ||
                frame.width != identity.width || frame.height != identity.height ||
                frame.topology_generation != identity.topology_generation) {
              return PLANK_BACKEND_OPERATION_INVALID_ARGUMENT_V1;
            }
            return operation_result(engine_->submit(
              *image, frame.frame_sequence, frame.monotonic_timestamp_ns
            ));
          });
        } catch (...) {
          return PLANK_BACKEND_OPERATION_FAILED_V1;
        }
      }

      PlankBackendOperationResultV1 next(
          std::uint32_t timeout_ms,
          PlankRetainedEncodedPacket &packet) override {
        (void) timeout_ms;
        packet = {};
        try {
          return worker_.invoke([this, &packet]() {
            if (!engine_) {
              return PLANK_BACKEND_OPERATION_UNAVAILABLE_V1;
            }
            video::retained_encoder_packet_t encoded;
            const auto result = engine_->next(encoded);
            if (result != video::retained_encoder_result_e::ok) {
              return operation_result(result);
            }
            if (!encoded.bytes || encoded.bytes->empty() ||
                encoded.frame_sequence == 0U ||
                encoded.monotonic_timestamp_ns == 0U) {
              return PLANK_BACKEND_OPERATION_FAILED_V1;
            }
            std::uint32_t flags = 0U;
            if (encoded.key_frame) {
              flags |= PLANK_MEDIA_PACKET_KEY_FRAME_V1;
            }
            if (encoded.codec_config) {
              flags |= PLANK_MEDIA_PACKET_CODEC_CONFIG_V1;
            }
            if (encoded.end_of_frame) {
              flags |= PLANK_MEDIA_PACKET_END_OF_FRAME_V1;
            }
            packet = {
              profile_id_, flags, encoded.frame_sequence,
              encoded.monotonic_timestamp_ns,
              encoded.monotonic_timestamp_ns, encoded.bytes->data(),
              encoded.bytes->size(), std::move(encoded.bytes),
            };
            return PLANK_BACKEND_OPERATION_OK_V1;
          });
        } catch (...) {
          packet = {};
          return PLANK_BACKEND_OPERATION_FAILED_V1;
        }
      }

      PlankBackendOperationResultV1 set_target_bitrate(
          std::uint64_t target_bitrate_bps) override {
        if (target_bitrate_bps == 0U || target_bitrate_bps % 1000U != 0U ||
            target_bitrate_bps / 1000U >
              std::numeric_limits<std::uint32_t>::max()) {
          return PLANK_BACKEND_OPERATION_INVALID_ARGUMENT_V1;
        }
        const auto bitrate_kbps =
          static_cast<std::uint32_t>(target_bitrate_bps / 1000U);
        try {
          return worker_.invoke([this, bitrate_kbps]() {
            return engine_ ?
              operation_result(engine_->set_target_bitrate(bitrate_kbps)) :
              PLANK_BACKEND_OPERATION_UNAVAILABLE_V1;
          });
        } catch (...) {
          return PLANK_BACKEND_OPERATION_FAILED_V1;
        }
      }

      PlankBackendOperationResultV1 recover(
          const PlankEncoderRecoveryRequestV1 &request) override {
        try {
          return worker_.invoke([this, request]() {
            if (!engine_) {
              return PLANK_BACKEND_OPERATION_UNAVAILABLE_V1;
            }
            if (request.mode == PLANK_ENCODER_RECOVERY_REQUEST_IDR_V1) {
              return operation_result(engine_->request_idr());
            }
            if (request.mode ==
                  PLANK_ENCODER_RECOVERY_INVALIDATE_REFERENCE_FRAMES_V1) {
              return operation_result(engine_->invalidate_reference_frames(
                request.first_frame_sequence, request.last_frame_sequence
              ));
            }
            return PLANK_BACKEND_OPERATION_INVALID_ARGUMENT_V1;
          });
        } catch (...) {
          return PLANK_BACKEND_OPERATION_FAILED_V1;
        }
      }

      PlankBackendOperationResultV1 flush() override {
        try {
          return worker_.invoke([this]() {
            return engine_ ? operation_result(engine_->flush()) :
                             PLANK_BACKEND_OPERATION_UNAVAILABLE_V1;
          });
        } catch (...) {
          return PLANK_BACKEND_OPERATION_FAILED_V1;
        }
      }

      void close() noexcept override {
        try {
          worker_.invoke([this]() {
            engine_.reset();
            media_session_.reset();
            profile_id_ = 0U;
          });
        } catch (...) {
        }
      }

    private:
      std::shared_ptr<video::retained_encoder_factory_t> factory_;
      encoder_worker_t worker_;
      std::unique_ptr<video::retained_encoder_engine_t> engine_;
      std::shared_ptr<PlankRetainedHostMediaSessionContext> media_session_;
      std::uint16_t profile_id_ {};
    };
  }  // namespace

  std::shared_ptr<IPlankRetainedEncoderTarget>
  create_retained_host_encoder_target_v1(
      std::shared_ptr<video::retained_encoder_factory_t> factory) {
    if (!factory) {
      return {};
    }
    return std::make_shared<retained_host_encoder_target_t>(
      std::move(factory)
    );
  }
}  // namespace plank::platform::linux_backend
