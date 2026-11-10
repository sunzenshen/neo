#pragma once

#include "NextBotBehavior.h"
#include "Path/NextBotPathFollow.h"

class CNEOBot;

//-------------------------------------------------------------------------------
// Go after the enemy that just killed a teammate, at where it was last seen (or where the teammate fell),
// avoiding the lane the teammate died in when another route is about as short. Fights on the way.
class CNEOBotTradeResponse : public Action< CNEOBot >
{
public:
	CNEOBotTradeResponse( const Vector &vecGoal, const Vector &vecDeathSpot, bool bKillerSeen, int iVictim );
	virtual ~CNEOBotTradeResponse() { }

	virtual ActionResult< CNEOBot >	OnStart( CNEOBot *me, Action< CNEOBot > *priorAction );
	virtual ActionResult< CNEOBot >	Update( CNEOBot *me, float interval );
	virtual ActionResult< CNEOBot >	OnResume( CNEOBot *me, Action< CNEOBot > *interruptingAction );
	virtual void OnEnd( CNEOBot *me, Action< CNEOBot > *nextAction );

	virtual EventDesiredResult< CNEOBot > OnStuck( CNEOBot *me );
	virtual EventDesiredResult< CNEOBot > OnMoveToSuccess( CNEOBot *me, const Path *path );
	virtual EventDesiredResult< CNEOBot > OnMoveToFailure( CNEOBot *me, const Path *path, MoveToFailureType reason );

	// In a hurry while answering, so retreat-to-cover and top-up reloads wait until the window ends
	virtual QueryResultType ShouldHurry( const INextBot *me ) const;

	virtual const char *GetName( void ) const	{ return "TradeResponse"; };

private:
	bool ComputePath( CNEOBot *me );

	Vector m_vecGoal;
	Vector m_vecDeathSpot;
	bool m_bKillerSeen;
	int m_iVictim;
	bool m_bEngaged;
	bool m_bReachedGoal;
	const char *m_pszEndReason;
	PathFollower m_path;
	CountdownTimer m_repathTimer;
	CountdownTimer m_window;
};
