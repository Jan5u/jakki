#pragma once

#include <cstdint>
#include <vector>

#include <msquic.hpp>

class Network {
  public:
    bool init(const char *host = "127.0.0.1", uint16_t port = 7777);
    bool sendDatagram(const uint8_t *data, uint32_t length);
    void shutdown();

  private:
    static QUIC_STATUS QUIC_API connectionCallback(HQUIC connection, void *context, QUIC_CONNECTION_EVENT *event);

    static QUIC_STATUS QUIC_API streamCallback(HQUIC stream, void *context, QUIC_STREAM_EVENT *event);

    void closeHandles();

    const QUIC_API_TABLE *api_ = nullptr;
    HQUIC registration_ = nullptr;
    HQUIC configuration_ = nullptr;
    HQUIC connection_ = nullptr;
    HQUIC stream_ = nullptr;
    std::vector<uint8_t> datagram_buffer_;
    QUIC_BUFFER datagram_buffer_desc_ = {};
};