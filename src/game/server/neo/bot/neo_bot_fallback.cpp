#include "cbase.h"
#include "neo_player.h"
#include "neo_gamerules.h"
#include "bot/neo_bot.h"
#include "bot/neo_bot_fallback.h"
#include "bot/behavior/neo_bot_fall_back.h"
#include "bot/behavior/neo_bot_retreat_to_cover.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

ConVar neo_bot_research_fallback( "neo_bot_research_fallback", "0", FCVAR_CHEAT,
	"Research: a bot that sees more enemies than it has support falls back instead of fighting alone. "
	"1 = retreat to cover, 2 = fall back toward the nearest teammates, "
	"3 = also a lone bot holds fire on an enemy that has not noticed it and falls back toward teammates." );
ConVar neo_bot_research_fallback_log( "neo_bot_research_fallback_log", "0", FCVAR_CHEAT,
	"Research: print NEO_FALLBACK lines when a bot falls back; 2 also prints skipped fall backs." );

extern ConVar neo_bot_research_team_arm;

namespace NEOBotFallback
{
	enum EFallBackMode
	{
		MODE_OFF = 0,
		MODE_COVER,
		MODE_TO_MATES,
		MODE_UNSEEN_ALONE,
	};

	// Teammates this close fight the same fight; friendly positions are on every player's HUD
	static constexpr float MATE_RANGE = 500.0f;

	// No teammate this close: the bot fights alone
	static constexpr float LONE_RANGE = 800.0f;

	// Farther teammates cannot arrive while the fight lasts
	static constexpr float SUPPORT_SEARCH_RANGE = 2000.0f;

	// Enemies farther than this belong to another fight
	static constexpr float FIGHT_RANGE = 2000.0f;

	// An enemy carrier this close is the bot's fight whatever the numbers
	static constexpr float CARRIER_GUARD_RANGE = 1200.0f;

	// Skipped fall backs are logged at most this often per bot
	static constexpr float SKIP_LOG_INTERVAL = 2.0f;

	// The ghost carrier's escorts keep their place; a fall back would strip the carrier
	static constexpr float ESCORT_RANGE = 800.0f;

	// Fighting time between two fall backs, so the team cannot back off for the whole round
	static constexpr float COOLDOWN = 8.0f;

	// Being shot at this recently means the enemy has noticed the bot
	static constexpr float SPOTTED_TIME = 3.0f;

	// The counts are cheap but run every think; a new decision this often is enough
	static constexpr float EVAL_INTERVAL = 0.2f;

	// Hold fire lapses unless the fall back renews it every update
	static constexpr float HOLD_FIRE_TIME = 0.3f;

	struct FallBackState_t
	{
		float flCooldownUntil;
		float flNextEval;
		float flHoldFireUntil;
		float flNextSkipLog;
	};

	static FallBackState_t s_state[ MAX_PLAYERS + 1 ];

	static const char *s_pszTriggerNames[ TRIGGER_COUNT ] =
	{
		"outnumbered",
		"unseen_alone",
	};

	static int GetMode()
	{
		return neo_bot_research_fallback.GetInt();
	}

	static bool IsArmed( const CNEOBot *me )
	{
		if ( GetMode() <= MODE_OFF )
		{
			return false;
		}

		const int iTeamArm = neo_bot_research_team_arm.GetInt();
		return iTeamArm == 0 || iTeamArm == me->GetTeamNumber();
	}

	static FallBackState_t *GetState( CNEOBot *me )
	{
		const int iIndex = me->entindex();
		if ( iIndex <= 0 || iIndex > MAX_PLAYERS )
		{
			return NULL;
		}

		return &s_state[ iIndex ];
	}

	// The upper bound is checked too, as curtime restarts on a map change
	static bool IsTimeLeft( float flUntil, float flWindow )
	{
		const float flLeft = flUntil - gpGlobals->curtime;
		return flLeft > 0.0f && flLeft <= flWindow;
	}

	// Counts the enemies the bot saw in the last few seconds and notes an enemy ghost carrier among the known
	class CCountRecentEnemies : public IVision::IForEachKnownEntity
	{
	public:
		CCountRecentEnemies( CNEOBot *me )
		{
			m_me = me;
			m_nEnemies = 0;
			m_bKnowsGhostCarrier = false;
		}

