#include "fh6/sources/internet_radio_source.hpp"
#include "fh6/log.hpp"
#include "fh6/subprocess.hpp"

#include <nlohmann/json.hpp>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace fh6::sources {

namespace {

using subprocess::create_kill_on_close_job;
using subprocess::describe_launch_failure;
using subprocess::open_nul;
using subprocess::open_stderr_log;
using subprocess::quote;
using subprocess::spawn_in_job;
using subprocess::widen;

// PCM contract written by ffmpeg: 48000 Hz * 2 ch * 2 bytes.
constexpr std::uint64_t kPcmBytesPerSec = 48000ull * 2ull * 2ull;
constexpr int kHttpTimeoutMs = 5000;

// Fixed ffmpeg decode overhead: network receive + loudnorm filter latency.
// This is the delay between "ffmpeg reads bytes from the network" and
// "those PCM samples appear in the ring buffer". The ring buffer's own
// depth is measured live in approx_ring_ms_ and added on top.
constexpr auto kFfmpegOverheadMs = std::chrono::milliseconds{2000};

// Parse an ISO 8601 UTC timestamp ("2024-01-15T10:30:00.000Z") into a
// system_clock time_point. Returns nullopt on any parse failure.
static std::optional<std::chrono::system_clock::time_point>
parse_iso8601(std::string_view s) noexcept {
    int Y = 0, Mo = 0, D = 0, h = 0, m = 0, sec = 0;
    if (std::sscanf(s.data(), "%d-%d-%dT%d:%d:%d",
                    &Y, &Mo, &D, &h, &m, &sec) != 6)
        return std::nullopt;
    std::tm t{};
    t.tm_year = Y - 1900; t.tm_mon = Mo - 1; t.tm_mday = D;
    t.tm_hour = h;        t.tm_min = m;      t.tm_sec  = sec;
    const std::time_t epoch = _mkgmtime(&t);
    if (epoch == static_cast<std::time_t>(-1)) return std::nullopt;
    return std::chrono::system_clock::from_time_t(epoch);
}

struct WinHttpDeleter {
    void operator()(void* h) const noexcept { if (h) WinHttpCloseHandle(h); }
};
using WinHttpHandle = std::unique_ptr<void, WinHttpDeleter>;

// Simple HTTP GET directly targeting api.laut.fm via HTTPS (port 443)
std::optional<std::string> http_get_lautfm(const std::string& path) {
    WinHttpHandle session{WinHttpOpen(L"FH6 Universal Radio/1.0",
                                       WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session) return std::nullopt;
    WinHttpSetTimeouts(session.get(), kHttpTimeoutMs, kHttpTimeoutMs,
                       kHttpTimeoutMs, kHttpTimeoutMs);

    WinHttpHandle conn{WinHttpConnect(session.get(), L"api.laut.fm", INTERNET_DEFAULT_HTTPS_PORT, 0)};
    if (!conn) return std::nullopt;

    WinHttpHandle req{WinHttpOpenRequest(conn.get(), L"GET", widen(path).c_str(), nullptr,
                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                          WINHTTP_FLAG_SECURE)};
    if (!req) return std::nullopt;

    if (!WinHttpSendRequest(req.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                             WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req.get(), nullptr)) {
        log::error("[internet_radio] HTTP send/receive failed (err {})", GetLastError());
        return std::nullopt;
    }

    DWORD status = 0, status_sz = sizeof(status);
    WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_sz, WINHTTP_NO_HEADER_INDEX);

    std::string body;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.get(), &avail) || avail == 0) break;
        const std::size_t off = body.size();
        body.resize(off + avail);
        DWORD got = 0;
        if (!WinHttpReadData(req.get(), body.data() + off, avail, &got)) break;
        body.resize(off + got);
        if (got == 0) break;
    }

    if (status != 200) {
        log::error("[internet_radio] HTTP {} from api.laut.fm: {}", status, body);
        return std::nullopt;
    }
    return body;
}

// ---------------------------------------------------------------------------
// ICY stream-metadata helpers
// ---------------------------------------------------------------------------

// Minimal buffered TCP reader for consuming raw Shoutcast/Icecast streams.
// We only need to skip audio bytes and extract the interleaved metadata blocks.
struct TcpReader {
    SOCKET sock = INVALID_SOCKET;
    char   buf[4096]{};
    int    buf_pos = 0, buf_len = 0;

