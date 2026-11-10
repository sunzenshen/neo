#include "cbase.h"
#include "neo_player.h"
#include "neo_gamerules.h"
#include "bot/neo_bot.h"
#include "bot/neo_bot_trade.h"
#include "bot/neo_bot_path_compute.h"
#include "bot/neo_bot_path_cost.h"
#include "bot/behavior/neo_bot_trade_response.h"
#include "bot/behavior/neo_bot_attack.h"
#include "bot/behavior/neo_bot_ctg_enemy.h"
#include "nav_mesh.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

// Areas this close to where the teammate fell are the lane it died in
static constexpr float DEATH_LANE_RADIUS = 200.0f;

// Extra cost per unit of travel in the death lane: a route up to this much longer elsewhere is taken instead
static constexpr float DEATH_LANE_EXTRA_COST = 1.0f;

// A trade is a local answer; a goal farther than this by path is not worth leaving the round plan for
static constexpr float MAX_TRADE_PATH_LENGTH = 3000.0f;

// Close enough to the goal to count it reached
static constexpr float GOAL_REACHED_RANGE = 64.0f;

static constexpr float REPATH_INTERVAL = 1.0f;

//---------------------------------------------------------------------------------------------
// Fastest route, but the lane the teammate died in costs more unless the goal itself is in it
class CNEOBotTradePathCost : public CNEOBotPathCost
{
public:
	CNEOBotTradePathCost( CNEOBot *me, const Vector &vecDeathSpot, const Vector &vecGoal )
		: CNEOBotPathCost( me, FASTEST_ROUTE ), m_vecDeathSpot( vecDeathSpot ), m_vecGoal( vecGoal )
	{
	}

	virtual float operator()( CNavArea *baseArea, CNavArea *fromArea, const CNavLadder *ladder, const CFuncElevator *elevator, float length ) const override
	{
		const float flCost = CNEOBotPathCost::operator()( baseArea, fromArea, ladder, elevator, length );
		if ( flCost <= 0.0f || !fromArea )
		{
			return flCost;
		}

		const float flRadiusSq = DEATH_LANE_RADIUS * DEATH_LANE_RADIUS;
		const Vector &vecCenter = baseArea->GetCenter();
		if ( vecCenter.DistToSqr( m_vecDeathSpot ) > flRadiusSq || vecCenter.DistToSqr( m_vecGoal ) < flRadiusSq )
		{
			return flCost;
		}

		const float flStepCost = flCost - fromArea->GetCostSoFar();
		return flCost + flStepCost * DEATH_LANE_EXTRA_COST;
	}

private:
	Vector m_vecDeathSpot;
	Vector m_vecGoal;
};

//---------------------------------------------------------------------------------------------
CNEOBotTradeResponse::CNEOBotTradeResponse( const Vector &vecGoal, const Vector &vecDeathSpot, bool bKillerSeen, int iVictim )
	: m_vecGoal( vecGoal ),
	m_vecDeathSpot( vecDeathSpot ),
	m_bKillerSeen( bKillerSeen ),
	m_iVictim( iVictim ),
	m_bEngaged( false ),
	m_bReachedGoal( false ),
	m_pszEndReason( "" )
{
}

//---------------------------------------------------------------------------------------------
bool CNEOBotTradeResponse::ComputePath( CNEOBot *me )
{
	CNEOBotTradePathCost cost( me, m_vecDeathSpot, m_vecGoal );
	if ( m_path.Compute( me, m_vecGoal, cost, MAX_TRADE_PATH_LENGTH, false ) && m_path.IsValid() )
	{
		return true;
	}

	return CNEOBotPathCompute( me, m_path, m_vecGoal, FASTEST_ROUTE, MAX_TRADE_PATH_LENGTH, false );
}

