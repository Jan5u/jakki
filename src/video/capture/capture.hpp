#pragma once

class Capture {
public:
    virtual ~Capture() = default;
    virtual void selectScreen() = 0;
};
