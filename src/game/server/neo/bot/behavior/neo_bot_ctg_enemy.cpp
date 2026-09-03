#include "cbase.h"
#include "neo_player.h"
#include "bot/neo_bot.h"
#include "bot/behavior/neo_bot_ctg_enemy.h"
#include "bot/behavior/neo_bot_ctg_enemy_chase.h"
#include "bot/behavior/neo_bot_ctg_enemy_intercept_cap_path.h"
#include "bot/neo_bot_path_compute.h"
#include "neo_gamerules.h"
#include "neo_ghost_cap_point.h"
#include "nav_mesh.h"
#include "nav_pathfind.h"

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
CNEOGhostCapturePoint *CNEOBotCtgEnemy::CarrierGoalCap( CNEOBot *me, CNEO_Player *pGhostCarrier )
{
	if ( !pGhostCarrier )
	{
		return nullptr;
	}

	const int iCarrierTeam = pGhostCarrier->GetTeamNumber();
	const Vector vecCarrier = pGhostCarrier->GetAbsOrigin();

	CNEOGhostCapturePoint *pBest = nullptr;
	float flBestDistSq = FLT_MAX;

	for ( int i = 0; i < NEORules()->m_pGhostCaps.Count(); ++i )
	{
		CNEOGhostCapturePoint *pCap = dynamic_cast< CNEOGhostCapturePoint * >(
			UTIL_EntityByIndex( NEORules()->m_pGhostCaps[i] ) );
		if ( !pCap || !pCap->GetActive() )
		{
			continue;
		}

		if ( pCap->owningTeamAlternate() != iCarrierTeam )
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
bool CNEOBotCtgEnemy::CarrierRouteToGoal( CNEOBot *me, CNEO_Player *pGhostCarrier,
	CUtlVector< CNavArea * > &routeAreas, float &routeLength, Vector &goalPos )
{
	routeAreas.RemoveAll();
	routeLength = 0.0f;

	CNEOGhostCapturePoint *pGoalCap = CarrierGoalCap( me, pGhostCarrier );
	if ( !pGoalCap )
	{
		return false;
	}

	goalPos = pGoalCap->GetAbsOrigin();

	CNavArea *pCarrierArea = pGhostCarrier->GetLastKnownArea();
	CNavArea *pGoalArea = TheNavMesh->GetNearestNavArea( goalPos );
	if ( !pCarrierArea || !pGoalArea )
	{
		return false;
	}

	if ( pCarrierArea == pGoalArea )
	{
		routeAreas.AddToTail( pGoalArea );
		return true;
	}

	ShortestPathCost cost;
	if ( !NavAreaBuildPath( pCarrierArea, pGoalArea, &goalPos, cost ) )
	{
		return false;
	}

	// Estimate length of path
	for ( CNavArea *pArea = pGoalArea; pArea; pArea = pArea->GetParent() )
	{
		routeAreas.AddToTail( pArea );

		CNavArea *pParent = pArea->GetParent();
		if ( pParent )
		{
			routeLength += ( pArea->GetCenter() - pParent->GetCenter() ).Length();
		}
	}

	return routeAreas.Count() > 0;
}

//---------------------------------------------------------------------------------------------
ActionResult< CNEOBot > CNEOBotCtgEnemy::Update( CNEOBot *me, float interval )
{
	CNEO_Player *pGhostCarrier = EnemyGhostCarrier( me );
	if ( !pGhostCarrier )
	{
		return Done( "No enemy ghost carrier" );
	}

	CUtlVector< CNavArea * > carrierRoute;
	float flCarrierToGoal = 0.0f;
	Vector vecGoal;
	if ( CarrierRouteToGoal( me, pGhostCarrier, carrierRoute, flCarrierToGoal, vecGoal ) )
	{
		CNavArea *pMyArea = me->GetLastKnownArea();
		const bool bOnCarrierRoute = ( pMyArea && carrierRoute.HasElement( pMyArea ) );

		if ( !bOnCarrierRoute )
		{
			PathFollower botToGoal;
			if ( CNEOBotPathPreview( me, botToGoal, vecGoal, FASTEST_ROUTE )
				&& botToGoal.GetLength() <= flCarrierToGoal )
			{
				return ChangeTo( new CNEOBotCtgEnemyInterceptCapPath, "Cutting off the carrier's route to the cap" );
			}
		}
	}

	return ChangeTo( new CNEOBotCtgEnemyChase, "Chasing the ghost carrier down" );
}
