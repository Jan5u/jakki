#include "encoder_vulkan.hpp"

#include <array>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <vector>

#include "../../network.hpp"
#include <libdrm/drm_fourcc.h>
#include <vk_video/vulkan_video_codec_h264std.h>

namespace {

const char *codecExtensionForName(const std::string &encoderName) {
    if (encoderName.find("hevc") != std::string::npos) {
        return VK_KHR_VIDEO_ENCODE_H265_EXTENSION_NAME;
    }

#ifdef VK_KHR_VIDEO_ENCODE_AV1_EXTENSION_NAME
    if (encoderName.find("av1") != std::string::npos) {
        return VK_KHR_VIDEO_ENCODE_AV1_EXTENSION_NAME;
    }
#endif

    return VK_KHR_VIDEO_ENCODE_H264_EXTENSION_NAME;
}

bool extensionSupported(const std::vector<VkExtensionProperties> &extensions, const char *name) {
    for (const auto &extension : extensions) {
        if (std::strcmp(extension.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

std::filesystem::path shaderDirectory() {
    if (const char *appdir = std::getenv("APPDIR")) {
        return std::filesystem::path(appdir) / "share" / "jakki" / "shaders";
    }
    return std::filesystem::path("shaders");
}

} // namespace

VulkanEncoder::VulkanEncoder(Network *network) : m_network(network) {}

VulkanEncoder::~VulkanEncoder() {
    cleanup();
}

auto VulkanEncoder::cleanup() -> void {
    flush();

    destroyComputePlanes(m_computePlanes);

    if (m_computeCommandBuffer != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(m_device, m_commandPool, 1, &m_computeCommandBuffer);
        m_computeCommandBuffer = VK_NULL_HANDLE;
    }
    if (m_computeDescriptorSet != VK_NULL_HANDLE) {
        vkFreeDescriptorSets(m_device, m_descriptorPool, 1, &m_computeDescriptorSet);
        m_computeDescriptorSet = VK_NULL_HANDLE;
    }
    if (m_shaderModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(m_device, m_shaderModule, nullptr);
        m_shaderModule = VK_NULL_HANDLE;
    }
    if (m_computePipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_computePipeline, nullptr);
        m_computePipeline = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
    if (m_descriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);
        m_descriptorSetLayout = VK_NULL_HANDLE;
    }
    if (m_commandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_device, m_commandPool, nullptr);
        m_commandPool = VK_NULL_HANDLE;
    }
    for (auto &[fd, imported] : m_importCache) {
        destroyImportedImage(imported);
    }
    m_importCache.clear();
    if (m_device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_device);
        vkDestroyDevice(m_device, nullptr);
        m_device = VK_NULL_HANDLE;
    }
    if (m_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
    }

    if (m_packet) {
        av_packet_free(&m_packet);
    }
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
    }
    if (m_hwFramesCtx) {
        av_buffer_unref(&m_hwFramesCtx);
    }
    if (m_hwDeviceCtx) {
        av_buffer_unref(&m_hwDeviceCtx);
    }

    m_ready = false;
    m_frameCount = 0;
}

auto VulkanEncoder::createShader(const std::filesystem::path &path) -> std::expected<VkShaderModule, std::string> {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return std::unexpected("Failed to open shader: " + path.string());
    }

    const auto size = static_cast<size_t>(file.tellg());
    if (size % sizeof(uint32_t) != 0) {
        return std::unexpected("Invalid SPIR-V size: " + path.string());
    }

    std::vector<uint32_t> code(size / sizeof(uint32_t));
    file.seekg(0);
    file.read(reinterpret_cast<char *>(code.data()), size);
    if (!file) {
        return std::unexpected("Failed to read shader: " + path.string());
    }

    VkShaderModuleCreateInfo shaderInfo{};
    shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderInfo.codeSize = size;
    shaderInfo.pCode = code.data();

    VkShaderModule shaderModule = VK_NULL_HANDLE;
    if (vkCreateShaderModule(m_device, &shaderInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        return std::unexpected("Failed to create shader module: " + path.string());
    }

    return shaderModule;
}

auto VulkanEncoder::init(const char *encoder, int width, int height) -> void {
    cleanup();

    if (!encoder) {
        std::println(stderr, "[VULKAN] Missing encoder name");
        return;
    }

    if (auto vulkan = initVulkan(encoder); !vulkan) {
        std::println(stderr, "[VULKAN] {}", vulkan.error());
        return;
    }

    if (auto ffmpeg = initEncoder(encoder, width, height); !ffmpeg) {
        std::println(stderr, "[ENCODER] {}", ffmpeg.error());
        cleanup();
        return;
    }

    m_encoder_name = encoder;
    m_width = width;
    m_height = height;
    m_ready = true;

    std::println("Vulkan encoder initialized: {}x{} using {}", width, height, encoder);
}

