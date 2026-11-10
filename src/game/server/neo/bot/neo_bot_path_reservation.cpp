#include "cbase.h"
#include "neo/bot/neo_bot_path_reservation.h"
#include "neo/bot/neo_bot.h"
#include "nav_mesh.h"
#include "neo_gamerules.h"
#include "neo/neo_detpack.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"


ConVar neo_bot_path_reservation_enable("neo_bot_path_reservation_enable", "1", FCVAR_NONE,
    "Enable the bot path reservation system.", true, 0, true, 1);

ConVar neo_bot_path_reservation_duration("neo_bot_path_reservation_duration", "30.0", FCVAR_NONE,
    "How long a path reservation lasts, in seconds.", true, 1, false, 0);

ConVar neo_bot_path_reservation_distance("neo_bot_path_reservation_distance", "100000", FCVAR_NONE,
    "How far along the path to reserve, in Hammer units.", true, 0, false, 0);

ConVar neo_bot_path_reservation_penalty("neo_bot_path_reservation_penalty", "3000", FCVAR_NONE,
    "Pathing cost penalty for a reserved area.", true, 0, false, 0);

ConVar neo_bot_path_reservation_friendly_penalty_enable("neo_bot_path_reservation_friendly_penalty_enable", "1", FCVAR_NONE,
    "Whether to update or retrieve the area friendly reservation penalty.", true, 0, true, 1);

ConVar neo_bot_path_reservation_avoid_penalty_enable("neo_bot_path_reservation_avoid_penalty_enable", "1", FCVAR_NONE,
    "Whether to update or retrieve the area avoid penalty.", true, 0, true, 1);

ConVar neo_bot_path_reservation_killed_penalty("neo_bot_path_reservation_killed_penalty", "10", FCVAR_NONE,
    "Path selection penalty added to a nav area each time a bot dies moving through that area.", true, 0, false, 0);

ConVar neo_bot_path_reservation_onstuck_penalty("neo_bot_path_reservation_onstuck_penalty", "1000", FCVAR_NONE,
    "Path selection penalty added to a nav area each time a bot gets stuck moving through that area.", true, 0, false, 0);

ConVar neo_bot_research_buddy("neo_bot_research_buddy", "0", FCVAR_CHEAT,
    "Research: split each bot team into groups that share one route: 0 off, 1 pairs, 2 trios.", true, 0, true, 2);

ConVar neo_bot_research_buddy_claims("neo_bot_research_buddy_claims", "1", FCVAR_CHEAT,
    "Research: with neo_bot_research_buddy, a group member's path reservations cost the others nothing (0 shares the route preference only).", true, 0, true, 1);

ConVar neo_bot_research_buddy_log("neo_bot_research_buddy_log", "0", FCVAR_CHEAT,
    "Research: print a NEO_BUDDY line for each bot when its team's groups are formed.", true, 0, true, 1);

extern ConVar neo_bot_research_team_arm;

//-------------------------------------------------------------------------------------------------
// Research: how many bots of this bot's team plan one shared route, 1 when the research is off for its team
int NeoBotBuddyGroupSize( const CNEOBot *bot )
{
    const int buddyMode = neo_bot_research_buddy.GetInt();
    if ( buddyMode <= 0 || !NEORules()->GetTeamPlayEnabled() )
    {
        return 1;
    }

    const int teamArm = neo_bot_research_team_arm.GetInt();
    if ( teamArm != 0 && bot->GetTeamNumber() != teamArm )
    {
        return 1;
    }

    return buddyMode + 1;
}

bool NeoBotBuddyClaimsFree()
{
    return neo_bot_research_buddy_claims.GetBool();
}



CNEOBotPathReservationSystem* CNEOBotPathReservations()
{
    static CNEOBotPathReservationSystem g_BotPathReservations;
    return &g_BotPathReservations;
}

//-------------------------------------------------------------------------------------------------
// Check if reservation claim is valid and not expired.
static bool ClaimIsLive(const AreaClaim_t &claim)
{
    return claim.hOwner.Get() != NULL && claim.flExpirationTime >= gpGlobals->curtime;
}

//-------------------------------------------------------------------------------------------------
// Count live reservations on a nav area.
int CNEOBotPathReservationSystem::CountLiveClaims(const AreaReservation_t &res, const CNEOBot *excluding, int excludingGroup) const
{
    int count = 0;
    for (int i = 0; i < res.claims.Count(); ++i)
    {
        const AreaClaim_t &claim = res.claims[i];
        if (!ClaimIsLive(claim))
        {
            continue;
        }
        if (excluding != NULL && claim.hOwner.Get() == excluding)
        {
            continue;
        }
        if ( excludingGroup != NO_BUDDY_GROUP && m_iBuddyGroup[claim.hOwner->entindex()] == excludingGroup )
        {
            continue;
        }
        ++count;
    }
    return count;
}

//-------------------------------------------------------------------------------------------------
/**
 * Record (or refresh) this bot's claim on a nav area for the given duration.
 */
