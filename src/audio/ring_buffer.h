// Single-producer single-consumer byte ring for the real-time path.
// Counters run free and the memory is touched up front, so the consumer never page-faults on
// first use, takes a lock or allocates.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace arpegio {

class RingBuffer {
public:
    explicit RingBuffer(size_t min_capacity);
    ~RingBuffer();
    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    size_t capacity() const noexcept { return capacity_; }

    // Producer side.
    size_t writable() const noexcept;
    size_t write(const uint8_t* source, size_t bytes) noexcept;

    // Consumer side.
    size_t readable() const noexcept;
    size_t read(uint8_t* destination, size_t bytes) noexcept;

    // Bytes that crossed each side since construction. Any thread may read these.
    uint64_t total_written() const noexcept { return write_pos_.load(std::memory_order_acquire); }
    uint64_t total_read() const noexcept { return read_pos_.load(std::memory_order_acquire); }

    // Makes the consumer skip everything written so far. Call it with the producer idle.
    void discard() noexcept;

private:
    uint8_t* memory_ = nullptr;
    size_t capacity_ = 0;
    size_t mask_ = 0;
    bool locked_ = false;

    alignas(64) std::atomic<uint64_t> write_pos_{0};
    alignas(64) std::atomic<uint64_t> read_pos_{0};
    std::atomic<uint64_t> discard_to_{0};  // target + 1, or 0 when nothing is pending
};

}  // namespace arpegio
