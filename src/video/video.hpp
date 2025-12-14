#pragma once

#include <QDebug>
#include <QSize>
#include <QVersionNumber>
#include <QVulkanInstance>
#include <QWidget>
#include <chrono>
#include <print>
#include <string>
#include <thread>
#include <vector>

#include "../config.hpp"
#include "video_impl.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

class QWidget;
class QVulkanInstance;
class VulkanWindow;
class ScreenRenderer;

class Video {
  public:
    Video(Config &config);
    ~Video();
    VulkanWindow *createVulkanWindow();
    QWidget *createVulkanTab(QWidget *parent);
    void selectScreen();
    void startDecodeThread();
    std::vector<std::string> getSupportedEncoders();
    std::vector<std::string> getSupportedNVIDIAEncoders();
    std::vector<std::string> getSupportedVulkanEncoders();
    std::vector<std::string> supportedNVIDIAEncoders;
    std::vector<std::string> supportedVulkanEncoders;

  private:
    AVFormatContext *input_ctx = nullptr;
    AVStream *video = nullptr;
    AVCodecContext *decoder_ctx = nullptr;
    const AVCodec *decoder = nullptr;
    AVPacket *packet = nullptr;
    AVHWDeviceType type;
    AVPixelFormat hw_pix_fmt;
    AVBufferRef *hw_device_ctx = nullptr;
    std::jthread decodeThread;
    std::jthread nvidiaEncoderThread;
    std::jthread vulkanEncoderThread;
    void decoderInit();

    VulkanWindow *m_vulkanWindow = nullptr;
    ScreenRenderer *m_renderer = nullptr;
    std::unique_ptr<VideoImpl> pImpl;
};