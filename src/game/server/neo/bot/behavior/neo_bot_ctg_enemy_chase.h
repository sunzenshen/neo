#pragma once

#include "bot/neo_bot.h"
#include "Path/NextBotChasePath.h"
#include "nav_pathfind.h"

class CNEO_Player;

//--------------------------------------------------------------------------------------------------------
// Runs an enemy ghost carrier down directly. Terminal for the duration of the carry: it only ends
// when the carrier stops being a valid enemy ghost holder (dies, drops the ghost, or caps), at
// which point control returns to the seek dispatcher for a fresh decision.
//
// The route type is re-evaluated on a slow timer:
//   - FASTEST_ROUTE while the carrier is closer to its cap than this bot is (we are behind and
//     need to close the distance)
//   - DEFAULT_ROUTE  otherwise (we are already ahead of the carrier relative to its cap, so the
//     normal route keeps us between it and the goal)
class CNEOBotCtgEnemyChase : public Action< CNEOBot >
{
public:
	virtual ActionResult< CNEOBot >	OnStart( CNEOBot *me, Action< CNEOBot > *priorAction ) override;
	virtual ActionResult< CNEOBot >	Update( CNEOBot *me, float interval ) override;

	virtual EventDesiredResult< CNEOBot > OnStuck( CNEOBot *me ) override;
	virtual EventDesiredResult< CNEOBot > OnMoveToSuccess( CNEOBot *me, const Path *path ) override;
	virtual EventDesiredResult< CNEOBot > OnMoveToFailure( CNEOBot *me, const Path *path, MoveToFailureType reason ) override;

	virtual const char *GetName( void ) const override { return "ctgEnemyChase"; }

private:
	ChasePath m_chasePath;
	CountdownTimer m_carrierGlanceTimer;	// throttle the look-toward-carrier glance
	CountdownTimer m_routeTypeTimer;		// throttle the FASTEST-vs-DEFAULT re-evaluation
	RouteType m_routeType = DEFAULT_ROUTE;
};
