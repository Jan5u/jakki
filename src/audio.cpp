#include "audio.hpp"

Audio::Audio(Network& network) : networkManager(network) {
    memset(&pwdata, 0, sizeof(pwdata));
}

Audio::~Audio() {
    std::lock_guard<std::mutex> lock(bufferMutex);
    for(auto &p : userStreams) {
        if(p.second.decoder) opus_decoder_destroy(p.second.decoder);
    }
    if(encoder) opus_encoder_destroy(encoder);
}

void Audio::initPipewire() {
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
    raw_info.format = SPA_AUDIO_FORMAT_S16;
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

    // start running loop
    pw_main_loop_run(pwdata.loop);

    // cleanup
    pw_stream_disconnect(pwdata.playback_stream);
    pw_stream_disconnect(pwdata.capture_stream);
    pw_stream_destroy(pwdata.playback_stream);
    pw_stream_destroy(pwdata.capture_stream);
    pw_proxy_destroy((struct pw_proxy *)pwdata.registry);
    pw_core_disconnect(pwdata.core);
    pw_context_destroy(pwdata.context);
    pw_main_loop_destroy(pwdata.loop);
    pw_deinit();
}

void Audio::on_process_record(void *userdata) {
    // PipewireData *data = static_cast<PipewireData *>(userdata);
    Audio *audio = static_cast<Audio *>(userdata); // Cast to Audio* instead of PipewireData*
    PipewireData *data = &audio->pwdata; // Get pwdata from the Audio instance

    struct pw_buffer *b;
    struct spa_buffer *buf;
    int16_t *samples;
    uint32_t n_channels, n_samples, format;

    if ((b = pw_stream_dequeue_buffer(data->capture_stream)) == NULL) {
        pw_log_warn("out of buffers: %m");
        return;
    }
    buf = b->buffer;
    if ((samples = static_cast<int16_t *>(buf->datas[0].data)) == NULL) {
        return;
    }
    n_channels = data->format.info.raw.channels; // 2
    format = data->format.info.raw.format;       // 259 - SPA_AUDIO_FORMAT_S16
    n_samples = buf->datas[0].chunk->size / sizeof(int16_t);

    std::cout << "n_channels: " << n_channels << " ";
    std::cout << "format: " << format << "\n";

    std::vector<int16_t> audioData(samples, samples + n_samples);


    audio->encodePacketWithOpus(audio->encoder, &audioData);



    pw_stream_queue_buffer(data->capture_stream, b);
}

void Audio::on_process_playback(void *userdata) {
    Audio *audio = static_cast<Audio *>(userdata);
    PipewireData *data = &audio->pwdata;

    struct pw_buffer *b;
    struct spa_buffer *buf;
    int stride;
    int16_t *dst;

    if ((b = pw_stream_dequeue_buffer(data->playback_stream)) == NULL) {
        pw_log_warn("out of buffers: %m");
        return;
    }
    buf = b->buffer;
    if ((dst = static_cast<int16_t *>(buf->datas[0].data)) == NULL) {
        return;
    }
    stride = sizeof(int16_t) * DEFAULT_CHANNELS;
    int n_frames = buf->datas[0].maxsize / stride;
    if (b->requested) {
        n_frames = SPA_MIN(b->requested, n_frames);
    }
    
    // Mix user audio
    std::vector<int16_t> mixed = audio->mixUserAudioBuffers(n_frames);
    if((int)mixed.size() < n_frames * DEFAULT_CHANNELS) {
        mixed.resize(n_frames * DEFAULT_CHANNELS, 0);
    }
    memcpy(dst, mixed.data(), n_frames * stride);

    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = stride;
    buf->datas[0].chunk->size = n_frames * stride;

    pw_stream_queue_buffer(data->playback_stream, b);
}


void Audio::on_stream_param_changed(void *_data, uint32_t id, const spa_pod *param) {
    PipewireData *data = static_cast<PipewireData *>(_data);

    if (param == NULL || id != SPA_PARAM_Format)
        return;
    if (spa_format_parse(param, &data->format.media_type, &data->format.media_subtype) < 0)
        return;
    if (data->format.media_type != SPA_MEDIA_TYPE_audio ||
        data->format.media_subtype != SPA_MEDIA_SUBTYPE_raw)
        return;

    spa_format_audio_raw_parse(param, &data->format.info.raw);

    fprintf(stdout, "capturing rate:%d channels:%d\n", data->format.info.raw.rate,
            data->format.info.raw.channels);
}

