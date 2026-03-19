#pragma once

#include "capture.hpp"
#include "../encode/encoder.hpp"

#include <memory>
#include <thread>

#include <QDebug>

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavutil/opt.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
}

class Network;

class DxgiCapture : public Capture {
  public:
    DxgiCapture(Network* network);
    ~DxgiCapture();
    void selectScreen() override;
    std::unique_ptr<D3D11Encoder> encoder;

  private:
    void captureDDA();
    void captureGFX();
    Network* net = nullptr;
    std::jthread m_capture_thread;
};