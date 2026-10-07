#include "cbase.h"
#include "neo_bot.h"
#include "neo_bot_path_cost.h"
#include "neo_gamerules.h"
#include "neo_bot_locomotion.h"
#include "nav_mesh.h"
#include "neo_bot_path_reservation.h"

ConVar neo_bot_path_around_friendly_cooldown("neo_bot_path_around_friendly_cooldown", "2.0", FCVAR_CHEAT,
	"How often to check for friendly path dispersion", true, 0, true, 60);

ConVar neo_bot_path_penalty_jump_multiplier("neo_bot_path_penalty_jump_multiplier", "100000.0", FCVAR_CHEAT,
	"Maximum penalty multiplier for jump height changes in pathfinding", true, 0.01f, false, 0.0f);

ConVar neo_bot_path_penalty_ladder_multiplier("neo_bot_path_penalty_ladder_multiplier", "3.0", FCVAR_CHEAT,
	"Penalty multiplier for ladder traversal in pathfinding", true, 0.1f, false, 0.0f);

// A mesh author marks an area NAV_MESH_AVOID where bots come to grief: the pit below a ladder they fall off,
// a shaft top they pile up on. A step into one costs this many times its length, so bots route around it
// wherever a reasonable alternative exists. The cost stays finite, so an AVOID area that is the only way
// through is still used.
static constexpr float NEO_BOT_PATH_AVOID_MULTIPLIER = 20.0f;

// Only Recon stands in a 64-71 u space; Assault (65) and Support (70) have to duck through one,
// which is slow and leaves them unable to shoot well. 1.0 is the old behaviour, so this is off
// until deliberately raised.
ConVar neo_bot_path_penalty_crouch_multiplier("neo_bot_path_penalty_crouch_multiplier", "1.0", FCVAR_CHEAT,
	"Penalty multiplier for entering a NAV_MESH_CROUCH area, applied to every class but Recon. "
	"1.0 is no penalty. A large value makes other classes route around crouch spaces where any "
	"alternative exists, while still allowing one when it is the only way through.",
	true, 1.0f, false, 0.0f);

// NEO-HARNESS-TEMP research arm (2026-09-25, patch 85): risk annotations as path cost. The meshes keep
// every reachable place and every ladder connected; a risky one is marked and made expensive instead,
// finite so it stays usable when it is the only way (user rule, 2026-09-25)
ConVar neo_bot_path_avoid_multiplier("neo_bot_path_avoid_multiplier", "1.0", FCVAR_CHEAT,
	"Research: path cost multiplier for a step into a NAV_MESH_AVOID area (1 = off)", true, 1.0f, false, 0.0f);
ConVar neo_bot_path_cliff_multiplier("neo_bot_path_cliff_multiplier", "1.0", FCVAR_CHEAT,
	"Research: path cost multiplier for a step into a NAV_MESH_CLIFF area (1 = off)", true, 1.0f, false, 0.0f);
// NEO-HARNESS-TEMP research arm (2026-09-25, patch 91): AVOID on a ladder's foot only charges the short walk into it, so a
// risky ladder climbed up from its AVOID foot stayed cheap (skyline 1: 84 of 146 up-approaches timed out with the foot AVOID
// on); charge the climb itself when either end of the ladder is AVOID
ConVar neo_bot_path_avoid_ladder_ends("neo_bot_path_avoid_ladder_ends", "0", FCVAR_CHEAT,
	"Research: a ladder crossing that starts or ends in a NAV_MESH_AVOID area pays the AVOID multiplier");
// NEO-HARNESS-TEMP research arm (2026-10-07, patch 134): seconds a bot routes around a ladder after failing to climb it
// twice in a row (neo_bot_ladder_memory.h); 0 = off
ConVar neo_bot_ladder_fail_skip("neo_bot_ladder_fail_skip", "0", FCVAR_CHEAT,
	"Research: seconds a bot avoids a ladder whose climb it failed twice (0 = off)", true, 0.0f, false, 0.0f);
// a skipped ladder costs as if it were this long, so a ladder that is the only way stays usable
static constexpr float NEO_BOT_PATH_SKIPPED_LADDER_COST = 50000.0f;

// NEO-HARNESS-TEMP research arm (2026-10-07): seconds a bot routes around a ladder after its approach to it timed out
// twice at one height, without the bot getting on (saitama ladder 2's foot, crouched under the hatch); 0 = off
ConVar neo_bot_ladder_unreachable_skip("neo_bot_ladder_unreachable_skip", "0", FCVAR_CHEAT,
	"Research: seconds a bot avoids a ladder it twice failed to get onto (0 = off)", true, 0.0f, false, 0.0f);

float NeoLadderUnreachableSkipTime()
{
	return neo_bot_ladder_unreachable_skip.GetFloat();
}

