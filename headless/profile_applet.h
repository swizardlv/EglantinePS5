// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "common/string_util.h"
#include "common/logging.h"
#include "core/frontend/applets/profile_select.h"
#include "system_keyboard.h"

namespace Eden {
struct ProfileAppletText {
    std::u16string create = u"Create user: enter a new name";
    std::u16string choose = u"Choose user: enter a name";
    std::u16string new_name = u"A new name creates a user";
};

class SystemProfileApplet final : public Core::Frontend::ProfileSelectApplet {
public:
    struct User { Common::UUID id; std::string name; };
    using Read = std::function<std::vector<User>()>;
    using Create = std::function<std::optional<Common::UUID>(const std::string&)>;
    using Ask = std::function<TextAnswer(const TextRequest&, const std::atomic<bool>&)>;

    SystemProfileApplet(Read read, Create create, Ask ask, ProfileAppletText text = {})
        : state{std::make_shared<State>()} {
        state->read = std::move(read);
        state->create = std::move(create);
        state->ask = std::move(ask);
        state->text = std::move(text);
    }
    ~SystemProfileApplet() override { Close(); }
    bool PreservesCancellation() const override { return true; }

    void Close() const override {
        const std::lock_guard lock(state->mutex);
        ++state->generation;
        if (state->stop) state->stop->store(true);
    }

    void SelectProfile(SelectProfileCallback callback,
                       const Core::Frontend::ProfileSelectParameters& parameters) const override {
        using Service::AM::Frontend::UiMode;
        std::shared_ptr<std::atomic<bool>> stop;
        std::uint64_t generation;
        std::vector<User> users;
        {
            const std::lock_guard lock(state->mutex);
            generation = ++state->generation;
            if (state->stop) state->stop->store(true);
            stop = state->stop = std::make_shared<std::atomic<bool>>(false);
            users = state->read();
        }
        const bool creator = parameters.mode == UiMode::UserCreator ||
                             parameters.mode == UiMode::UserCreatorForStarter;
        const bool can_create = users.size() < 8 &&
            (creator || (parameters.mode == UiMode::UserSelector &&
                         parameters.display_options.is_registration_permitted));
        std::erase_if(users, [&](const User& user) {
            return std::find(parameters.invalid_uid_list.begin(), parameters.invalid_uid_list.end(),
                             user.id) != parameters.invalid_uid_list.end();
        });
        LOG_INFO(Service_ACC, "PS5 profile request: mode={} eligible={} create={}",
                 static_cast<int>(parameters.mode), users.size(), can_create);
        std::thread([s = state, callback = std::move(callback), users = std::move(users),
                     creator, can_create, generation, stop] {
            TextRequest request;
            request.max_length = 31;
            request.title = creator ? s->text.create : s->text.choose;
            if (!creator) {
                std::string names;
                for (const auto& user : users) {
                    if (!names.empty()) names += ", ";
                    names += user.name;
                }
                request.placeholder = Common::UTF8ToUTF16(names);
                if (can_create) {
                    if (!names.empty()) request.placeholder += u"; ";
                    request.placeholder += s->text.new_name;
                }
                if (!users.empty()) request.initial = Common::UTF8ToUTF16(users.front().name);
            }
            std::optional<Common::UUID> selected;
            for (int attempt = 0; attempt < 3; ++attempt) {
                if ((creator && !can_create) || (!creator && users.empty() && !can_create)) break;
                const auto answer = s->ask(request, *stop);
                const std::lock_guard lock(s->mutex);
                if (s->generation != generation || stop->load()) return;
                if (answer.outcome != TextOutcome::accepted) break;
                std::string name = Common::UTF16ToUTF8(answer.text);
                const auto first = name.find_first_not_of(" \t\r\n");
                if (first == std::string::npos) continue;
                name = name.substr(first, name.find_last_not_of(" \t\r\n") - first + 1);
                if (name.size() > 31 || std::any_of(name.begin(), name.end(),
                        [](unsigned char c) { return c < 0x20; })) continue;
                const auto found = std::find_if(users.begin(), users.end(),
                    [&](const User& user) { return user.name == name; });
                if (!creator && found != users.end()) selected = found->id;
                else if (can_create) selected = s->create(name);
                if (selected && selected->IsValid()) break;
            }
            // The applet's completion callback can call Close() synchronously.
            const std::lock_guard lock(s->mutex);
            if (s->generation == generation && !stop->load()) callback(selected);
        }).detach();
    }

private:
    struct State {
        std::recursive_mutex mutex;
        Read read;
        Create create;
        Ask ask;
        ProfileAppletText text;
        std::uint64_t generation{};
        std::shared_ptr<std::atomic<bool>> stop;
    };
    std::shared_ptr<State> state;
};
} // namespace Eden
