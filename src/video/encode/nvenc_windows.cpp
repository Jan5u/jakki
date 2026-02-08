#include "nvenc_windows.hpp"

NvencWindowsEncoder::NvencWindowsEncoder(Network* network, EncoderType type) : net(network), encoder_type(type) {}

NvencWindowsEncoder::~NvencWindowsEncoder() {}

void NvencWindowsEncoder::init() {}

void NvencWindowsEncoder::flush() {}

bool NvencWindowsEncoder::isReady() const {
    return false;
}

std::string NvencWindowsEncoder::getName() const {
    return Encoder::encoderTypeToName(encoder_type);
}

bool NvencWindowsEncoder::encodeD3D11Frame(void* d3d11_texture) {
    return false;
}
