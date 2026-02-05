#include "encoder.hpp"

#ifdef __linux__
#include "nvenc_linux.hpp"
#endif

std::string Encoder::encoderTypeToName(EncoderType type) {
    switch (type) {
        case EncoderType::NVENC_H264: return "h264_nvenc";
        case EncoderType::NVENC_HEVC: return "hevc_nvenc";
        case EncoderType::NVENC_AV1:  return "av1_nvenc";
        default: return "unknown";
    }
}

EncoderType Encoder::nameToEncoderType(const std::string& name) {
    if (name == "h264_nvenc") return EncoderType::NVENC_H264;
    if (name == "hevc_nvenc") return EncoderType::NVENC_HEVC;
    if (name == "av1_nvenc")  return EncoderType::NVENC_AV1;
    throw std::runtime_error("Unknown encoder name: " + name);
}

std::vector<EncoderType> Encoder::getAvailableEncoders() {
    return {
        EncoderType::NVENC_H264,
        EncoderType::NVENC_HEVC,
        EncoderType::NVENC_AV1,
    };
}

std::vector<std::string> Encoder::getSupportedNVIDIAEncoders() {
    static const std::vector<std::string> nvencEncoders = {
        "av1_nvenc",
        "hevc_nvenc",
        "h264_nvenc"
    };
    
    std::vector<std::string> availableEncoders;
    
    for (const auto& encoder_name : nvencEncoders) {
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
    
    auto nvidiaEncoders = getSupportedNVIDIAEncoders();
    auto vulkanEncoders = getSupportedVulkanEncoders();
    
    encoders.insert(encoders.end(), nvidiaEncoders.begin(), nvidiaEncoders.end());
    encoders.insert(encoders.end(), vulkanEncoders.begin(), vulkanEncoders.end());
    
    return encoders;
}

#ifdef __linux__
std::unique_ptr<DmaBufEncoder> DmaBufEncoder::create(EncoderType type, Network* network) {
    switch (type) {
        case EncoderType::NVENC_H264:
        case EncoderType::NVENC_HEVC:
        case EncoderType::NVENC_AV1:
            return std::make_unique<NvencLinuxEncoder>(network, type);
        default:
            return nullptr;
    }
}
#elif defined(_WIN32)
std::unique_ptr<D3D11Encoder> D3D11Encoder::create(EncoderType type, Network* network) {
    switch (type) {
        case EncoderType::NVENC_H264:
        case EncoderType::NVENC_HEVC:
        case EncoderType::NVENC_AV1:
            return std::make_unique<NvencWindowsEncoder>(network, type);
        default:
            return nullptr;
    }
}
#endif
