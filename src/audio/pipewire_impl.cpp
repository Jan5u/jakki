#include "pipewire_impl.hpp"
#include "../network.hpp"
#include "../config.hpp"

PipewireImpl::PipewireImpl(Network& network, Config& cfg) 
    : AudioImpl(network), config(cfg) {
    memset(&pwdata, 0, sizeof(pwdata));
    inputDevices.clear();
    outputDevices.clear();
}

PipewireImpl::~PipewireImpl() {
    cleanup();
}

void PipewireImpl::initAudio() {
    // Initialize Opus codec
    initOpus();
    
    // Initialize PipeWire
    audioLoopThread = std::jthread([this] { initPipewire(); });
}

void PipewireImpl::startCapture() {
    capturing = true;
    setStreamsActive(true);
}

void PipewireImpl::stopCapture() {
    capturing = false;
    setStreamsActive(false);

    if (pwdata.loop) {
        pw_main_loop_quit(pwdata.loop);
    }
}

void PipewireImpl::setStreamsActive(bool active) {
    if (!pwdata.loop) {
        return;
    }

    // Use pw_loop_invoke to call pw_stream_set_active from the correct thread context
    pw_loop_invoke(pw_main_loop_get_loop(pwdata.loop), setStreamsActiveCallback, SPA_ID_INVALID, &active, sizeof(active), true, this);
}

int PipewireImpl::setStreamsActiveCallback(struct spa_loop *loop, bool async, uint32_t seq, const void *data, size_t size, void *user_data) {
    PipewireImpl *self = static_cast<PipewireImpl *>(user_data);
    bool active = *static_cast<const bool *>(data);

    if (self->pwdata.capture_stream) {
        pw_stream_set_active(self->pwdata.capture_stream, active);
    }
    if (self->pwdata.playback_stream) {
        pw_stream_set_active(self->pwdata.playback_stream, active);
    }

    return 0;
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

std::vector<AudioDevice> PipewireImpl::getInputDevices() const {
    return inputDevices;
}

std::vector<AudioDevice> PipewireImpl::getOutputDevices() const {
    return outputDevices;
}

void PipewireImpl::setInputDevice(const std::string &deviceId) {
    std::cout << "Setting input device to: " << (deviceId.empty() ? "default" : deviceId) << std::endl;

    // Update input stream with new device
    if (pwdata.loop) {
        std::string *deviceIdCopy = new std::string(deviceId);

        pw_loop_invoke(
            pw_main_loop_get_loop(pwdata.loop),
            [](struct spa_loop *loop, bool async, uint32_t seq, const void *data, size_t size, void *user_data) -> int {
                PipewireImpl *impl = static_cast<PipewireImpl *>(user_data);
                std::string *deviceIdPtr = *static_cast<std::string *const *>(data);
                impl->updateInputDevice(*deviceIdPtr);
                delete deviceIdPtr;
                return 0;
            },
            SPA_ID_INVALID, &deviceIdCopy, sizeof(std::string *), true, this);
    }
}

void PipewireImpl::setOutputDevice(const std::string &deviceId) {
    std::cout << "Setting output device to: " << (deviceId.empty() ? "default" : deviceId) << std::endl;

    // Update output stream with new device
    if (pwdata.loop) {
        std::string *deviceIdCopy = new std::string(deviceId);

        pw_loop_invoke(
            pw_main_loop_get_loop(pwdata.loop),
            [](struct spa_loop *loop, bool async, uint32_t seq, const void *data, size_t size, void *user_data) -> int {
                PipewireImpl *impl = static_cast<PipewireImpl *>(user_data);
                std::string *deviceIdPtr = *static_cast<std::string *const *>(data);
                impl->updateOutputDevice(*deviceIdPtr);
                delete deviceIdPtr;
                return 0;
            },
            SPA_ID_INVALID, &deviceIdCopy, sizeof(std::string *), true, this);
    }
}

void PipewireImpl::updateInputDevice(const std::string &deviceId) {
    if (!pwdata.capture_stream) {
        std::cerr << "Capture stream not initialized" << std::endl;
        return;
    }

    const char *targetInput = deviceId.empty() ? nullptr : deviceId.c_str();

    std::cout << "Updating capture stream target to: " << (targetInput ? targetInput : "default") << std::endl;

    bool wasCapturing = capturing;

    pw_stream_disconnect(pwdata.capture_stream);

    // Update target object property
    struct spa_dict_item items[] = {{PW_KEY_TARGET_OBJECT, targetInput}};
    struct spa_dict dict = SPA_DICT_INIT(items, 1);
    pw_stream_update_properties(pwdata.capture_stream, &dict);

    // Set up audio format
    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *params[1];

    spa_audio_info_raw raw_info;
    raw_info.format = SPA_AUDIO_FORMAT_F32;
    raw_info.channels = DEFAULT_CHANNELS;
    raw_info.rate = DEFAULT_RATE;

    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &raw_info);

    // Reconnect with same parameters
    pw_stream_connect(pwdata.capture_stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                      static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                                   PW_STREAM_FLAG_MAP_BUFFERS |
                                                   PW_STREAM_FLAG_RT_PROCESS |
                                                   (wasCapturing ? 0 : PW_STREAM_FLAG_INACTIVE)),
                      params, 1);

    std::cout << "Capture stream target updated successfully" << std::endl;
}

