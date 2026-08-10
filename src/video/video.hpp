#pragma once

#include <vector>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <print>


#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/imgutils.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/opt.h>
}

#include "encode/encoder.hpp"

struct VulkanDeviceFeatures {
    VkPhysicalDeviceFeatures2 device_features{};
    VkPhysicalDeviceVulkan11Features device_features_1_1{};
    VkPhysicalDeviceVulkan12Features device_features_1_2{};
    VkPhysicalDeviceVulkan13Features device_features_1_3{};
    VkPhysicalDeviceDescriptorBufferFeaturesEXT desc_buf_features{};
    VkPhysicalDeviceShaderAtomicFloatFeaturesEXT atomic_float_features{};
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR coop_matrix_features{};
};

struct VulkanVideoContext {
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    int presentQueueFamilyIndex = -1;
    int presentQueueCount = 0;
    int graphicsQueueFamilyIndex = -1;
    int graphicsQueueCount = 0;
    int transferQueueFamilyIndex = -1;
    int transferQueueCount = 0;
    int computeQueueFamilyIndex = -1;
    int computeQueueCount = 0;
    int decodeQueueFamilyIndex = -1;
    int decodeQueueCount = 0;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer *commandBuffers = nullptr;
    uint32_t commandBufferCount = 0;
    uint32_t commandBufferIndex = 0;

    const char **instanceExtensions = nullptr;
    int instanceExtensionsCount = 0;

    const char **deviceExtensions = nullptr;
    int deviceExtensionsCount = 0;

    VulkanDeviceFeatures features{};
};

class Video {
  public:
    Video();
    ~Video();
    void SetupVulkanRenderProperties(VulkanVideoContext *context, SDL_PropertiesID props);
    VulkanVideoContext *CreateVulkanVideoContext(SDL_Window *window);
    SDL_Texture *GetTexture() const;
    int GetTextureWidth() const;
    int GetTextureHeight() const;
    float GetVisibleUvLeft() const;
    float GetVisibleUvTop() const;
    float GetVisibleUvRight() const;
    float GetVisibleUvBottom() const;
    VulkanVideoContext *vulkan_context = nullptr;
    AVCodecContext *OpenVideoStream(AVFormatContext *ic, int stream);
    bool decodeLoop();
    Uint32 GetWakeupEventType() const;
    SDL_Renderer *renderer = nullptr;
    void receiveEncodedPacket(const std::vector<uint8_t>& packet);
    void startScreenShare(const char *encoder, int width, int height);

  private:
    struct TextureCacheEntry {
        VkImage image;
        SDL_Texture *texture;
        SDL_Colorspace colorspace;
    };
    SDL_Texture *m_texture = nullptr;
    std::vector<TextureCacheEntry> m_textureCache;
    int m_textureWidth = 0;
    int m_textureHeight = 0;
    float m_visibleUvLeft = 0.0f;
    float m_visibleUvTop = 0.0f;
    float m_visibleUvRight = 1.0f;
    float m_visibleUvBottom = 1.0f;
    AVFormatContext *m_formatContext = nullptr;
    AVCodecContext *m_codecContext = nullptr;
    AVPacket *m_packet = nullptr;
    AVFrame *m_frame = nullptr;
    int m_videoStreamIndex = -1;
    Uint64 video_start;
    bool flushing = false;
    bool decoded = false;
    int result;
    double first_pts = -1.0;
    AVCodecParserContext *m_parser = nullptr;
    std::queue<std::vector<uint8_t>> packetQueue;
    std::mutex queueMutex;
    std::condition_variable queueCV;
    const AVCodec *decoder = nullptr;
    Uint32 m_wakeupEventType = 0;

    bool ensureDecoder(SDL_Renderer *renderer);
    void releaseDecoder();
    void destroyTextureCache();
    bool GetTextureForVulkanFrame(AVFrame *frame, SDL_Texture **texture);
    bool DisplayVideoTexture(AVFrame *frame);
    bool GetTextureForFrame(AVFrame *frame, SDL_Texture **texture);
    int BeginFrameRendering(AVFrame *frame);
    int BeginVulkanFrameRendering(VulkanVideoContext *context, AVFrame *frame, SDL_Renderer *renderer);
    int CreateCommandBuffers(VulkanVideoContext *context, SDL_Renderer *renderer);
    int FinishFrameRendering(AVFrame *frame);
    int FinishVulkanFrameRendering(VulkanVideoContext *context, AVFrame *frame, SDL_Renderer *renderer);
};
