#include "cbase.h"
#include "neo_hrtf_system.h"

#include "engine/IEngineSound.h"
#include "filesystem.h"
#include "soundchars.h"
#include "SoundEmitterSystem/isoundemittersystembase.h"
#include "view.h"

#include "miniaudio.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

extern ISoundEmitterSystemBase *soundemitterbase;

namespace
{

constexpr int kHrtfOutputChannels = 2;
constexpr float kHrtfMetresPerUnit = 0.0254f; // 1 Source unit = 1 inch
constexpr int kHrtfMaxCachedSeconds = 30; // longer sounds (music-like loops) stay with the engine
constexpr int kHrtfMaxCachedFrames = kHrtfMaxCachedSeconds * CNeoHrtfSystem::kSampleRate;
constexpr int kHrtfErrorLen = 256;

// The engine mixer (snd_dma.cpp) attenuates with an inverse-distance law normalised so a
// sound at its scripted soundlevel is at unity gain snd_refdist units away:
//   dist_mult = 10^((snd_refdb - sndlvl) / 20) / snd_refdist,  gain = 1 / (dist * dist_mult)
// clamped to [snd_gain_min, snd_gain_max]. These are the engine's defaults for those cvars.
constexpr float kHrtfEngineRefDb = 60.0f;
constexpr float kHrtfEngineRefDistUnits = 36.0f;
constexpr float kHrtfEngineGainMax = 1.0f;
constexpr float kHrtfEngineGainMin = 0.01f;

#ifdef _WIN32
constexpr char kHrtfPhononLibrary[] = "bin/x64/phonon.dll";
#else
constexpr char kHrtfPhononLibrary[] = "bin/linux64/libphonon.so";
#endif

constexpr char kHrtfSoundDir[] = "sound/";

// RIFF layout (little endian): "RIFF" size "WAVE", then chunks of id(4) size(4) body, each
// body padded to an even length.
constexpr int kRiffHeaderSize = 12;
constexpr int kRiffChunkHeaderSize = 8;
constexpr int kWavFmtSampleRateOffset = 4;
constexpr int kWavCueCountSize = 4;
constexpr int kWavCuePointSize = 24;
constexpr int kWavCueSampleOffsetOffset = 20;

uint32 HrtfReadLE32(const uint8 *pData)
{
	return uint32(pData[0]) | (uint32(pData[1]) << 8) | (uint32(pData[2]) << 16) | (uint32(pData[3]) << 24);
}

// Source loops a wav from the sample offset of its first cue point (CAudioSourceWave::
// ParseCueChunk), so do the same. Returns the loop start in output-rate frames, or -1.
int HrtfParseWavLoopStart(const uint8 *pData, int size)
{
	if (size < kRiffHeaderSize || V_memcmp(pData, "RIFF", 4) != 0 || V_memcmp(pData + 8, "WAVE", 4) != 0)
	{
		return -1;
	}

	uint32 sourceRate = 0;
	int64 cueSampleOffset = -1;
	for (int64 pos = kRiffHeaderSize; pos + kRiffChunkHeaderSize <= size;)
	{
		const uint8 *pChunk = pData + pos;
		const uint32 chunkSize = HrtfReadLE32(pChunk + 4);
		const uint8 *pBody = pChunk + kRiffChunkHeaderSize;
		if (pos + kRiffChunkHeaderSize + chunkSize > size)
		{
			break;
		}

		if (V_memcmp(pChunk, "fmt ", 4) == 0 && chunkSize >= kWavFmtSampleRateOffset + sizeof(uint32))
		{
			sourceRate = HrtfReadLE32(pBody + kWavFmtSampleRateOffset);
		}
		else if (V_memcmp(pChunk, "cue ", 4) == 0 && chunkSize >= kWavCueCountSize + kWavCuePointSize
				 && HrtfReadLE32(pBody) > 0)
		{
			cueSampleOffset = HrtfReadLE32(pBody + kWavCueCountSize + kWavCueSampleOffsetOffset);
		}
		pos += kRiffChunkHeaderSize + chunkSize + (chunkSize & 1);
	}

	if (sourceRate == 0 || cueSampleOffset < 0)
	{
		return -1;
	}
	return static_cast<int>(cueSampleOffset * CNeoHrtfSystem::kSampleRate / sourceRate);
}

// Soundscripts and engine channels both reduce to this form: sound chars stripped,
// lower case, forward slashes, relative to sound/.
void HrtfNormaliseSoundName(const char *pszName, char *pszOut, int outSize)
{
	V_strncpy(pszOut, PSkipSoundChars(pszName), outSize);
	V_FixSlashes(pszOut, '/');
	V_strlower(pszOut);
	constexpr int soundDirLen = sizeof(kHrtfSoundDir) - 1;
	if (V_strncmp(pszOut, kHrtfSoundDir, soundDirLen) == 0)
	{
		V_memmove(pszOut, pszOut + soundDirLen, V_strlen(pszOut + soundDirLen) + 1);
	}
}

bool HrtfIsDecodable(const char *pszName)
{
	const char *pszExt = V_GetFileExtension(pszName);
	return pszExt && (V_strcmp(pszExt, "wav") == 0 || V_strcmp(pszExt, "mp3") == 0);
}

NeoSpatial::Vec3 HrtfToMetres(const Vector &v)
{
	return { v.x * kHrtfMetresPerUnit, v.y * kHrtfMetresPerUnit, v.z * kHrtfMetresPerUnit };
}

NeoSpatial::Vec3 HrtfToVec3(const Vector &v)
{
	return { v.x, v.y, v.z };
}

void HrtfDataCallback(ma_device *pDevice, void *pOutput, const void *pInput, ma_uint32 frameCount)
{
	(void)pInput;
	static_cast<CNeoHrtfSystem *>(pDevice->pUserData)->Render(static_cast<float *>(pOutput), static_cast<int>(frameCount));
}

} // namespace

