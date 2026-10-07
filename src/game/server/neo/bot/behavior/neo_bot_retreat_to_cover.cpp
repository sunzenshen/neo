//========= Copyright Valve Corporation, All rights reserved. ============//

#include "cbase.h"
#include "neo_player.h"
#include "neo_smokelineofsightblocker.h"
#include "bot/neo_bot.h"
#include "bot/behavior/neo_bot_grenade_dispatch.h"
#include "bot/behavior/neo_bot_retreat_from_grenade.h"
#include "bot/behavior/neo_bot_retreat_to_cover.h"
#include "bot/neo_bot_path_compute.h"
#include "bot/neo_bot_path_reservation.h"

extern ConVar neo_bot_path_lookahead_range;
ConVar neo_bot_retreat_to_cover_range( "neo_bot_retreat_to_cover_range", "1000", FCVAR_CHEAT );
ConVar neo_bot_debug_retreat_to_cover( "neo_bot_debug_retreat_to_cover", "0", FCVAR_CHEAT );
ConVar neo_bot_wait_in_cover_min_time( "neo_bot_wait_in_cover_min_time", "1", FCVAR_CHEAT );
ConVar neo_bot_wait_in_cover_max_time( "neo_bot_wait_in_cover_max_time", "2", FCVAR_CHEAT );


//---------------------------------------------------------------------------------------------
CNEOBotRetreatToCover::CNEOBotRetreatToCover( float hideDuration )
{
	m_hideDuration = hideDuration;
	m_actionToChangeToOnceCoverReached = NULL;
}


//---------------------------------------------------------------------------------------------
CNEOBotRetreatToCover::CNEOBotRetreatToCover( Action< CNEOBot > *actionToChangeToOnceCoverReached )
{
	m_hideDuration = -1.0f;
	m_actionToChangeToOnceCoverReached = actionToChangeToOnceCoverReached;
}


//---------------------------------------------------------------------------------------------
// for testing a given area's exposure to known threats
class CTestAreaAgainstThreats :  public IVision::IForEachKnownEntity
{
public:
	CTestAreaAgainstThreats( CNEOBot *me, CNavArea *area )
	{
		m_me = me;
		m_area = area;
		m_exposedThreatCount = 0;
	}

	virtual bool Inspect( const CKnownEntity &known )
	{
		VPROF_BUDGET( "CTestAreaAgainstThreats::Inspect", "NextBot" );

		if ( !m_me->IsEnemy( known.GetEntity() ) )
		{
			return true; // Known entity is not my enemy
		}

		const CNavArea *threatArea = known.GetLastKnownArea();

		if ( !threatArea )
		{
			return true; // Can't test area if we don't know last area of threat
		}

		if ( !threatArea->IsPotentiallyVisible( m_area ) )
		{
			return true; // Candidate area is not visible by threat
		}

		// Only consider smoke as concealment if I can see through it but the enemy cannot (thermal vision)
		CNEO_Player *pThreatPlayer = ToNEOPlayer( known.GetEntity() );
		if ( pThreatPlayer && (pThreatPlayer->GetClass() != NEO_CLASS_SUPPORT) && (m_me->GetClass() == NEO_CLASS_SUPPORT) )
		{
			ScopedSmokeLOS smokeScope( false );

			Vector vecThreatEye = known.GetLastKnownPosition() + pThreatPlayer->GetViewOffset();
			Vector vecCandidateArea = m_area->GetCenter() + m_me->GetViewOffset();

			trace_t tr;
			CTraceFilterSimple filter( known.GetEntity(), COLLISION_GROUP_NONE);
			UTIL_TraceLine( vecThreatEye, vecCandidateArea, MASK_BLOCKLOS, &filter, &tr );

			if ( tr.fraction < 1.0f )
			{
				return true; // Smoke provides concealment from this threat
			}
		}

		++m_exposedThreatCount;  // Area is exposed to threat

		// Return true to continue searching other known entities
		return true;
	}

	CNEOBot *m_me;
	CNavArea *m_area;
	int m_exposedThreatCount;
};


//---------------------------------------------------------------------------------------------
int CountThreatsExposingArea( CNEOBot *me, CNavArea *area )
{
	CTestAreaAgainstThreats test( me, area );
	me->GetVisionInterface()->ForEachKnownEntity( test );
	return test.m_exposedThreatCount;
}


