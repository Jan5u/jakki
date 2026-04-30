#include <thread>
#include <vector>
#include <string>

#include <SDL3/SDL_audio.h>

class Audio {
  public:
    int init();
    void startPlaybackStream();
    void startCaptureStream();
    std::vector<std::string> getPlaybackDevices();
    std::vector<std::string> getCaptureDevices();

  private:
    SDL_AudioStream *playback_stream = nullptr;
    SDL_AudioStream *capture_stream = nullptr;
    std::jthread playback_thread;
    std::jthread capture_thread;
};