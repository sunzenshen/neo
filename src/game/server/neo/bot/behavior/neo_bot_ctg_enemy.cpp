#include "cbase.h"
#include "neo_player.h"
#include "bot/neo_bot.h"
#include "bot/behavior/neo_bot_ctg_enemy.h"
#include "bot/behavior/neo_bot_ctg_enemy_chase.h"
#include "bot/behavior/neo_bot_ctg_enemy_intercept_cap_path.h"
#include "neo_gamerules.h"
#include "neo_ghost_cap_point.h"
#include "nav_mesh.h"
#include "nav_pathfind.h"

ConVar sv_neo_bot_ctg_enemy_intercept_lead( "sv_neo_bot_ctg_enemy_intercept_lead", "1.0", FCVAR_CHEAT,
	"CTG: a bot claims a point on the enemy ghost carrier's route only when its own travel there is at "
	"most this fraction of the carrier's. Below 1 it needs a head start; above 1 it will try marginal cut-offs.",
	true, 0.1f, true, 2.0f );

ConVar sv_neo_bot_ctg_enemy_cutoff_min_travel( "sv_neo_bot_ctg_enemy_cutoff_min_travel", "0", FCVAR_CHEAT,
	"CTG: how far along the enemy ghost carrier's route a cut-off should be, in Hammer units. The "
	"carrier sees through walls out to sv_neo_ghost_view_distance, so the earliest cut-off is often "
	"one it can already see and route around. 0 = always take the earliest.",
	true, 0.0f, false, 0.0f );

ConVar sv_neo_bot_ctg_enemy_choke_window( "sv_neo_bot_ctg_enemy_choke_window", "0", FCVAR_CHEAT,
	"CTG: how much further along the enemy ghost carrier's route, in Hammer units, to look for a "
	"narrower place to stand than the first one available. The route is a guess; a spot with few "
	"ways around it holds against the lines that were not guessed. 0 = take the first spot.",
	true, 0.0f, false, 0.0f );

ConVar sv_neo_bot_ctg_enemy_spread_spacing( "sv_neo_bot_ctg_enemy_spread_spacing", "0", FCVAR_CHEAT,
	"CTG: how far apart, in Hammer units measured along the enemy ghost carrier's route, successive "
	"defenders should place their cut-offs. Every defender runs the same prediction, so at 0 they "
	"all claim the same area and the carrier only has to get past one of them.",
	true, 0.0f, false, 0.0f );

ConVar sv_neo_bot_ctg_enemy_split_caps( "sv_neo_bot_ctg_enemy_split_caps", "0", FCVAR_CHEAT,
	"CTG: 1 = half the defenders cover the carrier's second-nearest scoring zone instead of the "
	"nearest, so a wrong guess about which zone it is heading for does not leave the other open." );

// NEO-HARNESS-TEMP: positive control for measuring the interception. When set, the decider always
// picks the direct chase, which is what CNEOBotCtgEnemy did before this branch. Never part of the
// PR -- see harness/patches/README.md.
ConVar sv_neo_bot_ctg_enemy_force_chase( "sv_neo_bot_ctg_enemy_force_chase", "0", FCVAR_CHEAT,
	"NEO harness debug: 1 = never intercept, always chase the enemy ghost carrier directly." );

//---------------------------------------------------------------------------------------------
// The cap zone the carrier is trying to reach: the active zone nearest it that its own team scores
// in. This is the same test CNEOBotCtgCarrier::GetNearestCapPoint uses to pick its objective, run
// from the outside on public information (the marker gives the carrier away, cap zones are fixed).
//
// Straight-line distance, deliberately. It is the naive guess a human defender makes from the
// marker, and it happens to be exactly what the carrier bot does, so ranking by travel distance
// instead would be a worse prediction dressed up as a better one.
CNEOGhostCapturePoint *CNEOBotCtgEnemy::CarrierGoalCap( CNEO_Player *pGhostCarrier, int iRank )
{
	const int iCarrierTeam = pGhostCarrier->GetTeamNumber();
	const Vector vecCarrier = pGhostCarrier->GetAbsOrigin();

	iRank = clamp( iRank, 0, kMaxCapRank - 1 );
	CNEOGhostCapturePoint *pPicked[ kMaxCapRank ] = {};

	// Selection sort, stopping as soon as the requested rank is filled. There are a handful of cap
	// zones on a map, so this is cheaper than building and sorting a list.
	for ( int r = 0; r <= iRank; ++r )
	{
		CNEOGhostCapturePoint *pBest = nullptr;
		float flBestDistSq = FLT_MAX;

		for ( int i = 0; i < NEORules()->m_pGhostCaps.Count(); ++i )
		{
			CNEOGhostCapturePoint *pCap = dynamic_cast< CNEOGhostCapturePoint * >(
				UTIL_EntityByIndex( NEORules()->m_pGhostCaps[i] ) );
			if ( !pCap || !pCap->GetActive() || pCap->owningTeamAlternate() != iCarrierTeam )
			{
				continue;
			}

			bool bAlreadyTaken = false;
			for ( int k = 0; k < r && !bAlreadyTaken; ++k )
			{
				bAlreadyTaken = ( pPicked[k] == pCap );
			}
			if ( bAlreadyTaken )
			{
				continue;
			}

			const float flDistSq = vecCarrier.DistToSqr( pCap->GetAbsOrigin() );
			if ( flDistSq < flBestDistSq )
			{
				flBestDistSq = flDistSq;
				pBest = pCap;
			}
		}

		if ( !pBest )
		{
			// The map has fewer scoring zones than the rank asked for. Cover the nearest instead
			// of covering nothing.
			return ( r > 0 ) ? pPicked[0] : nullptr;
		}

		pPicked[r] = pBest;
	}

	return pPicked[ iRank ];
}

