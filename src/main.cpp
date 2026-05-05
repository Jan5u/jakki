#include "platform/audio.hpp"
#include "platform/platform.cpp"
#include "platform/window.hpp"
#include "core/network.hpp"

#include <thread>

int main() {
    Platform::init();

    Network network;
    Audio audio;
    Window window;
    
    std::jthread network_thread([&network]() {
        if (!network.init()) {
            return;
        }

        const uint8_t payload[] = {'h', 'e', 'y'};
        network.sendDatagram(payload, sizeof(payload));
    });

    std::jthread audio_thread([&audio]() { audio.init(); });

    window.init();

    network.shutdown();
    Platform::shutdown();
    return 0;
}
