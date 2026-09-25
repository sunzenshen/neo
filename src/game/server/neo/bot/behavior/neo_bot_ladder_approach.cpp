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
static constexpr float NEO_LADDER_APPROACH_TIMEOUT = 3.0f;

// NEO-HARNESS-TEMP research arm (2026-09-24, patch 84): a descent approached from the landing behind the ladder's plane (its top
// forward area - bullet's shafts) pushes into the back of the ladder brush; the engine grabs the bot while it still stands on
// the landing, and MoveDown there is LadderMove's "on the ground, moving away": the bot steps back off (ldj C: bullet ladder 4
// 97 of 110 descents ended back at the top). Mount every descent the way patch 73 mounts side-landing ones: walk to the point
// in front of the face - over the shaft - then push into the face.
ConVar neo_bot_ladder_front_mount( "neo_bot_ladder_front_mount", "0", FCVAR_CHEAT,
	"Research: every descent is mounted from the point in front of the ladder's face (patch 73's approach for all ladders)" );

// NEO-HARNESS-TEMP research arm (2026-09-25, patch 89): the engine lets go of a player on the ground moving away from the face,
// so a descent mounted while the bot still stands on the top landing is dropped at once and the climb ends "reached the
// ground" (46 % of descents over lcn / vp89 / vp90; 82 % of those mounted on the ground vs 22 % airborne,
// scratch/rr/ladder_audit.md 3.6). Do not mount a descent until the bot is off the landing.
ConVar neo_bot_ladder_descent_airborne( "neo_bot_ladder_descent_airborne", "0", FCVAR_CHEAT,
	"Research: a descent is mounted only once the bot has left the top landing (airborne, or a step below the top)" );

// NEO-HARNESS-TEMP research arm (2026-09-25, patch 103): inside mount range but not lined up, the approach walked straight at
// the ladder's foot - from the side that is the ladder's edge (lt1: the leaning sentinel ladder 7's up-climbs that slip off
// began 16-26 u to the side of a 16 u ladder). Keep lining up in front of the face instead.
ConVar neo_bot_ladder_close_align( "neo_bot_ladder_close_align", "0", FCVAR_CHEAT,
	"Research: an up-approach inside mount range but not lined up steps to the front of the face instead of walking at the foot" );
static constexpr float NEO_LADDER_FRONT_STANDOFF = 20.0f;	// the point in front of the foot the approach lines up on

// NEO-HARNESS-TEMP research arm (2026-09-25, patch 105): a bot walking a narrow landing along the wall meets the side of the
// ladder brush where it stands proud of the wall (sentinel ladder 4: a solid 2 u grate on a 24 u ledge), the engine grabs it
// by that side, and the descent never starts (23 of 24 failed descents from its top began 32 u to the side). Beside the
// ladder and close to its wall, step out from the wall first; do not mount there.
ConVar neo_bot_ladder_clear_edge( "neo_bot_ladder_clear_edge", "0", FCVAR_CHEAT,
	"Research: a descent approach beside the ladder and close to its wall steps out past the ladder's edge before going on"
	" (1 = on the ground, 2 = also stepping down onto the landing, and behind the face's plane at side-landing ladders)" );
static constexpr float NEO_LADDER_EDGE_CLEARANCE = 4.0f;	// beyond the hull's half width, out from the face's plane
static constexpr float NEO_LADDER_LANDING_FLOOR = 24.0f;	// v3: floor this close under the feet is still the landing

// v3: on the landing or stepping down onto it - not falling into the gap beside the ladder
static bool NeoOnLadderLanding( CNEOBot *me )
{
	ILocomotion *mover = me->GetLocomotionInterface();
	if ( mover->IsOnGround() )
	{
		return true;
	}
	if ( neo_bot_ladder_clear_edge.GetInt() < 2 )
	{
		return false;
	}

	const Vector &feet = mover->GetFeet();
	trace_t tr;
	UTIL_TraceHull( feet, feet - Vector( 0.0f, 0.0f, NEO_LADDER_LANDING_FLOOR ), me->WorldAlignMins() * Vector( 1.0f, 1.0f, 0.0f ),
		me->WorldAlignMaxs() * Vector( 1.0f, 1.0f, 0.0f ) + Vector( 0.0f, 0.0f, 1.0f ), MASK_PLAYERSOLID, me, COLLISION_GROUP_PLAYER_MOVEMENT, &tr );
	return tr.fraction < 1.0f;
}

// NEO-HARNESS-TEMP research arm (2026-09-25, patch 107): the same trap at a ladder's foot - a bot coming along the wall runs
// past the front of the face and ends up beside the ladder, pressed between the wall and the ladder's side (ghost 1: every
// up-approach timeout there starts 31 u aside, behind the face's plane, wedged against the solid ladder prop). Beside the
// foot and close to the wall, not yet on the ladder, step out from the wall first.
ConVar neo_bot_ladder_clear_edge_up( "neo_bot_ladder_clear_edge_up", "0", FCVAR_CHEAT,
	"Research: an up-approach beside the ladder's foot and close to its wall, not yet on the ladder, steps out past the ladder's edge first" );