    ~TcpReader() { if (sock != INVALID_SOCKET) closesocket(sock); }

    bool fill(const std::stop_token& tok) {
        while (!tok.stop_requested()) {
            int got = recv(sock, buf, static_cast<int>(sizeof(buf)), 0);
            if (got <= 0) return false;
            buf_pos = 0; buf_len = got; return true;
        }
        return false;
    }
    bool read_exact(void* dst, int n, const std::stop_token& tok) {
        auto* out = static_cast<char*>(dst);
        while (n > 0) {
            if (buf_pos >= buf_len && !fill(tok)) return false;
            const int take = std::min(n, buf_len - buf_pos);
            std::memcpy(out, buf + buf_pos, take);
            buf_pos += take; out += take; n -= take;
        }
        return true;
    }
    bool skip(int n, const std::stop_token& tok) {
        char discard[1024];
        while (n > 0) {
            const int take = std::min(n, static_cast<int>(sizeof(discard)));
            if (!read_exact(discard, take, tok)) return false;
            n -= take;
        }
        return true;
    }
    bool read_line(std::string& out, const std::stop_token& tok) {
        out.clear();
        char c;
        while (read_exact(&c, 1, tok)) {
            if (c == '\n') return true;
            if (c != '\r') out += c;
        }
        return false;
    }
};

// Extract StreamTitle='...' from an ICY metadata block (null-padded).
static std::string icy_stream_title(const std::string& block) {
    constexpr std::string_view kKey = "StreamTitle='";
    const auto pos = block.find(kKey);
    if (pos == std::string::npos) return {};
    const auto start = pos + kKey.size();
    auto end = block.find("';", start);
    if (end == std::string::npos) end = block.find('\0', start);
    if (end == std::string::npos) end = block.size();
    return block.substr(start, end - start);
}

// True if a StreamTitle looks like an advertisement rather than music.
static bool icy_title_is_ad(const std::string& title) {
    if (title.empty()) return true;
    std::string low = title;
    for (auto& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (auto marker : {std::string_view{"werbung"}, {"reklama"}, {"publicidad"},
                        {"advertisement"}, {"ad break"}, {"commercial"}})
        if (low.find(marker) != std::string::npos) return true;
    return false;
}

} // namespace

struct InternetRadioSource::Pipe {
    HANDLE job       = nullptr;
    HANDLE proc      = nullptr;
    HANDLE read_pipe = nullptr;
    std::uint64_t bytes_written = 0;
    std::atomic<std::uint64_t> position_ms{0};
    bool ended = false;

    ~Pipe() {
        if (read_pipe) CloseHandle(read_pipe);
        if (job)       CloseHandle(job);
        if (proc)      CloseHandle(proc);
    }
};

InternetRadioSource::InternetRadioSource(InternetRadioConfig cfg, std::filesystem::path ffmpeg_path)
    : cfg_{std::move(cfg)}, ffmpeg_path_{std::move(ffmpeg_path)} {}

InternetRadioSource::~InternetRadioSource() {
    shutdown();
}

bool InternetRadioSource::initialize() {
    if (!cfg_.enabled) return false;
    
    // Set default fallback metadata
    {
        std::lock_guard lk{metadata_mu_};
        current_track_info_.title = "Eurobeat Radio";
        current_track_info_.artist = "Streaming Live";
        current_track_info_.album = "FH6 Custom";
        current_track_info_.duration_ms = 0;
        current_track_info_.position_ms = 0;
    }

    return true;
}

void InternetRadioSource::shutdown() noexcept {
    metadata_thread_.request_stop();
    icy_monitor_thread_.request_stop();
    if (metadata_thread_.joinable())    metadata_thread_.join();
    if (icy_monitor_thread_.joinable()) icy_monitor_thread_.join();

    std::scoped_lock lk{mu_};
    stop_pipe_locked();
}

