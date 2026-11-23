#include "cbase.h"
#include "util.h"
#include "neo_smokelineofsightblocker.h"
#include "saverestore_utlvector.h"

// NOTE: This has to be the last file included!
#include "tier0/memdbgon.h"

// Assume most existing traceline checks were not designed with blocking smoke in mind
// e.g. Jeff the tank in nt_rogue_ctg should see through smoke
bool CNEOSmokeLineOfSightBlocker::m_bNextEntitySeesThroughSmoke{ true };

LINK_ENTITY_TO_CLASS(env_smokelineofsightblocker, CNEOSmokeLineOfSightBlocker);
BEGIN_DATADESC(CNEOSmokeLineOfSightBlocker)
    DEFINE_FIELD(m_mins, FIELD_VECTOR),
    DEFINE_FIELD(m_maxs, FIELD_VECTOR),
	DEFINE_FIELD( m_hThrower, FIELD_EHANDLE ),
	DEFINE_UTLVECTOR( m_seenBy, FIELD_EHANDLE ),
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

bool CNEOSmokeLineOfSightBlocker::IsActive()
{
	return GetSolid() == SOLID_BBOX && GetNextThink() > gpGlobals->curtime && !IsMarkedForDeletion();
}

void CNEOSmokeLineOfSightBlocker::NoteSeenBy( CBaseEntity *pObserver )
{
	m_seenBy.AddToTail( pObserver );
}

bool CNEOSmokeLineOfSightBlocker::HasBeenSeenBy( const CBaseEntity *pObserver ) const
{
	FOR_EACH_VEC( m_seenBy, i )
	{
		if ( m_seenBy[i].Get() == pObserver )
		{
			return true;
		}
	}

	return false;
}
