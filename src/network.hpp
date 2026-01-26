#pragma once

#include <cstring>
#include <iostream>
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
#include "auth.hpp"

#include <QObject>
#include <QString>
#include <QStringList>

using json = nlohmann::json;

class Audio;
class Auth;

class Network : public QObject {
    Q_OBJECT

    public:
        Network();
        Network(Audio& audio);
        Network(Audio& audio, Auth& auth);
        void connectToServer(QString address, QString port);
        void disconnectQUIC();
        void sendVoicePackets(std::vector<uint8_t> encodedData);
        void joinVoiceChannel(QString channelName);
        bool isConnected() const { return connected; }
        void sendAdminMessage(const QString& requestType);

    signals:
        void channelsReceived(const QStringList& channels);
        void userJoinedChannel(const QString& user, const QString& channel);
        void authenticationFailed(const QString& reason);
        void adminResponseReceived(const QString& request, const QString& jsonData);


    private:
        int sockfd;
        std::jthread recvEventThread;
        std::jthread recvVoiceThread;
        std::jthread sendVoiceThread;
        std::jthread heartbeatThread;
        SSL *authStream = nullptr;
        SSL *eventStream = nullptr;
        SSL *voiceStream = nullptr;
        bool connected = false;
        SSL_CTX *ctx = nullptr;
        SSL *ssl = nullptr;
        BIO *bio = nullptr;
        void initOpenssl();
        void connectQUIC(QString address, QString port);
        void sendMessage(SSL *stream);
        void shutdown_ssl(SSL *ssl);
        void sendHeartbeat();
        void receiveEventPackets();
        void receiveVoicePackets();
        void handleEventPacket(char *buf, size_t bufsize);
        void handleEventMessage(std::string msg); 
        bool performAuthentication();
        static BIO *create_socket_bio(const char *hostname, const char *port, int family, BIO_ADDR **peer_addr);
        Audio* audioManager;
        Auth* authManager;
};