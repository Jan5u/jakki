#include "network.hpp"
#include "video/video.hpp"
#include <QJsonObject>

enum class EventType { ServerInfo, Message, UserJoin, AdminResponse, HistoryResponse, EmoteListResponse, TypingIndicator, UserList, UserStatusChange, Unknown };

Network::Network() : audioManager(nullptr), authManager(nullptr) {}

Network::Network(Audio &audio) : audioManager(&audio), authManager(nullptr) {}

Network::Network(Audio &audio, Auth &auth) : audioManager(&audio), authManager(&auth) {}

void Network::connectToServer(QString address, QString port) {
    std::cout << "connectToServer\n";

    if (connected) {
        std::cout << "Already connected, skipping connectQUIC\n";
        return;
    }

    // Load or generate authentication keys
    if (authManager) {
        if (!authManager->loadOrGenerateKeys()) {
            std::cerr << "Failed to load or generate authentication keys\n";
            emit authenticationFailed("Failed to load or generate authentication keys");
            return;
        }
    }

    // init openssl
    initOpenssl();

    // connect QUIC
    connectQUIC(address, port);
}

void Network::initOpenssl() {
    std::cout << "initOpenssl\n";
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();
}

BIO *Network::create_socket_bio(const char *hostname, const char *port, int family, BIO_ADDR **peer_addr) {
    int sock = -1;
    BIO_ADDRINFO *res;
    const BIO_ADDRINFO *ai = NULL;
    BIO *bio;

    /*
     * Lookup IP address info for the server.
     */
    if (!BIO_lookup_ex(hostname, port, BIO_LOOKUP_CLIENT, family, SOCK_DGRAM, 0, &res))
        return NULL;

    /*
     * Loop through all the possible addresses for the server and find one
     * we can connect to.
     */
    for (ai = res; ai != NULL; ai = BIO_ADDRINFO_next(ai)) {
        /*
         * Create a UDP socket. We could equally use non-OpenSSL calls such
         * as "socket" here for this and the subsequent connect and close
         * functions. But for portability reasons and also so that we get
         * errors on the OpenSSL stack in the event of a failure we use
         * OpenSSL's versions of these functions.
         */
        sock = BIO_socket(BIO_ADDRINFO_family(ai), SOCK_DGRAM, 0, 0);
        if (sock == -1)
            continue;
        /* Connect the socket to the server's address */
        if (!BIO_connect(sock, BIO_ADDRINFO_address(ai), 0)) {
            BIO_closesocket(sock);
            sock = -1;
            continue;
        }

        /* Set to nonblocking mode */
        if (!BIO_socket_nbio(sock, 1)) {
            BIO_closesocket(sock);
            sock = -1;
            continue;
        }

        break;
    }

    if (sock != -1) {
        *peer_addr = BIO_ADDR_dup(BIO_ADDRINFO_address(ai));
        if (*peer_addr == NULL) {
            BIO_closesocket(sock);
            return NULL;
        }
    }

    /* Free the address information resources we allocated earlier */
    BIO_ADDRINFO_free(res);

    /* If sock is -1 then we've been unable to connect to the server */
    if (sock == -1)
        return NULL;

    /* Create a BIO to wrap the socket */
    bio = BIO_new(BIO_s_datagram());
    if (bio == NULL) {
        BIO_closesocket(sock);
        return NULL;
    }

    /*
     * Associate the newly created BIO with the underlying socket. By
     * passing BIO_CLOSE here the socket will be automatically closed when
     * the BIO is freed. Alternatively you can use BIO_NOCLOSE, in which
     * case you must close the socket explicitly when it is no longer
     * needed.
     */
    BIO_set_fd(bio, sock, BIO_CLOSE);

    return bio;
}

