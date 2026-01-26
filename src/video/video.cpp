#include "video.hpp"
#include "screen_renderer.hpp"
#include "vulkan_renderer.hpp"

#ifdef _WIN32
#include "dxgi_impl.hpp"
#else
#include "pipewire_impl.hpp"
#endif


static AVBufferRef *hw_device_ctx = nullptr;
static AVPixelFormat hw_pix_fmt;
ScreenRenderer *g_renderer = nullptr;

Video::Video(Config& config) {
#ifdef _WIN32
    pImpl = std::make_unique<DxgiImpl>();
    std::println("Created DXGI video implementation");
#else
    pImpl = std::make_unique<VideoPipewireImpl>();
    std::println("Created PipeWire video implementation");
#endif
    supportedNVIDIAEncoders = config.getSupportedNVIDIAEncoders();
    supportedVulkanEncoders = config.getSupportedVulkanEncoders();

    if (!supportedNVIDIAEncoders.empty() && !supportedVulkanEncoders.empty()) {
        std::println("Loaded supported encoders from config: NVIDIA={}, Vulkan={}", supportedNVIDIAEncoders.size(), supportedVulkanEncoders.size());
    } else {
        nvidiaEncoderThread = std::jthread([this, &config]() {
            supportedNVIDIAEncoders = getSupportedNVIDIAEncoders();
            config.setSupportedNVIDIAEncoders(supportedNVIDIAEncoders);
            std::println("NVIDIA encoder detection complete: {} encoders found", supportedNVIDIAEncoders.size());
        });
        vulkanEncoderThread = std::jthread([this, &config]() {
            supportedVulkanEncoders = getSupportedVulkanEncoders();
            config.setSupportedVulkanEncoders(supportedVulkanEncoders);
            std::println("Vulkan encoder detection complete: {} encoders found", supportedVulkanEncoders.size());
        });
    }
}

Video::~Video() {}

void Video::selectScreen() {
    if (pImpl) {
        pImpl->selectScreen();
    }
}

void Video::startDecodeThread() {
    decodeThread = std::jthread([this] { decoderInit(); });
    std::println("Decode thread started");
}

std::vector<std::string> Video::getSupportedNVIDIAEncoders() {
    static const std::vector<std::string> supportedNVIDIAEncoders = {
        "av1_nvenc",
        "hevc_nvenc",
        "h264_nvenc"
    };
    
    std::vector<std::string> availableEncoders;
    
    for (const auto& encoder_name : supportedNVIDIAEncoders) {
        const AVCodec *codec = avcodec_find_encoder_by_name(encoder_name.c_str());
        if (!codec) {
            continue;
        }

        AVCodecContext *ctx = avcodec_alloc_context3(codec);
        if (!ctx) {
            continue;
        }

        ctx->pix_fmt = AV_PIX_FMT_NV12;
        ctx->width = 1920;
        ctx->height = 1080;
        ctx->time_base = (AVRational){1, 25};
        ctx->framerate = (AVRational){25, 1};

        if (avcodec_open2(ctx, codec, NULL) == 0) {
            availableEncoders.push_back(encoder_name);
        }
        
        avcodec_free_context(&ctx);
    }
    
    return availableEncoders;
}

std::vector<std::string> Video::getSupportedVulkanEncoders() {
    static const std::vector<std::string> supportedVulkanEncoders = {
        "av1_vulkan",
        "hevc_vulkan",
        "h264_vulkan"
    };
    
    std::vector<std::string> availableEncoders;
    
    for (const auto& encoder_name : supportedVulkanEncoders) {
        const AVCodec *codec = avcodec_find_encoder_by_name(encoder_name.c_str());
        if (!codec) {
            continue;
        }

        AVCodecContext *ctx = avcodec_alloc_context3(codec);
        if (!ctx) {
            continue;
        }

        AVBufferRef *hw_device_ref = nullptr;
        int ret = av_hwdevice_ctx_create(&hw_device_ref, AV_HWDEVICE_TYPE_VULKAN, nullptr, nullptr, 0);
        if (ret < 0) {
            avcodec_free_context(&ctx);
            continue;
        }

        AVBufferRef *hw_frames_ref = av_hwframe_ctx_alloc(hw_device_ref);
        if (!hw_frames_ref) {
            av_buffer_unref(&hw_device_ref);
            avcodec_free_context(&ctx);
            continue;
        }

        AVHWFramesContext *frames_ctx = (AVHWFramesContext*)hw_frames_ref->data;
        frames_ctx->format = AV_PIX_FMT_VULKAN;
        frames_ctx->sw_format = AV_PIX_FMT_NV12;
        frames_ctx->width = 1920;
        frames_ctx->height = 1080;

        ret = av_hwframe_ctx_init(hw_frames_ref);
        if (ret < 0) {
            av_buffer_unref(&hw_frames_ref);
            av_buffer_unref(&hw_device_ref);
            avcodec_free_context(&ctx);
            continue;
        }

        ctx->width  = 1920;
        ctx->height = 1080;
        ctx->pix_fmt = AV_PIX_FMT_VULKAN;
        ctx->time_base = (AVRational){1, 25};
        ctx->framerate = (AVRational){25, 1};
        ctx->hw_frames_ctx = av_buffer_ref(hw_frames_ref);

        ret = avcodec_open2(ctx, codec, NULL);
        if (ret == 0) {
            availableEncoders.push_back(encoder_name);
        }

        av_buffer_unref(&hw_frames_ref);
        av_buffer_unref(&hw_device_ref);
        avcodec_free_context(&ctx);
    }
    
    return availableEncoders;
}

