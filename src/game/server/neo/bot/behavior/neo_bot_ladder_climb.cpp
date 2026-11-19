#include "cbase.h"
#include "bot/behavior/neo_bot_attack.h"
#include "bot/behavior/neo_bot_ladder_climb.h"
#include "nav_ladder.h"
#include "NextBot/Path/NextBotPathFollow.h"
#include "NextBotUtil.h"
#include "movevars_shared.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

// The engine grabs a ladder within this distance of its face (CGameMovement::LadderDistance())
static constexpr float LADDER_GRAB_DIST = 2.0f;
// A hull placed off a ladder starts this far clear of the face,
static constexpr float LADDER_PLACE_GAP = 2.0f;
// and is slid at most this far towards the face to find it.
static constexpr float LADDER_PLACE_SEEK = 16.0f;
static constexpr float RECOVERY_NORMAL_ALIGNMENT = 0.9f;

static constexpr float RECOVERY_CONTACT_TIME = 0.2f;
static constexpr float RECOVERY_SLIP_HEIGHT = 16.0f;
static constexpr float RECOVERY_SLIP_TIME = 0.2f;
static constexpr float RECOVERY_DETACH_TIME = 0.2f;
static constexpr float RECOVERY_BOUNCE_MARGIN = 16.0f;
static constexpr float RECOVERY_STALL_WINDOW = 0.4f;
static constexpr float RECOVERY_STALL_PROGRESS = 2.0f;
static constexpr int RECOVERY_STALL_WINDOWS = 2;
static constexpr float RECOVERY_DISMOUNT_GRACE = 3.0f;
static constexpr float RECOVERY_OBJECTIVE_TOLERANCE = 4.0f;
static constexpr float RECOVERY_VIEW_ALIGNMENT = 0.9f;
static constexpr float RECOVERY_STUCK_CLEAR_PROGRESS = 4.0f;

enum LadderRecoveryLevel
{
	LADDER_RECOVERY_STOCK = 0,
	LADDER_RECOVERY_A,
	LADDER_RECOVERY_B,
	LADDER_RECOVERY_C
};

ConVar neo_bot_ladder_recovery( "neo_bot_ladder_recovery", "0", FCVAR_CHEAT,
	"Bot ladder recovery: 0 = off, 1 = A move to launch point and kick, 2 = B slide along ladder and kick, 3 = C move directly to a verified landing",
	true, LADDER_RECOVERY_STOCK, true, LADDER_RECOVERY_C );

ConVar neo_bot_ladder_recovery_bounce_landing( "neo_bot_ladder_recovery_bounce_landing", "0", FCVAR_CHEAT,
	"Also recover when an upward climber falls back onto the floor at the ladder foot" );
static constexpr float RECOVERY_BOUNCE_RISE = 8.0f;

enum LadderExpiryTeleportLevel
{
	EXPIRY_TELEPORT_OFF = 0,
	EXPIRY_TELEPORT_TIMERS,
	EXPIRY_TELEPORT_ALL
};

ConVar neo_bot_ladder_expiry_teleport( "neo_bot_ladder_expiry_teleport", "0", FCVAR_CHEAT,
	"When a bot's ladder action expires, place the bot on the ladder's exit area. 0 = off, 1 = climb and dismount timers, 2 = also the stall bail and the approach timer",
	true, EXPIRY_TELEPORT_OFF, true, EXPIRY_TELEPORT_ALL );
static constexpr float EXPIRY_TELEPORT_COOLDOWN = 10.0f;
static constexpr float EXPIRY_TELEPORT_APPROACH_RANGE = 48.0f;

static constexpr float RECOVERY_SEARCH_RANGE = 128.0f;
static constexpr float RECOVERY_CANDIDATE_GRID = 8.0f;
static constexpr int RECOVERY_MAX_CANDIDATES = 64;
static constexpr float RECOVERY_LANDING_INSET = 2.0f;
static constexpr float RECOVERY_SUPPORT_GRID = 4.0f;
static constexpr float RECOVERY_FLOOR_PROBE_HEIGHT = 1.0f;
static constexpr float RECOVERY_LANDING_CLEARANCE = 1.0f;
static constexpr float RECOVERY_DISTINCT_HULLS = 0.5f;
static constexpr float RECOVERY_DUPLICATE_TOLERANCE = 0.1f;
static constexpr int RECOVERY_PATH_SEGMENTS = 100;

//---------------------------------------------------------------------------------------------
bool CNEOBotLadderClimb::ResolveRecoveryExit( CNEOBot *me )
{
	CNEOBot::LadderRecoveryState &state = me->m_ladderRecovery;
	const PathFollower *path = me->GetCurrentPath();
	if ( !path || !path->IsValid() || !state.ladder )
	{
		return false;
	}

	const Path::Segment *ladderSegment = nullptr;
	const Path::Segment *segment = path->GetCurrentGoal();
	for ( int i = 0; segment && i < RECOVERY_PATH_SEGMENTS; ++i, segment = path->NextSegment( segment ) )
	{
		if ( segment->ladder == state.ladder && segment->type == Path::LADDER_UP && segment->how == GO_LADDER_UP )
		{
			ladderSegment = segment;
			break;
		}
	}

	// Keep the exit when normal dismount advances the path cursor past the ladder.
	if ( !ladderSegment )
	{
		return state.intendedDismount && state.landingArea;
	}
	if ( !ladderSegment->area || !state.ladder->IsConnected( ladderSegment->area, CNavLadder::LADDER_UP ) )
	{
		return false;
	}

	state.landingArea = ladderSegment->area;
	const Path::Segment *next = path->NextSegment( ladderSegment );
	if ( next && next->area && next->area != state.landingArea && !next->ladder
		&& next->type == Path::ON_GROUND && next->how >= GO_NORTH && next->how <= GO_WEST
		&& state.landingArea->IsConnected( next->area, static_cast<NavDirType>( next->how ) )
		&& state.landingArea->IsContiguous( next->area ) )
	{
		state.landingNextArea = next->area;
	}

	const Vector size = me->WorldAlignMaxs() - me->WorldAlignMins();
	const bool narrow = state.landingArea->GetSizeX() < size.x + RECOVERY_LANDING_INSET * 2.0f
		|| state.landingArea->GetSizeY() < size.y + RECOVERY_LANDING_INSET * 2.0f;
	state.landingUnionArea = narrow ? state.landingNextArea : nullptr;
	return true;
}

//---------------------------------------------------------------------------------------------
void CNEOBotLadderClimb::SaveLandingProbe( LandingProbe *probe, const trace_t &trace, const char *gate )
{
	probe->gate = gate;
	probe->hit = trace.m_pEnt ? trace.m_pEnt->entindex() : -1;
}

//---------------------------------------------------------------------------------------------
bool CNEOBotLadderClimb::CheckLandingSupport( CNEOBot *me, const Vector &sample, float floorZ, LandingProbe *probe, LandingSearchPurpose purpose )
{
	const CNEOBot::LadderRecoveryState &state = me->m_ladderRecovery;
	const CNavArea *unionArea = purpose == RECOVERY_HANDOFF ? state.landingNextArea : state.landingUnionArea;
	const CNavArea *area = state.landingArea->IsOverlapping( sample ) ? state.landingArea : unionArea;
	if ( !area || !area->IsOverlapping( sample ) )
	{
		probe->gate = "no_support";
		return false;
	}

	const float step = me->GetLocomotionInterface()->GetStepHeight();
	const float navZ = area->GetZ( sample );
	const Vector from( sample.x, sample.y, navZ + step );
	const Vector to( sample.x, sample.y, navZ - step );
	trace_t trace;
	NextBotTraceFilterIgnoreActors geometryFilter( me, COLLISION_GROUP_NONE );
	CTraceFilterSimple placementFilter( me, COLLISION_GROUP_NONE );
	ITraceFilter *filter = purpose == WATCH_GEOMETRY ? static_cast<ITraceFilter *>( &geometryFilter ) : &placementFilter;
	UTIL_TraceLine( from, to, MASK_PLAYERSOLID, filter, &trace );
	if ( trace.startsolid || trace.allsolid || !trace.DidHit() || !trace.m_pEnt || trace.m_pEnt->IsPlayer()
		|| trace.m_pEnt->GetMoveType() != MOVETYPE_NONE
		|| trace.plane.normal.z < me->GetLocomotionInterface()->GetTraversableSlopeLimit()
		|| fabsf( trace.endpos.z - floorZ ) > step )
	{
		SaveLandingProbe( probe, trace, "no_support" );
		return false;
	}
	return true;
}

//---------------------------------------------------------------------------------------------
bool CNEOBotLadderClimb::ValidateLanding( CNEOBot *me, const Vector &point, Vector *landing, LandingProbe *probe, LandingSearchPurpose purpose )
{
	const CNEOBot::LadderRecoveryState &state = me->m_ladderRecovery;
	if ( !state.landingArea || ( point - state.ladder->m_top ).AsVector2D().LengthSqr() > Square( RECOVERY_SEARCH_RANGE ) )
	{
		probe->gate = state.landingArea ? "distance_cap" : "no_exit";
		return false;
	}
	const CNavArea *unionArea = purpose == RECOVERY_HANDOFF ? state.landingNextArea : state.landingUnionArea;
	const CNavArea *area = state.landingArea->IsOverlapping( point ) ? state.landingArea : unionArea;
	if ( !area || !area->IsOverlapping( point ) )
	{
		probe->gate = "no_exit";
		return false;
	}

	const Vector mins = me->WorldAlignMins();
	Vector maxs = me->WorldAlignMaxs();
	maxs.z = mins.z + me->GetBodyInterface()->GetStandHullHeight();
	const float step = me->GetLocomotionInterface()->GetStepHeight();
	Vector navPoint = point;
	navPoint.z = area->GetZ( point );
	const Vector vertical( 0.0f, 0.0f, step );
	// Probe the floor without requiring extra ceiling clearance.
	Vector footMaxs = maxs;
	footMaxs.z = mins.z + RECOVERY_FLOOR_PROBE_HEIGHT;
	trace_t floor;
	NextBotTraceFilterIgnoreActors geometryFilter( me, COLLISION_GROUP_NONE );
	CTraceFilterSimple placementFilter( me, COLLISION_GROUP_NONE );
	ITraceFilter *filter = purpose == WATCH_GEOMETRY ? static_cast<ITraceFilter *>( &geometryFilter ) : &placementFilter;
	UTIL_TraceHull( navPoint + vertical, navPoint - vertical, mins, footMaxs, MASK_PLAYERSOLID, filter, &floor );
	if ( floor.startsolid || floor.allsolid )
	{
		SaveLandingProbe( probe, floor, floor.m_pEnt && floor.m_pEnt->IsPlayer() ? "occupied" : "ceiling" );
		return false;
	}
	if ( !floor.DidHit() || !floor.m_pEnt || floor.m_pEnt->IsPlayer() || floor.m_pEnt->GetMoveType() != MOVETYPE_NONE
		|| floor.plane.normal.z < me->GetLocomotionInterface()->GetTraversableSlopeLimit() )
	{
		SaveLandingProbe( probe, floor, "no_support" );
		return false;
	}

	if ( !CheckLandingSupport( me, floor.endpos, floor.endpos.z, probe, purpose ) )
	{
		return false;
	}
	const float supportMinX = mins.x - RECOVERY_LANDING_INSET;
	const float supportMinY = mins.y - RECOVERY_LANDING_INSET;
	const float supportSizeX = maxs.x - mins.x + RECOVERY_LANDING_INSET * 2.0f;
	const float supportSizeY = maxs.y - mins.y + RECOVERY_LANDING_INSET * 2.0f;
	const int samplesX = Max( 1, static_cast<int>( ceilf( supportSizeX / RECOVERY_SUPPORT_GRID ) ) );
	const int samplesY = Max( 1, static_cast<int>( ceilf( supportSizeY / RECOVERY_SUPPORT_GRID ) ) );
	for ( int x = 0; x <= samplesX; ++x )
	{
		for ( int y = 0; y <= samplesY; ++y )
		{
			const Vector sample = floor.endpos + Vector( supportMinX + supportSizeX * x / samplesX,
				supportMinY + supportSizeY * y / samplesY, 0.0f );
			if ( !CheckLandingSupport( me, sample, floor.endpos.z, probe, purpose ) )
			{
				return false;
			}
		}
	}

	*landing = floor.endpos + Vector( 0.0f, 0.0f, RECOVERY_LANDING_CLEARANCE );
	trace_t occupancy;
	UTIL_TraceHull( *landing, *landing, mins, maxs, MASK_PLAYERSOLID, filter, &occupancy );
	if ( occupancy.startsolid || occupancy.allsolid || occupancy.DidHit() )
	{
		SaveLandingProbe( probe, occupancy, occupancy.m_pEnt && occupancy.m_pEnt->IsPlayer() ? "occupied" : "ceiling" );
		return false;
	}
	SaveLandingProbe( probe, floor, "none" );
	return true;
}

//---------------------------------------------------------------------------------------------
int CNEOBotLadderClimb::CompareLandings( const LandingCandidate *left, const LandingCandidate *right )
{
	if ( left->distanceSqr < right->distanceSqr )
	{
		return -1;
	}
	if ( left->distanceSqr > right->distanceSqr )
	{
		return 1;
	}
	return 0;
}

