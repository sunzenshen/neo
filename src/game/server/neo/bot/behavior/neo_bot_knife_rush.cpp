#include "cbase.h"
#include "neo_player.h"
#include "neo_gamerules.h"
#include "bot/neo_bot.h"
#include "bot/behavior/neo_bot_knife_rush.h"
#include "bot/behavior/neo_bot_ctg_enemy.h"
#include "bot/neo_bot_path_compute.h"
#include "weapon_knife.h"

// Declared in neo_bot_ctg_enemy.cpp, next to CNEOBotCtgEnemy::IsLosingTheRace.
extern ConVar sv_neo_bot_ctg_no_retreat_when_carrier_ahead;

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
bool CNEOBotKnifeRush::IsPossible( CNEOBot *me )
{
	if ( !sv_neo_bot_ctg_no_retreat_when_carrier_ahead.GetBool()
		|| NEORules()->GetGameType() != NEO_GAME_TYPE_CTG
		|| !CNEOBotCtgEnemy::IsLosingTheRace( me ) )
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

	// Movement only - CNEOBotMainAction::Update runs every tick regardless of what covers
	// TacticalMonitor, and its EquipRequiredWeapon() / FireWeaponAtEnemy() /
	// UpdateLookingAroundForEnemies() already aim at and swing the forced knife once we are close
	// enough. Nothing here needs to press fire itself.
	CNEOBotPathUpdateChase( me, m_chasePath, threat->GetEntity(), FASTEST_ROUTE );

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
