#include "blip/ota/release_catalog.hpp"
#include "blip/ota/release_policy.hpp"
#include "test_harness.hpp"
#include <string>
#include <iostream>
#include <fstream>
#include <iterator>

namespace {
using namespace blip::ota;
constexpr ReleaseIdentity identity{"blip-v2", "creators-ball-v2", "esp32c6", "ota-8mb-v1", "minimal", "stable",
    8388608, 123, 1, 1, 2000, "0.1.0"};
const std::string valid = R"({"schema":1,"project":"blip-v2","board":"creators-ball-v2","target":"esp32c6","layout":"ota-8mb-v1","profile":"minimal","channel":"stable","flash_bytes":8388608,"features":123,"api":1,"firmware":{"code":2,"version":"0.2.0-alpha","bytes":1000000,"url":"https://www.goldengeek.org/blip/app.bin","sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","minimum_other_code":2000},"web":{"code":2001,"version":"0.2.1","bytes":22000,"url":"https://www.goldengeek.org/blip/web.bundle","sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","minimum_other_code":1}})";
bool owned_decode_and_compatibility() {
    ReleaseCatalog catalog;
    auto source = valid;
    BLIP_CHECK(decode_release_catalog(source, catalog));
    source.assign(source.size(), 'x');
    BLIP_CHECK(catalog.board.view() == identity.board && catalog.firmware.version.view() == "0.2.0-alpha");
    BLIP_CHECK(validate_release_catalog(catalog, identity, 3145728, 262144));
    BLIP_CHECK(firmware_update_available(catalog, identity) && web_update_available(catalog, identity));
    auto wrong = identity; wrong.board = "another-c6";
    BLIP_CHECK(!validate_release_catalog(catalog, wrong, 3145728, 262144));
    wrong = identity; wrong.features ^= 1;
    BLIP_CHECK(!validate_release_catalog(catalog, wrong, 3145728, 262144));
    wrong = identity; wrong.flash_bytes = 4194304;
    BLIP_CHECK(!validate_release_catalog(catalog, wrong, 3145728, 262144));
    wrong = identity; wrong.api = 2;
    BLIP_CHECK(!validate_release_catalog(catalog, wrong, 3145728, 262144));
    BLIP_CHECK(!validate_release_catalog(catalog, identity, 999999, 262144));
    BLIP_CHECK(!validate_release_catalog(catalog, identity, 3145728, 21999));
    wrong = identity; wrong.firmware_code = 2; wrong.web_code = 2001;
    BLIP_CHECK(!firmware_update_available(catalog, wrong) && !web_update_available(catalog, wrong));
    catalog.firmware.minimum_other_code = 2001;
    catalog.web.minimum_other_code = 2;
    BLIP_CHECK(!firmware_update_available(catalog, identity) && !web_update_available(catalog, identity));
    return true;
}
bool malformed_input_and_urls_fail_closed() {
    ReleaseCatalog catalog;
    for (const std::string source : {std::string{}, valid + "x", std::string("[]"),
        std::string("{\"schema\":1,\"schema\":1}"), std::string("{\"schema\":1e0}"),
        std::string("{\"schema\":01}"), std::string("{\"schema\":2}"), std::string("{\"schema\":-1}")}) {
        BLIP_CHECK(!decode_release_catalog(source, catalog));
        BLIP_CHECK(!catalog.firmware.present && !catalog.web.present);
    }
    auto duplicate = valid; duplicate.insert(1, "\"board\":\"evil\",");
    BLIP_CHECK(!decode_release_catalog(duplicate, catalog));
    auto overflow = valid; overflow.replace(overflow.find("1000000"), 7, "4294967296");
    BLIP_CHECK(!decode_release_catalog(overflow, catalog));
    auto corrupt = valid; corrupt.replace(corrupt.find("012345"), 6, "xxxxxx");
    BLIP_CHECK(!decode_release_catalog(corrupt, catalog));
    BLIP_CHECK(!decode_release_catalog(std::string(4097, ' '), catalog));
    for (const auto url : {"ftp://example/app", "https://user:pass@example/app", "https://example/app#fragment",
         "https:///app", "https://example:0/app", "https://example:99999/app", "https://example/ bad", "https://example/%zz"})
        BLIP_CHECK(!valid_release_url(url));
    BLIP_CHECK(valid_release_url("https://www.goldengeek.org/blip/update?schema=1"));
    BLIP_CHECK(valid_release_url("https://127.0.0.1:8443/artifact%20one.bin"));
    BLIP_CHECK(valid_release_url("http://127.0.0.1:8088/artifact.bin"));
    return true;
}
bool independent_artifacts_and_public_query() {
    auto source = valid;
    const auto start = source.find("\"firmware\":{"); const auto end = source.find(",\"web\"");
    source.replace(start, end - start, "\"firmware\":null");
    ReleaseCatalog catalog;
    BLIP_CHECK(decode_release_catalog(source, catalog));
    BLIP_CHECK(!catalog.firmware.present && catalog.web.present);
    BLIP_CHECK(validate_release_catalog(catalog, identity, 3145728, 262144));
    std::array<char, 1024> query{}; std::size_t count = 999;
    BLIP_CHECK(build_release_query(kDefaultReleaseEndpoint, identity, query, count));
    const std::string_view url{query.data(), count};
    BLIP_CHECK(url.starts_with("http://www.goldengeek.org/blip/update?schema=1&project=blip-v2"));
    BLIP_CHECK(url.find("&board=creators-ball-v2") != url.npos && url.find("&fw_code=1") != url.npos && url.find("&web_code=2000") != url.npos);
    BLIP_CHECK(url.find("password") == url.npos && url.find("ssid") == url.npos && url.find("mac") == url.npos);
    BLIP_CHECK(!build_release_query(kDefaultReleaseEndpoint, identity, std::span<char>{query}.first(20), count) && count == 0);
    auto escaped = identity; escaped.firmware_version = "0.1.0+test build";
    BLIP_CHECK(build_release_query(kDefaultReleaseEndpoint, escaped, query, count));
    BLIP_CHECK(std::string_view(query.data(), count).find("fw_version=0.1.0%2Btest%20build") != url.npos);
    escaped = identity; escaped.flash_bytes = 0;
    BLIP_CHECK(!build_release_query(kDefaultReleaseEndpoint, escaped, query, count) && count == 0);
    escaped = identity; escaped.board = std::string_view("bad\0board", 9);
    BLIP_CHECK(!build_release_query(kDefaultReleaseEndpoint, escaped, query, count) && count == 0);
    const std::string oversized(32, 'x'); escaped = identity; escaped.project = oversized;
    BLIP_CHECK(!build_release_query(kDefaultReleaseEndpoint, escaped, query, count) && count == 0);
    return true;
}
bool persisted_release_policy_is_bounded_and_canonical() {
    auto policy = default_release_policy();
    BLIP_CHECK(policy.endpoint.view() == kDefaultReleaseEndpoint && policy.channel.view() == "stable");
    std::array<std::byte, kReleasePolicyBytes> bytes{};
    BLIP_CHECK(encode_release_policy(policy, bytes));
    ReleasePolicy decoded;
    BLIP_CHECK(decode_release_policy(bytes, decoded) && decoded.interval_hours == 24 && !decoded.automatic_firmware);
    BLIP_CHECK(policy.endpoint.assign("https://192.168.27.176:8443/blip/update") && policy.channel.assign("beta"));
    policy.interval_hours = 168; policy.automatic_web = true;
    BLIP_CHECK(encode_release_policy(policy, bytes) && decode_release_policy(bytes, decoded));
    BLIP_CHECK(decoded.endpoint.view() == policy.endpoint.view() && decoded.channel.view() == "beta" && decoded.automatic_web);
    for (const auto index : {0U, 6U, 11U, 331U, 347U}) {
        auto corrupt = bytes; corrupt[index] = std::byte{1};
        BLIP_CHECK(!decode_release_policy(corrupt, decoded) && decoded.endpoint.view().empty());
    }
    auto corrupt = bytes; corrupt[5] = std::byte{4}; BLIP_CHECK(!decode_release_policy(corrupt, decoded));
    corrupt = bytes; corrupt[4] = std::byte{169}; BLIP_CHECK(!decode_release_policy(corrupt, decoded));
    std::fill(corrupt.begin() + 332, corrupt.end(), std::byte{'x'}); BLIP_CHECK(!decode_release_policy(corrupt, decoded));
    BLIP_CHECK(!decode_release_policy(std::span<const std::byte>(bytes).first(347), decoded));
    BLIP_CHECK(policy.endpoint.assign("ftp://example.org")); BLIP_CHECK(!encode_release_policy(policy, bytes));
    policy = default_release_policy(); policy.interval_hours = 169; BLIP_CHECK(!encode_release_policy(policy, bytes));
    policy = default_release_policy(); BLIP_CHECK(policy.channel.assign("bad channel")); BLIP_CHECK(!encode_release_policy(policy, bytes));
    return true;
}
} // namespace
int main(int argc, char** argv) {
    if (argc == 2) {
        std::ifstream input(argv[1], std::ios::binary);
        if (!input) return 2;
        const std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        ReleaseCatalog catalog;
        return decode_release_catalog(json, catalog) && validate_release_catalog(catalog, identity, 3145728, 262144) ? 0 : 1;
    }
    const TestCase cases[]{
        {"owned catalog and exact compatibility", owned_decode_and_compatibility},
        {"strict catalog parsing and HTTPS URLs", malformed_input_and_urls_fail_closed},
        {"independent artifacts and public GET metadata", independent_artifacts_and_public_query},
        {"bounded persistent release policy", persisted_release_policy_is_bounded_and_canonical},
    };
    return run_tests(cases);
}
