#include "capture_linux.hpp"

namespace {

const char *vulkanEncoderName(EncoderType encoderType) {
    (void)encoderType;
    return "h264_vulkan";
}

} // namespace

PipewireCapture::PipewireCapture(Network* network) : m_network(network) {
    std::println("PipewireCapture initialized");
}

bool PipewireCapture::isEncoderReady() const {
    return m_encoderReady.load(std::memory_order_acquire);
}

PipewireCapture::~PipewireCapture() {
    stopCapture();
    delete encoder;
    encoder = nullptr;
    if (m_pw_core) {
        pw_core_disconnect(m_pw_core);
    }
    if (m_pw_context) {
        pw_context_destroy(m_pw_context);
    }
    if (m_pipewire_fd >= 0) {
        close(m_pipewire_fd);
    }
}

void PipewireCapture::selectScreen() {
    if (m_sourcesSelected) {
        return;
    }
    if (m_portal_thread.joinable()) {
        return;
    }

    m_portal_thread = std::jthread([self = this]() { self->openPortalOnThread(); });
}

void PipewireCapture::openPortalOnThread() {
    if (!m_portal.openScreenCastPortal(m_portal_node_id, m_pipewire_fd)) {
        std::println(stderr, "Failed to open screencast portal");
        return;
    }

    m_sourcesSelected = true;
    if (m_startRequested) {
        startPortalStream();
    }
}

void PipewireCapture::startCapture() {
    if (m_captureStarted) {
        return;
    }

    if (!m_sourcesSelected) {
        m_startRequested = true;
        return;
    }

    startPortalStream();
}

void PipewireCapture::startEncoding(EncoderType encoderType) {
    if (m_encoderReady.load(std::memory_order_acquire) || !m_network) {
        return;
    }

    delete encoder;
    encoder = new VulkanEncoder(m_network);
    if (encoder) {
        encoder->init(vulkanEncoderName(encoderType), pwdata.format.info.raw.size.width, pwdata.format.info.raw.size.height);
    }
    if (encoder && encoder->isReady()) {
        m_encoderReady.store(true, std::memory_order_release);
        std::println("Encoder ready ({} {}x{})", encoder->getName(), pwdata.format.info.raw.size.width, pwdata.format.info.raw.size.height);
    } else {
        std::println(stderr, "Encoder failed to initialize - frames will be dropped");
        delete encoder;
        encoder = nullptr;
    }
}

void PipewireCapture::stopCapture() {
    m_startRequested = false;
    m_captureStarted = false;

    if (pwdata.loop) {
        pw_main_loop_quit(pwdata.loop);
    }

    if (m_pipewire_thread.joinable()) {
        m_pipewire_thread.request_stop();
        m_pipewire_thread.join();
    }
    if (m_portal_thread.joinable()) {
        m_portal_thread.join();
    }

    m_sourcesSelected = false;
    m_portal.close();
    if (m_pipewire_fd >= 0) {
        close(m_pipewire_fd);
        m_pipewire_fd = -1;
    }

    delete encoder;
    encoder = nullptr;

    m_encoderReady.store(false, std::memory_order_release);
}

void PipewireCapture::startPortalStream() {
    std::lock_guard lock(m_mutex);
    if (m_captureStarted) {
        return;
    }
    if (m_pipewire_fd < 0) {
        std::println(stderr, "Cannot start capture: PipeWire remote is not open");
        return;
    }

    m_captureStarted = true;
    m_startRequested = false;
    m_pipewire_thread = std::jthread([self = this]() { self->createPipewireNode(); });
}

