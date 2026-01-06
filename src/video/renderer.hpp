#pragma once

#include <QFile>
#include <QVulkanFunctions>
#include <QVulkanWindow>

#include <thread>

#include "decode.hpp"

class Renderer : public QVulkanWindowRenderer {
  public:
    Renderer(QVulkanWindow *w);
    void initResources() override;
    void initSwapChainResources() override;
    void releaseSwapChainResources() override;
    void releaseResources() override;
    void startNextFrame() override;

  protected:
    VkShaderModule createShader(const QString &name);
    QVulkanWindow *m_window;
    VkDevice m_device;
    QVulkanDeviceFunctions *m_devFuncs;
    VkDeviceMemory m_bufMem = VK_NULL_HANDLE;
    VkBuffer m_buf = VK_NULL_HANDLE;
    VkDescriptorPool m_descPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descSetLayout = VK_NULL_HANDLE;
    VkDescriptorSet m_descSet[QVulkanWindow::MAX_CONCURRENT_FRAME_COUNT];

    void createSampler();
    VkSampler m_ycbcrSampler = VK_NULL_HANDLE;
    VkSamplerYcbcrConversion m_ycbcrConversion = VK_NULL_HANDLE;
    VkSamplerYcbcrConversionInfo ycbcrInfo = {};

    std::unique_ptr<Decoder> pDecoder;
    std::jthread m_decoderThread;

    void createImageView();
    VkImageView m_imageView = VK_NULL_HANDLE;

    void createPipeline();
    VkPipelineCache m_pipelineCache = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;

    void createImageDescriptorSet();
    VkDescriptorSet m_imageDescSet = VK_NULL_HANDLE;

    VkImage m_videoImage;
    VkImageLayout m_videoImageLayout;
};