#include "capture_windows.hpp"

#include <cstring>
#include <iterator>
#include <print>

#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <winuser.h>

namespace {

constexpr const char *kEncoderName = "h264_vulkan";

void logHr(const char *message, HRESULT hr) {
    std::println(stderr, "{}: 0x{:x}", message, static_cast<uint32_t>(hr));
}

} // namespace

DxgiCapture::DxgiCapture(Network *network) : net(network) {}

DxgiCapture::~DxgiCapture() {
    stopCapture();
    releaseD3D11();
}

void DxgiCapture::selectScreen() {
    m_screenSelected = true;
}

void DxgiCapture::startCapture() {
    m_screenSelected = true;
    if (m_capture_thread.joinable()) {
        return;
    }
    m_capture_thread = std::jthread([this](std::stop_token stopToken) {
        captureLoop(stopToken);
    });
}

void DxgiCapture::startEncoding(EncoderType encoderType) {
    (void)encoderType;
    startCapture();
}

void DxgiCapture::stopCapture() {
    if (m_capture_thread.joinable()) {
        m_capture_thread.request_stop();
        m_capture_thread.join();
    }
    releaseDuplication();
    m_screenSelected = false;
}

bool DxgiCapture::initD3D11() {
    if (m_device && m_context) {
        return true;
    }

    static const bool dpiAware = [] {
        SetProcessDPIAware();
        return true;
    }();
    (void)dpiAware;

    constexpr D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    D3D_FEATURE_LEVEL selectedLevel{};

    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        featureLevels,
        static_cast<UINT>(std::size(featureLevels)),
        D3D11_SDK_VERSION,
        &m_device,
        &selectedLevel,
        &m_context);
    if (FAILED(hr)) {
        logHr("D3D11CreateDevice failed", hr);
        releaseD3D11();
        return false;
    }

    IDXGIDevice *dxgiDevice = nullptr;
    hr = m_device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (FAILED(hr)) {
        logHr("Failed to get IDXGIDevice", hr);
        releaseD3D11();
        return false;
    }

    IDXGIAdapter *adapter = nullptr;
    hr = dxgiDevice->GetAdapter(&adapter);
    dxgiDevice->Release();
    if (FAILED(hr)) {
        logHr("Failed to get DXGI adapter", hr);
        releaseD3D11();
        return false;
    }

    DXGI_ADAPTER_DESC adapterDesc{};
    hr = adapter->GetDesc(&adapterDesc);
    if (SUCCEEDED(hr)) {
        std::memcpy(m_adapterLuid.data(), &adapterDesc.AdapterLuid, m_adapterLuid.size());
        m_hasAdapterLuid = true;
        std::println("DXGI adapter LUID selected for Vulkan interop");
    }
    adapter->Release();

    return true;
}

bool DxgiCapture::initDuplication() {
    if (m_duplication) {
        return true;
    }

    IDXGIDevice *dxgiDevice = nullptr;
    HRESULT hr = m_device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (FAILED(hr)) {
        logHr("Failed to get IDXGIDevice for duplication", hr);
        return false;
    }

    IDXGIAdapter *adapter = nullptr;
    hr = dxgiDevice->GetAdapter(&adapter);
    dxgiDevice->Release();
    if (FAILED(hr)) {
        logHr("Failed to get DXGI adapter for duplication", hr);
        return false;
    }

    IDXGIOutput *output = nullptr;
    hr = adapter->EnumOutputs(0, &output);
    adapter->Release();
    if (FAILED(hr)) {
        logHr("Failed to enumerate DXGI output 0", hr);
        return false;
    }

    hr = output->QueryInterface(IID_PPV_ARGS(&m_output));
    output->Release();
    if (FAILED(hr)) {
        logHr("Failed to get IDXGIOutput1", hr);
        return false;
    }

    IDXGIOutput5 *output5 = nullptr;
    if (SUCCEEDED(m_output->QueryInterface(IID_PPV_ARGS(&output5)))) {
        constexpr DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM};
        hr = output5->DuplicateOutput1(m_device, 0, static_cast<UINT>(std::size(formats)), formats, &m_duplication);
        output5->Release();
    } else {
        std::println(stderr, "IDXGIOutput5 unavailable; using legacy DuplicateOutput");
        hr = m_output->DuplicateOutput(m_device, &m_duplication);
    }

    if (FAILED(hr)) {
        logHr("Failed to duplicate desktop output", hr);
        releaseDuplication();
        return false;
    }

    std::println("Desktop duplication started");
    return true;
}

