#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"

#include <array>
#include <cstddef>
#include <cstdint>

#ifndef BLIP_QUAL_RMT_GPIO
#define BLIP_QUAL_RMT_GPIO -1
#endif
#ifndef BLIP_QUAL_SPI_DATA_GPIO
#define BLIP_QUAL_SPI_DATA_GPIO -1
#endif
#ifndef BLIP_QUAL_SPI_CLOCK_GPIO
#define BLIP_QUAL_SPI_CLOCK_GPIO -1
#endif

namespace {

constexpr char kTag[] = "blip_led_qual";
constexpr std::uint32_t kResolutionHz = 10'000'000U;
constexpr std::size_t kMaximumPixels = 1024U;
constexpr std::size_t kMaximumLanes = 16U;
constexpr std::size_t kIterations = 8U;
constexpr std::array<std::size_t, 5> kPixelSweep{1U, 32U, 256U, 512U, 1024U};
constexpr std::array<std::size_t, 5> kLaneSweep{1U, 2U, 4U, 8U, 16U};

alignas(64) std::array<std::uint8_t, kMaximumPixels * 4U> pixel_bytes{};
alignas(64) std::array<std::uint8_t, kMaximumPixels * 9U> spi_waveform{};
alignas(64) std::array<std::uint8_t, 4U + kMaximumPixels * 4U + 128U> clocked_frame{};

void fill_pixels(std::size_t pixels, std::size_t channels) noexcept {
    for (std::size_t index = 0; index < pixels * channels; ++index) {
        pixel_bytes[index] = static_cast<std::uint8_t>((index * 73U + 29U) & 0xffU);
    }
}

std::size_t encode_spi_waveform(std::size_t bytes) noexcept {
    std::uint32_t pending = 0U;
    std::size_t pending_bits = 0U;
    std::size_t output_size = 0U;
    for (std::size_t index = 0; index < bytes; ++index) {
        for (std::uint8_t mask = 0x80U; mask != 0U; mask >>= 1U) {
            pending = (pending << 3U) | ((pixel_bytes[index] & mask) != 0U ? 0b110U : 0b100U);
            pending_bits += 3U;
            if (pending_bits == 24U) {
                spi_waveform[output_size++] = static_cast<std::uint8_t>(pending >> 16U);
                spi_waveform[output_size++] = static_cast<std::uint8_t>(pending >> 8U);
                spi_waveform[output_size++] = static_cast<std::uint8_t>(pending);
                pending = 0U;
                pending_bits = 0U;
            }
        }
    }
    return output_size;
}

std::size_t encode_apa102(std::size_t pixels) noexcept {
    std::size_t output = 0U;
    for (std::size_t index = 0; index < 4U; ++index) {
        clocked_frame[output++] = 0U;
    }
    for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
        clocked_frame[output++] = 0xffU;
        clocked_frame[output++] = pixel_bytes[pixel * 3U + 2U];
        clocked_frame[output++] = pixel_bytes[pixel * 3U];
        clocked_frame[output++] = pixel_bytes[pixel * 3U + 1U];
    }
    const std::size_t end_bytes = (pixels + 15U) / 16U;
    for (std::size_t index = 0; index < end_bytes; ++index) {
        clocked_frame[output++] = 0xffU;
    }
    return output;
}

void emit_result(const char* backend, const char* load, std::size_t lanes, std::size_t pixels,
                 bool supported, std::int64_t encoder_us, std::int64_t wall_us,
                 std::size_t staging_bytes, const char* detail) noexcept {
    ESP_LOGI(kTag,
             "BLIP_LED_QUAL {\"schema_version\":1,\"target\":\"%s\",\"backend\":\"%s\","
             "\"load\":\"%s\",\"lanes\":%u,\"pixels\":%u,\"iterations\":%u,"
             "\"supported\":%s,\"encoder_us\":%lld,\"wall_us\":%lld,"
             "\"staging_bytes\":%u,\"detail\":\"%s\"}",
             CONFIG_IDF_TARGET, backend, load, static_cast<unsigned>(lanes),
             static_cast<unsigned>(pixels), static_cast<unsigned>(kIterations),
             supported ? "true" : "false", static_cast<long long>(encoder_us),
             static_cast<long long>(wall_us), static_cast<unsigned>(staging_bytes), detail);
}

