#include "cbase.h"
#include "neo_player.h"
#include "bot/neo_bot.h"
#include "bot/behavior/neo_bot_grenade_throw_smoke.h"
#include "neo_smokelineofsightblocker.h"
#include "weapon_neobasecombatweapon.h"
#include "NextBotUtil.h"
#include "nav.h"

extern ConVar sv_neo_smoke_blocker_size;

static bool IsSmokeSightlineOpen( const Vector &vecEye, const Vector &vecThreatEye )
{
	NextBotVisionTraceFilter filter( nullptr, COLLISION_GROUP_NONE );
	trace_t tr;
	UTIL_TraceLine( vecEye, vecThreatEye, MASK_BLOCKLOS | CONTENTS_IGNORE_NODRAW_OPAQUE, &filter, &tr );
	return !tr.DidHit();
}

static Vector FindSmokeScreenPoint( CNEOBot *me, const Vector &vecScreenEye, const Vector &vecThreatEye, float flCloudExtent )
{
	const Vector vecToThreat = vecThreatEye - vecScreenEye;
	const float flDistance = vecToThreat.Length2D();
	if ( flDistance <= HalfHumanWidth )
	{
		return vec3_invalid;
	}

	const Vector vecScreen = vecScreenEye + vecToThreat * Min( 0.5f, flCloudExtent / flDistance );
	// A floor farther down would put the cloud below the sightline.
	NextBotTraceFilterIgnoreActors filter( nullptr, COLLISION_GROUP_NONE );
	trace_t tr;
	UTIL_TraceLine( vecScreen, vecScreen - Vector( 0.0f, 0.0f, flCloudExtent ), MASK_SOLID, &filter, &tr );
	if ( !tr.DidHit() || tr.startsolid )
	{
		return vec3_invalid;
	}

	constexpr float kFloorClearance = 1.0f;
	const Vector vecTarget = tr.endpos + Vector( 0.0f, 0.0f, kFloorClearance );
	if ( ( vecTarget - me->GetAbsOrigin() ).Length2DSqr() <= Square( HalfHumanWidth ) )
	{
		return vec3_invalid;
	}

	return me->IsThrowLineClear( vecTarget ) ? vecTarget : vec3_invalid;
}

namespace
{
bool IsActiveSmoke( CBaseEntity *pSmoke )
{
	return pSmoke && pSmoke->IsSolid() && !pSmoke->IsMarkedForDeletion();
}

bool IsObservedActiveSmoke( CNEOBot *me, CBaseEntity *pSmoke )
{
	return IsActiveSmoke( pSmoke ) &&
		( pSmoke->CollisionProp()->IsPointInBounds( me->GetAbsOrigin() ) ||
			me->FVisible( pSmoke->GetAbsOrigin(), MASK_VISIBLE ) );
}

bool IsPointInObservedSmoke( CNEOBot *me, const Vector &vecPoint )
{
	CBaseEntity *pSmoke = nullptr;
	while ( ( pSmoke = gEntList.FindEntityByClassname( pSmoke, SMOKELINEOFSIGHTBLOCKER_ENTITYNAME ) ) )
	{
		if ( pSmoke->CollisionProp()->IsPointInBounds( vecPoint ) && IsObservedActiveSmoke( me, pSmoke ) )
		{
			return true;
		}
	}
	return false;
}
} // namespace

CNEOBotGrenadeThrowSmoke::CNEOBotGrenadeThrowSmoke( CNEOBaseCombatWeapon *pWeapon, const CKnownEntity *threat )
	: CNEOBotGrenadeThrow( pWeapon, threat )
{
}

ActionResult< CNEOBot >	CNEOBotGrenadeThrowSmoke::OnStart( CNEOBot *me, Action< CNEOBot > *priorAction )
{
	// Pick the target before committing, so a covered spot costs no weapon swap or pin pull
	if ( m_vecThreatLastKnownPos != vec3_invalid && UpdateGrenadeTargeting( me, m_hGrenadeWeapon.Get() ) == THROW_TARGET_CANCEL )
	{
		return Done( m_vecTarget == vec3_invalid ? "No useful smoke target" : "Smoke target already covered" );
	}

	ActionResult< CNEOBot > result = CNEOBotGrenadeThrow::OnStart( me, priorAction );
	if ( result.IsDone() )
	{
		return result;
	}

	return Continue();
}

CNEOBotGrenadeThrow::ThrowTargetResult CNEOBotGrenadeThrowSmoke::UpdateGrenadeTargeting( CNEOBot *me, CNEOBaseCombatWeapon *pWeapon )
{
	Assert( m_vecThreatLastKnownPos != vec3_invalid );
	if ( m_vecTarget != vec3_invalid )
	{
		// A cloud observed while aiming can already cover the target.
		return IsPointInObservedSmoke( me, m_vecTarget ) ? THROW_TARGET_CANCEL : THROW_TARGET_READY;
	}

	const float flCloudExtent = static_cast< float >( sv_neo_smoke_blocker_size.GetInt() );
	if ( flCloudExtent <= 0.0f )
	{
		return THROW_TARGET_CANCEL;
	}

	const Vector vecThreatEye = m_vecThreatLastKnownPos + Vector( 0.0f, 0.0f, HumanEyeHeight );
	const Vector vecEye = me->EyePosition();
	const bool bExposed = IsSmokeSightlineOpen( vecEye, vecThreatEye );
	Vector vecScreenEye = vecEye;
	if ( !bExposed )
	{
		const PathFollower *path = me->GetCurrentPath();
		const Path::Segment *goal = ( path && path->IsValid() ) ? path->GetCurrentGoal() : nullptr;
		if ( !goal || goal->type != Path::ON_GROUND || !me->IsRangeLessThan( goal->pos, me->GetDesiredPathLookAheadRange() ) )
		{
			return THROW_TARGET_CANCEL;
		}

		vecScreenEye = goal->pos + me->GetViewOffset();
		if ( !IsSmokeSightlineOpen( vecScreenEye, vecThreatEye ) )
		{
			return THROW_TARGET_CANCEL;
		}
	}

	m_vecTarget = FindSmokeScreenPoint( me, vecScreenEye, vecThreatEye, flCloudExtent );
	if ( m_vecTarget == vec3_invalid || !CanReachThrowTarget( me, m_vecTarget ) )
	{
		const bool bCanScreenAtFeet = bExposed && me->GetViewOffset().z < flCloudExtent;
		m_vecTarget = bCanScreenAtFeet ? me->GetAbsOrigin() : vec3_invalid;
	}

	if ( m_vecTarget == vec3_invalid || me->IsCoveredByFriendlySmoke( m_vecTarget ) || IsPointInObservedSmoke( me, m_vecTarget ) )
	{
		return THROW_TARGET_CANCEL;
	}
	return THROW_TARGET_READY;
}

void CNEOBotGrenadeThrowSmoke::OnThrowReleased( CNEOBot *me )
{
	me->RememberSmokeTarget( m_vecTarget );
}