void NeoNoteLadderUnreachable( CNEOBot *me, const CNavLadder *ladder, bool bGoingUp )
{
	const float flSkipTime = neo_bot_ladder_unreachable_skip.GetFloat();
	if ( flSkipTime <= 0.0f || !ladder || me->GetMoveType() == MOVETYPE_LADDER )
	{
		return;
	}

	const Vector &feet = me->GetLocomotionInterface()->GetFeet();
	if ( !me->GetLadderMemory()->OnClimbFailed( ladder, bGoingUp, feet.z, flSkipTime ) )
	{
		return;
	}

	if ( sv_neo_forensic_log.GetBool() )
	{
		Msg( "NEO_FORENSIC_LADDERBEH t=%.2f p=%d beh=skip dir=%s ladder=%d pos=%.0f,%.0f,%.0f reason=unreachable twice, routing around the ladder\n",
			gpGlobals->curtime, me->entindex(), bGoingUp ? "up" : "down", ladder->GetID(), feet.x, feet.y, feet.z );
	}

	const PathFollower *path = me->GetCurrentPath();
	if ( path )
	{
		const_cast< PathFollower * >( path )->Invalidate();
	}
}
ConVar neo_bot_path_ladder_crossing_cost("neo_bot_path_ladder_crossing_cost", "0", FCVAR_CHEAT,
	"Research: distance-equivalent cost added to every ladder crossing, for the risk of any climb (0 = off)", true, 0.0f, false, 0.0f);

// NEO-HARNESS-TEMP research arm (2026-09-24, patch 68): nav_generate keeps a space walkable where
// HumanCrouchHeight (55 u) of clearance exists, but a ducked Support is 59 u tall and a ducked
// Juggernaut 75 u, so a crouch passage can be one they do not fit through at all (ridgeline's
// catwalk railings: every bot stuck under them was a Support). With this on, a step into or out of
// a crouch area is closed to such a class when its ducked hull does not fit on the portal.
ConVar neo_bot_path_duck_clearance("neo_bot_path_duck_clearance", "0", FCVAR_CHEAT,
	"Research: close crouch-area portals too low for this bot's ducked hull (Support, Juggernaut)");

// NEO-HARNESS-TEMP research arm (2026-10-03, patch 130): patch 68 narrowed. A portal closes only when no lane
// across it holds the hull, tested a step above the floor as the bot moves (notes/phase3/support-crouch.md)
ConVar neo_bot_path_duck_lane("neo_bot_path_duck_lane", "1", FCVAR_CHEAT,
	"Research: close portals no lane of this bot's hull fits through. 1 = crouch-area portals, for a ducked hull "
	"taller than the mesh's crouch height; 2 = also such a bot's standing hull into other areas");
ConVar neo_bot_path_duck_lane_min("neo_bot_path_duck_lane_min", "1", FCVAR_CHEAT,
	"Research: clear lanes (4 u apart) a portal needs to stay open under neo_bot_path_duck_lane", true, 1.0f, false, 0.0f);
ConVar neo_bot_path_duck_lane_level("neo_bot_path_duck_lane_level", "1", FCVAR_CHEAT,
	"Research: neo_bot_path_duck_lane tests level portals only (rise within a step); climbs and drops stay open");
ConVar neo_bot_path_duck_lane_log("neo_bot_path_duck_lane_log", "0", FCVAR_CHEAT,
	"Harness: log NEO_DUCKLANE for each portal neo_bot_path_duck_lane tests");

ConVar neo_bot_path_penalty_exposure_base("neo_bot_path_penalty_exposure_base", "5.0", FCVAR_CHEAT,
	"General additional penalty per visible area for bots to avoid exposed areas", true, 0.0f, false, 0.0f);

ConVar neo_bot_path_penalty_exposure_pistol("neo_bot_path_penalty_exposure_pistol", "10.0", FCVAR_CHEAT,
	"Additional penalty per visible area for bots wielding pistol caliber weapons", true, 0.0f, false, 0.0f);

ConVar neo_bot_path_penalty_exposure_shotgun("neo_bot_path_penalty_exposure_shotgun", "20.0", FCVAR_CHEAT,
	"Additional penalty per visible area for shotgun-wielding bots", true, 0.0f, false, 0.0f);

ConVar neo_bot_path_penalty_exposure_inverse_base_battle_rifle("neo_bot_path_penalty_exposure_inverse_base_battle_rifle", "500.0", FCVAR_CHEAT,
	"Base penalty for calculating inverse traversal penalty for semi-auto battle rifles", true, 1.0f, false, 0.0f);

ConVar neo_bot_path_penalty_exposure_inverse_base_scoped("neo_bot_path_penalty_exposure_inverse_base_scoped", "1000.0", FCVAR_CHEAT,
	"Base penalty for calculating inverse traversal penalty for scoped weapons", true, 1.0f, false, 0.0f);

ConVar neo_bot_path_visibility_exposure_enable("neo_bot_path_visibility_exposure_enable", "1", FCVAR_NONE,
	"Enable visibility-exposure pathing penalties", true, 0, true, 1);