// collect nearby areas that provide cover from our known threats
class CSearchForCover : public ISearchSurroundingAreasFunctor
{
public:
	CSearchForCover( CNEOBot *me )
	{
		m_me = me;
		m_onStuckPenalty = neo_bot_path_reservation_onstuck_penalty.GetFloat();
		m_minExposureCount = 9999;

		if ( neo_bot_debug_retreat_to_cover.GetBool() )
			TheNavMesh->ClearSelectedSet();
	}

	virtual bool operator() ( CNavArea *baseArea, CNavArea *priorArea, float travelDistanceSoFar )
	{
		VPROF_BUDGET( "CSearchForCover::operator()", "NextBot" );

		CNavArea *area = (CNavArea *)baseArea;

		// Skip areas that are hazardous or where bots get stuck
		if ( neo_bot_path_reservation_enable.GetBool() )
		{
			int navAreaId = area->GetID();
			if (CNEOBotPathReservations()->IsAreaHazardous(navAreaId, m_me))
			{
				return true;
			}
			if (CNEOBotPathReservations()->GetAreaAvoidPenalty(navAreaId) >= m_onStuckPenalty)
			{
				return true;
			}
		}

		CTestAreaAgainstThreats test( m_me, area );
		m_me->GetVisionInterface()->ForEachKnownEntity( test );

		if ( test.m_exposedThreatCount <= m_minExposureCount )
		{
			// this area is at least as good as already found cover
			if ( test.m_exposedThreatCount < m_minExposureCount )
			{
				// this area is better than already found cover - throw out list and start over
				m_coverAreaVector.RemoveAll();
				m_minExposureCount = test.m_exposedThreatCount;
			}

			m_coverAreaVector.AddToTail( area );
		}

		return true;				
	}

	// return true if 'adjArea' should be included in the ongoing search
	virtual bool ShouldSearch( CNavArea *adjArea, CNavArea *currentArea, float travelDistanceSoFar ) 
	{
		if ( travelDistanceSoFar > neo_bot_retreat_to_cover_range.GetFloat() )
			return false;

		// allow falling off ledges, but don't jump up - too slow
		return ( currentArea->ComputeAdjacentConnectionHeightChange( adjArea ) < m_me->GetLocomotionInterface()->GetStepHeight() );
	}

	virtual void PostSearch( void )
	{
		if ( neo_bot_debug_retreat_to_cover.GetBool() )
		{
			for( int i=0; i<m_coverAreaVector.Count(); ++i )
				TheNavMesh->AddToSelectedSet( m_coverAreaVector[i] );
		}
	}

	CNEOBot *m_me;
	CUtlVector< CNavArea * > m_coverAreaVector;
	int m_minExposureCount;
	float m_onStuckPenalty;
};


// NEO-HARNESS-TEMP research arm (2026-10-07): several bots retreating from one threat pick the same small cover area
// and meet in the way there (sentinel_jgr crate aisle, cover area 3089). Skip a cover area a teammate stands in,
// or one a teammate announced as its own retreat goal within the last few seconds.
ConVar neo_bot_retreat_cover_claim( "neo_bot_retreat_cover_claim", "0", FCVAR_CHEAT,
	"Research: a retreat skips cover areas a teammate occupies or is retreating to" );

// A teammate's announced cover goal counts for this long
static constexpr float NEO_COVER_CLAIM_TIME = 4.0f;

struct NeoCoverClaim
{
	int iAreaID;
	int iTeam;
	float flExpire;
};
static NeoCoverClaim s_coverClaims[ MAX_PLAYERS + 1 ];

static bool NeoCoverAreaTaken( CNEOBot *me, const CNavArea *area )
{
	for ( int i = 1; i <= gpGlobals->maxClients; ++i )
	{
		if ( i == me->entindex() )
		{
			continue;
		}

		const NeoCoverClaim &claim = s_coverClaims[ i ];
		if ( claim.iAreaID == (int)area->GetID() && claim.iTeam == me->GetTeamNumber() && claim.flExpire > gpGlobals->curtime )
		{
			CBasePlayer *pClaimant = UTIL_PlayerByIndex( i );
			if ( pClaimant && pClaimant->IsAlive() )
			{
				return true;
			}
		}
	}

	CUtlVector< CNEO_Player * > teammates;
	CollectPlayers( &teammates, me->GetTeamNumber(), COLLECT_ONLY_LIVING_PLAYERS );
	for ( CNEO_Player *pMate : teammates )
	{
		if ( pMate != me && pMate->GetLastKnownArea() == area )
		{
			return true;
		}
	}

	return false;
}

