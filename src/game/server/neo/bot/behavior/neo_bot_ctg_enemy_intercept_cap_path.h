#pragma once

#include "bot/neo_bot.h"
#include "bot/behavior/neo_bot_ctg_enemy.h"

//--------------------------------------------------------------------------------------------------------
// Cuts the enemy ghost off short of its cap: runs to the earliest area on the ghost's predicted
// route to the nearest cap the enemy can score into that this bot can beat it to. The plan is
// driven by the ghost's position (CNEOBotCtgEnemy::FindCutOff), so it works both while an enemy is
// carrying the ghost and, from freezetime, before any pickup - in which case the "ghost route"
// below is the predicted route a future carry would take.
//
// Arriving with a carrier already confirmed (EnemyGhostCarrier non-null) holds the spot, watching
// the way the carry has to come, and only rushes once the carrier is inside
// sv_neo_ghost_view_distance - it sees every enemy in that radius through walls, so waiting any
// longer only invites being flanked. This ambush is proposal 0001's shipped, measured mechanic and
// is unchanged here.
//
// Arriving with the ghost still loose (freezetime, before anyone has picked it up) has nothing to
// ambush, so it hands straight over to CNEOBotCtgEnemyChase instead of holding - which itself
// Done()s with "no enemy ghost carrier" and drops control back to the seek dispatcher, which goes
// and gets the loose ghost once freezetime is actually over. Every other way this behaviour gives
// up (no path, stuck, lost the cut-off) also hands over to the chase for the same reason: it is the
// one place that already knows how to fall back correctly whether or not anyone holds the ghost.
//
// The cut-off is re-picked on a slow timer, because the whole plan rests on a naive guess at the
// ghost's route: if the carry heads for another cap, or takes a line we did not predict, the point
// moves and this bot has to move with it.
class CNEOBotCtgEnemyInterceptCapPath : public Action< CNEOBot >
{
public:
	// Takes the route by non-const reference and swaps it in: it is the caller's copy, and the
	// vectors are large enough that handing them over beats copying them.
	CNEOBotCtgEnemyInterceptCapPath( const CNEOBotCtgEnemy::CutOff &cutOff, CNEOBotPredictedRoute &ghostRoute );

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
	CNEOBotPredictedRoute m_ghostRoute;		// the ghost's predicted route, kept to watch the approach
	PathFollower m_path;
	CountdownTimer m_replanTimer;			// throttles re-picking the cut-off
	CountdownTimer m_watchTimer;			// throttles the look back up the ghost's route
};
