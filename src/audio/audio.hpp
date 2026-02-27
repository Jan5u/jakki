#pragma once

#include <QObject>
#include <memory>
#include <string>
#include <vector>
#include <cstdint> // For uint8_t

// Forward declarations
class Network;
class AudioImpl;
class Config;
struct AudioDevice;

/**
 * Audio - Public API for cross-platform audio handling
 * 
 * This class uses the PIMPL (Pointer to Implementation) pattern to hide
 * platform-specific implementation details. Depending on the platform,
 * it will internally use either PipeWire (Linux) or WASAPI (Windows).
 */
class Audio : public QObject {
    Q_OBJECT

public:
    // Constructor and destructor
    Audio(Network& network, Config& config, QObject* parent = nullptr);
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
    
    // Device enumeration
    std::vector<struct AudioDevice> getInputDevices() const;
    std::vector<struct AudioDevice> getOutputDevices() const;
    
    // Device selection
    void setInputDevice(const std::string& deviceId);
    void setOutputDevice(const std::string& deviceId);

    // Volume control
    void setVolume(bool isInput, float volume);
    float getVolume(bool isInput) const;

    // Voice gate control
    void setVoiceGateThreshold(float thresholdDb);
    float getVoiceGateThreshold() const;
    void setVoiceGateEnabled(bool enabled);
    bool isVoiceGateEnabled() const;

signals:
    void deviceListChanged();
    void defaultDeviceChanged(bool isInput);
    void volumeChanged(bool isInput, float volume);

private:
    // PIMPL pattern - pointer to implementation
    std::unique_ptr<AudioImpl> pImpl;
};
