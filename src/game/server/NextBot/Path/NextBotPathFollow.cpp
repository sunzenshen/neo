// NextBotPathFollow.cpp
// Path following
// Author: Michael Booth, April 2005
//========= Copyright Valve Corporation, All rights reserved. ============//

#include "cbase.h"
#include "engine/IStaticPropMgr.h"

#include "BasePropDoor.h"
#ifdef NEO
#include "props.h"
#endif

#include "nav_mesh.h"
#include "NextBot.h"
#include "NextBotPathFollow.h"
#include "NextBotUtil.h"

#include "NextBotLocomotionInterface.h"
#include "NextBotBodyInterface.h"
#include "NextBotVisionInterface.h"

#include "tier0/vprof.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

ConVar NextBotSpeedLookAheadRange( "nb_speed_look_ahead_range", "150", FCVAR_CHEAT );
ConVar NextBotGoalLookAheadRange( "nb_goal_look_ahead_range", "50", FCVAR_CHEAT );
ConVar NextBotLadderAlignRange( "nb_ladder_align_range", "50", FCVAR_CHEAT );

ConVar NextBotAllowAvoiding( "nb_allow_avoiding", "1", FCVAR_CHEAT );
ConVar NextBotAllowClimbing( "nb_allow_climbing", "1", FCVAR_CHEAT );
ConVar NextBotAllowGapJumping( "nb_allow_gap_jumping", "1", FCVAR_CHEAT );

ConVar NextBotDebugClimbing( "nb_debug_climbing", "0", FCVAR_CHEAT );

#ifdef NEO
// How far from its feet a bot that landed looks for a nav area, the range UpdateLastKnownArea() searches
static const float LANDING_NAV_AREA_RANGE = 50.0f;
#endif

#ifdef NEO
// NEO-HARNESS-TEMP research arm (2026-09-23, patch 58): trust a CLIMB_UP link once at its launch point.
// 1 = climb as soon as the bot is within a hull width of the launch point; 2 = only when it is stuck there.
ConVar neo_bot_climb_trust_nav( "neo_bot_climb_trust_nav", "0", FCVAR_CHEAT,
	"Research: at a CLIMB_UP launch point, climb without the geometric ledge search (1 = always, 2 = when stuck)" );
extern ConVar sv_neo_forensic_log;
#endif

#ifdef NEO
// NEO-HARNESS-TEMP research arm (2026-09-22, ntre notes/prop-collision-initiative): steer round prop
// entities instead of pressing into them; the PR form has no cvar.
ConVar neo_bot_avoid_prop_entities( "neo_bot_avoid_prop_entities", "0", FCVAR_CHEAT,
	"Research: avoid whiskers see breakable props, and a prop squarely ahead is walked round" );

// Props the nav mesh cannot account for: nav_generate skips every entity, and they can be moved,
// broken or animated, so a path can run straight through one.
static bool NeoIsPropObstacle( CBaseEntity *ent )
{
	if ( !ent || ent->IsWorld() || ent->IsPlayer() || ent->MyCombatCharacterPointer() )
	{
		return false;
	}

	return dynamic_cast< CBreakableProp * >( ent ) != NULL || FClassnameIs( ent, "phys_bone_follower" )
		|| FClassnameIs( ent, "func_breakable" ) || FClassnameIs( ent, "func_physbox" );
}

// Aim a hull half-width past whichever side of the prop is nearer the goal and walkable, level
// with its far edge:        goal
//                   o   [ prop ]   o     <- the two candidates
//                          bot
static bool NeoPropDetour( INextBot *bot, CBaseEntity *prop, const Vector &goalPos, const Vector &forward,
	const Vector &left, Vector *detour )
{
	ILocomotion *mover = bot->GetLocomotionInterface();
	const Vector &feet = mover->GetFeet();
	Vector mins, maxs;
	prop->CollisionProp()->WorldSpaceAABB( &mins, &maxs );

	float sideLo = FLT_MAX, sideHi = -FLT_MAX, ahead = 0.0f;
	for ( int i = 0; i < 4; ++i )
	{
		const Vector corner( ( i & 1 ) ? maxs.x : mins.x, ( i & 2 ) ? maxs.y : mins.y, feet.z );
		const Vector to = corner - feet;
		const float side = to.x * left.x + to.y * left.y;
		sideLo = MIN( sideLo, side );
		sideHi = MAX( sideHi, side );
		ahead = MAX( ahead, to.x * forward.x + to.y * forward.y );
	}

	const float margin = bot->GetBodyInterface()->GetHullWidth() / 2.0f + 4.0f;
	Vector candidates[2] = {
		feet + ahead * forward + ( sideHi + margin ) * left,
		feet + ahead * forward + ( sideLo - margin ) * left,
	};
	if ( ( candidates[1] - goalPos ).LengthSqr() < ( candidates[0] - goalPos ).LengthSqr() )
	{
		V_swap( candidates[0], candidates[1] );
	}

	for ( int i = 0; i < 2; ++i )
	{
		if ( mover->IsPotentiallyTraversable( feet, candidates[i], ILocomotion::IMMEDIATELY ) )
		{
			*detour = candidates[i];
			return true;
		}
	}

	return false;
}

// NEO-HARNESS-TEMP research arm (2026-09-23, ntre notes/navmesh-release-roadmap): the follower's
// skip-ahead checks trust IsPotentiallyTraversable, whose probe is a quarter hull wide, so a skip can
// go through a holed wall, grate or railing a player cannot pass; the bot then presses into it.
ConVar neo_bot_path_skip_wide( "neo_bot_path_skip_wide", "0", FCVAR_CHEAT,
	"Research: the path follower only skips ahead where a nearly full-width hull fits (2 = and its full current height)" );

// patch 62: a path point entering a NAV_MESH_PRECISE area is a waypoint the bot must reach before heading further,
// so a mesh author can stop skip-ahead cutting a corner (a stair's handrail end, a pillar) by marking the area past it
// NEO-HARNESS-TEMP research arm (2026-09-24, patch 70): see the "goal too high" check in PathFollower::Update
ConVar neo_bot_felloff_steep( "neo_bot_felloff_steep", "0", FCVAR_CHEAT,
	"Research: a goal more than a jump above and steeper than 45 degrees away is acted on at once instead of after the stuck timeout - 1 = take the next segment or re-path, 2 = only take the next segment when it is reachable" );

ConVar neo_bot_path_precise_waypoint( "neo_bot_path_precise_waypoint", "0", FCVAR_CHEAT,
	"Research: 1 = skip-ahead never passes a path point that enters a NAV_MESH_PRECISE area" );

// NEO-HARNESS-TEMP research arm (2026-09-25, patch 95): the ladder behaviours (CNEOBotLadderApproach / Climb, started by
// the tactical monitor) mount ladders; handing one the bot is not yet on to the locomotion here as well puts it in an
// approach state the monitor never sees, since the goal has moved past the ladder (transit ladder 2's top, rg2)
ConVar neo_bot_ladder_follower_defer( "neo_bot_ladder_follower_defer", "0", FCVAR_CHEAT,
	"Research: the path follower leaves mounting a ladder the bot is not on to the ladder behaviours" );

// NEO-HARNESS-TEMP research arm (2026-09-25, patch 97): a bot off its path walks the straight line to the next waypoint,
// and that line can cross a drop the path never planned (patch 96's traces: apparatus 59 of 64 matched falls left the
// edge heading for the path goal, 3404 -> a gap-jump waypoint in 8352 across the 80 u void). When the floor ahead ends
// over such a drop, re-plan from where the bot is instead.
ConVar neo_bot_path_void_guard( "neo_bot_path_void_guard", "0", FCVAR_CHEAT,
	"Research: re-plan when the floor ahead ends over a drop the current ON_GROUND path segment does not plan (2 = not on the way to a ladder)" );

// NEO-HARNESS-TEMP research arm (2026-09-25, patch 108): a JUMP_OVER_GAP goal is exempt from the guard, rightly while the bot
// stands where the jump starts; a bot off its path heads for the far side's waypoint from elsewhere and walks into the gap
// without jumping (apparatus 3404: the strip the 8352 gap jump lands on, whose bots still steer at the 3404 -> 8352 gap
// point of a path planned from the far side - ~1 fall a match). Guard such a goal too.
ConVar neo_bot_path_void_guard_gap( "neo_bot_path_void_guard_gap", "0", FCVAR_CHEAT,
	"Research: the void guard also covers a gap-jump goal when the bot is not on the area the jump starts from"
	" (2 = also re-plans, without the probe, from off the path towards a goal more than a step up; 3 = measures that step from the path segment, not the steering point)" );

// NEO-HARNESS-TEMP research arm (2026-09-26, patch 110): a path recomputed while the bot is in the air over a gap starts
// from the area the jump left (GetLastKnownArea), so its goal is the take-off behind the bot. Landed low on the far side,
// the take-off is more than a step up and IsAtGoal never passes it, so the bot walks back into the gap (ap3: apparatus
// 3404, all 6 traced walk-backs; the path was 0.00-0.14 s old at the jump - a combat-sound / seek re-plan mid-air).
// The jump is already made: take its landing as the goal.
ConVar neo_bot_path_gap_resume( "neo_bot_path_gap_resume", "0", FCVAR_CHEAT,
	"Research: a gap-jump goal whose landing area the bot is already on, or whose gap the bot is jumping, moves on to the landing" );

static const float NEO_VOID_GUARD_DROP = 200.0f;		// no floor this far under the probe = a drop the path must plan
static const float NEO_VOID_GUARD_LEAD = 12.0f;			// probe this far past the hull's leading edge
static const float NEO_VOID_GUARD_HALF = 12.0f;			// probe box half-width: a crack the hull bridges is not a void
static const float NEO_VOID_GUARD_INTERVAL = 1.0f;		// per bot, at most one re-plan this often
static float s_flNeoVoidGuardNext[ MAX_PLAYERS + 1 ];

// a ladder end: the ladder behaviours mount it, and the drop beside it is the shaft the ladder is for
static bool NeoNearLadderEnd( const Path::Segment *goal, const Path::Segment *next )
{
	if ( next && next->ladder )
	{
		return true;
	}

	if ( !goal->area )
	{
		return false;
	}

	return goal->area->GetLadders( CNavLadder::LADDER_UP )->Count() > 0 || goal->area->GetLadders( CNavLadder::LADDER_DOWN )->Count() > 0;
}

static bool NeoVoidAhead( INextBot *bot, const Path::Segment *goal, const Path::Segment *prior, const Path::Segment *next, const Vector &goalPos )
{
	ILocomotion *mover = bot->GetLocomotionInterface();

	// patch 108: off the area a gap jump starts from, its waypoint is a walk into the gap
	const bool bOffPath = goal && prior && prior->area && bot->GetEntity()->GetLastKnownArea() != prior->area;
	const bool bOffPathGap = neo_bot_path_void_guard_gap.GetBool() && bOffPath && goal->type == Path::JUMP_OVER_GAP;
	if ( !goal || ( goal->type != Path::ON_GROUND && !bOffPathGap ) || !mover->IsOnGround() || mover->IsClimbingOrJumping() )
	{
		return false;
	}

	// v2: from off the path, a goal more than a step up needs a jump the bot is not lined up for - re-plan without the probe,
	// which a gap narrower than its box never reads as a void (apparatus 8340: a duct top ending 25 u short of the next)
	// (v3: against the segment's own height - Avoid() hands a steering point at the bot's feet height, which hid the rise;
	// ap3: 3404's bots walking back to the 8352 take-off never tripped v2)
	const float flGoalZ = ( neo_bot_path_void_guard_gap.GetInt() >= 3 ) ? goal->pos.z : goalPos.z;
	const bool bOffPathRise = neo_bot_path_void_guard_gap.GetInt() >= 2 && bOffPath
		&& flGoalZ - mover->GetFeet().z > mover->GetStepHeight();

	// v2 (rg4: v1 took the ladder descents that end back at the top from 42 to 59 %): not on the way to a ladder
	if ( neo_bot_path_void_guard.GetInt() >= 2 && NeoNearLadderEnd( goal, next ) )
	{
		return false;
	}

	const int idx = bot->GetEntity()->entindex();
	if ( idx <= 0 || idx > MAX_PLAYERS || gpGlobals->curtime < s_flNeoVoidGuardNext[idx] )
	{
		return false;
	}

	const Vector &feet = mover->GetFeet();
	Vector dir = goalPos - feet;
	dir.z = 0.0f;
	const float flLead = bot->GetBodyInterface()->GetHullWidth() * 0.5f + NEO_VOID_GUARD_LEAD;
	if ( dir.NormalizeInPlace() < flLead )
	{
		return false;
	}

	const Vector probe = feet + dir * flLead;
	const Vector mins( -NEO_VOID_GUARD_HALF, -NEO_VOID_GUARD_HALF, 0.0f );
	const Vector maxs( NEO_VOID_GUARD_HALF, NEO_VOID_GUARD_HALF, 2.0f );
	trace_t tr;
	UTIL_TraceHull( probe + Vector( 0.0f, 0.0f, mover->GetStepHeight() ), probe - Vector( 0.0f, 0.0f, NEO_VOID_GUARD_DROP ), mins, maxs,
		MASK_PLAYERSOLID, bot->GetEntity(), COLLISION_GROUP_PLAYER_MOVEMENT, &tr );
	if ( !bOffPathRise && ( tr.startsolid || tr.fraction < 1.0f ) )
	{
		return false;
	}

	s_flNeoVoidGuardNext[idx] = gpGlobals->curtime + NEO_VOID_GUARD_INTERVAL;
	if ( sv_neo_forensic_log.GetBool() )
	{
		const CNavArea *pArea = bot->GetEntity()->GetLastKnownArea();
		Msg( "NEO_FORENSIC_VOIDGUARD t=%.2f p=%d pos=%.0f,%.0f,%.0f area=%d goal=%.0f,%.0f,%.0f goalarea=%d\n",
			gpGlobals->curtime, idx, feet.x, feet.y, feet.z, pArea ? (int)pArea->GetID() : -1,
			goalPos.x, goalPos.y, goalPos.z, goal->area ? (int)goal->area->GetID() : -1 );
	}
	return true;
}

