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

// NEO: what to do when the engine has put a bot on a ladder its own path never asked for.
// `CGameMovement::LadderMove()` grabs any ladder the player's wish direction points at, while
// `TraverseLadder()` drops the move type straight back to walking - so a path that merely brushes
// past a ladder leaves the two fighting every tick, and the bot hangs there making no progress
// and firing no stuck event. Measured 2026-09-20: 85 such episodes over 200 matches.
enum LadderReleaseMode
{
	LADDER_RELEASE_ENGINE = 0,		// unchanged behaviour: drop the move type and nothing else
	LADDER_RELEASE_PUSHOFF,			// let go, then steer out of the ladder's face so it cannot re-grab
	LADDER_RELEASE_ADOPT,			// stop arguing - ride it to the nearer end and dismount there
	LADDER_RELEASE_SLIDE,			// keep going, but with the into-the-wall part of the heading removed
	LADDER_RELEASE_HOLD,			// stop fighting altogether: leave the move type alone for a moment
	LADDER_RELEASE_ADOPT_UP,		// adopt, but only when there is somewhere above to climb to
	LADDER_RELEASE_ADOPT_STUCK,		// adopt by the nearer end, but only once the contact has persisted
};

ConVar neo_bot_ladder_release_mode( "neo_bot_ladder_release_mode", "0", FCVAR_CHEAT,
	"How a bot leaves a ladder it never asked to be on: 0 engine behaviour, 1 push off, 2 adopt it, "
	"3 slide along it, 4 hold and stop fighting, 5 adopt only upward, 6 adopt once the contact sticks.",
	true, LADDER_RELEASE_ENGINE, true, LADDER_RELEASE_ADOPT_STUCK );

ConVar neo_bot_ladder_release_time( "neo_bot_ladder_release_time", "0.5", FCVAR_CHEAT,
	"Seconds a bot keeps steering away from a ladder it just let go of.", true, 0.0f, true, 5.0f );

