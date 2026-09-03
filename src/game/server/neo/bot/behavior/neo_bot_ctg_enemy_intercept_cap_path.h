#pragma once

#include "bot/neo_bot.h"

class CNavArea;

//--------------------------------------------------------------------------------------------------------
// Heads for the active cap zone nearest the enemy ghost carrier, along this bot's own fastest
// route. The instant the bot steps onto a nav area that lies on the carrier's predicted fastest
// route to that cap, the interception is done and it hands off to CNEOBotCtgEnemyChase - the bot
// is now on the carrier's path and should run it down.
//
// The carrier's route is re-plotted on a slow timer in case the carrier has veered toward a
// different cap since the bot set out.
class CNEOBotCtgEnemyInterceptCapPath : public Action< CNEOBot >
{
public:
	virtual ActionResult< CNEOBot >	OnStart( CNEOBot *me, Action< CNEOBot > *priorAction ) override;
	virtual ActionResult< CNEOBot >	Update( CNEOBot *me, float interval ) override;

	virtual const char *GetName( void ) const override { return "ctgEnemyInterceptCapPath"; }

private:
	bool RecomputeCarrierRoute( CNEOBot *me );

	PathFollower m_path;
	CUtlVector< CNavArea * > m_carrierRoute;	// nav areas on the carrier's predicted fastest route to its cap
	Vector m_goalPos = vec3_origin;			// cap zone this bot paths toward
	CountdownTimer m_replanTimer;			// throttle re-plotting the carrier route
};