static bool NeoIsSkipTraversable( INextBot *bot, const Vector &from, const Vector &to )
{
	ILocomotion *mover = bot->GetLocomotionInterface();
	if ( !mover->IsPotentiallyTraversable( from, to ) )
	{
		return false;
	}

	if ( !neo_bot_path_skip_wide.GetBool() )
	{
		return true;
	}

	// the probe's vertical span (step height to crouch height), nearly the full hull width
	IBody *body = bot->GetBodyInterface();
	const float halfWidth = 0.5f * body->GetHullWidth() - 1.0f;
	const Vector hullMin( -halfWidth, -halfWidth, mover->GetStepHeight() );
	// patch 58: mode 2 also spans the body's current height - a crouch-height probe passes lintels,
	// hanging signs and raised crates that a standing hull hits (the hand-reshaping pass found a dozen)
	const float probeTop = neo_bot_path_skip_wide.GetInt() >= 2 ? body->GetHullHeight() - 1.0f : body->GetCrouchHullHeight();
	const Vector hullMax( halfWidth, halfWidth, probeTop );
	NextBotTraversableTraceFilter filter( bot, ILocomotion::EVENTUALLY );
	trace_t result;
	mover->TraceHull( from, to, hullMin, hullMax, body->GetSolidMask(), &filter, &result );

	// a hull already in contact tells nothing either way: keep the probe's answer
	return result.startsolid || result.fraction >= 1.0f;
}
#endif

#ifdef NEO
// How far from its feet a bot that landed looks for a nav area, the range UpdateLastKnownArea() searches
static const float LANDING_NAV_AREA_RANGE = 50.0f;
#endif


//--------------------------------------------------------------------------------------------------------------
/**
 * Constructor
 */
PathFollower::PathFollower( void )
{
	m_goal = NULL;
	m_didAvoidCheck = false;

	m_avoidTimer.Invalidate();
	m_waitTimer.Invalidate();
	m_hindrance = NULL;

	m_minLookAheadRange = -1.0f;

	// was 10.0f for L4D - need a better solution here (MSB 5/15/09)
	m_goalTolerance = 25.0f;

#ifdef NEO
	m_wasOnGround = true;
#endif
}


//--------------------------------------------------------------------------------------------------------------
class CDetachPath
{
public:
	CDetachPath( PathFollower *path )
	{
		m_path = path;
	}

	bool operator() ( INextBot *bot )
	{
		bot->NotifyPathDestruction( m_path );
		return true;
	}

	PathFollower *m_path;
};

//--------------------------------------------------------------------------------------------------------------
PathFollower::~PathFollower()
{
	// allow bots to detach pointer to me
	CDetachPath detach( this );
	TheNextBots().ForEachBot( detach );
}


//--------------------------------------------------------------------------------------------------------------
/**
 * When the path is invalidated, the follower is also reset
 */
void PathFollower::Invalidate( void )
{
	// extend
	Path::Invalidate();

	m_goal = NULL;

	m_avoidTimer.Invalidate();
	m_waitTimer.Invalidate();
	m_hindrance = NULL;
}


//--------------------------------------------------------------------------------------------------------------
/**
 * Invoked when the path is (re)computed (path is valid at the time of this call)
 */
void PathFollower::OnPathChanged( INextBot *bot, Path::ResultType result )
{
	// start from the beginning
	m_goal = FirstSegment();
	m_result = result;
}


//--------------------------------------------------------------------------------------------------------------
/**
 * Adjust speed based on path curvature
 */
void PathFollower::AdjustSpeed( INextBot *bot )
{
	ILocomotion *mover = bot->GetLocomotionInterface();

	// if we're coming up on a gap jump, or we're in the air, use maximum speed
	if ( ( m_goal && m_goal->type == JUMP_OVER_GAP ) || !mover->IsOnGround() )
	{
		mover->SetDesiredSpeed( mover->GetRunSpeed() );
		return;
	}

	MoveCursorToClosestPosition( bot->GetPosition() );
	const Path::Data &data = GetCursorData();
	
	// speed based on curvature
	mover->SetDesiredSpeed( mover->GetRunSpeed() + fabs( data.curvature ) * ( mover->GetWalkSpeed() - mover->GetRunSpeed() ) );
}


//--------------------------------------------------------------------------------------------------------------
/**
 * Return true if reached current goal along path
 * NOTE: Ladder goals are handled elsewhere
 */
bool PathFollower::IsAtGoal( INextBot *bot ) const
{
	VPROF_BUDGET( "PathFollower::IsAtGoal", "NextBot" );

	ILocomotion *mover = bot->GetLocomotionInterface();
	IBody *body = bot->GetBodyInterface();

	//
	// m_goal is the node we are moving toward along the path
	// current is the node just behind us
	//
	const Segment *current = PriorSegment( m_goal );
	Vector toGoal = m_goal->pos - mover->GetFeet();

// 	if ( m_goal->type == JUMP_OVER_GAP && !mover->IsOnGround() )
// 	{
// 		// jumping over a gap, don't skip ahead until we land
// 		return false;
// 	}

	if ( current == NULL )
	{
		// passed goal
		return true;
	}
	else if ( m_goal->type == DROP_DOWN )
	{
		// m_goal is the top of the drop-down, and the following segment is the landing point
		const Segment *landing = NextSegment( m_goal );

		if ( landing == NULL )
		{
			// passed goal or corrupt path
			return true;
		}		
		else
		{
			// did we reach the ground
			if ( mover->GetFeet().z - landing->pos.z < mover->GetStepHeight() )
			{
				// reached goal
				return true;
			}
		}
				
		/// @todo: it is possible to fall into a bad place and get stuck - should move back onto the path
		
	}
	else if ( m_goal->type == CLIMB_UP )
	{
		// once jump is started, assume it is successful, since
		// nav mesh may be substantially off from actual ground height at landing
		const Segment *landing = NextSegment( m_goal );

		if ( landing == NULL )
		{
			// passed goal or corrupt path
			return true;
		}		
		else if ( /*!mover->IsOnGround() && */ mover->GetFeet().z > m_goal->pos.z + mover->GetStepHeight() )
		{
			// we're off the ground, presumably climbing - assume we reached the goal
			return true;
		}
		/* This breaks infected climbing up holes in the ceiling - they can get within 2D range of m_goal before finding a ledge to climb up to
		else if ( mover->IsOnGround() )
		{
			// proximity check
			// Z delta can be anything, since we may be climbing over a tall fence, a physics prop, etc.
			const float rangeTolerance = 10.0f;
			if ( toGoal.AsVector2D().IsLengthLessThan( rangeTolerance ) )
			{
				// reached goal
				return true;
			}
		}
		*/
	}
	else
	{
		const Segment *next = NextSegment( m_goal );

		if ( next )
		{
			// because mover may be off the path, check if it crossed the plane of the goal
			// check against average of current and next forward vectors
			Vector2D dividingPlane;

			if ( current->ladder )
			{
				dividingPlane = m_goal->forward.AsVector2D();
			}
			else
			{
				dividingPlane = current->forward.AsVector2D() + m_goal->forward.AsVector2D();
			}

			if ( DotProduct2D( toGoal.AsVector2D(), dividingPlane ) < 0.0001f &&
				 abs( toGoal.z ) < body->GetStandHullHeight() )
			{	
				// only skip higher Z goal if next goal is directly reachable
				// can't use this for positions below us because we need to be able
				// to climb over random objects along our path that we can't actually
				// move *through*
#ifdef NEO
				if ( toGoal.z < mover->GetStepHeight() && ( NeoIsSkipTraversable( bot, mover->GetFeet(), next->pos ) && !mover->HasPotentialGap( mover->GetFeet(), next->pos ) ) )
#else
				if ( toGoal.z < mover->GetStepHeight() && ( mover->IsPotentiallyTraversable( mover->GetFeet(), next->pos ) && !mover->HasPotentialGap( mover->GetFeet(), next->pos ) ) )
#endif
				{
					// passed goal
					return true;
				}
			}
		}

		// proximity check
		// Z delta can be anything, since we may be climbing over a tall fence, a physics prop, etc.
		if ( toGoal.AsVector2D().IsLengthLessThan( m_goalTolerance ) )
		{
			// reached goal
			return true;
		}
	}

	return false;	
}


//--------------------------------------------------------------------------------------------------------------
/**
 * Move bot along ladder. Return true if ladder motion is in progress, false if complete.
 */
bool PathFollower::LadderUpdate( INextBot *bot )
{
	VPROF_BUDGET( "PathFollower::LadderUpdate", "NextBot" );

	ILocomotion *mover = bot->GetLocomotionInterface();
	IBody *body = bot->GetBodyInterface();
	
	if ( mover->IsUsingLadder() )
	{
		// wait for locomotor to finish traversing ladder
		return true;
	}

	if ( m_goal->ladder == NULL )
	{
		// Check if we have somehow ended up on a ladder, if so, and its a tall down-ladder we are expecting, jump the path ahead.
		// This happens for players, who run off ledges and the gamemovement sticks them onto ladders.  We only care about
		// tall down-ladders, because up ladders work without this, and short ladders aren't dangerous to miss and drop down
		// instead of climbing down.
		if ( bot->GetEntity()->GetMoveType() == MOVETYPE_LADDER )
		{
			// 'current' is the segment we are on/just passed over
			const Segment *current = PriorSegment( m_goal );
			if ( current == NULL )
			{
				return false;
			}

			// Start with current, the segment we are currently traversing.  Skip the distance check for that segment, because
			// the pos is (hopefully) behind us.  And if it's a long path segment, it's already outside the climbLookAheadRange,
			// and thus it would prevent us looking at m_goal and further for imminent planned climbs.
			// 'current' is the segment we are on/just passed over
			const float ladderLookAheadRange = 50.0f;
			for( const Segment *s = current; s; s = NextSegment( s ) )
			{
				if ( s != current && ( s->pos - mover->GetFeet() ).AsVector2D().IsLengthGreaterThan( ladderLookAheadRange ) )
				{
					break;
				}

				// Only consider reasonably tall down ladders - if we don't grab onto a short ladder, it hopefully won't be a bad fall.
				if ( s->ladder != NULL && s->how == GO_LADDER_DOWN && s->ladder->m_length > mover->GetMaxJumpHeight() )
				{
					float destinationHeightDelta = s->pos.z - mover->GetFeet().z;
					if ( fabs(destinationHeightDelta) < mover->GetMaxJumpHeight() )
					{
						// Advance the goal, and fall through to the normal codepath.
						m_goal = s;
						break;
					}
				}
			}
		}

		if ( m_goal->ladder == NULL )
		{
			// no ladder to use
			return false;
		}
	}
	

	// start using the ladder
	const float mountRange = 25.0f;

	if ( m_goal->how == GO_LADDER_UP )
	{
		// check if we're off the ladder and at the top
		if ( !mover->IsUsingLadder() && mover->GetFeet().z > m_goal->ladder->m_top.z - mover->GetStepHeight() )
		{
			// we're up
			m_goal = NextSegment( m_goal );
			return false;
		}

		// approach the ladder
		Vector2D to = ( m_goal->ladder->m_bottom - mover->GetFeet() ).AsVector2D();

		body->AimHeadTowards( m_goal->ladder->m_top - 50.0f * m_goal->ladder->GetNormal() + Vector( 0, 0, body->GetCrouchHullHeight() ), 
							  IBody::CRITICAL, 
							  2.0f, 
							  NULL,
							  "Mounting upward ladder" );

		float range = to.NormalizeInPlace();
		if ( range < NextBotLadderAlignRange.GetFloat() )
		{
			// getting close - line up
			Vector2D ladderNormal2D = m_goal->ladder->GetNormal().AsVector2D();
			float dot = DotProduct2D( ladderNormal2D, to );

			const float cos5 = 0.9f;
			if ( dot < -cos5 )
			{
				// lined up - continue approach
				mover->Approach( m_goal->ladder->m_bottom );

#ifdef NEO
				// patch 95: not before the bot is on it
				const bool bDeferUp = neo_bot_ladder_follower_defer.GetBool() && bot->GetEntity()->GetMoveType() != MOVETYPE_LADDER;
				if ( range < mountRange && !bDeferUp )
#else
				if ( range < mountRange )
#endif
				{
					// go up ladder
					mover->ClimbLadder( m_goal->ladder, m_goal->area );
				}				
			}
			else
			{
				// rotate around ladder and maintain distance from it
				Vector myPerp( -to.y, to.x, 0.0f );
				Vector2D ladderPerp2D( -ladderNormal2D.y, ladderNormal2D.x );

				Vector goal = m_goal->ladder->m_bottom;
				
				float alignRange = NextBotLadderAlignRange.GetFloat();
				
				if ( dot < 0.0f )
				{
					// we are on the correct side of the ladder
					// align range should drop off as we reach alignment
					alignRange = mountRange + (1.0f + dot) * (alignRange - mountRange);
				}
				
				goal.x -= alignRange * to.x;
				goal.y -= alignRange * to.y;				
				
				if ( DotProduct2D( to, ladderPerp2D ) < 0.0f )
				{
					goal += 10.0f * myPerp;
				}
				else
				{
					goal -= 10.0f * myPerp;
				}
				
				mover->Approach( goal );
			}
		}
		else
		{
			// approach the base of the ladder - use normal path following in case there are jumps/climbs on the way to the ladder
			return false;
		}
	}
	else	// go down ladder
	{
		// check if we fell off and are now below the ladder
		if ( mover->GetFeet().z < m_goal->ladder->m_bottom.z + mover->GetStepHeight() )
		{
			// we fell
			m_goal = NextSegment( m_goal );
		}
		else
		{
			// approach the ladder
			Vector mountPoint = m_goal->ladder->m_top + 0.5f * body->GetHullWidth() * m_goal->ladder->GetNormal();
			Vector2D to = ( mountPoint - mover->GetFeet() ).AsVector2D();

			if ( bot->IsDebugging( NEXTBOT_PATH ) )
			{
				const float size = 5.0f;	
				NDebugOverlay::Sphere( mountPoint, size, 255, 0, 255, true, 0.1f );			
			}

			body->AimHeadTowards( m_goal->ladder->m_bottom + 50.0f * m_goal->ladder->GetNormal() + Vector( 0, 0, body->GetCrouchHullHeight() ), 
								  IBody::CRITICAL, 
								  1.0f, 
								  NULL,
								  "Mounting downward ladder" );

			float range = to.NormalizeInPlace();

			// Approach the top of the ladder.  If we're already on the ladder, start descending.
#ifdef NEO
			// patch 95: in mount range but not on the ladder - keep walking to it; the ladder behaviours mount it
			const bool bOnLadder = bot->GetEntity()->GetMoveType() == MOVETYPE_LADDER;
			if ( bOnLadder || ( range < mountRange && !neo_bot_ladder_follower_defer.GetBool() ) )
#else
			if ( range < mountRange || bot->GetEntity()->GetMoveType() == MOVETYPE_LADDER )
#endif
			{
				// go down ladder
				mover->DescendLadder( m_goal->ladder, m_goal->area );

				// increment goal segment since locomotor will move us along the ladder
				m_goal = NextSegment( m_goal );
			}
			else
			{
				// approach the top of the ladder - use normal path following in case there are jumps/climbs on the way to the ladder
				return false;
			}
		}
	}

	return true;
}


