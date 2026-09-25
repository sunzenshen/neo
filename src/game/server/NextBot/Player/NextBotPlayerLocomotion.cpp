// NextBotPlayerLocomotion.cpp
// Implementation of Locomotion interface for CBasePlayer-derived classes
// Author: Michael Booth, November 2005
//========= Copyright Valve Corporation, All rights reserved. ============//

#include "cbase.h"
#include "nav_mesh.h"
#include "in_buttons.h"
#include "NextBot.h"
#include "NextBotUtil.h"
#include "NextBotPlayer.h"
#include "NextBotPlayerLocomotion.h"
#include "bot/neo_bot.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

ConVar NextBotPlayerMoveDirect( "nb_player_move_direct", "0" );
#ifdef NEO
// NEO-HARNESS-TEMP research arm (2026-09-25, patch 109): on a PRECISE / CLIFF area (a fall edge) move in the exact direction
// to the goal instead of the nearest of eight button combinations - up to 22.5 deg of drift, which walks a bot that aims
// elsewhere off a 20-32 u strip (mvd1: nb_player_move_direct everywhere cut falls x0.91 but cost stuck x1.17)
ConVar neo_bot_precise_move_direct( "neo_bot_precise_move_direct", "0", FCVAR_CHEAT,
	"Research: on a NAV_MESH_PRECISE or NAV_MESH_CLIFF area a bot's move input follows the exact direction to its goal" );
// NEO-HARNESS-TEMP research arm (2026-09-23, patch 58): tuck (hold crouch) once a climb's jump has left the
// ground, so the bot reaches the crouch-jump height the path costs assume (CNEOBotLocomotion::GetMaxJumpHeight)
ConVar neo_bot_climb_crouch_jump( "neo_bot_climb_crouch_jump", "0", FCVAR_CHEAT,
	"Research: hold crouch while airborne on a ledge climb or gap jump (a crouch jump)" );
// NEO-HARNESS-TEMP research arm (2026-09-24, patch 59): a ledge climb leaves the ground with whatever horizontal
// velocity the bot had - at a launch point against the wall, almost none - and air control alone rarely carries it
// the half hull onto the ledge (NEO_FORENSIC_CLIMB: tarmac's basin 27 % of climbs landed). Gap jumps already set
// the run speed towards the landing at take-off; do the same for climbs.
ConVar neo_bot_climb_push( "neo_bot_climb_push", "0", FCVAR_CHEAT,
	"Research: at a ledge climb's take-off, set horizontal velocity towards the landing at run speed, as gap jumps do (2 = only for climbs over 40 u)" );
extern ConVar sv_neo_forensic_log;
#endif

//-----------------------------------------------------------------------------------------------------
PlayerLocomotion::PlayerLocomotion( INextBot *bot ) : ILocomotion( bot )
{
	m_player = NULL;
	Reset();
}


//-----------------------------------------------------------------------------------------------------
/**
 * Reset locomotor to initial state
 */
void PlayerLocomotion::Reset( void )
{
	m_player = static_cast< CBasePlayer * >( GetBot()->GetEntity() );
	
	m_isJumping = false;
	m_isClimbingUpToLedge = false;
	m_isJumpingAcrossGap = false;
	m_hasLeftTheGround = false;
	m_desiredSpeed = 0.0f;


	m_ladderState = NO_LADDER;
	m_ladderInfo = NULL;
	m_ladderDismountGoal = NULL;
	m_ladderTimer.Invalidate();

	m_unwantedLadderSince = 0.0f;
	m_unwantedLadderLastTouch = 0.0f;

	m_minSpeedLimit = 0.0f;
	m_maxSpeedLimit = 9999999.9f;

	BaseClass::Reset();
}


// NEO-HARNESS-TEMP: forensic instrumentation only (see harness/patches/README.md). Emits one
// NEO_FORENSIC_LADDER line per ladder-state transition, so an offline tool can tell an arrested
// climb (still ASCENDING_LADDER) from a bot that slipped off and is re-approaching. The position
// log alone cannot separate those. Never part of a PR.
extern ConVar sv_neo_forensic_log;

// NEO: what to do when the engine has put a bot on a ladder its own path never asked for.
// `CGameMovement::LadderMove()` grabs any ladder the player's wish direction points at, while
// `TraverseLadder()` drops the move type straight back to walking - so a path that merely brushes
// past a ladder leaves the two fighting every tick, and the bot hangs there making no progress
// and firing no stuck event. Measured on ntre_subsurface_ctg 2026-09-20.
// Collapsed 2026-09-20 to the one strategy that won: adopt the ladder by its nearer end once the
// contact has persisted. The six research modes this replaces (push off, plain adopt, slide, hold,
// adopt-upward) are in this branch's history and in notes/ladder-reachability-2026-09-20.md; they
// are not shipped, and there is deliberately no convar to switch the fix off - see the commit.

// How long a bot must be held against a ladder it did not ask for before taking it over. Short
// enough that a real snag is caught within half a second, long enough that the ordinary release
// still handles the many contacts that clear themselves immediately.
static const float LADDER_ADOPT_TIME = 0.5f;
// A ladder further than this from the bot is not the one it is stuck on.
static const float LADDER_TOUCH_RANGE = 64.0f;
// A gap longer than this means the bot walked away and came back, so the bout starts over.
static const float LADDER_CONTACT_RESET = 1.0f;