//---------------------------------------------------------------------------------------------
// Plot a route from pStartArea to vecGoal and record it start-first with a running travel
// distance. NavAreaBuildPath leaves the answer in the areas' parent pointers, and the next search
// overwrites them, so each route has to be copied out before another one is plotted.
template < typename CostFunctor >
static bool PredictRoute( CNavArea *pStartArea, const Vector &vecGoal, CostFunctor &cost,
	CNEOBotPredictedRoute &out )
{
	out.Reset();

	CNavArea *pGoalArea = TheNavMesh->GetNearestNavArea( vecGoal );
	if ( !pStartArea || !pGoalArea )
	{
		return false;
	}

	if ( !NavAreaBuildPath( pStartArea, pGoalArea, &vecGoal, cost ) )
	{
		return false;
	}

	// The parent chain runs goal -> start, so collect it and flip it.
	for ( CNavArea *pArea = pGoalArea; pArea; pArea = pArea->GetParent() )
	{
		out.areas.AddToTail( pArea );
	}

	out.areas.Reverse();

	const int count = out.areas.Count();
	out.travel.AddToTail( 0.0f );
	for ( int i = 1; i < count; ++i )
	{
		out.travel.AddToTail( out.travel[ i - 1 ]
			+ ( out.areas[i]->GetCenter() - out.areas[ i - 1 ]->GetCenter() ).Length() );
	}

	return count > 0;
}

//---------------------------------------------------------------------------------------------
float CNEOBotCtgEnemy::CarrierDistToGoal( CNEO_Player *pGhostCarrier )
{
	if ( !pGhostCarrier )
	{
		return -1.0f;
	}

	CNEOGhostCapturePoint *pGoalCap = CarrierGoalCap( pGhostCarrier );

	return pGoalCap ? pGhostCarrier->GetAbsOrigin().DistTo( pGoalCap->GetAbsOrigin() ) : -1.0f;
}

//---------------------------------------------------------------------------------------------
// This bot's place in the queue among its living teammates, counted by entity index. Nobody
// respawns inside a CTG round, so it is stable while the team holds together and tightens up as
// the team is whittled down - and every teammate derives the same ordering from the same public
// state, so a defence can space itself out without any shared bookkeeping to keep in sync.
static int DefenderRank( CNEOBot *me )
{
	int iRank = 0;
	for ( int i = 1; i <= gpGlobals->maxClients; ++i )
	{
		CNEO_Player *pTeammate = ToNEOPlayer( UTIL_PlayerByIndex( i ) );
		if ( pTeammate && pTeammate->IsAlive()
			&& pTeammate->GetTeamNumber() == me->GetTeamNumber()
			&& pTeammate->entindex() < me->entindex() )
		{
			++iRank;
		}
	}

	return iRank;
}

//---------------------------------------------------------------------------------------------
CNEO_Player *CNEOBotCtgEnemy::EnemyGhostCarrier( CNEOBot *me )
{
	if ( !NEORules()->GhostExists() )
	{
		return nullptr;
	}

	const int iGhoster = NEORules()->GetGhosterPlayer();
	if ( iGhoster <= 0 || iGhoster > gpGlobals->maxClients )
	{
		return nullptr;
	}

	CNEO_Player *pCarrier = ToNEOPlayer( UTIL_PlayerByIndex( iGhoster ) );
	if ( !pCarrier || !pCarrier->IsAlive() || pCarrier->GetTeamNumber() == me->GetTeamNumber() )
	{
		return nullptr;
	}

	return pCarrier;
}