//---------------------------------------------------------------------------------------------
void CNEOBotLadderClimb::FindRecoveryLandings( CNEOBot *me, LandingSearchPurpose purpose )
{
	CNEOBot::LadderRecoveryState &state = me->m_ladderRecovery;
	if ( ( purpose == WATCH_GEOMETRY && state.searchDone ) || ( purpose == RECOVERY_PLACEMENT && state.placementSearchDone ) )
	{
		return;
	}
	state.searchDone = true;
	if ( purpose == RECOVERY_PLACEMENT )
	{
		state.placementSearchDone = true;
	}
	state.landingCount = 0;
	state.searchTested = 0;
	LandingProbe lastRejected;
	if ( !ResolveRecoveryExit( me ) )
	{
		state.searchGate = "no_exit";
		return;
	}

	CUtlVector<LandingCandidate> priority;
	CUtlVector<LandingCandidate> grid;
	const Vector &end = state.ladder->m_top;
	const CNavArea *areas[] = { state.landingArea, state.landingUnionArea };
	for ( int areaIndex = 0; areaIndex < ARRAYSIZE( areas ); ++areaIndex )
	{
		const CNavArea *area = areas[areaIndex];
		if ( !area )
		{
			continue;
		}
		Vector closest;
		area->GetClosestPointOnArea( end, &closest );
		LandingCandidate candidate;
		candidate.position = closest;
		candidate.distanceSqr = ( closest - end ).AsVector2D().LengthSqr();
		if ( areaIndex == 0 )
		{
			priority.AddToTail( candidate );
		}
		else
		{
			grid.AddToTail( candidate );
		}

		const Vector northWest = area->GetCorner( NORTH_WEST );
		const Vector southEast = area->GetCorner( SOUTH_EAST );
		const Vector mins = me->WorldAlignMins();
		const Vector maxs = me->WorldAlignMaxs();
		const float minX = northWest.x - mins.x + RECOVERY_LANDING_INSET;
		const float minY = northWest.y - mins.y + RECOVERY_LANDING_INSET;
		const float maxX = southEast.x - maxs.x - RECOVERY_LANDING_INSET;
		const float maxY = southEast.y - maxs.y - RECOVERY_LANDING_INSET;
		if ( minX <= maxX && minY <= maxY )
		{
			candidate.position.x = Clamp( closest.x, minX, maxX );
			candidate.position.y = Clamp( closest.y, minY, maxY );
			candidate.position.z = area->GetZ( candidate.position );
			candidate.distanceSqr = ( candidate.position - end ).AsVector2D().LengthSqr();
			if ( areaIndex == 0 )
			{
				priority.AddToTail( candidate );
			}
			else
			{
				grid.AddToTail( candidate );
			}
		}
		candidate.position = area->GetCenter();
		candidate.distanceSqr = ( candidate.position - end ).AsVector2D().LengthSqr();
		if ( areaIndex == 0 )
		{
			priority.AddToTail( candidate );
		}
		else
		{
			grid.AddToTail( candidate );
		}

		const float gridMinX = ceilf( Max( northWest.x, end.x - RECOVERY_SEARCH_RANGE ) / RECOVERY_CANDIDATE_GRID ) * RECOVERY_CANDIDATE_GRID;
		const float gridMinY = ceilf( Max( northWest.y, end.y - RECOVERY_SEARCH_RANGE ) / RECOVERY_CANDIDATE_GRID ) * RECOVERY_CANDIDATE_GRID;
		const float gridMaxX = Min( southEast.x, end.x + RECOVERY_SEARCH_RANGE );
		const float gridMaxY = Min( southEast.y, end.y + RECOVERY_SEARCH_RANGE );
		for ( float x = gridMinX; x <= gridMaxX; x += RECOVERY_CANDIDATE_GRID )
		{
			for ( float y = gridMinY; y <= gridMaxY; y += RECOVERY_CANDIDATE_GRID )
			{
				candidate.position.Init( x, y, area->GetZ( x, y ) );
				candidate.distanceSqr = ( candidate.position - end ).AsVector2D().LengthSqr();
				if ( candidate.distanceSqr <= Square( RECOVERY_SEARCH_RANGE ) )
				{
					grid.AddToTail( candidate );
				}
			}
		}
	}
	grid.Sort( CompareLandings );

	CUtlVector<Vector> tested;
	const int total = priority.Count() + grid.Count();
	for ( int i = 0; i < total && state.searchTested < RECOVERY_MAX_CANDIDATES; ++i )
	{
		const LandingCandidate &candidate = i < priority.Count() ? priority[i] : grid[i - priority.Count()];
		if ( candidate.distanceSqr > Square( RECOVERY_SEARCH_RANGE ) )
		{
			continue;
		}
		bool duplicate = false;
		FOR_EACH_VEC( tested, testIndex )
		{
			if ( tested[testIndex].DistToSqr( candidate.position ) < Square( RECOVERY_DUPLICATE_TOLERANCE ) )
			{
				duplicate = true;
				break;
			}
		}
		if ( duplicate )
		{
			continue;
		}
		tested.AddToTail( candidate.position );
		++state.searchTested;
		LandingProbe probe;
		Vector landing;
		if ( !ValidateLanding( me, candidate.position, &landing, &probe, purpose ) )
		{
			lastRejected = probe;
			continue;
		}
		const float distinct = me->GetBodyInterface()->GetHullWidth() * RECOVERY_DISTINCT_HULLS;
		if ( state.landingCount && ( landing - state.landings[0] ).AsVector2D().LengthSqr() < Square( distinct ) )
		{
			continue;
		}
		state.landings[state.landingCount++] = landing;
		if ( state.landingCount == CNEOBot::LadderRecoveryState::LANDING_SLOTS )
		{
			break;
		}
	}
	state.searchGate = state.landingCount ? "none" : ( state.searchTested ? lastRejected.gate : "distance_cap" );
}

//---------------------------------------------------------------------------------------------
bool CNEOBotLadderClimb::IsRecoveryCorridor( CNEOBot *me )
{
	const CNEOBot::LadderRecoveryState &state = me->m_ladderRecovery;
	if ( !state.landingCount )
	{
		return false;
	}
	const Vector &from = me->GetAbsOrigin();
	const Vector &landing = state.landings[0];
	if ( from.z < landing.z - RECOVERY_LANDING_CLEARANCE
		|| from.z > landing.z + me->GetLocomotionInterface()->GetStepHeight()
		|| ( from - landing ).AsVector2D().LengthSqr() > Square( RECOVERY_SEARCH_RANGE ) )
	{
		return false;
	}
	Vector maxs = me->WorldAlignMaxs();
	maxs.z = me->WorldAlignMins().z + me->GetBodyInterface()->GetStandHullHeight();
	trace_t egress;
	UTIL_TraceHull( from, landing, me->WorldAlignMins(), maxs, MASK_PLAYERSOLID, me, COLLISION_GROUP_NONE, &egress );
	return !egress.startsolid && !egress.allsolid && !egress.DidHit();
}

static constexpr float RECOVERY_EPISODE_TIME = 3.0f;
static constexpr float RECOVERY_DISTANCE_CAP = 256.0f;
static constexpr float RECOVERY_AIM_TIME = 0.1f;
static constexpr float RECOVERY_SETTLE_TIMEOUT = 0.5f;
static constexpr float RECOVERY_GROUNDED_TIME = 0.2f;
static constexpr float RECOVERY_LANDING_TOLERANCE = 4.0f;
static constexpr float RECOVERY_HANDOFF_DISTANCE = 32.0f;
static constexpr float RECOVERY_HANDOFF_MINIMUM = 16.0f;
static constexpr float RECOVERY_HANDOFF_SPEED = 80.0f;
static constexpr float RECOVERY_COOLDOWN = 5.0f;
static constexpr float RECOVERY_FAILURE_EXCLUSION = 10.0f;
static constexpr int RECOVERY_NEUTRAL_TICKS = 1;

//---------------------------------------------------------------------------------------------
bool CNEOBot::OwnsLadderRecoveryInputs() const
{
	return m_ladderRecovery.ownsInputs;
}

//---------------------------------------------------------------------------------------------
bool CNEOBot::IsLadderRecoveryBlocked( const CNavLadder *ladder, bool goingUp ) const
{
	if ( !ladder || neo_bot_ladder_recovery.GetInt() == LADDER_RECOVERY_STOCK )
	{
		return false;
	}
	FOR_EACH_VEC( m_ladderRecoveryBlocks, i )
	{
		const LadderRecoveryBlock &block = m_ladderRecoveryBlocks[i];
		if ( block.ladderID == ladder->GetID() && block.goingUp == goingUp && block.until > gpGlobals->curtime )
		{
			return true;
		}
	}
	return false;
}

//---------------------------------------------------------------------------------------------
void CNEOBot::FilterUserCommand( CUserCmd *cmd )
{
	const LadderRecoveryState &state = m_ladderRecovery;
	if ( !state.ownsInputs && gpGlobals->tickcount > m_ladderNeutralThroughTick )
	{
		return;
	}

	cmd->buttons &= ~( IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT | IN_JUMP | IN_SPEED );
	cmd->forwardmove = 0.0f;
	cmd->sidemove = 0.0f;
	cmd->upmove = 0.0f;
	if ( !state.ownsInputs )
	{
		return;
	}
	if ( state.keepCrouch )
	{
		cmd->buttons |= IN_DUCK;
	}
	else
	{
		cmd->buttons &= ~IN_DUCK;
	}
	if ( state.phase != LadderRecoveryState::WALKING_OUT )
	{
		return;
	}

	Vector direction = state.handoffTarget - GetAbsOrigin();
	direction.z = 0.0f;
	direction.NormalizeInPlace();
	Vector forward;
	Vector right;
	AngleVectors( QAngle( 0.0f, cmd->viewangles.y, 0.0f ), &forward, &right, nullptr );
	cmd->forwardmove = DotProduct( forward, direction ) * RECOVERY_HANDOFF_SPEED;
	cmd->sidemove = DotProduct( right, direction ) * RECOVERY_HANDOFF_SPEED;
}

//---------------------------------------------------------------------------------------------
void CNEOBot::ClearLadderMovementInputs()
{
	ReleaseForwardButton();
	ReleaseBackwardButton();
	ReleaseLeftButton();
	ReleaseRightButton();
	ReleaseMoveUpButton();
	ReleaseMoveDownButton();
	ReleaseJumpButton();
	ReleaseRunButton();
	ReleaseWalkButton();
	const int movement = IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT | IN_JUMP | IN_SPEED | IN_WALK;
	m_inputButtons &= ~movement;
	m_prevInputButtons &= ~movement;
	m_moveDirection.Init();
	m_moveDirectionTick = -1;
}

//---------------------------------------------------------------------------------------------
void CNEOBotLadderClimb::ClearRecoveryInputs( CNEOBot *me )
{
	me->ClearLadderMovementInputs();
}

//---------------------------------------------------------------------------------------------
void CNEOBotLadderClimb::BlockRecoveryLadder( CNEOBot *me, float duration )
{
	const CNEOBot::LadderRecoveryState &state = me->m_ladderRecovery;
	FOR_EACH_VEC_BACK( me->m_ladderRecoveryBlocks, i )
	{
		CNEOBot::LadderRecoveryBlock &block = me->m_ladderRecoveryBlocks[i];
		if ( block.until <= gpGlobals->curtime )
		{
			me->m_ladderRecoveryBlocks.FastRemove( i );
			continue;
		}
		if ( block.ladderID == state.ladderID && block.goingUp == state.goingUp )
		{
			block.until = Max( block.until, gpGlobals->curtime + duration );
			return;
		}
	}
	CNEOBot::LadderRecoveryBlock block;
	block.ladderID = state.ladderID;
	block.goingUp = state.goingUp;
	block.until = gpGlobals->curtime + duration;
	me->m_ladderRecoveryBlocks.AddToTail( block );
}

//---------------------------------------------------------------------------------------------
bool CNEOBotLadderClimb::CheckRecoverySource( CNEOBot *me )
{
	if ( me->GetMoveType() != MOVETYPE_LADDER )
	{
		return true;
	}
	const CNEOBot::LadderRecoveryState &state = me->m_ladderRecovery;
	const Vector from = me->GetAbsOrigin();
	const Vector line = state.ladder->GetPosAtHeight( from.z );
	const float range = me->GetBodyInterface()->GetHullWidth() + RECOVERY_BOUNCE_MARGIN;
	const float step = me->GetLocomotionInterface()->GetStepHeight();
	if ( ( from - line ).AsVector2D().LengthSqr() > Square( range )
		|| from.z < state.ladder->m_bottom.z - step || from.z > state.ladder->m_top.z + step )
	{
		return false;
	}
	Vector normal = state.ladder->GetNormal();
	if ( DotProduct( from - line, normal ) < 0.0f )
	{
		normal = -normal;
	}
	trace_t face;
	UTIL_TraceHull( from, from - normal * LADDER_GRAB_DIST, me->WorldAlignMins(), me->WorldAlignMaxs(),
		MASK_PLAYERSOLID, me, COLLISION_GROUP_NONE, &face );
	if ( face.startsolid || face.allsolid || !face.DidHit() || !face.m_pEnt || face.m_pEnt->IsPlayer()
		|| DotProduct( face.plane.normal, normal ) < RECOVERY_NORMAL_ALIGNMENT )
	{
		return false;
	}
	if ( face.contents & CONTENTS_LADDER )
	{
		return true;
	}
	const surfacedata_t *surface = physprops->GetSurfaceData( face.surface.surfaceProps );
	return surface && surface->game.climbable;
}

//---------------------------------------------------------------------------------------------
bool CNEOBotLadderClimb::StartRecoveryC( CNEOBot *me )
{
	CNEOBot::LadderRecoveryState &state = me->m_ladderRecovery;
	if ( state.episodeUsed || !state.landingCount || me->IsLadderRecoveryBlocked( state.ladder, state.goingUp ) )
	{
		return false;
	}
	state.episodeUsed = true;
	state.episodeStart = me->GetAbsOrigin();
	state.deadline = gpGlobals->curtime + RECOVERY_EPISODE_TIME;
	state.phaseDeadline = gpGlobals->curtime;
	if ( me->GetGroundEntity() || me->GetMoveType() == MOVETYPE_LADDER )
	{
		state.phaseDeadline += RECOVERY_AIM_TIME;
	}
	state.phase = CNEOBot::LadderRecoveryState::AIMING;
	state.level = neo_bot_ladder_recovery.GetInt();
	state.round = NEORules()->roundNumber();
	state.savedMoveType = me->GetMoveType();
	state.ownsInputs = true;
	state.keepCrouch = ( me->GetFlags() & FL_DUCKING ) != 0;
	ClearRecoveryInputs( me );
	BlockRecoveryLadder( me, RECOVERY_COOLDOWN );

	return true;
}

//---------------------------------------------------------------------------------------------
bool CNEOBotLadderClimb::PlaceRecoveryC( CNEOBot *me )
{
	CNEOBot::LadderRecoveryState &state = me->m_ladderRecovery;
	LandingProbe probe;
	probe.gate = "no_support";
	while ( state.nextPlacement < state.landingCount && state.placements < CNEOBot::LadderRecoveryState::LANDING_SLOTS )
	{
		const Vector candidate = state.landings[state.nextPlacement++];
		Vector landing;
		if ( !ValidateLanding( me, candidate, &landing, &probe, RECOVERY_PLACEMENT ) )
		{
			continue;
		}
		if ( landing.DistToSqr( state.episodeStart ) > Square( RECOVERY_DISTANCE_CAP ) )
		{
			probe.gate = "distance_cap";
			continue;
		}
		ClearRecoveryInputs( me );
		me->ReleaseCrouchButton();
		state.keepCrouch = false;
		me->SetMoveType( MOVETYPE_WALK );
		state.savedMoveType = MOVETYPE_WALK;
		me->SetGroundEntity( nullptr );
		me->Teleport( &landing, nullptr, &vec3_origin );
		me->SetBaseVelocity( vec3_origin );
		me->GetLocomotionInterface()->Reset();
		state.landings[state.nextPlacement - 1] = landing;
		++state.placements;
		if ( me->IsDebugging( NEXTBOT_PATH ) )
		{
			DevMsg( "%s: Ladder recovery placed at verified landing\n", me->GetDebugIdentifier() );
		}
		state.phase = CNEOBot::LadderRecoveryState::SETTLING;
		state.phaseDeadline = gpGlobals->curtime + RECOVERY_SETTLE_TIMEOUT;
		state.groundedSince = -1.0f;
		return true;
	}
	StopRecovery( me, "failed", probe.gate );
	return false;
}