void InternetRadioSource::start_pipe_locked() {
    stop_pipe_locked();
    needs_reconnect_ = false;
    last_connection_attempt_ = std::chrono::steady_clock::now();

    if (cfg_.stream_url.empty()) {
        log::warn("[internet_radio] stream_url is empty, cannot play");
        return;
    }

    auto pipe = std::make_unique<Pipe>();
    pipe->job = create_kill_on_close_job();
    if (!pipe->job) {
        log::warn("[internet_radio] CreateJobObject failed ({})", GetLastError());
        return;
    }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE out_r = nullptr, out_w = nullptr;
    if (!CreatePipe(&out_r, &out_w, &sa, 1 << 20)) return;
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul_in  = open_nul(GENERIC_READ);
    HANDLE err_log = open_stderr_log();

    const std::wstring ff = ffmpeg_path_.empty() ? std::wstring{L"ffmpeg"}
                                                 : ffmpeg_path_.wstring();

    // Custom ffmpeg arguments for resilient internet radio streaming:
    // -reconnect 1: auto-reconnect on disconnect
    // -reconnect_streamed 1: reconnect when streaming
    // -reconnect_delay_max 5: wait up to 5 seconds between reconnects
    std::wstring cmd = quote(ff) +
        L" -loglevel error -reconnect 1 -reconnect_streamed 1 -reconnect_delay_max 5" +
        L" -i " + quote(widen(cfg_.stream_url)) + L" -f s16le ";
    if (volume_norm_.load(std::memory_order_acquire))
        cmd += L"-af loudnorm=I=-14:TP=-2:LRA=11 ";
    cmd += L"-acodec pcm_s16le -ar 48000 -ac 2 pipe:1";

    pipe->proc = spawn_in_job(pipe->job, cmd, nul_in, out_w, err_log);
    const DWORD ec = pipe->proc ? 0u : GetLastError();
    CloseHandle(out_w);
    if (nul_in)  CloseHandle(nul_in);
    if (err_log) CloseHandle(err_log);
    if (!pipe->proc) {
        CloseHandle(out_r);
        log::warn("[internet_radio] failed to launch ffmpeg -- {}",
                  describe_launch_failure(ff, ec, !ffmpeg_path_.empty()));
        needs_reconnect_ = true;
        return;  // ~Pipe reaps the job
    }

    pipe->read_pipe = out_r;
    pipe_           = std::move(pipe);
    log::info("[internet_radio] started stream: {}", cfg_.stream_url);
}

void InternetRadioSource::stop_pipe_locked() {
    pipe_.reset();
    state_.store(PlaybackState::stopped, std::memory_order_release);
}

void InternetRadioSource::play() {
    std::scoped_lock lk{mu_};
    if (!pipe_) start_pipe_locked();
    if (pipe_) state_.store(PlaybackState::playing, std::memory_order_release);

    if (!metadata_thread_.joinable() && !cfg_.station_name.empty())
        metadata_thread_ = std::jthread([this](const std::stop_token& tok) { run_metadata_loop(tok); });

    if (!icy_monitor_thread_.joinable() && !cfg_.stream_url.empty())
        icy_monitor_thread_ = std::jthread([this](const std::stop_token& tok) { run_icy_monitor(tok); });
}

void InternetRadioSource::pause() {
    state_.store(PlaybackState::paused, std::memory_order_release);
}

void InternetRadioSource::stop() {
    std::scoped_lock lk{mu_};
    stop_pipe_locked();
}

void InternetRadioSource::set_config(InternetRadioConfig cfg) {
    std::scoped_lock lk{mu_};
    const bool url_changed = cfg_.stream_url != cfg.stream_url;
    const bool station_changed = cfg_.station_name != cfg.station_name;
    const bool was_playing = state_.load(std::memory_order_acquire) == PlaybackState::playing;

    cfg_ = std::move(cfg);

    if (url_changed && was_playing) {
        start_pipe_locked();
        if (pipe_) state_.store(PlaybackState::playing, std::memory_order_release);

        // Restart ICY monitor against the new URL.
        icy_monitor_thread_.request_stop();
        if (icy_monitor_thread_.joinable()) icy_monitor_thread_.join();
        is_ad_.store(false, std::memory_order_release);
        if (!cfg_.stream_url.empty())
            icy_monitor_thread_ = std::jthread([this](const std::stop_token& tok) { run_icy_monitor(tok); });
    }

    if (station_changed && was_playing) {
        metadata_thread_.request_stop();
        if (metadata_thread_.joinable()) metadata_thread_.join();
        pending_track_.reset();
        if (!cfg_.station_name.empty())
            metadata_thread_ = std::jthread([this](const std::stop_token& tok) { run_metadata_loop(tok); });
    }
}

