// Playback engine. The loader thread decodes ahead into a large ring and the device callback only
// copies bytes from it, so disk access and DSP never run on the real-time path.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "audio/ring_buffer.h"
#include "decode/decoder.h"
#include "engine/pipeline.h"
#include "engine/render_plan.h"
#include "output/output.h"

namespace arpegio {

struct QueueItem {
    std::filesystem::path path;
    std::function<std::unique_ptr<Decoder>()> open;  // overrides `path` when set
};

enum class PlayerState { Stopped, Playing, Paused };

struct PlayerStatus {
    PlayerState state = PlayerState::Stopped;
    size_t queue_index = 0;
    double position_seconds = 0.0;
    double duration_seconds = 0.0;
    std::string signal_path;
    bool bit_perfect = false;
    size_t buffered_bytes = 0;
    size_t ring_capacity = 0;
    uint64_t underruns = 0;
    uint64_t clipped_samples = 0;
    uint64_t reopened = 0;
    OutputStats output;
    std::string error;
};

// Creates the output for a device. Tests replace it with a fake.
using OutputFactory = std::function<std::unique_ptr<Output>(const DeviceInfo&, const OutputOptions&)>;

class Player {
public:
    explicit Player(OutputFactory factory = make_output);
    ~Player();
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    void set_device(DeviceInfo device, OutputOptions options = {});
    void set_policy(const PlaybackPolicy& policy);
    void set_queue(std::vector<QueueItem> items);

    void play(size_t index = 0);
    void set_paused(bool paused);
    void stop();
    void seek(double seconds);

    PlayerStatus status() const;
    bool wait_until_stopped(uint32_t timeout_ms);

private:
    class Session;
    enum class PumpResult { Finished, Stop, Jump };

    struct Segment {
        uint64_t ring_position;
        size_t queue_index;
        double start_seconds;
        double duration_seconds;
    };

    void worker();
    void run_queue(size_t index);
    PumpResult pump(Decoder& decoder, const SourceFormat& source, size_t index);
    bool write_all(const std::vector<uint8_t>& bytes);
    void open_session(const RenderPlan& plan, const SourceFormat& source);
    void close_session();
    void start_output_if_ready(bool force);
    void reopen_output_if_needed();
    void drain();
    void add_segment(size_t index, double start_seconds, double duration_seconds);
    void set_error(const std::string& text);
    bool stop_requested() const { return stop_flag_.load(std::memory_order_acquire); }

    OutputFactory factory_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable stopped_;

    // Guarded by mutex_.
    DeviceInfo device_;
    OutputOptions options_;
    PlaybackPolicy policy_;
    std::vector<QueueItem> queue_;
    std::optional<size_t> play_request_;
    std::optional<double> seek_request_;
    bool quit_ = false;
    bool active_ = false;
    std::string error_;
    std::string signal_path_;
    bool bit_perfect_ = false;
    std::deque<Segment> segments_;
    OutputStats last_stats_;
    uint64_t last_underruns_ = 0;
    std::shared_ptr<Session> session_;

    std::atomic<bool> stop_flag_{false};
    std::atomic<bool> paused_{false};
    std::atomic<uint64_t> clipped_{0};
    std::atomic<uint64_t> reopened_{0};

    // Loader thread only.
    std::unique_ptr<Output> output_;
    std::unique_ptr<Pipeline> pipeline_;
    SourceFormat pipeline_source_;
    RenderPlan plan_;
    std::vector<uint8_t> chunk_;
    bool output_started_ = false;

    std::thread thread_;
};

}  // namespace arpegio
