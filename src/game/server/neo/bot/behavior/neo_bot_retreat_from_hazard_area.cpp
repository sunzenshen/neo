#include "cbase.h"
#include "neo_bot_retreat_from_hazard_area.h"
#include "neo_bot_retreat_from_grenade.h"
#include "neo_bot_retreat_to_cover.h"
#include "neo/bot/neo_bot_path_compute.h"
#include "neo/bot/neo_bot_path_reservation.h"
#include "weapon_neobasecombatweapon.h"

extern ConVar sv_neo_smoke_blocker_size;

ConVar sv_neo_bot_smoke_return_fire("sv_neo_bot_smoke_return_fire", "1", FCVAR_NONE,
    "Bots hit while retreating from a hazard area return fire at an attacker they cannot see.", true, 0, true, 1);

const int MAX_NON_HAZARD_AREA_CANDIDATES = 5;

// How long after being hit a bot keeps answering an attacker it cannot see
const float RETURN_FIRE_DURATION = 3.0f;
// Unsuppressed muzzle bloom stays visible through smoke this long after a shot
const float MUZZLE_FLASH_VISIBLE_TIME = 0.5f;
const float RETURN_FIRE_TORSO_HEIGHT = 36.0f;
const float RETURN_FIRE_BURST_TIME = 0.3f;
// How often a bot emptying its clip at smoke picks a new spot in the cloud
const float RETURN_FIRE_SWEEP_INTERVAL = 0.5f;
const float RETURN_FIRE_AIM_TOLERANCE = 0.998f; // cos of ~3.6 degrees

class CSearchForSafeArea : public ISearchSurroundingAreasFunctor
{
public:
    CSearchForSafeArea(CNEOBot *me)
        : m_me(me)
        , m_onStuckPenalty(neo_bot_path_reservation_onstuck_penalty.GetFloat())
    {
    }

    virtual bool operator()(CNavArea *baseArea, CNavArea *priorArea, float travelDistanceSoFar)
    {
        int id = baseArea->GetID();
        float candidateHazardTime = CNEOBotPathReservations()->GetAreaHazardousTime(id, m_me);
        if (candidateHazardTime > 0)
        {
            if ( m_me->IsDebugging( NEXTBOT_PATH ) )
            {
                baseArea->DrawFilled(255, 0, 0, MIN(255, candidateHazardTime), candidateHazardTime);
            }
        }
        else if (CNEOBotPathReservations()->GetAreaAvoidPenalty(id) < m_onStuckPenalty)
        {
            m_safeAreas.AddToTail(baseArea);
        }

        if (m_safeAreas.Count() >= MAX_NON_HAZARD_AREA_CANDIDATES)
        {
            return false; // found enough candidates
        }

        return true;
    }

    virtual bool ShouldSearch(CNavArea *adjArea, CNavArea *currentArea, float travelDistanceSoFar)
    {
        return (currentArea->ComputeAdjacentConnectionHeightChange(adjArea) <
                m_me->GetLocomotionInterface()->GetMaxJumpHeight());
    }

    CUtlVector<CNavArea *> m_safeAreas;

private:
    CNEOBot *m_me;
    float m_onStuckPenalty;
};

CNEOBotRetreatFromHazardArea::CNEOBotRetreatFromHazardArea()
{
}

CNavArea *CNEOBotRetreatFromHazardArea::FindSafeArea(CNEOBot *me)
{
    CNavArea *start = me->GetLastKnownArea();
    if (!start)
    {
        return nullptr;
    }

    CSearchForSafeArea search(me);
    SearchSurroundingAreas(start, search);

    if (search.m_safeAreas.Count() == 0)
    {
        return nullptr;
    }

    int last = MIN(MAX_NON_HAZARD_AREA_CANDIDATES, search.m_safeAreas.Count());
    int which = RandomInt(0, last - 1);
    return search.m_safeAreas[which];
}

ActionResult<CNEOBot> CNEOBotRetreatFromHazardArea::OnStart(CNEOBot *me, Action<CNEOBot> *priorAction)
{
    m_safeArea = FindSafeArea(me);

    if (!m_safeArea)
    {
        return Done("No safe area available!");
    }

    m_path.SetMinLookAheadDistance(me->GetDesiredPathLookAheadRange());
    m_path.Invalidate();

    m_hAttacker = nullptr;
    m_returnFireTimer.Invalidate();
    m_sweepTimer.Invalidate();
    m_bEmptyClipAtSmoke = false;

    return Continue();
}

