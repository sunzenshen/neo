#pragma once

#include "nav_pathfind.h"

class CNEOBot;
class PathFollower;
class ChasePath;
class CBaseEntity;

// bAvoidIrreversibleDrops: soften the path away from a one-way drop - an edge this bot can cross
// forward but not back. It is a preference, not a ban: a route that has no other way still takes
// the drop. Meant for a path that ends in *holding* a position (a cut-off, an ambush) rather than
// passing through one, where getting stuck downhill of a plan that changed is the failure mode -
// see CNEOBotCtgEnemyInterceptCapPath::RepathToCutOff for the motivating case. Off by default so
// every existing caller is unaffected.
bool CNEOBotPathCompute
(
	CNEOBot* bot,
	PathFollower& path,
	const Vector& goal,
	RouteType route,
	float maxPathLength = PATH_NO_LENGTH_LIMIT,
	bool includeGoalIfPathFails = PATH_TRUNCATE_INCOMPLETE_PATH,
	bool requireGoalArea = false,
	bool bAvoidIrreversibleDrops = false
);

bool CNEOBotPathUpdateChase
(
	CNEOBot *bot,
	ChasePath &path,
	CBaseEntity *subject,
	RouteType route,
	Vector *pPredictedSubjectPos = NULL
);