// patch 105: standing in front of the face's plane but beside the ladder, with the hull reaching the ladder brush's side
// (v2: only on the landing - a bot dropping into the gap beside the ladder is about to grab it; v3: see the cvar)
static bool NeoBesideLadderEdge( CNEOBot *me, const CNavLadder *ladder, bool bGoingUp, Vector *pvecOut )
{
	Vector2D vecNormal = ladder->GetNormal().AsVector2D();
	const bool bStanding = bGoingUp ? me->GetLocomotionInterface()->IsOnGround() : NeoOnLadderLanding( me );
	if ( !bStanding || vecNormal.NormalizeInPlace() <= 0.0f )
	{
		return false;
	}

	const Vector &feet = me->GetLocomotionInterface()->GetFeet();
	const Vector2D vecRel = ( feet - ( bGoingUp ? ladder->m_bottom : ladder->m_top ) ).AsVector2D();
	const float flDepth = DotProduct2D( vecRel, vecNormal );
	const float flSide = fabsf( vecRel.x * vecNormal.y - vecRel.y * vecNormal.x );
	const float flClear = me->GetBodyInterface()->GetHullWidth() * 0.5f + NEO_LADDER_EDGE_CLEARANCE;

	// behind the plane is the landing only where the ladder has no landing behind or in front of it (v3); at the foot
	// (107) behind the plane is the wall beside the ladder
	const bool bSideLanding = !ladder->m_topForwardArea && !ladder->m_topBehindArea;
	const float flMinDepth = ( bGoingUp || ( neo_bot_ladder_clear_edge.GetInt() >= 2 && bSideLanding ) ) ? -flClear : 0.0f;
	if ( flSide <= ladder->m_width * 0.5f || flDepth < flMinDepth || flDepth >= flClear )
	{
		return false;
	}

	// straight out from the wall, a little past where the hull clears the brush
	*pvecOut = feet;
	pvecOut->x += vecNormal.x * ( flClear + NEO_LADDER_EDGE_CLEARANCE - flDepth );
	pvecOut->y += vecNormal.y * ( flClear + NEO_LADDER_EDGE_CLEARANCE - flDepth );
	return true;
}

// la1: where the walk out over the edge is blocked (a clip or wall beside the top), never mounting on the landing is a
// deadlock, so after the first 1.2 s of an approach a ground mount is allowed again
static constexpr float NEO_LADDER_AIRBORNE_MOUNT_WINDOW = 1.2f;

// NEO-HARNESS-TEMP research arm (2026-09-25, patch 99): the 1.2 s window lets a ground mount through on ladders whose walk
// off the landing is not blocked at all, only slower (sentinel 3 / 6, top level with the shaft floor: 52-79 % of descents
// started at the top end there). Allow the ground mount only once the bot has stopped making progress - the deadlock case
// the window was for.
ConVar neo_bot_ladder_airborne_until_stall( "neo_bot_ladder_airborne_until_stall", "0", FCVAR_CHEAT,
	"Research: a descent may mount on the top landing only after the approach has made no progress for a while (not after 1.2 s)" );
static constexpr float NEO_LADDER_STALL_TIME = 0.6f;		// no progress this long = the walk off the landing is blocked
static constexpr float NEO_LADDER_PROGRESS_STEP = 4.0f;	// a move this far (xy) counts as progress