auto VulkanEncoder::isReady() const -> bool {
    return m_ready;
}

auto VulkanEncoder::getName() const -> std::string {
    return m_encoder_name;
}

auto VulkanEncoder::sendPacket(AVPacket *packet) -> void {
    if (!packet || !m_network) {
        return;
    }

    std::vector<uint8_t> framedPacket;
    const uint32_t packetSize = static_cast<uint32_t>(packet->size);

    framedPacket.push_back(static_cast<uint8_t>(packetSize & 0xFF));
    framedPacket.push_back(static_cast<uint8_t>((packetSize >> 8) & 0xFF));
    framedPacket.push_back(static_cast<uint8_t>((packetSize >> 16) & 0xFF));
    framedPacket.push_back(static_cast<uint8_t>((packetSize >> 24) & 0xFF));
    framedPacket.insert(framedPacket.end(), packet->data, packet->data + packet->size);

    m_network->sendScreensharePackets(framedPacket);
}

auto VulkanEncoder::flush() -> void {
    if (!m_codecCtx) {
        return;
    }

    avcodec_send_frame(m_codecCtx, nullptr);
    while (true) {
        const int ret = avcodec_receive_packet(m_codecCtx, m_packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            break;
        }

        sendPacket(m_packet);
        av_packet_unref(m_packet);
    }
}

auto VulkanEncoder::destroyImportedImage(ImportedImage &image) -> void {
    if (image.view != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, image.view, nullptr);
        image.view = VK_NULL_HANDLE;
    }
    if (image.image != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, image.image, nullptr);
        image.image = VK_NULL_HANDLE;
    }
    if (image.memory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, image.memory, nullptr);
        image.memory = VK_NULL_HANDLE;
    }
}

auto VulkanEncoder::lookupOrCreateImport(int dma_fd, int width, int height, uint64_t modifier) -> ImportedImage * {
    auto it = m_importCache.find(dma_fd);
    if (it != m_importCache.end()) {
        return &it->second;
    }

    auto imported = importDmaBufAsImage(dma_fd, width, height, modifier);
    if (!imported) {
        std::println(stderr, "{}", imported.error());
        return nullptr;
    }

    static constexpr size_t kMaxCache = 16;
    if (m_importCache.size() >= kMaxCache) {
        destroyImportedImage(m_importCache.begin()->second);
        m_importCache.erase(m_importCache.begin());
    }

    it = m_importCache.emplace(dma_fd, *imported).first;
    return &it->second;
}

auto VulkanEncoder::createComputePlanes(int width, int height) -> std::expected<ComputePlanes, std::string> {
    ComputePlanes planes;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    imageInfo.format = VK_FORMAT_R8_UNORM;
    imageInfo.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
    if (vkCreateImage(m_device, &imageInfo, nullptr, &planes.y) != VK_SUCCESS) {
        return std::unexpected("Failed to create Y plane compute image");
    }

    imageInfo.format = VK_FORMAT_R8G8_UNORM;
    imageInfo.extent = {static_cast<uint32_t>(width / 2), static_cast<uint32_t>(height / 2), 1};
    if (vkCreateImage(m_device, &imageInfo, nullptr, &planes.uv) != VK_SUCCESS) {
        destroyComputePlanes(planes);
        return std::unexpected("Failed to create UV plane compute image");
    }

    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memoryProperties);

    auto allocateAndBind = [&](VkImage image, VkDeviceMemory &memory) -> bool {
        VkMemoryRequirements memRequirements{};
        vkGetImageMemoryRequirements(m_device, image, &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;

        bool found = false;
        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
            if ((memRequirements.memoryTypeBits & (1u << i)) &&
                (memoryProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                allocInfo.memoryTypeIndex = i;
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &memory) != VK_SUCCESS) {
            return false;
        }
        if (vkBindImageMemory(m_device, image, memory, 0) != VK_SUCCESS) {
            return false;
        }
        return true;
    };

    if (!allocateAndBind(planes.y, planes.yMemory)) {
        destroyComputePlanes(planes);
        return std::unexpected("Failed to allocate memory for Y plane compute image");
    }
    if (!allocateAndBind(planes.uv, planes.uvMemory)) {
        destroyComputePlanes(planes);
        return std::unexpected("Failed to allocate memory for UV plane compute image");
    }

    auto yView = createImageView(planes.y, VK_FORMAT_R8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!yView) {
        destroyComputePlanes(planes);
        return std::unexpected(yView.error());
    }
    planes.yView = *yView;

    auto uvView = createImageView(planes.uv, VK_FORMAT_R8G8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!uvView) {
        destroyComputePlanes(planes);
        return std::unexpected(uvView.error());
    }
    planes.uvView = *uvView;

    return planes;
}

