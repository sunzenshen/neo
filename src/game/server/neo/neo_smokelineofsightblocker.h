#ifndef NEO_SMOKELOSBLOCKER_NEO_H
#define NEO_SMOKELOSBLOCKER_NEO_H
#ifdef _WIN32
#pragma once
#endif

#include "cbase.h"

#define SMOKELINEOFSIGHTBLOCKER_ENTITYNAME	"env_smokelineofsightblocker"

struct ScopedSmokeLOS;

class CNEOSmokeLineOfSightBlocker : public CBaseEntity {
public:
	DECLARE_CLASS(CNEOSmokeLineOfSightBlocker, CBaseEntity);
	DECLARE_DATADESC();

	// Starts LOS interception after scheduled delay when smoke particles are practically occluded
	void ActivateLOSBlocker();

	// A blocker that follows the server's smoke field of `pGrenade`
	// (volumetric smoke): sight is lost where the smoke gets thick enough.
	static CNEOSmokeLineOfSightBlocker *CreateForSmokeField( const Vector &origin, CBaseEntity *pGrenade );

	void Spawn() override {
		BaseClass::Spawn();
		SetSolid(SOLID_BBOX);
		SetCollisionBounds(m_mins, m_maxs);
		SetSolidFlags(FSOLID_CUSTOMRAYTEST | FSOLID_CUSTOMBOXTEST);
	}

	bool TestCollision(const Ray_t& ray, unsigned int fContentsMask, trace_t& tr) override
	{
		if (((fContentsMask & CONTENTS_BLOCKLOS) != 0) && !m_bNextEntitySeesThroughSmoke)
		{
			if (m_bFollowsSmokeField)
			{
				return TestSmokeField(ray, tr);
			}
			tr.fraction = 0.f; // NEO NOTE (Adam) Shouldn't returning true be enough? Should we calculate correct value for the fraction?
			return true;
		}
		return false;
	}

	bool TestHitboxes(const Ray_t& ray, unsigned int fContentsMask, trace_t& tr) override
	{ // NEO NOTE (Adam) Shotguns used to use hulls for half of their pellets, not sure if this los blocker would have affected that, check if/when any weapons do again
		return false;
	}

	void SetBounds(const Vector& mins, const Vector& maxs) { m_mins = mins; m_maxs = maxs; }

private:
	bool TestSmokeField(const Ray_t& ray, trace_t& tr) const;
	void UpdateSmokeBounds();

	Vector m_mins, m_maxs;
	bool m_bFollowsSmokeField = false;
	EHANDLE m_hGrenade;

	// for smoke LOS transparency toggle
	static bool m_bNextEntitySeesThroughSmoke;
	friend struct ScopedSmokeLOS;
};

struct ScopedSmokeLOS
{
	bool m_oldValue;

	// seeThrough == true  ? ignore smoke
	// seeThrough == false ? block smoke
	explicit ScopedSmokeLOS(bool seeThrough)
		: m_oldValue(CNEOSmokeLineOfSightBlocker::m_bNextEntitySeesThroughSmoke)
	{
		CNEOSmokeLineOfSightBlocker::m_bNextEntitySeesThroughSmoke = seeThrough;
	}

	~ScopedSmokeLOS()
	{
		CNEOSmokeLineOfSightBlocker::m_bNextEntitySeesThroughSmoke = m_oldValue;
	}
};

#endif // NEO_SMOKELOSBLOCKER_NEO_H
