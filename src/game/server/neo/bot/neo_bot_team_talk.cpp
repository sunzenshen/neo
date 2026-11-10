#include "cbase.h"
#include "bot/neo_bot_team_talk.h"
#include "bot/neo_bot.h"
#include "nav_mesh.h"
#include "nav_pathfind.h"
#include "neo_gamerules.h"
#include "neo_player.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

ConVar neo_bot_research_death_callout( "neo_bot_research_death_callout", "0", FCVAR_CHEAT,
	"Research: a dying bot calls out its killer to its team, and the team avoids where it fell for a while." );
ConVar neo_bot_research_spot_callout( "neo_bot_research_spot_callout", "0", FCVAR_CHEAT,
	"Research: a bot calls out to its team where it saw an enemy come into view." );
ConVar neo_bot_research_teamtalk_log( "neo_bot_research_teamtalk_log", "0", FCVAR_CHEAT,
	"Research: print NEO_DEATHCALL and NEO_SPOTCALL lines for team callouts." );

extern ConVar neo_bot_research_team_arm;

namespace NEOBotTeamTalk
{
	// Time from a death to the team acting on the dying player's last words
	static constexpr float DEATH_CALLOUT_DELAY = 1.0f;

	// A dying player names where their killer is only if they saw the killer this recently
	static constexpr float DEATH_CALLOUT_SEEN_WINDOW = 3.0f;

	// How long the team avoids the place where a teammate died, with the cost fading to nothing
	static constexpr float FRESH_DEATH_DURATION = 20.0f;

	// Peak path cost of a fresh death area: on the scale of a teammate's path claim
	// and of the per-bot route preference, so the cost can change which lane a bot takes
	static constexpr float FRESH_DEATH_COST = 6000.0f;

	// Travel distance around the death spot that the mark covers, so a neighboring area is not a free bypass
	static constexpr float FRESH_DEATH_RADIUS = 250.0f;

	// Time from a sighting to the team acting on the callout
	static constexpr float SPOT_CALLOUT_DELAY = 0.75f;

	// One spoken callout per bot this often, as a player cannot name every enemy at once
	static constexpr float SPOT_CALLER_INTERVAL = 1.0f;

	// The team calls out the same enemy at most this often, as players do not repeat a fresh callout
	static constexpr float SPOT_ENEMY_INTERVAL = 2.0f;

	// Bounds the queue if many callouts are spoken at once
	static constexpr int MAX_PENDING_CALLOUTS = 64;

	enum CalloutKind_t
	{
		CALLOUT_DEATH,
		CALLOUT_SPOT,
	};

	struct PendingCallout_t
	{
		CalloutKind_t eKind;
		int iTeam;
		int iSpeaker;
		EHANDLE hEnemy;
		Vector vecPos;
		float flKnownTime;
		float flDeliverTime;
	};

	static CUtlVector< PendingCallout_t > s_pendingCallouts;

	// Expired marks are dropped this often
	static constexpr float FRESH_DEATH_PRUNE_INTERVAL = 1.0f;

	// Nav area ID -> when a teammate died there
	struct FreshDeathMap_t
	{
		FreshDeathMap_t() : marks( DefLessFunc( int ) ) {}
		CUtlMap< int, float > marks;
	};

	static FreshDeathMap_t s_freshDeaths[ TEAM__TOTAL ];
	static float s_flNextPruneTime = 0.0f;
	static int s_freshDeathCostHits[ TEAM__TOTAL ];
	static int s_iRoundNumber = -1;

	static float s_flNextCallerTime[ MAX_PLAYERS + 1 ];
	static float s_flNextEnemyTime[ TEAM__TOTAL ][ MAX_PLAYERS + 1 ];
	static int s_spotRateLimited[ TEAM__TOTAL ];

	static bool IsTeamArmed( const ConVar &research, int iTeam )
	{
		if ( !research.GetBool() )
		{
			return false;
		}

		const int iTeamArm = neo_bot_research_team_arm.GetInt();
		return iTeamArm == 0 || iTeamArm == iTeam;
	}

	static bool IsLogging()
	{
		return neo_bot_research_teamtalk_log.GetBool();
	}

	static bool IsValidTeam( int iTeam )
	{
		return iTeam >= 0 && iTeam < TEAM__TOTAL;
	}

	static void ClearRound()
	{
		s_pendingCallouts.RemoveAll();
		for ( int i = 0; i < TEAM__TOTAL; ++i )
		{
			s_freshDeaths[ i ].marks.RemoveAll();
			s_freshDeathCostHits[ i ] = 0;
			s_spotRateLimited[ i ] = 0;
		}

		V_memset( s_flNextCallerTime, 0, sizeof( s_flNextCallerTime ) );
		V_memset( s_flNextEnemyTime, 0, sizeof( s_flNextEnemyTime ) );
	}

