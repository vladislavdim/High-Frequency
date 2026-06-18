#pragma once
// Sentinel_LockFreeRingBuffer.hpp
//
// Single-Producer/Single-Consumer lock-free ring buffer.
// One network/IO thread pushes MarketTick events; one analysis thread pops
// them. This is the only safe topology for a *wait-free* (not just lock-free)
// queue without a CAS loop. If you need multiple producers (e.g. several
// exchange connections), run one ring buffer per producer and fan them into
// the analysis thread, rather than turning this into an MPSC structure.
//
// Design notes:
//  - Capacity must be a power of two so index masking replaces modulo.
//  - head/tail are each pinned to their own cache line (alignas(64)) so the
//    producer writing `tail` never invalidates the consumer's cache line for
//    `head`, and vice-versa (avoids false sharing, the #1 latency killer in
//    naive ring buffer implementations).
//  - Memory ordering: producer publishes with release, consumer observes
//    with acquire. The "other" index is read with relaxed ordering as a
//    capacity hint only - acquire/release on the index actually being
//    updated is what creates the happens-before edge for the slot data.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

namespace sentinel {

constexpr std::size_t kCacheLineSize = 64;

template <typename T, std::size_t Capacity>
class LockFreeRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");
    static_assert(std::is_nothrow_move_constructible_v<T> ||
                  std::is_trivially_copyable_v<T>,
                  "T should be cheap/nothrow to move for a lock-free queue");

public:
    LockFreeRingBuffer() = default;
    LockFreeRingBuffer(const LockFreeRingBuffer&) = delete;
    LockFreeRingBuffer& operator=(const LockFreeRingBuffer&) = delete;

    // Producer side. Returns false if the buffer is full (caller decides
    // whether to drop the tick, spin, or grow a backlog counter).
    bool try_push(const T& value) noexcept {
        const std::size_t current_tail = tail_.load(std::memory_order_relaxed);
        const std::size_t next_tail = increment(current_tail);

        if (next_tail == head_.load(std::memory_order_acquire)) {
            ++dropped_; // ring full; never block the network thread
            return false;
        }

        buffer_[current_tail] = value;
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    bool try_push(T&& value) noexcept {
        const std::size_t current_tail = tail_.load(std::memory_order_relaxed);
        const std::size_t next_tail = increment(current_tail);

        if (next_tail == head_.load(std::memory_order_acquire)) {
            ++dropped_;
            return false;
        }

        buffer_[current_tail] = std::move(value);
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    // Consumer side. Returns std::nullopt if the buffer is empty.
    std::optional<T> try_pop() noexcept {
        const std::size_t current_head = head_.load(std::memory_order_relaxed);

        if (current_head == tail_.load(std::memory_order_acquire)) {
            return std::nullopt; // empty
        }

        T value = std::move(buffer_[current_head]);
        head_.store(increment(current_head), std::memory_order_release);
        return value;
    }

    // Approximate size; only safe as a monitoring hint since producer/consumer
    // indices can move between the two loads.
    std::size_t size_hint() const noexcept {
        const std::size_t h = head_.load(std::memory_order_acquire);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        return (t >= h) ? (t - h) : (Capacity - h + t);
    }

    std::uint64_t dropped_count() const noexcept { return dropped_; }
    static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    static constexpr std::size_t increment(std::size_t idx) noexcept {
        return (idx + 1) & (Capacity - 1);
    }

    std::array<T, Capacity> buffer_{};

    alignas(kCacheLineSize) std::atomic<std::size_t> head_{0};
    alignas(kCacheLineSize) std::atomic<std::size_t> tail_{0};
    alignas(kCacheLineSize) std::atomic<std::uint64_t> dropped_{0};
};

} // namespace sentinel