auto VulkanEncoder::destroyComputePlanes(ComputePlanes &planes) -> void {
    if (planes.yView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, planes.yView, nullptr);
        planes.yView = VK_NULL_HANDLE;
    }
    if (planes.uvView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, planes.uvView, nullptr);
        planes.uvView = VK_NULL_HANDLE;
    }
    if (planes.y != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, planes.y, nullptr);
        planes.y = VK_NULL_HANDLE;
    }
    if (planes.uv != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, planes.uv, nullptr);
        planes.uv = VK_NULL_HANDLE;
    }
    if (planes.yMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, planes.yMemory, nullptr);
        planes.yMemory = VK_NULL_HANDLE;
    }
    if (planes.uvMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, planes.uvMemory, nullptr);
        planes.uvMemory = VK_NULL_HANDLE;
    }
}

auto VulkanEncoder::createImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags) -> std::expected<VkImageView, std::string> {
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = aspectFlags;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(m_device, &viewInfo, nullptr, &view) != VK_SUCCESS) {
        return std::unexpected("Failed to create image view");
    }

    return view;
}

auto VulkanEncoder::transitionImage(VkCommandBuffer commandBuffer, VkImage image, VkImageAspectFlags aspectFlags, VkImageLayout oldLayout, VkImageLayout newLayout) -> void {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    barrier.dstStageMask = (newLayout == VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR) ? VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask = (newLayout == VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR) ? VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR : (VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspectFlags;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkDependencyInfo dependencyInfo{};
    dependencyInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependencyInfo.imageMemoryBarrierCount = 1;
    dependencyInfo.pImageMemoryBarriers = &barrier;

    vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);
}

auto VulkanEncoder::importDmaBufAsImage(int dma_fd, int width, int height, uint64_t modifier) -> std::expected<ImportedImage, std::string> {
    ImportedImage imported;

    const bool useModifier = modifier != DRM_FORMAT_MOD_LINEAR && modifier != DRM_FORMAT_MOD_INVALID;
    VkSubresourceLayout planeLayout{};
    VkImageDrmFormatModifierExplicitCreateInfoEXT modifierInfo{};

    if (useModifier) {
        planeLayout.offset = 0;
        planeLayout.size = 0;
        planeLayout.rowPitch = static_cast<uint64_t>(width) * 4;
        modifierInfo.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
        modifierInfo.drmFormatModifier = modifier;
        modifierInfo.drmFormatModifierPlaneCount = 1;
        modifierInfo.pPlaneLayouts = &planeLayout;
    }

    VkExternalMemoryImageCreateInfo externalInfo{};
    externalInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    externalInfo.pNext = useModifier ? &modifierInfo : nullptr;
    externalInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.pNext = &externalInfo;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
    imageInfo.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = useModifier ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT : VK_IMAGE_TILING_LINEAR;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(m_device, &imageInfo, nullptr, &imported.image) != VK_SUCCESS) {
        return std::unexpected("Failed to create image for DMA-BUF import");
    }

    VkMemoryRequirements memRequirements{};
    vkGetImageMemoryRequirements(m_device, imported.image, &memRequirements);

    const int dupFd = dup(dma_fd);
    if (dupFd < 0) {
        destroyImportedImage(imported);
        return std::unexpected("Failed to duplicate DMA-BUF file descriptor");
    }

    VkImportMemoryFdInfoKHR importInfo{};
    importInfo.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    importInfo.fd = dupFd;

    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memoryProperties);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.pNext = &importInfo;
    allocInfo.allocationSize = memRequirements.size;

    bool foundMemoryType = false;
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        if ((memRequirements.memoryTypeBits & (1u << i)) &&
            (memoryProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            allocInfo.memoryTypeIndex = i;
            foundMemoryType = true;
            break;
        }
    }

    if (!foundMemoryType || vkAllocateMemory(m_device, &allocInfo, nullptr, &imported.memory) != VK_SUCCESS) {
        close(dupFd);
        destroyImportedImage(imported);
        return std::unexpected("Failed to allocate memory for DMA-BUF import");
    }

    close(dupFd);

    if (vkBindImageMemory(m_device, imported.image, imported.memory, 0) != VK_SUCCESS) {
        destroyImportedImage(imported);
        return std::unexpected("Failed to bind DMA-BUF image memory");
    }

    auto view = createImageView(imported.image, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!view) {
        destroyImportedImage(imported);
        return std::unexpected(view.error());
    }

    imported.view = *view;
    return imported;
}