static void OnNeoHrtfConfigChanged(IConVar *pVar, const char *pOldValue, float flOldValue);

ConVar cl_neo_hrtf("cl_neo_hrtf", "0", FCVAR_CLIENTDLL | FCVAR_ARCHIVE,
	"Re-render positional sounds with HRTF (proof of concept). Turning it off stops the HRTF output; "
	"engine copies muted while it was on stay muted until they end.", true, 0.0f, true, 1.0f, OnNeoHrtfConfigChanged);
ConVar cl_neo_hrtf_backend("cl_neo_hrtf_backend", "steamaudio", FCVAR_CLIENTDLL | FCVAR_ARCHIVE,
	"HRTF spatializer backend: steamaudio or panner (falls back to panner if Steam Audio fails to load)",
	OnNeoHrtfConfigChanged);
ConVar cl_neo_hrtf_volume("cl_neo_hrtf_volume", "1.0", FCVAR_CLIENTDLL | FCVAR_ARCHIVE,
	"Volume of the HRTF output, on top of the master volume", true, 0.0f, true, 1.0f);
ConVar cl_neo_hrtf_mute_engine("cl_neo_hrtf_mute_engine", "1", FCVAR_CLIENTDLL,
	"Mute the engine's copy of each sound HRTF re-renders; 0 keeps both audible for A/B comparison",
	true, 0.0f, true, 1.0f);
ConVar cl_neo_hrtf_debug("cl_neo_hrtf_debug", "0", FCVAR_CLIENTDLL,
	"Print one line per HRTF voice: file, distance, gain, azimuth", true, 0.0f, true, 1.0f);

static CNeoHrtfSystem s_neoHrtfSystem;

static void OnNeoHrtfConfigChanged(IConVar *pVar, const char *pOldValue, float flOldValue)
{
	(void)pVar; (void)pOldValue; (void)flOldValue;
	s_neoHrtfSystem.RequestRestart();
}

