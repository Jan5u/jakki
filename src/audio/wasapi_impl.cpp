#include "wasapi_impl.hpp"
#include "../network.hpp"
#include "../config.hpp"
#include <iostream>
#include <algorithm>

const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);
const IID IID_IAudioClient = __uuidof(IAudioClient);
const IID IID_IAudioRenderClient = __uuidof(IAudioRenderClient);
const IID IID_IAudioCaptureClient = __uuidof(IAudioCaptureClient);

HRESULT STDMETHODCALLTYPE DeviceNotificationClient::OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR pwstrDeviceId) {
    if (role != eCommunications) {
        return S_OK;
    }
    std::wcout << L"Default device changed: ";
    if (flow == eRender) {
        std::wcout << L"Playback device changed to: ";
    } else if (flow == eCapture) {
        std::wcout << L"Capture device changed to: ";
    }
    if (pwstrDeviceId) {
        std::wcout << pwstrDeviceId << std::endl;
    } else {
        std::wcout << L"(none)" << std::endl;
    }
    if (wasapiImpl) {
        bool isInput = (flow == eCapture);
        wasapiImpl->notifyDefaultDeviceChanged(isInput);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceNotificationClient::OnDeviceAdded(LPCWSTR pwstrDeviceId) {
    std::wcout << L"Device added: " << (pwstrDeviceId ? pwstrDeviceId : L"(unknown)") << std::endl;
    if (wasapiImpl) {
        wasapiImpl->enumerateAudioDevices();
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceNotificationClient::OnDeviceRemoved(LPCWSTR pwstrDeviceId) {
    std::wcout << L"Device removed: " << (pwstrDeviceId ? pwstrDeviceId : L"(unknown)") << std::endl;
    if (wasapiImpl) {
        wasapiImpl->enumerateAudioDevices();
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceNotificationClient::OnDeviceStateChanged(LPCWSTR pwstrDeviceId, DWORD dwNewState) {
    std::wcout << L"Device state changed: " << (pwstrDeviceId ? pwstrDeviceId : L"(unknown)");
    std::wcout << L" New state: ";
    switch (dwNewState) {
    case DEVICE_STATE_ACTIVE:
        std::wcout << L"ACTIVE";
        break;
    case DEVICE_STATE_DISABLED:
        std::wcout << L"DISABLED";
        break;
    case DEVICE_STATE_NOTPRESENT:
        std::wcout << L"NOT PRESENT";
        break;
    case DEVICE_STATE_UNPLUGGED:
        std::wcout << L"UNPLUGGED";
        break;
    default:
        std::wcout << L"UNKNOWN";
    }
    std::wcout << std::endl;
    if (wasapiImpl) {
        wasapiImpl->enumerateAudioDevices();
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceNotificationClient::OnPropertyValueChanged(LPCWSTR pwstrDeviceId, const PROPERTYKEY key) {
    if (key == PKEY_Device_FriendlyName) {
        std::wcout << L"Device property changed: " << (pwstrDeviceId ? pwstrDeviceId : L"(unknown)") << std::endl;
    }
    return S_OK;
}

WasapiImpl::WasapiImpl(Network& network, Config& cfg) 
    : AudioImpl(network), config(cfg) {
    std::cout << "WASAPI implementation created" << std::endl;
}

WasapiImpl::~WasapiImpl() {
    cleanup();
}

void WasapiImpl::initAudio() {
    initOpus();
    createInstance();
    enumerateAudioDevices();
    selectInitialAudioDevices();
}

void WasapiImpl::startCapture() {
    audioCaptureThread = std::jthread([this] { captureLoop(); });
    startPlayback();
}

void WasapiImpl::setInputDevice(const std::string &deviceId) {
    std::cout << "setInputDevice called with: " << (deviceId.empty() ? "(default)" : deviceId) << std::endl;

    // Get current device ID
    std::string currentDeviceId;
    if (pCaptureDevice) {
        LPWSTR pwszID = nullptr;
        HRESULT hr = pCaptureDevice->GetId(&pwszID);
        if (SUCCEEDED(hr) && pwszID) {
            int idLen = WideCharToMultiByte(CP_UTF8, 0, pwszID, -1, nullptr, 0, nullptr, nullptr);
            currentDeviceId.resize(idLen - 1);
            WideCharToMultiByte(CP_UTF8, 0, pwszID, -1, &currentDeviceId[0], idLen, nullptr, nullptr);
            CoTaskMemFree(pwszID);
        }
    }

    // Check if the requested device is already in use
    if (!currentDeviceId.empty() && currentDeviceId == deviceId) {
        std::cout << "Device is already in use, skipping device change" << std::endl;
        return;
    }

    // If device ID is empty (requesting default), check if current device is already the default
    if (deviceId.empty() && pCaptureDevice) {
        IMMDevice *pDefaultDevice = nullptr;
        HRESULT hr = pEnumerator->GetDefaultAudioEndpoint(eCapture, eCommunications, &pDefaultDevice);
        if (SUCCEEDED(hr) && pDefaultDevice) {
            LPWSTR pwszDefaultID = nullptr;
            hr = pDefaultDevice->GetId(&pwszDefaultID);
            if (SUCCEEDED(hr) && pwszDefaultID) {
                int idLen = WideCharToMultiByte(CP_UTF8, 0, pwszDefaultID, -1, nullptr, 0, nullptr, nullptr);
                std::string defaultDeviceId(idLen - 1, 0);
                WideCharToMultiByte(CP_UTF8, 0, pwszDefaultID, -1, &defaultDeviceId[0], idLen, nullptr, nullptr);
                CoTaskMemFree(pwszDefaultID);

                if (currentDeviceId == defaultDeviceId) {
                    std::cout << "Current device is already the default, skipping device change" << std::endl;
                    pDefaultDevice->Release();
                    return;
                }
            }
            pDefaultDevice->Release();
        }
    }

    bool wasCapturing = audioCaptureThread.joinable();

    if (audioCaptureThread.joinable()) {
        audioCaptureThread.request_stop();
    }
    if (pCaptureClient) {
        HRESULT hr = pCaptureClient->Stop();
        if (FAILED(hr)) {
            std::cerr << "Warning: Failed to stop capture client. HRESULT: 0x" << std::hex << hr << std::dec << std::endl;
        }
    }
    if (audioCaptureThread.joinable()) {
        std::cout << "Waiting for capture thread to finish..." << std::endl;
        audioCaptureThread.join();
        std::cout << "Capture thread finished" << std::endl;
    }
    if (pCaptureService) {
        pCaptureService->Release();
        pCaptureService = nullptr;
        std::cout << "Released pCaptureService" << std::endl;
    }
    if (pCaptureClient) {
        pCaptureClient->Release();
        pCaptureClient = nullptr;
        std::cout << "Released pCaptureClient" << std::endl;
    }
    if (pwfx) {
        CoTaskMemFree(pwfx);
        pwfx = nullptr;
        std::cout << "Freed pwfx" << std::endl;
    }
    if (pCaptureDevice) {
        pCaptureDevice->Release();
        pCaptureDevice = nullptr;
        std::cout << "Released pCaptureDevice" << std::endl;
    }

    // If device ID is empty, select default device
    if (deviceId.empty()) {
        selectDefaultCaptureDevice();

        // Restart capture if it was running
        if (wasCapturing && pCaptureDevice) {
            std::cout << "Restarting capture with default device" << std::endl;
            audioBuffer.clear();
            audioCaptureThread = std::jthread([this] { captureLoop(); });
        }
        return;
    }

    // Convert std::string to wide string
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, deviceId.c_str(), -1, nullptr, 0);
    std::wstring wideDeviceId(wideLen - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, deviceId.c_str(), -1, &wideDeviceId[0], wideLen);

    // Get the new device by ID
    HRESULT hr = pEnumerator->GetDevice(wideDeviceId.c_str(), &pCaptureDevice);
    if (SUCCEEDED(hr)) {
        std::cout << "Input device set successfully: " << deviceId << std::endl;
        // Restart capture if it was running
        if (wasCapturing && pCaptureDevice) {
            std::cout << "Restarting capture with new device" << std::endl;
            audioBuffer.clear();
            audioCaptureThread = std::jthread([this] { captureLoop(); });
        }
    } else {
        std::cerr << "Failed to set input device. HRESULT: 0x" << std::hex << hr << std::dec << std::endl;
    }
}

void WasapiImpl::setOutputDevice(const std::string &deviceId) {
    std::cout << "setOutputDevice called with: " << (deviceId.empty() ? "(default)" : deviceId) << std::endl;

    // Get current device ID
    std::string currentDeviceId;
    if (pPlaybackDevice) {
        LPWSTR pwszID = nullptr;
        HRESULT hr = pPlaybackDevice->GetId(&pwszID);
        if (SUCCEEDED(hr) && pwszID) {
            int idLen = WideCharToMultiByte(CP_UTF8, 0, pwszID, -1, nullptr, 0, nullptr, nullptr);
            currentDeviceId.resize(idLen - 1);
            WideCharToMultiByte(CP_UTF8, 0, pwszID, -1, &currentDeviceId[0], idLen, nullptr, nullptr);
            CoTaskMemFree(pwszID);
        }
    }

    // Check if the requested device is already in use
    if (!currentDeviceId.empty() && currentDeviceId == deviceId) {
        std::cout << "Device is already in use, skipping device change" << std::endl;
        return;
    }

    // If device ID is empty (requesting default), check if current device is already the default
    if (deviceId.empty() && pPlaybackDevice) {
        IMMDevice *pDefaultDevice = nullptr;
        HRESULT hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eCommunications, &pDefaultDevice);
        if (SUCCEEDED(hr) && pDefaultDevice) {
            LPWSTR pwszDefaultID = nullptr;
            hr = pDefaultDevice->GetId(&pwszDefaultID);
            if (SUCCEEDED(hr) && pwszDefaultID) {
                int idLen = WideCharToMultiByte(CP_UTF8, 0, pwszDefaultID, -1, nullptr, 0, nullptr, nullptr);
                std::string defaultDeviceId(idLen - 1, 0);
                WideCharToMultiByte(CP_UTF8, 0, pwszDefaultID, -1, &defaultDeviceId[0], idLen, nullptr, nullptr);
                CoTaskMemFree(pwszDefaultID);

                if (currentDeviceId == defaultDeviceId) {
                    std::cout << "Current device is already the default, skipping device change" << std::endl;
                    pDefaultDevice->Release();
                    return;
                }
            }
            pDefaultDevice->Release();
        }
    }
    bool wasPlaying = audioPlaybackThread.joinable();

    if (audioPlaybackThread.joinable()) {
        audioPlaybackThread.request_stop();
    }
    if (pRenderClient) {
        HRESULT hr = pRenderClient->Stop();
        if (FAILED(hr)) {
            std::cerr << "Warning: Failed to stop render client. HRESULT: 0x" << std::hex << hr << std::dec << std::endl;
        }
    }
    if (audioPlaybackThread.joinable()) {
        std::cout << "Waiting for playback thread to finish..." << std::endl;
        audioPlaybackThread.join();
        std::cout << "Playback thread finished" << std::endl;
    }
    if (pRenderService) {
        pRenderService->Release();
        pRenderService = nullptr;
        std::cout << "Released pRenderService" << std::endl;
    }
    if (pRenderClient) {
        pRenderClient->Release();
        pRenderClient = nullptr;
        std::cout << "Released pRenderClient" << std::endl;
    }
    if (pb_pwfx) {
        CoTaskMemFree(pb_pwfx);
        pb_pwfx = nullptr;
        std::cout << "Freed pb_pwfx" << std::endl;
    }
    if (pPlaybackDevice) {
        pPlaybackDevice->Release();
        pPlaybackDevice = nullptr;
        std::cout << "Released pPlaybackDevice" << std::endl;
    }

    // If device ID is empty, select default device
    if (deviceId.empty()) {
        selectDefaultRenderDevice();

        // Restart playback if it was running
        if (wasPlaying && pPlaybackDevice) {
            std::cout << "Restarting playback with default device" << std::endl;
            playbackBuffer.clear();
            audioPlaybackThread = std::jthread([this] { playbackLoop(); });
        }
        return;
    }

    // Convert std::string to wide string
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, deviceId.c_str(), -1, nullptr, 0);
    std::wstring wideDeviceId(wideLen - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, deviceId.c_str(), -1, &wideDeviceId[0], wideLen);

    // Get the new device by ID
    HRESULT hr = pEnumerator->GetDevice(wideDeviceId.c_str(), &pPlaybackDevice);
    if (SUCCEEDED(hr)) {
        std::cout << "Output device set successfully: " << deviceId << std::endl;
        // Restart playback if it was running
        if (wasPlaying && pPlaybackDevice) {
            std::cout << "Restarting playback with new device" << std::endl;
            playbackBuffer.clear();
            audioPlaybackThread = std::jthread([this] { playbackLoop(); });
        }
    } else {
        std::cerr << "Failed to set output device. HRESULT: 0x" << std::hex << hr << std::dec << std::endl;
    }
}

void WasapiImpl::startPlayback() {
    audioPlaybackThread = std::jthread([this] { playbackLoop(); });
}

void WasapiImpl::playbackLoop() {
    UINT32 bufferFrameCount;
    UINT32 numFramesPadding;
    UINT32 numFramesAvailable;
    DWORD flags = 0;
    BYTE *pData;
    HANDLE hRenderEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    pPlaybackDevice->Activate(IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&pRenderClient);
    pRenderClient->GetMixFormat(&pb_pwfx);

    WAVEFORMATEX* pModifiedFormat = nullptr;
    WAVEFORMATEXTENSIBLE modifiedFormat = {0};
    if (pb_pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto wfex = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(pb_pwfx);
        modifiedFormat = *wfex;
        modifiedFormat.Format.nChannels = 2;
        modifiedFormat.Format.nSamplesPerSec = 48000;
        modifiedFormat.Format.nBlockAlign = (modifiedFormat.Format.nChannels * modifiedFormat.Format.wBitsPerSample) / 8;
        modifiedFormat.Format.nAvgBytesPerSec = modifiedFormat.Format.nSamplesPerSec * modifiedFormat.Format.nBlockAlign;
        modifiedFormat.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        modifiedFormat.dwChannelMask = KSAUDIO_SPEAKER_STEREO;

        pModifiedFormat = reinterpret_cast<WAVEFORMATEX*>(&modifiedFormat);
    }

    pRenderClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED, 
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 
        200000, 
        0, 
        pModifiedFormat, 
        NULL
    );

    pRenderClient->SetEventHandle(hRenderEvent);
    pRenderClient->GetBufferSize(&bufferFrameCount);
    pRenderClient->GetService(IID_IAudioRenderClient, (void**)&pRenderService);

    pRenderClient->Start();

    playbackBuffer.clear();
    
    std::stop_token stop_token = audioPlaybackThread.get_stop_token();
    while (!stop_token.stop_requested()) {
        DWORD waitResult = WaitForSingleObject(hRenderEvent, 100);
        if (waitResult == WAIT_TIMEOUT) {
            continue;
        }
        if (waitResult != WAIT_OBJECT_0) {
            std::cerr << "Failed to wait for render event" << std::endl;
            break;
        }
        
        pRenderClient->GetCurrentPadding(&numFramesPadding);
        numFramesAvailable = bufferFrameCount - numFramesPadding;
        
        pRenderService->GetBuffer(numFramesAvailable, &pData);
        float* floatBuffer = reinterpret_cast<float*>(pData);
        size_t totalSamples = numFramesAvailable * pModifiedFormat->nChannels;
        
        // If we need more audio data, mix some
        if (playbackBuffer.size() < totalSamples) {
            // Get a full Opus frame worth of data (or more if needed)
            size_t samplesNeeded = std::max(totalSamples, (size_t)OPUS_FRAME_SAMPLES);
            std::vector<float> newAudio = mixUserAudioBuffersFloat(samplesNeeded / pModifiedFormat->nChannels);
            playbackBuffer.insert(playbackBuffer.end(), newAudio.begin(), newAudio.end());
        }
        
        // Copy exactly what we need to the output buffer
        if (playbackBuffer.size() >= totalSamples) {
            // Copy data to output buffer
            std::copy(playbackBuffer.begin(), playbackBuffer.begin() + totalSamples, floatBuffer);
            
            // Remove used samples from buffer
            playbackBuffer.erase(playbackBuffer.begin(), playbackBuffer.begin() + totalSamples);
        } else {
            // Not enough data, fill with silence
            std::fill(floatBuffer, floatBuffer + totalSamples, 0.0f);
        }
        pRenderService->ReleaseBuffer(numFramesAvailable, 0);
    }

    pRenderClient->Stop();
    CloseHandle(hRenderEvent);
}

void WasapiImpl::captureLoop() {
    UINT32 nFrames = 0;
    DWORD flags = 0;
    BYTE* captureBuffer = nullptr;
    UINT32 bufferFrameCount = 0;
    
    HANDLE hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    pCaptureDevice->Activate(IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&pCaptureClient);
    pCaptureClient->GetMixFormat(&pwfx);
    printf("Capture Mix format:\n");
    printf("  wFormatTag     : %d\n", pwfx->wFormatTag);
    printf("  nChannels      : %d\n", pwfx->nChannels);
    printf("  nSamplesPerSec : %d\n", pwfx->nSamplesPerSec);
    printf("  nAvgBytesPerSec: %ld\n", pwfx->nAvgBytesPerSec);
    printf("  nBlockAlign:   : %ld\n", pwfx->nBlockAlign);
    printf("  wBitsPerSample:   : %ld\n", pwfx->wBitsPerSample);
    printf("  cbSize:   : %ld\n", pwfx->cbSize);

    pwfx->nChannels = 2;
    pwfx->nSamplesPerSec = 48000;
    pwfx->nBlockAlign = (pwfx->nChannels * pwfx->wBitsPerSample) / 8;
    pwfx->nAvgBytesPerSec = pwfx->nSamplesPerSec * pwfx->nBlockAlign;

    pCaptureClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED, 
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 
        hnsRequestedDuration, 
        0, 
        pwfx,
        NULL
    );

    pCaptureClient->SetEventHandle(hEvent);
    pCaptureClient->GetBufferSize(&bufferFrameCount);
    printf("bufferFrameCount: %d\n", bufferFrameCount);
    pCaptureClient->GetService(IID_IAudioCaptureClient, (void**)&pCaptureService);

    pCaptureClient->Start();

    std::stop_token stop_token = audioCaptureThread.get_stop_token();
    while (!stop_token.stop_requested()) {
        // wait for event with timeout to check stop token
        DWORD waitResult = WaitForSingleObject(hEvent, 100);
        if (waitResult == WAIT_TIMEOUT) {
            continue;
        }
        if (waitResult != WAIT_OBJECT_0) {
            std::cerr << "Failed to wait for capture event" << std::endl;
            break;
        }

        pCaptureService->GetBuffer(&captureBuffer, &nFrames, &flags, NULL, NULL);
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            captureBuffer = nullptr;
        }

        if (captureBuffer != nullptr) {
            float* floatSamples = reinterpret_cast<float*>(captureBuffer);
            size_t numSamples = nFrames * pwfx->nChannels;
                        
            // Add samples to our buffer
            audioBuffer.insert(audioBuffer.end(), floatSamples, floatSamples + numSamples);
            
            // Process complete Opus frames (960 samples per channel)
            while (audioBuffer.size() >= OPUS_FRAME_SAMPLES) {
                encodePacketWithOpusFloat(audioBuffer.data(), OPUS_FRAME_SAMPLES);
                
                // Remove processed samples from buffer
                audioBuffer.erase(audioBuffer.begin(), audioBuffer.begin() + OPUS_FRAME_SAMPLES);
            }
        }

        pCaptureService->ReleaseBuffer(nFrames);
    }

    pCaptureClient->Stop();
    std::cout << "Capture loop exited" << std::endl;
}

void WasapiImpl::stopCapture() {
    // TODO: Implement WASAPI audio capture stop
    std::cout << "TODO: WASAPI audio capture stop not implemented yet" << std::endl;
}

void WasapiImpl::cleanup() {
    // Stop threads if running
    if (audioPlaybackThread.joinable()) {
        audioPlaybackThread.join();
    }
    if (audioCaptureThread.joinable()) {
        audioCaptureThread.join();
    }

    pCaptureClient->Stop();
    pCaptureClient->Release();
    pCaptureService->Release();
    pCaptureDevice->Release();
    pPlaybackDevice->Release();
    pEndpoints->Release();
    
    // Unregister notification client
    if (pEnumerator && notificationClient) {
        pEnumerator->UnregisterEndpointNotificationCallback(notificationClient);
        notificationClient->Release();
        notificationClient = nullptr;
    }
    
    pEnumerator->Release();
    CoUninitialize();

    // Cleanup Opus
    opusCleanup();
}

std::vector<AudioDevice> WasapiImpl::getInputDevices() const {
    return inputDevices;
}

std::vector<AudioDevice> WasapiImpl::getOutputDevices() const {
    return outputDevices;
}

void WasapiImpl::createInstance() {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    hr = CoCreateInstance(CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void **)&pEnumerator);
    
    if (SUCCEEDED(hr) && pEnumerator) {
        // Create and register the notification client
        notificationClient = new DeviceNotificationClient(this);
        hr = pEnumerator->RegisterEndpointNotificationCallback(notificationClient);
        if (SUCCEEDED(hr)) {
            std::cout << "Device notification client registered successfully" << std::endl;
        } else {
            std::cerr << "Failed to register device notification client. HRESULT: 0x" 
                      << std::hex << hr << std::dec << std::endl;
        }
    }
}

void WasapiImpl::enumerateAudioDevices() {
    // Clear existing device lists
    outputDevices.clear();
    inputDevices.clear();

    // Enumerate output (render) devices
    HRESULT hr = pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pEndpoints);
    if (SUCCEEDED(hr)) {
        UINT deviceCount = 0;
        hr = pEndpoints->GetCount(&deviceCount);
        if (SUCCEEDED(hr)) {
            for (UINT i = 0; i < deviceCount; i++) {
                IMMDevice *device = nullptr;
                hr = pEndpoints->Item(i, &device);
                if (SUCCEEDED(hr) && device) {
                    // Get device ID
                    LPWSTR pwszID = nullptr;
                    hr = device->GetId(&pwszID);
                    if (SUCCEEDED(hr)) {
                        // Get device friendly name
                        IPropertyStore *pProps = nullptr;
                        hr = device->OpenPropertyStore(STGM_READ, &pProps);
                        if (SUCCEEDED(hr)) {
                            PROPVARIANT varName;
                            PropVariantInit(&varName);
                            hr = pProps->GetValue(PKEY_Device_FriendlyName, &varName);
                            if (SUCCEEDED(hr) && varName.pwszVal != nullptr) {
                                // Convert wide strings to std::string
                                int idLen = WideCharToMultiByte(CP_UTF8, 0, pwszID, -1, nullptr, 0, nullptr, nullptr);
                                std::string deviceId(idLen - 1, 0);
                                WideCharToMultiByte(CP_UTF8, 0, pwszID, -1, &deviceId[0], idLen, nullptr, nullptr);

                                int nameLen = WideCharToMultiByte(CP_UTF8, 0, varName.pwszVal, -1, nullptr, 0, nullptr, nullptr);
                                std::string deviceName(nameLen - 1, 0);
                                WideCharToMultiByte(CP_UTF8, 0, varName.pwszVal, -1, &deviceName[0], nameLen, nullptr, nullptr);

                                // Create AudioDevice struct and add to output devices
                                AudioDevice audioDevice;
                                audioDevice.id = deviceId;
                                audioDevice.name = deviceName;
                                audioDevice.isInput = false;
                                audioDevice.nodeId = 0;

                                outputDevices.push_back(audioDevice);

                                std::cout << "Output device: " << deviceName << " [" << deviceId << "]" << std::endl;
                            }
                            PropVariantClear(&varName);
                            pProps->Release();
                        }
                        CoTaskMemFree(pwszID);
                    }
                    device->Release();
                }
            }
        }
        pEndpoints->Release();
    }

    // Enumerate input (capture) devices
    hr = pEnumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &pEndpoints);
    if (SUCCEEDED(hr)) {
        UINT deviceCount = 0;
        hr = pEndpoints->GetCount(&deviceCount);
        if (SUCCEEDED(hr)) {
            for (UINT i = 0; i < deviceCount; i++) {
                IMMDevice *device = nullptr;
                hr = pEndpoints->Item(i, &device);
                if (SUCCEEDED(hr) && device) {
                    // Get device ID
                    LPWSTR pwszID = nullptr;
                    hr = device->GetId(&pwszID);
                    if (SUCCEEDED(hr)) {
                        // Get device friendly name
                        IPropertyStore *pProps = nullptr;
                        hr = device->OpenPropertyStore(STGM_READ, &pProps);
                        if (SUCCEEDED(hr)) {
                            PROPVARIANT varName;
                            PropVariantInit(&varName);
                            hr = pProps->GetValue(PKEY_Device_FriendlyName, &varName);
                            if (SUCCEEDED(hr) && varName.pwszVal != nullptr) {
                                // Convert wide strings to std::string
                                int idLen = WideCharToMultiByte(CP_UTF8, 0, pwszID, -1, nullptr, 0, nullptr, nullptr);
                                std::string deviceId(idLen - 1, 0);
                                WideCharToMultiByte(CP_UTF8, 0, pwszID, -1, &deviceId[0], idLen, nullptr, nullptr);

                                int nameLen = WideCharToMultiByte(CP_UTF8, 0, varName.pwszVal, -1, nullptr, 0, nullptr, nullptr);
                                std::string deviceName(nameLen - 1, 0);
                                WideCharToMultiByte(CP_UTF8, 0, varName.pwszVal, -1, &deviceName[0], nameLen, nullptr, nullptr);

                                // Create AudioDevice struct and add to input devices
                                AudioDevice audioDevice;
                                audioDevice.id = deviceId;
                                audioDevice.name = deviceName;
                                audioDevice.isInput = true;
                                audioDevice.nodeId = 0;

                                inputDevices.push_back(audioDevice);

                                std::cout << "Input device: " << deviceName << " [" << deviceId << "]" << std::endl;
                            }
                            PropVariantClear(&varName);
                            pProps->Release();
                        }
                        CoTaskMemFree(pwszID);
                    }
                    device->Release();
                }
            }
        }
        pEndpoints->Release();
    }

    notifyDeviceListChanged();
}

