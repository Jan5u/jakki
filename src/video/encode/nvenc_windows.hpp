#pragma once

#include "encoder.hpp"

#include <print>

extern "C" {
#include <libavutil/frame.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
}

class Network;

class NvencWindowsEncoder : public D3D11Encoder {
public:
    NvencWindowsEncoder(Network* network, EncoderType type = EncoderType::NVENC_H264);
    ~NvencWindowsEncoder() override;
    void init() override;
    void flush() override;
    bool isReady() const override;
    std::string getName() const override;
    bool encodeD3D11Frame(AVFrame* frame) override;

private:
    Network* net = nullptr;
    EncoderType encoder_type;
    AVCodecContext* m_ctx = nullptr;
    AVBSFContext* m_h264_annexb_bsf = nullptr;
    bool m_ready = false;
    int64_t m_frame_count = 0;
    bool m_h264_sent_parameter_sets = false;
    std::vector<uint8_t> m_h264_annexb_extradata;
};
