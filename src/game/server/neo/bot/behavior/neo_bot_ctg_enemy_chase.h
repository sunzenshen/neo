#pragma once

#include "bot/neo_bot.h"
#include "Path/NextBotChasePath.h"
#include "nav_pathfind.h"

//--------------------------------------------------------------------------------------------------------
// Runs an enemy ghost carrier down directly. Terminal for the duration of the carry: it only ends
// when the carrier stops being a valid enemy ghost holder (dies, drops the ghost, or captures), at
// which point control returns to the seek dispatcher for a fresh decision.
//
// The route type is re-evaluated on a slow timer, off whether a cut-off still exists - which is
// the same question as whether this bot is ahead of the carrier or behind it:
//   - FASTEST_ROUTE when there is no point on the carrier's route we can still beat it to. We are
//     behind it and cannot afford a detour.
//   - DEFAULT_ROUTE otherwise. That route type carries the friendly-reservation penalty and a
//     per-bot route preference, so defenders who are already ahead of the carrier come at it by
//     different lines instead of stacking into one file.
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
	CountdownTimer m_routeTypeTimer;		// throttles the FASTEST-vs-DEFAULT re-evaluation
	RouteType m_routeType = DEFAULT_ROUTE;
};
