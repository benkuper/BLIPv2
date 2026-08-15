#pragma once

#include "blip/core/descriptor.hpp"
#include "blip/core/error.hpp"
#include "blip/core/fixed_vector.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

namespace blip::resources {

struct ResourceSpec {
    core::ResourceClass resource_class{core::ResourceClass::gpio};
    std::string_view id{};
    std::uint32_t capabilities{};
    std::uint32_t capacity{1};
    std::string_view reserved_for{};
    std::string_view label{};
    std::int16_t gpio{-1};
    std::string_view electrical{};
    std::string_view reason{};
    std::string_view bus{};
    bool selectable{true};
};

struct ClaimView {
    std::string_view resource_id{};
    std::string_view owner{};
    std::string_view role{};
    core::OwnershipMode mode{core::OwnershipMode::exclusive};
    std::uint32_t member_key{};
    bool setting_optional{};
    bool reboot_required{};
};

enum class ReassignmentKind : std::uint8_t { swap, unassign_and_move };
enum class TransactionStage : std::uint8_t { stop_components, persist_settings, restart_components };

struct ReassignmentRequest {
    std::uint32_t expected_revision{};
    ReassignmentKind kind{ReassignmentKind::swap};
    std::string_view requester{};
    std::string_view previous_owner{};
    std::string_view target_resource{};
    std::uint32_t requester_capabilities{};
    std::uint32_t previous_owner_capabilities{};
    bool previous_owner_releasable{};
};

class TransactionHooks {
  public:
    virtual ~TransactionHooks() = default;
    [[nodiscard]] virtual core::Status run(TransactionStage stage,
                                           const ReassignmentRequest& request) noexcept = 0;
    virtual void rollback(TransactionStage last_completed,
                          const ReassignmentRequest& request) noexcept = 0;
};

template <std::size_t MaxResources, std::size_t MaxLeases> class Broker {
  public:
    class WeakToken {
      public:
        [[nodiscard]] bool valid() const noexcept {
            return broker_ != nullptr && broker_->token_valid(claim_index_, generation_);
        }

      private:
        friend class Broker;
        constexpr WeakToken(Broker* broker, std::size_t claim_index,
                            std::uint32_t generation) noexcept
            : broker_(broker), claim_index_(claim_index), generation_(generation) {}

        Broker* broker_{};
        std::size_t claim_index_{MaxLeases};
        std::uint32_t generation_{};
    };

    class Lease {
      public:
        Lease() noexcept = default;
        ~Lease() { release(); }
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        Lease(Lease&& other) noexcept { move_from(other); }

        Lease& operator=(Lease&& other) noexcept {
            if (this != &other) {
                release();
                move_from(other);
            }
            return *this;
        }

        [[nodiscard]] bool valid() const noexcept {
            return broker_ != nullptr && broker_->token_valid(claim_index_, generation_);
        }

        [[nodiscard]] std::string_view resource_id() const noexcept {
            return valid() ? broker_->resources_[broker_->claims_[claim_index_].resource_index].id
                           : std::string_view{};
        }

        [[nodiscard]] WeakToken weak_token() const noexcept {
            return WeakToken{broker_, claim_index_, generation_};
        }

        void release() noexcept {
            if (broker_ != nullptr) {
                broker_->release(claim_index_, generation_);
                broker_ = nullptr;
            }
        }

      private:
        friend class Broker;
        constexpr Lease(Broker* broker, std::size_t claim_index, std::uint32_t generation) noexcept
            : broker_(broker), claim_index_(claim_index), generation_(generation) {}

        void move_from(Lease& other) noexcept {
            broker_ = other.broker_;
            claim_index_ = other.claim_index_;
            generation_ = other.generation_;
            other.broker_ = nullptr;
            other.claim_index_ = MaxLeases;
            other.generation_ = 0;
        }

        Broker* broker_{};
        std::size_t claim_index_{MaxLeases};
        std::uint32_t generation_{};
    };

    struct LeaseBatch {
        core::FixedVector<Lease, MaxLeases> leases{};
    };

    [[nodiscard]] core::Status add_resource(ResourceSpec resource) noexcept {
        if (inventory_closed_) {
            return failure(core::ErrorCode::invalid_state, {}, "broker.add_resource", "closed");
        }
        if (resource.id.empty() || resource.capacity == 0) {
            return failure(core::ErrorCode::invalid_argument, {}, "broker.add_resource",
                           resource.id);
        }
        for (const auto& existing : resources_) {
            if (existing.resource_class == resource.resource_class && existing.id == resource.id) {
                return failure(core::ErrorCode::duplicate_id, {}, "broker.add_resource",
                               resource.id);
            }
        }
        if (!resources_.push_back(resource)) {
            return failure(core::ErrorCode::capacity_exceeded, {}, "broker.add_resource",
                           resource.id);
        }
        return core::Status::success();
    }

    [[nodiscard]] core::Result<Lease> acquire(std::string_view owner,
                                              const core::ResourceRequest& request) noexcept {
        inventory_closed_ = true;
        if (owner.empty() || request.logical_name.empty() || request.alternatives.empty() ||
            request.amount == 0) {
            return lease_failure(core::ErrorCode::invalid_argument, owner, request.logical_name,
                                 "invalid-request");
        }

        std::size_t selected = MaxResources;
        std::string_view conflict_owner{};
        for (std::size_t resource_index = 0; resource_index < resources_.size(); ++resource_index) {
            const auto& resource = resources_[resource_index];
            if (resource.resource_class != request.resource_class ||
                !is_alternative(resource.id, request.alternatives) ||
                (resource.capabilities & request.required_capabilities) !=
                    request.required_capabilities) {
                continue;
            }
            const auto compatibility = compatible(resource_index, owner, request);
            if (!compatibility.ok) {
                conflict_owner = compatibility.owner;
                continue;
            }
            if (selected == MaxResources || resource.id < resources_[selected].id) {
                selected = resource_index;
            }
        }

        if (selected == MaxResources) {
            return lease_failure(conflict_owner.empty() ? core::ErrorCode::resource_unavailable
                                                        : core::ErrorCode::resource_conflict,
                                 owner, request.logical_name, conflict_owner);
        }

        const std::size_t claim_index = free_claim();
        if (claim_index == MaxLeases) {
            return lease_failure(core::ErrorCode::capacity_exceeded, owner, request.logical_name,
                                 "lease-table");
        }
        auto& claim = claims_[claim_index];
        ++claim.generation;
        if (claim.generation == 0) {
            ++claim.generation;
        }
        claim.active = true;
        claim.resource_index = selected;
        claim.owner = owner;
        claim.role = request.logical_name;
        claim.mode = request.ownership;
        claim.amount = request.amount;
        claim.member_key = request.member_key;
        claim.feature_mask = request.feature_mask;
        claim.incompatible_features = request.incompatible_features;
        claim.setting_optional = request.setting_optional;
        claim.reboot_required = request.reboot_required;
        advance_revision();
        return core::Result<Lease>::success(Lease{this, claim_index, claim.generation});
    }

    [[nodiscard]] core::Result<LeaseBatch>
    acquire_batch(std::string_view owner,
                  std::span<const core::ResourceRequest> requests) noexcept {
        LeaseBatch batch{};
        std::array<bool, MaxLeases> acquired{};
        if (requests.size() > MaxLeases) {
            return batch_failure(core::ErrorCode::capacity_exceeded, owner, "broker.acquire_batch",
                                 "request-count");
        }
        for (std::size_t count = 0; count < requests.size(); ++count) {
            std::size_t selected = requests.size();
            for (std::size_t candidate = 0; candidate < requests.size(); ++candidate) {
                if (!acquired[candidate] &&
                    (selected == requests.size() ||
                     requests[candidate].logical_name < requests[selected].logical_name)) {
                    selected = candidate;
                }
            }
            auto result = acquire(owner, requests[selected]);
            if (!result) {
                return core::Result<LeaseBatch>::failure(result.error());
            }
            acquired[selected] = true;
            static_cast<void>(batch.leases.push_back(std::move(result.value())));
        }
        return core::Result<LeaseBatch>::success(std::move(batch));
    }

    [[nodiscard]] std::size_t active_lease_count() const noexcept {
        std::size_t count = 0;
        for (const auto& claim : claims_) {
            if (claim.active) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] std::uint32_t revision() const noexcept { return revision_; }

    [[nodiscard]] const ResourceSpec* find_resource(std::string_view id) const noexcept {
        const auto index = resource_by_id(id);
        return index == MaxResources ? nullptr : &resources_[index];
    }

    template <typename Visitor> void visit_resources(Visitor&& visitor) const noexcept {
        for (const auto& resource : resources_) {
            visitor(resource);
        }
    }

    template <typename Visitor> void visit_claims(Visitor&& visitor) const noexcept {
        for (const auto& claim : claims_) {
            if (!claim.active) {
                continue;
            }
            visitor(ClaimView{resources_[claim.resource_index].id, claim.owner, claim.role,
                              claim.mode, claim.member_key, claim.setting_optional,
                              claim.reboot_required});
        }
    }

    [[nodiscard]] core::Status reassign(const ReassignmentRequest& request,
                                        TransactionHooks& hooks) noexcept {
        if (request.expected_revision != revision_) {
            return failure(core::ErrorCode::invalid_state, request.requester, "broker.reassign",
                           "stale-revision");
        }
        const std::size_t requester_claim = claim_for_owner(request.requester);
        const std::size_t previous_claim = claim_for_owner(request.previous_owner);
        const std::size_t target = resource_by_id(request.target_resource);
        if (requester_claim == MaxLeases || previous_claim == MaxLeases ||
            target == MaxResources || claims_[previous_claim].resource_index != target ||
            requester_claim == previous_claim) {
            return failure(core::ErrorCode::validation_failed, request.requester,
                           "broker.reassign", "owner-or-target-changed");
        }
        const std::size_t requester_source = claims_[requester_claim].resource_index;
        if (!resources_[target].selectable || !resources_[target].reserved_for.empty() ||
            (resources_[target].capabilities & request.requester_capabilities) !=
                request.requester_capabilities) {
            return failure(core::ErrorCode::resource_unavailable, request.requester,
                           "broker.reassign", resources_[target].reason);
        }
        if (request.kind == ReassignmentKind::swap) {
            if (!resources_[requester_source].selectable ||
                (resources_[requester_source].capabilities &
                 request.previous_owner_capabilities) != request.previous_owner_capabilities) {
                return failure(core::ErrorCode::resource_unavailable, request.previous_owner,
                               "broker.reassign", "swap-source-incompatible");
            }
        } else if (!request.previous_owner_releasable) {
            return failure(core::ErrorCode::validation_failed, request.previous_owner,
                           "broker.reassign", "required-setting");
        }

        auto status = hooks.run(TransactionStage::stop_components, request);
        if (!status) {
            hooks.rollback(TransactionStage::stop_components, request);
            return status;
        }
        const Claim previous_before = claims_[previous_claim];
        claims_[requester_claim].resource_index = target;
        if (request.kind == ReassignmentKind::swap) {
            claims_[previous_claim].resource_index = requester_source;
        } else {
            claims_[previous_claim].active = false;
            claims_[previous_claim].owner = {};
            claims_[previous_claim].role = {};
            claims_[previous_claim].amount = 0;
        }
        for (const auto stage : {TransactionStage::persist_settings,
                                 TransactionStage::restart_components}) {
            status = hooks.run(stage, request);
            if (!status) {
                claims_[requester_claim].resource_index = requester_source;
                claims_[previous_claim] = previous_before;
                hooks.rollback(stage, request);
                return status;
            }
        }
        advance_revision();
        return core::Status::success();
    }

  private:
    struct Claim {
        bool active{};
        std::uint32_t generation{};
        std::size_t resource_index{MaxResources};
        std::string_view owner{};
        std::string_view role{};
        core::OwnershipMode mode{core::OwnershipMode::exclusive};
        std::uint32_t amount{};
        std::uint32_t member_key{};
        std::uint32_t feature_mask{};
        std::uint32_t incompatible_features{};
        bool setting_optional{};
        bool reboot_required{};
    };

    struct Compatibility {
        bool ok{};
        std::string_view owner{};
    };

    [[nodiscard]] static bool
    is_alternative(std::string_view id, std::span<const std::string_view> alternatives) noexcept {
        for (const auto alternative : alternatives) {
            if (alternative == id) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] Compatibility compatible(std::size_t resource_index, std::string_view owner,
                                           const core::ResourceRequest& request) const noexcept {
        const auto& resource = resources_[resource_index];
        if (!resource.reserved_for.empty() && resource.reserved_for != owner) {
            return {false, resource.reserved_for};
        }
        std::uint32_t used = 0;
        for (const auto& claim : claims_) {
            if (!claim.active || claim.resource_index != resource_index) {
                continue;
            }
            used += claim.amount;
            const bool ownership_compatible =
                (request.ownership == core::OwnershipMode::shared_read &&
                 claim.mode == core::OwnershipMode::shared_read) ||
                (request.ownership == core::OwnershipMode::bus_member &&
                 claim.mode == core::OwnershipMode::bus_member &&
                 request.member_key != claim.member_key) ||
                (request.ownership == core::OwnershipMode::multiplexed &&
                 claim.mode == core::OwnershipMode::multiplexed);
            const bool feature_conflict =
                (request.incompatible_features & claim.feature_mask) != 0 ||
                (claim.incompatible_features & request.feature_mask) != 0;
            if (!ownership_compatible || feature_conflict) {
                return {false, claim.owner};
            }
        }
        if (used > resource.capacity || request.amount > resource.capacity - used) {
            return {false, "capacity"};
        }
        return {true, {}};
    }

    [[nodiscard]] std::size_t free_claim() const noexcept {
        for (std::size_t index = 0; index < MaxLeases; ++index) {
            if (!claims_[index].active) {
                return index;
            }
        }
        return MaxLeases;
    }

    [[nodiscard]] std::size_t claim_for_owner(std::string_view owner) const noexcept {
        for (std::size_t index = 0; index < MaxLeases; ++index) {
            if (claims_[index].active && claims_[index].owner == owner) {
                return index;
            }
        }
        return MaxLeases;
    }

    [[nodiscard]] std::size_t resource_by_id(std::string_view id) const noexcept {
        for (std::size_t index = 0; index < resources_.size(); ++index) {
            if (resources_[index].id == id) {
                return index;
            }
        }
        return MaxResources;
    }

    [[nodiscard]] bool token_valid(std::size_t claim_index,
                                   std::uint32_t generation) const noexcept {
        return claim_index < MaxLeases && claims_[claim_index].active &&
               claims_[claim_index].generation == generation;
    }

    void release(std::size_t claim_index, std::uint32_t generation) noexcept {
        if (token_valid(claim_index, generation)) {
            claims_[claim_index].active = false;
            claims_[claim_index].owner = {};
            claims_[claim_index].role = {};
            claims_[claim_index].amount = 0;
            advance_revision();
        }
    }

    void advance_revision() noexcept {
        ++revision_;
        if (revision_ == 0U) {
            ++revision_;
        }
    }

    [[nodiscard]] static core::Status failure(core::ErrorCode code, std::string_view owner,
                                              std::string_view operation,
                                              std::string_view detail) noexcept {
        return core::Status::failure({core::ErrorDomain::resource, code, owner, operation, detail});
    }

    [[nodiscard]] static core::Result<Lease> lease_failure(core::ErrorCode code,
                                                           std::string_view owner,
                                                           std::string_view operation,
                                                           std::string_view detail) noexcept {
        return core::Result<Lease>::failure(
            {core::ErrorDomain::resource, code, owner, operation, detail});
    }

    [[nodiscard]] static core::Result<LeaseBatch> batch_failure(core::ErrorCode code,
                                                                std::string_view owner,
                                                                std::string_view operation,
                                                                std::string_view detail) noexcept {
        return core::Result<LeaseBatch>::failure(
            {core::ErrorDomain::resource, code, owner, operation, detail});
    }

    core::FixedVector<ResourceSpec, MaxResources> resources_{};
    std::array<Claim, MaxLeases> claims_{};
    bool inventory_closed_{};
    std::uint32_t revision_{1U};
};

} // namespace blip::resources
