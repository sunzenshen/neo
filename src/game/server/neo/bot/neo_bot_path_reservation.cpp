#include "cbase.h"
#include "neo/bot/neo_bot_path_reservation.h"
#include "neo/bot/neo_bot.h"
#include "nav_mesh.h"
#include "neo_gamerules.h"

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

// NEO-HARNESS-TEMP research arm (2026-09-23): crossing memory; 0 = off (upstream behaviour)
ConVar neo_bot_path_crossing_stuck_penalty("neo_bot_path_crossing_stuck_penalty", "0", FCVAR_CHEAT,
    "Research: path cost added to a nav crossing each time a bot gets stuck making it (0 = off)", true, 0, false, 0);
ConVar neo_bot_path_crossing_stuck_memory("neo_bot_path_crossing_stuck_memory", "60", FCVAR_CHEAT,
    "Research: seconds a failed crossing stays penalised after its last failure", true, 0, false, 0);

// NEO-HARNESS-TEMP research arm (2026-09-23, patch 50): soft path cost for areas under physics props
ConVar neo_bot_path_prop_obstacle_cost("neo_bot_path_prop_obstacle_cost", "0", FCVAR_CHEAT,
    "Research: an area's path cost is multiplied by 1 + this x the share of it covered by solid physics props (0 = off)", true, 0, false, 0);
ConVar neo_bot_path_prop_obstacle_min_size("neo_bot_path_prop_obstacle_min_size", "16", FCVAR_CHEAT,
    "Research: physics props smaller than this in both horizontal extents are ignored", true, 0, false, 0);
ConVar neo_bot_path_prop_obstacle_all_entities("neo_bot_path_prop_obstacle_all_entities", "0", FCVAR_CHEAT,
    "Research: prop_dynamic and func_physbox count as obstacles too, not only prop_physics");
ConVar neo_bot_path_prop_obstacle_log("neo_bot_path_prop_obstacle_log", "0", FCVAR_CHEAT,
    "Research: log each covered area (NEO_FORENSIC_PROPOBST) when the coverage is refreshed");

ConVar neo_bot_path_reservation_onstuck_penalty("neo_bot_path_reservation_onstuck_penalty", "1000", FCVAR_NONE,
    "Path selection penalty added to a nav area each time a bot gets stuck moving through that area.", true, 0, false, 0);



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
int CNEOBotPathReservationSystem::CountLiveClaims(const AreaReservation_t &res, const CNEOBot *excluding) const
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
    m_AreaAvoidPenalties.RemoveAll();
    m_CrossingAvoidPenalties.RemoveAll();
    for (int team = 0; team < TEAM__TOTAL; ++team)
    {
        m_DoorCrossingExpiry[team].RemoveAll();
    }
    m_PropObstacleFraction.RemoveAll();
    m_flNextPropObstacleUpdate = 0.0f;
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
    }
    m_BotReservedAreas.RemoveAll();
}

//-------------------------------------------------------------------------------------------------
int CNEOBotPathReservationSystem::GetPredictedFriendlyPathCount( int areaID, int teamID, const CNEOBot *excluding ) const
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

    return CountLiveClaims(m_Reservations[teamID][reservationIndex], excluding);
}

//-------------------------------------------------------------------------------------------------
void CNEOBotPathReservationSystem::IncrementAreaAvoidPenalty(unsigned int navAreaID, float penaltyAmount)
{
    if ( !neo_bot_path_reservation_avoid_penalty_enable.GetBool() )
    {
        return;
    }

    unsigned short index = m_AreaAvoidPenalties.Find(navAreaID);
    if (index == m_AreaAvoidPenalties.InvalidIndex())
    {
        index = m_AreaAvoidPenalties.Insert(navAreaID, 0.0f);
    }

    m_AreaAvoidPenalties[index] += penaltyAmount;
}

