/**
 * @file src/session_stream.cpp
 * @brief Native QUIC session negotiation and active-stream ownership.
 */

extern "C" {
#include <moonlight-common-c/src/Limelight.h>
#include <plank_transport.h>
#include <plank_transport_setup.h>
}

// standard includes
#include <algorithm>
#include <array>
#include <chrono>
#include <optional>
#include <set>
#include <utility>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "audio.h"
#include "config.h"
#include "globals.h"
#include "input.h"
#include "logging.h"
#include "session_stream.h"
#include "plank_bitrate.h"
#include "plank/platform/linux/legacy_profile_adapter_v1.h"
#include "plank_topology.h"
#include "stream.h"
#include "sync.h"
#include "thread_safe.h"
#include "video.h"

using namespace std::literals;

namespace session_stream {
  class session_server_t {
  public:
    void session_raise(std::shared_ptr<launch_session_t> launch_session) {
      {
        auto lock = launch_owner_.lock();
        if (*launch_owner_) {
          BOOST_LOG(error) << "Attempted to queue a second PLANK launch owner"sv;
          return;
        }
        *launch_owner_ = launch_session;
      }
      launch_event.raise(std::move(launch_session));
    }

    void session_complete(const std::uint32_t launch_session_id) {
      auto lock = launch_owner_.lock();
      if (*launch_owner_ && (*launch_owner_)->id == launch_session_id) {
        launch_owner_->reset();
      }
    }

    bool launch_owned() {
      auto lock = launch_owner_.lock();
      return *launch_owner_ != nullptr;
    }

    std::shared_ptr<launch_session_t> revoke_launch() {
      auto lock = launch_owner_.lock();
      auto launch_session = std::move(*launch_owner_);
      launch_owner_->reset();
      launch_event.pop(0s);
      return launch_session;
    }

    int session_count() {
      auto lock = session_slots_.lock();
      return static_cast<int>(session_slots_->size());
    }

    void clear(const bool all = true, const std::uint32_t termination_reason = 0) {
      auto lock = session_slots_.lock();
      for (auto iterator = session_slots_->begin();
           iterator != session_slots_->end();) {
        auto &slot = *(*iterator);
        if (all || stream::session::state(slot) == stream::session::state_e::STOPPING) {
          if (termination_reason != 0 &&
              stream::session::state(slot) != stream::session::state_e::STOPPING) {
            stream::session::stop(slot, termination_reason);
          } else {
            stream::session::stop(slot);
          }
          stream::session::join(slot);
          iterator = session_slots_->erase(iterator);
        } else {
          ++iterator;
        }
      }
    }

    void notify_desktop_handoff() {
      auto lock = session_slots_.lock();
      for (auto iterator = session_slots_->begin(); iterator != session_slots_->end(); ++iterator) {
        stream::session::notify_desktop_handoff(*(*iterator));
      }
    }

    void remove(const std::shared_ptr<stream::session_t> &session) {
      auto lock = session_slots_.lock();
      session_slots_->erase(session);
    }

    void insert(const std::shared_ptr<stream::session_t> &session) {
      auto lock = session_slots_.lock();
      session_slots_->emplace(session);
      BOOST_LOG(info) << "New streaming session started [active sessions: "sv
                      << session_slots_->size() << ']';
    }

    safe::event_t<std::shared_ptr<launch_session_t>> launch_event;

  private:
    sync_util::sync_t<std::set<std::shared_ptr<stream::session_t>>> session_slots_;
    // Retain ownership from HTTP acceptance through native setup. The setup
    // worker removes the request from launch_event while it waits for QUIC,
    // so the queue alone cannot represent an active session claim.
    sync_util::sync_t<std::shared_ptr<launch_session_t>> launch_owner_;
  };

  session_server_t server {};

  void launch_session_raise(std::shared_ptr<launch_session_t> launch_session) {
    server.session_raise(std::move(launch_session));
  }

  int session_count() {
    server.clear(false);
    return server.session_count();
  }

  bool launch_session_pending() {
    return server.launch_owned();
  }

  void terminate_sessions(const std::uint32_t termination_reason) {
    // Revoke an accepted launch that has not yet created its media session.
    // Releasing its one-use endpoint prevents the displaced peer from racing
    // the replacement launch after the Desktop reservation changes owner.
    auto launch_session = server.revoke_launch();
    if (launch_session && launch_session->plank_transport_endpoint) {
      auto *endpoint = static_cast<PlankTransportNativeEndpoint *>(
        launch_session->plank_transport_endpoint.get()
      );
      plank_transport_native_endpoint_stop(endpoint);
    }
    server.clear(true, termination_reason);
    input::terminate_retained_input();
  }

