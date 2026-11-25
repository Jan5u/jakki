#define WINDOWS_FOUNDATION_UNIVERSALAPICONTRACT_VERSION 0x130000
#define ____FIReference_1_boolean_INTERFACE_DEFINED__

#include "capture_windows.hpp"

#include <chrono>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <thread>

#include <dispatcherqueue.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <roapi.h>
#include <winstring.h>
#include <winuser.h>

#include <windows.foundation.h>
#include <windows.graphics.h>
#include <windows.graphics.capture.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.h>

#include "../encode/encoder_d3d12va.hpp"

extern "C" HRESULT WINAPI CreateDirect3D11DeviceFromDXGIDevice(IDXGIDevice *dxgiDevice,
                                                               IInspectable **graphicsDevice);

namespace {

constexpr const char *kEncoderName = "h264_vulkan";
constexpr const char *kD3D12EncoderName = "h264_d3d12va";

namespace Wgc = ABI::Windows::Graphics::Capture;
namespace Wgi = ABI::Windows::Graphics;
namespace Wgd = ABI::Windows::Graphics::DirectX;
namespace Wgd3d11 = ABI::Windows::Graphics::DirectX::Direct3D11;

struct IDirect3DDxgiInterfaceAccess : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID iid, void **object) = 0;
};

constexpr GUID kIIDDirect3DDxgiInterfaceAccess = {
    0xa9b3d012, 0x3df2, 0x4ee3, {0xb8, 0xd1, 0x86, 0x95, 0xf4, 0x57, 0xd3, 0xc1}};

template <typename T>
HRESULT activateFactory(const wchar_t *className, T **factory) {
    HSTRING name{};
    HRESULT hr = WindowsCreateString(className, static_cast<UINT32>(std::wcslen(className)), &name);
    if (FAILED(hr)) {
        return hr;
    }
    hr = RoGetActivationFactory(name, IID_PPV_ARGS(factory));
    WindowsDeleteString(name);
    return hr;
}

void closeInspectable(IUnknown *object) {
    if (!object) {
        return;
    }
    ABI::Windows::Foundation::IClosable *closable = nullptr;
    if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&closable)))) {
        closable->Close();
        closable->Release();
    }
}

} // namespace

WindowsVersion getWindowsVersion() {
    using RtlGetVersionFn = LONG(NTAPI *)(PRTL_OSVERSIONINFOW);

    static const WindowsVersion version = [] {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) {
            return WindowsVersion::Unsupported;
        }
        FARPROC address = GetProcAddress(ntdll, "RtlGetVersion");
        if (!address) {
            return WindowsVersion::Unsupported;
        }
        RtlGetVersionFn rtlGetVersion = nullptr;
        std::memcpy(&rtlGetVersion, &address, sizeof(rtlGetVersion));

        RTL_OSVERSIONINFOW info{};
        info.dwOSVersionInfoSize = sizeof(info);
        if (rtlGetVersion(&info) < 0) {
            return WindowsVersion::Unsupported;
        }
        if (info.dwMajorVersion > 10 || (info.dwMajorVersion == 10 && info.dwBuildNumber >= 22000)) {
            return WindowsVersion::Windows11;
        }
        if (info.dwMajorVersion == 10) {
            return WindowsVersion::Windows10;
        }
        return WindowsVersion::Unsupported;
    }();

    return version;
}

bool isWindows11() {
    return getWindowsVersion() == WindowsVersion::Windows11;
}

struct DxgiCapture::WgcContext {
    ~WgcContext() {
        release();
    }

    bool createCaptureItem() {
        HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
        if (FAILED(hr)) {
            return false;
        }
        m_winrtInitialized = true;

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
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, featureLevels,
                               static_cast<UINT>(std::size(featureLevels)), D3D11_SDK_VERSION,
                               &captureDevice, &selectedLevel, &captureContext);
        if (FAILED(hr)) {
            return false;
        }

        MSG message{};
        PeekMessage(&message, nullptr, 0, 0, PM_NOREMOVE);
        DispatcherQueueOptions options{};
        options.dwSize = sizeof(options);
        options.threadType = DQTYPE_THREAD_CURRENT;
        options.apartmentType = DQTAT_COM_NONE;
        hr = CreateDispatcherQueueController(options, &dispatcherQueueController);
        if (FAILED(hr)) {
            return false;
        }

