#include "blip/storage/device_identity_component.hpp"
#include "blip/resources/board_manifest.hpp"
#include "test_harness.hpp"
#include <map>
#include <string>
#include <vector>
#include <algorithm>

namespace {
using namespace blip;
class MemoryBlobs final : public storage::BlobStore {
  public:
    std::map<std::string, std::vector<std::byte>> values;
    bool fail{};
    core::Result<std::size_t> read(std::string_view key, std::span<std::byte> out) noexcept override {
        const auto it = values.find(std::string{key});
        if (it == values.end()) return core::Result<std::size_t>::failure(
            {core::ErrorDomain::storage, core::ErrorCode::not_found, {}, "read", "missing"});
        if (it->second.size() > out.size()) return core::Result<std::size_t>::failure(
            {core::ErrorDomain::storage, core::ErrorCode::capacity_exceeded, {}, "read", "size"});
        std::copy(it->second.begin(), it->second.end(), out.begin());
        return core::Result<std::size_t>::success(it->second.size());
    }
    core::Status write(std::string_view key, std::span<const std::byte> data) noexcept override {
        if (fail) return core::Status::failure({core::ErrorDomain::storage,
            core::ErrorCode::storage_full, {}, "write", "full"});
        values[std::string{key}] = {data.begin(), data.end()}; return core::Status::success();
    }
};
bool saved_identity_and_owned_reads() {
    MemoryBlobs blobs; std::array<std::byte, 512> scratch{};
    storage::SettingsStore settings{blobs, scratch};
    storage::DeviceIdentityComponent identity{settings, "Creators Club"};
    BLIP_CHECK(identity.start({}));
    BLIP_CHECK(std::string_view{identity.snapshot().name.data()} == "Creators Club");
    BLIP_CHECK(identity.type() == "Creators Club");
    BLIP_CHECK(identity.descriptor().parameters[0].default_value.string == "Creators Club");
    const auto before = identity.snapshot();
    BLIP_CHECK(identity.write_parameter("name", core::ScalarValue::from_string("Stage Left")));
    BLIP_CHECK(identity.snapshot().revision != before.revision);
    std::array<char, 64> owned{}; core::ScalarValue value;
    BLIP_CHECK(identity.read_parameter_owned("name", value, owned));
    BLIP_CHECK(value.string == "Stage Left");
    BLIP_CHECK(identity.write_parameter("name", core::ScalarValue::from_string("Stage Right")));
    BLIP_CHECK(value.string == "Stage Left");
    BLIP_CHECK(identity.type() == "Creators Club");
    BLIP_CHECK(identity.stop());
    storage::DeviceIdentityComponent rebooted{settings, "Creators Club"};
    BLIP_CHECK(rebooted.start({}));
    BLIP_CHECK(std::string_view{rebooted.snapshot().name.data()} == "Stage Right");
    blobs.fail = true;
    BLIP_CHECK(!rebooted.write_parameter("name", core::ScalarValue::from_string("Failed save")));
    BLIP_CHECK(std::string_view{rebooted.snapshot().name.data()} == "Stage Right");
    return true;
}
bool validation_and_dns_labels() {
    MemoryBlobs blobs; std::array<std::byte, 512> scratch{};
    storage::SettingsStore settings{blobs, scratch}; storage::DeviceIdentityComponent identity{settings, "M5 Dial"};
    BLIP_CHECK(identity.start({}));
    const std::array<std::string_view, 9> invalid{"", " name", "name ", "line\nfeed", "\x7f", "\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xc2\x85"};
    for (const auto name : invalid) BLIP_CHECK(!identity.write_parameter("name", core::ScalarValue::from_string(name)));
    BLIP_CHECK(!identity.write_parameter("name", core::ScalarValue::from_string(std::string(64, 'a'))));
    BLIP_CHECK(!identity.write_parameter("type", core::ScalarValue::from_string("Other")));
    BLIP_CHECK(identity.write_parameter("name", core::ScalarValue::from_string("Sc\xc3\xa8ne gauche")));
    std::array<char, 64> host{}; constexpr std::array<std::uint8_t, 6> mac{0x30, 0xae, 0xa4, 0xf3, 0xa3, 0x88};
    BLIP_CHECK(storage::device_hostname("Creators Club", mac, host));
    BLIP_CHECK(std::string_view{host.data()} == "creators-club-30aea4f3a388");
    BLIP_CHECK(storage::device_hostname("  INVALID", mac, host).ok() == false);
    BLIP_CHECK(storage::device_hostname("Main / Stage...Left!", mac, host));
    BLIP_CHECK(std::string_view{host.data()} == "main-stage-left-30aea4f3a388");
    BLIP_CHECK(storage::device_hostname(std::string(63, 'A'), mac, host));
    BLIP_CHECK(std::string_view{host.data()}.size() == 63);
    BLIP_CHECK(host[49] == 'a' && host[50] == '-');
    BLIP_CHECK(storage::device_hostname("\xe7\x81\xaf", mac, host));
    BLIP_CHECK(std::string_view{host.data()} == "blip-30aea4f3a388");
    BLIP_CHECK(!storage::device_hostname("name", mac, std::span{host}.first(63)));
    return true;
}
bool required_board_type_names() {
    constexpr std::array<std::string_view, 6> ids{"creators-club", "creators-ball-v2", "adafruit-huzzah32", "seeed-xiao-esp32c6-chip-antenna", "m5stack-m5stickc", "m5stack-m5dial"};
    constexpr std::array<std::string_view, 6> names{"Creators Club", "Creators Ball V2", "HUZZAH32", "XIAO C6", "M5 Stick C", "M5 Dial"};
    for (std::size_t i = 0; i < ids.size(); ++i) {
        resources::BoardManifest board{}; board.id = ids[i];
        BLIP_CHECK(resources::board_display_name(board) == names[i]);
    }
    return true;
}
}
int main() {
    const TestCase tests[]{{"saved identity and owned reads", saved_identity_and_owned_reads},
        {"name validation and stable DNS labels", validation_and_dns_labels}, {"six required board type names", required_board_type_names}};
    return run_tests(tests);
}
