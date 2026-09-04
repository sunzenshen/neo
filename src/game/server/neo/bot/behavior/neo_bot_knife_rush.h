#pragma once

#include "bot/neo_bot.h"
#include "Path/NextBotChasePath.h"

class CNEOBaseCombatWeapon;

//--------------------------------------------------------------------------------------------------------
// Charges a known threat that is already close with a forced knife. Meant to run alongside the CTG
// "no retreat" clause (sv_neo_bot_ctg_no_retreat_when_carrier_ahead, CNEOBotCtgEnemy::IsLosingTheRace):
// once the position race against an enemy ghost carrier is already lost, backing off to reload
// against a threat this close wins nothing, and a guaranteed melee trade is a better bet than
// hoping to win a ranged exchange from here. CNEOBotTacticalMonitor gates entry through
// IsPossible() before ever constructing one - the same pattern as CNEOBotGetAmmo::IsPossible /
// CNEOBotGetHealth::IsPossible.
//
// Firing and aiming are deliberately not this behaviour's job. CNEOBotMainAction::Update runs
// every tick regardless of what is suspended beneath it, and its EquipRequiredWeapon() +
// FireWeaponAtEnemy() + UpdateLookingAroundForEnemies() already look at, aim at and swing whatever
// weapon is forced onto the required-weapon stack. This behaviour only owns closing the distance
// and deciding when to give up on the plan - the enemy breaking off past knife range, especially
// while still holding a weapon with bullets left, is the one thing that makes charging a mistake.
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

	virtual const char *GetName( void ) const override { return "knifeRush"; }

private:
	CHandle< CNEOBaseCombatWeapon > m_hKnife;	// pushed onto the required-weapon stack for the duration
	bool m_bPushedRequiredWeapon = false;		// guards OnEnd's pop against an OnStart that bailed early
	ChasePath m_chasePath;
};
