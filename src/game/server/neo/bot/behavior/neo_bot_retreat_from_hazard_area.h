#pragma once

#include "NextBotBehavior.h"
#include "nav_mesh.h"
#include "neo/bot/neo_bot.h"
#include "neo/bot/neo_bot_path_compute.h"

// which hazards a retreat leaves: a bot in a hurry for its objective leaves only a frag grenade's
enum HazardRetreatKind
{
    RETREAT_FROM_ANY_HAZARD,
    RETREAT_FROM_FRAG_HAZARD,
};

class CNEOBotRetreatFromHazardArea : public Action<CNEOBot>
{
public:
    CNEOBotRetreatFromHazardArea(HazardRetreatKind kind = RETREAT_FROM_ANY_HAZARD);

    static bool IsHazardToLeave(const CNEOBot *me, const CNavArea *area, HazardRetreatKind kind);

    virtual ActionResult<CNEOBot> OnStart(CNEOBot *me, Action<CNEOBot> *priorAction) OVERRIDE;
    virtual ActionResult<CNEOBot> Update(CNEOBot *me, float interval) OVERRIDE;

    virtual EventDesiredResult<CNEOBot> OnStuck(CNEOBot *me) OVERRIDE;
    virtual EventDesiredResult<CNEOBot> OnMoveToFailure(CNEOBot *me, const Path *path, MoveToFailureType reason) OVERRIDE;

    virtual const char *GetName() const OVERRIDE { return "RetreatFromHazardArea"; }

private:
    HazardRetreatKind m_kind;
    CNavArea *m_safeArea;
    PathFollower m_path;

    CNavArea *FindSafeArea(CNEOBot *me);
};
