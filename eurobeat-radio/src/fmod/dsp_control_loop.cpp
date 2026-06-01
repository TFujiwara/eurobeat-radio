#include "fh6/fmod/dsp_control_loop.hpp"
#include "fh6/fmod/radio_discovery.hpp"
#include "fh6/audio_source.hpp"
#include "fh6/audio_source_manager.hpp"
#include "fh6/log.hpp"

#include <chrono>
#include <cstring>

namespace fh6::fmod_bridge {

namespace {
using namespace std::chrono_literals;
constexpr auto kTick           = 20ms;
constexpr auto kDiscoveryRetry = 5s;
constexpr int kDiscoveryTries  = 120; // 10-minute budget; the radio system
                                      // isn't wired up until well into launch.

// Ticks of no read_callback progress (while the source is producing PCM)
// before we conclude the game tore the radio channel down. 1s @ 20ms.
constexpr int kStaleTickThreshold = 50;

// Minimum gap between two off/on station toggles. The toggle blocks ~300ms
// and the game needs a moment to reallocate the channel, so we leave it well
// alone in between rather than thrashing the radio.
constexpr auto kRetuneCooldown = 6s;

// SoundName of the placeholder sample our DSP overwrites. Matches the carrier
// shipped by the radio-mod media overlay; if absent, we fall back to the
// first chain-valid instance so a stale overlay doesn't silently break audio.
constexpr const char* kTargetSoundName = "HZ6_R9_PeterBroderick_EyesClosedandTraveling";
} // namespace

ControlLoop::ControlLoop(DSPBridge& bridge, const PEImage& img, PlaybackConfig initial_playback,
                         float configured_gain)
    : bridge_{bridge}, img_{img}, configured_gain_{configured_gain}, game_state_{img},
      playback_opts_{std::make_shared<const PlaybackConfig>(std::move(initial_playback))},
      thread_{[this](const std::stop_token& tok) { run(tok); }} {}

void ControlLoop::push_playback_options(PlaybackConfig opts) {
    auto next = std::make_shared<const PlaybackConfig>(std::move(opts));
    std::lock_guard lock{playback_opts_mtx_};
    playback_opts_ = std::move(next);
}

ControlLoop::~ControlLoop() {
    thread_.request_stop();
    if (thread_.joinable()) thread_.join();
}

void ControlLoop::run(const std::stop_token& tok) {
    log::info("[ctrl] control loop started");

    bool acquired = false;
    for (int attempt = 0; attempt < kDiscoveryTries && !tok.stop_requested(); ++attempt) {
        if (acquire_target()) {
            acquired = true;
            break;
        }
        for (auto t = std::chrono::steady_clock::now() + kDiscoveryRetry;
             std::chrono::steady_clock::now() < t && !tok.stop_requested();)
            std::this_thread::sleep_for(kTick);
    }
    if (!acquired) {
        log::warn("[ctrl] discovery timed out; control loop exiting");
        return;
    }

    // The radio HUD reads from the SampleProperties slots at a much lower
    // rate than the audio mixer. 4 Hz is more than enough and keeps the
    // memory writes off the hot path.
    constexpr int kMetaEveryNTicks = 12; // ~240 ms at the 20 ms tick rate.
    int meta_tick                  = 0;

    auto next = std::chrono::steady_clock::now();
    while (!tok.stop_requested()) {
        next += kTick;
        // Only keep the DSP installed while the user is on our target station.
        // When they tune to R1-R8 we fully uninstall — passthrough mode still
        // intercepts the radio channel's audio and FMOD silences it after ~1s
        // when our callback's channel count doesn't match the source's layout.
        // The mode gate below promotes us back to PCM on re-entry and
        // retarget_if_needed reinstalls the DSP on the live handle.
        const bool on_target_pre = game_state_.read().on_target_station;
        if (on_target_pre) bridge_.retarget_if_needed();
        else               bridge_.uninstall();
        bridge_.manager().pump_once();

        if (++meta_tick >= kMetaEveryNTicks) {
            meta_tick = 0;
            push_metadata();
        }

        // Staleness watchdog: while a source is actively producing audio,
        // FMOD's mixer should be invoking our read_callback every tick. If
        // call_count() freezes for ~1s, the game tore down the radio channel
        // and won't rebuild it on its own. Toggling the in-game station off
        // and back on is the only thing that makes it allocate a fresh
        // channel; retarget_if_needed() then re-attaches the DSP next tick.
        // Gated on R10 so we never yank the user off a station they chose.
        auto* active          = bridge_.manager().active();
        const bool busy       = active && (active->playback_state() == PlaybackState::playing ||
                                           active->playback_state() == PlaybackState::buffering);
        const std::uint64_t c = bridge_.call_count();

        // Read once per tick; used by the staleness watchdog, DSP mode gate,
        // gain ramp, and playback state machines below.
        const auto game_snap = game_state_.read();

        if (busy && c == prev_calls_) {
            if (++stale_ticks_ >= kStaleTickThreshold) {
                stale_ticks_   = 0;
                const auto now = std::chrono::steady_clock::now();
                if (now - last_retune_ >= kRetuneCooldown &&
                    game_snap.on_target_station &&
                    game_state_.retune_streamer_station()) {
                    last_retune_ = now;
                    // The toggle may hand us a freshly-allocated RadioStreamFmod;
                    // re-point at the live one so retarget_if_needed installs there.
                    acquire_target();
                }
            }
        } else {
            stale_ticks_ = 0;
        }

        // Gate DSP mode on Horizon Opus selection. When the user is on a
        // different station, switch to passthrough so the game's own audio
        // flows through our DSP chain instead of being replaced or silenced.
        const DSPMode wanted_mode =
            game_snap.on_target_station ? DSPMode::pcm : DSPMode::passthrough;
        const bool entering_pcm = bridge_.mode() != wanted_mode && wanted_mode == DSPMode::pcm;
        if (bridge_.mode() != wanted_mode) {
            bridge_.set_mode(wanted_mode);
            // Drain stale audio that accumulated in the buffer while the user
            // was on another station so playback starts from the live position.
            if (wanted_mode == DSPMode::pcm)
                bridge_.manager().ring().drain();
            // Force an immediate retarget so the DSP is installed on the active
            // R9 handle before the next callback runs — eliminates the audible
            // gap where the game's original R9 track plays unfiltered.
            if (entering_pcm)
                bridge_.retarget_if_needed();
        }

        run_playback_state_machines(std::chrono::steady_clock::now());
        prev_calls_ = c;

        const float target = [this, active, &game_snap] {
            if (!active || !game_snap.on_target_station) return 0.0f;
            switch (active->playback_state()) {
                case PlaybackState::playing:
                case PlaybackState::buffering:
                    return configured_gain_.load(std::memory_order_acquire);
                default: return 0.0f;
            }
        }();
        // 1-pole low-pass at ~100 ms so play/pause fades smoothly.
        // Skip the fade when entering R9 so our stream comes in at full gain
        // instantly instead of bleeding ~600ms of the game's original R9 audio.
        const float cur = bridge_.gain();
        float next_g    = entering_pcm ? target : cur + (target - cur) * 0.1f;
        if (std::abs(next_g - cur) < 1e-4f) next_g = target;
        bridge_.set_gain(next_g);

        std::this_thread::sleep_until(next);
    }
    log::info("[ctrl] control loop exiting");
}

bool ControlLoop::acquire_target() noexcept {
    auto disc                   = discover_radio_instances(img_);
    const RadioInstance* chosen = select_instance(disc);
    if (!chosen) return false;
    if (!chosen->sound_name.starts_with("HZ6_R9_"))
        log::warn(R"([ctrl] no instance matches target "HZ6_R9_*"; falling back to "{}")",
                  chosen->sound_name);

    void* fmod_system = resolve_fmod_system(img_, chosen->radio_stream);
    if (!fmod_system) {
        log::warn("[ctrl] FMOD SystemI resolution failed");
        return false;
    }
    bridge_.set_target(*chosen, fmod_system);
    meta_.set_target(chosen->sample_props_body);
    log::info("[ctrl] targeting RadioStreamFmod @0x{:X} SoundName=\"{}\" SystemI*=0x{:X}",
              reinterpret_cast<uintptr_t>(chosen->radio_stream), chosen->sound_name,
              reinterpret_cast<uintptr_t>(fmod_system));
    return true;
}

const RadioInstance* ControlLoop::select_instance(const DiscoveryResult& disc) const noexcept {
    const RadioInstance* target   = nullptr;  // first R9 match, any handle state
    for (auto& i : disc.instances) {
        const bool is_target = i.sound_name.starts_with("HZ6_R9_");
        // FH6 can spin up several streams sharing the radio name (e.g.
        // an idle secondary mix); prefer the one whose channel is actually
        // live so we attach to the stream that's carrying audio.
        if (is_target && bridge_.channel_handle_alive(i.radio_stream)) return &i;
        if (is_target && !target) target = &i;
    }
    return target;
}

void ControlLoop::run_playback_state_machines(time_point now) noexcept {
    using namespace std::chrono_literals;
    // Debounce constants. 45 s ignores spurious race-flag flips during
    // loading screens; the 5 s race-restart window stays separate from the
    // 45 s race-start floor so a quick restart-then-engage still dispatches.
    constexpr auto kQuickSkipWindow     = 1000ms;
    constexpr auto kSpircCooldown       = 1500ms;
    constexpr auto kRaceStartDebounce   = 45s;
    constexpr auto kRaceRestartDebounce = 5s;

    std::shared_ptr<const PlaybackConfig> opts;
    {
        std::lock_guard lock{playback_opts_mtx_};
        opts = playback_opts_;
    }
    if (!opts) return;
    auto* active = bridge_.manager().active();
    if (!active) {
        prev_r10_ = prev_race_ = prev_race_restart_ = false;
        quick_skip_armed_ = false;
        return;
    }

    const auto game = game_state_.read();
    // R10 = "user is currently tuned to our station" via FH6 game state, NOT
    // FMOD channel aliveness. FMOD flaps the channel during race scene
    // transitions even though the user stayed on our station, which used to
    // trip a phantom quickStationSkip on every race start.
    const bool r10 = game.on_target_station;
    auto& ring     = bridge_.manager().ring();

    // --- raceStartPlayback (race_active edge, gated by R10 + debounces) ---
    const bool race_edge_in    = game.race_active && !prev_race_;
    const bool restart_edge_in = game.race_restart && !prev_race_restart_;
    const bool race_event      = (race_edge_in || restart_edge_in) && r10;
    const auto race_debounce   = restart_edge_in ? kRaceRestartDebounce : kRaceStartDebounce;
    if (race_event && now - last_race_event_ >= race_debounce &&
        now - last_skip_cmd_ >= kSpircCooldown) {
        const auto& mode    = opts->race_start_playback;
        const char* outcome = "keeping current position";
        bool fired          = false;
        if (mode == "next") {
            fired   = active->skip_next();
            outcome = fired ? "advanced to next track" : "could not advance queue";
        } else if (mode == "restart") {
            fired   = active->restart_current();
            outcome = fired ? "restarted current track" : "could not restart current track";
        }
        if (fired) {
            ring.drain();
            last_skip_cmd_ = now;
        }
        last_race_event_ = now;
        log::info("[ctrl] race {} -- {}", restart_edge_in ? "restarted" : "started", outcome);
    }
    prev_race_         = game.race_active;
    prev_race_restart_ = game.race_restart;

    // --- quickStationSkip (R10 edge) ---
    if (prev_r10_ && !r10) {
        last_r10_off_ = now;
        if (opts->quick_station_skip) quick_skip_armed_ = true;
    } else if (!prev_r10_ && r10) {
        if (quick_skip_armed_ && now - last_r10_off_ <= kQuickSkipWindow &&
            now - last_skip_cmd_ >= kSpircCooldown) {
            if (active->skip_next()) {
                ring.drain();
                last_skip_cmd_ = now;
                log::info("[ctrl] quick station return -- advanced to next track");
            }
        }
        quick_skip_armed_ = false;
    }
    prev_r10_ = r10;
}

void ControlLoop::push_metadata() noexcept {
    // Re-acquire active station target to update pointers in memory
    // (critical for event/race transitions where the game recreates FMOD instances)
    acquire_target();

    auto* a = bridge_.manager().active();
    if (!a) {
        meta_.update("FH6 Universal Radio", "Idle");
        return;
    }
    TrackInfo info;
    try {
        info = a->current_track();
    } catch (...) {
        return;
    }
    std::string title  = !info.title.empty() ? info.title : std::string{a->display_name()};
    std::string artist = info.artist;
    if (artist.empty()) {
        switch (a->playback_state()) {
            case PlaybackState::playing:   artist = "Playing"; break;
            case PlaybackState::buffering: artist = "Buffering"; break;
            case PlaybackState::paused:    artist = "Paused"; break;
            case PlaybackState::stopped:   artist = "Stopped"; break;
        }
    }
    meta_.update(title, artist);
}

} // namespace fh6::fmod_bridge