//---------------------------------------------------------------------------------------------
// Prices every area of a predicted route by how far *this bot* has to travel to reach it.
// SearchSurroundingAreas floods outward by travel distance, so one search prices every candidate
// cut-off at once - far cheaper than a path query per candidate.
class CNEOBotRouteTravelCost : public ISearchSurroundingAreasFunctor
{
public:
	CNEOBotRouteTravelCost( CNEOBot *me, const CNEOBotPredictedRoute &route, CUtlVector< float > &travelOut )
		: m_me( me ), m_route( route ), m_travel( travelOut )
	{
		m_travel.SetCount( route.Count() );
		for ( int i = 0; i < m_travel.Count(); ++i )
		{
			m_travel[i] = -1.0f;
		}
	}

	virtual bool operator()( CNavArea *area, CNavArea *priorArea, float travelDistanceSoFar ) override
	{
		const int i = m_route.areas.Find( area );
		if ( i >= 0 && m_travel[i] < 0.0f )
		{
			m_travel[i] = travelDistanceSoFar;
		}

		return true;
	}

	virtual bool ShouldSearch( CNavArea *adjArea, CNavArea *currentArea, float travelDistanceSoFar ) override
	{
		return !adjArea->IsBlocked( TEAM_ANY )
			&& m_me->GetLocomotionInterface()->IsAreaTraversable( adjArea );
	}

private:
	CNEOBot *m_me;
	const CNEOBotPredictedRoute &m_route;
	CUtlVector< float > &m_travel;
};

