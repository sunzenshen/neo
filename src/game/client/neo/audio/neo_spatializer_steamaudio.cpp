// NEO HRTF: Steam Audio backend for NeoSpatial::ISpatializer.
//
// phonon is loaded at runtime rather than linked so that a missing or incompatible library
// only disables this backend, and so the client does not depend on the dynamic loader finding
// the mod's bin directory. Only the functions listed in NEO_PHONON_FUNCTIONS are resolved.
#include "neo_spatializer.h"

#include <phonon.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace NeoSpatial
{

namespace
{

#define NEO_PHONON_FUNCTIONS(X) \
	X(iplContextCreate) \
	X(iplContextRelease) \
	X(iplHRTFCreate) \
	X(iplHRTFRelease) \
	X(iplBinauralEffectCreate) \
	X(iplBinauralEffectRelease) \
	X(iplBinauralEffectReset) \
	X(iplBinauralEffectApply)

struct PhononApi
{
#define NEO_PHONON_DECLARE(name) decltype(&::name) name = nullptr;
	NEO_PHONON_FUNCTIONS(NEO_PHONON_DECLARE)
#undef NEO_PHONON_DECLARE
};

#ifdef _WIN32
typedef HMODULE LibraryHandle;

LibraryHandle OpenLibrary(const char *path)
{
	// Resolve phonon.dll's own dependencies from its directory rather than the game executable's.
	return LoadLibraryExA(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

void *FindSymbol(LibraryHandle library, const char *name)
{
	return reinterpret_cast<void *>(GetProcAddress(library, name));
}

void CloseLibrary(LibraryHandle library)
{
	FreeLibrary(library);
}

void FormatLastLoaderError(char *buffer, int bufferLen)
{
	snprintf(buffer, static_cast<size_t>(bufferLen), "Win32 error %lu", GetLastError());
}
#else
typedef void *LibraryHandle;

LibraryHandle OpenLibrary(const char *path)
{
	// RTLD_LOCAL keeps phonon's bundled symbols from interposing on the engine's.
	return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}

void *FindSymbol(LibraryHandle library, const char *name)
{
	return dlsym(library, name);
}

void CloseLibrary(LibraryHandle library)
{
	dlclose(library);
}

void FormatLastLoaderError(char *buffer, int bufferLen)
{
	const char *error = dlerror();
	snprintf(buffer, static_cast<size_t>(bufferLen), "%s", error ? error : "unknown dlerror");
}
#endif

#if defined(__GNUC__)
__attribute__((format(printf, 3, 4)))
#endif
void WriteError(char *errorOut, int errorLen, const char *format, ...)
{
	if (!errorOut || errorLen <= 0)
	{
		return;
	}

	va_list args;
	va_start(args, format);
	vsnprintf(errorOut, static_cast<size_t>(errorLen), format, args);
	va_end(args);
}

constexpr int MAX_VOICES = 64;
constexpr int LOADER_ERROR_LEN = 256;

// Below this the source is effectively inside the listener's head and has no direction.
constexpr float MIN_SOURCE_DISTANCE_SQR = 1e-6f;

// Steam Audio's frame: +x right, +y up, -z ahead.
constexpr IPLVector3 STEAMAUDIO_AHEAD = { 0.0f, 0.0f, -1.0f };

float Dot(const Vec3 &a, const Vec3 &b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

IPLVector3 ToSteamAudioDirection(const Listener &listener, const Vec3 &origin)
{
	const Vec3 delta = { origin.x - listener.origin.x, origin.y - listener.origin.y, origin.z - listener.origin.z };
	const float distanceSqr = Dot(delta, delta);
	if (distanceSqr < MIN_SOURCE_DISTANCE_SQR)
	{
		return STEAMAUDIO_AHEAD;
	}

	const float invDistance = 1.0f / sqrtf(distanceSqr);
	const Vec3 dir = { delta.x * invDistance, delta.y * invDistance, delta.z * invDistance };
	return { Dot(dir, listener.right), Dot(dir, listener.up), -Dot(dir, listener.forward) };
}

class CSteamAudioSpatializer final : public ISpatializer
{
public:
	CSteamAudioSpatializer(LibraryHandle library, const PhononApi &api)
		: m_library(library)
		, m_api(api)
	{
	}

	~CSteamAudioSpatializer() override
	{
		Shutdown();
		CloseLibrary(m_library);
	}

	const char *GetName() const override
	{
		return "steamaudio";
	}

	bool Init(int sampleRate, int frameSize, char *errorOut, int errorLen) override
	{
		Shutdown();

		IPLContextSettings contextSettings = {};
		contextSettings.version = STEAMAUDIO_VERSION;
		// The default cap is SSE2; AVX512 is avoided because it can throttle the CPU clock.
		contextSettings.simdLevel = IPL_SIMDLEVEL_AVX2;
		IPLerror status = m_api.iplContextCreate(&contextSettings, &m_context);
		if (status != IPL_STATUS_SUCCESS)
		{
			WriteError(errorOut, errorLen, "iplContextCreate failed (status %d, built against Steam Audio %d.%d.%d)",
					   static_cast<int>(status), STEAMAUDIO_VERSION_MAJOR, STEAMAUDIO_VERSION_MINOR, STEAMAUDIO_VERSION_PATCH);
			Shutdown();
			return false;
		}

		m_audioSettings.samplingRate = sampleRate;
		m_audioSettings.frameSize = frameSize;

		IPLHRTFSettings hrtfSettings = {};
		hrtfSettings.type = IPL_HRTFTYPE_DEFAULT;
		hrtfSettings.volume = 1.0f;
		hrtfSettings.normType = IPL_HRTFNORMTYPE_NONE;
		status = m_api.iplHRTFCreate(m_context, &m_audioSettings, &hrtfSettings, &m_hrtf);
		if (status != IPL_STATUS_SUCCESS)
		{
			WriteError(errorOut, errorLen, "iplHRTFCreate failed (status %d)", static_cast<int>(status));
			Shutdown();
			return false;
		}

		return true;
	}

	void Shutdown() override
	{
		for (Voice &voice : m_voices)
		{
			if (voice.effect)
			{
				m_api.iplBinauralEffectRelease(&voice.effect);
			}
			voice.inUse = false;
		}

		if (m_hrtf)
		{
			m_api.iplHRTFRelease(&m_hrtf);
		}

		if (m_context)
		{
			m_api.iplContextRelease(&m_context);
		}
	}

	void SetListener(const Listener &listener) override
	{
		m_listener = listener;
	}

	VoiceHandle CreateVoice() override
	{
		if (!m_hrtf)
		{
			return INVALID_VOICE;
		}

		for (int i = 0; i < MAX_VOICES; ++i)
		{
			Voice &voice = m_voices[i];
			if (voice.inUse)
			{
				continue;
			}

			// Effects are kept after ReleaseVoice so a busy scene reuses them instead of
			// reallocating HRTF filter state for every new sound.
			if (voice.effect)
			{
				m_api.iplBinauralEffectReset(voice.effect);
			}
			else
			{
				IPLBinauralEffectSettings effectSettings = {};
				effectSettings.hrtf = m_hrtf;
				if (m_api.iplBinauralEffectCreate(m_context, &m_audioSettings, &effectSettings, &voice.effect) != IPL_STATUS_SUCCESS)
				{
					voice.effect = nullptr;
					return INVALID_VOICE;
				}
			}

			voice.inUse = true;
			return static_cast<VoiceHandle>(i + 1);
		}

		return INVALID_VOICE;
	}

	void ReleaseVoice(VoiceHandle handle) override
	{
		Voice *const voice = FindVoice(handle);
		if (voice)
		{
			voice->inUse = false;
		}
	}

	void Process(VoiceHandle handle, const Vec3 &origin, const float *monoIn,
				 float *outLeft, float *outRight, int frames) override
	{
		const Voice *const voice = FindVoice(handle);
		// Steam Audio's effects only accept the frame size they were created with.
		if (!voice || frames != m_audioSettings.frameSize)
		{
			std::fill_n(outLeft, frames, 0.0f);
			std::fill_n(outRight, frames, 0.0f);
			return;
		}

		IPLBinauralEffectParams params = {};
		params.direction = ToSteamAudioDirection(m_listener, origin);
		params.interpolation = IPL_HRTFINTERPOLATION_BILINEAR;
		params.spatialBlend = 1.0f;
		params.hrtf = m_hrtf;

		// The C API takes non-const channel pointers but never writes to an input buffer.
		float *inChannels[] = { const_cast<float *>(monoIn) };
		float *outChannels[] = { outLeft, outRight };
		IPLAudioBuffer inBuffer = { 1, frames, inChannels };
		IPLAudioBuffer outBuffer = { 2, frames, outChannels };
		m_api.iplBinauralEffectApply(voice->effect, &params, &inBuffer, &outBuffer);
	}

private:
	struct Voice
	{
		IPLBinauralEffect effect = nullptr;
		bool inUse = false;
	};

	Voice *FindVoice(VoiceHandle handle)
	{
		if (handle == INVALID_VOICE || handle > static_cast<VoiceHandle>(MAX_VOICES))
		{
			return nullptr;
		}

		Voice &voice = m_voices[handle - 1];
		return voice.inUse ? &voice : nullptr;
	}

	const LibraryHandle m_library;
	const PhononApi m_api;

	IPLContext m_context = nullptr;
	IPLHRTF m_hrtf = nullptr;
	IPLAudioSettings m_audioSettings = {};
	Listener m_listener = {};
	Voice m_voices[MAX_VOICES];
};

} // namespace

ISpatializer *CreateSteamAudioSpatializer(const char *phononLibraryPath, char *errorOut, int errorLen)
{
	const LibraryHandle library = OpenLibrary(phononLibraryPath);
	if (!library)
	{
		char loaderError[LOADER_ERROR_LEN];
		FormatLastLoaderError(loaderError, LOADER_ERROR_LEN);
		WriteError(errorOut, errorLen, "Failed to load %s: %s", phononLibraryPath, loaderError);
		return nullptr;
	}

	PhononApi api;
#define NEO_PHONON_RESOLVE(name) \
	api.name = reinterpret_cast<decltype(api.name)>(FindSymbol(library, #name)); \
	if (!api.name) \
	{ \
		WriteError(errorOut, errorLen, "%s does not export " #name, phononLibraryPath); \
		CloseLibrary(library); \
		return nullptr; \
	}
	NEO_PHONON_FUNCTIONS(NEO_PHONON_RESOLVE)
#undef NEO_PHONON_RESOLVE

	return new CSteamAudioSpatializer(library, api);
}

} // namespace NeoSpatial
