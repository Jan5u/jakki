#include "network.hpp"
#include "auth.hpp"
#include "gui.hpp"
#include "video/video.hpp"
// // #include "video/video.hpp"

enum class EventType {
    ServerInfo,
    Message,
    UserJoin,
    UserLeave,
    AdminResponse,
    HistoryResponse,
    EmoteListResponse,
    TypingIndicator,
    UserList,
    UserStatusChange,
    Unknown
};

EventType getEventType(const std::string &type) {
    if (type == "ServerInfo") return EventType::ServerInfo;
    if (type == "Message") return EventType::Message;
    if (type == "UserJoin") return EventType::UserJoin;
    if (type == "UserLeave") return EventType::UserLeave;
    if (type == "admin_response") return EventType::AdminResponse;
    if (type == "history_response") return EventType::HistoryResponse;
    if (type == "emote_list_response") return EventType::EmoteListResponse;
    if (type == "typing_indicator") return EventType::TypingIndicator;
    if (type == "user_list") return EventType::UserList;
    if (type == "user_status_change") return EventType::UserStatusChange;
    return EventType::Unknown;
}

Network::Network() {}

void Network::connectToServer(std::string &address, std::string &port) {
    if (connected) {
        std::println("Network::connectToServer: Already connected");
        return;
    }

    // init openssl
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();

    // connect QUIC
    auto QUIC_CONNECTION = connectQUIC(address, port);
    if (!QUIC_CONNECTION) {
        std::println("\033[31m[QUIC_CONNECTION] {}\033[0m", QUIC_CONNECTION.error());
    }
}

BIO *Network::create_socket_bio(const char *hostname, const char *port, int family, BIO_ADDR **peer_addr) {
    int sock = -1;
    BIO_ADDRINFO *res;
    const BIO_ADDRINFO *ai = NULL;
    BIO *bio;
    if (!BIO_lookup_ex(hostname, port, BIO_LOOKUP_CLIENT, family, SOCK_DGRAM, 0, &res))
        return NULL;
    for (ai = res; ai != NULL; ai = BIO_ADDRINFO_next(ai)) {
        sock = BIO_socket(BIO_ADDRINFO_family(ai), SOCK_DGRAM, 0, 0);
        if (sock == -1)
            continue;
        if (!BIO_connect(sock, BIO_ADDRINFO_address(ai), 0)) {
            BIO_closesocket(sock);
            sock = -1;
            continue;
        }
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
    BIO_ADDRINFO_free(res);
    if (sock == -1)
        return NULL;
    bio = BIO_new(BIO_s_datagram());
    if (bio == NULL) {
        BIO_closesocket(sock);
        return NULL;
    }
    BIO_set_fd(bio, sock, BIO_CLOSE);
    return bio;
}

auto Network::connectQUIC(std::string &address, std::string &port) -> std::expected<void, std::string> {
    const SSL_METHOD *method = OSSL_QUIC_client_method();
    ctx = SSL_CTX_new(method);
    if (ctx == nullptr) {
        return std::unexpected("Failed to create the SSL_CTX");
    }
    ssl = SSL_new(ctx);
    if (ssl == nullptr) {
        return std::unexpected("Failed to create the SSL object");
    }
    SSL_set_incoming_stream_policy(ssl, SSL_INCOMING_STREAM_POLICY_ACCEPT, 0);
    BIO_ADDR *peer_addr = nullptr;
    bio = create_socket_bio(address.data(), port.data(), AF_INET, &peer_addr);
    if (!bio) {
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        return std::unexpected("Failed to create and connect BIO");
    }
    SSL_set_bio(ssl, bio, bio);
    unsigned char alpn[] = {5, 'j', 'a', 'k', 'k', 'i'};
    if (SSL_set_alpn_protos(ssl, alpn, sizeof(alpn)) != 0) {
        return std::unexpected("Failed to set the ALPN for the connection");
    }
    if (!SSL_set_default_stream_mode(ssl, SSL_DEFAULT_STREAM_MODE_NONE)) {
        return std::unexpected("Failed to set the default stream mode");
    }
    if (SSL_connect(ssl) < 1) {
        return std::unexpected("Failed to connect to the server");
    }
    connected = true;
    std::println("Waiting for auth stream from server...");
    authStream = SSL_accept_stream(ssl, 0);
    if (!authStream) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        connected = false;
        return std::unexpected("Failed to accept auth stream from server");
    }
    std::println("Auth stream accepted from server");

    eventStream = SSL_new_stream(ssl, 0);
    voiceStream = SSL_new_stream(ssl, 0);

    streamScreenshareSend = SSL_new_stream(ssl, SSL_STREAM_FLAG_UNI);
    streamScreenshareRecv = SSL_accept_stream(ssl, SSL_ACCEPT_STREAM_NO_BLOCK);

    std::println("Starting authentication...");
    if (!performAuthentication()) {
        std::println("Authentication failed");
        SSL_stream_conclude(authStream, 0);
        SSL_stream_conclude(voiceStream, 0);
        SSL_stream_conclude(eventStream, 0);
        SSL_free(authStream);
        SSL_free(voiceStream);
        SSL_free(eventStream);
        SSL_shutdown(ssl);
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        connected = false;
        return std::unexpected("Server rejected authentication");
    }
    std::println("Authentication successful");

    sendMessage(eventStream);

    recvEventThread = std::jthread(&Network::receiveEventPackets, this);
    heartbeatThread = std::jthread(&Network::sendHeartbeat, this);

    return {};
}

