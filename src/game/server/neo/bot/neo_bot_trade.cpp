#include "cbase.h"
#include "bot/neo_bot.h"
#include "bot/neo_bot_trade.h"
#include "bot/behavior/neo_bot_trade_response.h"
#include "neo_gamerules.h"
#include "neo_player.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

ConVar neo_bot_research_trade( "neo_bot_research_trade", "0", FCVAR_CHEAT,
	"Research: when a teammate is killed, the nearest teammates that noticed it go after the killer for a few seconds." );
ConVar neo_bot_research_trade_log( "neo_bot_research_trade_log", "0", FCVAR_CHEAT,
	"Research: print NEO_TRADE lines for trade responses." );

extern ConVar neo_bot_research_team_arm;

namespace NEOBotTrade
{
	// Responders per death, so one kill does not pull the whole team onto one spot
	static constexpr int MAX_RESPONDERS_PER_DEATH = 2;

	// Responders of one team at once, across several deaths close together
	static constexpr int MAX_RESPONDERS_PER_TEAM = 3;

	// Only teammates this close to the death respond; farther ones keep their own job
	static constexpr float RESPONSE_RANGE = 1500.0f;

	// A death this close is heard through walls: the gunfire and the teammate's marker going out
	static constexpr float EARSHOT_RANGE = 800.0f;

	// The victim or a responder must have seen the killer this recently to know where the killer is
	static constexpr float KILLER_SEEN_WINDOW = 3.0f;

	// Time to notice a death and decide to answer it
	static constexpr float REACTION_DELAY = 0.5f;

	// An order not taken this soon is stale, e.g. the responder was busy fighting
	static constexpr float ORDER_LIFETIME = 1.5f;

	struct TradeOrder_t
	{
		bool bValid;
		int iRound;
		float flReadyTime;
		Vector vecGoal;
		Vector vecDeathSpot;
		bool bKillerSeen;
		int iVictim;
		int iKiller;
	};

	static TradeOrder_t s_orders[ MAX_PLAYERS + 1 ];
	static float s_flResponseEndTime[ MAX_PLAYERS + 1 ];

	static int s_iLastVictim = 0;
	static float s_flLastDeathTime = -1.0f;

	static bool IsTeamArmed( int iTeam )
	{
		if ( !neo_bot_research_trade.GetBool() )
		{
			return false;
		}

		const int iTeamArm = neo_bot_research_team_arm.GetInt();
		return iTeamArm == 0 || iTeamArm == iTeam;
	}

	bool IsLogging()
	{
		return neo_bot_research_trade_log.GetBool();
	}

	static bool IsValidIndex( int iIndex )
	{
		return iIndex > 0 && iIndex <= MAX_PLAYERS;
	}

	// Where this bot saw the killer, if it saw the killer recently; never the killer's live position
	static bool GetObservedKillerPos( CNEOBot *pObserver, CNEO_Player *pKiller, Vector *pPos, float *pAge )
	{
		if ( !pObserver )
		{
			return false;
		}

		const CKnownEntity *pKnown = pObserver->GetVisionInterface()->GetKnown( pKiller );
		if ( !pKnown || !pKnown->WasEverVisible() || pKnown->GetTimeSinceLastSeen() > KILLER_SEEN_WINDOW )
		{
			return false;
		}

		*pPos = pKnown->GetLastKnownPosition();
		*pAge = pKnown->GetTimeSinceLastKnown();
		return true;
	}

	// The end time is checked against the window too, as curtime restarts on a map change
	static bool IsResponding( int iIndex )
	{
		const float flLeft = s_flResponseEndTime[ iIndex ] - gpGlobals->curtime;
		return flLeft > 0.0f && flLeft <= RESPONSE_WINDOW;
	}

	static int CountActiveResponders( int iTeam )
	{
		int nActive = 0;
		for ( int i = 1; i <= gpGlobals->maxClients; ++i )
		{
			CNEO_Player *pPlayer = ToNEOPlayer( UTIL_PlayerByIndex( i ) );
			if ( pPlayer && pPlayer->IsAlive() && pPlayer->GetTeamNumber() == iTeam && IsResponding( i ) )
			{
				++nActive;
			}
		}

		return nActive;
	}

	// A teammate notices a death if it saw the killer, sees where the victim fell, or is within earshot
	static bool NoticedDeath( CNEOBot *pBot, CNEO_Player *pVictim, CNEO_Player *pKiller, float flDist )
	{
		if ( flDist <= EARSHOT_RANGE )
		{
			return true;
		}

		Vector vecUnused;
		float flUnused;
		if ( GetObservedKillerPos( pBot, pKiller, &vecUnused, &flUnused ) )
		{
			return true;
		}

		// The position overload is hidden by the bot vision class's entity overload
		const IVision *pVision = pBot->GetVisionInterface();
		return pVision->IsAbleToSee( pVictim->WorldSpaceCenter(), IVision::USE_FOV );
	}