        IGraphicsCaptureItemInterop *interop = nullptr;
        hr = activateFactory(RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureItem, &interop);
        if (FAILED(hr)) {
            return false;
        }

        HMONITOR monitor = MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY);
        hr = interop->CreateForMonitor(monitor, IID_PPV_ARGS(&captureItem));
        interop->Release();
        if (FAILED(hr)) {
            return false;
        }

        Wgi::SizeInt32 size{};
        hr = captureItem->get_Size(&size);
        if (FAILED(hr) || size.Width <= 0 || size.Height <= 0) {
            return false;
        }
        width = size.Width;
        height = size.Height;
        return true;
    }

    bool createSharedTexture(int textureWidth, int textureHeight) {
        if (sharedTexture && sharedWidth == textureWidth && sharedHeight == textureHeight) {
            return true;
        }

        releaseSharedTexture();

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(textureWidth);
        desc.Height = static_cast<UINT>(textureHeight);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

        HRESULT hr = captureDevice->CreateTexture2D(&desc, nullptr, &sharedTexture);
        if (FAILED(hr)) {
            return false;
        }

        hr = sharedTexture->QueryInterface(IID_PPV_ARGS(&sharedMutex));
        if (FAILED(hr)) {
            return false;
        }

        IDXGIResource1 *resource = nullptr;
        hr = sharedTexture->QueryInterface(IID_PPV_ARGS(&resource));
        if (FAILED(hr)) {
            return false;
        }
        hr = resource->CreateSharedHandle(nullptr,
                                          DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                          nullptr, &sharedHandle);
        resource->Release();
        if (FAILED(hr)) {
            return false;
        }

        sharedWidth = textureWidth;
        sharedHeight = textureHeight;
        return true;
    }

    HANDLE getSharedHandle() const {
        return sharedHandle;
    }

    bool createFramePoolSession() {
        IDXGIDevice *dxgiDevice = nullptr;
        HRESULT hr = captureDevice->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
        if (FAILED(hr)) {
            return false;
        }

        IInspectable *graphicsDevice = nullptr;
        hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice, &graphicsDevice);
        dxgiDevice->Release();
        if (FAILED(hr)) {
            return false;
        }

        hr = graphicsDevice->QueryInterface(IID_PPV_ARGS(&direct3dDevice));
        graphicsDevice->Release();
        if (FAILED(hr)) {
            return false;
        }

        Wgc::IDirect3D11CaptureFramePoolStatics2 *statics = nullptr;
        hr = activateFactory(RuntimeClass_Windows_Graphics_Capture_Direct3D11CaptureFramePool, &statics);
        if (FAILED(hr)) {
            return false;
        }

        Wgi::SizeInt32 size{width, height};
        hr = statics->CreateFreeThreaded(direct3dDevice,
                                         Wgd::DirectXPixelFormat_B8G8R8A8UIntNormalized,
                                         2, size, &framePool);
        statics->Release();
        if (FAILED(hr)) {
            return false;
        }

        hr = framePool->CreateCaptureSession(captureItem, &session);
        if (FAILED(hr)) {
            return false;
        }

        Wgc::IGraphicsCaptureSession2 *session2 = nullptr;
        if (SUCCEEDED(session->QueryInterface(IID_PPV_ARGS(&session2)))) {
            session2->put_IsCursorCaptureEnabled(true);
            session2->Release();
        }

        Wgc::IGraphicsCaptureSession3 *session3 = nullptr;
        if (SUCCEEDED(session->QueryInterface(IID_PPV_ARGS(&session3)))) {
            hr = session3->put_IsBorderRequired(false);
            if (FAILED(hr)) {
            }
            session3->Release();
        }

        hr = session->StartCapture();
        if (FAILED(hr)) {
            return false;
        }

        return true;
    }

    bool captureFrame(int *frameWidth, int *frameHeight) {
        if (!frameWidth || !frameHeight || !framePool || !sharedTexture || !captureContext) {
            return false;
        }

        Wgc::IDirect3D11CaptureFrame *frame = nullptr;
        HRESULT hr = framePool->TryGetNextFrame(&frame);
        if (FAILED(hr) || !frame) {
            return false;
        }

        ID3D11Texture2D *source = nullptr;
        Wgd3d11::IDirect3DSurface *surface = nullptr;
        hr = frame->get_Surface(&surface);
        if (SUCCEEDED(hr)) {
            IDirect3DDxgiInterfaceAccess *access = nullptr;
            hr = surface->QueryInterface(kIIDDirect3DDxgiInterfaceAccess, reinterpret_cast<void **>(&access));
            if (SUCCEEDED(hr)) {
                using GetInterfaceFn = HRESULT(STDMETHODCALLTYPE *)(void *, REFIID, void **);
                auto **vtable = *reinterpret_cast<void ***>(access);
                auto getInterface = reinterpret_cast<GetInterfaceFn>(vtable[3]);
                hr = getInterface(access, IID_ID3D11Texture2D, reinterpret_cast<void **>(&source));
                access->Release();
            } else {
            }
            surface->Release();
        } else {
        }

        Wgi::SizeInt32 contentSize{};
        if (SUCCEEDED(frame->get_ContentSize(&contentSize)) && contentSize.Width > 0 &&
            contentSize.Height > 0) {
            *frameWidth = contentSize.Width;
            *frameHeight = contentSize.Height;
        } else {
            *frameWidth = width;
            *frameHeight = height;
        }
        frame->Release();

        if (FAILED(hr) || !source) {
            return false;
        }

        hr = sharedMutex->AcquireSync(0, 1000);
        if (FAILED(hr)) {
            source->Release();
            return false;
        }
        captureContext->CopyResource(sharedTexture, source);
        sharedMutex->ReleaseSync(1);
        source->Release();
        return true;
    }

    void releaseSharedTexture() {
        if (sharedHandle) {
            CloseHandle(sharedHandle);
            sharedHandle = nullptr;
        }
        if (sharedMutex) {
            sharedMutex->Release();
            sharedMutex = nullptr;
        }
        if (sharedTexture) {
            sharedTexture->Release();
            sharedTexture = nullptr;
        }
        sharedWidth = 0;
        sharedHeight = 0;
    }

    void release() {
        releaseSharedTexture();
        if (session) {
            closeInspectable(session);
            session->Release();
            session = nullptr;
        }
        if (framePool) {
            closeInspectable(framePool);
            framePool->Release();
            framePool = nullptr;
        }
        if (captureItem) {
            captureItem->Release();
            captureItem = nullptr;
        }
        if (direct3dDevice) {
            direct3dDevice->Release();
            direct3dDevice = nullptr;
        }
        if (dispatcherQueueController) {
            dispatcherQueueController->Release();
            dispatcherQueueController = nullptr;
        }
        if (captureContext) {
            captureContext->Release();
            captureContext = nullptr;
        }
        if (captureDevice) {
            captureDevice->Release();
            captureDevice = nullptr;
        }
        if (m_winrtInitialized) {
            RoUninitialize();
            m_winrtInitialized = false;
        }
    }

    bool m_winrtInitialized = false;
    ABI::Windows::System::IDispatcherQueueController *dispatcherQueueController = nullptr;
    ID3D11Device *captureDevice = nullptr;
    ID3D11DeviceContext *captureContext = nullptr;
    ID3D11Texture2D *sharedTexture = nullptr;
    IDXGIKeyedMutex *sharedMutex = nullptr;
    HANDLE sharedHandle = nullptr;
    int sharedWidth = 0;
    int sharedHeight = 0;
    Wgd3d11::IDirect3DDevice *direct3dDevice = nullptr;
    Wgc::IGraphicsCaptureItem *captureItem = nullptr;
    Wgc::IDirect3D11CaptureFramePool *framePool = nullptr;
    Wgc::IGraphicsCaptureSession *session = nullptr;
    int width = 0;
    int height = 0;
};

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
    wgcContext.reset();
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
        releaseD3D11();
        return false;
    }

    IDXGIDevice *dxgiDevice = nullptr;
    hr = m_device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (FAILED(hr)) {
        releaseD3D11();
        return false;
    }

    IDXGIAdapter *adapter = nullptr;
    hr = dxgiDevice->GetAdapter(&adapter);
    dxgiDevice->Release();
    if (FAILED(hr)) {
        releaseD3D11();
        return false;
    }

    DXGI_ADAPTER_DESC adapterDesc{};
    hr = adapter->GetDesc(&adapterDesc);
    if (SUCCEEDED(hr)) {
        std::memcpy(m_adapterLuid.data(), &adapterDesc.AdapterLuid, m_adapterLuid.size());
        m_hasAdapterLuid = true;
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
        return false;
    }

    IDXGIAdapter *adapter = nullptr;
    hr = dxgiDevice->GetAdapter(&adapter);
    dxgiDevice->Release();
    if (FAILED(hr)) {
        return false;
    }

    IDXGIOutput *output = nullptr;
    hr = adapter->EnumOutputs(0, &output);
    adapter->Release();
    if (FAILED(hr)) {
        return false;
    }

    hr = output->QueryInterface(IID_PPV_ARGS(&m_output));
    output->Release();
    if (FAILED(hr)) {
        return false;
    }

    IDXGIOutput5 *output5 = nullptr;
    if (SUCCEEDED(m_output->QueryInterface(IID_PPV_ARGS(&output5)))) {
        constexpr DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM};
        hr = output5->DuplicateOutput1(m_device, 0, static_cast<UINT>(std::size(formats)), formats, &m_duplication);
        output5->Release();
    } else {
        hr = m_output->DuplicateOutput(m_device, &m_duplication);
    }

    if (FAILED(hr)) {
        releaseDuplication();
        return false;
    }

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
    wgcContext.reset();
    d3d12Encoder.reset();
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
        return false;
    }

    hr = m_sharedTexture->QueryInterface(IID_PPV_ARGS(&m_keyedMutex));
    if (FAILED(hr)) {
        return false;
    }

    IDXGIResource1 *resource = nullptr;
    hr = m_sharedTexture->QueryInterface(IID_PPV_ARGS(&resource));
    if (FAILED(hr)) {
        return false;
    }

    hr = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr, &m_sharedHandle);
    resource->Release();
    if (FAILED(hr)) {
        return false;
    }

    m_sharedWidth = static_cast<int>(desc.Width);
    m_sharedHeight = static_cast<int>(desc.Height);
    return true;
}

