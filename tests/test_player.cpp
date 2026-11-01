#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <numeric>
#include <thread>
#include <vector>

#include "decode/dsd_silence.h"
#include "dsd/dop.h"
#include "dsd/dsd_fir.h"
#include "engine/player.h"
#include "test_support.h"

using namespace arpegio;

namespace {

// Pulls as fast as it can and records every byte, standing in for a device.
class CaptureOutput final : public Output {
public:
    explicit CaptureOutput(std::shared_ptr<std::vector<uint8_t>> capture) : capture_(std::move(capture)) {}
    ~CaptureOutput() override { close(); }

    DeviceFormat open(const DeviceFormat& requested, FrameSource& source) override {
        format_ = requested;
        source_ = &source;
        return requested;
    }

    void start() override {
        running_ = true;
        thread_ = std::thread([this] {
            constexpr size_t kFrames = 4096;
            std::vector<uint8_t> block(kFrames * format_.frame_bytes());
            while (running_) {
                source_->pull(block.data(), kFrames);
                {
                    std::lock_guard lock(mutex_);
                    capture_->insert(capture_->end(), block.begin(), block.end());
                }
                Sleep(1);
            }
        });
    }

    void stop() override {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }

    void close() override { stop(); }
    bool needs_reopen() const override { return false; }
    std::string fault() const override { return {}; }
    OutputStats stats() const override { return {}; }

private:
    std::shared_ptr<std::vector<uint8_t>> capture_;
    DeviceFormat format_{};
    FrameSource* source_ = nullptr;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex mutex_;
};

// A counter ramp, so any lost, repeated or altered sample is visible.
class RampDecoder final : public Decoder {
public:
    RampDecoder(int64_t first, uint64_t frames, uint32_t rate = 96000) : first_(first) {
        format_ = {false, rate, 2, 24, frames};
    }

    const SourceFormat& format() const override { return format_; }

    size_t read_pcm(double* out, size_t frames) override {
        const size_t count = static_cast<size_t>(std::min<uint64_t>(frames, format_.length - position_));
        for (size_t i = 0; i < count; ++i) {
            const double value = static_cast<double>(first_ + static_cast<int64_t>(position_ + i)) / 8388608.0;
            out[i * 2] = value;
            out[i * 2 + 1] = -value;
        }
        position_ += count;
        return count;
    }

    void seek(uint64_t position) override { position_ = std::min(position, format_.length); }

private:
    SourceFormat format_;
    int64_t first_;
    uint64_t position_ = 0;
};

class DsdPatternDecoder final : public Decoder {
public:
    DsdPatternDecoder(uint32_t rate, uint64_t bytes) {
        format_ = {true, rate, 2, 1, bytes};
    }

    const SourceFormat& format() const override { return format_; }

    size_t read_dsd(uint8_t* const* planar, size_t bytes) override {
        const size_t count = static_cast<size_t>(std::min<uint64_t>(bytes, format_.length - position_));
        for (size_t i = 0; i < count; ++i) {
            planar[0][i] = static_cast<uint8_t>(position_ + i);
            planar[1][i] = static_cast<uint8_t>(~(position_ + i));
        }
        position_ += count;
        return count;
    }

    void seek(uint64_t position) override { position_ = position; }

private:
    SourceFormat format_;
    uint64_t position_ = 0;
};

DeviceInfo fake_device(OutputKind kind = OutputKind::Asio) {
    DeviceInfo device;
    device.kind = kind;
    device.name = "fake";
    device.channels = 2;
    for (uint32_t rate : {44100u, 48000u, 96000u, 176400u, 352800u}) device.pcm_modes.push_back({rate, WireEncoding::S32, 32});
    device.dsd_rates = {2822400, 5644800};
    return device;
}

struct Fixture {
    std::shared_ptr<std::vector<uint8_t>> capture = std::make_shared<std::vector<uint8_t>>();
    Player player;

    explicit Fixture(DeviceInfo device = fake_device())
        : player([this](const DeviceInfo&, const OutputOptions&) -> std::unique_ptr<Output> {
              return std::make_unique<CaptureOutput>(capture);
          }) {
        player.set_device(std::move(device));
    }

