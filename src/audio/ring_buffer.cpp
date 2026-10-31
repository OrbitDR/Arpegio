#include "audio/ring_buffer.h"

#include <windows.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <new>

namespace arpegio {

RingBuffer::RingBuffer(size_t min_capacity) {
    capacity_ = std::bit_ceil(std::max<size_t>(min_capacity, 4096));
    mask_ = capacity_ - 1;
    memory_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, capacity_, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!memory_) throw std::bad_alloc();

    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    for (size_t offset = 0; offset < capacity_; offset += info.dwPageSize) {
        memory_[offset] = 0;
    }

    // Best effort: keeps the ring in RAM, but playback does not depend on it.
    SIZE_T minimum = 0;
    SIZE_T maximum = 0;
    HANDLE process = GetCurrentProcess();
    if (GetProcessWorkingSetSize(process, &minimum, &maximum) &&
        SetProcessWorkingSetSize(process, minimum + capacity_, maximum + capacity_)) {
        locked_ = VirtualLock(memory_, capacity_) != 0;
    }
}

RingBuffer::~RingBuffer() {
    if (locked_) VirtualUnlock(memory_, capacity_);
    VirtualFree(memory_, 0, MEM_RELEASE);
}

size_t RingBuffer::writable() const noexcept {
    return capacity_ - static_cast<size_t>(write_pos_.load(std::memory_order_relaxed) -
                                           read_pos_.load(std::memory_order_acquire));
}

size_t RingBuffer::write(const uint8_t* source, size_t bytes) noexcept {
    const uint64_t position = write_pos_.load(std::memory_order_relaxed);
    const size_t count = std::min(bytes, writable());
    const size_t offset = static_cast<size_t>(position) & mask_;
    const size_t first = std::min(count, capacity_ - offset);
    std::memcpy(memory_ + offset, source, first);
    std::memcpy(memory_, source + first, count - first);
    write_pos_.store(position + count, std::memory_order_release);
    return count;
}

size_t RingBuffer::readable() const noexcept {
    return static_cast<size_t>(write_pos_.load(std::memory_order_acquire) -
                               read_pos_.load(std::memory_order_relaxed));
}

size_t RingBuffer::read(uint8_t* destination, size_t bytes) noexcept {
    if (const uint64_t target = discard_to_.exchange(0, std::memory_order_acq_rel)) {
        read_pos_.store(target - 1, std::memory_order_release);
    }
    const uint64_t position = read_pos_.load(std::memory_order_relaxed);
    const size_t count = std::min(bytes, readable());
    const size_t offset = static_cast<size_t>(position) & mask_;
    const size_t first = std::min(count, capacity_ - offset);
    std::memcpy(destination, memory_ + offset, first);
    std::memcpy(destination + first, memory_, count - first);
    read_pos_.store(position + count, std::memory_order_release);
    return count;
}

void RingBuffer::discard() noexcept {
    discard_to_.store(write_pos_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
}

}  // namespace arpegio