void Network::connectQUIC(QString address, QString port) {
    std::cout << "connectQUIC\n";
    const SSL_METHOD *method = OSSL_QUIC_client_method();
    // const SSL_METHOD *method = OSSL_QUIC_client_thread_method();

    ctx = SSL_CTX_new(method);
    if (ctx == nullptr) {
        std::cerr << "Failed to create the SSL_CTX\n";
    }
    ssl = SSL_new(ctx);
    if (ssl == nullptr) {
        std::cerr << "Failed to create the SSL object\n";
    }

    SSL_set_incoming_stream_policy(ssl, SSL_INCOMING_STREAM_POLICY_ACCEPT, 0);

    BIO_ADDR *peer_addr = nullptr;
    QByteArray addressUtf8 = address.toUtf8();
    QByteArray portUtf8 = port.toUtf8();
    const char *SERVER_IP = addressUtf8.constData();
    const char *SERVER_PORT = portUtf8.constData();
    bio = create_socket_bio(SERVER_IP, SERVER_PORT, AF_INET, &peer_addr);
    if (!bio) {
        std::cerr << "Failed to create and connect BIO\n";
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        return;
    }
    SSL_set_bio(ssl, bio, bio);

    unsigned char alpn[] = {5, 'j', 'a', 'k', 'k', 'i'};
    if (SSL_set_alpn_protos(ssl, alpn, sizeof(alpn)) != 0) {
        std::cerr << "Failed to set the ALPN for the connection\n";
    }

    if (!SSL_set_default_stream_mode(ssl, SSL_DEFAULT_STREAM_MODE_NONE)) {
        std::cerr << "Failed to set the default stream mode\n";
    }

    if (SSL_connect(ssl) < 1) {
        std::cerr << "Failed to connect to the server\n";
    }
    connected = true;

    // Accept auth stream from server
    std::cout << "Waiting for auth stream from server...\n";
    authStream = SSL_accept_stream(ssl, 0);
    if (!authStream) {
        std::cerr << "Failed to accept auth stream from server\n";
        shutdown_ssl(ssl);
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        connected = false;
        emit authenticationFailed("Failed to accept auth stream from server");
        return;
    }
    std::cout << "Auth stream accepted from server\n";

    eventStream = SSL_new_stream(ssl, 0);
    voiceStream = SSL_new_stream(ssl, 0);

    streamScreenshareSend = SSL_new_stream(ssl, SSL_STREAM_FLAG_UNI);
    streamScreenshareRecv = SSL_accept_stream(ssl, SSL_ACCEPT_STREAM_NO_BLOCK);

    // Perform authentication if authManager is available
    if (authManager) {
        std::cout << "Starting authentication...\n";
        if (!performAuthentication()) {
            std::cerr << "Authentication failed\n";
            SSL_stream_conclude(authStream, 0);
            SSL_stream_conclude(voiceStream, 0);
            SSL_stream_conclude(eventStream, 0);
            SSL_free(authStream);
            SSL_free(voiceStream);
            SSL_free(eventStream);
            shutdown_ssl(ssl);
            SSL_free(ssl);
            SSL_CTX_free(ctx);
            connected = false;
            emit authenticationFailed("Server rejected authentication");
            return;
        }
        std::cout << "Authentication successful\n";
    }

    sendMessage(eventStream);

    recvEventThread = std::jthread(&Network::receiveEventPackets, this);
    heartbeatThread = std::jthread(&Network::sendHeartbeat, this);
}

void Network::sendMessage(SSL *stream) {
    const char *message = "hello\n";

    int ret = SSL_write(stream, message, strlen(message));
    if (ret <= 0) {
        std::cerr << "SSL_write failed\n";
    }
}

void Network::sendAdminMessage(const QString &requestType) {
    if (!connected || !eventStream) {
        std::cerr << "Cannot send admin message: not connected or no event stream\n";
        return;
    }
    json adminRequest;
    adminRequest["type"] = "admin_request";
    QStringList requestParts = requestType.split(":");
    QString request = requestParts[0];
    adminRequest["request"] = request.toStdString();
    if (requestParts.size() > 1) {
        if (request == "approve_user") {
            adminRequest["user_id"] = requestParts[1].toInt();
        }
    }
    QString jsonString = QString::fromStdString(adminRequest.dump());
    QByteArray messageBytes = jsonString.toUtf8();
    messageBytes.append('\n');

    std::cout << "Sending admin message: " << jsonString.toStdString() << std::endl;
    int ret = SSL_write(eventStream, messageBytes.constData(), messageBytes.size());
    if (ret <= 0) {
        std::cerr << "Failed to send admin message" << std::endl;
    }
}

