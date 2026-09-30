#pragma once

#include "blip/led/engine.hpp"

namespace blip::led {

// Platform-neutral seam for ESP32 I2S/LCD, S3 LCD_CAM, C6 PARLIO, and the
// optional FastLED bridge. Platform ports own their interrupts and resources;
// this adapter guarantees that all of them expose exactly the output ABI.
class OutputPort {
  public:
    virtual ~OutputPort() = default;
    [[nodiscard]] virtual core::Status open(const OutputConfig&) noexcept = 0;
    [[nodiscard]] virtual core::Status write(const EncodedFrame&) noexcept = 0;
    [[nodiscard]] virtual Completion completion() noexcept = 0;
    [[nodiscard]] virtual core::Status close() noexcept = 0;
};

class PortOutputDriver final : public OutputDriver {
  public:
    PortOutputDriver(const DriverCapabilities& capabilities, OutputPort& port) noexcept
        : capabilities_(&capabilities), port_(&port) {}
    [[nodiscard]] const DriverCapabilities& capabilities() const noexcept override {
        return *capabilities_;
    }
    [[nodiscard]] core::Status start(const OutputConfig& config) noexcept override {
        const auto valid = validate_output_config(*capabilities_, config);
        return valid ? port_->open(config) : valid;
    }
    [[nodiscard]] core::Status submit(const EncodedFrame& frame) noexcept override {
        return port_->write(frame);
    }
    [[nodiscard]] Completion poll() noexcept override { return port_->completion(); }
    [[nodiscard]] core::Status stop() noexcept override { return port_->close(); }

  private:
    const DriverCapabilities* capabilities_{};
    OutputPort* port_{};
};

// Semantic aliases keep optional build composition explicit while retaining a
// single ABI implementation.
using ParallelWaveOutputDriver = PortOutputDriver;
using FastLedCompatibilityOutputDriver = PortOutputDriver;

} // namespace blip::led
