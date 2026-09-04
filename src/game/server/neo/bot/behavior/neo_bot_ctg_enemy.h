#ifndef NEO_BOT_CTG_ENEMY_H
#define NEO_BOT_CTG_ENEMY_H

#include "bot/neo_bot.h"

class CNEO_Player;
class CNEOGhostCapturePoint;
class CNavArea;

//--------------------------------------------------------------------------------------------------------
// A nav route predicted for somebody, ordered from where they are towards where they are going,
// with the travel distance to each area alongside it.
//
// Lengths are summed between area centres, which overstates the distance a player actually walks
// (they cut corners). Two of these are therefore only comparable with each other, never with a
// PathFollower's GetLength().
struct CNEOBotPredictedRoute
{
	CUtlVector< CNavArea * > areas;		// [0] is the start area, the last entry is the goal area
	CUtlVector< float > travel;			// travel[i] is the distance from areas[0] to areas[i]

	void Reset()
	{
		areas.RemoveAll();
		travel.RemoveAll();
	}

	int Count() const { return areas.Count(); }
	float Length() const { return travel.Count() > 0 ? travel[ travel.Count() - 1 ] : 0.0f; }
};

//--------------------------------------------------------------------------------------------------------
// Decides how this bot deals with an enemy player carrying the ghost, then hands over to the
// behaviour that carries the decision out. One shot: it never stays on the behaviour stack. When
// the child finishes (the carrier died, dropped the ghost or captured), control returns to the
// seek dispatcher, which re-enters here for a fresh decision.
//
// Everything here plans from what any opponent can see - the ghost marker gives the carrier's
// position away, and the cap zones are fixed map geometry - plus a naive shortest-route guess at
// where the carrier will go. It never reads the carrier's own goal or route.
class CNEOBotCtgEnemy : public Action< CNEOBot >
{
public:
	virtual ActionResult< CNEOBot >	Update( CNEOBot *me, float interval ) override;

	virtual const char *GetName( void ) const override { return "ctgEnemy"; }

	// Where to meet the carrier: the earliest area on its predicted route that this bot can reach
	// before the carrier gets there. Earliest, because that meets the carrier as far from its cap
	// as this bot can manage.
	struct CutOff
	{
		CNavArea *pArea = nullptr;
		Vector vecPos = vec3_origin;
		int iCarrierRouteIndex = -1;	// index of pArea within the carrier route
	};

	// The living enemy player carrying the ghost, or nullptr if there is none.
	static CNEO_Player *EnemyGhostCarrier( CNEOBot *me );

	// True when an enemy is carrying the ghost and is already nearer to the cap it is heading for
	// (straight-line) than `me` is - the position race for that cap is lost. Used to decide when
	// self-preservation stops paying: if the carrier is this close, retreating to reload or fall
	// back buys nothing but time we do not have, and preventing the capture outranks surviving the
	// engagement (see sv_neo_bot_ctg_no_retreat_when_carrier_ahead).
	static bool IsLosingTheRace( CNEOBot *me );

	// Predicts the route the ghost would take from where it is now (NEORules()->GetGhostPos() -
	// the carrier's position while carried, the ground/marker position while not) to the nearest
	// cap the enemy of `me` could score into, and finds the earliest area on it this bot can win
	// the race to. Returns false when there is no such point, which is also the test for "the
	// ghost is ahead of me and I cannot get in front of it". Works whether or not an enemy is
	// carrying the ghost, so freezetime callers can use it before any pickup. Optionally hands
	// back the predicted route so a caller that wants to watch the approach need not re-plot it.
	static bool FindCutOff( CNEOBot *me, CutOff &cutOff,
		CNEOBotPredictedRoute *pOutGhostRoute = nullptr );

	// The active scoring zone nearest vecFrom that iTeam can capture into - owned by iTeam, or
	// neutral. Public: this is the one piece of CNEORules()->m_pGhostCaps knowledge every CTG
	// behaviour that reasons about "which zone" needs, and only CNEOBotCtgEnemy is a friend of
	// CNEORules for it.
	static CNEOGhostCapturePoint *NearestCapForTeam( int iTeam, const Vector &vecFrom );

private:
	// Reaches NEORules()->m_pGhostCaps, so it has to be a member: that list is only open to the
	// CTG bot behaviours named as friends of CNEORules. The cap the ghost is heading for: the
	// nearest active zone the enemy of `me` can score into, straight-line from the ghost.
	static CNEOGhostCapturePoint *GhostGoalCap( CNEOBot *me );

	// Plots the naive shortest route from the ghost's current area to vecGoalOut (GhostGoalCap's
	// origin). Returns false when there is no scorable enemy cap or no route to it.
	static bool BuildGhostRoute( CNEOBot *me, CNEOBotPredictedRoute &routeOut, Vector &vecGoalOut );
};

#endif // NEO_BOT_CTG_ENEMY_H