void Network::sendMessage(SSL *stream) {
    const char *message = "hello\n";
    int ret = SSL_write(stream, message, strlen(message));
    if (ret <= 0) {
        std::cerr << "SSL_write failed\n";
    }
}

void Network::setAuthManager(Auth *auth) { authManager = auth; }

void Network::setGUI(GUI* g) { gui = g; }

void Network::setAudio(Audio *a) {audio = a; }

void Network::setVideo(Video *v) {video = v; }

// void Network::sendAdminMessage(const QString &requestType) {
//     if (!connected || !eventStream) {
//         std::cerr << "Cannot send admin message: not connected or no event stream\n";
//         return;
//     }
//     json adminRequest;
//     adminRequest["type"] = "admin_request";
//     QStringList requestParts = requestType.split(":");
//     QString request = requestParts[0];
//     adminRequest["request"] = request.toStdString();
//     if (requestParts.size() > 1) {
//         if (request == "approve_user") {
//             adminRequest["user_id"] = requestParts[1].toInt();
//         }
//     }
//     QString jsonString = QString::fromStdString(adminRequest.dump());
//     QByteArray messageBytes = jsonString.toUtf8();
//     messageBytes.append('\n');

//     std::cout << "Sending admin message: " << jsonString.toStdString() << std::endl;
//     int ret = SSL_write(eventStream, messageBytes.constData(), messageBytes.size());
//     if (ret <= 0) {
//         std::cerr << "Failed to send admin message" << std::endl;
//     }
// }

// void Network::sendTextMessage(const QString &jsonMessage) {
//     if (!connected || !eventStream) {
//         std::cerr << "Cannot send text message: not connected or no event stream\n";
//         return;
//     }
//     QByteArray messageBytes = jsonMessage.toUtf8();
//     messageBytes.append('\n');

//     int ret = SSL_write(eventStream, messageBytes.constData(), messageBytes.size());
//     if (ret <= 0) {
//         std::cerr << "Failed to send text message" << std::endl;
//     }
// }

// void Network::requestUserList() {
//     if (!connected || !eventStream) {
//         std::cerr << "Cannot request user list: not connected or no event stream\n";
//         return;
//     }
//     json request;
//     request["type"] = "user_list_request";
//     QString jsonString = QString::fromStdString(request.dump());
//     QByteArray messageBytes = jsonString.toUtf8();
//     messageBytes.append('\n');

//     std::cout << "Requesting user list" << std::endl;
//     int ret = SSL_write(eventStream, messageBytes.constData(), messageBytes.size());
//     if (ret <= 0) {
//         std::cerr << "Failed to send user list request" << std::endl;
//     }
// }

