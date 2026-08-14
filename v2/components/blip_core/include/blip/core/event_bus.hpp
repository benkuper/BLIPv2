#pragma once

#include "blip/core/error.hpp"
#include "blip/core/fixed_vector.hpp"
#include "blip/core/ring_queue.hpp"
#include "blip/core/scheduler.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace blip::core {

struct EventMetrics {
    std::uint32_t published{};
    std::uint32_t dispatched{};
    std::uint32_t rejected{};
    std::uint32_t dropped{};
    std::uint32_t coalesced{};
    std::uint32_t faults{};
    std::uint32_t stale_producers{};
    std::size_t high_water_mark{};
};

template <typename Event, std::size_t QueueCapacity, std::size_t MaxSubscribers> class EventBus {
  public:
    using Callback = Status (*)(const Event&, void*) noexcept;

    struct ProducerToken {
        EventBus* bus{};
        std::uint32_t generation{};
    };

    explicit EventBus(TaskClass task_class) noexcept : task_class_(task_class) {}

    [[nodiscard]] Status subscribe(std::string_view name, Callback callback,
                                   void* context) noexcept {
        if (sealed_ || name.empty() || callback == nullptr) {
            return failure(ErrorCode::invalid_state, "event.subscribe", name);
        }
        for (const auto& subscriber : subscribers_) {
            if (subscriber.name == name) {
                return failure(ErrorCode::duplicate_id, "event.subscribe", name);
            }
        }
        if (!subscribers_.push_back({name, callback, context})) {
            return failure(ErrorCode::capacity_exceeded, "event.subscribe", name);
        }
        return Status::success();
    }

    [[nodiscard]] ProducerToken producer() noexcept {
        sealed_ = true;
        return ProducerToken{this, generation_};
    }

    [[nodiscard]] Status publish(const ProducerToken& producer, Event event, std::uint32_t key,
                                 OverflowPolicy policy) noexcept {
        if (producer.bus != this || producer.generation != generation_) {
            saturating_increment(metrics_.stale_producers);
            return failure(ErrorCode::cancelled, "event.publish", "stale-producer");
        }
        if (faulted_) {
            return failure(ErrorCode::queue_faulted, "event.publish", "faulted");
        }
        saturating_increment(metrics_.published);
        Envelope envelope{event, key};
        if (!queue_.full()) {
            static_cast<void>(queue_.push(envelope));
            update_high_water();
            return Status::success();
        }

        switch (policy) {
        case OverflowPolicy::reject_new:
            saturating_increment(metrics_.rejected);
            return failure(ErrorCode::queue_full, "event.publish", "reject-new");
        case OverflowPolicy::drop_oldest: {
            Envelope discarded{};
            static_cast<void>(queue_.pop(discarded));
            static_cast<void>(queue_.push(envelope));
            saturating_increment(metrics_.dropped);
            return Status::success();
        }
        case OverflowPolicy::coalesce_by_key:
            for (std::size_t index = 0; index < queue_.size(); ++index) {
                if (queue_[index].key == key) {
                    queue_[index] = envelope;
                    saturating_increment(metrics_.coalesced);
                    return Status::success();
                }
            }
            saturating_increment(metrics_.rejected);
            return failure(ErrorCode::queue_full, "event.publish", "coalesce-key-missing");
        case OverflowPolicy::fault:
            faulted_ = true;
            saturating_increment(metrics_.faults);
            return failure(ErrorCode::queue_faulted, "event.publish", "overflow-fault");
        }
        return failure(ErrorCode::invalid_argument, "event.publish", "policy");
    }

    [[nodiscard]] Status dispatch_one(TaskClass context) noexcept {
        if (dispatching_) {
            return failure(ErrorCode::recursive_dispatch, "event.dispatch", "recursive");
        }
        if (context != task_class_) {
            return failure(ErrorCode::wrong_task_context, "event.dispatch", "task-context");
        }
        Envelope envelope{};
        if (!queue_.pop(envelope)) {
            return Status::success();
        }
        dispatching_ = true;
        for (const auto& subscriber : subscribers_) {
            const auto status = subscriber.callback(envelope.event, subscriber.context);
            if (!status) {
                dispatching_ = false;
                return status;
            }
        }
        dispatching_ = false;
        saturating_increment(metrics_.dispatched);
        return Status::success();
    }

    void stop() noexcept {
        ++generation_;
        if (generation_ == 0) {
            ++generation_;
        }
        queue_.clear();
    }

    [[nodiscard]] const EventMetrics& metrics() const noexcept { return metrics_; }
    [[nodiscard]] std::size_t queued() const noexcept { return queue_.size(); }
    [[nodiscard]] bool faulted() const noexcept { return faulted_; }

  private:
    struct Envelope {
        Event event{};
        std::uint32_t key{};
    };

    struct Subscriber {
        std::string_view name{};
        Callback callback{};
        void* context{};
    };

    static void saturating_increment(std::uint32_t& value) noexcept {
        if (value != std::numeric_limits<std::uint32_t>::max()) {
            ++value;
        }
    }

    void update_high_water() noexcept {
        if (queue_.high_water_mark() > metrics_.high_water_mark) {
            metrics_.high_water_mark = queue_.high_water_mark();
        }
    }

    [[nodiscard]] static Status failure(ErrorCode code, std::string_view operation,
                                        std::string_view detail) noexcept {
        return Status::failure({ErrorDomain::event, code, {}, operation, detail});
    }

    RingQueue<Envelope, QueueCapacity> queue_{};
    FixedVector<Subscriber, MaxSubscribers> subscribers_{};
    EventMetrics metrics_{};
    TaskClass task_class_{TaskClass::control};
    std::uint32_t generation_{1};
    bool sealed_{};
    bool dispatching_{};
    bool faulted_{};
};

} // namespace blip::core
