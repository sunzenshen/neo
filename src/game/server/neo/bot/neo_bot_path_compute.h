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

bool CNEOBotPathUpdateChase
(
	CNEOBot *bot,
	ChasePath &path,
	CBaseEntity *subject,
	RouteType route,
	Vector *pPredictedSubjectPos = NULL
);

// Which nav-mesh-baked visibility query decides whether a candidate area counts as "seen from
// here" - both are precomputed per-area relationships ("very fast" per CNavArea's own docs), not
// a live trace, so walking a path of candidates stays cheap regardless of how many bots do it.
// EMERGENCE_TEST_POTENTIALLY_VISIBLE (IsPotentiallyVisible - any part of the area might be seen
// from any part of ours) is looser and is what grenade-throw clearance uses - a thrown grenade
// already tolerates bouncing off something the coarse check missed, the way a human's throw can.
// EMERGENCE_TEST_COMPLETELY_VISIBLE (IsCompletelyVisible - the whole area is seen from somewhere
// in ours) is stricter and is what the heard-threat look uses - a bot caught staring at a spot
// that is only partly visible reads as an obvious bug to a spectator, where an imprecise
// prediction does not.
enum EmergencePointTest
{
	EMERGENCE_TEST_POTENTIALLY_VISIBLE,
	EMERGENCE_TEST_COMPLETELY_VISIBLE,
};

// Walk the fastest nav path from familiarPos to obscuredPos and return the point
// farthest along it (nearest obscuredPos) that the bot still has a clear line to -
// i.e. where someone travelling that route toward the bot would first come into view.
// Returns a reference to vec3_invalid when there is no path or no visible point.
const Vector &CNEOBotFindPathEmergencePoint
(
	const CNEOBot *bot,
	const Vector &familiarPos,
	const Vector &obscuredPos,
	EmergencePointTest visTest
);