void WasapiImpl::printEndpointProperties(IMMDevice *Device) {
    IPropertyStore *pProps = nullptr;
    Device->OpenPropertyStore(STGM_READ, &pProps);
    PROPVARIANT varName;
    PropVariantInit(&varName);
    pProps->GetValue(PKEY_Device_FriendlyName, &varName);
    if (varName.pwszVal != nullptr) {
        std::wcout << varName.pwszVal << std::endl;
    }
}

void WasapiImpl::selectDefaultRenderDevice() {
    if (pPlaybackDevice) {
        pPlaybackDevice->Release();
        pPlaybackDevice = nullptr;
    }

    HRESULT hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eCommunications, &pPlaybackDevice);
    if (SUCCEEDED(hr)) {
        std::cout << "Selected default render (output) device" << std::endl;
    } else {
        std::cerr << "Failed to get default render device. HRESULT: 0x" << std::hex << hr << std::dec << std::endl;
    }
}

void WasapiImpl::selectDefaultCaptureDevice() {
    if (pCaptureDevice) {
        pCaptureDevice->Release();
        pCaptureDevice = nullptr;
    }

    HRESULT hr = pEnumerator->GetDefaultAudioEndpoint(eCapture, eCommunications, &pCaptureDevice);
    if (SUCCEEDED(hr)) {
        std::cout << "Selected default capture (input) device" << std::endl;
    } else {
        std::cerr << "Failed to get default capture device. HRESULT: 0x" << std::hex << hr << std::dec << std::endl;
    }
}