//-------------------------------------------------------------------------------------------------
float CNEOBotPathReservationSystem::GetAreaAvoidPenalty(unsigned int navAreaID) const
{
    if ( !neo_bot_path_reservation_avoid_penalty_enable.GetBool() )
    {
        return 0.0f;
    }

    unsigned short index = m_AreaAvoidPenalties.Find(navAreaID);
    if (index != m_AreaAvoidPenalties.InvalidIndex())
    {
        return m_AreaAvoidPenalties[index];
    }
    return 0.0f;
}

//-------------------------------------------------------------------------------------------------
static uint64 CrossingKey( unsigned int fromAreaID, unsigned int toAreaID )
{
    return ( (uint64)fromAreaID << 32 ) | toAreaID;
}

//-------------------------------------------------------------------------------------------------
void CNEOBotPathReservationSystem::IncrementCrossingAvoidPenalty(unsigned int fromAreaID, unsigned int toAreaID, float penaltyAmount)
{
    const uint64 key = CrossingKey( fromAreaID, toAreaID );
    unsigned short index = m_CrossingAvoidPenalties.Find( key );
    if ( index == m_CrossingAvoidPenalties.InvalidIndex() )
    {
        CrossingAvoidInfo_t info = { 0.0f, 0.0f };
        index = m_CrossingAvoidPenalties.Insert( key, info );
    }
    else if ( gpGlobals->curtime - m_CrossingAvoidPenalties[index].lastFailTime > neo_bot_path_crossing_stuck_memory.GetFloat() )
    {
        m_CrossingAvoidPenalties[index].penalty = 0.0f; // expired: start over
    }

    m_CrossingAvoidPenalties[index].penalty += penaltyAmount;
    m_CrossingAvoidPenalties[index].lastFailTime = gpGlobals->curtime;
}

//-------------------------------------------------------------------------------------------------
ConVar neo_bot_stuck_door_penalty("neo_bot_stuck_door_penalty", "0", FCVAR_CHEAT,
    "Research: path cost added to a crossing where a bot got stuck against a closed, motionless door, for its team (0 = off)");
ConVar neo_bot_stuck_door_memory("neo_bot_stuck_door_memory", "30", FCVAR_CHEAT,
    "Research: seconds a stuck-door crossing stays penalised for the team (refreshed by each new stuck)");

void CNEOBotPathReservationSystem::BlockDoorCrossing(unsigned int fromAreaID, unsigned int toAreaID, int teamID)
{
    if ( teamID < 0 || teamID >= TEAM__TOTAL )
        return;
    const uint64 key = CrossingKey( fromAreaID, toAreaID );
    const float expiry = gpGlobals->curtime + neo_bot_stuck_door_memory.GetFloat();
    unsigned short index = m_DoorCrossingExpiry[teamID].Find( key );
    if ( index == m_DoorCrossingExpiry[teamID].InvalidIndex() )
        m_DoorCrossingExpiry[teamID].Insert( key, expiry );
    else
        m_DoorCrossingExpiry[teamID][index] = expiry;
}

void CNEOBotPathReservationSystem::RelieveDoorCrossing(unsigned int fromAreaID, unsigned int toAreaID, int teamID)
{
    if ( teamID < 0 || teamID >= TEAM__TOTAL || m_DoorCrossingExpiry[teamID].Count() == 0 )
        return;
    m_DoorCrossingExpiry[teamID].Remove( CrossingKey( fromAreaID, toAreaID ) );
}

float CNEOBotPathReservationSystem::GetDoorCrossingPenalty(unsigned int fromAreaID, unsigned int toAreaID, int teamID) const
{
    if ( teamID < 0 || teamID >= TEAM__TOTAL || m_DoorCrossingExpiry[teamID].Count() == 0 )
        return 0.0f;
    unsigned short index = m_DoorCrossingExpiry[teamID].Find( CrossingKey( fromAreaID, toAreaID ) );
    if ( index == m_DoorCrossingExpiry[teamID].InvalidIndex() || gpGlobals->curtime > m_DoorCrossingExpiry[teamID][index] )
        return 0.0f;
    return neo_bot_stuck_door_penalty.GetFloat();
}