CON_COMMAND(cl_neo_hrtf_status, "Print the HRTF backend, device state, live voices and cache size")
{
	s_neoHrtfSystem.PrintStatus();
}

CNeoHrtfSystem::CNeoHrtfSystem()
	: CAutoGameSystemPerFrame("CNeoHrtfSystem")
{
}

void CNeoHrtfSystem::Shutdown()
{
	StopDevice();
	m_cache.PurgeAndDeleteElements();
	m_soundLevels.Purge();
	m_bSoundLevelsBuilt = false;
}

void CNeoHrtfSystem::LevelShutdownPreEntity()
{
	ReleaseAllVoices();
	m_ignoredGuids.RemoveAll();
}

void CNeoHrtfSystem::Update(float frametime)
{
	(void)frametime;

	if (m_bRestartPending)
	{
		StopDevice();
		m_bRestartPending = false;
		m_bStartFailed = false;
	}

	if (!m_pDevice)
	{
		if (!cl_neo_hrtf.GetBool() || m_bStartFailed)
		{
			return;
		}
		m_bStartFailed = !StartDevice();
		if (m_bStartFailed)
		{
			return;
		}
	}

	if (!engine->IsInGame())
	{
		ReleaseAllVoices();
		return;
	}

	static ConVarRef s_masterVolume("volume");
	Assert(s_masterVolume.IsValid());
	// Our device bypasses the engine mixer, so the master volume has to be applied here.
	const float outputScale = cl_neo_hrtf_volume.GetFloat() * s_masterVolume.GetFloat();

	Vector listenerUp;
	m_listenerOrigin = MainViewOrigin();
	AngleVectors(MainViewAngles(), &m_listenerForward, &m_listenerRight, &listenerUp);

	m_activeSounds.RemoveAll();
	enginesound->GetActiveSounds(m_activeSounds);

	int liveVoices = 0;
	for (Voice &voice : m_voices)
	{
		voice.m_bSeenThisPoll = false;
		liveVoices += voice.m_bInUse ? 1 : 0;
	}

	// NEO HRTF: classify every engine channel. Existing voices only get new parameters;
	// string work and decoding happen once per guid, outside the lock.
	m_ignoredGuidsNext.RemoveAll();
	int pendingCount = 0;
	const int localPlayerIndex = engine->GetLocalPlayer();
	for (int i = 0; i < m_activeSounds.Count(); ++i)
	{
		const SndInfo_t &info = m_activeSounds[i];
		const int voiceIndex = FindVoice(info.m_nGuid);
		if (voiceIndex >= 0)
		{
			Voice &voice = m_voices[voiceIndex];
			voice.m_bSeenThisPoll = true;
			UpdateEngineMute(voice, info);
			m_stagedParams[voiceIndex] = ComputeParams(info, voice.m_sourceVolume, *voice.m_pSound, outputScale);
			continue;
		}

		if (IsIgnored(info.m_nGuid))
		{
			m_ignoredGuidsNext.AddToTail(info.m_nGuid);
			continue;
		}

		if (!IsSpatialCandidate(info, localPlayerIndex))
		{
			continue;
		}

		const bool bHasFreeVoice = liveVoices + pendingCount < kMaxVoices;
		const CachedSound *pSound = bHasFreeVoice ? FindOrLoadSound(info) : nullptr;
		if (!pSound)
		{
			m_ignoredGuidsNext.AddToTail(info.m_nGuid);
			continue;
		}
		m_pending[pendingCount++] = { i, pSound, -1 };
	}

	{
		AUTO_LOCK(m_mutex);
		m_listener.origin = HrtfToMetres(m_listenerOrigin);
		m_listener.forward = HrtfToVec3(m_listenerForward);
		m_listener.right = HrtfToVec3(m_listenerRight);
		m_listener.up = HrtfToVec3(listenerUp);

		for (int v = 0; v < kMaxVoices; ++v)
		{
			Voice &voice = m_voices[v];
			if (!voice.m_bInUse)
			{
				continue;
			}
			if (voice.m_bSeenThisPoll)
			{
				voice.m_params = m_stagedParams[v];
			}
			else
			{
				m_pSpatializer->ReleaseVoice(voice.m_hSpatial);
				voice = Voice();
			}
		}

		int freeSearch = 0;
		for (int p = 0; p < pendingCount; ++p)
		{
			PendingVoice &pending = m_pending[p];
			while (m_voices[freeSearch].m_bInUse)
			{
				++freeSearch;
				Assert(freeSearch < kMaxVoices);
			}

			const NeoSpatial::VoiceHandle hSpatial = m_pSpatializer->CreateVoice();
			if (hSpatial == NeoSpatial::INVALID_VOICE)
			{
				continue;
			}

			const SndInfo_t &info = m_activeSounds[pending.m_soundIndex];
			Voice &voice = m_voices[freeSearch];
			voice.m_bInUse = true;
			voice.m_guid = info.m_nGuid;
			voice.m_sourceVolume = info.m_flVolume;
			voice.m_bSeenThisPoll = true;
			voice.m_pSound = pending.m_pSound;
			voice.m_hSpatial = hSpatial;
			voice.m_params = ComputeParams(info, info.m_flVolume, *pending.m_pSound, outputScale);
			pending.m_voiceIndex = freeSearch;
		}
	}

	// Muting is an engine call, so it stays out of the audio thread's lock.
	for (int p = 0; p < pendingCount; ++p)
	{
		const PendingVoice &pending = m_pending[p];
		const SndInfo_t &info = m_activeSounds[pending.m_soundIndex];
		if (pending.m_voiceIndex >= 0)
		{
			UpdateEngineMute(m_voices[pending.m_voiceIndex], info);
		}
		else
		{
			m_ignoredGuidsNext.AddToTail(info.m_nGuid);
		}
	}
	m_ignoredGuids.Swap(m_ignoredGuidsNext);

	if (cl_neo_hrtf_debug.GetBool())
	{
		PrintDebug();
	}
}

