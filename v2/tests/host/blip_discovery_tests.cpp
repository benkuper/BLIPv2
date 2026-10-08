#include "blip/artnet/artnet.hpp"
#include "blip/ddp/ddp.hpp"

#include <array>
#include <iostream>
#include <string_view>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::cerr << "FAIL line " << __LINE__ << ": " #x "\n";                                 \
            return 1;                                                                              \
        }                                                                                          \
    } while (false)

int main() {
    using namespace blip;
    std::array<std::byte, 18> poll{};
    constexpr std::string_view id{"Art-Net"};
    for (std::size_t i = 0; i < id.size(); ++i)
        poll[i] = static_cast<std::byte>(id[i]);
    poll[9] = std::byte{0x20};
    poll[11] = std::byte{14};
    poll[12] = std::byte{0x22};
    poll[14] = std::byte{0x12};
    poll[15] = std::byte{0x35};
    poll[16] = std::byte{0x12};
    poll[17] = std::byte{0x30};
    auto parsed = artnet::parse(poll);
    CHECK(parsed && parsed.value().notify_changes && parsed.value().targeted);
    CHECK(artnet::poll_matches(parsed.value(), 0x1230));
    CHECK(artnet::poll_matches(parsed.value(), 0x1235));
    CHECK(!artnet::poll_matches(parsed.value(), 0x122f));
    CHECK(!artnet::poll_matches(parsed.value(), 0x1236));
    parsed = artnet::parse(std::span<const std::byte>{poll}.first(14));
    CHECK(parsed && artnet::poll_matches(parsed.value(), 0) &&
          !artnet::poll_matches(parsed.value(), 1));
    poll[11] = std::byte{13};
    CHECK(!artnet::parse(poll));
    poll[11] = std::byte{14};
    poll[12] = std::byte{0};
    parsed = artnet::parse(poll);
    CHECK(parsed && artnet::poll_matches(parsed.value(), 0x7fff));

    artnet::NodeIdentity node{
        {192, 168, 1, 2}, {0, 1, 2, 3, 4, 5}, "BLIP", "BLIP node", 0x7ff0, 0x1234};
    std::array<std::byte, artnet::kPollReplyBytes> reply{};
    CHECK(artnet::encode_poll_reply(node, reply));
    CHECK(reply[14] == std::byte{0x36} && reply[15] == std::byte{0x19});
    CHECK(reply[18] == std::byte{0x12} && reply[19] == std::byte{3} && reply[190] == std::byte{4});
    CHECK(reply[172] == std::byte{0} && reply[173] == std::byte{1} &&
          reply[174] == std::byte{0x80});
    CHECK(reply[182] == std::byte{0} && reply[211] == std::byte{0x0d});
    CHECK(reply[207] == std::byte{192} && reply[210] == std::byte{2});
    node.dhcp = true;
    node.output_active = true;
    node.report_counter = 10001;
    CHECK(artnet::encode_poll_reply(node, reply));
    CHECK(reply[182] == std::byte{0x80} && reply[211] == std::byte{0x0f});
    CHECK(std::string_view{reinterpret_cast<const char*>(reply.data() + 108)}.find("[0001]") !=
          std::string_view::npos);

    std::array<std::byte, 10> query{};
    query[0] = std::byte{0x42};
    query[3] = std::byte{251};
    std::array<std::byte, 384> response{};
    const ddp::DiscoveryIdentity display{{0, 1, 2, 3, 4, 5}, 36};
    auto encoded = ddp::encode_query_reply(query, display, response);
    CHECK(encoded && response[0] == std::byte{0x45} && response[3] == std::byte{251});
    auto text =
        std::string_view{reinterpret_cast<const char*>(response.data() + 10), encoded.value() - 10};
    CHECK(text.starts_with("{\"status\":{") &&
          text.find("00:01:02:03:04:05") != std::string_view::npos);
    CHECK(text.find("\"push\":false") != std::string_view::npos);
    CHECK(encoded.value() == 10U + (std::to_integer<unsigned>(response[8]) << 8U) +
                                 std::to_integer<unsigned>(response[9]));
    const auto whole = response;
    query[7] = std::byte{2};
    query[9] = std::byte{3};
    encoded = ddp::encode_query_reply(query, display, response);
    CHECK(encoded && encoded.value() == 13 && response[0] == std::byte{0x44} &&
          response[7] == std::byte{2});
    CHECK(response[10] == whole[12] && response[12] == whole[14]);
    query[7] = std::byte{255};
    query[9] = std::byte{0};
    encoded = ddp::encode_query_reply(query, display, response);
    CHECK(encoded && encoded.value() == 10 && response[0] == std::byte{0x45});
    query[7] = std::byte{0};
    query[3] = std::byte{250};
    encoded = ddp::encode_query_reply(query, display, response);
    CHECK(encoded);
    text = {reinterpret_cast<const char*>(response.data() + 10), encoded.value() - 10};
    CHECK(text.find("\"l\":36") != std::string_view::npos &&
          text.find("\"num_chan\":108") != std::string_view::npos);
    query[3] = std::byte{1};
    encoded = ddp::encode_query_reply(query, display, response);
    CHECK(encoded && encoded.value() == 10 && response[3] == std::byte{1});
    query[0] = std::byte{0x46};
    CHECK(!ddp::encode_query_reply(query, display, response));
    query[0] = std::byte{0x52};
    CHECK(!ddp::encode_query_reply(query, display, response));
    query[0] = std::byte{0x42};
    CHECK(!ddp::encode_query_reply(std::span<const std::byte>{query}.first(9), display, response));
    CHECK(!ddp::encode_query_reply(query, display, std::span<std::byte>{response}.first(10)));
    CHECK(!ddp::map(query, {})); // Queries must never enter the pixel stream.
    std::cout << "PASS BLIP discovery packet validation\n";
    return 0;
}