// Indexed by PlayerLocomotion::LadderState, which is private - hence the array rather than a
// switch over the enumerators.
static const char *s_neoHarnessLadderStateNames[] =
{
	"none", "approach_up", "approach_down", "ascend", "descend", "dismount_top", "dismount_bottom"
};

static const char *NEOHarnessLadderStateName( int state )
{
	if ( state < 0 || state >= ARRAYSIZE( s_neoHarnessLadderStateNames ) )
	{
		return "?";
	}

	return s_neoHarnessLadderStateNames[state];
}

//-----------------------------------------------------------------------------------------------------
/**
 * NEO: the nav ladder the bot is standing in, or NULL. The engine knows which brush it grabbed but
 * keeps that normal private, so match against the nav record instead - close in xy, and inside the
 * ladder's own height span.
 */
const CNavLadder *PlayerLocomotion::FindTouchedLadder( void ) const
{
	const Vector &feet = GetFeet();
	const CNavLadder *best = NULL;
	float bestRangeSq = LADDER_TOUCH_RANGE * LADDER_TOUCH_RANGE;

	// The hull reaches a standing height above the feet, and the grab only needs the hull to touch
	// the brush - so a bot standing on the floor well *below* a ladder whose foot hangs over a
	// walkway is exactly the case to catch here, not one to filter out.
	const float below = GetBot()->GetBodyInterface()->GetStandHullHeight();

	for ( int i = 0; i < TheNavMesh->GetLadders().Count(); ++i )
	{
		const CNavLadder *ladder = TheNavMesh->GetLadders()[i];

		if ( feet.z < ladder->m_bottom.z - below || feet.z > ladder->m_top.z + GetStepHeight() )
		{
			continue;
		}

		const float rangeSq = ( ladder->GetPosAtHeight( feet.z ) - feet ).AsVector2D().LengthSqr();
		if ( rangeSq < bestRangeSq )
		{
			bestRangeSq = rangeSq;
			best = ladder;
		}
	}

	return best;
}


//-----------------------------------------------------------------------------------------------------
/**
 * NEO: called when the engine has us on a ladder our own path never asked for. Returns true if the
 * ladder was taken over, in which case the caller leaves the move type alone and the normal ladder
 * state machine drives from here.
 */
bool PlayerLocomotion::HandleUnwantedLadder( void )
{
	const CNavLadder *ladder = FindTouchedLadder();
	if ( ladder == NULL )
	{
		return false;
	}

	// Most grabs sort themselves out within a second, and adopting every one of them buys a lot of
	// climbing nobody asked for. Only take the ladder over once the bot has actually been held
	// against it.
	const float now = gpGlobals->curtime;

	if ( now - m_unwantedLadderLastTouch > LADDER_CONTACT_RESET )
	{
		m_unwantedLadderSince = now;
	}

	m_unwantedLadderLastTouch = now;

	if ( now - m_unwantedLadderSince < LADDER_ADOPT_TIME )
	{
		return false;
	}

	// Leave by the nearer end rather than argue about being here at all. Whichever end the bot is
	// closer to is the one it can reach soonest, and the dismount goal has to be a real area or
	// DismountLadderTop/Bottom has nothing to walk to.
	//
	// Climbing out upward only was measured and is worse: it sends the bot somewhere it then has
	// to come back from, and costs captures even though it reads better on the ladder numbers.
	const bool bGoUp = ( GetFeet().z - ladder->m_bottom.z ) > ( ladder->m_top.z - GetFeet().z );

	// A ladder's top is recorded in whichever of the three slots the generator filled, and plenty
	// of them leave m_topForwardArea empty - reading only that slot silently skips those ladders.
	const CNavArea *dismount = ladder->m_bottomArea;

	if ( bGoUp )
	{
		dismount = ladder->m_topForwardArea ? ladder->m_topForwardArea :
			( ladder->m_topLeftArea ? ladder->m_topLeftArea : ladder->m_topRightArea );
	}

	if ( dismount == NULL )
	{
		return false;
	}

	m_ladderInfo = ladder;
	m_ladderDismountGoal = dismount;
	m_ladderState = bGoUp ? ASCENDING_LADDER : DESCENDING_LADDER;

	return true;
}


