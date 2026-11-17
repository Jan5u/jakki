FROM debian:forky
RUN apt update && apt install -y --no-install-recommends \
    build-essential git make ca-certificates \
    cmake meson ninja-build nasm pkg-config \
    gnome-desktop-testing libasound2-dev libpulse-dev \
    libaudio-dev libfribidi-dev libjack-dev libsndio-dev libx11-dev libxext-dev \
    libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev \
    libxkbcommon-dev libdrm-dev libgbm-dev libgl1-mesa-dev libgles2-mesa-dev \
    libegl1-mesa-dev libdbus-1-dev libibus-1.0-dev libudev-dev libthai-dev libusb-1.0-0-dev \
    libpipewire-0.3-dev libwayland-dev libdecor-0-dev liburing-dev \
    nlohmann-json3-dev libsecret-1-dev libzstd-dev glslc libvulkan-dev libssl-dev \
    g++-mingw-w64-x86-64 wget patchelf file nsis
RUN wget https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage -O /usr/local/bin/appimagetool && chmod +x /usr/local/bin/appimagetool
ENV APPIMAGE_EXTRACT_AND_RUN=1