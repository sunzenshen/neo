#include "cbase.h"
#include "neo_player.h"
#include "neo_gamerules.h"
#include "neo_ghost_cap_point.h"
#include "bot/neo_bot.h"
#include "bot/behavior/neo_bot_knife_rush.h"
#include "bot/neo_bot_path_compute.h"
#include "neo_enums.h"
#include "weapon_knife.h"

ConVar sv_neo_bot_ctg_knife_rush_enabled( "sv_neo_bot_ctg_knife_rush_enabled", "1", FCVAR_CHEAT,
	"CTG: 1 (default) lets a bot that has already lost the position race for the ghost "
	"(CNEOBotKnifeRush::IsLosingThePositionRace) charge a close threat with a forced knife instead "
	"of trading shots, once both weapon slots are empty. See CNEOBotKnifeRush." );

ConVar sv_neo_bot_ctg_knife_rush_enter_range( "sv_neo_bot_ctg_knife_rush_enter_range", "150", FCVAR_CHEAT,
	"CTG no-retreat: a known threat within this many units is close enough to charge with a knife "
	"instead of trading shots. Wider than the knife's own swing range (NEO_WEP_KNIFE_RANGE = 51 u) "
	"since the rush is expected to close the last bit of ground itself.",
	true, 0.0f, false, 0.0f );

ConVar sv_neo_bot_ctg_knife_rush_exit_range( "sv_neo_bot_ctg_knife_rush_exit_range", "250", FCVAR_CHEAT,
	"CTG no-retreat: give up the knife rush once the threat is this far away. Doubled "
	"(sv_neo_bot_ctg_knife_rush_exit_range_unarmed_mult) when the threat's own active weapon is "
	"empty or already a melee weapon, since it can no longer punish us for closing the extra distance.",
	true, 0.0f, false, 0.0f );

ConVar sv_neo_bot_ctg_knife_rush_exit_range_unarmed_mult( "sv_neo_bot_ctg_knife_rush_exit_range_unarmed_mult", "1.75", FCVAR_CHEAT,
	"CTG no-retreat: multiplies sv_neo_bot_ctg_knife_rush_exit_range when the threat has no working ranged weapon left.",
	true, 1.0f, false, 0.0f );

ConVar sv_neo_bot_ctg_knife_rush_flank_distance( "sv_neo_bot_ctg_knife_rush_flank_distance", "40", FCVAR_CHEAT,
	"CTG no-retreat: the knife rush paths to this many units behind the target's current facing, not "
	"to the target itself, to set up NEOTOKYO's backstab bonus (any hit within ~37 degrees of the "
	"target's own back does 226 damage - a guaranteed kill regardless of class) instead of a front-on "
	"trade (25 damage/swing, 4-9 swings to kill). Kept under the knife's own range (51 u) so arriving "
	"at the flank point already puts us in swing distance.",
	true, 0.0f, false, 0.0f );

ConVar sv_neo_bot_ctg_knife_rush_repath_seconds( "sv_neo_bot_ctg_knife_rush_repath_seconds", "0.3", FCVAR_CHEAT,
	"CTG no-retreat: seconds between the knife rush re-aiming its flank point at the target's current "
	"back. Short, because both bots are moving fast relative to how close this fight already is.",
	true, 0.05f, false, 0.0f );

// NEO-HARNESS-TEMP (proposal 0014 - CQC knife-vs-retreat experiment). See neo_bot_knife_rush.h.
ConVar sv_neo_bot_cqc_experiment( "sv_neo_bot_cqc_experiment", "0", FCVAR_CHEAT,
	"CQC experiment: 0 off; 1 = TEAM_JINRAI bots knife-rush a close threat while TEAM_NSF bots fall "
	"back and shoot; 2 = swapped. Pair with sv_neo_bot_cqc_no_fire_range to force knife-range "
	"encounters. Off = shipped behaviour.",
	true, 0.0f, true, 2.0f );