ActionResult<CNEOBot> CNEOBotRetreatFromHazardArea::Update(CNEOBot *me, float interval)
{
    CBaseEntity *dangerousGrenade = CNEOBotRetreatFromGrenade::FindDangerousGrenade(me);
    if (dangerousGrenade)
    {
        return ChangeTo(new CNEOBotRetreatFromGrenade(dangerousGrenade), "Encountered grenade while avoiding hazard!");
    }

    CNavArea *myArea = me->GetLastKnownArea();
    if (!myArea)
    {
        return Done("Can't identify my nav area!");
    }

    if ( !CNEOBotPathReservations()->IsAreaHazardous(myArea->GetID(), me))
    {
        return Done("Left hazardous area");
    }

    if ( !m_safeArea || (myArea->GetID() == m_safeArea->GetID()) || (CNEOBotPathReservations()->IsAreaHazardous(m_safeArea->GetID(), me)) )
    {
        // Safe area is no longer safe at this point, look for new candidate
        m_safeArea = FindSafeArea(me);
        m_path.Invalidate();
    }

    if (!m_safeArea)
    {
        return Done("Could not find replacement safe area!");
    }

    if (!m_path.IsValid())
    {
        // RETREAT_ROUTE: Allow pathing through hazardous areas in CNEOBotPathCost
        CNEOBotPathCompute(me, m_path, m_safeArea->GetCenter(), RETREAT_ROUTE);
    }

    m_path.Update(me);
    UpdateReturnFire(me);
    return Continue();
}

// The muzzle bloom of an unsuppressed weapon shows through smoke for a moment after each shot
bool CNEOBotRetreatFromHazardArea::CanSeeMuzzleFlash(CNEOBot *me, CBaseEntity *attacker) const
{
    CBaseCombatCharacter *shooter = attacker->MyCombatCharacterPointer();
    auto *weapon = shooter ? static_cast<CNEOBaseCombatWeapon *>(shooter->GetActiveWeapon()) : nullptr;
    if (!weapon)
    {
        return false;
    }

    const auto wepBits = weapon->GetNeoWepBits();
    if (!(wepBits & NEO_WEP_FIREARM) || (wepBits & NEO_WEP_SUPPRESSED))
    {
        return false;
    }

    if (gpGlobals->curtime - weapon->GetLastAttackTime() > MUZZLE_FLASH_VISIBLE_TIME)
    {
        return false;
    }

    if (!me->GetVisionInterface()->IsInFieldOfView(attacker))
    {
        return false;
    }

    // This sight check uses MASK_OPAQUE, which smoke does not block
    return me->IsLineOfSightClear(attacker, CBaseCombatCharacter::IGNORE_ACTORS);
}

//   from ----------> to      a point up to maxError to either side of 'to',
//                 <--+-->    as seen from 'from'
static Vector SidewaysOf(const Vector &from, const Vector &to, float maxError)
{
    Vector sideways(from.y - to.y, to.x - from.x, 0.0f);
    sideways.NormalizeInPlace();
    return to + sideways * RandomFloat(-maxError, maxError);
}

// Return my firearm if it can shoot right now, otherwise start fixing that and return nullptr
CNEOBaseCombatWeapon *CNEOBotRetreatFromHazardArea::ReadyFirearm(CNEOBot *me, CBaseEntity *attacker)
{
    auto *myWeapon = static_cast<CNEOBaseCombatWeapon *>(me->GetActiveWeapon());
    if (!myWeapon || !(myWeapon->GetNeoWepBits() & NEO_WEP_FIREARM))
    {
        const CKnownEntity *knownAttacker = me->GetVisionInterface()->GetKnown(attacker);
        if (knownAttacker)
        {
            me->EquipBestWeaponForThreat(knownAttacker);
        }
        return nullptr;
    }

    if (myWeapon->m_bInReload)
    {
        return nullptr;
    }

    if (myWeapon->Clip1() <= 0)
    {
        m_bEmptyClipAtSmoke = false;
        me->ReloadIfLowClip(true);
        return nullptr;
    }

    return myWeapon;
}

