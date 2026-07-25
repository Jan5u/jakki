#pragma once

#include "capture.hpp"
#include "../encode/encoder.hpp"
#include "../encode/encoder_vulkan.hpp"

#include <memory>
#include <print>
#include <thread>
#include <atomic>
#include <mutex>
#include <cstdlib>

#include <libdrm/drm_fourcc.h>

#include "../../portal.hpp"

class Network;

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
    VulkanEncoder *encoder = nullptr;

private:
    uint32_t m_portal_node_id = 0;
    Portal m_portal;
    
    void createPipewireNode();
    void startPortalStream();
    void openPortalOnThread();
    std::jthread m_pipewire_thread;
    std::jthread m_portal_thread;
    std::mutex m_mutex;

    Network* m_network = nullptr;
    std::atomic<bool> m_sourcesSelected{false};
    std::atomic<bool> m_startRequested{false};
    std::atomic<bool> m_captureStarted{false};
    std::atomic<bool> m_encoderReady{false};

    int m_pipewire_fd = -1;
    struct pw_context *m_pw_context = nullptr;
    struct pw_core *m_pw_core = nullptr;
};
