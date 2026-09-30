#include "blip/artnet/artnet.hpp"
#include "blip/ddp/ddp.hpp"
#include "blip/e131/e131.hpp"
#include "blip/led/backend_selector.hpp"
#include "blip/led/color.hpp"
#include "blip/led/compositor.hpp"
#include "blip/led/frame_pipeline.hpp"
#include "blip/led/parallel_encoder.hpp"
#include "blip/led/playback.hpp"
#include "blip/led/protocol_encoder.hpp"
#include "blip/led/stream.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>

namespace {
using namespace blip::led;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::cerr << "FAIL " << __func__ << ':' << __LINE__ << " " #x "\n";                    \
            return false;                                                                          \
        }                                                                                          \
    } while (false)

constexpr std::array kOneWire{PixelProtocol::ws2812, PixelProtocol::sk6812};
constexpr std::array kClocked{PixelProtocol::apa102, PixelProtocol::sk9822, PixelProtocol::hd108};
constexpr std::array kFormats{PixelFormat::rgb8, PixelFormat::rgbw8, PixelFormat::rgb16};
constexpr std::array<std::string_view, 2> kClaims{"dma.0", "spi.2"};

bool abi_reports_limits_and_periods() {
    constexpr DriverCapabilities cap{"spi",
                                     kClocked,
                                     kFormats,
                                     1,
                                     1,
                                     1024,
                                     1000000,
                                     40000000,
                                     0,
                                     8,
                                     16,
                                     4,
                                     BufferingMode::queued,
                                     DmaMode::required,
                                     SynchronizationMode::per_lane,
                                     true,
                                     "fixture",
                                     kClaims};
    CHECK(cap.supports(PixelProtocol::hd108));
    CHECK(!cap.supports(PixelProtocol::ws2812));
    CHECK(cap.required_staging_bytes(10) == 96U);
    CHECK(cap.physical_frame_period_us(PixelProtocol::hd108, 1, 1000000) == 256U);
    CHECK(validate_output_config(cap, {PixelProtocol::apa102, PixelFormat::rgb8, 1, 10, 20000000}));
    CHECK(
        !validate_output_config(cap, {PixelProtocol::apa102, PixelFormat::rgb8, 2, 10, 20000000}));
    return true;
}

bool compositor_is_linear_and_priority_ordered() {
    std::array<LinearPixel, 1> stream{{{65535, 0, 0, 0, 65535}}};
    std::array<LinearPixel, 1> playback{{{0, 65535, 0, 0, 32768}}};
    std::array<LinearPixel, 1> system{{{0, 0, 65535, 0, 65535}}};
    std::array<LinearPixel, 1> output{};
    Compositor compositor;
    CHECK(compositor.set_layer({LayerId::system, system, BlendMode::replace, 65535, true}));
    CHECK(compositor.set_layer({LayerId::playback, playback, BlendMode::alpha_over, 65535, true}));
    CHECK(compositor.set_layer({LayerId::stream, stream, BlendMode::replace, 65535, true}));
    CHECK(compositor.compose(output));
    CHECK(output[0] == system[0]);
    compositor.clear_layer(LayerId::system);
    CHECK(compositor.compose(output));
    CHECK(output[0].red >= 32766U && output[0].red <= 32767U);
    CHECK(output[0].green >= 32768U && output[0].green <= 32769U);
    return true;
}

