# jakki

## COMPILATION

### DEPENDENCIES

#### Debian

```bash
sudo apt install meson ninja-build qt6-base-dev libssl-dev libnotify-dev libpipewire-0.3-dev libopus-dev nlohmann-json3-dev libavcodec-dev libavformat-dev
```

#### Arch

```bash
sudo pacman -S --needed meson ninja qt6-base openssl libnotify libpipewire opus ffmpeg
```

#### Fedora

```bash
sudo dnf in meson ninja qt6-qtbase-devel openssl-devel opus-devel pipewire-devel ffmpeg-free-devel libnotify-devel
```

#### Nixos

Enter nix shell

```bash
nix-shell -p meson ninja qt6.full cmake pkg-config openssl.dev libopus.dev pipewire.dev ffmpeg.dev nlohmann_json pipewire
```

Run with env variable

```bash
XDG_RUNTIME_DIR=/run/user/$(id -u) ./build/jakki
```

### BUILDING

```bash
mkdir build
cd build
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release ..
ninja
./jakki
```