void DxgiCapture::releaseDuplication() {
    if (m_duplication) {
        m_duplication->Release();
        m_duplication = nullptr;
    }
    if (m_output) {
        m_output->Release();
        m_output = nullptr;
    }
}

void DxgiCapture::releaseD3D11() {
    releaseDuplication();
    encoder.reset();

    if (m_sharedHandle) {
        CloseHandle(m_sharedHandle);
        m_sharedHandle = nullptr;
    }
    if (m_keyedMutex) {
        m_keyedMutex->Release();
        m_keyedMutex = nullptr;
    }
    if (m_sharedTexture) {
        m_sharedTexture->Release();
        m_sharedTexture = nullptr;
    }
    m_sharedWidth = 0;
    m_sharedHeight = 0;
    if (m_context) {
        m_context->Release();
        m_context = nullptr;
    }
    if (m_device) {
        m_device->Release();
        m_device = nullptr;
    }
}

bool DxgiCapture::ensureSharedTexture(const D3D11_TEXTURE2D_DESC &desktopDesc) {
    D3D11_TEXTURE2D_DESC currentDesc{};
    if (m_sharedTexture) {
        m_sharedTexture->GetDesc(&currentDesc);
        if (currentDesc.Width == desktopDesc.Width && currentDesc.Height == desktopDesc.Height &&
            currentDesc.Format == desktopDesc.Format) {
            m_sharedWidth = static_cast<int>(currentDesc.Width);
            m_sharedHeight = static_cast<int>(currentDesc.Height);
            return true;
        }
    }

    if (m_sharedHandle) {
        CloseHandle(m_sharedHandle);
        m_sharedHandle = nullptr;
    }
    if (m_keyedMutex) {
        m_keyedMutex->Release();
        m_keyedMutex = nullptr;
    }
    if (m_sharedTexture) {
        m_sharedTexture->Release();
        m_sharedTexture = nullptr;
    }
    m_sharedWidth = 0;
    m_sharedHeight = 0;

    encoder.reset();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desktopDesc.Width;
    desc.Height = desktopDesc.Height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = desktopDesc.Format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

    HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &m_sharedTexture);
    if (FAILED(hr)) {
        logHr("Failed to create shared staging texture", hr);
        return false;
    }

    hr = m_sharedTexture->QueryInterface(IID_PPV_ARGS(&m_keyedMutex));
    if (FAILED(hr)) {
        logHr("Failed to get IDXGIKeyedMutex", hr);
        return false;
    }

    IDXGIResource1 *resource = nullptr;
    hr = m_sharedTexture->QueryInterface(IID_PPV_ARGS(&resource));
    if (FAILED(hr)) {
        logHr("Failed to get IDXGIResource1", hr);
        return false;
    }

    hr = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr, &m_sharedHandle);
    resource->Release();
    if (FAILED(hr)) {
        logHr("Failed to create shared texture handle", hr);
        return false;
    }

    m_sharedWidth = static_cast<int>(desc.Width);
    m_sharedHeight = static_cast<int>(desc.Height);
    std::println("Created shared DXGI texture {}x{} for Vulkan interop", desc.Width, desc.Height);
    return true;
}

bool DxgiCapture::ensureEncoder(int width, int height) {
    if (encoder && encoder->isReady()) {
        return true;
    }

    encoder = std::make_unique<VulkanEncoder>(net, m_hasAdapterLuid ? m_adapterLuid.data() : nullptr);
    encoder->init(kEncoderName, width, height);
    if (!encoder->isReady()) {
        std::println(stderr, "Failed to initialize {}", kEncoderName);
        encoder.reset();
        return false;
    }

    std::println("Encoder ready ({} {}x{})", encoder->getName(), width, height);
    return true;
}

