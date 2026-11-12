#include "cbase.h"
#include "util.h"
#include "neo_smokelineofsightblocker.h"
#include "neo_smoke_volume.h"
#include "neo_smoke_fill.h"
#include "neo_smoke_shared.h"

// NOTE: This has to be the last file included!
#include "tier0/memdbgon.h"

// Assume most existing traceline checks were not designed with blocking smoke in mind
// e.g. Jeff the tank in nt_rogue_ctg should see through smoke
bool CNEOSmokeLineOfSightBlocker::m_bNextEntitySeesThroughSmoke{ true };

LINK_ENTITY_TO_CLASS(env_smokelineofsightblocker, CNEOSmokeLineOfSightBlocker);
BEGIN_DATADESC(CNEOSmokeLineOfSightBlocker)
    DEFINE_FIELD(m_mins, FIELD_VECTOR),
    DEFINE_FIELD(m_maxs, FIELD_VECTOR),
    DEFINE_FIELD(m_bFollowsSmokeField, FIELD_BOOLEAN),
    DEFINE_FIELD(m_hGrenade, FIELD_EHANDLE),
    DEFINE_THINKFUNC(UpdateSmokeBounds),
END_DATADESC()

ConVar sv_neo_smoke_blocker_duration("sv_neo_smoke_blocker_duration", "20.5", FCVAR_CHEAT, "Time when LOS block should disappear", true, 0.0, true, 30.0);

// Activation scheduled by CNEOGrenadeSmoke::SetupParticles
void CNEOSmokeLineOfSightBlocker::ActivateLOSBlocker()
{
	// Start blocking LOS
	Spawn();
	// Active blocking duration ends after smoke can be seen through by a player
	float duration = sv_neo_smoke_blocker_duration.GetFloat();
	SetThink(&CNEOSmokeLineOfSightBlocker::SUB_Remove);
	SetNextThink(gpGlobals->curtime + duration);
}

static const char *SMOKE_BOUNDS_CONTEXT = "SmokeBounds";
static constexpr float SMOKE_BOUNDS_POLL = 0.1f; // seconds

CNEOSmokeLineOfSightBlocker *CNEOSmokeLineOfSightBlocker::CreateForSmokeField(const Vector &origin, CBaseEntity *pGrenade)
{
	auto blocker = static_cast<CNEOSmokeLineOfSightBlocker*>(CreateEntityByName(SMOKELINEOFSIGHTBLOCKER_ENTITYNAME));
	if (!blocker)
	{
		return nullptr;
	}

	// Until the field is filled the smoke is the nominal dome.
	const float radius = CNeoSmokeFill::DOME_RADIUS;
	blocker->SetAbsOrigin(origin);
	blocker->SetBounds(Vector(-radius), Vector(radius));
	blocker->m_bFollowsSmokeField = true;
	blocker->m_hGrenade = pGrenade;
	blocker->Spawn();

	// The query follows the bloom and fade, so the blocker is live from the
	// start; the next think is the end of the smoke, which bots read as the
	// expiry of the smoke hazards they register.
	blocker->SetThink(&CNEOSmokeLineOfSightBlocker::SUB_Remove);
	blocker->SetNextThink(gpGlobals->curtime + NEO_VOLSMOKE_FADE_END);
	blocker->SetContextThink(&CNEOSmokeLineOfSightBlocker::UpdateSmokeBounds, gpGlobals->curtime + SMOKE_BOUNDS_POLL, SMOKE_BOUNDS_CONTEXT);
	return blocker;
}

void CNEOSmokeLineOfSightBlocker::UpdateSmokeBounds()
{
	Vector mins;
	Vector maxs;
	if (!NEOSmokeVolume_SmokeBox(m_hGrenade.Get(), mins, maxs))
	{
		if (m_hGrenade.Get())
		{
			SetNextThink(gpGlobals->curtime + SMOKE_BOUNDS_POLL, SMOKE_BOUNDS_CONTEXT);
		}
		return;
	}

	SetBounds(mins - GetAbsOrigin(), maxs - GetAbsOrigin());
	SetCollisionBounds(m_mins, m_maxs);
}

bool CNEOSmokeLineOfSightBlocker::TestSmokeField(const Ray_t& ray, trace_t& tr) const
{
	const Vector from = ray.m_Start + ray.m_StartOffset;
	const float fraction = NEOSmoke_SightBlockFraction(from, from + ray.m_Delta);
	if (fraction >= 1.0f)
	{
		return false;
	}

	tr.fraction = fraction;
	tr.endpos = from + ray.m_Delta * fraction;
	return true;
}