void Network::sendTextMessage(const QString &jsonMessage) {
    if (!connected || !eventStream) {
        std::cerr << "Cannot send text message: not connected or no event stream\n";
        return;
    }
    QByteArray messageBytes = jsonMessage.toUtf8();
    messageBytes.append('\n');

    int ret = SSL_write(eventStream, messageBytes.constData(), messageBytes.size());
    if (ret <= 0) {
        std::cerr << "Failed to send text message" << std::endl;
    }
}

void Network::requestUserList() {
    if (!connected || !eventStream) {
        std::cerr << "Cannot request user list: not connected or no event stream\n";
        return;
    }
    json request;
    request["type"] = "user_list_request";
    QString jsonString = QString::fromStdString(request.dump());
    QByteArray messageBytes = jsonString.toUtf8();
    messageBytes.append('\n');

    std::cout << "Requesting user list" << std::endl;
    int ret = SSL_write(eventStream, messageBytes.constData(), messageBytes.size());
    if (ret <= 0) {
        std::cerr << "Failed to send user list request" << std::endl;
    }
}

EventType getEventType(const std::string &type) {
    if (type == "ServerInfo") return EventType::ServerInfo;
    if (type == "Message") return EventType::Message;
    if (type == "UserJoin") return EventType::UserJoin;
    if (type == "admin_response") return EventType::AdminResponse;
    if (type == "history_response") return EventType::HistoryResponse;
    if (type == "emote_list_response") return EventType::EmoteListResponse;
    if (type == "typing_indicator") return EventType::TypingIndicator;
    if (type == "user_list") return EventType::UserList;
    if (type == "user_status_change") return EventType::UserStatusChange;
    return EventType::Unknown;
}