void CNEOBotPathReservationSystem::ReserveArea(CNavArea *area, CNEOBot *bot, float duration)
{
    if (!area || !bot)
    {
        return;
    }

    if (!NEORules()->GetTeamPlayEnabled())
    {
        return;
    }

    int team = bot->GetTeamNumber();
    if (team < 0 || team >= TEAM__TOTAL)
    {
        return;
    }

    const int areaID = area->GetID();
    const float flExpiration = gpGlobals->curtime + duration;

    int reservationIndex = m_Reservations[team].Find(areaID);
    if (reservationIndex == m_Reservations[team].InvalidIndex())
    {
        reservationIndex = m_Reservations[team].Insert(areaID);
    }

    // Refresh this bot's existing claim, or add a new one.
    // For tracking number of teammates routing through here.
    AreaReservation_t &res = m_Reservations[team][reservationIndex];
    bool bHadClaim = false;
    for (int i = 0; i < res.claims.Count(); ++i)
    {
        if (res.claims[i].hOwner.Get() == bot)
        {
            res.claims[i].flExpirationTime = flExpiration;
            bHadClaim = true;
            break;
        }
    }
    if (!bHadClaim)
    {
        AreaClaim_t claim;
        claim.hOwner = bot;
        claim.flExpirationTime = flExpiration;
        res.claims.AddToTail(claim);
    }

    // Reverse index for fast release.
    int botIndex = m_BotReservedAreas.Find(bot->entindex());
    if (botIndex == m_BotReservedAreas.InvalidIndex())
    {
        botIndex = m_BotReservedAreas.Insert(bot->entindex());
    }
    if (m_BotReservedAreas[botIndex].areas.Find(area) == -1)
    {
        m_BotReservedAreas[botIndex].areas.AddToTail(area);
    }
}

//-------------------------------------------------------------------------------------------------
/**
 * Release all navigation area reservations for a specific bot.
 */
