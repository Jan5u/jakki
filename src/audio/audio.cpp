#include "audio.hpp"
#include "audio_impl.hpp"
#include "../config.hpp"
#include <iostream>

// Include platform-specific implementations
#ifdef _WIN32
#include "wasapi_impl.hpp"
#else
#include "pipewire_impl.hpp"
#endif

Audio::Audio(Network& network, Config& config, QObject* parent)
    : QObject(parent) {
    // Create the platform-specific implementation
#ifdef _WIN32
    pImpl = std::make_unique<WasapiImpl>(network, config);
    std::cout << "Created WASAPI audio implementation" << std::endl;
#else
    pImpl = std::make_unique<PipewireImpl>(network, config);
    std::cout << "Created PipeWire audio implementation" << std::endl;
#endif

    // Set up callback for device changes
    pImpl->setDeviceChangeCallback([this]() {
        emit deviceListChanged();
    });

    // Set up callback for default device changes
    pImpl->setDefaultDeviceChangeCallback([this](bool isInput) {
        emit defaultDeviceChanged(isInput);
    });

    // Set up callback for volume changes
    pImpl->setVolumeChangeCallback([this](bool isInput, float volume) {
        emit volumeChanged(isInput, volume);
    });

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

std::vector<AudioDevice> Audio::getInputDevices() const {
    return pImpl->getInputDevices();
}

std::vector<AudioDevice> Audio::getOutputDevices() const {
    return pImpl->getOutputDevices();
}

void Audio::setInputDevice(const std::string& deviceId) {
    pImpl->setInputDevice(deviceId);
}

void Audio::setOutputDevice(const std::string& deviceId) {
    pImpl->setOutputDevice(deviceId);
}

void Audio::setVolume(bool isInput, float volume) {
    pImpl->setVolume(isInput, volume);
}

float Audio::getVolume(bool isInput) const {
    return pImpl->getVolume(isInput);
}

void Audio::setVoiceGateThreshold(float thresholdDb) {
    pImpl->setVoiceGateThreshold(thresholdDb);
}

float Audio::getVoiceGateThreshold() const {
    return pImpl->getVoiceGateThreshold();
}

void Audio::setVoiceGateEnabled(bool enabled) {
    pImpl->setVoiceGateEnabled(enabled);
}

bool Audio::isVoiceGateEnabled() const {
    return pImpl->isVoiceGateEnabled();
}