    std::vector<int32_t> samples() const {
        std::vector<int32_t> out(capture->size() / 4);
        std::memcpy(out.data(), capture->data(), out.size() * 4);
        return out;
    }
};

QueueItem ramp(int64_t first, uint64_t frames, uint32_t rate = 96000) {
    QueueItem item;
    item.open = [=] { return std::make_unique<RampDecoder>(first, frames, rate); };
    return item;
}

}  // namespace

TEST(two_tracks_join_without_a_gap_and_bit_exact) {
    Fixture f;
    constexpr uint64_t kFrames = 96000;
    f.player.set_queue({ramp(1, kFrames), ramp(1 + kFrames, kFrames)});
    f.player.play();
    CHECK(f.player.wait_until_stopped(30000));
    const PlayerStatus status = f.player.status();
    CHECK(status.error.empty());
    CHECK(status.underruns == 0);

    const auto samples = f.samples();
    size_t start = 0;
    while (start < samples.size() && samples[start] == 0) ++start;
    CHECK(samples.size() >= start + kFrames * 4);
    bool exact = true;
    for (uint64_t frame = 0; frame < kFrames * 2 && exact; ++frame) {
        const int32_t expected = static_cast<int32_t>((frame + 1) * 256);
        exact = samples[start + frame * 2] == expected && samples[start + frame * 2 + 1] == -expected;
    }
    CHECK(exact);
}

TEST(format_change_reopens_the_device) {
    Fixture f;
    f.player.set_queue({ramp(1, 48000, 96000), ramp(1, 44100, 44100)});
    f.player.play();
    CHECK(f.player.wait_until_stopped(30000));
    CHECK(f.player.status().error.empty());
}

TEST(pause_holds_the_position) {
    Fixture f;
    f.player.set_queue({ramp(1, 96000 * 30)});
    f.player.play();
    while (f.player.status().position_seconds < 0.05) Sleep(2);
    f.player.set_paused(true);
    Sleep(30);
    const double held = f.player.status().position_seconds;
    Sleep(60);
    CHECK_NEAR(f.player.status().position_seconds, held, 1e-9);
    CHECK(f.player.status().state == PlayerState::Paused);
    f.player.set_paused(false);
    f.player.stop();
    CHECK(f.player.wait_until_stopped(5000));
}

TEST(seek_lands_on_the_right_sample) {
    Fixture f;
    f.player.set_queue({ramp(1, 96000 * 60)});
    f.player.play();
    while (f.player.status().position_seconds < 0.02) Sleep(2);
    f.player.seek(30.0);
    for (int i = 0; i < 2000 && f.player.status().position_seconds < 30.1; ++i) Sleep(2);
    CHECK(f.player.status().position_seconds >= 30.0);
    CHECK(f.player.status().underruns == 0);
    f.player.stop();
    CHECK(f.player.wait_until_stopped(5000));

    const auto samples = f.samples();
    const int32_t expected = static_cast<int32_t>((96000 * 30 + 1) * 256);
    bool found = false;
    for (size_t i = 0; i < samples.size() && !found; i += 2) found = samples[i] == expected;
    CHECK(found);
}

TEST(seek_right_after_play_is_not_lost) {
    Fixture f;
    f.player.set_queue({ramp(1, 96000 * 40)});
    f.player.play();
    f.player.seek(20.0);
    for (int i = 0; i < 2000 && f.player.status().position_seconds < 20.1; ++i) Sleep(2);
    CHECK(f.player.status().position_seconds >= 20.0);
    f.player.stop();
    CHECK(f.player.wait_until_stopped(5000));
}

namespace {

constexpr uint64_t kTrackTwo = 1000000;  // first ramp value of the second track, far from the first one

// Keeps reopening visible: a jump inside a stream must not touch the device.
struct CountingFixture {
    std::shared_ptr<std::vector<uint8_t>> capture = std::make_shared<std::vector<uint8_t>>();
    std::atomic<int> opened{0};
    Player player;

