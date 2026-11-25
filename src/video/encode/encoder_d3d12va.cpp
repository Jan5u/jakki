#include <winsock2.h>

#include "encoder_d3d12va.hpp"

#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include <combaseapi.h>
#include <d3d10.h>
#include <d3d11_3.h>
#include <d3d11on12.h>
#include <d3d12video.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>

#include "../../network.hpp"

extern "C" {
#include <libavutil/hwcontext_d3d12va.h>
#include <libavutil/opt.h>
}

namespace {

constexpr const char *kEncoderName = "h264_d3d12va";
constexpr const char *kBgraToNv12Hlsl = R"(
Texture2D<float4> source : register(t0);
RWTexture2D<float> yPlane : register(u0);
RWTexture2D<float2> uvPlane : register(u1);

float3 rgbToYuv(float3 rgb) {
    float y = 0.299 * rgb.r + 0.587 * rgb.g + 0.114 * rgb.b;
    float u = (rgb.b - y) / 1.772;
    float v = (rgb.r - y) / 1.402;
    return float3(y * 0.859 + 0.0625, u * 0.875 + 0.5, v * 0.875 + 0.5);
}

float3 sampleRgb(int2 pos) {
    // The NV12 surface is padded to a multiple of 16. Return neutral black for
    // padding instead of reading outside the capture texture.
    uint srcWidth, srcHeight;
    source.GetDimensions(srcWidth, srcHeight);
    if (pos.x < 0 || pos.y < 0 || pos.x >= (int)srcWidth || pos.y >= (int)srcHeight)
        return 0;
    return source.Load(int3(pos, 0)).rgb;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    uint width, height;
    yPlane.GetDimensions(width, height);
    if (id.x >= width || id.y >= height)
        return;

    int2 pos = int2(id.xy);
    yPlane[pos] = rgbToYuv(sampleRgb(pos)).r;

    if ((id.x & 1) != 0 || (id.y & 1) != 0)
        return;

    float2 uv = 0;
    uv += rgbToYuv(sampleRgb(pos)).gb;
    uv += rgbToYuv(sampleRgb(pos + int2(1, 0))).gb;
    uv += rgbToYuv(sampleRgb(pos + int2(0, 1))).gb;
    uv += rgbToYuv(sampleRgb(pos + int2(1, 1))).gb;
    uvPlane[pos / 2] = uv * 0.25;
}
)";

void releaseExternalDevice(AVHWDeviceContext *hwDevice) {
    auto *context = reinterpret_cast<AVD3D12VADeviceContext *>(hwDevice->hwctx);
    if (context->device) {
        context->device->Release();
        context->device = nullptr;
    }
}

} // namespace

struct D3D12VAEncoder::Impl {
    explicit Impl(Network *net) : network(net) {}

    ~Impl() {
        cleanup();
    }

    Network *network = nullptr;
    std::string encoderName;
    bool ready = false;
    int width = 0;
    int height = 0;
    int alignedWidth = 0;
    int alignedHeight = 0;
    int64_t frameCount = 0;

    ID3D12Device *device = nullptr;
    ID3D12CommandQueue *commandQueue = nullptr;

    ID3D11Device *d3d11Device = nullptr;
    ID3D11Device3 *d3d11Device3 = nullptr;
    ID3D11DeviceContext *d3d11Context = nullptr;
    ID3D11On12Device *d3d11On12 = nullptr;
    ID3D11ComputeShader *computeShader = nullptr;

    ID3D11Texture2D *captureTexture = nullptr;
    IDXGIKeyedMutex *captureMutex = nullptr;

    AVBufferRef *hwDeviceCtx = nullptr;
    AVBufferRef *hwFramesCtx = nullptr;
    AVCodecContext *codecCtx = nullptr;
    AVPacket *packet = nullptr;

