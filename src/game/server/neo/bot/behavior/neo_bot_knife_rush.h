#pragma once

#include "bot/neo_bot.h"
#include "nav_pathfind.h"

class CNEOBaseCombatWeapon;

//--------------------------------------------------------------------------------------------------------
// Charges a known threat that is already close with a forced knife. Entered once the position race
// against an enemy ghost carrier is already lost (IsLosingThePositionRace, below): backing off to
// reload against a threat this close wins nothing, and a guaranteed melee trade is a better bet than
// hoping to win a ranged exchange from here. CNEOBotTacticalMonitor gates entry through
// IsPossible() before ever constructing one - the same pattern as CNEOBotGetAmmo::IsPossible /
// CNEOBotGetHealth::IsPossible.
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
// only owns closing the distance and deciding when to give up on the plan - the enemy breaking off
// past knife range, especially while still holding a weapon with bullets left, is the one thing
// that makes charging a mistake.
class CNEOBotKnifeRush : public Action< CNEOBot >
{
public:
	// True when TacticalMonitor should suspend into a knife rush right now: there is a known,
	// living threat already within sv_neo_bot_ctg_knife_rush_enter_range, we have a knife to fight
	// with, and the threat's class is not faster than ours - a faster enemy that decides to run
	// cannot be run down, so closing on it with a knife only trades our gun for nothing.
	static bool IsPossible( CNEOBot *me );

	virtual ActionResult< CNEOBot >	OnStart( CNEOBot *me, Action< CNEOBot > *priorAction ) override;
	virtual ActionResult< CNEOBot >	Update( CNEOBot *me, float interval ) override;
	virtual void						OnEnd( CNEOBot *me, Action< CNEOBot > *nextAction ) override;

	virtual EventDesiredResult< CNEOBot > OnStuck( CNEOBot *me ) override;
	virtual EventDesiredResult< CNEOBot > OnMoveToFailure( CNEOBot *me, const Path *path, MoveToFailureType reason ) override;

	virtual const char *GetName( void ) const override { return "knifeRush"; }

private:
	// True when an enemy is carrying the ghost and is already nearer (straight-line) to the
	// nearest active cap zone its own team can score into than `me` is - the position race for
	// that cap is lost. Reads NEORules()->m_pGhostCaps directly (friend class CNEOBotKnifeRush in
	// neo_gamerules.h), the same fixed map geometry every other CTG behaviour that reasons about
	// "which zone" reads; it does not read anything from the carrier's own AI.
	static bool IsLosingThePositionRace( CNEOBot *me );

	bool RepathToFlank( CNEOBot *me );

	CHandle< CNEOBaseCombatWeapon > m_hKnife;	// pushed onto the required-weapon stack for the duration
	bool m_bPushedRequiredWeapon = false;		// guards OnEnd's pop against an OnStart that bailed early
	PathFollower m_path;
	CountdownTimer m_repathTimer;				// throttles re-aiming the flank point at the target's back
};