//-------------------------------------------------------------------------------------------------
CNEOBotPathCost::CNEOBotPathCost(CNEOBot* me, RouteType routeType)
{
	m_me = me;
	m_routeType = routeType;
	m_stepHeight = me->GetLocomotionInterface()->GetStepHeight();
	m_maxJumpHeight = me->GetLocomotionInterface()->GetMaxJumpHeight();
	m_maxDropHeight = me->GetLocomotionInterface()->GetDeathDropHeight();
	m_bIgnoreReservations = !neo_bot_path_reservation_enable.GetBool();
	m_bIgnoreHazards = (me->m_hCommandingPlayer.Get() != nullptr);
	m_bIgnoreVisibilityExposure = !neo_bot_path_visibility_exposure_enable.GetBool();
}

//-------------------------------------------------------------------------------------------------
// Only the world and static props: the answer below is cached for the map, so nothing that moves
// may decide it.
class CNeoStaticGeometryFilter : public CTraceFilter
{
public:
	virtual bool ShouldHitEntity( IHandleEntity *pHandleEntity, int contentsMask ) override
	{
		if ( staticpropmgr->IsStaticProp( pHandleEntity ) )
		{
			return true;
		}
		CBaseEntity *pEntity = EntityFromEntityHandle( pHandleEntity );
		return pEntity && pEntity->IsWorld();
	}
};

// Does this ducked hull fit standing on the portal between two adjacent areas? On a slope a hull
// rests on the highest floor under its footprint, so its base is the highest of the two areas'
// floors under the footprint's corners.
static bool NeoDuckHullFitsPortal( const CNavArea *from, const CNavArea *to, const Vector &vecMins, const Vector &vecMaxs )
{
	static CUtlMap<uint64, bool> s_cache( DefLessFunc( uint64 ) );
	static string_t s_mapName = NULL_STRING;
	if ( s_mapName != gpGlobals->mapname )
	{
		s_cache.RemoveAll();
		s_mapName = gpGlobals->mapname;
	}
	const uint64 key = ( (uint64)from->GetID() << 32 ) | ( (uint64)to->GetID() << 1 ) | ( vecMaxs.z > 70.0f ? 1 : 0 );
	const unsigned short idx = s_cache.Find( key );
	if ( idx != s_cache.InvalidIndex() )
	{
		return s_cache[idx];
	}

	bool bFits = true;
	for ( int d = 0; d < NUM_DIRECTIONS; ++d )
	{
		if ( !from->IsConnected( to, (NavDirType)d ) )
		{
			continue;
		}
		Vector center;
		float halfWidth;
		from->ComputePortal( to, (NavDirType)d, &center, &halfWidth );
		float zBase = MAX( from->GetZ( center ), to->GetZ( center ) );
		for ( int c = 0; c < 4; ++c )
		{
			const Vector corner( center.x + ( ( c & 1 ) ? vecMaxs.x : vecMins.x ), center.y + ( ( c & 2 ) ? vecMaxs.y : vecMins.y ), center.z );
			if ( to->IsOverlapping( corner ) )
			{
				zBase = MAX( zBase, to->GetZ( corner ) );
			}
			else if ( from->IsOverlapping( corner ) )
			{
				zBase = MAX( zBase, from->GetZ( corner ) );
			}
		}
		CNeoStaticGeometryFilter filter;
		trace_t tr;
		const Vector pos( center.x, center.y, zBase + 1.0f );
		UTIL_TraceHull( pos, pos, vecMins, vecMaxs, MASK_PLAYERSOLID, &filter, &tr );
		bFits = !tr.startsolid && !tr.allsolid;
		break;
	}
	s_cache.Insert( key, bFits );
	return bFits;
}

static constexpr float NEO_LANE_SPACING = 4.0f;		// between lanes across a portal
static constexpr float NEO_LANE_REACH_LEVEL = 2.0f;	// into each area on a level crossing
static constexpr float NEO_LANE_INSET = 0.5f;			// lane ends stay this far inside their area

// patch 130: a lane end inside the area, on the floor under the hull's footprint (the nav plane can sit
// a few units off it)
static Vector NeoLaneEnd( const CNavArea *area, float x, float y, const Vector &vecMins, const Vector &vecMaxs )
{
	const Vector &nw = area->GetCorner( NORTH_WEST );
	const Vector &se = area->GetCorner( SOUTH_EAST );
	const float flInsetX = MIN( NEO_LANE_INSET, ( se.x - nw.x ) / 2.0f );
	const float flInsetY = MIN( NEO_LANE_INSET, ( se.y - nw.y ) / 2.0f );
	Vector pos( clamp( x, nw.x + flInsetX, se.x - flInsetX ), clamp( y, nw.y + flInsetY, se.y - flInsetY ), 0.0f );
	pos.z = area->GetZ( pos.x, pos.y );

	CNeoStaticGeometryFilter filter;
	trace_t tr;
	UTIL_TraceHull( pos + Vector( 0, 0, StepHeight ), pos - Vector( 0, 0, StepHeight ), Vector( vecMins.x, vecMins.y, 0 ),
		Vector( vecMaxs.x, vecMaxs.y, 1.0f ), MASK_PLAYERSOLID, &filter, &tr );
	if ( !tr.startsolid && tr.fraction < 1.0f )
	{
		pos.z = tr.endpos.z;
	}

	return pos;
}

