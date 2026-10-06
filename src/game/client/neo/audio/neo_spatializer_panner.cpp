// NEO HRTF: reference backend for NeoSpatial::ISpatializer, constant-power stereo panning.
//
// It has no library dependency, so it is the fallback when phonon cannot be loaded and the
// baseline when A/B-ing the HRTF. Being purely left/right, it cannot tell front from behind.
#include "neo_spatializer.h"

#include <cmath>

namespace NeoSpatial
{

namespace
{

// The panner keeps no per-voice state, so every voice can share one handle.
constexpr VoiceHandle STATELESS_VOICE = 1;

constexpr float QUARTER_PI = 0.785398163f;

float Dot(const Vec3 &a, const Vec3 &b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

class CPannerSpatializer final : public ISpatializer
{
public:
	const char *GetName() const override
	{
		return "panner";
	}

	bool Init(int /*sampleRate*/, int /*frameSize*/, char * /*errorOut*/, int /*errorLen*/) override
	{
		return true;
	}

	void Shutdown() override
	{
	}

	void SetListener(const Listener &listener) override
	{
		m_listener = listener;
	}

	VoiceHandle CreateVoice() override
	{
		return STATELESS_VOICE;
	}

	void ReleaseVoice(VoiceHandle /*voice*/) override
	{
	}

	void Process(VoiceHandle /*voice*/, const Vec3 &origin, const float *monoIn,
				 float *outLeft, float *outRight, int frames) override
	{
		const Vec3 delta = { origin.x - m_listener.origin.x, origin.y - m_listener.origin.y, origin.z - m_listener.origin.z };
		const float azimuth = atan2f(Dot(delta, m_listener.right), Dot(delta, m_listener.forward));

		// sin(azimuth) folds front and back onto the same pan position, -1 hard left to +1 hard
		// right, which is then mapped onto a quarter circle so L^2 + R^2 stays 1.
		const float panAngle = (sinf(azimuth) + 1.0f) * QUARTER_PI;
		const float leftGain = cosf(panAngle);
		const float rightGain = sinf(panAngle);

		for (int i = 0; i < frames; ++i)
		{
			outLeft[i] = monoIn[i] * leftGain;
			outRight[i] = monoIn[i] * rightGain;
		}
	}

private:
	Listener m_listener = {};
};

} // namespace

ISpatializer *CreatePannerSpatializer()
{
	return new CPannerSpatializer();
}

void DestroySpatializer(ISpatializer *spatializer)
{
	delete spatializer;
}

} // namespace NeoSpatial