//--------------------------------------------------------------------------------------------------------------
/**
 * Check if we have reached our current path goal and
 * iterate to next goal or finish the path
 */
bool PathFollower::CheckProgress( INextBot *bot )
{
	ILocomotion *mover = bot->GetLocomotionInterface();

#ifdef NEO
	// patch 110: the gap was already jumped (a path recomputed mid-air starts from the take-off area)
	if ( neo_bot_path_gap_resume.GetBool() && m_goal && m_goal->type == JUMP_OVER_GAP )
	{
		const Path::Segment *landing = NextSegment( m_goal );
		const Path::Segment *takeoff = PriorSegment( m_goal );
		const CNavArea *pMyArea = bot->GetEntity()->GetLastKnownArea();
		if ( landing && landing->area && ( !takeoff || takeoff->area != landing->area )
			&& ( mover->IsJumpingAcrossGap() || pMyArea == landing->area ) )
		{
			if ( sv_neo_forensic_log.GetBool() )
			{
				const Vector &feet = mover->GetFeet();
				Msg( "NEO_FORENSIC_GAPRESUME t=%.2f p=%d pos=%.0f,%.0f,%.0f area=%d air=%d gap=%.0f,%.0f,%.0f landingarea=%d age=%.2f\n",
					gpGlobals->curtime, bot->GetEntity()->entindex(), feet.x, feet.y, feet.z, pMyArea ? (int)pMyArea->GetID() : -1,
					mover->IsJumpingAcrossGap() ? 1 : 0, m_goal->pos.x, m_goal->pos.y, m_goal->pos.z, (int)landing->area->GetID(), GetAge() );
			}
			m_goal = landing;
		}
	}
#endif

	// skip nearby goal points that are redundant to smooth path following motion
	const Path::Segment *pSkipToGoal = NULL;
	if ( m_minLookAheadRange > 0.0f )
	{
		pSkipToGoal = m_goal;
		const Vector &myFeet = mover->GetFeet();
		while( pSkipToGoal && pSkipToGoal->type == ON_GROUND && mover->IsOnGround() )
		{
			if ( ( pSkipToGoal->pos - myFeet ).IsLengthLessThan( m_minLookAheadRange ) )
			{
#ifdef NEO
				if ( pSkipToGoal->area && pSkipToGoal->area->HasAttributes( NAV_MESH_PRECISE ) )
				{
					// don't skip segments or take shortcuts in a PRECISE area
					break;
				}
#endif
				// goal is too close - step to next segment
				const Path::Segment *nextSegment = NextSegment( pSkipToGoal );

				if ( !nextSegment || nextSegment->type != ON_GROUND )
				{
					// can't skip ahead to next segment - head towards current goal
					break;
				}

				if ( nextSegment->pos.z > myFeet.z + mover->GetStepHeight() )
				{
					// going uphill or up stairs tends to cause problems if we skip ahead, so don't
					break;
				}

#ifdef DOTA_DLL
				if ( DotProduct( mover->GetMotionVector(), nextSegment->forward ) <= 0.1f )
				{
					// don't skip sharp turns
					break;
				}
#endif

				// can we reach the next path segment directly
#ifdef NEO
				if ( NeoIsSkipTraversable( bot, myFeet, nextSegment->pos ) && !mover->HasPotentialGap( myFeet, nextSegment->pos ) )
#else
				if ( mover->IsPotentiallyTraversable( myFeet, nextSegment->pos ) && !mover->HasPotentialGap( myFeet, nextSegment->pos ) )
#endif
				{
					pSkipToGoal = nextSegment;
				}
				else
				{
					// can't directly reach next segment - keep heading towards current goal
					break;
				}
			}
			else
			{
				// goal is farther than min lookahead
				break;
			}
		}

		// didn't find any goal to skip to
		if ( pSkipToGoal == m_goal )
		{
			pSkipToGoal = NULL;
		}
	}

	if ( IsAtGoal( bot ) )
	{
		// iterate to next segment of the path
		const Path::Segment *nextSegment = pSkipToGoal ? pSkipToGoal : NextSegment( m_goal );

		if ( nextSegment == NULL )
		{
			// must be on ground to complete the path
			if ( mover->IsOnGround() )
			{
				// the end of the path has been reached
				mover->GetBot()->OnMoveToSuccess( this );

				if ( bot->IsDebugging( NEXTBOT_PATH ) )
				{
					DevMsg( "PathFollower: OnMoveToSuccess\n" );
				}

				// don't invalidate if OnMoveToSuccess just recomputed a new path
				if ( GetAge() > 0.0f )
				{
					Invalidate();
				}

				return false;
			}
		}
		else
		{
			// keep moving
			m_goal = nextSegment;

			if ( bot->IsDebugging( NEXTBOT_PATH ) && !mover->IsPotentiallyTraversable( mover->GetFeet(), nextSegment->pos ) )
			{
				Warning( "PathFollower: path to my goal is blocked by something\n" );
				NDebugOverlay::Sphere( m_goal->pos, 5.f, 255, 0, 0, true, 3.f );
			}
		}
	}

	return true;
}


#ifdef NEO
//--------------------------------------------------------------------------------------------------------------
/**
 * Return true if we stand off the path, more than a step below it:
 * we walked or were pushed off a ledge the path does not drop from,
 * and the path cannot be followed from here
 */
bool PathFollower::HasFallenBelowPath( INextBot *bot ) const
{
	// a planned drop, gap jump, climb or ladder moves us off the path line on purpose
	if ( m_goal->type != ON_GROUND )
	{
		return false;
	}

	ILocomotion *mover = bot->GetLocomotionInterface();
	if ( mover->IsClimbingOrJumping() || mover->IsUsingLadder() )
	{
		return false;
	}

	const CNavArea *area = bot->GetEntity()->GetLastKnownArea();
	if ( !area )
	{
		return false;
	}

	// standing on an area of the path is on the path, whatever height its line runs at
	const Vector &feet = mover->GetFeet();
	if ( area->GetZ( feet ) - feet.z <= mover->GetStepHeight() )
	{
		for ( const Segment *s = FirstSegment(); s; s = NextSegment( s ) )
		{
			if ( s->area == area )
			{
				return false;
			}
		}
	}

	MoveCursorToClosestPosition( feet );
	return GetCursorData().pos.z - feet.z > mover->GetStepHeight();
}


//--------------------------------------------------------------------------------------------------------------
/**
 * Return true if a nav area lies near the bot's feet. Off the mesh its last known area is still
 * the one it fell from, and a path from there is the one it cannot follow
 */
static bool IsNearNavMesh( INextBot *bot )
{
	return TheNavMesh->GetNearestNavArea( bot->GetEntity(), GETNAVAREA_CHECK_GROUND | GETNAVAREA_CHECK_LOS, LANDING_NAV_AREA_RANGE ) != NULL;
}
#endif


//--------------------------------------------------------------------------------------------------------------
/**
 * Move mover along path
 */
void PathFollower::Update( INextBot *bot )
{
	VPROF_BUDGET( "PathFollower::Update", "NextBotSpiky" );

	// track most recent path followed
	bot->SetCurrentPath( this );


	ILocomotion *mover = bot->GetLocomotionInterface();
	
	if ( !IsValid() || m_goal == NULL )
	{
		return;
	}

	if ( !m_waitTimer.IsElapsed() )
	{
		// still waiting
		//mover->ClearStuckStatus( "Waiting for blocker to move" );
		return;
	}

//	m_didAvoidCheck = false;


	if ( LadderUpdate( bot ) )
	{
		// we are traversing a ladder
		return;
	}


	// adjust speed based on path curvature
	AdjustSpeed( bot );

	if ( CheckProgress( bot ) == false )
	{
		// goal reached
		return;
	}

	// use the direction towards the goal as 'forward' direction
	Vector forward = m_goal->pos - mover->GetFeet();

	if ( m_goal->type == CLIMB_UP )
	{
		const Segment *next = NextSegment( m_goal );
		if ( next )
		{
			// use landing of climb up as forward to help ledge detection
			forward = next->pos - mover->GetFeet();
		}
	}

	forward.z = 0.0f;

	float goalRange = forward.NormalizeInPlace();

	Vector left( -forward.y, forward.x, 0.0f );

	if ( left.IsZero() )
	{
		// if left is zero, forward must also be - path follow failure
		mover->GetBot()->OnMoveToFailure( this, FAIL_STUCK );

		// don't invalidate if OnMoveToFailure just recomputed a new path
		if ( GetAge() > 0.0f )
		{
			Invalidate();
		}

		if ( bot->IsDebugging( NEXTBOT_PATH ) )
		{
			DevMsg( "PathFollower: OnMoveToFailure( FAIL_STUCK ) because forward and left are ZERO\n" );
		}

		return;
	}

	// unit vectors must follow floor slope
	const Vector &normal = mover->GetGroundNormal();

	// get forward vector along floor
	forward = CrossProduct( left, normal );

	// correct the sideways vector
	left = CrossProduct( normal, forward );

	if ( bot->IsDebugging( NEXTBOT_PATH ) )
	{
		float axisSize = 25.0f;
		NDebugOverlay::Line( mover->GetFeet(), mover->GetFeet() + axisSize * forward, 255, 0, 0, true, 0.1f );
		NDebugOverlay::Line( mover->GetFeet(), mover->GetFeet() + axisSize * normal, 0, 255, 0, true, 0.1f );
		NDebugOverlay::Line( mover->GetFeet(), mover->GetFeet() + axisSize * left, 0, 0, 255, true, 0.1f );
	}

	// climb up ledges
	if ( !Climbing( bot, m_goal, forward, left, goalRange ) )
	{
		// a failed climb could mean an invalid path
		if ( !IsValid() )
		{
			return;
		}

		// jump over gaps
		JumpOverGaps( bot, m_goal, forward, left, goalRange );
	}

	// event callbacks from the above climbs and jumps may invalidate the path
	if ( !IsValid() )
	{
		return;
	}
	
#ifdef NEO
	// a bot that walks or is pushed off a ledge keeps a path it cannot follow from below,
	// so re-path on landing instead of pressing on until the stuck monitor fires
	const bool isOnGround = mover->IsOnGround();
	const bool hasJustLanded = isOnGround && !m_wasOnGround;
	m_wasOnGround = isOnGround;

	if ( hasJustLanded && HasFallenBelowPath( bot ) && IsNearNavMesh( bot ) )
	{
		mover->GetBot()->OnMoveToFailure( this, FAIL_FELL_OFF );

		// don't invalidate if OnMoveToFailure just recomputed a new path
		if ( GetAge() > 0.0f )
		{
			Invalidate();
		}

		if ( bot->IsDebugging( NEXTBOT_PATH ) )
		{
			DevMsg( "PathFollower: OnMoveToFailure( FAIL_FELL_OFF ) because we landed below the path\n" );
		}

		mover->ClearStuckStatus( "Landed below path" );
		return;
	}
#endif

	// if our movement goal is high above us, we must have fallen
	CNavArea *myArea = bot->GetEntity()->GetLastKnownArea();
	bool isOnStairs = ( myArea && myArea->HasAttributes( NAV_MESH_STAIRS ) );

	// limit too high distance to reasonable value for bots that can climb very high
	float tooHighDistance = mover->GetMaxJumpHeight();

	if ( !m_goal->ladder && !mover->IsClimbingOrJumping() && !isOnStairs && m_goal->pos.z > mover->GetFeet().z + tooHighDistance )
	{
		const float closeRange = 25.0f; // 75.0f;
		Vector2D to( mover->GetFeet().x - m_goal->pos.x, mover->GetFeet().y - m_goal->pos.y );
#ifdef NEO
		// NEO-HARNESS-TEMP research arm (patch 70): a goal more than a jump above the feet and steeper than 45 degrees
		// away can be neither walked nor jumped to - the bot carried on past a drop onto a narrow ledge and landed on
		// the floor below it (yard's 5680 / 5681). Stock code waits for the stuck timeout; act now instead.
		// Mode 1 also re-paths (FELL_OFF) when the next segment is out of reach too - that looped at ladder feet and on
		// vtol / terminal (fs1: FELL_OFF 5 -> 313 a match); mode 2 only ever moves the goal on to a reachable next segment.
		const bool bSteep = neo_bot_felloff_steep.GetBool() && mover->IsOnGround() &&
			( m_goal->pos.z - mover->GetFeet().z ) > to.Length() + mover->GetStepHeight();
		if ( bSteep && neo_bot_felloff_steep.GetInt() >= 2 && !mover->IsStuck() && !to.IsLengthLessThan( closeRange ) )
		{
			const Path::Segment *next = NextSegment( m_goal );
			if ( next && ( next->pos.z - mover->GetFeet().z <= mover->GetMaxJumpHeight() ) && mover->IsPotentiallyTraversable( mover->GetFeet(), next->pos ) )
			{
				m_goal = next;	// we already dropped past this goal
			}
		}
		else if ( mover->IsStuck() || to.IsLengthLessThan( closeRange ) || bSteep )
#else
		if ( mover->IsStuck() || to.IsLengthLessThan( closeRange ) )
#endif
		{
			// the goal is too high to reach

			// check if we can reach the next segment, in case this was a "jump down" situation
			const Path::Segment *next = NextSegment( m_goal );
			if ( mover->IsStuck() || !next || ( next->pos.z - mover->GetFeet().z > mover->GetMaxJumpHeight() ) || !mover->IsPotentiallyTraversable( mover->GetFeet(), next->pos ) )
			{
				// the next node is too high, too - we really did fall off the path
				mover->GetBot()->OnMoveToFailure( this, FAIL_FELL_OFF );

				// don't invalidate if OnMoveToFailure just recomputed a new path
				if ( GetAge() > 0.0f )
				{
					Invalidate();
				}

				if ( bot->IsDebugging( NEXTBOT_PATH ) )
				{
					DevMsg( "PathFollower: OnMoveToFailure( FAIL_FELL_OFF )\n" );
				}

				// reset stuck status since we're (likely) repathing anyways. otherwise, we could be stuck in a loop here and not move
				mover->ClearStuckStatus( "Fell off path" );

				return;
			}
#ifdef NEO
			else if ( bSteep )
			{
				// the next segment is reachable from here: we already dropped past this goal
				m_goal = next;
			}
#endif
		}
	}


	Vector goalPos = m_goal->pos;

#ifdef NEO
	// NEO-HARNESS-TEMP research arm (patch 51): steer round physics props across the path ahead
	goalPos = PropDetour( bot, goalPos );
#endif

	// avoid small obstacles
	forward = goalPos - mover->GetFeet();
	forward.z = 0.0f;
	float rangeToGoal = forward.NormalizeInPlace();

	left.x = -forward.y;
	left.y = forward.x;
	left.z = 0.0f;

	if ( true || m_goal != LastSegment() )	// think more about this - we often need to avoid to reach the final goal pos, too (MSB 5/15/09)
	{
		const float nearLedgeRange = 50.0f;
		if ( rangeToGoal > nearLedgeRange || ( m_goal && m_goal->type != CLIMB_UP ) )
		{
			goalPos = Avoid( bot, goalPos, forward, left );
		}
	}

#ifdef NEO
	// patch 97: not over a drop the path does not plan - re-plan from here
	if ( neo_bot_path_void_guard.GetBool() && NeoVoidAhead( bot, m_goal, PriorSegment( m_goal ), NextSegment( m_goal ), goalPos ) )
	{
		mover->GetBot()->OnMoveToFailure( this, FAIL_FELL_OFF );

		// don't invalidate if OnMoveToFailure just recomputed a new path
		if ( GetAge() > 0.0f )
		{
			Invalidate();
		}
		return;
	}
#endif

	// face towards movement goal
	if ( mover->IsOnGround() )
	{	
		mover->FaceTowards( goalPos );
	}

	// move bot along path
	mover->Approach( goalPos );

	// Currently, Approach determines STAND or CROUCH. 
	// Override this if we're approaching a climb or a jump
	if ( m_goal && ( m_goal->type == CLIMB_UP || m_goal->type == JUMP_OVER_GAP ) )
	{
		bot->GetBodyInterface()->SetDesiredPosture( IBody::STAND );
	}

	if ( bot->IsDebugging( NEXTBOT_PATH ) )
	{
		const Segment *start = GetCurrentGoal();
		if ( start )
		{
			start = PriorSegment( start );
		}

		Draw( start );

		/*
		else
		{
		DrawInterpolated( 0.0f, GetLength() );
		}
		*/

		NDebugOverlay::Cross3D( goalPos, 5.0f, 150, 150, 255, true, 0.1f );
		NDebugOverlay::Line( bot->GetEntity()->WorldSpaceCenter(), goalPos, 255, 255, 0, true, 0.1f );
	}
}