void PipewireImpl::updateOutputDevice(const std::string &deviceId) {
    if (!pwdata.playback_stream) {
        std::cerr << "Playback stream not initialized" << std::endl;
        return;
    }

    const char *targetOutput = deviceId.empty() ? nullptr : deviceId.c_str();

    std::cout << "Updating playback stream target to: " << (targetOutput ? targetOutput : "default") << std::endl;

    bool wasCapturing = capturing;

    // Disconnect stream
    pw_stream_disconnect(pwdata.playback_stream);

    // Update target object property
    struct spa_dict_item items[] = {{PW_KEY_TARGET_OBJECT, targetOutput}};
    struct spa_dict dict = SPA_DICT_INIT(items, 1);
    pw_stream_update_properties(pwdata.playback_stream, &dict);

    // Set up audio format
    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *params[1];

    spa_audio_info_raw raw_info;
    raw_info.format = SPA_AUDIO_FORMAT_F32;
    raw_info.channels = DEFAULT_CHANNELS;
    raw_info.rate = DEFAULT_RATE;

    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &raw_info);

    // Reconnect with same parameters
    pw_stream_connect(pwdata.playback_stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                      static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                                   PW_STREAM_FLAG_MAP_BUFFERS |
                                                   PW_STREAM_FLAG_RT_PROCESS |
                                                   (wasCapturing ? 0 : PW_STREAM_FLAG_INACTIVE)),
                      params, 1);
    
    std::cout << "Playback stream target updated successfully" << std::endl;
}

void PipewireImpl::setVolume(bool isInput, float volume) {
    if (!pwdata.loop) {
        std::cerr << "PipeWire not initialized" << std::endl;
        return;
    }
    volume = std::max(0.0f, std::min(1.0f, volume));
    std::cout << "Setting volume for " << (isInput ? "input" : "output") << " stream to " << volume << std::endl;
    struct VolumeData {
        bool isInput;
        float volume;
    };
    VolumeData volumeData{isInput, volume};
    pw_loop_invoke(
        pw_main_loop_get_loop(pwdata.loop),
        [](struct spa_loop *loop, bool async, uint32_t seq, const void *data, size_t size, void *user_data) -> int {
            PipewireImpl *impl = static_cast<PipewireImpl *>(user_data);
            const VolumeData *vdata = static_cast<const VolumeData *>(data);

            struct pw_stream *stream = vdata->isInput ? impl->pwdata.capture_stream : impl->pwdata.playback_stream;
            if (!stream) {
                std::cerr << "Stream not initialized" << std::endl;
                return 0;
            }
            uint8_t buffer[1024];
            struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

            float cubicVolume = vdata->volume * vdata->volume * vdata->volume;

            // Set volumes for each channel
            float volumes[SPA_AUDIO_MAX_CHANNELS];
            for (int i = 0; i < SPA_AUDIO_MAX_CHANNELS; i++) {
                volumes[i] = cubicVolume;
            }

            const struct spa_pod *param = static_cast<const struct spa_pod *>(spa_pod_builder_add_object(
                &b, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props, SPA_PROP_channelVolumes,
                SPA_POD_Array(sizeof(float), SPA_TYPE_Float, DEFAULT_CHANNELS, volumes), SPA_PROP_mute, SPA_POD_Bool(false)));

            pw_stream_set_param(stream, SPA_PARAM_Props, param);

            std::cout << "Volume parameter set successfully (linear: " << vdata->volume << ", cubic: " << cubicVolume << ")" << std::endl;

            return 0;
        },
        SPA_ID_INVALID, &volumeData, sizeof(VolumeData), true, this);

    notifyVolumeChanged(isInput, volume);
}