//-----------------------------------------------------------------------------------------------------
bool PlayerLocomotion::TraverseLadder( void )
{
	const LadderState stateBefore = m_ladderState;
	const CNavLadder *pLadderBefore = m_ladderInfo;

	bool bTraversing = true;

	switch( m_ladderState )
	{
	case APPROACHING_ASCENDING_LADDER:
		m_ladderState = ApproachAscendingLadder();
		break;

	case APPROACHING_DESCENDING_LADDER:
		m_ladderState = ApproachDescendingLadder();
		break;

	case ASCENDING_LADDER:
		m_ladderState = AscendLadder();
		break;

	case DESCENDING_LADDER:
		m_ladderState = DescendLadder();
		break;

	case DISMOUNTING_LADDER_TOP:
		m_ladderState = DismountLadderTop();
		break;

	case DISMOUNTING_LADDER_BOTTOM:
		m_ladderState = DismountLadderBottom();
		break;

	case NO_LADDER:
	default:
		m_ladderInfo = NULL;

		if ( GetBot()->GetEntity()->GetMoveType() == MOVETYPE_LADDER )
		{
			// On a ladder and don't want to be. HandleUnwantedLadder() may take the ladder over
			// instead, in which case the state machine drives from here and we are done.
			if ( HandleUnwantedLadder() )
			{
				bTraversing = true;
				break;
			}

			GetBot()->GetEntity()->SetMoveType( MOVETYPE_WALK );
		}

		bTraversing = false;
		break;
	}

	// NEO-HARNESS-TEMP
	if ( sv_neo_forensic_log.GetBool() && m_ladderState != stateBefore )
	{
		const CNavLadder *pLadder = m_ladderInfo ? m_ladderInfo : pLadderBefore;
		const Vector &vecFeet = GetFeet();

		const Vector &vecVel = GetBot()->GetEntity()->GetAbsVelocity();
		const QAngle angEye = m_player->EyeAngles();
		Msg( "NEO_FORENSIC_LADDER t=%.2f p=%d from=%s to=%s ladder=%d pos=%.0f,%.0f,%.0f "
			"ladderbottom=%.0f laddertop=%.0f movetype=%d onground=%d vel=%.0f,%.0f,%.0f ang=%.0f,%.0f btn=%d\n",
			gpGlobals->curtime, GetBot()->GetEntity()->entindex(),
			NEOHarnessLadderStateName( stateBefore ), NEOHarnessLadderStateName( m_ladderState ),
			pLadder ? pLadder->GetID() : -1,
			vecFeet.x, vecFeet.y, vecFeet.z,
			pLadder ? pLadder->m_bottom.z : 0.0f, pLadder ? pLadder->m_top.z : 0.0f,
			(int)GetBot()->GetEntity()->GetMoveType(),
			( GetBot()->GetEntity()->GetGroundEntity() != NULL ) ? 1 : 0,
			vecVel.x, vecVel.y, vecVel.z, angEye.x, angEye.y, m_player->m_nButtons );
	}

	return bTraversing;

	return true;
}


#ifdef NEO
extern ConVar neo_bot_ladder_claim;	// NEO-HARNESS-TEMP research arm, patch 71 (neo_bot_ladder_climb.cpp)

// NEO-HARNESS-TEMP research arm (2026-09-25, patch 95): the approach states below have no way out but the ladder, so a
// bot driven at a ladder it cannot grab from where it stands stays there for good (transit ladder 2's top: 30-70 s
// against the ladder_256 prop, IN_BACK held; lc1 subsurface 1's foot). Give up after this long; 0 = never (stock).
ConVar neo_bot_ladder_approach_timeout( "neo_bot_ladder_approach_timeout", "0", FCVAR_CHEAT,
	"Research: seconds the locomotion's ladder approach may take before it gives up (0 = no limit)" );

// NEO-HARNESS-TEMP research arm (2026-09-25, patch 98): with the ladder claim (patch 71) this state machine descends
// alongside CNEOBotLadderClimb, and the two steer against each other - the climb faces the ladder and presses forward
// while DescendLadder() below approaches a point out along the face normal at a huge weight, which, facing the ladder,
// is IN_BACK. On a ladder whose top is level with the landing that is a grounded player moving away from the face, and
// LadderMove pushes it off (sentinel 3 / 6: 52-72 % of descents started at the top end there). Leave the steering to the
// climb behaviour while it holds the ladder.
ConVar neo_bot_ladder_descend_handsoff( "neo_bot_ladder_descend_handsoff", "0", FCVAR_CHEAT,
	"Research: while the ladder behaviour holds a claimed ladder, the locomotion's descent only tracks state (no aim, no approach)" );

static bool NeoLadderApproachTimedOut( const CountdownTimer &timer )
{
	return neo_bot_ladder_approach_timeout.GetFloat() > 0.0f && timer.HasStarted() && timer.IsElapsed();
}

// ClimbLadder / DescendLadder set the approach state outside TraverseLadder, so NEO_FORENSIC_LADDER never showed who
// started an approach (why=call; movetype 2 = still walking, 9 = already on the ladder)
static void NeoStartLadderApproach( CountdownTimer &timer, INextBot *bot, int stateBefore, const CNavLadder *ladder, bool bUp )
{
	if ( neo_bot_ladder_approach_timeout.GetFloat() > 0.0f )
	{
		timer.Start( neo_bot_ladder_approach_timeout.GetFloat() );
	}

	if ( !sv_neo_forensic_log.GetBool() || !ladder )
	{
		return;
	}

	const Vector &vecFeet = bot->GetLocomotionInterface()->GetFeet();
	Msg( "NEO_FORENSIC_LADDER t=%.2f p=%d from=%s to=%s ladder=%d pos=%.0f,%.0f,%.0f ladderbottom=%.0f laddertop=%.0f movetype=%d why=call\n",
		gpGlobals->curtime, bot->GetEntity()->entindex(), NEOHarnessLadderStateName( stateBefore ), bUp ? "approach_up" : "approach_down",
		ladder->GetID(), vecFeet.x, vecFeet.y, vecFeet.z, ladder->m_bottom.z, ladder->m_top.z, (int)bot->GetEntity()->GetMoveType() );
}
#endif

//-----------------------------------------------------------------------------------------------------
/**
 * We're close, but not yet on, this ladder - approach it
 */
