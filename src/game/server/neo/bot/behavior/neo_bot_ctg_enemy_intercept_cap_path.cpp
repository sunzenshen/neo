#include "cbase.h"
#include "neo_player.h"
#include "bot/neo_bot.h"
#include "bot/behavior/neo_bot_ctg_enemy_intercept_cap_path.h"
#include "bot/behavior/neo_bot_ctg_enemy.h"
#include "bot/behavior/neo_bot_ctg_enemy_chase.h"
#include "bot/neo_bot_path_compute.h"
#include "neo_gamerules.h"

ConVar sv_neo_bot_ctg_enemy_intercept_replan_seconds( "sv_neo_bot_ctg_enemy_intercept_replan_seconds", "15", FCVAR_CHEAT,
	"CTG: seconds between an intercepting bot re-plotting the enemy ghost carrier's route to its cap, to catch it veering toward another.",
	true, 1.0f, false, 0.0f );

//---------------------------------------------------------------------------------------------
bool CNEOBotCtgEnemyInterceptCapPath::RecomputeCarrierRoute( CNEOBot *me )
{
	CNEO_Player *pGhostCarrier = CNEOBotCtgEnemy::EnemyGhostCarrier( me );
	if ( !pGhostCarrier )
	{
		return false;
	}

	float flRouteLength = 0.0f;
	return CNEOBotCtgEnemy::CarrierRouteToGoal( me, pGhostCarrier, m_carrierRoute, flRouteLength, m_goalPos );
}

//---------------------------------------------------------------------------------------------
ActionResult< CNEOBot > CNEOBotCtgEnemyInterceptCapPath::OnStart( CNEOBot *me, Action< CNEOBot > *priorAction )
{
	m_path.SetMinLookAheadDistance( me->GetDesiredPathLookAheadRange() );

	if ( !RecomputeCarrierRoute( me ) )
	{
		return ChangeTo( new CNEOBotCtgEnemyChase, "No carrier route to intercept" );
	}

	if ( !CNEOBotPathCompute( me, m_path, m_goalPos, FASTEST_ROUTE ) )
	{
		return ChangeTo( new CNEOBotCtgEnemyChase, "No path toward the carrier's cap" );
	}

	m_replanTimer.Start( sv_neo_bot_ctg_enemy_intercept_replan_seconds.GetFloat() );

	return Continue();
}

//---------------------------------------------------------------------------------------------
ActionResult< CNEOBot > CNEOBotCtgEnemyInterceptCapPath::Update( CNEOBot *me, float interval )
{
	if ( NEORules()->GetGameType() != NEO_GAME_TYPE_CTG )
	{
		return Done( "Game mode is no longer CTG" );
	}

	CNEO_Player *pGhostCarrier = CNEOBotCtgEnemy::EnemyGhostCarrier( me );
	if ( !pGhostCarrier )
	{
		return Done( "No enemy ghost carrier" );
	}

	// The interception is complete the moment we are standing on the carrier's own predicted
	// route: from here the direct chase is the right tool.
	CNavArea *pMyArea = me->GetLastKnownArea();
	if ( pMyArea && m_carrierRoute.HasElement( pMyArea ) )
	{
		return ChangeTo( new CNEOBotCtgEnemyChase, "Reached the carrier's route - chasing" );
	}

	// Slow re-plot: the carrier may have veered toward a different cap since we set out.
	if ( m_replanTimer.IsElapsed() )
	{
		m_replanTimer.Start( sv_neo_bot_ctg_enemy_intercept_replan_seconds.GetFloat() );

		if ( RecomputeCarrierRoute( me )
			&& !CNEOBotPathCompute( me, m_path, m_goalPos, FASTEST_ROUTE ) )
		{
			return ChangeTo( new CNEOBotCtgEnemyChase, "Lost the path toward the carrier's cap" );
		}
	}

	m_path.Update( me );
	if ( !m_path.IsValid() )
	{
		// Reached the cap (or the path broke) without ever crossing the carrier's route -- it
		// took a very different line. Fall back to the direct chase.
		return ChangeTo( new CNEOBotCtgEnemyChase, "Arrived without intercepting - chasing" );
	}

	return Continue();
}
