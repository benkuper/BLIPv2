#pragma once

#include "blip/core/error.hpp"
#include "blip/core/ring_queue.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace blip::core {

enum class TaskClass : std::uint8_t { render, control, transport, maintenance };
enum class OverflowPolicy : std::uint8_t { reject_new, drop_oldest, coalesce_by_key, fault };

class Clock {
  public:
    virtual ~Clock() = default;
    [[nodiscard]] virtual std::uint64_t now_us() noexcept = 0;
};

class Tick32Extender {
  public:
    [[nodiscard]] std::uint64_t update(std::uint32_t sample) noexcept {
        if (!initialized_) {
            initialized_ = true;
            last_ = sample;
            total_ = sample;
            return total_;
        }
        total_ += static_cast<std::uint32_t>(sample - last_);
        last_ = sample;
        return total_;
    }

  private:
    bool initialized_{};
    std::uint32_t last_{};
    std::uint64_t total_{};
};

struct CancellationToken {
    bool cancelled{};
};

using JobFunction = Status (*)(void*) noexcept;

struct Job {
    std::string_view name{};
    std::string_view owner{};
    TaskClass task_class{TaskClass::control};
    OverflowPolicy overflow_policy{OverflowPolicy::reject_new};
    JobFunction function{};
    void* context{};
    CancellationToken* cancellation{};
    std::uint32_t budget_us{};
};

struct QueueMetrics {
    std::uint32_t submitted{};
    std::uint32_t executed{};
    std::uint32_t rejected{};
    std::uint32_t dropped{};
    std::uint32_t coalesced{};
    std::uint32_t faults{};
    std::uint32_t cancelled{};
    std::uint32_t budget_overruns{};
    std::size_t high_water_mark{};
};

template <std::size_t QueueCapacity> class Scheduler {
  public:
    explicit Scheduler(Clock& clock) noexcept : clock_(&clock) {}

    [[nodiscard]] Status schedule(Job job) noexcept {
        if (job.name.empty() || job.owner.empty() || job.function == nullptr ||
            job.budget_us == 0) {
            return failure(ErrorCode::invalid_argument, job.owner, "scheduler.schedule", job.name);
        }
        const auto index = task_index(job.task_class);
        auto& queue = queues_[index];
        auto& metrics = metrics_[index];
        if (faulted_[index]) {
            return failure(ErrorCode::queue_faulted, job.owner, "scheduler.schedule", job.name);
        }
        saturating_increment(metrics.submitted);
        if (!queue.full()) {
            static_cast<void>(queue.push(job));
            update_high_water(index);
            return Status::success();
        }

        switch (job.overflow_policy) {
        case OverflowPolicy::reject_new:
            saturating_increment(metrics.rejected);
            return failure(ErrorCode::queue_full, job.owner, "scheduler.schedule", job.name);
        case OverflowPolicy::drop_oldest: {
            Job discarded{};
            static_cast<void>(queue.pop(discarded));
            static_cast<void>(queue.push(job));
            saturating_increment(metrics.dropped);
            return Status::success();
        }
        case OverflowPolicy::coalesce_by_key:
            for (std::size_t candidate = 0; candidate < queue.size(); ++candidate) {
                if (queue[candidate].name == job.name && queue[candidate].owner == job.owner) {
                    queue[candidate] = job;
                    saturating_increment(metrics.coalesced);
                    return Status::success();
                }
            }
            saturating_increment(metrics.rejected);
            return failure(ErrorCode::queue_full, job.owner, "scheduler.schedule", job.name);
        case OverflowPolicy::fault:
            faulted_[index] = true;
            saturating_increment(metrics.faults);
            return failure(ErrorCode::queue_faulted, job.owner, "scheduler.schedule", job.name);
        }
        return failure(ErrorCode::invalid_argument, job.owner, "scheduler.schedule", job.name);
    }

    [[nodiscard]] Status run_next(TaskClass task_class) noexcept {
        const auto index = task_index(task_class);
        Job job{};
        if (!queues_[index].pop(job)) {
            return Status::success();
        }
        if (job.cancellation != nullptr && job.cancellation->cancelled) {
            saturating_increment(metrics_[index].cancelled);
            return failure(ErrorCode::cancelled, job.owner, "scheduler.run_next", job.name);
        }

        current_task_ = task_class;
        in_job_ = true;
        const auto start = clock_->now_us();
        const auto job_status = job.function(job.context);
        const auto elapsed = clock_->now_us() - start;
        in_job_ = false;
        saturating_increment(metrics_[index].executed);

        if (!job_status) {
            return job_status;
        }
        if (elapsed > job.budget_us) {
            saturating_increment(metrics_[index].budget_overruns);
            return failure(ErrorCode::budget_exceeded, job.owner, "scheduler.run_next", job.name);
        }
        return Status::success();
    }

    [[nodiscard]] Status require_context(TaskClass expected,
                                         std::string_view operation) const noexcept {
        if (!in_job_ || current_task_ != expected) {
            return failure(ErrorCode::wrong_task_context, {}, operation, "task-context");
        }
        return Status::success();
    }

    void cancel_owner(std::string_view owner) noexcept {
        for (std::size_t index = 0; index < queues_.size(); ++index) {
            auto& queue = queues_[index];
            const auto original_size = queue.size();
            for (std::size_t item = 0; item < original_size; ++item) {
                Job job{};
                static_cast<void>(queue.pop(job));
                if (job.owner == owner) {
                    saturating_increment(metrics_[index].cancelled);
                } else {
                    static_cast<void>(queue.push(job));
                }
            }
        }
    }

    [[nodiscard]] const QueueMetrics& metrics(TaskClass task_class) const noexcept {
        return metrics_[task_index(task_class)];
    }

    [[nodiscard]] bool faulted(TaskClass task_class) const noexcept {
        return faulted_[task_index(task_class)];
    }

  private:
    [[nodiscard]] static constexpr std::size_t task_index(TaskClass task_class) noexcept {
        return static_cast<std::size_t>(task_class);
    }

    static void saturating_increment(std::uint32_t& value) noexcept {
        if (value != std::numeric_limits<std::uint32_t>::max()) {
            ++value;
        }
    }

    void update_high_water(std::size_t index) noexcept {
        if (queues_[index].high_water_mark() > metrics_[index].high_water_mark) {
            metrics_[index].high_water_mark = queues_[index].high_water_mark();
        }
    }

    [[nodiscard]] static Status failure(ErrorCode code, std::string_view owner,
                                        std::string_view operation,
                                        std::string_view detail) noexcept {
        return Status::failure({ErrorDomain::scheduler, code, owner, operation, detail});
    }

    Clock* clock_{};
    std::array<RingQueue<Job, QueueCapacity>, 4> queues_{};
    std::array<QueueMetrics, 4> metrics_{};
    std::array<bool, 4> faulted_{};
    TaskClass current_task_{TaskClass::control};
    bool in_job_{};
};

} // namespace blip::core
