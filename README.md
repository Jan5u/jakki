# jakki

## BUILDING

### Building on Linux

#### Dependencies

Debian 13 / Ubuntu 25

```bash
sudo apt install ninja-build qt6-base-dev libssl-dev libnotify-dev libpipewire-0.3-dev libopus-dev nlohmann-json3-dev libavcodec-dev libavformat-dev
```

Arch

```bash
sudo pacman -S --needed ninja qt6-base openssl libnotify libpipewire opus ffmpeg nlohmann-json
```

Fedora

```bash
sudo dnf in ninja qt6-qtbase-devel openssl-devel opus-devel pipewire-devel ffmpeg-free-devel libnotify-devel json-devel
```

Enter nix shell

```bash
nix-shell -p ninja qt6.full cmake pkg-config openssl.dev libopus.dev pipewire.dev ffmpeg.dev nlohmann_json pipewire
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

- Install [`Visual Studio`](https://visualstudio.microsoft.com/) with `Desktop development with C++`
- Install [`Git`](https://github.com/git-for-windows/git/releases/latest)

<br>

> [!TIP]
> Alternatively you can install these using WinGet

<br>

```bash
winget install --id Microsoft.VisualStudio.2022.Community --override "--quiet --add Microsoft.VisualStudio.Workload.NativeDesktop --includeRecommended"
```
```bash
winget install --id Git.Git
```


- Install `vcpkg`
```bash
git clone https://github.com/microsoft/vcpkg.git; cd vcpkg; .\bootstrap-vcpkg.bat -disableMetrics
```

#### Building
1. Open `x64 Native Tools Command Prompt for VS 2022`
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