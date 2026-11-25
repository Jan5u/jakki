#pragma once

#ifdef _WIN32

#include "encoder.hpp"

#include <d3d11.h>
#include <d3d12.h>
#include <memory>
#include <string>

class Network;

class D3D12VAEncoder : public Encoder {
  public:
    explicit D3D12VAEncoder(Network *network = nullptr);
    ~D3D12VAEncoder() override;
    void init(const char *encoder, int width, int height) override;
    void flush();
    bool isReady() const;
    std::string getName() const;
    ID3D12Device *getD3D12Device() const;
    ID3D11Device *getD3D11Device() const;
    ID3D11DeviceContext *getD3D11Context() const;
    bool setCaptureTexture(HANDLE sharedHandle);
    bool encodeSharedFrame();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

#endif // _WIN32
