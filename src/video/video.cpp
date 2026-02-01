#include "video.hpp"
#include "screen_renderer.hpp"
#include "vulkan_renderer.hpp"
#include "../network.hpp"

#ifdef _WIN32
#include "dxgi_impl.hpp"
#else
#include "pipewire_impl.hpp"
#endif

Video::Video(Config& config, Network &network) : m_network(network) {
    m_decoder = std::make_unique<Decoder>();
#ifdef _WIN32
    pImpl = std::make_unique<DxgiImpl>();
    std::println("Created DXGI video implementation");
#else
    pImpl = std::make_unique<VideoPipewireImpl>(&network);
    std::println("Created PipeWire video implementation");
#endif
    supportedNVIDIAEncoders = config.getSupportedNVIDIAEncoders();
    supportedVulkanEncoders = config.getSupportedVulkanEncoders();

    if (!supportedNVIDIAEncoders.empty() || !supportedVulkanEncoders.empty()) {
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
    if (m_vulkanWindow) {
        m_renderer = m_vulkanWindow->getRenderer();
    }
    
    if (m_decoder && m_renderer) {
        m_decoder->startDecodeThread(m_renderer);
        std::println("Decode thread started successfully");
    } else {
        m_decodeThreadStartPending = true;
        std::println("Decode thread start deferred - waiting for renderer to be ready");
    }
}

void Video::setRenderer(ScreenRenderer *renderer) {
    m_renderer = renderer;
    std::println("Renderer set for Video class");
}

void Video::receiveEncodedPacket(const std::vector<uint8_t>& packetData) {
    if (m_decoder) {
        m_decoder->receiveEncodedPacket(packetData);
    }
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
    std::vector<std::string> availableEncoders;
    AVBufferRef *hw_device_ref = nullptr;
    int ret = av_hwdevice_ctx_create(&hw_device_ref, AV_HWDEVICE_TYPE_VULKAN, nullptr, nullptr, 0);
    if (ret < 0 || !hw_device_ref) {
        return availableEncoders;
    }

    AVHWDeviceContext *dev_ctx = (AVHWDeviceContext*)hw_device_ref->data;
    AVVulkanDeviceContext *vk_ctx = (AVVulkanDeviceContext*)dev_ctx->hwctx;

    bool has_h264 = false, has_hevc = false, has_av1 = false;
    for (int i = 0; i < vk_ctx->nb_enabled_dev_extensions; ++i) {
        const char* ext = vk_ctx->enabled_dev_extensions[i];
        if (strcmp(ext, "VK_KHR_video_encode_h264") == 0) {
            has_h264 = true;
        } else if (strcmp(ext, "VK_KHR_video_encode_h265") == 0) {
            has_hevc = true;
        } else if (strcmp(ext, "VK_KHR_video_encode_av1") == 0) {
            has_av1 = true;
        }
    }

    if (has_h264) availableEncoders.push_back("h264_vulkan");
    if (has_hevc) availableEncoders.push_back("hevc_vulkan");
    if (has_av1)  availableEncoders.push_back("av1_vulkan");

    av_buffer_unref(&hw_device_ref);
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
    
    QObject::connect(m_vulkanWindow, &VulkanWindow::rendererReady, 
                     [this](ScreenRenderer *renderer) {
        m_renderer = renderer;
        std::println("Renderer ready signal received");
        
        if (m_decodeThreadStartPending && m_decoder) {
            m_decoder->startDecodeThread(m_renderer);
            m_decodeThreadStartPending = false;
            std::println("Decode thread started after renderer became ready");
        }
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


