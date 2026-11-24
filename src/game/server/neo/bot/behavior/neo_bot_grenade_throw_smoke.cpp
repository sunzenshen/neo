#include "cbase.h"
#include "neo_player.h"
#include "bot/neo_bot.h"
#include "bot/behavior/neo_bot_grenade_throw_smoke.h"
#include "weapon_neobasecombatweapon.h"

CNEOBotGrenadeThrowSmoke::CNEOBotGrenadeThrowSmoke( CNEOBaseCombatWeapon *pWeapon, const CKnownEntity *threat )
	: CNEOBotGrenadeThrow( pWeapon, threat )
{
}

EventDesiredResult< CNEOBot > CNEOBotGrenadeThrowSmoke::OnMoveToSuccess( CNEOBot *me, const Path *path )
{
	if ( path == &m_PathFollower )
	{
		m_vantageArea = nullptr;
	}
	return TryContinue();
}

CNEOBotGrenadeThrow::ThrowTargetResult CNEOBotGrenadeThrowSmoke::UpdateGrenadeTargeting( CNEOBot *me, CNEOBaseCombatWeapon *pWeapon )
{
	Assert( m_vecThreatLastKnownPos != vec3_invalid );

	if ( m_vecTarget != vec3_invalid )
	{
		return THROW_TARGET_READY;
	}

	if ( !m_scanTimer.IsElapsed() )
	{
		return THROW_TARGET_WAIT;
	}

	const Vector vecVelocity = me->GetAbsVelocity();
	m_vecTarget = me->FindVisibleThrowPointNear( m_vecThreatLastKnownPos, me->GetLastKnownArea(),
		me->EyePosition(), &vecVelocity );
	if ( m_vecTarget == vec3_invalid )
	{
		m_scanTimer.Start( 0.2f );
		return THROW_TARGET_WAIT;
	}

	return THROW_TARGET_READY;
}