//---------------------------------------------------------------------------------------------
bool CNEOBotLadderClimb::CheckRecoveryWalk( CNEOBot *me, const Vector &target )
{
	CNEOBot::LadderRecoveryState &state = me->m_ladderRecovery;
	if ( target.DistToSqr( state.episodeStart ) > Square( RECOVERY_DISTANCE_CAP ) )
	{
		return false;
	}
	const Vector from = me->GetAbsOrigin();
	Vector maxs = me->WorldAlignMaxs();
	maxs.z = me->WorldAlignMins().z + me->GetBodyInterface()->GetStandHullHeight();
	trace_t trace;
	const Vector clearance( 0.0f, 0.0f, RECOVERY_LANDING_CLEARANCE );
	UTIL_TraceHull( from + clearance, target + clearance, me->WorldAlignMins(), maxs,
		MASK_PLAYERSOLID, me, COLLISION_GROUP_NONE, &trace );
	if ( trace.startsolid || trace.allsolid || trace.DidHit() )
	{
		return false;
	}
	const int steps = Max( 1, static_cast<int>( ceilf( from.DistTo( target ) / RECOVERY_SUPPORT_GRID ) ) );
	for ( int i = 0; i <= steps; ++i )
	{
		const Vector point = from + ( target - from ) * ( static_cast<float>( i ) / steps );
		Vector landing;
		LandingProbe probe;
		if ( !ValidateLanding( me, point, &landing, &probe, RECOVERY_HANDOFF )
			|| fabsf( landing.z - from.z ) > me->GetLocomotionInterface()->GetStepHeight() )
		{
			return false;
		}
	}
	return true;
}

//---------------------------------------------------------------------------------------------
bool CNEOBotLadderClimb::FindRecoveryHandoff( CNEOBot *me )
{
	CNEOBot::LadderRecoveryState &state = me->m_ladderRecovery;
	const Vector from = me->GetAbsOrigin();
	Vector toward = state.landingNextArea ? state.landingNextArea->GetCenter() : state.landingArea->GetCenter();
	Vector direction = toward - from;
	direction.z = 0.0f;
	if ( direction.NormalizeInPlace() < RECOVERY_DUPLICATE_TOLERANCE )
	{
		direction = from - state.ladder->m_top;
		direction.z = 0.0f;
		if ( direction.NormalizeInPlace() < RECOVERY_DUPLICATE_TOLERANCE )
		{
			return false;
		}
	}
	for ( float distance = RECOVERY_HANDOFF_DISTANCE; distance >= RECOVERY_HANDOFF_MINIMUM; distance -= RECOVERY_CANDIDATE_GRID )
	{
		Vector target = from + direction * distance;
		Vector landing;
		LandingProbe probe;
		if ( !ValidateLanding( me, target, &landing, &probe, RECOVERY_HANDOFF ) || !CheckRecoveryWalk( me, landing ) )
		{
			continue;
		}
		state.handoffTarget = landing;
		return true;
	}
	return false;
}

//---------------------------------------------------------------------------------------------
bool CNEOBotLadderClimb::RecomputeRecoveryPath( CNEOBot *me )
{
	const CNEOBot::LadderRecoveryState &state = me->m_ladderRecovery;
	PathFollower *path = const_cast<PathFollower *>( me->GetCurrentPath() );
	if ( !path || !state.hasObjective )
	{
		return false;
	}
	me->ClearLastKnownArea();
	me->UpdateLastKnownArea();
	if ( !me->GetLastKnownArea() )
	{
		path->Invalidate();
		return false;
	}
	CNEOBotPathCost cost( me, DEFAULT_ROUTE );
	const Vector objective = state.objective;
	const bool computed = path->Compute( me, objective, cost );
	return computed && path->IsValid();
}

//---------------------------------------------------------------------------------------------
void CNEOBotLadderClimb::TryRecoveryReturn( CNEOBot *me )
{
	CNEOBot::LadderRecoveryState &state = me->m_ladderRecovery;
	if ( me->GetGroundEntity() || state.returnedToSupport || !state.hasLastSupport || !state.lastSupportArea
		|| gpGlobals->curtime > state.deadline || state.lastSupport.DistToSqr( state.episodeStart ) > Square( RECOVERY_DISTANCE_CAP ) )
	{
		return;
	}
	state.returnedToSupport = true;
	const CNavArea *exitArea = state.landingArea;
	const CNavArea *unionArea = state.landingUnionArea;
	state.landingArea = state.lastSupportArea;
	state.landingUnionArea = nullptr;
	LandingProbe probe;
	Vector landing;
	const bool valid = ValidateLanding( me, state.lastSupport, &landing, &probe, RECOVERY_PLACEMENT );
	state.landingArea = exitArea;
	state.landingUnionArea = unionArea;
	if ( !valid || landing.DistToSqr( state.episodeStart ) > Square( RECOVERY_DISTANCE_CAP ) )
	{
		return;
	}
	ClearRecoveryInputs( me );
	me->SetMoveType( MOVETYPE_WALK );
	me->SetGroundEntity( nullptr );
	me->Teleport( &landing, nullptr, &vec3_origin );
	me->SetBaseVelocity( vec3_origin );
	me->GetLocomotionInterface()->Reset();
	me->m_ladderNeutralThroughTick = gpGlobals->tickcount + RECOVERY_NEUTRAL_TICKS;
}

//---------------------------------------------------------------------------------------------
static const char *ExpiryTimerName( CNEOBotLadderClimb::ExpiryTimer timer )
{
	switch ( timer )
	{
	case CNEOBotLadderClimb::EXPIRY_CLIMB:
		return "climb";
	case CNEOBotLadderClimb::EXPIRY_DISMOUNT:
		return "dismount";
	case CNEOBotLadderClimb::EXPIRY_STALL:
		return "stall";
	case CNEOBotLadderClimb::EXPIRY_APPROACH:
		return "approach";
	}
	return "unknown";
}

//---------------------------------------------------------------------------------------------
static void LogExpiryTeleport( CNEOBot *me, const CNavLadder *ladder, bool goingUp, CNEOBotLadderClimb::ExpiryTimer timer,
	const Vector &from, const Vector &to, const CNavArea *area, const char *gate )
{
	const bool placed = FStrEq( gate, "none" );
	Msg( "NEO_LADTELE %s t=%.2f bot=%d lad=%u up=%d timer=%s from=%.0f,%.0f,%.0f to=%.0f,%.0f,%.0f area=%u gate=%s\n",
		placed ? "placed" : "refused", gpGlobals->curtime, me->entindex(), ladder->GetID(), goingUp ? 1 : 0,
		ExpiryTimerName( timer ), from.x, from.y, from.z, to.x, to.y, to.z, area ? area->GetID() : 0, gate );
}

//---------------------------------------------------------------------------------------------
// A bot still waiting at the foot must not jump the queue past a bot already on the ladder
static bool IsLadderOccupied( CNEOBot *me, const CNavLadder *ladder, float range )
{
	for ( int i = 1; i <= gpGlobals->maxClients; ++i )
	{
		CBasePlayer *player = UTIL_PlayerByIndex( i );
		if ( !player || player == me || !player->IsAlive() || player->GetMoveType() != MOVETYPE_LADDER )
		{
			continue;
		}

		Vector closest;
		CalcClosestPointOnLineSegment( player->GetAbsOrigin(), ladder->m_bottom, ladder->m_top, closest );
		if ( player->GetAbsOrigin().DistToSqr( closest ) < Square( range ) )
		{
			return true;
		}
	}
	return false;
}

//---------------------------------------------------------------------------------------------
// An escape hatch for a ladder action that ran out of time: place the bot on the ladder's exit area,
// so a climb the bot cannot finish costs one timeout rather than the round.
// ValidateLanding reads the recovery state, which is empty while ladder recovery is off, so it is set for the call.
bool CNEOBotLadderClimb::TryExpiryTeleport( CNEOBot *me, const CNavLadder *ladder, bool goingUp,
	const CNavArea *pathExit, const Vector *dismountPos, ExpiryTimer timer )
{
	const int level = ( timer == EXPIRY_CLIMB || timer == EXPIRY_DISMOUNT ) ? EXPIRY_TELEPORT_TIMERS : EXPIRY_TELEPORT_ALL;
	if ( !ladder || !me->IsAlive() || neo_bot_ladder_expiry_teleport.GetInt() < level )
	{
		return false;
	}

	const Vector feet = me->GetLocomotionInterface()->GetFeet();
	const Vector &entry = goingUp ? ladder->m_bottom : ladder->m_top;
	const Vector &end = goingUp ? ladder->m_top : ladder->m_bottom;
	Vector onLadder;
	CalcClosestPointOnLineSegment( feet, ladder->m_bottom, ladder->m_top, onLadder );
	const char *gate = nullptr;
	if ( feet.DistToSqr( onLadder ) > Square( MAX_DEVIATION_DIST ) )
	{
		gate = "far";
	}
	else if ( timer == EXPIRY_APPROACH && ( feet - entry ).AsVector2D().LengthSqr() > Square( EXPIRY_TELEPORT_APPROACH_RANGE ) )
	{
		gate = "far";
	}
	else if ( me->GetVisionInterface()->GetPrimaryKnownThreat( true ) )
	{
		gate = "threat";
	}
	else if ( me->IsCarryingGhost() )
	{
		gate = "ghost";
	}
	else if ( !me->m_ladderTeleportCooldown.IsElapsed() )
	{
		gate = "cooldown";
	}
	else if ( me->GetNeoFlags() & NEO_FL_FREEZETIME )
	{
		gate = "frozen";
	}
	else if ( timer == EXPIRY_APPROACH && IsLadderOccupied( me, ladder, MAX_DEVIATION_DIST ) )
	{
		gate = "queue";
	}
	if ( gate )
	{
		LogExpiryTeleport( me, ladder, goingUp, timer, feet, vec3_origin, nullptr, gate );
		return false;
	}

	const CNavLadder::LadderDirectionType dir = goingUp ? CNavLadder::LADDER_UP : CNavLadder::LADDER_DOWN;
	const CNavArea *ladderEnd = goingUp ? ladder->GetTopArea() : ladder->m_bottomArea;
	const CNavArea *areas[] = { pathExit, ladderEnd != pathExit ? ladderEnd : nullptr };
	const float step = me->GetLocomotionInterface()->GetStepHeight();
	const Vector mins = me->WorldAlignMins();
	const Vector maxs = me->WorldAlignMaxs();

	CNEOBot::LadderRecoveryState &state = me->m_ladderRecovery;
	const CNavLadder *savedLadder = state.ladder;
	const CNavArea *savedArea = state.landingArea;
	const CNavArea *savedUnionArea = state.landingUnionArea;

	LandingProbe probe;
	probe.gate = "no_exit";
	const CNavArea *placedArea = nullptr;
	Vector landing = vec3_origin;
	for ( int areaIndex = 0; areaIndex < ARRAYSIZE( areas ) && !placedArea; ++areaIndex )
	{
		const CNavArea *area = areas[areaIndex];
		if ( !area || !ladder->IsConnected( area, dir ) )
		{
			continue;
		}

		CUtlVector<Vector> candidates;
		if ( dismountPos && area == pathExit )
		{
			candidates.AddToTail( *dismountPos );
		}

		// Clamped so the whole hull lands inside the area
		Vector closest;
		area->GetClosestPointOnArea( end, &closest );
		const Vector northWest = area->GetCorner( NORTH_WEST );
		const Vector southEast = area->GetCorner( SOUTH_EAST );
		const float minX = northWest.x - mins.x + RECOVERY_LANDING_INSET;
		const float minY = northWest.y - mins.y + RECOVERY_LANDING_INSET;
		const float maxX = southEast.x - maxs.x - RECOVERY_LANDING_INSET;
		const float maxY = southEast.y - maxs.y - RECOVERY_LANDING_INSET;
		if ( minX <= maxX && minY <= maxY )
		{
			Vector clamped( Clamp( closest.x, minX, maxX ), Clamp( closest.y, minY, maxY ), 0.0f );
			clamped.z = area->GetZ( clamped );
			candidates.AddToTail( clamped );
		}
		candidates.AddToTail( area->GetCenter() );

		state.ladder = ladder;
		state.landingArea = area;
		state.landingUnionArea = nullptr;
		FOR_EACH_VEC( candidates, i )
		{
			Vector spot;
			if ( !ValidateLanding( me, candidates[i], &spot, &probe, RECOVERY_PLACEMENT ) )
			{
				continue;
			}

			const bool rightWay = goingUp ? spot.z > feet.z - step : spot.z < feet.z + step;
			if ( !rightWay )
			{
				probe.gate = "direction";
				continue;
			}

			if ( spot.DistTo( feet ) > ladder->m_length + RECOVERY_SEARCH_RANGE )
			{
				probe.gate = "distance_cap";
				continue;
			}

			landing = spot;
			placedArea = area;
			break;
		}
		state.ladder = savedLadder;
		state.landingArea = savedArea;
		state.landingUnionArea = savedUnionArea;
	}

	if ( !placedArea )
	{
		LogExpiryTeleport( me, ladder, goingUp, timer, feet, vec3_origin, nullptr, probe.gate );
		return false;
	}

	PathFollower *path = const_cast<PathFollower *>( me->GetCurrentPath() );
	const bool hasGoal = path && path->IsValid();
	const Vector goal = hasGoal ? path->GetEndPosition() : vec3_origin;

	me->ClearLadderMovementInputs();
	me->ReleaseCrouchButton();
	me->SetMoveType( MOVETYPE_WALK );
	me->SetGroundEntity( nullptr );

	// Keep the yaw; a climb leaves the pitch pointing up or down the ladder
	const QAngle upright( 0.0f, me->EyeAngles().y, 0.0f );
	me->Teleport( &landing, &upright, &vec3_origin );
	me->SetBaseVelocity( vec3_origin );
	me->GetLocomotionInterface()->Reset();

	// A buffered forward press would grab the ladder again at its foot
	me->m_ladderNeutralThroughTick = gpGlobals->tickcount + RECOVERY_NEUTRAL_TICKS;

	// The old path still leads to this ladder, and from the exit area of a short ladder
	// the bot is close enough to its entry to start another approach
	if ( path )
	{
		me->ClearLastKnownArea();
		me->UpdateLastKnownArea();
		CNEOBotPathCost cost( me, DEFAULT_ROUTE );
		if ( !hasGoal || !me->GetLastKnownArea() || !path->Compute( me, goal, cost ) )
		{
			path->Invalidate();
		}
	}

	me->m_ladderTeleportCooldown.Start( EXPIRY_TELEPORT_COOLDOWN );
	LogExpiryTeleport( me, ladder, goingUp, timer, feet, landing, placedArea, "none" );
	return true;
}

//---------------------------------------------------------------------------------------------
bool CNEOBotLadderClimb::TeleportOnExpiry( CNEOBot *me, ExpiryTimer timer )
{
	if ( neo_bot_ladder_expiry_teleport.GetInt() == EXPIRY_TELEPORT_OFF )
	{
		return false;
	}

	// The path's exit can have changed since the climb started
	ResolveExitArea( me );
	return TryExpiryTeleport( me, m_ladder, m_bGoingUp, m_pExitArea, &m_dismountPos, timer );
}