void DxgiCapture::captureLoop(std::stop_token stopToken) {
    if (!initD3D11() || !initDuplication()) {
        return;
    }

    while (!stopToken.stop_requested()) {
        DXGI_OUTDUPL_FRAME_INFO frameInfo{};
        IDXGIResource *desktopResource = nullptr;

        HRESULT hr = m_duplication->AcquireNextFrame(100, &frameInfo, &desktopResource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            continue;
        }
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            std::println(stderr, "Desktop duplication access lost; reinitializing");
            releaseDuplication();
            if (!initDuplication()) {
                return;
            }
            continue;
        }
        if (FAILED(hr)) {
            logHr("AcquireNextFrame failed", hr);
            return;
        }

        ID3D11Texture2D *desktopTexture = nullptr;
        const bool hasDesktopUpdate = frameInfo.LastPresentTime.QuadPart != 0 && frameInfo.AccumulatedFrames != 0;
        int width = m_sharedWidth;
        int height = m_sharedHeight;

        if (hasDesktopUpdate && desktopResource) {
            hr = desktopResource->QueryInterface(IID_PPV_ARGS(&desktopTexture));
            desktopResource->Release();
            if (FAILED(hr)) {
                logHr("Failed to get duplicated desktop texture", hr);
                m_duplication->ReleaseFrame();
                continue;
            }

            D3D11_TEXTURE2D_DESC desktopDesc{};
            desktopTexture->GetDesc(&desktopDesc);

            if (desktopDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
                std::println(stderr, "Unsupported desktop format {}; expected B8G8R8A8_UNORM",
                             static_cast<uint32_t>(desktopDesc.Format));
                desktopTexture->Release();
                m_duplication->ReleaseFrame();
                return;
            }

            width = static_cast<int>(desktopDesc.Width);
            height = static_cast<int>(desktopDesc.Height);
            if (!ensureSharedTexture(desktopDesc) || !ensureEncoder(width, height)) {
                desktopTexture->Release();
                m_duplication->ReleaseFrame();
                return;
            }
        } else {
            if (desktopResource) {
                desktopResource->Release();
            }
            if (!m_sharedTexture || !m_keyedMutex || !m_sharedHandle || width <= 0 || height <= 0) {
                m_duplication->ReleaseFrame();
                continue;
            }
            if (!ensureEncoder(width, height)) {
                m_duplication->ReleaseFrame();
                return;
            }
            constexpr auto kPointerOnlyInterval = std::chrono::milliseconds(16667);
            const auto now = std::chrono::steady_clock::now();
            if (m_hasLastFrameTime && now - m_lastFrameTime < kPointerOnlyInterval) {
                m_duplication->ReleaseFrame();
                continue;
            }
        }

        HRESULT mutexHr = m_keyedMutex->AcquireSync(0, 1000);
        if (FAILED(mutexHr)) {
            logHr("Failed to acquire D3D keyed mutex", mutexHr);
            if (desktopTexture) {
                desktopTexture->Release();
            }
            m_duplication->ReleaseFrame();

            if (SUCCEEDED(m_keyedMutex->AcquireSync(1, 1000))) {
                m_keyedMutex->ReleaseSync(0);
            }
            continue;
        }

        if (desktopTexture) {
            m_context->CopySubresourceRegion(m_sharedTexture, 0, 0, 0, 0, desktopTexture, 0, nullptr);
            desktopTexture->Release();
        }
        m_keyedMutex->ReleaseSync(1);

        m_duplication->ReleaseFrame();

        bool submitted = false;
        const bool encoded = encoder->encodeWin32SharedTexture(m_sharedHandle, width, height, &submitted);
        if (!encoded && !submitted) {
            if (SUCCEEDED(m_keyedMutex->AcquireSync(1, 1000))) {
                m_keyedMutex->ReleaseSync(0);
            } else {
                std::println(stderr, "Failed to recover D3D keyed mutex after encode failure");
                return;
            }
        }
        if (!encoded) {
            std::println(stderr, "Vulkan encode failed");
            return;
        }

        m_lastFrameTime = std::chrono::steady_clock::now();
        m_hasLastFrameTime = true;
    }
}