PlayerLocomotion::LadderState PlayerLocomotion::ApproachAscendingLadder( void )
{
	if ( m_ladderInfo == NULL )
	{
		return NO_LADDER;
	}

#ifdef NEO
	// patch 95
	if ( NeoLadderApproachTimedOut( m_ladderTimer ) && GetBot()->GetEntity()->GetMoveType() != MOVETYPE_LADDER )
	{
		m_ladderInfo = NULL;
		return NO_LADDER;
	}
#endif

	// sanity check - are we already at the end of this ladder?
	if ( GetFeet().z >= m_ladderInfo->m_top.z - GetStepHeight() )
	{
		m_ladderTimer.Start( 2.0f );
		return DISMOUNTING_LADDER_TOP;
	}

#ifdef NEO
	// NEO-HARNESS-TEMP research arm (2026-09-24, patch 71 v3): a bot the ladder behaviour has already put on the ladder
	// is not "too far below" it, whatever the nav ladder's bottom says - on a ladder whose foot hangs above the floor
	// (subsurface ladder 1: 55 u) the check below refused 29 of 96 claims (lc6), and the unwanted-ladder takeover then
	// adopted the ladder by its nearer end and drove the bot down
	if ( neo_bot_ladder_claim.GetBool() && GetBot()->GetEntity()->GetMoveType() == MOVETYPE_LADDER )
	{
		return ASCENDING_LADDER;
	}
#endif

	// sanity check - are we too far below this ladder to reach it?
	if ( GetFeet().z <= m_ladderInfo->m_bottom.z - GetMaxJumpHeight() )
	{
		return NO_LADDER;
	}

	FaceTowards( m_ladderInfo->m_bottom );

	// it is important to approach precisely, so use a very large weight to wash out all other Approaches
	Approach( m_ladderInfo->m_bottom, 9999999.9f );

	if ( GetBot()->GetEntity()->GetMoveType() == MOVETYPE_LADDER )
	{
		// we're on the ladder
		return ASCENDING_LADDER;
	}

	if ( GetBot()->IsDebugging( NEXTBOT_LOCOMOTION ) )
	{
		NDebugOverlay::EntityText( GetBot()->GetEntity()->entindex(), 0, "Approach ascending ladder", 0.1f, 255, 255, 255, 255 );
	}

	return APPROACHING_ASCENDING_LADDER;
}


//-----------------------------------------------------------------------------------------------------
PlayerLocomotion::LadderState PlayerLocomotion::ApproachDescendingLadder( void )
{
	if ( m_ladderInfo == NULL )
	{
		return NO_LADDER;
	}

#ifdef NEO
	// patch 95
	if ( NeoLadderApproachTimedOut( m_ladderTimer ) && GetBot()->GetEntity()->GetMoveType() != MOVETYPE_LADDER )
	{
		m_ladderInfo = NULL;
		return NO_LADDER;
	}
#endif

	// sanity check - are we already at the end of this ladder?
	if ( GetFeet().z <= m_ladderInfo->m_bottom.z + GetMaxJumpHeight() )
	{
		m_ladderTimer.Start( 2.0f );
		return DISMOUNTING_LADDER_BOTTOM;
	}

	Vector mountPoint = m_ladderInfo->m_top + 0.25f * GetBot()->GetBodyInterface()->GetHullWidth() * m_ladderInfo->GetNormal();
	Vector to = mountPoint - GetFeet();
	to.z = 0.0f;

	float mountRange = to.NormalizeInPlace();
	Vector moveGoal;

	const float veryClose = 10.0f;
	if ( mountRange < veryClose )
	{
		// we're right at the ladder - just keep moving forward until we grab it
		const Vector &forward = GetMotionVector();
		moveGoal = GetFeet() + 100.0f * forward;
	}
	else
	{
		if ( DotProduct( to, m_ladderInfo->GetNormal() ) < 0.0f )
		{
			// approaching front of downward ladder
			//     ##
			// ->+ ##
			//   | ##
			//   | ##
			//   | ##
			// <-+ ##
			// ######
			//
			moveGoal = m_ladderInfo->m_top - 100.0f * m_ladderInfo->GetNormal();
		}
		else
		{
			// approaching back of downward ladder
			//
			// ->+
			// ##|
			// ##|
			// ##+-->
			// ######
			//
			moveGoal = m_ladderInfo->m_top + 100.0f * m_ladderInfo->GetNormal();
		}
	}

	FaceTowards( moveGoal );

	// it is important to approach precisely, so use a very large weight to wash out all other Approaches
	Approach( moveGoal, 9999999.9f );

	if ( GetBot()->GetEntity()->GetMoveType() == MOVETYPE_LADDER )
	{
		// we're on the ladder
		return DESCENDING_LADDER;
	}

	if ( GetBot()->IsDebugging( NEXTBOT_LOCOMOTION ) )
	{
		NDebugOverlay::EntityText( GetBot()->GetEntity()->entindex(), 0, "Approach descending ladder", 0.1f, 255, 255, 255, 255 );
	}

	return APPROACHING_DESCENDING_LADDER;
}


//-----------------------------------------------------------------------------------------------------
PlayerLocomotion::LadderState PlayerLocomotion::AscendLadder( void )
{
	if ( m_ladderInfo == NULL )
	{
		return NO_LADDER;
	}

	if ( GetBot()->GetEntity()->GetMoveType() != MOVETYPE_LADDER )
	{
		// slipped off ladder
		m_ladderInfo = NULL;
		return NO_LADDER;
	}

	if ( GetFeet().z >= m_ladderInfo->m_top.z )
	{
		// reached top of ladder
		m_ladderTimer.Start( 2.0f );
		return DISMOUNTING_LADDER_TOP;
	}

	// climb up this ladder - look up
	Vector goal = GetFeet() + 100.0f * ( -m_ladderInfo->GetNormal() + Vector( 0, 0, 2 ) );

	GetBot()->GetBodyInterface()->AimHeadTowards( goal, IBody::MANDATORY, 0.1f, NULL, "Ladder" );

	// it is important to approach precisely, so use a very large weight to wash out all other Approaches
	Approach( goal, 9999999.9f );

	if ( GetBot()->IsDebugging( NEXTBOT_LOCOMOTION ) )
	{
		NDebugOverlay::EntityText( GetBot()->GetEntity()->entindex(), 0, "Ascend", 0.1f, 255, 255, 255, 255 );
	}

	return ASCENDING_LADDER;
}