//-------------------------------------------------------------------------------------------------
void CNEOBotPathReservationSystem::RelieveCrossingAvoidPenalty(unsigned int fromAreaID, unsigned int toAreaID)
{
    if ( m_CrossingAvoidPenalties.Count() == 0 )
    {
        return;
    }

    m_CrossingAvoidPenalties.Remove( CrossingKey( fromAreaID, toAreaID ) );
}

//-------------------------------------------------------------------------------------------------
float CNEOBotPathReservationSystem::GetCrossingAvoidPenalty(unsigned int fromAreaID, unsigned int toAreaID) const
{
    if ( m_CrossingAvoidPenalties.Count() == 0 )
    {
        return 0.0f;
    }

    unsigned short index = m_CrossingAvoidPenalties.Find( CrossingKey( fromAreaID, toAreaID ) );
    if ( index == m_CrossingAvoidPenalties.InvalidIndex() )
    {
        return 0.0f;
    }

    const CrossingAvoidInfo_t &info = m_CrossingAvoidPenalties[index];
    if ( gpGlobals->curtime - info.lastFailTime > neo_bot_path_crossing_stuck_memory.GetFloat() )
    {
        return 0.0f;
    }
    return info.penalty;
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
// Area tagged by neo_npc_targetsystem turret, whose motion vision
// cannot see slow cloaked players, so bots escaping it should cloak
void CNEOBotPathReservationSystem::AddNpcTurretHazard(int navAreaID, float expireTime, int teamID, bool propagatePVS)
{
    AddDeadlyHazard(navAreaID, expireTime, teamID, propagatePVS);

    if ( (teamID < 0) || (teamID >= TEAM__TOTAL) )
    {
        return;
    }

    int index = m_HazardAreas[teamID].Find(navAreaID);
    if ( m_HazardAreas[teamID].IsValidIndex(index) )
    {
        // Inform the bots in the specific target area to turn on cloak
        m_HazardAreas[teamID][index].npcTurretExpireTime = expireTime;
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
// NEO-HARNESS-TEMP research arm (2026-09-23, patch 50). Physics props are not in the nav mesh and move,
// so their footprint is re-projected onto the mesh every second: each solid prop_physics* whose box
// reaches into a player's body space (step height to standing height above an area's floor) marks the
// share of the area it covers, its box grown by a hull half-width (a player's centre cannot come
// closer) and lowered to the floor, so props hanging over a passage claim the floor under them too.
void CNEOBotPathReservationSystem::UpdatePropObstacles()
{
    if ( neo_bot_path_prop_obstacle_cost.GetFloat() <= 0.0f )
    {
        if ( m_PropObstacleFraction.Count() )
        {
            m_PropObstacleFraction.RemoveAll();
        }
        return;
    }

    if ( gpGlobals->curtime < m_flNextPropObstacleUpdate && gpGlobals->curtime >= m_flNextPropObstacleUpdate - 2.0f )
    {
        return;
    }
    m_flNextPropObstacleUpdate = gpGlobals->curtime + 1.0f;
    m_PropObstacleFraction.RemoveAll();

    const float flHalfHull = 16.0f;
    const float flStep = StepHeight;
    const float flBody = HumanHeight;
    const float flMinSize = neo_bot_path_prop_obstacle_min_size.GetFloat();
    const bool bLog = neo_bot_path_prop_obstacle_log.GetBool();
    const bool bAllEnts = neo_bot_path_prop_obstacle_all_entities.GetBool();

    CUtlVector< CNavArea * > areas;
    for ( CBaseEntity *pEnt = gEntList.FirstEnt(); pEnt; pEnt = gEntList.NextEnt( pEnt ) )
    {
        // physics props, or with _all_entities also the other movable / map-toggled solid props the mesh
        // was not built round (prop_dynamic: e.g. rogue's competition-mode container, APC and fences)
        const bool bPhys = FClassnameIs( pEnt, "prop_physics*" );
        if ( !bPhys && !( bAllEnts && ( FClassnameIs( pEnt, "prop_dynamic*" ) || FClassnameIs( pEnt, "func_physbox*" ) ) ) )
        {
            continue;
        }
        if ( pEnt->IsSolidFlagSet( FSOLID_NOT_SOLID ) || !pEnt->IsSolid() )
        {
            continue;
        }
        const int iGroup = pEnt->GetCollisionGroup();
        if ( iGroup == COLLISION_GROUP_DEBRIS || iGroup == COLLISION_GROUP_DEBRIS_TRIGGER || iGroup == COLLISION_GROUP_INTERACTIVE_DEBRIS )
        {
            continue;
        }

        Vector vecMins, vecMaxs;
        pEnt->CollisionProp()->WorldSpaceAABB( &vecMins, &vecMaxs );
        if ( ( vecMaxs.x - vecMins.x ) < flMinSize && ( vecMaxs.y - vecMins.y ) < flMinSize )
        {
            continue;
        }
        if ( ( vecMaxs.x - vecMins.x ) > 512.0f || ( vecMaxs.y - vecMins.y ) > 512.0f )
        {
            continue;	// structural: the mesh was built round it
        }
        if ( vecMaxs.z - vecMins.z < flStep )
        {
            continue;
        }

        Extent ext;
        ext.lo = Vector( vecMins.x - flHalfHull, vecMins.y - flHalfHull, vecMins.z - flBody );
        ext.hi = Vector( vecMaxs.x + flHalfHull, vecMaxs.y + flHalfHull, vecMaxs.z );
        areas.RemoveAll();
        TheNavMesh->CollectAreasOverlappingExtent( ext, &areas );

        for ( int i = 0; i < areas.Count(); ++i )
        {
            CNavArea *pArea = areas[i];
            const Vector &lo = pArea->GetCorner( NORTH_WEST );
            const Vector &hi = pArea->GetCorner( SOUTH_EAST );
            const float flFloor = pArea->GetZ( clamp( ( vecMins.x + vecMaxs.x ) * 0.5f, lo.x, hi.x ), clamp( ( vecMins.y + vecMaxs.y ) * 0.5f, lo.y, hi.y ) );
            // the prop has to occupy the body space over this floor: above a step, below head height
            if ( vecMins.z > flFloor + flBody || vecMaxs.z < flFloor + flStep )
            {
                continue;
            }
            const float flOverX = MIN( ext.hi.x, hi.x ) - MAX( ext.lo.x, lo.x );
            const float flOverY = MIN( ext.hi.y, hi.y ) - MAX( ext.lo.y, lo.y );
            const float flAreaSize = MAX( ( hi.x - lo.x ) * ( hi.y - lo.y ), 1.0f );
            if ( flOverX <= 0.0f || flOverY <= 0.0f )
            {
                continue;
            }
            const float flFrac = ( flOverX * flOverY ) / flAreaSize;
            unsigned short idx = m_PropObstacleFraction.Find( pArea->GetID() );
            if ( idx == m_PropObstacleFraction.InvalidIndex() )
            {
                idx = m_PropObstacleFraction.Insert( pArea->GetID(), 0.0f );
            }
            m_PropObstacleFraction[idx] = MIN( 1.0f, m_PropObstacleFraction[idx] + flFrac );
        }
    }

    if ( bLog )
    {
        FOR_EACH_MAP_FAST( m_PropObstacleFraction, i )
        {
            Msg( "NEO_FORENSIC_PROPOBST t=%.2f area=%u frac=%.2f\n", gpGlobals->curtime, m_PropObstacleFraction.Key( i ), m_PropObstacleFraction[i] );
        }
    }
}

//-------------------------------------------------------------------------------------------------
float CNEOBotPathReservationSystem::GetPropObstacleFraction( unsigned int navAreaID ) const
{
    if ( m_PropObstacleFraction.Count() == 0 )
    {
        return 0.0f;
    }
    const unsigned short idx = m_PropObstacleFraction.Find( navAreaID );
    return ( idx == m_PropObstacleFraction.InvalidIndex() ) ? 0.0f : m_PropObstacleFraction[idx];
}