bool DxgiCapture::ensureEncoder(int width, int height) {
    if (encoder && encoder->isReady()) {
        return true;
    }

    encoder = std::make_unique<VulkanEncoder>(net, m_hasAdapterLuid ? m_adapterLuid.data() : nullptr);
    encoder->init(kEncoderName, width, height);
    if (!encoder->isReady()) {
        encoder.reset();
        return false;
    }

    return true;
}

void DxgiCapture::captureLoop(std::stop_token stopToken) {
    if (getWindowsVersion() == WindowsVersion::Windows11) {
        wgcCaptureLoop(stopToken);
    } else {
        dxgiCaptureLoop(stopToken);
    }
}

void DxgiCapture::wgcCaptureLoop(std::stop_token stopToken) {

    wgcContext = std::make_unique<WgcContext>();
    if (!wgcContext->createCaptureItem()) {
        wgcContext.reset();
        return;
    }

    if (!wgcContext->createSharedTexture(wgcContext->width, wgcContext->height)) {
        wgcContext.reset();
        return;
    }

    d3d12Encoder = std::make_unique<D3D12VAEncoder>(net);
    d3d12Encoder->init(kD3D12EncoderName, wgcContext->width, wgcContext->height);
    if (!d3d12Encoder->isReady() ||
        !d3d12Encoder->setCaptureTexture(wgcContext->getSharedHandle())) {
        d3d12Encoder.reset();
        wgcContext.reset();
        return;
    }

    if (!wgcContext->createFramePoolSession()) {
        d3d12Encoder.reset();
        wgcContext.reset();
        return;
    }

    while (!stopToken.stop_requested()) {
        MSG message{};
        while (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessage(&message);
        }

        int frameWidth = 0;
        int frameHeight = 0;
        if (wgcContext->captureFrame(&frameWidth, &frameHeight)) {
            static bool loggedFrame = false;
            if (!loggedFrame) {
                loggedFrame = true;
            }
            const bool encoded = d3d12Encoder->encodeSharedFrame();
            if (!encoded) {
                break;
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
    }

    d3d12Encoder.reset();
    wgcContext.reset();
}

void DxgiCapture::dxgiCaptureLoop(std::stop_token stopToken) {
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
            releaseDuplication();
            if (!initDuplication()) {
                return;
            }
            continue;
        }
        if (FAILED(hr)) {
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
                m_duplication->ReleaseFrame();
                continue;
            }

            D3D11_TEXTURE2D_DESC desktopDesc{};
            desktopTexture->GetDesc(&desktopDesc);

            if (desktopDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
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
                return;
            }
        }
        if (!encoded) {
            return;
        }

        m_lastFrameTime = std::chrono::steady_clock::now();
        m_hasLastFrameTime = true;
    }
}
