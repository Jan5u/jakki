#pragma once

#include "video_impl.hpp"
#include "encode.hpp"

#include <print>
#include <thread>

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

class VideoPipewireImpl : public VideoImpl {
  public:
    VideoPipewireImpl(Network* network);
    ~VideoPipewireImpl();
    void selectScreen() override;
    PipewireData pwdata;
    Encoder encoder;

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
    std::jthread m_pipewire_thread;

    int m_pipewire_fd = -1;
    struct pw_context *m_pw_context = nullptr;
    struct pw_core *m_pw_core = nullptr;
};