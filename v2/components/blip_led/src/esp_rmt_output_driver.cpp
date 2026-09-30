#include "blip/led/esp_rmt_output_driver.hpp"

#include "esp_timer.h"
#include "soc/soc_caps.h"

#include <array>

namespace blip::led {
namespace {
constexpr std::array kProtocols{PixelProtocol::ws2812, PixelProtocol::sk6812};
constexpr std::array kFormats{PixelFormat::rgb8, PixelFormat::rgbw8};
constexpr std::array<std::string_view, 2> kResources{"rmt.tx", "gpio.output"};
constexpr DriverCapabilities kRmt{"esp-rmt",
                                  kProtocols,
                                  kFormats,
                                  1U,
                                  1U,
                                  1024U,
                                  0U,
                                  0U,
                                  80U,
                                  0U,
                                  0U,
                                  1U,
                                  BufferingMode::queued,
                                  DmaMode::none,
                                  SynchronizationMode::per_lane,
                                  false,
                                  "pending-Gate-C-capture",
                                  kResources};
constexpr DriverCapabilities kRmtDma{"esp-rmt-dma",
                                     kProtocols,
                                     kFormats,
                                     1U,
                                     1U,
                                     1024U,
                                     0U,
                                     0U,
                                     80U,
                                     0U,
                                     0U,
                                     4U,
                                     BufferingMode::queued,
                                     DmaMode::required,
                                     SynchronizationMode::per_lane,
                                     false,
                                     "pending-Gate-C-capture",
                                     kResources};
[[nodiscard]] core::Status fail(core::ErrorCode code, std::string_view op,
                                std::string_view why) noexcept {
    return core::Status::failure({core::ErrorDomain::transport, code, "blip.led.rmt", op, why});
}
} // namespace

const DriverCapabilities& EspRmtOutputDriver::capabilities() const noexcept {
    return use_dma_ ? kRmtDma : kRmt;
}

bool EspRmtOutputDriver::on_done(rmt_channel_handle_t, const rmt_tx_done_event_data_t*,
                                 void* context) noexcept {
    auto* self = static_cast<EspRmtOutputDriver*>(context);
    self->completion_pending_.store(true, std::memory_order_release);
    return false;
}

core::Status EspRmtOutputDriver::start(const OutputConfig& config) noexcept {
    if (channel_ != nullptr || !validate_output_config(capabilities(), config))
        return fail(core::ErrorCode::validation_failed, "start", "invalid-config-or-state");
    rmt_tx_channel_config_t channel_config{};
    channel_config.gpio_num = static_cast<gpio_num_t>(gpio_);
    channel_config.clk_src = RMT_CLK_SRC_DEFAULT;
    channel_config.resolution_hz = 10'000'000U;
    channel_config.mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL;
    channel_config.trans_queue_depth = 1U;
    channel_config.flags.with_dma = use_dma_;
    if (rmt_new_tx_channel(&channel_config, &channel_) != ESP_OK)
        return fail(core::ErrorCode::resource_unavailable, "start", "channel-allocation");
    const bool rgbw = config.protocol == PixelProtocol::sk6812;
    rmt_bytes_encoder_config_t encoder_config{};
    encoder_config.bit0.duration0 = rgbw ? 3U : 4U;
    encoder_config.bit0.level0 = 1U;
    encoder_config.bit0.duration1 = 9U;
    encoder_config.bit0.level1 = 0U;
    encoder_config.bit1.duration0 = rgbw ? 6U : 9U;
    encoder_config.bit1.level0 = 1U;
    encoder_config.bit1.duration1 = rgbw ? 6U : 4U;
    encoder_config.bit1.level1 = 0U;
    encoder_config.flags.msb_first = true;
    rmt_tx_event_callbacks_t callbacks{};
    callbacks.on_trans_done = &EspRmtOutputDriver::on_done;
    if (rmt_new_bytes_encoder(&encoder_config, &encoder_) != ESP_OK ||
        rmt_tx_register_event_callbacks(channel_, &callbacks, this) != ESP_OK ||
        rmt_enable(channel_) != ESP_OK) {
        static_cast<void>(stop());
        return fail(core::ErrorCode::start_failed, "start", "encoder-or-enable");
    }
    return core::Status::success();
}

core::Status EspRmtOutputDriver::submit(const EncodedFrame& frame) noexcept {
    if (channel_ == nullptr || encoder_ == nullptr || frame.bytes.empty())
        return fail(core::ErrorCode::invalid_state, "submit", "not-ready");
    if (in_flight_.exchange(true))
        return fail(core::ErrorCode::queue_full, "submit", "in-flight");
    active_sequence_.store(frame.sequence);
    rmt_transmit_config_t tx{};
    tx.loop_count = 0;
    tx.flags.eot_level = 0;
    if (rmt_transmit(channel_, encoder_, frame.bytes.data(), frame.bytes.size(), &tx) != ESP_OK) {
        active_sequence_.store(0U);
        in_flight_.store(false);
        return fail(core::ErrorCode::io_failed, "submit", "rmt-transmit");
    }
    return core::Status::success();
}

Completion EspRmtOutputDriver::poll() noexcept {
    if (!completion_pending_.exchange(false, std::memory_order_acquire))
        return {};
    const auto completed = active_sequence_.load();
    active_sequence_.store(0U);
    in_flight_.store(false);
    return {CompletionState::complete, completed, static_cast<std::uint64_t>(esp_timer_get_time())};
}

core::Status EspRmtOutputDriver::stop() noexcept {
    if (channel_ != nullptr) {
        static_cast<void>(rmt_disable(channel_));
        static_cast<void>(rmt_del_channel(channel_));
        channel_ = nullptr;
    }
    if (encoder_ != nullptr) {
        static_cast<void>(rmt_del_encoder(encoder_));
        encoder_ = nullptr;
    }
    active_sequence_.store(0U);
    in_flight_.store(false);
    completion_pending_.store(false);
    return core::Status::success();
}

} // namespace blip::led
