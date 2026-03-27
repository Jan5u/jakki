#include "video.hpp"
#include "render/screen_renderer.hpp"
#include "render/vulkan_renderer.hpp"
#include "encode/encoder.hpp"

#ifdef _WIN32
#include "capture/capture_windows.hpp"
#else
#include "capture/capture_linux.hpp"
#endif

Video::Video(Config& config, Network &network) : m_network(network) {
    m_decoder = std::make_unique<Decoder>(config.getPreferredDecoder());
#ifdef _WIN32
    pImpl = std::make_unique<DxgiCapture>(&network);
    std::println("Created DXGI video implementation");
#else
    pImpl = std::make_unique<PipewireCapture>(&network);
    std::println("Created PipeWire video implementation");
#endif
    supportedNVIDIAEncoders = config.getSupportedNVIDIAEncoders();
    supportedVulkanEncoders = config.getSupportedVulkanEncoders();

    if (!supportedNVIDIAEncoders.empty() || !supportedVulkanEncoders.empty()) {
        std::println("Loaded supported encoders from config: NVIDIA={}, Vulkan={}", supportedNVIDIAEncoders.size(), supportedVulkanEncoders.size());
    } else {
        nvidiaEncoderThread = std::jthread([this, &config]() {
            supportedNVIDIAEncoders = Encoder::getSupportedNVIDIAEncoders();
            config.setSupportedNVIDIAEncoders(supportedNVIDIAEncoders);
            std::println("NVIDIA encoder detection complete: {} encoders found", supportedNVIDIAEncoders.size());
        });
        vulkanEncoderThread = std::jthread([this, &config]() {
            supportedVulkanEncoders = Encoder::getSupportedVulkanEncoders();
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

void Video::startScreenShareCapture() {
    if (!pImpl) {
        return;
    }
    pImpl->startCapture();
}

void Video::startScreenShareEncoding(const std::string& encoderName) {
    if (!pImpl) {
        return;
    }
    if (encoderName.empty()) {
        std::println(stderr, "No encoder selected for screen share");
        return;
    }

    try {
        EncoderType encoderType = Encoder::nameToEncoderType(encoderName);
        pImpl->startEncoding(encoderType);
    } catch (const std::exception& ex) {
        std::println(stderr, "Unsupported encoder selected: {}", encoderName);
        std::println(stderr, "Details: {}", ex.what());
    }
}

void Video::stopScreenShareCapture() {
    if (!pImpl) {
        return;
    }
    pImpl->stopCapture();
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
    std::println("Vulkan tab created");
    
    return vulkanTab;
}


