#pragma once

#include "blip/led/engine.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace blip::led {

enum class OverloadPolicy : std::uint8_t { reject_newest, drop_oldest_queued };

struct DeadlineMetrics {
    std::uint64_t submitted{};
    std::uint64_t completed{};
    std::uint64_t failed{};
    std::uint64_t rejected{};
    std::uint64_t dropped{};
    std::uint64_t deadline_misses{};
    std::uint64_t maximum_lateness_us{};
    std::size_t queue_high_water{};
};

template <std::size_t BufferBytes, std::size_t PoolSize> class FramePipeline {
    static_assert(BufferBytes > 0U && PoolSize >= 2U);

  public:
    struct WriteLease {
        std::span<std::byte> bytes{};
        std::size_t slot{PoolSize};
        std::uint32_t generation{};
        [[nodiscard]] explicit operator bool() const noexcept { return slot < PoolSize; }
    };

    explicit FramePipeline(OutputDriver& driver,
                           OverloadPolicy policy = OverloadPolicy::reject_newest) noexcept
        : driver_(&driver), policy_(policy) {}

    [[nodiscard]] core::Status start(const OutputConfig& config) noexcept {
        if (started_) {
            return error(core::ErrorCode::invalid_state, "already-started");
        }
        const auto status = driver_->start(config);
        started_ = status.ok();
        return status;
    }

    [[nodiscard]] WriteLease acquire() noexcept {
        if (!started_) {
            return {};
        }
        std::size_t selected = PoolSize;
        for (std::size_t index = 0; index < PoolSize; ++index) {
            if (slots_[index].state == SlotState::free) {
                selected = index;
                break;
            }
        }
        if (selected == PoolSize && policy_ == OverloadPolicy::drop_oldest_queued) {
            std::uint64_t oldest = std::numeric_limits<std::uint64_t>::max();
            for (std::size_t index = 0; index < PoolSize; ++index) {
                if (slots_[index].state == SlotState::queued && slots_[index].sequence < oldest) {
                    oldest = slots_[index].sequence;
                    selected = index;
                }
            }
            if (selected < PoolSize) {
                ++metrics_.dropped;
            }
        }
        if (selected == PoolSize) {
            ++metrics_.rejected;
            return {};
        }
        auto& slot = slots_[selected];
        slot.state = SlotState::writing;
        ++slot.generation;
        if (slot.generation == 0U) {
            ++slot.generation;
        }
        return {slot.bytes, selected, slot.generation};
    }

    [[nodiscard]] core::Status commit(const WriteLease& lease, std::size_t used,
                                      std::uint64_t deadline_us) noexcept {
        if (lease.slot >= PoolSize || slots_[lease.slot].generation != lease.generation ||
            slots_[lease.slot].state != SlotState::writing || used == 0U || used > BufferBytes) {
            return error(core::ErrorCode::invalid_argument, "invalid-write-lease");
        }
        auto& slot = slots_[lease.slot];
        slot.used = used;
        slot.deadline_us = deadline_us;
        slot.sequence = ++next_sequence_;
        slot.state = SlotState::queued;
        update_high_water();
        return core::Status::success();
    }

    void cancel(const WriteLease& lease) noexcept {
        if (lease.slot < PoolSize && slots_[lease.slot].generation == lease.generation &&
            slots_[lease.slot].state == SlotState::writing) {
            slots_[lease.slot].state = SlotState::free;
        }
    }

    // Called by the render/output task. It performs no allocation and does at
    // most one completion plus one submission per call.
    void service(std::uint64_t now_us) noexcept {
        if (!started_) {
            return;
        }
        const auto completion = driver_->poll();
        if (completion.state != CompletionState::pending) {
            for (auto& slot : slots_) {
                if (slot.state == SlotState::in_flight && slot.sequence == completion.sequence) {
                    if (completion.state == CompletionState::complete) {
                        ++metrics_.completed;
                        const auto at =
                            completion.completed_at_us == 0U ? now_us : completion.completed_at_us;
                        if (slot.deadline_us != 0U && at > slot.deadline_us) {
                            ++metrics_.deadline_misses;
                            const auto late = at - slot.deadline_us;
                            if (late > metrics_.maximum_lateness_us) {
                                metrics_.maximum_lateness_us = late;
                            }
                        }
                    } else {
                        ++metrics_.failed;
                    }
                    slot.state = SlotState::free;
                    break;
                }
            }
        }
        if (has_in_flight()) {
            return;
        }
        auto* next = oldest_queued();
        if (next == nullptr) {
            return;
        }
        const auto status =
            driver_->submit({std::span<const std::byte>{next->bytes}.first(next->used),
                             next->sequence, next->deadline_us});
        if (status) {
            next->state = SlotState::in_flight;
            ++metrics_.submitted;
        } else if (status.error().code != core::ErrorCode::queue_full) {
            next->state = SlotState::free;
            ++metrics_.failed;
        }
    }

    [[nodiscard]] core::Status stop() noexcept {
        if (!started_) {
            return core::Status::success();
        }
        const auto status = driver_->stop();
        for (auto& slot : slots_) {
            slot.state = SlotState::free;
        }
        started_ = false;
        return status;
    }

    [[nodiscard]] const DeadlineMetrics& metrics() const noexcept { return metrics_; }

  private:
    enum class SlotState : std::uint8_t { free, writing, queued, in_flight };
    struct Slot {
        alignas(16) std::array<std::byte, BufferBytes> bytes{};
        SlotState state{SlotState::free};
        std::size_t used{};
        std::uint32_t generation{};
        std::uint64_t sequence{};
        std::uint64_t deadline_us{};
    };

    [[nodiscard]] bool has_in_flight() const noexcept {
        for (const auto& slot : slots_) {
            if (slot.state == SlotState::in_flight) {
                return true;
            }
        }
        return false;
    }
    [[nodiscard]] Slot* oldest_queued() noexcept {
        Slot* selected = nullptr;
        for (auto& slot : slots_) {
            if (slot.state == SlotState::queued &&
                (selected == nullptr || slot.sequence < selected->sequence)) {
                selected = &slot;
            }
        }
        return selected;
    }
    void update_high_water() noexcept {
        std::size_t queued = 0U;
        for (const auto& slot : slots_) {
            queued +=
                slot.state == SlotState::queued || slot.state == SlotState::in_flight ? 1U : 0U;
        }
        if (queued > metrics_.queue_high_water) {
            metrics_.queue_high_water = queued;
        }
    }
    [[nodiscard]] static core::Status error(core::ErrorCode code,
                                            std::string_view detail) noexcept {
        return core::Status::failure(
            {core::ErrorDomain::transport, code, "blip.led.pipeline", "frame", detail});
    }

    OutputDriver* driver_{};
    OverloadPolicy policy_{};
    std::array<Slot, PoolSize> slots_{};
    DeadlineMetrics metrics_{};
    std::uint64_t next_sequence_{};
    bool started_{};
};

} // namespace blip::led
