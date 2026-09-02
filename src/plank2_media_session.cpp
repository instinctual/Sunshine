/**
 * @file src/plank2_media_session.cpp
 * @brief Shared retained Host capture/encoder session ownership.
 */
/* SPDX-License-Identifier: GPL-3.0-only */

#include "plank2_media_session.h"

#include <utility>

namespace {
  bool identity_is_valid(
      const PlankRetainedHostMediaSessionIdentity &identity) noexcept {
    return identity.profile_id != 0U && identity.pixel_layout != 0U &&
           identity.memory_kind != 0U && identity.width != 0U &&
           identity.height != 0U && identity.refresh_millihz != 0U &&
           !identity.topology_generation.empty();
  }

  bool identities_match(
      const PlankRetainedHostMediaSessionIdentity &left,
      const PlankRetainedHostMediaSessionIdentity &right) noexcept {
    return left.account_uid == right.account_uid &&
           left.profile_id == right.profile_id &&
           left.pixel_layout == right.pixel_layout &&
           left.memory_kind == right.memory_kind &&
           left.width == right.width && left.height == right.height &&
           left.refresh_millihz == right.refresh_millihz &&
           left.topology_generation == right.topology_generation;
  }
}

PlankRetainedHostMediaSessionContext::PlankRetainedHostMediaSessionContext(
    PlankRetainedHostMediaSessionIdentity identity,
    std::shared_ptr<platf::display_t> display):
    identity_ {std::move(identity)}, display_ {std::move(display)} {
}

const PlankRetainedHostMediaSessionIdentity &
PlankRetainedHostMediaSessionContext::identity() const noexcept {
  return identity_;
}

const std::shared_ptr<platf::display_t> &
PlankRetainedHostMediaSessionContext::display() const noexcept {
  return display_;
}

PlankRetainedHostMediaSessionResult PlankRetainedHostMediaSessionSlot::publish(
    std::shared_ptr<PlankRetainedHostMediaSessionContext> context,
    std::uint64_t &publication_id) {
  publication_id = 0U;
  if (!context || !context->display() ||
      !identity_is_valid(context->identity())) {
    return PlankRetainedHostMediaSessionResult::invalid;
  }

  std::lock_guard lock {mutex_};
  if (!context_.expired()) {
    return PlankRetainedHostMediaSessionResult::occupied;
  }
  context_.reset();
  publication_id_ = 0U;

  std::uint64_t candidate = next_publication_id_++;
  if (candidate == 0U) candidate = next_publication_id_++;
  if (candidate == 0U) {
    return PlankRetainedHostMediaSessionResult::unavailable;
  }
  context_ = context;
  publication_id_ = candidate;
  publication_id = candidate;
  return PlankRetainedHostMediaSessionResult::ok;
}

PlankRetainedHostMediaSessionResult PlankRetainedHostMediaSessionSlot::acquire(
    const PlankRetainedHostMediaSessionIdentity &identity,
    std::shared_ptr<PlankRetainedHostMediaSessionContext> &context) {
  context.reset();
  if (!identity_is_valid(identity)) {
    return PlankRetainedHostMediaSessionResult::invalid;
  }

  std::lock_guard lock {mutex_};
  if (publication_id_ == 0U) {
    return PlankRetainedHostMediaSessionResult::unavailable;
  }
  auto current = context_.lock();
  if (!current) {
    publication_id_ = 0U;
    context_.reset();
    return PlankRetainedHostMediaSessionResult::unavailable;
  }
  if (!identities_match(current->identity(), identity)) {
    return PlankRetainedHostMediaSessionResult::mismatch;
  }
  context = std::move(current);
  return PlankRetainedHostMediaSessionResult::ok;
}

void PlankRetainedHostMediaSessionSlot::unpublish(
    std::uint64_t publication_id) {
  if (publication_id == 0U) return;
  std::lock_guard lock {mutex_};
  if (publication_id != publication_id_) return;
  publication_id_ = 0U;
}

const char *plank_retained_host_media_session_result_name(
    PlankRetainedHostMediaSessionResult result) noexcept {
  switch (result) {
    case PlankRetainedHostMediaSessionResult::ok:
      return "ok";
    case PlankRetainedHostMediaSessionResult::invalid:
      return "invalid";
    case PlankRetainedHostMediaSessionResult::occupied:
      return "occupied";
    case PlankRetainedHostMediaSessionResult::unavailable:
      return "unavailable";
    case PlankRetainedHostMediaSessionResult::mismatch:
      return "mismatch";
    default:
      return "unknown";
  }
}
