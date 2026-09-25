#pragma once

#include "cbase.h"
#include "utlmap.h"
#include "utlvector.h"
#include "nav_area.h"
#include "neo_player_shared.h"

class CNEOBot;

struct AreaClaim_t
{
    EHANDLE hOwner;
    float flExpirationTime; // gpGlobals->curtime past which this claim no longer counts
};

struct AreaReservation_t
{
    CUtlVector<AreaClaim_t> claims;

    AreaReservation_t() {}
    AreaReservation_t(const AreaReservation_t& src) { claims = src.claims; }
    AreaReservation_t& operator=(const AreaReservation_t& src) { claims = src.claims; return *this; }
};

struct HazardInfo
{
	float smokeExpireTime;      // when the smoke hazard risk expires
	float hazardExpireTime;     // when a general deadly hazard risk expires
	float smokePropagateTime;   // expiry this area has already propagated to its visible set
	float npcTurretExpireTime;  // when a neo_npc_targetsystem hazard expires
};

struct BotReservedAreas_t
{
    CUtlVector<CNavArea*> areas;

    BotReservedAreas_t() {}
    BotReservedAreas_t(const BotReservedAreas_t& src) { areas = src.areas; }
    BotReservedAreas_t& operator=(const BotReservedAreas_t& src) { areas = src.areas; return *this; }
};

//----------------------------------------------------------------------------------------------------
/**
 * This singleton allows bots to temporarily "claim" navigation areas along their intended path,
 * discouraging other bots from using the same route simultaneously.
 */
class CNEOBotPathReservationSystem
{
public:
    static bool AreaIDLessFunc(const int &lhs, const int &rhs)
    {
        return lhs < rhs;
    }

    CNEOBotPathReservationSystem()
        : m_BotReservedAreas(DefLessFunc(int))
        , m_AreaAvoidPenalties(DefLessFunc(unsigned int))
        , m_CrossingAvoidPenalties(DefLessFunc(uint64))
        , m_PropObstacleFraction(DefLessFunc(unsigned int))
    {
        for (int i = 0; i < TEAM__TOTAL; ++i)
        {
            m_Reservations[i].SetLessFunc(AreaIDLessFunc);
            m_HazardAreas[i].SetLessFunc(AreaIDLessFunc);
            m_DoorCrossingExpiry[i].SetLessFunc(DefLessFunc(uint64));
        }
    }

    void ReserveArea(CNavArea *area, CNEOBot *bot, float duration);
    void ReleaseAllAreas(CNEOBot *bot);
    void Clear();
    void ClearRound();

    int GetPredictedFriendlyPathCount( int areaID, int teamID, const CNEOBot *excluding = NULL ) const;

    void IncrementAreaAvoidPenalty(unsigned int navAreaID, float penaltyAmount);
    float GetAreaAvoidPenalty(unsigned int navAreaID) const;

    // NEO-HARNESS-TEMP research arm (2026-09-23): penalise the crossing a stuck bot failed, not only its area
    void IncrementCrossingAvoidPenalty(unsigned int fromAreaID, unsigned int toAreaID, float penaltyAmount);
    void RelieveCrossingAvoidPenalty(unsigned int fromAreaID, unsigned int toAreaID);
    float GetCrossingAvoidPenalty(unsigned int fromAreaID, unsigned int toAreaID) const;

    // NEO-HARNESS-TEMP research arm (2026-09-23, patch 57): a crossing a bot got stuck on against a closed,
    // motionless door is closed to its team for a while (sentinel_jgr's team spawn doors: the map hands door
    // ownership over at run time and its func_nav_blockers do not follow)
    void BlockDoorCrossing(unsigned int fromAreaID, unsigned int toAreaID, int teamID);
    void RelieveDoorCrossing(unsigned int fromAreaID, unsigned int toAreaID, int teamID);
    float GetDoorCrossingPenalty(unsigned int fromAreaID, unsigned int toAreaID, int teamID) const;

    // NEO-HARNESS-TEMP research arm (2026-09-23, patch 50): share of each nav area covered by solid
    // physics props, refreshed every second, for a soft path-cost penalty
    void UpdatePropObstacles();
    float GetPropObstacleFraction(unsigned int navAreaID) const;

    void AddDeadlyHazard(int navAreaID, float expireTime, int teamID, bool propagatePVS = false);
    void AddFragHazard(int navAreaID, float expireTime, int teamID);
    void AddSmokeHazard(int navAreaID, float expireTime, int teamID, bool propagatePVS = true);
    void AddNpcTurretHazard(int navAreaID, float expireTime, int teamID, bool propagatePVS = true);
    bool IsAreaNpcTurretHazard(int navAreaID, const CNEOBot *me) const;
    float GetAreaHazardousTime(int navAreaID, const CNEOBot *me) const;
    bool IsAreaHazardous(int navAreaID, const CNEOBot *me) const;

private:
    int CountLiveClaims(const AreaReservation_t &res, const CNEOBot *excluding) const;

    CUtlMap<int, AreaReservation_t> m_Reservations[TEAM__TOTAL];   // keyed by nav area ID
    CUtlMap<int, BotReservedAreas_t> m_BotReservedAreas;           // keyed by bot entindex
    CUtlMap<unsigned int, float> m_AreaAvoidPenalties;

    struct CrossingAvoidInfo_t
    {
        float penalty;
        float lastFailTime;
    };
    CUtlMap<uint64, CrossingAvoidInfo_t> m_CrossingAvoidPenalties;
    CUtlMap<uint64, float> m_DoorCrossingExpiry[TEAM__TOTAL];
    CUtlMap<unsigned int, float> m_PropObstacleFraction;
    float m_flNextPropObstacleUpdate = 0.0f;
    CUtlMap<int, HazardInfo> m_HazardAreas[TEAM__TOTAL];
};


CNEOBotPathReservationSystem* CNEOBotPathReservations();

extern ConVar neo_bot_path_reservation_enable;
extern ConVar neo_bot_path_reservation_penalty;
extern ConVar neo_bot_path_reservation_duration;
extern ConVar neo_bot_path_reservation_distance;
extern ConVar neo_bot_path_reservation_onstuck_penalty;
extern ConVar neo_bot_path_reservation_killed_penalty;
