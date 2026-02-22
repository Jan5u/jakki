<div align="center">
    <img height="128px" src="./ui/images/icon.svg" alt="" />
    <h1>Jakki</h1>
</div>

## BUILDING

### Building on Linux

#### Dependencies

- Qt 6
- OpenSSL
- PipeWire
- libdrm
- libopus
- libnotify
- QtKeychain
- FFmpeg
- XDG Desktop Portal
- CMake (make)
- Ninja (make)
- nv-codec-headers (make) 
- Vulkan-Headers (make)
- nlohmann-json (make)
- glslc/shaderc (make)


Debian 13 / Ubuntu 25

```bash
sudo apt install cmake ninja-build qt6-base-dev libssl-dev libnotify-dev libdrm-dev libpipewire-0.3-dev qtkeychain-qt6-dev libopus-dev nlohmann-json3-dev libavcodec-dev libavformat-dev glslc libffmpeg-nvenc-dev xdg-desktop-portal
```

Arch

```bash
sudo pacman -S --needed ninja qt6-base openssl libnotify libdrm libpipewire qtkeychain-qt6 opus ffmpeg ffnvcodec-headers xdg-desktop-portal nlohmann-json vulkan-headers shaderc
```

Fedora

```bash
sudo dnf in ninja qt6-qtbase-devel openssl-devel opus-devel libdrm-devel pipewire-devel qtkeychain-qt6-devel ffmpeg-free-devel libnotify-devel json-devel
```

Enter nix shell

```bash
nix-shell -p ninja qt6.full cmake pkg-config openssl.dev libopus.dev libdrm.dev pipewire.dev libsForQt6.qtkeychain ffmpeg.dev nlohmann_json pipewire
```

Run with env variable

```bash
XDG_RUNTIME_DIR=/run/user/$(id -u) ./build/jakki
```


#### Building

```bash
cmake --preset gcc-release
cd build/release
ninja
./jakki
```

### Building on Windows using vcpkg

#### Prerequisites

- Install [`Visual Studio`](https://visualstudio.microsoft.com/downloads) with `Desktop development with C++`
  - or install only the build tools without IDE [`Build Tools for Visual Studio`](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2026)

- Install [`Git`](https://github.com/git-for-windows/git/releases/latest)

<br>

> [!TIP]
> Alternatively you can install these using WinGet

<br>

```bash
winget install --id Microsoft.VisualStudio.Community --override "--quiet --add Microsoft.VisualStudio.Workload.NativeDesktop --includeRecommended --wait"
```

```bash
winget install --id Microsoft.VisualStudio.BuildTools --override "--quiet --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --wait"
```

```bash
winget install --id Git.Git
```

- Install `vcpkg`
```bash
git clone https://github.com/microsoft/vcpkg.git; cd vcpkg; .\bootstrap-vcpkg.bat -disableMetrics
```

#### Building
1. Open `x64 Native Tools Command Prompt for VS`
2. Create CMakeFiles using preset
```bash
cmake --preset vcpkg-release
```
3. Navigate to build folder
```bash
cd build\release
```
4. Build with ninja
```bash
ninja
.\jakki.exe
```