#include "capture_windows.hpp"

DxgiCapture::DxgiCapture(Network* network) : net(network) {}

DxgiCapture::~DxgiCapture() {
    if (m_capture_thread.joinable()) {
        m_capture_thread.request_stop();
        m_capture_thread.join();
    }
}

void DxgiCapture::selectScreen() {
    if (m_capture_thread.joinable()) {
        std::println("DxgiCapture already running");
        return;
    }

    encoder = D3D11Encoder::create(EncoderType::NVENC_HEVC, net);
    if (encoder) {
        encoder->init();
    }

    m_capture_thread = std::jthread([this](std::stop_token stop_token) {
        (void)stop_token;
        captureDDA();
    });
}

/**
 * Capture via Desktop Duplication API
 *
 * https://ffmpeg.org/ffmpeg-filters.html#ddagrab
 *
 */
void DxgiCapture::captureDDA() {
    AVBufferRef *hw_device_ctx = NULL;
    av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_D3D11VA, NULL, NULL, 0);
    AVFilterGraph *filter_graph = avfilter_graph_alloc();
    const AVFilter *src = avfilter_get_by_name("ddagrab");
    const AVFilter *sink = avfilter_get_by_name("buffersink");
    AVFilterContext *buffersrc_ctx = NULL;
    AVFilterContext *buffersink_ctx = NULL;

    filter_graph = avfilter_graph_alloc();
    char args[256];
    snprintf(args, sizeof(args), "output_idx=0:framerate=60");
    avfilter_graph_create_filter(&buffersrc_ctx, src, "in", args, NULL, filter_graph);
    avfilter_graph_create_filter(&buffersink_ctx, sink, "out", NULL, NULL, filter_graph);
    avfilter_link(buffersrc_ctx, 0, buffersink_ctx, 0);
    avfilter_graph_config(filter_graph, NULL);

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