		virtual bool Inspect( const CKnownEntity &known )
		{
			CBaseEntity *pEntity = known.GetEntity();
			if ( !pEntity || !pEntity->IsAlive() || !m_me->IsEnemy( pEntity ) )
			{
				return true;
			}

			if ( !known.IsVisibleRecently() )
			{
				return true;
			}

			if ( IsThreatGhostCarrier( &known )
				&& ( known.GetLastKnownPosition() - m_me->GetAbsOrigin() ).IsLengthLessThan( CARRIER_GUARD_RANGE ) )
			{
				m_bKnowsGhostCarrier = true;
			}

			if ( ( known.GetLastKnownPosition() - m_me->GetAbsOrigin() ).IsLengthLessThan( FIGHT_RANGE ) )
			{
				++m_nEnemies;
			}

			return true;
		}

		CNEOBot *m_me;
		int m_nEnemies;
		bool m_bKnowsGhostCarrier;
	};

	struct MateScan_t
	{
		int nNear;						// within MATE_RANGE
		float flNearestDist;
		CNEO_Player *pSupport;			// best teammate outside MATE_RANGE to fall back to
		bool bEscorting;				// a friendly ghost carrier is close
	};

	static int CountMatesNear( CNEO_Player *pCenter, int iTeam )
	{
		int nNear = 0;
		for ( int i = 1; i <= gpGlobals->maxClients; ++i )
		{
			CNEO_Player *pPlayer = ToNEOPlayer( UTIL_PlayerByIndex( i ) );
			if ( !pPlayer || pPlayer == pCenter || !pPlayer->IsAlive() || pPlayer->GetTeamNumber() != iTeam )
			{
				continue;
			}

			if ( pCenter->GetAbsOrigin().DistToSqr( pPlayer->GetAbsOrigin() ) < Square( MATE_RANGE ) )
			{
				++nNear;
			}
		}

		return nNear;
	}

	// Teammate positions are shown on every player's HUD
	static void ScanMates( CNEOBot *me, MateScan_t *pScan )
	{
		pScan->nNear = 0;
		pScan->flNearestDist = FLT_MAX;
		pScan->pSupport = NULL;
		pScan->bEscorting = false;

		const int iTeam = me->GetTeamNumber();
		float flBestScore = FLT_MAX;

		for ( int i = 1; i <= gpGlobals->maxClients; ++i )
		{
			CNEO_Player *pPlayer = ToNEOPlayer( UTIL_PlayerByIndex( i ) );
			if ( !pPlayer || pPlayer == me || !pPlayer->IsAlive() || pPlayer->GetTeamNumber() != iTeam )
			{
				continue;
			}

			const float flDist = me->GetAbsOrigin().DistTo( pPlayer->GetAbsOrigin() );
			pScan->flNearestDist = MIN( pScan->flNearestDist, flDist );

			if ( pPlayer->IsCarryingGhost() && flDist < ESCORT_RANGE )
			{
				pScan->bEscorting = true;
			}

			if ( flDist < MATE_RANGE )
			{
				++pScan->nNear;
				continue;
			}

			if ( flDist > SUPPORT_SEARCH_RANGE )
			{
				continue;
			}

			// a teammate with others around it is worth a longer walk
			const float flScore = flDist / ( 1 + CountMatesNear( pPlayer, iTeam ) );
			if ( flScore < flBestScore )
			{
				flBestScore = flScore;
				pScan->pSupport = pPlayer;
			}
		}
	}

	void CountLocalSides( CNEOBot *me, int *pMates, int *pEnemies )
	{
		CCountRecentEnemies count( me );
		me->GetVisionInterface()->ForEachKnownEntity( count );

		*pEnemies = count.m_nEnemies;
		*pMates = CountMatesNear( me, me->GetTeamNumber() );
	}

	bool IsSpotted( CNEOBot *me )
	{
		if ( me->GetTimeSinceLastInjury( GetEnemyTeam( me->GetTeamNumber() ) ) < SPOTTED_TIME )
		{
			return true;
		}

		const CKnownEntity *threat = me->GetVisionInterface()->GetPrimaryKnownThreat( true );
		if ( !threat || !threat->GetEntity() || !threat->IsVisibleInFOVNow() )
		{
			return false;
		}

		return me->IsThreatAimingTowardMe( threat->GetEntity() );
	}

	void HoldFire( CNEOBot *me )
	{
		FallBackState_t *pState = GetState( me );
		if ( pState )
		{
			pState->flHoldFireUntil = gpGlobals->curtime + HOLD_FIRE_TIME;
		}
	}

	bool HoldsFire( CNEOBot *me )
	{
		if ( !IsArmed( me ) )
		{
			return false;
		}

		const FallBackState_t *pState = GetState( me );
		return pState && IsTimeLeft( pState->flHoldFireUntil, HOLD_FIRE_TIME );
	}

