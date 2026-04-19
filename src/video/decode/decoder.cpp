#include "decoder.hpp"
#include "../render/screen_renderer.hpp"

Decoder::Decoder(const std::string &preferredDecoder) : m_preferredDecoder(preferredDecoder) {}

Decoder::~Decoder() {
    stop();
}

void Decoder::startDecodeThread(ScreenRenderer *renderer) {
    m_renderer = renderer;
    decodeThread = std::jthread([this] { decoderInit(); });
    std::println("Decode thread started");
}

void Decoder::receiveEncodedPacket(const std::vector<uint8_t>& packetData) {
    std::lock_guard<std::mutex> lock(queueMutex);
    packetQueue.push(packetData);
    queueCV.notify_one();
    std::println("Queued encoded packet: {} bytes (queue size: {})", packetData.size(), packetQueue.size());
}

void Decoder::stop() {
    shouldStopDecoding = true;
    queueCV.notify_all();
}

int Decoder::hwDecoderInit(AVCodecContext *ctx, AVHWDeviceType type) {
    int err = av_hwdevice_ctx_create(&hw_device_ctx, type, nullptr, nullptr, 0);

    if (err < 0) {
        std::println(stderr, "Failed to create specified HW device.");
        return err;
    }

    ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
    return err;
}

AVPixelFormat Decoder::getHwFormat(AVCodecContext *ctx, const AVPixelFormat *pix_fmts) {
    Decoder *decoder = static_cast<Decoder*>(ctx->opaque);
    std::println("get_hw_format called, looking for: {}", av_get_pix_fmt_name(decoder->hw_pix_fmt));
    
    std::println("Available formats:");
    for (const AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        std::println("  - {}", av_get_pix_fmt_name(*p));
    }
    
    for (const AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == decoder->hw_pix_fmt) {
            std::println("Selected hardware format: {}", av_get_pix_fmt_name(*p));
            return *p;
        }
    }

    std::println(stderr, "Failed to get HW surface format. Expected: {}", av_get_pix_fmt_name(decoder->hw_pix_fmt));
    return AV_PIX_FMT_NONE;
}

int Decoder::decodeWrite(AVCodecContext *avctx, AVPacket *packet) {
    int ret = avcodec_send_packet(avctx, packet);
    if (ret < 0) {
        std::println(stderr, "Error during decoding");
        return ret;
    }

    while (true) {
        AVFrame *frame = av_frame_alloc();
        if (!frame) {
            std::println(stderr, "Cannot allocate frame");
            return AVERROR(ENOMEM);
        }

        ret = avcodec_receive_frame(avctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_frame_free(&frame);
            return 0;
        } else if (ret < 0) {
            std::println(stderr, "Error while decoding");
            av_frame_free(&frame);
            return ret;
        }

        if (frame->format == hw_pix_fmt) {
            if (m_renderer) {
                m_renderer->receiveDecodedFrame(frame);
            }
        } else {
            std::println("Frame is not in supported format: {}", av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format)));
        }

        av_frame_free(&frame);
    }
}

bool Decoder::initHardwareDecoder(const char *decoderName, AVHWDeviceType deviceType) {
    int i = 0;

    if (decoderName && decoderName[0] != '\0') {
        decoder = avcodec_find_decoder_by_name(decoderName);
        if (!decoder) {
            std::println("Hardware decoder '{}' not found", decoderName);
            return false;
        }
    } else {
        decoder = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!decoder) {
            std::println("No decoder found for H.264");
            return false;
        }
    }

    type = deviceType;
    std::println("Trying hardware decoder: {} ({})", decoder->name, av_hwdevice_get_type_name(type));

    bool found_config = false;
    for (i = 0;; i++) {
        const AVCodecHWConfig *config = avcodec_get_hw_config(decoder, i);
        if (!config) {
            std::println("Decoder {} does not support device type: {}", decoder->name, av_hwdevice_get_type_name(type));
            return false;
        }
        if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
            config->device_type == type) {
            hw_pix_fmt = config->pix_fmt;
            std::println("Found hardware config with pixel format: {}", av_get_pix_fmt_name(hw_pix_fmt));
            found_config = true;
            break;
        }
    }

    if (!found_config) {
        std::println("No compatible hardware config found for {}", decoder->name);
        return false;
    }

    if (!(decoder_ctx = avcodec_alloc_context3(decoder))) {
        std::println("could not allocate codec context");
        return false;
    }

    decoder_ctx->opaque = this;
    decoder_ctx->get_format = getHwFormat;

    if (hwDecoderInit(decoder_ctx, type) < 0) {
        std::println("decoder init fail");
        avcodec_free_context(&decoder_ctx);
        if (hw_device_ctx) {
            av_buffer_unref(&hw_device_ctx);
        }
        return false;
    }

    if (avcodec_open2(decoder_ctx, decoder, nullptr) < 0) {
        std::println("avcodec open2 fail");
        avcodec_free_context(&decoder_ctx);
        if (hw_device_ctx) {
            av_buffer_unref(&hw_device_ctx);
        }
        return false;
    }

    return true;
}

