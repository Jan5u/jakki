#include "network.hpp"

#include <msquic.hpp>

#include <cstdio>
#include <cstring>

namespace {
constexpr const char *kDefaultAlpn = "jakki";

void logStatus(const char *label, QUIC_STATUS status) {
    if (status == QUIC_STATUS_SUCCESS) {
        return;
    }
    if (status == QUIC_STATUS_PENDING) {
        printf("MsQuic %s pending\n", label);
        return;
    }
    printf("MsQuic %s failed: 0x%x\n", label, static_cast<unsigned int>(status));
}
} // namespace

bool Network::init(const char *host, uint16_t port) {
    QUIC_STATUS status = MsQuicOpen2(&api_);
    logStatus("MsQuicOpen2", status);
    if (QUIC_FAILED(status)) {
        return false;
    }

    QUIC_REGISTRATION_CONFIG reg_config = {
        "jakki",
        QUIC_EXECUTION_PROFILE_LOW_LATENCY,
    };

    status = api_->RegistrationOpen(&reg_config, &registration_);
    logStatus("RegistrationOpen", status);
    if (QUIC_FAILED(status)) {
        closeHandles();
        return false;
    }

    QUIC_SETTINGS settings = {};
    settings.IsSet.PeerBidiStreamCount = TRUE;
    settings.PeerBidiStreamCount = 1;
    settings.IsSet.DatagramReceiveEnabled = TRUE;
    settings.DatagramReceiveEnabled = TRUE;

    QUIC_BUFFER alpn = {
        static_cast<uint32_t>(std::strlen(kDefaultAlpn)),
        reinterpret_cast<uint8_t *>(const_cast<char *>(kDefaultAlpn)),
    };

    status = api_->ConfigurationOpen(registration_, &alpn, 1, &settings, sizeof(settings), nullptr, &configuration_);
    logStatus("ConfigurationOpen", status);
    if (QUIC_FAILED(status)) {
        closeHandles();
        return false;
    }

    datagram_buffer_.clear();
    datagram_buffer_desc_ = {};

    QUIC_CREDENTIAL_CONFIG cred_config = {};
    cred_config.Type = QUIC_CREDENTIAL_TYPE_NONE;
    cred_config.Flags = QUIC_CREDENTIAL_FLAG_CLIENT | QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;

    status = api_->ConfigurationLoadCredential(configuration_, &cred_config);
    logStatus("ConfigurationLoadCredential", status);
    if (QUIC_FAILED(status)) {
        closeHandles();
        return false;
    }

    status = api_->ConnectionOpen(registration_, &Network::connectionCallback, this, &connection_);
    logStatus("ConnectionOpen", status);
    if (QUIC_FAILED(status)) {
        closeHandles();
        return false;
    }

    status = api_->ConnectionStart(connection_, configuration_, QUIC_ADDRESS_FAMILY_UNSPEC, host, port);
    logStatus("ConnectionStart", status);
    if (QUIC_FAILED(status)) {
        closeHandles();
        return false;
    }

    return true;
}

void Network::shutdown() { closeHandles(); }

QUIC_STATUS QUIC_API Network::connectionCallback(HQUIC connection, void *context, QUIC_CONNECTION_EVENT *event) {
    auto *self = static_cast<Network *>(context);

    switch (event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED: {
        printf("MsQuic connected\n");
        if (self->stream_ != nullptr) {
            break;
        }

        QUIC_STATUS status = self->api_->StreamOpen(connection, QUIC_STREAM_OPEN_FLAG_NONE, &Network::streamCallback, self, &self->stream_);
        logStatus("StreamOpen", status);
        if (QUIC_FAILED(status)) {
            break;
        }

        status = self->api_->StreamStart(self->stream_, QUIC_STREAM_START_FLAG_IMMEDIATE);
        logStatus("StreamStart", status);

        break;
    }
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
        printf("MsQuic shutdown by transport: status=0x%x error=0x%llx\n", static_cast<unsigned int>(event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status),
               static_cast<unsigned long long>(event->SHUTDOWN_INITIATED_BY_TRANSPORT.ErrorCode));
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
        printf("MsQuic shutdown by peer: error=0x%llx\n", static_cast<unsigned long long>(event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode));
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        printf("MsQuic shutdown complete\n");
        if (self->connection_ != nullptr) {
            self->api_->ConnectionClose(self->connection_);
            self->connection_ = nullptr;
        }
        break;
    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API Network::streamCallback(HQUIC stream, void *context, QUIC_STREAM_EVENT *event) {
    auto *self = static_cast<Network *>(context);

    switch (event->Type) {
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        if (self->stream_ != nullptr) {
            self->api_->StreamClose(self->stream_);
            self->stream_ = nullptr;
        }
        break;
    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}

void Network::closeHandles() {
    if (api_ == nullptr) {
        return;
    }

    HQUIC stream = stream_;
    stream_ = nullptr;
    if (stream != nullptr) {
        api_->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
        api_->StreamClose(stream);
    }

    HQUIC connection = connection_;
    connection_ = nullptr;
    if (connection != nullptr) {
        api_->ConnectionShutdown(connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
        api_->ConnectionClose(connection);
    }

    if (configuration_ != nullptr) {
        api_->ConfigurationClose(configuration_);
        configuration_ = nullptr;
    }

    if (registration_ != nullptr) {
        api_->RegistrationClose(registration_);
        registration_ = nullptr;
    }

    MsQuicClose(api_);
    api_ = nullptr;
}

bool Network::sendDatagram(const uint8_t *data, uint32_t length) {
    if (api_ == nullptr || connection_ == nullptr || data == nullptr || length == 0) {
        return false;
    }

    datagram_buffer_.assign(data, data + length);
    datagram_buffer_desc_.Length = static_cast<uint32_t>(datagram_buffer_.size());
    datagram_buffer_desc_.Buffer = datagram_buffer_.data();

    QUIC_STATUS status = api_->DatagramSend(connection_, &datagram_buffer_desc_, 1, QUIC_SEND_FLAG_NONE, nullptr);
    logStatus("DatagramSend", status);
    return !QUIC_FAILED(status);
}