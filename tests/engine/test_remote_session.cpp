#include "check.h"

#include "blackvolt/bv_api.h"
#include "remote/ChiakiImport.h"
#include "remote/Discovery.h"
#include "remote/Registration.h"
#include "remote/StreamSession.h"
#include "telemetry/Telemetry.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

using namespace bv::remote;
using namespace std::chrono_literals;

namespace {

class NullPresenter final : public bv::player::IVideoPresenter {
public:
    bool present(const AVFrame&) override { return true; }
};

template <class Pred>
bool waitFor(Pred pred, std::chrono::milliseconds timeout) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return pred();
}

std::filesystem::path fixturePath() { return std::filesystem::path(BV_SOURCE_DIR) / "tests/data/chiaki-export.ini"; }

} // namespace

TEST(wake_credential_from_regist_key) {
    const auto hosts = chiaki::fromIniFile(fixturePath());
    CHECK(hosts.size() == 2);
    if (hosts.size() != 2) return;
    uint64_t cred = 0;
    CHECK(wakeCredential(hosts[0], cred) && cred == 0xa1b2c3d4ull);
    CHECK(!wakeCredential(hosts[1], cred)); // binary key: not a wake credential
}

TEST(account_id_formats) {
    std::array<uint8_t, 8> id{};
    CHECK(parseAccountId("AQIDBAUGBwg=", id) && id[0] == 1 && id[7] == 8);
    CHECK(parseAccountId("0102030405060708", id) && id[0] == 1 && id[7] == 8);
    CHECK(parseAccountId(" 578437695752307201 ", id) && id[0] == 1 && id[7] == 8); // 0x0807060504030201
    CHECK(!parseAccountId("not an id", id));
    CHECK(!parseAccountId("AQID", id));
}

TEST(discovery_starts_and_stops) {
    Discovery d;
    std::string err;
    CHECK(d.start({"127.0.0.1"}, &err));
    CHECK(d.running());
    std::this_thread::sleep_for(150ms);
    CHECK(d.consoles().empty());
    d.stop();
    CHECK(!d.running());
}

TEST(registration_fails_cleanly_without_console) {
    Registration r;
    RegistrationRequest req;
    req.host = "127.0.0.1"; // nothing listens on the Remote Play ports here
    req.pin = 12345678;
    std::string err;
    CHECK(r.start(req, &err));
    CHECK(waitFor([&] { return r.state() != Registration::State::Running; }, 20s));
    CHECK(r.state() == Registration::State::Failed || r.state() == Registration::State::Canceled);
}

TEST(stream_session_reports_unreachable_console) {
    const auto hosts = chiaki::fromIniFile(fixturePath());
    if (hosts.empty()) return;
    auto console = hosts[0];
    console.host = "127.0.0.1";
    bv::telemetry::Telemetry telemetry;
    NullPresenter presenter;
    StreamSession session(nullptr, presenter, nullptr, nullptr, telemetry);
    StreamSettings settings;
    settings.autoReconnect = false;
    settings.wakeIfAsleep = false;
    session.start(console, settings);
    CHECK(waitFor([&] { return session.snapshot().state == StreamState::Error; }, 20s));
    CHECK(!session.snapshot().error.empty());
    session.stop();
    CHECK(session.snapshot().state == StreamState::Error);
}

TEST(stream_session_stop_while_connecting) {
    const auto hosts = chiaki::fromIniFile(fixturePath());
    if (hosts.empty()) return;
    auto console = hosts[0];
    console.host = "192.0.2.1"; // TEST-NET-1: never answers
    bv::telemetry::Telemetry telemetry;
    NullPresenter presenter;
    StreamSession session(nullptr, presenter, nullptr, nullptr, telemetry);
    session.start(console, StreamSettings{});
    std::this_thread::sleep_for(200ms);
    const auto t0 = std::chrono::steady_clock::now();
    session.stop();
    CHECK(std::chrono::steady_clock::now() - t0 < 5s); // stop must not hang on a dead host
}

TEST(c_api_console_store) {
    const auto dir = std::filesystem::temp_directory_path() / "bv_api_consoles";
    std::filesystem::remove_all(dir);
    const std::string store = (dir / "consoles.dat").string();
    bv_consoles* c = nullptr;
    CHECK(bv_consoles_open(store.c_str(), &c) == BV_OK && c);
    if (!c) return;
    CHECK(bv_consoles_count(c) == 0);
    uint32_t added = 0;
    CHECK(bv_consoles_import_path(c, fixturePath().string().c_str(), &added) == BV_OK && added == 2);
    CHECK(bv_consoles_import_path(c, fixturePath().string().c_str(), &added) == BV_OK && added == 0);
    bv_console_info info{};
    CHECK(bv_consoles_get(c, 0, &info) == BV_OK);
    CHECK(std::string(info.nickname) == "PS5-Sala" && std::string(info.console_id) == "00127fa05b3c");
    CHECK(info.kind == BV_CONSOLE_PS5 && info.has_account_id == 1);
    CHECK(bv_consoles_update(c, 1, "Cuarto", "192.168.1.99") == BV_OK);
    bv_consoles_close(c);

    c = nullptr;
    CHECK(bv_consoles_open(store.c_str(), &c) == BV_OK && c);
    if (!c) return;
    CHECK(bv_consoles_count(c) == 2);
    CHECK(bv_consoles_get(c, 1, &info) == BV_OK && std::string(info.nickname) == "Cuarto" &&
          std::string(info.host) == "192.168.1.99");
    CHECK(bv_consoles_remove(c, 0) == BV_OK && bv_consoles_count(c) == 1);
    CHECK(bv_consoles_get(c, 5, &info) == BV_ERR_INVALID_ARGUMENT);

    bv_discovery* d = nullptr;
    CHECK(bv_discovery_start(c, &d) == BV_OK && d);
    uint32_t count = 99;
    bv_discovered_console items[4];
    CHECK(bv_discovery_list(d, items, 4, &count) == BV_OK && count == 0);
    bv_discovery_stop(d);

    bv_pairing* p = nullptr;
    CHECK(bv_pairing_start(c, "127.0.0.1", BV_CONSOLE_PS5, "garbage", 12345678, &p) == BV_ERR_INVALID_ARGUMENT);
    CHECK(bv_pairing_start(c, "127.0.0.1", BV_CONSOLE_PS5, nullptr, 123456789, &p) == BV_ERR_INVALID_ARGUMENT);
    bv_consoles_close(c);
    std::filesystem::remove_all(dir);
}
