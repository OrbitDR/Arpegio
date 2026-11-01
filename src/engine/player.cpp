#include "engine/player.h"

#include <windows.h>

#include <objbase.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>

#include "audio/volume.h"
#include "dsd/dop.h"
#include "dsd/dsd_fir.h"

namespace arpegio {

namespace {

constexpr double kRingSeconds = 20.0;
constexpr size_t kMinRingBytes = 32 * 1024 * 1024;
constexpr double kPrefillSeconds = 1.0;
constexpr double kSeekRefillSeconds = 0.2;
constexpr DWORD kLoaderPollMs = 10;
constexpr double kRestartSeconds = 3.0;  // past this, previous() restarts the track

}  // namespace

// What the device callback sees. Everything here is lock-free.
class Player::Session final : public FrameSource {
public:
    Session(const DeviceFormat& format, bool dop, size_t ring_bytes, std::atomic<bool>& paused)
        : ring(ring_bytes), format_(format), dop_(dop), paused_(paused) {}

    // The device may change the DSD bit order when it opens.
    void set_format(const DeviceFormat& format) { format_ = format; }

    // Before the stream starts. Without it the bytes go out as they are.
    void set_volume(const DeviceFormat& format, int dither_bits, const std::atomic<double>& gain) {
        volume_.emplace(format, dither_bits, gain);
    }

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
            if (volume_) volume_->apply(destination, got / format_.frame_bytes());
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
    std::optional<Volume> volume_;
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

void Player::set_repeat(RepeatMode mode) {
    std::lock_guard lock(mutex_);
    repeat_ = mode;
}

void Player::set_queue(std::vector<QueueItem> items) {
    std::lock_guard lock(mutex_);
    queue_ = std::move(items);
}

void Player::update_queue(std::vector<QueueItem> items, size_t current) {
    std::lock_guard lock(mutex_);
    if (!active_ || jump_request_) {
        queue_ = std::move(items);
        if (jump_request_) jump_request_->index = current;
        else loader_index_ = current;
        return;
    }

    // The loader sits before the heard track only when it wrapped around to repeat.
    const size_t heard = heard_index();
    bool kept = loader_index_ >= heard;
    for (size_t i = 0; kept && heard + i <= loader_index_; ++i) {
        if (heard + i < queue_.size()) {
            kept = current + i < items.size() && items[current + i].id == queue_[heard + i].id;
        }
    }
    if (kept) {
        for (Segment& segment : segments_) segment.queue_index += current - heard;
        loader_index_ += current - heard;
    } else {
        jump_request_ = Jump{current, heard_position()};
    }
    queue_ = std::move(items);
}

void Player::play(size_t index, double start_seconds) {
    {
        std::lock_guard lock(mutex_);
        if (active_ && !stop_requested()) {
            error_.clear();
            paused_.store(false, std::memory_order_release);
            jump_request_ = Jump{index, start_seconds};
            return;
        }
    }
    stop();
    {
        std::unique_lock lock(mutex_);
        stopped_.wait(lock, [this] { return !active_; });
        stop_flag_.store(false, std::memory_order_release);
        paused_.store(false, std::memory_order_release);
        error_.clear();
        jump_request_.reset();
        play_request_ = Jump{index, start_seconds};
        loader_index_ = index;
        waiting_position_ = start_seconds;
        active_ = true;
    }
    wake_.notify_all();
}

void Player::next() {
    std::lock_guard lock(mutex_);
    if (active_ && !stop_requested()) jump_request_ = Jump{heard_index() + 1, 0.0};
}

void Player::previous() {
    std::lock_guard lock(mutex_);
    if (!active_ || stop_requested()) return;
    const size_t heard = heard_index();
    const bool restart = heard == 0 || heard_position() > kRestartSeconds;
    jump_request_ = Jump{restart ? heard : heard - 1, 0.0};
}

void Player::set_volume(double gain) {
    volume_.store(std::clamp(gain, 0.0, 1.0), std::memory_order_relaxed);
}

void Player::reload() {
    std::lock_guard lock(mutex_);
    if (active_ && !stop_requested()) jump_request_ = Jump{heard_index(), heard_position()};
}

void Player::set_paused(bool paused) {
    paused_.store(paused, std::memory_order_release);
}

void Player::stop() {
    stop_flag_.store(true, std::memory_order_release);
}

void Player::seek(double seconds) {
    std::lock_guard lock(mutex_);
    if (active_ && !stop_requested()) jump_request_ = Jump{heard_index(), std::max(seconds, 0.0)};
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
    status.hardware_gain_db = hardware_gain_db_;
    status.queue_index = heard_index();
    if (active_) status.position_seconds = heard_position();
    status.clipped_samples = clipped_.load(std::memory_order_relaxed);
    status.reopened = reopened_.load(std::memory_order_relaxed);
    status.underruns = last_underruns_;
    status.output = last_stats_;
    if (!session_) return status;

    status.underruns = session_->underruns.load(std::memory_order_relaxed);
    status.buffered_bytes = session_->ring.readable();
    status.ring_capacity = session_->ring.capacity();
    status.stream_open = true;
    status.plan = plan_;

    status.position_seconds = heard_position();
    const Segment* current = heard_segment();
    if (current) {
        status.duration_seconds = current->duration_seconds;
        status.track_gain = current->gain;
    }
    const bool volume_scales = plan_.accepts_gain() && volume_.load(std::memory_order_relaxed) != 1.0;
    status.bit_perfect = bit_perfect_ && !volume_scales && status.track_gain == 1.0;
    return status;
}

const Player::Segment* Player::heard_segment() const {
    if (!session_) return nullptr;
    const uint64_t played = session_->ring.total_read();
    const Segment* current = nullptr;
    for (const Segment& segment : segments_) {
        if (segment.ring_position <= played) current = &segment;
    }
    return current;
}

size_t Player::heard_index() const {
    if (jump_request_) return jump_request_->index;
    const Segment* current = heard_segment();
    return current ? current->queue_index : loader_index_;
}

double Player::heard_position() const {
    const Segment* current = heard_segment();
    if (!current) return waiting_position_;
    return current->start_seconds + static_cast<double>(session_->ring.total_read() - current->ring_position) /
                                        (plan_.device.frame_bytes() * plan_.device.ring_frame_rate());
}

void Player::set_error(const std::string& text) {
    std::lock_guard lock(mutex_);
    error_ = text;
}

void Player::apply_track_gain(double gain) {
    track_gain_ = plan_.accepts_gain() ? gain : 1.0;
    pipeline_->set_track_gain(track_gain_);
}

// Reads the index here and not earlier: the queue can change while a decoder opens, and update_queue() only
// moves this one.
void Player::add_segment(double start_seconds, double duration_seconds) {
    std::lock_guard lock(mutex_);
    segments_.push_back({session_->ring.total_written(), loader_index_, start_seconds, duration_seconds, track_gain_});
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
    if (plan_.accepts_gain()) session->set_volume(actual, plan_.gain_dither_bits, volume_);
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
                if (jump_request_) return false;
            }
            reopen_output_if_needed();
            if (ring.writable() >= ring.capacity() / 2) break;
            Sleep(kLoaderPollMs);
        }
    }
    return true;
}

