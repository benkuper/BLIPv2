#pragma once

#include <array>
#include <cstddef>
#include <utility>

namespace blip::core {

template <typename T, std::size_t Capacity> class FixedVector {
    static_assert(Capacity > 0, "FixedVector capacity must be positive");

  public:
    using value_type = T;

    [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }
    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] constexpr bool full() const noexcept { return size_ == Capacity; }

    [[nodiscard]] constexpr T* begin() noexcept { return values_.data(); }
    [[nodiscard]] constexpr const T* begin() const noexcept { return values_.data(); }
    [[nodiscard]] constexpr T* end() noexcept { return values_.data() + size_; }
    [[nodiscard]] constexpr const T* end() const noexcept { return values_.data() + size_; }

    [[nodiscard]] constexpr T& operator[](std::size_t index) noexcept { return values_[index]; }
    [[nodiscard]] constexpr const T& operator[](std::size_t index) const noexcept {
        return values_[index];
    }

    [[nodiscard]] constexpr bool push_back(const T& value) noexcept {
        if (full()) {
            return false;
        }
        values_[size_++] = value;
        return true;
    }

    [[nodiscard]] constexpr bool push_back(T&& value) noexcept {
        if (full()) {
            return false;
        }
        values_[size_++] = std::move(value);
        return true;
    }

    constexpr void pop_back() noexcept {
        if (size_ > 0) {
            --size_;
            values_[size_] = T{};
        }
    }

    constexpr void clear() noexcept {
        while (!empty()) {
            pop_back();
        }
    }

  private:
    std::array<T, Capacity> values_{};
    std::size_t size_{0};
};

} // namespace blip::core