//---------------------------------------------------------------------------------------------
ActionResult< CNEOBot >	CNEOBotTradeResponse::OnStart( CNEOBot *me, Action< CNEOBot > *priorAction )
{
	m_window.Start( NEOBotTrade::RESPONSE_WINDOW );
	NEOBotTrade::OnResponseStart( me, gpGlobals->curtime + NEOBotTrade::RESPONSE_WINDOW );

	m_path.SetMinLookAheadDistance( me->GetDesiredPathLookAheadRange() );
	m_repathTimer.Start( REPATH_INTERVAL );
	if ( !ComputePath( me ) )
	{
		m_pszEndReason = "nopath";
		return Done( "No short path to the trade spot" );
	}

	return Continue();
}

//---------------------------------------------------------------------------------------------
ActionResult< CNEOBot >	CNEOBotTradeResponse::Update( CNEOBot *me, float interval )
{
	if ( NEORules()->IsRoundOver() )
	{
		m_pszEndReason = "roundover";
		return Done( "Round over" );
	}

	if ( m_window.IsElapsed() )
	{
		m_pszEndReason = "window";
		return Done( "Trade window over" );
	}

	// The ghost changes hands: the CTG behaviors below take over
	if ( me->IsCarryingGhost() || ( !NEOBotTrade::RespondsDuringEnemyCarry() && CNEOBotCtgEnemy::EnemyGhostCarrier( me ) ) )
	{
		m_pszEndReason = "ghost";
		return Done( "Ghost carrier changed" );
	}

	if ( me->GetVisionInterface()->GetPrimaryKnownThreat( true ) )
	{
		m_bEngaged = true;
		return SuspendFor( new CNEOBotAttack, "Trading a teammate's death" );
	}

	if ( m_bReachedGoal || me->IsRangeLessThan( m_vecGoal, GOAL_REACHED_RANGE ) )
	{
		m_pszEndReason = "reached";
		return Done( "Reached the trade spot" );
	}

	if ( !m_path.IsValid() && m_repathTimer.IsElapsed() )
	{
		m_repathTimer.Start( REPATH_INTERVAL );
		if ( !ComputePath( me ) )
		{
			m_pszEndReason = "nopath";
			return Done( "No short path to the trade spot" );
		}
	}

	m_path.Update( me );

	return Continue();
}

//---------------------------------------------------------------------------------------------
ActionResult< CNEOBot > CNEOBotTradeResponse::OnResume( CNEOBot *me, Action< CNEOBot > *interruptingAction )
{
	m_path.Invalidate();
	m_repathTimer.Invalidate();

	return Continue();
}

//---------------------------------------------------------------------------------------------
void CNEOBotTradeResponse::OnEnd( CNEOBot *me, Action< CNEOBot > *nextAction )
{
	NEOBotTrade::OnResponseEnd( me );

	if ( NEOBotTrade::IsLogging() )
	{
		Msg( "NEO_TRADE_END t=%.2f team=%d responder=%d victim=%d killer_seen=%d engaged=%d reason=%s left=%.1f\n",
			gpGlobals->curtime, me->GetTeamNumber(), me->entindex(), m_iVictim, m_bKillerSeen ? 1 : 0,
			m_bEngaged ? 1 : 0, m_pszEndReason[ 0 ] ? m_pszEndReason : "ended", m_window.GetRemainingTime() );
	}
}

//---------------------------------------------------------------------------------------------
EventDesiredResult< CNEOBot > CNEOBotTradeResponse::OnStuck( CNEOBot *me )
{
	m_path.Invalidate();
	return TryContinue();
}

//---------------------------------------------------------------------------------------------
EventDesiredResult< CNEOBot > CNEOBotTradeResponse::OnMoveToSuccess( CNEOBot *me, const Path *path )
{
	m_bReachedGoal = true;
	return TryContinue();
}

//---------------------------------------------------------------------------------------------
EventDesiredResult< CNEOBot > CNEOBotTradeResponse::OnMoveToFailure( CNEOBot *me, const Path *path, MoveToFailureType reason )
{
	m_path.Invalidate();
	return TryContinue();
}

//---------------------------------------------------------------------------------------------
QueryResultType CNEOBotTradeResponse::ShouldHurry( const INextBot *me ) const
{
	return m_window.IsElapsed() ? ANSWER_UNDEFINED : ANSWER_YES;
}