    CountingFixture()
        : player([this](const DeviceInfo&, const OutputOptions&) -> std::unique_ptr<Output> {
              ++opened;
              return std::make_unique<CaptureOutput>(capture);
          }) {
        player.set_device(fake_device());
    }
};

// The fake device runs some forty times faster than real time, so a short track can be over before the
// test gets to look at it. Paused, the position holds still while the loader goes on filling the ring.
void hold_at_first_sound(Player& player) {
    while (player.status().position_seconds < 0.02) Sleep(1);
    player.set_paused(true);
}

bool wait_for_position(Player& player, double seconds) {
    for (int i = 0; i < 2000; ++i) {
        if (player.status().position_seconds >= seconds) return true;
        Sleep(2);
    }
    return false;
}

bool wait_for_index(Player& player, size_t index) {
    for (int i = 0; i < 2000; ++i) {
        if (player.status().queue_index == index) return true;
        Sleep(2);
    }
    return false;
}

// The duration comes from the decoder that is open, while the index only says where the queue is.
bool wait_for_duration(Player& player, double seconds) {
    for (int i = 0; i < 2000; ++i) {
        if (std::abs(player.status().duration_seconds - seconds) < 1e-3) return true;
        Sleep(2);
    }
    return false;
}

// The capture contains this ramp value somewhere after `from`.
bool contains_sample(const std::vector<uint8_t>& capture, size_t from, int64_t value) {
    const int32_t wanted = static_cast<int32_t>(value * 256);
    for (size_t at = from; at + 4 <= capture.size(); at += 8) {
        int32_t sample = 0;
        std::memcpy(&sample, capture.data() + at, 4);
        if (sample == wanted) return true;
    }
    return false;
}

}  // namespace

TEST(seek_applies_to_the_track_being_heard_not_the_one_being_decoded) {
    Fixture f;
    constexpr uint64_t kFrames = 96000 * 8;  // much shorter than the ring: the loader is already on track 2
    f.player.set_queue({ramp(1, kFrames), ramp(kTrackTwo, kFrames)});
    f.player.play();
    hold_at_first_sound(f.player);
    Sleep(50);
    CHECK(f.player.status().queue_index == 0);

    const size_t before = f.capture->size();
    f.player.seek(4.0);
    CHECK(wait_for_position(f.player, 4.0));
    CHECK(f.player.status().queue_index == 0);
    f.player.set_paused(false);
    Sleep(50);
    f.player.stop();
    CHECK(f.player.wait_until_stopped(5000));

    CHECK(contains_sample(*f.capture, before, 1 + 96000 * 4));
    CHECK(f.player.status().error.empty());
}

TEST(seek_while_the_last_track_plays_out) {
    Fixture f;
    f.player.set_queue({ramp(1, 96000 * 8)});
    f.player.play();
    hold_at_first_sound(f.player);
    Sleep(100);  // the whole track is decoded by now and only the ring is left

    f.player.seek(1.0);
    CHECK(wait_for_position(f.player, 1.0));
    f.player.set_paused(false);
    Sleep(50);
    f.player.stop();
    CHECK(f.player.wait_until_stopped(5000));
    CHECK(contains_sample(*f.capture, 0, 1 + 96000));
}

TEST(next_stays_in_the_open_stream) {
    CountingFixture f;
    f.player.set_queue({ramp(1, 96000 * 8), ramp(kTrackTwo, 96000 * 8)});
    f.player.play();
    hold_at_first_sound(f.player);

    const size_t before = f.capture->size();
    f.player.next();
    CHECK(wait_for_index(f.player, 1));
    f.player.set_paused(false);
    Sleep(50);
    f.player.stop();
    CHECK(f.player.wait_until_stopped(5000));

    CHECK(f.opened == 1);
    CHECK(contains_sample(*f.capture, before, kTrackTwo));
    CHECK(f.player.status().underruns == 0);
}

TEST(next_on_the_last_track_ends_the_queue) {
    Fixture f;
    f.player.set_queue({ramp(1, 96000 * 8)});
    f.player.play();
    while (f.player.status().position_seconds < 0.02) Sleep(1);
    f.player.next();
    CHECK(f.player.wait_until_stopped(5000));
    CHECK(f.player.status().error.empty());
}