void Network::handleEventMessage(std::string msg) {
    json j;
    try {
        j = json::parse(msg);
    } catch (const json::parse_error &e) {
        std::cerr << "JSON parse error: " << e.what() << std::endl;
        return;
    }
    if (!j.contains("type")) {
        std::cerr << "Invalid event\n";
        return;
    }
    EventType type = getEventType(j["type"]);
    // match event type
    switch (type) {
    case EventType::ServerInfo:
        if (j.contains("channels") && j["channels"].is_array()) {
            // Extract channels from JSON and convert to QStringList
            QStringList channelList;
            for (const auto &channel : j["channels"]) {
                if (channel.is_string()) {
                    channelList << QString::fromStdString(channel.get<std::string>());
                }
            }

            std::cout << "Emitting " << channelList.size() << " channels to GUI\n";
            emit channelsReceived(channelList);
        }
        break;
    case EventType::UserJoin:
        if (j.contains("user") && j.contains("channel")) {
            QString user = QString::fromStdString(j["user"].get<std::string>());
            QString channel = QString::fromStdString(j["channel"].get<std::string>());
            std::cout << "User " << user.toStdString() << " joined channel " << channel.toStdString() << std::endl;
            emit userJoinedChannel(user, channel);
        }
        break;
    case EventType::AdminResponse:
        if (j.contains("request") && j.contains("data")) {
            QString request = QString::fromStdString(j["request"].get<std::string>());
            QString jsonData = QString::fromStdString(j["data"].dump());
            std::cout << "Admin response for: " << request.toStdString() << std::endl;
            emit adminResponseReceived(request, jsonData);
        }
        break;
    case EventType::Message:
        if (j.contains("channel") && j.contains("user") && j.contains("content")) {
            QString channel = QString::fromStdString(j["channel"].get<std::string>());
            QString sender = QString::fromStdString(j["user"].get<std::string>());
            QString content = QString::fromStdString(j["content"].get<std::string>());
            bool compressed = j.value("compressed", false);
            std::cout << "Text message from " << sender.toStdString() << " in " << channel.toStdString() << std::endl;
            emit textMessageReceived(channel, sender, content, compressed);
        }
        break;
    case EventType::HistoryResponse:
        if (j.contains("channel") && j.contains("messages") && j["messages"].is_array()) {
            QString channel = QString::fromStdString(j["channel"].get<std::string>());
            QJsonArray messagesArray;
            for (const auto &msg : j["messages"]) {
                QJsonObject msgObj;
                if (msg.contains("id"))
                    msgObj["id"] = msg["id"].get<int>();
                if (msg.contains("user"))
                    msgObj["user"] = QString::fromStdString(msg["user"].get<std::string>());
                if (msg.contains("content"))
                    msgObj["content"] = QString::fromStdString(msg["content"].get<std::string>());
                if (msg.contains("timestamp"))
                    msgObj["timestamp"] = QString::fromStdString(msg["timestamp"].get<std::string>());
                msgObj["compressed"] = msg.value("compressed", false);
                messagesArray.append(msgObj);
            }
            std::cout << "History response for " << channel.toStdString() << ": " << messagesArray.size() << " messages" << std::endl;
            emit historyResponseReceived(channel, messagesArray);
        }
        break;
    case EventType::EmoteListResponse:
        if (j.contains("emotes") && j["emotes"].is_array()) {
            QJsonArray emotesArray;
            for (const auto &emote : j["emotes"]) {
                QJsonObject emoteObj;
                if (emote.contains("name"))
                    emoteObj["name"] = QString::fromStdString(emote["name"].get<std::string>());
                if (emote.contains("data"))
                    emoteObj["data"] = QString::fromStdString(emote["data"].get<std::string>());
                emotesArray.append(emoteObj);
            }
            std::cout << "Emote list received: " << emotesArray.size() << " emotes" << std::endl;
            emit emoteListReceived(emotesArray);
        }
        break;
    case EventType::TypingIndicator:
        if (j.contains("channel") && j.contains("user")) {
            QString channel = QString::fromStdString(j["channel"].get<std::string>());
            QString user = QString::fromStdString(j["user"].get<std::string>());
            emit typingIndicatorReceived(channel, user);
        }
        break;
    case EventType::UserList: {
        QStringList onlineUsers;
        QStringList offlineUsers;
        if (j.contains("online") && j["online"].is_array()) {
            for (const auto &user : j["online"]) {
                if (user.is_string()) {
                    onlineUsers << QString::fromStdString(user.get<std::string>());
                }
            }
        }
        if (j.contains("offline") && j["offline"].is_array()) {
            for (const auto &user : j["offline"]) {
                if (user.is_string()) {
                    offlineUsers << QString::fromStdString(user.get<std::string>());
                }
            }
        }
        std::cout << "User list received: " << onlineUsers.size() << " online, " << offlineUsers.size() << " offline" << std::endl;
        emit usersListReceived(onlineUsers, offlineUsers);
    } break;
    case EventType::UserStatusChange:
        if (j.contains("user") && j.contains("status")) {
            QString user = QString::fromStdString(j["user"].get<std::string>());
            QString status = QString::fromStdString(j["status"].get<std::string>());
            bool online = (status == "online");
            std::cout << "User status change: " << user.toStdString() << " -> " << status.toStdString() << std::endl;
            emit userStatusChanged(user, online);
        }
        break;
    default:
        std::cout << "Unknown event type.\n";
        break;
    }
}

void Network::receiveEventPackets() {
    char buf[102400] = {};
    size_t readbytes;
    std::string accumBuffer;
    while (SSL_read_ex(eventStream, buf, sizeof(buf), &readbytes)) {
        std::cout << "receiveEventPackets rb: " << readbytes << std::endl;
        accumBuffer.append(buf, readbytes);

        size_t pos;
        while ((pos = accumBuffer.find('\n')) != std::string::npos) {
            std::string msg = accumBuffer.substr(0, pos);
            accumBuffer.erase(0, pos + 1);
            if (!msg.empty()) {
                std::cout << "Received message: " << msg.substr(0, 40) << "..." << std::endl;
                handleEventMessage(msg);
            }
        }

        std::memset(buf, 0, readbytes);
    }
    std::cout << "Stopping receiveEventPackets thread." << std::endl;
}

