/**
 * @file src/audio.cpp
 * @brief Definitions for audio capture and encoding.
 */
// standard includes
#include <chrono>
#include <map>
#include <mutex>
#include <thread>

// lib includes
#include <opus/opus_multistream.h>
#include <plank_transport.h>

// local includes
#include "audio.h"
#include "config.h"
#include "globals.h"
#include "logging.h"
#include "platform/common.h"
#ifdef __linux__
  #include "session/session_context.h"
#endif
#include "thread_safe.h"
#include "utility.h"

namespace audio {
  using namespace std::literals;
  /**
   * @brief Owning pointer for an Opus multistream encoder.
   */
  using opus_t = util::safe_ptr<OpusMSEncoder, opus_multistream_encoder_destroy>;
  /**
   * @brief Shared queue carrying captured PCM sample buffers to the encoder thread.
   */
  struct captured_block_t {
    std::vector<float> samples;
    std::uint64_t observed_us;
  };
  using sample_queue_t = std::shared_ptr<safe::queue_t<captured_block_t>>;

  static int start_audio_control(audio_ctx_t &ctx);
  static void stop_audio_control(audio_ctx_t &);
  static void apply_surround_params(opus_stream_config_t &stream, const stream_params_t &params);

  static std::string *select_capture_sink(
    audio_ctx_t &ctx, const opus_stream_config_t &stream, bool host_audio
  ) {
    // Keep this order shared by the legacy stream and the PLANK 2 adapter.
    // Virtual sink, configured sink, then host sink.
    std::string *sink = &ctx.sink.host;
    if (!config::audio.sink.empty()) sink = &config::audio.sink;
    if (ctx.sink.null && (!host_audio || sink->empty())) {
      auto &null = *ctx.sink.null;
      switch (stream.channelCount) {
        case 2: sink = &null.stereo; break;
        case 6: sink = &null.surround51; break;
        case 8: sink = &null.surround71; break;
      }
    }
    return sink;
  }

  static bool select_capture_sink_once(audio_ctx_t &ctx,
                                       const std::string &sink) {
    if (ctx.sink_flag->exchange(true, std::memory_order_acquire)) return true;
    ctx.restore_sink = ctx.sink.host != sink;
    return !ctx.restore_sink || ctx.control->set_sink(sink) == 0;
  }

  /**
   * @brief Select the Opus stream configuration for a channel count and quality tier.
   *
   * @param channels Number of audio channels in the stream.
   * @param quality Whether the high-quality Opus layout should be selected.
   * @return Index into `stream_configs` for the requested layout.
   */
  int map_stream(int channels, bool quality);

  constexpr auto SAMPLE_RATE = 48000;  ///< Audio sample rate in hertz required by Opus.

  // NOTE: If you adjust the bitrates listed here, make sure to update the
  // corresponding bitrate adjustment logic in native session negotiation.
  /**
   * @brief Opus stream layouts and bitrates advertised to clients.
   */
  opus_stream_config_t stream_configs[MAX_STREAM_CONFIG] {
    {
      SAMPLE_RATE,
      2,
      1,
      1,
      platf::speaker::map_stereo.data(),
      96000,
    },
    {
      SAMPLE_RATE,
      2,
      1,
      1,
      platf::speaker::map_stereo.data(),
      512000,
    },
    {
      SAMPLE_RATE,
      6,
      4,
      2,
      platf::speaker::map_surround51.data(),
      256000,
    },
    {
      SAMPLE_RATE,
      6,
      6,
      0,
      platf::speaker::map_surround51.data(),
      1536000,
    },
    {
      SAMPLE_RATE,
      8,
      5,
      3,
      platf::speaker::map_surround71.data(),
      450000,
    },
    {
      SAMPLE_RATE,
      8,
      8,
      0,
      platf::speaker::map_surround71.data(),
      2048000,
    },
  };