std::vector<std::string> Video::getSupportedEncoders() {
    std::vector<std::string> encoders;
    
    auto nvidiaEncoders = getSupportedNVIDIAEncoders();
    auto vulkanEncoders = getSupportedVulkanEncoders();
    
    encoders.insert(encoders.end(), nvidiaEncoders.begin(), nvidiaEncoders.end());
    encoders.insert(encoders.end(), vulkanEncoders.begin(), vulkanEncoders.end());
    
    return encoders;
}

VulkanWindow* Video::createVulkanWindow() {
    QVulkanInstance *inst = new QVulkanInstance;
    inst->setApiVersion(QVersionNumber(1, 3, 0));
    inst->setLayers({"VK_LAYER_KHRONOS_validation"});
    // inst->setExtensions({});
    if (!inst->create()) {
        qFatal("Failed to create Vulkan instance: %d", inst->errorCode());
    }

    m_vulkanWindow = new VulkanWindow;
    m_vulkanWindow->setVulkanInstance(inst);
    m_vulkanWindow->setEnabledFeaturesModifier([](VkPhysicalDeviceFeatures2 &features) {
        static VkPhysicalDeviceDynamicRenderingFeatures dynamicRenderingFeatures = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES,
            .pNext = features.pNext,
            .dynamicRendering = VK_TRUE
        };
        features.pNext = &dynamicRenderingFeatures;

        static VkPhysicalDeviceDynamicRenderingLocalReadFeatures dynamicRenderingLocalReadFeatures = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES,
            .pNext = features.pNext,
            .dynamicRenderingLocalRead = VK_TRUE
        };
        features.pNext = &dynamicRenderingLocalReadFeatures;

        static VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR unifiedImageLayoutsFeatures = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFIED_IMAGE_LAYOUTS_FEATURES_KHR,
            .pNext = features.pNext,
            .unifiedImageLayouts = VK_TRUE,
            .unifiedImageLayoutsVideo = VK_TRUE
        };
        features.pNext = &unifiedImageLayoutsFeatures;

        static VkPhysicalDeviceTimelineSemaphoreFeatures timelineSemaphoreFeatures = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
            .pNext = features.pNext,
            .timelineSemaphore = VK_TRUE
        };
        features.pNext = &timelineSemaphoreFeatures;

        static VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcrConversionFeatures = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,
            .pNext = features.pNext,
            .samplerYcbcrConversion = VK_TRUE
        };
        features.pNext = &ycbcrConversionFeatures;
    });
    
    m_vulkanWindow->setDeviceExtensions({
        "VK_KHR_dynamic_rendering_local_read",
        "VK_KHR_external_memory_fd",
        "VK_EXT_external_memory_dma_buf",
        "VK_KHR_external_memory_win32"
    });
    std::println("Vulkan window created");
    return m_vulkanWindow;
}

QWidget* Video::createVulkanTab(QWidget *parent) {
    if (!m_vulkanWindow) {
        std::println(stderr, "Vulkan window not initialized");
        return nullptr;
    }
    
    QWidget *vulkanTab = QWidget::createWindowContainer(m_vulkanWindow, parent);
    vulkanTab->setMinimumSize(QSize(640, 480));
    vulkanTab->setMaximumSize(QSize(1920, 1080));
    std::println("Vulkan tab created");
    
    return vulkanTab;
}

static int hw_decoder_init(AVCodecContext *ctx, AVHWDeviceType type) {
    int err = av_hwdevice_ctx_create(&hw_device_ctx, type, nullptr, nullptr, 0);

    if (err < 0) {
        std::println(stderr, "Failed to create specified HW device.");
        return err;
    }

    ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
    return err;
}

