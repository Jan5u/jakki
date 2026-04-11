#include "audio_impl.hpp"
#include "../network.hpp"
#include <iostream>
#include <cstring>
#include <cmath>
#include <algorithm>

// --- Voice gate ---

float AudioImpl::calculateRmsDb(const float* samples, size_t numSamples) {
    if (!samples || numSamples == 0) return -100.0f;

    double sum = 0.0;
    for (size_t i = 0; i < numSamples; ++i) {
        sum += static_cast<double>(samples[i]) * samples[i];
    }
    double rms = std::sqrt(sum / numSamples);
    if (rms < 1e-10) return -100.0f;
    return static_cast<float>(20.0 * std::log10(rms));
}

bool AudioImpl::isAboveVoiceGate(const float* samples, size_t numSamples) const {
    if (!voiceGateEnabled.load(std::memory_order_relaxed)) return true;
    float db = calculateRmsDb(samples, numSamples);
    return db >= voiceGateThresholdDb.load(std::memory_order_relaxed);
}

void AudioImpl::setVoiceGateThreshold(float thresholdDb) {
    voiceGateThresholdDb.store(std::clamp(thresholdDb, -60.0f, 0.0f), std::memory_order_relaxed);
}

float AudioImpl::getVoiceGateThreshold() const {
    return voiceGateThresholdDb.load(std::memory_order_relaxed);
}

void AudioImpl::setVoiceGateEnabled(bool enabled) {
    voiceGateEnabled.store(enabled, std::memory_order_relaxed);
}

bool AudioImpl::isVoiceGateEnabled() const {
    return voiceGateEnabled.load(std::memory_order_relaxed);
}

void AudioImpl::initOpus() {
    std::cout << "Initializing Opus encoder\n";

    int error;

    // encoder
    encoder = opus_encoder_create(DEFAULT_RATE, DEFAULT_CHANNELS, OPUS_APPLICATION_VOIP, &error);
     // encoder = opus_encoder_create(DEFAULT_RATE, DEFAULT_CHANNELS, OPUS_APPLICATION_AUDIO, &error);
    if (error != OPUS_OK) {
        std::cerr << "Failed to create Opus encoder: " << opus_strerror(error) << "\n";
        return;
    }
    
    // Configure encoder
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(64000));
    opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(10));
    opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
    // opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(encoder, OPUS_SET_DTX(0));
}

void AudioImpl::opusCleanup() {
    if(encoder) {
        opus_encoder_destroy(encoder);
        encoder = nullptr;
    }
    
    std::lock_guard<std::mutex> lock(bufferMutex);
    for(auto &p : userStreams) {
        if(p.second.decoder) {
            opus_decoder_destroy(p.second.decoder);
            p.second.decoder = nullptr;
        }
    }
}

void AudioImpl::encodePacketWithOpusFloat(const float* floatSamples, size_t numSamples) {
    if (!encoder) {
        std::cerr << "Encoder not initialized\n";
        return;
    }
    
    // Calculate frame size in samples (per channel)
    int frameSize = numSamples / DEFAULT_CHANNELS;
    if (frameSize != FRAME_SIZE) {
        std::cerr << "Invalid frame size: expected " << FRAME_SIZE << " samples per channel, got " << frameSize << "\n";
        return;
    }
    
    int maxBytes = 4000; // Maximum packet size for Opus
    std::vector<uint8_t> encodedData(maxBytes);
    
    int encodedBytes = opus_encode_float(encoder, floatSamples, FRAME_SIZE, encodedData.data(), maxBytes);
    if (encodedBytes < 0) {
        std::cerr << "Failed to encode float audio: " << opus_strerror(encodedBytes) << "\n";
        return;
    }
    
    std::cout << "Encoded " << encodedBytes << " bytes from float audio\n";
    encodedData.resize(encodedBytes);

    // Send to server
    networkManager.sendVoicePackets(encodedData);
}

std::vector<float> AudioImpl::decodeOpusPacketFloat(const std::string& userId, const std::vector<uint8_t>& payload) {
    std::lock_guard<std::mutex> lock(bufferMutex);
    auto &stream = userStreams[userId];
    if(!stream.decoder) {
        int err; 
        stream.decoder = opus_decoder_create(DEFAULT_RATE, DEFAULT_CHANNELS, &err);
        if(err != OPUS_OK) {
            std::cerr << "Failed create decoder for user " << userId << " err=" << opus_strerror(err) << "\n";
            return {};
        }
    }
    std::vector<float> decoded(FRAME_SIZE * DEFAULT_CHANNELS);
    int samples = opus_decode_float(stream.decoder, payload.data(), payload.size(), decoded.data(), FRAME_SIZE, 0);
    if(samples < 0) {
        std::cerr << "Decode fail user=" << userId << " err=" << opus_strerror(samples) << "\n";
        return {};
    }
    std::cout << "Decoded (float) " << payload.size() << " bytes to " << samples << " samples for user " << userId << "\n";
    decoded.resize(samples * DEFAULT_CHANNELS);
    return decoded;
}

std::vector<float> AudioImpl::mixUserAudioBuffersFloat(int n_frames) {
    std::lock_guard<std::mutex> lock(bufferMutex);
    int samples_needed = n_frames * DEFAULT_CHANNELS;
    std::vector<float> mix(samples_needed, 0.0f);
    size_t contributors = 0;

    for (auto &kv : userStreams) {
        auto &q = kv.second.floatBuffers;
        if (q.empty()) continue;

        auto packet = std::move(q.front());
        q.pop();

        if (packet.empty()) continue;
        contributors++;

        size_t copy = std::min((int)packet.size(), samples_needed);
        for (size_t i = 0; i < copy; i++) {
            mix[i] += packet[i];
        }
    }

    if (contributors == 0) return mix;

    // Average if more than one contributor
    if (contributors > 1) {
        for (int i = 0; i < samples_needed; i++) {
            mix[i] /= static_cast<float>(contributors);
        }
    }

    return mix;
}

void AudioImpl::handleIncomingVoicePacket(const std::string& userId, const std::vector<uint8_t>& payload) {
    auto decoded = decodeOpusPacketFloat(userId, payload);
    if(!decoded.empty()) {
        std::lock_guard<std::mutex> lock(bufferMutex);
        auto &stream = userStreams[userId];
        stream.floatBuffers.push(std::move(decoded));
        while(stream.floatBuffers.size() > MAX_BUFFER_SIZE) {
            stream.floatBuffers.pop();
        }
    }
}
