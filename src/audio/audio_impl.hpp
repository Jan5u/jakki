#pragma once

#include <opus/opus.h>
#include <queue>
#include <string>
#include <thread>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <functional>

#define DEFAULT_RATE 48000
#define DEFAULT_CHANNELS 2
#define MIC_CHANNELS 1
#define FRAME_SIZE 960

// Forward declaration for Network class
class Network;

struct AudioDevice {
    std::string id;
    std::string name;
    bool isInput;
    uint32_t nodeId;
};

/**
 * AudioImpl - Abstract base class for platform-specific audio implementations
 * This serves as the interface that all platform-specific implementations must follow
 */
class AudioImpl {
public:
    // Constructor and destructor
    explicit AudioImpl(Network& network) : networkManager(network) {}
    virtual ~AudioImpl() = default;

    // Core audio lifecycle methods that each implementation must provide
    virtual void initAudio() = 0;
    virtual void startCapture() = 0;
    virtual void stopCapture() = 0;
    virtual void cleanup() = 0;

    // Handle incoming voice packets (common implementation can be shared)
    virtual void handleIncomingVoicePacket(const std::string& userId, const std::vector<uint8_t>& payload);
    
    // Device enumeration - must be implemented by platform-specific classes
    virtual std::vector<AudioDevice> getInputDevices() const = 0;
    virtual std::vector<AudioDevice> getOutputDevices() const = 0;
    
    // Device selection - must be implemented by platform-specific classes
    virtual void setInputDevice(const std::string& deviceId) = 0;
    virtual void setOutputDevice(const std::string& deviceId) = 0;

    // Volume control - must be implemented by platform-specific classes
    virtual void setVolume(bool isInput, float volume) = 0;
    virtual float getVolume(bool isInput) const = 0;

    // Set callback for device list changes
    void setDeviceChangeCallback(std::function<void()> callback) {
        deviceChangeCallback = callback;
    }

    // Set callback for default device changes
    void setDefaultDeviceChangeCallback(std::function<void(bool)> callback) {
        defaultDeviceChangeCallback = callback;
    }

    // Set callback for volume changes
    void setVolumeChangeCallback(std::function<void(bool, float)> callback) {
        volumeChangeCallback = callback;
    }

    // Helper struct for user audio streams
    struct UserStream {
        OpusDecoder* decoder{nullptr};
        std::queue<std::vector<float>> floatBuffers;
    };

protected:
    // Notify about device changes
    void notifyDeviceListChanged() {
        if (deviceChangeCallback) {
            deviceChangeCallback();
        }
    }

    // Notify about default device changes
    void notifyDefaultDeviceChanged(bool isInput) {
        if (defaultDeviceChangeCallback) {
            defaultDeviceChangeCallback(isInput);
        }
    }

    // Notify about volume changes
    void notifyVolumeChanged(bool isInput, float volume) {
        if (volumeChangeCallback) {
            volumeChangeCallback(isInput, volume);
        }
    }

    // Opus codec methods
    void initOpus();
    void opusCleanup();
    void encodePacketWithOpusFloat(const float* floatSamples, size_t numSamples);
    std::vector<float> decodeOpusPacketFloat(const std::string& userId, const std::vector<uint8_t>& payload);
    std::vector<float> mixUserAudioBuffersFloat(int n_frames);

    // Common data members
    Network& networkManager;
    std::unordered_map<std::string, UserStream> userStreams; // per-user decoder + queued frames
    std::mutex bufferMutex;
    static const size_t MAX_BUFFER_SIZE = 5; // Max 5 packets per user

    // Opus encoder
    OpusEncoder* encoder{nullptr};

    // Device change callback
    std::function<void()> deviceChangeCallback;
    
    // Default device change callback
    std::function<void(bool)> defaultDeviceChangeCallback;
    
    // Volume change callback
    std::function<void(bool, float)> volumeChangeCallback;
};