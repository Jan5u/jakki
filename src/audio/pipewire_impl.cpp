#include "pipewire_impl.hpp"
#include "../network.hpp"
#include <iostream>
#include <cstring>

PipewireImpl::PipewireImpl(Network& network) 
    : AudioImpl(network) {
    memset(&pwdata, 0, sizeof(pwdata));
}

PipewireImpl::~PipewireImpl() {
    cleanup();
}

void PipewireImpl::initAudio() {
    // Initialize Opus codec
    initOpus();
}

void PipewireImpl::startCapture() {
    audioLoopThread = std::jthread([this] { initPipewire(); });
}

void PipewireImpl::stopCapture() {
    if (pwdata.loop) {
        pw_main_loop_quit(pwdata.loop);
    }
}

void PipewireImpl::cleanup() {
    // Wait for audio thread to finish
    if (audioLoopThread.joinable()) {
        stopCapture();
        audioLoopThread.join();
    }

    // Cleanup Opus
    opusCleanup();
}

void PipewireImpl::initPipewire() {
    pw_init(nullptr, nullptr);

    pwdata.loop = pw_main_loop_new(nullptr);
    pwdata.context = pw_context_new(pw_main_loop_get_loop(pwdata.loop), nullptr, 0);
    pwdata.core = pw_context_connect(pwdata.context, nullptr, 0);
    pwdata.registry = pw_core_get_registry(pwdata.core, PW_VERSION_REGISTRY, 0);

    pw_stream_events stream_events = {
        .version = PW_VERSION_STREAM_EVENTS,
        .param_changed = on_stream_param_changed,
        .process = on_process_record,
    };

    pw_stream_events stream_events_playback = {
        .version = PW_VERSION_STREAM_EVENTS,
        .process = on_process_playback,
    };

    pwdata.capture_stream = pw_stream_new_simple(
        pw_main_loop_get_loop(pwdata.loop), "jakki-capture",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
                          PW_KEY_MEDIA_ROLE, "Music", PW_KEY_NODE_LATENCY, "20/1000",
                          PW_KEY_NODE_RATE, "1/20",   PW_KEY_NODE_FORCE_QUANTUM, "960", nullptr),
        &stream_events, this);

    pwdata.playback_stream = pw_stream_new_simple(
        pw_main_loop_get_loop(pwdata.loop), "jakki-playback",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
                          PW_KEY_MEDIA_ROLE, "Music", PW_KEY_NODE_LATENCY, "20/1000",
                          PW_KEY_NODE_RATE, "1/20",   PW_KEY_NODE_FORCE_QUANTUM, "960", nullptr),
        &stream_events_playback, this);

    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *params[1];

    spa_audio_info_raw raw_info;
    raw_info.format = SPA_AUDIO_FORMAT_F32;
    raw_info.channels = DEFAULT_CHANNELS;
    raw_info.rate = DEFAULT_RATE;

    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &raw_info);

    pw_stream_connect(pwdata.capture_stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                      static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                                  PW_STREAM_FLAG_MAP_BUFFERS |
                                                  PW_STREAM_FLAG_RT_PROCESS),
                      params, 1);

    pw_stream_connect(pwdata.playback_stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                      static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                                  PW_STREAM_FLAG_MAP_BUFFERS |
                                                  PW_STREAM_FLAG_RT_PROCESS),
                      params, 1);

    // Start running loop
    pw_main_loop_run(pwdata.loop);

    // Cleanup when loop exits
    pw_stream_disconnect(pwdata.playback_stream);
    pw_stream_disconnect(pwdata.capture_stream);
    pw_stream_destroy(pwdata.playback_stream);
    pw_stream_destroy(pwdata.capture_stream);
    pw_proxy_destroy((struct pw_proxy *)pwdata.registry);
    pw_core_disconnect(pwdata.core);
    pw_context_destroy(pwdata.context);
    pw_main_loop_destroy(pwdata.loop);
    pw_deinit();
    
    // Reset pointers
    pwdata = {};
}

void PipewireImpl::on_process_record(void *userdata) {
    PipewireImpl *audio = static_cast<PipewireImpl *>(userdata);
    PipewireData *data = &audio->pwdata;

    struct pw_buffer *b;
    struct spa_buffer *buf;
    float *samples;
    uint32_t n_channels, n_samples;

    if ((b = pw_stream_dequeue_buffer(data->capture_stream)) == NULL) {
        pw_log_warn("out of buffers: %m");
        return;
    }

    buf = b->buffer;
    if ((samples = static_cast<float *>(buf->datas[0].data)) == NULL) {
        pw_stream_queue_buffer(data->capture_stream, b);
        return;
    }

    n_channels = data->format.info.raw.channels;
    n_samples = buf->datas[0].chunk->size / sizeof(float);

    // Copy audio data
    std::vector<float> audioData(samples, samples + n_samples);
    
    // Encode and send packet
    audio->encodePacketWithOpusFloat(audioData.data(), audioData.size());

    pw_stream_queue_buffer(data->capture_stream, b);
}

void PipewireImpl::on_process_playback(void *userdata) {
    PipewireImpl *audio = static_cast<PipewireImpl *>(userdata);
    PipewireData *data = &audio->pwdata;

    struct pw_buffer *b;
    struct spa_buffer *buf;
    float *dst;

    if ((b = pw_stream_dequeue_buffer(data->playback_stream)) == NULL) {
        pw_log_warn("out of buffers: %m");
        return;
    }

    buf = b->buffer;
    if ((dst = static_cast<float *>(buf->datas[0].data)) == NULL) {
        pw_stream_queue_buffer(data->playback_stream, b);
        return;
    }

    int n_frames = buf->datas[0].maxsize / (sizeof(float) * DEFAULT_CHANNELS);
    if (b->requested) {
        n_frames = SPA_MIN(b->requested, n_frames);
    }

    std::vector<float> mixed = audio->mixUserAudioBuffersFloat(n_frames);
    if (static_cast<int>(mixed.size()) < n_frames * DEFAULT_CHANNELS) {
        mixed.resize(n_frames * DEFAULT_CHANNELS, 0.0f);
    }
    memcpy(dst, mixed.data(), n_frames * sizeof(float) * DEFAULT_CHANNELS);

    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = sizeof(float) * DEFAULT_CHANNELS;
    buf->datas[0].chunk->size = n_frames * sizeof(float) * DEFAULT_CHANNELS;

    pw_stream_queue_buffer(data->playback_stream, b);
}

void PipewireImpl::on_stream_param_changed(void *_data, uint32_t id, const spa_pod *param) {
    PipewireImpl *audio = static_cast<PipewireImpl *>(_data);
    PipewireData *data = &audio->pwdata;

    if (param == NULL || id != SPA_PARAM_Format)
        return;
        
    if (spa_format_parse(param, &data->format.media_type, &data->format.media_subtype) < 0)
        return;
        
    if (data->format.media_type != SPA_MEDIA_TYPE_audio ||
        data->format.media_subtype != SPA_MEDIA_SUBTYPE_raw)
        return;

    spa_format_audio_raw_parse(param, &data->format.info.raw);

    std::cout << "Capturing rate: " << data->format.info.raw.rate
              << " channels: " << data->format.info.raw.channels << std::endl;
}