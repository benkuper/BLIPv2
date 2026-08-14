#include "blip/transport/esp_serial_transport_component.hpp"

#include "driver/uart.h"
#include "esp_err.h"
#include "soc/soc_caps.h"

#if SOC_USB_SERIAL_JTAG_SUPPORTED
#include "driver/usb_serial_jtag.h"
#endif

#include <array>
#include <limits>
#include <string_view>

namespace blip::transport {
namespace {

constexpr std::array<std::string_view, 1> kProvidedServices{"transport.serial"};
constexpr std::array<std::string_view, 1> kRequiredServices{"control.dispatch"};
constexpr std::array<core::DiagnosticDescriptor, 4> kDiagnostics{{
    {"rx_frames", core::ValueType::integer, "frames"},
    {"tx_frames", core::ValueType::integer, "frames"},
    {"rejected_frames", core::ValueType::integer, "frames"},
    {"overflow_frames", core::ValueType::integer, "frames"},
}};

[[nodiscard]] constexpr core::ComponentDescriptor serial_descriptor() noexcept {
    core::ComponentDescriptor descriptor{};
    descriptor.schema_version = 1;
    descriptor.id = "blip.transport.serial";
    descriptor.display_name = "Serial/USB control transport";
    descriptor.description = "COBS-framed BLIP envelope over the console UART or USB Serial/JTAG";
    descriptor.provided_services = kProvidedServices;
    descriptor.required_services = kRequiredServices;
    descriptor.diagnostics = kDiagnostics;
    descriptor.settings = {1, 1};
    descriptor.disable_policy = core::DisablePolicy::reboot_required;
    descriptor.cost = {32768, 8192, EspSerialTransportComponent::kTaskStackBytes};
    return descriptor;
}

[[nodiscard]] core::Error serial_error(core::ErrorCode code, std::string_view operation,
                                       std::string_view detail) noexcept {
    return {core::ErrorDomain::transport, code, "blip.transport.serial", operation, detail};
}

void saturating_increment(std::atomic<std::uint32_t>& value) noexcept {
    std::uint32_t current = value.load();
    while (current != std::numeric_limits<std::uint32_t>::max() &&
           !value.compare_exchange_weak(current, current + 1U)) {
    }
}

} // namespace

const core::ComponentDescriptor EspSerialTransportComponent::descriptor_{serial_descriptor()};

EspSerialTransportComponent::EspSerialTransportComponent(core::ControlService& controls) noexcept
    : controls_(&controls), endpoint_(controls, decode_buffer_, envelope_buffer_, payload_buffer_) {
}

const core::ComponentDescriptor& EspSerialTransportComponent::descriptor() const noexcept {
    return descriptor_;
}

core::Status EspSerialTransportComponent::install_driver() noexcept {
#if SOC_USB_SERIAL_JTAG_SUPPORTED
    if (usb_serial_jtag_is_driver_installed()) {
        driver_owned_ = false;
        return core::Status::success();
    }
    usb_serial_jtag_driver_config_t configuration{
        .tx_buffer_size = 1024,
        .rx_buffer_size = 1024,
    };
    if (usb_serial_jtag_driver_install(&configuration) != ESP_OK) {
        return core::Status::failure(
            serial_error(core::ErrorCode::start_failed, "install-driver", "usb-jtag-failed"));
    }
#else
    if (uart_is_driver_installed(UART_NUM_0)) {
        driver_owned_ = false;
        return core::Status::success();
    }
    if (uart_driver_install(UART_NUM_0, 1024, 1024, 0, nullptr, 0) != ESP_OK) {
        return core::Status::failure(
            serial_error(core::ErrorCode::start_failed, "install-driver", "uart-failed"));
    }
    driver_owned_ = true;
    if (uart_set_baudrate(UART_NUM_0, 115200) != ESP_OK) {
        static_cast<void>(uart_driver_delete(UART_NUM_0));
        driver_owned_ = false;
        return core::Status::failure(
            serial_error(core::ErrorCode::start_failed, "install-driver", "uart-baud-failed"));
    }
#endif
    driver_owned_ = true;
    return core::Status::success();
}

core::Status EspSerialTransportComponent::uninstall_driver() noexcept {
    if (!driver_owned_) {
        return core::Status::success();
    }
#if SOC_USB_SERIAL_JTAG_SUPPORTED
    const esp_err_t result = usb_serial_jtag_driver_uninstall();
#else
    const esp_err_t result = uart_driver_delete(UART_NUM_0);
#endif
    driver_owned_ = false;
    return result == ESP_OK
               ? core::Status::success()
               : core::Status::failure(serial_error(core::ErrorCode::stop_failed,
                                                    "uninstall-driver", "driver-failed"));
}

core::Status EspSerialTransportComponent::start(const core::StartContext&) noexcept {
    if (running_.load()) {
        return core::Status::success();
    }
    auto status = install_driver();
    if (!status) {
        return status;
    }
    incoming_size_ = 0;
    discard_until_delimiter_ = false;
    control_enabled_.store(false);
    quiesced_.store(false);
    running_.store(true);
    task_handle_ = xTaskCreateStatic(task_entry, "blip_serial", task_stack_.size(), this, 5,
                                     task_stack_.data(), &task_storage_);
    if (task_handle_ == nullptr) {
        running_.store(false);
        quiesced_.store(true);
        static_cast<void>(uninstall_driver());
        return core::Status::failure(
            serial_error(core::ErrorCode::start_failed, "start", "task-create-failed"));
    }
    return core::Status::success();
}

core::Status EspSerialTransportComponent::stop() noexcept {
    if (!running_.exchange(false) && quiesced_.load()) {
        return uninstall_driver();
    }
    for (std::size_t attempt = 0; attempt < 50U && !quiesced_.load(); ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (!quiesced_.load()) {
        return core::Status::failure(
            serial_error(core::ErrorCode::stop_failed, "stop", "task-timeout"));
    }
    task_handle_ = nullptr;
    return uninstall_driver();
}

int EspSerialTransportComponent::read_bytes(std::span<std::byte> output) noexcept {
#if SOC_USB_SERIAL_JTAG_SUPPORTED
    return usb_serial_jtag_read_bytes(output.data(), output.size(), pdMS_TO_TICKS(20));
#else
    return uart_read_bytes(UART_NUM_0, output.data(), output.size(), pdMS_TO_TICKS(20));
#endif
}

bool EspSerialTransportComponent::write_bytes(std::span<const std::byte> input) noexcept {
    std::size_t offset{};
    while (offset < input.size() && running_.load()) {
#if SOC_USB_SERIAL_JTAG_SUPPORTED
        const int written = usb_serial_jtag_write_bytes(input.data() + offset,
                                                        input.size() - offset, pdMS_TO_TICKS(100));
#else
        const int written =
            uart_write_bytes(UART_NUM_0, input.data() + offset, input.size() - offset);
#endif
        if (written <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return offset == input.size();
}

void EspSerialTransportComponent::process_bytes(std::span<const std::byte> input) noexcept {
    for (const auto byte : input) {
        if (byte == std::byte{0}) {
            if (discard_until_delimiter_) {
                discard_until_delimiter_ = false;
                incoming_size_ = 0;
                continue;
            }
            if (incoming_size_ == 0U || !control_enabled_.load()) {
                incoming_size_ = 0;
                continue;
            }
            response_frame_[0] = std::byte{0};
            const auto response =
                endpoint_.handle_frame({incoming_frame_.data(), incoming_size_},
                                       std::span<std::byte>{response_frame_}.subspan(1U));
            incoming_size_ = 0;
            if (!response) {
                saturating_increment(rejected_frames_);
                continue;
            }
            saturating_increment(received_frames_);
            if (write_bytes({response_frame_.data(), response.value() + 1U})) {
                saturating_increment(transmitted_frames_);
            } else {
                saturating_increment(rejected_frames_);
            }
            continue;
        }
        if (discard_until_delimiter_) {
            continue;
        }
        if (incoming_size_ == incoming_frame_.size()) {
            incoming_size_ = 0;
            discard_until_delimiter_ = true;
            saturating_increment(overflow_frames_);
            continue;
        }
        incoming_frame_[incoming_size_++] = byte;
    }
}

void EspSerialTransportComponent::run() noexcept {
    std::array<std::byte, 128> input{};
    while (running_.load()) {
        const int count = read_bytes(input);
        if (count > 0) {
            process_bytes({input.data(), static_cast<std::size_t>(count)});
        }
    }
    quiesced_.store(true);
    vTaskDelete(nullptr);
}

void EspSerialTransportComponent::task_entry(void* context) noexcept {
    static_cast<EspSerialTransportComponent*>(context)->run();
}

} // namespace blip::transport