static bool s_bLaneVerbose = false;	// NEO-HARNESS-TEMP: neo_bot_path_duck_lane_dump with area ids

// patch 130: sweep the hull along the legs with its underside a step above the floor, as CGameMovement
// steps: lips and slopes under a step do not block it
static bool NeoLaneClear( const Vector *legs, int nLegs, const Vector &vecMins, const Vector &vecMaxs )
{
	const Vector mins( vecMins.x, vecMins.y, StepHeight );
	const Vector maxs( vecMaxs.x, vecMaxs.y, vecMaxs.z - vecMins.z );
	CNeoStaticGeometryFilter filter;
	for ( int i = 0; i + 1 < nLegs; ++i )
	{
		trace_t tr;
		UTIL_TraceHull( legs[i], legs[i + 1], mins, maxs, MASK_PLAYERSOLID, &filter, &tr );
		if ( tr.startsolid || tr.fraction < 1.0f )
		{
			if ( s_bLaneVerbose )
			{
				Msg( "NEO_DUCKLANE_LEG leg=%d of=%d from=%.0f,%.0f,%.0f to=%.0f,%.0f,%.0f solid=%d frac=%.2f hit=%s ent=%s\n", i, nLegs - 1,
					legs[i].x, legs[i].y, legs[i].z, legs[i + 1].x, legs[i + 1].y, legs[i + 1].z, tr.startsolid ? 1 : 0, tr.fraction,
					tr.surface.name ? tr.surface.name : "-", tr.m_pEnt ? tr.m_pEnt->GetClassname() : "-" );
			}
			return false;
		}
	}

	return true;
}

// patch 130: clear lanes, 4 u apart, across the portal from 'from' into 'to', a step deep each side. A climb
// rises a hull's width back from the edge, then moves over; a drop walks to the edge (legs: feet positions)
static int NeoCountHullLanes( const CNavArea *from, const CNavArea *to, const Vector &vecMins, const Vector &vecMaxs,
	int nEnough, int *pLanes )
{
	*pLanes = 0;
	NavDirType dir = NUM_DIRECTIONS;
	for ( int d = 0; d < NUM_DIRECTIONS; ++d )
	{
		if ( from->IsConnected( to, (NavDirType)d ) )
		{
			dir = (NavDirType)d;
			break;
		}
	}

	if ( dir == NUM_DIRECTIONS )
	{
		return 0;
	}

	Vector center;
	float flHalfWidth;
	from->ComputePortal( to, dir, &center, &flHalfWidth );
	Vector2D across( 0, 0 );
	DirectionToVector2D( dir, &across );
	const Vector2D along( across.y != 0.0f ? 1.0f : 0.0f, across.x != 0.0f ? 1.0f : 0.0f );
	const float flRise = from->ComputeAdjacentConnectionHeightChange( to );
	const bool bLevel = fabs( flRise ) <= StepHeight;
	const float flReach = ( bLevel || flRise < 0.0f ) ? NEO_LANE_REACH_LEVEL : vecMaxs.x + 1.0f;
	const float flSpan = MAX( 0.0f, flHalfWidth - NEO_LANE_INSET );
	const int nLanes = 1 + (int)( 2.0f * flSpan / NEO_LANE_SPACING );

	int nClear = 0;
	for ( int l = 0; l < nLanes && nClear < nEnough; ++l )
	{
		const float flOffset = ( nLanes == 1 ) ? 0.0f : -flSpan + 2.0f * flSpan * l / ( nLanes - 1 );
		const float x = center.x + along.x * flOffset;
		const float y = center.y + along.y * flOffset;
		const Vector a = NeoLaneEnd( from, x - across.x * flReach, y - across.y * flReach, vecMins, vecMaxs );
		Vector legs[4];
		int nLegs = 0;
		legs[nLegs++] = a;
		if ( flRise < -StepHeight )
		{
			// a drop needs room only to walk to the edge: the bot falls once its hull leaves the ledge
			legs[nLegs++] = Vector( x + across.x * flReach, y + across.y * flReach, a.z );
		}
		else
		{
			const Vector b = NeoLaneEnd( to, x + across.x * flReach, y + across.y * flReach, vecMins, vecMaxs );
			const float flTop = MAX( a.z, b.z );
			legs[nLegs++] = Vector( a.x, a.y, flTop );
			legs[nLegs++] = Vector( b.x, b.y, flTop );
			legs[nLegs++] = b;
		}

		*pLanes = l + 1;
		if ( NeoLaneClear( legs, nLegs, vecMins, vecMaxs ) )
		{
			++nClear;
		}
	}

	return nClear;
}

static bool NeoHullLaneOpen( const CNavArea *from, const CNavArea *to, const Vector &vecMins, const Vector &vecMaxs );

// the juggernaut's standing hull is taller than the HumanHeight nav_generate leaves room for
static bool NeoStandsAboveMesh( CNEOBot *me )
{
	return VEC_HULL_MAX_SCALED( me ).z - VEC_HULL_MIN_SCALED( me ).z > HumanHeight;
}

