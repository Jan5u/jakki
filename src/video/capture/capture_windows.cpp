#include "capture_windows.hpp"

DxgiCapture::DxgiCapture(Network* network) : net(network) {}

DxgiCapture::~DxgiCapture() {
    if (m_capture_thread.joinable()) {
        m_capture_thread.request_stop();
        m_capture_thread.join();
    }
}

void DxgiCapture::selectScreen() {
    m_screenSelected = true;
}

void DxgiCapture::startCapture() {
    m_screenSelected = true;
}

void DxgiCapture::startEncoding(EncoderType encoderType) {
    if (m_capture_thread.joinable()) {
        std::println("DxgiCapture already running");
        return;
    }

    if (!m_screenSelected) {
        m_screenSelected = true;
    }

    encoder = D3D11Encoder::create(encoderType, net);
    if (encoder) {
        encoder->init();
    }

    m_capture_thread = std::jthread([this](std::stop_token stop_token) {
        (void)stop_token;
        captureDDA();
    });
}

void DxgiCapture::stopCapture() {
    if (m_capture_thread.joinable()) {
        m_capture_thread.request_stop();
        m_capture_thread.join();
    }
    m_screenSelected = false;
}

/**
 * Capture via Desktop Duplication API
 *
 * https://ffmpeg.org/ffmpeg-filters.html#ddagrab
 *
 */
void DxgiCapture::captureDDA() {
    AVBufferRef* hw_device_ctx = nullptr;
    av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0);

    AVFilterGraph* filter_graph = avfilter_graph_alloc();
    const AVFilter* src = avfilter_get_by_name("ddagrab");
    const AVFilter* sink = avfilter_get_by_name("buffersink");
    AVFilterContext* buffersrc_ctx = nullptr;
    AVFilterContext* buffersink_ctx = nullptr;
    AVFilterContext* scale_ctx = nullptr;

    const bool useAmfPath = encoder && encoder->getName().find("_amf") != std::string::npos;

    if (!filter_graph || !src || !sink) {
        std::println(stderr, "Failed to initialize Windows capture filter graph");
        if (filter_graph) {
            avfilter_graph_free(&filter_graph);
        }
        if (hw_device_ctx) {
            av_buffer_unref(&hw_device_ctx);
        }
        return;
    }

    char args[256];
    snprintf(args, sizeof(args), "output_idx=0:framerate=60");
    if (avfilter_graph_create_filter(&buffersrc_ctx, src, "in", args, nullptr, filter_graph) < 0) {
        std::println(stderr, "Failed to create ddagrab filter");
        avfilter_graph_free(&filter_graph);
        if (hw_device_ctx) {
            av_buffer_unref(&hw_device_ctx);
        }
        return;
    }

    if (avfilter_graph_create_filter(&buffersink_ctx, sink, "out", nullptr, nullptr, filter_graph) < 0) {
        std::println(stderr, "Failed to create buffersink filter");
        avfilter_graph_free(&filter_graph);
        if (hw_device_ctx) {
            av_buffer_unref(&hw_device_ctx);
        }
        return;
    }

    int linkRet = 0;
    if (useAmfPath) {
        const AVFilter* scale = avfilter_get_by_name("scale_d3d11");
        if (scale && avfilter_graph_create_filter(&scale_ctx, scale, "scale_amf_nv12", "format=nv12", nullptr, filter_graph) >= 0) {
            const int linkSrcToScale = avfilter_link(buffersrc_ctx, 0, scale_ctx, 0);
            const int linkScaleToSink = (linkSrcToScale >= 0) ? avfilter_link(scale_ctx, 0, buffersink_ctx, 0) : linkSrcToScale;
            if (linkSrcToScale >= 0 && linkScaleToSink >= 0) {
                std::println("Using AMF GPU path: ddagrab -> scale_d3d11(format=nv12) -> buffersink");
            } else {
                std::println(stderr, "Failed to link AMF d3d11 scaling path");
                linkRet = -1;
            }
        } else {
            std::println(stderr, "scale_d3d11 not available, AMF will use direct ddagrab frames");
            linkRet = avfilter_link(buffersrc_ctx, 0, buffersink_ctx, 0);
        }
    } else {
        linkRet = avfilter_link(buffersrc_ctx, 0, buffersink_ctx, 0);
    }

    if (linkRet < 0) {
        std::println(stderr, "Failed to link capture filter graph");
        avfilter_graph_free(&filter_graph);
        if (hw_device_ctx) {
            av_buffer_unref(&hw_device_ctx);
        }
        return;
    }

    if (avfilter_graph_config(filter_graph, nullptr) < 0) {
        std::println(stderr, "Failed to configure capture filter graph");
        avfilter_graph_free(&filter_graph);
        if (hw_device_ctx) {
            av_buffer_unref(&hw_device_ctx);
        }
        return;
    }

    AVFrame *frame = av_frame_alloc();
    while (true) {
        if (av_buffersink_get_frame(buffersink_ctx, frame) < 0) {
            break;
        }
        if (encoder) {
            encoder->encodeD3D11Frame(frame);
        }
        av_frame_unref(frame);
    }

    av_frame_free(&frame);
    if (filter_graph) {
        avfilter_graph_free(&filter_graph);
    }
    if (hw_device_ctx) {
        av_buffer_unref(&hw_device_ctx);
    }
    if (encoder) {
        encoder->flush();
    }
}

/**
 * Capture screen via Windows.Graphics.Capture API
 * 
 * https://ffmpeg.org/ffmpeg-filters.html#gfxcapture
 * 
 */
void DxgiCapture::captureGFX() {
    // TODO: Implement gfxcapture when ffmpeg 8.1 is available on vcpkg
    const AVFilter *gfxcapture = avfilter_get_by_name("gfxcapture");
    if (!gfxcapture) {
        qDebug() << "gfxcapture not found";
    }
}