FROM debian:13.1-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
  build-essential \
  cmake \
  ninja-build \
  pkg-config \
  qt6-base-dev \
  qt6-base-dev-tools \
  libssl-dev \
  libnotify-dev \
  libpipewire-0.3-dev \
  libopus-dev \
  nlohmann-json3-dev \
  libavcodec-dev \
  libavformat-dev \
  g++ \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN cmake --preset gcc-release && cd build/release && ninja
