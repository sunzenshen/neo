# Steam Audio (headers only)

- Version: **4.8.1**
- Release: https://github.com/ValveSoftware/steam-audio/releases/download/v4.8.1/steamaudio_4.8.1.zip
- SHA256: `4a0aa5ec1176f38f0b0993a37c2259d9e86f27e22d5e24f83ec4c3cb9a1d5449`
- License: Apache-2.0 (see `LICENSE`)

## What is vendored

Only the public C API headers, copied verbatim from the release zip's `include/` directory:
`phonon.h`, `phonon_interfaces.h` and `phonon_version.h`.

The client never links against Steam Audio. `neo_spatializer_steamaudio.cpp` loads the runtime
library (`libphonon.so` / `phonon.dll`) with `dlopen` / `LoadLibrary` and resolves the few
functions it uses by name, so a missing or incompatible library only disables the HRTF backend.

## Where the runtime library comes from

`src/cmake/steamaudio.cmake` (enabled by `NEO_STEAMAUDIO`, on by default for client builds)
downloads the release zip above with `FetchContent`, verifies the SHA256, and the
`steamaudio_copy_lib` target copies the platform library into `game/neo/bin/<plat>`
(`linux64` or `x64`) next to `client.so` / `client.dll`. `copy_all_libs` depends on it, so a
normal build puts it in place.

To use a locally built or already unpacked SDK instead of downloading, point
`NEO_STEAMAUDIO_SDK_PATH` at its root (the directory containing `include/` and `lib/`):

```
cmake --preset linux-debug -DNEO_STEAMAUDIO_SDK_PATH=/path/to/steamaudio
```

## Upgrading

1. Bump the URL and `URL_HASH` in `src/cmake/steamaudio.cmake`.
2. Copy the three headers from the new release's `include/` over the ones here.
3. Update the version, URL and hash in this file.
4. Rebuild; the context is created with `STEAMAUDIO_VERSION`, so a header/library
   mismatch fails cleanly at runtime rather than misbehaving.

## Source

The full source is the [ValveSoftware/steam-audio](https://github.com/ValveSoftware/steam-audio)
repository, kept as a sibling checkout of this repo (`../steam-audio`) when it needs to be
built or patched. Build its `core` project, then set `NEO_STEAMAUDIO_SDK_PATH` to the resulting
SDK directory.
