#pragma once

#include "nav_pathfind.h"

class CNEOBot;
class PathFollower;
class ChasePath;
class CBaseEntity;

bool CNEOBotPathCompute
(
	CNEOBot* bot,
	PathFollower& path,
	const Vector& goal,
	RouteType route,
	float maxPathLength = PATH_NO_LENGTH_LIMIT,
	bool includeGoalIfPathFails = PATH_TRUNCATE_INCOMPLETE_PATH,
	bool requireGoalArea = false
);

// Same routing as CNEOBotPathCompute, but does NOT reserve the resulting path in the friendly
// path-reservation system. Use this to plot a hypothetical route (length, nav areas) for a
// decision without making other bots avoid it.
bool CNEOBotPathPreview
(
	CNEOBot* bot,
	PathFollower& path,
	const Vector& goal,
	RouteType route,
	float maxPathLength = PATH_NO_LENGTH_LIMIT,
	bool includeGoalIfPathFails = PATH_TRUNCATE_INCOMPLETE_PATH,
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