TEST(previous_restarts_the_track_and_then_goes_back) {
    CountingFixture f;
    f.player.set_queue({ramp(1, 96000 * 8), ramp(kTrackTwo, 96000 * 8)});
    f.player.play(1, 5.0);
    hold_at_first_sound(f.player);
    CHECK(wait_for_position(f.player, 5.0));
    CHECK(f.player.status().queue_index == 1);

    f.player.previous();
    for (int i = 0; i < 2000 && f.player.status().position_seconds > 3.0; ++i) Sleep(2);
    CHECK(f.player.status().queue_index == 1);
    CHECK(f.player.status().position_seconds <= 3.0);

    f.player.previous();
    CHECK(wait_for_index(f.player, 0));
    f.player.stop();
    CHECK(f.player.wait_until_stopped(5000));
    CHECK(f.opened == 1);
}

TEST(play_while_playing_reuses_the_device) {
    CountingFixture f;
    f.player.set_queue({ramp(1, 96000 * 20), ramp(kTrackTwo, 96000 * 200)});
    f.player.play();
    hold_at_first_sound(f.player);
    f.player.play(1, 2.0);
    CHECK(wait_for_index(f.player, 1));
    CHECK(wait_for_position(f.player, 2.0));
    f.player.stop();
    CHECK(f.player.wait_until_stopped(5000));
    CHECK(f.opened == 1);
}

TEST(a_new_queue_plays_its_own_track_at_the_index_being_decoded) {
    constexpr int64_t kChosen = 6000000;  // past every value of the track it replaces
    CountingFixture f;
    QueueItem playing = ramp(1, 96000 * 60);
    QueueItem chosen = ramp(kChosen, 96000 * 20);
    playing.id = 1;
    chosen.id = 2;
    f.player.set_queue({playing});
    f.player.play();
    hold_at_first_sound(f.player);
    const size_t before = f.capture->size();

    f.player.set_queue({chosen});
    f.player.play(0);
    CHECK(wait_for_duration(f.player, 20.0));
    CHECK(wait_for_position(f.player, 1.0));
    f.player.stop();
    CHECK(f.player.wait_until_stopped(5000));

    CHECK(f.opened == 1);
    CHECK(contains_sample(*f.capture, before, kChosen));
}

TEST(a_jump_to_another_format_reopens_at_once) {
    CountingFixture f;
    f.player.set_queue({ramp(1, 96000 * 30, 96000), ramp(1, 44100 * 2, 44100)});
    f.player.play();
    while (f.player.status().position_seconds < 0.02) Sleep(1);
    f.player.next();
    CHECK(wait_for_index(f.player, 1));
    CHECK(f.player.wait_until_stopped(5000));
    CHECK(f.opened == 2);
    CHECK(f.player.status().error.empty());
}

TEST(appending_while_the_last_track_plays_out_continues_the_stream) {
    CountingFixture f;
    f.player.set_queue({ramp(1, 96000 * 8)});
    f.player.play();
    hold_at_first_sound(f.player);
    Sleep(100);

    std::vector<QueueItem> longer = {ramp(1, 96000 * 8), ramp(1 + 96000 * 8, 96000)};
    f.player.update_queue(std::move(longer), 0);
    f.player.set_paused(false);
    CHECK(f.player.wait_until_stopped(30000));
    CHECK(f.opened == 1);

    // Nothing was lost or repeated across the join. The ramp is never zero, so skipping zeros only
    // removes the silence of the pause.
    std::vector<int32_t> samples(f.capture->size() / 4);
    std::memcpy(samples.data(), f.capture->data(), samples.size() * 4);
    uint64_t expected = 1;
    bool exact = true;
    for (size_t i = 0; i < samples.size() && exact; i += 2) {
        if (samples[i] == 0) continue;
        exact = samples[i] == static_cast<int32_t>(expected * 256);
        ++expected;
    }
    CHECK(exact);
    CHECK(expected == 96000ull * 9 + 1);
}

