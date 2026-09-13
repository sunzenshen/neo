#include "cbase.h"
#include "neo_player.h"
#include "bot/neo_bot.h"
#include "bot/behavior/neo_bot_ctg_enemy_intercept_cap_path.h"
#include "bot/behavior/neo_bot_ctg_enemy.h"
#include "bot/behavior/neo_bot_ctg_enemy_chase.h"
#include "bot/neo_bot_path_compute.h"
#include "neo_gamerules.h"

ConVar sv_neo_bot_ctg_enemy_intercept_replan_seconds( "sv_neo_bot_ctg_enemy_intercept_replan_seconds", "5", FCVAR_CHEAT,
	"CTG: seconds between an intercepting bot re-picking its cut-off on the enemy ghost carrier's route, to catch the carrier taking a line it did not predict.",
	true, 0.5f, false, 0.0f );

// How close to the cut-off area's centre counts as being there. A nav area is wider than a player,
// so standing anywhere in it is close enough to meet whoever comes through.
static const float CTG_ENEMY_CUTOFF_ARRIVAL_TOLERANCE = 64.0f;

//---------------------------------------------------------------------------------------------
CNEOBotCtgEnemyInterceptCapPath::CNEOBotCtgEnemyInterceptCapPath( const CNEOBotCtgEnemy::CutOff &cutOff )
	: m_cutOff( cutOff )
{
}

//---------------------------------------------------------------------------------------------
bool CNEOBotCtgEnemyInterceptCapPath::RepathToCutOff( CNEOBot *me )
{
	// No explicit path reservation for the cut-off area: CNEOBotPathCompute reserves every area on
	// the path it computes, the destination included, and it starts by releasing this bot's
	// previous claims. Anything claimed here would be dropped again a line later.
	return CNEOBotPathCompute( me, m_path, m_cutOff.vecPos, FASTEST_ROUTE );
}

//---------------------------------------------------------------------------------------------
// Re-pick the cut-off. Returns false when there is no longer one to head for, in which case the
// caller drops to the chase rather than walking on with a plan it can no longer check.
bool CNEOBotCtgEnemyInterceptCapPath::Replan( CNEOBot *me, CNEO_Player *pGhostCarrier )
{
	CNEOBotCtgEnemy::CutOff cutOff;
	if ( !CNEOBotCtgEnemy::FindCutOff( me, pGhostCarrier, cutOff ) )
	{
		return false;
	}

	const bool bMoved = ( cutOff.pArea != m_cutOff.pArea );

	m_cutOff = cutOff;

	return !bMoved || RepathToCutOff( me );
}

//---------------------------------------------------------------------------------------------
ActionResult< CNEOBot > CNEOBotCtgEnemyInterceptCapPath::OnStart( CNEOBot *me, Action< CNEOBot > *priorAction )
{
	m_path.SetMinLookAheadDistance( me->GetDesiredPathLookAheadRange() );

	if ( !RepathToCutOff( me ) )
	{
		return ChangeTo( new CNEOBotCtgEnemyChase, "No path to the cut-off" );
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

	if ( m_replanTimer.IsElapsed() )
	{
		m_replanTimer.Start( sv_neo_bot_ctg_enemy_intercept_replan_seconds.GetFloat() );

		if ( !Replan( me, pGhostCarrier ) )
		{
			return ChangeTo( new CNEOBotCtgEnemyChase, "Lost the cut-off - chasing" );
		}
	}

	const bool bArrived = ( me->GetLastKnownArea() == m_cutOff.pArea )
		|| ( ( me->GetAbsOrigin() - m_cutOff.vecPos ).Length2DSqr() < Square( CTG_ENEMY_CUTOFF_ARRIVAL_TOLERANCE ) );

	if ( bArrived )
	{
		// No hold, no ambush: the cut-off's value is in the ground it wins, not in waiting on it.
		// Once we are standing on it, chase from here.
		return ChangeTo( new CNEOBotCtgEnemyChase, "Arrived at the cut-off - chasing" );
	}

	m_path.Update( me );
	if ( !m_path.IsValid() )
	{
		return ChangeTo( new CNEOBotCtgEnemyChase, "Lost the path to the cut-off - chasing" );
	}

	return Continue();
}

//---------------------------------------------------------------------------------------------
EventDesiredResult< CNEOBot > CNEOBotCtgEnemyInterceptCapPath::OnStuck( CNEOBot *me )
{
	if ( !RepathToCutOff( me ) )
	{
		return TryChangeTo( new CNEOBotCtgEnemyChase, RESULT_TRY, "Stuck with no way to the cut-off - chasing" );
	}

	return TryContinue();
}

//---------------------------------------------------------------------------------------------
EventDesiredResult< CNEOBot > CNEOBotCtgEnemyInterceptCapPath::OnMoveToFailure( CNEOBot *me, const Path *path, MoveToFailureType reason )
{
	if ( !RepathToCutOff( me ) )
	{
		return TryChangeTo( new CNEOBotCtgEnemyChase, RESULT_TRY, "Cannot reach the cut-off - chasing" );
	}

	return TryContinue();
}
