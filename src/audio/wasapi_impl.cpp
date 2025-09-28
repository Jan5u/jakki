#include "wasapi_impl.hpp"
#include "../network.hpp"
#include <iostream>
#include <algorithm>

const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);
const IID IID_IAudioClient = __uuidof(IAudioClient);
const IID IID_IAudioRenderClient = __uuidof(IAudioRenderClient);
const IID IID_IAudioCaptureClient = __uuidof(IAudioCaptureClient);

WasapiImpl::WasapiImpl(Network& network)
    : AudioImpl(network) {
    std::cout << "WASAPI implementation created" << std::endl;
}

WasapiImpl::~WasapiImpl() {
    cleanup();
}

void WasapiImpl::initAudio() {
    initOpus();
    createInstance();
    enumerateAudioDevices();
    selectDefaultAudioDevices();
}

void WasapiImpl::startCapture() {
    audioCaptureThread = std::jthread([this] { captureLoop(); });
    startPlayback();
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
    // create event for playback
    HANDLE hRenderEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (hRenderEvent == NULL) {
        throw std::runtime_error("Failed to create event for playback");
    }

    pPlaybackDevice->Activate(IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&pRenderClient);
    pRenderClient->GetMixFormat(&pb_pwfx);

    WAVEFORMATEX* pModifiedFormat = nullptr;
    WAVEFORMATEXTENSIBLE modifiedFormat = {0};
    if (pb_pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto wfex = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(pb_pwfx);
        std::cout << "  EXTENSIBLE FORMAT DETAILS:" << std::endl;
        std::cout << "    cbSize: " << pb_pwfx->cbSize << std::endl;
        std::cout << "    wValidBitsPerSample: " << wfex->Samples.wValidBitsPerSample << std::endl;
        std::cout << "    nChannels: " << wfex->Format.nChannels << std::endl;
        std::cout << "    wBitsPerSample: " << wfex->Format.wBitsPerSample << std::endl;

        modifiedFormat = *wfex;
        modifiedFormat.Format.nChannels = 2;
        modifiedFormat.Format.nBlockAlign = (modifiedFormat.Format.nChannels * modifiedFormat.Format.wBitsPerSample) / 8;
        modifiedFormat.Format.nAvgBytesPerSec = modifiedFormat.Format.nSamplesPerSec * modifiedFormat.Format.nBlockAlign;
        modifiedFormat.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        modifiedFormat.dwChannelMask = KSAUDIO_SPEAKER_STEREO;

        pModifiedFormat = reinterpret_cast<WAVEFORMATEX*>(&modifiedFormat);
    }

    WAVEFORMATEX* pFormatToUse = nullptr;
    WAVEFORMATEX* pClosestMatch = nullptr;
    HRESULT hr = pRenderClient->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, pModifiedFormat, &pClosestMatch);
    if (hr == S_OK) {
        std::cout << "Modified format is supported exactly" << std::endl;
        pFormatToUse = pModifiedFormat;
    } else if (hr == S_FALSE) {
        std::cout << "Modified format is not supported, but closest match found" << std::endl;
        pFormatToUse = pClosestMatch;
        std::cout << "Closest Match Format:" << std::endl;
        std::cout << "  Channels: " << pClosestMatch->nChannels << std::endl;
        std::cout << "  Sample Rate: " << pClosestMatch->nSamplesPerSec << std::endl;
        std::cout << "  Bits Per Sample: " << pClosestMatch->wBitsPerSample << std::endl;
    } else {
        std::cout << "Modified format is not supported at all, using original" << std::endl;
        pFormatToUse = pb_pwfx;
    }

    std::cout << "pFormatToUse Match Format:" << std::endl;
    std::cout << "  Channels: " << pFormatToUse->nChannels << std::endl;
    std::cout << "  Sample Rate: " << pFormatToUse->nSamplesPerSec << std::endl;
    std::cout << "  Bits Per Sample: " << pFormatToUse->wBitsPerSample << std::endl;


    pRenderClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 200000, 0, pFormatToUse, NULL);

    pRenderClient->SetEventHandle(hRenderEvent);

    pRenderClient->GetBufferSize(&bufferFrameCount);
    pRenderClient->GetService(IID_IAudioRenderClient, (void**)&pRenderService);

    pRenderClient->Start();

    // Initialize playback buffer
    playbackBuffer.clear();
    
    while (flags != AUDCLNT_BUFFERFLAGS_SILENT) {
        // Wait for the next buffer event to be signaled.
        DWORD waitResult = WaitForSingleObject(hRenderEvent, INFINITE);
        if (waitResult != WAIT_OBJECT_0) {
            throw std::runtime_error("Failed to wait for render event");
        }
        
        pRenderClient->GetCurrentPadding(&numFramesPadding);
        numFramesAvailable = bufferFrameCount - numFramesPadding;
        
        // Get the buffer
        pRenderService->GetBuffer(numFramesAvailable, &pData);
        float* floatBuffer = reinterpret_cast<float*>(pData);
        size_t totalSamples = numFramesAvailable * pFormatToUse->nChannels;
        
        // If we need more audio data, mix some
        if (playbackBuffer.size() < totalSamples) {
            // Get a full Opus frame worth of data (or more if needed)
            size_t samplesNeeded = std::max(totalSamples, (size_t)OPUS_FRAME_SAMPLES);
            std::vector<float> newAudio = mixUserAudioBuffersFloat(samplesNeeded / pFormatToUse->nChannels);
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
        
        // Release the buffer
        pRenderService->ReleaseBuffer(numFramesAvailable, 0);
    }

    pRenderClient->Stop();

}

