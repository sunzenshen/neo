#include "cbase.h"
#include "bot/neo_bot.h"
#include "bot/neo_bot_commit.h"
#include "neo_gamerules.h"
#include "neo_player.h"
#include "weapon_neobasecombatweapon.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

ConVar neo_bot_research_commit( "neo_bot_research_commit", "0", FCVAR_CHEAT,
	"Research: a bot in a fight its team can win holds its ground instead of retreating. "
	"1 = after a teammate's death it noticed, 2 = also when nearby teammates match the visible enemies, "
	"3 = also fights on with a loaded secondary instead of retreating to reload." );
ConVar neo_bot_research_commit_log( "neo_bot_research_commit_log", "0", FCVAR_CHEAT,
	"Research: print NEO_COMMIT lines when a bot holds its ground." );

extern ConVar neo_bot_research_team_arm;

namespace NEOBotCommit
{
	enum ECommitMode
	{
		MODE_OFF = 0,
		MODE_AVENGE,
		MODE_NUMBERS,
		MODE_SECONDARY,
	};

	enum ETrigger
	{
		TRIGGER_NONE = 0,
		TRIGGER_AVENGE,
		TRIGGER_NUMBERS,

		TRIGGER_COUNT
	};

	// The fight that killed a teammate is decided in the next few seconds, and a trade needs the bot in it
	static constexpr float AVENGE_WINDOW = 3.0f;

	// A death this close is heard through walls: the gunfire and the teammate's marker going out
	static constexpr float EARSHOT_RANGE = 800.0f;

	// A death seen farther away than this belongs to another fight
	static constexpr float SEEN_DEATH_RANGE = 1500.0f;

	// Teammates this close fight the same fight; friendly positions are on every player's HUD
	static constexpr float MATE_RANGE = 500.0f;

	// The counts change every think; holding the decision this long stops a retreat from flickering on and off
	static constexpr float NUMBERS_HOLD = 1.0f;

	// A gap this long between two holding decisions starts a new commitment
	static constexpr float EPISODE_GAP = 1.0f;

	struct CommitState_t
	{
		float flAvengeUntil;
		float flNumbersUntil;
		float flLastCommitTime;
		int iKiller;
		bool bEndedRetreat;
		bool bLogged[ RETREAT_SITE_COUNT ];
	};

	static CommitState_t s_state[ MAX_PLAYERS + 1 ];

	static const char *s_pszSiteNames[ RETREAT_SITE_COUNT ] =
	{
		"back_off",
		"reload_cover",
		"too_close",
		"at_goal",
		"in_progress",
	};

	static const char *s_pszTriggerNames[ TRIGGER_COUNT ] =
	{
		"none",
		"avenge",
		"numbers",
	};