float PipewireImpl::getVolume(bool isInput) const {
    if (!pwdata.loop) {
        std::cerr << "PipeWire not initialized" << std::endl;
        return 1.0f;
    }
    struct pw_stream *stream = isInput ? pwdata.capture_stream : pwdata.playback_stream;
    if (!stream) {
        std::cerr << "Stream not initialized" << std::endl;
        return 1.0f;
    }
    const struct pw_stream_control *control = pw_stream_get_control(stream, SPA_PROP_channelVolumes);
    if (!control || !control->values) {
        return 1.0f;
    }
    if (control->n_values > 0) {
        float cubicVolume = control->values[0];
        // Convert from cubic to linear
        float linearVolume = std::cbrt(cubicVolume);
        return std::max(0.0f, std::min(1.0f, linearVolume));
    }
    return 1.0f;
}

void PipewireImpl::initPipewire() {
    pw_init(nullptr, nullptr);

    pwdata.loop = pw_main_loop_new(nullptr);
    pwdata.context = pw_context_new(pw_main_loop_get_loop(pwdata.loop), nullptr, 0);
    pwdata.core = pw_context_connect(pwdata.context, nullptr, 0);
    pwdata.registry = pw_core_get_registry(pwdata.core, PW_VERSION_REGISTRY, 0);

    pw_registry_events registry_events = {
        .version = PW_VERSION_REGISTRY_EVENTS,
        .global = registry_event_global,
        .global_remove = registry_event_global_remove,
    };

    pw_stream_events stream_events = {
        .version = PW_VERSION_STREAM_EVENTS,
        .param_changed = on_stream_param_changed_capture,
        .process = on_process_record,
    };

    pw_stream_events stream_events_playback = {
        .version = PW_VERSION_STREAM_EVENTS,
        .param_changed = on_stream_param_changed_playback,
        .process = on_process_playback,
    };

    // Get saved devices from config
    std::string savedInputDevice = config.getInputDevice();
    std::string savedOutputDevice = config.getOutputDevice();

    // Use saved input device if available, otherwise let PipeWire choose default
    const char *targetInput = savedInputDevice.empty() ? nullptr : savedInputDevice.c_str();
    const char *targetOutput = savedOutputDevice.empty() ? nullptr : savedOutputDevice.c_str();

    std::cout << "Creating capture stream with target: " << (targetInput ? targetInput : "default") << std::endl;
    std::cout << "Creating playback stream with target: " << (targetOutput ? targetOutput : "default") << std::endl;

    pwdata.capture_stream = pw_stream_new_simple(
        pw_main_loop_get_loop(pwdata.loop), "jakki-capture",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
                          PW_KEY_MEDIA_ROLE, "Communication", PW_KEY_NODE_LATENCY, "20/1000",
                          PW_KEY_NODE_RATE, "1/20", PW_KEY_NODE_FORCE_QUANTUM, "960", 
                          PW_KEY_TARGET_OBJECT, targetInput, nullptr),
        &stream_events, this);

    pwdata.playback_stream = pw_stream_new_simple(
        pw_main_loop_get_loop(pwdata.loop), "jakki-playback",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
                          PW_KEY_MEDIA_ROLE, "Communication", PW_KEY_NODE_LATENCY, "20/1000",
                          PW_KEY_NODE_RATE, "1/20", PW_KEY_NODE_FORCE_QUANTUM, "960",
                          PW_KEY_TARGET_OBJECT, targetOutput, nullptr),
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
                                                   PW_STREAM_FLAG_RT_PROCESS |
                                                   PW_STREAM_FLAG_INACTIVE),
                      params, 1);

    pw_stream_connect(pwdata.playback_stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                      static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                                   PW_STREAM_FLAG_MAP_BUFFERS |
                                                   PW_STREAM_FLAG_RT_PROCESS |
                                                   PW_STREAM_FLAG_INACTIVE),
                      params, 1);

    spa_zero(pwdata.registry_listener);
    pw_registry_add_listener(pwdata.registry, &pwdata.registry_listener, &registry_events, this);

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

void PipewireImpl::on_stream_param_changed_capture(void *_data, uint32_t id, const spa_pod *param) {
    PipewireImpl *audio = static_cast<PipewireImpl *>(_data);
    PipewireData *data = &audio->pwdata;

    if (param == NULL) {
        return;
    }

    // Handle Format changes
    if (id == SPA_PARAM_Format) {
        if (spa_format_parse(param, &data->format.media_type, &data->format.media_subtype) < 0)
            return;

        if (data->format.media_type != SPA_MEDIA_TYPE_audio || data->format.media_subtype != SPA_MEDIA_SUBTYPE_raw)
            return;

        spa_format_audio_raw_parse(param, &data->format.info.raw);

        std::cout << "Capture stream rate: " << data->format.info.raw.rate << " channels: " << data->format.info.raw.channels << std::endl;
    }

    // Handle Props changes
    if (id == SPA_PARAM_Props) {
        struct spa_pod_prop *prop;
        struct spa_pod_object *obj = (struct spa_pod_object *)param;

        SPA_POD_OBJECT_FOREACH(obj, prop) {
            if (prop->key == SPA_PROP_channelVolumes) {
                std::cout << "Detected channelVolumes property change (capture)" << std::endl;

                // Extract volume values
                if (spa_pod_is_array(&prop->value)) {
                    struct spa_pod_array *arr = (struct spa_pod_array *)&prop->value;
                    uint32_t n_values = SPA_POD_ARRAY_N_VALUES(arr);
                    if (arr->body.child.size == sizeof(float) && n_values > 0) {
                        float *volumes = (float *)SPA_MEMBER(&arr->body, sizeof(struct spa_pod_array_body), void);
                        float cubicVolume = volumes[0];
                        float linearVolume = std::cbrt(cubicVolume);
                        std::cout << "Capture volume changed (cubic: " << cubicVolume << ", linear: " << linearVolume << ")" << std::endl;
                        audio->notifyVolumeChanged(true, linearVolume);
                    }
                }
            }
        }
    }
}