// patch 130: the hull this bot crosses the portal with, when the mesh does not promise room for it: its
// ducked hull on a crouch portal above HumanCrouchHeight; with mode 2 such a bot's standing hull elsewhere
static bool NeoUnpromisedHull( CNEOBot *me, const CNavArea *from, const CNavArea *to, Vector *pMins, Vector *pMaxs )
{
	const Vector vecDuckMins = VEC_DUCK_HULL_MIN_SCALED( me );
	const Vector vecDuckMaxs = VEC_DUCK_HULL_MAX_SCALED( me );
	if ( vecDuckMaxs.z - vecDuckMins.z <= HumanCrouchHeight )
	{
		return false;
	}

	if ( neo_bot_path_duck_lane_level.GetBool() && fabs( from->ComputeAdjacentConnectionHeightChange( to ) ) > StepHeight )
	{
		return false;
	}

	if ( from->HasAttributes( NAV_MESH_CROUCH ) || to->HasAttributes( NAV_MESH_CROUCH ) )
	{
		*pMins = vecDuckMins;
		*pMaxs = vecDuckMaxs;
		return true;
	}

	*pMins = VEC_HULL_MIN_SCALED( me );
	*pMaxs = VEC_HULL_MAX_SCALED( me );
	if ( NeoStandsAboveMesh( me ) )
	{
		// the juggernaut ducks where its standing hull has no lane (the locomotion does that),
		// so its ducked hull is the one that has to fit
		if ( NeoHullLaneOpen( from, to, *pMins, *pMaxs ) )
		{
			return false;
		}

		*pMins = vecDuckMins;
		*pMaxs = vecDuckMaxs;
		return true;
	}

	return neo_bot_path_duck_lane.GetInt() >= 2 && pMaxs->z - pMins->z <= HumanHeight;
}

bool NeoBotMustDuckThrough( CNEOBot *me, const CNavArea *from, const CNavArea *to )
{
	if ( !neo_bot_path_duck_lane.GetBool() || !NeoStandsAboveMesh( me ) || !from->IsConnected( to, NUM_DIRECTIONS ) )
	{
		return false;
	}

	if ( from->HasAttributes( NAV_MESH_CROUCH ) || to->HasAttributes( NAV_MESH_CROUCH ) )
	{
		return false;
	}

	if ( neo_bot_path_duck_lane_level.GetBool() && fabs( from->ComputeAdjacentConnectionHeightChange( to ) ) > StepHeight )
	{
		return false;
	}

	return !NeoHullLaneOpen( from, to, VEC_HULL_MIN_SCALED( me ), VEC_HULL_MAX_SCALED( me ) );
}

// patch 130: the lane traces see only the world and static props, so a verdict holds until the level changes
static CUtlMap<uint64, bool> s_hullLaneVerdicts( DefLessFunc( uint64 ) );

class CNeoHullLaneReset : public CAutoGameSystem
{
public:
	CNeoHullLaneReset() : CAutoGameSystem( "CNeoHullLaneReset" )
	{
	}

	virtual void LevelShutdownPostEntity() override
	{
		s_hullLaneVerdicts.RemoveAll();
	}
};

static CNeoHullLaneReset s_hullLaneReset;

// patch 130: is there a lane across this portal for the hull? Cached for the map: static geometry only
static bool NeoHullLaneOpen( const CNavArea *from, const CNavArea *to, const Vector &vecMins, const Vector &vecMaxs )
{
	const uint64 key = ( (uint64)from->GetID() << 32 ) | ( (uint64)to->GetID() << 8 ) | ( (int)( vecMaxs.z - vecMins.z ) & 0xFF );
	const unsigned short idx = s_hullLaneVerdicts.Find( key );
	if ( idx != s_hullLaneVerdicts.InvalidIndex() )
	{
		return s_hullLaneVerdicts[idx];
	}

	const bool bLog = neo_bot_path_duck_lane_log.GetBool();
	const int nMin = neo_bot_path_duck_lane_min.GetInt();
	int nLanes = 0;
	const int nClear = NeoCountHullLanes( from, to, vecMins, vecMaxs, bLog ? INT_MAX : nMin, &nLanes );
	const bool bOpen = nClear >= nMin;
	if ( bLog )
	{
		Msg( "NEO_DUCKLANE map=%s from=%u to=%u h=%.0f lanes=%d clear=%d open=%d crouch=%d,%d\n", STRING( gpGlobals->mapname ),
			from->GetID(), to->GetID(), vecMaxs.z - vecMins.z, nLanes, nClear, bOpen ? 1 : 0,
			from->HasAttributes( NAV_MESH_CROUCH ) ? 1 : 0, to->HasAttributes( NAV_MESH_CROUCH ) ? 1 : 0 );
	}

	s_hullLaneVerdicts.Insert( key, bOpen );
	return bOpen;
}

