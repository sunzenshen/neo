#include "cbase.h"
#include "neo_player.h"
#include "bot/neo_bot.h"
#include "bot/behavior/neo_bot_knife_rush.h"
#include "bot/neo_bot_path_compute.h"

ConVar sv_neo_bot_knife_rush_enter_range( "sv_neo_bot_knife_rush_enter_range", "150", FCVAR_CHEAT,
	"A known threat within this many units is close enough to break off into the knife rush "
	"(CNEOBotAttack::ShouldKnifeRush). Wider than the knife's own swing range (NEO_WEP_KNIFE_RANGE "
	"= 51 u) since the rush closes the last of the ground itself; kept below "
	"sv_neo_bot_knife_rush_exit_range so the enter/exit band gives the rush hysteresis and does not "
	"thrash against Attack.",
	true, 0.0f, false, 0.0f );

ConVar sv_neo_bot_knife_rush_exit_range( "sv_neo_bot_knife_rush_exit_range", "250", FCVAR_CHEAT,
	"Give up the knife rush once the threat is this far away. Multiplied by "
	"sv_neo_bot_knife_rush_exit_range_unarmed_mult when the threat has no working ranged weapon "
	"left, since it can no longer punish us for closing the extra distance.",
	true, 0.0f, false, 0.0f );

ConVar sv_neo_bot_knife_rush_exit_range_unarmed_mult( "sv_neo_bot_knife_rush_exit_range_unarmed_mult", "1.75", FCVAR_CHEAT,
	"Multiplies sv_neo_bot_knife_rush_exit_range when the threat has no working ranged weapon left.",
	true, 1.0f, false, 0.0f );

ConVar sv_neo_bot_knife_rush_flank_distance( "sv_neo_bot_knife_rush_flank_distance", "40", FCVAR_CHEAT,
	"The knife rush paths to this many units behind the target's current facing, not to the target "
	"itself, to set up NEOTOKYO's backstab bonus (any hit within ~37 degrees of the target's own "
	"back does 226 damage - a guaranteed kill regardless of class) instead of a front-on trade "
	"(25 damage/swing, 4-9 swings to kill). Kept under the knife's own range (51 u) so arriving at "
	"the flank point already puts us in swing distance.",
	true, 0.0f, false, 0.0f );

ConVar sv_neo_bot_knife_rush_repath_seconds( "sv_neo_bot_knife_rush_repath_seconds", "0.3", FCVAR_CHEAT,
	"Seconds between the knife rush re-aiming its flank point at the target's current back. Short, "
	"because both bots are moving fast relative to how close this fight already is.",
	true, 0.05f, false, 0.0f );

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
// Aim sv_neo_bot_knife_rush_flank_distance units behind the target's current facing, not at the
// target itself - see the class comment for why. Falls back to the target's own position if it is
// not a player we can read a facing from (should not happen for a live PvP threat, but a path to
// fight from is better than none).
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
		vecGoal -= vecForward * sv_neo_bot_knife_rush_flank_distance.GetFloat();
	}

	return CNEOBotPathCompute( me, m_path, vecGoal, FASTEST_ROUTE );
}

//---------------------------------------------------------------------------------------------
ActionResult< CNEOBot > CNEOBotKnifeRush::OnStart( CNEOBot *me, Action< CNEOBot > *priorAction )
{
	m_hKnife = static_cast< CNEOBaseCombatWeapon * >( me->Weapon_OwnsThisType( "weapon_knife" ) );
	if ( !m_hKnife )
	{
		// The caller checked this, but the weapon could in principle be gone by OnStart - nothing
		// to fight with, so don't force a null onto the required-weapon stack.
		return Done( "No knife to rush with" );
	}

	me->PushRequiredWeapon( m_hKnife );
	m_bPushedRequiredWeapon = true;

	m_path.SetMinLookAheadDistance( me->GetDesiredPathLookAheadRange() );
	RepathToFlank( me );
	m_repathTimer.Start( sv_neo_bot_knife_rush_repath_seconds.GetFloat() );

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

	const float flExitRange = sv_neo_bot_knife_rush_exit_range.GetFloat()
		* ( ThreatHasWorkingRangedWeapon( pThreatPlayer ) ? 1.0f : sv_neo_bot_knife_rush_exit_range_unarmed_mult.GetFloat() );

	const float flDistSq = me->GetAbsOrigin().DistToSqr( threat->GetEntity()->GetAbsOrigin() );
	if ( flDistSq > Square( flExitRange ) )
	{
		return Done( "Threat broke off - resuming normal combat" );
	}

	if ( m_repathTimer.IsElapsed() )
	{
		m_repathTimer.Start( sv_neo_bot_knife_rush_repath_seconds.GetFloat() );
		RepathToFlank( me );
	}

	// Movement only - CNEOBotMainAction::Update runs every tick regardless of what is suspended
	// beneath it, and its EquipRequiredWeapon() / FireWeaponAtEnemy() / UpdateLookingAroundForEnemies()
	// already aim at and swing the forced knife once we are close enough, on whatever bearing we
	// ended up approaching from. Nothing here needs to press fire.
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
