// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "core/hle/service/acc/profile_manager.h"
#include "profile_applet.h"
#include "profiles.h"

namespace Eden {
inline std::vector<SystemProfileApplet::User> ReadProfileUsers(
        Service::Account::ProfileManager& manager) {
    const auto lock = manager.Lock();
    std::vector<SystemProfileApplet::User> users;
    for (const auto& id : manager.GetAllUsers()) {
        Service::Account::ProfileBase base{};
        if (id.IsValid() && manager.GetProfileBase(id, base))
            users.push_back({id, std::string(base.username.begin(),
                std::find(base.username.begin(), base.username.end(), u8{}))});
    }
    return users;
}

inline std::optional<Common::UUID> CreateProfileUser(Service::Account::ProfileManager& manager,
        const std::string& name, const std::string& file = Profiles::File(),
        const std::string& settings = SettingsFile()) {
    const auto lock = manager.Lock();
    if (manager.GetUserCount() >= Profiles::kMax || manager.GetUserIndex(name)) return std::nullopt;
    const auto before = Profiles::Read(file);
    if (before.size() != manager.GetUserCount()) return std::nullopt;
    for (const auto& user : before)
        if (!manager.UserExists(Common::UUID{user.id})) return std::nullopt;
    const int index = Profiles::Add(name, file, settings);
    if (index < 0) return std::nullopt;
    const auto profiles = Profiles::Read(file);
    if (static_cast<std::size_t>(index) >= profiles.size()) return std::nullopt;
    const auto& profile = profiles[static_cast<std::size_t>(index)];
    const Common::UUID id{profile.id};
    if (manager.CreateNewUser(id, profile.name).IsError()) return std::nullopt;
    return id;
}
} // namespace Eden