bool CNeoHrtfSystem::StartDevice()
{
	Assert(!m_pDevice && !m_pSpatializer);
	m_pSpatializer = CreateConfiguredSpatializer();
	if (!m_pSpatializer)
	{
		return false;
	}

	ma_device_config config = ma_device_config_init(ma_device_type_playback);
	config.playback.format = ma_format_f32;
	config.playback.channels = kHrtfOutputChannels;
	config.sampleRate = kSampleRate;
	config.dataCallback = HrtfDataCallback;
	config.pUserData = this;

	m_carryRead = 0;
	m_carryAvailable = 0;
	m_pDevice = new ma_device;
	if (ma_device_init(nullptr, &config, m_pDevice) != MA_SUCCESS)
	{
		Warning("NEO HRTF: could not open an audio output device\n");
		delete m_pDevice;
		m_pDevice = nullptr;
		NeoSpatial::DestroySpatializer(m_pSpatializer);
		m_pSpatializer = nullptr;
		return false;
	}

	if (ma_device_start(m_pDevice) != MA_SUCCESS)
	{
		Warning("NEO HRTF: could not start the audio output device\n");
		StopDevice();
		return false;
	}
	return true;
}

void CNeoHrtfSystem::StopDevice()
{
	if (!m_pDevice)
	{
		return;
	}

	// Uninit joins the audio thread, so nothing below can race the callback.
	ma_device_uninit(m_pDevice);
	delete m_pDevice;
	m_pDevice = nullptr;

	ReleaseAllVoices();
	m_pSpatializer->Shutdown();
	NeoSpatial::DestroySpatializer(m_pSpatializer);
	m_pSpatializer = nullptr;
}

