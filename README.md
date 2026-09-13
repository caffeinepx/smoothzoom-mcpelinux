# SmoothZoom for mcpelauncher-linux

Hold-to-zoom for Minecraft Bedrock on [mcpelauncher-linux](https://github.com/minecraft-linux/mcpelauncher-manifest), with eased FOV and cinematic letterbox bars.

Built and tested against **Minecraft 1.26.45.1** (`x86_64`).

## Install

1. Download `libzoom.so` from [Releases](https://github.com/caffeinepx/smoothzoom-mcpelinux/releases).
2. Copy it into your mcpelauncher mods folder:

```bash
cp libzoom.so ~/.local/share/mcpelauncher/mods/
```

3. Restart the game.

## Usage

| Action | What it does |
| --- | --- |
| Hold **C** | Zoom in (eased) |
| Scroll while holding | Zoom further in/out |
| Release **C** | Ease back to normal FOV |

Each new hold starts from the **default zoom**, not the last scroll depth.

Open the launcher overlay and use **Mods → Zoom** to:

- Rebind the zoom key
- Change default zoom, bar height, and ease speed
- Toggle black bars

Settings are also stored next to the mod as `zoom.conf`:

```
zoomKey=67
zoomFov=0.3000
barHeight=0.1600
smoothSpeed=10.0000
bars=1
```

`zoomKey` is a Java key code (`C` = 67).

## Build

Needs the [Android NDK](https://developer.android.com/ndk/downloads) (r21+, r30 used here) and CMake.

```bash
export ANDROID_NDK_HOME=/path/to/android-ndk-r30

cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=x86_64 \
  -DANDROID_PLATFORM=android-24 \
  -DANDROID_STL=c++_shared \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build -j$(nproc)
cp build/libzoom.so ~/.local/share/mcpelauncher/mods/
```

ABI must match your game (`x86_64` on most Linux installs). Do not build with host GCC/Clang.

## Notes

- Native `.so` loaded by mcpelauncher at startup. It hooks `CameraAPI::tryGetFOV`.
- Letterbox bars are drawn on the launcher swap-buffers callback.
- Logs use the `Zoom` tag.

## License

MIT