// Shoot back at where I believe my unseen attacker is, while continuing to retreat
void CNEOBotRetreatFromHazardArea::UpdateReturnFire(CNEOBot *me)
{
    if (m_returnFireTimer.IsElapsed() && !m_bEmptyClipAtSmoke)
    {
        return;
    }

    CBaseEntity *attacker = m_hAttacker.Get();
    if (!attacker || !attacker->IsAlive())
    {
        m_returnFireTimer.Invalidate();
        m_bEmptyClipAtSmoke = false;
        return;
    }

    if (me->HasAttribute(CNEOBot::SUPPRESS_FIRE) || me->HasAttribute(CNEOBot::IGNORE_ENEMIES))
    {
        return;
    }

    const CKnownEntity *threat = me->GetVisionInterface()->GetPrimaryKnownThreat();
    if (threat && threat->IsVisibleRecently())
    {
        return; // CNEOBotMainAction::FireWeaponAtEnemy owns aiming and firing at threats I can see
    }

    const bool bSawFlash = CanSeeMuzzleFlash(me, attacker);
    if (bSawFlash)
    {
        me->GetVisionInterface()->UpdateKnownEntityPosition(attacker);
        m_vecAttackerBelievedPos = attacker->GetAbsOrigin();
    }

    CNEOBaseCombatWeapon *myWeapon = ReadyFirearm(me, attacker);
    if (!myWeapon)
    {
        return;
    }

    const Vector torso(0, 0, RETURN_FIRE_TORSO_HEIGHT);
    if (bSawFlash || !m_returnFireTimer.IsElapsed())
    {
        m_vecAimSpot = m_vecAttackerBelievedPos + torso;
    }
    else if (m_sweepTimer.IsElapsed())
    {
        // No fresh hint: sweep across the cloud, reaching its full width as the clip runs out
        m_sweepTimer.Start(RETURN_FIRE_SWEEP_INTERVAL);
        const float clipUsed = 1.0f - ((float)myWeapon->Clip1() / MAX(1, myWeapon->GetMaxClip1()));
        const float sweepWidth = clipUsed * sv_neo_smoke_blocker_size.GetFloat();
        m_vecAimSpot = SidewaysOf(me->GetAbsOrigin(), m_vecAttackerBelievedPos, sweepWidth) + torso;
    }

    // Players know the map: only shoot where geometry lets a bullet through, smoke aside
    const Vector myEyes = me->EyePosition();
    if (!me->IsLineOfFireClear(m_vecAimSpot, CNEOBot::LINE_OF_FIRE_FLAGS_DEFAULT) || !me->IsLineOfFireClearOfFriendlies(myEyes, m_vecAimSpot))
    {
        return;
    }

    me->GetBodyInterface()->AimHeadTowards(m_vecAimSpot, IBody::CRITICAL, 0.5f, nullptr, "Returning fire at unseen attacker");

    Vector forward;
    me->EyeVectors(&forward);
    Vector toAimSpot = m_vecAimSpot - myEyes;
    toAimSpot.NormalizeInPlace();
    if (DotProduct(forward, toAimSpot) < RETURN_FIRE_AIM_TOLERANCE)
    {
        return;
    }

    if (me->IsContinuousFireWeapon(myWeapon))
    {
        me->PressFireButton(RETURN_FIRE_BURST_TIME);
    }
    else if (me->m_nButtons & IN_ATTACK)
    {
        me->ReleaseFireButton(); // semi-auto needs the trigger released between shots
    }
    else
    {
        me->PressFireButton();
    }
}

EventDesiredResult< CNEOBot > CNEOBotRetreatFromHazardArea::OnInjured( CNEOBot *me, const CTakeDamageInfo &info )
{
    CBaseEntity *attacker = info.GetAttacker();
    if (!sv_neo_bot_smoke_return_fire.GetBool() || !attacker || !attacker->IsPlayer() || !me->IsEnemy(attacker))
    {
        return TryContinue();
    }

    if (!(info.GetDamageType() & (DMG_BULLET | DMG_BUCKSHOT)))
    {
        return TryContinue(); // grenade damage says nothing about where the thrower is now
    }

    m_hAttacker = attacker;
    m_returnFireTimer.Start(RETURN_FIRE_DURATION);
    m_vecAttackerBelievedPos = attacker->GetAbsOrigin();

    // Only a bot that smoke is hiding the enemy from keeps shooting until its clip is empty
    const CNavArea *myArea = me->GetLastKnownArea();
    m_bEmptyClipAtSmoke = myArea && CNEOBotPathReservations()->IsAreaSmokeHazard(myArea->GetID(), me);

    if (!CanSeeMuzzleFlash(me, attacker))
    {
        // Being hit only gives a rough direction: the attacker is somewhere in that part of the cloud
        m_vecAttackerBelievedPos = SidewaysOf(me->GetAbsOrigin(), m_vecAttackerBelievedPos, sv_neo_smoke_blocker_size.GetFloat());
    }

    return TryContinue();
}

EventDesiredResult< CNEOBot > CNEOBotRetreatFromHazardArea::OnStuck( CNEOBot *me )
{
    m_safeArea = FindSafeArea(me);
    m_path.Invalidate();
	return TryContinue();
}

EventDesiredResult< CNEOBot > CNEOBotRetreatFromHazardArea::OnMoveToFailure( CNEOBot *me, const Path *path, MoveToFailureType reason )
{
    m_safeArea = FindSafeArea(me);
    m_path.Invalidate();
	return TryContinue();
}