//-----------------------------------------------------------------------------------------------------
PlayerLocomotion::LadderState PlayerLocomotion::DescendLadder( void )
{
	if ( m_ladderInfo == NULL )
	{
		return NO_LADDER;
	}

	if ( GetBot()->GetEntity()->GetMoveType() != MOVETYPE_LADDER )
	{
		// slipped off ladder
		m_ladderInfo = NULL;
		return NO_LADDER;
	}

	if ( GetFeet().z <= m_ladderInfo->m_bottom.z + GetBot()->GetLocomotionInterface()->GetStepHeight() )
	{
		// reached bottom of ladder
		m_ladderTimer.Start( 2.0f );
		return DISMOUNTING_LADDER_BOTTOM;
	}

#ifdef NEO
	// patch 98
	if ( neo_bot_ladder_descend_handsoff.GetBool() && neo_bot_ladder_claim.GetBool() )
	{
		return DESCENDING_LADDER;
	}
#endif

	// climb down this ladder - look down 
	Vector goal = GetFeet() + 100.0f * ( m_ladderInfo->GetNormal() + Vector( 0, 0, -2 ) );

	GetBot()->GetBodyInterface()->AimHeadTowards( goal, IBody::MANDATORY, 0.1f, NULL, "Ladder" );

	// it is important to approach precisely, so use a very large weight to wash out all other Approaches
	Approach( goal, 9999999.9f );

	if ( GetBot()->IsDebugging( NEXTBOT_LOCOMOTION ) )
	{
		NDebugOverlay::EntityText( GetBot()->GetEntity()->entindex(), 0, "Descend", 0.1f, 255, 255, 255, 255 );
	}

	return DESCENDING_LADDER;
}


//-----------------------------------------------------------------------------------------------------
PlayerLocomotion::LadderState PlayerLocomotion::DismountLadderTop( void )
{
	if ( m_ladderInfo == NULL || m_ladderTimer.IsElapsed() )
	{
		m_ladderInfo = NULL;
		return NO_LADDER;
	}

	IBody *body = GetBot()->GetBodyInterface();
	Vector toGoal = m_ladderDismountGoal->GetCenter() - GetFeet();
	toGoal.z = 0.0f;
	float range = toGoal.NormalizeInPlace();
	toGoal.z = 1.0f;
	
	body->AimHeadTowards( body->GetEyePosition() + 100.0f * toGoal, IBody::MANDATORY, 0.1f, NULL, "Ladder dismount" );

	// it is important to approach precisely, so use a very large weight to wash out all other Approaches
	Approach( GetFeet() + 100.0f * toGoal, 9999999.9f );

	if ( GetBot()->IsDebugging( NEXTBOT_LOCOMOTION ) )
	{
		NDebugOverlay::EntityText( GetBot()->GetEntity()->entindex(), 0, "Dismount top", 0.1f, 255, 255, 255, 255 );
		NDebugOverlay::HorzArrow( GetFeet(), m_ladderDismountGoal->GetCenter(), 5.0f, 255, 255, 0, 255, true, 0.1f );
	}

	// test 2D vector here in case nav area is under the geometry a bit
	const float tolerance = 10.0f;
	if ( GetBot()->GetEntity()->GetLastKnownArea() == m_ladderDismountGoal && range < tolerance )
	{
		// reached dismount goal
		m_ladderInfo = NULL;
		return NO_LADDER;
	}

	return DISMOUNTING_LADDER_TOP;
}


//-----------------------------------------------------------------------------------------------------
PlayerLocomotion::LadderState PlayerLocomotion::DismountLadderBottom( void )
{
	if ( m_ladderInfo == NULL || m_ladderTimer.IsElapsed() )
	{
		m_ladderInfo = NULL;
		return NO_LADDER;
	}

	if ( GetBot()->GetEntity()->GetMoveType() == MOVETYPE_LADDER )
	{
		// near the bottom - just let go
		GetBot()->GetEntity()->SetMoveType( MOVETYPE_WALK );
		m_ladderInfo = NULL;
	}

	return NO_LADDER;
}


//-----------------------------------------------------------------------------------------------------
/**
 * Update internal state
 */
