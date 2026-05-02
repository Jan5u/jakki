#include "platform/audio.hpp"
#include "platform/platform.cpp"
#include "platform/window.hpp"
#include "core/network.hpp"

int main() {
    Platform::init();

    Network network;
    network.init();

    const uint8_t payload[] = {'h','e','y'};
    network.sendDatagram(payload, sizeof(payload));

    Audio audio;
    audio.init();

    Window window;
    window.init();

    network.shutdown();
    Platform::shutdown();
    return 0;
}
