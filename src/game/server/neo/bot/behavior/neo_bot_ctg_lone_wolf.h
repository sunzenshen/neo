#pragma once

#include "bot/neo_bot.h"
#include "bot/neo_bot_hearing.h"

//--------------------------------------------------------------------------------------------------------
class CNEOBotCtgLoneWolf : public Action< CNEOBot >
{
public:
	CNEOBotCtgLoneWolf( void ) = default;

	virtual ActionResult< CNEOBot > OnStart( CNEOBot *me, Action< CNEOBot > *priorAction ) override;
	virtual ActionResult< CNEOBot > Update( CNEOBot *me, float interval ) override;
	virtual ActionResult< CNEOBot > OnSuspend( CNEOBot *me, Action< CNEOBot > *interruptingAction ) override;
	virtual ActionResult< CNEOBot > OnResume( CNEOBot *me, Action< CNEOBot > *interruptingAction ) override;

	virtual EventDesiredResult< CNEOBot > OnStuck( CNEOBot *me ) override;
	virtual EventDesiredResult< CNEOBot > OnMoveToFailure( CNEOBot *me, const Path *path, MoveToFailureType reason ) override;

	virtual const char *GetName( void ) const override { return "ctgLoneWolf"; }

protected:
	virtual ActionResult< CNEOBot > ConsiderGhostInterception( CNEOBot *me, const CBaseCombatCharacter *pGhostOwner = nullptr );
	virtual ActionResult< CNEOBot > ConsiderGhostVisualCheck( CNEOBot *me );
	virtual ActionResult< CNEOBot > ConsiderGhostCaptureTransition( CNEOBot *me );

	Vector GetNearestEnemyCapPoint( CNEOBot *me ) const;

	// Research: turn toward the loudest enemy noise; true while one is heard
	bool UpdateHearing( CNEOBot *me );

	static constexpr float HEARD_LOOK_TIME = 1.0f;

	PathFollower m_path;

	NEOBotHearing::HeardNoise m_heard;
	bool m_bHeardEnemy{ false };
};
