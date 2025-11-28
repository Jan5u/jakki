#include "network.hpp"

enum class EventType {
    ServerInfo,
    Message,
    UserJoin,
    Unknown
};

Network::Network() : audioManager(nullptr) {
    // Default constructor
}

Network::Network(Audio& audio) : audioManager(&audio) {
    // Constructor with Audio reference
}

void Network::connectToServer(QString address, QString port) {
    std::cout << "connectToServer\n";

    if (connected) {
        std::cout << "Already connected, skipping connectQUIC\n";
        return;
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
    const char* SERVER_IP = addressUtf8.constData();
    const char* SERVER_PORT = portUtf8.constData();
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


    // bidirectional event stream
    stream1 = SSL_new_stream(ssl, 0);

    // unidirectional send voice stream
    // stream2 = SSL_new_stream(ssl, SSL_STREAM_FLAG_UNI);

    stream2 = SSL_new_stream(ssl, 0);

    sendMessage(stream1);
    // sendMessage(stream2);
    // sendMessage(stream3);
    // unidirectional receive voice stream
    // stream3 = SSL_accept_stream(ssl, 0);
    // SSL_get_stream_type()

    recvEventThread = std::jthread(&Network::receiveEventPackets, this);
    // recvVoiceThread = std::jthread(&Network::receiveVoicePackets, this);
    heartbeatThread = std::jthread(&Network::sendHeartbeat, this);


}

void Network::sendMessage(SSL *stream) {
    const char *message = "hello";

    int ret = SSL_write(stream, message, strlen(message));
    if (ret <= 0) {
        std::cerr << "SSL_write failed\n";
    }
}

void Network::handleEventPacket(char *buf, size_t bufsize) {
    std::string data(buf, bufsize);
    size_t start = 0;
    size_t end;
    while ((end = data.find('\n', start)) != std::string::npos) {
        std::string msg = data.substr(start, end - start);
        if (!msg.empty()) {
            std::cout << "Received message: " << msg << std::endl;
            handleEventMessage(msg);
        }
        start = end + 1;
    }
    std::memset(buf, 0, bufsize);
}

EventType getEventType(const std::string& type) {
    if (type == "ServerInfo") return EventType::ServerInfo;
    if (type == "Message")    return EventType::Message;
    if (type == "UserJoin")   return EventType::UserJoin;
    return EventType::Unknown;
}

void Network::handleEventMessage(std::string msg) {
    json j = json::parse(msg);
    if (!j.contains("type")) {
        std::cerr << "Invalid event\n";
    }
    EventType type = getEventType(j["type"]);
    // match event type
    switch (type) {
        case EventType::ServerInfo:
            if (j.contains("channels") && j["channels"].is_array()) {
                // Extract channels from JSON and convert to QStringList
                QStringList channelList;
                for (const auto& channel : j["channels"]) {
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
        default:
            std::cout << "Unknown event type.\n";
            break;
    }
}

void Network::receiveEventPackets() {
    char buf[102400] = {};
    size_t readbytes;
    while (SSL_read_ex(stream1, buf, sizeof(buf), &readbytes)) {
        std::cout << "receiveEventPackets rb: " << readbytes << "\n";
        handleEventPacket(buf, sizeof(buf));
    }
    std::cout << "Stopping receiveEventPackets thread.\n";
}

void Network::receiveVoicePackets() {
    char buf[102400] = {};
    size_t readbytes;
    std::vector<uint8_t> buffer; // Use vector for binary data
    
    while (SSL_read_ex(stream2, buf, sizeof(buf), &readbytes)) {
        std::cout << "receiveVoicePackets rb: " << readbytes << "\n";
        
        // Append new data to buffer
        buffer.insert(buffer.end(), buf, buf + readbytes);
        
        // Process all complete packets
        size_t offset = 0;
        while (offset + 4 <= buffer.size()) { // Need at least 4 bytes for packet size
            // Read packet size (first 4 bytes, little endian)
            uint32_t packetSize = 
                static_cast<uint32_t>(buffer[offset]) |
                (static_cast<uint32_t>(buffer[offset + 1]) << 8) |
                (static_cast<uint32_t>(buffer[offset + 2]) << 16) |
                (static_cast<uint32_t>(buffer[offset + 3]) << 24);
            
            std::cout << "Read packet size: " << packetSize << " bytes\n";
            
            // Check if we have the complete packet
            if (offset + 4 + packetSize > buffer.size()) {
                std::cout << "Incomplete packet, waiting for more data\n";
                break; // Wait for more data
            }
            
            // Extract packet data (skip the 4-byte size header)
            std::vector<uint8_t> packetData(
                buffer.begin() + offset + 4,
                buffer.begin() + offset + 4 + packetSize
            );
            
            // Convert to string to find the colon delimiter
            std::string packet(packetData.begin(), packetData.end());
            
            // Parse packet: "user1:payload"
            size_t colonPos = packet.find(':');
            if (colonPos != std::string::npos) {
                std::string userId = packet.substr(0, colonPos);
                
                // Extract binary payload (everything after colon)
                std::vector<uint8_t> payload(
                    packetData.begin() + colonPos + 1, 
                    packetData.end()
                );
                
                std::cout << "Parsed voice packet - User: " << userId 
                          << ", Payload size: " << payload.size() << " bytes\n";
                
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
            std::cout << "Removed " << offset << " bytes from buffer, " 
                      << buffer.size() << " bytes remaining\n";
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
        SSL_stream_conclude(stream2, 0);
        SSL_stream_conclude(stream1, 0);
        SSL_free(stream2);
        SSL_free(stream1);
        shutdown_ssl(ssl);
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        std::cout << "connected=false\n";
        connected = false;
    }
}

void Network::sendVoicePackets(std::vector<uint8_t> encodedData) {
    int ret = SSL_write(stream2, encodedData.data(), encodedData.size());
    if (ret <= 0) {
        std::cerr << "SSL_write failed\n";
    }
}

void Network::sendHeartbeat() {
    const char *message = "hb";
    while (true) {
        size_t written = 0;
        int result = SSL_write_ex(stream1, message, strlen(message), &written);
        if (!result) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
}

void Network::joinVoiceChannel(QString channelName) {
    // send channel name to voice stream
    std::string channelNameStr = channelName.toStdString();
    const char* message = channelNameStr.c_str();
    size_t written = 0;
    int result = SSL_write_ex(stream2, message, strlen(message), &written);
    if (!result) {
        std::cerr << "Failed to send channel name to voice stream\n";
    } else {
        std::cout << "Sent voice channel join request: " << channelNameStr << std::endl;
    }

    // wait for confirmation (only inspect the first 2 bytes for "ok")
    char confirmBuf[1024] = {};
    size_t confirmReadBytes = 0;
    std::cout << "Waiting for voice channel join confirmation...\n";

    int confirmResult = SSL_read_ex(stream2, confirmBuf, sizeof(confirmBuf), &confirmReadBytes);
    if (confirmResult && confirmReadBytes >= 2) {
        bool isOk = (confirmBuf[0] == 'o' && confirmBuf[1] == 'k');
        if (isOk) {
            std::cout << "Successfully joined voice channel: " << channelNameStr << std::endl;

            // Initialize record and playback loops
            audioManager->startAudioThread();

            // Start receiving voice packets
            recvVoiceThread = std::jthread(&Network::receiveVoicePackets, this);
        } else {
            std::cerr << "Voice channel join rejected (did not receive leading 'ok')\n";
        }
    } else {
        std::cerr << "Failed to read confirmation from voice stream\n";
    }
}