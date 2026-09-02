/**
 * @file src/audio_pcm.h
 * @brief Lightweight raw PCM boundary for the PLANK 2 audio adapter.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <sys/types.h>

namespace audio {
  /** Result of one raw PCM capture attempt. */
  enum class pcm_capture_result_e {
    ok,
    timeout,
    reattach,
    unavailable,
  };

  /** One captured interleaved floating-point PCM block. */
  struct pcm_block_t {
    std::vector<float> samples;
    std::uint64_t monotonic_timestamp_ns {};
  };

  /**
   * Raw PCM capture lease used by the PLANK 2 side-by-side audio adapter.
   *
   * The accepted PLANK 1 stream remains the owner of Opus encoding. This
   * narrow source exposes the same selected system-output monitor before that
   * encoder so PLANK 2 can qualify capture independently.
   */
  class pcm_capture_t {
  public:
    virtual ~pcm_capture_t() = default;
    virtual pcm_capture_result_e next(pcm_block_t &block) = 0;
  };

  /** Open the accepted Linux system-output capture path for one owner. */
  std::unique_ptr<pcm_capture_t> open_pcm_capture(
    uid_t account_uid, const std::uint8_t *mapping, int channels,
    std::uint32_t sample_rate, std::uint32_t frame_size,
    bool continuous_audio, bool preserve_host_playback
  );

  /** Return true only while the supervisor attests the requested owner. */
  bool pcm_capture_available(uid_t account_uid);
}  // namespace audio
