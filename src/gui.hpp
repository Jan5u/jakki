#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct User {
    std::string username;
};

struct Channel {
    std::string name;
    std::vector<User> users;
};

class GUI {
  public:
    GUI() = default;
    void setOnStateChanged(std::function<void()> callback);
    void onChannelsReceived(const std::vector<Channel> &channels);
    void onUserJoinVoiceChannel(const std::string &userName, const std::string &channelName);
    std::vector<Channel> getChannelList() const;

  private:
    std::unordered_map<std::string, Channel> channels;
    std::function<void()> onStateChanged;
    mutable std::mutex mutex;
    void clear();
    void notifyStateChanged();
};