//---------------------------------------------------------------------------------------------
bool CNEOBotCtgEnemy::FindCutOff( CNEOBot *me, CNEO_Player *pGhostCarrier, CutOff &cutOff,
	CNEOBotPredictedRoute *pOutCarrierRoute )
{
	cutOff = CutOff();
	if ( pOutCarrierRoute )
	{
		pOutCarrierRoute->Reset();
	}

	if ( !pGhostCarrier )
	{
		return false;
	}

	// Splitting the defence across two scoring zones costs half the defenders their shot at the
	// zone the carrier is actually heading for, and buys cover on the one it might switch to. The
	// entity index is an arbitrary but stable partition, so a bot does not flip sides mid-round.
	const int iCapRank = ( sv_neo_bot_ctg_enemy_split_caps.GetBool() && ( me->entindex() & 1 ) ) ? 1 : 0;

	CNEOGhostCapturePoint *pGoalCap = CarrierGoalCap( pGhostCarrier, iCapRank );
	if ( !pGoalCap )
	{
		return false;
	}

	const Vector vecGoal = pGoalCap->GetAbsOrigin();

	// The carrier's side is a naive shortest route: we do not know its class, its loadout or where
	// it actually means to go, so guessing anything richer would be reading its mind.
	CNEOBotPredictedRoute carrierRoute;
	ShortestPathCost carrierCost;
	if ( !PredictRoute( pGhostCarrier->GetLastKnownArea(), vecGoal, carrierCost, carrierRoute ) )
	{
		return false;
	}

	// How far we have to travel to reach each area of that route. This is the honest half of the
	// comparison: it knows what this bot can actually traverse, and it is measured the same way as
	// the carrier's side (between area centres), so the two numbers are comparable.
	//
	// Note the search has to come *after* the carrier's route is copied out: both this and
	// NavAreaBuildPath work through the nav areas' shared search state.
	const float flLead = sv_neo_bot_ctg_enemy_intercept_lead.GetFloat();

	CUtlVector< float > myTravel;
	CNEOBotRouteTravelCost search( me, carrierRoute, myTravel );
	SearchSurroundingAreas( me->GetLastKnownArea(), search, carrierRoute.Length() * flLead );

	// Walk the carrier's route outward from the carrier and take the first area we beat it to.
	// Earliest wins: that is the point furthest from the carrier's cap where we can still be
	// standing in its way. Pathing at the route directly, rather than intersecting it with our own
	// route to the cap, is what makes an early meeting possible at all - two routes that both aim
	// at the cap tend not to share ground until they are nearly there.
	//
	// sv_neo_bot_ctg_enemy_cutoff_min_travel pushes that choice further down the route. The
	// earliest winnable area is by construction the one closest to the carrier, and the carrier
	// sees every enemy within sv_neo_ghost_view_distance through walls - so the earliest cut-off is
	// frequently one it can watch being set up and simply walk around. A cut-off taken beyond that
	// radius is still standing between the carrier and its endzone, but it is set up unseen.
	//
	// sv_neo_bot_ctg_enemy_spread_spacing then stacks a per-defender offset on top, so the team
	// forms a line in depth along the route instead of every defender claiming the same area. The
	// clamp keeps the deepest defender out of the endzone: past that point it is not cutting the
	// carrier off, it is standing on the goal line waiting to lose the round there.
	const float flSpread = sv_neo_bot_ctg_enemy_spread_spacing.GetFloat() * DefenderRank( me );
	const float flMinTravel = MIN( sv_neo_bot_ctg_enemy_cutoff_min_travel.GetFloat() + flSpread,
		carrierRoute.Length() * 0.8f );
	int iEarliest = -1;
	int iChosen = -1;

	for ( int i = 0; i < carrierRoute.Count(); ++i )
	{
		if ( myTravel[i] < 0.0f || myTravel[i] > carrierRoute.travel[i] * flLead )
		{
			continue;
		}

		if ( iEarliest < 0 )
		{
			iEarliest = i;
		}

		if ( carrierRoute.travel[i] >= flMinTravel )
		{
			iChosen = i;
			break;
		}
	}

	// Nothing far enough along the route is winnable: take the earliest rather than nothing, since
	// a cut-off the carrier can see still has to be walked around.
	if ( iChosen < 0 )
	{
		iChosen = iEarliest;
	}

	if ( iChosen < 0 )
	{
		return false;
	}

	// The predicted route is a guess about a line the carrier may not take, so where the route runs
	// through a narrow place it is worth giving up a little ground to stand there: a spot with two
	// ways out covers the lines we did not guess, and a spot in the open covers only the one we did.
	// Nav-area connection count stands in for how narrow a place is - cheap, and no precomputation.
	const float flChokeWindow = sv_neo_bot_ctg_enemy_choke_window.GetFloat();
	if ( flChokeWindow > 0.0f )
	{
		const float flLimit = carrierRoute.travel[ iChosen ] + flChokeWindow;
		int iNarrowest = iChosen;
		int iFewestWays = INT_MAX;

		for ( int i = iChosen; i < carrierRoute.Count() && carrierRoute.travel[i] <= flLimit; ++i )
		{
			if ( myTravel[i] < 0.0f || myTravel[i] > carrierRoute.travel[i] * flLead )
			{
				continue;
			}

			int iWays = 0;
			for ( int dir = 0; dir < NUM_DIRECTIONS; ++dir )
			{
				iWays += carrierRoute.areas[i]->GetAdjacentCount( static_cast< NavDirType >( dir ) );
			}

			if ( iWays < iFewestWays )
			{
				iFewestWays = iWays;
				iNarrowest = i;
			}
		}

		iChosen = iNarrowest;
	}

	cutOff.pArea = carrierRoute.areas[ iChosen ];
	cutOff.vecPos = cutOff.pArea->GetCenter();
	cutOff.iCarrierRouteIndex = iChosen;

	if ( pOutCarrierRoute )
	{
		pOutCarrierRoute->areas.Swap( carrierRoute.areas );
		pOutCarrierRoute->travel.Swap( carrierRoute.travel );
	}

	return true;
}

//---------------------------------------------------------------------------------------------
ActionResult< CNEOBot > CNEOBotCtgEnemy::Update( CNEOBot *me, float interval )
{
	CNEO_Player *pGhostCarrier = EnemyGhostCarrier( me );
	if ( !pGhostCarrier )
	{
		return Done( "No enemy ghost carrier" );
	}

	// Cut the carrier off when there is somewhere on its route we can reach first. Already standing
	// on that spot means the cut-off is made and the direct chase is the better tool; no such spot
	// at all means the carrier is ahead of us and a detour would only give up more ground.
	CutOff cutOff;
	CNEOBotPredictedRoute carrierRoute;
	if ( !sv_neo_bot_ctg_enemy_force_chase.GetBool() /* NEO-HARNESS-TEMP */
		&& FindCutOff( me, pGhostCarrier, cutOff, &carrierRoute )
		&& cutOff.pArea != me->GetLastKnownArea() )
	{
		return ChangeTo( new CNEOBotCtgEnemyInterceptCapPath( cutOff, carrierRoute ),
			"Cutting the carrier off short of its cap" );
	}

	return ChangeTo( new CNEOBotCtgEnemyChase, "Chasing the ghost carrier down" );
}