// NEO-HARNESS-TEMP (patch 130): every walk connection the rule would test for a Support (the base hull),
// with its lane count: crouch portals always, other portals under mode 2. Logged closed ones, or all with 'all'
CON_COMMAND_F( neo_bot_path_duck_lane_dump, "Harness: log the lanes neo_bot_path_duck_lane finds for a Support. [all]", FCVAR_CHEAT )
{
	if ( !UTIL_IsCommandIssuedByServerAdmin() )
	{
		return;
	}

	const bool bAll = args.ArgC() > 1 && !V_stricmp( args.Arg( 1 ), "all" );
	const unsigned int iFrom = ( args.ArgC() > 2 ) ? atoi( args.Arg( 1 ) ) : 0;
	const unsigned int iTo = ( args.ArgC() > 2 ) ? atoi( args.Arg( 2 ) ) : 0;
	const bool bStanding = neo_bot_path_duck_lane.GetInt() >= 2;
	int nTested[2] = { 0, 0 }, nClosed[2] = { 0, 0 };
	FOR_EACH_VEC( TheNavAreas, it )
	{
		const CNavArea *from = TheNavAreas[it];
		for ( int d = 0; d < NUM_DIRECTIONS; ++d )
		{
			for ( int i = 0; i < from->GetAdjacentCount( (NavDirType)d ); ++i )
			{
				const CNavArea *to = from->GetAdjacentArea( (NavDirType)d, i );
				const bool bCrouch = from->HasAttributes( NAV_MESH_CROUCH ) || to->HasAttributes( NAV_MESH_CROUCH );
				if ( iFrom && ( from->GetID() != iFrom || to->GetID() != iTo ) )
				{
					continue;
				}

				if ( !bCrouch && !bStanding && !iFrom )
				{
					continue;
				}

				s_bLaneVerbose = iFrom != 0;

				const Vector vecMins = bCrouch ? VEC_DUCK_HULL_MIN : VEC_HULL_MIN;
				const Vector vecMaxs = bCrouch ? VEC_DUCK_HULL_MAX : VEC_HULL_MAX;
				int nLanes = 0;
				const int nMin = neo_bot_path_duck_lane_min.GetInt();
				const int nClear = NeoCountHullLanes( from, to, vecMins, vecMaxs, ( bAll || bCrouch ) ? INT_MAX : nMin, &nLanes );
				const bool bOpen = nClear >= nMin;
				++nTested[bCrouch];
				nClosed[bCrouch] += bOpen ? 0 : 1;
				s_bLaneVerbose = false;
				if ( bAll || !bOpen || iFrom )
				{
					const int iPatch68 = bCrouch ? ( NeoDuckHullFitsPortal( from, to, vecMins, vecMaxs ) ? 1 : 0 ) : -1;
					Msg( "NEO_DUCKLANE map=%s from=%u to=%u h=%.0f lanes=%d clear=%d open=%d crouch=%d,%d rise=%.1f p68=%d\n",
						STRING( gpGlobals->mapname ), from->GetID(), to->GetID(), vecMaxs.z - vecMins.z, nLanes, nClear,
						bOpen ? 1 : 0, from->HasAttributes( NAV_MESH_CROUCH ) ? 1 : 0, to->HasAttributes( NAV_MESH_CROUCH ) ? 1 : 0,
						from->ComputeAdjacentConnectionHeightChange( to ), iPatch68 );
				}
			}
		}
	}

	Msg( "NEO_DUCKLANE_SUMMARY map=%s crouch_tested=%d crouch_closed=%d other_tested=%d other_closed=%d\n",
		STRING( gpGlobals->mapname ), nTested[1], nClosed[1], nTested[0], nClosed[0] );
}

