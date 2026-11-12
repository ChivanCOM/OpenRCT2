# OpenRCT2 on 32-bit PowerPC

This branch is OpenRCT2 v0.5.7 with patches that make it run on big-endian 32-bit PowerPC Linux. It was developed on a slot-loading iMac G3 (500 MHz PowerPC 750, 512 MB RAM), where it runs at 800×600 in 256 colours.

There are no binaries here, only source. You still need the original RollerCoaster Tycoon 2 data files (Steam or GOG), just like on any other platform.

## What the patches do

| Commit | What |
|---|---|
| Support big-endian hosts | Byte-swapping for RCT1/RCT2 files, park files, sprites and caches, plus fixes for code that assumed little-endian memory |
| Big-endian hosts: keep narrow union members in the low bits | Fixes empty button captions, wrong ride reliability and similar union aliasing on big-endian |
| Money arguments as money64 | "Not enough cash" no longer asks for billions on big-endian; slider settings are saved once when the options window closes |
| Insert new windows by index | Fixes a crash when a window opens another window in `onOpen()` |
| Entity spatial index: take the dirty list before updating | Fixes iterating over a list that can be cleared during the loop |
| Display engine: software renderer fallback, 8-bit direct output | Runs on plain framebuffer video drivers without accelerated rendering |
| Display engine: draw straight into an 8-bit window surface | Saves a full-screen copy per frame when the screen is 8-bit |
| Performance on slow 32-bit CPUs | `DISABLE_PIC` build option, faster guest path finding, smaller paint margins |
| Fewer cache misses in the game update and paint setup | Less memory traffic per tick, found with a simulation of the G3's caches |
| Paint: draw each column right after setting it up | Better cache use when painting single-threaded; faster short sprite copies |
| Paint: terrain edge masks four pixels at a time | Faster smoothed terrain edges; cheaper profiling check |

Some of these have been submitted upstream: [#27406](https://github.com/OpenRCT2/OpenRCT2/pull/27406), [#27407](https://github.com/OpenRCT2/OpenRCT2/pull/27407), [#27408](https://github.com/OpenRCT2/OpenRCT2/pull/27408), [#27409](https://github.com/OpenRCT2/OpenRCT2/pull/27409), [#27410](https://github.com/OpenRCT2/OpenRCT2/pull/27410).

## What has been tested

- iMac G3 (PowerMac2,2), Linux 6.18, musl libc, GCC 14.3, SDL 2.32 with a framebuffer video driver, no X11. Cross-built with Buildroot.
- The same source builds and runs on x86-64.

Not tested: desktop distributions with X11 or Wayland, glibc, G4/G5 machines, 64-bit PowerPC. It should work there, since nothing in the patches is specific to the iMac, but I haven't tried it. Reports are welcome.

## Building on a PowerPC machine

Install the dependencies listed in OpenRCT2's [Linux build instructions](https://github.com/OpenRCT2/OpenRCT2/wiki/Building-OpenRCT2-on-Linux) (SDL2, libpng, zlib, zstd, libzip, ICU, FLAC, Vorbis, nlohmann-json), then:

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DDISABLE_NETWORK=ON -DDISABLE_HTTP=ON -DDISABLE_DISCORD_RPC=ON -DDISABLE_TTF=ON \
    -DDISABLE_OPENGL=ON -DENABLE_SCRIPTING=OFF -DDISABLE_PIC=ON \
    -DCMAKE_EXE_LINKER_FLAGS="-latomic -no-pie" -DCMAKE_SHARED_LINKER_FLAGS=-latomic
cmake --build build
cmake --install build --prefix ~/openrct2
```

- `-latomic` is needed because 32-bit PowerPC has no native 64-bit atomics.
- `-DDISABLE_PIC=ON` builds one non-PIE executable. Position-independent code is noticeably slower on 32-bit PowerPC.
- Network, HTTP, TrueType fonts and scripting are switched off because that is the configuration I tested; they are not known to be broken.
- The build downloads objects, title sequences and the open sound/music packs. Add `-DDOWNLOAD_OBJECTS=OFF` and friends if the machine has no internet, and copy the `data/` folder of the official Linux release of the same version instead.

Point OpenRCT2 at your RCT2 files on first start, or set `game_path` in `~/.config/OpenRCT2/config.ini`.

## Cross-building

A cross build cannot run the game to generate `g2.dat`, `fonts.dat`, `palettes.dat` and `tracks.dat`. Take them from the `data/` folder of the official x86-64 Linux release of v0.5.7; they are the same on every platform.

## On a slow machine

At 500 MHz, keep parks small to medium, turn off multithreading in the options, and run at 800×600 or less. Most of the time per frame goes to guest path finding and painting.

## License

GPL-3.0, like OpenRCT2. See `licence.txt`.
