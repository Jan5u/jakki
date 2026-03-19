#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <cstring>
#include <print>
#include <stdexcept>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_
#endif
#include <winsock2.h>
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
}

class Network;

enum class EncoderType {
    NVENC_H264,
    NVENC_HEVC,
    NVENC_AV1,
};

class Encoder {
public:
    virtual ~Encoder() = default;
    virtual void init() = 0;
    virtual void flush() = 0;
    virtual bool isReady() const = 0;
    virtual std::string getName() const = 0;
    static std::string encoderTypeToName(EncoderType type);
    static EncoderType nameToEncoderType(const std::string& name);
    static std::vector<EncoderType> getAvailableEncoders();
    static std::vector<std::string> getSupportedNVIDIAEncoders();
    static std::vector<std::string> getSupportedVulkanEncoders();
    static std::vector<std::string> getSupportedEncoders();
};

#ifdef __linux__
class DmaBufEncoder : public Encoder {
public:
    virtual bool encodeDmaBufFrame(int dma_fd, int width, int height, int stride, uint64_t modifier) = 0;
    static std::unique_ptr<DmaBufEncoder> create(EncoderType type, Network* network);
};
#endif

#ifdef _WIN32
class D3D11Encoder : public Encoder {
public:
    virtual bool encodeD3D11Frame(AVFrame* frame) = 0;
    static std::unique_ptr<D3D11Encoder> create(EncoderType type, Network* network);
};
#endif