  /**
   * @brief Encode captured PCM samples into Opus packets on the audio worker thread.
   *
   * @param samples Queue of captured PCM sample buffers to encode.
   * @param config Audio stream settings negotiated with the client.
   * @param channel_data Platform-specific audio capture context passed to packet metadata.
   */
  void encodeThread(sample_queue_t samples, config_t config, void *channel_data) {
    auto packets = mail::man->queue<packet_t>(mail::audio_packets);
    auto stream = stream_configs[map_stream(config.channels, config.flags[config_t::HIGH_QUALITY])];
    if (config.flags[config_t::CUSTOM_SURROUND_PARAMS]) {
      apply_surround_params(stream, config.customStreamParams);
    }

    // Encoding takes place on this thread
    platf::set_thread_name("audio::encode");
    platf::adjust_thread_priority(platf::thread_priority_e::high);

    opus_t opus {opus_multistream_encoder_create(
      stream.sampleRate,
      stream.channelCount,
      stream.streams,
      stream.coupledStreams,
      stream.mapping,
      OPUS_APPLICATION_RESTRICTED_LOWDELAY,
      nullptr
    )};

    opus_multistream_encoder_ctl(opus.get(), OPUS_SET_BITRATE(stream.bitrate));
    opus_multistream_encoder_ctl(opus.get(), OPUS_SET_VBR(0));

    BOOST_LOG(info) << "Opus initialized: "sv << stream.sampleRate / 1000 << " kHz, "sv
                    << stream.channelCount << " channels, "sv
                    << stream.bitrate / 1000 << " kbps (total), LOWDELAY"sv;

    auto frame_size = config.packetDuration * stream.sampleRate / 1000;
    while (auto sample = samples->pop()) {
      buffer_t packet {1400};

      int bytes = opus_multistream_encode_float(opus.get(), sample->samples.data(), frame_size, std::begin(packet), (opus_int32) packet.size());
      if (bytes < 0) {
        BOOST_LOG(error) << "Couldn't encode audio: "sv << opus_strerror(bytes);
        packets->stop();

        return;
      }

      packet.fake_resize(bytes);
      packets->raise(channel_data, std::move(packet), sample->observed_us);
    }
  }

  /**
   * @brief Run the capture loop for this backend.
   */
  void capture(safe::mail_t mail, config_t config, void *channel_data) {
    auto shutdown_event = mail->event<bool>(mail::shutdown);
    if (!config::audio.stream) {
      shutdown_event->view();
      return;
    }
    auto stream = stream_configs[map_stream(config.channels, config.flags[config_t::HIGH_QUALITY])];
    if (config.flags[config_t::CUSTOM_SURROUND_PARAMS]) {
      apply_surround_params(stream, config.customStreamParams);
    }

    auto frame_size = config.packetDuration * stream.sampleRate / 1000;
    bool host_audio = config.flags[config_t::HOST_AUDIO];
    bool continuous_audio = config.flags[config_t::CONTINUOUS_AUDIO];
    int samples_per_frame = frame_size * stream.channelCount;

    auto generation = []() -> std::uint64_t {
#ifdef __linux__
      return plank::session::desktop_generation();
#else
      return 0;
#endif
    };

    enum class capture_result_e { shutdown, reattach, unavailable };
    auto capture_generation = [&](std::uint64_t attached_generation) {
      auto ref = get_audio_ctx_ref();
      if (!ref || !ref->control) return capture_result_e::unavailable;

      auto *sink = select_capture_sink(*ref.get(), stream, host_audio);
      if (!select_capture_sink_once(*ref.get(), *sink)) {
        return capture_result_e::unavailable;
      }

      auto mic = ref->control->microphone(
        stream.mapping, stream.channelCount, stream.sampleRate, frame_size,
        continuous_audio, host_audio
      );
      if (!mic) return capture_result_e::unavailable;

      auto samples = std::make_shared<sample_queue_t::element_type>(30);
      std::jthread thread {encodeThread, samples, config, channel_data};
      auto encoder_guard = util::fail_guard([&]() {
        samples->stop();
        thread.join();
      });

      while (!shutdown_event->peek()) {
        if (generation() != attached_generation) return capture_result_e::reattach;
        std::vector<float> sample_buffer(samples_per_frame);
        const auto status = mic->sample(sample_buffer);
        // Observe completion before queueing/encoding, using the same native
        // clock as video. This is not a hardware first-sample timestamp: Pulse
        // capture/device delay is not yet qualified, and must not be inferred.
        std::uint64_t observed_us {};
        if (status == platf::capture_e::ok &&
            plank_transport_clock_now_us(&observed_us) != PLANK_TRANSPORT_OK) {
          BOOST_LOG(error) << "Native audio clock read failed"sv;
          return capture_result_e::unavailable;
        }
        if (generation() != attached_generation) return capture_result_e::reattach;
        switch (status) {
          case platf::capture_e::ok:
            samples->raise(std::move(sample_buffer), observed_us);
            break;
          case platf::capture_e::timeout:
            break;
          case platf::capture_e::reinit:
            BOOST_LOG(info) << "Reinitializing audio capture"sv;
            mic.reset();
            do {
              mic = ref->control->microphone(
                stream.mapping, stream.channelCount, stream.sampleRate, frame_size,
                continuous_audio, host_audio
              );
              if (!mic) BOOST_LOG(warning) << "Couldn't re-initialize audio input"sv;
            } while (!mic && generation() == attached_generation &&
                     !shutdown_event->view(5s));
            if (generation() != attached_generation) return capture_result_e::reattach;
            if (!mic) return capture_result_e::shutdown;
            break;
          default:
            return capture_result_e::unavailable;
        }
      }
      return capture_result_e::shutdown;
    };

    platf::adjust_thread_priority(platf::thread_priority_e::critical);
    while (!shutdown_event->peek()) {
      const auto attached_generation = generation();
      const auto result = capture_generation(attached_generation);
      if (result == capture_result_e::shutdown) return;
      if (result == capture_result_e::reattach) {
        BOOST_LOG(info) << "Rebinding audio to desktop generation "sv << generation();
        continue;
      }

      BOOST_LOG(error) << "Unable to initialize audio capture. The stream will not have audio."sv;
      while (!shutdown_event->view(1s) && generation() == attached_generation) {}
    }
  }

