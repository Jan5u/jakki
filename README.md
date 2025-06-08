# jakki

## COMPILATION

### DEPENDENCIES

```bash
sudo apt install meson ninja-build qt6-base-dev libssl-dev libnotify4 libpipewire-0.3-dev libopus-dev libavcodec-dev libavformat-dev
```
```bash
sudo pacman -S --needed meson ninja qt6-base openssl libnotify libpipewire opus ffmpeg
```
```bash
sudo dnf in meson ninja qt6-qtbase-devel openssl-devel opus-devel pipewire-devel ffmpeg-free-devel libnotify-devel
```

### BUILDING

```bash
meson setup build
meson compile -C build
# meson install -C build
```