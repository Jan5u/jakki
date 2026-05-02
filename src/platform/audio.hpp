#pragma once

#include <string>
#include <vector>

#include <SDL3/SDL_audio.h>

class Audio {
  public:
    int init();
    std::vector<std::string> getPlaybackDevices();
    std::vector<std::string> getCaptureDevices();

  private:
    SDL_AudioStream *playback_stream = nullptr;
    SDL_AudioStream *capture_stream = nullptr;
};