TEST(editing_the_tracks_already_in_the_ring_refills_from_the_position_heard) {
    CountingFixture f;
    QueueItem first = ramp(1, 96000 * 8);
    QueueItem second = ramp(kTrackTwo, 96000 * 8);
    QueueItem third = ramp(5000000, 96000 * 8);
    first.id = 1;
    second.id = 2;
    third.id = 3;
    f.player.set_queue({first, second});
    f.player.play();
    hold_at_first_sound(f.player);
    Sleep(50);
    const size_t before = f.capture->size();

    f.player.update_queue({first, third}, 0);  // the second track was already decoded
    f.player.set_paused(false);
    CHECK(wait_for_index(f.player, 1));
    CHECK(f.player.wait_until_stopped(30000));
    CHECK(f.opened == 1);
    CHECK(contains_sample(*f.capture, before, 5000000));
}

TEST(moving_the_heard_track_onto_the_index_being_decoded_keeps_it_heard) {
    CountingFixture f;
    QueueItem heard = ramp(1, 96000 * 8);
    QueueItem decoding = ramp(kTrackTwo, 96000 * 100);  // longer than the ring, so the loader stays on it
    heard.id = 1;
    decoding.id = 2;
    f.player.set_queue({heard, decoding});
    f.player.play();
    hold_at_first_sound(f.player);
    Sleep(50);
    const size_t before = f.capture->size();

    f.player.update_queue({decoding, heard}, 1);
    f.player.set_paused(false);
    CHECK(f.player.wait_until_stopped(30000));
    CHECK(f.opened == 1);
    CHECK(contains_sample(*f.capture, before, 1 + 96000 * 5));
}

TEST(editing_before_the_current_track_only_renumbers_it) {
    CountingFixture f;
    QueueItem first = ramp(1, 96000 * 40);
    QueueItem second = ramp(kTrackTwo, 96000 * 40);
    first.id = 1;
    second.id = 2;
    f.player.set_queue({first, second});
    f.player.play(1);
    hold_at_first_sound(f.player);
    CHECK(f.player.status().queue_index == 1);

    const double position = f.player.status().position_seconds;
    f.player.update_queue({second}, 0);
    CHECK(f.player.status().queue_index == 0);
    CHECK(f.player.status().position_seconds >= position);
    f.player.stop();
    CHECK(f.player.wait_until_stopped(5000));
    CHECK(f.opened == 1);
}

TEST(repeat_track_keeps_the_index_and_repeat_queue_wraps_around) {
    Fixture f;
    f.player.set_repeat(RepeatMode::Track);
    f.player.set_queue({ramp(1, 9600), ramp(kTrackTwo, 9600)});
    f.player.play();
    Sleep(200);
    CHECK(f.player.status().queue_index == 0);
    CHECK(f.player.status().state == PlayerState::Playing);

    f.player.set_repeat(RepeatMode::Queue);
    CHECK(wait_for_index(f.player, 1));
    CHECK(wait_for_index(f.player, 0));
    CHECK(f.player.status().state == PlayerState::Playing);
    f.player.stop();
    CHECK(f.player.wait_until_stopped(5000));
}

TEST(position_reads_the_start_point_before_the_first_sound) {
    Fixture f;
    f.player.set_queue({ramp(1, 96000 * 60)});
    f.player.play(0, 25.0);
    // The device has not produced anything yet, and the position must not fall back to zero meanwhile.
    for (int i = 0; i < 200; ++i) {
        const double position = f.player.status().position_seconds;
        CHECK(position >= 25.0);
        if (position < 25.0) break;
    }
    f.player.stop();
    CHECK(f.player.wait_until_stopped(5000));
}

TEST(status_reports_the_plan_of_the_open_stream) {
    Fixture f;
    f.player.set_queue({ramp(1, 96000 * 8)});
    CHECK(!f.player.status().stream_open);
    f.player.play();
    while (!f.player.status().stream_open) Sleep(1);
    const PlayerStatus status = f.player.status();
    CHECK(status.plan.path == RenderPath::Pcm);
    CHECK(status.plan.device.rate == 96000);
    f.player.stop();
    CHECK(f.player.wait_until_stopped(5000));
    CHECK(!f.player.status().stream_open);
}

