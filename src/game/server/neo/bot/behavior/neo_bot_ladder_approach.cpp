#include "cbase.h"
#include "bot/behavior/neo_bot_ladder_approach.h"
#include "bot/behavior/neo_bot_ladder_climb.h"
#include "bot/behavior/neo_bot_attack.h"
#include "nav_ladder.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

// NEO research (patch 63): some ladders' climbable volume starts above the floor (a nodraw kick block at the foot),
// so a bot standing at the foot pressing forward never attaches - it presses into the block until the approach times
// out, then the path sends it back (skyline ladder 1, bullet ladder 1: every run). Players hop onto such ladders.
ConVar neo_bot_ladder_mount_hop( "neo_bot_ladder_mount_hop", "0", FCVAR_CHEAT,
	"Research: 1 = a bot aligned at a ladder's foot that has pushed forward without attaching hops onto the ladder" );

// NEO-HARNESS-TEMP research arm (2026-09-24, patch 73): a ladder whose top landings are only to its sides (no forward or
// behind area - transit ladder 2, saitama 2, apparatus 4) cannot be mounted going down by walking at its top point: the
// walk runs along the ladder's face, and the engine only grabs a ladder the wish direction points into (lc2: 146 of 246
// down-approach timeouts on those three). Step out in front of the face first, then move into it. v1 pushed into the
// face from the usual 25 u mount range, while the bot still stood on the landing (lc45: timeouts 129 -> 113).
ConVar neo_bot_ladder_side_mount( "neo_bot_ladder_side_mount", "0", FCVAR_CHEAT,
	"Research: going down a ladder whose top landings are only at its sides, approach its face from the front" );

// NEO-HARNESS-TEMP research arm (2026-09-24, patch 79): the approach aligns by heading for a point pulled out from the
// ladder along its normal by up to ALIGN_RANGE (100 u). In a slot narrower than that the point lies inside the far wall,
// and a bot coming from the side slides along that wall's end instead of turning into the slot (lcg: bullet ladder 4, 29
// up-approach timeouts, 18 of them at one wall end). Keep the point inside the free space in front of the ladder.
ConVar neo_bot_ladder_align_clamp( "neo_bot_ladder_align_clamp", "0", FCVAR_CHEAT,
	"Research: the ladder approach's alignment point stays inside the free space in front of the ladder" );

// NEO-HARNESS-TEMP research arm (2026-09-24, patch 82): the approach looks at whether the engine has put the bot on the
// ladder only once it is within mount range - 6 u of the point over the shaft for a side-landing descent. A bot that steps
// into the shaft and is grabbed short of that keeps approaching while the locomotion, seeing a ladder nobody claimed, adopts
// it by its nearer end and drives the bot up and off (lch C: 22 timeouts and 64 threat endings with the bot on the ladder).
// On the ladder is mounted: start the climb.
ConVar neo_bot_ladder_mount_on_contact( "neo_bot_ladder_mount_on_contact", "0", FCVAR_CHEAT,
	"Research: a bot the engine has put on the approached ladder starts the climb at once, whatever its range to the mount point" );
static constexpr float NEO_LADDER_CONTACT_RANGE = 48.0f;	// xy from the ladder's line: this ladder, not a neighbour

// patch 79: how far out from the ladder, along alignOut, the bot's hull still fits at its own floor height
static float NeoFreeAlignDistance( CNEOBot *me, const Vector &ladderPoint, const Vector2D &alignOut, float maxDist )
{
	ILocomotion *mover = me->GetLocomotionInterface();
	const Vector out( alignOut.x, alignOut.y, 0.0f );
	const float flNear = me->GetBodyInterface()->GetHullWidth() * 0.5f + 2.0f;
	if ( maxDist <= flNear )
	{
		return maxDist;
	}

	// start where a mounting bot stands, lifted a step so floor bumps do not count
	Vector start = ladderPoint + out * flNear;
	start.z = mover->GetFeet().z + mover->GetStepHeight();
	const Vector end = ladderPoint + out * maxDist + Vector( 0.0f, 0.0f, start.z - ladderPoint.z );

	trace_t tr;
	UTIL_TraceHull( start, end, me->WorldAlignMins(), me->WorldAlignMaxs(), MASK_PLAYERSOLID_BRUSHONLY, me, COLLISION_GROUP_NONE, &tr );
	if ( tr.startsolid )
	{
		return maxDist;
	}

	return flNear + tr.fraction * ( maxDist - flNear );
}