	static void IssueOrder( CNEOBot *pBot, CNEO_Player *pVictim, CNEO_Player *pKiller, float flDist )
	{
		const Vector &vecDeathSpot = pVictim->GetAbsOrigin();

		// The victim's last words and the responder's own eyes; the fresher sighting wins
		Vector vecGoal = vecDeathSpot;
		bool bKillerSeen = false;
		float flBestAge = FLT_MAX;

		Vector vecSeen;
		float flAge;
		if ( GetObservedKillerPos( ToNEOBot( pVictim ), pKiller, &vecSeen, &flAge ) && flAge < flBestAge )
		{
			vecGoal = vecSeen;
			flBestAge = flAge;
			bKillerSeen = true;
		}

		if ( GetObservedKillerPos( pBot, pKiller, &vecSeen, &flAge ) && flAge < flBestAge )
		{
			vecGoal = vecSeen;
			flBestAge = flAge;
			bKillerSeen = true;
		}

		TradeOrder_t &order = s_orders[ pBot->entindex() ];
		order.bValid = true;
		order.iRound = NEORules()->roundNumber();
		order.flReadyTime = gpGlobals->curtime + REACTION_DELAY;
		order.vecGoal = vecGoal;
		order.vecDeathSpot = vecDeathSpot;
		order.bKillerSeen = bKillerSeen;
		order.iVictim = pVictim->entindex();
		order.iKiller = pKiller->entindex();

		if ( IsLogging() )
		{
			Msg( "NEO_TRADE t=%.2f team=%d responder=%d victim=%d killer_seen=%d dist=%.0f killer=%d goal=%.0f,%.0f,%.0f\n",
				gpGlobals->curtime, pBot->GetTeamNumber(), pBot->entindex(), pVictim->entindex(), bKillerSeen ? 1 : 0,
				flDist, pKiller->entindex(), vecGoal.x, vecGoal.y, vecGoal.z );
		}
	}

	void OnPlayerKilled( CBaseCombatCharacter *pVictimEntity, const CTakeDamageInfo &info )
	{
		if ( !neo_bot_research_trade.GetBool() || !NEORules()->IsTeamplay() )
		{
			return;
		}

		CNEO_Player *pVictim = ToNEOPlayer( pVictimEntity );
		if ( !pVictim )
		{
			return;
		}

		if ( pVictim->entindex() == s_iLastVictim && gpGlobals->curtime == s_flLastDeathTime )
		{
			return;
		}

		s_iLastVictim = pVictim->entindex();
		s_flLastDeathTime = gpGlobals->curtime;

		const int iTeam = pVictim->GetTeamNumber();
		if ( !IsTeamArmed( iTeam ) )
		{
			return;
		}

		// Team kills, suicides and world damage leave nobody to trade
		CNEO_Player *pKiller = ToNEOPlayer( info.GetAttacker() );
		if ( !pKiller || !pKiller->IsAlive() || pKiller->GetTeamNumber() == iTeam )
		{
			return;
		}

		const int nSlots = MAX_RESPONDERS_PER_TEAM - CountActiveResponders( iTeam );
		const int nWanted = MIN( MAX_RESPONDERS_PER_DEATH, nSlots );
		if ( nWanted <= 0 )
		{
			return;
		}

		CNEOBot *pChosen[ MAX_RESPONDERS_PER_DEATH ] = {};
		float flChosenDist[ MAX_RESPONDERS_PER_DEATH ];
		int nChosen = 0;

		const Vector &vecDeathSpot = pVictim->GetAbsOrigin();
		for ( int i = 1; i <= gpGlobals->maxClients; ++i )
		{
			CNEOBot *pBot = ToNEOBot( UTIL_PlayerByIndex( i ) );
			if ( !pBot || pBot == pVictim || !pBot->IsAlive() || pBot->GetTeamNumber() != iTeam )
			{
				continue;
			}

			// The carrier's job is the ghost
			if ( pBot->IsCarryingGhost() || IsResponding( i ) )
			{
				continue;
			}

			const float flDist = pBot->GetAbsOrigin().DistTo( vecDeathSpot );
			if ( flDist > RESPONSE_RANGE || !NoticedDeath( pBot, pVictim, pKiller, flDist ) )
			{
				continue;
			}

			// Insert sorted by distance, keeping the nearest nWanted
			int iSlot = nChosen;
			while ( iSlot > 0 && flChosenDist[ iSlot - 1 ] > flDist )
			{
				if ( iSlot < nWanted )
				{
					pChosen[ iSlot ] = pChosen[ iSlot - 1 ];
					flChosenDist[ iSlot ] = flChosenDist[ iSlot - 1 ];
				}

				--iSlot;
			}

			if ( iSlot < nWanted )
			{
				pChosen[ iSlot ] = pBot;
				flChosenDist[ iSlot ] = flDist;
				nChosen = MIN( nChosen + 1, nWanted );
			}
		}

		for ( int i = 0; i < nChosen; ++i )
		{
			IssueOrder( pChosen[ i ], pVictim, pKiller, flChosenDist[ i ] );
		}
	}

	CNEOBotTradeResponse *TakeResponse( CNEOBot *me )
	{
		const int iIndex = me->entindex();
		if ( !IsValidIndex( iIndex ) || !s_orders[ iIndex ].bValid )
		{
			return nullptr;
		}

		TradeOrder_t &order = s_orders[ iIndex ];
		const float flNow = gpGlobals->curtime;
		if ( flNow < order.flReadyTime )
		{
			return nullptr;
		}

		order.bValid = false;

		const bool bStale = ( flNow > order.flReadyTime + ORDER_LIFETIME ) || ( order.iRound != NEORules()->roundNumber() );
		if ( bStale || !IsTeamArmed( me->GetTeamNumber() ) || me->IsCarryingGhost() )
		{
			return nullptr;
		}

		return new CNEOBotTradeResponse( order.vecGoal, order.vecDeathSpot, order.bKillerSeen, order.iVictim );
	}

	void OnResponseStart( const CNEOBot *me, float flEndTime )
	{
		const int iIndex = me->entindex();
		if ( IsValidIndex( iIndex ) )
		{
			s_flResponseEndTime[ iIndex ] = flEndTime;
		}
	}

	void OnResponseEnd( const CNEOBot *me )
	{
		const int iIndex = me->entindex();
		if ( IsValidIndex( iIndex ) )
		{
			s_flResponseEndTime[ iIndex ] = 0.0f;
		}
	}
}
