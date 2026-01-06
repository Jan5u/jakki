#include "decode.hpp"

static AVPixelFormat hw_pix_fmt;

Decoder::Decoder() {
    std::println("decoder constructor");
}

Decoder::~Decoder() {}

static AVPixelFormat get_hw_format(AVCodecContext *ctx, const AVPixelFormat *pix_fmts) {
    std::println("get_hw_format called, looking for: {}", av_get_pix_fmt_name(hw_pix_fmt));
    
    std::println("Available formats:");
    for (const AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        std::println("  - {}", av_get_pix_fmt_name(*p));
    }
    
    for (const AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == hw_pix_fmt) {
            std::println("Selected hardware format: {}", av_get_pix_fmt_name(*p));
            return *p;
        }
    }

    std::println(stderr, "Failed to get HW surface format. Expected: {} or cuda", av_get_pix_fmt_name(hw_pix_fmt));
    return AV_PIX_FMT_NONE;
}

void Decoder::init() {
    AVHWDeviceType type = av_hwdevice_find_type_by_name("vulkan");
    AVPacket *packet = av_packet_alloc();
    if (!packet) {
        std::println("Failed to allocate AVPacket");
    }

    AVFormatContext *input_ctx = NULL;
    avformat_open_input(&input_ctx, PATH, NULL, NULL);
    avformat_find_stream_info(input_ctx, NULL);
    int ret = av_find_best_stream(input_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    int video_stream = ret;
    for (int i = 0;; i++) {
        const AVCodecHWConfig *config = avcodec_get_hw_config(decoder, i);
        if (!config) {
            std::println("decoder {} does not support type {}", decoder->name, av_hwdevice_get_type_name(type));
            return;
        } else {
            std::println("decoder {} do support type {}", decoder->name, av_hwdevice_get_type_name(type));
        }
        std::println("conf.methods: {}", config->methods);
        std::println("devicetype: {}", av_hwdevice_get_type_name(config->device_type));
        std::println("type: {}", av_hwdevice_get_type_name(type));
        std::println("pixel format: {}", av_get_pix_fmt_name(config->pix_fmt));


        if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX && config->device_type == type) {
            hw_pix_fmt = config->pix_fmt;
            std::println("found match pixel format: {}", av_get_pix_fmt_name(hw_pix_fmt));
            break;
        }
    }

    AVCodecContext *decoder_ctx;
    if (!(decoder_ctx = avcodec_alloc_context3(decoder))) {
        std::println("could not allocate codec context");
        return;
    }

    video = input_ctx->streams[video_stream];
    if ((ret = avcodec_parameters_to_context(decoder_ctx, input_ctx->streams[video_stream]->codecpar)) < 0) {
        std::println("Failed to copy codec parameters to decoder context");
        return;
    }

    decoder_ctx->get_format = get_hw_format;

    int err = av_hwdevice_ctx_create(&hw_device_ctx, type, nullptr, nullptr, 0);
    if (err < 0) {
        std::println(stderr, "Failed to create specified HW device.");
    }
    decoder_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);

    if ((ret = avcodec_open2(decoder_ctx, decoder, NULL)) < 0) {
        std::println("avcodec_open2 fail");
        return;
    }


    int frame_count = 0;
    while (ret >= 0) {
        if ((ret = av_read_frame(input_ctx, packet)) < 0)
            break;

        if (video_stream == packet->stream_index) {
            ret = decode(decoder_ctx, packet);
            frame_count++;
        }

        av_packet_unref(packet);
    }
    std::println("decoded {} frames", frame_count);

    /* flush the decoder */
    ret = decode(decoder_ctx, NULL);

    av_packet_free(&packet);
    avcodec_free_context(&decoder_ctx);
    avformat_close_input(&input_ctx);
    av_buffer_unref(&hw_device_ctx);
}

int Decoder::decode(AVCodecContext *avctx, AVPacket *packet) {
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
            // static auto last_frame_time = std::chrono::steady_clock::now();
            // static const auto frame_duration = std::chrono::microseconds(16667);
            
            // auto now = std::chrono::steady_clock::now();
            // auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - last_frame_time);
            
            // if (elapsed < frame_duration) {
            //     std::this_thread::sleep_for(frame_duration - elapsed);
            // }
            // last_frame_time = std::chrono::steady_clock::now();

            pushFrame(std::shared_ptr<AVFrame>(frame, [](AVFrame* f){ av_frame_free(&f); }));
        } else {
            std::println("Frame is not in hardware format");
            av_frame_free(&frame);
        }
    }
}

void Decoder::pushFrame(std::shared_ptr<AVFrame> frame) {
    std::unique_lock<std::mutex> lock(queueMutex);
    if (frameQueue.size() >= MAX_QUEUE_SIZE) {
        frameQueue.pop();
    }
    frameQueue.push(std::move(frame));
    queueCond.notify_one();
}

std::shared_ptr<AVFrame> Decoder::getNextFrame() {
    std::unique_lock<std::mutex> lock(queueMutex);
    if (frameQueue.empty()) {
        return nullptr;
    }
    auto frame = frameQueue.front();
    frameQueue.pop();
    return frame;
}
