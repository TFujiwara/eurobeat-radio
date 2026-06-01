#pragma once

#include "fh6/audio_source.hpp"
#include "fh6/config.hpp"
#include "fh6/playback_dsp.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace fh6::sources {

class InternetRadioSource final : public IAudioSource {
public:
    InternetRadioSource(InternetRadioConfig cfg, std::filesystem::path ffmpeg_path);
    ~InternetRadioSource() override;

    std::string_view name() const noexcept override { return "internet_radio"; }
    std::string_view display_name() const noexcept override { return "Eurobeat Radio"; }

    bool initialize() override;
    void shutdown() noexcept override;

    void play() override;
    void pause() override;
    void stop() override;
    void next() override {}
    void previous() override {}
    bool skip_next() override { return false; }
    bool restart_current() override { return false; }

    void pump(RingBuffer& ring) override;

    void set_config(InternetRadioConfig cfg);
    void set_ffmpeg_path(std::filesystem::path p);
    void set_playback_options(const PlaybackConfig& opts) override;

    TrackInfo current_track() const override;
    PlaybackState playback_state() const noexcept override {
        return state_.load(std::memory_order_acquire);
    }

    AuthState auth_state() const noexcept override { return AuthState::none_required; }
    std::string auth_instructions() const override { return ""; }

    SourceCapabilities capabilities() const noexcept override { return {false, false, false}; }

private:
    void start_pipe_locked();
    void stop_pipe_locked();
    void run_metadata_loop(const std::stop_token& tok);
    void update_metadata_from_api();
    void apply_pending_if_ready() noexcept;
    void run_icy_monitor(const std::stop_token& tok);

    InternetRadioConfig cfg_;
    std::filesystem::path ffmpeg_path_;

    struct Pipe;
    std::unique_ptr<Pipe> pipe_;

    mutable std::mutex mu_;
    std::atomic<PlaybackState> state_{PlaybackState::stopped};
    std::atomic<bool> volume_norm_{true};

    EqualizerStage eq_;

    // Metadata management
    std::jthread metadata_thread_;
    std::jthread icy_monitor_thread_;
    mutable std::mutex metadata_mu_;
    TrackInfo current_track_info_;   // what the HUD currently shows (guarded by metadata_mu_)

    // Pending track scheduled for display once its audio has reached us.
    // Only accessed from the metadata thread — no extra locking needed.
    std::optional<TrackInfo> pending_track_;
    std::chrono::system_clock::time_point pending_display_at_{};

    // Set by the ICY monitor when a new non-ad title arrives; causes the
    // metadata thread to poll the API immediately rather than wait for the
    // next 15-second tick.
    std::atomic<bool> request_immediate_poll_{false};

    // Approximate milliseconds of PCM audio currently queued in the ring
    // buffer; updated by pump() so the ICY / API latency estimate is live.
    std::atomic<uint32_t> approx_ring_ms_{0};

    // Reconnection control
    std::chrono::steady_clock::time_point last_connection_attempt_;
    bool needs_reconnect_ = false;

    // Set when the metadata API indicates an ad break (null artist on laut.fm).
    // pump() drains the ring and discards pipe data so the DSP outputs silence.
    std::atomic<bool> is_ad_{false};
};

} // namespace fh6::sources