  void notify_desktop_handoff() {
    server.notify_desktop_handoff();
  }

  struct native_start_result_t {
    int status {PLANK_TRANSPORT_SETUP_STATUS_INTERNAL_ERROR};
    std::string message;
    nlohmann::json response;
  };

  native_start_result_t start_native_session(
    const std::shared_ptr<launch_session_t> &launch_session,
    const nlohmann::json &request
  ) {
    native_start_result_t result;
    if (!launch_session || !request.is_object()) {
      result.status = PLANK_TRANSPORT_SETUP_STATUS_INVALID_REQUEST;
      result.message = "Malformed native session request";
      return result;
    }

    const auto *media_profile = plank_media_profile_find_v1(
      launch_session->media_profile_id
    );
    PlankLinuxLegacyMediaProfileV1 legacy_profile {};
    if (media_profile == nullptr ||
        plank_linux_legacy_media_profile_v1(media_profile, &legacy_profile) !=
          PLANK_LINUX_LEGACY_PROFILE_OK_V1 ||
        launch_session->capture_source != legacy_profile.capture_source ||
        launch_session->encoder_backend != legacy_profile.encoder_backend ||
        launch_session->encoding_mode != legacy_profile.encoding_mode) {
      result.status = PLANK_TRANSPORT_SETUP_STATUS_UNSUPPORTED;
      result.message = "Accepted media profile is invalid";
      return result;
    }

    stream::config_t config {};
    config.monitor.span_desktop = launch_session->span_desktop;
    config.monitor.output_name = launch_session->span_desktop ?
      std::string {} : launch_session->output_name;
    config.monitor.encoder_backend = legacy_profile.encoder_backend;
    if (media_profile->capture_source == PLANK_MEDIA_CAPTURE_NVFBC_8BIT_V1) {
      config.monitor.capture_source = video::capture_source_e::nvfbc_8bit;
    } else if (media_profile->capture_source ==
                 PLANK_MEDIA_CAPTURE_X11_XSHM_10BIT_V1) {
      config.monitor.capture_source = video::capture_source_e::x11_native10;
    } else {
      result.status = PLANK_TRANSPORT_SETUP_STATUS_UNSUPPORTED;
      result.message = "Unsupported capture source";
      return result;
    }

    int encoder_target_kbps {};
    std::uint32_t requested_video_format {};
    bool high_quality_audio {};
    try {
      const auto &video_request = request.at("video");
      const auto &audio_request = request.at("audio");
      config.monitor.width = video_request.at("width").get<int>();
      config.monitor.height = video_request.at("height").get<int>();
      config.monitor.framerate = video_request.at("fps").get<int>();
      config.monitor.framerateX100 = video_request.value("fps_x100", 0);
      config.monitor.slicesPerFrame = video_request.value("slices_per_frame", 1);
      config.monitor.numRefFrames = video_request.value("reference_frames", 0);
      config.monitor.encoderCscMode = video_request.at("encoder_csc_mode").get<int>();
      config.monitor.videoFormat = video_request.at("codec").get<int>();
      config.monitor.dynamicRange = video_request.at("ten_bit").get<bool>() ? 1 : 0;
      config.monitor.chromaSamplingType = video_request.at("chroma").get<int>();
      config.monitor.enableIntraRefresh = video_request.value("intra_refresh", 0);
      encoder_target_kbps = video_request.at("encoder_target_kbps").get<int>();
      requested_video_format = video_request.at("negotiated_format").get<std::uint32_t>();

      config.audio.channels = audio_request.at("channels").get<int>();
      config.audio.mask = audio_request.at("channel_mask").get<int>();
      config.audio.packetDuration = audio_request.value("packet_duration_ms", 5);
      high_quality_audio = audio_request.value("high_quality", false);
    } catch (const nlohmann::json::exception &) {
      result.status = PLANK_TRANSPORT_SETUP_STATUS_INVALID_REQUEST;
      result.message = "Missing or invalid native session field";
      return result;
    }

    if (config.monitor.width != launch_session->width ||
        config.monitor.height != launch_session->height ||
        config.monitor.framerate != launch_session->fps ||
        config.monitor.width <= 0 || config.monitor.height <= 0 ||
        config.monitor.framerate <= 0 || config.monitor.framerate > 240 ||
        config.monitor.framerateX100 < 0 ||
        config.monitor.slicesPerFrame <= 0 ||
        config.monitor.numRefFrames < 0 ||
        config.monitor.chromaSamplingType < 0 ||
        config.monitor.chromaSamplingType > 2 ||
        config.audio.packetDuration <= 0 || config.audio.packetDuration > 120 ||
        (config.audio.channels != 2 && config.audio.channels != 6 &&
         config.audio.channels != 8)) {
      result.status = PLANK_TRANSPORT_SETUP_STATUS_INVALID_REQUEST;
      result.message = "Native session values are outside qualified bounds";
      return result;
    }
    if (config.monitor.framerateX100 > 0) {
      const double strict_fps = config.monitor.framerateX100 / 100.0;
      const double ratio = strict_fps / config.monitor.framerate;
      if (ratio < 0.99 || ratio > 1.01) {
        config.monitor.framerateX100 = 0;
      }
    }

    if (requested_video_format != legacy_profile.video_format ||
        config.monitor.videoFormat != legacy_profile.video_codec ||
        config.monitor.dynamicRange != legacy_profile.dynamic_range ||
        config.monitor.chromaSamplingType != legacy_profile.chroma_sampling ||
        config.monitor.encoderCscMode != legacy_profile.encoder_csc_mode) {
      result.status = PLANK_TRANSPORT_SETUP_STATUS_UNSUPPORTED;
      result.message = "Native stream format differs from accepted media profile";
      return result;
    }

    const auto exact_target = plank::bitrate::validate_target(
      encoder_target_kbps
    );
    if (!exact_target) {
      result.status = PLANK_TRANSPORT_SETUP_STATUS_INVALID_REQUEST;
      result.message = "Encoder target is outside the qualified range";
      return result;
    }
    config.monitor.bitrate = *exact_target;
    const auto peak_target = *exact_target * config::video.sw.vbv_maxrate_percentage / 100;

    auto *plank_transport_endpoint = launch_session->plank_transport_endpoint ?
      static_cast<PlankTransportNativeEndpoint *>(launch_session->plank_transport_endpoint.get()) :
      nullptr;
    if (!plank_transport_endpoint ||
        plank_transport_native_set_video_bitrate(
          plank_transport_endpoint, static_cast<std::uint32_t>(*exact_target),
          static_cast<std::uint32_t>(peak_target)
        ) != PLANK_TRANSPORT_OK) {
      result.status = PLANK_TRANSPORT_SETUP_STATUS_INTERNAL_ERROR;
      result.message = "Unable to apply the negotiated video transport rate";
      return result;
    }

    if ((platf::get_capabilities() & platf::platform_caps::local_cursor) == 0) {
      result.status = PLANK_TRANSPORT_SETUP_STATUS_INTERNAL_ERROR;
      result.message = "PLANK local cursor transport is unavailable";
      return result;
    }

    config.audio.flags[audio::config_t::HOST_AUDIO] = launch_session->host_audio;
    config.audio.flags[audio::config_t::HIGH_QUALITY] = high_quality_audio;
    config.audio.flags[audio::config_t::CONTINUOUS_AUDIO] =
      launch_session->continuous_audio;
    const auto audio_index = audio::map_stream(
      config.audio.channels, high_quality_audio
    );
    if (audio_index < 0 || audio_index >= audio::MAX_STREAM_CONFIG) {
      result.status = PLANK_TRANSPORT_SETUP_STATUS_UNSUPPORTED;
      result.message = "Unsupported audio layout";
      return result;
    }
    const auto &opus = audio::stream_configs[audio_index];

    auto stream_session = stream::session::alloc(config, *launch_session);
    if (!stream_session) {
      result.status = PLANK_TRANSPORT_SETUP_STATUS_INTERNAL_ERROR;
      result.message = "Failed to open the PLANK2 retained video session";
      return result;
    }
    server.insert(stream_session);
    if (stream::session::start(*stream_session, {})) {
      server.remove(stream_session);
      result.status = PLANK_TRANSPORT_SETUP_STATUS_INTERNAL_ERROR;
      result.message = "Failed to start native streaming session";
      return result;
    }

    result.status = PLANK_TRANSPORT_SETUP_STATUS_OK;
    const auto host_feature_flags =
      static_cast<std::uint32_t>(platf::get_capabilities() |
                                 platf::platform_caps::dynamic_video_bitrate |
                                 platf::platform_caps::encoder_target_ack);
    result.response = {
      {"video_format", legacy_profile.video_format},
      {"host_feature_flags", host_feature_flags},
      {"reference_frame_invalidation",
       video::last_encoder_probe_supported_ref_frames_invalidation ? 1 : 0},
      {"audio", {
        {"sample_rate", opus.sampleRate},
        {"channels", opus.channelCount},
        {"streams", opus.streams},
        {"coupled_streams", opus.coupledStreams},
        {"packet_duration_ms", config.audio.packetDuration},
        {"mapping", nlohmann::json::array()},
      }},
    };
    for (int index = 0; index < opus.channelCount; ++index) {
      result.response["audio"]["mapping"].push_back(opus.mapping[index]);
    }
    BOOST_LOG(info) << "Native QUIC session negotiated: "sv
                    << config.monitor.width << 'x' << config.monitor.height
                    << '@' << config.monitor.framerate << " target="sv
                    << config.monitor.bitrate << " Kbps"sv;
    return result;
  }