//--------------------------------------------------------------------------------------------------------------
/**
 * If entity is returned, it is blocking us from continuing along our path
 */
CBaseEntity *PathFollower::FindBlocker( INextBot *bot )
{
	IIntention *think = bot->GetIntentionInterface();

	// if we don't care about hindrances, don't do the expensive tests
#ifdef NEO
	AssertOnce(uintp(IS_ANY_HINDRANCE_POSSIBLE) == uintp(-1));
#endif
	if ( think->IsHindrance( bot, IS_ANY_HINDRANCE_POSSIBLE ) != ANSWER_YES )
		return NULL;

	ILocomotion *mover = bot->GetLocomotionInterface();
	IBody *body = bot->GetBodyInterface();

	trace_t result;
	NextBotTraceFilterOnlyActors filter( bot->GetEntity(), COLLISION_GROUP_NONE );

	const float size = body->GetHullWidth()/4.0f;	// keep this small to avoid lockups when groups of bots get close
	Vector blockerMins( -size, -size, mover->GetStepHeight() );
	Vector blockerMaxs( size, size, body->GetCrouchHullHeight() );

	Vector from = mover->GetFeet();
	float range = 0.0f;

	const float maxHindranceRangeAlong = 750.0f;

	// because our path goal may be far ahead of us if the way to there is unobstructed, we
	// need to start looking from the point of the path we are actually standing on
	MoveCursorToClosestPosition( mover->GetFeet() );

	for( const Segment *s = GetCursorData().segmentPrior; s && range < maxHindranceRangeAlong; s = NextSegment( s ) )
	{
		// trace along direction toward goal a minimum range, in case goal and hindrance are
		// very close, but goal is closer

		Vector traceForward = s->pos - from;
		float traceRange = traceForward.NormalizeInPlace();

		const float minTraceRange = 2.0f * body->GetHullWidth();
		if ( traceRange < minTraceRange )
		{
			traceRange = minTraceRange;
		}

		mover->TraceHull( from, from + traceRange * traceForward, blockerMins, blockerMaxs, body->GetSolidMask(), &filter, &result );

		if ( result.DidHitNonWorldEntity() )
		{
			// if blocker is close, they could be behind us - check
			Vector toBlocker = result.m_pEnt->GetAbsOrigin() - bot->GetLocomotionInterface()->GetFeet();

			Vector alongPath = s->pos - from;
			alongPath.z = 0.0f;

			if ( DotProduct( toBlocker, alongPath ) > 0.0f )
			{
				// ask the bot if this really is a hindrance
				if ( think->IsHindrance( bot, result.m_pEnt ) == ANSWER_YES )
				{
					if ( bot->IsDebugging( NEXTBOT_PATH ) )
					{
						NDebugOverlay::Circle( bot->GetLocomotionInterface()->GetFeet(), QAngle( -90.0f, 0, 0 ), 10.0f, 255, 0, 0, 255, true, 1.0f );
						NDebugOverlay::HorzArrow( bot->GetLocomotionInterface()->GetFeet(), result.m_pEnt->GetAbsOrigin(), 1.0f, 255, 0, 0, 255, true, 1.0f );
					}

					// we are blocked
					return result.m_pEnt;
				}
			}
		}

		from = s->pos;
		range += s->length;
	}

	return NULL;
}


//--------------------------------------------------------------------------------------------------------------
/**
 * Do reflex avoidance movements of very nearby obstacles.
 * Return adjusted goal.
 */
Vector PathFollower::Avoid( INextBot *bot, const Vector &goalPos, const Vector &forward, const Vector &left )
{
	VPROF_BUDGET( "PathFollower::Avoid", "NextBotExpensive" );

	if ( !NextBotAllowAvoiding.GetBool() )
	{
		return goalPos;
	}

	if ( !m_avoidTimer.IsElapsed() )
	{
		return goalPos;
	}
	
	// low frequency check until we actually hit something we need to avoid
	const float avoidInterval = 0.5f; // 1.0f;
	m_avoidTimer.Start( avoidInterval );

	ILocomotion *mover = bot->GetLocomotionInterface();

	if ( mover->IsClimbingOrJumping() || !mover->IsOnGround() )
	{
		return goalPos;
	}

	//
	// Check for potential blockers along our path and wait if we're blocked
	//
	m_hindrance = FindBlocker( bot );
	if ( m_hindrance != NULL )
	{
		// wait 
		m_waitTimer.Start( avoidInterval * RandomFloat( 1.0f, 2.0f ) );

		return mover->GetFeet();
	}


	// if we are in a "precise" area, do not use avoid volumes
	CNavArea *area = bot->GetEntity()->GetLastKnownArea();
	if ( area && ( area->GetAttributes() & NAV_MESH_PRECISE ) )
	{
		return goalPos;
	}

	m_didAvoidCheck = true;

	// we want to avoid other players, etc	
	trace_t result;
	NextBotTraceFilterOnlyActors filter( bot->GetEntity(), COLLISION_GROUP_NONE );

	IBody *body = bot->GetBodyInterface();
	unsigned int mask = body->GetSolidMask();

	const float size = body->GetHullWidth()/4.0f;
	const float offset = size + 2.0f;

	float range = mover->IsRunning() ? 50.0f : 30.0f;
	range *= bot->GetEntity()->GetModelScale();

	m_hullMin = Vector( -size, -size, mover->GetStepHeight()+0.1f );
	
	// only use crouch-high avoid volumes, since we'll just crouch if higher obstacles are near
	m_hullMax = Vector( size, size, body->GetCrouchHullHeight() );
	
	Vector nextStepHullMin( -size, -size, 2.0f * mover->GetStepHeight() + 0.1f );

	// avoid any open doors in our way
	CBasePropDoor *door = NULL;

	// check left side
	m_leftFrom = mover->GetFeet() + offset * left;
	m_leftTo = m_leftFrom + range * forward;

	m_isLeftClear = true;
	float leftAvoid = 0.0f;

#ifdef NEO
	// breakable props are traversable EVENTUALLY (the bot may break them) but not right now
	NextBotTraversableTraceFilter traverseFilter( bot, neo_bot_avoid_prop_entities.GetBool() ? ILocomotion::IMMEDIATELY : ILocomotion::EVENTUALLY );
	CBaseEntity *leftHit = NULL;
	CBaseEntity *rightHit = NULL;
#else
	NextBotTraversableTraceFilter traverseFilter( bot );
#endif
	mover->TraceHull( m_leftFrom, m_leftTo, m_hullMin, m_hullMax, mask, &traverseFilter, &result );
	if ( result.fraction < 1.0f || result.startsolid )
	{
		// if this sensor is starting in a solid, set fraction to emulate being against a wall
		if ( result.startsolid )
		{
			result.fraction = 0.0f;
		}

		leftAvoid = clamp( 1.0f - result.fraction, 0.0f, 1.0f );

		m_isLeftClear = false;

		// track any doors we need to avoid
		if ( result.DidHitNonWorldEntity() )
		{
			door = dynamic_cast< CBasePropDoor * >( result.m_pEnt );
#ifdef NEO
			leftHit = result.m_pEnt;
#endif
		}

		// check for steps
// 		float firstHit = result.fraction;
// 		mover->TraceHull( m_leftFrom, m_leftTo, nextStepHullMin, m_hullMax, mask, &filter, &result );
// 		if ( result.fraction <= firstHit ) //+ mover->GetStepHeight()/2.0f )
// 		{
// 			// it's not a step - we hit something
// 			m_isLeftClear = false;
// 		}
	}

	// check right side
	m_rightFrom = mover->GetFeet() - offset * left;
	m_rightTo = m_rightFrom + range * forward;

	m_isRightClear = true;
	float rightAvoid = 0.0f;

	mover->TraceHull( m_rightFrom, m_rightTo, m_hullMin, m_hullMax, mask, &traverseFilter, &result );
	if ( result.fraction < 1.0f || result.startsolid )
	{
		// if this sensor is starting in a solid, set fraction to emulate being against a wall
		if ( result.startsolid )
		{
			result.fraction = 0.0f;
		}

		rightAvoid = clamp( 1.0f - result.fraction, 0.0f, 1.0f );

		m_isRightClear = false;

		// track any doors we need to avoid
		if ( !door && result.DidHitNonWorldEntity() )
		{
			door = dynamic_cast< CBasePropDoor * >( result.m_pEnt );
		}
#ifdef NEO
		if ( result.DidHitNonWorldEntity() )
		{
			rightHit = result.m_pEnt;
		}
#endif

		// check for steps
// 		float firstHit = result.fraction;
// 		mover->TraceHull( m_rightFrom, m_rightTo, nextStepHullMin, m_hullMax, mask, &filter, &result );
// 		if ( result.fraction <= firstHit ) // + mover->GetStepHeight()/2.0f)
// 		{
// 			// it's not a step - we hit something
// 			m_isRightClear = false;
// 		}
	}

	Vector adjustedGoal = goalPos;

#ifdef NEO
	// a prop squarely ahead: walk round it rather than into it
	if ( neo_bot_avoid_prop_entities.GetBool() && !door && !m_isLeftClear && !m_isRightClear )
	{
		CBaseEntity *prop = NeoIsPropObstacle( leftHit ) ? leftHit : ( NeoIsPropObstacle( rightHit ) ? rightHit : NULL );
		Vector detour;
		if ( prop && NeoPropDetour( bot, prop, goalPos, forward, left, &detour ) )
		{
			m_avoidTimer.Invalidate();
			return detour;
		}
	}
#endif

	// avoid doors directly in our way
	if ( door && !m_isLeftClear && !m_isRightClear )
	{
		Vector forward, right, up;
		AngleVectors( door->GetAbsAngles(), &forward, &right, &up );

		const float doorWidth = 100.0f;
		Vector doorEdge = door->GetAbsOrigin() - doorWidth * right;

		if ( bot->IsDebugging( NEXTBOT_PATH ) )
		{
			NDebugOverlay::Axis( door->GetAbsOrigin(), door->GetAbsAngles(), 20.0f, true, 10.0f );
			NDebugOverlay::Line( door->GetAbsOrigin(), doorEdge, 255, 255, 0, true, 10.0f );
		}

		// move around door
		adjustedGoal.x = doorEdge.x;
		adjustedGoal.y = doorEdge.y;

		// do avoid check again next frame
		m_avoidTimer.Invalidate();
	}
	else if ( !m_isLeftClear || !m_isRightClear )
	{
		// adjust goal to avoid small obstacle
		float avoidResult = 0.0f;
		if ( m_isLeftClear )
		{
			avoidResult = -rightAvoid;
		}
		else if (m_isRightClear)
		{
			avoidResult = leftAvoid;
		}
		else
		{
			// both left and right are blocked, avoid nearest
			const float equalTolerance = 0.01f;
			if ( fabs( rightAvoid - leftAvoid ) < equalTolerance )
			{
				// squarely against a wall, etc
				return adjustedGoal;
			} 
			else if ( rightAvoid > leftAvoid )
			{
				avoidResult = -rightAvoid;
			}
			else
			{
				avoidResult = leftAvoid;
			}
		}
		
		// adjust goal to avoid obstacle
		Vector avoidDir = 0.5f * forward - left * avoidResult;
		avoidDir.NormalizeInPlace();
		
		adjustedGoal = mover->GetFeet() + 100.0f * avoidDir;
		
		// do avoid check again next frame
		m_avoidTimer.Invalidate();
	}
	
	return adjustedGoal;
}


#ifdef EXPERIMENTAL_LEDGE_FINDER
//--------------------------------------------------------------------------------------------------------------
/**
 * Given a hull that defines the area of space that may contain a climbable ledge,
 * subdivide it until we find the ledge.
 */
