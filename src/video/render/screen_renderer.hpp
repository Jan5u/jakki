#pragma once

#include <QFile>
#include <QVulkanFunctions>
#include <QVulkanWindow>

#include <mutex>
#include <print>

#ifdef _WIN32
#include <windows.h>
#include <vulkan/vulkan_win32.h>
#else
#include <unistd.h>
#endif

extern "C" {
#include <ffnvcodec/dynlink_loader.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
#include <libavutil/pixdesc.h>
}

struct AVFrame;

class ScreenRenderer : public QVulkanWindowRenderer {
  public:
    ScreenRenderer(QVulkanWindow *w);
    void initResources() override;
    void initSwapChainResources() override;
    void releaseSwapChainResources() override;
    void releaseResources() override;
    void startNextFrame() override;
    void receiveDecodedFrame(AVFrame *frame);
    void updateVideoImage(AVFrame *frame);

  protected:
    VkShaderModule createShader(const QString &name);
    void requestFrameUpdate();
    QVulkanWindow *m_window;
    QVulkanDeviceFunctions *m_devFuncs;
    VkDeviceMemory m_bufMem = VK_NULL_HANDLE;
    VkBuffer m_buf = VK_NULL_HANDLE;
    VkDescriptorBufferInfo m_uniformBufInfo[QVulkanWindow::MAX_CONCURRENT_FRAME_COUNT];
    VkDescriptorPool m_descPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descSetLayout = VK_NULL_HANDLE;
    VkDescriptorSet m_descSet[QVulkanWindow::MAX_CONCURRENT_FRAME_COUNT];
    VkPipelineCache m_pipelineCache = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkImage m_videoImage = VK_NULL_HANDLE;
    VkDeviceMemory m_videoImageMemory = VK_NULL_HANDLE;
    VkImageView m_videoImageView = VK_NULL_HANDLE;
    VkSampler m_videoSampler = VK_NULL_HANDLE;
    VkSamplerYcbcrConversion m_ycbcrConversion = VK_NULL_HANDLE;
    VkImageLayout m_videoImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkDescriptorPool m_imageDescPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_imageDescSetLayout = VK_NULL_HANDLE;
    VkDescriptorSet m_imageDescSet = VK_NULL_HANDLE;
    VkDescriptorImageInfo m_imageDescInfo = {};
#ifdef _WIN32
    PFN_vkGetMemoryWin32HandleKHR m_vkGetMemoryWin32HandleKHR = nullptr;
#else
    PFN_vkGetMemoryFdKHR m_vkGetMemoryFdKHR = nullptr;
#endif
    AVFrame *m_currentVideoFrame = nullptr;
    std::mutex m_frameMutex;
    bool m_hasVideoFrame = false;
    bool m_videoImageCreated = false;
    bool m_pendingFrameUpdate = false;
    uint32_t m_videoWidth = 0;
    uint32_t m_videoHeight = 0;
    void createVideoImage(uint32_t width, uint32_t height);
    void releaseVideoImage();
    void createImageDescriptorSet();
    void transitionImageLayout(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout);
    bool ensureCudaLoader();
    bool ensureCudaInterop(AVFrame *frame, VkDevice dev, CUcontext cuCtx);
    void cleanupCudaInterop();
    CudaFunctions *m_cu = nullptr;
    CUexternalMemory m_cudaExternalMemory = nullptr;
    CUstream m_cudaStream = nullptr;
    CUdeviceptr m_cudaStagingPtr = 0;
    VkBuffer m_cudaStagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_cudaStagingMemory = VK_NULL_HANDLE;
    VkDeviceSize m_cudaStagingSize = 0;
};