void PlayerLocomotion::Update( void )
{
	if ( TraverseLadder() )
	{
		return BaseClass::Update();
	}

	if ( m_isJumpingAcrossGap || m_isClimbingUpToLedge )
	{
		// force a run
		SetMinimumSpeedLimit( GetRunSpeed() );

		Vector toLanding = m_landingGoal - GetFeet();
		toLanding.z = 0.0f;
		toLanding.NormalizeInPlace();

		if ( m_hasLeftTheGround )
		{
			// face into the jump/climb
			GetBot()->GetBodyInterface()->AimHeadTowards( GetBot()->GetEntity()->EyePosition() + 100.0 * toLanding, IBody::MANDATORY, 0.25f, NULL, "Facing impending jump/climb" );

#ifdef NEO
			if ( neo_bot_climb_crouch_jump.GetBool() && !IsOnGround() )
			{
				INextBotPlayerInput *tuck = dynamic_cast< INextBotPlayerInput * >( GetBot() );
				if ( tuck )
				{
					tuck->PressCrouchButton( 0.2f );
				}
			}
			if ( IsOnGround() && sv_neo_forensic_log.GetBool() )
			{
				// NEO-HARNESS-TEMP (patch 58): did the climb / gap jump land where it meant to?
				Msg( "NEO_FORENSIC_CLIMB t=%.2f p=%d kind=%s pos=%.0f,%.0f,%.0f goal=%.0f,%.0f,%.0f ok=%d\n", gpGlobals->curtime,
					 GetBot()->GetEntity()->entindex(), m_isClimbingUpToLedge ? "climb" : "gap",
					 GetFeet().x, GetFeet().y, GetFeet().z, m_landingGoal.x, m_landingGoal.y, m_landingGoal.z,
					 GetFeet().z > m_landingGoal.z - GetStepHeight() ? 1 : 0 );
			}
#endif
			if ( IsOnGround() )
			{
				// back on the ground - jump is complete
				m_isClimbingUpToLedge = false;
				m_isJumpingAcrossGap = false;
				SetMinimumSpeedLimit( 0.0f );
			}
		}
		else
		{
			// haven't left the ground yet - just starting the jump

			if ( !IsClimbingOrJumping() )
			{
				Jump();
			}

			Vector vel = GetBot()->GetEntity()->GetAbsVelocity();

			if ( m_isJumpingAcrossGap
#ifdef NEO
				 || ( m_isClimbingUpToLedge && neo_bot_climb_push.GetBool() &&
					  ( neo_bot_climb_push.GetInt() < 2 || m_landingGoal.z - GetFeet().z > 40.0f ) )
#endif
				)
			{
				// cheat and max our velocity in case we were stopped at the edge of this gap
				vel.x = GetRunSpeed() * toLanding.x;
				vel.y = GetRunSpeed() * toLanding.y;
				// leave vel.z unchanged
			}

			GetBot()->GetEntity()->SetAbsVelocity( vel );

			if ( !IsOnGround() )
			{
				// jump has begun
				m_hasLeftTheGround = true;
			}
		}

		
		Approach( m_landingGoal );
	}

	BaseClass::Update();
}


//-----------------------------------------------------------------------------------------------------
void PlayerLocomotion::AdjustPosture( const Vector &moveGoal )
{
	// This function has no effect if we're not standing or crouching
	IBody *body = GetBot()->GetBodyInterface();
	if ( !body->IsActualPosture( IBody::STAND ) && !body->IsActualPosture( IBody::CROUCH ) )
		return;

	// not all games have auto-crouch, so don't assume it here
	BaseClass::AdjustPosture( moveGoal );
}


//-----------------------------------------------------------------------------------------------------
/**
 * Build a user command to move this player towards the goal position
 */
