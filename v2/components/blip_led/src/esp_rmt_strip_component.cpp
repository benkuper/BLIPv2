#include "blip/led/esp_rmt_strip_component.hpp"
#include "blip/led/compositor.hpp"
#include "blip/led/protocol_encoder.hpp"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "soc/soc_caps.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>

#ifndef BLIP_LED_DEFAULT_GPIO
#if defined(CONFIG_IDF_TARGET_ESP32S3)
#define BLIP_LED_DEFAULT_GPIO 21
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
#define BLIP_LED_DEFAULT_GPIO 20
#else
#define BLIP_LED_DEFAULT_GPIO 15
#endif
#endif

namespace blip::led {
namespace {

constexpr char kTag[] = "blip_rmt_strip";
#if defined(BLIP_BOARD_CREATORS_BALL_V2)
constexpr bool kClockedBoard = true;
constexpr std::uint8_t kDefaultProtocol = 2U;
constexpr std::uint16_t kDefaultPixels = 36U;
constexpr std::array<std::string_view, 1> kClockAlternatives{"gpio.2"};
constexpr std::array<std::string_view, 1> kSpiAlternatives{"spi2"};
constexpr std::array<core::ResourceRequest, 2> kResources{{
    {core::ResourceClass::gpio, "clock", core::OwnershipMode::exclusive,
     kClockAlternatives, resources::kGpioOutput, 1U},
    {core::ResourceClass::spi, "spi", core::OwnershipMode::exclusive,
     kSpiAlternatives, 0U, 1U},
}};
#else
constexpr bool kClockedBoard = false;
constexpr std::uint8_t kDefaultProtocol = 0U;
constexpr std::uint16_t kDefaultPixels = 1U;
#endif
#if defined(BLIP_BOARD_CREATORS_TAB)
constexpr gpio_num_t kLedPowerGpio = GPIO_NUM_27;
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
constexpr gpio_num_t kLedPowerGpio = GPIO_NUM_38;
#endif
constexpr std::string_view kPinOwner{"blip.output.strip0:pin"};
#if defined(BLIP_BOARD_CREATORS_BALL_V2)
constexpr std::string_view kClockOwner{"blip.output.strip0:clock"};
constexpr std::string_view kSpiOwner{"blip.output.strip0:spi"};
#endif
constexpr std::array<std::string_view, 1> kProvidedServices{"output.pixel-strip"};
constexpr std::array<std::string_view, 2> kRequiredServices{"storage.settings", "power.cpu"};
#if !defined(BLIP_BOARD_CREATORS_BALL_V2)
constexpr std::array<std::string_view, 4> kRmtAlternatives{"rmt.tx0", "rmt.tx1", "rmt.tx2",
                                                           "rmt.tx3"};
constexpr std::array<core::ResourceRequest, 1> kResources{{
    {core::ResourceClass::rmt, "waveform", core::OwnershipMode::exclusive, kRmtAlternatives, 0U, 1U,
     0U, 0x02U, 0U, false},
}};
#endif
constexpr std::array<core::MetadataEntry, 8> kMetadata{{
    {"backend", kClockedBoard ? "esp-spi-dma" : "native-rmt"},
    {"qualification", "ADR-0007"},
    {"lane_count", "1"},
    {"protocol_values", kClockedBoard ? "2=hd108-rgb16" : "0=ws2812-grb,1=sk6812-grbw"},
    {"gpio_parameter", "pin"},
    {"maximum_pixels", kClockedBoard ? "36" : "1024"},
    {"transport_dma", kClockedBoard ? "spi-required" : "rmt-disabled-m3-baseline"},
    {"current_model", "1mA/pixel idle; 20mA/full channel; calculated only"},
}};
constexpr std::array<core::ParameterDescriptor, 17> kParameters{{
    {"enabled",
     "Strip enabled",
     core::ValueType::boolean,
     core::Access::read_write,
     true,
     core::ScalarValue::from_bool(false),
     {},
     ""},
    {"pin",
     "Data GPIO",
     core::ValueType::integer,
     kClockedBoard ? core::Access::read_only : core::Access::read_write,
     true,
     core::ScalarValue::from_integer(BLIP_LED_DEFAULT_GPIO),
     {true, 0, 63, 1},
     "",
     {!kClockedBoard, core::ResourceClass::gpio, kClockedBoard ? 0x00000002U : 0x00000042U,
      false, !kClockedBoard, !kClockedBoard}},
    {"protocol",
     "Pixel protocol",
     core::ValueType::integer,
     kClockedBoard ? core::Access::read_only : core::Access::read_write,
     true,
     core::ScalarValue::from_integer(kDefaultProtocol),
     {true, kClockedBoard ? 2 : 0, kClockedBoard ? 2 : 1, 1},
     ""},
    {"pixels",
     "Pixel count",
     core::ValueType::integer,
     kClockedBoard ? core::Access::read_only : core::Access::read_write,
     true,
     core::ScalarValue::from_integer(kDefaultPixels),
     {true, kClockedBoard ? 36 : 1, kClockedBoard ? 36 : kMaximumStripPixels, 1},
     "pixels"},
    {"brightness",
     "Global brightness",
     core::ValueType::integer,
     core::Access::read_write,
     true,
     core::ScalarValue::from_integer(255),
     {true, 0, 255, 1},
     ""},
    {"power_budget_ma",
     "Calculated LED current budget",
     core::ValueType::integer,
     core::Access::read_write,
     true,
     core::ScalarValue::from_integer(1500),
     {true, 1, kHardPowerBudgetMa, 1},
     "mA"},
    {"red",
     "Red",
     core::ValueType::integer,
     core::Access::read_write,
     true,
     core::ScalarValue::from_integer(0),
     {true, 0, 255, 1},
     ""},
    {"green",
     "Green",
     core::ValueType::integer,
     core::Access::read_write,
     true,
     core::ScalarValue::from_integer(0),
     {true, 0, 255, 1},
     ""},
    {"blue",
     "Blue",
     core::ValueType::integer,
     core::Access::read_write,
     true,
     core::ScalarValue::from_integer(0),
     {true, 0, 255, 1},
     ""},
    {"white",
     "White",
     core::ValueType::integer,
     core::Access::read_write,
     true,
     core::ScalarValue::from_integer(0),
     {true, 0, 255, 1},
     ""},
    {"applied_frames",
     "Applied frames",
     core::ValueType::integer,
     core::Access::read_only,
     false,
     core::ScalarValue::from_integer(0),
     {},
     "frames"},
    {"failed_frames",
     "Failed frames",
     core::ValueType::integer,
     core::Access::read_only,
     false,
     core::ScalarValue::from_integer(0),
     {},
     "frames"},
    {"last_frame_us",
     "Last frame completion",
     core::ValueType::integer,
     core::Access::read_only,
     false,
     core::ScalarValue::from_integer(0),
     {},
     "us"},
    {"estimated_current_ma",
     "Estimated LED current after limiting",
     core::ValueType::integer,
     core::Access::read_only,
     false,
     core::ScalarValue::from_integer(0),
     {},
     "mA"},
    {"power_scale_q16",
     "Applied power scale",
     core::ValueType::integer,
     core::Access::read_only,
     false,
     core::ScalarValue::from_integer(65535),
     {true, 0, 65535, 1},
     ""},
    {"worker_stack_headroom",
     "Output worker stack headroom",
     core::ValueType::integer,
     core::Access::read_only,
     false,
     core::ScalarValue::from_integer(0),
     {true, 0, EspRmtStripComponent::kTaskStackBytes, 1},
     "bytes"},
    {"backend",
     "Selected backend",
     core::ValueType::string,
     core::Access::read_only,
     false,
     core::ScalarValue::from_string(kClockedBoard ? "esp-spi-dma" : "native-rmt"),
     {},
     ""},
}};
#if defined(BLIP_BOARD_CREATORS_BALL_V2)
constexpr std::array<core::FieldDescriptor, 4> kStreamPixelArguments{{
    {"index", core::ValueType::integer, true},
    {"red", core::ValueType::integer, true},
    {"green", core::ValueType::integer, true},
    {"blue", core::ValueType::integer, true},
}};
constexpr std::array<core::ActionDescriptor, 2> kActions{{
    {"blackout", "Set all channels to zero", {}},
    {"stream_pixel", "Set one stream-layer pixel", kStreamPixelArguments},
}};
#else
constexpr std::array<core::ActionDescriptor, 1> kActions{{
    {"blackout", "Set all channels to zero", {}},
}};
#endif
constexpr std::array<core::DiagnosticDescriptor, 5> kDiagnostics{{
    {"applied_frames", core::ValueType::integer, "frames"},
    {"failed_frames", core::ValueType::integer, "frames"},
    {"last_frame_us", core::ValueType::integer, "us"},
    {"estimated_current_ma", core::ValueType::integer, "mA"},
    {"power_scale_q16", core::ValueType::integer, ""},
}};

[[nodiscard]] constexpr core::ComponentDescriptor strip_descriptor() noexcept {
    core::ComponentDescriptor descriptor{};
    descriptor.schema_version = 2U;
    descriptor.id = "blip.output.strip0";
    descriptor.display_name = "Pixel strip 0";
    descriptor.description = kClockedBoard ? "Onboard 36-pixel HD108 SPI-DMA output"
                                            : "Single-lane WS2812/SK6812 native RMT output";
    descriptor.metadata = kMetadata;
    descriptor.provided_services = kProvidedServices;
    descriptor.required_services = kRequiredServices;
    descriptor.parameters = kParameters;
    descriptor.actions = kActions;
    descriptor.diagnostics = kDiagnostics;
    descriptor.resources = kResources;
    descriptor.settings = {2U, 2U};
    descriptor.disable_policy = core::DisablePolicy::live;
    descriptor.supports_restart = true;
    descriptor.cost = {32768U, kClockedBoard ? 12288U : 24576U,
                       EspRmtStripComponent::kTaskStackBytes};
    return descriptor;
}

[[nodiscard]] core::Error output_error(core::ErrorCode code, std::string_view operation,
                                       std::string_view detail) noexcept {
    return {core::ErrorDomain::transport, code, "blip.output.strip0", operation, detail};
}

void saturating_increment(std::atomic<std::uint32_t>& value) noexcept {
    std::uint32_t current = value.load();
    while (current != std::numeric_limits<std::uint32_t>::max() &&
           !value.compare_exchange_weak(current, current + 1U)) {
    }
}

} // namespace

const core::ComponentDescriptor EspRmtStripComponent::descriptor_{strip_descriptor()};

EspRmtStripComponent::EspRmtStripComponent(storage::SettingsStore& settings,
                                           resources::DeviceBroker& resources,
                                           pm::EspPowerManagerComponent& power_manager) noexcept
    : settings_(&settings), resources_(&resources), power_manager_(&power_manager) {}

const core::ComponentDescriptor& EspRmtStripComponent::descriptor() const noexcept {
    return descriptor_;
}

bool EspRmtStripComponent::lock_config() noexcept {
    return config_mutex_ != nullptr && xSemaphoreTake(config_mutex_, portMAX_DELAY) == pdTRUE;
}

void EspRmtStripComponent::unlock_config() noexcept {
    if (config_mutex_ != nullptr) {
        static_cast<void>(xSemaphoreGive(config_mutex_));
    }
}

core::Status EspRmtStripComponent::load_config() noexcept {
    const auto loaded = settings_->load(descriptor_, settings_buffer_);
    if (!loaded) {
        if (loaded.error().code == core::ErrorCode::not_found) {
            config_ = {};
            config_.gpio = BLIP_LED_DEFAULT_GPIO;
#if defined(BLIP_BOARD_CREATORS_BALL_V2)
            config_.protocol = StripProtocol::hd108_rgb;
            config_.pixel_count = kDefaultPixels;
#endif
            return validate_strip_config(config_);
        }
        return core::Status::failure(loaded.error());
    }
    const auto decoded = decode_strip_config(
        std::span<const std::byte>{settings_buffer_.data(), loaded.value().payload_size});
    if (!decoded) {
        return core::Status::failure(decoded.error());
    }
    config_ = decoded.value();
#if defined(BLIP_BOARD_CREATORS_BALL_V2)
    // Earlier Ball images had a disabled RMT placeholder. Replace its
    // transport settings with the fixed onboard clocked chain.
    if (config_.gpio != BLIP_LED_DEFAULT_GPIO ||
        config_.protocol != StripProtocol::hd108_rgb ||
        config_.pixel_count != kDefaultPixels) {
        config_.gpio = BLIP_LED_DEFAULT_GPIO;
        config_.protocol = StripProtocol::hd108_rgb;
        config_.pixel_count = kDefaultPixels;
        config_.enabled = false;
        ESP_LOGW(kTag, "migrated strip settings to onboard HD108 SPI output");
        return save_config(config_);
    }
#elif defined(BLIP_BOARD_CREATORS_TAB)
    // Generic ESP32 images used GPIO23, which is the Tab's IMU SDA pin.
    if (config_.gpio != BLIP_LED_DEFAULT_GPIO) {
        config_.gpio = BLIP_LED_DEFAULT_GPIO;
        config_.enabled = false;
        ESP_LOGW(kTag, "migrated strip settings to Creators Tab GPIO25");
        return save_config(config_);
    }
#elif defined(BLIP_BOARD_M5STICKC)
    // Generic ESP32 images used GPIO23, the M5StickC display DC pin.
    if (config_.gpio == 23U) {
        config_.gpio = BLIP_LED_DEFAULT_GPIO;
        config_.enabled = false;
        ESP_LOGW(kTag, "migrated strip settings from display GPIO23 to HAT GPIO26");
        return save_config(config_);
    }
#elif defined(CONFIG_IDF_TARGET_ESP32)
    // The earlier generic ESP32 image defaulted to GPIO23, which is absent
    // from the HUZZAH32 pin inventory. Migrate that default to the wired strip.
    if (config_.gpio == 23U) {
        config_.gpio = BLIP_LED_DEFAULT_GPIO;
        config_.enabled = false;
        ESP_LOGW(kTag, "migrated HUZZAH32 strip settings to GPIO15");
        return save_config(config_);
    }
#endif
    if (!kClockedBoard && config_.protocol == StripProtocol::hd108_rgb) {
        return core::Status::failure(output_error(core::ErrorCode::validation_failed,
                                                  "load", "clocked-backend-required"));
    }
    return core::Status::success();
}

core::Status EspRmtStripComponent::save_config(const StripConfig& config) noexcept {
    const auto encoded = encode_strip_config(config, settings_buffer_);
    if (!encoded) {
        return core::Status::failure(encoded.error());
    }
    return settings_->save(descriptor_,
                           std::span<const std::byte>{settings_buffer_.data(), encoded.value()});
}

core::Result<resources::DeviceBroker::Lease>
EspRmtStripComponent::reserve_pin(std::uint8_t gpio) noexcept {
    std::array<char, 12> id{};
    const int size = std::snprintf(id.data(), id.size(), "gpio.%u", static_cast<unsigned>(gpio));
    if (size <= 0 || static_cast<std::size_t>(size) >= id.size()) {
        return core::Result<resources::DeviceBroker::Lease>::failure(
            output_error(core::ErrorCode::invalid_argument, "reserve-pin", "gpio-id"));
    }
    const std::string_view alternative{id.data(), static_cast<std::size_t>(size)};
    const std::array<std::string_view, 1> alternatives{alternative};
    core::ResourceRequest request{};
    request.resource_class = core::ResourceClass::gpio;
    request.logical_name = "LED strip data";
    request.ownership = core::OwnershipMode::exclusive;
    request.alternatives = alternatives;
    request.required_capabilities = resources::kGpioOutput |
                                    (kClockedBoard ? 0U : resources::kGpioRmt);
    request.live_reacquire = true;
    return resources_->acquire(kPinOwner, request);
}

core::Status EspRmtStripComponent::initialize_output(const StripConfig& config) noexcept {
#if defined(BLIP_BOARD_CREATORS_BALL_V2)
    if (config.gpio != 3U || config.protocol != StripProtocol::hd108_rgb ||
        config.pixel_count != kDefaultPixels) {
        return core::Status::failure(output_error(core::ErrorCode::validation_failed,
                                                  "initialize", "fixed-board-layout"));
    }
    const OutputConfig output{PixelProtocol::hd108, PixelFormat::rgb16, 1U,
                              kDefaultPixels, 4'000'000U};
    auto clock = resources_->acquire(kClockOwner, kResources[0]);
    if (!clock) {
        return core::Status::failure(clock.error());
    }
    auto spi = resources_->acquire(kSpiOwner, kResources[1]);
    if (!spi) {
        return core::Status::failure(spi.error());
    }
    const auto started = spi_driver_.start(output);
    if (!started) {
        return started;
    }
    if (gpio_set_level(GPIO_NUM_21, 1) != ESP_OK) {
        static_cast<void>(spi_driver_.stop());
        return core::Status::failure(output_error(core::ErrorCode::start_failed,
                                                  "initialize", "led-power-enable"));
    }
    clock_lease_ = std::move(clock.value());
    spi_lease_ = std::move(spi.value());
    return core::Status::success();
#else
    if (channel_ != nullptr || encoder_ != nullptr) {
        return core::Status::failure(
            output_error(core::ErrorCode::invalid_state, "initialize", "already-initialized"));
    }
    rmt_tx_channel_config_t channel_config{};
    channel_config.gpio_num = static_cast<gpio_num_t>(config.gpio);
    channel_config.clk_src = RMT_CLK_SRC_DEFAULT;
    channel_config.resolution_hz = 10'000'000U;
    channel_config.mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL;
    channel_config.trans_queue_depth = 2U;
    channel_config.flags.with_dma = false;
    if (rmt_new_tx_channel(&channel_config, &channel_) != ESP_OK) {
        channel_ = nullptr;
        return core::Status::failure(
            output_error(core::ErrorCode::resource_unavailable, "initialize", "rmt-channel"));
    }
    const auto timing = strip_timing(config.protocol);
    rmt_bytes_encoder_config_t encoder_config{};
    encoder_config.bit0.duration0 = timing.zero_high_ticks;
    encoder_config.bit0.level0 = 1U;
    encoder_config.bit0.duration1 = timing.zero_low_ticks;
    encoder_config.bit0.level1 = 0U;
    encoder_config.bit1.duration0 = timing.one_high_ticks;
    encoder_config.bit1.level0 = 1U;
    encoder_config.bit1.duration1 = timing.one_low_ticks;
    encoder_config.bit1.level1 = 0U;
    encoder_config.flags.msb_first = true;
    if (rmt_new_bytes_encoder(&encoder_config, &encoder_) != ESP_OK ||
        rmt_enable(channel_) != ESP_OK) {
        if (encoder_ != nullptr) {
            static_cast<void>(rmt_del_encoder(encoder_));
            encoder_ = nullptr;
        }
        static_cast<void>(rmt_del_channel(channel_));
        channel_ = nullptr;
        return core::Status::failure(
            output_error(core::ErrorCode::start_failed, "initialize", "rmt-encoder"));
    }
#if defined(BLIP_BOARD_CREATORS_TAB) || defined(CONFIG_IDF_TARGET_ESP32S3)
    if (gpio_set_level(kLedPowerGpio, 1) != ESP_OK) {
        deinitialize_output();
        return core::Status::failure(
            output_error(core::ErrorCode::start_failed, "initialize", "led-power-enable"));
    }
#endif
    return core::Status::success();
#endif
}

void EspRmtStripComponent::deinitialize_output() noexcept {
#if defined(BLIP_BOARD_CREATORS_BALL_V2)
    static_cast<void>(gpio_set_level(GPIO_NUM_21, 0));
    static_cast<void>(spi_driver_.stop());
    clock_lease_.release();
    spi_lease_.release();
#else
    if (channel_ != nullptr) {
        static_cast<void>(rmt_tx_wait_all_done(channel_, 100));
        static_cast<void>(rmt_disable(channel_));
        static_cast<void>(rmt_del_channel(channel_));
        channel_ = nullptr;
    }
    if (encoder_ != nullptr) {
        static_cast<void>(rmt_del_encoder(encoder_));
        encoder_ = nullptr;
    }
#if defined(BLIP_BOARD_CREATORS_TAB) || defined(CONFIG_IDF_TARGET_ESP32S3)
    static_cast<void>(gpio_set_level(kLedPowerGpio, 0));
#endif
#endif
}

#if !defined(BLIP_BOARD_CREATORS_BALL_V2)
core::Result<std::size_t>
EspRmtStripComponent::render_one_wire(const StripConfig& config) noexcept {
    if (stream_mutex_ == nullptr || xSemaphoreTake(stream_mutex_, portMAX_DELAY) != pdTRUE) {
        return core::Result<std::size_t>::failure(
            output_error(core::ErrorCode::resource_unavailable, "render", "stream-busy"));
    }
    const auto release = [this]() { static_cast<void>(xSemaphoreGive(stream_mutex_)); };
    const bool stream_active = stream_layer_.active();
    const bool system_active = config.red != 0U || config.green != 0U ||
                               config.blue != 0U || config.white != 0U;
    const auto rgb_channel = [white = config.white](std::uint8_t value) {
        return static_cast<std::uint16_t>(
            std::min<std::uint16_t>(255U, static_cast<std::uint16_t>(value) + white) * 257U);
    };
    const bool rgbw = config.protocol == StripProtocol::sk6812_rgbw;
    const LinearPixel system_pixel{
        rgbw ? static_cast<std::uint16_t>(config.red * 257U) : rgb_channel(config.red),
        rgbw ? static_cast<std::uint16_t>(config.green * 257U) : rgb_channel(config.green),
        rgbw ? static_cast<std::uint16_t>(config.blue * 257U) : rgb_channel(config.blue),
        rgbw ? static_cast<std::uint16_t>(config.white * 257U) : std::uint16_t{0U},
        65535U};
    ColorTransform color{};
    color.transfer = TransferFunction::linear;
    color.brightness = static_cast<std::uint16_t>(config.brightness * 257U);
    color.order = {ColorChannel::green, ColorChannel::red, ColorChannel::blue,
                   ColorChannel::white};
    const auto format = rgbw ? PixelFormat::rgbw8 : PixelFormat::rgb8;
    const std::size_t bytes_per_pixel = rgbw ? 4U : 3U;
    auto buffer = std::span<std::byte>{reinterpret_cast<std::byte*>(pixel_buffer_.data()),
                                       pixel_buffer_.size()};
    std::array<LinearPixel, 1> rendered{};
    for (std::size_t index = 0U; index < config.pixel_count; ++index) {
        Compositor compositor{};
        if (stream_active) {
            const auto layer = compositor.set_layer(
                {LayerId::stream, std::span<const LinearPixel>{&stream_pixels_[index], 1U},
                 BlendMode::replace, 65535U, true});
            if (!layer) {
                release();
                return core::Result<std::size_t>::failure(layer.error());
            }
        }
        const auto layer = compositor.set_layer(
            {LayerId::system, std::span<const LinearPixel>{&system_pixel, 1U},
             BlendMode::replace, 65535U, system_active});
        if (!layer) {
            release();
            return core::Result<std::size_t>::failure(layer.error());
        }
        const auto composed = compositor.compose(rendered);
        if (!composed) {
            release();
            return core::Result<std::size_t>::failure(composed.error());
        }
        const auto encoded = encode_color(rendered[0], format, color,
                                          buffer.subspan(index * bytes_per_pixel,
                                                         bytes_per_pixel));
        if (!encoded) {
            release();
            return encoded;
        }
    }
    release();
    return core::Result<std::size_t>::success(config.pixel_count * bytes_per_pixel);
}
#endif

core::Status EspRmtStripComponent::transmit(const StripConfig& config,
                                            std::size_t payload_size) noexcept {
#if defined(BLIP_BOARD_CREATORS_BALL_V2)
    static_cast<void>(payload_size);
    std::array<LinearPixel, kDefaultPixels> system_pixels{};
    std::array<LinearPixel, kDefaultPixels> pixels{};
    const auto channel = [white = config.white](std::uint8_t value) {
        const auto added = std::min<std::uint16_t>(255U,
                                                   static_cast<std::uint16_t>(value) + white);
        return static_cast<std::uint16_t>(added * 257U);
    };
    const LinearPixel color{channel(config.red), channel(config.green),
                            channel(config.blue), 0U, 65535U};
    system_pixels.fill(color);
    std::array<LinearPixel, kDefaultPixels> stream_pixels{};
    bool stream_active{};
    if (stream_mutex_ != nullptr && xSemaphoreTake(stream_mutex_, portMAX_DELAY) == pdTRUE) {
        stream_active = stream_layer_.active();
        if (stream_active) {
            std::copy(stream_layer_.pixels().begin(), stream_layer_.pixels().end(),
                      stream_pixels.begin());
        }
        static_cast<void>(xSemaphoreGive(stream_mutex_));
    }
    Compositor compositor{};
    if (stream_active) {
        const auto layer = compositor.set_layer(
            {LayerId::stream, stream_pixels, BlendMode::replace, 65535U, true});
        if (!layer) {
            return layer;
        }
    }
    const auto layer = compositor.set_layer(
        {LayerId::system, system_pixels, BlendMode::replace, 65535U,
         config.red != 0U || config.green != 0U || config.blue != 0U || config.white != 0U});
    if (!layer) {
        return layer;
    }
    const auto composed = compositor.compose(pixels);
    if (!composed) {
        return composed;
    }
    EncoderOptions options{};
    options.color.transfer = TransferFunction::linear;
    options.color.brightness = static_cast<std::uint16_t>(config.brightness * 257U);
    auto buffer = std::span<std::byte>{reinterpret_cast<std::byte*>(pixel_buffer_.data()),
                                       pixel_buffer_.size()};
    const auto encoded = encode_frame({pixels, 1U, kDefaultPixels}, PixelProtocol::hd108,
                                      options, buffer);
    if (!encoded) {
        return core::Status::failure(encoded.error());
    }
    const auto power = limit_encoded(config, buffer.first(encoded.value()));
    if (!power) {
        return power;
    }
    const auto started_at = esp_timer_get_time();
    const auto queued = spi_driver_.submit({buffer.first(encoded.value()),
                                            applied_frames_.load() + failed_frames_.load() + 1U,
                                            static_cast<std::uint64_t>(started_at + 100'000)});
    if (!queued) {
        return queued;
    }
    const auto deadline_us = started_at + 100'000;
    while (esp_timer_get_time() < deadline_us) {
        const auto completion = spi_driver_.poll();
        if (completion.state == CompletionState::complete) {
            last_frame_us_.store(static_cast<std::uint32_t>(esp_timer_get_time() - started_at));
            return core::Status::success();
        }
        if (completion.state == CompletionState::failed) {
            return core::Status::failure(output_error(core::ErrorCode::io_failed,
                                                      "transmit", "spi-failed"));
        }
        vTaskDelay(1);
    }
    return core::Status::failure(output_error(core::ErrorCode::io_failed,
                                              "transmit", "completion-timeout"));
#else
    if (channel_ == nullptr || encoder_ == nullptr) {
        return core::Status::failure(
            output_error(core::ErrorCode::invalid_state, "transmit", "output-not-initialized"));
    }
    const auto power = limit_encoded(
        config, std::span<std::byte>{reinterpret_cast<std::byte*>(pixel_buffer_.data()),
                                     payload_size});
    if (!power) {
        return power;
    }
    rmt_transmit_config_t transmit_config{};
    transmit_config.flags.eot_level = 0U;
    const auto started_at = esp_timer_get_time();
    if (rmt_transmit(channel_, encoder_, pixel_buffer_.data(), payload_size, &transmit_config) !=
        ESP_OK) {
        return core::Status::failure(
            output_error(core::ErrorCode::io_failed, "transmit", "submit-failed"));
    }
    const int timeout_ms = static_cast<int>(config.pixel_count / 20U + 100U);
    if (rmt_tx_wait_all_done(channel_, timeout_ms) != ESP_OK) {
        return core::Status::failure(
            output_error(core::ErrorCode::io_failed, "transmit", "completion-timeout"));
    }
    esp_rom_delay_us(strip_timing(config.protocol).reset_us);
    last_frame_us_.store(static_cast<std::uint32_t>(esp_timer_get_time() - started_at));
    return core::Status::success();
#endif
}

core::Status EspRmtStripComponent::limit_encoded(const StripConfig& config,
                                                  std::span<std::byte> frame) noexcept {
    const auto protocol = config.protocol == StripProtocol::hd108_rgb
                              ? PixelProtocol::hd108
                              : config.protocol == StripProtocol::sk6812_rgbw
                                    ? PixelProtocol::sk6812
                                    : PixelProtocol::ws2812;
    const auto result = current_limiter_.limit(frame, protocol, config.pixel_count,
                                                config.power_budget_ma);
    if (!result) {
        return core::Status::failure(result.error());
    }
    estimated_current_ma_.store(result.value().estimated_after_ma);
    power_scale_q16_.store(result.value().applied_scale_q16);
    return core::Status::success();
}

core::Status EspRmtStripComponent::start(const core::StartContext&) noexcept {
    if (started_.load()) {
        return core::Status::success();
    }
    config_mutex_ = xSemaphoreCreateMutexStatic(&config_mutex_storage_);
    request_mutex_ = xSemaphoreCreateMutexStatic(&request_mutex_storage_);
    completion_ = xSemaphoreCreateBinaryStatic(&completion_storage_);
    stream_mutex_ = xSemaphoreCreateMutexStatic(&stream_mutex_storage_);
    if (config_mutex_ == nullptr || request_mutex_ == nullptr || completion_ == nullptr ||
        stream_mutex_ == nullptr) {
        return core::Status::failure(
            output_error(core::ErrorCode::start_failed, "start", "mutex-create-failed"));
    }
    stream_layer_.clear();
    current_limiter_.reset();
    estimated_current_ma_.store(0U);
    power_scale_q16_.store(65535U);
    auto status = load_config();
    if (!status) {
        config_mutex_ = nullptr;
        request_mutex_ = nullptr;
        completion_ = nullptr;
        return status;
    }
    auto reserved_pin = reserve_pin(config_.gpio);
    if (!reserved_pin) {
        config_mutex_ = nullptr;
        request_mutex_ = nullptr;
        completion_ = nullptr;
        return core::Status::failure(reserved_pin.error());
    }
    pin_lease_ = std::move(reserved_pin.value());
    if (config_.enabled) {
        status = initialize_output(config_);
        if (!status) {
            config_mutex_ = nullptr;
            request_mutex_ = nullptr;
            completion_ = nullptr;
            pin_lease_.release();
            return status;
        }
    }
    started_.store(true);
    task_quiesced_.store(false);
    task_ = xTaskCreateStatic(task_entry, "blip_led", task_stack_.size(), this, 5,
                              task_stack_.data(), &task_storage_);
    if (task_ == nullptr) {
        started_.store(false);
        task_quiesced_.store(true);
        deinitialize_output();
        pin_lease_.release();
        config_mutex_ = nullptr;
        request_mutex_ = nullptr;
        completion_ = nullptr;
        return core::Status::failure(
            output_error(core::ErrorCode::start_failed, "start", "task-create-failed"));
    }
    xTaskNotifyGive(task_);
    ESP_LOGI(kTag, "ready pin=%u protocol=%u pixels=%u enabled=%u",
             static_cast<unsigned>(config_.gpio), static_cast<unsigned>(config_.protocol),
             static_cast<unsigned>(config_.pixel_count), config_.enabled ? 1U : 0U);
    return core::Status::success();
}

core::Status EspRmtStripComponent::stop() noexcept {
    if (!started_.exchange(false) && task_quiesced_.load()) {
        return core::Status::success();
    }
    if (task_ != nullptr) {
        xTaskNotifyGive(task_);
    }
    for (std::size_t attempt = 0; attempt < 200U && !task_quiesced_.load(); ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    task_ = nullptr;
    deinitialize_output();
    pin_lease_.release();
    config_mutex_ = nullptr;
    request_mutex_ = nullptr;
    completion_ = nullptr;
    return task_quiesced_.load() ? core::Status::success()
                                 : core::Status::failure(output_error(core::ErrorCode::stop_failed,
                                                                      "stop", "task-active"));
}

bool EspRmtStripComponent::callbacks_quiesced() const noexcept { return task_quiesced_.load(); }

core::Status EspRmtStripComponent::apply_config_locked(const StripConfig& candidate) noexcept {
    const auto valid = validate_strip_config(candidate);
    if (!valid) {
        return valid;
    }
#if defined(BLIP_BOARD_CREATORS_BALL_V2)
    if (candidate.gpio != 3U || candidate.protocol != StripProtocol::hd108_rgb ||
        candidate.pixel_count != kDefaultPixels) {
        return core::Status::failure(output_error(core::ErrorCode::validation_failed,
                                                  "configure", "fixed-board-layout"));
    }
#else
    if (candidate.protocol == StripProtocol::hd108_rgb) {
        return core::Status::failure(output_error(core::ErrorCode::validation_failed,
                                                  "configure", "clocked-backend-required"));
    }
#endif
    while (completion_ != nullptr && xSemaphoreTake(completion_, 0) == pdTRUE) {
    }
    if (!started_.load() || !lock_config()) {
        return core::Status::failure(
            output_error(core::ErrorCode::invalid_state, "configure", "not-started"));
    }
    const StripConfig previous = config_;
    if (candidate == previous) {
        unlock_config();
        return core::Status::success();
    }
    const bool transport_changed =
        candidate.gpio != previous.gpio || candidate.protocol != previous.protocol;
    if (transport_changed && previous.enabled && candidate.enabled) {
        unlock_config();
        return core::Status::failure(output_error(core::ErrorCode::invalid_state, "configure",
                                                  "disable-before-pin-or-protocol-change"));
    }
    pending_config_ = candidate;
    ++next_update_id_;
    if (next_update_id_ == 0U) {
        ++next_update_id_;
    }
    pending_update_id_ = next_update_id_;
    const std::uint32_t update_id = pending_update_id_;
    reconfigure_pending_ = true;
    unlock_config();
    if (task_ != nullptr) {
        xTaskNotifyGive(task_);
    }
    core::Status status = core::Status::failure(
        output_error(core::ErrorCode::io_failed, "configure", "commit-timeout"));
    const TickType_t started_at = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(10000);
    bool complete{};
    while (!complete && completion_ != nullptr) {
        const TickType_t now = xTaskGetTickCount();
        const TickType_t elapsed = now - started_at;
        if (elapsed >= timeout || xSemaphoreTake(completion_, timeout - elapsed) != pdTRUE) {
            break;
        }
        if (lock_config()) {
            complete = completed_update_id_ == update_id;
            if (complete) {
                status = completed_update_status_;
            }
            unlock_config();
        }
    }
    return status;
}

void EspRmtStripComponent::process_pending_config() noexcept {
    StripConfig candidate{};
    StripConfig previous{};
    std::uint32_t update_id{};
    if (!lock_config()) {
        return;
    }
    if (!reconfigure_pending_) {
        unlock_config();
        return;
    }
    candidate = pending_config_;
    previous = config_;
    update_id = pending_update_id_;
    unlock_config();

    const bool transport_changed =
        candidate.gpio != previous.gpio || candidate.protocol != previous.protocol;
    const bool platform_changed = candidate.enabled != previous.enabled || transport_changed;
    core::Status status = core::Status::success();
    resources::DeviceBroker::Lease candidate_pin{};
    if (candidate.gpio != previous.gpio) {
        std::array<char, 12> candidate_id{};
        const int id_size = std::snprintf(candidate_id.data(), candidate_id.size(), "gpio.%u",
                                          static_cast<unsigned>(candidate.gpio));
        const bool transaction_already_moved_lease =
            id_size > 0 && static_cast<std::size_t>(id_size) < candidate_id.size() &&
            pin_lease_.resource_id() ==
                std::string_view{candidate_id.data(), static_cast<std::size_t>(id_size)};
        if (!transaction_already_moved_lease) {
            auto reservation = reserve_pin(candidate.gpio);
            if (!reservation) {
                status = core::Status::failure(reservation.error());
            } else {
                candidate_pin = std::move(reservation.value());
            }
        }
    }
    if (status && platform_changed) {
        deinitialize_output();
        if (candidate.enabled) {
            status = initialize_output(candidate);
        }
    }
    if (status) {
        status = save_config(candidate);
    }
    if (!status && platform_changed) {
        deinitialize_output();
        if (previous.enabled && !initialize_output(previous)) {
            ESP_LOGE(kTag, "failed to restore prior output after configuration failure");
        }
    }
    if (lock_config()) {
        if (status) {
            config_ = candidate;
            if (candidate_pin.valid()) {
                pin_lease_ = std::move(candidate_pin);
            }
        }
        completed_update_id_ = update_id;
        completed_update_status_ = status;
        reconfigure_pending_ = false;
        unlock_config();
    }
    if (completion_ != nullptr) {
        static_cast<void>(xSemaphoreGive(completion_));
    }
}

core::Status EspRmtStripComponent::read_parameter(std::string_view id,
                                                  core::ScalarValue& output) noexcept {
    if (id == "applied_frames") {
        output = core::ScalarValue::from_integer(applied_frames_.load());
        return core::Status::success();
    }
    if (id == "failed_frames") {
        output = core::ScalarValue::from_integer(failed_frames_.load());
        return core::Status::success();
    }
    if (id == "estimated_current_ma") {
        output = core::ScalarValue::from_integer(estimated_current_ma_.load());
        return core::Status::success();
    }
    if (id == "power_scale_q16") {
        output = core::ScalarValue::from_integer(power_scale_q16_.load());
        return core::Status::success();
    }
    if (id == "last_frame_us") {
        output = core::ScalarValue::from_integer(last_frame_us_.load());
        return core::Status::success();
    }
    if (id == "worker_stack_headroom") {
        const auto words = task_ == nullptr ? 0U : uxTaskGetStackHighWaterMark(task_);
        output = core::ScalarValue::from_integer(words * sizeof(StackType_t));
        return core::Status::success();
    }
    if (id == "backend") {
        output = core::ScalarValue::from_string(kClockedBoard ? "esp-spi-dma" : "native-rmt");
        return core::Status::success();
    }
    if (!started_.load() || !lock_config()) {
        return core::Status::failure(
            output_error(core::ErrorCode::invalid_state, "read-parameter", "not-started"));
    }
    if (id == "enabled") {
        output = core::ScalarValue::from_bool(config_.enabled);
    } else if (id == "pin") {
        output = core::ScalarValue::from_integer(config_.gpio);
    } else if (id == "protocol") {
        output = core::ScalarValue::from_integer(static_cast<std::uint8_t>(config_.protocol));
    } else if (id == "pixels") {
        output = core::ScalarValue::from_integer(config_.pixel_count);
    } else if (id == "brightness") {
        output = core::ScalarValue::from_integer(config_.brightness);
    } else if (id == "power_budget_ma") {
        output = core::ScalarValue::from_integer(config_.power_budget_ma);
    } else if (id == "red") {
        output = core::ScalarValue::from_integer(config_.red);
    } else if (id == "green") {
        output = core::ScalarValue::from_integer(config_.green);
    } else if (id == "blue") {
        output = core::ScalarValue::from_integer(config_.blue);
    } else if (id == "white") {
        output = core::ScalarValue::from_integer(config_.white);
    } else {
        unlock_config();
        return core::Status::failure(
            output_error(core::ErrorCode::not_found, "read-parameter", id));
    }
    unlock_config();
    return core::Status::success();
}

core::Status EspRmtStripComponent::write_parameter(std::string_view id,
                                                   const core::ScalarValue& value) noexcept {
    if (request_mutex_ == nullptr ||
        xSemaphoreTake(request_mutex_, pdMS_TO_TICKS(10000)) != pdTRUE) {
        return core::Status::failure(
            output_error(core::ErrorCode::invalid_state, "write-parameter", "not-started"));
    }
    if (!started_.load() || !lock_config()) {
        static_cast<void>(xSemaphoreGive(request_mutex_));
        return core::Status::failure(
            output_error(core::ErrorCode::invalid_state, "write-parameter", "not-started"));
    }
    StripConfig candidate = config_;
    unlock_config();
    if (id == "enabled" && value.type == core::ValueType::boolean) {
        candidate.enabled = value.boolean;
    } else if (id == "pin" && !kClockedBoard && value.type == core::ValueType::integer && value.integer >= 0 &&
               value.integer <= 63) {
        candidate.gpio = static_cast<std::uint8_t>(value.integer);
    } else if (id == "protocol" && !kClockedBoard && value.type == core::ValueType::integer && value.integer >= 0 &&
               value.integer <= 1) {
        candidate.protocol = static_cast<StripProtocol>(value.integer);
    } else if (id == "pixels" && !kClockedBoard && value.type == core::ValueType::integer && value.integer >= 1 &&
               value.integer <= static_cast<std::int64_t>(kMaximumStripPixels)) {
        candidate.pixel_count = static_cast<std::uint16_t>(value.integer);
    } else if (id == "power_budget_ma" && value.type == core::ValueType::integer &&
               value.integer >= 1 && value.integer <= kHardPowerBudgetMa) {
        candidate.power_budget_ma = static_cast<std::uint16_t>(value.integer);
    } else if ((id == "brightness" || id == "red" || id == "green" || id == "blue" ||
                id == "white") &&
               value.type == core::ValueType::integer && value.integer >= 0 &&
               value.integer <= 255) {
        auto& channel = id == "brightness" ? candidate.brightness
                        : id == "red"      ? candidate.red
                        : id == "green"    ? candidate.green
                        : id == "blue"     ? candidate.blue
                                           : candidate.white;
        channel = static_cast<std::uint8_t>(value.integer);
    } else {
        static_cast<void>(xSemaphoreGive(request_mutex_));
        return core::Status::failure(
            output_error(core::ErrorCode::validation_failed, "write-parameter", "invalid-value"));
    }
    const auto status = apply_config_locked(candidate);
    static_cast<void>(xSemaphoreGive(request_mutex_));
    return status;
}

core::Status EspRmtStripComponent::ingest_stream(
    std::uint8_t sequence, std::uint16_t start_pixel, std::span<const std::byte> channels,
    std::uint8_t channels_per_pixel, bool sixteen_bit, std::uint64_t received_at_us) noexcept {
    if (!started_.load() || stream_mutex_ == nullptr) {
        return core::Status::failure(output_error(core::ErrorCode::invalid_state,
                                                  "ingest-stream", "not-started"));
    }
    if (xSemaphoreTake(stream_mutex_, pdMS_TO_TICKS(100)) != pdTRUE) {
        return core::Status::failure(output_error(core::ErrorCode::resource_unavailable,
                                                  "ingest-stream", "stream-busy"));
    }
    if (!lock_config()) {
        static_cast<void>(xSemaphoreGive(stream_mutex_));
        return core::Status::failure(output_error(core::ErrorCode::invalid_state,
                                                  "ingest-stream", "config-unavailable"));
    }
    const auto pixel_count = config_.pixel_count;
    unlock_config();
    const auto channel_width = static_cast<std::size_t>(channels_per_pixel) *
                               (sixteen_bit ? 2U : 1U);
    if (start_pixel >= pixel_count ||
        (channel_width != 0U && channels.size() / channel_width > pixel_count - start_pixel)) {
        static_cast<void>(xSemaphoreGive(stream_mutex_));
        return core::Status::failure(output_error(core::ErrorCode::validation_failed,
                                                  "ingest-stream", "configured-pixel-range"));
    }
    const auto status = stream_layer_.ingest(sequence, start_pixel, channels, channels_per_pixel,
                                              sixteen_bit, received_at_us);
    static_cast<void>(xSemaphoreGive(stream_mutex_));
    if (status && task_ != nullptr) {
        xTaskNotifyGive(task_);
    }
    return status;
}

core::Status EspRmtStripComponent::invoke_action(std::string_view id,
                                                 std::span<const core::ScalarValue> arguments,
                                                 std::span<core::ScalarValue>,
                                                 std::size_t& output_count) noexcept {
    output_count = 0U;
#if defined(BLIP_BOARD_CREATORS_BALL_V2)
    if (id == "stream_pixel") {
        if (arguments.size() != 4U || !started_.load() || stream_mutex_ == nullptr) {
            return core::Status::failure(output_error(core::ErrorCode::invalid_argument,
                                                      "stream-pixel", "invalid-arguments"));
        }
        for (std::size_t index = 0U; index < arguments.size(); ++index) {
            if (arguments[index].type != core::ValueType::integer || arguments[index].integer < 0 ||
                arguments[index].integer > (index == 0U ? 35 : 255)) {
                return core::Status::failure(output_error(core::ErrorCode::validation_failed,
                                                          "stream-pixel", "invalid-value"));
            }
        }
        const std::array<std::byte, 3> channels{
            static_cast<std::byte>(arguments[1].integer),
            static_cast<std::byte>(arguments[2].integer),
            static_cast<std::byte>(arguments[3].integer)};
        return ingest_stream(0U, static_cast<std::uint16_t>(arguments[0].integer), channels,
                             3U, false, static_cast<std::uint64_t>(esp_timer_get_time()));
    }
#endif
    if (id != "blackout" || !arguments.empty()) {
        return core::Status::failure(
            output_error(core::ErrorCode::invalid_argument, "invoke-action", "invalid-action"));
    }
    if (request_mutex_ == nullptr ||
        xSemaphoreTake(request_mutex_, pdMS_TO_TICKS(10000)) != pdTRUE) {
        return core::Status::failure(
            output_error(core::ErrorCode::invalid_state, "invoke-action", "not-started"));
    }
    if (!started_.load() || !lock_config()) {
        static_cast<void>(xSemaphoreGive(request_mutex_));
        return core::Status::failure(
            output_error(core::ErrorCode::invalid_state, "invoke-action", "not-started"));
    }
    StripConfig candidate = config_;
    unlock_config();
    candidate.red = 0U;
    candidate.green = 0U;
    candidate.blue = 0U;
    candidate.white = 0U;
    if (stream_mutex_ != nullptr && xSemaphoreTake(stream_mutex_, portMAX_DELAY) == pdTRUE) {
        stream_layer_.clear();
        static_cast<void>(xSemaphoreGive(stream_mutex_));
    }
    const auto status = apply_config_locked(candidate);
    if (status && task_ != nullptr) {
        xTaskNotifyGive(task_);
    }
    static_cast<void>(xSemaphoreGive(request_mutex_));
    return status;
}

void EspRmtStripComponent::run() noexcept {
    while (started_.load()) {
        const auto notifications = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        if (!started_.load()) {
            break;
        }
        bool stream_expired{};
        if (stream_mutex_ != nullptr && xSemaphoreTake(stream_mutex_, portMAX_DELAY) == pdTRUE) {
            stream_expired = stream_layer_.active();
            stream_layer_.expire(static_cast<std::uint64_t>(esp_timer_get_time()), 1'000'000U,
                                 true);
            stream_expired = stream_expired && !stream_layer_.active();
            static_cast<void>(xSemaphoreGive(stream_mutex_));
        }
        if (notifications == 0U && !stream_expired) {
            continue;
        }
        process_pending_config();
        StripConfig snapshot{};
        if (!lock_config()) {
            saturating_increment(failed_frames_);
            continue;
        }
        snapshot = config_;
        unlock_config();
        if (!snapshot.enabled) {
            continue;
        }
        pm::FrameCpuLock frame_lock{*power_manager_};
        if (!frame_lock.acquired()) {
            saturating_increment(failed_frames_);
            continue;
        }
#if defined(BLIP_BOARD_CREATORS_BALL_V2)
        const auto status = transmit(snapshot, 0U);
#else
        const auto payload = render_one_wire(snapshot);
        if (!payload) {
            saturating_increment(failed_frames_);
            continue;
        }
        const auto status = transmit(snapshot, payload.value());
#endif
        if (status) {
            saturating_increment(applied_frames_);
        } else {
            saturating_increment(failed_frames_);
            const auto& error = status.error();
            ESP_LOGE(kTag, "frame failed code=%u operation=%.*s detail=%.*s",
                     static_cast<unsigned>(error.code), static_cast<int>(error.operation.size()),
                     error.operation.data(), static_cast<int>(error.detail.size()),
                     error.detail.data());
        }
    }
    task_quiesced_.store(true);
    vTaskDelete(nullptr);
}

void EspRmtStripComponent::task_entry(void* context) noexcept {
    static_cast<EspRmtStripComponent*>(context)->run();
}

} // namespace blip::led
