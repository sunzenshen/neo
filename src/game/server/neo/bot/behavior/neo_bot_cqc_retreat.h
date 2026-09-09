#pragma once

#include "bot/neo_bot.h"
#include "nav_pathfind.h"

class CNEOBaseCombatWeapon;

//--------------------------------------------------------------------------------------------------------
// NEO-HARNESS-TEMP (proposal 0014 - CQC knife-vs-retreat experiment).
//
// The "retreat with a gun" arm of the close-quarters A/B. Mirror image of CNEOBotKnifeRush: where
// the knife arm charges a close threat with a forced melee, this one breaks contact - paths
// backward along the reverse bearing from the threat until it is beyond gun-favourable range - and
// lets CNEOBotMainAction keep shooting the bot's normal weapon while it repositions. It does NOT
// push a required weapon, so the bot fights with whatever it already had.
//
// Only ever entered for a bot the experiment has assigned CQC_ARM_RETREAT
// (CqcExperimentArmForBot, in neo_bot_knife_rush.h). With sv_neo_bot_cqc_experiment 0 this action
// is never constructed and the build is behaviour-identical to the branch tip.
class CNEOBotCqcRetreat : public Action< CNEOBot >
{
public:
	// True when TacticalMonitor should suspend into a CQC retreat right now: this bot is the
	// retreat arm and there is a known, living threat within the shared close-quarters trigger
	// range (sv_neo_bot_ctg_knife_rush_enter_range - the same distance the knife arm charges at,
	// so both arms react to "a threat is close" at the same threshold).
	static bool IsPossible( CNEOBot *me );

	virtual ActionResult< CNEOBot >	OnStart( CNEOBot *me, Action< CNEOBot > *priorAction ) override;
	virtual ActionResult< CNEOBot >	Update( CNEOBot *me, float interval ) override;

	virtual EventDesiredResult< CNEOBot > OnStuck( CNEOBot *me ) override;
	virtual EventDesiredResult< CNEOBot > OnMoveToFailure( CNEOBot *me, const Path *path, MoveToFailureType reason ) override;

	virtual const char *GetName( void ) const override { return "cqcRetreat"; }

private:
	// Paths to sv_neo_bot_cqc_retreat_distance units back along the flat reverse bearing from the
	// current primary threat. Recomputed on a short timer as the threat moves.
	bool RepathAway( CNEOBot *me );

	PathFollower m_path;
	CountdownTimer m_repathTimer;
	CountdownTimer m_giveUpTimer;	// hard cap so a bot never backpedals forever
};