//---------------------------------------------------------------------------------------------
CNavArea *CNEOBotRetreatToCover::FindCoverArea( CNEOBot *me )
{
	VPROF_BUDGET( "CNEOBotRetreatToCover::FindCoverArea", "NextBot" );

	CSearchForCover search( me );
	SearchSurroundingAreas( me->GetLastKnownArea(), search );

	if ( search.m_coverAreaVector.Count() == 0 )
	{
		return NULL;
	}

	if ( neo_bot_retreat_cover_claim.GetBool() )
	{
		// the closest 10 cover areas no teammate holds or is heading to; all taken, choose among them all as before
		CUtlVector< CNavArea * > free;
		for ( int i = 0; i < search.m_coverAreaVector.Count() && free.Count() < 10; ++i )
		{
			if ( !NeoCoverAreaTaken( me, search.m_coverAreaVector[ i ] ) )
			{
				free.AddToTail( search.m_coverAreaVector[ i ] );
			}
		}

		CNavArea *pChoice = free.Count() > 0 ? free[ RandomInt( 0, free.Count() - 1 ) ]
			: search.m_coverAreaVector[ RandomInt( 0, MIN( 10, search.m_coverAreaVector.Count() ) - 1 ) ];

		NeoCoverClaim &claim = s_coverClaims[ me->entindex() ];
		claim.iAreaID = pChoice->GetID();
		claim.iTeam = me->GetTeamNumber();
		claim.flExpire = gpGlobals->curtime + NEO_COVER_CLAIM_TIME;
		return pChoice;
	}

	// first in vector should be closest via travel distance
	// pick from the closest 10 areas to avoid the whole team bunching up in one spot
	int last = MIN( 10, search.m_coverAreaVector.Count() );
	int which = RandomInt( 0, last-1 );
	return search.m_coverAreaVector[ which ];
}


//---------------------------------------------------------------------------------------------
ActionResult< CNEOBot >	CNEOBotRetreatToCover::OnStart( CNEOBot *me, Action< CNEOBot > *priorAction )
{
	m_path.SetMinLookAheadDistance( me->GetDesiredPathLookAheadRange() );

	m_coverArea = FindCoverArea( me );

	if ( m_coverArea == NULL )
		return Done( "No cover available!" );

	if ( m_hideDuration < 0.0f )
	{
		m_hideDuration = RandomFloat( neo_bot_wait_in_cover_min_time.GetFloat(), neo_bot_wait_in_cover_max_time.GetFloat() );
	}

	m_waitInCoverTimer.Start( m_hideDuration );

	return Continue();
}


