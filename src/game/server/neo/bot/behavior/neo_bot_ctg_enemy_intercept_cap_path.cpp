#include "cbase.h"
#include "neo_player.h"
#include "bot/neo_bot.h"
#include "bot/behavior/neo_bot_ctg_enemy_intercept_cap_path.h"
#include "bot/behavior/neo_bot_ctg_enemy.h"
#include "bot/behavior/neo_bot_ctg_enemy_chase.h"
#include "bot/neo_bot_path_compute.h"
#include "neo_gamerules.h"
#include "neo_player_shared.h"
#include "vphysics_interface.h"
#include "bot/neo_bot_path_reservation.h"

extern ConVar sv_neo_bot_ctg_enemy_distinct_cutoffs;

ConVar sv_neo_bot_ctg_enemy_intercept_replan_seconds( "sv_neo_bot_ctg_enemy_intercept_replan_seconds", "5", FCVAR_CHEAT,
	"CTG: seconds between an intercepting bot re-picking its cut-off on the enemy ghost carrier's route, to catch the carrier taking a line it did not predict.",
	true, 0.5f, false, 0.0f );

// How close to the cut-off area's centre counts as being there. A nav area is wider than a player,
// so standing anywhere in it is close enough to meet whoever comes through.
static const float kCutOffArrivalTolerance = 64.0f;

// How many nav areas back up the carrier's route to consider when picking a spot to watch.
static const int kWatchAreaLimit = 12;

//---------------------------------------------------------------------------------------------
CNEOBotCtgEnemyInterceptCapPath::CNEOBotCtgEnemyInterceptCapPath( const CNEOBotCtgEnemy::CutOff &cutOff,
	CNEOBotPredictedRoute &carrierRoute )
	: m_cutOff( cutOff )
{
	m_carrierRoute.areas.Swap( carrierRoute.areas );
	m_carrierRoute.travel.Swap( carrierRoute.travel );
}

//---------------------------------------------------------------------------------------------
bool CNEOBotCtgEnemyInterceptCapPath::RepathToCutOff( CNEOBot *me )
{
	// Claim the area for sv_neo_bot_ctg_enemy_distinct_cutoffs: every place this function is
	// called (OnStart, a replan that moved, OnStuck, OnMoveToFailure) is a moment this bot is
	// committing or recommitting to m_cutOff, so this is the one place that needs to say so. The
	// duration is the replan interval plus a margin, so a bot that stops re-confirming - it moved
	// on to chasing, or died - lets the claim lapse on its own rather than needing an explicit
	// release.
	if ( sv_neo_bot_ctg_enemy_distinct_cutoffs.GetBool() && m_cutOff.pArea )
	{
		CNEOBotPathReservations()->ReserveArea( m_cutOff.pArea, me,
			sv_neo_bot_ctg_enemy_intercept_replan_seconds.GetFloat() + 2.0f );
	}

	// This walk ends in holding a spot, not passing through it. A one-way drop on the way is bad
	// ground for that: if the next replan moves the cut-off, or the hold gets abandoned to chase,
	// getting back means detouring around instead of retracing the same steps. Preferred against,
	// not banned - see CNEOBotPathCompute's parameter of the same name.
	return CNEOBotPathCompute( me, m_path, m_cutOff.vecPos, FASTEST_ROUTE,
		PATH_NO_LENGTH_LIMIT, PATH_TRUNCATE_INCOMPLETE_PATH, false, true );
}

//---------------------------------------------------------------------------------------------
// Re-pick the cut-off. Returns false when there is no longer one to head for, in which case the
// caller drops to the chase rather than walking on with a plan it can no longer check.
bool CNEOBotCtgEnemyInterceptCapPath::Replan( CNEOBot *me, CNEO_Player *pGhostCarrier )
{
	CNEOBotCtgEnemy::CutOff cutOff;
	CNEOBotPredictedRoute carrierRoute;
	if ( !CNEOBotCtgEnemy::FindCutOff( me, pGhostCarrier, cutOff, &carrierRoute ) )
	{
		return false;
	}

	const bool bMoved = ( cutOff.pArea != m_cutOff.pArea );

	m_cutOff = cutOff;
	m_carrierRoute.areas.Swap( carrierRoute.areas );
	m_carrierRoute.travel.Swap( carrierRoute.travel );

	return !bMoved || RepathToCutOff( me );
}

//---------------------------------------------------------------------------------------------
// Look back up the carrier's route, at the furthest point along it we still have a clear line to.
// That is where the carrier should come into view, which beats staring at the wall its marker is
// behind. Only ever called once the bot has arrived and stopped: a bot still travelling steers by
// where it is looking, so forcing its view off the path would make it strafe there.
void CNEOBotCtgEnemyInterceptCapPath::WatchForTheCarrier( CNEOBot *me )
{
	if ( !m_watchTimer.IsElapsed() )
	{
		return;
	}
	m_watchTimer.Start( 0.5f );

	// Index 0 is the carrier's own area: there is nothing further back up the route to watch.
	if ( m_cutOff.iCarrierRouteIndex <= 0 || m_cutOff.iCarrierRouteIndex >= m_carrierRoute.Count() )
	{
		return;
	}

	const Vector vecEyeOffset( 0, 0, HumanEyeHeight );
	bool bFound = false;
	Vector vecWatch = vec3_origin;

	// One trace per area, so bound the walk: anything much further back than this is around a
	// corner in practice, and the loop stops at the first area we cannot see anyway.
	const int iStopAt = MAX( 0, m_cutOff.iCarrierRouteIndex - kWatchAreaLimit );
	for ( int i = m_cutOff.iCarrierRouteIndex - 1; i >= iStopAt; --i )
	{
		const Vector vecSpot = m_carrierRoute.areas[i]->GetCenter() + vecEyeOffset;
		if ( !me->GetVisionInterface()->IsLineOfSightClear( vecSpot ) )
		{
			break;
		}

		vecWatch = vecSpot;
		bFound = true;
	}

	if ( bFound )
	{
		// IMPORTANT is the same weight the bot's own "look where a hidden threat might appear"
		// scan uses, so this takes its turn with that scan instead of pinning the view.
		me->GetBodyInterface()->AimHeadTowards( vecWatch, IBody::IMPORTANT, 1.0f, nullptr,
			"Watching where the ghost carrier should appear" );
	}
}

//---------------------------------------------------------------------------------------------
ActionResult< CNEOBot > CNEOBotCtgEnemyInterceptCapPath::OnStart( CNEOBot *me, Action< CNEOBot > *priorAction )
{
	m_path.SetMinLookAheadDistance( me->GetDesiredPathLookAheadRange() );
	m_watchTimer.Invalidate();

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
		|| ( ( me->GetAbsOrigin() - m_cutOff.vecPos ).Length2DSqr() < Square( kCutOffArrivalTolerance ) );

	if ( bArrived )
	{
		// The carrier sees every enemy within sv_neo_ghost_view_distance through walls, so once it
		// is that close there is nothing left to ambush and waiting only invites being flanked.
		const float flGhostViewUnits = sv_neo_ghost_view_distance.GetFloat() / METERS_PER_INCH;
		if ( me->GetAbsOrigin().DistToSqr( pGhostCarrier->GetAbsOrigin() ) < Square( flGhostViewUnits ) )
		{
			return ChangeTo( new CNEOBotCtgEnemyChase, "Carrier is on top of the cut-off - chasing" );
		}

		// Hold the cut-off, watching the way the carrier has to come.
		WatchForTheCarrier( me );
		return Continue();
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