TEST(native_dsd_passes_through_untouched) {
    Fixture f;
    QueueItem item;
    item.open = [] { return std::make_unique<DsdPatternDecoder>(2822400, 2822400 / 8); };
    f.player.set_queue({item});
    f.player.play();
    CHECK(f.player.wait_until_stopped(30000));
    CHECK(f.player.status().signal_path.find("DSD native") != std::string::npos);

    const auto& bytes = *f.capture;
    size_t start = 0;
    while (start + 1 < bytes.size() && bytes[start] == kDsdSilenceByte) start += 2;
    bool exact = bytes.size() >= start + 2822400 / 8 * 2;
    for (size_t i = 0; i < 2822400 / 8 && exact; ++i) {
        exact = bytes[start + i * 2] == static_cast<uint8_t>(i) && bytes[start + i * 2 + 1] == static_cast<uint8_t>(~i);
    }
    CHECK(exact);
}

TEST(dop_markers_never_break_even_with_a_seek) {
    DeviceInfo device = fake_device(OutputKind::WasapiExclusive);
    device.dsd_rates.clear();
    Fixture f(device);
    PlaybackPolicy policy;
    policy.dsd = DsdPolicy::Dop;
    f.player.set_policy(policy);
    QueueItem item;
    item.open = [] { return std::make_unique<DsdPatternDecoder>(2822400, 2822400 / 8 * 30); };
    f.player.set_queue({item});
    f.player.play();
    while (f.player.status().position_seconds < 0.02) Sleep(2);
    f.player.seek(12.0);
    Sleep(50);
    f.player.stop();
    CHECK(f.player.wait_until_stopped(5000));

    const auto& bytes = *f.capture;
    CHECK(bytes.size() > 8000);
    bool alternating = true;
    for (size_t frame = 1; frame * 8 + 8 <= bytes.size() && alternating; ++frame) {
        const uint8_t previous = bytes[(frame - 1) * 8 + 3];
        const uint8_t current = bytes[frame * 8 + 3];
        alternating = (previous == kDopMarkerA && current == kDopMarkerB) || (previous == kDopMarkerB && current == kDopMarkerA);
        alternating = alternating && bytes[frame * 8 + 7] == current;
    }
    CHECK(alternating);
}

TEST(dsd_to_pcm_keeps_the_length) {
    DeviceInfo device = fake_device();
    device.dsd_rates.clear();
    Fixture f(device);
    QueueItem item;
    item.open = [] { return std::make_unique<DsdPatternDecoder>(5644800, 5644800 / 8); };
    f.player.set_queue({item});
    f.player.play();
    CHECK(f.player.wait_until_stopped(30000));
    const PlayerStatus status = f.player.status();
    CHECK(status.error.empty());
    CHECK(status.signal_path.find("352800") != std::string::npos);
    // One second of DSD must give one second of PCM, plus the capture's trailing silence.
    CHECK(f.capture->size() >= 352800u * 8);
}

namespace {

constexpr int64_t kRampUnit = 256;  // one 24-bit step of the ramp in a 32-bit word

// Where the ramp starts in the capture, whatever the silence before it.
size_t first_sound(const std::vector<int32_t>& samples) {
    size_t start = 0;
    while (start < samples.size() && samples[start] == 0) ++start;
    return start;
}

}  // namespace

TEST(the_volume_is_heard_at_once_and_unity_is_bit_exact_again) {
    Fixture f;
    f.player.set_queue({ramp(1, 96000 * 40)});
    f.player.play();
    CHECK(wait_for_position(f.player, 0.5));
    CHECK(f.player.status().bit_perfect);

    f.player.set_volume(0.5);
    CHECK(!f.player.status().bit_perfect);
    const double lowered_at = f.player.status().position_seconds;
    CHECK(wait_for_position(f.player, lowered_at + 1.0));

    f.player.set_volume(1.0);
    CHECK(f.player.status().bit_perfect);
    const double raised_at = f.player.status().position_seconds;
    CHECK(wait_for_position(f.player, raised_at + 1.0));
    f.player.stop();
    CHECK(f.player.wait_until_stopped(5000));
    CHECK(f.player.status().error.empty());

    // Every frame is compared with the ramp it came from, so a level that arrived late would show.
    const auto samples = f.samples();
    const size_t start = first_sound(samples);
    size_t exact = 0;
    size_t exact_after_halved = 0;
    size_t halved = 0;
    size_t other = 0;
    for (size_t at = start; at + 1 < samples.size(); at += 2) {
        const int64_t expected = static_cast<int64_t>((at - start) / 2 + 1) * kRampUnit;
        const int64_t got = samples[at];
        if (got == expected && samples[at + 1] == -got) {
            ++exact;
            if (halved > 0) ++exact_after_halved;
        } else if (std::llabs(got - expected / 2) <= 2 * kRampUnit) {
            ++halved;
        } else {
            ++other;
        }
    }
    CHECK(halved > 96000 / 2);
    CHECK(exact_after_halved > 96000 / 2);
    CHECK(exact > 96000);
    // Two ramps of 20 ms at 96 kHz are all that is neither one level nor the other.
    CHECK(other <= 2 * 1920);
}

