#pragma once

#include "bot/neo_bot_fallback.h"

// Moves toward a teammate until the teammates nearby match the enemies seen, then hands the fight back
class CNEOBotFallBack : public Action< CNEOBot >
{
public:
	CNEOBotFallBack( CNEO_Player *pMate, NEOBotFallback::ETrigger trigger );

	virtual ActionResult< CNEOBot >	OnStart( CNEOBot *me, Action< CNEOBot > *priorAction );
	virtual ActionResult< CNEOBot >	Update( CNEOBot *me, float interval );
	virtual void OnEnd( CNEOBot *me, Action< CNEOBot > *nextAction );

	virtual EventDesiredResult< CNEOBot > OnStuck( CNEOBot *me );
	virtual EventDesiredResult< CNEOBot > OnMoveToFailure( CNEOBot *me, const Path *path, MoveToFailureType reason );

	virtual QueryResultType ShouldHurry( const INextBot *me ) const;

	virtual const char *GetName( void ) const	{ return "FallBack"; };

private:
	CHandle< CNEO_Player > m_hMate;
	NEOBotFallback::ETrigger m_trigger;
	const char *m_pszEndReason;
	float m_flStartTime;

	PathFollower m_path;
	CountdownTimer m_repathTimer;
	CountdownTimer m_giveUpTimer;
	CountdownTimer m_grenadeCheckTimer;

	ActionResult< CNEOBot > End( const char *pszReason );
	const char *CheckDone( CNEOBot *me, CNEO_Player *pMate );
};
