#pragma once

#include "capture.hpp"
#include "../encode/encoder_vulkan.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>

#include <array>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <thread>

class Network;
class D3D12VAEncoder;

enum class WindowsVersion {
    Unsupported,
    Windows10,
    Windows11,
};

WindowsVersion getWindowsVersion();
bool isWindows11();

class DxgiCapture : public Capture {
  public:
    explicit DxgiCapture(Network *network);
    ~DxgiCapture() override;

    void selectScreen() override;
    void startCapture() override;
    void startEncoding(EncoderType encoderType) override;
    void stopCapture() override;

  private:
    struct WgcContext;

    bool initD3D11();
    bool initDuplication();
    void releaseDuplication();
    void releaseD3D11();
    bool ensureSharedTexture(const D3D11_TEXTURE2D_DESC &desktopDesc);
    bool ensureEncoder(int width, int height);
    void captureLoop(std::stop_token stopToken);
    void dxgiCaptureLoop(std::stop_token stopToken);
    void wgcCaptureLoop(std::stop_token stopToken);

    Network *net = nullptr;
    std::jthread m_capture_thread;
    bool m_screenSelected = false;

    ID3D11Device *m_device = nullptr;
    ID3D11DeviceContext *m_context = nullptr;
    IDXGIOutput1 *m_output = nullptr;
    IDXGIOutputDuplication *m_duplication = nullptr;

    ID3D11Texture2D *m_sharedTexture = nullptr;
    IDXGIKeyedMutex *m_keyedMutex = nullptr;
    HANDLE m_sharedHandle = nullptr;
    int m_sharedWidth = 0;
    int m_sharedHeight = 0;

    std::chrono::steady_clock::time_point m_lastFrameTime{};
    bool m_hasLastFrameTime = false;

    std::array<uint8_t, 8> m_adapterLuid{};
    bool m_hasAdapterLuid = false;

    std::unique_ptr<VulkanEncoder> encoder;
    std::unique_ptr<D3D12VAEncoder> d3d12Encoder;
    std::unique_ptr<WgcContext> wgcContext;
};
