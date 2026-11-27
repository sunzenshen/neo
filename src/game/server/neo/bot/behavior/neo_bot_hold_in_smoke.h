#pragma once

#include "bot/neo_bot.h"

class CNEOBotHoldInSmoke : public Action< CNEOBot >
{
public:
	CNEOBotHoldInSmoke( const Vector &vecThreatPos, const Vector &vecAimPoint ) : m_vecThreatPos( vecThreatPos ), m_vecAimPoint( vecAimPoint )
	{
	}

	ActionResult< CNEOBot > OnStart( CNEOBot *me, Action< CNEOBot > *priorAction ) override;
	ActionResult< CNEOBot > Update( CNEOBot *me, float interval ) override;
	EventDesiredResult< CNEOBot > OnInjured( CNEOBot *me, const CTakeDamageInfo &info ) override;
	// Stay in smoke until an enemy hits us, while still allowing grenade avoidance.
	QueryResultType ShouldRetreat( const INextBot *me ) const override { return ANSWER_NO; }
	const char *GetName() const override { return "HoldInSmoke"; }

private:
	const Vector m_vecThreatPos;
	const Vector m_vecAimPoint;
	EHANDLE m_hSmoke;
	PathFollower m_path;
	CountdownTimer m_timeout;
	bool m_bEnteredSmoke = false;
};
