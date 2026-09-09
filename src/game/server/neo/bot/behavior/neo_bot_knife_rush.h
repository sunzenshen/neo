#pragma once

#include "bot/neo_bot.h"
#include "nav_pathfind.h"

class CNEOBaseCombatWeapon;

extern ConVar sv_neo_bot_knife_rush_enter_range;	// "close enough to break off into the rush" distance

//--------------------------------------------------------------------------------------------------------
// Charges a known threat with a forced knife, aiming for the backstab arc.
//
// This is only the *approach* behaviour. The decision to enter it lives in CNEOBotAttack
// (CNEOBotAttack::ShouldKnifeRush): by default a bot rushes only when it is completely out of
// bullets - both primary and secondary weapon absent or with clip and reserve at zero - so the
// knife is genuinely all it has left. sv_neo_bot_force_knife_fight forces it on sight for
// debugging / reviewer evaluation.
//
// The approach deliberately aims for the target's back, not their current position: NEOTOKYO's
// knife (weapon_knife.cpp) is a flat 25 damage per swing at ~1.8 swings/second - 4-9 hits to kill
// depending on class - but any hit within about 37 degrees of the target's own facing does 226
// (a guaranteed kill regardless of class). A front-on approach is a multi-second trade a still-armed
// enemy usually wins; arriving at their back turns it into one hit. The aim point is recomputed on
// a short timer as the target moves and turns, which - since it always aims for wherever "behind
// them" currently is - naturally routes around a target that turns to face the incoming bot,
// without any explicit circle-strafe logic.
//
// Firing and aiming are deliberately not this behaviour's job. CNEOBotMainAction::Update runs
// every tick regardless of what is suspended beneath it, and its EquipRequiredWeapon() +
// FireWeaponAtEnemy() + UpdateLookingAroundForEnemies() already look at, aim at and swing whatever
// weapon is forced onto the required-weapon stack, on whatever bearing the approach ends up on -
// arriving at their side or front still swings, just without the backstab bonus. This behaviour
// only owns closing the distance and deciding when to give up on the plan.
class CNEOBotKnifeRush : public Action< CNEOBot >
{
public:
	virtual ActionResult< CNEOBot >	OnStart( CNEOBot *me, Action< CNEOBot > *priorAction ) override;
	virtual ActionResult< CNEOBot >	Update( CNEOBot *me, float interval ) override;
	virtual void						OnEnd( CNEOBot *me, Action< CNEOBot > *nextAction ) override;

	virtual EventDesiredResult< CNEOBot > OnStuck( CNEOBot *me ) override;
	virtual EventDesiredResult< CNEOBot > OnMoveToFailure( CNEOBot *me, const Path *path, MoveToFailureType reason ) override;

	virtual const char *GetName( void ) const override { return "knifeRush"; }

private:
	bool RepathToFlank( CNEOBot *me );

	CHandle< CNEOBaseCombatWeapon > m_hKnife;	// pushed onto the required-weapon stack for the duration
	bool m_bPushedRequiredWeapon = false;		// guards OnEnd's pop against an OnStart that bailed early
	PathFollower m_path;
	CountdownTimer m_repathTimer;				// throttles re-aiming the flank point at the target's back
};