auto VulkanEncoder::initVulkan(const char *encoder) -> std::expected<void, std::string> {
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &appInfo;

    const std::vector<const char*> validationLayers = {
    "VK_LAYER_KHRONOS_validation"
    };

#ifdef NDEBUG
    const bool enableValidationLayers = false;
#else
    static const bool noValidation = std::getenv("JAKKI_NO_VALIDATION") != nullptr;
    const bool enableValidationLayers = !noValidation;
#endif

    if (enableValidationLayers) {
    instanceInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
    instanceInfo.ppEnabledLayerNames = validationLayers.data();
    } else {
        instanceInfo.enabledLayerCount = 0;
    }

    if (vkCreateInstance(&instanceInfo, nullptr, &m_instance) != VK_SUCCESS) {
        return std::unexpected("Failed to create Vulkan instance");
    }

    uint32_t physicalDeviceCount = 0;
    if (vkEnumeratePhysicalDevices(m_instance, &physicalDeviceCount, nullptr) != VK_SUCCESS || physicalDeviceCount == 0) {
        return std::unexpected("No Vulkan devices found");
    }

    std::vector<VkPhysicalDevice> physicalDevices(physicalDeviceCount);
    if (vkEnumeratePhysicalDevices(m_instance, &physicalDeviceCount, physicalDevices.data()) != VK_SUCCESS) {
        return std::unexpected("Failed to enumerate Vulkan devices");
    }

    const char *codecExtension = codecExtensionForName(encoder);

    for (VkPhysicalDevice device : physicalDevices) {
        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
        if (queueFamilyCount == 0) {
            continue;
        }

        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

        int computeFamily = -1;
        int transferFamily = -1;
        int encodeFamily = -1;
        for (uint32_t i = 0; i < queueFamilyCount; ++i) {
            if (computeFamily < 0 && (queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
                computeFamily = static_cast<int>(i);
            }
            if (transferFamily < 0 && (queueFamilies[i].queueFlags & VK_QUEUE_TRANSFER_BIT)) {
                transferFamily = static_cast<int>(i);
            }
            if (encodeFamily < 0 && (queueFamilies[i].queueFlags & VK_QUEUE_VIDEO_ENCODE_BIT_KHR)) {
                encodeFamily = static_cast<int>(i);
            }
        }

        if (computeFamily < 0 || transferFamily < 0 || encodeFamily < 0) {
            continue;
        }

        uint32_t deviceExtensionCount = 0;
        vkEnumerateDeviceExtensionProperties(device, nullptr, &deviceExtensionCount, nullptr);
        if (deviceExtensionCount == 0) {
            continue;
        }

        std::vector<VkExtensionProperties> deviceExtensions(deviceExtensionCount);
        if (vkEnumerateDeviceExtensionProperties(device, nullptr, &deviceExtensionCount, deviceExtensions.data()) != VK_SUCCESS) {
            continue;
        }

        std::vector<const char *> requiredExtensions{
            VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
            VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
            VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
            VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
            VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
            VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME,
            VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME,
            codecExtension,
        };

        bool extensionsSupported = true;
        for (const char *extension : requiredExtensions) {
            if (!extensionSupported(deviceExtensions, extension)) {
                extensionsSupported = false;
                break;
            }
        }
        if (!extensionsSupported) {
            continue;
        }

        m_enabledDeviceExtensions = requiredExtensions;

        m_videoMaintenance1Features = {};
        m_videoMaintenance1Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_1_FEATURES_KHR;
        m_videoMaintenance1Features.pNext = &m_deviceFeatures12;
        m_videoMaintenance1Features.videoMaintenance1 = VK_TRUE;

        m_deviceFeatures2 = {};
        m_deviceFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        m_deviceFeatures2.pNext = &m_videoMaintenance1Features;

        m_deviceFeatures12 = {};
        m_deviceFeatures12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        m_deviceFeatures12.pNext = &m_deviceFeatures13;
        m_deviceFeatures12.timelineSemaphore = VK_TRUE;

        m_deviceFeatures13 = {};
        m_deviceFeatures13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        m_deviceFeatures13.synchronization2 = VK_TRUE;

        std::array<float, 2> priorities{1.0f, 1.0f};
        std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
        queueCreateInfos.reserve(3);

        VkDeviceQueueCreateInfo computeQueueInfo{};
        computeQueueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        computeQueueInfo.queueFamilyIndex = static_cast<uint32_t>(computeFamily);
        computeQueueInfo.queueCount = 1;
        computeQueueInfo.pQueuePriorities = &priorities[0];
        queueCreateInfos.push_back(computeQueueInfo);

        if (transferFamily != computeFamily) {
            VkDeviceQueueCreateInfo transferQueueInfo{};
            transferQueueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            transferQueueInfo.queueFamilyIndex = static_cast<uint32_t>(transferFamily);
            transferQueueInfo.queueCount = 1;
            transferQueueInfo.pQueuePriorities = &priorities[1];
            queueCreateInfos.push_back(transferQueueInfo);
        }

        if (encodeFamily != computeFamily) {
            VkDeviceQueueCreateInfo encodeQueueInfo{};
            encodeQueueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            encodeQueueInfo.queueFamilyIndex = static_cast<uint32_t>(encodeFamily);
            encodeQueueInfo.queueCount = 1;
            encodeQueueInfo.pQueuePriorities = &priorities[1];
            queueCreateInfos.push_back(encodeQueueInfo);
        }

        VkDeviceCreateInfo deviceInfo{};
        deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        deviceInfo.pNext = &m_deviceFeatures2;
        deviceInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
        deviceInfo.pQueueCreateInfos = queueCreateInfos.data();
        deviceInfo.enabledExtensionCount = static_cast<uint32_t>(m_enabledDeviceExtensions.size());
        deviceInfo.ppEnabledExtensionNames = m_enabledDeviceExtensions.data();

        if (vkCreateDevice(device, &deviceInfo, nullptr, &m_device) != VK_SUCCESS) {
            continue;
        }

        m_physicalDevice = device;
        m_computeQueueFamilyIndex = static_cast<uint32_t>(computeFamily);
        m_transferQueueFamilyIndex = static_cast<uint32_t>(transferFamily);
        m_encodeQueueFamilyIndex = static_cast<uint32_t>(encodeFamily);
        vkGetDeviceQueue(m_device, m_computeQueueFamilyIndex, 0, &m_computeQueue);
        vkGetDeviceQueue(m_device, m_transferQueueFamilyIndex, 0, &m_transferQueue);
        vkGetDeviceQueue(m_device, m_encodeQueueFamilyIndex, 0, &m_encodeQueue);

        VkCommandPoolCreateInfo commandPoolInfo{};
        commandPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        commandPoolInfo.queueFamilyIndex = m_computeQueueFamilyIndex;
        commandPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

        if (vkCreateCommandPool(m_device, &commandPoolInfo, nullptr, &m_commandPool) != VK_SUCCESS) {
            return std::unexpected("Failed to create Vulkan command pool");
        }

        std::array<VkDescriptorSetLayoutBinding, 3> bindings{
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        };

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();

        if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS) {
            return std::unexpected("Failed to create Vulkan descriptor set layout");
        }

        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        poolSize.descriptorCount = 48;

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        poolInfo.maxSets = 16;

        if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
            return std::unexpected("Failed to create Vulkan descriptor pool");
        }

        auto shader = createShader(shaderDirectory() / "bgra_to_nv12.comp.spv");
        if (!shader) {
            return std::unexpected(shader.error());
        }
        m_shaderModule = *shader;

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &m_descriptorSetLayout;

        if (vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
            return std::unexpected("Failed to create Vulkan pipeline layout");
        }

        VkPipelineShaderStageCreateInfo stageInfo{};
        stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stageInfo.module = m_shaderModule;
        stageInfo.pName = "main";

        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.stage = stageInfo;
        pipelineInfo.layout = m_pipelineLayout;

        if (vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_computePipeline) != VK_SUCCESS) {
            return std::unexpected("Failed to create Vulkan compute pipeline");
        }

        std::println("Vulkan device initialized for {}", encoder);
        return {};
    }

    return std::unexpected("No Vulkan device supports the requested compute and encode queues");
}

