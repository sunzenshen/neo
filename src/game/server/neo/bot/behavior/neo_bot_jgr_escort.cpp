#include "cbase.h"
#include "neo_player.h"
#include "bot/neo_bot.h"
#include "bot/behavior/neo_bot_attack.h"
#include "bot/behavior/neo_bot_jgr_escort.h"
#include "bot/neo_bot_path_compute.h"
#include "neo_gamerules.h"

extern ConVar sv_neo_grenade_blast_radius;

//---------------------------------------------------------------------------------------------
CNEOBotJgrEscort::CNEOBotJgrEscort( void )
{
}

//---------------------------------------------------------------------------------------------
ActionResult< CNEOBot >	CNEOBotJgrEscort::OnStart( CNEOBot *me, Action< CNEOBot > *priorAction )
{
	m_chasePath.Invalidate();
	return Continue();
}



//---------------------------------------------------------------------------------------------
ActionResult< CNEOBot >	CNEOBotJgrEscort::Update( CNEOBot *me, float interval )
{
	const int iJuggernautPlayer = NEORules()->GetJuggernautPlayer();
	if ( iJuggernautPlayer <= 0 )
	{
		return Done( "No juggernaut player" );
	}

	CNEO_Player* pJuggernaut = ToNEOPlayer( UTIL_PlayerByIndex( iJuggernautPlayer ) );
	if ( !pJuggernaut )
	{
		return Done( "Juggernaut is invalid" );
	}

	if ( !pJuggernaut->IsAlive() )
	{
		return Done( "Juggernaut is dead" );
	}

	if ( pJuggernaut->GetTeamNumber() != me->GetTeamNumber() )
	{
		return Done( "Juggernaut is not friendly" );
	}

	// Check for own threats
	const CKnownEntity *myThreat = me->GetVisionInterface()->GetPrimaryKnownThreat(true);
	if ( myThreat && myThreat->GetEntity() && myThreat->GetEntity()->IsAlive() )
	{
		return SuspendFor( new CNEOBotAttack( pJuggernaut->GetAbsOrigin() ), "Engaging enemy to lure them to Juggernaut" );
	}

	// Just follow the Juggernaut
	// If too close, stop moving to avoid crowding
	float flDistSq = me->GetAbsOrigin().DistToSqr( pJuggernaut->GetAbsOrigin() );
	float flSafeRadius = sv_neo_grenade_blast_radius.GetFloat();
	float flSafeRadiusSq = flSafeRadius * flSafeRadius;

	if ( flDistSq < flSafeRadiusSq )
	{
		// Too close, stop moving
		if ( m_chasePath.IsValid() )
		{
			m_chasePath.Invalidate();
		}
		return Continue();
	}

	CNEOBotPathUpdateChase( me, m_chasePath, pJuggernaut, DEFAULT_ROUTE );

	return Continue();
}

//---------------------------------------------------------------------------------------------
ActionResult< CNEOBot > CNEOBotJgrEscort::OnResume( CNEOBot *me, Action< CNEOBot > *interruptingAction )
{
	// NEO-HARNESS-TEMP research arm (neo_bot_chase_stuck_repath, 2026-09-23): the chase path only
	// re-plans when the juggernaut moves away from its end, not when the interrupting action moved us
	extern ConVar neo_bot_chase_stuck_repath;
	if ( neo_bot_chase_stuck_repath.GetBool() )
	{
		m_chasePath.Invalidate();
	}

	return Continue();
}

//---------------------------------------------------------------------------------------------
EventDesiredResult< CNEOBot > CNEOBotJgrEscort::OnStuck( CNEOBot *me )
{
	// NEO-HARNESS-TEMP research arm (neo_bot_chase_stuck_repath, 2026-09-23): replan from where we are
	extern ConVar neo_bot_chase_stuck_repath;
	if ( neo_bot_chase_stuck_repath.GetBool() )
	{
		m_chasePath.Invalidate();
	}

	return TryContinue();
}

//---------------------------------------------------------------------------------------------
EventDesiredResult< CNEOBot > CNEOBotJgrEscort::OnMoveToSuccess( CNEOBot *me, const Path *path )
{
	return TryContinue();
}

//---------------------------------------------------------------------------------------------
EventDesiredResult< CNEOBot > CNEOBotJgrEscort::OnMoveToFailure( CNEOBot *me, const Path *path, MoveToFailureType reason )
{
	return TryContinue();
}

//---------------------------------------------------------------------------------------------
// Hurry back to the friendly juggernaut if there are enemies closer to it
QueryResultType CNEOBotJgrEscort::ShouldHurry( const INextBot *me ) const
{
	const CNEOBot *meBot = static_cast<const CNEOBot *>( me );

	CNEO_Player *pJuggernaut = ToNEOPlayer( UTIL_PlayerByIndex( NEORules()->GetJuggernautPlayer() ) );
	if ( !pJuggernaut || !pJuggernaut->IsAlive() || pJuggernaut->GetTeamNumber() != meBot->GetTeamNumber() || pJuggernaut == meBot )
	{
		return ANSWER_UNDEFINED;
	}

	return meBot->IsKnownEnemyNearer( pJuggernaut->GetAbsOrigin(), false ) ? ANSWER_YES : ANSWER_UNDEFINED;
}
