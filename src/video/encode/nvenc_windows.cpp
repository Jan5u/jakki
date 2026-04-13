#include "nvenc_windows.hpp"
#include "../../network.hpp"

namespace {
bool codecSupportsPixFmt(const AVCodec* codec, AVPixelFormat fmt) {
    if (!codec || !codec->pix_fmts) {
        return false;
    }

    for (const AVPixelFormat* p = codec->pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == fmt) {
            return true;
        }
    }
    return false;
}

void setEncoderOptionInt(AVCodecContext* ctx, const char* key, int64_t value) {
    if (!ctx || !ctx->priv_data) {
        return;
    }
    av_opt_set_int(ctx->priv_data, key, value, 0);
}

void setEncoderOptionStr(AVCodecContext* ctx, const char* key, const char* value) {
    if (!ctx || !ctx->priv_data) {
        return;
    }
    av_opt_set(ctx->priv_data, key, value, 0);
}

bool isAnnexB(const uint8_t* data, int size) {
    if (!data || size < 4) {
        return false;
    }
    return (size >= 4 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00 && data[3] == 0x01) ||
           (size >= 3 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01);
}

bool containsSpsPpsAnnexB(const uint8_t* data, int size) {
    if (!data || size < 5) {
        return false;
    }

    bool hasSps = false;
    bool hasPps = false;

    for (int i = 0; i + 4 < size; ++i) {
        int startCodeSize = 0;
        if (i + 4 < size && data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x00 && data[i + 3] == 0x01) {
            startCodeSize = 4;
        } else if (i + 3 < size && data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x01) {
            startCodeSize = 3;
        }

        if (startCodeSize == 0 || i + startCodeSize >= size) {
            continue;
        }

        const uint8_t nalType = data[i + startCodeSize] & 0x1F;
        if (nalType == 7) {
            hasSps = true;
        } else if (nalType == 8) {
            hasPps = true;
        }

        if (hasSps && hasPps) {
            return true;
        }
    }

    return false;
}

std::vector<uint8_t> convertH264ExtradataToAnnexB(const uint8_t* data, int size) {
    std::vector<uint8_t> out;
    if (!data || size <= 0) {
        return out;
    }

    if (isAnnexB(data, size)) {
        out.insert(out.end(), data, data + size);
        return out;
    }

    if (size >= 7 && data[0] == 1) {
        int pos = 5;
        const int lengthSizeMinusOne = data[pos++] & 0x03;
        (void)lengthSizeMinusOne;
        const int numSps = data[pos++] & 0x1F;

        auto appendNal = [&out](const uint8_t* nal, int nalSize) {
            static const uint8_t sc[4] = {0x00, 0x00, 0x00, 0x01};
            out.insert(out.end(), sc, sc + 4);
            out.insert(out.end(), nal, nal + nalSize);
        };

        for (int i = 0; i < numSps; ++i) {
            if (pos + 2 > size) return {};
            const int spsLen = (data[pos] << 8) | data[pos + 1];
            pos += 2;
            if (spsLen <= 0 || pos + spsLen > size) return {};
            appendNal(data + pos, spsLen);
            pos += spsLen;
        }

        if (pos + 1 > size) return {};
        const int numPps = data[pos++];
        for (int i = 0; i < numPps; ++i) {
            if (pos + 2 > size) return {};
            const int ppsLen = (data[pos] << 8) | data[pos + 1];
            pos += 2;
            if (ppsLen <= 0 || pos + ppsLen > size) return {};
            appendNal(data + pos, ppsLen);
            pos += ppsLen;
        }
    }

    return out;
}

void sendPacketToNetwork(Network* net, const AVPacket* pkt, const std::vector<uint8_t>* prefix = nullptr) {
    if (!net || !pkt || !pkt->data || pkt->size <= 0) {
        return;
    }

    std::vector<uint8_t> framedPacket;
    const uint32_t packetSize = static_cast<uint32_t>(pkt->size + (prefix ? prefix->size() : 0));

    framedPacket.push_back(packetSize & 0xFF);
    framedPacket.push_back((packetSize >> 8) & 0xFF);
    framedPacket.push_back((packetSize >> 16) & 0xFF);
    framedPacket.push_back((packetSize >> 24) & 0xFF);

    if (prefix && !prefix->empty()) {
        framedPacket.insert(framedPacket.end(), prefix->begin(), prefix->end());
    }

    framedPacket.insert(framedPacket.end(), pkt->data, pkt->data + pkt->size);
    net->sendScreensharePackets(framedPacket);
}
}

void logCodecPixelFormats(const AVCodec* codec, const std::string& codecName) {
    if (!codec || !codec->pix_fmts) {
        std::println(stderr, "Encoder {} does not expose pixel format list", codecName);
        return;
    }

    std::string formats;
    for (const AVPixelFormat* p = codec->pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
        const char* name = av_get_pix_fmt_name(*p);
        if (!formats.empty()) {
            formats += ", ";
        }
        formats += (name ? name : "unknown");
    }
    std::println("Encoder {} supported pixel formats: {}", codecName, formats);
}
NvencWindowsEncoder::NvencWindowsEncoder(Network* network, EncoderType type) : net(network), encoder_type(type) {}

