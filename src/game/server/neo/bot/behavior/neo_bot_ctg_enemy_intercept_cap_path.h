#pragma once

#include "bot/neo_bot.h"
#include "bot/behavior/neo_bot_ctg_enemy.h"

//--------------------------------------------------------------------------------------------------------
// Cuts the enemy ghost off short of its cap: runs to a point on the ghost's predicted route to the
// nearest cap the enemy can score into, and holds it facing back up that route. The plan is driven
// by the ghost's position (CNEOBotCtgEnemy::FindCutOff), so it works both while an enemy is
// carrying the ghost and, from freezetime, before any pickup - in which case the "carrier route"
// below is the predicted route a future carry would take.
//
// Two modes:
//   - intercept (default): the cut-off is the earliest area on the route this bot can beat the
//     ghost to - the standard behaviour shipped in proposal 0001.
//   - goalie (bGoalie): the hold point is a fixed depth back from the cap along the route, a last
//     line on the cap's approach throat. Used from freezetime by the slow class to pre-cover a
//     threatened wing cap. Goalie mode automatically drops to intercept once an enemy actually
//     picks the ghost up, so the bot stops camping the throat and starts racing the live carrier.
//
// Holding only pays while the carrier cannot see us. It sees every enemy within
// sv_neo_ghost_view_distance regardless of walls, so once it closes inside that radius the ambush
// is spent and the bot hands over to the direct chase instead of waiting to be flanked.
//
// The cut-off is re-picked on a slow timer, because the whole plan rests on a naive guess at the
// ghost's route: if the carry heads for another cap, or takes a line we did not predict, the
// point moves and this bot has to move with it.
class CNEOBotCtgEnemyInterceptCapPath : public Action< CNEOBot >
{
public:
	// Takes the route by non-const reference and swaps it in: it is the caller's copy, and the
	// vectors are large enough that handing them over beats copying them. flGoalieDepth is only
	// read in goalie mode (the units-back-from-cap hold distance).
	CNEOBotCtgEnemyInterceptCapPath( const CNEOBotCtgEnemy::CutOff &cutOff, CNEOBotPredictedRoute &carrierRoute,
		bool bGoalie = false, float flGoalieDepth = 0.0f );

	virtual ActionResult< CNEOBot >	OnStart( CNEOBot *me, Action< CNEOBot > *priorAction ) override;
	virtual ActionResult< CNEOBot >	Update( CNEOBot *me, float interval ) override;

	virtual EventDesiredResult< CNEOBot > OnStuck( CNEOBot *me ) override;
	virtual EventDesiredResult< CNEOBot > OnMoveToFailure( CNEOBot *me, const Path *path, MoveToFailureType reason ) override;

	virtual const char *GetName( void ) const override { return "ctgEnemyInterceptCapPath"; }

private:
	bool Replan( CNEOBot *me );
	bool RepathToCutOff( CNEOBot *me );
	void WatchForTheCarrier( CNEOBot *me );

	CNEOBotCtgEnemy::CutOff m_cutOff;
	CNEOBotPredictedRoute m_carrierRoute;	// the ghost's predicted route, kept to watch the approach
	PathFollower m_path;
	CountdownTimer m_replanTimer;			// throttles re-picking the cut-off
	CountdownTimer m_watchTimer;			// throttles the look back up the ghost's route
	bool m_bGoalie = false;					// hold a fixed depth off the cap instead of the earliest cut-off
	float m_flGoalieDepth = 0.0f;			// units back from the cap along the route, goalie mode only
};