void CNeoHrtfSystem::ReleaseAllVoices()
{
	AUTO_LOCK(m_mutex);
	for (Voice &voice : m_voices)
	{
		if (voice.m_bInUse)
		{
			Assert(m_pSpatializer);
			m_pSpatializer->ReleaseVoice(voice.m_hSpatial);
			voice = Voice();
		}
	}
}

NeoSpatial::ISpatializer *CNeoHrtfSystem::CreateConfiguredSpatializer() const
{
	char error[kHrtfErrorLen] = "";
	const char *pszBackend = cl_neo_hrtf_backend.GetString();
	if (V_stricmp(pszBackend, "steamaudio") == 0)
	{
		char path[MAX_PATH];
		V_snprintf(path, sizeof(path), "%s/%s", engine->GetGameDirectory(), kHrtfPhononLibrary);
		V_FixSlashes(path);

		NeoSpatial::ISpatializer *pSteamAudio = NeoSpatial::CreateSteamAudioSpatializer(path, error, sizeof(error));
		if (pSteamAudio && pSteamAudio->Init(kSampleRate, kFrameSize, error, sizeof(error)))
		{
			return pSteamAudio;
		}
		if (pSteamAudio)
		{
			NeoSpatial::DestroySpatializer(pSteamAudio);
		}
		Warning("NEO HRTF: Steam Audio unavailable (%s), falling back to the panner\n", error);
	}
	else if (V_stricmp(pszBackend, "panner") != 0)
	{
		Warning("NEO HRTF: unknown cl_neo_hrtf_backend \"%s\", using the panner\n", pszBackend);
	}

	NeoSpatial::ISpatializer *pPanner = NeoSpatial::CreatePannerSpatializer();
	if (!pPanner->Init(kSampleRate, kFrameSize, error, sizeof(error)))
	{
		Warning("NEO HRTF: panner failed to initialise (%s)\n", error);
		NeoSpatial::DestroySpatializer(pPanner);
		return nullptr;
	}
	return pPanner;
}

bool CNeoHrtfSystem::IsSpatialCandidate(const SndInfo_t &info, int localPlayerIndex) const
{
	if (info.m_bIsSentence || info.m_bDryMix || info.m_bSpeaker || !info.m_pOrigin)
	{
		return false;
	}
	// The local player's own weapon and viewmodel sounds are non-positional by design.
	if (info.m_nSoundSource == localPlayerIndex)
	{
		return false;
	}
	// UI and menu sounds: no source entity and no position.
	return info.m_nSoundSource > 0 || *info.m_pOrigin != vec3_origin;
}

const CNeoHrtfSystem::CachedSound *CNeoHrtfSystem::FindOrLoadSound(const SndInfo_t &info)
{
	char rawName[MAX_PATH];
	if (!filesystem->String(info.m_filenameHandle, rawName, sizeof(rawName)) || TestSoundChar(rawName, CHAR_STREAM))
	{
		return nullptr;
	}

	char name[MAX_PATH];
	HrtfNormaliseSoundName(rawName, name, sizeof(name));
	if (!HrtfIsDecodable(name))
	{
		return nullptr;
	}

	int index = m_cache.Find(name);
	if (index == m_cache.InvalidIndex())
	{
		// Failures are cached too (empty samples), so a bad file is only read once per session.
		CachedSound *pSound = new CachedSound;
		pSound->m_name = name;
		LoadSound(*pSound);
		index = m_cache.Insert(name, pSound);
	}

	const CachedSound *pSound = m_cache[index];
	return pSound->m_samples.IsEmpty() ? nullptr : pSound;
}