//---------------------------------------------------------------------------------------------
ECqcArm CqcExperimentArmForBot( CNEOBot *me )
{
	const int iMode = sv_neo_bot_cqc_experiment.GetInt();
	if ( iMode <= 0 || !me )
	{
		return CQC_ARM_NONE;
	}

	const bool bJinraiKnifes = ( iMode == 1 );
	const bool bIsJinrai = ( me->GetTeamNumber() == TEAM_JINRAI );
	return ( bIsJinrai == bJinraiKnifes ) ? CQC_ARM_KNIFE : CQC_ARM_RETREAT;
}

//---------------------------------------------------------------------------------------------
// The threat's active weapon still being able to shoot us is what makes closing distance risky.
// No weapon, an empty one, or a melee weapon of their own all mean the same thing here: they
// cannot punish us for the last bit of ground, so we can afford to chase further before giving up.
static bool ThreatHasWorkingRangedWeapon( CNEO_Player *pThreat )
{
	if ( !pThreat )
	{
		return false;
	}

	CNEOBaseCombatWeapon *pWeapon = static_cast< CNEOBaseCombatWeapon * >( pThreat->GetActiveWeapon() );
	if ( !pWeapon || !CNEOBot::IsRanged( pWeapon ) )
	{
		return false;
	}

	return ( pWeapon->Clip1() + pWeapon->m_iPrimaryAmmoCount ) > 0;
}

//---------------------------------------------------------------------------------------------
bool CNEOBotKnifeRush::IsLosingThePositionRace( CNEOBot *me )
{
	if ( !NEORules()->GhostExists() )
	{
		return false;
	}

	const int iGhoster = NEORules()->GetGhosterPlayer();
	if ( iGhoster <= 0 || iGhoster > gpGlobals->maxClients )
	{
		return false;
	}

	CNEO_Player *pCarrier = ToNEOPlayer( UTIL_PlayerByIndex( iGhoster ) );
	if ( !pCarrier || !pCarrier->IsAlive() || pCarrier->GetTeamNumber() == me->GetTeamNumber() )
	{
		return false;
	}

	// Nearest active cap zone the carrier's own team can score into - either owned by that team, or
	// neutral (TEAM_ANY). Straight-line, deliberately: the naive read of the ghost marker every
	// player gets, not anything read out of the carrier's own AI or route.
	CNEOGhostCapturePoint *pBestCap = nullptr;
	float flBestCapDistSq = FLT_MAX;
	for ( int i = 0; i < NEORules()->m_pGhostCaps.Count(); ++i )
	{
		CNEOGhostCapturePoint *pCap = dynamic_cast< CNEOGhostCapturePoint * >(
			UTIL_EntityByIndex( NEORules()->m_pGhostCaps[i] ) );
		if ( !pCap || !pCap->GetActive() )
		{
			continue;
		}

		const int iCapTeam = pCap->owningTeamAlternate();
		if ( iCapTeam != pCarrier->GetTeamNumber() && iCapTeam != TEAM_ANY )
		{
			continue;
		}

		const float flDistSq = pCarrier->GetAbsOrigin().DistToSqr( pCap->GetAbsOrigin() );
		if ( flDistSq < flBestCapDistSq )
		{
			flBestCapDistSq = flDistSq;
			pBestCap = pCap;
		}
	}

	if ( !pBestCap )
	{
		return false;
	}

	const float flCarrierDistSq = pCarrier->GetAbsOrigin().DistToSqr( pBestCap->GetAbsOrigin() );
	const float flMyDistSq = me->GetAbsOrigin().DistToSqr( pBestCap->GetAbsOrigin() );
	return flCarrierDistSq < flMyDistSq;
}