//---------------------------------------------------------------------------------------------
void CNEOBotLadderClimb::StopRecovery( CNEOBot *me, const char *result, const char *gate )
{
	CNEOBot::LadderRecoveryState &state = me->m_ladderRecovery;
	if ( !state.ownsInputs )
	{
		return;
	}
	const bool failed = FStrEq( result, "failed" );
	if ( failed )
	{
		TryRecoveryReturn( me );
		BlockRecoveryLadder( me, RECOVERY_FAILURE_EXCLUSION );
		RecomputeRecoveryPath( me );
	}
	ClearRecoveryInputs( me );
	if ( FStrEq( result, "cancelled" ) )
	{
		// Releasing control must preserve an attachment or an airborne kick.
		me->SetMoveType( state.savedMoveType );
	}
	else
	{
		me->SetMoveType( MOVETYPE_WALK );
		me->SetAbsVelocity( vec3_origin );
		me->SetBaseVelocity( vec3_origin );
	}
	me->GetLocomotionInterface()->Reset();
	state.ownsInputs = false;
	state.commandedUp = false;
	state.phase = FStrEq( result, "complete" ) ? CNEOBot::LadderRecoveryState::COMPLETE : CNEOBot::LadderRecoveryState::FAILED;
	state.traversalClosed = state.phase == CNEOBot::LadderRecoveryState::COMPLETE;
	if ( state.traversalClosed )
	{
		BlockRecoveryLadder( me, RECOVERY_COOLDOWN );
	}
	me->m_ladderNeutralThroughTick = gpGlobals->tickcount + RECOVERY_NEUTRAL_TICKS;
	if ( me->IsDebugging( NEXTBOT_PATH ) )
	{
		DevMsg( "%s: Ladder recovery %s (gate %s)\n", me->GetDebugIdentifier(), result, gate );
	}
}

static constexpr float RECOVERY_LAUNCH_HEIGHT = 4.0f;
static constexpr float RECOVERY_LAUNCH_GRID = 8.0f;
static constexpr float RECOVERY_MAX_FLIGHT_TIME = 1.25f;
static constexpr float RECOVERY_MAX_FLIGHT_SPEED = 300.0f;
static constexpr float RECOVERY_ARC_SEGMENT = 4.0f;
static constexpr float RECOVERY_FLIGHT_GRACE = 0.2f;
static constexpr float RECOVERY_FLIGHT_ERROR = 8.0f;
static constexpr float RECOVERY_GRAVITY_HALF = 0.5f;
static constexpr float RECOVERY_CONTACT_GAP_FRACTION = 0.5f;
static constexpr float RECOVERY_SLIDE_STEP = 4.0f;
static constexpr float RECOVERY_SLIDE_SPEED = 240.0f;
static constexpr float RECOVERY_SLIDE_TIME = 1.25f;
static constexpr float RECOVERY_FLIGHT_SPEEDS[] = { 0.0f, 40.0f, 80.0f, 150.0f, 200.0f, 240.0f };

//---------------------------------------------------------------------------------------------
bool CNEOBotLadderClimb::TraceLaunchFace( CNEOBot *me, const Vector &point, const Vector &normal, trace_t *face )
{
	UTIL_TraceHull( point, point - normal * LADDER_GRAB_DIST, me->WorldAlignMins(), me->WorldAlignMaxs(),
		MASK_PLAYERSOLID, me, COLLISION_GROUP_NONE, face );
	if ( face->startsolid || face->allsolid || !face->DidHit() || !face->m_pEnt || face->m_pEnt->IsPlayer()
		|| DotProduct( face->plane.normal, normal ) < RECOVERY_NORMAL_ALIGNMENT )
	{
		return false;
	}
	if ( face->contents & CONTENTS_LADDER )
	{
		return true;
	}
	const surfacedata_t *surface = physprops->GetSurfaceData( face->surface.surfaceProps );
	return surface && surface->game.climbable;
}

//---------------------------------------------------------------------------------------------
bool CNEOBotLadderClimb::FindLaunchContact( CNEOBot *me, float height, const Vector &normal, Vector *point, trace_t *face )
{
	const CNEOBot::LadderRecoveryState &state = me->m_ladderRecovery;
	const Vector mins = me->WorldAlignMins();
	const Vector maxs = me->WorldAlignMaxs();
	const float reach = -( normal.x >= 0.0f ? mins.x : maxs.x ) * normal.x
		- ( normal.y >= 0.0f ? mins.y : maxs.y ) * normal.y
		- ( normal.z >= 0.0f ? mins.z : maxs.z ) * normal.z;
	const Vector clear = state.ladder->GetPosAtHeight( height ) + normal * ( reach + LADDER_PLACE_GAP );
	UTIL_TraceHull( clear, clear - normal * LADDER_PLACE_SEEK, mins, maxs,
		MASK_PLAYERSOLID, me, COLLISION_GROUP_NONE, face );
	if ( face->startsolid || face->allsolid || !face->DidHit() || !face->m_pEnt || face->m_pEnt->IsPlayer()
		|| DotProduct( face->plane.normal, normal ) < RECOVERY_NORMAL_ALIGNMENT )
	{
		return false;
	}
	*point = face->endpos + face->plane.normal * ( LADDER_GRAB_DIST * RECOVERY_CONTACT_GAP_FRACTION );
	if ( point->DistToSqr( state.episodeStart ) > Square( RECOVERY_DISTANCE_CAP ) )
	{
		return false;
	}
	trace_t occupancy;
	UTIL_TraceHull( *point, *point, mins, maxs, MASK_PLAYERSOLID, me, COLLISION_GROUP_NONE, &occupancy );
	if ( occupancy.startsolid || occupancy.allsolid || occupancy.DidHit() )
	{
		return false;
	}
	const Vector tracedNormal = face->plane.normal;
	return TraceLaunchFace( me, *point, tracedNormal, face );
}

//---------------------------------------------------------------------------------------------
bool CNEOBotLadderClimb::CheckRecoveryArc( CNEOBot *me, const Vector &start, const Vector &velocity, float flightTime )
{
	const CNEOBot::LadderRecoveryState &state = me->m_ladderRecovery;
	const float tick = gpGlobals->interval_per_tick;
	if ( tick <= 0.0f )
	{
		return false;
	}
	const float apex = velocity.z / state.flightGravity;
	float time = 0.0f;
	Vector prior = start;
	while ( time < flightTime )
	{
		float next = Min( time + tick, flightTime );
		if ( apex > time && apex < next )
		{
			next = apex;
		}
		const Vector end = start + velocity * next - Vector( 0.0f, 0.0f,
			RECOVERY_GRAVITY_HALF * state.flightGravity * next * next );
		const int pieces = Max( 1, static_cast<int>( ceilf( prior.DistTo( end ) / RECOVERY_ARC_SEGMENT ) ) );
		for ( int i = 1; i <= pieces; ++i )
		{
			const float at = time + ( next - time ) * static_cast<float>( i ) / pieces;
			const Vector point = start + velocity * at - Vector( 0.0f, 0.0f,
				RECOVERY_GRAVITY_HALF * state.flightGravity * at * at );
			if ( point.DistToSqr( state.episodeStart ) > Square( RECOVERY_DISTANCE_CAP ) )
			{
				return false;
			}
			trace_t trace;
			UTIL_TraceHull( prior, point, me->WorldAlignMins(), me->WorldAlignMaxs(),
				MASK_PLAYERSOLID, me, COLLISION_GROUP_NONE, &trace );
			if ( trace.startsolid || trace.allsolid )
			{
				return false;
			}
			if ( trace.DidHit() )
			{
				// The only permitted contact is the verified static landing at the descending endpoint.
				const bool intendedFloor = next == flightTime && i == pieces && trace.m_pEnt == state.landingEntity.Get()
					&& trace.plane.normal.z >= me->GetLocomotionInterface()->GetTraversableSlopeLimit()
					&& trace.endpos.DistToSqr( state.flightLanding ) <= Square( RECOVERY_LANDING_TOLERANCE );
				return intendedFloor;
			}
			prior = point;
		}
		time = next;
	}
	return true;
}

//---------------------------------------------------------------------------------------------
bool CNEOBotLadderClimb::SolveRecoveryFlight( CNEOBot *me, const Vector &start, Vector *velocity, float *flightTime )
{
	const CNEOBot::LadderRecoveryState &state = me->m_ladderRecovery;
	const float gravity = state.flightGravity;
	if ( gravity <= 0.0f || gpGlobals->interval_per_tick <= 0.0f )
	{
		return false;
	}
	for ( int i = 0; i < ARRAYSIZE( RECOVERY_FLIGHT_SPEEDS ); ++i )
	{
		const float vertical = RECOVERY_FLIGHT_SPEEDS[i];
		const float discriminant = vertical * vertical + 2.0f * gravity * ( start.z - state.flightLanding.z );
		if ( discriminant < 0.0f )
		{
			continue;
		}
		const float time = ( vertical + sqrtf( discriminant ) ) / gravity;
		if ( time < gpGlobals->interval_per_tick || time > RECOVERY_MAX_FLIGHT_TIME )
		{
			continue;
		}
		Vector candidate = ( state.flightLanding - start ) / time;
		candidate.z = vertical;
		if ( candidate.AsVector2D().LengthSqr() > Square( RECOVERY_MAX_FLIGHT_SPEED )
			|| !CheckRecoveryArc( me, start, candidate, time ) )
		{
			continue;
		}
		*velocity = candidate;
		*flightTime = time;
		return true;
	}
	return false;
}

//---------------------------------------------------------------------------------------------
bool CNEOBotLadderClimb::FindRecoveryLaunch( CNEOBot *me, RecoveryWatchPhase phase )
{
	CNEOBot::LadderRecoveryState &state = me->m_ladderRecovery;
	LandingProbe probe;
	if ( !ValidateLanding( me, state.landings[0], &state.flightLanding, &probe, RECOVERY_PLACEMENT ) )
	{
		return false;
	}
	// Resolve world support as well as ordinary entities.
	state.landingEntity = probe.hit >= 0 ? CBaseEntity::Instance( probe.hit ) : nullptr;
	const float playerGravity = me->GetGravity();
	state.flightGravity = GetCurrentGravity() * ( playerGravity != 0.0f ? playerGravity : 1.0f );
	if ( state.flightGravity <= 0.0f )
	{
		return false;
	}
	Vector normal = state.ladder->GetNormal();
	if ( me->GetMoveType() == MOVETYPE_LADDER )
	{
		const Vector line = state.ladder->GetPosAtHeight( me->GetAbsOrigin().z );
		if ( DotProduct( me->GetAbsOrigin() - line, normal ) < 0.0f )
		{
			normal = -normal;
		}
		trace_t source;
		if ( !TraceLaunchFace( me, me->GetAbsOrigin(), normal, &source ) )
		{
			return false;
		}
		normal = source.plane.normal;
	}
	const float lower = phase == RECOVERY_APPROACH ? state.ladder->m_bottom.z : me->GetAbsOrigin().z;
	const float top = state.ladder->m_top.z;
	const float preferred = Max( lower, state.flightLanding.z + RECOVERY_LAUNCH_HEIGHT );
	for ( float height = preferred; height <= top; height = Min( height + RECOVERY_LAUNCH_GRID, top ) )
	{
		if ( TryRecoveryLaunchAt( me, height, normal ) )
		{
			return true;
		}
		if ( height == top )
		{
			break;
		}
	}

	// Search below the lip when the ladder ends short of the landing.
	const float fastest = RECOVERY_FLIGHT_SPEEDS[ARRAYSIZE( RECOVERY_FLIGHT_SPEEDS ) - 1];
	const float reachable = Max( lower, state.flightLanding.z - Square( fastest ) / ( 2.0f * state.flightGravity ) );
	for ( float height = Min( top, preferred - RECOVERY_LAUNCH_GRID ); height >= reachable; height -= RECOVERY_LAUNCH_GRID )
	{
		if ( TryRecoveryLaunchAt( me, height, normal ) )
		{
			return true;
		}
	}
	return false;
}

//---------------------------------------------------------------------------------------------
bool CNEOBotLadderClimb::TryRecoveryLaunchAt( CNEOBot *me, float height, const Vector &normal )
{
	CNEOBot::LadderRecoveryState &state = me->m_ladderRecovery;
	Vector point;
	trace_t face;
	Vector velocity;
	float time;
	if ( !FindLaunchContact( me, height, normal, &point, &face ) || !SolveRecoveryFlight( me, point, &velocity, &time ) )
	{
		return false;
	}
	state.launchPoint = point;
	state.launchNormal = face.plane.normal;
	state.launchEntity = face.m_pEnt;
	state.launchVelocity = velocity;
	state.flightTime = time;
	return true;
}

//---------------------------------------------------------------------------------------------
void CNEOBotLadderClimb::FallbackRecoveryC( CNEOBot *me, const char *gate )
{
	CNEOBot::LadderRecoveryState &state = me->m_ladderRecovery;
	if ( state.fellBack || gpGlobals->curtime >= state.deadline )
	{
		StopRecovery( me, "failed", state.fellBack ? gate : "deadline" );
		return;
	}
	if ( me->IsDebugging( NEXTBOT_PATH ) )
	{
		DevMsg( "%s: Ladder recovery falling back to direct landing (gate %s)\n", me->GetDebugIdentifier(), gate );
	}
	state.fellBack = true;
	state.nextPlacement = 0;
	state.phase = CNEOBot::LadderRecoveryState::AIMING;
	state.phaseDeadline = gpGlobals->curtime;
	if ( me->GetGroundEntity() || me->GetMoveType() == MOVETYPE_LADDER )
	{
		state.phaseDeadline += RECOVERY_AIM_TIME;
	}
	ClearRecoveryInputs( me );
}

//---------------------------------------------------------------------------------------------
bool CNEOBotLadderClimb::StartRecoveryA( CNEOBot *me, RecoveryWatchPhase phase, bool slide )
{
	if ( !StartRecoveryC( me ) )
	{
		return false;
	}
	CNEOBot::LadderRecoveryState &state = me->m_ladderRecovery;
	if ( !FindRecoveryLaunch( me, phase ) )
	{
		FallbackRecoveryC( me, "arc_blocked" );
		return true;
	}
	if ( !slide )
	{
		state.phase = CNEOBot::LadderRecoveryState::POSITIONING;
		return true;
	}

	// A detached bot has no contact line for a continuous slide.
	if ( me->GetMoveType() != MOVETYPE_LADDER )
	{
		FallbackRecoveryC( me, "no_attached_start" );
		return true;
	}
	state.phase = CNEOBot::LadderRecoveryState::SLIDING;
	state.slideStarted = gpGlobals->curtime;
	return true;
}

//---------------------------------------------------------------------------------------------
void CNEOBotLadderClimb::UpdateRecoverySlide( CNEOBot *me )
{
	CNEOBot::LadderRecoveryState &state = me->m_ladderRecovery;
	if ( me->GetMoveType() != MOVETYPE_LADDER )
	{
		FallbackRecoveryC( me, "attach_failed" );
		return;
	}
	if ( gpGlobals->curtime - state.slideStarted > RECOVERY_SLIDE_TIME )
	{
		FallbackRecoveryC( me, "slide_timeout" );
		return;
	}

	const Vector from = me->GetAbsOrigin();
	Vector toLaunch = state.launchPoint - from;
	const float remaining = toLaunch.NormalizeInPlace();
	if ( remaining <= RECOVERY_DUPLICATE_TOLERANCE )
	{
		state.attachTick = gpGlobals->tickcount;
		state.phase = CNEOBot::LadderRecoveryState::VERIFYING_ATTACH;
		return;
	}

	// Bound each slide step even after missed ticks.
	const float step = Min( Min( RECOVERY_SLIDE_STEP, RECOVERY_SLIDE_SPEED * gpGlobals->interval_per_tick ), remaining );
	const Vector to = from + toLaunch * step;
	trace_t sweep;
	UTIL_TraceHull( from, to, me->WorldAlignMins(), me->WorldAlignMaxs(), MASK_PLAYERSOLID, me, COLLISION_GROUP_NONE, &sweep );
	trace_t face;
	if ( sweep.startsolid || sweep.allsolid || sweep.DidHit() || !TraceLaunchFace( me, to, state.launchNormal, &face ) )
	{
		FallbackRecoveryC( me, "slide_blocked" );
		return;
	}
	me->SetAbsOrigin( to );
	me->SetAbsVelocity( vec3_origin );
	me->SetBaseVelocity( vec3_origin );
}

