// Single-producer single-consumer ring buffer (lock-free): one thread pushes, one thread
// pops. Used for player -> sim commands and sim -> player results (sim/commands.hpp).
// Capacity is a power of two; push fails (returns false) when full, so the producer can
// retry next frame instead of blocking. Elements must be trivially copyable.

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <type_traits>

namespace geosim::core {

template<typename T, std::size_t Capacity>
class spsc_queue {
    static_assert((Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>, "elements are copied between threads");

public:
    // Producer thread only.
    bool push(T const& value) noexcept {
        const auto head = head_.load(std::memory_order_relaxed);
        const auto tail = tail_.load(std::memory_order_acquire);
        if(head - tail == Capacity) return false;
        slots_[head & (Capacity - 1)] = value;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }
    // Consumer thread only.
    std::optional<T> pop() noexcept {
        const auto tail = tail_.load(std::memory_order_relaxed);
        const auto head = head_.load(std::memory_order_acquire);
        if(tail == head) return std::nullopt;
        T value = slots_[tail & (Capacity - 1)];
        tail_.store(tail + 1, std::memory_order_release);
        return value;
    }
    // Consumer thread only: the next element without removing it.
    [[nodiscard]] T const* peek() const noexcept {
        const auto tail = tail_.load(std::memory_order_relaxed);
        const auto head = head_.load(std::memory_order_acquire);
        return tail == head ? nullptr : &slots_[tail & (Capacity - 1)];
    }
    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

private:
    std::array<T, Capacity> slots_{};
    alignas(64) std::atomic<std::size_t> head_{0}; // written by the producer
    alignas(64) std::atomic<std::size_t> tail_{0}; // written by the consumer
};

} // namespace geosim::core
