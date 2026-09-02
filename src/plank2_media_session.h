/**
 * @file src/plank2_media_session.h
 * @brief Shared retained Host capture/encoder session ownership.
 */
/* SPDX-License-Identifier: GPL-3.0-only */

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include <sys/types.h>

namespace platf {
  class display_t;
}

/**
 * Immutable identity of one retained Host media session.
 *
 * Capture publishes this identity after opening the real display. The encoder
 * must present the same tuple before it may share that display. Bitrate is not
 * part of the identity because it may change while the session is active.
 */
struct PlankRetainedHostMediaSessionIdentity {
  uid_t account_uid {};
  std::uint16_t profile_id {};
  std::uint16_t pixel_layout {};
  std::uint16_t memory_kind {};
  std::uint32_t width {};
  std::uint32_t height {};
  std::uint32_t refresh_millihz {};
  std::string topology_generation;
};

/**
 * Owns the original capture display for the complete capture/encode session.
 *
 * The encoder may retain this context, but never a producer frame, plane
 * pointer, or native frame handle. Keeping this context alive after capture
 * stops deliberately prevents a replacement display from opening until the
 * old encoder has also closed.
 */
class PlankRetainedHostMediaSessionContext final {
public:
  PlankRetainedHostMediaSessionContext(
    PlankRetainedHostMediaSessionIdentity identity,
    std::shared_ptr<platf::display_t> display
  );

  const PlankRetainedHostMediaSessionIdentity &identity() const noexcept;
  const std::shared_ptr<platf::display_t> &display() const noexcept;

private:
  PlankRetainedHostMediaSessionIdentity identity_;
  std::shared_ptr<platf::display_t> display_;
};

enum class PlankRetainedHostMediaSessionResult {
  ok,
  invalid,
  occupied,
  unavailable,
  mismatch,
};

/**
 * Instance-owned rendezvous between one capture source and one encoder target.
 *
 * This object must be created by the product session assembly and passed to
 * both sides. It is intentionally neither global nor keyed by an opaque
 * process-wide handle. A publication is discoverable only until capture
 * unpublishes it. Its weak reference remains as an overlap guard until every
 * encoder owner has released the old context.
 */
class PlankRetainedHostMediaSessionSlot final {
public:
  PlankRetainedHostMediaSessionResult publish(
    std::shared_ptr<PlankRetainedHostMediaSessionContext> context,
    std::uint64_t &publication_id
  );

  PlankRetainedHostMediaSessionResult acquire(
    const PlankRetainedHostMediaSessionIdentity &identity,
    std::shared_ptr<PlankRetainedHostMediaSessionContext> &context
  );

  void unpublish(std::uint64_t publication_id);

private:
  std::mutex mutex_;
  std::weak_ptr<PlankRetainedHostMediaSessionContext> context_;
  std::uint64_t publication_id_ {};
  std::uint64_t next_publication_id_ {1U};
};

const char *plank_retained_host_media_session_result_name(
  PlankRetainedHostMediaSessionResult result
) noexcept;
