#include "gui.hpp"
#include <algorithm>
#include <print>

void GUI::setOnStateChanged(std::function<void()> callback) {
    onStateChanged = std::move(callback);
}

void GUI::notifyStateChanged() {
    if (onStateChanged) {
        onStateChanged();
    }
}

void GUI::onChannelsReceived(const std::vector<Channel> &chs) {
    {
        std::lock_guard lock(mutex);
        clear();
        for (const auto &c : chs) {
            if (c.name.empty()) {
                continue;
            }
            channels.emplace(c.name, c);
        }
    }
    notifyStateChanged();
}

std::vector<Channel> GUI::getChannelList() const {
    std::lock_guard lock(mutex);
    std::vector<Channel> out;
    out.reserve(channels.size());
    for (const auto &p : channels)
        out.push_back(p.second);
    return out;
}

void GUI::clear() { channels.clear(); }

void GUI::onUserJoinVoiceChannel(const std::string &userName, const std::string &channelName) {
    if (channelName.empty() || userName.empty()) {
        return;
    }

    {
        std::lock_guard lock(mutex);
        auto &channel = channels[channelName];
        if (channel.name.empty()) {
            channel.name = channelName;
        }

        const auto userIt = std::find_if(channel.users.begin(), channel.users.end(), [&](const User &user) {
            return user.username == userName;
        });
        if (userIt == channel.users.end()) {
            channel.users.push_back(User{userName});
        }
    }
    notifyStateChanged();
}