bool color_and_protocol_goldens() {
    CHECK(quantize_channel(0, TransferFunction::srgb) == 0U);
    CHECK(quantize_channel(65535, TransferFunction::srgb) == 255U);
    CHECK(quantize_channel(32768, TransferFunction::srgb) == 188U);
    ColorTransform transform{};
    transform.transfer = TransferFunction::linear;
    transform.order = {ColorChannel::green, ColorChannel::red, ColorChannel::blue,
                       ColorChannel::white};
    std::array<std::byte, 4> color{};
    CHECK(encode_color({0x1111, 0x2222, 0x3333, 0x4444, 65535}, PixelFormat::rgbw8, transform,
                       color));
    CHECK(color[0] == std::byte{0x22} && color[1] == std::byte{0x11} &&
          color[2] == std::byte{0x33} && color[3] == std::byte{0x44});

    std::array<LinearPixel, 1> pixels{{{0x1111, 0x2222, 0x3333, 0, 65535}}};
    ConstPixelSurface surface{pixels, 1, 1};
    EncoderOptions options{};
    options.color.transfer = TransferFunction::linear;
    std::array<std::byte, 32> encoded{};
    const auto apa = encode_frame(surface, PixelProtocol::apa102, options, encoded);
    CHECK(apa && apa.value() == 9U);
    CHECK(encoded[4] == std::byte{0xff} && encoded[5] == std::byte{0x33} &&
          encoded[6] == std::byte{0x22} && encoded[7] == std::byte{0x11});
    const auto hd = encode_frame(surface, PixelProtocol::hd108, options, encoded);
    CHECK(encoded_frame_size(PixelProtocol::hd108, 36U) == 312U);
    CHECK(hd && hd.value() == 32U && encoded[16] == std::byte{0xff} &&
          encoded[17] == std::byte{0xff} && encoded[18] == std::byte{0x11});
    CHECK(std::all_of(encoded.begin() + 24, encoded.begin() + 32,
                      [](std::byte value) { return value == std::byte{0}; }));
    std::array<std::byte, 1> zero{std::byte{0}};
    std::array<std::byte, 3> waveform{};
    CHECK(encode_spi_one_wire(zero, waveform));
    CHECK(
        (waveform == std::array<std::byte, 3>{std::byte{0x92}, std::byte{0x49}, std::byte{0x24}}));
    const std::array<std::byte, 2> lanes{std::byte{0x80}, std::byte{0x40}};
    std::array<std::uint16_t, 24> samples{};
    const auto parallel = encode_parallel_wave(lanes, 2U, 1U, samples);
    CHECK(parallel && samples[0] == 0b11U && samples[1] == 0b01U && samples[2] == 0U);
    CHECK(samples[3] == 0b11U && samples[4] == 0b10U && samples[5] == 0U);
    return true;
}

class FakeDriver final : public OutputDriver {
  public:
    const DriverCapabilities& capabilities() const noexcept override { return cap; }
    blip::core::Status start(const OutputConfig&) noexcept override {
        return blip::core::Status::success();
    }
    blip::core::Status submit(const EncodedFrame& frame) noexcept override {
        active = frame.sequence;
        return blip::core::Status::success();
    }
    Completion poll() noexcept override {
        if (!complete)
            return {};
        complete = false;
        return {CompletionState::complete, active, completed_at};
    }
    blip::core::Status stop() noexcept override { return blip::core::Status::success(); }
    static constexpr DriverCapabilities cap{"fake",
                                            kOneWire,
                                            kFormats,
                                            1,
                                            1,
                                            1024,
                                            0,
                                            0,
                                            80,
                                            3,
                                            0,
                                            1,
                                            BufferingMode::queued,
                                            DmaMode::optional,
                                            SynchronizationMode::per_lane,
                                            true,
                                            "host",
                                            kClaims};
    std::uint64_t active{};
    std::uint64_t completed_at{};
    bool complete{};
};

bool frame_pool_is_bounded_and_observable() {
    FakeDriver driver;
    FramePipeline<16, 2> pipeline(driver, OverloadPolicy::reject_newest);
    CHECK(pipeline.start({PixelProtocol::ws2812, PixelFormat::rgb8, 1, 1, 0}));
    const auto first = pipeline.acquire();
    CHECK(first);
    first.bytes[0] = std::byte{1};
    CHECK(pipeline.commit(first, 1, 100));
    const auto second = pipeline.acquire();
    CHECK(second);
    CHECK(pipeline.commit(second, 1, 200));
    CHECK(!pipeline.acquire());
    pipeline.service(10);
    CHECK(driver.active == 1U);
    driver.completed_at = 105;
    driver.complete = true;
    pipeline.service(105);
    CHECK(driver.active == 2U);
    driver.completed_at = 190;
    driver.complete = true;
    pipeline.service(190);
    const auto& metrics = pipeline.metrics();
    CHECK(metrics.submitted == 2U && metrics.completed == 2U && metrics.rejected == 1U);
    CHECK(metrics.deadline_misses == 1U && metrics.maximum_lateness_us == 5U &&
          metrics.queue_high_water == 2U);
    return true;
}

