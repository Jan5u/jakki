#include "audio.hpp"

#include <stdio.h>

int Audio::init() {

    // const SDL_AudioSpec spec = {
    //     .format = SDL_AUDIO_F32,
    //     .channels = 2,
    //     .freq = 48000
    // };

    playback_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr, nullptr, nullptr);
    capture_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, nullptr, nullptr, nullptr);

    return 0;
}

void Audio::startPlaybackStream() {
    SDL_ResumeAudioStreamDevice(playback_stream);

    playback_thread = std::jthread([this] {
        while (true) {
            // SDL_PutAudioStreamData();
            // SDL_PutAudioStreamDataNoCopy();
        }
        SDL_DestroyAudioStream(playback_stream);
    });

    SDL_DestroyAudioStream(playback_stream);
}

void Audio::startCaptureStream() {
    SDL_ResumeAudioStreamDevice(capture_stream);

    capture_thread = std::jthread([this] {
        while (true) {
            // SDL_GetAudioStreamData();
        }
        SDL_DestroyAudioStream(capture_stream);
    });
}

using DeviceGetter = SDL_AudioDeviceID *(*)(int *);

static std::vector<std::string> getDevices(DeviceGetter getter, const char *errorMsg) {
    std::vector<std::string> devices;

    int count = 0;
    SDL_AudioDeviceID *ids = getter(&count);

    if (!ids) {
        printf("%s: %s\n", errorMsg, SDL_GetError());
        return devices;
    }

    for (int i = 0; i < count; ++i) {
        const char *name = SDL_GetAudioDeviceName(ids[i]);
        devices.emplace_back(name ? name : "Unknown device");
    }

    SDL_free(ids);
    return devices;
}

std::vector<std::string> Audio::getPlaybackDevices() { return getDevices(SDL_GetAudioPlaybackDevices, "SDL_GetAudioPlaybackDevices failed"); }

std::vector<std::string> Audio::getCaptureDevices() { return getDevices(SDL_GetAudioRecordingDevices, "SDL_GetAudioRecordingDevices failed"); }