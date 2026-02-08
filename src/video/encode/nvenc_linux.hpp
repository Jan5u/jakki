#pragma once

#include "encoder.hpp"

#include <print>
#include <cstdio>
#include <vector>
#include <fstream>
#include <unistd.h>

#include <QFile>
#include <QVulkanInstance>
#include <QVulkanFunctions>
#include <QVulkanDeviceFunctions>

#include <libdrm/drm_fourcc.h>

extern "C" {
#include <ffnvcodec/dynlink_loader.h>
#include <libavutil/frame.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
#include <libavutil/pixdesc.h>
#include <libavutil/opt.h>
}

class Network;

class NvencLinuxEncoder : public DmaBufEncoder {
public:
    NvencLinuxEncoder(Network* network, EncoderType type = EncoderType::NVENC_H264);
    ~NvencLinuxEncoder() override;
    void init() override;
    void flush() override;
    bool isReady() const override;
    std::string getName() const override;
    bool encodeDmaBufFrame(int dma_fd, int width, int height, int stride, uint64_t modifier) override;

private:
    AVCodecContext *codec_ctx = nullptr;
    AVPacket *packet = nullptr;
    AVFrame *av_frame = nullptr;
    AVBufferRef *hw_device_ctx = nullptr;
    AVBufferRef *hw_frames_ctx = nullptr;
    int64_t frame_count = 0;
    FILE *output_file = nullptr;
    FILE *bgra_file = nullptr;
    FILE *nv12_file = nullptr;
    Network* m_network = nullptr;
    EncoderType m_encoder_type;
    bool m_ready = false;
    
    CUcontext cuda_ctx = nullptr;
    CudaFunctions *cu = nullptr;
    CUstream cuda_stream = nullptr;
    QVulkanInstance *vk_instance = nullptr;
    QVulkanFunctions *vk_funcs = nullptr;
    QVulkanDeviceFunctions *vk_dev_funcs = nullptr;
    VkPhysicalDevice vk_physical_device = VK_NULL_HANDLE;
    VkDevice vk_device = VK_NULL_HANDLE;
    
    VkQueue vk_compute_queue = VK_NULL_HANDLE;
    uint32_t vk_queue_family_index = 0;
    VkCommandPool vk_command_pool = VK_NULL_HANDLE;
    VkDescriptorSetLayout vk_descriptor_set_layout = VK_NULL_HANDLE;
    VkDescriptorPool vk_descriptor_pool = VK_NULL_HANDLE;
    VkPipelineLayout vk_pipeline_layout = VK_NULL_HANDLE;
    VkPipeline vk_compute_pipeline = VK_NULL_HANDLE;
    VkShaderModule vk_shader_module = VK_NULL_HANDLE;
    
    VkSemaphore vk_timeline_semaphore = VK_NULL_HANDLE;
    uint64_t vk_timeline_value = 0;
    
    struct PendingFrame {
        uint64_t timeline_value;
        int frame_index;
        VkImage input_image;
        VkImageView input_view;
        VkDeviceMemory input_memory;
    };
    
    struct FrameResources {
        VkCommandBuffer command_buffer;
        VkDescriptorSet descriptor_set;
        VkImage y_image;
        VkImage uv_image;
        VkImageView y_view;
        VkImageView uv_view;
        VkDeviceMemory y_memory;
        VkDeviceMemory uv_memory;
        CUexternalMemory cuda_ext_mem_y;
        CUexternalMemory cuda_ext_mem_uv;
        CUdeviceptr cuda_ptr_y;
        CUdeviceptr cuda_ptr_uv;
        size_t y_pitch;
        size_t uv_pitch;
        bool in_use;
    };
    
    std::vector<PendingFrame> pending_frames;
    std::vector<FrameResources> frame_resources;
    static constexpr int MAX_FRAMES_IN_FLIGHT = 3;
    int current_width = 0;
    int current_height = 0;
    
    void initFrameResources();
    void cleanupFrameResources();
    void cleanupCompletedFrames();
    int acquireFrameResources();
    bool initFFmpegEncoder(int width, int height);
    bool initCUDA();
    bool setupCUDAInterop(FrameResources& res, int width, int height);
    bool encodeFrame(FrameResources& res, uint64_t timeline_value);
    VkShaderModule createShader(const QString &name);
    VkImage importDmaBufAsImage(int dma_fd, int width, int height, uint64_t modifier, VkDeviceMemory& memory);
    VkImage createNV12Image(int width, int height, VkImageAspectFlags aspect, VkFormat format, VkDeviceMemory& memory);
    VkImageView createImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags);
    void transitionImageLayout(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout);
    void saveBGRAFrame(VkImage image, int width, int height);
    void saveNV12Frame(FrameResources& res, int width, int height);

    const char* getFFmpegEncoderName() const;
};
