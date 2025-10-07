#pragma once

#include "audio_impl.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys.h>

#define REFTIMES_PER_SEC 10000000

class WasapiImpl : public AudioImpl {
public:
    explicit WasapiImpl(Network& network);
    ~WasapiImpl() override;
    
    // Implementation of AudioImpl interface
    void initAudio() override;
    void startCapture() override;
    void stopCapture() override;
    void cleanup() override;

private:
    void startPlayback();
    void playbackLoop();
    void captureLoop();
    void createInstance();
    void enumerateAudioDevices();
    void printEndpointProperties(IMMDevice *Device);
    void selectDefaultAudioDevices();


    IMMDeviceEnumerator *pEnumerator = nullptr;
    IMMDeviceCollection *pEndpoints = nullptr;
    IMMDevice *pPlaybackDevice = nullptr;
    IMMDevice *pCaptureDevice = nullptr;
    IAudioClient *pCaptureClient = nullptr;
    IAudioClient *pRenderClient = nullptr;
    IAudioCaptureClient *pCaptureService = nullptr;
    IAudioRenderClient *pRenderService = nullptr;
    WAVEFORMATEX *pwfx = NULL;
    WAVEFORMATEX *pb_pwfx = NULL;
    std::jthread audioPlaybackThread;
    std::jthread audioCaptureThread;
    REFERENCE_TIME hnsRequestedDuration = REFTIMES_PER_SEC;

    
    // Audio buffering for Opus frame alignment
    std::vector<float> playbackBuffer;
    std::vector<float> audioBuffer;
    const size_t OPUS_FRAME_SAMPLES = 960 * 2; // 960 samples per channel * 2 channels
};