    bool hasVideoEncodeSupport(ID3D12Device *candidate) {
        ID3D12Device3 *device3 = nullptr;
        if (FAILED(candidate->QueryInterface(IID_PPV_ARGS(&device3)))) {
            return false;
        }
        device3->Release();

        ID3D12VideoDevice3 *videoDevice = nullptr;
        if (FAILED(candidate->QueryInterface(IID_PPV_ARGS(&videoDevice)))) {
            return false;
        }

        D3D12_FEATURE_DATA_VIDEO_FEATURE_AREA_SUPPORT area{};
        area.NodeIndex = 0;
        const HRESULT hr = videoDevice->CheckFeatureSupport(
            D3D12_FEATURE_VIDEO_FEATURE_AREA_SUPPORT, &area, sizeof(area));
        videoDevice->Release();
        return SUCCEEDED(hr) && area.VideoEncodeSupport;
    }

    bool createDevice() {
        IDXGIFactory4 *factory = nullptr;
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(hr)) {
            return false;
        }

        for (UINT index = 0;; ++index) {
            IDXGIAdapter1 *adapter = nullptr;
            hr = factory->EnumAdapters1(index, &adapter);
            if (hr == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (FAILED(hr)) {
                break;
            }

            DXGI_ADAPTER_DESC1 description{};
            adapter->GetDesc1(&description);
            if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                ID3D12Device *candidate = nullptr;
                if (SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0,
                                                IID_PPV_ARGS(&candidate)))) {
                    if (hasVideoEncodeSupport(candidate)) {
                        device = candidate;
                    } else {
                        candidate->Release();
                    }
                }
            }
            adapter->Release();
            if (device) {
                break;
            }
        }
        factory->Release();

        if (!device) {
            hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
            if (FAILED(hr)) {
                return false;
            }
            if (!hasVideoEncodeSupport(device)) {
                return false;
            }
        }

        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        hr = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&commandQueue));
        if (FAILED(hr)) {
            return false;
        }
        return true;
    }

    bool createD3D11On12() {
        constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1};
        IUnknown *queues[] = {commandQueue};

        HRESULT hr = D3D11On12CreateDevice(device, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                           levels, static_cast<UINT>(std::size(levels)),
                                           queues, 1, 0, &d3d11Device, &d3d11Context, nullptr);
        if (FAILED(hr)) {
            return false;
        }

        hr = d3d11Device->QueryInterface(IID_PPV_ARGS(&d3d11On12));
        if (FAILED(hr)) {
            return false;
        }

        hr = d3d11Device->QueryInterface(IID_PPV_ARGS(&d3d11Device3));
        if (FAILED(hr)) {
            return false;
        }

        ID3D10Multithread *multithread = nullptr;
        hr = d3d11Device->QueryInterface(IID_PPV_ARGS(&multithread));
        if (FAILED(hr)) {
            return false;
        }
        multithread->SetMultithreadProtected(TRUE);
        multithread->Release();
        return true;
    }

    bool createComputeShader() {
        ID3DBlob *shaderBlob = nullptr;
        ID3DBlob *errors = nullptr;
        HRESULT hr = D3DCompile(kBgraToNv12Hlsl, std::strlen(kBgraToNv12Hlsl), nullptr, nullptr, nullptr,
                                "CSMain", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                &shaderBlob, &errors);
        if (FAILED(hr)) {
            if (errors) {
                errors->Release();
            } else {
            }
            return false;
        }
        if (errors) {
            errors->Release();
        }

        hr = d3d11Device->CreateComputeShader(shaderBlob->GetBufferPointer(),
                                              shaderBlob->GetBufferSize(), nullptr, &computeShader);
        shaderBlob->Release();
        if (FAILED(hr)) {
            return false;
        }
        return true;
    }

    bool initEncoder(int w, int h) {
        alignedWidth = (w + 15) & ~15;
        alignedHeight = (h + 15) & ~15;

        const AVCodec *codec = avcodec_find_encoder_by_name(encoderName.c_str());
        if (!codec) {
            return false;
        }

        codecCtx = avcodec_alloc_context3(codec);
        if (!codecCtx) {
            return false;
        }

        hwDeviceCtx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D12VA);
        if (!hwDeviceCtx) {
            return false;
        }
        auto *hwDevice = reinterpret_cast<AVHWDeviceContext *>(hwDeviceCtx->data);
        auto *d3d12DeviceContext = reinterpret_cast<AVD3D12VADeviceContext *>(hwDevice->hwctx);
        device->AddRef();
        d3d12DeviceContext->device = device;
        hwDevice->free = releaseExternalDevice;
        if (av_hwdevice_ctx_init(hwDeviceCtx) < 0) {
            return false;
        }

        hwFramesCtx = av_hwframe_ctx_alloc(hwDeviceCtx);
        if (!hwFramesCtx) {
            return false;
        }
        auto *frames = reinterpret_cast<AVHWFramesContext *>(hwFramesCtx->data);
        auto *d3d12Frames = reinterpret_cast<AVD3D12VAFramesContext *>(frames->hwctx);
        frames->format = AV_PIX_FMT_D3D12;
        frames->sw_format = AV_PIX_FMT_NV12;
        frames->width = alignedWidth;
        frames->height = alignedHeight;
        frames->initial_pool_size = 4;
        d3d12Frames->format = DXGI_FORMAT_NV12;
        d3d12Frames->resource_flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (av_hwframe_ctx_init(hwFramesCtx) < 0) {
            return false;
        }

        codecCtx->width = w;
        codecCtx->height = h;
        codecCtx->pix_fmt = AV_PIX_FMT_D3D12;
        codecCtx->hw_frames_ctx = av_buffer_ref(hwFramesCtx);
        codecCtx->time_base = {1, 60};
        codecCtx->framerate = {60, 1};
        codecCtx->gop_size = 60;
        codecCtx->max_b_frames = 0;
        codecCtx->bit_rate = 8000000;
        codecCtx->rc_max_rate = 10000000;
        codecCtx->rc_buffer_size = 16000000;

        av_opt_set(codecCtx->priv_data, "rc_mode", "VBR", 0);
        av_opt_set_int(codecCtx->priv_data, "async_depth", 1, 0);
        av_opt_set_int(codecCtx->priv_data, "b_depth", 1, 0);

        av_log_set_level(AV_LOG_WARNING);

        if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
            return false;
        }

        packet = av_packet_alloc();
        if (!packet) {
            return false;
        }

        width = w;
        height = h;
        return true;
    }

    bool init(const char *encoder, int w, int h) {
        cleanup();
        encoderName = encoder ? encoder : kEncoderName;

        if (!createDevice() || !createD3D11On12() || !createComputeShader() || !initEncoder(w, h)) {
            cleanup();
            return false;
        }

        ready = true;
        return true;
    }

    void sendPacket(const AVPacket *p) {
        if (!p || !network || !p->data || p->size <= 0) {
            return;
        }

        std::vector<uint8_t> framedPacket;
        const auto packetSize = static_cast<uint32_t>(p->size);
        framedPacket.reserve(sizeof(packetSize) + p->size);
        for (int shift = 0; shift < 32; shift += 8) {
            framedPacket.push_back(static_cast<uint8_t>(packetSize >> shift));
        }
        framedPacket.insert(framedPacket.end(), p->data, p->data + p->size);
        network->sendScreensharePackets(framedPacket);
    }

    void receivePackets() {
        if (!codecCtx || !packet) {
            return;
        }
        while (true) {
            const int result = avcodec_receive_packet(codecCtx, packet);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                return;
            }
            if (result < 0) {
                return;
            }
            sendPacket(packet);
            av_packet_unref(packet);
        }
    }

    void flush() {
        if (!codecCtx || !packet) {
            return;
        }
        avcodec_send_frame(codecCtx, nullptr);
        receivePackets();
    }

    bool setCaptureTexture(HANDLE sharedHandle) {
        if (!ready || !sharedHandle) {
            return false;
        }

        ID3D11Device1 *device1 = nullptr;
        HRESULT hr = d3d11Device->QueryInterface(IID_PPV_ARGS(&device1));
        if (FAILED(hr)) {
            return false;
        }
        hr = device1->OpenSharedResource1(sharedHandle, IID_PPV_ARGS(&captureTexture));
        device1->Release();
        if (FAILED(hr)) {
            return false;
        }

        hr = captureTexture->QueryInterface(IID_PPV_ARGS(&captureMutex));
        if (FAILED(hr)) {
            captureTexture->Release();
            captureTexture = nullptr;
            return false;
        }
        return true;
    }

    bool encodeSharedFrame() {
        if (!ready || !captureTexture || !captureMutex) {
            return false;
        }

        HRESULT mutexHr = captureMutex->AcquireSync(1, 1000);
        if (FAILED(mutexHr)) {
            return false;
        }
        struct KeyedMutexRelease {
            IDXGIKeyedMutex *mutex;
            ~KeyedMutexRelease() {
                if (mutex) {
                    mutex->ReleaseSync(0);
                }
            }
        } keyedMutexRelease{captureMutex};

        static bool loggedFrameSteps = false;
        const bool logSteps = !loggedFrameSteps;
        if (logSteps) {
            loggedFrameSteps = true;
        }
        auto trace = [logSteps](const char *message) {
            if (logSteps) {
            }
        };

        D3D11_TEXTURE2D_DESC sourceDesc{};
        captureTexture->GetDesc(&sourceDesc);
        if (sourceDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
            sourceDesc.Width != static_cast<UINT>(width) ||
            sourceDesc.Height != static_cast<UINT>(height)) {
            return false;
        }

        AVFrame *frame = av_frame_alloc();
        if (!frame) {
            return false;
        }
        int result = av_hwframe_get_buffer(hwFramesCtx, frame, 0);
        if (result < 0) {
            receivePackets();
            result = av_hwframe_get_buffer(hwFramesCtx, frame, 0);
        }
        if (result < 0) {
            av_frame_free(&frame);
            return false;
        }

        auto *d3d12Frame = reinterpret_cast<AVD3D12VAFrame *>(frame->data[0]);
        if (!d3d12Frame || !d3d12Frame->texture || !d3d12Frame->sync_ctx.fence) {
            av_frame_free(&frame);
            return false;
        }
        trace("first frame: acquired FFmpeg D3D12 texture");

        D3D11_RESOURCE_FLAGS wrappedFlags{};
        wrappedFlags.BindFlags = D3D11_BIND_UNORDERED_ACCESS;

        ID3D11Resource *wrappedResource = nullptr;
        HRESULT hr = d3d11On12->CreateWrappedResource(d3d12Frame->texture, &wrappedFlags,
                                                      D3D12_RESOURCE_STATE_COMMON,
                                                      D3D12_RESOURCE_STATE_COMMON,
                                                      IID_PPV_ARGS(&wrappedResource));
        if (FAILED(hr)) {
            av_frame_free(&frame);
            return false;
        }

        ID3D11Texture2D *target = nullptr;
        hr = wrappedResource->QueryInterface(IID_PPV_ARGS(&target));
        if (FAILED(hr)) {
            wrappedResource->Release();
            av_frame_free(&frame);
            return false;
        }
        trace("first frame: wrapped D3D12 texture for D3D11");

        ID3D11ShaderResourceView *sourceView = nullptr;
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        hr = d3d11Device->CreateShaderResourceView(captureTexture, &srvDesc, &sourceView);
        if (FAILED(hr)) {
            target->Release();
            wrappedResource->Release();
            av_frame_free(&frame);
            return false;
        }

        ID3D11UnorderedAccessView *targetViews[2]{};
        ID3D11UnorderedAccessView1 *yView = nullptr;
        D3D11_UNORDERED_ACCESS_VIEW_DESC1 yDesc{};
        yDesc.Format = DXGI_FORMAT_R8_UNORM;
        yDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        yDesc.Texture2D.PlaneSlice = 0;
        hr = d3d11Device3->CreateUnorderedAccessView1(target, &yDesc, &yView);
        targetViews[0] = yView;
        if (SUCCEEDED(hr)) {
            ID3D11UnorderedAccessView1 *uvView = nullptr;
            D3D11_UNORDERED_ACCESS_VIEW_DESC1 uvDesc{};
            uvDesc.Format = DXGI_FORMAT_R8G8_UNORM;
            uvDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            uvDesc.Texture2D.PlaneSlice = 1;
            hr = d3d11Device3->CreateUnorderedAccessView1(target, &uvDesc, &uvView);
            targetViews[1] = uvView;
        }
        if (FAILED(hr)) {
            if (targetViews[0]) targetViews[0]->Release();
            if (targetViews[1]) targetViews[1]->Release();
            sourceView->Release();
            target->Release();
            wrappedResource->Release();
            av_frame_free(&frame);
            return false;
        }

        d3d11On12->AcquireWrappedResources(&wrappedResource, 1);

        ID3D11ShaderResourceView *nullView = nullptr;
        ID3D11UnorderedAccessView *nullViews[2]{};
        d3d11Context->CSSetShader(computeShader, nullptr, 0);
        d3d11Context->CSSetShaderResources(0, 1, &sourceView);
        d3d11Context->CSSetUnorderedAccessViews(0, 2, targetViews, nullptr);
        d3d11Context->Dispatch((alignedWidth + 7) / 8, (alignedHeight + 7) / 8, 1);
        d3d11Context->CSSetShaderResources(0, 1, &nullView);
        d3d11Context->CSSetUnorderedAccessViews(0, 2, nullViews, nullptr);
        d3d11Context->CSSetShader(nullptr, nullptr, 0);

        d3d11On12->ReleaseWrappedResources(&wrappedResource, 1);
        d3d11Context->Flush();
        trace("first frame: BGRA to NV12 compute submitted");

        ++d3d12Frame->sync_ctx.fence_value;
        hr = commandQueue->Signal(d3d12Frame->sync_ctx.fence, d3d12Frame->sync_ctx.fence_value);

        targetViews[0]->Release();
        targetViews[1]->Release();
        sourceView->Release();
        target->Release();
        wrappedResource->Release();

        if (FAILED(hr)) {
            av_frame_free(&frame);
            return false;
        }
        trace("first frame: D3D12 fence signaled");

        frame->pts = frameCount++;
        frame->color_range = AVCOL_RANGE_MPEG;
        result = avcodec_send_frame(codecCtx, frame);
        av_frame_free(&frame);
        if (result < 0) {
            return false;
        }
        trace("first frame: sent to FFmpeg encoder");

        receivePackets();
        trace("first frame: packet receive completed");
        return true;
    }

    void cleanup() {
        flush();

        if (packet) {
            av_packet_free(&packet);
        }
        if (codecCtx) {
            avcodec_free_context(&codecCtx);
        }
        if (hwFramesCtx) {
            av_buffer_unref(&hwFramesCtx);
        }
        if (hwDeviceCtx) {
            av_buffer_unref(&hwDeviceCtx);
        }

        if (captureMutex) {
            captureMutex->Release();
            captureMutex = nullptr;
        }
        if (captureTexture) {
            captureTexture->Release();
            captureTexture = nullptr;
        }
        if (computeShader) {
            computeShader->Release();
            computeShader = nullptr;
        }
        if (d3d11On12) {
            d3d11On12->Release();
            d3d11On12 = nullptr;
        }
        if (d3d11Device3) {
            d3d11Device3->Release();
            d3d11Device3 = nullptr;
        }
        if (d3d11Context) {
            d3d11Context->Release();
            d3d11Context = nullptr;
        }
        if (d3d11Device) {
            d3d11Device->Release();
            d3d11Device = nullptr;
        }
        if (commandQueue) {
            commandQueue->Release();
            commandQueue = nullptr;
        }
        if (device) {
            device->Release();
            device = nullptr;
        }

        ready = false;
        width = 0;
        height = 0;
        alignedWidth = 0;
        alignedHeight = 0;
        frameCount = 0;
    }
};

D3D12VAEncoder::D3D12VAEncoder(Network *network) : impl(std::make_unique<Impl>(network)) {}

D3D12VAEncoder::~D3D12VAEncoder() = default;

void D3D12VAEncoder::init(const char *encoder, int width, int height) {
    impl->init(encoder, width, height);
}

void D3D12VAEncoder::flush() {
    impl->flush();
}

bool D3D12VAEncoder::isReady() const {
    return impl->ready;
}

std::string D3D12VAEncoder::getName() const {
    return impl->encoderName;
}

ID3D12Device *D3D12VAEncoder::getD3D12Device() const {
    return impl->device;
}

ID3D11Device *D3D12VAEncoder::getD3D11Device() const {
    return impl->d3d11Device;
}

ID3D11DeviceContext *D3D12VAEncoder::getD3D11Context() const {
    return impl->d3d11Context;
}

bool D3D12VAEncoder::setCaptureTexture(HANDLE sharedHandle) {
    return impl->setCaptureTexture(sharedHandle);
}

bool D3D12VAEncoder::encodeSharedFrame() {
    return impl->encodeSharedFrame();
}