	// Teammates see on the HUD where a teammate fell, so the whole team marks the spot
	static int MarkFreshDeath( int iTeam, CNEOBot *pVictim )
	{
		CNavArea *pArea = pVictim->GetLastKnownArea();
		if ( !pArea )
		{
			pArea = TheNavMesh->GetNearestNavArea( pVictim->GetAbsOrigin() );
		}

		if ( !pArea )
		{
			return 0;
		}

		CUtlVector< CNavArea * > nearAreas;
		CollectSurroundingAreas( &nearAreas, pArea, FRESH_DEATH_RADIUS );

		CUtlMap< int, float > &marks = s_freshDeaths[ iTeam ].marks;
		for ( CNavArea *pNear : nearAreas )
		{
			marks.InsertOrReplace( pNear->GetID(), gpGlobals->curtime );
		}

		return nearAreas.Count();
	}

	static void QueueCallout( CalloutKind_t eKind, int iTeam, int iSpeaker, CBaseEntity *pEnemy,
		const Vector &vecPos, float flKnownTime, float flDelay )
	{
		if ( s_pendingCallouts.Count() >= MAX_PENDING_CALLOUTS )
		{
			return;
		}

		PendingCallout_t callout;
		callout.eKind = eKind;
		callout.iTeam = iTeam;
		callout.iSpeaker = iSpeaker;
		callout.hEnemy = pEnemy;
		callout.vecPos = vecPos;
		callout.flKnownTime = flKnownTime;
		callout.flDeliverTime = gpGlobals->curtime + flDelay;
		s_pendingCallouts.AddToTail( callout );
	}

	void OnBotKilled( CNEOBot *pVictim, const CTakeDamageInfo &info )
	{
		if ( !pVictim || !NEORules()->IsTeamplay() )
		{
			return;
		}

		const int iTeam = pVictim->GetTeamNumber();
		if ( !IsValidTeam( iTeam ) || !IsTeamArmed( neo_bot_research_death_callout, iTeam ) )
		{
			return;
		}

		const int nMarked = MarkFreshDeath( iTeam, pVictim );

		// Last words name the killer's position only when the victim saw who shot them;
		// a shot from an unseen enemy gives a direction at best, so nothing is called
		CNEO_Player *pKiller = ToNEOPlayer( info.GetAttacker() );
		const CKnownEntity *pKnown = ( pKiller && pVictim->IsEnemy( pKiller ) )
			? pVictim->GetVisionInterface()->GetKnown( pKiller ) : nullptr;
		const bool bSawKiller = pKnown && pKnown->WasEverVisible()
			&& pKnown->GetTimeSinceLastSeen() < DEATH_CALLOUT_SEEN_WINDOW;

		if ( IsLogging() )
		{
			Msg( "NEO_DEATHCALL mark t=%.2f team=%d victim=%d killer=%d areas=%d saw=%d\n",
				gpGlobals->curtime, iTeam, pVictim->entindex(), pKiller ? pKiller->entindex() : 0, nMarked, bSawKiller ? 1 : 0 );
		}

		if ( !bSawKiller )
		{
			return;
		}

		const float flKnownTime = gpGlobals->curtime - pKnown->GetTimeSinceLastKnown();
		QueueCallout( CALLOUT_DEATH, iTeam, pVictim->entindex(), pKiller, pKnown->GetLastKnownPosition(),
			flKnownTime, DEATH_CALLOUT_DELAY );
	}

	void OnBotSighted( CNEOBot *pCaller, CBaseEntity *pSubject )
	{
		if ( !pCaller || !NEORules()->IsTeamplay() )
		{
			return;
		}

		const int iTeam = pCaller->GetTeamNumber();
		if ( !IsValidTeam( iTeam ) || !IsTeamArmed( neo_bot_research_spot_callout, iTeam ) )
		{
			return;
		}

		CNEO_Player *pEnemy = ToNEOPlayer( pSubject );
		if ( !pEnemy || !pEnemy->IsAlive() || !pCaller->IsEnemy( pEnemy ) )
		{
			return;
		}

		// An enemy the ghost shows through a wall reaches the team by the ghost callout, not by sight
		if ( pCaller->IsRevealedByMyGhost( pEnemy )
			&& !pCaller->GetVisionInterface()->IsLineOfSightClear( pEnemy->WorldSpaceCenter() ) )
		{
			return;
		}

		const int iCaller = pCaller->entindex();
		const int iEnemy = pEnemy->entindex();
		if ( gpGlobals->curtime < s_flNextCallerTime[ iCaller ] || gpGlobals->curtime < s_flNextEnemyTime[ iTeam ][ iEnemy ] )
		{
			++s_spotRateLimited[ iTeam ];
			return;
		}

		s_flNextCallerTime[ iCaller ] = gpGlobals->curtime + SPOT_CALLER_INTERVAL;
		s_flNextEnemyTime[ iTeam ][ iEnemy ] = gpGlobals->curtime + SPOT_ENEMY_INTERVAL;

		// The position where the caller sees the enemy now, never where the enemy is when the team hears it
		QueueCallout( CALLOUT_SPOT, iTeam, iCaller, pEnemy, pEnemy->GetAbsOrigin(), gpGlobals->curtime, SPOT_CALLOUT_DELAY );
	}

