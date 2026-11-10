#include "cbase.h"

#include "neo_gamerules.h"
#include "neo_player.h"
#include "bot/neo_bot.h"
#include "bot/neo_bot_pacing.h"

ConVar neo_bot_research_pacing( "neo_bot_research_pacing", "0", FCVAR_CHEAT,
	"Research: before first contact in CTG, a bot ahead of every living teammate walks or holds until one catches up" );
ConVar neo_bot_research_pacing_lead( "neo_bot_research_pacing_lead", "500", FCVAR_CHEAT,
	"Research pacing: distance to the nearest living teammate beyond which a leading bot paces" );
ConVar neo_bot_research_pacing_max_wait( "neo_bot_research_pacing_max_wait", "8", FCVAR_CHEAT,
	"Research pacing: total seconds a bot may pace per round" );
ConVar neo_bot_research_pacing_log( "neo_bot_research_pacing_log", "0", FCVAR_CHEAT,
	"Research pacing: print NEO_PACING lines when a bot starts and stops pacing" );

extern ConVar neo_bot_research_team_arm;

// Walking closes the gap only against a slower teammate, so a bot beyond the lead holds still and walks once the
// nearest teammate is inside it; it runs again only well inside, so it does not flicker at the boundary
static const float NEO_BOT_PACE_RELEASE_MARGIN = 100.0f;
// The sneak press lasts a little past the next tick so the walk does not drop between updates
static const float NEO_BOT_PACE_SNEAK_PRESS = 0.1f;
// A long frame (pause, hitch) is not counted as waiting
static const float NEO_BOT_PACE_MAX_STEP = 0.25f;
static const float NEO_BOT_PACE_LOG_INTERVAL = 1.0f;

namespace
{

struct PaceState
{
	int iRound = -1;
	NeoBotPaceMode mode = NEO_BOT_PACE_NONE;
	float flWaited = 0.0f;
	float flLastUpdate = 0.0f;
	float flLastLog = -1000.0f;
};

struct TeamContactState
{
	int iRound = -1;
	int iCheckedTick = -1;
	bool bContact = false;
};

PaceState s_paceStates[MAX_PLAYERS + 1];
TeamContactState s_teamContact[TEAM_NSF + 1];

bool IsPacingArmed( const CNEOBot *me )
{
	if ( !neo_bot_research_pacing.GetBool() )
	{
		return false;
	}

	if ( NEORules()->GetGameType() != NEO_GAME_TYPE_CTG )
	{
		return false;
	}

	const int teamArm = neo_bot_research_team_arm.GetInt();
	return teamArm == 0 || me->GetTeamNumber() == teamArm;
}

// Contact is what the team could know: a teammate died (kill feed, scoreboard) or a teammate bot sees an enemy
// (a callout). It latches for the rest of the round
bool HasTeamMadeContact( int iTeam )
{
	if ( iTeam < TEAM_JINRAI || iTeam > TEAM_NSF )
	{
		return true;
	}

	TeamContactState &state = s_teamContact[iTeam];
	const int iRound = NEORules()->roundNumber();
	if ( state.iRound != iRound )
	{
		state.iRound = iRound;
		state.iCheckedTick = -1;
		state.bContact = false;
	}

	if ( state.bContact || state.iCheckedTick == gpGlobals->tickcount )
	{
		return state.bContact;
	}

	state.iCheckedTick = gpGlobals->tickcount;

	for ( int i = 1; i <= gpGlobals->maxClients; ++i )
	{
		CNEO_Player *pPlayer = ToNEOPlayer( UTIL_PlayerByIndex( i ) );
		if ( !pPlayer || pPlayer->GetTeamNumber() != iTeam )
		{
			continue;
		}

		if ( !pPlayer->IsAlive() )
		{
			state.bContact = true;
			break;
		}

		CNEOBot *pBot = ToNEOBot( pPlayer );
		if ( pBot && pBot->GetVisionInterface()->GetPrimaryKnownThreat( true ) )
		{
			state.bContact = true;
			break;
		}
	}

	return state.bContact;
}

// Distance to the nearest living teammate, and whether that teammate is behind the bot's direction of travel.
// Straight-line distance: cheap, and at the lead distances used it rarely differs much from travel distance
bool FindNearestTeammate( const CNEOBot *me, float &flDist, bool &bBehind )
{
	const Vector &myPos = me->GetAbsOrigin();
	const CNEO_Player *pNearest = nullptr;
	float flNearestSqr = FLT_MAX;

	for ( int i = 1; i <= gpGlobals->maxClients; ++i )
	{
		CNEO_Player *pPlayer = ToNEOPlayer( UTIL_PlayerByIndex( i ) );
		if ( !pPlayer || pPlayer == me || !pPlayer->IsAlive() || pPlayer->GetTeamNumber() != me->GetTeamNumber() )
		{
			continue;
		}

		const float flSqr = ( pPlayer->GetAbsOrigin() - myPos ).LengthSqr();
		if ( flSqr < flNearestSqr )
		{
			flNearestSqr = flSqr;
			pNearest = pPlayer;
		}
	}

	if ( !pNearest )
	{
		return false;
	}

	flDist = sqrtf( flNearestSqr );

	// The motion vector keeps its last direction while the bot holds still
	const Vector toMate = pNearest->GetAbsOrigin() - myPos;
	const Vector &motion = const_cast< CNEOBot * >( me )->GetLocomotionInterface()->GetMotionVector();
	bBehind = DotProduct2D( toMate.AsVector2D(), motion.AsVector2D() ) < 0.0f;
	return true;
}

NeoBotPaceMode ChoosePaceMode( CNEOBot *me, const PaceState &state, float &flLead )
{
	flLead = 0.0f;

	if ( me->IsCarryingGhost() )
	{
		return NEO_BOT_PACE_NONE;
	}

	if ( state.flWaited >= neo_bot_research_pacing_max_wait.GetFloat() )
	{
		return NEO_BOT_PACE_NONE;
	}

	// Holding still mid-jump or on a ladder would strand the bot
	ILocomotion *mover = me->GetLocomotionInterface();
	if ( !mover->IsOnGround() || mover->IsScrambling() || me->IsBotOnLadder() )
	{
		return NEO_BOT_PACE_NONE;
	}

	if ( HasTeamMadeContact( me->GetTeamNumber() ) )
	{
		return NEO_BOT_PACE_NONE;
	}

	bool bBehind = false;
	if ( !FindNearestTeammate( me, flLead, bBehind ) )
	{
		return NEO_BOT_PACE_NONE;
	}

	// The bot trails a teammate that is ahead: it should catch up, not wait
	if ( !bBehind )
	{
		return NEO_BOT_PACE_NONE;
	}

	const float flMaxLead = neo_bot_research_pacing_lead.GetFloat();
	if ( flLead > flMaxLead )
	{
		return NEO_BOT_PACE_HOLD;
	}

	const bool bWasPacing = ( state.mode != NEO_BOT_PACE_NONE );
	if ( bWasPacing && flLead > flMaxLead - NEO_BOT_PACE_RELEASE_MARGIN )
	{
		return NEO_BOT_PACE_WALK;
	}

	return NEO_BOT_PACE_NONE;
}

void LogPacing( const CNEOBot *me, PaceState &state, float flLead, const char *pszEvent )
{
	if ( !neo_bot_research_pacing_log.GetBool() )
	{
		return;
	}

	// A stop is always printed so the waited total is complete; starts are rate limited
	const bool bStart = ( V_strcmp( pszEvent, "start" ) == 0 );
	if ( bStart && gpGlobals->curtime - state.flLastLog < NEO_BOT_PACE_LOG_INTERVAL )
	{
		return;
	}

	state.flLastLog = gpGlobals->curtime;
	Msg( "NEO_PACING t=%.2f team=%d bot=%d class=%d lead=%.0f waited=%.1f event=%s\n",
		gpGlobals->curtime, me->GetTeamNumber(), me->entindex(), me->GetClass(), flLead, state.flWaited, pszEvent );
}

void ApplyPaceMode( CNEOBot *me, NeoBotPaceMode mode )
{
	if ( mode == NEO_BOT_PACE_NONE )
	{
		return;
	}

	me->ReleaseRunButton();

	if ( mode == NEO_BOT_PACE_HOLD )
	{
		me->GetLocomotionInterface()->Stop();
		return;
	}

	me->GetLocomotionInterface()->Walk();
	me->PressSneakButton( NEO_BOT_PACE_SNEAK_PRESS );
}

} // namespace

