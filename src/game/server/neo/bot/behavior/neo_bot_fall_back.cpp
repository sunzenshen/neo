#include "cbase.h"
#include "neo_player.h"
#include "bot/neo_bot.h"
#include "bot/neo_bot_path_compute.h"
#include "bot/behavior/neo_bot_fall_back.h"
#include "bot/behavior/neo_bot_retreat_from_grenade.h"
#include "bot/behavior/neo_bot_retreat_to_cover.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

// A fall back that has not found support by then is not going to; the bot fights where it is
static constexpr float FALL_BACK_GIVE_UP_TIME = 4.0f;

// The teammate keeps moving, so the route to it goes stale
static constexpr float FALL_BACK_REPATH_INTERVAL = 0.5f;

// Close enough to the teammate that both fight the same fight
static constexpr float FALL_BACK_REACHED_RANGE = 250.0f;

static constexpr float FALL_BACK_GRENADE_CHECK_INTERVAL = 0.2f;


//---------------------------------------------------------------------------------------------
CNEOBotFallBack::CNEOBotFallBack( CNEO_Player *pMate, NEOBotFallback::ETrigger trigger )
{
	m_hMate = pMate;
	m_trigger = trigger;
	m_pszEndReason = "interrupted";
	m_flStartTime = 0.0f;
}


//---------------------------------------------------------------------------------------------
ActionResult< CNEOBot > CNEOBotFallBack::OnStart( CNEOBot *me, Action< CNEOBot > *priorAction )
{
	m_path.SetMinLookAheadDistance( me->GetDesiredPathLookAheadRange() );
	m_flStartTime = gpGlobals->curtime;
	m_giveUpTimer.Start( FALL_BACK_GIVE_UP_TIME );
	m_repathTimer.Invalidate();

	return Continue();
}


//---------------------------------------------------------------------------------------------
ActionResult< CNEOBot > CNEOBotFallBack::End( const char *pszReason )
{
	m_pszEndReason = pszReason;
	return Done( pszReason );
}


//---------------------------------------------------------------------------------------------
// The reason to hand the fight back, or NULL to keep falling back
const char *CNEOBotFallBack::CheckDone( CNEOBot *me, CNEO_Player *pMate )
{
	if ( !pMate || !pMate->IsAlive() || pMate->GetTeamNumber() != me->GetTeamNumber() )
	{
		return "mate_lost";
	}

	if ( m_giveUpTimer.IsElapsed() )
	{
		return "give_up";
	}

	const CKnownEntity *threat = me->GetVisionInterface()->GetPrimaryKnownThreat( true );
	if ( IsThreatGhostCarrier( threat ) )
	{
		return "ghost_carrier";
	}

	int nMates;
	int nEnemies;
	NEOBotFallback::CountLocalSides( me, &nMates, &nEnemies );

	if ( m_trigger == NEOBotFallback::TRIGGER_UNSEEN_ALONE )
	{
		if ( nMates > 0 )
		{
			return "support";
		}

		if ( NEOBotFallback::IsSpotted( me ) )
		{
			return "spotted";
		}
	}
	else if ( 1 + nMates >= nEnemies )
	{
		return "support";
	}

	if ( me->IsRangeLessThan( pMate, FALL_BACK_REACHED_RANGE ) )
	{
		return "reached";
	}

	return NULL;
}


//---------------------------------------------------------------------------------------------
ActionResult< CNEOBot > CNEOBotFallBack::Update( CNEOBot *me, float interval )
{
	if ( m_grenadeCheckTimer.IsElapsed() )
	{
		m_grenadeCheckTimer.Start( FALL_BACK_GRENADE_CHECK_INTERVAL );

		CBaseEntity *dangerousGrenade = CNEOBotRetreatFromGrenade::FindDangerousGrenade( me );
		if ( dangerousGrenade )
		{
			m_pszEndReason = "grenade";
			return ChangeTo( new CNEOBotRetreatFromGrenade( dangerousGrenade ), "Encountered grenade while falling back" );
		}
	}

	CNEO_Player *pMate = m_hMate.Get();
	const char *pszDone = CheckDone( me, pMate );
	if ( pszDone )
	{
		return End( pszDone );
	}

	// an enemy that has not noticed the bot is left alone until support is near
	if ( m_trigger == NEOBotFallback::TRIGGER_UNSEEN_ALONE )
	{
		NEOBotFallback::HoldFire( me );
	}

	// shoot back while falling back
	const CKnownEntity *threat = me->GetVisionInterface()->GetPrimaryKnownThreat( true );
	me->EquipBestWeaponForThreat( threat );
	me->EnableCloak( 3.0f );

	if ( !m_path.IsValid() || m_repathTimer.IsElapsed() )
	{
		m_repathTimer.Start( FALL_BACK_REPATH_INTERVAL );

		CNEOBotPathCompute( me, m_path, pMate->GetAbsOrigin(), RETREAT_ROUTE );
		if ( !CNEOBotPathReachesGoal( m_path ) )
		{
			return End( "no_route" );
		}
	}

	m_path.Update( me );

	return Continue();
}


//---------------------------------------------------------------------------------------------
void CNEOBotFallBack::OnEnd( CNEOBot *me, Action< CNEOBot > *nextAction )
{
	NEOBotFallback::OnFallBackEnd( me, m_trigger, m_pszEndReason, gpGlobals->curtime - m_flStartTime );
}


//---------------------------------------------------------------------------------------------
EventDesiredResult< CNEOBot > CNEOBotFallBack::OnStuck( CNEOBot *me )
{
	m_path.Invalidate();
	return TryContinue();
}


//---------------------------------------------------------------------------------------------
EventDesiredResult< CNEOBot > CNEOBotFallBack::OnMoveToFailure( CNEOBot *me, const Path *path, MoveToFailureType reason )
{
	m_path.Invalidate();
	return TryContinue();
}


//---------------------------------------------------------------------------------------------
// Moving to support is the retreat; the back-off check would only send the bot to cover away from it
QueryResultType CNEOBotFallBack::ShouldHurry( const INextBot *me ) const
{
	return ANSWER_YES;
}