	float GetFreshDeathPathCost( const CNEOBot *me, const CNavArea *area )
	{
		const int iTeam = me->GetTeamNumber();
		if ( !IsValidTeam( iTeam ) )
		{
			return 0.0f;
		}

		const CUtlMap< int, float > &marks = s_freshDeaths[ iTeam ].marks;
		if ( marks.Count() == 0 || !IsTeamArmed( neo_bot_research_death_callout, iTeam ) )
		{
			return 0.0f;
		}

		const unsigned short index = marks.Find( area->GetID() );
		if ( !marks.IsValidIndex( index ) )
		{
			return 0.0f;
		}

		const float flAge = gpGlobals->curtime - marks[ index ];
		if ( flAge >= FRESH_DEATH_DURATION )
		{
			return 0.0f;
		}

		++s_freshDeathCostHits[ iTeam ];
		return FRESH_DEATH_COST * ( 1.0f - flAge / FRESH_DEATH_DURATION );
	}

	// Every live bot teammate of the speaker hears the callout on voice
	static void DeliverCallout( const PendingCallout_t &callout )
	{
		CBaseEntity *pEnemy = callout.hEnemy.Get();
		if ( !pEnemy || !pEnemy->IsAlive() )
		{
			return;
		}

		CNavArea *pArea = TheNavMesh->GetNavArea( callout.vecPos );
		int nMates = 0;
		int nUpdated = 0;
		for ( int i = 1; i <= gpGlobals->maxClients; ++i )
		{
			CNEOBot *pMate = ToNEOBot( UTIL_PlayerByIndex( i ) );
			if ( !pMate || !pMate->IsAlive() || pMate->entindex() == callout.iSpeaker || pMate->GetTeamNumber() != callout.iTeam )
			{
				continue;
			}

			++nMates;
			if ( pMate->GetVisionInterface()->ReportKnownEntityPosition( pEnemy, callout.vecPos, pArea, callout.flKnownTime ) )
			{
				++nUpdated;
			}
		}

		if ( !IsLogging() )
		{
			return;
		}

		if ( callout.eKind == CALLOUT_SPOT )
		{
			Msg( "NEO_SPOTCALL deliver t=%.2f team=%d caller=%d enemy=%d mates=%d updated=%d ratelimited=%d\n",
				gpGlobals->curtime, callout.iTeam, callout.iSpeaker, pEnemy->entindex(), nMates, nUpdated,
				s_spotRateLimited[ callout.iTeam ] );
			s_spotRateLimited[ callout.iTeam ] = 0;
		}
		else
		{
			Msg( "NEO_DEATHCALL deliver t=%.2f team=%d victim=%d killer=%d age=%.2f mates=%d updated=%d costhits=%d\n",
				gpGlobals->curtime, callout.iTeam, callout.iSpeaker, pEnemy->entindex(),
				gpGlobals->curtime - callout.flKnownTime, nMates, nUpdated, s_freshDeathCostHits[ callout.iTeam ] );
			s_freshDeathCostHits[ callout.iTeam ] = 0;
		}
	}

	static void PruneFreshDeaths()
	{
		if ( gpGlobals->curtime < s_flNextPruneTime )
		{
			return;
		}

		s_flNextPruneTime = gpGlobals->curtime + FRESH_DEATH_PRUNE_INTERVAL;
		for ( int iTeam = 0; iTeam < TEAM__TOTAL; ++iTeam )
		{
			CUtlMap< int, float > &marks = s_freshDeaths[ iTeam ].marks;
			for ( unsigned short i = marks.FirstInorder(); marks.IsValidIndex( i ); )
			{
				const unsigned short next = marks.NextInorder( i );
				if ( gpGlobals->curtime - marks[ i ] >= FRESH_DEATH_DURATION )
				{
					marks.RemoveAt( i );
				}
				i = next;
			}
		}
	}

	void Update()
	{
		// A new round respawns everyone, so callouts and marks from the last one are stale
		if ( NEORules()->roundNumber() != s_iRoundNumber )
		{
			s_iRoundNumber = NEORules()->roundNumber();
			ClearRound();
		}

		for ( int i = 0; i < s_pendingCallouts.Count(); ++i )
		{
			if ( s_pendingCallouts[ i ].flDeliverTime > gpGlobals->curtime )
			{
				continue;
			}

			DeliverCallout( s_pendingCallouts[ i ] );
			s_pendingCallouts.Remove( i );
			--i;
		}

		PruneFreshDeaths();
	}

	void Reset()
	{
		s_iRoundNumber = -1;
		s_flNextPruneTime = 0.0f;
		ClearRound();
	}
}
