#pragma once

#include "NextBot/Path/NextBotPath.h"
#include "nav_pathfind.h"
#include "tier1/utlmap.h"

class CNEOBot;
class CNavArea;
class CNavLadder;
class CFuncElevator;

// a bot standing taller than the mesh promises (the juggernaut) has no lane for its standing hull across this portal
// and has to duck through it (neo_bot_path_duck_lane)
bool NeoBotMustDuckThrough( CNEOBot *me, const CNavArea *from, const CNavArea *to );

#include "neo_bot_path_reservation.h"
#include "tier1/utlstack.h"

//----------------------------------------------------------------------------------------------------------------
/**
 * Functor used with NavAreaBuildPath()
 */
class CNEOBotPathCost : public IPathCost
{
public:
	CNEOBotPathCost(CNEOBot* me, RouteType routeType);

	virtual float operator()(CNavArea* baseArea, CNavArea* fromArea, const CNavLadder* ladder, const CFuncElevator* elevator, float length) const override;

	bool m_bIgnoreReservations;
	bool m_bIgnoreVisibilityExposure;

private:
	bool m_bIgnoreHazards;
	CNEOBot* m_me;
	RouteType m_routeType;
	float m_stepHeight;
	float m_maxJumpHeight;
	float m_maxDropHeight;
};