// NEO-HARNESS-TEMP forensic instrumentation (2026-09-24): see NEO_FORENSIC_LADDERBEH in neo_bot_ladder_climb.cpp
extern ConVar sv_neo_forensic_log;
static void NeoLogLadderApproach( CNEOBot *me, const CNavLadder *ladder, bool goingUp, const char *reason )
{
	if ( !sv_neo_forensic_log.GetBool() || !me )
	{
		return;
	}
	const Vector &feet = me->GetLocomotionInterface()->GetFeet();
	Msg( "NEO_FORENSIC_LADDERBEH t=%.2f p=%d beh=approach dir=%s ladder=%d pos=%.0f,%.0f,%.0f ladderbottom=%.0f laddertop=%.0f movetype=%d onground=%d reason=%s\n",
		gpGlobals->curtime, me->entindex(), goingUp ? "up" : "down", ladder ? ladder->GetID() : -1,
		feet.x, feet.y, feet.z, ladder ? ladder->m_bottom.z : 0.0f, ladder ? ladder->m_top.z : 0.0f,
		(int)me->GetMoveType(), me->GetGroundEntity() ? 1 : 0, reason );
}

//---------------------------------------------------------------------------------------------
CNEOBotLadderApproach::CNEOBotLadderApproach( const CNavLadder *ladder, bool goingUp )
	: m_ladder( ladder ), m_bGoingUp( goingUp )
{
	m_ladderCenter = ladder ? ( ladder->m_top + ladder->m_bottom ) * 0.5f : vec3_origin;
}

//---------------------------------------------------------------------------------------------
ActionResult<CNEOBot> CNEOBotLadderApproach::OnStart( CNEOBot *me, Action<CNEOBot> *priorAction )
{
	if ( !m_ladder )
	{
		return Done( "No ladder specified" );
	}

	// Timeout for approach phase
	m_timeoutTimer.Start( 3.0f );

	if ( me->IsDebugging( NEXTBOT_PATH ) )
	{
		DevMsg( "%s: Starting ladder approach (%s), length %.1f\n",
			me->GetDebugIdentifier(),
			m_bGoingUp ? "up" : "down",
			m_ladder->m_length );
	}

	// Don't interfere with look direction
	// Can exit out of behavior if threat is present
	me->StopLookingAroundForEnemies();
	me->SetAttribute( CNEOBot::IGNORE_ENEMIES );

	return Continue();
}