bool PathFollower::FindClimbLedge( INextBot *bot, Vector startTracePos, Vector ledgeRegionMins, Vector ledgeRegionMaxs )
{
	float deltaZ = ledgeRegionMaxs.z - ledgeRegionMins.z;

	if ( deltaZ <= bot->GetLocomotionInterface()->GetStepHeight() )
	{
		// reached minimum subdivision limit - stop
		return false;
	}

	trace_t result;
	NextBotTraversableTraceFilter filter( bot, ILocomotion::IMMEDIATELY );

	mover->TraceHull( startTracePos, startTracePos,
					ledgeRegionMins, ledgeRegionMaxs, 
					bot->GetBodyInterface()->GetSolidMask(), &filter, &result );


	if ( result.DidHit() )
	{
		// volume is blocked - split into upper and lower volumes and try again
		float midZ = ( ledgeRegionMins.z + ledgeRegionMaxs.z ) / 2.0f;

		Vector upperLedgeRegionMins( ledgeRegionMins.x, ledgeRegionMins.y, midZ );
		Vector upperLedgeRegionMaxs = ledgeRegionMaxs;
		FindClimbLedge( bot, startTracePos, upperLedgeRegionMins, upperLedgeRegionMaxs );


		Vector lowerLedgeRegionMins = ledgeRegionMins;
		Vector lowerLedgeRegionMaxs( ledgeRegionMaxs.x, ledgeRegionMaxs.y, midZ );
		FindClimbLedge( bot, startTracePos, lowerLedgeRegionMins, lowerLedgeRegionMaxs );
	}
	else
	{
		// volume is clear, trace straight down to find ledge and keep lowest one we've found
		mover->TraceHull( startTracePos,
						startTracePos + Vector( 0, 0, -100.0f ),
						ledgeRegionMins, ledgeRegionMaxs, 
						bot->GetBodyInterface()->GetSolidMask(), &filter, &result );	
	}
}
#endif // _DEBUG


//--------------------------------------------------------------------------------------------------------------
/**
 * Climb up ledges
 */
