// NEO HRTF: backend-neutral spatializer contract.
//
// This header is the whole boundary between NT;RE and any spatial-audio library.
// It deliberately has no Source SDK dependencies (no Vector, no tier0), so a backend
// implementation can be built and tested outside the game, and a library upgrade
// (e.g. a new Steam Audio release) never touches game code.
//
// Conventions
//  - Coordinates are in the Source engine's world frame (right-handed, +Z up,
//    x/y horizontal) but scaled to METRES. The game layer converts units; the
//    backend converts axes to whatever its library expects.
//  - Audio is mono-in, stereo-out, 32-bit float, non-interleaved, one block of
//    `frameSize` frames per Process() call at the sample rate given to Init().
//  - Threading: Init/Shutdown/CreateVoice/ReleaseVoice are called from the game
//    thread. SetListener/Process are called from the audio (mixer) thread. The game
//    layer guarantees these never overlap for one backend instance.
//  - Process() OVERWRITES outLeft/outRight with the spatialised signal at unity
//    gain. Distance attenuation and volume are the game layer's job, so the
//    distance model can be tuned (or swapped) without touching a backend.
#pragma once

#include <cstdint>

namespace NeoSpatial
{

struct Vec3
{
	float x, y, z;
};

struct Listener
{
	Vec3 origin;
	Vec3 forward;
	Vec3 right;
	Vec3 up;
};

typedef uint32_t VoiceHandle;
static constexpr VoiceHandle INVALID_VOICE = 0;

class ISpatializer
{
public:
	virtual ~ISpatializer() {}

	// Short identifier for cvars / debug output ("steamaudio", "panner").
	virtual const char *GetName() const = 0;

	// Returns false and fills errorOut (NUL-terminated, up to errorLen) on failure.
	virtual bool Init(int sampleRate, int frameSize, char *errorOut, int errorLen) = 0;
	virtual void Shutdown() = 0;

	virtual void SetListener(const Listener &listener) = 0;

	// A voice holds per-source filter state (e.g. HRTF crossfade history).
	virtual VoiceHandle CreateVoice() = 0;
	virtual void ReleaseVoice(VoiceHandle voice) = 0;

	// Spatialise one block. `origin` is the source position in the same frame as the
	// listener. `frames` equals the frameSize passed to Init().
	virtual void Process(VoiceHandle voice, const Vec3 &origin, const float *monoIn,
						 float *outLeft, float *outRight, int frames) = 0;

	// Optional: world geometry for occlusion/propagation. Triangle indices are
	// 0-based into `vertices` (xyz triples, metres). Backends without geometry
	// support ignore it. Called from the game thread while no voices exist.
	virtual void SetSceneGeometry(const float *vertices, int numVertices,
								  const uint32_t *triangles, int numTriangles)
	{
		(void)vertices; (void)numVertices; (void)triangles; (void)numTriangles;
	}
};

// Factories. Each backend lives in its own translation unit; only that unit includes
// the third-party library's headers.
//
// Steam Audio: `phononLibraryPath` is the absolute path of phonon.dll / libphonon.so.
// The library is loaded at runtime (never linked), so a missing or broken library
// returns nullptr and the game keeps running with another backend.
ISpatializer *CreateSteamAudioSpatializer(const char *phononLibraryPath, char *errorOut, int errorLen);

// Reference backend: constant-power stereo panning, no HRTF. Useful for A/B
// listening and for exercising the pipeline where phonon is unavailable.
ISpatializer *CreatePannerSpatializer();

void DestroySpatializer(ISpatializer *spatializer);

} // namespace NeoSpatial