//---------------------------------------------------------------------------------------------
bool CNEOBotLadderClimb::PlaceRecoveryLaunch( CNEOBot *me )
{
	CNEOBot::LadderRecoveryState &state = me->m_ladderRecovery;
	trace_t occupancy;
	UTIL_TraceHull( state.launchPoint, state.launchPoint, me->WorldAlignMins(), me->WorldAlignMaxs(),
		MASK_PLAYERSOLID, me, COLLISION_GROUP_NONE, &occupancy );
	trace_t face;
	if ( occupancy.startsolid || occupancy.allsolid || occupancy.DidHit()
		|| !TraceLaunchFace( me, state.launchPoint, state.launchNormal, &face ) || face.m_pEnt != state.launchEntity.Get() )
	{
		FallbackRecoveryC( me, "attach_failed" );
		return false;
	}
	ClearRecoveryInputs( me );
	me->SetGroundEntity( nullptr );
	me->Teleport( &state.launchPoint, nullptr, &vec3_origin );
	me->SetLadderNormal( state.launchNormal );
	me->SetMoveType( MOVETYPE_LADDER );
	state.savedMoveType = MOVETYPE_LADDER;
	me->SetBaseVelocity( vec3_origin );
	me->GetLocomotionInterface()->Reset();
	state.attachTick = gpGlobals->tickcount;
	state.phase = CNEOBot::LadderRecoveryState::VERIFYING_ATTACH;
	if ( me->IsDebugging( NEXTBOT_PATH ) )
	{
		DevMsg( "%s: Ladder recovery reached launch point\n", me->GetDebugIdentifier() );
	}
	return true;
}

//---------------------------------------------------------------------------------------------
void CNEOBotLadderClimb::LaunchRecovery( CNEOBot *me )
{
	CNEOBot::LadderRecoveryState &state = me->m_ladderRecovery;
	trace_t face;
	if ( me->GetMoveType() != MOVETYPE_LADDER || !TraceLaunchFace( me, me->GetAbsOrigin(), state.launchNormal, &face )
		|| face.m_pEnt != state.launchEntity.Get() )
	{
		FallbackRecoveryC( me, "attach_failed" );
		return;
	}
	Vector velocity;
	float time;
	if ( !SolveRecoveryFlight( me, me->GetAbsOrigin(), &velocity, &time ) )
	{
		FallbackRecoveryC( me, "arc_blocked" );
		return;
	}
	state.launchPoint = me->GetAbsOrigin();
	state.launchVelocity = velocity;
	state.flightTime = time;
	ClearRecoveryInputs( me );
	me->SetMoveType( MOVETYPE_WALK );
	state.savedMoveType = MOVETYPE_WALK;
	me->SetGroundEntity( nullptr );
	me->SetBaseVelocity( vec3_origin );
	me->GetLocomotionInterface()->Reset();
	me->SetAbsVelocity( velocity );
	state.flightStarted = gpGlobals->curtime;
	state.phase = CNEOBot::LadderRecoveryState::FLYING;
	if ( me->IsDebugging( NEXTBOT_PATH ) )
	{
		DevMsg( "%s: Ladder recovery launched toward verified landing\n", me->GetDebugIdentifier() );
	}
}

//---------------------------------------------------------------------------------------------
void CNEOBotLadderClimb::UpdateRecoveryFlight( CNEOBot *me )
{
	CNEOBot::LadderRecoveryState &state = me->m_ladderRecovery;
	CBaseEntity *ground = me->GetGroundEntity();
	if ( ground && ( me->GetFlags() & FL_ONGROUND ) )
	{
		// Accept static exit floor even when the kick lands short of the solved point.
		const Vector &feet = me->GetAbsOrigin();
		const bool nearSolved = feet.DistToSqr( state.flightLanding ) <= Square( RECOVERY_FLIGHT_ERROR );
		const CNavArea *touchdown = TheNavMesh->GetNavArea( feet, me->GetLocomotionInterface()->GetStepHeight() );
		const bool exitSide = touchdown && ( touchdown == state.landingArea || touchdown == state.landingNextArea
			|| state.landingArea->IsConnected( touchdown, NUM_DIRECTIONS ) || touchdown->IsConnected( state.landingArea, NUM_DIRECTIONS ) );
		const float step = me->GetLocomotionInterface()->GetStepHeight();
		// The exit lip can support touchdown without its own nav area.
		const bool onLip = fabsf( feet.z - state.flightLanding.z ) <= step
			&& ( feet - state.ladder->m_top ).AsVector2D().LengthSqr() <= Square( RECOVERY_SEARCH_RANGE );
		const bool onExit = onLip || ( exitSide && fabsf( feet.z - touchdown->GetZ( feet ) ) <= step );
		if ( ground->IsPlayer() || ground->GetMoveType() != MOVETYPE_NONE || ( !nearSolved && !onExit ) )
		{
			FallbackRecoveryC( me, "unexpected_collision" );
			return;
		}

		// The kick's horizontal speed would carry the bot past its landing; stop it there.
		me->SetAbsVelocity( vec3_origin );

		// Walk off the lip to avoid leaving the bot at the drop.
		if ( !nearSolved )
		{
			if ( !CheckRecoveryWalk( me, state.flightLanding ) )
			{
				FallbackRecoveryC( me, "lip_unwalkable" );
				return;
			}
			state.handoffTarget = state.flightLanding;
			state.phase = CNEOBot::LadderRecoveryState::WALKING_OUT;
			return;
		}
		state.nextPlacement = 1;
		state.landings[0] = feet;
		state.phase = CNEOBot::LadderRecoveryState::SETTLING;
		state.groundedSince = gpGlobals->curtime;
		state.phaseDeadline = gpGlobals->curtime + RECOVERY_SETTLE_TIMEOUT;
		me->ReleaseCrouchButton();
		state.keepCrouch = false;
		return;
	}
	const float elapsed = gpGlobals->curtime - state.flightStarted;
	const Vector expected = state.launchPoint + state.launchVelocity * elapsed
		- Vector( 0.0f, 0.0f, RECOVERY_GRAVITY_HALF * state.flightGravity * elapsed * elapsed );
	if ( me->GetMoveType() == MOVETYPE_LADDER || me->GetAbsOrigin().DistToSqr( expected ) > Square( RECOVERY_FLIGHT_ERROR ) )
	{
		FallbackRecoveryC( me, "unexpected_collision" );
		return;
	}
	if ( elapsed >= state.flightTime + RECOVERY_FLIGHT_GRACE )
	{
		FallbackRecoveryC( me, "no_landing" );
	}
}

//---------------------------------------------------------------------------------------------
void CNEOBotLadderClimb::UpdateRecovery( CNEOBot *me )
{
	CNEOBot::LadderRecoveryState &state = me->m_ladderRecovery;
	if ( !state.ownsInputs )
	{
		return;
	}
	if ( !IsRecoveryOwner( me, state.owner ) || !HasRecoveryIntent( me ) || NEORules()->roundNumber() != state.round )
	{
		StopRecovery( me, "cancelled", "stale_intent" );
		return;
	}
	if ( neo_bot_ladder_recovery.GetInt() != state.level )
	{
		StopRecovery( me, "cancelled", "cvar_changed" );
		return;
	}
	if ( gpGlobals->curtime >= state.deadline || me->GetAbsOrigin().DistToSqr( state.episodeStart ) > Square( RECOVERY_DISTANCE_CAP ) )
	{
		StopRecovery( me, "failed", gpGlobals->curtime >= state.deadline ? "deadline" : "distance_cap" );
		return;
	}

	ClearRecoveryInputs( me );
	if ( state.phase == CNEOBot::LadderRecoveryState::POSITIONING )
	{
		PlaceRecoveryLaunch( me );
		return;
	}
	if ( state.phase == CNEOBot::LadderRecoveryState::SLIDING )
	{
		UpdateRecoverySlide( me );
		return;
	}
	if ( state.phase == CNEOBot::LadderRecoveryState::VERIFYING_ATTACH )
	{
		// One physics tick must hold the attachment before the kick replaces it.
		if ( gpGlobals->tickcount > state.attachTick )
		{
			LaunchRecovery( me );
		}
		return;
	}
	if ( state.phase == CNEOBot::LadderRecoveryState::FLYING )
	{
		UpdateRecoveryFlight( me );
		return;
	}
	if ( state.phase == CNEOBot::LadderRecoveryState::AIMING )
	{
		const Vector look = state.landingNextArea ? state.landingNextArea->GetCenter() : state.landingArea->GetCenter();
		me->GetBodyInterface()->AimHeadTowards( look, IBody::MANDATORY, RECOVERY_AIM_TIME, nullptr, "Facing the outgoing ground path" );
		if ( gpGlobals->curtime >= state.phaseDeadline )
		{
			PlaceRecoveryC( me );
		}
		return;
	}
	if ( state.phase == CNEOBot::LadderRecoveryState::SETTLING )
	{
		const Vector &landing = state.landings[state.nextPlacement - 1];
		CBaseEntity *ground = me->GetGroundEntity();
		const bool supported = ground && !ground->IsPlayer() && ground->GetMoveType() == MOVETYPE_NONE
			&& ( me->GetFlags() & FL_ONGROUND ) && me->GetAbsOrigin().DistToSqr( landing ) <= Square( RECOVERY_LANDING_TOLERANCE );
		if ( !supported )
		{
			state.groundedSince = -1.0f;
		}
		else if ( state.groundedSince < 0.0f )
		{
			state.groundedSince = gpGlobals->curtime;
		}
		if ( supported && gpGlobals->curtime - state.groundedSince >= RECOVERY_GROUNDED_TIME )
		{
			if ( FindRecoveryHandoff( me ) )
			{
				state.phase = CNEOBot::LadderRecoveryState::WALKING_OUT;
				return;
			}

			// A narrow exit can support standing without room for a walk-out.
			if ( !RecomputeRecoveryPath( me ) )
			{
				StopRecovery( me, "failed", "no_path" );
				return;
			}
			StopRecovery( me, "complete", "no_walkout" );
			return;
		}
		if ( gpGlobals->curtime >= state.phaseDeadline )
		{
			PlaceRecoveryC( me );
		}
		return;
	}
	if ( state.phase == CNEOBot::LadderRecoveryState::WALKING_OUT )
	{
		if ( !me->GetGroundEntity() || !CheckRecoveryWalk( me, state.handoffTarget ) )
		{
			StopRecovery( me, "failed", "no_support" );
			return;
		}
		if ( ( me->GetAbsOrigin() - state.handoffTarget ).AsVector2D().LengthSqr() <= Square( RECOVERY_LANDING_TOLERANCE ) )
		{
			if ( !RecomputeRecoveryPath( me ) )
			{
				StopRecovery( me, "failed", "no_path" );
				return;
			}
			StopRecovery( me, "complete", "none" );
		}
	}
}

//---------------------------------------------------------------------------------------------
void CNEOBot::CancelLadderRecovery()
{
	CNEOBotLadderClimb::EndRecoveryWatch( this, m_ladderRecovery.owner );
}

//---------------------------------------------------------------------------------------------
void CNEOBot::ResetLadderRecovery()
{
	if ( m_ladderRecovery.ownsInputs )
	{
		CNEOBotLadderClimb::StopRecovery( this, "cancelled", "lifecycle" );
	}
	m_ladderRecovery = LadderRecoveryState();
}

//---------------------------------------------------------------------------------------------
bool CNEOBotLadderClimb::IsRecoveryOwner( CNEOBot *me, const void *owner )
{
	if ( !me->IsAlive() || !owner || me->m_ladderRecovery.owner != owner
		|| ( me->GetNeoFlags() & NEO_FL_FREEZETIME ) || ( me->GetFlags() & FL_FROZEN )
		|| ( NEORules()->GetRoundStatus() != RoundLive && NEORules()->GetRoundStatus() != Overtime ) )
	{
		return false;
	}

	INextBotEventResponder *leaf = me->GetIntentionInterface()->FirstContainedResponder();
	while ( leaf && leaf->FirstContainedResponder() )
	{
		leaf = leaf->FirstContainedResponder();
	}
	return leaf == owner;
}

//---------------------------------------------------------------------------------------------
bool CNEOBotLadderClimb::HasRecoveryIntent( CNEOBot *me )
{
	const CNEOBot::LadderRecoveryState &state = me->m_ladderRecovery;
	const PathFollower *path = me->GetCurrentPath();
	if ( !state.ladder || !state.goingUp || !state.hasObjective || !path || !path->IsValid()
		|| ( state.hadSubject && !state.subject.Get() ) )
	{
		return false;
	}
	if ( path->GetSubject() != state.subject.Get()
		|| ( !path->GetSubject() && path->GetEndPosition().DistToSqr( state.objective ) > Square( RECOVERY_OBJECTIVE_TOLERANCE ) ) )
	{
		return false;
	}
	if ( state.intendedDismount )
	{
		return true;
	}
	const Path::Segment *segment = path->GetCurrentGoal();
	for ( int i = 0; segment && i < RECOVERY_PATH_SEGMENTS; ++i, segment = path->NextSegment( segment ) )
	{
		if ( segment->ladder == state.ladder && segment->type == Path::LADDER_UP && segment->how == GO_LADDER_UP )
		{
			return true;
		}
	}
	return false;
}

//---------------------------------------------------------------------------------------------
void CNEOBotLadderClimb::BeginRecoveryWatch( CNEOBot *me, const CNavLadder *ladder, bool goingUp, const void *owner )
{
	if ( neo_bot_ladder_recovery.GetInt() == LADDER_RECOVERY_STOCK )
	{
		return;
	}

	CNEOBot::LadderRecoveryState &state = me->m_ladderRecovery;
	const PathFollower *path = me->GetCurrentPath();
	const bool hasObjective = path && path->IsValid();
	const bool sameObjective = hasObjective && state.hasObjective && path->GetSubject() == state.subject.Get()
		&& ( path->GetSubject() || path->GetEndPosition().DistToSqr( state.objective ) <= Square( RECOVERY_OBJECTIVE_TOLERANCE ) );
	if ( state.ladder != ladder || state.goingUp != goingUp || !sameObjective || state.traversalClosed )
	{
		EndRecoveryWatch( me, state.owner );
		state = CNEOBot::LadderRecoveryState();
		state.ladder = ladder;
		state.ladderID = ladder ? ladder->GetID() : 0;
		state.goingUp = goingUp;
		state.hasObjective = hasObjective;
		if ( hasObjective )
		{
			state.objective = path->GetEndPosition();
			state.subject = path->GetSubject();
			state.hadSubject = path->GetSubject() != nullptr;
		}
		state.peakZ = me->GetAbsOrigin().z;
		state.windowPeakZ = state.peakZ;
		state.windowSince = gpGlobals->curtime;
	}
	if ( state.owner != owner && state.phase == CNEOBot::LadderRecoveryState::FAILED )
	{
		// Allow ordinary climbing after cooldown without renewing the recovery budget.
		state.phase = CNEOBot::LadderRecoveryState::WATCHING;
	}
	state.owner = owner;
}

