#pragma once

#include "nav_pathfind.h"
#include "NextBot/Path/NextBotChasePath.h"

class CNEOBot;
class PathFollower;
class CBaseEntity;

bool CNEOBotPathCompute
(
	CNEOBot* bot,
	PathFollower& path,
	const Vector& goal,
	RouteType route,
	float maxPathLength = PATH_NO_LENGTH_LIMIT,
	bool includeGoalIfPathFails = true, // return incomplete path if full path not found by default
	bool requireGoalArea = false
);

bool CNEOBotPathUpdateChase
(
	CNEOBot *bot,
	ChasePath &path,
	CBaseEntity *subject,
	RouteType route,
	Vector *pPredictedSubjectPos = NULL
);

// A chase that leads its subject toward a position on the nav mesh,
// so the path has a goal area to reach
class CNEOBotChasePath : public ChasePath
{
public:
	CNEOBotChasePath( SubjectChaseType chaseHow ) : ChasePath( chaseHow ) { }

	virtual Vector PredictSubjectPosition( INextBot *bot, CBaseEntity *subject ) const override;
};
