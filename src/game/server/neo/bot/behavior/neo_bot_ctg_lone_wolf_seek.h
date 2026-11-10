#pragma once

#include "bot/neo_bot.h"
#include "bot/behavior/neo_bot_ctg_lone_wolf.h"
#include "utlmap.h"
#include <memory>

class CNEOIgnoredWeaponsCache;

//--------------------------------------------------------------------------------------------------------
class CNEOBotCtgLoneWolfSeek : public CNEOBotCtgLoneWolf
{
public:
	typedef CNEOBotCtgLoneWolf BaseClass;
	CNEOBotCtgLoneWolfSeek( bool detpackHunt = false );
	virtual ~CNEOBotCtgLoneWolfSeek();

	virtual ActionResult< CNEOBot > OnStart( CNEOBot *me, Action< CNEOBot > *priorAction ) override;
	virtual ActionResult< CNEOBot > Update( CNEOBot *me, float interval ) override;

	virtual EventDesiredResult< CNEOBot > OnStuck( CNEOBot *me ) override;
	virtual EventDesiredResult< CNEOBot > OnMoveToSuccess( CNEOBot *me, const Path *path ) override;
	virtual EventDesiredResult< CNEOBot > OnMoveToFailure( CNEOBot *me, const Path *path, MoveToFailureType reason ) override;

	virtual const char *GetName( void ) const override { return "ctgLoneWolfSeek"; }

private:
	static constexpr float DEBUG_OVERLAY_DURATION = 3.0f;
	static constexpr float ENEMY_LAST_KNOWN_DIST = 100.0f;
	static constexpr float PATH_RECOMPUTE_DIST = 64.0f;
	static constexpr float SEARCH_WAYPOINT_REACHED_DIST = 200.0f;
	// Research hearing: a new noise moves the search only this far away from the current waypoint,
	static constexpr float HEARD_RETARGET_DIST = 256.0f;
	// and no more often than this
	static constexpr float HEARD_RETARGET_INTERVAL = 1.0f;

	Vector m_vecLastGhostPos{ CNEO_Player::VECTOR_INVALID_WAYPOINT };
	CNavArea *m_pCachedGhostArea{ nullptr };

	CUtlMap<int, bool> m_exploredAreaIds;
	int m_iExplorationTargetId{-1};
	Vector m_vecSearchWaypoint{CNEO_Player::VECTOR_INVALID_WAYPOINT};

	std::unique_ptr<CNEOIgnoredWeaponsCache> m_pIgnoredWeapons;
	CountdownTimer m_scavengeTimer;
	bool m_bDetpackHunt;

	float m_flHeardWaypointWhen{ -1.0f };
	CountdownTimer m_heardRetargetTimer;

	void ConsiderHeardWaypoint( CNEOBot *me );

	void MarkVisibleAreasAsExplored( CNEOBot *me, CNavArea *currentArea, CNavArea *ghostArea = nullptr );
};