void PlayerLocomotion::Approach( const Vector &pos, float goalWeight )
{
	VPROF_BUDGET( "PlayerLocomotion::Approach", "NextBot" );

	BaseClass::Approach( pos );

	AdjustPosture( pos );

	if ( GetBot()->IsDebugging( NEXTBOT_LOCOMOTION ) )
	{
		NDebugOverlay::Line( GetFeet(), pos, 255, 255, 0, true, 0.1f );
	}

	INextBotPlayerInput *playerButtons = dynamic_cast< INextBotPlayerInput * >( GetBot() );

	if ( !playerButtons )
	{
		DevMsg( "PlayerLocomotion::Approach: No INextBotPlayerInput\n " );
		return;
	}

	Vector forward3D;
	m_player->EyeVectors( &forward3D );
	
	Vector2D forward( forward3D.x, forward3D.y );
	forward.NormalizeInPlace();
		
	Vector2D right( forward.y, -forward.x );

	// compute unit vector to goal position
	Vector2D to = ( pos - GetFeet() ).AsVector2D();
	float goalDistance = to.NormalizeInPlace();

	float ahead = to.Dot( forward );
	float side = to.Dot( right );

#ifdef NEED_TO_INTEGRATE_MOTION_CONTROLLED_CODE_FROM_L4D_PLAYERS
	// If we're climbing ledges, we need to stay crouched to prevent player movement code from messing
	// with our origin.
	CTerrorPlayer *player = ToTerrorPlayer(m_player);
	if ( player && player->IsMotionControlledZ( player->GetMainActivity() ) )
	{
		playerButtons->PressCrouchButton();
		return;
	}
#endif

	if ( m_player->IsOnLadder() && IsUsingLadder() && ( m_ladderState == ASCENDING_LADDER || m_ladderState == DESCENDING_LADDER ) )
	{
		// we are on a ladder and WANT to be on a ladder.
		playerButtons->PressForwardButton();

		// Stay in center of ladder.  The gamemovement will autocenter us in most cases, but this is needed in case it doesn't.
		if ( m_ladderInfo )
		{
			Vector posOnLadder;
			CalcClosestPointOnLine( GetFeet(), m_ladderInfo->m_bottom, m_ladderInfo->m_top, posOnLadder );

			Vector alongLadder = m_ladderInfo->m_top - m_ladderInfo->m_bottom;
			alongLadder.NormalizeInPlace();

			Vector rightLadder = CrossProduct( alongLadder, m_ladderInfo->GetNormal() );

			Vector away = GetFeet() - posOnLadder;

			// we only want error in plane of ladder
			float error = DotProduct( away, rightLadder );
			away.NormalizeInPlace();

			const float tolerance = 5.0f + 0.25f * GetBot()->GetBodyInterface()->GetHullWidth();
			if ( error > tolerance )
			{
				if ( DotProduct( away, rightLadder ) > 0.0f )
				{
					playerButtons->PressLeftButton();
				}
				else
				{
					playerButtons->PressRightButton();
				}
			}
		}
	}
	else
	{
		const float epsilon = 0.25f;
		bool bMoveDirect = NextBotPlayerMoveDirect.GetBool();
#ifdef NEO
		// patch 109: on a fall edge, no quantized drift
		const CNavArea *pArea = m_player->GetLastKnownArea();
		bMoveDirect |= neo_bot_precise_move_direct.GetBool() && pArea && pArea->HasAttributes( NAV_MESH_PRECISE | NAV_MESH_CLIFF );
#endif
		if ( bMoveDirect )
		{
			if ( goalDistance > epsilon )
			{
				playerButtons->SetButtonScale( ahead, side );
			}
		}

		if ( ahead > epsilon )
		{
			playerButtons->PressForwardButton();

			if ( GetBot()->IsDebugging( NEXTBOT_LOCOMOTION ) )
			{
				NDebugOverlay::HorzArrow( m_player->GetAbsOrigin(), m_player->GetAbsOrigin() + 50.0f * Vector( forward.x, forward.y, 0.0f ), 15.0f, 0, 255, 0, 255, true, 0.1f );
			}
		}
		else if ( ahead < -epsilon )
		{
			playerButtons->PressBackwardButton();

			if ( GetBot()->IsDebugging( NEXTBOT_LOCOMOTION ) )
			{
				NDebugOverlay::HorzArrow( m_player->GetAbsOrigin(), m_player->GetAbsOrigin() - 50.0f * Vector( forward.x, forward.y, 0.0f ), 15.0f, 255, 0, 0, 255, true, 0.1f );
			}
		}

		if ( side <= -epsilon )
		{
			playerButtons->PressLeftButton();

			if ( GetBot()->IsDebugging( NEXTBOT_LOCOMOTION ) )
			{
				NDebugOverlay::HorzArrow( m_player->GetAbsOrigin(), m_player->GetAbsOrigin() - 50.0f * Vector( right.x, right.y, 0.0f ), 15.0f, 255, 0, 255, 255, true, 0.1f );
			}
		}
		else if ( side >= epsilon )
		{
			playerButtons->PressRightButton();

			if ( GetBot()->IsDebugging( NEXTBOT_LOCOMOTION ) )
			{
				NDebugOverlay::HorzArrow( m_player->GetAbsOrigin(), m_player->GetAbsOrigin() + 50.0f * Vector( right.x, right.y, 0.0f ), 15.0f, 0, 255, 255, 255, true, 0.1f );
			}
		}
	}

#ifdef NEO
	CNEOBot* me = (CNEOBot*)GetBot()->GetEntity();
	if ( me->m_nButtons & IN_WALK )
	{
		// If walk key was activated this tick, stop pressing run button.
	}
	else if ( IsRunning() )
	{
		playerButtons->PressRunButton();
	}
#else
	if ( !IsRunning() )
	{
		playerButtons->PressWalkButton();
	}
#endif
}


//----------------------------------------------------------------------------------------------------
/**
 * Move the bot to the precise given position immediately, 
 */
void PlayerLocomotion::DriveTo( const Vector &pos )
{
	BaseClass::DriveTo( pos );

	Approach( pos );
}


//----------------------------------------------------------------------------------------------------
bool PlayerLocomotion::IsClimbPossible( INextBot *me, const CBaseEntity *obstacle ) const
{
	// don't jump unless we have to
	const PathFollower *path = GetBot()->GetCurrentPath();
	if ( path )
	{
		const float watchForClimbRange = 75.0f;
		if ( !path->IsDiscontinuityAhead( GetBot(), Path::CLIMB_UP, watchForClimbRange ) )
		{
			// we are not planning on climbing

			// always allow climbing over movable obstacles
			if ( obstacle && !const_cast< CBaseEntity * >( obstacle )->IsWorld() )
			{
				IPhysicsObject *physics = obstacle->VPhysicsGetObject();
				if ( physics && physics->IsMoveable() )
				{
					// movable physics object - climb over it
					return true;
				}
			}

			if ( !GetBot()->GetLocomotionInterface()->IsStuck() )
			{
				// we're not stuck - don't try to jump up yet
				return false;
			}
		}
	}

	return true;
}


//----------------------------------------------------------------------------------------------------
bool PlayerLocomotion::ClimbUpToLedge( const Vector &landingGoal, const Vector &landingForward, const CBaseEntity *obstacle )
{
	if ( !IsClimbPossible( GetBot(), obstacle ) )
	{
		return false;
	}

	Jump();

	m_isClimbingUpToLedge = true;
	m_landingGoal = landingGoal;
	m_hasLeftTheGround = false;

	return true;
}