// Moves the loader to the requested track and drops what the ring holds. The device gets silence until
// the audio of that track arrives. Returns where in the track to start.
std::optional<double> Player::take_jump() {
    std::lock_guard lock(mutex_);
    if (!jump_request_) return std::nullopt;
    const double seconds = jump_request_->seconds;
    loader_index_ = jump_request_->index;
    waiting_position_ = seconds;
    jump_request_.reset();
    if (session_) {
        session_->refilling.store(true, std::memory_order_release);
        session_->feeding.store(true, std::memory_order_release);
        session_->ring.discard();
        segments_.clear();
    }
    return seconds;
}

Player::PumpResult Player::pump(Decoder& decoder, const SourceFormat& source, const QueueItem& item,
                                std::optional<double> seek) {
    const double bytes_per_second = static_cast<double>(plan_.device.frame_bytes()) * plan_.device.ring_frame_rate();
    uint64_t refill_target = 0;
    double gain = item.gain;
    for (;;) {
        if (stop_requested()) return PumpResult::Stop;
        {
            std::lock_guard lock(mutex_);
            const size_t index = loader_index_;
            if (jump_request_) {
                // A changed queue can hold another track at this index, and the decoder only has its own.
                const bool same_track = index < queue_.size() && queue_[index].id == item.id;
                if (jump_request_->index != index || !same_track) return PumpResult::Jump;
                seek = jump_request_->seconds;
                gain = queue_[index].gain;
                jump_request_.reset();
            }
        }
        if (seek) {
            const double seconds = std::min(*seek, source.seconds());
            const double units = source.dsd ? seconds * source.rate / 8.0 : seconds * source.rate;
            session_->refilling.store(true, std::memory_order_release);
            session_->ring.discard();
            decoder.seek(static_cast<uint64_t>(units));
            pipeline_->reset();
            apply_track_gain(gain);
            add_segment(seconds, source.seconds());
            refill_target = session_->ring.total_written() + static_cast<uint64_t>(bytes_per_second * kSeekRefillSeconds);
            seek.reset();
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

// False when a stop, a jump or a queue that grew cut it short.
bool Player::play_out(bool end_of_queue) {
    std::vector<uint8_t> tail;
    pipeline_->flush(tail);
    return write_all(tail) && drain(end_of_queue);
}

bool Player::drain(bool end_of_queue) {
    session_->feeding.store(false, std::memory_order_release);
    start_output_if_ready(true);
    while (session_->ring.readable() > 0) {
        if (stop_requested()) return false;
        {
            std::lock_guard lock(mutex_);
            if (jump_request_ || (end_of_queue && loader_index_ < queue_.size())) return false;
        }
        reopen_output_if_needed();
        Sleep(kLoaderPollMs);
    }
    // What left the ring is still in the driver's buffers.
    const OutputStats stats = output_->stats();
    const uint64_t rate = std::max<uint32_t>(plan_.device.ring_frame_rate(), 1);
    const uint64_t frames = 2ull * stats.period_frames + stats.latency_frames;
    Sleep(static_cast<DWORD>(frames * 1000 / rate) + kLoaderPollMs);
    return true;
}

void Player::run_queue(double start_seconds) {
    PlaybackPolicy policy;
    DeviceInfo device;
    {
        std::lock_guard lock(mutex_);
        policy = policy_;
        device = device_;
    }
    device.hardware_gain_db = current_hardware_gain_db(device);
    {
        std::lock_guard lock(mutex_);
        hardware_gain_db_ = device.hardware_gain_db;
    }

    std::optional<double> start;
    if (start_seconds > 0.0) start = start_seconds;
    bool jumped = false;  // the ring was dropped, so nothing is left to play out

    const auto follow_jump = [&] {
        start = take_jump();
        jumped = start.has_value();
    };

    for (;;) {
        QueueItem item;
        bool found = false;
        {
            std::lock_guard lock(mutex_);
            const bool wraps = repeat_ == RepeatMode::Queue && !jumped && !queue_.empty();
            if (wraps && loader_index_ >= queue_.size()) loader_index_ = 0;
            found = loader_index_ < queue_.size();
            if (found) item = queue_[loader_index_];
        }

        if (!found) {
            if (!session_ || jumped) return;
            if (play_out(true) || stop_requested()) return;
            follow_jump();  // without a jump the queue grew, and the stream simply carries on
            session_->feeding.store(true, std::memory_order_release);
            pipeline_->reset();
            continue;
        }

        auto decoder = item.open ? item.open() : open_decoder(item.path);
        const SourceFormat source = decoder->format();
        policy.attenuated = volume_.load(std::memory_order_relaxed) < 1.0;
        const RenderPlan plan = make_render_plan(source, device, policy);

        // A new device format means the old stream must play out first. Otherwise the same
        // session continues, with no gap between tracks.
        const bool continues = session_ && plan == plan_ && source.rate == pipeline_source_.rate &&
                               source.channels == pipeline_source_.channels;
        if (session_ && !continues) {
            if (!jumped && !play_out(false)) {
                if (stop_requested()) return;
                follow_jump();
                continue;
            }
            close_session();
        }
        if (!session_) open_session(plan, source);

        {
            std::lock_guard lock(mutex_);
            waiting_position_ = start.value_or(0.0);
        }
        apply_track_gain(item.gain);
        add_segment(0.0, source.seconds());
        const PumpResult result = pump(*decoder, source, item, start);
        if (result == PumpResult::Stop) return;
        if (result == PumpResult::Jump) {
            follow_jump();
            continue;
        }

        std::lock_guard lock(mutex_);
        if (repeat_ != RepeatMode::Track) ++loader_index_;
        start.reset();
        jumped = false;
    }
}

void Player::worker() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        double start_seconds = 0.0;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return quit_ || play_request_.has_value(); });
            if (quit_) break;
            loader_index_ = play_request_->index;
            start_seconds = play_request_->seconds;
            play_request_.reset();
        }
        try {
            run_queue(start_seconds);
        } catch (const std::exception& error) {
            set_error(error.what());
        }
        close_session();
        {
            std::lock_guard lock(mutex_);
            // A request made while the stream was closing still counts.
            if (jump_request_ && !stop_requested()) {
                play_request_.swap(jump_request_);
            } else {
                jump_request_.reset();
                active_ = false;
            }
        }
        stopped_.notify_all();
    }
    if (SUCCEEDED(com)) CoUninitialize();
}

}  // namespace arpegio