void WasapiImpl::captureLoop() {
    UINT32 nFrames;
    DWORD flags;
    BYTE* captureBuffer;
    UINT32 bufferFrameCount;
    
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

    pCaptureClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, hnsRequestedDuration, 0, pwfx, NULL);

    // set event handle
    pCaptureClient->SetEventHandle(hEvent);

    // Get the size of the allocated buffer.
    pCaptureClient->GetBufferSize(&bufferFrameCount);
    printf("bufferFrameCount: %d\n", bufferFrameCount);

    pCaptureClient->GetService(IID_IAudioCaptureClient, (void**)&pCaptureService);

    pCaptureClient->Start();  // Start recording.

    // capturing = true;
    while (true) {
        // wait for event
        DWORD waitResult = WaitForSingleObject(hEvent, INFINITE);
        if (waitResult != WAIT_OBJECT_0) {
            throw std::runtime_error("Failed to wait for capture event");
        }


        (pCaptureService->GetBuffer(&captureBuffer, &nFrames, &flags, NULL, NULL));
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

        (pCaptureService->ReleaseBuffer(nFrames));
    }
    
    pCaptureClient->Stop();
    CloseHandle(hEvent);
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
    pEnumerator->Release();
    CoUninitialize();

    // Cleanup Opus
    opusCleanup();
}

void WasapiImpl::createInstance() {
    HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void **)&pEnumerator);
}

void WasapiImpl::enumerateAudioDevices() {
    HRESULT hr = pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pEndpoints);
    UINT deviceCount = 0;
    hr = pEndpoints->GetCount(&deviceCount);
    for (UINT i = 0; i < deviceCount; i++) {
        IMMDevice *device = nullptr;
        hr = pEndpoints->Item(i, &device);
        printEndpointProperties(device);
    }

    hr = pEnumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &pEndpoints);
    deviceCount = 0;
    hr = pEndpoints->GetCount(&deviceCount);
    for (UINT i = 0; i < deviceCount; i++) {
        IMMDevice *device = nullptr;
        hr = pEndpoints->Item(i, &device);
        printEndpointProperties(device);
    }
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

void WasapiImpl::selectDefaultAudioDevices() {
    pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pPlaybackDevice);
    pEnumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &pCaptureDevice);
}