//---------------------------------------------------------------------------------------------
ActionResult< CNEOBot >	CNEOBotRetreatToCover::Update( CNEOBot *me, float interval )
{
	if ( m_grenadeCheckTimer.IsElapsed() )
	{
		m_grenadeCheckTimer.Start( 0.2f );

		CBaseEntity *dangerousGrenade = CNEOBotRetreatFromGrenade::FindDangerousGrenade( me );
		if ( dangerousGrenade )
		{
			// ChangeTo: Avoid behavior pingpong if grenade avoidance can't find cover
			return ChangeTo( new CNEOBotRetreatFromGrenade( dangerousGrenade ), "Encountered grenade while retreating to cover!" );
		}
	}

	const CKnownEntity *threat = me->GetVisionInterface()->GetPrimaryKnownThreat( true );

	if (!threat)
	{
		me->ReloadIfLowClip();
	}
	else if ( threat->GetEntity() && threat->GetEntity()->IsPlayer() )
	{
		CNEO_Player *pThreatPlayer = ToNEOPlayer( threat->GetEntity() );
		if ( pThreatPlayer && pThreatPlayer->IsCarryingGhost() )
		{
			return Done( "Stopping retreat because my threat is the ghost carrier" );
		}
	}

	if ( ShouldRetreat( me ) == ANSWER_NO )
		return Done( "No longer need to retreat" );

	// attack while retreating
	me->EquipBestWeaponForThreat( threat );

	// NEO JANK: How we handle reloads and how we classify barrage and reload weapons might not mix well
	// It's probably a bad idea to waste a partial magazine for the PZ or SRS
#ifndef NEO
	// reload while moving to cover
	bool isDoingAFullReload = false;
	CNEOBaseCombatWeapon *myPrimary = (CNEOBaseCombatWeapon*)me->GetActiveWeapon();
	if ( myPrimary && myPrimary->GetPrimaryAmmoCount() > 0 && me->IsBarrageAndReloadWeapon(myPrimary))
	{
		if ( myPrimary->Clip1() < myPrimary->GetMaxClip1() )
		{
			me->PressReloadButton();
			isDoingAFullReload = true;
		}
	}
#endif

	// Consider throwing a grenade
	if ( ( !m_grenadeThrowCooldownTimer.HasStarted() || m_grenadeThrowCooldownTimer.IsElapsed() ) &&
	     threat && threat->GetEntity() && !me->IsLineOfFireClear( threat->GetEntity()->EyePosition(), CNEOBot::LINE_OF_FIRE_FLAGS_DEFAULT ) )
	{
		m_grenadeThrowCooldownTimer.Start( sv_neo_bot_grenade_throw_cooldown.GetFloat() );
		Action<CNEOBot> *pGrenadeBehavior = CNEOBotGrenadeDispatch::ChooseGrenadeThrowBehavior( me, threat );
		if ( pGrenadeBehavior )
		{
			return SuspendFor( pGrenadeBehavior, "Throwing grenade while taking cover!" );
		}
	}

	// move to cover, or stop if we've found opportunistic cover (no visible threats right now)
	if ( me->GetLastKnownArea() == m_coverArea || !threat )
	{
		// we are now in cover

		if ( threat )
		{
			// threats are still visible - find new cover
			CNavArea *pPrevCoverArea = m_coverArea;
			m_coverArea = FindCoverArea( me );

			if ( m_coverArea == NULL )
			{
				return Done( "My cover is exposed, and there is no other cover available!" );
			}

			// cover destination changed, stop following the path to the old spot
			if ( m_coverArea != pPrevCoverArea )
			{
				m_path.Invalidate();
			}
		}
		else
		{
			me->DisableCloak();
		}

		if ( m_actionToChangeToOnceCoverReached )
		{
			return ChangeTo( m_actionToChangeToOnceCoverReached, "Doing given action now that I'm in cover" );
		}

		// stay in cover while we fully reload
		CNEOBaseCombatWeapon* myWeapon = static_cast<CNEOBaseCombatWeapon*>(me->GetActiveWeapon());
		if ( myWeapon && myWeapon->m_bInReload )
		{
			return Continue();
		}

		if ( m_waitInCoverTimer.IsElapsed() )
		{
			return Done( "Been in cover long enough" );
		}
	}
	else
	{
		// not in cover yet
		me->EnableCloak( 3.0f );

		m_waitInCoverTimer.Reset();

		if ( !m_path.IsValid() )
		{
			CNEOBotPathCompute( me, m_path, m_coverArea->GetCenter(), RETREAT_ROUTE );

			// Cover is chosen where the retreat starts; after a drop off a ledge no route
			// may reach it any more, so choose again from here
			if ( !CNEOBotPathReachesGoal( m_path ) )
			{
				m_coverArea = FindCoverArea( me );
				if ( m_coverArea == NULL )
				{
					return Done( "My cover is out of reach, and there is no other cover available!" );
				}

				CNEOBotPathCompute( me, m_path, m_coverArea->GetCenter(), RETREAT_ROUTE );
			}
		}

		m_path.Update( me );
	}

	return Continue();
}


//---------------------------------------------------------------------------------------------
EventDesiredResult< CNEOBot > CNEOBotRetreatToCover::OnStuck( CNEOBot *me )
{
	m_path.Invalidate();
	return TryContinue();
}


//---------------------------------------------------------------------------------------------
EventDesiredResult< CNEOBot > CNEOBotRetreatToCover::OnMoveToSuccess( CNEOBot *me, const Path *path )
{
	return TryContinue();
}


//---------------------------------------------------------------------------------------------
EventDesiredResult< CNEOBot > CNEOBotRetreatToCover::OnMoveToFailure( CNEOBot *me, const Path *path, MoveToFailureType reason )
{
	m_path.Invalidate();
	return TryContinue();
}


//---------------------------------------------------------------------------------------------
// Hustle yer butt to safety!
QueryResultType CNEOBotRetreatToCover::ShouldHurry( const INextBot *me ) const
{
	return ANSWER_YES;
}


