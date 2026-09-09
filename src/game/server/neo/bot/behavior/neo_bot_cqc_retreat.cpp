#include "cbase.h"
#include "neo_player.h"
#include "bot/neo_bot.h"
#include "bot/behavior/neo_bot_cqc_retreat.h"
#include "bot/behavior/neo_bot_knife_rush.h"	// CqcExperimentArmForBot, sv_neo_bot_ctg_knife_rush_enter_range
#include "bot/neo_bot_path_compute.h"

// NEO-HARNESS-TEMP (proposal 0014). All FCVAR_CHEAT, all default to a disabled/neutral value.
ConVar sv_neo_bot_cqc_retreat_distance( "sv_neo_bot_cqc_retreat_distance", "400", FCVAR_CHEAT,
	"CQC experiment (retreat arm): how many units back along the reverse bearing from the threat "
	"to path when breaking contact.",
	true, 0.0f, false, 0.0f );

ConVar sv_neo_bot_cqc_retreat_exit_range( "sv_neo_bot_cqc_retreat_exit_range", "450", FCVAR_CHEAT,
	"CQC experiment (retreat arm): stop retreating once the threat is at least this far away - the "
	"bot has re-opened the fight to gun range. Defaults to the same 450u the no-fire gate uses.",
	true, 0.0f, false, 0.0f );

ConVar sv_neo_bot_cqc_retreat_repath_seconds( "sv_neo_bot_cqc_retreat_repath_seconds", "0.3", FCVAR_CHEAT,
	"CQC experiment (retreat arm): seconds between re-aiming the retreat path at the threat's "
	"current position.",
	true, 0.05f, false, 0.0f );

ConVar sv_neo_bot_cqc_retreat_max_seconds( "sv_neo_bot_cqc_retreat_max_seconds", "4.0", FCVAR_CHEAT,
	"CQC experiment (retreat arm): give up the retreat after this many seconds even if the threat "
	"is still close, so a cornered bot turns and fights instead of backpedalling into a wall.",
	true, 0.5f, false, 0.0f );

//---------------------------------------------------------------------------------------------
bool CNEOBotCqcRetreat::IsPossible( CNEOBot *me )
{
	if ( CqcExperimentArmForBot( me ) != CQC_ARM_RETREAT )
	{
		return false;
	}

	const CKnownEntity *threat = me->GetVisionInterface()->GetPrimaryKnownThreat( true );
	if ( !threat || !threat->GetEntity() || !threat->GetEntity()->IsAlive() )
	{
		return false;
	}

	const float flDistSq = me->GetAbsOrigin().DistToSqr( threat->GetEntity()->GetAbsOrigin() );
	return flDistSq <= Square( sv_neo_bot_ctg_knife_rush_enter_range.GetFloat() );
}

//---------------------------------------------------------------------------------------------
bool CNEOBotCqcRetreat::RepathAway( CNEOBot *me )
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

	Vector vecAway = me->GetAbsOrigin() - pThreat->GetAbsOrigin();
	vecAway.z = 0.0f;
	if ( vecAway.NormalizeInPlace() < 0.01f )
	{
		// Practically on top of each other - pick the bot's own facing as the escape bearing.
		Vector vecForward;
		AngleVectors( QAngle( 0.0f, me->EyeAngles().y, 0.0f ), &vecForward );
		vecAway = vecForward;
	}

	const Vector vecGoal = me->GetAbsOrigin() + vecAway * sv_neo_bot_cqc_retreat_distance.GetFloat();
	return CNEOBotPathCompute( me, m_path, vecGoal, RETREAT_ROUTE );
}

//---------------------------------------------------------------------------------------------
ActionResult< CNEOBot > CNEOBotCqcRetreat::OnStart( CNEOBot *me, Action< CNEOBot > *priorAction )
{
	m_path.SetMinLookAheadDistance( me->GetDesiredPathLookAheadRange() );
	RepathAway( me );
	m_repathTimer.Start( sv_neo_bot_cqc_retreat_repath_seconds.GetFloat() );
	m_giveUpTimer.Start( sv_neo_bot_cqc_retreat_max_seconds.GetFloat() );

	return Continue();
}

//---------------------------------------------------------------------------------------------
ActionResult< CNEOBot > CNEOBotCqcRetreat::Update( CNEOBot *me, float interval )
{
	const CKnownEntity *threat = me->GetVisionInterface()->GetPrimaryKnownThreat( true );
	if ( !threat || !threat->GetEntity() || !threat->GetEntity()->IsAlive() )
	{
		return Done( "Threat is gone" );
	}

	const float flDistSq = me->GetAbsOrigin().DistToSqr( threat->GetEntity()->GetAbsOrigin() );
	if ( flDistSq >= Square( sv_neo_bot_cqc_retreat_exit_range.GetFloat() ) )
	{
		return Done( "Opened the fight back to gun range" );
	}

	if ( m_giveUpTimer.IsElapsed() )
	{
		return Done( "Retreat timed out - turning to fight" );
	}

	if ( m_repathTimer.IsElapsed() )
	{
		m_repathTimer.Start( sv_neo_bot_cqc_retreat_repath_seconds.GetFloat() );
		RepathAway( me );
	}

	// Movement only. CNEOBotMainAction::Update still runs every tick and its FireWeaponAtEnemy()
	// keeps shooting the bot's own weapon at the threat while it falls back - that is the whole
	// point of this arm (retreat *with a gun*, not just retreat). The no-fire gate still applies,
	// so it only actually fires once inside sv_neo_bot_cqc_no_fire_range.
	m_path.Update( me );

	return Continue();
}

//---------------------------------------------------------------------------------------------
EventDesiredResult< CNEOBot > CNEOBotCqcRetreat::OnStuck( CNEOBot *me )
{
	if ( !RepathAway( me ) )
	{
		return TryDone( RESULT_TRY, "Stuck with nowhere to fall back to" );
	}

	return TryContinue();
}

//---------------------------------------------------------------------------------------------
EventDesiredResult< CNEOBot > CNEOBotCqcRetreat::OnMoveToFailure( CNEOBot *me, const Path *path, MoveToFailureType reason )
{
	if ( !RepathAway( me ) )
	{
		return TryDone( RESULT_TRY, "No retreat path available" );
	}

	return TryContinue();
}