//---------------------------------------------------------------------------------------------
bool CNEOBotKnifeRush::HasCloseKnifeableThreat( CNEOBot *me )
{
	// Support is a terrible knife fighter - slow to close, slow to reposition for the backstab this
	// whole approach is built around - so it sits this one out regardless of the relative-speed
	// check below.
	if ( me->GetClass() == NEO_CLASS_SUPPORT )
	{
		return false;
	}

	const CKnownEntity *threat = me->GetVisionInterface()->GetPrimaryKnownThreat( true );
	if ( !threat || !threat->GetEntity() || !threat->GetEntity()->IsAlive() )
	{
		return false;
	}

	const float flDistSq = me->GetAbsOrigin().DistToSqr( threat->GetEntity()->GetAbsOrigin() );
	if ( flDistSq > Square( sv_neo_bot_ctg_knife_rush_enter_range.GetFloat() ) )
	{
		return false;
	}

	if ( !me->Weapon_OwnsThisType( "weapon_knife" ) )
	{
		return false;
	}

	// Don't charge a faster class: if it decides to run instead of fight, we cannot close the gap
	// again and have thrown away our gun for the length of the chase.
	CNEO_Player *pThreatPlayer = ToNEOPlayer( threat->GetEntity() );
	if ( pThreatPlayer && pThreatPlayer->GetSprintSpeed() > me->GetSprintSpeed() )
	{
		return false;
	}

	return true;
}

//---------------------------------------------------------------------------------------------
bool CNEOBotKnifeRush::IsPossible( CNEOBot *me )
{
	// NEO-HARNESS-TEMP (proposal 0014): the CQC experiment's knife arm enters here directly, on
	// the same close-threat gate as the shipped rush but without the CTG-only / losing-the-race /
	// empty-clip preconditions - the no-fire gate already keeps these bots off their guns, so
	// forcing the knife is the arm. A bot the experiment assigned to the retreat arm never rushes.
	const ECqcArm arm = CqcExperimentArmForBot( me );
	if ( arm == CQC_ARM_KNIFE )
	{
		return HasCloseKnifeableThreat( me );
	}
	if ( arm == CQC_ARM_RETREAT )
	{
		return false;
	}

	if ( !sv_neo_bot_ctg_knife_rush_enabled.GetBool()
		|| NEORules()->GetGameType() != NEO_GAME_TYPE_CTG
		|| !IsLosingThePositionRace( me ) )
	{
		return false;
	}

	// A knife only beats a gun we can't fire right now. A loaded clip - in *either* slot, not just
	// whichever is out - always carries a chance of a headshot, which is a better bet than a
	// 25-damage knife swing: switching to a sidearm that still has rounds beats drawing a knife.
	// Reserve ammo does not help with that until a reload finishes, which this bot may not have
	// time for with a threat this close, so it is each weapon's clip specifically that has to be
	// empty, not its total ammo count.
	CNEOBaseCombatWeapon *pPrimary = static_cast< CNEOBaseCombatWeapon * >( me->Weapon_GetSlot( 0 ) );
	CNEOBaseCombatWeapon *pSecondary = static_cast< CNEOBaseCombatWeapon * >( me->Weapon_GetSlot( 1 ) );

	const bool bPrimaryHasAmmo = pPrimary && CNEOBot::IsRanged( pPrimary ) && pPrimary->Clip1() > 0;
	const bool bSecondaryHasAmmo = pSecondary && CNEOBot::IsRanged( pSecondary ) && pSecondary->Clip1() > 0;
	if ( bPrimaryHasAmmo || bSecondaryHasAmmo )
	{
		return false;
	}

	return HasCloseKnifeableThreat( me );
}

//---------------------------------------------------------------------------------------------
// Aim sv_neo_bot_ctg_knife_rush_flank_distance units behind the target's current facing, not at
// the target itself - see the class comment for why. Falls back to the target's own position if it
// is not a player we can read a facing from (should not happen for a live PvP threat, but a path
// to fight from is better than none).
bool CNEOBotKnifeRush::RepathToFlank( CNEOBot *me )
{
	CBaseEntity *pThreat = nullptr;
	if ( const CKnownEntity *threat = me->GetVisionInterface()->GetPrimaryKnownThreat( true ) )
	{
		pThreat = threat->GetEntity();
	}

	if ( !pThreat )
	{
		return false;
	}

	Vector vecGoal = pThreat->GetAbsOrigin();

	if ( CNEO_Player *pThreatPlayer = ToNEOPlayer( pThreat ) )
	{
		QAngle facing( 0.0f, pThreatPlayer->EyeAngles().y, 0.0f );
		Vector vecForward;
		AngleVectors( facing, &vecForward );
		vecGoal -= vecForward * sv_neo_bot_ctg_knife_rush_flank_distance.GetFloat();
	}

	return CNEOBotPathCompute( me, m_path, vecGoal, FASTEST_ROUTE );
}