bool PathFollower::Climbing( INextBot *bot, const Path::Segment *goal, const Vector &forward, const Vector &right, float goalRange )
{
	VPROF_BUDGET( "PathFollower::Climbing", "NextBot" );

	ILocomotion *mover = bot->GetLocomotionInterface();
	IBody *body = bot->GetBodyInterface();
	CNavArea *myArea = bot->GetEntity()->GetLastKnownArea();

	if ( !mover->IsAbleToClimb() || !NextBotAllowClimbing.GetBool() )
	{
		return false;
	}

	// use the 2D direction towards our goal
	Vector climbDirection = forward;
	climbDirection.z = 0.0f;
	climbDirection.NormalizeInPlace();

	// we can't have this as large as our hull width, or we'll find ledges ahead of us
	// that we will fall from when we climb up because our hull wont actually touch at the top.
	const float ledgeLookAheadRange = body->GetHullWidth() - 1;

	if ( mover->IsClimbingOrJumping() || mover->IsAscendingOrDescendingLadder() || !mover->IsOnGround() )
	{
		return false;
	}

	// can be in any posture when we climb

	if ( m_goal == NULL )
	{
		return false;
	}

	if ( TheNavMesh->IsAuthoritative() )
	{
		//
		// Trust what that nav mesh tells us.
		// No need for expensive ledge-finding for games with simpler geometry (like TF2)
		//
		if ( m_goal->type == CLIMB_UP )
		{
			const Segment *afterClimb = NextSegment( m_goal );
			if ( afterClimb && afterClimb->area )
			{
				// find closest point on climb-destination area
				Vector nearClimbGoal;
				afterClimb->area->GetClosestPointOnArea( mover->GetFeet(), &nearClimbGoal );

				climbDirection = nearClimbGoal - mover->GetFeet();
				climbDirection.z = 0.0f;
				climbDirection.NormalizeInPlace();

				if ( mover->ClimbUpToLedge( nearClimbGoal, climbDirection, NULL ) )
					return true;
			}
		}

		return false;
	}


#ifdef NEO
	// NEO-HARNESS-TEMP research arm (patch 58): the ledge search below gives up silently in places the
	// mesh says are a climb (tarmac's basin rim: bots stood at the launch point for whole rounds); trust
	// the link there, as the authoritative-mesh branch above does
	if ( neo_bot_climb_trust_nav.GetInt() > 0 && m_goal->type == CLIMB_UP &&
		 ( neo_bot_climb_trust_nav.GetInt() == 1 || mover->IsStuck() ) &&
		 ( m_goal->pos - mover->GetFeet() ).AsVector2D().IsLengthLessThan( body->GetHullWidth() ) )
	{
		const Segment *afterClimb = NextSegment( m_goal );
		if ( afterClimb && afterClimb->area )
		{
			Vector nearClimbGoal;
			afterClimb->area->GetClosestPointOnArea( mover->GetFeet(), &nearClimbGoal );
			Vector trustDirection = nearClimbGoal - mover->GetFeet();
			trustDirection.z = 0.0f;
			trustDirection.NormalizeInPlace();
			if ( mover->ClimbUpToLedge( nearClimbGoal, trustDirection, NULL ) )
			{
				if ( sv_neo_forensic_log.GetBool() )
				{
					Msg( "NEO_FORENSIC_CLIMBTRUST t=%.2f p=%d pos=%.0f,%.0f,%.0f to=%.0f,%.0f,%.0f area=%d\n", gpGlobals->curtime,
						 bot->GetEntity()->entindex(), mover->GetFeet().x, mover->GetFeet().y, mover->GetFeet().z,
						 nearClimbGoal.x, nearClimbGoal.y, nearClimbGoal.z, afterClimb->area->GetID() );
				}
				return true;
			}
		}
	}
#endif

	// If we're approaching a CLIMB_UP link, save off the height delta for it, and trust the nav *just* enough
	// to climb up to that ledge and only that ledge.  We keep as large a tolerance as possible, to trust
	// the nav as little as possible.  There's no valid way to have another CLIMB_UP link within crouch height,
	// because we can't actually fit in between the two areas, so one climb is invalid.
	float climbUpLedgeHeightDelta = -1.0f;
	const float climbUpLedgeTolerance = body->GetCrouchHullHeight();

	if ( m_goal->type == CLIMB_UP )
	{
		const Segment *afterClimb = NextSegment( m_goal );
		if ( afterClimb && afterClimb->area )
		{
			// find closest point on climb-destination area
			Vector nearClimbGoal;
			afterClimb->area->GetClosestPointOnArea( mover->GetFeet(), &nearClimbGoal );

			climbDirection = nearClimbGoal - mover->GetFeet();
			climbUpLedgeHeightDelta = climbDirection.z;
			climbDirection.z = 0.0f;
			climbDirection.NormalizeInPlace();
		}
	}

	// don't try to climb up stairs
	if ( m_goal->area->HasAttributes( NAV_MESH_STAIRS ) || ( myArea && myArea->HasAttributes( NAV_MESH_STAIRS ) ) )
	{
		if ( bot->IsDebugging( NEXTBOT_PATH ) )
		{
			NDebugOverlay::Cross3D( mover->GetFeet(), 5.0f, 0, 255, 255, true, 5.0f );
			DevMsg( "%3.2f: %s ON STAIRS\n", gpGlobals->curtime, bot->GetDebugIdentifier() );
		}
		return false;
	}

	// 'current' is the segment we are on/just passed over
	const Segment *current = PriorSegment( m_goal );
	if ( current == NULL )
	{
		return false;
	}

	// If path segment immediately ahead of us is not obstructed, don't try to climb.
	// This is required to try to avoid accidentally climbing onto valid high ledges when we really want to run UNDER them to our destination.
	// We need to check "immediate" traversability to pay attention to breakable objects in our way that we should climb over.
	// We also need to check traversability out to 2 * ledgeLookAheadRange in case our goal is just before a tricky ledge climb and once we pass the goal it will be too late.
	// When we're in a CLIMB_UP segment, allow us to look for ledges - we know the destination ledge height, and will only grab the correct ledge.
	Vector toGoal = m_goal->pos - mover->GetFeet();
	toGoal.NormalizeInPlace();

	if ( toGoal.z < mover->GetTraversableSlopeLimit() &&
		 !mover->IsStuck() && m_goal->type != CLIMB_UP &&
		 mover->IsPotentiallyTraversable( mover->GetFeet(), mover->GetFeet() + 2.0f * ledgeLookAheadRange * toGoal, ILocomotion::IMMEDIATELY ) )
	{
		return false;
	}


	// can't do this - we have to find the ledge to deal with breakable railings
#if 0
	// If our path requires a climb, do the climb.
	// This solves some issues where there are several possible climbable ledges at a given
	// location, and we need to know which ledge to climb - just use the preplanned path's choice.
	const Segment *ledge = NextSegment( m_goal );
	if ( m_goal->type == CLIMB_UP && ledge )
	{
		const float startClimbRange = body->GetHullWidth();
		if ( ( m_goal->pos - mover->GetFeet() ).IsLengthLessThan( startClimbRange ) )
		{
			mover->ClimbUpToLedge( ledge->pos, climbDirection );
			return true;
		}
	}
#endif 


	
	// Determine if we're approaching a planned climb.
	// Start with current, the segment we are currently traversing.  Skip the distance check for that segment, because
	// the pos is (hopefully) behind us.  And if it's a long path segment, it's already outside the climbLookAheadRange,
	// and thus it would prevent us looking at m_goal and further for imminent planned climbs.
	const float climbLookAheadRange = 150.0f;
	bool isPlannedClimbImminent = false;
	float plannedClimbZ = 0.0f;
	for( const Segment *s = current; s; s = NextSegment( s ) )
	{
		if ( s != current && ( s->pos - mover->GetFeet() ).AsVector2D().IsLengthGreaterThan( climbLookAheadRange ) )
		{
			break;
		}

		if ( s->type == CLIMB_UP )
		{
			isPlannedClimbImminent = true;

			const Segment *next = NextSegment( s );
			if ( next )
			{
				plannedClimbZ = next->pos.z;
			}
			break;
		}
	}

	unsigned int mask = body->GetSolidMask();
	trace_t result;
	NextBotTraversableTraceFilter filter( bot, ILocomotion::IMMEDIATELY );

	const float hullWidth = body->GetHullWidth();
	const float halfSize = hullWidth / 2.0f;
	const float minHullHeight = body->GetCrouchHullHeight();
	const float minLedgeHeight = mover->GetStepHeight() + 0.1f;

	Vector skipStepHeightHullMin( -halfSize, -halfSize, minLedgeHeight );

	// need to use minimum actual hull height here to catch porous fences and railings
	Vector skipStepHeightHullMax( halfSize, halfSize, minHullHeight + 0.1f );


	// Find the highest height we can stand at our current location.
	// Using the full width hull catches on small lips/ledges, so back up and try again.
	float ceilingFraction;

	// We can't use IsPotentiallyTraversable to test for ledges, because it's smaller Hull can cause the
	// next trace (trace the ceiling height forward) to start solid.
	//	mover->IsPotentiallyTraversable( mover->GetFeet(), mover->GetFeet() + Vector( 0, 0, mover->GetMaxJumpHeight() ), ILocomotion::IMMEDIATELY, &ceilingFraction );

	// Instead of IsPotentiallyTraversable, we back up the same distance and use a second upward trace
	// to see if that one finds a higher ceiling.  If so, we use that ceiling height, and use the
	// backed-up feet position for the ledge finding traces.
	Vector feet( mover->GetFeet() );
	Vector ceiling( feet + Vector( 0, 0, mover->GetMaxJumpHeight() ) );
	mover->TraceHull( feet, ceiling,
		skipStepHeightHullMin, skipStepHeightHullMax, mask, &filter, &result );
	ceilingFraction = result.fraction;
	bool isBackupTraceUsed = false;
	if ( ceilingFraction < 1.0f || result.startsolid )
	{
		trace_t backupTrace;
		const float backupDistance = hullWidth * 0.25f;	// The IsPotentiallyTraversable check this replaces uses a 1/4 hull width trace
		Vector backupFeet( feet - climbDirection * backupDistance );
		Vector backupCeiling( backupFeet + Vector( 0, 0, mover->GetMaxJumpHeight() ) );
		mover->TraceHull( backupFeet, backupCeiling,
			skipStepHeightHullMin, skipStepHeightHullMax, mask, &filter, &backupTrace );
		if ( !backupTrace.startsolid && backupTrace.fraction > ceilingFraction )
		{
			bot->DebugConColorMsg( NEXTBOT_PATH, Color( 255, 255, 255, 255 ), "%s backing up when looking for max ledge height\n", bot->GetDebugIdentifier() );
			result = backupTrace;
			ceilingFraction = result.fraction;
			feet = backupFeet;
			ceiling = backupCeiling;
			isBackupTraceUsed = true;
		}
	}

	float maxLedgeHeight = ceilingFraction * mover->GetMaxJumpHeight();

	if ( maxLedgeHeight <= mover->GetStepHeight() )
	{
		return false; // early out when we can't even climb StepHeight.
	}

	//
	// Check for ledge climbs over things in our way.
	// Even if we have a CLIMB_UP link in our path, we still need
	// to find the actual ledge by tracing the local geometry.
	//
	Vector climbHullMax( halfSize, halfSize, maxLedgeHeight );

	Vector ledgePos = feet;	// to be computed below

	mover->TraceHull( feet, 
					  feet + climbDirection * ledgeLookAheadRange, 
					  skipStepHeightHullMin, climbHullMax, mask, &filter, &result );

	if ( bot->IsDebugging( NEXTBOT_PATH ) && NextBotDebugClimbing.GetBool() )
	{
		// show ledge-finding hull as we move
		NDebugOverlay::SweptBox( feet, 
								 feet + climbDirection * ledgeLookAheadRange, 
								 skipStepHeightHullMin, climbHullMax, vec3_angle,
								 255, 100, 0, 255, 0.1f );
	}

	bool wasPotentialLedgeFound = result.DidHit() && !result.startsolid;
	// To test climbing up past small lips on walls, we can force the bot to run past the overhang and use the backup trace:
	// wasPotentialLedgeFound = wasPotentialLedgeFound && (result.fraction == 0 || isBackupTraceUsed);
	if ( wasPotentialLedgeFound )
	{
		VPROF_BUDGET( "PathFollower::Climbing( Search for ledge to climb )", "NextBot" );

		if ( bot->IsDebugging( NEXTBOT_PATH ) && NextBotDebugClimbing.GetBool() )
		{
			// show ledge-finding hull that found a ledge candidate 
			NDebugOverlay::SweptBox( feet, 
									 feet + climbDirection * ledgeLookAheadRange, 
									 skipStepHeightHullMin, climbHullMax, vec3_angle,
									 255, 100, 0, 100, 999.9f );

			// show primary climb direction
			NDebugOverlay::HorzArrow( feet, feet + 50.0f * climbDirection, 2.0f, 0, 255, 0, 255, true, 9999.9f );
		}

		// what are we climbing over?
		CBaseEntity *obstacle = result.m_pEnt;

		if ( !result.DidHitNonWorldEntity() || bot->IsAbleToClimbOnto( obstacle ) )
		{			
			if ( bot->IsDebugging( NEXTBOT_PATH ) )
			{
				DevMsg( "%3.2f: %s at potential ledge climb\n", gpGlobals->curtime, bot->GetDebugIdentifier() );
			}

			// the low hull sweep hit an obstacle - note how 'far in' this is
			float ledgeFrontWallDepth = ledgeLookAheadRange * result.fraction;

			float minLedgeDepth = body->GetHullWidth()/2.0f; // 5.0f;
			if ( m_goal->type == CLIMB_UP )
			{
				// Climbing up to a narrow nav area indicates a narrow ledge.  We need to reduce our minLedgeDepth
				// here or our path will say we should climb but we'll forever fail to find a wide enough ledge.
				const Segment *afterClimb = NextSegment( m_goal );
				if ( afterClimb && afterClimb->area )
				{
					Vector depthVector = climbDirection * minLedgeDepth;
					depthVector.z = 0;
					if ( fabs( depthVector.x ) > afterClimb->area->GetSizeX() )
					{
						depthVector.x = (depthVector.x > 0) ? afterClimb->area->GetSizeX() : -afterClimb->area->GetSizeX();
					}
					if ( fabs( depthVector.y ) > afterClimb->area->GetSizeY() )
					{
						depthVector.y = (depthVector.y > 0) ? afterClimb->area->GetSizeY() : -afterClimb->area->GetSizeY();
					}

					float areaDepth = depthVector.NormalizeInPlace();
					minLedgeDepth = MIN( minLedgeDepth, areaDepth );
				}
			}

			//
			// Find the ledge.  Start at the lowest jump we can make
			// and step up until we find the actual ledge.  
			//
			// The scan is limited to maxLedgeHeight in case our max 
			// jump/climb height is so tall the highest horizontal hull 
			// trace could be on the other side of the ceiling above us
			//

			float ledgeHeight = minLedgeHeight;
			const float ledgeHeightIncrement = 0.5f * mover->GetStepHeight();

			bool foundWall = false;
			bool foundLedge = false;
			
			// once we have found the ledge's front wall, we must look at least minLedgeDepth farther in to verify it is a ledge
			// NOTE: This *must* be ledgeLookAheadRange since ledges are compared against the initial trace which was ledgeLookAheadRange deep
			float ledgeTopLookAheadRange = ledgeLookAheadRange;

			// TODO: we also must look far enough ahead in case the ledge we actually find is "deeper" than the initial wall at the base

			Vector climbHullMin( -halfSize, -halfSize, 0.0f );
			Vector climbHullMax( halfSize, halfSize, minHullHeight );

			Vector wallPos;
			float wallDepth = 0.0f;

			bool isLastIteration = false;
			while( true )
			{				
				// trace forward to find the wall in front of us, or the empty space of the ledge above us
				mover->TraceHull( feet + Vector( 0, 0, ledgeHeight ), 
								feet + Vector( 0, 0, ledgeHeight ) + climbDirection * ledgeTopLookAheadRange, 
								climbHullMin, climbHullMax, mask, &filter, &result );

				float traceDepth = ledgeTopLookAheadRange * result.fraction;

				if ( !result.startsolid )
				{
					// if trace reached minLedgeDepth farther, this is a potential ledge
					if ( foundWall )
					{
						// TODO: test that potential ledge is flat enough to stand on
						if ( ( traceDepth - ledgeFrontWallDepth ) > minLedgeDepth )
						{
							bool isUsable = true;

							// initialize ledgePos from result of last trace
							ledgePos = result.endpos;

							// Find the actual ground level on the potential ledge
							// Only trace back down to the previous ledge height trace. 
							// The ledge can be no lower, or we would've found it in the last iteration.
							mover->TraceHull( ledgePos,
											ledgePos + Vector( 0, 0, -ledgeHeightIncrement ),
											climbHullMin, climbHullMax, mask, &filter, &result );

							ledgePos = result.endpos;

							// if the whole trace is in solid, we're out of luck, but
							// if the trace just started solid, 'ledgePos' should still be valid
							// since the trace left the solid and then hit.
							// if the trace hit nothing, the potential ledge is actually deeper in
							const float MinGroundNormal = 0.7f;	// players can't stand on ground steeper than 0.7
							if ( result.allsolid || !result.DidHit() || result.plane.normal.z < MinGroundNormal )
							{
								// not a usable ledge, try again
								isUsable = false;
							}
							else
							{
								if ( climbUpLedgeHeightDelta > 0.0f )
								{
									// if we're climbing to a specific ledge via a CLIMB_UP link, only climb to that ledge.
									// Do this only for the world (which includes static props) so we can still opportunistically
									// climb up onto breakable railings and physics props.
									if ( result.DidHitWorld() )
									{
										float potentialLedgeHeight = result.endpos.z - feet.z;
										if ( fabs(potentialLedgeHeight - climbUpLedgeHeightDelta) > climbUpLedgeTolerance )
										{
											isUsable = false;
										}
									}
								}
							}

							if ( isUsable )
							{
								// back up until we no longer are hitting the ledge to determine the
								// exact ledge edge position
								Vector validLedgePos = ledgePos; // save off a valid ledge pos
								const float edgeTolerance = 4.0f;
								const float maxBackUp = hullWidth;
								float backUpSoFar = edgeTolerance;
								Vector testPos = ledgePos;

								while( backUpSoFar < maxBackUp )
								{
									testPos -= edgeTolerance * climbDirection;
									backUpSoFar += edgeTolerance;

									mover->TraceHull( testPos,
													testPos + Vector( 0, 0, -ledgeHeightIncrement ),
													climbHullMin, climbHullMax, mask, &filter, &result );

									
									if ( bot->IsDebugging( NEXTBOT_PATH ) && NextBotDebugClimbing.GetBool() )
									{
										// show edge-finder hulls
										NDebugOverlay::SweptBox( testPos,
																 testPos + Vector( 0, 0, -mover->GetStepHeight() ), 
																 climbHullMin, climbHullMax, vec3_angle, 255, 0, 0, 255, 999.9f );
									}

									if ( result.DidHit() && result.plane.normal.z >= MinGroundNormal )
									{
										// we hit, this is closer to the actual ledge edge
										ledgePos = result.endpos;
									}
									else
									{
										// nothing but air or a steep slope below us, we have found the edge
										break;
									}
								}

								// we want ledgePos to be right on the edge itself, so move 
								// it ahead by half of the hull width
								ledgePos += climbDirection * halfSize;

								// Make sure this doesn't embed us in the far wall if the ledge is narrow, since we would
								// have backed up less than halfSize.
								Vector climbHullMinStep( climbHullMin ); // skip StepHeight for sloped ledges
								mover->TraceHull( validLedgePos,
									ledgePos,
									climbHullMinStep, climbHullMax, mask, &filter, &result );

								ledgePos = result.endpos;

								// Now since ledgePos + StepHeight is valid, trace down to find ground on sloped ledges.
								mover->TraceHull( ledgePos + Vector( 0, 0, StepHeight ),
									ledgePos,
									climbHullMin, climbHullMax, mask, &filter, &result );
								if ( !result.startsolid )
								{
									ledgePos = result.endpos;
								}
							}


/*** NOTE: While this saves us from climbing into a window below the window we want to get in,
 *** it also causes us to climb in midair high over crates sitting against walls we need to climb over.
							if ( isUsable && m_goal->type == CLIMB_UP )
							{
								// we can only accept ledges at least as high as our current CLIMB_UP destination
								// NOTE: Can't use plannedClimbZ here, since that could be 2 or 3 short climbs ahead
								const Segment *ledge = NextSegment( m_goal );

								if ( !ledge || ledgeHeight < ledge->pos.z - feet.z - mover->GetStepHeight() )
								{
									// this ledge is below the CLIMB_UP destination - can't use it
									isUsable = false;
								}
							}
*/


							if ( isUsable )
							{
								// found a usable ledge here
								foundLedge = true;
								break;
							}
						}
					}
					else if ( result.DidHit() )
					{
						// this iteration hit the wall under the ledge, 
						// meaning the next iteration that reaches far enough will be our ledge

						// Since we know that our desired route is likely blocked (via the 
						// IsTraversable check above) - any ledge we hit we must climb.

						// found a valid ledge wall
						foundWall = true;
						wallDepth = traceDepth;

						// make sure the subsequent traces are at least minLedgeDepth deeper than
						// the wall we just found, or all ledge checks will fail
						float minTraceDepth = traceDepth + minLedgeDepth + 0.1f;

						if ( ledgeTopLookAheadRange < minTraceDepth )
						{
							ledgeTopLookAheadRange = minTraceDepth;
						}

						if ( bot->IsDebugging( NEXTBOT_PATH ) )
						{
							DevMsg( "%3.2f: Climbing - found wall.\n", gpGlobals->curtime );
							if ( NextBotDebugClimbing.GetBool() )
							{
								NDebugOverlay::HorzArrow( result.endpos, result.endpos + 20.0f * result.plane.normal, 5.0f, 255, 100, 0, 255, true, 9999.9f );
							}
							wallPos = result.endpos;
						}
					}
					else if ( ledgeHeight > body->GetCrouchHullHeight() && !isPlannedClimbImminent )
					{
						// we haven't hit anything yet, and we're already above our heads - no obstacle
						if ( bot->IsDebugging( NEXTBOT_PATH ) )
						{
							DevMsg( "%3.2f: Climbing - skipping overhead climb we can walk/crawl under.\n", gpGlobals->curtime );
						}
						break;
					}
				}

				ledgeHeight += ledgeHeightIncrement;

				if ( ledgeHeight >= maxLedgeHeight )
				{
					if ( isLastIteration )
					{
						// tested at max height
						break;
					}

					// check one more time at max jump height
					isLastIteration = true;
					ledgeHeight = maxLedgeHeight;
				}
			}
			
			if ( foundLedge )
			{
				if ( bot->IsDebugging( NEXTBOT_PATH ) )
				{
					DevMsg( "%3.2f: STARTING LEDGE CLIMB UP\n", gpGlobals->curtime );

					if ( NextBotDebugClimbing.GetBool() ) 
					{
						NDebugOverlay::Cross3D( ledgePos, 10.0f, 0, 255, 0, true, 9999.9f );

						// display approximation of idealized ledge that has been found
						Vector side( -climbDirection.y, climbDirection.x, 0.0f );

						// this is an approximation, since AABB can hit at any angle
						Vector base = feet + halfSize * climbDirection;

						Vector wallBottomLeft = base + halfSize * side;
						Vector wallBottomRight = base - halfSize * side;
						Vector wallTopLeft = wallBottomLeft + Vector( 0, 0, ledgeHeight );
						Vector wallTopRight = wallBottomRight + Vector( 0, 0, ledgeHeight );

						NDebugOverlay::Triangle( wallBottomRight, wallBottomLeft, wallTopLeft, 255, 100, 0, 100, true, 9999.9f );
						NDebugOverlay::Triangle( wallBottomRight, wallTopLeft, wallTopRight, 255, 100, 0, 100, true, 9999.9f );

						Vector ledgeLeft = ledgePos + halfSize * side;
						Vector ledgeRight = ledgePos - halfSize * side;

						NDebugOverlay::Triangle( wallTopRight, wallTopLeft, ledgeLeft, 0, 100, 255, 100, true, 9999.9f );
						NDebugOverlay::Triangle( wallTopRight, ledgeLeft, ledgeRight, 0, 100, 255, 100, true, 9999.9f );
					}
				}

				if ( !mover->ClimbUpToLedge( ledgePos, climbDirection, obstacle ) )
				{
					// climb failed - build a new path in case we're now stuck
					//Invalidate();
					return false;
				}

				return true;
			}
			else if ( bot->IsDebugging( NEXTBOT_PATH ) )
			{
				DevMsg( "%3.2f: CANT FIND LEDGE TO CLIMB\n", gpGlobals->curtime );
			}
		}
	}

	return false;
}


//--------------------------------------------------------------------------------------------------------------
/**
 * Jump over gaps
 */
bool PathFollower::JumpOverGaps( INextBot *bot, const Path::Segment *goal, const Vector &forward, const Vector &right, float goalRange )
{
	VPROF_BUDGET( "PathFollower::JumpOverGaps", "NextBot" );

	ILocomotion *mover = bot->GetLocomotionInterface();
	IBody *body = bot->GetBodyInterface();

	if ( !mover->IsAbleToJumpAcrossGaps() || !NextBotAllowGapJumping.GetBool() )
	{
		return false;
	}

	if ( mover->IsClimbingOrJumping() || mover->IsAscendingOrDescendingLadder() || !mover->IsOnGround() )
	{
		return false;
	}
	
	if ( !body->IsActualPosture( IBody::STAND ) )
	{
		// can't jump if we're not standing
		return false;
	}

	if ( m_goal == NULL )
	{
		return false;
	}

	//trace_t result;
	NextBotTraversableTraceFilter filter( bot, ILocomotion::IMMEDIATELY );

	const float hullWidth = ( body ) ? body->GetHullWidth() : 1.0f;

	// 'current' is the segment we are on/just passed over
	const Segment *current = PriorSegment( m_goal );
	if ( current == NULL )
	{
		return false;
	}

	const float minGapJumpRange = 2.0f * hullWidth;

	const Segment *gap = NULL;

	if ( current->type == JUMP_OVER_GAP )
	{
		gap = current;
	}
	else
	{
		float searchRange = goalRange;
		for( const Segment *s = m_goal; s; s = NextSegment( s ) )
		{
			if ( searchRange > minGapJumpRange )
			{
				break;
			}

			if ( s->type == JUMP_OVER_GAP )
			{
				gap = s;
				break;
			}

			searchRange += s->length;
		}
	}

	if ( gap )
	{
		VPROF_BUDGET( "PathFollower::GapJumping", "NextBot" );

		float halfWidth = hullWidth/2.0f;

		if ( mover->IsGap( mover->GetFeet() + halfWidth * gap->forward, gap->forward ) )
		{
			// there is a gap to jump over
			const Segment *landing = NextSegment( gap );
			if ( landing )
			{
				mover->JumpAcrossGap( landing->pos, landing->forward );

				// if we're jumping over this gap, make sure our goal is the landing so we aim for it
				m_goal = landing;

				if ( bot->IsDebugging( NEXTBOT_PATH ) )
				{
					NDebugOverlay::Cross3D( m_goal->pos, 5.0f, 0, 255, 255, true, 5.0f );
					DevMsg( "%3.2f: GAP JUMP\n", gpGlobals->curtime );
				}
				return true;
			}
		}
	}

	return false;
}