NvencWindowsEncoder::~NvencWindowsEncoder() {
    if (m_h264_annexb_bsf) {
        av_bsf_free(&m_h264_annexb_bsf);
    }
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
        if (m_h264_annexb_bsf) {
            const int sendRet = av_bsf_send_packet(m_h264_annexb_bsf, pkt);
            if (sendRet >= 0) {
                while (av_bsf_receive_packet(m_h264_annexb_bsf, pkt) == 0) {
                    const bool needsParamSets = !m_h264_sent_parameter_sets || ((pkt->flags & AV_PKT_FLAG_KEY) && !containsSpsPpsAnnexB(pkt->data, pkt->size));
                    if (needsParamSets && !m_h264_annexb_extradata.empty()) {
                        sendPacketToNetwork(net, pkt, &m_h264_annexb_extradata);
                        m_h264_sent_parameter_sets = true;
                    } else {
                        sendPacketToNetwork(net, pkt);
                        if (containsSpsPpsAnnexB(pkt->data, pkt->size)) {
                            m_h264_sent_parameter_sets = true;
                        }
                    }
                    av_packet_unref(pkt);
                }
            }
        } else {
            sendPacketToNetwork(net, pkt);
        }
        av_packet_unref(pkt);
    }

    if (m_h264_annexb_bsf) {
        av_bsf_send_packet(m_h264_annexb_bsf, nullptr);
        while (av_bsf_receive_packet(m_h264_annexb_bsf, pkt) == 0) {
            const bool needsParamSets = !m_h264_sent_parameter_sets || ((pkt->flags & AV_PKT_FLAG_KEY) && !containsSpsPpsAnnexB(pkt->data, pkt->size));
            if (needsParamSets && !m_h264_annexb_extradata.empty()) {
                sendPacketToNetwork(net, pkt, &m_h264_annexb_extradata);
                m_h264_sent_parameter_sets = true;
            } else {
                sendPacketToNetwork(net, pkt);
                if (containsSpsPpsAnnexB(pkt->data, pkt->size)) {
                    m_h264_sent_parameter_sets = true;
                }
            }
            av_packet_unref(pkt);
        }
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
        const bool isAmf = getName().find("_amf") != std::string::npos;
        const AVCodec* codec = avcodec_find_encoder_by_name(getName().c_str());
        if (!codec) {
            std::println(stderr, "GPU encoder not found: {}", getName());
            return false;
        }

        if (isAmf) {
            logCodecPixelFormats(codec, getName());
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
        m_ctx->gop_size = 60;
        m_ctx->max_b_frames = 0;
        m_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
        AVPixelFormat selectedPixFmt = AV_PIX_FMT_D3D11;
        if (isAmf) {
            const AVPixelFormat inputFmt = static_cast<AVPixelFormat>(frame->format);

            if (inputFmt == AV_PIX_FMT_D3D11 || inputFmt == AV_PIX_FMT_D3D11VA_VLD) {
                selectedPixFmt = inputFmt;
            } else if (codecSupportsPixFmt(codec, inputFmt)) {
                selectedPixFmt = inputFmt;
            } else if (codecSupportsPixFmt(codec, AV_PIX_FMT_D3D11)) {
                selectedPixFmt = AV_PIX_FMT_D3D11;
            } else if (codecSupportsPixFmt(codec, AV_PIX_FMT_NV12)) {
                selectedPixFmt = AV_PIX_FMT_NV12;
            }
        }
        m_ctx->pix_fmt = selectedPixFmt;

        if (isAmf) {
            m_ctx->color_range = AVCOL_RANGE_MPEG;
            m_ctx->colorspace = AVCOL_SPC_BT709;
            m_ctx->color_primaries = AVCOL_PRI_BT709;
            m_ctx->color_trc = AVCOL_TRC_BT709;
        }

        if (frame->hw_frames_ctx) {
            AVHWFramesContext* frames_ctx = reinterpret_cast<AVHWFramesContext*>(frame->hw_frames_ctx->data);
            if (frames_ctx && frames_ctx->device_ref) {
                m_ctx->hw_device_ctx = av_buffer_ref(frames_ctx->device_ref);
            }
            if (frames_ctx) {
                m_ctx->sw_pix_fmt = frames_ctx->sw_format;
            }

            m_ctx->hw_frames_ctx = av_buffer_ref(frame->hw_frames_ctx);
        }

        if (isAmf) {
            const char* inputFmtName = av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format));
            const char* selectedFmtName = av_get_pix_fmt_name(m_ctx->pix_fmt);
            const char* swFmtName = av_get_pix_fmt_name(m_ctx->sw_pix_fmt);
            std::println("AMF input pix_fmt={}, selected pix_fmt={}, sw_pix_fmt={}",
                         inputFmtName ? inputFmtName : "unknown",
                         selectedFmtName ? selectedFmtName : "unknown",
                         swFmtName ? swFmtName : "unknown");
        }

        if (!isAmf) {
            setEncoderOptionStr(m_ctx, "cq", "20");
            setEncoderOptionInt(m_ctx, "forced-idr", 1);
            setEncoderOptionInt(m_ctx, "repeat-headers", 1);
            setEncoderOptionInt(m_ctx, "delay", 0);
        } else {
            setEncoderOptionStr(m_ctx, "usage", "ultralowlatency");
            setEncoderOptionStr(m_ctx, "quality", "speed");
            setEncoderOptionStr(m_ctx, "rc", "cbr");
            setEncoderOptionInt(m_ctx, "forced-idr", 1);
            setEncoderOptionInt(m_ctx, "repeat-headers", 1);
            setEncoderOptionInt(m_ctx, "aud", 1);
            setEncoderOptionInt(m_ctx, "delay", 0);
        }

        if (avcodec_open2(m_ctx, codec, nullptr) < 0) {
            std::println(stderr, "Failed to open GPU encoder: {}", getName());
            avcodec_free_context(&m_ctx);
            return false;
        }

        if (isAmf && getName() == "h264_amf") {
            const AVBitStreamFilter* bsf = av_bsf_get_by_name("h264_mp4toannexb");
            if (bsf && av_bsf_alloc(bsf, &m_h264_annexb_bsf) >= 0) {
                if (avcodec_parameters_from_context(m_h264_annexb_bsf->par_in, m_ctx) >= 0) {
                    m_h264_annexb_bsf->time_base_in = m_ctx->time_base;
                    if (av_bsf_init(m_h264_annexb_bsf) < 0) {
                        std::println(stderr, "Failed to initialize h264_mp4toannexb bitstream filter");
                        av_bsf_free(&m_h264_annexb_bsf);
                    } else {
                        std::println("Enabled h264_mp4toannexb filter for h264_amf output");
                    }
                } else {
                    av_bsf_free(&m_h264_annexb_bsf);
                }
            }

            if (m_ctx->extradata && m_ctx->extradata_size > 0) {
                m_h264_annexb_extradata = convertH264ExtradataToAnnexB(m_ctx->extradata, m_ctx->extradata_size);
                std::println("AMF h264 extradata cached: {} bytes (annexb={})",
                             m_h264_annexb_extradata.size(),
                             m_h264_annexb_extradata.empty() ? "no" : "yes");
            }
        }

        m_ready = true;
    }

    if (getName().find("_amf") != std::string::npos && m_frame_count == 0) {
        frame->pict_type = AV_PICTURE_TYPE_I;
        frame->flags |= AV_FRAME_FLAG_KEY;
    }

    if (getName().find("_amf") != std::string::npos) {
        frame->color_range = AVCOL_RANGE_MPEG;
        frame->colorspace = AVCOL_SPC_BT709;
        frame->color_primaries = AVCOL_PRI_BT709;
        frame->color_trc = AVCOL_TRC_BT709;
    }

    if (avcodec_send_frame(m_ctx, frame) < 0) {
        if (getName().find("_amf") != std::string::npos) {
            const char* inputFmtName = av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format));
            const char* selectedFmtName = av_get_pix_fmt_name(m_ctx->pix_fmt);
            std::println(stderr, "AMF send_frame failed (input={}, selected={})", inputFmtName ? inputFmtName : "unknown", selectedFmtName ? selectedFmtName : "unknown");
        }
        return false;
    }
    ++m_frame_count;

    AVPacket* pkt = av_packet_alloc();
    while (avcodec_receive_packet(m_ctx, pkt) == 0) {
        if (m_h264_annexb_bsf) {
            const int sendRet = av_bsf_send_packet(m_h264_annexb_bsf, pkt);
            if (sendRet >= 0) {
                while (av_bsf_receive_packet(m_h264_annexb_bsf, pkt) == 0) {
                    const bool needsParamSets = !m_h264_sent_parameter_sets || ((pkt->flags & AV_PKT_FLAG_KEY) && !containsSpsPpsAnnexB(pkt->data, pkt->size));
                    if (needsParamSets && !m_h264_annexb_extradata.empty()) {
                        sendPacketToNetwork(net, pkt, &m_h264_annexb_extradata);
                        m_h264_sent_parameter_sets = true;
                    } else {
                        sendPacketToNetwork(net, pkt);
                        if (containsSpsPpsAnnexB(pkt->data, pkt->size)) {
                            m_h264_sent_parameter_sets = true;
                        }
                    }
                    av_packet_unref(pkt);
                }
            }
        } else {
            sendPacketToNetwork(net, pkt);
        }

        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    return true;
}
