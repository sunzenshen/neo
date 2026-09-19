#pragma once

#include "NextBotBehavior.h"
#include "nav_mesh.h"
#include "neo/bot/neo_bot.h"
#include "neo/bot/neo_bot_path_compute.h"

class CNEOBotRetreatFromHazardArea : public Action<CNEOBot>
{
public:
    CNEOBotRetreatFromHazardArea();

    virtual ActionResult<CNEOBot> OnStart(CNEOBot *me, Action<CNEOBot> *priorAction) OVERRIDE;
    virtual ActionResult<CNEOBot> Update(CNEOBot *me, float interval) OVERRIDE;

    virtual void OnEnd(CNEOBot *me, Action<CNEOBot> *nextAction) OVERRIDE;
    virtual EventDesiredResult<CNEOBot> OnStuck(CNEOBot *me) OVERRIDE;
    virtual EventDesiredResult<CNEOBot> OnMoveToFailure(CNEOBot *me, const Path *path, MoveToFailureType reason) OVERRIDE;
    virtual EventDesiredResult<CNEOBot> OnInjured(CNEOBot *me, const CTakeDamageInfo &info) OVERRIDE;

    virtual const char *GetName() const OVERRIDE { return "RetreatFromHazardArea"; }

private:
    CNavArea *m_safeArea;
    PathFollower m_path;

    CNavArea *FindSafeArea(CNEOBot *me);

    // Returning fire at an attacker hidden by smoke
    EHANDLE m_hAttacker;
    Vector m_vecAttackerBelievedPos;
    CountdownTimer m_returnFireTimer;
    CountdownTimer m_sweepTimer;
    Vector m_vecAimSpot;
    bool m_bEmptyClipAtSmoke;

    bool CanSeeMuzzleFlash(CNEOBot *me, CBaseEntity *attacker) const;
    class CNEOBaseCombatWeapon *ReadyFirearm(CNEOBot *me, CBaseEntity *attacker);
    void StartReturnFire(CNEOBot *me, CBaseEntity *attacker, const Vector &believedPos, float duration);
    void ContinueInterruptedFight(CNEOBot *me);
    void RecallReturnFire(CNEOBot *me);
    void UpdateReturnFire(CNEOBot *me);
};