void CNeoHrtfSystem::LoadSound(CachedSound &sound)
{
	char path[MAX_PATH];
	V_snprintf(path, sizeof(path), "%s%s", kHrtfSoundDir, sound.m_name.Get());
	FileHandle_t hFile = filesystem->Open(path, "rb", "GAME");
	if (!hFile)
	{
		DevMsg("NEO HRTF: cannot open %s, leaving it to the engine\n", path);
		return;
	}
	const int fileSize = static_cast<int>(filesystem->Size(hFile));
	m_fileBuffer.SetCount(fileSize);
	const int bytesRead = filesystem->Read(m_fileBuffer.Base(), fileSize, hFile);
	filesystem->Close(hFile);
	if (fileSize <= 0 || bytesRead != fileSize)
	{
		DevMsg("NEO HRTF: cannot read %s, leaving it to the engine\n", path);
		return;
	}

	const ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 1, kSampleRate);
	ma_decoder decoder;
	if (ma_decoder_init_memory(m_fileBuffer.Base(), fileSize, &config, &decoder) != MA_SUCCESS)
	{
		DevMsg("NEO HRTF: cannot decode %s, leaving it to the engine\n", path);
		return;
	}

	ma_uint64 length = 0;
	if (ma_decoder_get_length_in_pcm_frames(&decoder, &length) == MA_SUCCESS
		&& length > 0 && length <= static_cast<ma_uint64>(kHrtfMaxCachedFrames))
	{
		sound.m_samples.SetCount(static_cast<int>(length));
		ma_uint64 framesRead = 0;
		ma_decoder_read_pcm_frames(&decoder, sound.m_samples.Base(), length, &framesRead);
		sound.m_samples.SetCountNonDestructively(static_cast<int>(framesRead));
	}
	else
	{
		DevMsg("NEO HRTF: %s is empty or longer than %d s, leaving it to the engine\n", path, kHrtfMaxCachedSeconds);
	}
	ma_decoder_uninit(&decoder);

	if (V_strcmp(V_GetFileExtension(sound.m_name.Get()), "wav") == 0)
	{
		const int loopStart = HrtfParseWavLoopStart(m_fileBuffer.Base(), fileSize);
		sound.m_loopStart = (loopStart < sound.m_samples.Count()) ? loopStart : -1;
	}
	sound.m_distMult = LookupDistMult(sound.m_name.Get());
}

float CNeoHrtfSystem::LookupDistMult(const char *pszNormalisedName)
{
	if (!m_bSoundLevelsBuilt)
	{
		BuildSoundLevelMap();
	}

	const int index = m_soundLevels.Find(pszNormalisedName);
	const soundlevel_t level = (index != m_soundLevels.InvalidIndex()) ? m_soundLevels[index] : SNDLVL_NORM;
	// SNDLVL_NONE means "heard everywhere": no distance falloff at all.
	if (level == SNDLVL_NONE)
	{
		return 0.0f;
	}
	return powf(10.0f, (kHrtfEngineRefDb - static_cast<float>(level)) / 20.0f) / kHrtfEngineRefDistUnits;
}

void CNeoHrtfSystem::BuildSoundLevelMap()
{
	m_bSoundLevelsBuilt = true;
	Assert(soundemitterbase);

	// Channels only carry the wave file, so map every scripted wave back to its sound level.
	char name[MAX_PATH];
	for (int i = soundemitterbase->First(); i != soundemitterbase->InvalidIndex(); i = soundemitterbase->Next(i))
	{
		const CSoundParametersInternal *pParams = soundemitterbase->InternalGetParametersForSound(i);
		if (!pParams)
		{
			continue;
		}

		const soundlevel_t level = static_cast<soundlevel_t>(pParams->GetSoundLevel().start);
		const SoundFile *pWaves = pParams->GetSoundNames();
		for (int w = 0; w < pParams->NumSoundNames(); ++w)
		{
			CUtlSymbol waveSymbol = pWaves[w].symbol;
			HrtfNormaliseSoundName(soundemitterbase->GetWaveName(waveSymbol), name, sizeof(name));
			const int index = m_soundLevels.Find(name);
			if (index == m_soundLevels.InvalidIndex())
			{
				m_soundLevels.Insert(name, level);
			}
			else if (level > m_soundLevels[index])
			{
				m_soundLevels[index] = level;
			}
		}
	}
}

