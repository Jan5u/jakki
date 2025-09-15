#pragma once

#include <iostream>
#include <cstring>
#include <thread>
#ifdef _WIN32
# include <winsock2.h>
#else
# include <sys/socket.h>
#endif
#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <nlohmann/json.hpp>
#include "audio/audio.hpp"

#include <QObject>
#include <QString>
#include <QStringList>

using json = nlohmann::json;

class Audio;

class Network : public QObject {
    Q_OBJECT

    public:
        Network();
        Network(Audio& audio);
        void connectToServer(QString address, QString port);
        void disconnectQUIC();
        void sendVoicePackets(std::vector<uint8_t> encodedData);
        void joinVoiceChannel(QString channelName);

    signals:
        void channelsReceived(const QStringList& channels);
        void userJoinedChannel(const QString& user, const QString& channel);


    private:
        int sockfd;
        std::jthread recvEventThread;
        std::jthread recvVoiceThread;
        std::jthread sendVoiceThread;
        std::jthread heartbeatThread;
        SSL *stream1 = nullptr;
        SSL *stream2 = nullptr;
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
        static BIO *create_socket_bio(const char *hostname, const char *port, int family, BIO_ADDR **peer_addr);
        Audio* audioManager;
};