//---------------------------------------------------------------------------------------------
// Implementation based on ladder climbing implementation in https://github.com/Dragoteryx/drgbase/
ActionResult<CNEOBot> CNEOBotLadderApproach::Update( CNEOBot *me, float )
{
	if ( !m_ladder )
	{
		NeoLogLadderApproach( me, m_ladder, m_bGoingUp, "Ladder is invalid" );
		return Done( "Ladder is invalid" );
	}

	if ( m_timeoutTimer.IsElapsed() )
	{
		NeoLogLadderApproach( me, m_ladder, m_bGoingUp, "Ladder approach timeout" );
		return Done( "Ladder approach timeout" );
	}

	// patch 82: already on this ladder - climb it, before the locomotion adopts it the other way
	if ( neo_bot_ladder_mount_on_contact.GetBool() && me->IsOnLadder() )
	{
		const Vector &feet = me->GetLocomotionInterface()->GetFeet();
		if ( ( m_ladder->GetPosAtHeight( feet.z ) - feet ).AsVector2D().IsLengthLessThan( NEO_LADDER_CONTACT_RANGE ) )
		{
			me->SetAbsVelocity( vec3_origin );
			NeoLogLadderApproach( me, m_ladder, m_bGoingUp, "mounted" );
			return ChangeTo( new CNEOBotLadderClimb( m_ladder, m_bGoingUp ), "Mounting ladder" );
		}
	}

	const CKnownEntity *threat = me->GetVisionInterface()->GetPrimaryKnownThreat(true);
	if ( threat )
	{
		if ( me->IsDebugging( NEXTBOT_PATH ) )
		{
			DevMsg( "%s: Threat detected during ladder approach - engaging\n", me->GetDebugIdentifier() );
		}
		// ChangeTo: We may move away from ladder when fighting, so don't want to get stuck in ladder approach behavior
		NeoLogLadderApproach( me, m_ladder, m_bGoingUp, "threat" );
		return ChangeTo( new CNEOBotAttack(), "Engaging enemy before climbing" );
	}

	ILocomotion *mover = me->GetLocomotionInterface();
	IBody *body = me->GetBodyInterface();

	const Vector& myPos = mover->GetFeet();

	// If we end up closer to exit point than entry point,
	// may not need to continue climb, exit to reevaluate situation
	Vector entryPos = m_bGoingUp ? m_ladder->m_bottom : m_ladder->m_top;
	Vector exitPos = m_bGoingUp ? m_ladder->m_top : m_ladder->m_bottom;
	float distToEntrySq = myPos.DistToSqr( entryPos );
	float distToExitSq = myPos.DistToSqr( exitPos );

	if ( distToExitSq < distToEntrySq )
	{
		NeoLogLadderApproach( me, m_ladder, m_bGoingUp, "Closer to ladder exit than entry, assuming goal reached accidentally" );
		return Done( "Closer to ladder exit than entry, assuming goal reached accidentally" );
	}

	// Are we climbing up or down the ladder?
	Vector targetPos = m_bGoingUp ? m_ladder->m_bottom : m_ladder->m_top;

	// Going down, head for where the bot will hang on the ladder: in front of its face, over the drop
	if ( !m_bGoingUp )
	{
		targetPos += m_ladder->GetNormal() * ( body->GetHullWidth() * 0.5f + HANG_CLEARANCE );
	}

	// patch 73: side-landing ladders are mounted going down from in front of the face
	const bool bSideMount = !m_bGoingUp && neo_bot_ladder_side_mount.GetBool()
		&& !m_ladder->m_topForwardArea && !m_ladder->m_topBehindArea;
	if ( bSideMount )
	{
		targetPos = m_ladder->m_top + m_ladder->GetNormal() * ( body->GetHullWidth() * 0.5f + 2.0f );
	}

	// Calculate 2D vector from bot to ladder mount point
	Vector2D to = ( targetPos - myPos ).AsVector2D();
	float range = to.NormalizeInPlace();

	// Get ladder's outward normal in 2D
	Vector2D ladderNormal2D = m_ladder->GetNormal().AsVector2D();
	float dot = DotProduct2D( ladderNormal2D, to );

	// Aim at the ladder center at eye level to carefully attach to ladder
	// Eye level in order to not accidentally move and detach between behavior transition
	// If bot was looking up or down, sometimes the fast climb movement causes a detachment
	Vector lookTarget = m_ladderCenter;
	lookTarget.z = me->EyePosition().z;

	// Going down, look down the ladder instead: the forward press that grabs it stays held for a few
	// ticks, and with a level view it climbs the bot up and off the top
	if ( !m_bGoingUp )
	{
		lookTarget = m_ladder->m_bottom;
	}

	body->AimHeadTowards( lookTarget, IBody::MANDATORY, 0.1f, nullptr, "Stare at ladder center" );

	if ( me->IsDebugging( NEXTBOT_PATH ) )
	{
		NDebugOverlay::Cross3D( targetPos, 5.0f, 255, 255, 0, true, 0.1f );
		NDebugOverlay::Line( myPos, targetPos, 255, 255, 0, true, 0.1f );
	}

	// Going down, mount only once off the top: grabbed while still up there, on the landing or on the
	// ladder's own top, the bot cannot press its way down, so it keeps walking out over the drop
	const float flTopZ = m_ladder->m_top.z;
	const bool bOnTop = !m_bGoingUp
		&& ( myPos.z >= flTopZ || ( mover->IsOnGround() && myPos.z > flTopZ - mover->GetStepHeight() ) );

	// Within mount range and on the ladder: start climbing
	if ( range < MOUNT_RANGE && me->IsOnLadder() && !bOnTop )
	{
		if ( me->IsDebugging( NEXTBOT_PATH ) )
		{
			DevMsg( "%s: Starting ladder climb\n", me->GetDebugIdentifier() );
		}

		// Stop the bot before behavior transition to prevent falling off the ladder
		// there can be a delay in the state change, so momentum can cause a fall
		me->SetAbsVelocity( vec3_origin );
		// ChangeTo: if something goes wrong during climb, reevaluate situation
		NeoLogLadderApproach( me, m_ladder, m_bGoingUp, "mounted" );
		return ChangeTo( new CNEOBotLadderClimb( m_ladder, m_bGoingUp ), "Mounting ladder" );
	}

	// Are we aligned and close enough to mount the ladder? Going down, line up only from in front of the
	// face (at an angle the bot ends up on the floor beside the drop); from elsewhere walk straight on
	// (patch 73 v2: a side-landing descent must first walk off the landing to the point in front of the face - that point
	// is over the shaft, and at the usual 25 u mount range the bot was still on the landing, pushing into the wall)
	const float flLineUpRange = bSideMount ? 6.0f : MOUNT_RANGE;
	const bool bAligned = dot < ALIGN_DOT_THRESHOLD;
	const bool bLineUp = m_bGoingUp ? range >= flLineUpRange
		: ( dot < 0.0f && ( range >= flLineUpRange || !bAligned ) );
	if ( bLineUp )
	{
		m_pushTimer.Invalidate();

		// Perpendicular alignment line
		Vector2D alignNormal = ladderNormal2D;
		if ( dot > 0.0f )
		{
			alignNormal = -alignNormal; // Target behind ladder
		}

		Vector goal = targetPos;
		
		// Pull the goal point outwards along the ladder's normal
		// to guide bot movement along approach
		// (patch 73: not for a side-landing descent - out along the normal is the open shaft in front of the ladder)
		float offsetDist = bSideMount ? 0.0f : Clamp( range * 0.8f, 10.0f, ALIGN_RANGE );
		// patch 79: in a slot shallower than the offset, aim at the middle of the slot rather than into its far wall
		if ( !bSideMount && neo_bot_ladder_align_clamp.GetBool() )
		{
			const float flFree = NeoFreeAlignDistance( me, targetPos, alignNormal, offsetDist );
			if ( flFree < offsetDist )
			{
				offsetDist = Max( 10.0f, 0.5f * ( flFree + body->GetHullWidth() * 0.5f + 2.0f ) );
			}
		}
		goal.x += alignNormal.x * offsetDist;
		goal.y += alignNormal.y * offsetDist;

		mover->Approach( goal );

		if ( me->IsDebugging( NEXTBOT_PATH ) )
		{
			NDebugOverlay::Cross3D( goal, 5.0f, 255, 0, 255, true, 0.1f );
		}
	}
	else if ( m_bGoingUp ? bAligned : !bOnTop )
	{
		// Aligned (or going down and off the top), push forward to attach to the ladder
		me->PressForwardButton();
		// patch 73: in front of a side-landing ladder's face, move into the face so the engine grabs it
		mover->Approach( bSideMount ? m_ladder->m_top - m_ladder->GetNormal() * 16.0f : targetPos );

		if ( m_bGoingUp && neo_bot_ladder_mount_hop.GetBool() )
		{
			// pushed into the foot without attaching: hop onto the rungs
			if ( !m_pushTimer.HasStarted() )
			{
				m_pushTimer.Start();
			}
			else if ( m_pushTimer.IsGreaterThen( 0.4f ) && mover->IsOnGround() && ( !m_hopTimer.HasStarted() || m_hopTimer.IsElapsed() ) )
			{
				me->PressJumpButton();
				m_hopTimer.Start( 0.8f );
			}
		}
	}
	else
	{
		// Close but not aligned (or going down and still on the top) - continue approaching
		mover->Approach( targetPos );
	}

	return Continue();
}

//---------------------------------------------------------------------------------------------
void CNEOBotLadderApproach::OnEnd( CNEOBot *me, Action<CNEOBot> *nextAction )
{
	me->StartLookingAroundForEnemies();
	me->ClearAttribute( CNEOBot::IGNORE_ENEMIES );

	if ( me->IsDebugging( NEXTBOT_PATH ) )
	{
		DevMsg( "%s: Finished ladder approach\n", me->GetDebugIdentifier() );
	}
}

//---------------------------------------------------------------------------------------------
ActionResult<CNEOBot> CNEOBotLadderApproach::OnSuspend( CNEOBot *me, Action<CNEOBot> *interruptingAction )
{
	return Done( "OnSuspend: Cancel out of ladder approach, situation will likely become stale." );
}

//---------------------------------------------------------------------------------------------
ActionResult<CNEOBot> CNEOBotLadderApproach::OnResume( CNEOBot *me, Action<CNEOBot> *interruptingAction )
{
	return Done( "OnResume: Cancel out of ladder approach, situation is likely stale." );
}
