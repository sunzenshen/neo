#pragma once

#include "bot/neo_bot.h"

class CNEOBotHoldInSmoke : public Action< CNEOBot >
{
public:
	explicit CNEOBotHoldInSmoke( const Vector &vecThreatPos ) : m_vecThreatPos( vecThreatPos )
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
	EHANDLE m_hSmoke;
	PathFollower m_path;
	CountdownTimer m_timeout;
	bool m_bEnteredSmoke = false;
};
