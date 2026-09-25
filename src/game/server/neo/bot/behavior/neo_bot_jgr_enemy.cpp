#include "cbase.h"
#include "neo_player.h"
#include "bot/neo_bot.h"
#include "bot/behavior/neo_bot_jgr_enemy.h"
#include "bot/neo_bot_path_compute.h"
#include "neo_gamerules.h"
#include "bot/behavior/neo_bot_attack.h"

//---------------------------------------------------------------------------------------------
CNEOBotJgrEnemy::CNEOBotJgrEnemy( void )
{
}

//---------------------------------------------------------------------------------------------
ActionResult< CNEOBot >	CNEOBotJgrEnemy::OnStart( CNEOBot *me, Action< CNEOBot > *priorAction )
{
	return Continue();
}

//---------------------------------------------------------------------------------------------
ActionResult< CNEOBot >	CNEOBotJgrEnemy::Update( CNEOBot *me, float interval )
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

	if ( pJuggernaut->GetTeamNumber() == me->GetTeamNumber() )
	{
		return Done( "Juggernaut is friendly" );
	}

	const CKnownEntity *threat = me->GetVisionInterface()->GetPrimaryKnownThreat(true);
	if ( threat && !threat->IsObsolete() && me->GetIntentionInterface()->ShouldAttack( me, threat ) )
	{
		return SuspendFor( new CNEOBotAttack( pJuggernaut->GetAbsOrigin() ), "Attacking threat en route to juggernaut" );
	}

	// Chase the Juggernaut
	CNEOBotPathUpdateChase( me, m_chasePath, pJuggernaut, DEFAULT_ROUTE );

	return Continue();
}

//---------------------------------------------------------------------------------------------
ActionResult< CNEOBot > CNEOBotJgrEnemy::OnResume( CNEOBot *me, Action< CNEOBot > *interruptingAction )
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
EventDesiredResult< CNEOBot > CNEOBotJgrEnemy::OnStuck( CNEOBot *me )
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
EventDesiredResult< CNEOBot > CNEOBotJgrEnemy::OnMoveToSuccess( CNEOBot *me, const Path *path )
{
	return TryContinue();
}

//---------------------------------------------------------------------------------------------
EventDesiredResult< CNEOBot > CNEOBotJgrEnemy::OnMoveToFailure( CNEOBot *me, const Path *path, MoveToFailureType reason )
{
	return TryContinue();
}