//----------------------------------------------------------------------------------------------------
void PlayerLocomotion::JumpAcrossGap( const Vector &landingGoal, const Vector &landingForward )
{
	Jump();

	// face forward
	GetBot()->GetBodyInterface()->AimHeadTowards( landingGoal, IBody::MANDATORY, 1.0f, NULL, "Looking forward while jumping a gap" );

	m_isJumpingAcrossGap = true;
	m_landingGoal = landingGoal;
	m_hasLeftTheGround = false;
}


//----------------------------------------------------------------------------------------------------
void PlayerLocomotion::Jump( void )
{
	m_isJumping = true;
	m_jumpTimer.Start( 0.5f );

	INextBotPlayerInput *playerButtons = dynamic_cast< INextBotPlayerInput * >( GetBot() );
	if ( playerButtons )
	{
		playerButtons->PressJumpButton();
	}
}

#ifdef NEO

//----------------------------------------------------------------------------------------------------
void PlayerLocomotion::Thermoptic( void )
{
	INextBotPlayerInput *playerButtons = dynamic_cast< INextBotPlayerInput * >( GetBot() );
	if ( playerButtons )
	{
		playerButtons->PressThermopticButton();
	}
}

#endif // NEO


//----------------------------------------------------------------------------------------------------
bool PlayerLocomotion::IsClimbingOrJumping( void ) const
{
	if ( !m_isJumping )
		return false;
		
	if ( m_jumpTimer.IsElapsed() && IsOnGround() )
	{
		m_isJumping = false;
		return false;
	}
	
	return true;
}


//----------------------------------------------------------------------------------------------------
bool PlayerLocomotion::IsClimbingUpToLedge( void ) const
{
	return m_isClimbingUpToLedge;
}


//----------------------------------------------------------------------------------------------------
bool PlayerLocomotion::IsJumpingAcrossGap( void ) const
{
	return m_isJumpingAcrossGap;
}


//----------------------------------------------------------------------------------------------------
/**
 * Return true if standing on something
 */
bool PlayerLocomotion::IsOnGround( void ) const
{
	return (m_player->GetGroundEntity() != NULL);
}


//----------------------------------------------------------------------------------------------------
/**
 * Return the current ground entity or NULL if not on the ground
 */
CBaseEntity *PlayerLocomotion::GetGround( void ) const
{
	return m_player->GetGroundEntity();
}


//----------------------------------------------------------------------------------------------------
/**
 * Surface normal of the ground we are in contact with
 */
const Vector &PlayerLocomotion::GetGroundNormal( void ) const
{
	static Vector up( 0, 0, 1.0f );
	return up;

	// TODO: Integrate movehelper_server for this:  return m_player->GetGroundNormal();
}


//----------------------------------------------------------------------------------------------------
/**
 * Climb the given ladder to the top and dismount
 */
void PlayerLocomotion::ClimbLadder( const CNavLadder *ladder, const CNavArea *dismountGoal )
{
	// look up and push forward
// 	Vector goal =  GetBot()->GetPosition() + 100.0f * ( Vector( 0, 0, 1.0f ) - ladder->GetNormal() );
// 	Approach( goal );
// 	FaceTowards( goal );
	
#ifdef NEO
	NeoStartLadderApproach( m_ladderTimer, GetBot(), (int)m_ladderState, ladder, true );
#endif

	m_ladderState = APPROACHING_ASCENDING_LADDER;
	m_ladderInfo = ladder;
	m_ladderDismountGoal = dismountGoal;
}


//----------------------------------------------------------------------------------------------------
/**
 * Descend the given ladder to the bottom and dismount
 */
void PlayerLocomotion::DescendLadder( const CNavLadder *ladder, const CNavArea *dismountGoal )
{
	// look down and push forward
// 	Vector goal =  GetBot()->GetPosition() + 100.0f * ( Vector( 0, 0, -1.0f ) - ladder->GetNormal() );
// 	Approach( goal );
// 	FaceTowards( goal );

#ifdef NEO
	NeoStartLadderApproach( m_ladderTimer, GetBot(), (int)m_ladderState, ladder, false );
#endif

	m_ladderState = APPROACHING_DESCENDING_LADDER;
	m_ladderInfo = ladder;
	m_ladderDismountGoal = dismountGoal;
}


//----------------------------------------------------------------------------------------------------
bool PlayerLocomotion::IsUsingLadder( void ) const
{
	return ( m_ladderState != NO_LADDER );
}


//----------------------------------------------------------------------------------------------------
/**
 * Rotate body to face towards "target"
 */
void PlayerLocomotion::FaceTowards( const Vector &target )
{
	// player body follows view direction
	Vector look( target.x, target.y, GetBot()->GetEntity()->EyePosition().z );

	GetBot()->GetBodyInterface()->AimHeadTowards( look, IBody::BORING, 0.1f, NULL, "Body facing" );
}


//-----------------------------------------------------------------------------------------------------
/**
* Return position of "feet" - point below centroid of bot at feet level
*/
const Vector &PlayerLocomotion::GetFeet( void ) const
{
	return m_player->GetAbsOrigin();
}


//-----------------------------------------------------------------------------------------------------
/**
 * Return current world space velocity
 */
const Vector &PlayerLocomotion::GetVelocity( void ) const
{
	return m_player->GetAbsVelocity();
}


//-----------------------------------------------------------------------------------------------------
float PlayerLocomotion::GetRunSpeed( void ) const
{
	return m_player->MaxSpeed();
}


//-----------------------------------------------------------------------------------------------------
float PlayerLocomotion::GetWalkSpeed( void ) const
{
	return 0.5f * m_player->MaxSpeed();
}