CNeoHrtfSystem::VoiceParams CNeoHrtfSystem::ComputeParams(const SndInfo_t &info, float sourceVolume,
														  const CachedSound &sound, float outputScale) const
{
	Assert(info.m_pOrigin);
	Vector position = *info.m_pOrigin;
	// m_bUpdatePositions is the engine's own "follows its entity" flag; without it the
	// origin is explicit (e.g. an impact point) and must not snap to the emitting entity.
	if (info.m_bUpdatePositions && info.m_nSoundSource > 0)
	{
		C_BaseEntity *pEntity = ClientEntityList().GetEnt(info.m_nSoundSource);
		if (pEntity && !pEntity->IsDormant())
		{
			position = pEntity->GetAbsOrigin();
		}
	}

	// The engine's inverse-distance model (see kHrtfEngineRefDb), so audible ranges match
	// the engine's own copy. Below snd_gain_min the engine stops mixing the channel.
	const float relativeDistance = position.DistTo(m_listenerOrigin) * sound.m_distMult;
	float distanceGain = (relativeDistance > 1.0f) ? (1.0f / relativeDistance) : kHrtfEngineGainMax;
	if (distanceGain < kHrtfEngineGainMin)
	{
		distanceGain = 0.0f;
	}

	Assert(info.m_nPitch > 0);
	VoiceParams params;
	params.m_origin = HrtfToMetres(position);
	params.m_gain = sourceVolume * distanceGain * outputScale;
	params.m_rate = static_cast<float>(Max(info.m_nPitch, 1)) / PITCH_NORM;
	return params;
}

void CNeoHrtfSystem::UpdateEngineMute(Voice &voice, const SndInfo_t &info)
{
	// Once we have muted the engine copy its reported volume is our 0, not the sound's;
	// a non-zero report afterwards is a genuine server volume change.
	if (!voice.m_bEngineMuted || info.m_flVolume > 0.0f)
	{
		voice.m_sourceVolume = info.m_flVolume;
	}
	if (cl_neo_hrtf_mute_engine.GetBool() && info.m_flVolume > 0.0f)
	{
		enginesound->SetVolumeByGuid(info.m_nGuid, 0.0f);
		voice.m_bEngineMuted = true;
	}
}

int CNeoHrtfSystem::FindVoice(int guid) const
{
	for (int v = 0; v < kMaxVoices; ++v)
	{
		if (m_voices[v].m_bInUse && m_voices[v].m_guid == guid)
		{
			return v;
		}
	}
	return -1;
}

bool CNeoHrtfSystem::IsIgnored(int guid) const
{
	return m_ignoredGuids.Find(guid) != m_ignoredGuids.InvalidIndex();
}

void CNeoHrtfSystem::PrintDebug() const
{
	const Vector listenerMetres = m_listenerOrigin * kHrtfMetresPerUnit;
	for (int v = 0; v < kMaxVoices; ++v)
	{
		const Voice &voice = m_voices[v];
		if (!voice.m_bInUse)
		{
			continue;
		}
		const NeoSpatial::Vec3 &origin = voice.m_params.m_origin;
		const Vector toSource = Vector(origin.x, origin.y, origin.z) - listenerMetres;
		const float azimuthDeg = RAD2DEG(atan2f(DotProduct(toSource, m_listenerRight), DotProduct(toSource, m_listenerForward)));
		engine->Con_NPrintf(v, "hrtf %s  %.1f m  gain %.2f  az %+.0f", voice.m_pSound->m_name.Get(),
			toSource.Length(), voice.m_params.m_gain, azimuthDeg);
	}
}