TEST(a_stream_that_opens_with_the_volume_down_never_plays_loud) {
    Fixture f;
    f.player.set_volume(0.25);
    f.player.set_queue({ramp(1 << 20, 96000)});
    f.player.play();
    CHECK(f.player.wait_until_stopped(30000));

    const auto samples = f.samples();
    const size_t start = first_sound(samples);
    CHECK(start < samples.size());
    const int64_t expected = (1 << 20) * kRampUnit / 4;
    CHECK(std::llabs(samples[start] - expected) <= 2 * kRampUnit);
}

TEST(dsd_is_converted_while_the_volume_is_down) {
    Fixture f;
    f.player.set_volume(0.5);
    QueueItem item;
    item.open = [] { return std::make_unique<DsdPatternDecoder>(2822400, 2822400 / 8 * 2); };
    f.player.set_queue({item});
    f.player.play();
    while (!f.player.status().stream_open) Sleep(1);
    CHECK(f.player.status().plan.path == RenderPath::DsdToPcm);
    CHECK(!f.player.status().bit_perfect);
    f.player.stop();
    CHECK(f.player.wait_until_stopped(5000));
}

TEST(native_dsd_is_left_alone_if_the_volume_moves_under_it) {
    Fixture f;
    QueueItem item;
    item.open = [] { return std::make_unique<DsdPatternDecoder>(2822400, 2822400 / 8 * 6); };
    f.player.set_queue({item});
    f.player.play();
    CHECK(wait_for_position(f.player, 0.1));
    CHECK(f.player.status().plan.path == RenderPath::DsdNative);
    f.player.set_volume(0.5);
    CHECK(f.player.status().bit_perfect);
    CHECK(f.player.wait_until_stopped(30000));

    size_t start = 0;
    const auto& bytes = *f.capture;
    while (start + 1 < bytes.size() && !(bytes[start] == 0 && bytes[start + 1] == 0xFF)) ++start;
    CHECK(bytes.size() >= start + size_t{2822400} / 8 * 6 * 2);
    bool exact = true;
    for (size_t i = 0; i < 2822400 / 8 * 6 && exact; ++i) {
        exact = bytes[start + i * 2] == static_cast<uint8_t>(i) && bytes[start + i * 2 + 1] == static_cast<uint8_t>(~i);
    }
    CHECK(exact);
}

TEST(a_gain_belongs_to_its_track_and_does_not_end_the_stream) {
    CountingFixture f;
    constexpr uint64_t kFrames = 96000;
    QueueItem quieter = ramp(1, kFrames);
    quieter.gain = 0.5;
    f.player.set_queue({quieter, ramp(1 + kFrames, kFrames)});
    f.player.play();
    CHECK(f.player.wait_until_stopped(30000));
    CHECK(f.player.status().error.empty());
    CHECK(f.opened == 1);

    std::vector<int32_t> samples(f.capture->size() / 4);
    std::memcpy(samples.data(), f.capture->data(), samples.size() * 4);
    const size_t start = first_sound(samples);
    CHECK(samples.size() >= start + kFrames * 4);

    bool first_scaled = true;
    for (uint64_t frame = 0; frame < kFrames && first_scaled; ++frame) {
        const int64_t expected = static_cast<int64_t>(frame + 1) * kRampUnit / 2;
        first_scaled = std::llabs(samples[start + frame * 2] - expected) <= 2 * kRampUnit;
    }
    CHECK(first_scaled);

    bool second_exact = true;
    for (uint64_t frame = kFrames; frame < kFrames * 2 && second_exact; ++frame) {
        second_exact = samples[start + frame * 2] == static_cast<int32_t>((frame + 1) * kRampUnit);
    }
    CHECK(second_exact);
}