static AVPixelFormat get_hw_format(AVCodecContext *ctx, const AVPixelFormat *pix_fmts) {
    std::println("get_hw_format called, looking for: {}", av_get_pix_fmt_name(hw_pix_fmt));
    
    std::println("Available formats:");
    for (const AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        std::println("  - {}", av_get_pix_fmt_name(*p));
    }
    
    for (const AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_CUDA) {
            std::println("Selected CUDA hardware format");
            hw_pix_fmt = AV_PIX_FMT_CUDA;
            return *p;
        }
        if (*p == hw_pix_fmt) {
            std::println("Selected hardware format: {}", av_get_pix_fmt_name(*p));
            return *p;
        }
    }

    std::println(stderr, "Failed to get HW surface format. Expected: {} or cuda", av_get_pix_fmt_name(hw_pix_fmt));
    return AV_PIX_FMT_NONE;
}

static int decode_write(AVCodecContext *avctx, AVPacket *packet)
{
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
            static auto last_frame_time = std::chrono::steady_clock::now();
            static const auto frame_duration = std::chrono::microseconds(16667);
            
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - last_frame_time);
            
            if (elapsed < frame_duration) {
                std::this_thread::sleep_for(frame_duration - elapsed);
            }
            last_frame_time = std::chrono::steady_clock::now();
            
            extern ScreenRenderer *g_renderer;
            if (g_renderer) {
                g_renderer->receiveDecodedFrame(frame);
            }
        } else {
            std::println("Frame is not in hardware format");
        }

        av_frame_free(&frame);
    }
}

void Video::decoderInit() {
    const char path[] = "./output.h264";
    int video_stream, ret, i;

    while ((type = av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE) {
        std::println("{}", av_hwdevice_get_type_name(type));
    }
    type = AV_HWDEVICE_TYPE_CUDA;
    std::println("Using NVIDIA CUDA hardware decoder");

    packet = av_packet_alloc();
    if (!packet) {
        std::println("Failed to allocate AVPacket");
    }

    if (avformat_open_input(&input_ctx, path, NULL, NULL) != 0) {
        std::println("Cannot open input file");
    }

    if (avformat_find_stream_info(input_ctx, NULL) < 0) {
        std::println("Cannot find input stream information");
    }

    ret = av_find_best_stream(input_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (ret < 0) {
        std::println("Cannot find a video stream in the input file");
        avformat_close_input(&input_ctx);
        return;
    }
    video_stream = ret;
    
    AVCodecID codec_id = input_ctx->streams[video_stream]->codecpar->codec_id;
    const char *decoder_name = nullptr;
    switch (codec_id) {
        case AV_CODEC_ID_H264:
            decoder_name = "h264_cuvid";
            break;
        case AV_CODEC_ID_HEVC:
            decoder_name = "hevc_cuvid";
            break;
        case AV_CODEC_ID_VP9:
            decoder_name = "vp9_cuvid";
            break;
        case AV_CODEC_ID_AV1:
            decoder_name = "av1_cuvid";
            break;
        default:
            std::println("Unsupported codec for NVIDIA hardware decoding");
            avformat_close_input(&input_ctx);
            return;
    }
    
    decoder = avcodec_find_decoder_by_name(decoder_name);
    if (!decoder) {
        std::println("NVIDIA decoder '{}' not found, falling back to software", decoder_name);
        decoder = avcodec_find_decoder(codec_id);
        if (!decoder) {
            std::println("No decoder found for codec");
            avformat_close_input(&input_ctx);
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
            avformat_close_input(&input_ctx);
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
        avformat_close_input(&input_ctx);
        return;
    }

    if (!(decoder_ctx = avcodec_alloc_context3(decoder))) {
        std::println("could not allocate codec context");
        return;
    }

    if ((ret = avcodec_parameters_to_context(decoder_ctx, input_ctx->streams[video_stream]->codecpar)) < 0) {
        std::println("Failed to copy codec parameters to decoder context");
        return;
    }

    decoder_ctx->get_format = get_hw_format;

    if (hw_decoder_init(decoder_ctx, type) < 0) {
        std::println("decoder init fail");
        return;
    }

    if ((ret = avcodec_open2(decoder_ctx, decoder, nullptr)) < 0) {
        std::println("avcodec open2 fail");
        return;
    }

    std::println("decode start...");
    while (ret >= 0) {
        if ((ret = av_read_frame(input_ctx, packet)) < 0) {
            break;
        }

        if (video_stream == packet->stream_index) {
            ret = decode_write(decoder_ctx, packet);
        }

        av_packet_unref(packet);
    }
    std::println("decoding end...");

    av_packet_free(&packet);
    avcodec_free_context(&decoder_ctx);
    avformat_close_input(&input_ctx);
}


