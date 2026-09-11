#include "cbase.h"
#include "bot/neo_bot.h"
#include "neo_bot_path_compute.h"
#include "bot/neo_bot_path_reservation.h"
#include "NextBot/Path/NextBotPathFollow.h"
#include "NextBot/Path/NextBotChasePath.h"
#include "nav_mesh.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

extern ConVar neo_bot_path_reservation_enable;
extern ConVar neo_bot_path_reservation_penalty;
extern ConVar neo_bot_path_reservation_duration;
extern ConVar neo_bot_path_reservation_distance;

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

bool CNEOBotPathCompute(CNEOBot* bot, PathFollower& path, const Vector& goal, RouteType route, float maxPathLength, bool includeGoalIfPathFails, bool requireGoalArea)
{
	Assert(goal.IsValid());

	CNEOBotPathCost cost_with_reservations(bot, route);
	if (path.Compute(bot, goal, cost_with_reservations, maxPathLength, includeGoalIfPathFails, requireGoalArea) && path.IsValid())
	{
		CNEOBotReservePath(bot, path);
		return true;
	}

	CNEOBotPathCost cost_without_reservations(bot, route);
	cost_without_reservations.m_bIgnoreReservations = true;
	if (path.Compute(bot, goal, cost_without_reservations, maxPathLength, includeGoalIfPathFails, requireGoalArea) && path.IsValid())
	{
		CNEOBotReservePath(bot, path);
		return true;
	}

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

//---------------------------------------------------------------------------------------------
// proposal 0019 (fairness pass): how many random hops through adjacent nav areas a bot takes,
// away from a threat's true last-known area, before predicting where it will emerge. An exact
// guess every time is a perfect prediction from imperfect (heard/seen, not omniscient) sensing -
// this makes the *reasoning* imperfect too, scaled so Expert wanders least and Easy wanders
// most. Applies to both the heard-threat look and grenade-throw targeting, since both predict
// off the same "last known position" signal (the grenade code already flagged this as a
// "CHEAT" - see neo_bot_grenade_throw_frag.cpp).
static int NEOBotEmergenceWalkSteps(CNEOBot::DifficultyType difficulty)
{
	switch (difficulty)
	{
	case CNEOBot::EXPERT: return 2;
	case CNEOBot::HARD:   return 4;
	case CNEOBot::NORMAL: return 6;
	case CNEOBot::EASY:   return 8;
	}

	return 6;
}

// Take 'steps' random hops through adjacent nav areas from 'start'. Stops early (returning
// wherever it got to) if an area has no further connections.
static CNavArea *NEOBotRandomWalkArea(CNavArea *start, int steps)
{
	CNavArea *area = start;

	for (int i = 0; i < steps && area; ++i)
	{
		CUtlVector<CNavArea *> adjacent;
		for (int dir = 0; dir < NUM_DIRECTIONS; ++dir)
		{
			const NavConnectVector *connect = area->GetAdjacentAreas((NavDirType)dir);
			FOR_EACH_VEC((*connect), it)
			{
				if ((*connect)[it].area)
				{
					adjacent.AddToTail((*connect)[it].area);
				}
			}
		}

		if (adjacent.Count() == 0)
		{
			break;
		}

		area = adjacent[RandomInt(0, adjacent.Count() - 1)];
	}

	return area;
}

// Used to anticipate where an enemy would emerge from cover ahead along a path.
// Walks the path in reverse (from the obscured position back toward the bot) and
// returns the farthest point the bot can still see - the spot where a mover on that
// route first comes into view. Assumes familiarPos is nearer the bot than obscuredPos.
// Shared by the grenade-throw behaviours and the bot's heard-threat look (proposal 0019).
const Vector &CNEOBotFindPathEmergencePoint(const CNEOBot *bot, const Vector &familiarPos,
	const Vector &obscuredPos, EmergencePointTest visTest)
{
	CNavArea *familiarArea = TheNavMesh->GetNavArea(familiarPos);
	if (!familiarArea)
	{
		return vec3_invalid;
	}

	CNavArea *obscuredArea = TheNavMesh->GetNavArea(obscuredPos);
	if (!obscuredArea)
	{
		return vec3_invalid;
	}

	// Fuzz which area we treat as the threat's position before predicting its emergence point.
	obscuredArea = NEOBotRandomWalkArea(obscuredArea, NEOBotEmergenceWalkSteps(bot->GetDifficulty()));

	// Visibility from the bot's own area, via the nav mesh's precomputed per-area visibility
	// (see EmergencePointTest) rather than a live trace per candidate - keeps this cheap no
	// matter how many bots walk a path of candidates. No area of our own means no visibility.
	CNavArea *myArea = bot->GetLastKnownArea();
	auto isVisibleFromMe = [myArea, visTest](CNavArea *area) -> bool
	{
		if (!myArea)
		{
			return false;
		}

		return (visTest == EMERGENCE_TEST_POTENTIALLY_VISIBLE)
			? myArea->IsPotentiallyVisible(area)
			: myArea->IsCompletelyVisible(area);
	};

	// The walk can land us somewhere already in plain view - an "emergence point" deep inside
	// an area we can already see would look wrong, so use it directly instead of running the
	// backward search meant for a position that starts out hidden.
	if (isVisibleFromMe(obscuredArea))
	{
		return obscuredArea->GetCenter();
	}

	ShortestPathCost cost;
	const Vector &vecGoal = obscuredArea->GetCenter();
	if (NavAreaBuildPath(familiarArea, obscuredArea, &vecGoal, cost))
	{
		// search backwards from the obscured position for the first area visible to the bot
		for (CNavArea *area = obscuredArea; area; area = area->GetParent())
		{
			if (isVisibleFromMe(area))
			{
				return area->GetCenter();
			}

			if (area == familiarArea)
			{
				return vec3_invalid;
			}
		}
	}

	return vec3_invalid;
}