void Network::receiveVoicePackets() {
    char buf[102400] = {};
    size_t readbytes;
    std::vector<uint8_t> buffer; // Use vector for binary data

    while (SSL_read_ex(voiceStream, buf, sizeof(buf), &readbytes)) {
        std::cout << "receiveVoicePackets rb: " << readbytes << "\n";

        // Append new data to buffer
        buffer.insert(buffer.end(), buf, buf + readbytes);

        // Process all complete packets
        size_t offset = 0;
        while (offset + 4 <= buffer.size()) { // Need at least 4 bytes for packet size
            // Read packet size (first 4 bytes, little endian)
            uint32_t packetSize = static_cast<uint32_t>(buffer[offset]) | (static_cast<uint32_t>(buffer[offset + 1]) << 8) |
                                  (static_cast<uint32_t>(buffer[offset + 2]) << 16) | (static_cast<uint32_t>(buffer[offset + 3]) << 24);

            std::cout << "Read packet size: " << packetSize << " bytes\n";

            // Check if we have the complete packet
            if (offset + 4 + packetSize > buffer.size()) {
                std::cout << "Incomplete packet, waiting for more data\n";
                break; // Wait for more data
            }

            // Extract packet data (skip the 4-byte size header)
            std::vector<uint8_t> packetData(buffer.begin() + offset + 4, buffer.begin() + offset + 4 + packetSize);

            // Convert to string to find the colon delimiter
            std::string packet(packetData.begin(), packetData.end());

            // Parse packet: "user1:payload"
            size_t colonPos = packet.find(':');
            if (colonPos != std::string::npos) {
                std::string userId = packet.substr(0, colonPos);

                // Extract binary payload (everything after colon)
                std::vector<uint8_t> payload(packetData.begin() + colonPos + 1, packetData.end());

                std::cout << "Parsed voice packet - User: " << userId << ", Payload size: " << payload.size() << " bytes\n";

                if (audioManager && !payload.empty()) {
                    audioManager->handleIncomingVoicePacket(userId, payload);
                }
            } else {
                std::cerr << "Invalid packet format - no colon delimiter found\n";
            }

            // Move to next packet
            offset += 4 + packetSize;
        }

        // Remove processed packets from buffer
        if (offset > 0) {
            buffer.erase(buffer.begin(), buffer.begin() + offset);
            std::cout << "Removed " << offset << " bytes from buffer, " << buffer.size() << " bytes remaining\n";
        }

        // Clear read buffer
        std::memset(buf, 0, sizeof(buf));
    }
    std::cout << "Stopping receiveVoicePackets thread.\n";
}

void Network::shutdown_ssl(SSL *ssl) {
    if (!ssl) return;
    int ret = 0;
    do {
        ret = SSL_shutdown(ssl);
        if (ret < 0) {
            std::cerr << "Error shutting down SSL: " << ret << std::endl;
            ERR_print_errors_fp(stderr);
            break;
        }
    } while (ret != 1);
}

void Network::disconnectQUIC() {
    std::cout << "disconnectQUIC\n";
    if (connected) {
        if (streamScreenshareSend) {
            SSL_stream_conclude(streamScreenshareSend, 0);
            SSL_free(streamScreenshareSend);
            streamScreenshareSend = nullptr;
        }
        if (streamScreenshareRecv) {
            SSL_stream_conclude(streamScreenshareRecv, 0);
            SSL_free(streamScreenshareRecv);
            streamScreenshareRecv = nullptr;
        }
        SSL_stream_conclude(voiceStream, 0);
        SSL_stream_conclude(eventStream, 0);
        SSL_free(voiceStream);
        SSL_free(eventStream);
        shutdown_ssl(ssl);
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        std::cout << "connected=false" << std::endl;
        connected = false;
    }
}

void Network::setVideoManager(Video* video) {
    videoManager = video;
    std::cout << "Video manager set for Network" << std::endl;
}

