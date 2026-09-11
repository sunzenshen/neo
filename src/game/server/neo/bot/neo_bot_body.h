#pragma once

#include "NextBot/Player/NextBotPlayerBody.h"

class CNEO_Player;

//----------------------------------------------------------------------------
class CNEOBotBody : public PlayerBody
{
public:
	CNEOBotBody( INextBot* bot ) : PlayerBody( bot )
	{
	}

	virtual ~CNEOBotBody() { }

	CNEO_Player *NEOPlayerEnt() const;

	float GetHeadAimTrackingInterval() const override;			// return how often we should sample our target's position and velocity to update our aim tracking, to allow realistic slop in tracking
	float GetMaxHeadAngularVelocity() const override;			// slower turn for non-threat scanning looks so spectators riding the bot's eyes aren't whipped around (proposal 0019)

	// proposal 0019: for one look, override the scan-speed cap with a faster rate (0 = use the
	// idle-scan default). Set each tick by the look that wants it; cleared by whoever drives looks.
	void SetScanSpeedOverride( float degPerSec ) { m_scanSpeedOverride = degPerSec; }
	float GetCloakPower() const;          		    		// return available thermoptic charge
	bool IsCloakEnabled() const;                  			// return if thermoptic is enabled

	// NEO NOTE (nullsystem): These are override to use the NT;RE's VEC_... macros
	float GetHullWidth() const override;
	float GetStandHullHeight() const override;
	float GetCrouchHullHeight() const override;
	const Vector &GetHullMins() const override;
	const Vector &GetHullMaxs() const override;

private:
	float m_scanSpeedOverride = 0.0f;
};

