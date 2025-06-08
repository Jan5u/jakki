#pragma once

#include <iostream>
#include <opus/opus.h>
#include <pipewire/pipewire.h>
#include <queue>
#include <spa/param/audio/format-utils.h>
#include <thread>
#include <vector>
#include <unordered_map>
#include "network.hpp"

#define DEFAULT_RATE 48000
#define DEFAULT_CHANNELS 2
#define MIC_CHANNELS 1
#define FRAME_SIZE 960

struct PipewireData {
    struct pw_main_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_stream *capture_stream;
    struct pw_stream *playback_stream;
    struct pw_registry *registry;
    struct spa_hook listener;
    struct spa_audio_info format;
};

class Network;

class Audio {
  public:
    Audio(Network& network);
    void startAudioThread();
    void handleIncomingVoicePacket(const std::string& userId, const std::vector<uint8_t>& payload);

  private:
    PipewireData pwdata;
    std::jthread audioLoopThread;
    OpusEncoder *encoder;
    OpusDecoder *decoder;
    void initPipewire();
    void initOpus();
    void initAudioThread();
    static void on_process_record(void *data);
    static void on_process_playback(void *data);
    static void on_stream_param_changed(void *_data, uint32_t id, const struct spa_pod *param);
    void encodePacketWithOpus(OpusEncoder *encoder, std::vector<int16_t> *audio);
    void opusCleanup();
    Network& networkManager;
    std::unordered_map<std::string, std::queue<std::vector<int16_t>>> userAudioBuffers;
    std::mutex bufferMutex;
    static const size_t MAX_BUFFER_SIZE = 5; // Max 5 packets per user
    std::vector<int16_t> mixUserAudioBuffers(int n_frames);
    std::vector<int16_t> decodeOpusPacket(const std::vector<uint8_t>& payload);
    void addUserAudio(const std::string& userId, const std::vector<int16_t>& audioData);
};