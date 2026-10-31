#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <numeric>
#include <thread>
#include <vector>

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