auto VulkanEncoder::initEncoder(const char *encoder, int width, int height) -> std::expected<void, std::string> {
    const AVCodec *codec = avcodec_find_encoder_by_name(encoder);
    if (!codec) {
        return std::unexpected("Failed to find FFmpeg encoder: " + std::string(encoder));
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        return std::unexpected("Failed to allocate codec context");
    }

    m_hwDeviceCtx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN);
    if (!m_hwDeviceCtx) {
        return std::unexpected("Failed to allocate Vulkan hardware device context");
    }

    auto *deviceContext = reinterpret_cast<AVHWDeviceContext *>(m_hwDeviceCtx->data);
    auto *vkDeviceContext = reinterpret_cast<AVVulkanDeviceContext *>(deviceContext->hwctx);
    vkDeviceContext->get_proc_addr = vkGetInstanceProcAddr;
    vkDeviceContext->inst = m_instance;
    vkDeviceContext->phys_dev = m_physicalDevice;
    vkDeviceContext->act_dev = m_device;
    vkDeviceContext->device_features = m_deviceFeatures2;
    vkDeviceContext->enabled_inst_extensions = nullptr;
    vkDeviceContext->nb_enabled_inst_extensions = 0;
    vkDeviceContext->enabled_dev_extensions = m_enabledDeviceExtensions.data();
    vkDeviceContext->nb_enabled_dev_extensions = static_cast<int>(m_enabledDeviceExtensions.size());
    vkDeviceContext->nb_qf = 0;

    auto addQueueFamily = [vkDeviceContext](int idx, int num, VkQueueFlagBits flags, VkVideoCodecOperationFlagBitsKHR videoCaps) {
        AVVulkanDeviceQueueFamily *entry = &vkDeviceContext->qf[vkDeviceContext->nb_qf++];
        entry->idx = idx;
        entry->num = num;
        entry->flags = flags;
        entry->video_caps = videoCaps;
    };

    if (m_computeQueueFamilyIndex == m_encodeQueueFamilyIndex) {
        addQueueFamily(static_cast<int>(m_computeQueueFamilyIndex), 1,
                       static_cast<VkQueueFlagBits>(VK_QUEUE_COMPUTE_BIT | VK_QUEUE_VIDEO_ENCODE_BIT_KHR),
                       VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR);
    } else {
        addQueueFamily(static_cast<int>(m_computeQueueFamilyIndex), 1, VK_QUEUE_COMPUTE_BIT, VK_VIDEO_CODEC_OPERATION_NONE_KHR);
        addQueueFamily(static_cast<int>(m_transferQueueFamilyIndex), 1, VK_QUEUE_TRANSFER_BIT, VK_VIDEO_CODEC_OPERATION_NONE_KHR);
        addQueueFamily(static_cast<int>(m_encodeQueueFamilyIndex), 1, VK_QUEUE_VIDEO_ENCODE_BIT_KHR, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR);
    }

    if (av_hwdevice_ctx_init(m_hwDeviceCtx) < 0) {
        return std::unexpected("Failed to initialize FFmpeg Vulkan hardware device context");
    }

    m_hwFramesCtx = av_hwframe_ctx_alloc(m_hwDeviceCtx);
    if (!m_hwFramesCtx) {
        return std::unexpected("Failed to allocate FFmpeg Vulkan hardware frames context");
    }

    auto *framesContext = reinterpret_cast<AVHWFramesContext *>(m_hwFramesCtx->data);
    auto *vkFramesContext = reinterpret_cast<AVVulkanFramesContext *>(framesContext->hwctx);

    const int alignedWidth = (width + 15) & ~15;
    const int alignedHeight = (height + 15) & ~15;

    framesContext->format = AV_PIX_FMT_VULKAN;
    framesContext->sw_format = AV_PIX_FMT_NV12;
    framesContext->width = alignedWidth;
    framesContext->height = alignedHeight;

    vkFramesContext->tiling = VK_IMAGE_TILING_OPTIMAL;
    vkFramesContext->usage = static_cast<VkImageUsageFlagBits>(VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR
    | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    vkFramesContext->flags = static_cast<AVVkFrameFlags>(0);
    vkFramesContext->img_flags = 0;
    vkFramesContext->format[0] = VK_FORMAT_UNDEFINED;
    vkFramesContext->format[1] = VK_FORMAT_UNDEFINED;
    vkFramesContext->nb_layers = 0;

    m_encodeUsageInfo = VkVideoEncodeUsageInfoKHR{
        .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_USAGE_INFO_KHR,
        .pNext = nullptr,
        .videoUsageHints = VK_VIDEO_ENCODE_USAGE_DEFAULT_KHR,
        .videoContentHints = VK_VIDEO_ENCODE_CONTENT_DEFAULT_KHR,
        .tuningMode = VK_VIDEO_ENCODE_TUNING_MODE_LOW_LATENCY_KHR,
    };
    m_h264ProfileInfo = VkVideoEncodeH264ProfileInfoKHR{
        .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PROFILE_INFO_KHR,
        .pNext = &m_encodeUsageInfo,
        .stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH,
    };
    m_profileInfo = VkVideoProfileInfoKHR{
        .sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR,
        .pNext = &m_h264ProfileInfo,
        .videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR,
        .chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR,
        .lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR,
        .chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR,
    };
    m_profileList = VkVideoProfileListInfoKHR{
        .sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR,
        .pNext = nullptr,
        .profileCount = 1,
        .pProfiles = &m_profileInfo,
    };
    vkFramesContext->create_pnext = &m_profileList;

    if (av_hwframe_ctx_init(m_hwFramesCtx) < 0) {
        return std::unexpected("Failed to initialize FFmpeg Vulkan hardware frames context");
    }

    m_codecCtx->width = width;
    m_codecCtx->height = height;
    m_codecCtx->time_base = {1, 60};
    m_codecCtx->framerate = {60, 1};
    m_codecCtx->pix_fmt = AV_PIX_FMT_VULKAN;
    m_codecCtx->hw_frames_ctx = av_buffer_ref(m_hwFramesCtx);
    m_codecCtx->gop_size = 60;
    m_codecCtx->bit_rate = 8000000;
    m_codecCtx->rc_max_rate = 10000000;
    m_codecCtx->rc_buffer_size = 16000000;

    av_opt_set(m_codecCtx->priv_data, "preset", "p4", 0);
    av_opt_set(m_codecCtx->priv_data, "tune", "ll", 0);
    av_opt_set(m_codecCtx->priv_data, "rc_mode", "vbr", 0);
    av_opt_set_int(m_codecCtx->priv_data, "forced-idr", 1, 0);
    av_opt_set_int(m_codecCtx->priv_data, "repeat-headers", 1, 0);
    av_opt_set_int(m_codecCtx->priv_data, "delay", 0, 0);

    av_log_set_level(AV_LOG_WARNING);

    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        return std::unexpected("Failed to open FFmpeg Vulkan encoder");
    }

    m_packet = av_packet_alloc();
    if (!m_packet) {
        return std::unexpected("Failed to allocate packet");
    }

    auto planes = createComputePlanes(alignedWidth, alignedHeight);
    if (!planes) {
        return std::unexpected(planes.error());
    }
    m_computePlanes = *planes;

    VkDescriptorSetLayout layouts[] = {m_descriptorSetLayout};
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = layouts;

    if (vkAllocateDescriptorSets(m_device, &allocInfo, &m_computeDescriptorSet) != VK_SUCCESS) {
        return std::unexpected("Failed to allocate compute descriptor set");
    }

    VkCommandBufferAllocateInfo cmdAllocInfo{};
    cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAllocInfo.commandPool = m_commandPool;
    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandBufferCount = 1;

    if (vkAllocateCommandBuffers(m_device, &cmdAllocInfo, &m_computeCommandBuffer) != VK_SUCCESS) {
        return std::unexpected("Failed to allocate compute command buffer");
    }

    return {};
}

