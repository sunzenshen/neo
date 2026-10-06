# NEO HRTF — client-side spatial audio (proof of concept)

Binaural (HRTF) rendering of positional in-game sounds — other players' weapons, footsteps,
world sounds — without touching the closed-source Source engine mixer.

## Why it is built this way

The engine (`engine.dll` / `engine.so`) owns the mixer and plays most sounds on its own, including
every sound the server starts, so `client.dll` never sees an audio buffer. What the client *can*
do through public interfaces is:

- see every active engine channel each frame: `IEngineSound::GetActiveSounds()` returns guid,
  filename handle, source entity, origin, volume, pitch and flags for all channels, including
  server-started sounds;
- silence any one of them: `IEngineSound::SetVolumeByGuid()`;
- read the sound files itself: `IFileSystem` sees the same `sound/` tree (VPKs and the mounted
  original NEOTOKYO content) the engine plays from;
- output audio itself: `miniaudio` is already vendored for the MP3 player.

So the proof of concept re-renders positional sounds in parallel: poll the engine's channel
list, mute the engine's copy, decode the same file, and play it through a second output device
with HRTF applied. Non-positional sounds (UI, music, sentences, the local player's own weapon)
are left to the engine.

```
engine mixer  ──GetActiveSounds()──►  CNeoHrtfSystem (game thread, once per frame)
      ▲                                   │  new guid: resolve file, decode+cache, SetVolumeByGuid(0)
      │ SetVolumeByGuid(guid, 0)          │  every frame: entity/origin → metres, gain, pitch, listener
      └───────────────────────────────────┤
                                          ▼  (mutex-guarded voice table)
                               miniaudio ma_device callback (audio thread)
                                          │  per 512-frame block, per voice:
                                          ▼
                     NeoSpatial::ISpatializer  (neo_spatializer.h — the only contract)
                        ├── neo_spatializer_steamaudio.cpp   Steam Audio binaural effect
                        └── neo_spatializer_panner.cpp       constant-power pan (reference / fallback)
```

## Separation of concerns

| Layer | Files | Knows about |
| --- | --- | --- |
| Game integration | `neo_hrtf_system.{h,cpp}` | Source SDK (engine sound list, entities, filesystem, view, cvars), miniaudio decode/output |
| Contract | `neo_spatializer.h` | nothing but plain C++ (metres, Source axes, mono-in/stereo-out float blocks) |
| Steam Audio backend | `neo_spatializer_steamaudio.cpp` | `phonon.h` only; loads `libphonon.so` / `phonon.dll` at runtime |
| Reference backend | `neo_spatializer_panner.cpp` | nothing |
| Dependency | `src/thirdparty/steamaudio/` (headers), `src/cmake/steamaudio.cmake` (runtime library download/copy) | Steam Audio release artefacts |

The backends have no Source SDK dependency, so they are compiled outside the unity build and
without the client PCH, and the same two files are built into an offline demo
(`ntre/harness/hrtf/`) that renders test wavs without the game. Steam Audio is never linked:
a missing or incompatible `phonon` library produces a warning and the panner takes over.
Upgrading Steam Audio is a URL/hash bump plus a header recopy (see
`src/thirdparty/steamaudio/README.md` and `NEO-INTEGRATION.md` in the sibling `steam-audio`
checkout).

## Trying it

1. Build normally. CMake downloads the Steam Audio 4.8.1 release once and copies the runtime
   library next to `client` in `game/neo/bin/<plat>/` (`NEO_STEAMAUDIO=OFF` builds without it;
   `NEO_STEAMAUDIO_SDK_PATH` points at a local SDK/built tree instead of downloading).
2. In game, with headphones: `cl_neo_hrtf 1`. `cl_neo_hrtf_status` shows which backend loaded.
3. A/B: `cl_neo_hrtf_backend panner` vs `steamaudio` (re-run `cl_neo_hrtf 1` to re-init),
   `cl_neo_hrtf_mute_engine 0` to hear the engine's own copy as well, `cl_neo_hrtf_debug 1`
   for a per-voice readout. Player pings (`)gameplay/ping.wav` at a world position) and bots
   firing are easy sources.

## Known limitations of the proof of concept

- The engine's copy plays for up to one client frame before it is muted (the poll runs once per
  frame), so the attack of a sound is briefly doubled. Hooking emission earlier would need
  engine code; a `C_BaseEntity::EmitSound` hook would only catch client-emitted sounds.
- Distance attenuation re-implements the engine's linear sound-level model from the sound
  script's `soundlevel`; sounds played by raw filename fall back to `SNDLVL_NORM`. Engine DSP,
  ducking, `snd_surround` and room reverb do not apply to the HRTF copy.
- Occlusion / propagation is not wired up: `ISpatializer::SetSceneGeometry()` is reserved for
  feeding the map's collision mesh (`vcollide` of the world model) into an `IPLStaticMesh`.
- MP3 player music is out of scope by design.
