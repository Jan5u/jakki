#pragma once

#include <print>
#include <string>
#include <expected>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif
#include <nlohmann/json.hpp>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>



#include "audio/audio.hpp"
class Auth;
class GUI;
class Audio;
class Video;

using json = nlohmann::json;

class Network {
    public:
        Network();
        // Network(Audio& audio);
        // Network(Audio& audio, Auth& auth);
        void connectToServer(std::string& address, std::string& port);
        void setAuthManager(Auth* auth);
        void setGUI(GUI* gui);
        void setAudio(Audio* audio);
        void setVideo(Video* video);

        // void disconnectQUIC();
        void sendVoicePackets(std::vector<uint8_t> encodedData);
        void sendScreensharePackets(std::vector<uint8_t> encodedData);
        void joinVoiceChannel(const std::string& channelName);
        void leaveVoiceChannel();
        // bool isInVoiceChannel() const { return inVoiceChannel; }
        void joinScreenShare(std::string userName);
        // bool isConnected() const { return connected; }
        // void sendAdminMessage(const QString& requestType);
        // void sendTextMessage(const QString& jsonMessage);
        // void requestUserList();
        // void setVideoManager(Video* video);

    // signals:
    //     void channelsReceived(const QStringList& channels);
    //     void userJoinedChannel(const QString& user, const QString& channel);
    //     void userLeftChannel(const QString& user, const QString& channel);
    //     void authenticationFailed(const QString& reason);
    //     void adminResponseReceived(const QString& request, const QString& jsonData);
    //     void textMessageReceived(const QString& channel, const QString& sender, const QString& content, bool compressed);
    //     void historyResponseReceived(const QString& channel, const QJsonArray& messages);
    //     void emoteListReceived(const QJsonArray& emotes);
    //     void typingIndicatorReceived(const QString& channel, const QString& user);
    //     void usersListReceived(const QStringList& onlineUsers, const QStringList& offlineUsers);
    //     void userStatusChanged(const QString& user, bool online);


    private:
        std::jthread recvEventThread;
        std::jthread recvVoiceThread;
        std::jthread recvScreenshareThread;
        std::jthread sendVoiceThread;
        std::jthread heartbeatThread;
        SSL *authStream = nullptr;
        SSL *eventStream = nullptr;
        SSL *voiceStream = nullptr;
        SSL *streamScreenshareSend = nullptr;
        SSL *streamScreenshareRecv = nullptr;
        bool connected = false;
        SSL_CTX *ctx = nullptr;
        SSL *ssl = nullptr;
        BIO *bio = nullptr;
        auto connectQUIC(std::string& address, std::string& port) -> std::expected<void, std::string>;
        void sendMessage(SSL *stream);
        // void shutdown_ssl(SSL *ssl);
        void sendHeartbeat();
        void receiveEventPackets();
        void receiveVoicePackets();
        void receiveScreensharePackets();
        void handleEventMessage(std::string msg); 
        bool performAuthentication();
        static BIO *create_socket_bio(const char *hostname, const char *port, int family, BIO_ADDR **peer_addr);
        // Audio* audioManager;
        Auth* authManager = nullptr;
        GUI* gui = nullptr;
        Audio* audio = nullptr;
        Video* video = nullptr;
        // Video* videoManager = nullptr;
        bool inVoiceChannel = false;
};