void PipewireImpl::on_stream_param_changed_playback(void *_data, uint32_t id, const spa_pod *param) {
    PipewireImpl *audio = static_cast<PipewireImpl *>(_data);

    if (param == NULL) {
        return;
    }

    // Handle Format changes
    if (id == SPA_PARAM_Format) {
        std::cout << "Playback stream format changed" << std::endl;
    }

    // Handle Props changes
    if (id == SPA_PARAM_Props) {
        struct spa_pod_prop *prop;
        struct spa_pod_object *obj = (struct spa_pod_object *)param;

        SPA_POD_OBJECT_FOREACH(obj, prop) {
            if (prop->key == SPA_PROP_channelVolumes) {
                std::cout << "Detected channelVolumes property change (playback)" << std::endl;
                if (spa_pod_is_array(&prop->value)) {
                    struct spa_pod_array *arr = (struct spa_pod_array *)&prop->value;
                    uint32_t n_values = SPA_POD_ARRAY_N_VALUES(arr);
                    if (arr->body.child.size == sizeof(float) && n_values > 0) {
                        float *volumes = (float *)SPA_MEMBER(&arr->body, sizeof(struct spa_pod_array_body), void);
                        float cubicVolume = volumes[0];
                        float linearVolume = std::cbrt(cubicVolume);
                        std::cout << "Playback volume changed (cubic: " << cubicVolume << ", linear: " << linearVolume << ")" << std::endl;
                        audio->notifyVolumeChanged(false, linearVolume);
                    }
                }
            }
        }
    }
}

void PipewireImpl::registry_event_global(void *data, uint32_t id, uint32_t permissions, const char *type, uint32_t version, const struct spa_dict *props) {
    PipewireImpl *impl = static_cast<PipewireImpl *>(data);

    if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0) {
        return;
    }

    const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    const char *node_name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    const char *node_description = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);

    if (!media_class || !node_description) {
        return;
    }

    AudioDevice device;
    device.id = node_name;
    device.name = node_description;
    device.nodeId = id;

    if (strcmp(media_class, "Audio/Source") == 0) {
        std::cout << "Found input device: " << device.name << " (ID: " << device.id << ", Node ID: " << id << ")\n";
        device.isInput = true;
        impl->inputDevices.push_back(device);
        impl->notifyDeviceListChanged();
    } else if (strcmp(media_class, "Audio/Sink") == 0) {
        std::cout << "Found output device: " << device.name << " (ID: " << device.id << ", Node ID: " << id << ")\n";
        device.isInput = false;
        impl->outputDevices.push_back(device);
        impl->notifyDeviceListChanged();
    }
}

void PipewireImpl::registry_event_global_remove(void *data, uint32_t id) {
    if (!data) {
        return;
    }

    PipewireImpl *impl = static_cast<PipewireImpl *>(data);

    // Remove from input devices using node ID
    auto inputIt = std::remove_if(impl->inputDevices.begin(), impl->inputDevices.end(),
        [id](const AudioDevice& device) {
        return device.nodeId == id;
    });

    if (inputIt != impl->inputDevices.end()) {
        std::cout << "Removed input device with node ID: " << id << std::endl;
        impl->inputDevices.erase(inputIt, impl->inputDevices.end());
        impl->notifyDeviceListChanged();
        return;
    }

    // Remove from output devices using node ID
    auto outputIt = std::remove_if(impl->outputDevices.begin(), impl->outputDevices.end(),
        [id](const AudioDevice& device) {
        return device.nodeId == id;
    });

    if (outputIt != impl->outputDevices.end()) {
        std::cout << "Removed output device with node ID: " << id << std::endl;
        impl->outputDevices.erase(outputIt, impl->outputDevices.end());
        impl->notifyDeviceListChanged();
    }
}