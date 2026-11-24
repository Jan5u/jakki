#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <print>
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <unordered_map>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <vulkan/vulkan.h>
#ifdef _WIN32
#include <vulkan/vulkan_win32.h>
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
}

#include "encoder.hpp"

class Network;

class VulkanEncoder : public Encoder {
  public:
    explicit VulkanEncoder(Network *network = nullptr, const uint8_t *adapterLuid = nullptr);
    ~VulkanEncoder();
    void init(const char *encoder, int width, int height) override;
    auto initWithAdapter(const char *encoder, int width, int height, const uint8_t *adapterLuid) -> void;
    void flush();
    bool isReady() const;
    std::string getName() const;
    bool encodeDmaBufFrame(int dma_fd, int width, int height, int stride, uint64_t modifier);
#ifdef _WIN32
    bool encodeWin32SharedTexture(HANDLE sharedHandle, int width, int height, bool *submitted = nullptr);
#endif

  private:
    struct ImportedImage {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkImageLayout lastLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bool hasValidLayout = false;
    };

    struct ComputePlanes {
        VkImage y = VK_NULL_HANDLE;
        VkImage uv = VK_NULL_HANDLE;
        VkDeviceMemory yMemory = VK_NULL_HANDLE;
        VkDeviceMemory uvMemory = VK_NULL_HANDLE;
        VkImageView yView = VK_NULL_HANDLE;
        VkImageView uvView = VK_NULL_HANDLE;
    };

    auto initVulkan(const char *encoder) -> std::expected<void, std::string>;
    auto initEncoder(const char *encoder, int width, int height) -> std::expected<void, std::string>;
    auto createShader(const std::filesystem::path &path) -> std::expected<VkShaderModule, std::string>;
    auto importDmaBufAsImage(int dma_fd, int width, int height, uint64_t modifier) -> std::expected<ImportedImage, std::string>;
#ifdef _WIN32
    auto importWin32HandleAsImage(HANDLE sharedHandle, int width, int height) -> std::expected<ImportedImage, std::string>;
#endif
    auto createImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags) -> std::expected<VkImageView, std::string>;
    auto createComputePlanes(int width, int height) -> std::expected<ComputePlanes, std::string>;
    auto destroyComputePlanes(ComputePlanes &planes) -> void;
    auto transitionImage(VkCommandBuffer commandBuffer, VkImage image, VkImageAspectFlags aspectFlags, VkImageLayout oldLayout, VkImageLayout newLayout) -> void;
    auto destroyImportedImage(ImportedImage &image) -> void;
    auto sendPacket(AVPacket *packet) -> void;
    auto encodeImportedImage(ImportedImage *imported, int width, int height, const void *submitPNext, bool *submitted = nullptr) -> bool;

    Network *m_network = nullptr;
    std::string m_encoder_name;
    bool m_ready = false;
    int m_width = 0;
    int m_height = 0;

    std::array<uint8_t, 8> m_adapterLuid{};
    bool m_hasAdapterLuid = false;

    VkInstance m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_computeQueue = VK_NULL_HANDLE;
    VkQueue m_transferQueue = VK_NULL_HANDLE;
    VkQueue m_encodeQueue = VK_NULL_HANDLE;
    uint32_t m_computeQueueFamilyIndex = UINT32_MAX;
    uint32_t m_transferQueueFamilyIndex = UINT32_MAX;
    uint32_t m_encodeQueueFamilyIndex = UINT32_MAX;

    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_computePipeline = VK_NULL_HANDLE;
    VkShaderModule m_shaderModule = VK_NULL_HANDLE;
    ComputePlanes m_computePlanes;
    VkCommandBuffer m_computeCommandBuffer = VK_NULL_HANDLE;
    VkDescriptorSet m_computeDescriptorSet = VK_NULL_HANDLE;

    AVBufferRef *m_hwDeviceCtx = nullptr;
    AVBufferRef *m_hwFramesCtx = nullptr;
    AVCodecContext *m_codecCtx = nullptr;
    AVPacket *m_packet = nullptr;

    VkVideoEncodeUsageInfoKHR m_encodeUsageInfo{};
    VkVideoEncodeH264ProfileInfoKHR m_h264ProfileInfo{};
    VkVideoProfileInfoKHR m_profileInfo{};
    VkVideoProfileListInfoKHR m_profileList{};

    VkPhysicalDeviceFeatures2 m_deviceFeatures2{};
    VkPhysicalDeviceVideoMaintenance1FeaturesKHR m_videoMaintenance1Features{};
    VkPhysicalDeviceVulkan12Features m_deviceFeatures12{};
    VkPhysicalDeviceVulkan13Features m_deviceFeatures13{};
    std::vector<const char *> m_enabledInstanceExtensions;
    std::vector<const char *> m_enabledDeviceExtensions;
    std::vector<std::string> m_supportedDeviceExtensionNames;

    int64_t m_frameCount = 0;
    std::unordered_map<uintptr_t, ImportedImage> m_importCache{};

    auto lookupOrCreateImport(uintptr_t key, int width, int height, uint64_t modifier) -> ImportedImage *;

    auto cleanup() -> void;
};
