#pragma once

#include "blip/core/descriptor.hpp"
#include "blip/core/error.hpp"

namespace blip::core {

enum class ComponentState : std::uint8_t {
    absent,
    constructed,
    validated,
    starting,
    running,
    suspended,
    failed,
    stopping,
    stopped,
};

struct ValidationContext final {};
struct StartContext final {};

class Component {
  public:
    Component() = default;
    virtual ~Component() = default;
    Component(const Component&) = delete;
    Component& operator=(const Component&) = delete;
    Component(Component&&) = delete;
    Component& operator=(Component&&) = delete;

    [[nodiscard]] virtual const ComponentDescriptor& descriptor() const noexcept = 0;
    [[nodiscard]] virtual Status validate(const ValidationContext&) noexcept {
        return Status::success();
    }
    [[nodiscard]] virtual Status start(const StartContext&) noexcept = 0;
    [[nodiscard]] virtual Status suspend() noexcept { return Status::success(); }
    [[nodiscard]] virtual Status resume() noexcept { return Status::success(); }
    [[nodiscard]] virtual Status stop() noexcept = 0;
    [[nodiscard]] virtual bool callbacks_quiesced() const noexcept { return true; }
};

} // namespace blip::core
