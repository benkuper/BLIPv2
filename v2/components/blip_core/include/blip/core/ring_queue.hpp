#pragma once

#include <array>
#include <cstddef>
#include <utility>

namespace blip::core {

template <typename T, std::size_t Capacity> class RingQueue {
    static_assert(Capacity > 0, "RingQueue capacity must be positive");

  public:
    [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }
    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] constexpr bool full() const noexcept { return size_ == Capacity; }
    [[nodiscard]] constexpr std::size_t high_water_mark() const noexcept {
        return high_water_mark_;
    }

    [[nodiscard]] bool push(T value) noexcept {
        if (full()) {
            return false;
        }
        values_[(head_ + size_) % Capacity] = std::move(value);
        ++size_;
        if (size_ > high_water_mark_) {
            high_water_mark_ = size_;
        }
        return true;
    }

    [[nodiscard]] bool pop(T& output) noexcept {
        if (empty()) {
            return false;
        }
        output = std::move(values_[head_]);
        values_[head_] = T{};
        head_ = (head_ + 1) % Capacity;
        --size_;
        return true;
    }

    [[nodiscard]] T& operator[](std::size_t index) noexcept {
        return values_[(head_ + index) % Capacity];
    }

    [[nodiscard]] const T& operator[](std::size_t index) const noexcept {
        return values_[(head_ + index) % Capacity];
    }

    void clear() noexcept {
        T discarded{};
        while (pop(discarded)) {
        }
    }

  private:
    std::array<T, Capacity> values_{};
    std::size_t head_{};
    std::size_t size_{};
    std::size_t high_water_mark_{};
};

} // namespace blip::core