void Audio::initOpus() {
    std::cout << "initOpus\n";

    int error;

    // encoder
    encoder = opus_encoder_create(DEFAULT_RATE, DEFAULT_CHANNELS, OPUS_APPLICATION_VOIP, &error);
    // encoder = opus_encoder_create(DEFAULT_RATE, DEFAULT_CHANNELS, OPUS_APPLICATION_AUDIO, &error);
    if (error != OPUS_OK) {
        std::cout << "Failed to create Opus encoder: " << opus_strerror(error) << "\n";
    }
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(64000));
    opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(10));
    opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
    // opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(encoder, OPUS_SET_DTX(0));

}

void Audio::encodePacketWithOpus(OpusEncoder *encoder, std::vector<int16_t> *audio) {
    std::cout << "encodePacketWithOpus\n";
    int maxSamples = FRAME_SIZE * DEFAULT_CHANNELS;
    std::vector<uint8_t> encodedData(maxSamples);
    int encodedBytes = opus_encode(encoder, audio->data(), FRAME_SIZE, encodedData.data(), maxSamples);
    if (encodedBytes < 0) {
        std::cout << "Failed to encode audio: " << opus_strerror(encodedBytes) << "\n";
        return;
    }
    std::cout << "Encoded " << encodedBytes << " bytes\n";
    encodedData.resize(encodedBytes);

    // send to server
    networkManager.sendVoicePackets(encodedData);
}

void Audio::opusCleanup() {
    if(encoder) {
        opus_encoder_destroy(encoder);
        encoder = nullptr;
    }
}

void Audio::startAudioThread() {
    audioLoopThread = std::jthread(&Audio::initAudioThread, this);
}

void Audio::initAudioThread() {
    initOpus();
    initPipewire();
}

void Audio::handleIncomingVoicePacket(const std::string& userId, const std::vector<uint8_t>& payload) {
    auto decoded = decodeOpusPacket(userId, payload);
    if(!decoded.empty()) {
        addUserAudio(userId, std::move(decoded));
    }
}

std::vector<int16_t> Audio::decodeOpusPacket(const std::string& userId, const std::vector<uint8_t>& payload) {
    std::lock_guard<std::mutex> lock(bufferMutex);
    auto &stream = userStreams[userId];
    if(!stream.decoder) {
        int err; stream.decoder = opus_decoder_create(DEFAULT_RATE, DEFAULT_CHANNELS, &err);
        if(err != OPUS_OK) {
            std::cerr << "Failed create decoder for user " << userId << " err=" << opus_strerror(err) << "\n";
            return {};
        }
    }
    std::vector<int16_t> decoded(FRAME_SIZE * DEFAULT_CHANNELS);
    int samples = opus_decode(stream.decoder, payload.data(), payload.size(), decoded.data(), FRAME_SIZE, 0);
    if(samples < 0) {
        std::cerr << "Decode fail user=" << userId << " err=" << opus_strerror(samples) << "\n";
        return {};
    }
    decoded.resize(samples * DEFAULT_CHANNELS);
    return decoded;
}

void Audio::addUserAudio(const std::string& userId, std::vector<int16_t>&& audioData) {
    std::lock_guard<std::mutex> lock(bufferMutex);
    auto &stream = userStreams[userId];
    stream.buffers.push(std::move(audioData));
    while(stream.buffers.size() > MAX_BUFFER_SIZE) stream.buffers.pop();
}

std::vector<int16_t> Audio::mixUserAudioBuffers(int n_frames) {
    std::lock_guard<std::mutex> lock(bufferMutex);
    int samples_needed = n_frames * DEFAULT_CHANNELS;
    std::vector<int32_t> mix(samples_needed, 0);
    size_t contributors = 0;
    for(auto &kv : userStreams) {
        auto &q = kv.second.buffers;
        if(q.empty()) continue;
        auto packet = std::move(q.front()); q.pop();
        if(packet.empty()) continue;
        contributors++;
        size_t copy = std::min((int)packet.size(), samples_needed);
        for(size_t i=0;i<copy;i++) mix[i] += packet[i];
    }
    std::vector<int16_t> out(samples_needed, 0);
    if(contributors == 0) return out;
    for(int i=0;i<samples_needed;i++) {
        int32_t v = contributors > 1 ? mix[i] / (int32_t)contributors : mix[i];
        if(v > INT16_MAX) v = INT16_MAX; else if(v < INT16_MIN) v = INT16_MIN;
        out[i] = (int16_t)v;
    }
    return out;
}