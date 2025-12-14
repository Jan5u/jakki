#include "pipewire_impl.hpp"

VideoPipewireImpl::VideoPipewireImpl() {
    std::println("VideoPipewireImpl initialized");
}

VideoPipewireImpl::~VideoPipewireImpl() {
    if (m_session_handle) {
        g_free(m_session_handle);
    }
    if (m_screencast_proxy) {
        g_object_unref(m_screencast_proxy);
    }
    if (m_connection) {
        g_object_unref(m_connection);
    }
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

void VideoPipewireImpl::initPortal() {
    GError *error = nullptr;
    m_connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (error) {
        std::println(stderr, "Failed to connect to session bus: {}", error->message);
        g_error_free(error);
        return;
    }

    m_screencast_proxy = g_dbus_proxy_new_sync(
        m_connection,
        G_DBUS_PROXY_FLAGS_NONE,
        nullptr,
        "org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.ScreenCast",
        nullptr,
        &error
    );

    if (error) {
        std::println(stderr, "Failed to create ScreenCast proxy: {}", error->message);
        g_error_free(error);
        if (m_connection) {
            g_object_unref(m_connection);
            m_connection = nullptr;
        }
        return;
    }

    std::println("XDG Desktop Portal ScreenCast interface initialized successfully");
}

void VideoPipewireImpl::selectScreen() {
    if (!m_screencast_proxy) {
        initPortal();
    }
    if (!m_screencast_proxy) {
        std::println(stderr, "ScreenCast proxy not initialized");
        return;
    }
    static int request_counter = 0;
    char token[64];
    snprintf(token, sizeof(token), "jakki_token_%d_%ld", ++request_counter, time(nullptr));
    
    char sender_name[256];
    strncpy(sender_name, g_dbus_connection_get_unique_name(m_connection) + 1, sizeof(sender_name) - 1);
    for (char *p = sender_name; *p; p++) {
        if (*p == '.') *p = '_';
    }
    
    char request_path[512];
    snprintf(request_path, sizeof(request_path), "/org/freedesktop/portal/desktop/request/%s/%s", sender_name, token);

    g_dbus_connection_signal_subscribe(
        m_connection,
        "org.freedesktop.portal.Desktop",
        "org.freedesktop.portal.Request",
        "Response",
        request_path,
        nullptr,
        G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
        onCreateSessionResponse,
        this,
        nullptr
    );

    GVariantBuilder options_builder;
    g_variant_builder_init(&options_builder, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&options_builder, "{sv}", "handle_token", g_variant_new_string(token));
    g_variant_builder_add(&options_builder, "{sv}", "session_handle_token", g_variant_new_string(token));

    GError *error = nullptr;
    GVariant *result = g_dbus_proxy_call_sync(
        m_screencast_proxy,
        "CreateSession",
        g_variant_new("(a{sv})", &options_builder),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        nullptr,
        &error
    );

    if (error) {
        std::println(stderr, "CreateSession failed: {}", error->message);
        g_error_free(error);
        return;
    }

    if (result) {
        g_variant_unref(result);
    }

    std::println("CreateSession called, waiting for response...");
}

void VideoPipewireImpl::onCreateSessionResponse(GDBusConnection *connection, const char *sender_name,
                                          const char *object_path, const char *interface_name,
                                          const char *signal_name, GVariant *parameters,
                                          gpointer user_data) {
    auto *self = static_cast<VideoPipewireImpl*>(user_data);
    
    guint32 response;
    GVariant *results;
    g_variant_get(parameters, "(u@a{sv})", &response, &results);
    if (response != 0) {
        std::println(stderr, "CreateSession failed with response code: {}", response);
        g_variant_unref(results);
        return;
    }

    GVariant *session_handle_variant = g_variant_lookup_value(results, "session_handle", G_VARIANT_TYPE_STRING);
    if (session_handle_variant) {
        self->m_session_handle = g_strdup(g_variant_get_string(session_handle_variant, nullptr));
        std::println("Session created: {}", self->m_session_handle);
        g_variant_unref(session_handle_variant);
    }
    g_variant_unref(results);

    static int request_counter = 0;
    char token[64];
    snprintf(token, sizeof(token), "jakki_select_%d_%ld", ++request_counter, time(nullptr));
    
    char sender_name_clean[256];
    strncpy(sender_name_clean, g_dbus_connection_get_unique_name(connection) + 1, sizeof(sender_name_clean) - 1);
    for (char *p = sender_name_clean; *p; p++) {
        if (*p == '.') *p = '_';
    }
    
    char request_path[512];
    snprintf(request_path, sizeof(request_path), "/org/freedesktop/portal/desktop/request/%s/%s", sender_name_clean, token);

    g_dbus_connection_signal_subscribe(
        connection,
        "org.freedesktop.portal.Desktop",
        "org.freedesktop.portal.Request",
        "Response",
        request_path,
        nullptr,
        G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
        onSelectSourcesResponse,
        self,
        nullptr
    );

    GVariantBuilder options_builder;
    g_variant_builder_init(&options_builder, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&options_builder, "{sv}", "handle_token", g_variant_new_string(token));
    g_variant_builder_add(&options_builder, "{sv}", "types", g_variant_new_uint32(1 | 2)); // Monitor and Window
    g_variant_builder_add(&options_builder, "{sv}", "multiple", g_variant_new_boolean(FALSE));

    GError *error = nullptr;
    GVariant *result = g_dbus_proxy_call_sync(
        self->m_screencast_proxy,
        "SelectSources",
        g_variant_new("(oa{sv})", self->m_session_handle, &options_builder),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        nullptr,
        &error
    );

    if (error) {
        std::println(stderr, "SelectSources failed: {}", error->message);
        g_error_free(error);
        return;
    }

    if (result) {
        g_variant_unref(result);
    }

    std::println("SelectSources called, waiting for user selection...");
}

void VideoPipewireImpl::onSelectSourcesResponse(GDBusConnection *connection, const char *sender_name,
                                         const char *interface_name, const char *signal_name,
                                         const char *object_path, GVariant *parameters,
                                         gpointer user_data) {
    auto *self = static_cast<VideoPipewireImpl*>(user_data);
    
    guint32 response;
    GVariant *results;
    g_variant_get(parameters, "(u@a{sv})", &response, &results);
    if (response != 0) {
        std::println(stderr, "SelectSources cancelled or failed with response code: {}", response);
        g_variant_unref(results);
        return;
    }
    std::println("Sources selected, calling Start...");
    g_variant_unref(results);

    static int request_counter = 0;
    char token[64];
    snprintf(token, sizeof(token), "jakki_start_%d_%ld", ++request_counter, time(nullptr));
    
    char sender_name_clean[256];
    strncpy(sender_name_clean, g_dbus_connection_get_unique_name(connection) + 1, sizeof(sender_name_clean) - 1);
    for (char *p = sender_name_clean; *p; p++) {
        if (*p == '.') *p = '_';
    }
    
    char request_path[512];
    snprintf(request_path, sizeof(request_path), "/org/freedesktop/portal/desktop/request/%s/%s", sender_name_clean, token);

    g_dbus_connection_signal_subscribe(
        connection,
        "org.freedesktop.portal.Desktop",
        "org.freedesktop.portal.Request",
        "Response",
        request_path,
        nullptr,
        G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
        onStartResponse,
        self,
        nullptr
    );

    GVariantBuilder options_builder;
    g_variant_builder_init(&options_builder, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&options_builder, "{sv}", "handle_token", g_variant_new_string(token));

    GError *error = nullptr;
    GVariant *result = g_dbus_proxy_call_sync(
        self->m_screencast_proxy,
        "Start",
        g_variant_new("(osa{sv})", self->m_session_handle, "", &options_builder),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        nullptr,
        &error
    );

    if (error) {
        std::println(stderr, "Start failed: {}", error->message);
        g_error_free(error);
        return;
    }

    if (result) {
        g_variant_unref(result);
    }

    std::println("Start called, waiting for stream information...");
}

void VideoPipewireImpl::onStartResponse(GDBusConnection *connection, const char *sender_name,
                                 const char *object_path, const char *interface_name,
                                 const char *signal_name, GVariant *parameters,
                                 gpointer user_data) {
    auto *self = static_cast<VideoPipewireImpl *>(user_data);

    guint32 response;
    GVariant *results;
    g_variant_get(parameters, "(u@a{sv})", &response, &results);

    if (response != 0) {
        std::println(stderr, "Start failed with response code: {}", response);
        g_variant_unref(results);
        return;
    }

    GVariant *streams_variant = g_variant_lookup_value(results, "streams", G_VARIANT_TYPE("a(ua{sv})"));
    if (streams_variant) {
        GVariantIter iter;
        g_variant_iter_init(&iter, streams_variant);
        guint32 node_id;
        GVariant *stream_properties;
        while (g_variant_iter_next(&iter, "(u@a{sv})", &node_id, &stream_properties)) {
            self->m_portal_node_id = node_id;
            self->openPipewireRemote();
            if (self->m_pipewire_fd >= 0) {
                self->m_pipewire_thread = std::jthread([self]() { self->createPipewireNode(); });
            }
            g_variant_unref(stream_properties);
        }
        g_variant_unref(streams_variant);
    }
    g_variant_unref(results);
    std::println("Screen selection complete!");
}

static void on_process(void *userdata) {
    VideoPipewireImpl *self = static_cast<VideoPipewireImpl *>(userdata);
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
    std::println("got a frame: size={}, stride={}, flags={}", d->chunk->size, d->chunk->stride, d->chunk->flags);
    if (d->type == SPA_DATA_MemFd || d->type == SPA_DATA_DmaBuf) {
        std::println("  type: DMA-BUF/MemFd (fd={})", d->fd);
    } else if (d->type == SPA_DATA_MemPtr) {
        std::println("  type: MemPtr (data={})", (void *)d->data);
    }
    pw_stream_queue_buffer(data->stream, b);
}

static void on_stream_state_changed(void *userdata, enum pw_stream_state old, enum pw_stream_state state, const char *error) {
    VideoPipewireImpl *self = static_cast<VideoPipewireImpl *>(userdata);
    std::println("Stream state changed: {} -> {}", pw_stream_state_as_string(old), pw_stream_state_as_string(state));
    if (error) {
        std::println(stderr, "Stream error: {}", error);
    }
}

static void on_param_changed(void *userdata, uint32_t id, const struct spa_pod *param) {
    VideoPipewireImpl *self = static_cast<VideoPipewireImpl *>(userdata);
    PipewireData *data = &self->pwdata;

    if (param == NULL || id != SPA_PARAM_Format)
        return;

    if (spa_format_parse(param, &data->format.media_type, &data->format.media_subtype) < 0)
        return;

    if (data->format.media_type != SPA_MEDIA_TYPE_video || data->format.media_subtype != SPA_MEDIA_SUBTYPE_raw)
        return;

    if (spa_format_video_raw_parse(param, &data->format.info.raw) < 0)
        return;

    std::println("Got video format: {}x{}", data->format.info.raw.size.width, data->format.info.raw.size.height);
}

void VideoPipewireImpl::openPipewireRemote() {
    if (!m_session_handle) {
        std::println(stderr, "No session handle available");
        return;
    }

    GError *error = nullptr;
    GVariantBuilder options_builder;
    g_variant_builder_init(&options_builder, G_VARIANT_TYPE_VARDICT);

    GUnixFDList *fd_list = nullptr;
    
    GVariant *result = g_dbus_proxy_call_with_unix_fd_list_sync(
        m_screencast_proxy,
        "OpenPipeWireRemote",
        g_variant_new("(oa{sv})", m_session_handle, &options_builder),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        nullptr,
        &fd_list,
        nullptr,
        &error
    );

    if (error) {
        std::println(stderr, "OpenPipeWireRemote failed: {}", error->message);
        g_error_free(error);
        return;
    }

    if (fd_list) {
        gint32 fd_index;
        g_variant_get(result, "(h)", &fd_index);
        m_pipewire_fd = g_unix_fd_list_get(fd_list, fd_index, &error);
        
        if (error) {
            std::println(stderr, "Failed to get FD: {}", error->message);
            g_error_free(error);
            g_object_unref(fd_list);
            g_variant_unref(result);
            return;
        }
        
        std::println("OpenPipeWireRemote successful, FD: {}", m_pipewire_fd);
        g_object_unref(fd_list);
    } else {
        std::println(stderr, "No file descriptor list returned");
    }

    g_variant_unref(result);
}

void VideoPipewireImpl::createPipewireNode() {
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
    pw_main_loop_destroy(pwdata.loop);

}