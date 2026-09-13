#pragma once

#include "bot/neo_bot.h"
#include "bot/behavior/neo_bot_ctg_enemy.h"

//--------------------------------------------------------------------------------------------------------
// Cuts the enemy ghost carrier off: runs to the point where the carrier's predicted route to its
// cap and this bot's own route to the same cap converge, then immediately hands over to the direct
// chase from there - no hold, no ambush. The value of the cut-off is purely in the ground it wins:
// arriving somewhere on the carrier's route ahead of it, rather than starting the chase from
// wherever this bot happened to be standing.
//
// The cut-off is re-picked on a slow timer while still travelling, because the whole plan rests on
// a naive guess at the carrier's route: if it heads for another cap, or takes a line we did not
// predict, the convergence point moves and this bot has to move with it.
class CNEOBotCtgEnemyInterceptCapPath : public Action< CNEOBot >
{
public:
	CNEOBotCtgEnemyInterceptCapPath( const CNEOBotCtgEnemy::CutOff &cutOff );

	virtual ActionResult< CNEOBot >	OnStart( CNEOBot *me, Action< CNEOBot > *priorAction ) override;
	virtual ActionResult< CNEOBot >	Update( CNEOBot *me, float interval ) override;

	virtual EventDesiredResult< CNEOBot > OnStuck( CNEOBot *me ) override;
	virtual EventDesiredResult< CNEOBot > OnMoveToFailure( CNEOBot *me, const Path *path, MoveToFailureType reason ) override;

	virtual const char *GetName( void ) const override { return "ctgEnemyInterceptCapPath"; }

private:
	bool Replan( CNEOBot *me, CNEO_Player *pGhostCarrier );
	bool RepathToCutOff( CNEOBot *me );

	CNEOBotCtgEnemy::CutOff m_cutOff;
	PathFollower m_path;
	CountdownTimer m_replanTimer;			// throttles re-picking the cut-off
};