  audio_ctx_ref_t get_audio_ctx_ref() {
    static std::mutex controls_mutex;
    static std::map<std::uint64_t, std::unique_ptr<safe::shared_t<audio_ctx_t>>> controls;
    std::uint64_t generation = 0;
#ifdef __linux__
    generation = plank::session::desktop_generation();
#endif
    std::lock_guard lock {controls_mutex};
    auto &control = controls[generation];
    if (!control) {
      control = std::make_unique<safe::shared_t<audio_ctx_t>>(
        start_audio_control, stop_audio_control
      );
    }
    return control->ref();
  }

  bool is_audio_ctx_sink_available(const audio_ctx_t &ctx) {
    if (!ctx.control) {
      return false;
    }

    const std::string &sink = ctx.sink.host.empty() ? config::audio.sink : ctx.sink.host;
    if (sink.empty()) {
      return false;
    }

    return ctx.control->is_sink_available(sink);
  }

  namespace {
    class legacy_pcm_capture_t final: public pcm_capture_t {
    public:
      legacy_pcm_capture_t(audio_ctx_ref_t ref, std::unique_ptr<platf::mic_t> mic,
                           uid_t account_uid, std::uint64_t generation,
                           std::size_t sample_count):
          ref_ {std::move(ref)}, mic_ {std::move(mic)},
          account_uid_ {account_uid}, generation_ {generation},
          sample_count_ {sample_count} {
      }

      pcm_capture_result_e next(pcm_block_t &block) override {
#ifdef __linux__
        if (plank::session::desktop_generation() != generation_ ||
            !plank::session::supervisor_attests_account_for_active_seat0(
              account_uid_)) {
          return pcm_capture_result_e::reattach;
        }
#endif
        block.samples.assign(sample_count_, 0.0F);
        const auto result = mic_->sample(block.samples);
#ifdef __linux__
        if (plank::session::desktop_generation() != generation_ ||
            !plank::session::supervisor_attests_account_for_active_seat0(
              account_uid_)) {
          block.samples.clear();
          return pcm_capture_result_e::reattach;
        }
#endif
        block.monotonic_timestamp_ns = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
          ).count()
        );
        switch (result) {
          case platf::capture_e::ok:
            return pcm_capture_result_e::ok;
          case platf::capture_e::timeout:
            return pcm_capture_result_e::timeout;
          case platf::capture_e::reinit:
          case platf::capture_e::interrupted:
          case platf::capture_e::error:
          default:
            block.samples.clear();
            return pcm_capture_result_e::unavailable;
        }
      }