bool selector_is_deterministic_and_fail_closed() {
    constexpr DriverCapabilities rmt{"rmt",
                                     kOneWire,
                                     kFormats,
                                     1,
                                     1,
                                     1024,
                                     0,
                                     0,
                                     80,
                                     0,
                                     0,
                                     1,
                                     BufferingMode::queued,
                                     DmaMode::none,
                                     SynchronizationMode::per_lane,
                                     true,
                                     "host",
                                     kClaims};
    constexpr DriverCapabilities spi{"spi-wave",
                                     kOneWire,
                                     kFormats,
                                     1,
                                     1,
                                     1024,
                                     0,
                                     0,
                                     80,
                                     9,
                                     0,
                                     4,
                                     BufferingMode::queued,
                                     DmaMode::required,
                                     SynchronizationMode::per_lane,
                                     true,
                                     "host",
                                     kClaims};
    const std::array candidates{
        BackendCandidate{BackendKind::spi_encoded_one_wire, &spi, true, true},
        BackendCandidate{BackendKind::rmt, &rmt, true, true}};
    auto short_strip =
        select_backend({BackendKind::automatic, PixelProtocol::ws2812, 1, 32, 4096}, candidates);
    CHECK(short_strip && short_strip.value().candidate->kind == BackendKind::rmt);
    auto long_strip =
        select_backend({BackendKind::automatic, PixelProtocol::ws2812, 1, 512, 8192}, candidates);
    CHECK(long_strip && long_strip.value().candidate->kind == BackendKind::spi_encoded_one_wire);
    auto forced =
        select_backend({BackendKind::rmt_dma, PixelProtocol::ws2812, 1, 32, 4096}, candidates);
    CHECK(!forced);
    return true;
}

bool streaming_and_playback_are_bounded() {
    std::array<LinearPixel, 2> pixels{};
    StreamLayer stream(pixels);
    const std::array<std::byte, 6> rgb{std::byte{1}, std::byte{2}, std::byte{3},
                                       std::byte{4}, std::byte{5}, std::byte{6}};
    CHECK(stream.ingest(1, 0, rgb, 3, false, 100));
    CHECK(pixels[1].red == 4U * 257U && pixels[1].alpha == 65535U);
    CHECK(!stream.ingest(1, 0, rgb, 3, false, 101));
    stream.expire(201, 100, true);
    CHECK(!stream.active() && pixels[0].alpha == 0U);
    CHECK(stream.ingest(2, 0, rgb, 3, false, 202));
    stream.clear();
    CHECK(!stream.active() && pixels[0].alpha == 0U && pixels[1].alpha == 0U);
    CHECK(stream.ingest(1, 0, rgb, 3, false, 203));

    constexpr std::array<std::byte, 8> legacy{std::byte{0x80}, std::byte{1},    std::byte{2},
                                              std::byte{3},    std::byte{0xff}, std::byte{4},
                                              std::byte{5},    std::byte{6}};
    std::array<std::byte, 64> converted{};
    auto imported = import_legacy_playback("{\"fps\":30}", legacy, 2, converted);
    CHECK(imported && imported.value() == 48U);
    PlaybackReader reader;
    CHECK(reader.open(std::span<const std::byte>{converted}.first(imported.value())));
    CHECK(reader.info().frame_count == 1U && reader.info().fps_milli == 30000U);
    CHECK(reader.read_frame(0, pixels));
    CHECK((pixels[0] == LinearPixel{257, 514, 771, 0, 0x8080}));
    std::array<std::byte, 9> corrupt{};
    CHECK(!import_legacy_playback("{\"fps\":30}", corrupt, 2, converted));
    return true;
}

