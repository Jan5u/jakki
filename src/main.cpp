#include "platform/audio.hpp"
#include "platform/platform.cpp"
#include "platform/window.hpp"

int main() {
    Platform::init();

    Audio audio;
    audio.init();
    audio.startPlaybackStream();
    audio.startCaptureStream();

    Window window;
    window.init();

    Platform::shutdown();
    return 0;
}