	static void LogStart( CNEOBot *me, ETrigger trigger, const char *pszGoal, CNEO_Player *pMate, int nMates, int nEnemies )
	{
		if ( !neo_bot_research_fallback_log.GetBool() )
		{
			return;
		}

		const float flGoalDist = pMate ? me->GetAbsOrigin().DistTo( pMate->GetAbsOrigin() ) : 0.0f;
		Msg( "NEO_FALLBACK t=%.2f team=%d bot=%d cls=%d mode=%d trigger=%s enemies=%d mates=%d goal=%s goal_mate=%d goal_dist=%.0f name=%s\n",
			gpGlobals->curtime, me->GetTeamNumber(), me->entindex(), me->GetClass(), GetMode(), s_pszTriggerNames[ trigger ],
			nEnemies, nMates, pszGoal, pMate ? pMate->entindex() : 0, flGoalDist, me->GetPlayerName() );
	}

	void OnFallBackEnd( CNEOBot *me, ETrigger trigger, const char *pszReason, float flDuration )
	{
		if ( !neo_bot_research_fallback_log.GetBool() )
		{
			return;
		}

		int nMates;
		int nEnemies;
		CountLocalSides( me, &nMates, &nEnemies );

		Msg( "NEO_FALLBACK_END t=%.2f team=%d bot=%d trigger=%s reason=%s dur=%.2f enemies=%d mates=%d alive=%d\n",
			gpGlobals->curtime, me->GetTeamNumber(), me->entindex(), s_pszTriggerNames[ trigger ], pszReason, flDuration,
			nEnemies, nMates, me->IsAlive() ? 1 : 0 );
	}

	// An enemy the bot is watching that faces away and has not shot it does not know the bot is there
	static bool SawUnnoticed( CNEOBot *me, const CKnownEntity *threat )
	{
		return threat->IsVisibleInFOVNow() && !IsSpotted( me );
	}

	Action< CNEOBot > *StartFallBack( CNEOBot *me )
	{
		if ( !IsArmed( me ) || me->IsCarryingGhost() )
		{
			return NULL;
		}

		FallBackState_t *pState = GetState( me );
		if ( !pState || IsTimeLeft( pState->flCooldownUntil, COOLDOWN ) || IsTimeLeft( pState->flNextEval, EVAL_INTERVAL ) )
		{
			return NULL;
		}

		pState->flNextEval = gpGlobals->curtime + EVAL_INTERVAL;

		const CKnownEntity *threat = me->GetVisionInterface()->GetPrimaryKnownThreat( true );
		if ( !threat || !threat->GetEntity() || !threat->IsVisibleRecently() )
		{
			return NULL;
		}

		CCountRecentEnemies count( me );
		me->GetVisionInterface()->ForEachKnownEntity( count );

		MateScan_t mates;
		ScanMates( me, &mates );

		const int nEnemies = count.m_nEnemies;
		ETrigger trigger;
		if ( nEnemies > 1 + mates.nNear )
		{
			trigger = TRIGGER_OUTNUMBERED;
		}
		else if ( GetMode() >= MODE_UNSEEN_ALONE && mates.flNearestDist > LONE_RANGE && mates.pSupport
			&& SawUnnoticed( me, threat ) )
		{
			trigger = TRIGGER_UNSEEN_ALONE;
		}
		else
		{
			return NULL;
		}

		// the enemy carrier must be stopped whatever the numbers, and the own carrier keeps its escorts
		const char *pszSkip = NULL;
		if ( count.m_bKnowsGhostCarrier )
		{
			pszSkip = "enemy_carrier";
		}
		else if ( mates.bEscorting )
		{
			pszSkip = "escort";
		}

		if ( pszSkip )
		{
			if ( neo_bot_research_fallback_log.GetInt() >= 2 && !IsTimeLeft( pState->flNextSkipLog, SKIP_LOG_INTERVAL ) )
			{
				pState->flNextSkipLog = gpGlobals->curtime + SKIP_LOG_INTERVAL;
				Msg( "NEO_FALLBACK_SKIP t=%.2f team=%d bot=%d trigger=%s skip=%s enemies=%d mates=%d\n",
					gpGlobals->curtime, me->GetTeamNumber(), me->entindex(), s_pszTriggerNames[ trigger ], pszSkip,
					nEnemies, mates.nNear );
			}

			return NULL;
		}

		pState->flCooldownUntil = gpGlobals->curtime + COOLDOWN;

		// the unseen-alone trigger always has a teammate to fall back to
		if ( GetMode() == MODE_COVER || !mates.pSupport )
		{
			LogStart( me, trigger, "cover", NULL, mates.nNear, nEnemies );
			return new CNEOBotRetreatToCover;
		}

		LogStart( me, trigger, "mate", mates.pSupport, mates.nNear, nEnemies );
		return new CNEOBotFallBack( mates.pSupport, trigger );
	}
}