    private:
      audio_ctx_ref_t ref_;
      std::unique_ptr<platf::mic_t> mic_;
      uid_t account_uid_ {};
      std::uint64_t generation_ {};
      std::size_t sample_count_ {};
    };
  }

  bool pcm_capture_available(uid_t account_uid) {
#ifdef __linux__
    return plank::session::desktop_generation() != 0U &&
           plank::session::supervisor_attests_account_for_active_seat0(
             account_uid
           );
#else
    (void) account_uid;
    return false;
#endif
  }

  std::unique_ptr<pcm_capture_t> open_pcm_capture(
      uid_t account_uid, const std::uint8_t *mapping, int channels,
      std::uint32_t sample_rate, std::uint32_t frame_size,
      bool continuous_audio, bool preserve_host_playback) {
#ifndef __linux__
    (void) account_uid;
    (void) mapping;
    (void) channels;
    (void) sample_rate;
    (void) frame_size;
    (void) continuous_audio;
    (void) preserve_host_playback;
    return nullptr;
#else
    if (mapping == nullptr || channels <= 0 || channels > 8 ||
        sample_rate != SAMPLE_RATE || frame_size == 0U ||
        !pcm_capture_available(account_uid)) return nullptr;
    const auto generation = plank::session::desktop_generation();
    auto ref = get_audio_ctx_ref();
    if (!ref || !ref->control) return nullptr;
    opus_stream_config_t stream {
      static_cast<std::int32_t>(sample_rate), channels, 0, 0, mapping, 0
    };
    auto *sink = select_capture_sink(*ref.get(), stream,
                                     preserve_host_playback);
    if (!select_capture_sink_once(*ref.get(), *sink)) return nullptr;
    auto mic = ref->control->microphone(
      mapping, channels, sample_rate, frame_size, continuous_audio,
      preserve_host_playback
    );
    if (!mic || plank::session::desktop_generation() != generation ||
        !pcm_capture_available(account_uid)) return nullptr;
    return std::make_unique<legacy_pcm_capture_t>(
      std::move(ref), std::move(mic), account_uid, generation,
      static_cast<std::size_t>(frame_size) * static_cast<std::size_t>(channels)
    );
#endif
  }

  /**
   * @brief Select the Opus stream configuration for a channel count and quality tier.
   */
  int map_stream(int channels, bool quality) {
    int shift = quality ? 1 : 0;
    switch (channels) {
      case 2:
        return STEREO + shift;
      case 6:
        return SURROUND51 + shift;
      case 8:
        return SURROUND71 + shift;
    }
    return STEREO;
  }

  int start_audio_control(audio_ctx_t &ctx) {
    auto fg = util::fail_guard([]() {
      BOOST_LOG(warning) << "There will be no audio"sv;
    });

    ctx.sink_flag = std::make_unique<std::atomic_bool>(false);

    // The default sink has not been replaced yet.
    ctx.restore_sink = false;

    if (!(ctx.control = platf::audio_control())) {
      return 0;
    }

    auto sink = ctx.control->sink_info();
    if (!sink) {
      // Let the calling code know it failed
      ctx.control.reset();
      return 0;
    }

    ctx.sink = std::move(*sink);

    fg.disable();
    return 0;
  }

  void stop_audio_control(audio_ctx_t &ctx) {
    // restore audio-sink if applicable
    if (!ctx.restore_sink) {
      return;
    }

    // Change back to the host sink, unless there was none
    const std::string &sink = ctx.sink.host.empty() ? config::audio.sink : ctx.sink.host;
    if (!sink.empty()) {
      // Best effort, it's allowed to fail
      ctx.control->set_sink(sink);
    }
  }

  void apply_surround_params(opus_stream_config_t &stream, const stream_params_t &params) {
    stream.channelCount = params.channelCount;
    stream.streams = params.streams;
    stream.coupledStreams = params.coupledStreams;
    stream.mapping = params.mapping;
  }
}  // namespace audio
