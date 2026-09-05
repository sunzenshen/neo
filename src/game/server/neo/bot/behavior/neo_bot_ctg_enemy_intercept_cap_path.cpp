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

ConVar sv_neo_bot_ctg_enemy_intercept_replan_seconds( "sv_neo_bot_ctg_enemy_intercept_replan_seconds", "5", FCVAR_CHEAT,
	"CTG: seconds between an intercepting bot re-picking its cut-off on the enemy ghost carrier's route, to catch the carrier taking a line it did not predict.",
	true, 0.5f, false, 0.0f );

// How close to the cut-off area's centre counts as being there. A nav area is wider than a player,
// so standing anywhere in it is close enough to meet whoever comes through.
static const float CTG_ENEMY_CUTOFF_ARRIVAL_TOLERANCE = 64.0f;

// How many nav areas back up the ghost's route to consider when picking a spot to watch.
static const int CTG_ENEMY_WATCH_AREA_LIMIT = 12;

//---------------------------------------------------------------------------------------------
CNEOBotCtgEnemyInterceptCapPath::CNEOBotCtgEnemyInterceptCapPath( const CNEOBotCtgEnemy::CutOff &cutOff,
	CNEOBotPredictedRoute &ghostRoute )
	: m_cutOff( cutOff )
{
	m_ghostRoute.areas.Swap( ghostRoute.areas );
	m_ghostRoute.travel.Swap( ghostRoute.travel );
}

//---------------------------------------------------------------------------------------------
bool CNEOBotCtgEnemyInterceptCapPath::RepathToCutOff( CNEOBot *me )
{
	// No explicit path reservation for the cut-off area: CNEOBotPathCompute reserves every area on
	// the path it computes, the destination included, and it starts by releasing this bot's
	// previous claims. Anything claimed here would be dropped again a line later.
	//
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
bool CNEOBotCtgEnemyInterceptCapPath::Replan( CNEOBot *me )
{
	CNEOBotCtgEnemy::CutOff cutOff;
	CNEOBotPredictedRoute ghostRoute;
	if ( !CNEOBotCtgEnemy::FindCutOff( me, cutOff, &ghostRoute ) )
	{
		return false;
	}

	const bool bMoved = ( cutOff.pArea != m_cutOff.pArea );

	m_cutOff = cutOff;
	m_ghostRoute.areas.Swap( ghostRoute.areas );
	m_ghostRoute.travel.Swap( ghostRoute.travel );

	return !bMoved || RepathToCutOff( me );
}

//---------------------------------------------------------------------------------------------
// Look back up the ghost's route, at the furthest point along it we still have a clear line to.
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

	// Index 0 is the ghost's own area: there is nothing further back up the route to watch.
	if ( m_cutOff.iCarrierRouteIndex <= 0 || m_cutOff.iCarrierRouteIndex >= m_ghostRoute.Count() )
	{
		return;
	}

	const Vector vecEyeOffset( 0, 0, HumanEyeHeight );
	bool bFound = false;
	Vector vecWatch = vec3_origin;

	// One trace per area, so bound the walk: anything much further back than this is around a
	// corner in practice, and the loop stops at the first area we cannot see anyway.
	const int iStopAt = MAX( 0, m_cutOff.iCarrierRouteIndex - CTG_ENEMY_WATCH_AREA_LIMIT );
	for ( int i = m_cutOff.iCarrierRouteIndex - 1; i >= iStopAt; --i )
	{
		const Vector vecSpot = m_ghostRoute.areas[i]->GetCenter() + vecEyeOffset;
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

	// The ghost may be loose (freezetime, or a post-drop hold) - EnemyGhostCarrier is then null and
	// that is fine, see the arrival handling below. What is not recoverable here is the ghost being
	// gone, or an ally having taken it (the escort's problem, not ours).
	if ( !NEORules()->GhostExists() )
	{
		return Done( "Ghost no longer exists" );
	}

	CNEO_Player *pGhostCarrier = CNEOBotCtgEnemy::EnemyGhostCarrier( me );
	if ( !pGhostCarrier )
	{
		const int iGhoster = NEORules()->GetGhosterPlayer();
		if ( iGhoster > 0 && iGhoster <= gpGlobals->maxClients )
		{
			CNEO_Player *pCarrier = ToNEOPlayer( UTIL_PlayerByIndex( iGhoster ) );
			if ( pCarrier && pCarrier->GetTeamNumber() == me->GetTeamNumber() )
			{
				return Done( "A teammate has the ghost" );
			}
		}
	}

	if ( m_replanTimer.IsElapsed() )
	{
		m_replanTimer.Start( sv_neo_bot_ctg_enemy_intercept_replan_seconds.GetFloat() );

		if ( !Replan( me ) )
		{
			return ChangeTo( new CNEOBotCtgEnemyChase, "Lost the cut-off - chasing" );
		}
	}

	const bool bArrived = ( me->GetLastKnownArea() == m_cutOff.pArea )
		|| ( ( me->GetAbsOrigin() - m_cutOff.vecPos ).Length2DSqr() < Square( CTG_ENEMY_CUTOFF_ARRIVAL_TOLERANCE ) );

	if ( bArrived )
	{
		if ( !pGhostCarrier )
		{
			// Nobody has the ghost yet - this is the freezetime case, arriving before any pickup.
			// There is nothing to ambush, so hand over to the chase: it Done()s immediately with
			// "no enemy ghost carrier" and drops control back to the seek dispatcher, which goes
			// and gets the loose ghost once freezetime is actually over.
			return ChangeTo( new CNEOBotCtgEnemyChase, "Arrived at the cut-off with the ghost still loose - moving in" );
		}

		// The carrier sees every enemy within sv_neo_ghost_view_distance through walls, so once it
		// is that close there is nothing left to ambush and waiting only invites being flanked.
		const float flGhostViewUnits = sv_neo_ghost_view_distance.GetFloat() / METERS_PER_INCH;
		if ( me->GetAbsOrigin().DistToSqr( pGhostCarrier->GetAbsOrigin() ) < Square( flGhostViewUnits ) )
		{
			return ChangeTo( new CNEOBotCtgEnemyChase, "Carrier is on top of the cut-off - chasing" );
		}

		// Hold the spot, watching the way the carry has to come.
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