//---------------------------------------------------------------------------------------------
ActionResult< CNEOBot > CNEOBotKnifeRush::OnStart( CNEOBot *me, Action< CNEOBot > *priorAction )
{
	m_hKnife = static_cast< CNEOBaseCombatWeapon * >( me->Weapon_OwnsThisType( "weapon_knife" ) );
	if ( !m_hKnife )
	{
		// IsPossible already checked this, but the weapon could in principle be gone by OnStart -
		// nothing to fight with, so don't force a null onto the required-weapon stack.
		return Done( "No knife to rush with" );
	}

	me->PushRequiredWeapon( m_hKnife );
	m_bPushedRequiredWeapon = true;

	m_path.SetMinLookAheadDistance( me->GetDesiredPathLookAheadRange() );
	RepathToFlank( me );
	m_repathTimer.Start( sv_neo_bot_ctg_knife_rush_repath_seconds.GetFloat() );

	return Continue();
}

//---------------------------------------------------------------------------------------------
ActionResult< CNEOBot > CNEOBotKnifeRush::Update( CNEOBot *me, float interval )
{
	const CKnownEntity *threat = me->GetVisionInterface()->GetPrimaryKnownThreat( true );
	if ( !threat || !threat->GetEntity() || !threat->GetEntity()->IsAlive() )
	{
		return Done( "Threat is gone" );
	}

	CNEO_Player *pThreatPlayer = ToNEOPlayer( threat->GetEntity() );

	const float flExitRange = sv_neo_bot_ctg_knife_rush_exit_range.GetFloat()
		* ( ThreatHasWorkingRangedWeapon( pThreatPlayer ) ? 1.0f : sv_neo_bot_ctg_knife_rush_exit_range_unarmed_mult.GetFloat() );

	const float flDistSq = me->GetAbsOrigin().DistToSqr( threat->GetEntity()->GetAbsOrigin() );
	if ( flDistSq > Square( flExitRange ) )
	{
		return Done( "Threat broke off - resuming normal combat" );
	}

	if ( m_repathTimer.IsElapsed() )
	{
		m_repathTimer.Start( sv_neo_bot_ctg_knife_rush_repath_seconds.GetFloat() );
		RepathToFlank( me );
	}

	// Movement only - CNEOBotMainAction::Update runs every tick regardless of what covers
	// TacticalMonitor, and its EquipRequiredWeapon() / FireWeaponAtEnemy() /
	// UpdateLookingAroundForEnemies() already aim at and swing the forced knife once we are close
	// enough, on whatever bearing we ended up approaching from. Nothing here needs to press fire.
	m_path.Update( me );

	return Continue();
}

//---------------------------------------------------------------------------------------------
void CNEOBotKnifeRush::OnEnd( CNEOBot *me, Action< CNEOBot > *nextAction )
{
	if ( m_bPushedRequiredWeapon )
	{
		me->PopRequiredWeapon();
	}
}

//---------------------------------------------------------------------------------------------
EventDesiredResult< CNEOBot > CNEOBotKnifeRush::OnStuck( CNEOBot *me )
{
	if ( !RepathToFlank( me ) )
	{
		return TryDone( RESULT_TRY, "Stuck with no way to the target's flank" );
	}

	return TryContinue();
}

//---------------------------------------------------------------------------------------------
EventDesiredResult< CNEOBot > CNEOBotKnifeRush::OnMoveToFailure( CNEOBot *me, const Path *path, MoveToFailureType reason )
{
	if ( !RepathToFlank( me ) )
	{
		return TryDone( RESULT_TRY, "Cannot reach the target's flank" );
	}

	return TryContinue();
}
