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
CNEOGhostCapturePoint *CNEOBotCtgEnemy::CarrierGoalCap( CNEO_Player *pGhostCarrier )
{
	const int iCarrierTeam = pGhostCarrier->GetTeamNumber();
	const Vector vecCarrier = pGhostCarrier->GetAbsOrigin();

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

		const float flDistSq = vecCarrier.DistToSqr( pCap->GetAbsOrigin() );
		if ( flDistSq < flBestDistSq )
		{
			flBestDistSq = flDistSq;
			pBest = pCap;
		}
	}

	return pBest;
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

	CNEOGhostCapturePoint *pGoalCap = CarrierGoalCap( pGhostCarrier );
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
	// Every rule tried for choosing a *later* point than this - to set the ambush outside the
	// carrier's through-wall vision, to space the defence out in depth, or to converge the whole
	// team on one area - measured worse, and for the same reason each time: it makes the bot claim
	// ground it cannot actually be standing on in time, so it spends the walk and arrives nowhere.
	// The evidence is in notes/ctg-defence-arms.md.
	int iChosen = -1;

	for ( int i = 0; i < carrierRoute.Count(); ++i )
	{
		if ( myTravel[i] >= 0.0f && myTravel[i] <= carrierRoute.travel[i] * flLead )
		{
			iChosen = i;
			break;
		}
	}

	if ( iChosen < 0 )
	{
		return false;
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