TEST(status_follows_the_gain_of_the_track_being_heard) {
    Fixture f;
    constexpr uint64_t kFrames = 96000 * 4;
    QueueItem scaled = ramp(1, kFrames);
    scaled.gain = 0.5;
    f.player.set_queue({scaled, ramp(kTrackTwo, kFrames)});
    f.player.play();
    hold_at_first_sound(f.player);
    Sleep(50);  // the loader is on the second track by now, which must not show
    CHECK_NEAR(f.player.status().track_gain, 0.5, 1e-12);
    CHECK(!f.player.status().bit_perfect);
    f.player.set_paused(false);
    CHECK(wait_for_index(f.player, 1));
    CHECK_NEAR(f.player.status().track_gain, 1.0, 1e-12);
    CHECK(f.player.status().bit_perfect);
    f.player.stop();
    CHECK(f.player.wait_until_stopped(5000));
}

TEST(reload_applies_a_new_gain_from_the_position_heard) {
    Fixture f;
    constexpr uint64_t kFrames = 96000 * 30;
    QueueItem item = ramp(1, kFrames);
    item.id = 7;
    f.player.set_queue({item});
    f.player.play();
    hold_at_first_sound(f.player);
    const double heard = f.player.status().position_seconds;

    item.gain = 0.5;
    f.player.update_queue({item}, 0);
    f.player.reload();
    for (int i = 0; i < 2000 && f.player.status().track_gain == 1.0; ++i) Sleep(2);
    CHECK_NEAR(f.player.status().track_gain, 0.5, 1e-12);
    CHECK_NEAR(f.player.status().position_seconds, heard, 0.01);
    f.player.stop();
    CHECK(f.player.wait_until_stopped(5000));
}

namespace {

class SilentDecoder final : public Decoder {
public:
    const SourceFormat& format() const override { return format_; }

    size_t read_pcm(double* out, size_t frames) override {
        const size_t count = static_cast<size_t>(std::min<uint64_t>(frames, format_.length - position_));
        std::fill_n(out, count * 2, 0.0);
        position_ += count;
        return count;
    }

    void seek(uint64_t position) override { position_ = position; }

private:
    SourceFormat format_{false, 96000, 2, 24, 96000};
    uint64_t position_ = 0;
};

}  // namespace

TEST(dsd_silence_goes_out_as_dop_with_nothing_but_the_silence_pattern) {
    Fixture f;
    PlaybackPolicy policy;
    policy.dsd = DsdPolicy::Dop;
    f.player.set_policy(policy);
    QueueItem item;
    item.open = [] { return make_dsd_silence(2822400, 2, 1.0); };
    f.player.set_queue({item});
    f.player.play();
    CHECK(f.player.wait_until_stopped(30000));
    CHECK(f.player.status().error.empty());

    // 32-bit little endian words: an empty byte, two of DSD, and the marker on top.
    const auto& bytes = *f.capture;
    CHECK(bytes.size() >= 176400u * 8);
    bool clean = true;
    for (size_t at = 0; at + 4 <= bytes.size() && clean; at += 4) {
        const bool marker = bytes[at + 3] == kDopMarkerA || bytes[at + 3] == kDopMarkerB;
        clean = marker && bytes[at] == 0 && bytes[at + 1] == kDsdSilenceByte && bytes[at + 2] == kDsdSilenceByte;
    }
    CHECK(clean);
}

TEST(a_gain_rewrites_the_samples_with_dither) {
    Fixture f;
    QueueItem item;
    item.open = [] { return std::make_unique<SilentDecoder>(); };
    item.gain = 0.5;
    f.player.set_queue({item});
    f.player.play();
    CHECK(f.player.wait_until_stopped(30000));

    // Digital silence through a gain still comes out with the 24-bit dither on top.
    size_t nonzero = 0;
    for (const int32_t sample : f.samples()) nonzero += sample != 0;
    CHECK(nonzero > 96000);
}