static bool NeoStillOnTopLanding( CNEOBot *me, const CNavLadder *ladder, bool bGoingUp, float flApproachAge, float flSinceProgress )
{
	// patch 99: the window ends when the walk off stalls, not at a fixed age
	const bool bWindowOver = neo_bot_ladder_airborne_until_stall.GetBool() ? flSinceProgress > NEO_LADDER_STALL_TIME
		: flApproachAge > NEO_LADDER_AIRBORNE_MOUNT_WINDOW;
	if ( bGoingUp || !neo_bot_ladder_descent_airborne.GetBool() || bWindowOver )
	{
		return false;
	}

	ILocomotion *mover = me->GetLocomotionInterface();
	return mover->IsOnGround() && mover->GetFeet().z > ladder->m_top.z - mover->GetStepHeight();
}

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
	: m_ladder( ladder ), m_bGoingUp( goingUp ), m_vecLastProgressPos( vec3_origin ), m_flLastProgressTime( 0.0f )
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
	m_timeoutTimer.Start( NEO_LADDER_APPROACH_TIMEOUT );
	m_vecLastProgressPos = me->GetLocomotionInterface()->GetFeet();
	m_flLastProgressTime = gpGlobals->curtime;

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

	// patch 99: progress along the approach
	const Vector &vecFeetNow = me->GetLocomotionInterface()->GetFeet();
	if ( ( vecFeetNow - m_vecLastProgressPos ).AsVector2D().IsLengthGreaterThan( NEO_LADDER_PROGRESS_STEP ) )
	{
		m_vecLastProgressPos = vecFeetNow;
		m_flLastProgressTime = gpGlobals->curtime;
	}

	// patch 105: beside the top, against the wall - held by the ladder's side, which is no mount
	Vector vecEdgeOut;
	// (107: only while the step out makes progress - in a slot shallower than the step the bot would press the far wall)
	const bool bBesideEdge = m_bGoingUp
		? ( neo_bot_ladder_clear_edge_up.GetBool() && !me->IsOnLadder() && gpGlobals->curtime - m_flLastProgressTime < NEO_LADDER_STALL_TIME
			&& NeoBesideLadderEdge( me, m_ladder, true, &vecEdgeOut ) )
		: ( neo_bot_ladder_clear_edge.GetBool() && NeoBesideLadderEdge( me, m_ladder, false, &vecEdgeOut ) );

	// patch 82: already on this ladder - climb it, before the locomotion adopts it the other way
	if ( neo_bot_ladder_mount_on_contact.GetBool() && me->IsOnLadder() && !bBesideEdge && !NeoStillOnTopLanding( me, m_ladder, m_bGoingUp, NEO_LADDER_APPROACH_TIMEOUT - m_timeoutTimer.GetRemainingTime(), gpGlobals->curtime - m_flLastProgressTime ) )
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

	// patch 89: a descent that has just stepped off its landing is falling past the face it means to grab, not done
	const bool bMountingInAir = !m_bGoingUp && neo_bot_ladder_descent_airborne.GetBool() && !mover->IsOnGround()
		&& ( m_ladder->GetPosAtHeight( myPos.z ) - myPos ).AsVector2D().IsLengthLessThan( NEO_LADDER_CONTACT_RANGE );
	if ( distToExitSq < distToEntrySq && !bMountingInAir )
	{
		NeoLogLadderApproach( me, m_ladder, m_bGoingUp, "Closer to ladder exit than entry, assuming goal reached accidentally" );
		return Done( "Closer to ladder exit than entry, assuming goal reached accidentally" );
	}

	// Are we climbing up or down the ladder?
	Vector targetPos = m_bGoingUp ? m_ladder->m_bottom : m_ladder->m_top;

	// patch 73: side-landing ladders are mounted going down from in front of the face (patch 84: every descent)
	const bool bSideLanding = !m_ladder->m_topForwardArea && !m_ladder->m_topBehindArea;
	const bool bSideMount = !m_bGoingUp && ( neo_bot_ladder_front_mount.GetBool()
		|| ( neo_bot_ladder_side_mount.GetBool() && bSideLanding ) );
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
	body->AimHeadTowards( lookTarget, IBody::MANDATORY, 0.1f, nullptr, "Stare at ladder center" );

	// patch 105: out from the wall first, then round the ladder's edge to the front of its face
	if ( bBesideEdge )
	{
		mover->Approach( vecEdgeOut );
		return Continue();
	}

	if ( me->IsDebugging( NEXTBOT_PATH ) )
	{
		NDebugOverlay::Cross3D( targetPos, 5.0f, 255, 255, 0, true, 0.1f );
		NDebugOverlay::Line( myPos, targetPos, 255, 255, 0, true, 0.1f );
	}

	// Are we aligned and close enough to mount the ladder?
	// (patch 73 v2: a side-landing descent must first walk off the landing to the point in front of the face - that point
	// is over the shaft, and at the usual 25 u mount range the bot was still on the landing, pushing into the wall)
	if ( range >= ( bSideMount ? 6.0f : MOUNT_RANGE ) )
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
	else
	{
		// Within mount range - check if aligned to start climbing
		bool onLadder = me->IsOnLadder();
		if ( onLadder && !NeoStillOnTopLanding( me, m_ladder, m_bGoingUp, NEO_LADDER_APPROACH_TIMEOUT - m_timeoutTimer.GetRemainingTime(), gpGlobals->curtime - m_flLastProgressTime ) )
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
		else if ( !m_bGoingUp || dot < ALIGN_DOT_THRESHOLD )
		{
			// Aligned (or going down), push forward to attach to the ladder
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
		else if ( m_bGoingUp && neo_bot_ladder_close_align.GetBool() )
		{
			// patch 103: close but off to the side - step round to the front of the face first; walking straight at the
			// foot from the side touches the ladder's edge, the engine grabs it there, and on a narrow ladder the climb
			// then slips off the side
			Vector front = targetPos;
			front.x += ladderNormal2D.x * NEO_LADDER_FRONT_STANDOFF;
			front.y += ladderNormal2D.y * NEO_LADDER_FRONT_STANDOFF;
			mover->Approach( front );
		}
		else
		{
			// Close but not aligned - continue approaching to align
			mover->Approach( targetPos );
		}
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
