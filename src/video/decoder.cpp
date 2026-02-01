#include "decoder.hpp"
#include "screen_renderer.hpp"

Decoder::Decoder() {}

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
        if (*p == AV_PIX_FMT_CUDA) {
            std::println("Selected CUDA hardware format");
            decoder->hw_pix_fmt = AV_PIX_FMT_CUDA;
            return *p;
        }
        if (*p == decoder->hw_pix_fmt) {
            std::println("Selected hardware format: {}", av_get_pix_fmt_name(*p));
            return *p;
        }
    }

    std::println(stderr, "Failed to get HW surface format. Expected: {} or cuda", av_get_pix_fmt_name(decoder->hw_pix_fmt));
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
            std::println("Frame is not in hardware format");
        }

        av_frame_free(&frame);
    }
}

void Decoder::decoderInit() {
    int ret, i;

    while ((type = av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE) {
        std::println("{}", av_hwdevice_get_type_name(type));
    }
    type = AV_HWDEVICE_TYPE_CUDA;
    std::println("Using NVIDIA CUDA hardware decoder");

    packet = av_packet_alloc();
    if (!packet) {
        std::println("Failed to allocate AVPacket");
        return;
    }

    const char *decoder_name = "h264_cuvid";
    decoder = avcodec_find_decoder_by_name(decoder_name);
    if (!decoder) {
        std::println("NVIDIA decoder '{}' not found, falling back to software", decoder_name);
        decoder = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!decoder) {
            std::println("No decoder found for H.264");
            av_packet_free(&packet);
            return;
        }
    } else {
        std::println("Using NVIDIA hardware decoder: {}", decoder_name);
    }

    bool found_config = false;
    for (i = 0;; i++) {
        const AVCodecHWConfig *config = avcodec_get_hw_config(decoder, i);
        if (!config) {
            std::println("Decoder does not support device type: {}", av_hwdevice_get_type_name(type));
            av_packet_free(&packet);
            return;
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
        std::println("No compatible hardware config found");
        av_packet_free(&packet);
        return;
    }

    if (!(decoder_ctx = avcodec_alloc_context3(decoder))) {
        std::println("could not allocate codec context");
        av_packet_free(&packet);
        return;
    }

    decoder_ctx->opaque = this;
    decoder_ctx->get_format = getHwFormat;

    if (hwDecoderInit(decoder_ctx, type) < 0) {
        std::println("decoder init fail");
        avcodec_free_context(&decoder_ctx);
        av_packet_free(&packet);
        return;
    }

    if ((ret = avcodec_open2(decoder_ctx, decoder, nullptr)) < 0) {
        std::println("avcodec open2 fail");
        avcodec_free_context(&decoder_ctx);
        av_packet_free(&packet);
        return;
    }

    parser = av_parser_init(AV_CODEC_ID_H264);
    if (!parser) {
        std::println("Failed to create H.264 parser");
        avcodec_free_context(&decoder_ctx);
        av_packet_free(&packet);
        return;
    }

    std::println("Decoder and parser initialized, waiting for packets...");
    
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
            
            uint8_t *data = packetData.data();
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
        }
    }
    
    std::println("Decoder thread stopping...");

    if (parser) {
        av_parser_close(parser);
    }
    av_packet_free(&packet);
    avcodec_free_context(&decoder_ctx);
}
