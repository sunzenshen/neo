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

	// Predicts the carrier's route and finds the earliest point on it this bot can win the race
	// to. Returns false when there is no such point, which is also the test for "the carrier is
	// ahead of me and I cannot get in front of it". Optionally hands back the carrier's route, so
	// a caller that wants to watch the approach does not have to plot it again.
	static bool FindCutOff( CNEOBot *me, CNEO_Player *pGhostCarrier, CutOff &cutOff,
		CNEOBotPredictedRoute *pOutCarrierRoute = nullptr );

	// Straight-line distance from the carrier to the scoring zone it is nearest to, or -1 when it
	// has none. Straight-line because this is a cheap "how much time is left" question asked every
	// tick, not a route: capture is instantaneous on touching the zone, so what matters is only
	// whether the carrier is nearly there.
	static float CarrierDistToGoal( CNEO_Player *pGhostCarrier );

private:
	// Reaches NEORules()->m_pGhostCaps, so it has to be a member: that list is only open to the
	// CTG bot behaviours named as friends of CNEORules.
	//
	// iRank selects among the carrier's scoring zones by straight-line distance from it: 0 is the
	// nearest, which is the zone the carrier itself is heading for. Higher ranks exist so a
	// defence can cover a second zone instead of every defender guessing the same one; a rank the
	// map cannot supply falls back to the nearest.
	static const int kMaxCapRank = 4;
	static CNEOGhostCapturePoint *CarrierGoalCap( CNEO_Player *pGhostCarrier, int iRank = 0 );
};

#endif // NEO_BOT_CTG_ENEMY_H