void InternetRadioSource::set_ffmpeg_path(std::filesystem::path p) {
    std::scoped_lock lk{mu_};
    ffmpeg_path_ = std::move(p);
}

void InternetRadioSource::set_playback_options(const PlaybackConfig& opts) {
    {
        std::scoped_lock lk{mu_};
        eq_.set_options(opts.equalizer_enabled, opts.equalizer_bands, 48000.0f);
    }
    // loudnorm config applies on next connection/stream start
    volume_norm_.store(opts.volume_normalization, std::memory_order_release);
}

TrackInfo InternetRadioSource::current_track() const {
    std::scoped_lock lk{metadata_mu_};
    TrackInfo info = current_track_info_;
    
    // Update live position count since started
    std::scoped_lock pipe_lk{mu_};
    if (pipe_) {
        info.position_ms = pipe_->position_ms.load(std::memory_order_acquire);
    }
    return info;
}

void InternetRadioSource::pump(RingBuffer& ring) {
    if (state_.load(std::memory_order_acquire) != PlaybackState::playing) return;

    std::scoped_lock lk{mu_};

    // Auto-reconnection logic: if needs reconnect and cooldown passed, try again
    if (needs_reconnect_) {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_connection_attempt_ >= std::chrono::seconds(2)) {
            log::info("[internet_radio] attempting auto-reconnect...");
            start_pipe_locked();
        }
        return;
    }

    Pipe* p = pipe_.get();
    if (!p) {
        start_pipe_locked();
        return;
    }

    auto update_position = [&] {
        const std::size_t r = ring.readable();
        const std::uint64_t played = p->bytes_written > r ? p->bytes_written - r : 0;
        p->position_ms.store(played * 1000ull / kPcmBytesPerSec, std::memory_order_release);
    };
    auto on_eof = [&] {
        if (p->read_pipe) {
            CloseHandle(p->read_pipe);
            p->read_pipe = nullptr;
        }
        p->ended = true;
        needs_reconnect_ = true;
        last_connection_attempt_ = std::chrono::steady_clock::now();
        log::warn("[internet_radio] connection EOF or error, scheduled reconnect in 2 seconds");
    };

    if (p->ended) {
        update_position();
        return;
    }
    if (!p->read_pipe) return;

    DWORD avail = 0;
    if (!PeekNamedPipe(p->read_pipe, nullptr, 0, nullptr, &avail, nullptr)) {
        on_eof();
        return;
    }

    // Ad break: drain the ring so the DSP outputs silence, and discard the
    // raw pipe data so ffmpeg does not stall (its pipe buffer would fill and
    // block the process, desynchronising the stream after the break ends).
    if (is_ad_.load(std::memory_order_acquire)) {
        ring.drain();
        if (avail > 0) {
            std::byte discard[4096];
            DWORD got = 0;
            ReadFile(p->read_pipe, discard,
                     static_cast<DWORD>(std::min<std::size_t>(avail, sizeof(discard))),
                     &got, nullptr);
        }
        update_position();
        return;
    }

    while (avail > 0) {
        const std::size_t writable = ring.writable();
        if (writable < 4) break;
        std::size_t want = std::min<std::size_t>(writable, avail);
        if (want > 4096) want = 4096;
        want &= ~std::size_t{3};   // whole stereo s16 frames -- EQ never sees half a sample
        if (!want) break;

        std::byte buf[4096];
        DWORD got = 0;
        if (!ReadFile(p->read_pipe, buf, (DWORD)want, &got, nullptr) || got == 0) {
            on_eof();
            break;
        }
        const DWORD aligned = (got / 4u) * 4u;
        if (aligned) eq_.process(reinterpret_cast<int16_t*>(buf), aligned / 4u);
        ring.write(buf, aligned);
        p->bytes_written += aligned;
        avail = avail > got ? avail - got : 0;
    }
    // Keep a live estimate of the ring's depth so the ICY monitor and API
    // path can compute an accurate display-at time without a fixed guess.
    approx_ring_ms_.store(
        static_cast<uint32_t>(ring.readable() * 1000u / kPcmBytesPerSec),
        std::memory_order_release);
    update_position();
}