struct RmtLaneSet {
    std::array<rmt_channel_handle_t, kMaximumLanes> channels{};
    std::array<rmt_encoder_handle_t, kMaximumLanes> encoders{};
    std::size_t count{};
};

void destroy_rmt(RmtLaneSet& set) noexcept {
    for (std::size_t index = 0; index < set.count; ++index) {
        if (set.channels[index] != nullptr) {
            static_cast<void>(rmt_disable(set.channels[index]));
            static_cast<void>(rmt_del_channel(set.channels[index]));
        }
        if (set.encoders[index] != nullptr) {
            static_cast<void>(rmt_del_encoder(set.encoders[index]));
        }
    }
    set = {};
}

bool create_rmt(RmtLaneSet& set, std::size_t lanes, bool dma) noexcept {
    for (std::size_t index = 0; index < lanes; ++index) {
        rmt_tx_channel_config_t channel_config{};
        channel_config.gpio_num = static_cast<gpio_num_t>(BLIP_QUAL_RMT_GPIO);
        channel_config.clk_src = RMT_CLK_SRC_DEFAULT;
        channel_config.resolution_hz = kResolutionHz;
        channel_config.mem_block_symbols = dma ? 512U : SOC_RMT_MEM_WORDS_PER_CHANNEL;
        channel_config.trans_queue_depth = 2U;
        channel_config.flags.with_dma = dma;
        if (rmt_new_tx_channel(&channel_config, &set.channels[index]) != ESP_OK) {
            destroy_rmt(set);
            return false;
        }
        rmt_bytes_encoder_config_t encoder_config{};
        encoder_config.bit0.duration0 = 4U;
        encoder_config.bit0.level0 = 1U;
        encoder_config.bit0.duration1 = 9U;
        encoder_config.bit0.level1 = 0U;
        encoder_config.bit1.duration0 = 9U;
        encoder_config.bit1.level0 = 1U;
        encoder_config.bit1.duration1 = 4U;
        encoder_config.bit1.level1 = 0U;
        encoder_config.flags.msb_first = true;
        if (rmt_new_bytes_encoder(&encoder_config, &set.encoders[index]) != ESP_OK ||
            rmt_enable(set.channels[index]) != ESP_OK) {
            set.count = index + 1U;
            destroy_rmt(set);
            return false;
        }
        set.count = index + 1U;
    }
    return true;
}

void run_rmt(bool dma) noexcept {
#if !SOC_RMT_SUPPORT_DMA
    if (dma) {
        for (const auto lanes : kLaneSweep) {
            for (const auto pixels : kPixelSweep) {
                emit_result("rmt-dma", "idle", lanes, pixels, false, 0, 0, 0, "soc-no-rmt-dma");
            }
        }
        return;
    }
#endif
    const char* backend = dma ? "rmt-dma" : "rmt";
    fill_pixels(kMaximumPixels, 3U);
    for (const auto lanes : kLaneSweep) {
        RmtLaneSet set{};
        if (!create_rmt(set, lanes, dma)) {
            for (const auto pixels : kPixelSweep) {
                emit_result(backend, "idle", lanes, pixels, false, 0, 0, 0,
                            "lane-allocation-failed");
            }
            continue;
        }
        for (const auto pixels : kPixelSweep) {
            const auto start = esp_timer_get_time();
            bool passed = true;
            rmt_transmit_config_t transmit{};
            transmit.flags.eot_level = 0U;
            for (std::size_t iteration = 0; iteration < kIterations && passed; ++iteration) {
                for (std::size_t lane = 0; lane < lanes; ++lane) {
                    passed = rmt_transmit(set.channels[lane], set.encoders[lane],
                                          pixel_bytes.data(), pixels * 3U, &transmit) == ESP_OK;
                    if (!passed) {
                        break;
                    }
                }
                for (std::size_t lane = 0; lane < lanes && passed; ++lane) {
                    passed = rmt_tx_wait_all_done(set.channels[lane], 1000) == ESP_OK;
                }
            }
            emit_result(backend, "idle", lanes, pixels, passed, 0, esp_timer_get_time() - start, 0,
                        passed ? "completed" : "tx-failed");
        }
        destroy_rmt(set);
    }
}

