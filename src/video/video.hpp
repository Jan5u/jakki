#pragma once

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

#define VULKAN_FUNCTIONS()                                                                                                                           \
    VULKAN_GLOBAL_FUNCTION(vkCreateInstance)                                                                                                         \
    VULKAN_GLOBAL_FUNCTION(vkEnumerateInstanceExtensionProperties)                                                                                   \
    VULKAN_GLOBAL_FUNCTION(vkEnumerateInstanceLayerProperties)                                                                                       \
    VULKAN_INSTANCE_FUNCTION(vkCreateDevice)                                                                                                         \
    VULKAN_INSTANCE_FUNCTION(vkDestroyInstance)                                                                                                      \
    VULKAN_INSTANCE_FUNCTION(vkDestroySurfaceKHR)                                                                                                    \
    VULKAN_INSTANCE_FUNCTION(vkEnumerateDeviceExtensionProperties)                                                                                   \
    VULKAN_INSTANCE_FUNCTION(vkEnumeratePhysicalDevices)                                                                                             \
    VULKAN_INSTANCE_FUNCTION(vkGetDeviceProcAddr)                                                                                                    \
    VULKAN_INSTANCE_FUNCTION(vkGetPhysicalDeviceFeatures2)                                                                                           \
    VULKAN_INSTANCE_FUNCTION(vkGetPhysicalDeviceMemoryProperties)                                                                                    \
    VULKAN_INSTANCE_FUNCTION(vkGetPhysicalDeviceQueueFamilyProperties)                                                                               \
    VULKAN_INSTANCE_FUNCTION(vkGetPhysicalDeviceSurfaceSupportKHR)                                                                                   \
    VULKAN_INSTANCE_FUNCTION(vkQueueWaitIdle)                                                                                                        \
    VULKAN_DEVICE_FUNCTION(vkAllocateCommandBuffers)                                                                                                 \
    VULKAN_DEVICE_FUNCTION(vkAllocateMemory)                                                                                                         \
    VULKAN_DEVICE_FUNCTION(vkBeginCommandBuffer)                                                                                                     \
    VULKAN_DEVICE_FUNCTION(vkBindImageMemory)                                                                                                        \
    VULKAN_DEVICE_FUNCTION(vkCmdPipelineBarrier2)                                                                                                    \
    VULKAN_DEVICE_FUNCTION(vkCreateCommandPool)                                                                                                      \
    VULKAN_DEVICE_FUNCTION(vkCreateImage)                                                                                                            \
    VULKAN_DEVICE_FUNCTION(vkCreateSemaphore)                                                                                                        \
    VULKAN_DEVICE_FUNCTION(vkDestroyCommandPool)                                                                                                     \
    VULKAN_DEVICE_FUNCTION(vkDestroyDevice)                                                                                                          \
    VULKAN_DEVICE_FUNCTION(vkDestroySemaphore)                                                                                                       \
    VULKAN_DEVICE_FUNCTION(vkDeviceWaitIdle)                                                                                                         \
    VULKAN_DEVICE_FUNCTION(vkEndCommandBuffer)                                                                                                       \
    VULKAN_DEVICE_FUNCTION(vkFreeCommandBuffers)                                                                                                     \
    VULKAN_DEVICE_FUNCTION(vkGetDeviceQueue)                                                                                                         \
    VULKAN_DEVICE_FUNCTION(vkGetImageMemoryRequirements)                                                                                             \
    VULKAN_DEVICE_FUNCTION(vkQueueSubmit)                                                                                                            \
                                                                                                                                                     \
    VULKAN_INSTANCE_FUNCTION(vkGetPhysicalDeviceVideoFormatPropertiesKHR)

typedef struct {
    VkPhysicalDeviceFeatures2 device_features;
    VkPhysicalDeviceVulkan11Features device_features_1_1;
    VkPhysicalDeviceVulkan12Features device_features_1_2;
    VkPhysicalDeviceVulkan13Features device_features_1_3;
    VkPhysicalDeviceDescriptorBufferFeaturesEXT desc_buf_features;
    VkPhysicalDeviceShaderAtomicFloatFeaturesEXT atomic_float_features;
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR coop_matrix_features;
} VulkanDeviceFeatures;

struct VulkanVideoContext {
    VkInstance instance;
    VkSurfaceKHR surface;
    VkPhysicalDevice physicalDevice;
    int presentQueueFamilyIndex;
    int presentQueueCount;
    int graphicsQueueFamilyIndex;
    int graphicsQueueCount;
    int transferQueueFamilyIndex;
    int transferQueueCount;
    int computeQueueFamilyIndex;
    int computeQueueCount;
    int decodeQueueFamilyIndex;
    int decodeQueueCount;
    VkDevice device;
    VkQueue graphicsQueue;
    VkCommandPool commandPool;
    VkCommandBuffer *commandBuffers;
    uint32_t commandBufferCount;
    uint32_t commandBufferIndex;

    const char **instanceExtensions;
    int instanceExtensionsCount;

    const char **deviceExtensions;
    int deviceExtensionsCount;

    VulkanDeviceFeatures features;

    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
#define VULKAN_GLOBAL_FUNCTION(name) PFN_##name name;
#define VULKAN_INSTANCE_FUNCTION(name) PFN_##name name;
#define VULKAN_DEVICE_FUNCTION(name) PFN_##name name;
    VULKAN_FUNCTIONS()
#undef VULKAN_GLOBAL_FUNCTION
#undef VULKAN_INSTANCE_FUNCTION
#undef VULKAN_DEVICE_FUNCTION
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
    VulkanVideoContext *vulkan_context = nullptr;
    AVCodecContext *OpenVideoStream(AVFormatContext *ic, int stream, const AVCodec *codec);
    void decodeLoop();
    SDL_Renderer *renderer = nullptr;

  private:
    SDL_Texture *m_texture = nullptr;
    int m_textureWidth = 0;
    int m_textureHeight = 0;
    AVFormatContext *m_formatContext = nullptr;
    AVCodecContext *m_codecContext = nullptr;
    AVPacket *m_packet = nullptr;
    AVFrame *m_frame = nullptr;
    int m_videoStreamIndex = -1;
    Uint64 video_start;
    bool flushing = false;
    bool decoded = false;
    int result;
    // AVFormatContext *ic = NULL;
    double first_pts = -1.0;


    bool ensureDecoder(SDL_Renderer *renderer);
    void releaseDecoder();
    bool GetTextureForVulkanFrame(AVFrame *frame, SDL_Texture **texture);
    void HandleVideoFrame(AVFrame *frame, double pts);
    void DisplayVideoFrame(AVFrame *frame);
    void DisplayVideoTexture(AVFrame *frame);
    bool GetTextureForFrame(AVFrame *frame, SDL_Texture **texture);
    int BeginFrameRendering(AVFrame *frame);
    int BeginVulkanFrameRendering(VulkanVideoContext *context, AVFrame *frame, SDL_Renderer *renderer);
    int CreateCommandBuffers(VulkanVideoContext *context, SDL_Renderer *renderer);
    int FinishFrameRendering(AVFrame *frame);
    int FinishVulkanFrameRendering(VulkanVideoContext *context, AVFrame *frame, SDL_Renderer *renderer);
};