void CNeoHrtfSystem::PrintStatus() const
{
	int liveVoices = 0;
	for (const Voice &voice : m_voices)
	{
		liveVoices += voice.m_bInUse ? 1 : 0;
	}

	const char *pszDeviceState = "off";
	if (m_pDevice)
	{
		pszDeviceState = ma_device_is_started(m_pDevice) ? "started" : "stopped";
	}
	Msg("NEO HRTF: backend %s, device %s, voices %d/%d, cached sounds %d, scripted wave levels %d\n",
		m_pSpatializer ? m_pSpatializer->GetName() : "none", pszDeviceState, liveVoices, kMaxVoices,
		m_cache.Count(), m_soundLevels.Count());
}

void CNeoHrtfSystem::Render(float *pOutInterleaved, int frameCount)
{
	// Backends take a fixed block size, while miniaudio asks for whatever its period is,
	// so render whole blocks and hand them out through the carry buffer.
	while (frameCount > 0)
	{
		if (m_carryAvailable == 0)
		{
			RenderBlock();
			m_carryRead = 0;
			m_carryAvailable = kFrameSize;
		}
		const int frames = Min(frameCount, m_carryAvailable);
		V_memcpy(pOutInterleaved, m_carry + m_carryRead * kHrtfOutputChannels, frames * kHrtfOutputChannels * sizeof(float));
		pOutInterleaved += frames * kHrtfOutputChannels;
		frameCount -= frames;
		m_carryRead += frames;
		m_carryAvailable -= frames;
	}
}

void CNeoHrtfSystem::RenderBlock()
{
	V_memset(m_carry, 0, sizeof(m_carry));
	{
		AUTO_LOCK(m_mutex);
		Assert(m_pSpatializer);
		m_pSpatializer->SetListener(m_listener);
		for (Voice &voice : m_voices)
		{
			if (!voice.m_bInUse || voice.m_bFinished)
			{
				continue;
			}
			voice.m_bFinished = !ReadVoiceSamples(voice);

			const float gain = voice.m_params.m_gain;
			if (gain <= 0.0f)
			{
				continue;
			}
			m_pSpatializer->Process(voice.m_hSpatial, voice.m_params.m_origin, m_scratchMono,
				m_scratchLeft, m_scratchRight, kFrameSize);
			for (int f = 0; f < kFrameSize; ++f)
			{
				m_carry[f * kHrtfOutputChannels] += gain * m_scratchLeft[f];
				m_carry[f * kHrtfOutputChannels + 1] += gain * m_scratchRight[f];
			}
		}
	}

	for (float &sample : m_carry)
	{
		sample = clamp(sample, -1.0f, 1.0f);
	}
}

bool CNeoHrtfSystem::ReadVoiceSamples(Voice &voice)
{
	const CachedSound &sound = *voice.m_pSound;
	const float *pSamples = sound.m_samples.Base();
	const int count = sound.m_samples.Count();
	const int loopStart = sound.m_loopStart;
	const double rate = voice.m_params.m_rate;
	double cursor = voice.m_cursor;

	for (int f = 0; f < kFrameSize; ++f)
	{
		if (cursor >= count)
		{
			if (loopStart < 0)
			{
				V_memset(m_scratchMono + f, 0, (kFrameSize - f) * sizeof(float));
				voice.m_cursor = cursor;
				return false;
			}
			cursor = loopStart + fmod(cursor - loopStart, static_cast<double>(count - loopStart));
		}

		// Linear interpolation is enough for engine pitch shifts, which stay within an octave or so.
		const int index = static_cast<int>(cursor);
		const int nextIndex = (index + 1 < count) ? index + 1 : Max(loopStart, index);
		const float frac = static_cast<float>(cursor - index);
		m_scratchMono[f] = pSamples[index] + (pSamples[nextIndex] - pSamples[index]) * frac;
		cursor += rate;
	}
	voice.m_cursor = cursor;
	return true;
}
