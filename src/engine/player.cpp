#include "engine/player.h"

#include <windows.h>

#include <objbase.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>

#include "dsd/dop.h"
#include "dsd/dsd_fir.h"

namespace arpegio {

namespace {

constexpr double kRingSeconds = 20.0;
constexpr size_t kMinRingBytes = 32 * 1024 * 1024;
constexpr double kPrefillSeconds = 1.0;
constexpr double kSeekRefillSeconds = 0.2;
constexpr DWORD kLoaderPollMs = 10;

}  // namespace

// What the device callback sees. Everything here is lock-free.
class Player::Session final : public FrameSource {
public:
    Session(const DeviceFormat& format, bool dop, size_t ring_bytes, std::atomic<bool>& paused)
        : ring(ring_bytes), format_(format), dop_(dop), paused_(paused) {}

    // The device may change the DSD bit order when it opens.
    void set_format(const DeviceFormat& format) { format_ = format; }

    void pull(uint8_t* destination, size_t frames) noexcept override {
        const size_t want = frames * format_.frame_bytes();
        size_t got = 0;
        if (paused_.load(std::memory_order_acquire) || refilling.load(std::memory_order_acquire)) {
            ring.read(destination, 0);  // still applies a pending discard
        } else {
            got = ring.read(destination, want);
            if (got < want && feeding.load(std::memory_order_acquire)) {
                underruns.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (got < want) fill_silence(destination + got, want - got);
        if (dop_) {
            dop_stamp_markers(destination, frames, format_.channels, wire_sample_bytes(format_.encoding), dop_phase_);
        }
    }

    void silence(uint8_t* destination, size_t frames) noexcept override {
        fill_silence(destination, frames * format_.frame_bytes());
        if (dop_) {
            dop_stamp_markers(destination, frames, format_.channels, wire_sample_bytes(format_.encoding), dop_phase_);
        }
    }

    RingBuffer ring;
    std::atomic<bool> feeding{true};
    std::atomic<bool> refilling{false};  // after a seek: silence until the ring has data again
    std::atomic<uint64_t> underruns{0};

private:
    void fill_silence(uint8_t* destination, size_t bytes) noexcept {
        if (format_.encoding == WireEncoding::DsdMsb) {
            std::memset(destination, kDsdSilenceByte, bytes);
        } else if (format_.encoding == WireEncoding::DsdLsb) {
            std::memset(destination, 0x96, bytes);  // 0x69 with its bits reversed
        } else if (dop_) {
            const size_t sample_bytes = wire_sample_bytes(format_.encoding);
            for (size_t i = 0; i < bytes; i += sample_bytes) {
                destination[i] = 0;
                std::memset(destination + i + sample_bytes - 3, kDsdSilenceByte, 2);
            }
        } else {
            std::memset(destination, 0, bytes);
        }
    }

    DeviceFormat format_;
    bool dop_;
    uint32_t dop_phase_ = 0;
    std::atomic<bool>& paused_;
};

Player::Player(OutputFactory factory) : factory_(std::move(factory)) {
    thread_ = std::thread([this] { worker(); });
}

Player::~Player() {
    {
        std::lock_guard lock(mutex_);
        quit_ = true;
    }
    stop_flag_.store(true, std::memory_order_release);
    wake_.notify_all();
    thread_.join();
}

void Player::set_device(DeviceInfo device, OutputOptions options) {
    std::lock_guard lock(mutex_);
    device_ = std::move(device);
    options_ = options;
}

void Player::set_policy(const PlaybackPolicy& policy) {
    std::lock_guard lock(mutex_);
    policy_ = policy;
}

void Player::set_queue(std::vector<QueueItem> items) {
    std::lock_guard lock(mutex_);
    queue_ = std::move(items);
}

void Player::play(size_t index) {
    stop();
    {
        std::unique_lock lock(mutex_);
        stopped_.wait(lock, [this] { return !active_; });
        stop_flag_.store(false, std::memory_order_release);
        paused_.store(false, std::memory_order_release);
        error_.clear();
        seek_request_.reset();
        play_request_ = index;
        active_ = true;
    }
    wake_.notify_all();
}

void Player::set_paused(bool paused) {
    paused_.store(paused, std::memory_order_release);
}

void Player::stop() {
    stop_flag_.store(true, std::memory_order_release);
}

void Player::seek(double seconds) {
    std::lock_guard lock(mutex_);
    seek_request_ = std::max(seconds, 0.0);
}

bool Player::wait_until_stopped(uint32_t timeout_ms) {
    std::unique_lock lock(mutex_);
    return stopped_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return !active_; });
}

PlayerStatus Player::status() const {
    PlayerStatus status;
    std::lock_guard lock(mutex_);
    status.state = !active_ ? PlayerState::Stopped
                            : paused_.load(std::memory_order_acquire) ? PlayerState::Paused : PlayerState::Playing;
    status.error = error_;
    status.signal_path = signal_path_;
    status.bit_perfect = bit_perfect_;
    status.clipped_samples = clipped_.load(std::memory_order_relaxed);
    status.reopened = reopened_.load(std::memory_order_relaxed);
    status.underruns = last_underruns_;
    status.output = last_stats_;
    if (!session_) return status;

    status.underruns = session_->underruns.load(std::memory_order_relaxed);
    status.buffered_bytes = session_->ring.readable();
    status.ring_capacity = session_->ring.capacity();

    const uint64_t played = session_->ring.total_read();
    const Segment* current = nullptr;
    for (const Segment& segment : segments_) {
        if (segment.ring_position <= played) current = &segment;
    }
    if (current) {
        status.queue_index = current->queue_index;
        status.duration_seconds = current->duration_seconds;
        status.position_seconds = current->start_seconds + static_cast<double>(played - current->ring_position) /
                                                               (plan_.device.frame_bytes() * plan_.device.ring_frame_rate());
    }
    return status;
}

void Player::set_error(const std::string& text) {
    std::lock_guard lock(mutex_);
    error_ = text;
}

void Player::add_segment(size_t index, double start_seconds, double duration_seconds) {
    std::lock_guard lock(mutex_);
    segments_.push_back({session_->ring.total_written(), index, start_seconds, duration_seconds});
    while (segments_.size() > 1 && segments_[1].ring_position <= session_->ring.total_read()) segments_.pop_front();
}

void Player::open_session(const RenderPlan& plan, const SourceFormat& source) {
    DeviceInfo device;
    OutputOptions options;
    {
        std::lock_guard lock(mutex_);
        device = device_;
        options = options_;
    }

    const size_t bytes_per_second = plan.device.frame_bytes() * plan.device.ring_frame_rate();
    const size_t ring_bytes = std::max(kMinRingBytes, static_cast<size_t>(bytes_per_second * kRingSeconds));
    const bool dop = plan.path == RenderPath::DsdDop;
    auto session = std::make_shared<Session>(plan.device, dop, ring_bytes, paused_);

    output_ = factory_(device, options);
    const DeviceFormat actual = output_->open(plan.device, *session);
    session->set_format(actual);

    plan_ = plan;
    plan_.device = actual;
    pipeline_ = std::make_unique<Pipeline>(source, plan_);
    pipeline_source_ = source;
    output_started_ = false;

    std::lock_guard lock(mutex_);
    session_ = std::move(session);
    segments_.clear();
    signal_path_ = describe_plan(plan_, source);
    bit_perfect_ = plan_.bit_perfect;
}

void Player::close_session() {
    if (pipeline_) clipped_.fetch_add(pipeline_->clipped(), std::memory_order_relaxed);
    if (output_) {
        const OutputStats stats = output_->stats();
        output_->close();
        output_.reset();
        std::lock_guard lock(mutex_);
        last_stats_ = stats;
    }
    pipeline_.reset();
    output_started_ = false;
    std::lock_guard lock(mutex_);
    if (session_) last_underruns_ = session_->underruns.load(std::memory_order_relaxed);
    session_.reset();
    segments_.clear();
}

void Player::start_output_if_ready(bool force) {
    if (output_started_) return;
    const double buffered = static_cast<double>(session_->ring.readable());
    const double target = plan_.device.frame_bytes() * plan_.device.ring_frame_rate() * kPrefillSeconds;
    if (force || buffered >= target) {
        output_->start();
        output_started_ = true;
    }
}

void Player::reopen_output_if_needed() {
    if (!output_ || !output_->needs_reopen()) return;
    // The ring keeps its audio, so playback resumes where the driver left it.
    output_->close();
    output_->open(plan_.device, *session_);
    output_->start();
    output_started_ = true;
    reopened_.fetch_add(1, std::memory_order_relaxed);
}

bool Player::write_all(const std::vector<uint8_t>& bytes) {
    size_t written = 0;
    RingBuffer& ring = session_->ring;
    const size_t frame_bytes = plan_.device.frame_bytes();
    while (written < bytes.size()) {
        // Whole frames only: a seek abandons the rest of the chunk, and the next data must
        // still start on a frame boundary.
        const size_t room = ring.writable() / frame_bytes * frame_bytes;
        written += ring.write(bytes.data() + written, std::min(room, bytes.size() - written));
        if (written == bytes.size()) break;
        start_output_if_ready(false);
        for (;;) {
            if (stop_requested()) return false;
            {
                std::lock_guard lock(mutex_);
                if (seek_request_) return false;
            }
            reopen_output_if_needed();
            if (ring.writable() >= ring.capacity() / 2) break;
            Sleep(kLoaderPollMs);
        }
    }
    return true;
}

Player::PumpResult Player::pump(Decoder& decoder, const SourceFormat& source, size_t index) {
    const double bytes_per_second = static_cast<double>(plan_.device.frame_bytes()) * plan_.device.ring_frame_rate();
    uint64_t refill_target = 0;
    for (;;) {
        if (stop_requested()) return PumpResult::Stop;
        std::optional<double> seek;
        {
            std::lock_guard lock(mutex_);
            seek.swap(seek_request_);
        }
        if (seek) {
            const double seconds = std::min(*seek, source.seconds());
            const double units = source.dsd ? seconds * source.rate / 8.0 : seconds * source.rate;
            session_->refilling.store(true, std::memory_order_release);
            session_->ring.discard();
            decoder.seek(static_cast<uint64_t>(units));
            pipeline_->reset();
            add_segment(index, seconds, source.seconds());
            refill_target = session_->ring.total_written() + static_cast<uint64_t>(bytes_per_second * kSeekRefillSeconds);
        }

        const bool more = pipeline_->produce(decoder, chunk_);
        if (!write_all(chunk_) && stop_requested()) return PumpResult::Stop;
        if (!more || session_->ring.total_written() >= refill_target) {
            session_->refilling.store(false, std::memory_order_release);
        }
        start_output_if_ready(false);
        reopen_output_if_needed();
        if (!more) return PumpResult::Finished;
    }
}

void Player::drain() {
    session_->feeding.store(false, std::memory_order_release);
    start_output_if_ready(true);
    while (session_->ring.readable() > 0) {
        if (stop_requested()) return;
        reopen_output_if_needed();
        Sleep(kLoaderPollMs);
    }
    // What left the ring is still in the driver's buffers.
    const OutputStats stats = output_->stats();
    const uint64_t rate = std::max<uint32_t>(plan_.device.ring_frame_rate(), 1);
    const uint64_t frames = 2ull * stats.period_frames + stats.latency_frames;
    Sleep(static_cast<DWORD>(frames * 1000 / rate) + kLoaderPollMs);
}

void Player::run_queue(size_t index) {
    PlaybackPolicy policy;
    DeviceInfo device;
    {
        std::lock_guard lock(mutex_);
        policy = policy_;
        device = device_;
    }

    for (;;) {
        QueueItem item;
        {
            std::lock_guard lock(mutex_);
            if (index >= queue_.size()) break;
            item = queue_[index];
        }

        auto decoder = item.open ? item.open() : open_decoder(item.path);
        const SourceFormat source = decoder->format();
        const RenderPlan plan = make_render_plan(source, device, policy);

        // A new device format means the old stream must play out first. Otherwise the same
        // session continues, with no gap between tracks.
        const bool continues = session_ && plan == plan_ && source.rate == pipeline_source_.rate &&
                               source.channels == pipeline_source_.channels;
        if (session_ && !continues) {
            std::vector<uint8_t> tail;
            pipeline_->flush(tail);
            write_all(tail);
            drain();
            if (stop_requested()) return;
            close_session();
        }
        if (!session_) open_session(plan, source);

        add_segment(index, 0.0, source.seconds());
        if (pump(*decoder, source, index) != PumpResult::Finished) return;
        ++index;
    }

    if (session_) {
        std::vector<uint8_t> tail;
        pipeline_->flush(tail);
        write_all(tail);
        drain();
    }
}

void Player::worker() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        size_t index = 0;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return quit_ || play_request_.has_value(); });
            if (quit_) break;
            index = *play_request_;
            play_request_.reset();
        }
        try {
            run_queue(index);
        } catch (const std::exception& error) {
            set_error(error.what());
        }
        close_session();
        {
            std::lock_guard lock(mutex_);
            active_ = false;
        }
        stopped_.notify_all();
    }
    if (SUCCEEDED(com)) CoUninitialize();
}

}  // namespace arpegio