bool network_compatibility_packets_validate() {
    std::array<std::byte, 20> dmx{};
    const char id[] = "Art-Net";
    for (std::size_t i = 0; i < 7; ++i)
        dmx[i] = static_cast<std::byte>(id[i]);
    dmx[8] = std::byte{0x00};
    dmx[9] = std::byte{0x50};
    dmx[10] = std::byte{0};
    dmx[11] = std::byte{14};
    dmx[12] = std::byte{7};
    dmx[14] = std::byte{2};
    dmx[16] = std::byte{0};
    dmx[17] = std::byte{2};
    dmx[18] = std::byte{1};
    dmx[19] = std::byte{2};
    auto art = blip::artnet::parse(dmx);
    CHECK(art && art.value().universe == 2U && art.value().dmx.size() == 2U);
    const std::array<std::byte, 10> dmx_channels{
        std::byte{99}, std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
        std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}, std::byte{9}};
    const blip::artnet::Packet art_pixels{blip::artnet::PacketKind::dmx, 2U, 7U,
                                         dmx_channels};
    const auto mapped = blip::artnet::map_dmx(art_pixels, {2U, 2U, 0U, 3U, false}, 2U);
    CHECK(mapped && mapped.value().channels.size() == 6U);
    CHECK(mapped.value().channels.front() == std::byte{1});
    CHECK(mapped.value().channels.back() == std::byte{6});
    CHECK(!blip::artnet::map_dmx(art_pixels, {2U, 0U, 0U, 3U, false}, 2U));
    std::array<std::byte, blip::artnet::kPollReplyBytes> reply{};
    CHECK(blip::artnet::encode_poll_reply(
        {{192, 168, 1, 2}, {1, 2, 3, 4, 5, 6}, "BLIP", "BLIP node", 1, 2}, reply));
    CHECK(reply[8] == std::byte{0x00} && reply[9] == std::byte{0x21});

    std::array<std::byte, 128> e131{};
    e131[0] = std::byte{0};
    e131[1] = std::byte{0x10};
    constexpr char acn[] = "ASC-E1.17";
    for (std::size_t i = 0; i < 9; ++i)
        e131[4 + i] = static_cast<std::byte>(acn[i]);
    e131[16] = std::byte{0x70};
    e131[17] = std::byte{0x70};
    e131[21] = std::byte{4};
    e131[38] = std::byte{0x70};
    e131[39] = std::byte{0x5a};
    e131[43] = std::byte{2};
    e131[108] = std::byte{100};
    e131[111] = std::byte{9};
    e131[113] = std::byte{0};
    e131[114] = std::byte{1};
    e131[115] = std::byte{0x70};
    e131[116] = std::byte{0x0d};
    e131[117] = std::byte{2};
    e131[118] = std::byte{0xa1};
    e131[122] = std::byte{1};
    e131[123] = std::byte{0};
    e131[124] = std::byte{3};
    e131[125] = std::byte{0};
    e131[126] = std::byte{1};
    e131[127] = std::byte{2};
    const auto sacn = blip::e131::parse(e131);
    CHECK(sacn && sacn.value().universe == 1U && sacn.value().priority == 100U &&
          sacn.value().sequence == 9U && sacn.value().dmx.size() == 2U);

    std::array<std::byte, 12> ddp{};
    ddp[0] = std::byte{0x41};
    ddp[1] = std::byte{3};
    ddp[2] = std::byte{1};
    ddp[3] = std::byte{1};
    ddp[9] = std::byte{2};
    ddp[10] = std::byte{7};
    ddp[11] = std::byte{8};
    auto parsed_ddp = blip::ddp::parse(ddp);
    CHECK(parsed_ddp && parsed_ddp.value().push && parsed_ddp.value().data.size() == 2U);
    CHECK(parsed_ddp.value().sequence == 3U);
    std::array<std::byte, 13> rgb_ddp{};
    rgb_ddp[0] = std::byte{0x41};
    rgb_ddp[1] = std::byte{4};
    rgb_ddp[2] = std::byte{0x0b};
    rgb_ddp[3] = std::byte{1};
    rgb_ddp[9] = std::byte{3};
    rgb_ddp[10] = std::byte{10};
    rgb_ddp[11] = std::byte{20};
    rgb_ddp[12] = std::byte{30};
    const auto update = blip::ddp::map(rgb_ddp, {});
    CHECK(update && update.value().start_pixel == 0U &&
          update.value().channels.size() == 3U && update.value().sequence == 4U);
    rgb_ddp[3] = std::byte{2};
    CHECK(!blip::ddp::map(rgb_ddp, {}));
    rgb_ddp[3] = std::byte{1};
    rgb_ddp[0] = std::byte{0x51};
    CHECK(!blip::ddp::map(rgb_ddp, {}));
    blip::ddp::SequenceTracker sequences;
    CHECK(sequences.accept(14U, 100U));
    CHECK(sequences.accept(14U, 101U));
    CHECK(sequences.accept(15U, 102U));
    CHECK(sequences.accept(1U, 103U));
    CHECK(!sequences.accept(15U, 104U));
    CHECK(sequences.accept(3U, 105U));
    CHECK(sequences.accept(1U, 1'000'106U));
    return true;
}

