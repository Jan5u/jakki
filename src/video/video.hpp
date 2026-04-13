#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_
#endif
#include <winsock2.h>
#endif

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
#include <queue>
#include <mutex>
#include <condition_variable>

#include "../config.hpp"
#include "capture/capture.hpp"
#include "decode/decoder.hpp"

class Network;

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

class QWidget;
class QVulkanInstance;
class VulkanWindow;
class ScreenRenderer;

class Video {
  public:
    Video(Config &config, Network &network);
    ~Video();
    VulkanWindow *createVulkanWindow();
    QWidget *createVulkanTab(QWidget *parent);
    void selectScreen();
    void startScreenShareCapture();
    void startScreenShareEncoding(const std::string& encoderName);
    void stopScreenShareCapture();
    void startDecodeThread();
    void setRenderer(ScreenRenderer *renderer);
    void receiveEncodedPacket(const std::vector<uint8_t>& packet);
    std::vector<std::string> supportedNVIDIAEncoders;
    std::vector<std::string> supportedAMDEncoders;
    std::vector<std::string> supportedVulkanEncoders;

  private:
    std::jthread nvidiaEncoderThread;
    std::jthread amdEncoderThread;
    std::jthread vulkanEncoderThread;

    VulkanWindow *m_vulkanWindow = nullptr;
    ScreenRenderer *m_renderer = nullptr;
    std::unique_ptr<Capture> pImpl;
    std::unique_ptr<Decoder> m_decoder;
    Network &m_network;
    bool m_decodeThreadStartPending = false;
};