void Network::handleEventMessage(std::string msg) {
    json j;
    try {
        j = json::parse(msg);
    } catch (const json::parse_error &e) {
        std::cerr << "JSON parse error: " << e.what() << std::endl;
        return;
    }

    if (!j.contains("type") || !j["type"].is_string()) {
        std::cerr << "Invalid event (missing or non-string type)\n";
        return;
    }

    std::string typeStr = j["type"].get<std::string>();
    EventType type = getEventType(typeStr);

    switch (type) {
    case EventType::ServerInfo: {
        if (!j.contains("channels") || !j["channels"].is_array()) break;
        std::vector<Channel> chs;
        for (const auto &ch : j["channels"]) {
            if (!ch.is_string()) continue;
            Channel c;
            c.name = ch.get<std::string>();
            chs.push_back(std::move(c));
        }
        std::println("Channels: {}", chs.size());
        if (gui) gui->onChannelsReceived(chs);
    } break;
    case EventType::UserJoin:
        if (j.contains("user") && j.contains("channel") && j["user"].is_string() && j["channel"].is_string()) {
            std::string user = j["user"].get<std::string>();
            std::string channel = j["channel"].get<std::string>();
            std::println("User {} joined channel {}", user, channel);
            if (gui) gui->onUserJoinVoiceChannel(user, channel);
        }
        break;
    case EventType::UserLeave:
        if (j.contains("user") && j.contains("channel") && j["user"].is_string() && j["channel"].is_string()) {
            std::string user = j["user"].get<std::string>();
            std::string channel = j["channel"].get<std::string>();
            std::println("User {} left channel {}", user, channel);
        }
        break;
    case EventType::AdminResponse:
        if (j.contains("request") && j.contains("data") && j["request"].is_string()) {
            std::string request = j["request"].get<std::string>();
            std::string jsonData = j["data"].dump();
            std::println("Admin response for: {} - data length {}", request, jsonData.size());
        }
        break;
    case EventType::Message:
        if (j.contains("channel") && j.contains("user") && j.contains("content") && j["channel"].is_string() && j["user"].is_string() && j["content"].is_string()) {
            std::string channel = j["channel"].get<std::string>();
            std::string sender = j["user"].get<std::string>();
            std::string content = j["content"].get<std::string>();
            bool compressed = j.value("compressed", false);
            std::println("Text message from {} in {} ({} bytes{})", sender, channel, content.size(), compressed ? ", compressed" : "");
        }
        break;
    case EventType::HistoryResponse:
        if (j.contains("channel") && j.contains("messages") && j["channel"].is_string() && j["messages"].is_array()) {
            std::string channel = j["channel"].get<std::string>();
            std::vector<json> messages;
            for (const auto &m : j["messages"]) messages.push_back(m);
            std::println("History response for {}: {} messages", channel, messages.size());
        }
        break;
    case EventType::EmoteListResponse:
        if (j.contains("emotes") && j["emotes"].is_array()) {
            std::vector<std::pair<std::string,std::string>> emotes;
            for (const auto &emote : j["emotes"]) {
                std::string name = emote.value("name", std::string());
                std::string data = emote.value("data", std::string());
                emotes.emplace_back(std::move(name), std::move(data));
            }
            std::println("Emote list received: {} emotes", emotes.size());
        }
        break;
    case EventType::TypingIndicator:
        if (j.contains("channel") && j.contains("user") && j["channel"].is_string() && j["user"].is_string()) {
            std::string channel = j["channel"].get<std::string>();
            std::string user = j["user"].get<std::string>();
            std::println("Typing indicator: {} by {}", channel, user);
        }
        break;
    case EventType::UserList: {
        std::vector<std::string> onlineUsers;
        std::vector<std::string> offlineUsers;
        if (j.contains("online") && j["online"].is_array()) {
            for (const auto &u : j["online"]) if (u.is_string()) onlineUsers.push_back(u.get<std::string>());
        }
        if (j.contains("offline") && j["offline"].is_array()) {
            for (const auto &u : j["offline"]) if (u.is_string()) offlineUsers.push_back(u.get<std::string>());
        }
        std::println("User list received: {} online, {} offline", onlineUsers.size(), offlineUsers.size());
    } break;
    case EventType::UserStatusChange:
        if (j.contains("user") && j.contains("status") && j["user"].is_string() && j["status"].is_string()) {
            std::string user = j["user"].get<std::string>();
            std::string status = j["status"].get<std::string>();
            bool online = (status == "online");
            std::println("User status change: {} -> {}", user, status);
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
        accumBuffer.append(buf, readbytes);
        size_t pos;
        while ((pos = accumBuffer.find('\n')) != std::string::npos) {
            std::string msg = accumBuffer.substr(0, pos);
            accumBuffer.erase(0, pos + 1);
            if (!msg.empty()) {
                std::println("Received message: {}", msg);
                handleEventMessage(msg);
            }
        }
        std::memset(buf, 0, readbytes);
    }
    std::println("Stopping receiveEventPackets thread");
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

                if (audio && !payload.empty()) {
                    audio->handleIncomingVoicePacket(userId, payload);
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

// void Network::shutdown_ssl(SSL *ssl) {
//     if (!ssl) return;
//     int ret = 0;
//     do {
//         ret = SSL_shutdown(ssl);
//         if (ret < 0) {
//             std::cerr << "Error shutting down SSL: " << ret << std::endl;
//             ERR_print_errors_fp(stderr);
//             break;
//         }
//     } while (ret != 1);
// }

// void Network::disconnectQUIC() {
//     std::cout << "disconnectQUIC\n";
//     if (connected) {
//         if (streamScreenshareSend) {
//             SSL_stream_conclude(streamScreenshareSend, 0);
//             SSL_free(streamScreenshareSend);
//             streamScreenshareSend = nullptr;
//         }
//         if (streamScreenshareRecv) {
//             SSL_stream_conclude(streamScreenshareRecv, 0);
//             SSL_free(streamScreenshareRecv);
//             streamScreenshareRecv = nullptr;
//         }
//         SSL_stream_conclude(voiceStream, 0);
//         SSL_stream_conclude(eventStream, 0);
//         SSL_free(voiceStream);
//         SSL_free(eventStream);
//         shutdown_ssl(ssl);
//         SSL_free(ssl);
//         SSL_CTX_free(ctx);
//         std::cout << "connected=false" << std::endl;
//         connected = false;
//         inVoiceChannel = false;
//     }
// }

// void Network::setVideoManager(Video* video) {
//     videoManager = video;
//     std::cout << "Video manager set for Network" << std::endl;
// }

void Network::sendVoicePackets(std::vector<uint8_t> encodedData) {
    int ret = SSL_write(voiceStream, encodedData.data(), encodedData.size());
    if (ret <= 0) {
        std::cerr << "SSL_write failed" << std::endl;
    }
}

void Network::sendScreensharePackets(std::vector<uint8_t> encodedData) {
    if (!streamScreenshareSend) {
        std::println("Screenshare send stream not initialized");
        return;
    }

    int ret = SSL_write(streamScreenshareSend, encodedData.data(), encodedData.size());
    if (ret <= 0) {
        std::println("Failed to send screenshare packet");
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

void Network::joinVoiceChannel(const std::string& channelName) {
    // send channel name to voice stream
    const char *message = channelName.c_str();
    size_t written = 0;
    int result = SSL_write_ex(voiceStream, message, strlen(message), &written);
    if (!result) {
        std::println("Failed to send channel name to voice stream");
    } else {
        std::println("Sent voice channel join request: {}", channelName);
    }

    char confirmBuf[1024] = {};
    size_t confirmReadBytes = 0;
    std::println("Waiting for voice channel join confirmation...");

    int confirmResult = SSL_read_ex(voiceStream, confirmBuf, sizeof(confirmBuf), &confirmReadBytes);
    if (confirmResult && confirmReadBytes >= 2) {
        bool isOk = (confirmBuf[0] == 'o' && confirmBuf[1] == 'k');
        if (isOk) {
            std::println("Successfully joined voice channel: {}", channelName);

            inVoiceChannel = true;

            // Initialize record and playback loops
            audio->startAudioThread();

            // Start receiving voice packets
            recvVoiceThread = std::jthread(&Network::receiveVoicePackets, this);
        } else {
            std::println("Voice channel join rejected (did not receive leading 'ok')");
        }
    } else {
        std::println("Failed to read confirmation from voice stream");
    }
}

void Network::leaveVoiceChannel() {
    if (!connected || !inVoiceChannel) {
        std::println("Not in a voice channel, nothing to leave");
        return;
    }

    json event;
    event["type"] = "leaveVoice";
    std::string eventStr = event.dump() + "\n";

    size_t written = 0;
    int result = SSL_write_ex(eventStream, eventStr.c_str(), eventStr.length(), &written);
    if (!result) {
        std::println("Failed to send leaveVoice event");
    } else {
        std::println("Sent leaveVoice event");
    }

    // if (audioManager) {
    //     audioManager->stopAudio();
    // }

    inVoiceChannel = false;
}

void Network::joinScreenShare(std::string userName) {
    std::cout << "Joining screenshare from user: " << userName.data() << std::endl;
    
    json event;
    event["type"] = "joinScreenshare";
    event["user"] = userName.data();
    std::string eventStr = event.dump() + "\n";
    
    size_t written = 0;
    int result = SSL_write_ex(eventStream, eventStr.c_str(), eventStr.length(), &written);
    if (!result) {
        std::cerr << "Failed to send joinScreenshare event\n";
        return;
    }
    
    std::cout << "Sent joinScreenshare event for user: " << userName.data() << std::endl;
    
    recvScreenshareThread = std::jthread(&Network::receiveScreensharePackets, this);
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

            if (video) {
                video->receiveEncodedPacket(std::move(packetData));
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
    if (!authStream) {
        std::cerr << "performAuthentication: no auth stream" << std::endl;
        return false;
    }

    char buf[8192] = {};
    size_t readbytes = 0;
    int r = SSL_read_ex(authStream, buf, sizeof(buf), &readbytes);
    if (r <= 0 || readbytes == 0) {
        std::cerr << "performAuthentication: failed to read challenge" << std::endl;
        return false;
    }

    std::string challengeStr(buf, readbytes);
    std::cout << "Received auth challenge (" << readbytes << " bytes)" << std::endl;

    auto bytesToHex = [](const std::vector<uint8_t> &bytes) {
        static const char hex[] = "0123456789abcdef";
        std::string out;
        out.reserve(bytes.size()*2);
        for (uint8_t b: bytes) {
            out.push_back(hex[b>>4]);
            out.push_back(hex[b&0xf]);
        }
        return out;
    };

    std::vector<uint8_t> challengeBytes;
    bool isHex = (challengeStr.size() % 2 == 0);
    for (char c : challengeStr) if (!isxdigit((unsigned char)c)) { isHex = false; break; }
    if (isHex) {
        challengeBytes.reserve(challengeStr.size()/2);
        for (size_t i=0;i<challengeStr.size();i+=2) {
            std::string byteStr = challengeStr.substr(i,2);
            uint8_t b = static_cast<uint8_t>(std::stoul(byteStr, nullptr, 16));
            challengeBytes.push_back(b);
        }
    } else {
        challengeBytes.assign(challengeStr.begin(), challengeStr.end());
    }

    if (authManager) {
        if (!authManager->loadOrGenerateKeys()) {
            std::cerr << "Auth manager failed to load/generate keys" << std::endl;
            return false;
        }

        std::vector<uint8_t> signature = authManager->signChallenge(challengeBytes);
        if (signature.empty()) {
            std::cerr << "Failed to sign challenge" << std::endl;
            return false;
        }

        std::string username = authManager->getUsername();
        std::string pubkeyHex = authManager->getPublicKeyHex();
        std::string sigHex = bytesToHex(signature);
        std::string response = username + ":" + pubkeyHex + ":" + sigHex;
        size_t written = 0;
        if (SSL_write_ex(authStream, response.c_str(), response.size(), &written) == 0) {
            std::cerr << "performAuthentication: failed to write signed response" << std::endl;
            return false;
        }
        std::cout << "Sent signed auth response (" << written << " bytes)" << std::endl;

        // read confirmation
        char confirmBuf[16] = {};
        size_t confirmRead = 0;
        if (SSL_read_ex(authStream, confirmBuf, sizeof(confirmBuf), &confirmRead) > 0 && confirmRead >= 2) {
            if (confirmRead >= 2 && confirmBuf[0] == 'o' && confirmBuf[1] == 'k') {
                std::cout << "Authentication confirmed by server" << std::endl;
                return true;
            }
            std::cerr << "Authentication rejected by server" << std::endl;
            return false;
        }

        std::cerr << "No confirmation from server after auth" << std::endl;
        return false;
    }
    return true;
}