// Far enough out that the wish direction clearly leaves the brush LadderMove() traces against.
static const float LADDER_PUSHOFF_RANGE = 100.0f;
// A ladder further than this from the bot is not the one it is stuck on.
static const float LADDER_TOUCH_RANGE = 64.0f;
// A gap longer than this means the bot walked away and came back, so the bout starts over.
static const float LADDER_CONTACT_RESET = 1.0f;

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

	m_unwantedLadderTimer.Invalidate();
	m_unwantedLadderNormal.Init();
	m_unwantedLadderSince = 0.0f;
	m_unwantedLadderLastTouch = 0.0f;

	m_minSpeedLimit = 0.0f;
	m_maxSpeedLimit = 9999999.9f;

	BaseClass::Reset();
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
	const int mode = neo_bot_ladder_release_mode.GetInt();

	if ( mode == LADDER_RELEASE_ENGINE )
	{
		return false;
	}

	const CNavLadder *ladder = FindTouchedLadder();
	if ( ladder == NULL )
	{
		return false;
	}

	if ( mode == LADDER_RELEASE_PUSHOFF || mode == LADDER_RELEASE_SLIDE )
	{
		m_unwantedLadderNormal = ladder->GetNormal();
		m_unwantedLadderTimer.Start( neo_bot_ladder_release_time.GetFloat() );
		return false;
	}

	if ( mode == LADDER_RELEASE_HOLD )
	{
		// Stop fighting at all: leave the move type alone and let the engine's own ladder movement
		// carry the bot for a moment. Nothing here steers, which is the point.
		if ( m_unwantedLadderTimer.HasStarted() && m_unwantedLadderTimer.IsElapsed() )
		{
			// held long enough and still here - fall back to letting go
			m_unwantedLadderTimer.Invalidate();
			return false;
		}

		if ( !m_unwantedLadderTimer.HasStarted() )
		{
			m_unwantedLadderTimer.Start( neo_bot_ladder_release_time.GetFloat() );
		}

		return true;
	}

	if ( mode == LADDER_RELEASE_ADOPT_STUCK )
	{
		// Most grabs sort themselves out within a second, and adopting every one of them buys a
		// lot of climbing nobody asked for. Only take the ladder over once the bot has actually
		// been stuck against it.
		const float now = gpGlobals->curtime;

		if ( now - m_unwantedLadderLastTouch > LADDER_CONTACT_RESET )
		{
			m_unwantedLadderSince = now;
		}

		m_unwantedLadderLastTouch = now;

		if ( now - m_unwantedLadderSince < neo_bot_ladder_release_time.GetFloat() )
		{
			return false;
		}
	}

	// LADDER_RELEASE_ADOPT: leave by the nearer end rather than argue about being here at all.
	// Whichever end the bot is closer to is the one it can reach soonest, and the dismount goal
	// has to be a real area or DismountLadderTop/Bottom has nothing to walk to.
	bool bGoUp = ( GetFeet().z - ladder->m_bottom.z ) > ( ladder->m_top.z - GetFeet().z );

	if ( mode == LADDER_RELEASE_ADOPT_UP )
	{
		// Adopting a descent at the foot of a ladder is a wasted round trip: DescendLadder sees
		// "reached bottom" at once, dismounts, and the engine grabs again. Only climb.
		if ( GetFeet().z > ladder->m_top.z - GetStepHeight() )
		{
			return false;
		}

		bGoUp = true;
	}

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
	switch( m_ladderState )
	{
	case APPROACHING_ASCENDING_LADDER:
		m_ladderState = ApproachAscendingLadder();
		return true;

	case APPROACHING_DESCENDING_LADDER:
		m_ladderState = ApproachDescendingLadder();
		return true;

	case ASCENDING_LADDER:
		m_ladderState = AscendLadder();
		return true;

	case DESCENDING_LADDER:
		m_ladderState = DescendLadder();
		return true;

	case DISMOUNTING_LADDER_TOP:
		m_ladderState = DismountLadderTop();
		return true;

	case DISMOUNTING_LADDER_BOTTOM:
		m_ladderState = DismountLadderBottom();
		return true;

	case NO_LADDER:
	default:
		m_ladderInfo = NULL;

		if ( GetBot()->GetEntity()->GetMoveType() == MOVETYPE_LADDER )
		{
			// On a ladder and don't want to be. HandleUnwantedLadder() may take the ladder over
			// instead, in which case the state machine drives from here and we are done.
			if ( HandleUnwantedLadder() )
			{
				return true;
			}

			GetBot()->GetEntity()->SetMoveType( MOVETYPE_WALK );
		}

		// Keep steering out of the ladder's face for a moment after letting go. Dropping the move
		// type alone leaves the bot's wish direction pointing at the brush, which is exactly what
		// LadderMove() traces against, so it grabs again on the very next tick.
		if ( !m_unwantedLadderTimer.IsElapsed() )
		{
			Vector steer = m_unwantedLadderNormal;

			if ( neo_bot_ladder_release_mode.GetInt() == LADDER_RELEASE_SLIDE )
			{
				// Backing straight off just hands the path a reason to walk back in, so keep the
				// heading and take out only the part pushing into the ladder - the bot slides
				// along the wall instead of pressing against it.
				//
				//   wall ##|  heading ->\        becomes  ##|  ->----
				//        ##|             v                 ##|
				steer = GetGroundMotionVector();
				const float into = -DotProduct( steer, m_unwantedLadderNormal );
				if ( into > 0.0f )
				{
					steer += into * m_unwantedLadderNormal;
				}

				if ( steer.NormalizeInPlace() < 0.1f )
				{
					// heading was straight into the wall and nothing survived the projection
					steer = m_unwantedLadderNormal;
				}
			}

			// it is important to approach precisely, so use a very large weight to wash out all other Approaches
			Approach( GetFeet() + LADDER_PUSHOFF_RANGE * steer, 9999999.9f );
		}

		return false;
	}

	return true;
}


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

	// sanity check - are we already at the end of this ladder?
	if ( GetFeet().z >= m_ladderInfo->m_top.z - GetStepHeight() )
	{
		m_ladderTimer.Start( 2.0f );
		return DISMOUNTING_LADDER_TOP;
	}

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

			if ( m_isJumpingAcrossGap )
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
		if ( NextBotPlayerMoveDirect.GetBool() )
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

