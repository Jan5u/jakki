#include "portal.hpp"

#include <gio/gio.h>

#include <print>

namespace {

struct ScreencastState {
    GMainLoop *loop = nullptr;
    XdpSession *session = nullptr;
    GError *error = nullptr;
    bool startFinished = false;
};

static void onSessionStartFinished(GObject *source_object, GAsyncResult *result, gpointer user_data) {
    auto *state = static_cast<ScreencastState *>(user_data);
    auto *session = XDP_SESSION(source_object);

    if (!xdp_session_start_finish(session, result, &state->error)) {
        g_main_loop_quit(state->loop);
        return;
    }

    state->startFinished = true;
    g_main_loop_quit(state->loop);
}

static void onScreencastSessionCreated(GObject *source_object, GAsyncResult *result, gpointer user_data) {
    auto *state = static_cast<ScreencastState *>(user_data);
    auto *portal = XDP_PORTAL(source_object);

    state->session = xdp_portal_create_screencast_session_finish(portal, result, &state->error);
    if (!state->session) {
        g_main_loop_quit(state->loop);
        return;
    }

    xdp_session_start(state->session, nullptr, nullptr, onSessionStartFinished, state);
}

} // namespace

Portal::Portal() {
    GError *error = nullptr;
    m_portal = xdp_portal_initable_new(&error);
    if (!m_portal) {
        if (error) {
            std::println(stderr, "Failed to initialize XDG desktop portal: {}", error->message);
            g_error_free(error);
        }
    }
}

Portal::~Portal() {
    close();
    if (m_portal) {
        g_object_unref(m_portal);
        m_portal = nullptr;
    }
}

bool Portal::openScreenCastPortal(uint32_t &nodeId, int &pipewireFd) {
    nodeId = 0;
    pipewireFd = -1;

    if (!m_portal) {
        return false;
    }

    close();

    GMainLoop *loop = g_main_loop_new(nullptr, FALSE);
    ScreencastState state;
    state.loop = loop;

    xdp_portal_create_screencast_session(
        m_portal,
        static_cast<XdpOutputType>(XDP_OUTPUT_MONITOR | XDP_OUTPUT_WINDOW),
        XDP_SCREENCAST_FLAG_NONE,               
        XDP_CURSOR_MODE_EMBEDDED,
        XDP_PERSIST_MODE_NONE, 
        nullptr, 
        nullptr, 
        onScreencastSessionCreated, 
        &state
    );

    g_main_loop_run(loop);
    g_main_loop_unref(loop);

    if (state.error) {
        std::println(stderr, "Failed to start screencast session: {}", state.error->message);
        g_error_free(state.error);
        return false;
    }

    if (!state.session || !state.startFinished) {
        if (state.session) {
            g_object_unref(state.session);
        }
        return false;
    }

    GVariant *streams = xdp_session_get_streams(state.session);
    if (!streams) {
        std::println(stderr, "Screencast session did not return any streams");
        g_object_unref(state.session);
        return false;
    }

    GVariantIter iter;
    g_variant_iter_init(&iter, streams);

    GVariant *streamProperties = nullptr;
    if (!g_variant_iter_next(&iter, "(u@a{sv})", &nodeId, &streamProperties)) {
        std::println(stderr, "Screencast session returned an empty stream list");
        g_variant_unref(streams);
        g_object_unref(state.session);
        return false;
    }

    if (streamProperties) {
        g_variant_unref(streamProperties);
    }
    g_variant_unref(streams);

    pipewireFd = xdp_session_open_pipewire_remote(state.session);
    if (pipewireFd < 0) {
        std::println(stderr, "Failed to open PipeWire remote from screencast session");
        g_object_unref(state.session);
        return false;
    }

    m_session = state.session;
    std::println("Screencast portal opened successfully, node id: {}, fd: {}", nodeId, pipewireFd);
    return true;
}

void Portal::close() {
    if (m_session) {
        g_object_unref(m_session);
        m_session = nullptr;
    }
}
