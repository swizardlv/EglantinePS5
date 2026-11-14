// SPDX-License-Identifier: GPL-3.0-or-later
#include "profile_backend.h"
#include "common/fs/path_util.h"
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>

namespace Core::Frontend { ProfileSelectApplet::~ProfileSelectApplet() = default; }

int main() {
    using namespace Eden;
    using Service::AM::Frontend::UiMode;
    char directory[] = "/tmp/eden-profile-applet-XXXXXX";
    assert(mkdtemp(directory));
    const std::filesystem::path root{directory};
    const auto file = (root / "nand/system/save/8000000000000010/su/avators/profiles.dat").string();
    const auto settings = (root / "settings.json").string();
    assert(Profiles::Write({Profiles::Make("Existing")}, file));
    Common::FS::SetEdenPath(Common::FS::EdenPath::NANDDir, root / "nand");
    Service::Account::ProfileManager manager;
    const auto original = manager.GetUser(0).value();
    const auto create = [&](const std::string& name) { return CreateProfileUser(manager, name, file, settings); };
    const auto read = [&] { return ReadProfileUsers(manager); };
    assert(!create("Existing"));
    assert(!create(" "));
    assert(!CreateProfileUser(manager, "Missing", file + ".absent", settings));
    assert(manager.GetUserCount() == 1);
    const auto bytes = [&] {
        std::ifstream in(file, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };
    const auto original_bytes = bytes();
    std::filesystem::create_directory(file + ".new");
    assert(!create("Cannot publish"));
    assert(bytes() == original_bytes && manager.GetUserCount() == 1);
    std::filesystem::remove_all(file + ".new");
    std::filesystem::create_directory(settings + ".tmp");
    assert(!create("Cannot seed settings"));
    assert(bytes() == original_bytes && manager.GetUserCount() == 1);
    std::filesystem::remove_all(settings + ".tmp");

    auto run = [&](Core::Frontend::ProfileSelectParameters parameters, TextAnswer answer,
                   std::optional<Common::UUID> expected, bool expect_new = false) {
        std::promise<std::optional<Common::UUID>> done;
        auto future = done.get_future();
        SystemProfileApplet app(read, create, [answer](const auto&, const auto&) { return answer; });
        assert(app.PreservesCancellation());
        app.SelectProfile([&](auto result) { app.Close(); done.set_value(result); }, parameters);
        assert(future.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
        const auto result = future.get();
        if (expect_new) assert(result && *result != original && manager.UserExists(*result));
        else assert(result == expected);
        return result;
    };
    Core::Frontend::ProfileSelectParameters select{};
    select.mode = UiMode::UserSelector;
    run(select, {TextOutcome::accepted, u"Existing"}, original);
    run(select, {TextOutcome::cancelled, {}}, std::nullopt);
    run(select, {TextOutcome::unavailable, {}}, std::nullopt);
    select.invalid_uid_list[0] = original;
    run(select, {TextOutcome::accepted, u"Existing"}, std::nullopt);
    assert(manager.GetUserCount() == 1);
    select.display_options.is_registration_permitted = true;
    const auto second = run(select, {TextOutcome::accepted, u"New player"}, std::nullopt, true).value();
    assert(manager.GetUser(0) == original && manager.GetUserCount() == 2);
    Service::Account::ProfileManager restarted;
    assert(restarted.UserExists(second) && restarted.UserExists(original));
    Core::Frontend::ProfileSelectParameters creator{};
    creator.mode = UiMode::UserCreator;
    run(creator, {TextOutcome::accepted, u"中文"}, std::nullopt, true);
    run(creator, {TextOutcome::accepted, u"Existing"}, std::nullopt);
    assert(manager.GetUserCount() == 3);
    while (manager.GetUserCount() < 8) assert(create("Extra " + std::to_string(manager.GetUserCount())));
    run(creator, {TextOutcome::accepted, u"Overflow"}, std::nullopt);
    assert(manager.GetUserCount() == 8 && Profiles::Read(file).size() == 8);

    std::promise<void> opened;
    auto entered = opened.get_future();
    std::promise<void> finished;
    auto stopped = finished.get_future();
    std::atomic<bool> replied{};
    SystemProfileApplet app(read, create, [&](const auto&, const auto& stop) {
        opened.set_value();
        while (!stop.load()) std::this_thread::yield();
        finished.set_value();
        return TextAnswer{TextOutcome::accepted, u"Existing"};
    });
    select.invalid_uid_list = {};
    app.SelectProfile([&](auto) { replied = true; }, select);
    entered.wait();
    app.Close();
    assert(stopped.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
    assert(!replied);
    std::filesystem::remove_all(root);
    std::cout << "Profile applet: selection, exclusions, creation, persistence, cancellation, capacity and close PASS\n";
}