void InternetRadioSource::run_metadata_loop(const std::stop_token& tok) {
    log::info("[internet_radio] metadata polling thread started for station: {}", cfg_.station_name);

    // Initial update
    update_metadata_from_api();

    while (!tok.stop_requested()) {
        // Sleep 15 s in 100 ms increments; check the pending-track timer every
        // ~1 s so the HUD switches at the right moment without busy-waiting.
        // Also check for an ICY-triggered immediate poll request on each tick.
        for (int i = 0; i < 150 && !tok.stop_requested(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (i % 10 == 9) apply_pending_if_ready();
            if (request_immediate_poll_.exchange(false, std::memory_order_acq_rel) &&
                state_.load(std::memory_order_acquire) == PlaybackState::playing) {
                update_metadata_from_api();
                apply_pending_if_ready();
            }
        }

        if (tok.stop_requested()) break;

        if (state_.load(std::memory_order_acquire) == PlaybackState::playing) {
            update_metadata_from_api();
        }
    }
    log::info("[internet_radio] metadata polling thread exiting");
}

void InternetRadioSource::update_metadata_from_api() {
    std::string station;
    {
        std::scoped_lock lk{mu_};
        station = cfg_.station_name;
    }

    if (station.empty()) return;

    const std::string path = std::format("/station/{}/current_song", station);
    auto body = http_get_lautfm(path);
    if (!body) return;

    try {
        const auto root = nlohmann::json::parse(*body);
        std::string title = root.value("title", std::string{});
        std::string artist_name;
        if (auto it = root.find("artist"); it != root.end() && it->is_object())
            artist_name = it->value("name", std::string{});

        // Compute when this song's audio actually reaches the listener.
        // Total latency = ring-buffer depth (live) + fixed ffmpeg decode overhead.
        // Using the live ring depth avoids the fixed-guess problem: if the ring
        // is nearly empty the HUD switches sooner; if it's full, it waits longer.
        const auto now = std::chrono::system_clock::now();
        const auto audio_latency =
            std::chrono::milliseconds{approx_ring_ms_.load(std::memory_order_acquire)}
            + kFfmpegOverheadMs;
        std::chrono::system_clock::time_point display_at = now; // default: apply immediately
        if (auto started = parse_iso8601(root.value("started_at", std::string{}))) {
            const auto candidate = *started + audio_latency;
            if (candidate > now) display_at = candidate;
        }

        TrackInfo info;
        info.title  = title.empty() ? "Eurobeat Radio" : std::move(title);
        info.artist = artist_name.empty() ? "Live Stream" : std::move(artist_name);
        info.album  = "Eurobeat FM";

        // Schedule the update. apply_pending_if_ready() will apply it when
        // display_at is reached; if display_at is already in the past it
        // applies on the very next call (within ~1 s).
        pending_track_      = std::move(info);
        pending_display_at_ = display_at;

    } catch (const std::exception& e) {
        log::error("[internet_radio] failed to parse metadata JSON: {}", e.what());
    }
}

void InternetRadioSource::apply_pending_if_ready() noexcept {
    if (!pending_track_) return;
    if (std::chrono::system_clock::now() < pending_display_at_) return;

    std::lock_guard lk{metadata_mu_};
    current_track_info_ = std::move(*pending_track_);
    pending_track_.reset();
}

void InternetRadioSource::run_icy_monitor(const std::stop_token& tok) {
    while (!tok.stop_requested()) {
        std::string url;
        { std::lock_guard lk{mu_}; url = cfg_.stream_url; }

        // Only raw HTTP streams carry ICY metadata inline; HTTPS goes through a
        // TLS layer we can't tap here, so we skip and never set is_ad_.
        if (url.rfind("http://", 0) != 0) {
            log::info("[internet_radio] ICY monitor: stream is not plain HTTP, ad detection unavailable");
            return;
        }

        // Parse http://host[:port]/path
        const std::string rest = url.substr(7);
        const auto slash       = rest.find('/');
        const std::string host_port = slash != std::string::npos ? rest.substr(0, slash) : rest;
        const std::string path      = slash != std::string::npos ? rest.substr(slash) : "/";
        const auto colon = host_port.rfind(':');
        std::string host;
        uint16_t    port = 80;
        if (colon != std::string::npos) {
            host = host_port.substr(0, colon);
            try { port = static_cast<uint16_t>(std::stoi(host_port.substr(colon + 1))); } catch (...) {}
        } else {
            host = host_port;
        }

        // Resolve hostname
        addrinfo hints{}, *res = nullptr;
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        const std::string port_str = std::to_string(port);
        if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0 || !res) {
            log::warn("[internet_radio] ICY monitor: DNS failed for {}", host);
            for (int i = 0; i < 50 && !tok.stop_requested(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // Connect
        TcpReader reader;
        reader.sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (reader.sock == INVALID_SOCKET) {
            freeaddrinfo(res);
            for (int i = 0; i < 50 && !tok.stop_requested(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        DWORD rcv_timeout = 15000;
        setsockopt(reader.sock, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&rcv_timeout), sizeof(rcv_timeout));
        const int conn_err = connect(reader.sock, res->ai_addr, static_cast<int>(res->ai_addrlen));
        freeaddrinfo(res);
        if (conn_err != 0) {
            log::warn("[internet_radio] ICY monitor: connect to {}:{} failed", host, port);
            for (int i = 0; i < 50 && !tok.stop_requested(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // HTTP request with ICY metadata requested
        const std::string req =
            "GET " + path + " HTTP/1.0\r\n"
            "Host: " + host + "\r\n"
            "Icy-MetaData: 1\r\n"
            "User-Agent: FH6 Universal Radio/1.0\r\n"
            "Connection: close\r\n"
            "\r\n";
        send(reader.sock, req.c_str(), static_cast<int>(req.size()), 0);

        // Read response headers; extract icy-metaint
        int metaint = 0;
        std::string line;
        while (reader.read_line(line, tok) && !line.empty()) {
            std::string low = line;
            for (auto& c : low)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (low.rfind("icy-metaint:", 0) == 0) {
                try { metaint = std::stoi(line.substr(12)); } catch (...) {}
            }
        }
        if (tok.stop_requested()) break;

        if (metaint <= 0) {
            log::warn("[internet_radio] ICY monitor: no icy-metaint in response, retrying in 5s");
            is_ad_.store(false, std::memory_order_release);
            for (int i = 0; i < 50 && !tok.stop_requested(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        log::info("[internet_radio] ICY monitor connected to {} (metaint={})", host, metaint);

        std::string last_icy_title_;  // tracks last seen StreamTitle for change detection

        // Main loop: skip audio block → read metadata block → detect ads
        while (!tok.stop_requested()) {
            if (!reader.skip(metaint, tok)) break;

            uint8_t meta_len_raw = 0;
            if (!reader.read_exact(&meta_len_raw, 1, tok)) break;
            if (meta_len_raw == 0) continue; // no metadata this interval

            const int meta_len = meta_len_raw * 16;
            std::string meta_block(meta_len, '\0');
            if (!reader.read_exact(meta_block.data(), meta_len, tok)) break;

            const std::string title = icy_stream_title(meta_block);
            const bool ad           = icy_title_is_ad(title);
            const bool was_ad       = is_ad_.exchange(ad, std::memory_order_acq_rel);
            if (ad != was_ad) {
                log::info("[internet_radio] ICY: {} (StreamTitle=\"{}\")",
                          ad ? "ad break, muting" : "music resumed, unmuting", title);
                if (!ad)
                    // Song resumed after ad: get fresh metadata immediately.
                    request_immediate_poll_.store(true, std::memory_order_release);
            } else if (!ad && title != last_icy_title_) {
                // New non-ad song arrived in the stream. Signal the metadata
                // thread to poll the API now rather than waiting up to 15 s.
                // The ring-buffer depth at this moment is the most accurate
                // estimate of how far behind the audio is; the API path reads
                // it via approx_ring_ms_ to compute the correct display_at.
                log::info("[internet_radio] ICY: new song detected (StreamTitle=\"{}\")", title);
                request_immediate_poll_.store(true, std::memory_order_release);
            }
            last_icy_title_ = title;
        }

        is_ad_.store(false, std::memory_order_release);
        if (!tok.stop_requested())
            log::warn("[internet_radio] ICY monitor disconnected, retrying in 5s");
        for (int i = 0; i < 50 && !tok.stop_requested(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

} // namespace fh6::sources