void CNEOBotPathReservationSystem::ReleaseAllAreas(CNEOBot *bot)
{
    if (!bot)
    {
        return;
    }

    int botIndex = m_BotReservedAreas.Find(bot->entindex());
    if (botIndex == m_BotReservedAreas.InvalidIndex())
    {
        return; // No reservations for this bot
    }

    const int team = bot->GetTeamNumber();
    if (team >= 0 && team < TEAM__TOTAL)
    {
        const CUtlVector<CNavArea*> &areas = m_BotReservedAreas[botIndex].areas;
        for (int a = 0; a < areas.Count(); ++a)
        {
            CNavArea *area = areas[a];
            if (!area)
            {
                continue;
            }

            int reservationIndex = m_Reservations[team].Find(area->GetID());
            if (reservationIndex == m_Reservations[team].InvalidIndex())
            {
                continue;
            }

            AreaReservation_t &res = m_Reservations[team][reservationIndex];
            for (int i = res.claims.Count() - 1; i >= 0; --i)
            {
                if (res.claims[i].hOwner.Get() == bot)
                {
                    res.claims.Remove(i);
                }
            }
            if (res.claims.Count() == 0)
            {
                m_Reservations[team].RemoveAt(reservationIndex);
            }
        }
    }

    m_BotReservedAreas.RemoveAt(botIndex);
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Clear all current path reservations.
 */
void CNEOBotPathReservationSystem::Clear()
{
    ClearRound();
    m_AreaPenalties.RemoveAll();
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Clear round specific path reservations.
 */
void CNEOBotPathReservationSystem::ClearRound()
{
    for (int team = 0; team < TEAM__TOTAL; ++team)
    {
        m_Reservations[team].RemoveAll();
        m_HazardAreas[team].RemoveAll();
        m_DetpackMarks[team].RemoveAll();
        m_bBuddyGroupsBuilt[team] = false;
    }
    m_BotReservedAreas.RemoveAll();
    V_memset( m_iBuddyGroup, 0, sizeof( m_iBuddyGroup ) );
	m_pCachedGhostArea = nullptr;
	m_flNextDetpackScan = 0.0f;

    // a new round respawns everyone, so where bots got stuck starts over
    FOR_EACH_MAP_FAST( m_AreaPenalties, i )
    {
        m_AreaPenalties[i].flStuck = 0.0f;
    }
}

//-------------------------------------------------------------------------------------------------
int CNEOBotPathReservationSystem::GetPredictedFriendlyPathCount( int areaID, int teamID, const CNEOBot *excluding, int excludingGroup ) const
{
    if (!NEORules()->GetTeamPlayEnabled())
    {
        return 0;
    }

    if (!neo_bot_path_reservation_friendly_penalty_enable.GetBool()
        || teamID < 0 || teamID >= TEAM__TOTAL)
    {
        return 0;
    }

    int reservationIndex = m_Reservations[teamID].Find(areaID);
    if (reservationIndex == m_Reservations[teamID].InvalidIndex())
    {
        return 0;
    }

    return CountLiveClaims(m_Reservations[teamID][reservationIndex], excluding, excludingGroup);
}

//-------------------------------------------------------------------------------------------------
// Research: the group this bot plans its route with, formed for its team at the first ask of a round.
// A group is kept for the whole round: a member who dies is not replaced, so the others' routes do not jump.
int CNEOBotPathReservationSystem::GetBuddyGroup( const CNEOBot *bot )
{
    const int groupSize = NeoBotBuddyGroupSize( bot );
    const int team = bot->GetTeamNumber();
    if ( groupSize <= 1 || team < 0 || team >= TEAM__TOTAL )
    {
        return NO_BUDDY_GROUP;
    }

    if ( !m_bBuddyGroupsBuilt[team] )
    {
        BuildBuddyGroups( team, groupSize );
    }

    return m_iBuddyGroup[bot->entindex()];
}

//-------------------------------------------------------------------------------------------------
// Greedy grouping by where teammates stand when the round's first route is planned (their spawns),
// which any player can see. Each group starts at the lowest unassigned entindex and takes its nearest
// unassigned teammates; bots left over join the group of their nearest teammate.
void CNEOBotPathReservationSystem::BuildBuddyGroups( int teamID, int groupSize )
{
    m_bBuddyGroupsBuilt[teamID] = true;

    CUtlVector<CNEOBot *> mates;
    for ( int i = 1; i <= gpGlobals->maxClients; ++i )
    {
        CNEOBot *pMate = ToNEOBot( UTIL_PlayerByIndex( i ) );
        if ( !pMate || !pMate->IsAlive() || pMate->GetTeamNumber() != teamID )
        {
            continue;
        }

        m_iBuddyGroup[i] = NO_BUDDY_GROUP;
        mates.AddToTail( pMate );
    }

    // a team too small for one full group still forms one, so two bots in trio mode pair up
    const int nGroups = ( mates.Count() < 2 ) ? 0 : Max( 1, mates.Count() / groupSize );
    for ( int g = 0; g < nGroups; ++g )
    {
        CNEOBot *pSeed = nullptr;
        for ( CNEOBot *pMate : mates )
        {
            if ( m_iBuddyGroup[pMate->entindex()] == NO_BUDDY_GROUP )
            {
                pSeed = pMate;
                break;
            }
        }

        const int group = pSeed->entindex();
        m_iBuddyGroup[group] = group;
        for ( int n = 1; n < groupSize; ++n )
        {
            CNEOBot *pNearest = nullptr;
            float flNearestSq = FLT_MAX;
            for ( CNEOBot *pMate : mates )
            {
                const float flDistSq = pMate->GetAbsOrigin().DistToSqr( pSeed->GetAbsOrigin() );
                if ( m_iBuddyGroup[pMate->entindex()] == NO_BUDDY_GROUP && flDistSq < flNearestSq )
                {
                    pNearest = pMate;
                    flNearestSq = flDistSq;
                }
            }

            if ( !pNearest )
            {
                break;
            }

            m_iBuddyGroup[pNearest->entindex()] = group;
        }
    }

    // Leftovers are found after every group is formed, so they join groups and never each other
    CUtlVector<CNEOBot *> leftovers;
    for ( CNEOBot *pMate : mates )
    {
        if ( m_iBuddyGroup[pMate->entindex()] == NO_BUDDY_GROUP )
        {
            leftovers.AddToTail( pMate );
        }
    }

    for ( CNEOBot *pLeft : leftovers )
    {
        float flNearestSq = FLT_MAX;
        for ( CNEOBot *pMate : mates )
        {
            const int mateGroup = m_iBuddyGroup[pMate->entindex()];
            const float flDistSq = pMate->GetAbsOrigin().DistToSqr( pLeft->GetAbsOrigin() );
            if ( pMate != pLeft && mateGroup != NO_BUDDY_GROUP && leftovers.Find( pMate ) == -1 && flDistSq < flNearestSq )
            {
                m_iBuddyGroup[pLeft->entindex()] = mateGroup;
                flNearestSq = flDistSq;
            }
        }
    }

    if ( !neo_bot_research_buddy_log.GetBool() )
    {
        return;
    }

    for ( CNEOBot *pMate : mates )
    {
        const int group = m_iBuddyGroup[pMate->entindex()];
        char szBuddies[128] = "-";
        int len = 0;
        for ( CNEOBot *pOther : mates )
        {
            if ( pOther != pMate && group != NO_BUDDY_GROUP && m_iBuddyGroup[pOther->entindex()] == group )
            {
                len += V_snprintf( szBuddies + len, sizeof( szBuddies ) - len, len ? ",%d" : "%d", pOther->entindex() );
            }
        }

        Msg( "NEO_BUDDY t=%.2f team=%d bot=%d group=%d buddy=%s\n", gpGlobals->curtime, teamID, pMate->entindex(), group, szBuddies );
    }
}

//-------------------------------------------------------------------------------------------------
AreaPenalty_t *CNEOBotPathReservationSystem::FindOrAddAreaPenalty(unsigned int navAreaID)
{
    if ( !neo_bot_path_reservation_avoid_penalty_enable.GetBool() )
    {
        return nullptr;
    }

    unsigned short index = m_AreaPenalties.Find(navAreaID);
    if (index == m_AreaPenalties.InvalidIndex())
    {
        index = m_AreaPenalties.Insert(navAreaID, AreaPenalty_t());
    }
    return &m_AreaPenalties[index];
}

//-------------------------------------------------------------------------------------------------
void CNEOBotPathReservationSystem::IncrementAreaAvoidPenalty(unsigned int navAreaID, float penaltyAmount)
{
    if ( AreaPenalty_t *pPenalty = FindOrAddAreaPenalty(navAreaID) )
    {
        pPenalty->flStuck += penaltyAmount;
    }
}

//-------------------------------------------------------------------------------------------------
void CNEOBotPathReservationSystem::IncrementAreaDeathPenalty(unsigned int navAreaID, float penaltyAmount)
{
    if ( AreaPenalty_t *pPenalty = FindOrAddAreaPenalty(navAreaID) )
    {
        pPenalty->flDeath += penaltyAmount;
    }
}

//-------------------------------------------------------------------------------------------------
AreaPenalty_t CNEOBotPathReservationSystem::GetAreaPenalty(unsigned int navAreaID) const
{
    if ( !neo_bot_path_reservation_avoid_penalty_enable.GetBool() )
    {
        return AreaPenalty_t();
    }

    unsigned short index = m_AreaPenalties.Find(navAreaID);
    if (index != m_AreaPenalties.InvalidIndex())
    {
        return m_AreaPenalties[index];
    }
    return AreaPenalty_t();
}

// Cover and retreat searches skip an area once bots got stuck in it this many times in the round;
// an area with fewer stucks is only a cost to them, since one stuck can be a crowd or a pushed prop
static constexpr int NEO_BOT_STUCK_PRONE_AREA_COUNT = 2;

//-------------------------------------------------------------------------------------------------
bool AreaPenalty_t::IsStuckProne() const
{
    const float flStuckPronePenalty = NEO_BOT_STUCK_PRONE_AREA_COUNT * neo_bot_path_reservation_onstuck_penalty.GetFloat();
    return flStuck >= flStuckPronePenalty;
}

//-------------------------------------------------------------------------------------------------
// Functor to propagate deadly hazard to PVS-adjacent areas
struct CNEOFunctorPropagatePVSDeadlyHazard
{
	CNEOFunctorPropagatePVSDeadlyHazard(float expireTime, int teamID)
		: m_expireTime(expireTime), m_teamID(teamID)
	{
	}

	bool operator()(CNavArea *area)
	{
		CNEOBotPathReservations()->AddDeadlyHazard(area->GetID(), m_expireTime, m_teamID);
		return true;
	}

	float m_expireTime;
	int m_teamID;
};

//-------------------------------------------------------------------------------------------------
// Marks an area temporarily for bots to avoid or escape from
void CNEOBotPathReservationSystem::AddDeadlyHazard(int navAreaID, float expireTime, int teamID, bool propagatePVS)
{
    if ( !neo_bot_path_reservation_avoid_penalty_enable.GetBool() )
    {
        return;
    }

	if ( (teamID < 0) || (teamID >= TEAM__TOTAL) )
	{
		return;
	}

	int index = m_HazardAreas[teamID].Find(navAreaID);
	if ( !m_HazardAreas[teamID].IsValidIndex(index) )
	{
        // Initialize blank slate lookup entry
		HazardInfo blank;
		blank.hazardExpireTime = expireTime;
		blank.smokeExpireTime = 0.0f;
		blank.smokePropagateTime = 0.0f;
		blank.npcTurretExpireTime = 0.0f;
		blank.detpackExpireTime = 0.0f;
		index = m_HazardAreas[teamID].Insert(navAreaID, blank);
	}
    else
    {
	    HazardInfo &existing = m_HazardAreas[teamID][index];
        // Optimizing simplification: assume new time is later to skip comparisons
        // May also work for resetting an area to be non-hazardous early
	    existing.hazardExpireTime = expireTime;
    }

    if ( propagatePVS )
    {
        CNavArea *area = TheNavMesh->GetNavAreaByID(navAreaID);
        if (area)
        {
            CNEOFunctorPropagatePVSDeadlyHazard propagate(expireTime, teamID);
            // CompletelyVisible: fewer areas to iterate through and definitely exposed
            // vs PotentiallyVisible: for narrow corridors, some areas would count even if only a sliver was exposed
            area->ForAllCompletelyVisibleAreas(propagate);
        }
    }
}

//-------------------------------------------------------------------------------------------------
void CNEOBotPathReservationSystem::AddFragHazard(int navAreaID, float expireTime, int teamID)
{
    AddDeadlyHazard(navAreaID, expireTime, teamID, true);
    // true (propagatePVS) - propagate hazard to all adjacent PVS areas
    // shorthand for labeling entire trajectory and potential stray angles as hazardous
    // also intended for grenade thrower to duck behind cover from perspective of target
}

//-------------------------------------------------------------------------------------------------
// Functor to propagate a smoke hazard to PVS-adjacent areas
struct CNEOFunctorPropagatePVSSmokeHazard
{
	CNEOFunctorPropagatePVSSmokeHazard(float expireTime, int teamID)
		: m_expireTime(expireTime), m_teamID(teamID)
	{
	}

	bool operator()(CNavArea *area)
	{
		CNEOBotPathReservations()->AddSmokeHazard(area->GetID(), m_expireTime, m_teamID, false);
		return true;
	}

	float m_expireTime;
	int m_teamID;
};

//-------------------------------------------------------------------------------------------------
// Marks an area temporarily for bots to avoid or escape from
// Support bots ignore this smoke hazard
void CNEOBotPathReservationSystem::AddSmokeHazard(int navAreaID, float expireTime, int teamID, bool propagatePVS)
{
    if ( !neo_bot_path_reservation_avoid_penalty_enable.GetBool() )
    {
        return;
    }

	if (teamID < 0 || teamID >= TEAM__TOTAL)
	{
		return;
	}

	int index = m_HazardAreas[teamID].Find(navAreaID);
	if (!m_HazardAreas[teamID].IsValidIndex(index))
	{
        // Initialize blank slate lookup entry
		HazardInfo blank;
		blank.hazardExpireTime = 0.0f;
		blank.smokeExpireTime = expireTime;
		blank.smokePropagateTime = 0.0f;
		blank.npcTurretExpireTime = 0.0f;
		blank.detpackExpireTime = 0.0f;
		index = m_HazardAreas[teamID].Insert(navAreaID, blank);
	}
    else
    {
        HazardInfo &existing = m_HazardAreas[teamID][index];
        // Optimizing simplification: assume new time is later to skip comparisons
        // May also work for resetting an area to be non-hazardous early
        existing.smokeExpireTime = expireTime;

        if (propagatePVS && (existing.smokePropagateTime == expireTime))
        {
            return;
        }
    }

    if (propagatePVS)
    {
        CNavArea *area = TheNavMesh->GetNavAreaByID(navAreaID);
        if (area)
        {
            m_HazardAreas[teamID][index].smokePropagateTime = expireTime;

            CNEOFunctorPropagatePVSSmokeHazard propagate(expireTime, teamID);
            // CompletelyVisible: fewer areas to iterate through and definitely exposed
            // vs PotentiallyVisible: for narrow corridors, some areas would count even if only a sliver was exposed
            area->ForAllCompletelyVisibleAreas(propagate);
        }
    }
}

//-------------------------------------------------------------------------------------------------
// Functor to propagate a neo_npc_targetsystem turret hazard to PVS-adjacent areas
struct CNEOFunctorPropagatePVSNpcTurretHazard
{
	CNEOFunctorPropagatePVSNpcTurretHazard(float expireTime, int teamID)
		: m_expireTime(expireTime), m_teamID(teamID)
	{
	}

	bool operator()(CNavArea *area)
	{
		CNEOBotPathReservations()->AddNpcTurretHazard(area->GetID(), m_expireTime, m_teamID, false);
		return true;
	}

	float m_expireTime;
	int m_teamID;
};

//-------------------------------------------------------------------------------------------------
// Area tagged by neo_npc_targetsystem turret, whose motion vision
// cannot see slow cloaked players, so bots escaping it should cloak
void CNEOBotPathReservationSystem::AddNpcTurretHazard(int navAreaID, float expireTime, int teamID, bool propagatePVS)
{
    if ( !neo_bot_path_reservation_avoid_penalty_enable.GetBool() )
    {
        return;
    }

    if ( (teamID < 0) || (teamID >= TEAM__TOTAL) )
    {
        return;
    }

    // Kept apart from deadly hazards, so a bot in a hurry can accept the turret's risk
    int index = m_HazardAreas[teamID].Find(navAreaID);
    if ( !m_HazardAreas[teamID].IsValidIndex(index) )
    {
        HazardInfo blank;
        blank.hazardExpireTime = 0.0f;
        blank.smokeExpireTime = 0.0f;
        blank.smokePropagateTime = 0.0f;
        blank.npcTurretExpireTime = 0.0f;
		blank.detpackExpireTime = 0.0f;
        index = m_HazardAreas[teamID].Insert(navAreaID, blank);
    }

    // Inform the bots in every area the turret can see to turn on cloak
    m_HazardAreas[teamID][index].npcTurretExpireTime = expireTime;

    if ( propagatePVS )
    {
        CNavArea *area = TheNavMesh->GetNavAreaByID(navAreaID);
        if (area)
        {
            // Same visible set as a propagated deadly hazard
            CNEOFunctorPropagatePVSNpcTurretHazard propagate(expireTime, teamID);
            area->ForAllCompletelyVisibleAreas(propagate);
        }
    }
}

//-------------------------------------------------------------------------------------------------
bool CNEOBotPathReservationSystem::IsAreaNpcTurretHazard(int navAreaID, const CNEOBot *me) const
{
    if ( !neo_bot_path_reservation_avoid_penalty_enable.GetBool() || !me )
    {
        return false;
    }

    int teamID = me->GetTeamNumber();
    if ( (teamID < 0) || (teamID >= TEAM__TOTAL) )
    {
        return false;
    }

    int index = m_HazardAreas[teamID].Find(navAreaID);
    if ( !m_HazardAreas[teamID].IsValidIndex(index) )
    {
        return false;
    }

    return m_HazardAreas[teamID][index].npcTurretExpireTime > gpGlobals->curtime;
}

//-------------------------------------------------------------------------------------------------
float CNEOBotPathReservationSystem::GetAreaHazardousTime(int navAreaID, const CNEOBot *me) const
{
    if (!neo_bot_path_reservation_avoid_penalty_enable.GetBool())
    {
        return 0.0f;
    }

	if (!me)
	{
		return 0.0f;
	}

	int teamID = me->GetTeamNumber();

	if (teamID < 0 || teamID >= TEAM__TOTAL)
	{
		return 0.0f;
	}

	int index = m_HazardAreas[teamID].Find(navAreaID);
	if (m_HazardAreas[teamID].IsValidIndex(index))
	{
		const HazardInfo &h = m_HazardAreas[teamID][index];

		auto hazardTimeLeft = h.hazardExpireTime - gpGlobals->curtime;
		if (hazardTimeLeft > 0)
		{
	        // All bots avoid explosive hazards
			return hazardTimeLeft;
		}

		auto npcTurretTimeLeft = h.npcTurretExpireTime - gpGlobals->curtime;
		if (npcTurretTimeLeft > 0)
		{
			return npcTurretTimeLeft;
		}

        // Support class can see through smoke
        if (me->GetClass() != NEO_CLASS_SUPPORT)
        {
            auto smokeTimeLeft = h.smokeExpireTime - gpGlobals->curtime;
            if (smokeTimeLeft > 0)
            {
                return smokeTimeLeft;
            }
        }
	}

	return 0.0f;
}

//-------------------------------------------------------------------------------------------------
bool CNEOBotPathReservationSystem::IsAreaHazardous(int navAreaID, const CNEOBot *me) const
{
   return GetAreaHazardousTime(navAreaID, me) > 0;
}

//-------------------------------------------------------------------------------------------------
// Unlike IsAreaHazardous, smoke and the turret's sightline do not count: only marks that can kill, such as a frag grenade's blast area
bool CNEOBotPathReservationSystem::IsAreaDeadlyHazard(int navAreaID, const CNEOBot *me) const
{
	if (!neo_bot_path_reservation_avoid_penalty_enable.GetBool())
	{
		return false;
	}

	if (!me)
	{
		return false;
	}

	const int teamID = me->GetTeamNumber();
	if (teamID < 0 || teamID >= TEAM__TOTAL)
	{
		return false;
	}

	const int index = m_HazardAreas[teamID].Find(navAreaID);
	if (!m_HazardAreas[teamID].IsValidIndex(index))
	{
		return false;
	}

	return m_HazardAreas[teamID][index].hazardExpireTime > gpGlobals->curtime;
}

// How often the server looks for any deployed detpack, so bots skip the sight check when there is none
static constexpr float DETPACK_SCAN_INTERVAL = 0.5f;
// How long the areas in a seen detpack's blast radius stay blocked after the last sighting
static constexpr float DETPACK_HAZARD_DURATION = 15.0f;
// and how long a team waits before refreshing that mark, so many bots seeing one detpack mark it once
static constexpr float DETPACK_REMARK_INTERVAL = 5.0f;
// The area search reaches this far past the blast,
// since a large nav area's center can lie outside it while its floor is inside
static constexpr float DETPACK_AREA_SEARCH_MARGIN = 256.0f;

//-------------------------------------------------------------------------------------------------
// The detpacks lying in the world, refreshed at most every half second for all bots
const CUtlVector<DeployedDetpack_t> &CNEOBotPathReservationSystem::GetDeployedDetpacks()
{
	// a map change resets curtime, so a scan time far in the future is stale
	const bool bClockReset = m_flNextDetpackScan > gpGlobals->curtime + DETPACK_SCAN_INTERVAL;
	const bool bScanDue = ( gpGlobals->curtime >= m_flNextDetpackScan ) || bClockReset;
	if ( !bScanDue )
	{
		return m_DeployedDetpacks;
	}

	if ( bClockReset )
	{
		// The cached nav-area pointer belongs to the previous map's nav mesh.
		m_pCachedGhostArea = nullptr;
	}

	m_flNextDetpackScan = gpGlobals->curtime + DETPACK_SCAN_INTERVAL;
	m_DeployedDetpacks.RemoveAll();
	for ( CBaseEntity *pEnt = gEntList.FindEntityByClassname( nullptr, "neo_deployed_detpack" ); pEnt;
		pEnt = gEntList.FindEntityByClassname( pEnt, "neo_deployed_detpack" ) )
	{
		DeployedDetpack_t &det = m_DeployedDetpacks[m_DeployedDetpacks.AddToTail()];
		det.hDetpack = pEnt;
		det.pArea = TheNavMesh->GetNearestNavArea( pEnt->GetAbsOrigin() );
	}

	// refresh the cached ghost area while we are here, so per-bot checks avoid the nav lookup
	if ( NEORules()->GhostExists() && NEORules()->GetGhosterPlayer() <= 0 )
	{
		const Vector &ghostPos = NEORules()->GetGhostPos();
		m_pCachedGhostArea = TheNavMesh->GetNearestNavArea( ghostPos );
	}
	else
	{
		m_pCachedGhostArea = nullptr;
	}

	if ( m_DeployedDetpacks.Count() == 0 )
	{
		ClearDetpackMarks();
	}

	return m_DeployedDetpacks;
}

//-------------------------------------------------------------------------------------------------
// Whether a still-deployed detpack previously marked by this team threatens this nav area.
// The deployed list is shared and scanned at most every 0.5 seconds; mark expiry does not end a hunt.
bool CNEOBotPathReservationSystem::HasDeployedDetpackMarkInBlastOfGhostArea( int teamID )
{
	if ( ( teamID < 0 ) || ( teamID >= TEAM__TOTAL ) )
	{
		return false;
	}

	const CUtlVector<DeployedDetpack_t> &deployedDetpacks = GetDeployedDetpacks();
	const CNavArea *ghostArea = m_pCachedGhostArea;
	if ( !ghostArea )
	{
		return false;
	}

	const CUtlVector<DetpackMark_t> &marks = m_DetpackMarks[teamID];
	for ( int i = 0; i < marks.Count(); ++i )
	{
		CBaseEntity *pMarkedDetpack = marks[i].hDetpack.Get();
		if ( !pMarkedDetpack )
		{
			continue;
		}

		for ( int j = 0; j < deployedDetpacks.Count(); ++j )
		{
			CBaseEntity *pDeployedDetpack = deployedDetpacks[j].hDetpack.Get();
			if ( ( pDeployedDetpack == pMarkedDetpack )
				&& ( ghostArea->GetDistanceSquaredToPoint( pDeployedDetpack->GetAbsOrigin() )
					<= Square( NEO_DETPACK_DAMAGE_RADIUS ) ) )
			{
				return true;
			}
		}
	}

	return false;
}

//-------------------------------------------------------------------------------------------------
// Whether my team marked this detpack recently enough that seeing it again changes nothing
bool CNEOBotPathReservationSystem::IsDetpackMarkFresh( const CBaseEntity *pDetpack, int teamID ) const
{
	if ( ( teamID < 0 ) || ( teamID >= TEAM__TOTAL ) )
	{
		return false;
	}

	const CUtlVector<DetpackMark_t> &marks = m_DetpackMarks[teamID];
	for ( int i = 0; i < marks.Count(); ++i )
	{
		if ( ( marks[i].hDetpack.Get() == pDetpack ) && ( gpGlobals->curtime < marks[i].flMarkTime + DETPACK_REMARK_INTERVAL ) )
		{
			return true;
		}
	}

	return false;
}

//-------------------------------------------------------------------------------------------------
// Marks every nav area whose floor reaches into the blast, not only those whose center does
struct CNEOFunctorMarkDetpackHazard
{
	CNEOFunctorMarkDetpackHazard( const Vector &vecBlast, float expireTime, int teamID )
		: m_vecBlast( vecBlast ), m_expireTime( expireTime ), m_teamID( teamID )
	{
	}

	bool operator()( CNavArea *area )
	{
		if ( area->GetDistanceSquaredToPoint( m_vecBlast ) <= Square( NEO_DETPACK_DAMAGE_RADIUS ) )
		{
			CNEOBotPathReservations()->AddDetpackHazard( area->GetID(), m_expireTime, m_teamID );
		}
		return true;
	}

	Vector m_vecBlast;
	float m_expireTime;
	int m_teamID;
};

//-------------------------------------------------------------------------------------------------
// Marks the nav areas within a detpack's blast radius as deadly on every route for a team.
// Returns true when this is the first mark of that detpack, so bots with a path through it re-path.
bool CNEOBotPathReservationSystem::MarkDetpackBlast( CBaseEntity *pDetpack, int teamID )
{
	if ( !pDetpack || ( teamID < 0 ) || ( teamID >= TEAM__TOTAL ) )
	{
		return false;
	}

	if ( !neo_bot_path_reservation_avoid_penalty_enable.GetBool() )
	{
		return false;
	}

	CUtlVector<DetpackMark_t> &marks = m_DetpackMarks[teamID];
	DetpackMark_t *pMark = nullptr;
	for ( int i = marks.Count() - 1; i >= 0; --i )
	{
		if ( !marks[i].hDetpack.Get() )
		{
			marks.FastRemove( i );
			continue;
		}

		if ( marks[i].hDetpack.Get() == pDetpack )
		{
			pMark = &marks[i];
		}
	}

	if ( pMark && ( gpGlobals->curtime < pMark->flMarkTime + DETPACK_REMARK_INTERVAL ) )
	{
		return false;
	}

	const bool bFirstMark = ( pMark == nullptr );
	if ( bFirstMark )
	{
		pMark = &marks[marks.AddToTail()];
		pMark->hDetpack = pDetpack;
		pMark->flFirstMarkTime = gpGlobals->curtime;
	}
	else if ( gpGlobals->curtime >= pMark->flMarkTime + DETPACK_HAZARD_DURATION )
	{
		// the old mark lapsed, so a path built since then may run through the blast
		pMark->flFirstMarkTime = gpGlobals->curtime;
	}

	pMark->flMarkTime = gpGlobals->curtime;

	const Vector &vecBlast = pDetpack->GetAbsOrigin();
	CNEOFunctorMarkDetpackHazard mark( vecBlast, gpGlobals->curtime + DETPACK_HAZARD_DURATION, teamID );
	TheNavMesh->ForAllAreasInRadius( mark, vecBlast, NEO_DETPACK_DAMAGE_RADIUS + DETPACK_AREA_SEARCH_MARGIN );
	return bFirstMark;
}

//-------------------------------------------------------------------------------------------------
// A detpack mark is also a general deadly hazard, so default routes, cover searches and the hazard escape see it
void CNEOBotPathReservationSystem::AddDetpackHazard( int navAreaID, float expireTime, int teamID )
{
	AddDeadlyHazard( navAreaID, expireTime, teamID );

	const int index = m_HazardAreas[teamID].Find( navAreaID );
	if ( m_HazardAreas[teamID].IsValidIndex( index ) )
	{
		m_HazardAreas[teamID][index].detpackExpireTime = expireTime;
	}
}

//-------------------------------------------------------------------------------------------------
bool CNEOBotPathReservationSystem::IsAreaDetpackHazard( int navAreaID, int teamID ) const
{
	if ( ( teamID < 0 ) || ( teamID >= TEAM__TOTAL ) || ( m_DetpackMarks[teamID].Count() == 0 ) )
	{
		return false;
	}

	const int index = m_HazardAreas[teamID].Find( navAreaID );
	if ( !m_HazardAreas[teamID].IsValidIndex( index ) )
	{
		return false;
	}

	return m_HazardAreas[teamID][index].detpackExpireTime > gpGlobals->curtime;
}

//-------------------------------------------------------------------------------------------------
// Once every detpack has blown or been removed, its blast radius no longer blocks anything
void CNEOBotPathReservationSystem::ClearDetpackMarks()
{
	for ( int team = 0; team < TEAM__TOTAL; ++team )
	{
		if ( m_DetpackMarks[team].Count() == 0 )
		{
			continue;
		}

		m_DetpackMarks[team].RemoveAll();
		FOR_EACH_MAP_FAST( m_HazardAreas[team], i )
		{
			HazardInfo &h = m_HazardAreas[team][i];
			if ( h.detpackExpireTime > gpGlobals->curtime )
			{
				h.detpackExpireTime = 0.0f;
				h.hazardExpireTime = 0.0f;
			}
		}
	}
}

//-------------------------------------------------------------------------------------------------
bool CNEOBotPathReservationSystem::HasDetpackMark( int teamID ) const
{
	if ( ( teamID < 0 ) || ( teamID >= TEAM__TOTAL ) )
	{
		return false;
	}

	const CUtlVector<DetpackMark_t> &marks = m_DetpackMarks[teamID];
	for ( int i = 0; i < marks.Count(); ++i )
	{
		if ( marks[i].hDetpack.Get() && ( marks[i].flMarkTime + DETPACK_HAZARD_DURATION > gpGlobals->curtime ) )
		{
			return true;
		}
	}

	return false;
}

//-------------------------------------------------------------------------------------------------
// When the team last saw a detpack it had not marked yet, or -1 when it has no mark
float CNEOBotPathReservationSystem::GetLastDetpackMarkTime( int teamID ) const
{
	if ( ( teamID < 0 ) || ( teamID >= TEAM__TOTAL ) )
	{
		return -1.0f;
	}

	float flLast = -1.0f;
	const CUtlVector<DetpackMark_t> &marks = m_DetpackMarks[teamID];
	for ( int i = 0; i < marks.Count(); ++i )
	{
		if ( marks[i].hDetpack.Get() )
		{
			flLast = MAX( flLast, marks[i].flFirstMarkTime );
		}
	}

	return flLast;
}