auto VulkanEncoder::encodeDmaBufFrame(int dma_fd, int width, int height, int stride, uint64_t modifier) -> bool {
    if (!m_ready || !m_codecCtx || !m_hwFramesCtx) {
        std::println(stderr, "encodeDmaBufFrame: encoder not ready");
        return false;
    }

    if (width != m_width || height != m_height) {
        std::println(stderr, "Vulkan encoder resolution change is not supported without reinitialization");
        return false;
    }

    const int alignedWidth = (width + 15) & ~15;
    const int alignedHeight = (height + 15) & ~15;

    ImportedImage *imported = lookupOrCreateImport(dma_fd, width, height, modifier);
    if (!imported) {
        return false;
    }

    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        std::println(stderr, "encodeDmaBufFrame: av_frame_alloc failed");
        return false;
    }

    int ret = av_hwframe_get_buffer(m_hwFramesCtx, frame, 0);
    if (ret < 0) {
        char err[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err, sizeof(err));
        std::println(stderr, "encodeDmaBufFrame: av_hwframe_get_buffer failed: {}", err);
        av_frame_free(&frame);
        return false;
    }

    auto *outputVkFrame = reinterpret_cast<AVVkFrame *>(frame->data[0]);
    if (!outputVkFrame) {
        std::println(stderr, "encodeDmaBufFrame: frame->data[0] is null");
        av_frame_free(&frame);
        return false;
    }
    if (outputVkFrame->img[0] == VK_NULL_HANDLE) {
        std::println(stderr, "encodeDmaBufFrame: output VkImage missing (img[0]={})",
                     (void *)outputVkFrame->img[0]);
        av_frame_free(&frame);
        return false;
    }

    ComputePlanes &planes = m_computePlanes;
    VkDescriptorSet descriptorSet = m_computeDescriptorSet;
    VkCommandBuffer commandBuffer = m_computeCommandBuffer;

    std::array<VkDescriptorImageInfo, 3> imageInfos{};
    imageInfos[0].imageView = imported->view;
    imageInfos[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    imageInfos[1].imageView = planes.yView;
    imageInfos[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    imageInfos[2].imageView = planes.uvView;
    imageInfos[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    std::array<VkWriteDescriptorSet, 3> writes{};
    for (uint32_t i = 0; i < writes.size(); ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = descriptorSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo = &imageInfos[i];
    }

    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

    vkResetCommandBuffer(commandBuffer, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        av_frame_free(&frame);
        return false;
    }

    transitionImage(commandBuffer, imported->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    transitionImage(commandBuffer, planes.y, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    transitionImage(commandBuffer, planes.uv, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_computePipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);

    const uint32_t groupCountX = static_cast<uint32_t>((alignedWidth + 15) / 16);
    const uint32_t groupCountY = static_cast<uint32_t>((alignedHeight + 15) / 16);
    vkCmdDispatch(commandBuffer, groupCountX, groupCountY, 1);

    VkImageMemoryBarrier2 copyBarriers[2]{};
    copyBarriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    copyBarriers[0].srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    copyBarriers[0].srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    copyBarriers[0].dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    copyBarriers[0].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    copyBarriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    copyBarriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    copyBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    copyBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    copyBarriers[0].image = planes.y;
    copyBarriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    copyBarriers[1] = copyBarriers[0];
    copyBarriers[1].image = planes.uv;

    VkDependencyInfo copyDependency{};
    copyDependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    copyDependency.imageMemoryBarrierCount = 2;
    copyDependency.pImageMemoryBarriers = copyBarriers;
    vkCmdPipelineBarrier2(commandBuffer, &copyDependency);

    VkImageMemoryBarrier2 frameBarrier{};
    frameBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    frameBarrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    frameBarrier.srcAccessMask = outputVkFrame->access[0];
    frameBarrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    frameBarrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    frameBarrier.oldLayout = outputVkFrame->layout[0];
    frameBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    frameBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    frameBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    frameBarrier.image = outputVkFrame->img[0];
    frameBarrier.subresourceRange = {VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 1, 0, 1};

    VkDependencyInfo frameDependency{};
    frameDependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    frameDependency.imageMemoryBarrierCount = 1;
    frameDependency.pImageMemoryBarriers = &frameBarrier;
    vkCmdPipelineBarrier2(commandBuffer, &frameDependency);

    VkImageCopy copyRegions[2]{};
    copyRegions[0].srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copyRegions[0].srcOffset = {0, 0, 0};
    copyRegions[0].dstSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1};
    copyRegions[0].dstOffset = {0, 0, 0};
    copyRegions[0].extent = {static_cast<uint32_t>(alignedWidth), static_cast<uint32_t>(alignedHeight), 1};

    copyRegions[1].srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copyRegions[1].srcOffset = {0, 0, 0};
    copyRegions[1].dstSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1};
    copyRegions[1].dstOffset = {0, 0, 0};
    copyRegions[1].extent = {static_cast<uint32_t>(alignedWidth / 2), static_cast<uint32_t>(alignedHeight / 2), 1};

    vkCmdCopyImage(commandBuffer, planes.y, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   outputVkFrame->img[0], VK_IMAGE_LAYOUT_GENERAL, 1, &copyRegions[0]);
    vkCmdCopyImage(commandBuffer, planes.uv, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   outputVkFrame->img[0], VK_IMAGE_LAYOUT_GENERAL, 1, &copyRegions[1]);

    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
        av_frame_free(&frame);
        return false;
    }

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    if (vkQueueSubmit(m_computeQueue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS) {
        av_frame_free(&frame);
        return false;
    }

    vkQueueWaitIdle(m_computeQueue);

    outputVkFrame->layout[0] = VK_IMAGE_LAYOUT_GENERAL;
    outputVkFrame->access[0] = static_cast<VkAccessFlagBits>(0);

    frame->pts = m_frameCount++;
    ret = avcodec_send_frame(m_codecCtx, frame);

    if (ret < 0) {
        char err[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err, sizeof(err));

        std::println(stderr,
            "avcodec_send_frame failed: {}",
            err);

        av_frame_free(&frame);
        return false;
    }

    av_frame_free(&frame);

    while (true) {
        ret = avcodec_receive_packet(m_codecCtx, m_packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            return false;
        }

        sendPacket(m_packet);
        av_packet_unref(m_packet);
    }

    return true;
}
