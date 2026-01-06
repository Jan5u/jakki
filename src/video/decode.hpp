#pragma once

#include <print>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/pixdesc.h>
}

#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>

#define PATH "h264.mp4"

class Decoder {
  public:
    Decoder();
    ~Decoder();
    void init();
    AVCodecContext *decoder_ctx = nullptr;
    std::shared_ptr<AVFrame> getNextFrame();

  private:
    AVStream *video = nullptr;
    AVCodecContext *ctx = nullptr;
    AVPacket *packet = nullptr;
    AVHWDeviceType type;
    AVFormatContext *input_ctx = nullptr;
    const AVCodec *decoder = nullptr;
    AVBufferRef *hw_device_ctx = nullptr;
    int decode(AVCodecContext *avctx, AVPacket *packet);
    std::queue<std::shared_ptr<AVFrame>> frameQueue;
    std::mutex queueMutex;
    std::condition_variable queueCond;
    static constexpr size_t MAX_QUEUE_SIZE = 60;
    void pushFrame(std::shared_ptr<AVFrame> frame);
};