//--------------------------------------------------------------------------------------------------------------
/**
 * Draw the path for debugging
 */
void PathFollower::Draw( const Path::Segment *start ) const
{
	if ( m_goal == NULL )
		return;
		
	// show avoid volumes	
	if ( m_didAvoidCheck )
	{
		QAngle angles( 0, 0, 0 );
		
		if (m_isLeftClear)
			NDebugOverlay::SweptBox( m_leftFrom, m_leftTo, m_hullMin, m_hullMax, angles, 0, 255, 0, 255, 0.1f );
		else
			NDebugOverlay::SweptBox( m_leftFrom, m_leftTo, m_hullMin, m_hullMax, angles, 255, 0, 0, 255, 0.1f );
			
		if (m_isRightClear)
			NDebugOverlay::SweptBox( m_rightFrom, m_rightTo, m_hullMin, m_hullMax, angles, 0, 255, 0, 255, 0.1f );
		else
			NDebugOverlay::SweptBox( m_rightFrom, m_rightTo, m_hullMin, m_hullMax, angles, 255, 0, 0, 255, 0.1f );

		const_cast< PathFollower * >( this )->m_didAvoidCheck = false;
	}

	// highlight current goal segment
	if ( m_goal )
	{
		const float size = 5.0f;	
		NDebugOverlay::Sphere( m_goal->pos, size, 255, 255, 0, true, 0.1f );

		switch( m_goal->how )
		{
		case GO_NORTH:
		case GO_SOUTH:
			NDebugOverlay::Line( m_goal->m_portalCenter - Vector( m_goal->m_portalHalfWidth, 0, 0 ), m_goal->m_portalCenter + Vector( m_goal->m_portalHalfWidth, 0, 0 ), 255, 0, 255, true, 0.1f );
			break;

		default:
			NDebugOverlay::Line( m_goal->m_portalCenter - Vector( 0, m_goal->m_portalHalfWidth, 0 ), m_goal->m_portalCenter + Vector( 0, m_goal->m_portalHalfWidth, 0 ), 255, 0, 255, true, 0.1f );
			break;
		}

		// 'current' is the segment we are on/just passed over
		const Segment *current = PriorSegment( m_goal );
		if ( current )
		{
			NDebugOverlay::Line( current->pos, m_goal->pos, 255, 255, 0, true, 0.1f );
		}
	}

	// extend
	Path::Draw();
}


//--------------------------------------------------------------------------------------------------------------
/**
 * Return true if there is a the given discontinuity ahead in the path within the given range (-1 = entire remaining path)
 */
bool PathFollower::IsDiscontinuityAhead( INextBot *bot, Path::SegmentType type, float range ) const
{
	if ( m_goal )
	{
		const Path::Segment *current = PriorSegment( m_goal );
		if ( current && current->type == type )
		{
			// we're on the discontinuity now
			return true;
		}

		float rangeSoFar = ( m_goal->pos - bot->GetLocomotionInterface()->GetFeet() ).Length();

		for( const Segment *s = m_goal; s; s = NextSegment( s ) )
		{
			if ( rangeSoFar >= range )
			{
				break;
			}

			if ( s->type == type )
			{
				return true;
			}

			rangeSoFar += s->length;
		}
	}

	return false;
}




#ifdef NEO
//--------------------------------------------------------------------------------------------------------------
// NEO-HARNESS-TEMP research arm (2026-09-23, patch 51): local prop detour.
//
// The nav mesh does not know about physics props: they move, and the mesh runs straight under them. When
// the next stretch of the path crosses a solid physics prop (its box grown by a hull half-width, within
// body height of the bot), this lays a small grid over the bot, the props and a rejoin point on the path
// past them, marks a cell free when it is on the nav mesh near the bot's height and outside every grown
// prop box, runs A* across it, straightens the result and steers through its waypoints. Replanned every
// quarter second, so it follows props that swing or get pushed.
ConVar neo_bot_prop_detour( "neo_bot_prop_detour", "0", FCVAR_CHEAT, "Research: plan a local grid detour round physics props on the path ahead" );
ConVar neo_bot_prop_detour_lookahead( "neo_bot_prop_detour_lookahead", "256", FCVAR_CHEAT, "Research: how far along the path to look for props" );
ConVar neo_bot_prop_detour_all_entities( "neo_bot_prop_detour_all_entities", "1", FCVAR_CHEAT, "Research: detour round every solid entity in range (sphere query), not only prop_physics" );
ConVar neo_bot_prop_detour_static( "neo_bot_prop_detour_static", "0", FCVAR_CHEAT, "Research: 1 = static props start and shape detours, 2 = they only shape a detour a moving obstacle started" );
ConVar neo_bot_prop_detour_exact( "neo_bot_prop_detour_exact", "0", FCVAR_CHEAT, "Research: a cell is occupied only when a player box there overlaps the obstacle collision model (else its grown box)" );
ConVar neo_bot_prop_detour_linked( "neo_bot_prop_detour_linked", "0", FCVAR_CHEAT, "Research: detour grid steps only between cells whose nav areas are the same or connected" );
ConVar neo_bot_prop_detour_cell( "neo_bot_prop_detour_cell", "24", FCVAR_CHEAT, "Research: detour grid cell size (grows by x1.5 when the region needs more than 72 cells a side)" );
ConVar neo_bot_prop_detour_perf( "neo_bot_prop_detour_perf", "0", FCVAR_CHEAT, "Research: log detour planning cost every 30 s (NEO_FORENSIC_DETOUR_PERF)" );
ConVar neo_bot_prop_detour_grid_pass( "neo_bot_prop_detour_grid_pass", "0", FCVAR_CHEAT, "Research: once the detour grid is laid, also mark every obstacle inside it (not only those near the bot)" );
ConVar neo_bot_prop_detour_debug( "neo_bot_prop_detour_debug", "0", FCVAR_CHEAT, "Research: log detour plans (NEO_FORENSIC_DETOUR)" );

static int g_nPropDetourGrids = 0;
static long long g_nPropDetourCells = 0;

namespace
{
	struct DetourBox_t { Vector2D lo, hi; CBaseEntity *pEnt; ICollideable *pStatic; bool bTrigger; };

	bool SegmentHitsBox( const Vector2D &a, const Vector2D &b, const DetourBox_t &box, float *tEnter, float *tExit )
	{
		float t0 = 0.0f, t1 = 1.0f;
		const Vector2D d = b - a;
		for ( int axis = 0; axis < 2; ++axis )
		{
			const float lo = box.lo[axis], hi = box.hi[axis];
			if ( fabsf( d[axis] ) < 1e-4f )
			{
				if ( a[axis] < lo || a[axis] > hi )
					return false;
				continue;
			}
			float ta = ( lo - a[axis] ) / d[axis], tb = ( hi - a[axis] ) / d[axis];
			if ( ta > tb ) { float tmp = ta; ta = tb; tb = tmp; }
			t0 = MAX( t0, ta ); t1 = MIN( t1, tb );
			if ( t0 > t1 )
				return false;
		}
		*tEnter = t0; *tExit = t1;
		return true;
	}
}

Vector PathFollower::PropDetour( INextBot *bot, const Vector &goalPos )
{
	if ( !neo_bot_prop_detour.GetBool() )
	{
		m_propDetour.RemoveAll();
		return goalPos;
	}

	if ( m_propDetourTimer.IsElapsed() )
	{
		m_propDetourTimer.Start( 0.25f );
		// NEO-HARNESS-TEMP: planning cost telemetry
		static int s_nCalls = 0, s_nFound = 0;
		static double s_flTotal = 0.0, s_flMax = 0.0, s_flNextLog = 0.0;
		const double t0 = Plat_FloatTime();
		const bool bFound = PlanPropDetour( bot );
		const double dt = Plat_FloatTime() - t0;
		++s_nCalls; s_nFound += bFound ? 1 : 0; s_flTotal += dt; s_flMax = MAX( s_flMax, dt );
		if ( neo_bot_prop_detour_perf.GetBool() && gpGlobals->curtime >= s_flNextLog )
		{
			Msg( "NEO_FORENSIC_DETOUR_PERF t=%.2f calls=%d found=%d total_ms=%.2f mean_us=%.1f max_ms=%.3f grids=%d cells=%lld\n",
				gpGlobals->curtime, s_nCalls, s_nFound, s_flTotal * 1000.0, s_nCalls ? s_flTotal * 1e6 / s_nCalls : 0.0, s_flMax * 1000.0,
				g_nPropDetourGrids, g_nPropDetourCells );
			s_nCalls = s_nFound = 0; s_flTotal = s_flMax = 0.0; g_nPropDetourGrids = 0; g_nPropDetourCells = 0;
			s_flNextLog = gpGlobals->curtime + 30.0;
		}
	}

	const Vector &feet = bot->GetLocomotionInterface()->GetFeet();
	while ( m_propDetour.Count() && ( m_propDetour[0].AsVector2D() - feet.AsVector2D() ).IsLengthLessThan( 12.0f ) )
	{
		m_propDetour.Remove( 0 );
	}
	return m_propDetour.Count() ? m_propDetour[0] : goalPos;
}

