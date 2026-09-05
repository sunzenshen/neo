#pragma once

#include "bot/neo_bot.h"
#include "Path/NextBotChasePath.h"
#include "nav_pathfind.h"

//--------------------------------------------------------------------------------------------------------
// Runs an enemy ghost carrier down directly. Terminal for the duration of the carry: it only ends
// when the carrier stops being a valid enemy ghost holder (dies, drops the ghost, or captures), at
// which point control returns to the seek dispatcher for a fresh decision.
//
// Always paths FASTEST_ROUTE. This used to alternate with DEFAULT_ROUTE (whose friendly-reservation
// penalty was meant to spread defenders across different lines when several were already ahead of
// the carrier), but ballistrade forensics showed DEFAULT_ROUTE's real path running ~18% longer than
// the optimal route with no such spreading benefit showing up in practice - the reservation penalty
// was just making the chase slower. Measured with a temporary A/B convar (7 pinned ballistrade
// spawns, one build): attacker-captured 31% (alternation) -> 23% (FASTEST_ROUTE always), directionally
// favourable though not significant at that sample size (Fisher p=0.755). See
// knowledge/maps/ntre_ballistrade_ctg/README.md for the full analysis and numbers.
//
// The bot's view is deliberately left to CNEOBot::UpdateLookingAroundForEnemies. It already aims at
// the carrier when it is visible and scans where it should appear when it is not, and it does so
// through the vision system's recognition delay rather than around it.
class CNEOBotCtgEnemyChase : public Action< CNEOBot >
{
public:
	virtual ActionResult< CNEOBot >	OnStart( CNEOBot *me, Action< CNEOBot > *priorAction ) override;
	virtual ActionResult< CNEOBot >	Update( CNEOBot *me, float interval ) override;

	// OnStuck / OnMoveToSuccess / OnMoveToFailure are deliberately not overridden. A ChasePath is
	// recomputed every Update against a moving target, so the next tick already does what a handler
	// would, and Action's defaults (TryContinue) let the event fall through to the actions below.
	virtual const char *GetName( void ) const override { return "ctgEnemyChase"; }

private:
	ChasePath m_chasePath;
};
