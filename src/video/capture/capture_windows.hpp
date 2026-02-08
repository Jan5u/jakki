#pragma once

#include "capture.hpp"
#include "../encode/encoder.hpp"

#include <memory>

class Network;

class DxgiCapture : public Capture {
  public:
    DxgiCapture(Network* network);
    ~DxgiCapture();
    void selectScreen() override;
    std::unique_ptr<D3D11Encoder> encoder;

  private:
    Network* net = nullptr;
};