NeoBotPaceMode NeoBotUpdatePacing( CNEOBot *me )
{
	const int iIndex = me->entindex();
	if ( iIndex < 1 || iIndex > MAX_PLAYERS )
	{
		return NEO_BOT_PACE_NONE;
	}

	PaceState &state = s_paceStates[iIndex];
	const int iRound = NEORules()->roundNumber();
	if ( state.iRound != iRound || ( me->GetNeoFlags() & NEO_FL_FREEZETIME ) )
	{
		state = PaceState();
		state.iRound = iRound;
		state.flLastUpdate = gpGlobals->curtime;
	}

	const float flStep = Min( gpGlobals->curtime - state.flLastUpdate, NEO_BOT_PACE_MAX_STEP );
	state.flLastUpdate = gpGlobals->curtime;

	if ( !IsPacingArmed( me ) || ( me->GetNeoFlags() & NEO_FL_FREEZETIME ) )
	{
		state.mode = NEO_BOT_PACE_NONE;
		return NEO_BOT_PACE_NONE;
	}

	if ( state.mode != NEO_BOT_PACE_NONE )
	{
		state.flWaited += flStep;
	}

	float flLead = 0.0f;
	const NeoBotPaceMode mode = ChoosePaceMode( me, state, flLead );
	if ( mode != NEO_BOT_PACE_NONE && state.mode == NEO_BOT_PACE_NONE )
	{
		LogPacing( me, state, flLead, "start" );
	}
	else if ( mode == NEO_BOT_PACE_NONE && state.mode != NEO_BOT_PACE_NONE )
	{
		LogPacing( me, state, flLead, "stop" );
	}

	state.mode = mode;
	ApplyPaceMode( me, mode );
	return mode;
}

bool NeoBotIsPacingHold( const CNEOBot *me )
{
	const int iIndex = me->entindex();
	if ( iIndex < 1 || iIndex > MAX_PLAYERS )
	{
		return false;
	}

	return s_paceStates[iIndex].mode == NEO_BOT_PACE_HOLD;
}