bool PathFollower::PlanPropDetour( INextBot *bot )
{
	m_propDetour.RemoveAll();
	if ( !m_goal || m_goal->ladder || m_goal->type != ON_GROUND )
	{
		return false;
	}

	ILocomotion *mover = bot->GetLocomotionInterface();
	const Vector feet = mover->GetFeet();
	const float flGrow = 18.0f;		// hull half-width + margin
	const float flLookAhead = neo_bot_prop_detour_lookahead.GetFloat();

	// the path ahead as a polyline, starting at the bot
	CUtlVector< Vector > line;
	line.AddToTail( feet );
	float flLen = 0.0f;
	for ( const Segment *s = m_goal; s && flLen < flLookAhead; s = NextSegment( s ) )
	{
		if ( s->ladder || ( s->type != ON_GROUND && s != m_goal ) )
			break;
		flLen += ( s->pos - line.Tail() ).Length2D();
		line.AddToTail( s->pos );
	}
	if ( line.Count() < 2 )
		return false;

	// solid entities near the bot that stand in its body space (a sphere query over every entity, so
	// dynamic props and physboxes count too, not only prop_physics)
	CUtlVector< DetourBox_t > boxes;
	const bool bAllEnts = neo_bot_prop_detour_all_entities.GetBool();
	const int iStaticMode = neo_bot_prop_detour_static.GetInt();
	auto addEntity = [&]( CBaseEntity *pEnt, float zLo, float zHi ) {
		if ( !pEnt || pEnt == bot->GetEntity() || pEnt->IsWorld() || pEnt->IsPlayer() || pEnt->IsBaseCombatWeapon() )
			return;
		if ( !bAllEnts && !FClassnameIs( pEnt, "prop_physics*" ) )
			return;
		if ( !pEnt->IsSolid() || pEnt->IsSolidFlagSet( FSOLID_NOT_SOLID | FSOLID_TRIGGER ) )
			return;
		const int iGroup = pEnt->GetCollisionGroup();
		if ( iGroup == COLLISION_GROUP_DEBRIS || iGroup == COLLISION_GROUP_DEBRIS_TRIGGER || iGroup == COLLISION_GROUP_INTERACTIVE_DEBRIS
			|| iGroup == COLLISION_GROUP_WEAPON || iGroup == COLLISION_GROUP_PLAYER || iGroup == COLLISION_GROUP_PLAYER_MOVEMENT )
			return;
		// doors open for bots; glass is shot out (CNEOBotPathClearBreakable)
		if ( FClassnameIs( pEnt, "prop_door*" ) || FClassnameIs( pEnt, "func_door*" ) || FClassnameIs( pEnt, "func_breakable_surf" ) )
			return;
		// anything the bot already walks through or breaks right away (non-solid brushes, breakables)
		if ( !FClassnameIs( pEnt, "prop_physics*" ) && mover->IsEntityTraversable( pEnt, ILocomotion::IMMEDIATELY ) )
			return;
		for ( int b = 0; b < boxes.Count(); ++b )
			if ( boxes[b].pEnt == pEnt )
				return;
		Vector lo, hi;
		pEnt->CollisionProp()->WorldSpaceAABB( &lo, &hi );
		// small in every direction (a can, a bottle): not worth steering round. Thin but tall (a post,
		// a pipe) still blocks a hull that brushes it
		if ( ( hi.x - lo.x ) < 16.0f && ( hi.y - lo.y ) < 16.0f && ( hi.z - lo.z ) < 40.0f )
			return;
		// a box this big is a structural brush entity the nav mesh was built round already
		if ( ( hi.x - lo.x ) > 512.0f || ( hi.y - lo.y ) > 512.0f )
			return;
		if ( lo.z > zHi + HumanHeight || hi.z < zLo + StepHeight )
			return;
		DetourBox_t box;
		box.lo.Init( lo.x - flGrow, lo.y - flGrow );
		box.hi.Init( hi.x + flGrow, hi.y + flGrow );
		box.pEnt = pEnt; box.pStatic = NULL; box.bTrigger = true;
		boxes.AddToTail( box );
	};
	// static props: the nav mesh was generated through them (brush-only traces); 1 = they start and
	// shape detours, 2 = they only shape a detour a moving obstacle started
	auto addStatics = [&]( const Vector &qlo, const Vector &qhi, float zLo, float zHi ) {
		extern IStaticPropMgrServer *staticpropmgr;
		CUtlVector< ICollideable * > statics;
		staticpropmgr->GetAllStaticPropsInAABB( qlo, qhi, &statics );
		for ( int s = 0; s < statics.Count(); ++s )
		{
			ICollideable *pColl = statics[s];
			if ( !pColl || pColl->GetSolid() == SOLID_NONE )
				continue;
			bool bHave = false;
			for ( int b = 0; b < boxes.Count() && !bHave; ++b )
				bHave = boxes[b].pStatic == pColl;
			if ( bHave )
				continue;
			Vector lo, hi;
			pColl->WorldSpaceSurroundingBounds( &lo, &hi );
			if ( ( hi.x - lo.x ) > 512.0f || ( hi.y - lo.y ) > 512.0f )
				continue;
			if ( lo.z > zHi + HumanHeight || hi.z < zLo + StepHeight )
				continue;
			DetourBox_t box;
			box.lo.Init( lo.x - flGrow, lo.y - flGrow );
			box.hi.Init( hi.x + flGrow, hi.y + flGrow );
			box.pEnt = NULL; box.pStatic = pColl;
			box.bTrigger = iStaticMode == 1;
			boxes.AddToTail( box );
		}
	};

	for ( CEntitySphereQuery sphere( feet, flLookAhead + 64.0f ); CBaseEntity *pEnt = sphere.GetCurrentEntity(); sphere.NextEntity() )
	{
		addEntity( pEnt, feet.z, feet.z );
	}
	if ( iStaticMode == 1 || ( iStaticMode == 2 && boxes.Count() > 0 ) )
	{
		const float r = flLookAhead + 64.0f;
		addStatics( feet - Vector( r, r, 64.0f ), feet + Vector( r, r, HumanHeight + 64.0f ), feet.z, feet.z );
	}
	if ( boxes.Count() == 0 )
		return false;

	// where along the polyline the props are: rejoin 32 u past the last one hit
	bool bHit = false;
	int iRejoinSeg = -1;
	float flRejoinT = 0.0f;
	for ( int i = 0; i + 1 < line.Count(); ++i )
	{
		for ( int b = 0; b < boxes.Count(); ++b )
		{
			float t0, t1;
			if ( boxes[b].bTrigger && SegmentHitsBox( line[i].AsVector2D(), line[i + 1].AsVector2D(), boxes[b], &t0, &t1 ) )
			{
				bHit = true;
				if ( i > iRejoinSeg || ( i == iRejoinSeg && t1 > flRejoinT ) )
				{
					iRejoinSeg = i;
					flRejoinT = t1;
				}
			}
		}
	}
	if ( !bHit )
		return false;

	Vector rejoin;
	{
		Vector a = line[iRejoinSeg], b = line[iRejoinSeg + 1];
		Vector dir = b - a; dir.z = 0.0f;
		float flSeg = dir.NormalizeInPlace();
		float flAt = flRejoinT * flSeg + 32.0f;
		int i = iRejoinSeg;
		while ( flAt > flSeg && i + 2 < line.Count() )
		{
			flAt -= flSeg; ++i;
			a = line[i]; b = line[i + 1]; dir = b - a; dir.z = 0.0f; flSeg = dir.NormalizeInPlace();
		}
		flAt = MIN( flAt, flSeg );
		rejoin = a + dir * flAt;
		rejoin.z = a.z + ( b.z - a.z ) * ( flSeg > 0.0f ? flAt / flSeg : 0.0f );
	}

	// the grid
	Vector2D gLo( MIN( feet.x, rejoin.x ), MIN( feet.y, rejoin.y ) ), gHi( MAX( feet.x, rejoin.x ), MAX( feet.y, rejoin.y ) );
	for ( int b = 0; b < boxes.Count(); ++b )
	{
		gLo.x = MIN( gLo.x, boxes[b].lo.x ); gLo.y = MIN( gLo.y, boxes[b].lo.y );
		gHi.x = MAX( gHi.x, boxes[b].hi.x ); gHi.y = MAX( gHi.y, boxes[b].hi.y );
	}
	gLo -= Vector2D( 48.0f, 48.0f ); gHi += Vector2D( 48.0f, 48.0f );
	const int nMax = 72;
	float flCell = MAX( 4.0f, neo_bot_prop_detour_cell.GetFloat() );
	while ( ( gHi.x - gLo.x ) / flCell > nMax || ( gHi.y - gLo.y ) / flCell > nMax )
		flCell *= 1.5f;
	// Everything else standing inside the grid counts too - a prop just past the rejoin point or beside
	// the detour, outside the first query's sphere, would otherwise send the bot from one prop into the
	// next. Heights span the bot's and the rejoin point's floors.
	if ( neo_bot_prop_detour_grid_pass.GetBool() )
	{
		const float zLo = MIN( feet.z, rejoin.z ), zHi = MAX( feet.z, rejoin.z );
		const Vector qlo( gLo.x, gLo.y, zLo - 64.0f ), qhi( gHi.x, gHi.y, zHi + HumanHeight + 64.0f );
		CBaseEntity *list[ 256 ];
		const int nEnts = UTIL_EntitiesInBox( list, ARRAYSIZE( list ), qlo, qhi, 0 );
		for ( int e = 0; e < nEnts; ++e )
		{
			addEntity( list[e], zLo, zHi );
		}
		if ( iStaticMode >= 1 )
		{
			addStatics( qlo, qhi, zLo, zHi );
		}
	}

	const int nx = (int)ceilf( ( gHi.x - gLo.x ) / flCell ), ny = (int)ceilf( ( gHi.y - gLo.y ) / flCell );
	++g_nPropDetourGrids; g_nPropDetourCells += nx * ny;

	CUtlVector< unsigned char > free; free.SetCount( nx * ny );
	CUtlVector< unsigned char > blocked; blocked.SetCount( nx * ny );
	for ( int i = 0; i < nx * ny; ++i ) blocked[i] = 0;
	const bool bExact = neo_bot_prop_detour_exact.GetBool();
	const float flHull = 15.0f, flBodyTop = 64.0f;
	CUtlVector< float > height; height.SetCount( nx * ny );
	CUtlVector< const CNavArea * > cellArea; cellArea.SetCount( nx * ny );
	const float flZTol = 40.0f;
	for ( int y = 0; y < ny; ++y )
	{
		for ( int x = 0; x < nx; ++x )
		{
			const int i = y * nx + x;
			const Vector2D c( gLo.x + ( x + 0.5f ) * flCell, gLo.y + ( y + 0.5f ) * flCell );
			free[i] = 0;
			cellArea[i] = NULL;
			const CNavArea *area = TheNavMesh->GetNavArea( Vector( c.x, c.y, feet.z + flZTol ), 2.0f * flZTol );
			if ( !area )
				continue;
			const float z = area->GetZ( c.x, c.y );
			if ( fabsf( z - feet.z ) > flZTol && fabsf( z - rejoin.z ) > flZTol )
				continue;
			bool bInProp = false, bTriggerHit = false;
			for ( int b = 0; b < boxes.Count() && !bInProp; ++b )
			{
				if ( c.x < boxes[b].lo.x || c.x > boxes[b].hi.x || c.y < boxes[b].lo.y || c.y > boxes[b].hi.y )
					continue;
				if ( !bExact )
				{
					bInProp = true;
					bTriggerHit = boxes[b].bTrigger;
					continue;
				}
				// a player-sized box standing here, from a step over the floor to crouch-plus height:
				// does it overlap the obstacle's own collision model?
				Ray_t ray;
				const Vector at( c.x, c.y, z );
				ray.Init( at, at, Vector( -flHull, -flHull, StepHeight ), Vector( flHull, flHull, flBodyTop ) );
				trace_t tr;
				if ( boxes[b].pEnt )
					enginetrace->ClipRayToEntity( ray, MASK_PLAYERSOLID, boxes[b].pEnt, &tr );
				else
					enginetrace->ClipRayToCollideable( ray, MASK_PLAYERSOLID, boxes[b].pStatic, &tr );
				bInProp = tr.startsolid || tr.allsolid;
				bTriggerHit = bInProp && boxes[b].bTrigger;
			}
			if ( bInProp )
			{
				blocked[i] = bTriggerHit ? 2 : 1;
				continue;
			}
			free[i] = 1;
			height[i] = z;
			cellArea[i] = area;
		}
	}

	auto cellOf = [&]( const Vector &p, int &cx, int &cy ) {
		cx = clamp( (int)( ( p.x - gLo.x ) / flCell ), 0, nx - 1 );
		cy = clamp( (int)( ( p.y - gLo.y ) / flCell ), 0, ny - 1 );
	};
	auto nearestFree = [&]( int &cx, int &cy ) -> bool {
		for ( int r = 0; r <= 3; ++r )
			for ( int dy = -r; dy <= r; ++dy )
				for ( int dx = -r; dx <= r; ++dx )
				{
					const int x = cx + dx, y = cy + dy;
					if ( x >= 0 && y >= 0 && x < nx && y < ny && free[y * nx + x] ) { cx = x; cy = y; return true; }
				}
		return false;
	};
	// Two free cells are only neighbours if the mesh lets a bot walk between them: the same area, or areas
	// with a connection either way. Being on the mesh is not enough - areas either side of a thin wall
	// are both "free" cells
	const bool bLinked = neo_bot_prop_detour_linked.GetBool();
	auto cellsLinked = [&]( int a, int b ) -> bool {
		if ( !bLinked || cellArea[a] == cellArea[b] )
			return true;
		return cellArea[a] && cellArea[b] && ( cellArea[a]->IsConnected( cellArea[b], NUM_DIRECTIONS ) || cellArea[b]->IsConnected( cellArea[a], NUM_DIRECTIONS ) );
	};

	// does the path, as it stands, run through an occupied cell? If not, nothing to steer round
	{
		bool bCrosses = false;
		for ( int i = 0; i + 1 < line.Count() && !bCrosses; ++i )
		{
			const float flSeg = ( line[i + 1] - line[i] ).Length2D();
			const int steps = MAX( 1, (int)( flSeg / ( flCell * 0.5f ) ) );
			for ( int k = 0; k <= steps && !bCrosses; ++k )
			{
				const Vector p = line[i] + ( line[i + 1] - line[i] ) * ( (float)k / steps );
				int px, py; cellOf( p, px, py );
				bCrosses = blocked[py * nx + px] == 2;
			}
		}
		if ( !bCrosses )
			return false;
	}

	int sx, sy, tx, ty;
	cellOf( feet, sx, sy ); cellOf( rejoin, tx, ty );
	if ( !nearestFree( sx, sy ) || !nearestFree( tx, ty ) )
		return false;

	// A*, 8-connected; no corner cutting past a blocked cell
	CUtlVector< float > g; g.SetCount( nx * ny );
	CUtlVector< int > parent; parent.SetCount( nx * ny );
	CUtlVector< unsigned char > closed; closed.SetCount( nx * ny );
	for ( int i = 0; i < nx * ny; ++i ) { g[i] = FLT_MAX; parent[i] = -1; closed[i] = 0; }
	struct Open_t { float f; int i; };
	CUtlVector< Open_t > open;
	const int start = sy * nx + sx, target = ty * nx + tx;
	g[start] = 0.0f;
	open.AddToTail( { 0.0f, start } );
	bool bFound = false;
	int nExpanded = 0;
	while ( open.Count() && nExpanded < 6000 )
	{
		int best = 0;
		for ( int k = 1; k < open.Count(); ++k )
			if ( open[k].f < open[best].f ) best = k;
		const int cur = open[best].i;
		open.FastRemove( best );
		if ( closed[cur] ) continue;
		closed[cur] = 1; ++nExpanded;
		if ( cur == target ) { bFound = true; break; }
		const int cx = cur % nx, cy = cur / nx;
		for ( int dy = -1; dy <= 1; ++dy )
			for ( int dx = -1; dx <= 1; ++dx )
			{
				if ( !dx && !dy ) continue;
				const int x = cx + dx, y = cy + dy;
				if ( x < 0 || y < 0 || x >= nx || y >= ny ) continue;
				const int n = y * nx + x;
				if ( !free[n] || closed[n] ) continue;
				if ( dx && dy && ( !free[cy * nx + x] || !free[y * nx + cx] ) ) continue;
				if ( !cellsLinked( cur, n ) ) continue;
				const float step = ( dx && dy ) ? 1.4142f : 1.0f;
				if ( g[cur] + step < g[n] )
				{
					g[n] = g[cur] + step; parent[n] = cur;
					const float h = sqrtf( (float)( ( x - tx ) * ( x - tx ) + ( y - ty ) * ( y - ty ) ) );
					open.AddToTail( { g[n] + h, n } );
				}
			}
	}

	if ( neo_bot_prop_detour_debug.GetBool() )
	{
		Msg( "NEO_FORENSIC_DETOUR t=%.2f p=%d props=%d grid=%dx%d cell=%.0f found=%d len=%.0f from=%.0f,%.0f to=%.0f,%.0f\n",
			gpGlobals->curtime, bot->GetEntity()->entindex(), boxes.Count(), nx, ny, flCell, bFound ? 1 : 0,
			bFound ? g[target] * flCell : 0.0f, feet.x, feet.y, rejoin.x, rejoin.y );
	}
	if ( !bFound )
		return false;

	// back-track, then keep only the cells a straight grid line cannot skip
	CUtlVector< int > cells;
	for ( int i = target; i != -1; i = parent[i] ) cells.AddToHead( i );
	auto lineFree = [&]( int a, int b ) -> bool {
		const int ax = a % nx, ay = a / nx, bx = b % nx, by = b / nx;
		const int steps = MAX( abs( bx - ax ), abs( by - ay ) ) * 2;
		int prev = a;
		for ( int k = 0; k <= steps; ++k )
		{
			const float t = steps ? (float)k / steps : 0.0f;
			const int x = (int)( ax + ( bx - ax ) * t + 0.5f ), y = (int)( ay + ( by - ay ) * t + 0.5f );
			const int cell = y * nx + x;
			if ( !free[cell] ) return false;
			if ( !cellsLinked( prev, cell ) ) return false;
			prev = cell;
		}
		return true;
	};
	int anchor = 0;
	for ( int k = 1; k < cells.Count(); ++k )
	{
		if ( k == cells.Count() - 1 || !lineFree( cells[anchor], cells[k + 1] ) )
		{
			const int i = cells[k];
			m_propDetour.AddToTail( Vector( gLo.x + ( i % nx + 0.5f ) * flCell, gLo.y + ( i / nx + 0.5f ) * flCell, height[i] ) );
			anchor = k;
		}
	}
	return m_propDetour.Count() > 0;
}
#endif // NEO
