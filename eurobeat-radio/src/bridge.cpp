// Bridge entry point. Wires up config, sources, the FMOD DSP, and the HTTP
// dashboard, then parks the thread the DLL spawned us on.

#include "fh6/log.hpp"
#include "fh6/config.hpp"
#include "fh6/config_store.hpp"
#include "fh6/audio_source_manager.hpp"
#include "fh6/fmod/dsp_bridge.hpp"
#include "fh6/fmod/dsp_control_loop.hpp"
#include "fh6/fmod/pe_image.hpp"
#include "fh6/http/http_server.hpp"
#include "fh6/sources/local_file_source.hpp"
#include "fh6/sources/youtube_music_source.hpp"
#include "fh6/sources/jellyfin_source.hpp"
#include "fh6/sources/internet_radio_source.hpp"
#include "fh6/logo_hook.hpp"

#include <windows.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>

namespace fh6 {

namespace {

std::filesystem::path module_directory(HMODULE self) {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(self, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path{buf}.parent_path();
}

std::string slurp(const std::filesystem::path& p) {
    std::ifstream in{p, std::ios::binary};
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return std::move(ss).str();
}

// GPLv3 requires attribution, but enforcement is handled through documentation
// and licensing terms, not runtime checks. The original code checked for specific
// donation links at runtime; this fork removes that DRM while maintaining proper
// credits in README, LICENSE, and the dashboard.
bool verify_ui_credits(const std::filesystem::path& ui_dir) {
    const auto index = ui_dir / "index.html";
    const auto html  = slurp(index);
    if (html.empty()) {
        log::error("[bridge] webui index.html missing or unreadable at {}", index.string());
        return false;
    }
    // Attribution is verified through documentation, not runtime checks
    log::info("[bridge] webui loaded; credits and attribution in README/LICENSE");
    return true;
}

} // namespace

void run_bridge(HMODULE self) noexcept {
    const auto dir      = module_directory(self);
    const auto data_dir = dir / "fh6-radio";
    std::error_code ec;
    std::filesystem::create_directories(data_dir, ec);

    log::init(data_dir / "bridge.log");
    log::info("[bridge] FH6 Universal Radio starting; data_dir={}", data_dir.string());

    // Logo hook disabled - caused game freezes. Use apply_logos.py scripts instead.
    // install_logo_hook(dir);

    const auto ui_dir = data_dir / "ui";
    if (!verify_ui_credits(ui_dir)) {
        log::error("[bridge] aborting startup: webui credits/donation links check failed");
        return;
    }

    ConfigStore store{data_dir / "config.toml", load_config(data_dir / "config.toml")};
    auto cfg = store.snapshot();

    auto img = fmod_bridge::parse(reinterpret_cast<std::byte*>(GetModuleHandleW(nullptr)));
    if (!img.valid()) {
        log::error("[bridge] failed to parse host PE image; aborting");
        return;
    }
    fmod_bridge::FMODFns fns;
    if (!fmod_bridge::resolve_fmod_signatures(img, fns)) {
        log::warn("[bridge] some FMOD signatures unresolved -- DSP injection disabled");
    }

    const std::size_t ring_bytes = static_cast<std::size_t>(cfg.general.ring_buffer_mb) << 20;
    AudioSourceManager mgr{ring_bytes};

    // Register/unregister sources to match the enabled flags. Called at
    // startup and on every config change so toggling enabled adds/removes
    // the dashboard tile live, without a game restart.
    auto sync_sources = [&mgr](const Config& c) {
        if (c.local_files.enabled && !mgr.find("local_files")) {
            auto src = std::make_unique<sources::LocalFileSource>(c.local_files,
                                                                  c.general.ffmpeg_path);
            if (src->initialize()) mgr.register_source(std::move(src));
        } else if (!c.local_files.enabled && mgr.find("local_files")) {
            mgr.unregister_source("local_files");
        }
        if (c.youtube_music.enabled && !mgr.find("youtube_music")) {
            auto src = std::make_unique<sources::YouTubeMusicSource>(c.youtube_music,
                                                                     c.general.ffmpeg_path);
            if (src->initialize()) mgr.register_source(std::move(src));
        } else if (!c.youtube_music.enabled && mgr.find("youtube_music")) {
            mgr.unregister_source("youtube_music");
        }
        if (c.jellyfin.enabled && !mgr.find("jellyfin")) {
            auto src = std::make_unique<sources::JellyfinSource>(c.jellyfin, c.general.ffmpeg_path);
            if (src->initialize()) mgr.register_source(std::move(src));
        } else if (!c.jellyfin.enabled && mgr.find("jellyfin")) {
            mgr.unregister_source("jellyfin");
        }
        if (c.internet_radio.enabled && !mgr.find("internet_radio")) {
            auto src = std::make_unique<sources::InternetRadioSource>(c.internet_radio,
                                                                      c.general.ffmpeg_path);
            if (src->initialize()) mgr.register_source(std::move(src));
        } else if (!c.internet_radio.enabled && mgr.find("internet_radio")) {
            mgr.unregister_source("internet_radio");
        }
    };

    sync_sources(cfg);

    if (!mgr.switch_to(cfg.general.default_source) && !mgr.switch_to(cfg.general.fallback_source)) {
        log::warn("[bridge] neither default nor fallback source was registered");
    }

    fmod_bridge::DSPBridge bridge{mgr, fns};
    bridge.set_gain(cfg.audio.output_gain);
    bridge.set_force_stereo_audio(cfg.playback.force_stereo_audio);

    std::unique_ptr<fmod_bridge::ControlLoop> ctrl;
    if (fns.ready())
        ctrl = std::make_unique<fmod_bridge::ControlLoop>(bridge, img, cfg.playback,
                                                          cfg.audio.output_gain);

    for (auto* s : mgr.sources_snapshot()) s->set_playback_options(cfg.playback);

    store.on_change([&bridge, &mgr, sync_sources, ctrl_ptr = ctrl.get()](const Config& c) {
        sync_sources(c);
        if (!mgr.active()) {
            if (!mgr.switch_to(c.general.default_source)) mgr.switch_to(c.general.fallback_source);
        }

        // Push the gain to both: the control loop's ramper otherwise snaps
        // the bridge value back to its own cached target on the next tick.
        bridge.set_gain(c.audio.output_gain);
        bridge.set_force_stereo_audio(c.playback.force_stereo_audio);
        if (ctrl_ptr) ctrl_ptr->set_configured_gain(c.audio.output_gain);
        if (auto* local = dynamic_cast<sources::LocalFileSource*>(mgr.find("local_files"))) {
            local->set_shuffle(c.local_files.shuffle);
            local->set_ffmpeg_path(c.general.ffmpeg_path);
            local->set_directory(c.local_files.music_dir, c.local_files.recursive);
            if (mgr.active() == local && local->track_count() > 0 &&
                local->playback_state() != PlaybackState::playing) {
                local->play();
            }
        }
        if (auto* yt = dynamic_cast<sources::YouTubeMusicSource*>(mgr.find("youtube_music"))) {
            yt->set_shuffle(c.youtube_music.shuffle);
            yt->set_ffmpeg_path(c.general.ffmpeg_path);
        }
        if (auto* jf = dynamic_cast<sources::JellyfinSource*>(mgr.find("jellyfin"))) {
            jf->set_ffmpeg_path(c.general.ffmpeg_path);
            jf->set_config(c.jellyfin);
        }
        if (auto* ir = dynamic_cast<sources::InternetRadioSource*>(mgr.find("internet_radio"))) {
            ir->set_ffmpeg_path(c.general.ffmpeg_path);
            ir->set_config(c.internet_radio);
        }

        for (auto* s : mgr.sources_snapshot()) s->set_playback_options(c.playback);
        if (ctrl_ptr) ctrl_ptr->push_playback_options(c.playback);
    });

    http::HttpServer http{mgr, bridge, store, cfg.general.port, ui_dir};
    log::info("[bridge] running on port {}", cfg.general.port);

    for (;;) Sleep(60'000);
}

} // namespace fh6
