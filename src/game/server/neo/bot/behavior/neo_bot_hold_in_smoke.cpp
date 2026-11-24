#include "cbase.h"
#include "basegrenade_shared.h"
#include "bot/behavior/neo_bot_hold_in_smoke.h"
#include "bot/behavior/neo_bot_retreat_to_cover.h"
#include "bot/neo_bot_path_compute.h"
#include "nav_mesh.h"
#include "neo_smokelineofsightblocker.h"

#include "tier0/memdbgon.h"

extern ConVar sv_neo_grenade_fuse_timer;
extern ConVar sv_neo_smoke_blocker_delay;

namespace
{
constexpr float kSmokeNoticeRange = 200.0f;
constexpr float kSmokeSettleAllowance = 1.0f;
constexpr float kSmokeTravelTimeout = 6.0f;

void HoldPosition( CNEOBot *me )
{
	me->ReleaseForwardButton();
	me->ReleaseBackwardButton();
	me->ReleaseLeftButton();
	me->ReleaseRightButton();
	me->SetMoveDirection( vec2_origin );
}

bool IsActiveSmoke( CBaseEntity *pSmoke )
{
	return pSmoke && pSmoke->IsSolid() && !pSmoke->IsMarkedForDeletion();
}

// Each blocker is centered on its settled grenade. Use this association only to break ties.
bool IsOwnSmoke( CNEOBot *me, const Vector &vecCenter )
{
	CBaseEntity *pGrenade = nullptr;
	while ( ( pGrenade = gEntList.FindEntityByClassname( pGrenade, "neo_grenade_smoke" ) ) )
	{
		if ( !pGrenade->IsSolid() && pGrenade->GetAbsOrigin() == vecCenter &&
			static_cast< CBaseGrenade * >( pGrenade )->GetOriginalThrower() == me )
		{
			return true;
		}
	}
	return false;
}

CBaseEntity *FindNearbySmoke( CNEOBot *me )
{
	CBaseEntity *pClosest = nullptr;
	float flClosestDistSq = Square( kSmokeNoticeRange );
	const Vector &vecBotPos = me->GetAbsOrigin();
	CBaseEntity *pSmoke = nullptr;
	while ( ( pSmoke = gEntList.FindEntityByClassname( pSmoke, SMOKELINEOFSIGHTBLOCKER_ENTITYNAME ) ) )
	{
		const Vector &vecCenter = pSmoke->GetAbsOrigin();
		const float flDistSq = vecBotPos.DistToSqr( vecCenter );
		if ( flDistSq > flClosestDistSq || !IsActiveSmoke( pSmoke ) )
		{
			continue;
		}
		if ( !pSmoke->CollisionProp()->IsPointInBounds( vecBotPos ) && !me->FVisible( vecCenter, MASK_VISIBLE ) )
		{
			continue;
		}

		if ( !pClosest || flDistSq < flClosestDistSq ||
			( vecCenter != pClosest->GetAbsOrigin() && IsOwnSmoke( me, vecCenter ) && !IsOwnSmoke( me, pClosest->GetAbsOrigin() ) ) )
		{
			pClosest = pSmoke;
			flClosestDistSq = flDistSq;
		}
	}
	return pClosest;
}

bool ComputeSmokePath( CNEOBot *me, PathFollower &path, CBaseEntity *pSmoke )
{
	const Vector &vecCenter = pSmoke->GetAbsOrigin();
	const CNavArea *pArea = TheNavMesh->GetNearestNavArea( vecCenter, false, pSmoke->BoundingRadius(), true, false, me->GetTeamNumber() );
	if ( !pArea )
	{
		return false;
	}

	Vector vecGoal;
	pArea->GetClosestPointOnArea( vecCenter, &vecGoal );
	return pSmoke->CollisionProp()->IsPointInBounds( vecGoal ) &&
		CNEOBotPathCompute( me, path, vecGoal, FASTEST_ROUTE, PATH_NO_LENGTH_LIMIT, false, true ) &&
		CNEOBotPathReachesGoal( path );
}
} // namespace

ActionResult< CNEOBot > CNEOBotHoldInSmoke::OnStart( CNEOBot *me, Action< CNEOBot > *priorAction )
{
	m_timeout.Start( sv_neo_grenade_fuse_timer.GetFloat() + sv_neo_smoke_blocker_delay.GetFloat() + kSmokeSettleAllowance );
	m_path.SetMinLookAheadDistance( me->GetDesiredPathLookAheadRange() );
	HoldPosition( me );
	return Continue();
}

ActionResult< CNEOBot > CNEOBotHoldInSmoke::Update( CNEOBot *me, float interval )
{
	if ( me->GetIntentionInterface()->ShouldHurry( me ) == ANSWER_YES )
	{
		return Done( "Need to hurry" );
	}

	me->GetBodyInterface()->AimHeadTowards( m_vecThreatPos + Vector( 0, 0, HumanEyeHeight ),
		IBody::IMPORTANT, 0.2f, nullptr, "Watching for threats from smoke" );

	// A stale handle still identifies a cloud that was selected and then removed.
	if ( !m_hSmoke.ChangedFrom( nullptr ) )
	{
		m_hSmoke = FindNearbySmoke( me );
		if ( !m_hSmoke )
		{
			if ( m_timeout.IsElapsed() )
			{
				return Done( "No nearby smoke bloomed" );
			}
			HoldPosition( me );
			return Continue();
		}

		if ( !m_hSmoke->CollisionProp()->IsPointInBounds( me->GetAbsOrigin() ) &&
			!ComputeSmokePath( me, m_path, m_hSmoke.Get() ) )
		{
			return Done( "No path into smoke" );
		}
		m_timeout.Start( kSmokeTravelTimeout );
	}

	if ( !IsActiveSmoke( m_hSmoke.Get() ) )
	{
		return Done( "Smoke cleared" );
	}

	if ( m_hSmoke->CollisionProp()->IsPointInBounds( me->GetAbsOrigin() ) )
	{
		m_bEnteredSmoke = true;
		m_path.Invalidate();
		HoldPosition( me );
		return Continue();
	}
	if ( m_bEnteredSmoke )
	{
		return Done( "Left the smoke" );
	}
	if ( !m_path.IsValid() || m_timeout.IsElapsed() )
	{
		return Done( "Failed to reach smoke" );
	}

	m_path.Update( me );
	return Continue();
}

EventDesiredResult< CNEOBot > CNEOBotHoldInSmoke::OnInjured( CNEOBot *me, const CTakeDamageInfo &info )
{
	CBaseEntity *pAttacker = info.GetAttacker();
	if ( !pAttacker || !me->IsEnemy( pAttacker ) )
	{
		return TryContinue();
	}
	return TryChangeTo( new CNEOBotRetreatToCover( m_hSmoke ), RESULT_IMPORTANT, "Enemy hit during smoke hold" );
}