	static int GetMode()
	{
		return neo_bot_research_commit.GetInt();
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

	static bool IsValidIndex( int iIndex )
	{
		return iIndex > 0 && iIndex <= MAX_PLAYERS;
	}

	// The upper bound is checked too, as curtime restarts on a map change
	static bool IsTimeLeft( float flUntil, float flWindow )
	{
		const float flLeft = flUntil - gpGlobals->curtime;
		return flLeft > 0.0f && flLeft <= flWindow;
	}

	static bool NoticedDeath( CNEOBot *me, CNEO_Player *pVictim, float flDist )
	{
		if ( flDist <= EARSHOT_RANGE )
		{
			return true;
		}

		if ( flDist > SEEN_DEATH_RANGE )
		{
			return false;
		}

		// The position overload is hidden by the bot vision class's entity overload
		const IVision *pVision = me->GetVisionInterface();
		return pVision->IsAbleToSee( pVictim->WorldSpaceCenter(), IVision::USE_FOV );
	}

	static void CountLocalSides( CNEOBot *me, int *pMates, int *pEnemies )
	{
		const int iTeam = me->GetTeamNumber();
		const int iEnemyTeam = ( iTeam == TEAM_JINRAI ) ? TEAM_NSF : TEAM_JINRAI;
		*pEnemies = me->GetVisionInterface()->GetKnownCount( iEnemyTeam, true );

		int nMates = 0;
		for ( int i = 1; i <= gpGlobals->maxClients; ++i )
		{
			CNEO_Player *pPlayer = ToNEOPlayer( UTIL_PlayerByIndex( i ) );
			if ( !pPlayer || pPlayer == me || !pPlayer->IsAlive() || pPlayer->GetTeamNumber() != iTeam )
			{
				continue;
			}

			if ( me->IsRangeLessThan( pPlayer, MATE_RANGE ) )
			{
				++nMates;
			}
		}

		*pMates = nMates;
	}

	static ETrigger EvaluateTrigger( CNEOBot *me, CommitState_t &state )
	{
		const CKnownEntity *threat = me->GetVisionInterface()->GetPrimaryKnownThreat( true );
		if ( !threat || !threat->GetEntity() || !threat->IsVisibleRecently() )
		{
			return TRIGGER_NONE;
		}

		if ( IsTimeLeft( state.flAvengeUntil, AVENGE_WINDOW ) )
		{
			return TRIGGER_AVENGE;
		}

		if ( GetMode() < MODE_NUMBERS )
		{
			return TRIGGER_NONE;
		}

		int nMates;
		int nEnemies;
		CountLocalSides( me, &nMates, &nEnemies );
		if ( nEnemies > 0 && nMates >= nEnemies )
		{
			state.flNumbersUntil = gpGlobals->curtime + NUMBERS_HOLD;
			return TRIGGER_NUMBERS;
		}

		return IsTimeLeft( state.flNumbersUntil, NUMBERS_HOLD ) ? TRIGGER_NUMBERS : TRIGGER_NONE;
	}

	// Out of rounds or reloading: holding then only makes sense when the secondary can keep firing
	static bool IsLowOnRounds( CNEOBot *me )
	{
		CNEOBaseCombatWeapon *pWeapon = static_cast< CNEOBaseCombatWeapon * >( me->GetActiveWeapon() );
		if ( !pWeapon || !CNEOBot::IsRanged( pWeapon ) || !pWeapon->IsWeaponReloadable() )
		{
			return false;
		}

		return pWeapon->m_bInReload || pWeapon->Clip1() <= 1;
	}

	static bool CanFightOnSecondary( CNEOBot *me )
	{
		CBaseCombatWeapon *pPrimary = me->Weapon_GetSlot( 0 );
		CBaseCombatWeapon *pSecondary = me->Weapon_GetSlot( 1 );
		if ( !pPrimary || !pSecondary || me->GetActiveWeapon() != pPrimary )
		{
			return false;
		}

		return pPrimary->Clip1() <= 1 && pSecondary->Clip1() > 0;
	}

	static void LogHold( CNEOBot *me, CommitState_t &state, ERetreatSite site, ETrigger trigger, bool bLowOnRounds )
	{
		if ( !neo_bot_research_commit_log.GetBool() || state.bLogged[ site ] )
		{
			return;
		}

		state.bLogged[ site ] = true;

		int nMates;
		int nEnemies;
		CountLocalSides( me, &nMates, &nEnemies );

		const CKnownEntity *threat = me->GetVisionInterface()->GetPrimaryKnownThreat( true );
		const bool bKillerSeen = threat && threat->GetEntity() && state.iKiller > 0
			&& threat->GetEntity()->entindex() == state.iKiller;

		Msg( "NEO_COMMIT t=%.2f team=%d bot=%d cls=%d trigger=%s suppressed=%s low=%d killer_seen=%d mates=%d enemies=%d mode=%d\n",
			gpGlobals->curtime, me->GetTeamNumber(), me->entindex(), me->GetClass(), s_pszTriggerNames[ trigger ],
			s_pszSiteNames[ site ], bLowOnRounds ? 1 : 0, bKillerSeen ? 1 : 0, nMates, nEnemies, GetMode() );
	}

	void OnOtherKilled( CNEOBot *me, CBaseCombatCharacter *pVictimEntity, const CTakeDamageInfo &info )
	{
		if ( !IsArmed( me ) || !me->IsAlive() || !IsValidIndex( me->entindex() ) )
		{
			return;
		}

		CNEO_Player *pVictim = ToNEOPlayer( pVictimEntity );
		if ( !pVictim || pVictim == me || pVictim->GetTeamNumber() != me->GetTeamNumber() )
		{
			return;
		}

		const float flDist = me->GetAbsOrigin().DistTo( pVictim->GetAbsOrigin() );
		if ( !NoticedDeath( me, pVictim, flDist ) )
		{
			return;
		}

		CommitState_t &state = s_state[ me->entindex() ];
		state.flAvengeUntil = gpGlobals->curtime + AVENGE_WINDOW;

		CBaseEntity *pKiller = info.GetAttacker();
		state.iKiller = ( pKiller && pKiller->IsPlayer() ) ? pKiller->entindex() : 0;

		if ( neo_bot_research_commit_log.GetBool() )
		{
			Msg( "NEO_COMMIT_NOTICE t=%.2f team=%d bot=%d victim=%d killer=%d dist=%.0f\n",
				gpGlobals->curtime, me->GetTeamNumber(), me->entindex(), pVictim->entindex(), state.iKiller, flDist );
		}
	}

	bool HoldsGround( CNEOBot *me, ERetreatSite site )
	{
		if ( !IsArmed( me ) || !IsValidIndex( me->entindex() ) )
		{
			return false;
		}

		CommitState_t &state = s_state[ me->entindex() ];
		const ETrigger trigger = EvaluateTrigger( me, state );
		if ( trigger == TRIGGER_NONE )
		{
			return false;
		}

		if ( !IsTimeLeft( state.flLastCommitTime + EPISODE_GAP, EPISODE_GAP ) )
		{
			state.bEndedRetreat = false;
			V_memset( state.bLogged, 0, sizeof( state.bLogged ) );
		}

		state.flLastCommitTime = gpGlobals->curtime;

		// Ending one retreat per commitment is enough; a retreat other code starts again is left alone, so the two do not ping-pong
		if ( site == RETREAT_IN_PROGRESS && state.bEndedRetreat )
		{
			return false;
		}

		const bool bLowOnRounds = IsLowOnRounds( me );
		if ( bLowOnRounds && ( GetMode() < MODE_SECONDARY || !CanFightOnSecondary( me ) ) )
		{
			return false;
		}

		if ( site == RETREAT_IN_PROGRESS )
		{
			state.bEndedRetreat = true;
		}

		LogHold( me, state, site, trigger, bLowOnRounds );
		return true;
	}

	bool PrefersSecondaryWhenEmpty( CNEOBot *me )
	{
		if ( GetMode() < MODE_SECONDARY || !IsArmed( me ) || !IsValidIndex( me->entindex() ) )
		{
			return false;
		}

		return EvaluateTrigger( me, s_state[ me->entindex() ] ) != TRIGGER_NONE;
	}
}
