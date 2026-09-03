#include "cbase.h"
#include "neo_player.h"
#include "bot/neo_bot.h"
#include "bot/behavior/neo_bot_ctg_enemy_chase.h"
#include "bot/behavior/neo_bot_ctg_enemy.h"
#include "bot/behavior/neo_bot_attack.h"
#include "bot/neo_bot_path_compute.h"
#include "neo_gamerules.h"

ConVar sv_neo_bot_ctg_enemy_chase_replan_seconds( "sv_neo_bot_ctg_enemy_chase_replan_seconds", "20", FCVAR_CHEAT,
	"CTG: seconds between a chasing bot re-checking whether to take the fastest or the default route to the enemy ghost carrier.",
	true, 1.0f, false, 0.0f );

//---------------------------------------------------------------------------------------------
ActionResult< CNEOBot > CNEOBotCtgEnemyChase::OnStart( CNEOBot *me, Action< CNEOBot > *priorAction )
{
	m_chasePath.SetMinLookAheadDistance( me->GetDesiredPathLookAheadRange() );
	m_routeTypeTimer.Invalidate();
	m_routeType = DEFAULT_ROUTE;

	return Continue();
}

//---------------------------------------------------------------------------------------------
ActionResult< CNEOBot > CNEOBotCtgEnemyChase::Update( CNEOBot *me, float interval )
{
	CNEO_Player *pGhostCarrier = CNEOBotCtgEnemy::EnemyGhostCarrier( me );
	if ( !pGhostCarrier )
	{
		return Done( "No enemy ghost carrier" );
	}

	// Slow re-evaluation of the route type.
	if ( m_routeTypeTimer.IsElapsed() )
	{
		m_routeTypeTimer.Start( sv_neo_bot_ctg_enemy_chase_replan_seconds.GetFloat() );

		// A cut-off exists exactly when there is somewhere on the carrier's route we can still beat
		// it to, which is the same question as "am I ahead of it or behind it".
		CNEOBotCtgEnemy::CutOff cutOff;
		m_routeType = CNEOBotCtgEnemy::FindCutOff( me, pGhostCarrier, cutOff )
			? DEFAULT_ROUTE : FASTEST_ROUTE;
	}

	const CKnownEntity *threat = me->GetVisionInterface()->GetPrimaryKnownThreat( true );
	if ( threat && !threat->IsObsolete() && me->GetIntentionInterface()->ShouldAttack( me, threat ) )
	{
		return SuspendFor( new CNEOBotAttack( pGhostCarrier->GetAbsOrigin() ), "Attacking ghoster team" );
	}

	CNEOBotPathUpdateChase( me, m_chasePath, pGhostCarrier, m_routeType );

	return Continue();
}

//---------------------------------------------------------------------------------------------
EventDesiredResult< CNEOBot > CNEOBotCtgEnemyChase::OnStuck( CNEOBot *me )
{
	return TryContinue();
}

//---------------------------------------------------------------------------------------------
EventDesiredResult< CNEOBot > CNEOBotCtgEnemyChase::OnMoveToSuccess( CNEOBot *me, const Path *path )
{
	return TryContinue();
}

//---------------------------------------------------------------------------------------------
EventDesiredResult< CNEOBot > CNEOBotCtgEnemyChase::OnMoveToFailure( CNEOBot *me, const Path *path, MoveToFailureType reason )
{
	return TryContinue();
}
