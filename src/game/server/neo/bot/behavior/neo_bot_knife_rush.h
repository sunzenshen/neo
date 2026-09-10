#pragma once

#include "bot/neo_bot.h"
#include "nav_pathfind.h"

class CNEOBaseCombatWeapon;

class CNEOBotKnifeRush : public Action< CNEOBot >
{
public:
	static bool ShouldRush( CNEOBot *me );
	QueryResultType ShouldRetreat( const INextBot *me );

	virtual ActionResult< CNEOBot >	OnStart( CNEOBot *me, Action< CNEOBot > *priorAction ) override;
	virtual ActionResult< CNEOBot >	Update( CNEOBot *me, float interval ) override;
	virtual void					OnEnd( CNEOBot *me, Action< CNEOBot > *nextAction ) override;

	virtual EventDesiredResult< CNEOBot > OnStuck( CNEOBot *me ) override;
	virtual EventDesiredResult< CNEOBot > OnMoveToFailure( CNEOBot *me, const Path *path, MoveToFailureType reason ) override;

	virtual const char *GetName( void ) const override { return "knifeRush"; }

private:
	bool RepathToFlank( CNEOBot *me );

	CHandle< CNEOBaseCombatWeapon > m_hKnife;
	bool m_bPushedRequiredWeapon = false;
	PathFollower m_path;
	CountdownTimer m_repathTimer;
};