//---------------------------------------------------------------------------------------------
void CNEOBotLadderClimb::EndRecoveryWatch( CNEOBot *me, const void *owner )
{
	CNEOBot::LadderRecoveryState &state = me->m_ladderRecovery;
	if ( state.owner != owner )
	{
		return;
	}
	StopRecovery( me, "cancelled", "behavior_end" );
	state.owner = nullptr;
	state.commandedUp = false;
}

//---------------------------------------------------------------------------------------------
void CNEOBotLadderClimb::LatchRecoveryDismount( CNEOBot *me, const void *owner )
{
	CNEOBot::LadderRecoveryState &state = me->m_ladderRecovery;
	if ( state.owner != owner || !state.goingUp || state.intendedDismount )
	{
		return;
	}
	state.intendedDismount = true;
	state.commandedUp = false;
	state.dismountSince = gpGlobals->curtime;
}

//---------------------------------------------------------------------------------------------
void CNEOBotLadderClimb::RecoveryDecision( CNEOBot *me, RecoveryTrigger trigger, RecoveryWatchPhase phase, const char *gate )
{
	CNEOBot::LadderRecoveryState &state = me->m_ladderRecovery;
	const char *name = "on_stuck";
	switch ( trigger )
	{
	case RECOVERY_SLIP:
	{
		name = "mounted_slip";
		break;
	}
	case RECOVERY_BOUNCE:
	{
		name = "mount_bounce";
		break;
	}
	case RECOVERY_STALL:
	{
		name = "mounted_stall";
		break;
	}
	case RECOVERY_EXIT_TIMEOUT:
	{
		name = "exit_timeout";
		break;
	}
	case RECOVERY_BOUNCE_LANDING:
	{
		name = "bounce_landing";
		break;
	}
	default:
	{
		break;
	}
	}
	const int level = neo_bot_ladder_recovery.GetInt();
	const char *result = "refused";
	if ( !gate && level != LADDER_RECOVERY_STOCK )
	{
		if ( state.episodeUsed )
		{
			gate = "episode_budget";
		}
		else if ( me->IsLadderRecoveryBlocked( state.ladder, state.goingUp ) )
		{
			gate = "cooldown";
		}
		else if ( !CheckRecoverySource( me ) )
		{
			gate = "source_face";
		}
		else
		{
			FindRecoveryLandings( me, RECOVERY_PLACEMENT );
			gate = state.landingCount ? "variant_pending" : state.searchGate;
			const bool started = state.landingCount && ( level == LADDER_RECOVERY_C ? StartRecoveryC( me )
				: StartRecoveryA( me, phase, level == LADDER_RECOVERY_B ) );
			if ( started )
			{
				result = "started";
				gate = "none";
			}
			else if ( !state.landingCount )
			{
				state.episodeUsed = true;
				state.level = level;
				state.episodeStart = me->GetAbsOrigin();
				state.deadline = gpGlobals->curtime + RECOVERY_EPISODE_TIME;
				state.phase = CNEOBot::LadderRecoveryState::FAILED;
				TryRecoveryReturn( me );
				BlockRecoveryLadder( me, RECOVERY_FAILURE_EXCLUSION );
				RecomputeRecoveryPath( me );
			}
		}
	}
	if ( me->IsDebugging( NEXTBOT_PATH ) )
	{
		DevMsg( "%s: Ladder recovery %s: %s (gate %s)\n", me->GetDebugIdentifier(), name, result, gate ? gate : "none" );
	}
}

//---------------------------------------------------------------------------------------------
void CNEOBotLadderClimb::UpdateRecoveryWatch( CNEOBot *me, const void *owner, const CNavArea *exitArea, RecoveryWatchPhase phase )
{
	if ( neo_bot_ladder_recovery.GetInt() == LADDER_RECOVERY_STOCK )
	{
		return;
	}

	if ( !IsRecoveryOwner( me, owner ) || me->m_ladderRecovery.ownsInputs )
	{
		return;
	}

	CNEOBot::LadderRecoveryState &state = me->m_ladderRecovery;
	if ( state.mountClosedOnSupport && state.contactSince < 0.0f && me->GetMoveType() == MOVETYPE_LADDER )
	{
		state.intendedDismount = false;
		state.dismountSince = -1.0f;
		state.conditions = 0;
		state.mountClosedOnSupport = false;
	}
	// Locomotion's hop must stay excluded even if its path became invalid during the dismount.
	if ( me->GetLocomotionInterface()->IsDismountingLadderTop() )
	{
		LatchRecoveryDismount( me, owner );
	}
	if ( !HasRecoveryIntent( me ) )
	{
		return;
	}
	FindRecoveryLandings( me, WATCH_GEOMETRY );
	if ( !state.intendedDismount && IsRecoveryCorridor( me ) )
	{
		LatchRecoveryDismount( me, owner );
	}

	if ( state.stuckLatched && me->GetAbsOrigin().DistToSqr( state.stuckPosition ) > Square( RECOVERY_STUCK_CLEAR_PROGRESS ) )
	{
		state.stuckLatched = false;
	}

	const float now = gpGlobals->curtime;
	const float feetZ = me->GetAbsOrigin().z;
	const bool mounted = me->GetMoveType() == MOVETYPE_LADDER;
	const bool supported = me->GetGroundEntity() != nullptr && ( me->GetFlags() & FL_ONGROUND );
	// Restrict bounce landings to the ladder foot so intermediate ledges stay excluded.
	bool bounceLanded = false;
	if ( !mounted && supported && neo_bot_ladder_recovery_bounce_landing.GetBool() && state.wasMounted
		&& !state.intendedDismount && !state.traversalClosed
		&& feetZ <= state.ladder->m_bottom.z + me->GetLocomotionInterface()->GetStepHeight()
		&& state.peakZ >= feetZ + RECOVERY_BOUNCE_RISE )
	{
		Vector foot;
		CalcClosestPointOnLineSegment( me->GetAbsOrigin(), state.ladder->m_bottom, state.ladder->m_top, foot );
		const float range = me->GetBodyInterface()->GetHullWidth() + RECOVERY_BOUNCE_MARGIN;
		bounceLanded = ( me->GetAbsOrigin() - foot ).AsVector2D().LengthSqr() <= Square( range );
	}
	if ( !mounted && supported )
	{
		CBaseEntity *ground = me->GetGroundEntity();
		if ( !ground->IsPlayer() && ground->GetMoveType() == MOVETYPE_NONE && me->GetLastKnownArea() )
		{
			state.hasLastSupport = true;
			state.lastSupport = me->GetAbsOrigin();
			state.lastSupportArea = me->GetLastKnownArea();
		}
		// Require fresh ladder contact after a supported ledge step.
		state.mountCycle = false;
		state.wasMounted = false;
		state.contactSince = -1.0f;
		state.detachedSince = -1.0f;
		state.lostSince = -1.0f;
		state.stalledWindows = 0;
		if ( exitArea && exitArea->IsOverlapping( me->GetAbsOrigin() )
			&& fabsf( feetZ - exitArea->GetZ( me->GetAbsOrigin() ) ) <= me->GetLocomotionInterface()->GetStepHeight() )
		{
			state.traversalClosed = true;
		}
		else
		{
			state.mountClosedOnSupport = true;
		}
	}

	if ( mounted )
	{
		if ( state.contactSince < 0.0f )
		{
			state.contactSince = now;
			state.peakZ = feetZ;
			state.windowPeakZ = feetZ;
			state.windowSince = now;
			state.stalledWindows = 0;
		}
		state.wasMounted = true;
		state.mountCycle = now - state.contactSince >= RECOVERY_CONTACT_TIME;
		state.detachedSince = -1.0f;
		if ( feetZ > state.peakZ + RECOVERY_STALL_PROGRESS )
		{
			state.stuckLatched = false;
		}
		state.peakZ = Max( state.peakZ, feetZ );
	}
	else
	{
		state.contactSince = -1.0f;
		if ( state.wasMounted && !supported && state.detachedSince < 0.0f )
		{
			state.detachedSince = now;
		}
	}

	unsigned int conditions = 0;
	if ( state.intendedDismount )
	{
		if ( !state.traversalClosed && now - state.dismountSince >= RECOVERY_DISMOUNT_GRACE )
		{
			conditions |= RECOVERY_EXIT_TIMEOUT;
		}
	}
	else
	{
		const bool losingHeight = mounted && state.mountCycle && state.peakZ - feetZ > RECOVERY_SLIP_HEIGHT;
		if ( losingHeight && state.lostSince < 0.0f )
		{
			state.lostSince = now;
		}
		else if ( !losingHeight )
		{
			state.lostSince = -1.0f;
		}
		if ( state.lostSince >= 0.0f && now - state.lostSince >= RECOVERY_SLIP_TIME )
		{
			conditions |= RECOVERY_SLIP;
		}

		Vector closest;
		CalcClosestPointOnLineSegment( me->GetAbsOrigin(), state.ladder->m_bottom, state.ladder->m_top, closest );
		const float bounceRange = me->GetBodyInterface()->GetHullWidth() + RECOVERY_BOUNCE_MARGIN;
		if ( !mounted && state.wasMounted && !supported && state.detachedSince >= 0.0f
			&& now - state.detachedSince >= RECOVERY_DETACH_TIME && me->GetAbsVelocity().z < 0.0f
			&& state.landingCount && feetZ < state.landings[0].z - RECOVERY_LANDING_CLEARANCE - me->GetLocomotionInterface()->GetStepHeight()
			&& ( me->GetAbsOrigin() - closest ).AsVector2D().LengthSqr() <= Square( bounceRange ) )
		{
			conditions |= RECOVERY_BOUNCE;
		}

		Vector facing;
		me->EyeVectors( &facing );
		Vector2D facing2D = facing.AsVector2D();
		facing2D.NormalizeInPlace();
		Vector2D normal2D = state.ladder->GetNormal().AsVector2D();
		normal2D.NormalizeInPlace();
		const bool aligned = fabsf( DotProduct2D( facing2D, normal2D ) ) >= RECOVERY_VIEW_ALIGNMENT;
		if ( !mounted || !aligned || !state.commandedUp || phase != RECOVERY_CLIMB )
		{
			state.windowSince = now;
			state.windowPeakZ = state.peakZ;
			state.stalledWindows = 0;
		}
		else if ( now - state.windowSince >= RECOVERY_STALL_WINDOW )
		{
			state.stalledWindows = state.peakZ - state.windowPeakZ < RECOVERY_STALL_PROGRESS ? state.stalledWindows + 1 : 0;
			state.windowSince = now;
			state.windowPeakZ = state.peakZ;
		}
		if ( state.stalledWindows >= RECOVERY_STALL_WINDOWS )
		{
			conditions |= RECOVERY_STALL;
		}
	}

	if ( bounceLanded )
	{
		conditions |= RECOVERY_BOUNCE_LANDING;
	}
	const unsigned int fresh = conditions & ~state.conditions;
	state.conditions = conditions;
	const RecoveryTrigger triggers[] = { RECOVERY_SLIP, RECOVERY_BOUNCE, RECOVERY_STALL, RECOVERY_EXIT_TIMEOUT, RECOVERY_BOUNCE_LANDING };
	for ( int i = 0; i < ARRAYSIZE( triggers ); ++i )
	{
		if ( fresh & triggers[i] )
		{
			RecoveryDecision( me, triggers[i], phase, nullptr );
			if ( state.ownsInputs )
			{
				return;
			}
		}
	}
}

//---------------------------------------------------------------------------------------------
bool CNEOBotLadderClimb::TryStuckRecovery( CNEOBot *me, INextBotEventResponder *behavior )
{
	if ( me->m_ladderRecovery.ownsInputs )
	{
		return true;
	}
	if ( !behavior || neo_bot_ladder_recovery.GetInt() == LADDER_RECOVERY_STOCK )
	{
		return false;
	}

	// Only the active leaf can own this recovery; buried or suspended climbs are stale.
	INextBotEventResponder *responder = behavior->FirstContainedResponder();
	if ( !responder )
	{
		return false;
	}

	while ( INextBotEventResponder *child = responder->FirstContainedResponder() )
	{
		responder = child;
	}

	Action<CNEOBot> *action = static_cast<Action<CNEOBot> *>( responder );
	if ( action->IsNamed( "LadderClimb" ) || action->IsNamed( "LadderApproach" ) )
	{
		CNEOBot::LadderRecoveryState &state = me->m_ladderRecovery;
		if ( IsRecoveryOwner( me, action ) && me->GetLocomotionInterface()->IsDismountingLadderTop() )
		{
			LatchRecoveryDismount( me, action );
		}
		if ( IsRecoveryOwner( me, action ) && HasRecoveryIntent( me ) && !state.stuckLatched )
		{
			state.stuckLatched = true;
			state.stuckPosition = me->GetAbsOrigin();
			RecoveryDecision( me, RECOVERY_STUCK, action->IsNamed( "LadderApproach" ) ? RECOVERY_APPROACH : RECOVERY_CLIMB,
				state.intendedDismount ? "dismount_grace" : nullptr );
		}
	}

	return me->m_ladderRecovery.ownsInputs;
}

//---------------------------------------------------------------------------------------------
CNEOBotLadderClimb::CNEOBotLadderClimb( const CNavLadder *ladder, bool goingUp )
	: m_ladder( ladder ), m_bGoingUp( goingUp ), m_flLastZ( 0.0f ),
	m_bDismountPhase( false ), m_bJumpedOffLadder( false ), m_bCrawlDismount( false ), m_bCrawlPushed( false ), m_pExitArea( nullptr ), m_nNudges( 0 )
{
	m_dismountPos = vec3_origin;
	m_ladderForward = ladder ? -ladder->GetNormal() : vec3_origin;
}

