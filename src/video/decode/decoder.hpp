#pragma once

#include <condition_variable>
#include <mutex>
#include <print>
#include <queue>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}

class ScreenRenderer;

class Decoder {
  public:
    explicit Decoder(const std::string &preferredDecoder);
    ~Decoder();

    void startDecodeThread(ScreenRenderer *renderer);
    void receiveEncodedPacket(const std::vector<uint8_t>& packet);
    void stop();

  private:
    void decoderInit();
    int hwDecoderInit(AVCodecContext *ctx, AVHWDeviceType type);
    static AVPixelFormat getHwFormat(AVCodecContext *ctx, const AVPixelFormat *pix_fmts);
    int decodeWrite(AVCodecContext *avctx, AVPacket *packet);
    bool initHardwareDecoder(const char *decoderName, AVHWDeviceType deviceType);
    bool initSoftwareDecoder();

    AVCodecContext *decoder_ctx = nullptr;
    const AVCodec *decoder = nullptr;
    AVPacket *packet = nullptr;
    AVCodecParserContext *parser = nullptr;
    AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
    AVBufferRef *hw_device_ctx = nullptr;
    AVPixelFormat hw_pix_fmt = AV_PIX_FMT_NONE;
    std::string m_preferredDecoder;

    std::jthread decodeThread;
    std::queue<std::vector<uint8_t>> packetQueue;
    std::mutex queueMutex;
    std::condition_variable queueCV;
    bool shouldStopDecoding = false;

    ScreenRenderer *m_renderer = nullptr;
};
