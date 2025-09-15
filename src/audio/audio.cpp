#include "audio.hpp"
#include "audio_impl.hpp"
#include <iostream>

// Include platform-specific implementations
#ifdef _WIN32
#include "wasapi_impl.hpp"
#else
#include "pipewire_impl.hpp"
#endif

Audio::Audio(Network& network) {
    // Create the platform-specific implementation
#ifdef _WIN32
    pImpl = std::make_unique<WasapiImpl>(network);
    std::cout << "Created WASAPI audio implementation" << std::endl;
#else
    pImpl = std::make_unique<PipewireImpl>(network);
    std::cout << "Created PipeWire audio implementation" << std::endl;
#endif

    // Initialize the audio system
    pImpl->initAudio();
}

Audio::~Audio() {
    // pImpl's destructor will be called automatically
}

void Audio::startAudioThread() {
    pImpl->startCapture();
}

void Audio::stopAudio() {
    pImpl->stopCapture();
}

void Audio::handleIncomingVoicePacket(const std::string& userId, const std::vector<uint8_t>& payload) {
    pImpl->handleIncomingVoicePacket(userId, payload);
}