  void process_native_launch(const std::shared_ptr<launch_session_t> &launch_session) {
    auto complete_launch = util::fail_guard([&]() {
      if (launch_session) {
        server.session_complete(launch_session->id);
      }
    });
    auto *endpoint = launch_session && launch_session->plank_transport_endpoint ?
      static_cast<PlankTransportNativeEndpoint *>(launch_session->plank_transport_endpoint.get()) :
      nullptr;
    if (!endpoint) {
      BOOST_LOG(error) << "Pending native launch has no PlankTransport endpoint"sv;
      return;
    }

    const auto timeout_ms = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
        config::stream.ping_timeout
      ).count(), 100, 120000
    ));
    if (plank_transport_native_endpoint_wait_ready(endpoint, timeout_ms) !=
        PLANK_TRANSPORT_OK) {
      std::array<char, 512> last_error {};
      plank_transport_native_endpoint_last_error(
        endpoint, last_error.data(), last_error.size()
      );
      BOOST_LOG(error) << "Native QUIC connection did not become ready: "sv
                       << last_error.data();
      return;
    }

    std::vector<std::uint8_t> packet(PLANK_TRANSPORT_SETUP_MAX_PACKET_SIZE);
    std::size_t packet_size {};
    const auto receive_result = plank_transport_native_data_receive(
      endpoint, packet.data(), packet.size(), &packet_size, timeout_ms
    );
    PlankTransportSetupPacket request_packet {};
    if (receive_result != PLANK_TRANSPORT_OK ||
        plank_transport_setup_decode(packet.data(), packet_size, &request_packet) != 0 ||
        request_packet.type != PLANK_TRANSPORT_SETUP_LAUNCH_REQUEST ||
        (request_packet.flags & PLANK_TRANSPORT_SETUP_FLAG_RESPONSE) != 0) {
      BOOST_LOG(error) << "Native session negotiation did not receive a valid launch request"sv;
      return;
    }

    const auto request_json = nlohmann::json::parse(
      request_packet.payload,
      request_packet.payload + request_packet.payload_size,
      nullptr,
      false
    );
    auto start_result = start_native_session(launch_session, request_json);
    nlohmann::json response_json = start_result.response;
    if (!start_result.message.empty()) {
      response_json["message"] = start_result.message;
    }
    const auto response_payload = response_json.dump();
    packet.resize(PLANK_TRANSPORT_SETUP_HEADER_SIZE + response_payload.size());
    std::size_t response_size {};
    const auto response_type = start_result.status == PLANK_TRANSPORT_SETUP_STATUS_OK ?
      PLANK_TRANSPORT_SETUP_LAUNCH_RESPONSE : PLANK_TRANSPORT_SETUP_ERROR;
    if (plank_transport_setup_encode(
          response_type,
          PLANK_TRANSPORT_SETUP_FLAG_RESPONSE,
          static_cast<std::uint16_t>(start_result.status),
          request_packet.request_id,
          reinterpret_cast<const std::uint8_t *>(response_payload.data()),
          response_payload.size(), packet.data(), packet.size(), &response_size
        ) != 0 ||
        plank_transport_native_data_send(endpoint, packet.data(), response_size) !=
          PLANK_TRANSPORT_OK) {
      BOOST_LOG(error) << "Unable to return native session negotiation result"sv;
    }
  }

  void start() {
    platf::set_thread_name("native-setup");
    auto shutdown_event = mail::man->event<bool>(mail::shutdown);
    auto broadcast_shutdown_event = mail::man->event<bool>(mail::broadcast_shutdown);
    BOOST_LOG(info) << "Native QUIC session negotiation active"sv;
    while (!shutdown_event->peek()) {
      auto launch_session = server.launch_event.pop(500ms);
      if (launch_session) {
        process_native_launch(launch_session);
      }
      if (broadcast_shutdown_event->peek()) {
        server.clear();
      } else {
        server.clear(false);
      }
    }
    terminate_sessions();
  }

}  // namespace session_stream
