#pragma once

#include "capture.hpp"
#include "../encode/encoder.hpp"

#include <memory>
#include <print>
#include <thread>
#include <atomic>

#include <libdrm/drm_fourcc.h>

class Network;

#undef signals
#include <gio/gio.h>
#define signals Q_SIGNALS

#include <pipewire/pipewire.h>
#include <spa/debug/types.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/type-info.h>
#include <spa/support/loop.h>

struct PipewireData {
    struct pw_main_loop *loop;
    struct pw_stream *stream;
    struct spa_video_info format;
    struct spa_hook stream_listener;
};

class PipewireCapture : public Capture {
public:
    PipewireCapture(Network* network);
    ~PipewireCapture();
    void selectScreen() override;
    void startCapture() override;
    void startEncoding(EncoderType encoderType) override;
    void stopCapture() override;
    bool isEncoderReady() const;
    PipewireData pwdata;
    std::unique_ptr<DmaBufEncoder> encoder;

private:
    void initPortal();
    GDBusConnection *m_connection = nullptr;
    GDBusProxy *m_screencast_proxy = nullptr;
    char *m_session_handle = nullptr;
    uint32_t m_portal_node_id;
    
    static void onCreateSessionResponse(GDBusConnection *connection, const char *sender_name, const char *object_path, const char *interface_name, const char *signal_name, GVariant *parameters, gpointer user_data);
    static void onSelectSourcesResponse(GDBusConnection *connection, const char *sender_name, const char *interface_name, const char *signal_name, const char *object_path, GVariant *parameters, gpointer user_data);
    static void onStartResponse(GDBusConnection *connection, const char *sender_name, const char *object_path, const char *interface_name, const char *signal_name, GVariant *parameters, gpointer user_data);

    void createPipewireNode();
    void openPipewireRemote();
    void startPortalStream();
    void stopPortalSession();
    std::jthread m_pipewire_thread;

    Network* m_network = nullptr;
    bool m_sourcesSelected = false;
    bool m_startRequested = false;
    bool m_captureStarted = false;
    std::atomic<bool> m_encoderReady{false};

    int m_pipewire_fd = -1;
    struct pw_context *m_pw_context = nullptr;
    struct pw_core *m_pw_core = nullptr;
};
