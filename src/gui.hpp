#pragma once

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
    void onChannelsReceived(const std::vector<Channel> &channels);
    void onUserJoinVoiceChannel(const std::string &userName, const std::string &channelName);
    std::vector<Channel> getChannelList() const;

  private:
    std::unordered_map<std::string, Channel> channels;
    void clear();
};