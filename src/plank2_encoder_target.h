/**
 * @file src/plank2_encoder_target.h
 * @brief Concrete PLANK2 target for the retained Host encoder engine.
 */
/* SPDX-License-Identifier: GPL-3.0-only */

#pragma once

#include "plank2_encoder_source.h"
#include "plank2_retained_encoder_engine.h"

#include <memory>

namespace plank::platform::linux_backend {
  std::shared_ptr<IPlankRetainedEncoderTarget>
  create_retained_host_encoder_target_v1(
    std::shared_ptr<video::retained_encoder_factory_t> factory
  );
}