void WasapiImpl::selectInitialAudioDevices() {
    // Get saved device IDs from config
    std::string inputDeviceId = config.getInputDevice();
    std::string outputDeviceId = config.getOutputDevice();

    // Select output device
    if (outputDeviceId.empty()) {
        // No saved device, use system default
        selectDefaultRenderDevice();
        std::cout << "Using system default output device" << std::endl;
    } else {
        // Try to use the saved device ID
        int wideLen = MultiByteToWideChar(CP_UTF8, 0, outputDeviceId.c_str(), -1, nullptr, 0);
        std::wstring wideDeviceId(wideLen - 1, 0);
        MultiByteToWideChar(CP_UTF8, 0, outputDeviceId.c_str(), -1, &wideDeviceId[0], wideLen);

        HRESULT hr = pEnumerator->GetDevice(wideDeviceId.c_str(), &pPlaybackDevice);
        if (SUCCEEDED(hr) && pPlaybackDevice) {
            // Check if the device is actually active
            DWORD state;
            hr = pPlaybackDevice->GetState(&state);
            if (SUCCEEDED(hr) && state == DEVICE_STATE_ACTIVE) {
                std::cout << "Restored saved output device: " << outputDeviceId << std::endl;
            } else {
                std::cerr << "Saved output device is not active (state: " << state << "), using default" << std::endl;
                pPlaybackDevice->Release();
                pPlaybackDevice = nullptr;
                selectDefaultRenderDevice();
            }
        } else {
            std::cerr << "Failed to restore saved output device, using default. HRESULT: 0x" << std::hex << hr << std::dec << std::endl;
            if (pPlaybackDevice) {
                pPlaybackDevice->Release();
                pPlaybackDevice = nullptr;
            }
            selectDefaultRenderDevice();
        }
    }

    // Select input device
    if (inputDeviceId.empty()) {
        // No saved device, use system default
        selectDefaultCaptureDevice();
        std::cout << "Using system default input device" << std::endl;
    } else {
        // Try to use the saved device ID
        int wideLen = MultiByteToWideChar(CP_UTF8, 0, inputDeviceId.c_str(), -1, nullptr, 0);
        std::wstring wideDeviceId(wideLen - 1, 0);
        MultiByteToWideChar(CP_UTF8, 0, inputDeviceId.c_str(), -1, &wideDeviceId[0], wideLen);

        HRESULT hr = pEnumerator->GetDevice(wideDeviceId.c_str(), &pCaptureDevice);
        if (SUCCEEDED(hr) && pCaptureDevice) {
            // Check if the device is actually active
            DWORD state;
            hr = pCaptureDevice->GetState(&state);
            if (SUCCEEDED(hr) && state == DEVICE_STATE_ACTIVE) {
                std::cout << "Restored saved input device: " << inputDeviceId << std::endl;
            } else {
                std::cerr << "Saved input device is not active (state: " << state << "), using default" << std::endl;
                pCaptureDevice->Release();
                pCaptureDevice = nullptr;
                selectDefaultCaptureDevice();
            }
        } else {
            std::cerr << "Failed to restore saved input device, using default. HRESULT: 0x" << std::hex << hr << std::dec << std::endl;
            if (pCaptureDevice) {
                pCaptureDevice->Release();
                pCaptureDevice = nullptr;
            }
            selectDefaultCaptureDevice();
        }
    }
}
