#pragma once

#include "bot/neo_bot.h"
#include "bot/behavior/neo_bot_ctg_enemy.h"

//--------------------------------------------------------------------------------------------------------
// Cuts the enemy ghost carrier off: runs to the point where the carrier's predicted route to its
// cap and this bot's own route to the same cap converge, and holds it facing back up that route.
//
// Holding only pays while the carrier cannot see us. It sees every enemy within
// sv_neo_ghost_view_distance regardless of walls, so once it closes inside that radius the ambush
// is spent and the bot hands over to the direct chase instead of waiting to be flanked.
//
// The cut-off is re-picked on a slow timer, because the whole plan rests on a naive guess at the
// carrier's route: if it heads for another cap, or takes a line we did not predict, the
// convergence point moves and this bot has to move with it.
class CNEOBotCtgEnemyInterceptCapPath : public Action< CNEOBot >
{
public:
	// Takes the route by non-const reference and swaps it in: it is the caller's copy, and the
	// vectors are large enough that handing them over beats copying them.
	CNEOBotCtgEnemyInterceptCapPath( const CNEOBotCtgEnemy::CutOff &cutOff, CNEOBotPredictedRoute &carrierRoute );

	virtual ActionResult< CNEOBot >	OnStart( CNEOBot *me, Action< CNEOBot > *priorAction ) override;
	virtual ActionResult< CNEOBot >	Update( CNEOBot *me, float interval ) override;

	virtual EventDesiredResult< CNEOBot > OnStuck( CNEOBot *me ) override;
	virtual EventDesiredResult< CNEOBot > OnMoveToFailure( CNEOBot *me, const Path *path, MoveToFailureType reason ) override;

	virtual const char *GetName( void ) const override { return "ctgEnemyInterceptCapPath"; }

private:
	bool Replan( CNEOBot *me, CNEO_Player *pGhostCarrier );
	bool RepathToCutOff( CNEOBot *me );
	void WatchForTheCarrier( CNEOBot *me );

	CNEOBotCtgEnemy::CutOff m_cutOff;
	CNEOBotPredictedRoute m_carrierRoute;	// the carrier's predicted route, kept to watch the approach
	PathFollower m_path;
	CountdownTimer m_replanTimer;			// throttles re-picking the cut-off
	CountdownTimer m_watchTimer;			// throttles the look back up the carrier's route
};