//---------------------------------------------------------------------------------------------
ActionResult<CNEOBot> CNEOBotLadderClimb::OnStart( CNEOBot *me, Action<CNEOBot> *priorAction )
{
	if ( !m_ladder )
	{
		return Done( "No ladder specified" );
	}

	// Don't interfere with look direction
	// Can exit out of behavior if threat is present
	me->StopLookingAroundForEnemies();
	me->SetAttribute( CNEOBot::IGNORE_ENEMIES );

	// Timeout based on ladder length
	float estimatedClimbTime = m_ladder->m_length / MAX_CLIMB_SPEED + 3.0f;
	m_timeoutTimer.Start( estimatedClimbTime );

	ILocomotion *mover = me->GetLocomotionInterface();
	m_flLastZ = mover->GetFeet().z;
	m_stuckTimer.Start( STUCK_CHECK_INTERVAL );

	if ( m_bGoingUp )
	{
		// Hull trace check: Ensure clear path to climb in the intended direction.
		Vector traceStart = mover->GetFeet();

		// Trace upwards from feet to check the space the bot would occupy.
		// e.g. ladder going through opening in ceiling
		float targetZ = m_ladder->m_top.z;
		Vector traceEnd = traceStart;
		traceEnd.z = MIN( targetZ, traceStart.z + 100.0f );

		trace_t tr;
		UTIL_TraceHull( traceStart, traceEnd, me->WorldAlignMins(), me->WorldAlignMaxs(), MASK_NPCSOLID, me, COLLISION_GROUP_NONE, &tr );

		// If vertical path blocked by world geometry due to failed approach alignment,
		// teleport bot to ideal position, assuming approach placed bot close enough
		// Ideally we only transition into this behavior when bot has attached to ladder
		if ( tr.DidHit() && tr.fraction < 1.0f )
		{
			if ( me->IsDebugging( NEXTBOT_PATH ) )
			{
				DevMsg( "%s: Ladder climb path obstructed (fraction %.2f), teleporting to ideal position\n",
					me->GetDebugIdentifier(), tr.fraction );
				NDebugOverlay::Box( traceStart, me->WorldAlignMins(), me->WorldAlignMaxs(), 255, 0, 0, 0, 2.0f );
				NDebugOverlay::Box( traceEnd, me->WorldAlignMins(), me->WorldAlignMaxs(), 255, 0, 0, 0, 2.0f );
			}

			// Fallback: Teleport to a center position on the ladder.
			mover->Reset(); // clear velocity cache in locomotion interface
			me->SetAbsVelocity( vec3_origin );

			// Half a hull off the nav line, unchecked: the fallback spot when the clear spot below starts in solid
			Vector idealPos = m_ladder->GetPosAtHeight( m_flLastZ );
			// Offset slightly from the ladder surface based on the bot's collision box
			float offsetDist = me->CollisionProp()->OBBSize().x / 2.0f + LADDER_PLACE_GAP;
			idealPos += m_ladder->GetNormal() * offsetDist;
			idealPos.z = m_flLastZ;

			// On a ladder turned off the world axes, half a hull width off the face leaves the hull's corner in the brush,
			// so clear the corners first, then slide in until the hull touches the face
			const Vector &normal = m_ladder->GetNormal();
			const float flReach = me->CollisionProp()->OBBSize().x / 2.0f * ( fabsf( normal.x ) + fabsf( normal.y ) );
			Vector clearPos = m_ladder->GetPosAtHeight( m_flLastZ ) + normal * ( flReach + LADDER_PLACE_GAP );
			clearPos.z = m_flLastZ;

			trace_t trFace;
			UTIL_TraceHull( clearPos, clearPos - normal * LADDER_PLACE_SEEK, me->WorldAlignMins(), me->WorldAlignMaxs(),
				MASK_PLAYERSOLID, me, COLLISION_GROUP_PLAYER_MOVEMENT, &trFace );
			if ( !trFace.startsolid )
			{
				idealPos = trFace.DidHit() ? trFace.endpos + normal * ( LADDER_GRAB_DIST * 0.5f ) : clearPos;
			}

			// The floor at the spot can stand higher than the feet were, and a hull sunk into a displacement does not start solid,
			// so stand the hull on that floor: unducking later on the ladder lowers the feet by the crouch's lift, into the floor
			trace_t trFloor;
			UTIL_TraceHull( idealPos + Vector( 0.0f, 0.0f, mover->GetStepHeight() ), idealPos, me->WorldAlignMins(), me->WorldAlignMaxs(),
				MASK_PLAYERSOLID, me, COLLISION_GROUP_PLAYER_MOVEMENT, &trFloor );
			if ( !trFloor.startsolid && trFloor.DidHit() )
			{
				idealPos.z = trFloor.endpos.z;
			}

			// Face perpendicularly straight on to the ladder (-normal)
			Vector idealLookDir = -m_ladder->GetNormal();
			QAngle idealAngles;
			VectorAngles( idealLookDir, idealAngles );

			// A spot still in solid, such as inside a ladder brush that stands in front of its nav line, would hold the bot
			// there for good, so the bot climbs from where it is instead
			trace_t trSpot;
			UTIL_TraceHull( idealPos, idealPos, me->WorldAlignMins(), me->WorldAlignMaxs(),
				MASK_PLAYERSOLID, me, COLLISION_GROUP_PLAYER_MOVEMENT, &trSpot );
			if ( !trSpot.startsolid )
			{
				// Teleport the bot
				me->SetAbsOrigin( idealPos );
				me->SetAbsAngles( idealAngles );
				// the view too: a forward press with the view off the face's normal slides the bot off the side of a narrow ladder
				me->SnapEyeAngles( idealAngles );

				// Update mover feet to new teleported position for stuck checking
				m_flLastZ = idealPos.z;
			}
		}
		else if ( me->IsDebugging( NEXTBOT_PATH ) )
		{
			NDebugOverlay::Line( traceStart, traceEnd, 0, 255, 0, true, 2.0f );
		}
	}

	if ( me->IsDebugging( NEXTBOT_PATH ) )
	{
		DevMsg( "%s: Starting ladder climb (%s), length %.1f\n",
			me->GetDebugIdentifier(),
			m_bGoingUp ? "up" : "down",
			m_ladder->m_length );
	}

	// Try to resolve the exit area from the current path early
	ResolveExitArea( me );
	BeginRecoveryWatch( me, m_ladder, m_bGoingUp, this );

	ClaimLadder( me );

	return Continue();
}

//---------------------------------------------------------------------------------------------
// PlayerLocomotion lets go of a ladder it was never asked to use, and once the contact persists
// takes it over by the nearer end - at the foot, the bottom. Tell it this climb is wanted.
void CNEOBotLadderClimb::ClaimLadder( CNEOBot *me ) const
{
	ILocomotion *mover = me->GetLocomotionInterface();
	if ( mover->IsUsingLadder() )
	{
		return;
	}

	const CNavArea *pExitArea = m_pExitArea;
	if ( !pExitArea )
	{
		pExitArea = LadderEndArea();
	}

	// The locomotion's dismount walks to this area, so there is nothing to claim with without one
	if ( !pExitArea )
	{
		return;
	}

	if ( m_bGoingUp )
	{
		mover->ClimbLadder( m_ladder, pExitArea );
	}
	else
	{
		mover->DescendLadder( m_ladder, pExitArea );
	}
}

//---------------------------------------------------------------------------------------------
void CNEOBotLadderClimb::ResolveExitArea( CNEOBot *me )
{
	// Clear potentially stale exit area
	m_pExitArea = nullptr;

	const PathFollower *path = me->GetCurrentPath();
	if ( path && path->IsValid() )
	{
		const Path::Segment *seg = path->GetCurrentGoal();
		if ( !seg )
		{
			return;
		}

		constexpr int MAX_PATH_SEARCH_STEPS = 100;

		// The path's current goal can still be the area at the ladder's foot when the climb reaches the
		// top, and that area is no exit: the exit is the first area after this ladder
		const Path::Segment *ladderSeg = seg;
		for ( int i = 0; ladderSeg && ladderSeg->ladder != m_ladder && i < MAX_PATH_SEARCH_STEPS; ++i )
		{
			ladderSeg = path->NextSegment( ladderSeg );
		}

		if ( ladderSeg && ladderSeg->ladder == m_ladder )
		{
			seg = ladderSeg;
		}

		int safetyCounter = 0;
		while ( seg && safetyCounter < MAX_PATH_SEARCH_STEPS )
		{
			if ( !seg->ladder )
			{
				break;
			}

			seg = path->NextSegment( seg );
			safetyCounter++;
		}
		Assert( safetyCounter < MAX_PATH_SEARCH_STEPS );

		if ( seg && !seg->ladder && seg->area )
		{
			m_pExitArea = FindLanding( path, seg->area );
			m_dismountPos = GetDismountPos( me );
		}
	}
}

//---------------------------------------------------------------------------------------------
// By the dismount the path's goal can already be past the area this ladder lands on,
// so take a landing the path passes through, or else the ladder's own end area, or, with no end area, the path's
const CNavArea *CNEOBotLadderClimb::FindLanding( const PathFollower *path, const CNavArea *pathExit ) const
{
	const CNavLadder::LadderDirectionType dir = m_bGoingUp ? CNavLadder::LADDER_UP : CNavLadder::LADDER_DOWN;
	if ( m_ladder->IsConnected( pathExit, dir ) )
	{
		return pathExit;
	}

	for ( const Path::Segment *seg = path->FirstSegment(); seg; seg = path->NextSegment( seg ) )
	{
		if ( seg->area && m_ladder->IsConnected( seg->area, dir ) )
		{
			return seg->area;
		}
	}

	const CNavArea *ladderEnd = LadderEndArea();
	return ladderEnd ? ladderEnd : pathExit;
}

//---------------------------------------------------------------------------------------------
// The area at the end this climb leaves the ladder by
const CNavArea *CNEOBotLadderClimb::LadderEndArea() const
{
	return m_bGoingUp ? m_ladder->GetTopArea() : m_ladder->m_bottomArea;
}

//---------------------------------------------------------------------------------------------
// A landing whose short side is under half a hull is a wall cap the climb crosses, not a floor to stand on,
// and a deeper one holds the bot's center even where the path follower's LadderCapAhead() needs the mesh link to climb onto it
bool CNEOBotLadderClimb::IsNarrowLanding( CNEOBot *me ) const
{
	const float flMinDepth = me->GetBodyInterface()->GetHullWidth() * NARROW_LANDING_HULLS;
	return Min( m_pExitArea->GetSizeX(), m_pExitArea->GetSizeY() ) < flMinDepth;
}

//---------------------------------------------------------------------------------------------
// Step off toward where the landing meets this end of the ladder: a long, narrow landing's center
// can lie off to one side, and the dismount kick toward it carries the bot past the landing's edge
Vector CNEOBotLadderClimb::GetDismountPos( CNEOBot *me ) const
{
	if ( IsNarrowLanding( me ) )
	{
		return m_pExitArea->GetCenter();
	}

	const Vector &ladderEnd = m_bGoingUp ? m_ladder->m_top : m_ladder->m_bottom;
	Vector nearPoint;
	m_pExitArea->GetClosestPointOnArea( ladderEnd, &nearPoint );

	// into the wall at the top, where the landing is, and away from it at the bottom
	Vector inward = m_bGoingUp ? m_ladderForward : -m_ladderForward;
	inward.z = 0.0f;
	if ( inward.NormalizeInPlace() > 0.0f )
	{
		nearPoint += inward * LANDING_INSET;
	}

	Vector dismountPos;
	m_pExitArea->GetClosestPointOnArea( nearPoint, &dismountPos );
	return dismountPos;
}


