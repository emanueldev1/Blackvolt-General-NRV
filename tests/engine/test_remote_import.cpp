#include "check.h"

#include "remote/ChiakiImport.h"
#include "remote/ConsoleStore.h"
#include "remote/QtSettingsCodec.h"
#include "remote/VeyraImport.h"

#include <cstring>
#include <filesystem>

using namespace bv::remote;

namespace {
std::vector<ConsoleProfile> fixture() {
    return chiaki::fromIniFile(std::filesystem::path(BV_SOURCE_DIR) / "tests/data/chiaki-export.ini");
}

std::vector<uint8_t> veyraRecord(uint8_t version, const std::string& host, const std::string& consoleId, uint8_t viewOnly = 0) {
    std::vector<uint8_t> b{'V', 'R', 'P', version};
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i))); };
    u32(static_cast<uint32_t>(host.size()));
    u32(1920); u32(1080); u32(60); u32(15000); u32(1);
    for (uint8_t i = 1; i <= 8; ++i) b.push_back(i);           // account id
    for (uint8_t i = 0; i < 16; ++i) b.push_back(0xA0 + i);    // regist key
    for (uint8_t i = 0; i < 16; ++i) b.push_back(0x30 + i);    // rp key
    b.insert(b.end(), host.begin(), host.end());
    if (version == 2) {
        b.push_back(viewOnly);
        b.push_back(static_cast<uint8_t>(consoleId.size()));
        b.insert(b.end(), consoleId.begin(), consoleId.end());
    }
    return b;
}
} // namespace

TEST(qt_ini_unescape_and_bytearray) {
    const auto v = qt::unescapeIniValue(R"x("@ByteArray(\0\x12\x7f\xa0[<)")x");
    const auto bytes = qt::decodeByteArray(v);
    CHECK(bytes.has_value());
    const std::vector<uint8_t> expected{0x00, 0x12, 0x7f, 0xa0, '[', '<'};
    CHECK(bytes && *bytes == expected);
    CHECK(!qt::decodeByteArray(u"plain text").has_value());
    CHECK(qt::toUtf8(qt::unescapeIniValue("\"PS4 Pro, cuarto\"")) == "PS4 Pro, cuarto");
}

TEST(chiaki_ini_imports_registered_hosts) {
    const auto hosts = fixture();
    CHECK(hosts.size() == 2);
    if (hosts.size() != 2) return;

    const auto& ps5 = hosts[0];
    CHECK(ps5.nickname == "PS5-Sala");
    CHECK(ps5.macHex() == "00127fa05b3c");
    CHECK(ps5.kind == ConsoleKind::PS5);
    CHECK(ps5.target == 1000100);
    CHECK(ps5.rpKeyType == 2);
    CHECK(ps5.host == "192.168.1.50");
    CHECK(std::memcmp(ps5.registKey.data(), "a1b2c3d4\0\0\0\0\0\0\0\0", 16) == 0);
    for (int i = 0; i < 16; ++i) CHECK(ps5.rpKey[i] == 0x10 + i);
    CHECK(ps5.hasAccountId);
    for (int i = 0; i < 8; ++i) CHECK(ps5.psnAccountId[i] == i + 1);

    const auto& ps4 = hosts[1];
    CHECK(ps4.nickname == "PS4 Pro, cuarto");
    CHECK(ps4.macHex() == "f8461c0a0b0c");
    CHECK(ps4.kind == ConsoleKind::PS4);
    CHECK(ps4.target == 1000);
    CHECK(ps4.host.empty());
    const uint8_t regist[16] = {0xff, 0, 0x7f, 0x80, '"', '\\', ',', '=', 'x', 'y', 'z', 1, 2, 3, 4, 5};
    CHECK(std::memcmp(ps4.registKey.data(), regist, 16) == 0);
    CHECK(ps4.rpKey[8] == '\n' && ps4.rpKey[9] == '\r' && ps4.rpKey[14] == 0xc3 && ps4.rpKey[15] == 0xa9);
}

TEST(chiaki_rejects_incomplete_hosts) {
    const auto none = chiaki::fromIniText("[registered_hosts]\n1\\server_mac=@ByteArray(abc)\nsize=1\n");
    CHECK(none.empty());
}

TEST(veyra_record_v1_and_v2) {
    const auto v1 = veyraRecord(1, "192.168.1.77", "");
    auto p1 = veyra::parseRecord(v1);
    CHECK(p1.has_value());
    if (p1) {
        CHECK(p1->host == "192.168.1.77");
        CHECK(!p1->hasMac);
        CHECK(p1->registKey[0] == 0xA0 && p1->rpKey[15] == 0x3F && p1->psnAccountId[7] == 8);
        CHECK(p1->prefs && p1->prefs->width == 1920 && p1->prefs->fps == 60 && p1->prefs->codec == 1);
    }
    const auto v2 = veyraRecord(2, "ps5.local", "00127fa05b3c", 1);
    auto p2 = veyra::parseRecord(v2);
    CHECK(p2.has_value());
    if (p2) {
        CHECK(p2->hasMac && p2->macHex() == "00127fa05b3c");
        CHECK(p2->prefs && p2->prefs->viewOnly);
    }
}

TEST(veyra_record_rejects_garbage) {
    auto bad = veyraRecord(2, "h", "00127fa05b3c");
    bad[3] = 9;
    CHECK(!veyra::parseRecord(bad));
    auto truncated = veyraRecord(1, "192.168.1.77", "");
    truncated.resize(truncated.size() - 3);
    CHECK(!veyra::parseRecord(truncated));
    auto badCodec = veyraRecord(1, "h", "");
    badCodec[24] = 7;
    CHECK(!veyra::parseRecord(badCodec));
    auto badId = veyraRecord(2, "h", "zz127fa05b3c");
    CHECK(!veyra::parseRecord(badId));
}

TEST(merge_dedupes_same_console) {
    std::vector<ConsoleProfile> store = fixture();
    auto veyraPs5 = veyra::parseRecord(veyraRecord(2, "192.168.1.60", "00127fa05b3c"));
    CHECK(veyraPs5.has_value());
    if (!veyraPs5) return;
    CHECK(mergeProfiles(store, {*veyraPs5}) == 0);
    CHECK(store.size() == 2);
    CHECK(store[0].host == "192.168.1.60");        // newer address wins
    CHECK(store[0].nickname == "PS5 (192.168.1.60)");
    CHECK(store[0].registKey[0] == 0xA0);          // re-paired keys win
}

TEST(console_store_roundtrip) {
    const auto dir = std::filesystem::temp_directory_path() / "bv_console_store_test";
    std::filesystem::remove_all(dir);
    ConsoleStore store(dir / "consoles.dat");
    CHECK(store.import(fixture()) == 2);
    CHECK(store.save());

    ConsoleStore again(dir / "consoles.dat");
    CHECK(again.load());
    CHECK(again.consoles().size() == 2);
    if (again.consoles().size() == 2) {
        const auto& a = store.consoles()[1];
        const auto& b = again.consoles()[1];
        CHECK(a.nickname == b.nickname && a.mac == b.mac && a.registKey == b.registKey && a.rpKey == b.rpKey);
        CHECK(b.kind == ConsoleKind::PS4 && b.target == 1000 && b.hasAccountId);
    }
    CHECK(again.remove(0) && again.consoles().size() == 1);
    std::filesystem::remove_all(dir);
}
