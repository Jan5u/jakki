#pragma once

#include <memory>
#include <string>
#include <vector>
#include <cstdint> // For uint8_t

// Forward declarations
class Network;
class AudioImpl;

/**
 * Audio - Public API for cross-platform audio handling
 * 
 * This class uses the PIMPL (Pointer to Implementation) pattern to hide
 * platform-specific implementation details. Depending on the platform,
 * it will internally use either PipeWire (Linux) or WASAPI (Windows).
 */
class Audio {
public:
    // Constructor and destructor
    Audio(Network& network);
    ~Audio();
    
    // Delete copy and move operations to ensure proper resource management
    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;
    Audio(Audio&&) = delete;
    Audio& operator=(Audio&&) = delete;

    // Audio control functions
    void startAudioThread();
    void stopAudio();
    
    // Network audio functions
    void handleIncomingVoicePacket(const std::string& userId, const std::vector<uint8_t>& payload);

private:
    // PIMPL pattern - pointer to implementation
    std::unique_ptr<AudioImpl> pImpl;
};
