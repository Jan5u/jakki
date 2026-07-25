#include "encoder.hpp"
#include "encoder_vulkan.hpp"

std::unique_ptr<Encoder> CreateEncoder(Backend backend) {
    switch (backend) {
    case Backend::Vulkan:
        return std::make_unique<VulkanEncoder>();
#ifdef _WIN32
    case Backend::D3D12:
        return std::make_unique<D3D12Encoder>();
#endif
    }

    return nullptr;
}

std::vector<std::string> Encoder::getSupportedVulkanEncoders() {
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

std::vector<std::string> Encoder::getSupportedEncoders() {
    std::vector<std::string> encoders;
    auto vulkanEncoders = getSupportedVulkanEncoders();
    encoders.insert(encoders.end(), vulkanEncoders.begin(), vulkanEncoders.end());
    return encoders;
}