void run_spi(bool dma, bool clocked) noexcept {
    const char* backend = clocked ? "spi-clocked" : (dma ? "spi-waveform-dma" : "spi-waveform");
    spi_bus_config_t bus{};
    bus.mosi_io_num = BLIP_QUAL_SPI_DATA_GPIO;
    bus.miso_io_num = -1;
    bus.sclk_io_num = BLIP_QUAL_SPI_CLOCK_GPIO;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = static_cast<int>(spi_waveform.size());
    const auto dma_channel = dma ? SPI_DMA_CH_AUTO : SPI_DMA_DISABLED;
    if (spi_bus_initialize(SPI2_HOST, &bus, dma_channel) != ESP_OK) {
        for (const auto pixels : kPixelSweep) {
            emit_result(backend, "idle", 1U, pixels, false, 0, 0, 0, "bus-initialize-failed");
        }
        return;
    }
    spi_device_interface_config_t device_config{};
    device_config.clock_speed_hz = clocked ? 10'000'000 : 2'400'000;
    device_config.mode = 0;
    device_config.spics_io_num = -1;
    device_config.queue_size = 2;
    spi_device_handle_t device{};
    if (spi_bus_add_device(SPI2_HOST, &device_config, &device) != ESP_OK) {
        static_cast<void>(spi_bus_free(SPI2_HOST));
        return;
    }
    for (const auto pixels : kPixelSweep) {
        fill_pixels(pixels, 3U);
        const auto encode_start = esp_timer_get_time();
        std::size_t payload_size = 0U;
        for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
            payload_size = clocked ? encode_apa102(pixels) : encode_spi_waveform(pixels * 3U);
        }
        const auto encoder_us = esp_timer_get_time() - encode_start;
        const auto wall_start = esp_timer_get_time();
        bool passed = true;
        for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
            spi_transaction_t transaction{};
            transaction.length = payload_size * 8U;
            transaction.tx_buffer = clocked ? static_cast<const void*>(clocked_frame.data())
                                            : static_cast<const void*>(spi_waveform.data());
            if (spi_device_polling_transmit(device, &transaction) != ESP_OK) {
                passed = false;
                break;
            }
        }
        emit_result(backend, "idle", 1U, pixels, passed, encoder_us,
                    esp_timer_get_time() - wall_start, payload_size,
                    passed ? "completed" : "tx-failed");
    }
    static_cast<void>(spi_bus_remove_device(device));
    static_cast<void>(spi_bus_free(SPI2_HOST));
}

void emit_fixture_requirements() noexcept {
    const char* parallel = "target-parallel";
#if SOC_PARLIO_SUPPORTED
    parallel = "parlio";
#elif SOC_LCD_I80_SUPPORTED
    parallel = "lcd-i80";
#endif
    for (const auto lanes : kLaneSweep) {
        for (const auto pixels : kPixelSweep) {
            emit_result("spi-multiline", "idle", lanes, pixels, false, 0, 0, 0,
                        "external-pin-capture-fixture-required");
            emit_result(parallel, "idle", lanes, pixels, false, 0, 0, 0,
                        "external-pin-capture-fixture-required");
        }
    }
}

} // namespace

extern "C" void app_main() {
    // Native USB consoles can re-enumerate after reset. Give the host capture
    // process time to reopen the port before emitting the first JSON record.
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(kTag,
             "BLIP_LED_QUAL_BEGIN target=%s idf=%s rmt_gpio=%d spi_data_gpio=%d "
             "spi_clock_gpio=%d heap=%u",
             CONFIG_IDF_TARGET, esp_get_idf_version(), BLIP_QUAL_RMT_GPIO, BLIP_QUAL_SPI_DATA_GPIO,
             BLIP_QUAL_SPI_CLOCK_GPIO,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
    run_rmt(false);
    run_rmt(true);
    run_spi(false, false);
    run_spi(true, false);
    run_spi(true, true);
    emit_fixture_requirements();
    ESP_LOGI(kTag, "BLIP_LED_QUAL_DONE target=%s heap=%u", CONFIG_IDF_TARGET,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
