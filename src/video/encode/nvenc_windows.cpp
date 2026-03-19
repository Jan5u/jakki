#include "nvenc_windows.hpp"
#include "../../network.hpp"

NvencWindowsEncoder::NvencWindowsEncoder(Network* network, EncoderType type) : net(network), encoder_type(type) {}

NvencWindowsEncoder::~NvencWindowsEncoder() {
    if (m_ctx) {
        avcodec_free_context(&m_ctx);
    }
}

void NvencWindowsEncoder::init() {}

void NvencWindowsEncoder::flush() {
    if (!m_ctx) {
        return;
    }

    avcodec_send_frame(m_ctx, nullptr);
    AVPacket* pkt = av_packet_alloc();
    while (avcodec_receive_packet(m_ctx, pkt) == 0) {
        if (net) {
            std::vector<uint8_t> framedPacket;
            uint32_t packetSize = pkt->size;

            framedPacket.push_back(packetSize & 0xFF);
            framedPacket.push_back((packetSize >> 8) & 0xFF);
            framedPacket.push_back((packetSize >> 16) & 0xFF);
            framedPacket.push_back((packetSize >> 24) & 0xFF);

            framedPacket.insert(framedPacket.end(), pkt->data, pkt->data + pkt->size);
            net->sendScreensharePackets(framedPacket);
        }

        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
}

bool NvencWindowsEncoder::isReady() const {
    return m_ready;
}

std::string NvencWindowsEncoder::getName() const {
    return Encoder::encoderTypeToName(encoder_type);
}

bool NvencWindowsEncoder::encodeD3D11Frame(AVFrame* frame) {
    if (!frame) {
        return false;
    }

    if (!m_ready) {
        const AVCodec* codec = avcodec_find_encoder_by_name(getName().c_str());
        if (!codec) {
            std::println(stderr, "NVENC encoder not found: {}", getName());
            return false;
        }

        m_ctx = avcodec_alloc_context3(codec);
        if (!m_ctx) {
            std::println(stderr, "Failed to allocate encoder context");
            return false;
        }

        m_ctx->width = frame->width;
        m_ctx->height = frame->height;
        m_ctx->time_base = AVRational{1, 60};
        m_ctx->framerate = AVRational{60, 1};
        m_ctx->pix_fmt = AV_PIX_FMT_D3D11;

        if (frame->hw_frames_ctx) {
            m_ctx->hw_frames_ctx = av_buffer_ref(frame->hw_frames_ctx);
            AVHWFramesContext* frames_ctx = reinterpret_cast<AVHWFramesContext*>(frame->hw_frames_ctx->data);
            if (frames_ctx && frames_ctx->device_ref) {
                m_ctx->hw_device_ctx = av_buffer_ref(frames_ctx->device_ref);
            }
        }

        av_opt_set(m_ctx->priv_data, "cq", "20", 0);

        if (avcodec_open2(m_ctx, codec, nullptr) < 0) {
            std::println(stderr, "Failed to open NVENC encoder");
            avcodec_free_context(&m_ctx);
            return false;
        }

        m_ready = true;
    }

    if (avcodec_send_frame(m_ctx, frame) < 0) {
        return false;
    }

    AVPacket* pkt = av_packet_alloc();
    while (avcodec_receive_packet(m_ctx, pkt) == 0) {
        if (net) {
            std::vector<uint8_t> framedPacket;
            uint32_t packetSize = pkt->size;

            framedPacket.push_back(packetSize & 0xFF);
            framedPacket.push_back((packetSize >> 8) & 0xFF);
            framedPacket.push_back((packetSize >> 16) & 0xFF);
            framedPacket.push_back((packetSize >> 24) & 0xFF);

            framedPacket.insert(framedPacket.end(), pkt->data, pkt->data + pkt->size);
            net->sendScreensharePackets(framedPacket);
        }

        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    return true;
}
