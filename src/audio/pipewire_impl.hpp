#pragma once

#include "audio_impl.hpp"
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

/**
 * PipewireImpl - Linux-specific implementation using PipeWire
 */
struct PipewireData {
    struct pw_main_loop *loop{nullptr};
    struct pw_context *context{nullptr};
    struct pw_core *core{nullptr};
    struct pw_stream *capture_stream{nullptr};
    struct pw_stream *playback_stream{nullptr};
    struct pw_registry *registry{nullptr};
    struct spa_hook listener{};
    struct spa_audio_info format{};
};

class PipewireImpl : public AudioImpl {
public:
    explicit PipewireImpl(Network& network);
    ~PipewireImpl() override;
    
    // Implementation of AudioImpl interface
    void initAudio() override;
    void startCapture() override;
    void stopCapture() override;
    void cleanup() override;

private:
    // PipeWire-specific methods
    void initPipewire();
    
    // Static callbacks for PipeWire
    static void on_process_record(void *data);
    static void on_process_playback(void *data);
    static void on_stream_param_changed(void *data, uint32_t id, const struct spa_pod *param);
    
    // Data members
    PipewireData pwdata;
    std::jthread audioLoopThread;
};