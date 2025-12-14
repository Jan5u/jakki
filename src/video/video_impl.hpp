#pragma once

class VideoImpl {
  public:
    virtual ~VideoImpl() = default;
    virtual void selectScreen() = 0;
};