static void on_process(void *userdata) {
    PipewireCapture *self = static_cast<PipewireCapture *>(userdata);
    PipewireData *data = &self->pwdata;
    struct pw_buffer *b;
    struct spa_buffer *buf;
    if ((b = pw_stream_dequeue_buffer(data->stream)) == NULL) {
        std::println(stderr, "out of buffers");
        return;
    }
    buf = b->buffer;
    if (buf->n_datas == 0) {
        std::println(stderr, "no data blocks in buffer");
        pw_stream_queue_buffer(data->stream, b);
        return;
    }
    struct spa_data *d = &buf->datas[0];
    if (d->chunk->size == 0) {
        std::println(stderr, "chunk size is 0");
        pw_stream_queue_buffer(data->stream, b);
        return;
    }
    if (d->type == SPA_DATA_MemFd) {
        std::println("  type: MemFd (fd={})", d->fd);
    } else if (d->type == SPA_DATA_DmaBuf) {
        int width = data->format.info.raw.size.width;
        int height = data->format.info.raw.size.height;
        int stride = width * 4;

        if (d->maxsize > 0 && height > 0) {
            stride = d->maxsize / height;
        }

        uint64_t modifier = data->format.info.raw.modifier;
        if (modifier == 0) {
            modifier = DRM_FORMAT_MOD_LINEAR;
        }

        if (self->isEncoderReady() && self->encoder) {
            const bool ok = self->encoder->encodeDmaBufFrame(d->fd, width, height, stride, modifier);
        }
    } else if (d->type == SPA_DATA_MemPtr) {
        std::println("  type: MemPtr (data={})", (void *)d->data);
    }
    pw_stream_queue_buffer(data->stream, b);
}

static void on_stream_state_changed(void *userdata, enum pw_stream_state old, enum pw_stream_state state, const char *error) {
    PipewireCapture *self = static_cast<PipewireCapture *>(userdata);
    std::println("Stream state changed: {} -> {}", pw_stream_state_as_string(old), pw_stream_state_as_string(state));
    if (error) {
        std::println(stderr, "Stream error: {}", error);
    }
}

static void on_param_changed(void *userdata, uint32_t id, const struct spa_pod *param) {
    PipewireCapture *self = static_cast<PipewireCapture *>(userdata);
    PipewireData *data = &self->pwdata;

    if (param == NULL || id != SPA_PARAM_Format)
        return;

    if (spa_format_parse(param, &data->format.media_type, &data->format.media_subtype) < 0)
        return;

    if (data->format.media_type != SPA_MEDIA_TYPE_video || data->format.media_subtype != SPA_MEDIA_SUBTYPE_raw)
        return;

    if (spa_format_video_raw_parse(param, &data->format.info.raw) < 0)
        return;

    std::println("res: {}x{} format: {} mod: {}", data->format.info.raw.size.width, data->format.info.raw.size.height, uint8_t(data->format.info.raw.format), data->format.info.raw.modifier);

    if (!self->isEncoderReady()) {
        self->startEncoding(static_cast<EncoderType>(0));
    }
}

void PipewireCapture::createPipewireNode() {
    pw_stream_events stream_events = {
        .version = PW_VERSION_STREAM_EVENTS,
        .state_changed = on_stream_state_changed,
        .param_changed = on_param_changed,
        .process = on_process,
    };

    pwdata.loop = pw_main_loop_new(nullptr);
    if (!pwdata.loop) {
        std::println(stderr, "Failed to create PipeWire main loop");
        return;
    }

    m_pw_context = pw_context_new(pw_main_loop_get_loop(pwdata.loop), nullptr, 0);
    if (!m_pw_context) {
        std::println(stderr, "Failed to create PipeWire context");
        pw_main_loop_destroy(pwdata.loop);
        return;
    }

    m_pw_core = pw_context_connect_fd(m_pw_context, m_pipewire_fd, nullptr, 0);
    if (!m_pw_core) {
        std::println(stderr, "Failed to connect to PipeWire remote");
        pw_context_destroy(m_pw_context);
        pw_main_loop_destroy(pwdata.loop);
        return;
    }

    std::println("Connected to PipeWire via portal, target node: {}", m_portal_node_id);

    pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Video",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Screen",
        nullptr
    );

    pwdata.stream = pw_stream_new(
        m_pw_core,
        "jakki-video-capture",
        props
    );
    
    if (!pwdata.stream) {
        std::println(stderr, "Failed to create PipeWire stream");
        return;
    }
    
    pw_stream_add_listener(pwdata.stream, &pwdata.stream_listener, &stream_events, this);

    pw_stream_connect(
        pwdata.stream,
        PW_DIRECTION_INPUT,
        m_portal_node_id,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_AUTOCONNECT),
        nullptr, 0
    );

    pw_main_loop_run(pwdata.loop);

    pw_stream_destroy(pwdata.stream);
    pwdata.stream = nullptr;
    if (m_pw_core) {
        pw_core_disconnect(m_pw_core);
        m_pw_core = nullptr;
    }
    if (m_pw_context) {
        pw_context_destroy(m_pw_context);
        m_pw_context = nullptr;
    }
    pw_main_loop_destroy(pwdata.loop);
    pwdata.loop = nullptr;
}
