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

class Config;
class WasapiImpl;

// Notification client for device change events
class DeviceNotificationClient : public IMMNotificationClient {
public:
    DeviceNotificationClient(WasapiImpl* parent) : refCount(1), wasapiImpl(parent) {}
    virtual ~DeviceNotificationClient() {}

    // IUnknown methods
    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&refCount);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG ulRef = InterlockedDecrement(&refCount);
        if (0 == ulRef) {
            delete this;
        }
        return ulRef;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, VOID **ppvInterface) override {
        if (IID_IUnknown == riid) {
            AddRef();
            *ppvInterface = (IUnknown*)this;
        } else if (__uuidof(IMMNotificationClient) == riid) {
            AddRef();
            *ppvInterface = (IMMNotificationClient*)this;
        } else {
            *ppvInterface = NULL;
            return E_NOINTERFACE;
        }
        return S_OK;
    }

    // IMMNotificationClient methods
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR pwstrDeviceId) override;
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR pwstrDeviceId) override;
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR pwstrDeviceId) override;
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR pwstrDeviceId, DWORD dwNewState) override;
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR pwstrDeviceId, const PROPERTYKEY key) override;

private:
    LONG refCount;
    WasapiImpl* wasapiImpl;
};

class WasapiImpl : public AudioImpl {
public:
    explicit WasapiImpl(Network& network, Config& config);
    ~WasapiImpl() override;
    
    // Implementation of AudioImpl interface
    void initAudio() override;
    void startCapture() override;
    void stopCapture() override;
    void cleanup() override;

    // Device enumeration
    std::vector<AudioDevice> getInputDevices() const override;
    std::vector<AudioDevice> getOutputDevices() const override;
    
    // Device selection
    void setInputDevice(const std::string& deviceId) override;
    void setOutputDevice(const std::string& deviceId) override;

private:
    void startPlayback();
    void playbackLoop();
    void captureLoop();
    void createInstance();
    void enumerateAudioDevices();
    void printEndpointProperties(IMMDevice *Device);
    void selectDefaultRenderDevice();
    void selectDefaultCaptureDevice();
    void selectInitialAudioDevices();

    friend class DeviceNotificationClient;

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

    Config& config;

    std::vector<AudioDevice> inputDevices;
    std::vector<AudioDevice> outputDevices;
    DeviceNotificationClient* notificationClient = nullptr;
};