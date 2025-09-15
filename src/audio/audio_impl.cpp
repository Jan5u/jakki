#include "audio_impl.hpp"
#include "../network.hpp"
#include <iostream>
#include <cstring>

void AudioImpl::initOpus() {
    std::cout << "Initializing Opus encoder\n";

    int error;

    // encoder
    encoder = opus_encoder_create(DEFAULT_RATE, DEFAULT_CHANNELS, OPUS_APPLICATION_VOIP, &error);
    if (error != OPUS_OK) {
        std::cerr << "Failed to create Opus encoder: " << opus_strerror(error) << "\n";
        return;
    }
    
    // Configure encoder
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(64000));
    opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(10));
    opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
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

void AudioImpl::encodePacketWithOpus(const std::vector<int16_t>& audio) {
    if (!encoder) {
        std::cerr << "Encoder not initialized\n";
        return;
    }
    
    int maxSamples = FRAME_SIZE * DEFAULT_CHANNELS;
    std::vector<uint8_t> encodedData(maxSamples);
    
    int encodedBytes = opus_encode(encoder, audio.data(), FRAME_SIZE, encodedData.data(), maxSamples);
    if (encodedBytes < 0) {
        std::cerr << "Failed to encode audio: " << opus_strerror(encodedBytes) << "\n";
        return;
    }
    
    std::cout << "Encoded " << encodedBytes << " bytes\n";
    encodedData.resize(encodedBytes);

    // Send to server
    networkManager.sendVoicePackets(encodedData);
}

std::vector<int16_t> AudioImpl::decodeOpusPacket(const std::string& userId, const std::vector<uint8_t>& payload) {
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
    
    std::vector<int16_t> decoded(FRAME_SIZE * DEFAULT_CHANNELS);
    int samples = opus_decode(stream.decoder, payload.data(), payload.size(), decoded.data(), FRAME_SIZE, 0);
    if(samples < 0) {
        std::cerr << "Decode fail user=" << userId << " err=" << opus_strerror(samples) << "\n";
        return {};
    }
    
    decoded.resize(samples * DEFAULT_CHANNELS);
    return decoded;
}

void AudioImpl::handleIncomingVoicePacket(const std::string& userId, const std::vector<uint8_t>& payload) {
    auto decoded = decodeOpusPacket(userId, payload);
    if(!decoded.empty()) {
        addUserAudio(userId, std::move(decoded));
    }
}

void AudioImpl::addUserAudio(const std::string& userId, std::vector<int16_t>&& audioData) {
    std::lock_guard<std::mutex> lock(bufferMutex);
    auto &stream = userStreams[userId];
    stream.buffers.push(std::move(audioData));
    while(stream.buffers.size() > MAX_BUFFER_SIZE) {
        stream.buffers.pop();
    }
}

std::vector<int16_t> AudioImpl::mixUserAudioBuffers(int n_frames) {
    std::lock_guard<std::mutex> lock(bufferMutex);
    int samples_needed = n_frames * DEFAULT_CHANNELS;
    std::vector<int32_t> mix(samples_needed, 0);
    size_t contributors = 0;
    
    for(auto &kv : userStreams) {
        auto &q = kv.second.buffers;
        if(q.empty()) continue;
        
        auto packet = std::move(q.front());
        q.pop();
        
        if(packet.empty()) continue;
        contributors++;
        
        size_t copy = std::min((int)packet.size(), samples_needed);
        for(size_t i = 0; i < copy; i++) {
            mix[i] += packet[i];
        }
    }
    
    std::vector<int16_t> out(samples_needed, 0);
    if(contributors == 0) return out;
    
    for(int i = 0; i < samples_needed; i++) {
        int32_t v = contributors > 1 ? mix[i] / (int32_t)contributors : mix[i];
        if(v > INT16_MAX) v = INT16_MAX;
        else if(v < INT16_MIN) v = INT16_MIN;
        out[i] = (int16_t)v;
    }
    
    return out;
}