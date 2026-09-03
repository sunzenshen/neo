#ifndef NEO_BOT_CTG_ENEMY_H
#define NEO_BOT_CTG_ENEMY_H

#include "bot/neo_bot.h"

class CNEO_Player;
class CNEOGhostCapturePoint;
class CNavArea;

//--------------------------------------------------------------------------------------------------------
class CNEOBotCtgEnemy : public Action< CNEOBot >
{
public:
	virtual ActionResult< CNEOBot >	Update( CNEOBot *me, float interval ) override;

	virtual const char *GetName( void ) const override { return "ctgEnemy"; }

	static CNEO_Player *EnemyGhostCarrier( CNEOBot *me );
	static CNEOGhostCapturePoint *CarrierGoalCap( CNEOBot *me, CNEO_Player *pGhostCarrier );
	static bool CarrierRouteToGoal( CNEOBot *me, CNEO_Player *pGhostCarrier,
		CUtlVector< CNavArea * > &routeAreas, float &routeLength, Vector &goalPos );
};

#endif // NEO_BOT_CTG_ENEMY_H
