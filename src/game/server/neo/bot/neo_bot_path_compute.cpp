#include "cbase.h"
#include "bot/neo_bot.h"
#include "neo_bot_path_compute.h"
#include "bot/neo_bot_path_reservation.h"
#include "NextBot/Path/NextBotPathFollow.h"
#include "NextBot/Path/NextBotChasePath.h"
#include "nav_mesh.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

static void CNEOBotReservePath(CNEOBot* me, PathFollower& path)
{
	if (!neo_bot_path_reservation_enable.GetBool() || !path.IsValid())
	{
		return;
	}

	CNEOBotPathReservations()->ReleaseAllAreas(me);

	const float reservation_distance = neo_bot_path_reservation_distance.GetFloat();
	const float reservation_duration = neo_bot_path_reservation_duration.GetFloat();

	for (const Path::Segment* seg = path.FirstSegment(); seg; seg = path.NextSegment(seg))
	{
		if (seg->distanceFromStart > reservation_distance)
		{
			break;
		}

		if (seg->area)
		{
			CNEOBotPathReservations()->ReserveArea(seg->area, me, reservation_duration);
		}
	}
}

// A path that stops short of the goal is only wanted if the caller asked for partial paths
static bool IsPathUsable(const PathFollower& path, bool reachedGoal, bool includeGoalIfPathFails)
{
	return path.IsValid() && (reachedGoal || includeGoalIfPathFails);
}

// By default, assumes that we would prefer to have partial paths even if we can't make a full path
// to accomodate scenarios like getting as close as possible to a hazard area but not into it
// Set includeGoalIfPathFails to false if you need to get the full path (or nothing on failure)
bool CNEOBotPathCompute(CNEOBot* bot, PathFollower& path, const Vector& goal, RouteType route, float maxPathLength, bool includeGoalIfPathFails, bool requireGoalArea)
{
	Assert(goal.IsValid());

	CNEOBotPathCost cost_with_reservations(bot, route);
	bool reachedGoal = path.Compute(bot, goal, cost_with_reservations, maxPathLength, includeGoalIfPathFails, requireGoalArea);
	if (IsPathUsable(path, reachedGoal, includeGoalIfPathFails))
	{
		CNEOBotReservePath(bot, path);
		return true;
	}

	CNEOBotPathCost cost_without_reservations(bot, FASTEST_ROUTE);
	cost_without_reservations.m_bIgnoreReservations = true;
	reachedGoal = path.Compute(bot, goal, cost_without_reservations, maxPathLength, includeGoalIfPathFails, requireGoalArea);
	if (IsPathUsable(path, reachedGoal, includeGoalIfPathFails))
	{
		CNEOBotReservePath(bot, path);
		return true;
	}

	// Path::Compute leaves a partial path valid even when told not to append the goal
	path.Invalidate();
	return false;
}

bool CNEOBotPathUpdateChase(CNEOBot* bot, ChasePath& path, CBaseEntity* subject, RouteType route, Vector* pPredictedSubjectPos)
{
	CNEOBotPathCost cost_with_reservations(bot, route);
	path.Update(bot, subject, cost_with_reservations, pPredictedSubjectPos);
	if (path.IsValid())
	{
		CNEOBotReservePath(bot, path);
		return true;
	}

	CNEOBotPathCost cost_without_reservations(bot, route);
	cost_without_reservations.m_bIgnoreReservations = true;
	path.Update(bot, subject, cost_without_reservations, pPredictedSubjectPos);
	if (path.IsValid())
	{
		CNEOBotReservePath(bot, path);
		return true;
	}

	return false;
}

// A chase goal off the mesh moves onto the nearest area when it is at most a player's width past the area's edge,
static constexpr float BOT_CHASE_GOAL_MAX_OUTSIDE_AREA = 2.0f * HalfHumanWidth;
// and at most a step below the area's surface (a jump above it bounds a subject in mid-jump)
static constexpr float BOT_CHASE_GOAL_MAX_DEPTH_BELOW_AREA = StepHeight;

static bool GetChaseGoalOnMesh(INextBot* bot, const Vector& pos, Vector* onMesh)
{
	const float maxHeightAboveArea = bot->GetLocomotionInterface()->GetMaxJumpHeight();

	CNavArea* area = TheNavMesh->GetNavArea(pos, maxHeightAboveArea);
	if (!area)
	{
		area = TheNavMesh->GetNearestNavArea(pos, true, BOT_CHASE_GOAL_MAX_OUTSIDE_AREA, true);
	}

	if (!area)
	{
		return false;
	}

	area->GetClosestPointOnArea(pos, onMesh);

	const float heightAboveArea = pos.z - onMesh->z;
	if (heightAboveArea > maxHeightAboveArea || heightAboveArea < -BOT_CHASE_GOAL_MAX_DEPTH_BELOW_AREA)
	{
		return false;
	}

	// a step above the area's plane, as nav_generate areas often sit a little below the floor they cover
	onMesh->z += StepHeight;
	return true;
}

// ChasePath leads its subject by the subject's velocity, and the lead can end past an area's edge, inside a wall or over a void,
// where Path::Compute finds no goal area and the chase fails with no path
Vector CNEOBotChasePath::PredictSubjectPosition(INextBot* bot, CBaseEntity* subject) const
{
	const Vector predicted = ChasePath::PredictSubjectPosition(bot, subject);

	Vector onMesh;
	if (GetChaseGoalOnMesh(bot, predicted, &onMesh))
	{
		return onMesh;
	}

	// a lead with no area near it heads for the subject itself
	if (GetChaseGoalOnMesh(bot, subject->GetAbsOrigin(), &onMesh))
	{
		return onMesh;
	}

	return predicted;
}
