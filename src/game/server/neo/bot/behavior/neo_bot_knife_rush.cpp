#include "cbase.h"
#include "neo_player.h"
#include "bot/neo_bot.h"
#include "bot/behavior/neo_bot_knife_rush.h"
#include "bot/neo_bot_path_compute.h"

ConVar sv_neo_bot_force_knife_fight( "sv_neo_bot_force_knife_fight", "0", FCVAR_CHEAT,
	"If enabled, bots only use knives against enemies", true, 0, true, 1 );

ConVar sv_neo_bot_knife_rush_enter_range( "sv_neo_bot_knife_rush_enter_range", "150", FCVAR_CHEAT,
	"Range at which the bots consider drawing a knife", true, 0.0f, false, 0.0f );

ConVar sv_neo_bot_knife_rush_exit_range( "sv_neo_bot_knife_rush_exit_range", "250", FCVAR_CHEAT,
	"Range at which bots give up rushing with a knife", true, 0.0f, false, 0.0f );

ConVar sv_neo_bot_knife_rush_flank_distance( "sv_neo_bot_knife_rush_flank_distance", "40", FCVAR_CHEAT,
	"Distance from target's back that bots target for knife fighting positioning", true, 0.0f, false, 0.0f );

ConVar sv_neo_bot_knife_rush_repath_seconds( "sv_neo_bot_knife_rush_repath_seconds", "0.3", FCVAR_CHEAT,
	"Repath timer for bots maneuvering for knife attack", true, 0.05f, false, 0.0f );

//---------------------------------------------------------------------------------------------
static bool WeaponSlotIsDry( CNEOBot *me, int slot )
{
	CNEOBaseCombatWeapon *weapon = static_cast< CNEOBaseCombatWeapon * >( me->Weapon_GetSlot( slot ) );
	if ( !weapon || !CNEOBot::IsRanged( weapon ) )
	{
		return true;
	}
	return weapon->Clip1() <= 0 && me->GetAmmoCount( weapon->GetPrimaryAmmoType() ) <= 0;
}

//---------------------------------------------------------------------------------------------
bool CNEOBotKnifeRush::ShouldRush( CNEOBot *me )
{
	if ( !me->Weapon_OwnsThisType( "weapon_knife" ) )
	{
		return false;
	}

	const CKnownEntity *threat = me->GetVisionInterface()->GetPrimaryKnownThreat( true );
	if ( !threat || !threat->GetEntity() || !threat->GetEntity()->IsAlive() )
	{
		return false;
	}

	if ( !me->IsRangeLessThan( threat->GetEntity()->GetAbsOrigin(), sv_neo_bot_knife_rush_enter_range.GetFloat() ) )
	{
		return false;
	}

	if ( sv_neo_bot_force_knife_fight.GetBool() )
	{
		return true;
	}

	return WeaponSlotIsDry( me, 0 ) && WeaponSlotIsDry( me, 1 );
}

//---------------------------------------------------------------------------------------------
QueryResultType CNEOBotKnifeRush::ShouldRetreat( const INextBot *me )
{
	return ANSWER_NO;
}

//---------------------------------------------------------------------------------------------
// Position to move towards target's back
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

	Vector vecGoal = pThreat->GetAbsOrigin(); // will subtract back position later

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

	const float flExitRange = sv_neo_bot_knife_rush_exit_range.GetFloat();

	const float flDistSq = me->GetAbsOrigin().DistToSqr( threat->GetEntity()->GetAbsOrigin() );
	if ( !sv_neo_bot_force_knife_fight.GetBool() && flDistSq > Square( flExitRange ) )
	{
		return Done( "Threat broke off from knife range, reevaluating situation" );
	}

	if ( m_repathTimer.IsElapsed() )
	{
		m_repathTimer.Start( sv_neo_bot_knife_rush_repath_seconds.GetFloat() );
		RepathToFlank( me );
	}

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
	return TryDone( RESULT_TRY, "Got stuck during knife fight, reevaluating situation" );
}

//---------------------------------------------------------------------------------------------
EventDesiredResult< CNEOBot > CNEOBotKnifeRush::OnMoveToFailure( CNEOBot *me, const Path *path, MoveToFailureType reason )
{
	return TryDone( RESULT_TRY, "Failed to move towards knife target, reevaluating situation" );
}
