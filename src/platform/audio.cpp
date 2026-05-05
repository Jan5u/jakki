#include "audio.hpp"

#include <stdio.h>

static void SDLCALL playbackCallback(void *userdata, SDL_AudioStream *astream, int additional_amount, int total_amount) {
    additional_amount /= sizeof(float);
    while (additional_amount > 0) {
        float samples[128];
        const int total = SDL_min(additional_amount, SDL_arraysize(samples));
        SDL_memset(samples, 0, static_cast<size_t>(total) * sizeof(float));
        SDL_PutAudioStreamData(astream, samples, total * sizeof(float));
        additional_amount -= total;
    }
}

static void SDLCALL captureCallback(void *userdata, SDL_AudioStream *astream, int additional_amount, int total_amount) {
    int remaining = additional_amount;
    while (remaining > 0) {
        uint8_t buffer[256];
        const int to_read = SDL_min(remaining, static_cast<int>(sizeof(buffer)));
        const int read_bytes = SDL_GetAudioStreamData(astream, buffer, to_read);
        if (read_bytes <= 0) {
            break;
        }
        remaining -= read_bytes;
    }
}

int Audio::init() {

    // const SDL_AudioSpec spec = {
    //     .format = SDL_AUDIO_F32,
    //     .channels = 2,
    //     .freq = 48000
    // };

    playback_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr, playbackCallback, nullptr);
    capture_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, nullptr, captureCallback, nullptr);
    SDL_ResumeAudioStreamDevice(playback_stream);
    SDL_ResumeAudioStreamDevice(capture_stream);

    return 0;
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