bool Decoder::initSoftwareDecoder() {
    decoder = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!decoder) {
        std::println("No decoder found for H.264");
        return false;
    }

    type = AV_HWDEVICE_TYPE_NONE;
    hw_pix_fmt = AV_PIX_FMT_NONE;

    if (!(decoder_ctx = avcodec_alloc_context3(decoder))) {
        std::println("could not allocate codec context");
        return false;
    }

    if (avcodec_open2(decoder_ctx, decoder, nullptr) < 0) {
        std::println("avcodec open2 fail");
        avcodec_free_context(&decoder_ctx);
        return false;
    }

    return true;
}

void Decoder::decoderInit() {
    int ret;

    while ((type = av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE) {
        std::println("{}", av_hwdevice_get_type_name(type));
    }

    packet = av_packet_alloc();
    if (!packet) {
        std::println("Failed to allocate AVPacket");
        return;
    }

    std::vector<std::pair<const char *, AVHWDeviceType>> candidates;
    if (m_preferredDecoder == "vulkan") {
        candidates = {
            {"", AV_HWDEVICE_TYPE_VULKAN},
            {"h264_cuvid", AV_HWDEVICE_TYPE_CUDA}
        };
    } else if (m_preferredDecoder == "nvidia") {
        candidates = {
            {"h264_cuvid", AV_HWDEVICE_TYPE_CUDA},
            {"", AV_HWDEVICE_TYPE_VULKAN}
        };
    } else {
        candidates = {
            {"h264_cuvid", AV_HWDEVICE_TYPE_CUDA},
            {"", AV_HWDEVICE_TYPE_VULKAN}
        };
    }

    bool initialized = false;
    for (const auto &candidate : candidates) {
        if (initHardwareDecoder(candidate.first, candidate.second)) {
            std::println("Using hardware decoder: {}", decoder ? decoder->name : "unknown");
            initialized = true;
            break;
        }
    }

    if (!initialized) {
        std::println("Falling back to software decoder");
        if (!initSoftwareDecoder()) {
            av_packet_free(&packet);
            return;
        }
    }

    parser = av_parser_init(AV_CODEC_ID_H264);
    if (!parser) {
        std::println("Failed to create H.264 parser");
        avcodec_free_context(&decoder_ctx);
        av_packet_free(&packet);
        return;
    }

    std::println("Decoder and parser initialized, waiting for packets...");
    
    auto processPacket = [this, &ret](const std::vector<uint8_t> &packetData) {
        uint8_t *data = const_cast<uint8_t *>(packetData.data());
        size_t data_size = packetData.size();

        while (data_size > 0) {
            av_packet_unref(packet);

            ret = av_parser_parse2(parser, decoder_ctx, &packet->data, &packet->size,
                                  data, data_size, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);

            if (ret < 0) {
                std::println("Error parsing packet");
                break;
            }

            data += ret;
            data_size -= ret;

            if (packet->size > 0) {
                std::println("Parsed frame: {} bytes", packet->size);

                int decode_ret = decodeWrite(decoder_ctx, packet);
                if (decode_ret < 0) {
                    std::println("Error decoding parsed packet");
                }
            }
        }
    };

    while (!shouldStopDecoding) {
        std::unique_lock<std::mutex> lock(queueMutex);
        queueCV.wait(lock, [this] { return !packetQueue.empty() || shouldStopDecoding; });
        
        if (shouldStopDecoding && packetQueue.empty()) {
            break;
        }
        
        if (!packetQueue.empty()) {
            std::vector<uint8_t> packetData = std::move(packetQueue.front());
            packetQueue.pop();
            lock.unlock();

            std::println("Processing packet: {} bytes", packetData.size());
            processPacket(packetData);
        }
    }
    
    std::println("Decoder thread stopping...");

    if (parser) {
        av_parser_close(parser);
    }
    if (hw_device_ctx) {
        av_buffer_unref(&hw_device_ctx);
    }
    av_packet_free(&packet);
    avcodec_free_context(&decoder_ctx);
}