void Network::sendVoicePackets(std::vector<uint8_t> encodedData) {
    int ret = SSL_write(voiceStream, encodedData.data(), encodedData.size());
    if (ret <= 0) {
        std::cerr << "SSL_write failed" << std::endl;
    }
}

void Network::sendHeartbeat() {
    const char *message = "hb\n";
    while (true) {
        size_t written = 0;
        int result = SSL_write_ex(eventStream, message, strlen(message), &written);
        if (!result) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
}

void Network::joinVoiceChannel(QString channelName) {
    // send channel name to voice stream
    std::string channelNameStr = channelName.toStdString();
    const char *message = channelNameStr.c_str();
    size_t written = 0;
    int result = SSL_write_ex(voiceStream, message, strlen(message), &written);
    if (!result) {
        std::cerr << "Failed to send channel name to voice stream" << std::endl;
    } else {
        std::cout << "Sent voice channel join request: " << channelNameStr << std::endl;
    }

    // wait for confirmation (only inspect the first 2 bytes for "ok")
    char confirmBuf[1024] = {};
    size_t confirmReadBytes = 0;
    std::cout << "Waiting for voice channel join confirmation..." << std::endl;

    int confirmResult = SSL_read_ex(voiceStream, confirmBuf, sizeof(confirmBuf), &confirmReadBytes);
    if (confirmResult && confirmReadBytes >= 2) {
        bool isOk = (confirmBuf[0] == 'o' && confirmBuf[1] == 'k');
        if (isOk) {
            std::cout << "Successfully joined voice channel: " << channelNameStr << std::endl;

            // Initialize record and playback loops
            audioManager->startAudioThread();

            // Start receiving voice packets
            recvVoiceThread = std::jthread(&Network::receiveVoicePackets, this);
        } else {
            std::cerr << "Voice channel join rejected (did not receive leading 'ok')" << std::endl;
        }
    } else {
        std::cerr << "Failed to read confirmation from voice stream" << std::endl;
    }
}

void Network::joinScreenShare(QString userName) {
    std::cout << "Joining screenshare from user: " << userName.toStdString() << std::endl;
    
    json event;
    event["type"] = "joinScreenshare";
    event["user"] = userName.toStdString();
    std::string eventStr = event.dump() + "\n";
    
    size_t written = 0;
    int result = SSL_write_ex(eventStream, eventStr.c_str(), eventStr.length(), &written);
    if (!result) {
        std::cerr << "Failed to send joinScreenshare event\n";
        return;
    }
    
    std::cout << "Sent joinScreenshare event for user: " << userName.toStdString() << std::endl;
    
    recvScreenshareThread = std::jthread(&Network::receiveScreensharePackets, this);
}

void Network::sendScreensharePackets(std::vector<uint8_t> encodedData) {
    if (!streamScreenshareSend) {
        std::cerr << "Screenshare send stream not initialized\n";
        return;
    }
    
    int ret = SSL_write(streamScreenshareSend, encodedData.data(), encodedData.size());
    if (ret <= 0) {
        std::cerr << "Failed to send screenshare packet: SSL_write failed\n";
    }
}

void Network::receiveScreensharePackets() {
    std::cout << "Starting screenshare receive thread\n";
    
    if (!streamScreenshareRecv) {
        std::cout << "Waiting for incoming screenshare stream...\n";
        streamScreenshareRecv = SSL_accept_stream(ssl, 0);
        if (!streamScreenshareRecv) {
            std::cerr << "Failed to accept screenshare stream\n";
            return;
        }
        std::cout << "Accepted screenshare stream\n";
    }
    
    char buf[102400] = {};
    size_t readbytes;
    std::vector<uint8_t> buffer;
    
    while (SSL_read_ex(streamScreenshareRecv, buf, sizeof(buf), &readbytes)) {
        std::cout << "Received screenshare packet: " << readbytes << " bytes\n";
        
        buffer.insert(buffer.end(), buf, buf + readbytes);
        
        size_t offset = 0;
        while (offset + 4 <= buffer.size()) {
            uint32_t packetSize = 
                static_cast<uint32_t>(buffer[offset]) |
                (static_cast<uint32_t>(buffer[offset + 1]) << 8) |
                (static_cast<uint32_t>(buffer[offset + 2]) << 16) |
                (static_cast<uint32_t>(buffer[offset + 3]) << 24);
            
            std::cout << "Screenshare packet size: " << packetSize << " bytes\n";
            
            if (offset + 4 + packetSize > buffer.size()) {
                std::cout << "Incomplete screenshare packet, waiting for more data\n";
                break;
            }
            
            std::vector<uint8_t> packetData(
                buffer.begin() + offset + 4,
                buffer.begin() + offset + 4 + packetSize
            );
            
            if (videoManager) {
                videoManager->receiveEncodedPacket(packetData);
            }
            std::cout << "Received complete screenshare frame: " << packetData.size() << " bytes\n";
            
            offset += 4 + packetSize;
        }
        
        if (offset > 0) {
            buffer.erase(buffer.begin(), buffer.begin() + offset);
        }
        
        std::memset(buf, 0, sizeof(buf));
    }
    
    std::cout << "Stopping receiveScreensharePackets thread\n";
}

bool Network::performAuthentication() {
    if (!authManager || !authStream) {
        std::cerr << "Auth manager or auth stream not available" << std::endl;
        return false;
    }

    char challengeBuf[1024] = {};
    size_t challengeReadBytes = 0;
    std::cout << "Waiting for authentication challenge from server..." << std::endl;
    int readResult = SSL_read_ex(authStream, challengeBuf, sizeof(challengeBuf), &challengeReadBytes);
    if (!readResult || challengeReadBytes == 0) {
        std::cerr << "Failed to read authentication challenge from server" << std::endl;
        return false;
    }

    std::cout << "Received challenge of " << challengeReadBytes << " bytes" << std::endl;
    std::string challengeHexStr(challengeBuf, challengeReadBytes);
    std::vector<uint8_t> challenge;
    challenge.reserve(challengeHexStr.length() / 2);
    for (size_t i = 0; i < challengeHexStr.length(); i += 2) {
        std::string byteString = challengeHexStr.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(std::stoi(byteString, nullptr, 16));
        challenge.push_back(byte);
    }
    std::cout << "Decoded challenge to " << challenge.size() << " bytes" << std::endl;

    std::vector<uint8_t> signature = authManager->signChallenge(challenge);
    if (signature.empty()) {
        std::cerr << "Failed to sign challenge" << std::endl;
        return false;
    }

    QString username = authManager->getUsername();
    QString publicKeyHex = authManager->getPublicKeyHex();
    std::cout << "Authenticating as user: " << username.toStdString() << std::endl;
    std::cout << "Signing with public key: " << publicKeyHex.toStdString() << std::endl;

    // convert signature to hex
    QString signatureHex;
    for (uint8_t byte : signature) {
        signatureHex.append(QString("%1").arg(byte, 2, 16, QChar('0')));
    }

    // username:publicKey:signature
    QString response = username + ":" + publicKeyHex + ":" + signatureHex;
    QByteArray responseBytes = response.toUtf8();
    std::cout << "Sending authentication response (" << responseBytes.size() << " bytes)" << std::endl;
    size_t written = 0;
    int writeResult = SSL_write_ex(authStream, responseBytes.constData(), responseBytes.size(), &written);
    if (!writeResult) {
        std::cerr << "Failed to send authentication response" << std::endl;
        return false;
    }

    char confirmBuf[1024] = {};
    size_t confirmReadBytes = 0;
    std::cout << "Waiting for authentication confirmation..." << std::endl;
    int confirmResult = SSL_read_ex(authStream, confirmBuf, sizeof(confirmBuf), &confirmReadBytes);
    if (confirmResult && confirmReadBytes >= 2) {
        bool isOk = (confirmBuf[0] == 'o' && confirmBuf[1] == 'k');
        if (isOk) {
            std::cout << "Authentication confirmed by server" << std::endl;
            return true;
        } else {
            std::cerr << "Server rejected authentication (did not receive 'ok')" << std::endl;
            return false;
        }
    } else {
        std::cerr << "Failed to read authentication confirmation" << std::endl;
        return false;
    }
}