//---------------------------------------------------------------------------------------------
// Implementation based on ladder climbing logic in https://github.com/Dragoteryx/drgbase/
ActionResult<CNEOBot> CNEOBotLadderClimb::Update( CNEOBot *me, float /*interval*/ )
{
	if ( !me->m_ladderRecovery.owner )
	{
		BeginRecoveryWatch( me, m_ladder, m_bGoingUp, this );
		if ( m_bDismountPhase || m_bCrawlDismount )
		{
			LatchRecoveryDismount( me, this );
		}
	}
	UpdateRecoveryWatch( me, this, m_pExitArea, RECOVERY_CLIMB );
	me->m_ladderRecovery.commandedUp = false;
	if ( me->m_ladderRecovery.ownsInputs )
	{
		return Continue();
	}
	if ( me->m_ladderRecovery.owner == this && ( me->m_ladderRecovery.phase == CNEOBot::LadderRecoveryState::COMPLETE
		|| me->m_ladderRecovery.phase == CNEOBot::LadderRecoveryState::FAILED ) )
	{
		return Done( "Ladder recovery episode finished" );
	}

	if ( m_timeoutTimer.IsElapsed() && TeleportOnExpiry( me, EXPIRY_CLIMB ) )
	{
		return Done( "Ladder climb timeout, placed on exit area" );
	}

	if ( m_timeoutTimer.IsElapsed() )
	{
		return Done( "Ladder climb timeout" );
	}

	const CKnownEntity *threat = me->GetVisionInterface()->GetPrimaryKnownThreat(true);
	if ( threat )
	{
		if ( me->IsDebugging( NEXTBOT_PATH ) )
		{
			DevMsg( "%s: Threat detected during ladder climb - engaging\n", me->GetDebugIdentifier() );
		}
		// Detach from ladder to ready weapon
		me->PressJumpButton();
		me->PressBackwardButton(0.1f);
		// ChangeTo: We may move away from ladder when fighting, reevaluate later
		return ChangeTo( new CNEOBotAttack, "Interrupting climb to engage enemy" );
	}

	ILocomotion *mover = me->GetLocomotionInterface();
	IBody *body = me->GetBodyInterface();
	const Vector& myPos = mover->GetFeet();
	bool bExitIsBehind = false;
	Vector toExit = vec3_origin;

	if ( m_pExitArea )
	{
		toExit = m_dismountPos - myPos;
		toExit.z = 0.0f;
		if ( toExit.Length2DSqr() > 0.01f )
		{
			toExit.NormalizeInPlace();
		}
		else
		{
			toExit = m_ladderForward; // Fallback direction
		}

		if ( DotProduct( toExit, m_ladderForward ) < 0.0f )
		{
			bExitIsBehind = true;
		}
	}

	// Check if we're on the ladder (MOVETYPE_LADDER or locomotion says so)
	bool onLadder = me->IsBotOnLadder();

	if ( !onLadder )
	{
		// crawling off, the gap's sill is ground too, short of the landing
		if ( mover->IsOnGround() && !m_bCrawlDismount )
		{
			return Done( "Reached the ground, ending climb" );
		}
		
		if ( !m_bDismountPhase && !m_bJumpedOffLadder )
		{
			Vector ladderClosestPoint;
			CalcClosestPointOnLineSegment( myPos, m_ladder->m_bottom, m_ladder->m_top, ladderClosestPoint );
			if ( myPos.DistToSqr( ladderClosestPoint ) > Square( MAX_DEVIATION_DIST ) )
			{
				return Done( "Fallen too far from ladder, resetting" );
			}
		}
	}

	//------------------------------------------------------------
	// Ladder climbing phase
	//------------------------------------------------------------
	if ( !m_bDismountPhase )
	{
		// The locomotion drops its claim whenever the engine lets go of the bot, even for a tick
		if ( me->GetMoveType() == MOVETYPE_LADDER )
		{
			ClaimLadder( me );
		}

		float currentZ = myPos.z;
		float targetZ = m_bGoingUp ? m_ladder->m_top.z : m_ladder->m_bottom.z;

		// Going down, the bot holds still until its view makes forward take it down: not a stall
		const bool bHoldForView = !m_bGoingUp && !me->GetLocomotionInterface()->IsForwardDownLadder( m_ladder );
		if ( bHoldForView )
		{
			m_flLastZ = currentZ;
			m_stuckTimer.Start( STUCK_CHECK_INTERVAL );
		}

		// Stuck detection: if we haven't made vertical progress, bail out gracefully
		if ( m_stuckTimer.IsElapsed() )
		{
			float verticalDelta = fabsf( currentZ - m_flLastZ );
			if ( verticalDelta < STUCK_Z_TOLERANCE )
			{
				// No vertical progress - if we're close enough to the target, consider it done
				float distToTarget = fabsf( currentZ - targetZ );
				if ( distToTarget < mover->GetStepHeight() * 2.0f )
				{
					EnterDismountPhase( me );
					return Continue();
				}
				else if ( m_nNudges < STALL_NUDGES )
				{
					// Usually snagged on something at the ladder's edge, not blocked outright:
					// shimmy sideways, right then left, until the next progress check, before giving up
					++m_nNudges;
					m_nudgeTimer.Start( STUCK_CHECK_INTERVAL );
				}
				else
				{
					// We are stuck mid-climb. Reset scenario by jumping backwards and ending condition
					if ( me->IsDebugging( NEXTBOT_PATH ) )
					{
						DevMsg( "%s: Ladder climb stuck - jumping backwards to reset (delta %.1f)\n",
							me->GetDebugIdentifier(), verticalDelta );
					}
					if ( TeleportOnExpiry( me, EXPIRY_STALL ) )
					{
						return Done( "Got stuck on something climbing the ladder, placed on exit area" );
					}
					me->PressJumpButton();
					me->PressBackwardButton(0.1f);
					return Done( "Got stuck on something climbing the ladder, jumping off to reset." );
				}
			}
			m_flLastZ = currentZ;
			m_stuckTimer.Start( STUCK_CHECK_INTERVAL );
		}

		// Early jump-off. Going down, only once a standing height below the top: from any higher, the
		// kick towards the exit lands the bot back on the floor the descent started from.
		bool bWantsDismount = false;
		if ( m_pExitArea )
		{
			float zDistToExit = currentZ - m_dismountPos.z;
			const bool bBelowTopFloor = m_bGoingUp || currentZ < m_ladder->m_top.z - body->GetStandHullHeight();

			if ( zDistToExit > 0.0f && zDistToExit <= SAFE_FALL_DIST && bBelowTopFloor )
			{
				bWantsDismount = true;
			}
		}

		if ( m_bGoingUp )
		{
			body->SetDesiredPosture( IBody::STAND );
		}

		float dismountZ = targetZ;
		if ( m_pExitArea )
		{
			if ( m_bGoingUp )
			{
				if ( bExitIsBehind )
				{
					dismountZ = Min( m_dismountPos.z, targetZ );
				}
			}
			else
			{
				// Allow early drop-off at intermediate floors
				dismountZ = Max( m_dismountPos.z, targetZ );
			}
		}

		// Check if we've reached the target exit height
		if ( m_bGoingUp ? ( currentZ >= dismountZ ) : ( currentZ <= dismountZ ) )
		{
			bWantsDismount = true;
		}

		// A gap at the top only a crouched body fits through, such as a vent or a low window: climbing on never clears it,
		// so duck on the way up, and step off crouched once the gap is in reach
		if ( m_bGoingUp && onLadder && m_pExitArea && currentZ > GetCrawlFeetZ() - CRAWL_DUCK_RANGE && IsCrawlOnlyExit( me, toExit ) )
		{
			me->PressCrouchButton( CRAWL_CROUCH_HOLD );
			if ( ( me->GetFlags() & FL_DUCKING ) && fabsf( GetCrawlFeetZ() - currentZ ) <= CRAWL_SNAP_DIST )
			{
				m_bCrawlDismount = true;
				EnterDismountPhase( me );
				UpdateCrawlDismount( me, toExit, onLadder );
				return Continue();
			}
		}

		// A parapet between the ladder and a roof exit blocks the step off at floor height:
		// keep climbing until the hull clears it (stuck detection ends it if nothing does)
		const bool bLipInTheWay = bWantsDismount && m_bGoingUp && onLadder
			&& IsDismountBlocked( me, m_pExitArea ? toExit : m_ladderForward );

		if ( bWantsDismount && !bLipInTheWay )
		{
			EnterDismountPhase( me );
			return Continue();
		}

		// Adjust vertical height based on target exit area
		bool bIsClimbingDown = false;
		if ( onLadder )
		{
			bool bShouldGoUp = m_bGoingUp;
			if ( m_pExitArea )
			{
				bShouldGoUp = ( currentZ < m_dismountPos.z );
			}

			if ( bLipInTheWay )
			{
				bShouldGoUp = true;
			}

			if ( bShouldGoUp )
			{
				me->PressMoveUpButton();
				me->m_ladderRecovery.commandedUp = m_bGoingUp && !bHoldForView;
			}
			else
			{
				me->PressMoveDownButton();
				bIsClimbingDown = true;
			}
		}


		// A descent the locomotion has claimed is aimed by its DescendLadder();
		// a second MANDATORY aim here would hold the view back from it
		if ( m_bGoingUp || !mover->IsUsingLadder() )
		{
			// Look at and move to the dismount height, slightly behind the ladder
			Vector lookTarget = m_ladder->GetPosAtHeight( dismountZ );
			lookTarget -= m_ladder->GetNormal() * 50.0f;
			body->AimHeadTowards( lookTarget, IBody::MANDATORY, 0.1f, nullptr,
				m_bGoingUp ? "Climbing up (looking at dismount position)" : "Climbing down (looking at dismount position)" );
		}

		if ( bHoldForView )
		{
			me->ReleaseForwardButton();
		}
		else
		{
			me->PressForwardButton(0.1f);
		}

		if ( onLadder && m_nudgeTimer.HasStarted() && !m_nudgeTimer.IsElapsed() )
		{
			if ( m_nNudges % 2 )
			{
				me->PressRightButton( 0.1f );
			}
			else
			{
				me->PressLeftButton( 0.1f );
			}
		}
	}

	//------------------------------------------------------------
	// Ladder dismount phase
	//------------------------------------------------------------
	if ( m_bDismountPhase )
	{
		// The last known area becomes the landing while the bot is still in the air beside it,
		// so finish once the bot stands on the landing, or enters a narrow one it crosses
		if ( m_pExitArea && me->GetLastKnownArea() == m_pExitArea
			&& ( IsNarrowLanding( me ) || ( mover->IsOnGround() && m_pExitArea->IsOverlapping( myPos ) ) ) )
		{
			return Done( "Reached next NavArea after dismount" );
		}

		// Safety timeout
		if ( m_dismountTimer.IsElapsed() && TeleportOnExpiry( me, EXPIRY_DISMOUNT ) )
		{
			return Done( "Dismount walk timed out, placed on exit area" );
		}
		if ( m_dismountTimer.IsElapsed() )
		{
			return Done( "Dismount walk timed out" );
		}

		if ( m_bCrawlDismount && m_pExitArea )
		{
			UpdateCrawlDismount( me, toExit, onLadder );
			return Continue();
		}

		// Build look target toward exit area center with vertical bias preserved
		if ( m_pExitArea )
		{
			bool bDroppingEarly = ( myPos.z >= m_dismountPos.z ); 
			// Maintain Z-height while on the ladder in the dismount phase
			if ( onLadder )
			{
				if ( myPos.z < m_dismountPos.z )
				{
					me->PressMoveUpButton();
				}
				else
				{
					me->PressMoveDownButton();
				}
			}
			else
			{
				bDroppingEarly = true;
			}

			body->AimHeadTowards( m_dismountPos, IBody::MANDATORY, 0.1f, nullptr, "Walking to exit area" );

			// Jump to detach from ladder if exit is not straight ahead, or if we have reached the exit height
			float dot = DotProduct( toExit, m_ladderForward );

			if ( dot < 0.5f || bDroppingEarly )
			{
				// Velocity kick to simulate a jump to the next NavArea
				if ( !m_bJumpedOffLadder )
				{
					me->PressJumpButton(); // mostly to trigger animation if possible

					mover->Reset(); // clear velocity cache in locomotion interface

					Vector jumpVelocity = toExit * 150.0f;
					jumpVelocity.z = 150.0f - me->GetAbsVelocity().z;

					me->ApplyAbsVelocityImpulse( jumpVelocity );
					m_bJumpedOffLadder = true;
				}
				else if ( !onLadder )
				{
					me->PressForwardButton();
				}
			}
			else
			{
				// Forward exit, just clamber over
				me->PressForwardButton();
			}

			if ( me->IsDebugging( NEXTBOT_PATH ) )
			{
				NDebugOverlay::Line( myPos, m_dismountPos, 0, 255, 255, true, 0.1f );
			}
		}
		else
		{
			// No exit area resolved - just push forward along the ladder normal
			Vector pushDir = m_ladderForward;
			Vector pushTarget = myPos + 100.0f * pushDir;
			body->AimHeadTowards( pushTarget, IBody::MANDATORY, 0.1f, nullptr, "Dismount push forward" );
			me->PressForwardButton();
			
			if ( mover->IsOnGround() )
			{
				return Done( "Dismounted to ground" );
			}
		}
	}

	return Continue();
}

//---------------------------------------------------------------------------------------------
bool CNEOBotLadderClimb::IsDismountBlocked( CNEOBot *me, const Vector &toExit ) const
{
	const Vector &feet = me->GetLocomotionInterface()->GetFeet();
	const float hullWidth = me->GetBodyInterface()->GetHullWidth();
	const Vector traceEnd = feet + toExit * ( DISMOUNT_CLEARANCE_HULLS * hullWidth );

	trace_t tr;
	UTIL_TraceHull( feet, traceEnd, me->WorldAlignMins(), me->WorldAlignMaxs(), MASK_NPCSOLID, me, COLLISION_GROUP_NONE, &tr );

	return tr.DidHit();
}
//---------------------------------------------------------------------------------------------
// Which way to crawl off toward the exit: along the face for an exit beside the ladder,
// since on the ladder a press with any part into the face climbs
Vector CNEOBotLadderClimb::GetCrawlDirection( const Vector &toExit ) const
{
	if ( DotProduct( toExit, m_ladderForward ) >= 0.5f )
	{
		return toExit;
	}

	const Vector &normal = m_ladder->GetNormal();
	Vector along = toExit - DotProduct( toExit, normal ) * normal;
	along.z = 0.0f;
	return ( along.NormalizeInPlace() > 0.0f ) ? along : toExit;
}

//---------------------------------------------------------------------------------------------
// Where the feet step off crawling: just above the ladder's top
float CNEOBotLadderClimb::GetCrawlFeetZ() const
{
	return m_ladder->m_top.z + CRAWL_FEET_ABOVE_TOP;
}

//---------------------------------------------------------------------------------------------
// From the ladder's top toward the exit, the standing body is blocked but a crouched one is clear
bool CNEOBotLadderClimb::IsCrawlOnlyExit( CNEOBot *me, const Vector &toExit ) const
{
	const IBody *body = me->GetBodyInterface();
	const float halfWidth = body->GetHullWidth() * 0.5f;
	const Vector mins( -halfWidth, -halfWidth, 0.0f );
	const Vector standMaxs( halfWidth, halfWidth, body->GetStandHullHeight() );
	const Vector crouchMaxs( halfWidth, halfWidth, body->GetCrouchHullHeight() );

	Vector start = me->GetLocomotionInterface()->GetFeet();
	start.z = GetCrawlFeetZ();
	const Vector end = start + GetCrawlDirection( toExit ) * ( DISMOUNT_CLEARANCE_HULLS * body->GetHullWidth() );

	trace_t tr;
	UTIL_TraceHull( start, end, mins, standMaxs, MASK_NPCSOLID, me, COLLISION_GROUP_NONE, &tr );
	if ( !tr.DidHit() )
	{
		return false;
	}

	UTIL_TraceHull( start, end, mins, crouchMaxs, MASK_NPCSOLID, me, COLLISION_GROUP_NONE, &tr );
	return !tr.DidHit();
}

//---------------------------------------------------------------------------------------------
// Bot updates come a tenth of a second apart, too coarse to stop a climb inside the gap's few units,
// so the crouched bot is set just above the ladder's top, let go of the ladder and pushed into the gap:
//
//              |#####|          <- the gap's top
//      ladder  | o-> |  landing
//       ||     |_____|________  <- the gap's sill, the ladder's top
//       ||     |#####|
void CNEOBotLadderClimb::UpdateCrawlDismount( CNEOBot *me, const Vector &toExit, bool bOnLadder )
{
	me->PressCrouchButton( CRAWL_CROUCH_HOLD );
	if ( bOnLadder && !m_bCrawlPushed )
	{
		m_bCrawlPushed = true;

		Vector crawlPos = me->GetLocomotionInterface()->GetFeet();
		crawlPos.z = GetCrawlFeetZ();
		trace_t tr;
		UTIL_TraceHull( crawlPos, crawlPos, me->WorldAlignMins(), me->WorldAlignMaxs(), MASK_PLAYERSOLID, me, COLLISION_GROUP_PLAYER_MOVEMENT, &tr );
		if ( tr.startsolid )
		{
			return;
		}

		me->ReleaseMoveUpButton();
		me->ReleaseForwardButton();
		me->SetAbsOrigin( crawlPos );
		me->SetMoveType( MOVETYPE_WALK );

		// with a little rise, so the bot reaches the sill before it falls below it
		Vector push = GetCrawlDirection( toExit ) * CRAWL_PUSH_SPEED;
		push.z = CRAWL_PUSH_RISE_SPEED;
		me->SetAbsVelocity( push );
		return;
	}

	// Level, so the bot looks where it walks. In the air beside the ladder a press toward its face grabs it again,
	// so the push alone carries the bot until it lands
	me->GetBodyInterface()->AimHeadTowards( me->EyePosition() + toExit * 100.0f, IBody::MANDATORY, 0.1f, nullptr, "Crawling off the ladder" );
	ILocomotion *mover = me->GetLocomotionInterface();
	if ( mover->IsOnGround() )
	{
		mover->Approach( mover->GetFeet() + toExit * 100.0f );
	}
}

//---------------------------------------------------------------------------------------------
void CNEOBotLadderClimb::EnterDismountPhase( CNEOBot *me )
{
	me->SetAbsVelocity( vec3_origin ); // stop momentum
	m_bDismountPhase = true;
	LatchRecoveryDismount( me, this );
	m_dismountTimer.Start( DISMOUNT_TIMEOUT );
	ResolveExitArea( me );

	if ( me->IsDebugging( NEXTBOT_PATH ) )
	{
		DevMsg( "%s: Entering dismount phase (exit area %s)\n",
			me->GetDebugIdentifier(),
			m_pExitArea ? "found" : "NOT found" );
	}
}

//---------------------------------------------------------------------------------------------
void CNEOBotLadderClimb::OnEnd( CNEOBot *me, Action<CNEOBot> *nextAction )
{
	EndRecoveryWatch( me, this );
	me->StartLookingAroundForEnemies();
	me->ClearAttribute( CNEOBot::IGNORE_ENEMIES );

	if ( me->IsDebugging( NEXTBOT_PATH ) )
	{
		DevMsg( "%s: Finished ladder climb\n", me->GetDebugIdentifier() );
	}
}

//---------------------------------------------------------------------------------------------
ActionResult<CNEOBot> CNEOBotLadderClimb::OnSuspend( CNEOBot *me, Action<CNEOBot> *interruptingAction )
{
	EndRecoveryWatch( me, this );
	return Done( "OnSuspend: Cancel out of ladder climb, situation will likely become stale." );
}

//---------------------------------------------------------------------------------------------
ActionResult<CNEOBot> CNEOBotLadderClimb::OnResume( CNEOBot *me, Action<CNEOBot> *interruptingAction )
{
	return Done( "OnResume: Cancel out of ladder climb, situation is likely stale." );
}