//-------------------------------------------------------------------------------------------------
float CNEOBotPathCost::operator()(CNavArea* baseArea, CNavArea* fromArea, const CNavLadder* ladder, const CFuncElevator* elevator, float length) const
{
	VPROF_BUDGET("CNEOBotPathCost::operator()", "NextBot");

	CNavArea* area = (CNavArea*)baseArea;

	if (fromArea == NULL)
	{
		// first area in path, no cost
		return 0.0f;
	}

	if (!m_me->GetLocomotionInterface()->IsAreaTraversable(area))
	{
		return -1.0f;
	}

	// NEO-HARNESS-TEMP research arm (patch 68): see neo_bot_path_duck_clearance
	if ( !ladder && neo_bot_path_duck_clearance.GetBool()
		&& ( area->HasAttributes( NAV_MESH_CROUCH ) || fromArea->HasAttributes( NAV_MESH_CROUCH ) ) )
	{
		const Vector vecDuckMins = VEC_DUCK_HULL_MIN_SCALED( m_me );
		const Vector vecDuckMaxs = VEC_DUCK_HULL_MAX_SCALED( m_me );
		if ( vecDuckMaxs.z - vecDuckMins.z > HumanCrouchHeight && !NeoDuckHullFitsPortal( fromArea, area, vecDuckMins, vecDuckMaxs ) )
		{
			return -1.0f;
		}
	}

	// NEO-HARNESS-TEMP research arm (patch 130): see neo_bot_path_duck_lane
	Vector vecHullMins, vecHullMaxs;
	if ( !ladder && neo_bot_path_duck_lane.GetBool() && NeoUnpromisedHull( m_me, fromArea, area, &vecHullMins, &vecHullMaxs )
		&& !NeoHullLaneOpen( fromArea, area, vecHullMins, vecHullMaxs ) )
	{
		return -1.0f;
	}

	if ( !m_bIgnoreHazards && CNEOBotPathReservations()->IsAreaHazardous(area->GetID(), m_me) )
	{
		if ( m_routeType == DEFAULT_ROUTE )
		{
			// attempt to route around hazards
			// if blocked, assumes includeGoalIfPathFails=true
			// to partially path up to boundary of hazard area
			return -1.0f;
		}
	}

	// compute distance traveled along path so far
	float dist;

	if (ladder)
	{
		dist = ladder->m_length;

		// ladders leave bots exposed, but can be a shortcut
		const float ladderPenalty = neo_bot_path_penalty_ladder_multiplier.GetFloat();
		dist *= ladderPenalty;

		// patch 85: a climb can fail however short the ladder is
		dist += neo_bot_path_ladder_crossing_cost.GetFloat();

		// patch 134: a climb this bot failed twice lately
		const bool bGoingUp = ( area != ladder->m_bottomArea );
		if ( ( neo_bot_ladder_fail_skip.GetFloat() > 0.0f || neo_bot_ladder_unreachable_skip.GetFloat() > 0.0f )
			&& m_me->GetLadderMemory()->IsSkipped( ladder, bGoingUp ) )
		{
			dist += NEO_BOT_PATH_SKIPPED_LADDER_COST;
		}
	}
	else if (length > 0.0)
	{
		dist = length;
	}
	else
	{
		dist = (area->GetCenter() - fromArea->GetCenter()).Length();
	}

	// A ladder crossing pays for either end, so a ladder climbed up out of an AVOID area is as
	// expensive as one that ends in one
	if (area->HasAttributes(NAV_MESH_AVOID) || (ladder && fromArea->HasAttributes(NAV_MESH_AVOID)))
	{
		dist *= NEO_BOT_PATH_AVOID_MULTIPLIER;
	}

	// Ducking through a crouch space is slow and leaves a bot unable to fight. Recon is the only
	// class whose standing hull fits one, so every other class pays to enter. The cost stays
	// finite on purpose: where a crouch area is the only way through - a spawn door built to stop
	// the JGR class ducking under it, say - the bot still takes it.
	if (area->HasAttributes(NAV_MESH_CROUCH) && !fromArea->HasAttributes(NAV_MESH_CROUCH))
	{
		if (m_me->GetClass() != NEO_CLASS_RECON)
		{
			dist *= neo_bot_path_penalty_crouch_multiplier.GetFloat();
		}
	}

	// patch 85: marked risk - AVOID (a spot bots should not favour), CLIFF (a lethal fall beside it)
	if (area->HasAttributes(NAV_MESH_AVOID)
		|| ( ladder && neo_bot_path_avoid_ladder_ends.GetBool() && fromArea->HasAttributes(NAV_MESH_AVOID) ))
	{
		dist *= neo_bot_path_avoid_multiplier.GetFloat();
	}
	if (area->HasAttributes(NAV_MESH_CLIFF))
	{
		dist *= neo_bot_path_cliff_multiplier.GetFloat();
	}

	// Only apply height restrictions for non-ladder jump paths
	if (!ladder)
	{
		// check height change
		float deltaZ = fromArea->ComputeAdjacentConnectionHeightChange(area);

		if (deltaZ >= m_stepHeight)
		{
			if (deltaZ >= m_maxJumpHeight)
			{
				// too high to reach
				return -1.0f;
			}

			// jumping is slower than flat ground
			const float jumpPenalty = neo_bot_path_penalty_jump_multiplier.GetFloat() * Square( deltaZ / m_maxJumpHeight );
			dist *= jumpPenalty;
		}
		else if (deltaZ < -m_maxDropHeight)
		{
			// too far to drop
			return -1.0f;
		}
	}

	// add a random penalty unique to this character so they choose different routes to the same place
	float preference = 1.0f;

	if (m_routeType == DEFAULT_ROUTE)
	{
		// this term causes the same bot to choose different routes over time,
		// but keep the same route for a period in case of repaths
		int timeMod = (int)(gpGlobals->curtime / 10.0f) + 1;
		preference = 1.0f + 50.0f * (1.0f + FastCos((float)(m_me->GetEntity()->entindex() * area->GetID() * timeMod)));
	}

	if (m_routeType == SAFEST_ROUTE)
	{
		// misyl: combat areas.
#if 0
		// avoid combat areas
		if (area->IsInCombat())
		{
			const float combatDangerCost = 4.0f;
			dist *= combatDangerCost * area->GetCombatIntensity();
		}
#endif
	}


	float cost = (dist * preference);

	// NEO-HARNESS-TEMP research arm (2026-09-23, patch 50): physics props standing in an area make
	// crossing it slower (pushing, squeezing past), in proportion to how much of it they cover; finite,
	// so an area stays usable when it is the only way
	{
		extern ConVar neo_bot_path_prop_obstacle_cost;
		const float flPropFrac = CNEOBotPathReservations()->GetPropObstacleFraction( area->GetID() );
		if ( flPropFrac > 0.0f )
		{
			cost *= 1.0f + neo_bot_path_prop_obstacle_cost.GetFloat() * flPropFrac;
		}
	}

	// ------------------------------------------------------------------------------------------------
	// New path reservation related cost adjustments
	if ( !m_bIgnoreReservations && (m_routeType != FASTEST_ROUTE) )
	{
		cost += CNEOBotPathReservations()->GetAreaAvoidPenalty(area->GetID());

		if ( NEORules()->IsTeamplay() )
		{
			const int nFriendly = CNEOBotPathReservations()->GetPredictedFriendlyPathCount(area->GetID(), m_me->GetTeamNumber(), m_me);
			if (nFriendly > 0)
			{
				// Discourage team clustering: (n^2 * penalty)
				cost += nFriendly * nFriendly * neo_bot_path_reservation_penalty.GetFloat();
			}

			if (m_routeType == SAFEST_ROUTE)
			{
				// NEO Jank Cheat: Incorporate enemy bot paths so that we don't run directly into their line of fire
				// Intended for use by ghost carrier team, to emulate a team that knows where enemies are likely to ambush
				// Compensates for bots' lack of meta knowledge by making them prefer routes not reserved by enemies
				// Adheres to cheat against bots but not against humans philosophy by not considering human players' positions
				const int nEnemy = CNEOBotPathReservations()->GetPredictedFriendlyPathCount(area->GetID(), GetEnemyTeam(m_me->GetTeamNumber()));
				cost += nEnemy * neo_bot_path_reservation_penalty.GetFloat() * 2.0f;
			}
		}
	}

	if ( !m_bIgnoreVisibilityExposure && (m_routeType != FASTEST_ROUTE) )
	{
		// Weapon range penalties
		auto* myWeapon = assert_cast<CNEOBaseCombatWeapon*>(m_me->GetActiveWeapon());
		if (myWeapon)
		{
			const int nWeaponBits = myWeapon->GetNeoWepBits();
			if (nWeaponBits & NEO_WEP_FIREARM)
			{
				const int visibleAreaCount = area->GetPotentiallyVisibleAreaCount();
				if (visibleAreaCount > 0)
				{
					constexpr int nShotgunBits = NEO_WEP_AA13 | NEO_WEP_SUPA7;
					constexpr int nBattleRifleBits = NEO_WEP_M41 | NEO_WEP_M41_S;
					constexpr int nPistolCaliberBits = NEO_WEP_MILSO | NEO_WEP_TACHI | NEO_WEP_KYLA
						| NEO_WEP_MPN | NEO_WEP_MPN_S | NEO_WEP_JITTE | NEO_WEP_JITTE_S | NEO_WEP_SRM | NEO_WEP_SRM_S;

					if (nWeaponBits & nPistolCaliberBits)
					{
						// Weapons that don't have max first shot accuracy
						const float exposurePenalty = neo_bot_path_penalty_exposure_pistol.GetFloat();
						cost += visibleAreaCount * exposurePenalty;
					}
					else if (nWeaponBits & nShotgunBits)
					{
						// Weapons that have spread that can't hit long range targets
						const float exposurePenalty = neo_bot_path_penalty_exposure_shotgun.GetFloat();
						cost += visibleAreaCount * exposurePenalty;
					}
					else if (nWeaponBits & nBattleRifleBits)
					{
						// Weapons that benefit from medium sightlines that can see many NavAreas
						const float baseline_penalty = neo_bot_path_penalty_exposure_inverse_base_battle_rifle.GetFloat();
						cost += baseline_penalty / visibleAreaCount;
					}
					else if (nWeaponBits & NEO_WEP_SCOPEDWEAPON)
					{
						// Weapons that benefit from long sightlines that can see many NavAreas
						const float baseline_penalty = neo_bot_path_penalty_exposure_inverse_base_scoped.GetFloat();
						cost += baseline_penalty / visibleAreaCount;
					}
					else
					{
						// Generally avoiding exposed areas when traversing a wide open area
						const float exposurePenalty = neo_bot_path_penalty_exposure_base.GetFloat();
						cost += visibleAreaCount * exposurePenalty;
					}
				}
			}
		}
	}
	// ------------------------------------------------------------------------------------------------

	// NEO-HARNESS-TEMP research arm (2026-09-23): a crossing bots recently got stuck making costs more,
	// for every route type - it is about whether the way is passable, not about tactics
	cost += CNEOBotPathReservations()->GetCrossingAvoidPenalty( fromArea->GetID(), area->GetID() );
	cost += CNEOBotPathReservations()->GetDoorCrossingPenalty( fromArea->GetID(), area->GetID(), m_me->GetTeamNumber() );

	if (area->HasAttributes(NAV_MESH_FUNC_COST))
	{
		cost *= area->ComputeFuncNavCost(m_me);
		DebuggerBreakOnNaN_StagingOnly(cost);
	}

	return cost + fromArea->GetCostSoFar();
}
