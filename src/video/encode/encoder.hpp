#pragma once

#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/opt.h>
}

enum class Backend { Vulkan, D3D12 };

class Encoder {
  public:
    virtual ~Encoder() = default;
    virtual void init(const char *encoder, int width, int height) = 0;
    static std::vector<std::string> getSupportedVulkanEncoders();
    static std::vector<std::string> getSupportedEncoders();

  private:
};

std::unique_ptr<Encoder> CreateEncoder(Backend backend);