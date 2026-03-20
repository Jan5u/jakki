#pragma once

enum class EncoderType;

class Capture {
public:
    virtual ~Capture() = default;
    virtual void selectScreen() = 0;
    virtual void startCapture() = 0;
    virtual void startEncoding(EncoderType encoderType) = 0;
};