bool e131_sources_merge_and_expire() {
    using blip::e131::SourceResult;
    blip::e131::SourceMixer mixer;
    const std::array<std::byte, 6> a{std::byte{10}, std::byte{1}, std::byte{3},
                                      std::byte{4}, std::byte{5}, std::byte{6}};
    const std::array<std::byte, 6> b{std::byte{2}, std::byte{8}, std::byte{1},
                                      std::byte{9}, std::byte{1}, std::byte{2}};
    blip::e131::Packet first{};
    first.source_id[0] = std::byte{1};
    first.priority = 100U;
    first.sequence = 1U;
    first.dmx = a;
    CHECK(mixer.ingest(first, 100U) == SourceResult::accepted);
    auto second = first;
    second.source_id[0] = std::byte{2};
    second.dmx = b;
    CHECK(mixer.ingest(second, 110U) == SourceResult::accepted);
    std::array<std::byte, 512> merged{};
    std::size_t size{};
    std::size_t sources{};
    CHECK(mixer.compose(merged, size, sources) && size == 6U && sources == 2U);
    CHECK(merged[0] == std::byte{10} && merged[1] == std::byte{8} &&
          merged[3] == std::byte{9});
    const auto mapped = blip::e131::map_dmx(std::span<const std::byte>{merged}.first(size),
                                             {}, 1U);
    CHECK(mapped && mapped.value().channels.size() == 3U &&
          mapped.value().channels[0] == std::byte{10});
    CHECK(!blip::e131::map_dmx(std::span<const std::byte>{merged}.first(size),
                                {1U, 0U, 0U, 3U, false}, 1U));
    auto third = first;
    third.source_id[0] = std::byte{3};
    CHECK(mixer.ingest(third, 120U) == SourceResult::capacity);
    second.priority = 120U;
    second.sequence = 2U;
    CHECK(mixer.ingest(second, 130U) == SourceResult::accepted);
    CHECK(mixer.compose(merged, size, sources) && sources == 1U && merged[0] == std::byte{2});
    CHECK(mixer.ingest(second, 140U) == SourceResult::stale);
    second.options = 0x40U;
    CHECK(mixer.ingest(second, 150U) == SourceResult::terminated);
    CHECK(mixer.compose(merged, size, sources) && sources == 1U && merged[0] == std::byte{10});
    first.options = 0x80U;
    first.sequence = 2U;
    CHECK(mixer.ingest(first, 160U) == SourceResult::ignored);
    CHECK(mixer.expire(100U + blip::e131::SourceMixer::kSourceTimeoutUs));
    CHECK(!mixer.compose(merged, size, sources) && sources == 0U);
    return true;
}

bool sustained_network_and_output_soak_is_bounded() {
    FakeDriver driver;
    FramePipeline<16, 3> pipeline(driver, OverloadPolicy::reject_newest);
    CHECK(pipeline.start({PixelProtocol::ws2812, PixelFormat::rgb8, 1, 1, 0}));
    std::array<LinearPixel, 1> pixels{};
    StreamLayer stream(pixels);
    std::array<std::byte, 22> dmx{};
    constexpr char id[] = "Art-Net";
    for (std::size_t index = 0; index < 7U; ++index)
        dmx[index] = static_cast<std::byte>(id[index]);
    dmx[8] = std::byte{0x00};
    dmx[9] = std::byte{0x50};
    dmx[11] = std::byte{14};
    dmx[16] = std::byte{0};
    dmx[17] = std::byte{4};
    blip::artnet::Receiver receiver{{}, {0, 1, 0, 4, false}, stream};
    std::array<std::byte, blip::artnet::kPollReplyBytes> response{};
    for (std::uint64_t iteration = 0; iteration < 50000U; ++iteration) {
        dmx[12] = static_cast<std::byte>(iteration % 255U + 1U);
        dmx[18] = static_cast<std::byte>(iteration);
        dmx[19] = std::byte{2};
        dmx[20] = std::byte{3};
        dmx[21] = std::byte{255};
        std::size_t response_size{};
        const auto action = receiver.receive(dmx, iteration, response, response_size);
        CHECK(action && action.value() == blip::artnet::ReceiverAction::stream_updated &&
              response_size == 0U);
        const auto lease = pipeline.acquire();
        CHECK(lease);
        lease.bytes[0] = dmx[18];
        CHECK(pipeline.commit(lease, 1U, iteration + 1U));
        pipeline.service(iteration);
        driver.completed_at = iteration;
        driver.complete = true;
        pipeline.service(iteration);
    }
    CHECK(pipeline.metrics().submitted == 50000U && pipeline.metrics().completed == 50000U);
    CHECK(pipeline.metrics().rejected == 0U && pipeline.metrics().dropped == 0U &&
          pipeline.metrics().queue_high_water == 1U);
    CHECK(stream.metrics().accepted == 50000U && stream.metrics().malformed == 0U);
    return true;
}
} // namespace

int main() {
    const std::array tests{abi_reports_limits_and_periods,
                           compositor_is_linear_and_priority_ordered,
                           color_and_protocol_goldens,
                           frame_pool_is_bounded_and_observable,
                           selector_is_deterministic_and_fail_closed,
                           streaming_and_playback_are_bounded,
                           network_compatibility_packets_validate,
                           e131_sources_merge_and_expire,
                           sustained_network_and_output_soak_is_bounded};
    for (const auto test : tests)
        if (!test())
            return 1;
    std::cout << "PASS blip_led_m4_tests " << tests.size() << " cases\n";
    return 0;
}
