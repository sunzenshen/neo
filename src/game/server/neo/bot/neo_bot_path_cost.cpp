#include "cbase.h"
#include "neo_bot.h"
#include "neo_bot_path_cost.h"
#include "neo_gamerules.h"
#include "neo_bot_locomotion.h"
#include "nav_mesh.h"
#include "neo_bot_path_reservation.h"

ConVar neo_bot_path_penalty_avoid_multiplier("neo_bot_path_penalty_avoid_multiplier", "20.0", FCVAR_CHEAT,
	"Penalty multiplier for navmesh avoid areas in pathfinding", true, 1.0f, false, 0.0f);

ConVar neo_bot_path_penalty_jump_multiplier("neo_bot_path_penalty_jump_multiplier", "1000.0", FCVAR_CHEAT,
	"Maximum penalty multiplier for jump height changes in pathfinding", true, 0.01f, false, 0.0f);

ConVar neo_bot_path_penalty_ladder_multiplier("neo_bot_path_penalty_ladder_multiplier", "3.0", FCVAR_CHEAT,
	"Penalty multiplier for ladder traversal in pathfinding", true, 0.1f, false, 0.0f);

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

ConVar neo_bot_research_exposure_dir( "neo_bot_research_exposure_dir", "0", FCVAR_CHEAT,
	"Research: count an area's exposure only from the directions a bot entering it faces. "
	"0 = every direction, 1 = all but the 90 degrees behind, 2 = the half ahead, 3 = the 90 degrees ahead", true, 0, true, 3 );

ConVar neo_bot_research_team_arm( "neo_bot_research_team_arm", "0", FCVAR_CHEAT,
	"Research: 0 = research behaviors apply to every bot, 2 or 3 = only to that team's bots", true, 0, true, 3 );

ConVar neo_bot_path_penalty_hurry_hazard_multiplier("neo_bot_path_penalty_hurry_hazard_multiplier", "20.0", FCVAR_CHEAT,
	"Cost multiplier for a hurrying bot crossing a hazard area that is not deadly, such as smoke, which it takes only when no shorter detour exists", true, 1.0f, false, 0.0f);

// Research behaviors run for every bot, or only for the bots of the team under test
static bool NeoResearchArmApplies( const CNEOBot *me )
{
	const int teamArm = neo_bot_research_team_arm.GetInt();
	return teamArm == 0 || me->GetTeamNumber() == teamArm;
}

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
	m_exposureDirMode = NeoResearchArmApplies( me ) ? neo_bot_research_exposure_dir.GetInt() : 0;
	m_bInAHurry = ( me->GetIntentionInterface()->ShouldHurry( me ) == ANSWER_YES );
	m_vecDuckHullMins = VEC_DUCK_HULL_MIN_SCALED( me );
	m_vecDuckHullMaxs = VEC_DUCK_HULL_MAX_SCALED( me );
	m_bDucksAboveCrouchHeight = ( m_vecDuckHullMaxs.z - m_vecDuckHullMins.z ) > HumanCrouchHeight;
}

//-------------------------------------------------------------------------------------------------
// A crouch area promises HumanCrouchHeight of clearance, but Support and Juggernaut ducked hulls are taller,
// so some crouch gaps in the mesh are ones they cannot get through

static constexpr float NEO_DUCK_LANE_SPACING = 4.0f;	// one lane per this much portal width, edge to edge
static constexpr float NEO_DUCK_LANE_REACH = 2.0f;	// how far a lane runs into each area
static constexpr float NEO_DUCK_LANE_INSET = 0.5f;	// lane ends keep this far inside their area

// A crouch portal and a ducked hull height: the key of one lane verdict
struct NeoDuckLaneKey_t
{
	unsigned int m_fromId;
	unsigned int m_toId;
	int m_height;

	bool operator<( const NeoDuckLaneKey_t &other ) const
	{
		if ( m_fromId != other.m_fromId )
		{
			return m_fromId < other.m_fromId;
		}

		if ( m_toId != other.m_toId )
		{
			return m_toId < other.m_toId;
		}

		return m_height < other.m_height;
	}
};

// The lane traces see only the world and static props, so a verdict holds while the navmesh does
static CUtlMap<NeoDuckLaneKey_t, bool> s_duckLaneVerdicts( DefLessFunc( NeoDuckLaneKey_t ) );

// nav_load and nav editing commands renumber areas without a level change,
// so the verdicts are dropped when the area count they were made on changes
static unsigned int s_nDuckLaneAreaCount = 0;

class CNeoDuckLaneReset : public CAutoGameSystem
{
public:
	CNeoDuckLaneReset() : CAutoGameSystem( "CNeoDuckLaneReset" )
	{
	}

	virtual void LevelShutdownPostEntity() override
	{
		s_duckLaneVerdicts.RemoveAll();
	}
};

static CNeoDuckLaneReset s_duckLaneReset;

// The point of the area nearest (x, y), on the floor under the hull's footprint,
// which can lie slightly off the nav plane
static Vector NeoLaneEnd( const CNavArea *area, float x, float y, const Vector &vecMins, const Vector &vecMaxs )
{
	const Vector nw = area->GetCorner( NORTH_WEST );
	const Vector se = area->GetCorner( SOUTH_EAST );
	const float flInsetX = Min( NEO_DUCK_LANE_INSET, ( se.x - nw.x ) / 2.0f );
	const float flInsetY = Min( NEO_DUCK_LANE_INSET, ( se.y - nw.y ) / 2.0f );
	Vector pos( clamp( x, nw.x + flInsetX, se.x - flInsetX ), clamp( y, nw.y + flInsetY, se.y - flInsetY ), 0.0f );
	pos.z = area->GetZ( pos.x, pos.y );

	// A footprint-sized slab dropped from above the nav plane to below it
	CTraceFilterWorldAndPropsOnly filter;
	trace_t tr;
	UTIL_TraceHull( pos + Vector( 0.0f, 0.0f, StepHeight ), pos - Vector( 0.0f, 0.0f, StepHeight ),
		Vector( vecMins.x, vecMins.y, 0.0f ), Vector( vecMaxs.x, vecMaxs.y, 1.0f ), MASK_PLAYERSOLID, &filter, &tr );
	if ( !tr.startsolid && tr.fraction < 1.0f )
	{
		pos.z = tr.endpos.z;
	}

	return pos;
}

// Can the hull get from a to b? It is swept with its underside a step above the floor,
// so it ignores a lip or slope lower than a step, as the bot steps over it
static bool NeoLaneClear( const Vector &a, const Vector &b, const Vector &vecMins, const Vector &vecMaxs )
{
	const Vector mins( vecMins.x, vecMins.y, StepHeight );
	const Vector maxs( vecMaxs.x, vecMaxs.y, vecMaxs.z - vecMins.z );
	const float flTop = Max( a.z, b.z );
	const Vector legs[] = { a, Vector( a.x, a.y, flTop ), Vector( b.x, b.y, flTop ), b };
	constexpr int nLegs = ARRAYSIZE( legs );

	CTraceFilterWorldAndPropsOnly filter;
	trace_t tr;
	for ( int i = 0; i + 1 < nLegs; ++i )
	{
		UTIL_TraceHull( legs[i], legs[i + 1], mins, maxs, MASK_PLAYERSOLID, &filter, &tr );
		if ( tr.startsolid || tr.fraction < 1.0f )
		{
			return false;
		}
	}

	return true;
}

// Does the hull fit through the portal from 'from' into 'to' on at least one lane?
// Lanes run across the portal, each from just inside one area to just inside the other
static bool NeoDuckLaneFits( const CNavArea *from, const CNavArea *to, const Vector &vecMins, const Vector &vecMaxs )
{
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
		return true;
	}

	Vector center;
	float flHalfWidth;
	from->ComputePortal( to, dir, &center, &flHalfWidth );

	Vector2D across;
	DirectionToVector2D( dir, &across );
	const Vector2D along( across.y != 0.0f ? 1.0f : 0.0f, across.x != 0.0f ? 1.0f : 0.0f );
	const float flSpan = Max( 0.0f, flHalfWidth - NEO_DUCK_LANE_INSET );
	const int nLanes = 1 + (int)( 2.0f * flSpan / NEO_DUCK_LANE_SPACING );
	for ( int i = 0; i < nLanes; ++i )
	{
		const float flOffset = ( nLanes == 1 ) ? 0.0f : -flSpan + 2.0f * flSpan * i / ( nLanes - 1 );
		const float x = center.x + along.x * flOffset;
		const float y = center.y + along.y * flOffset;
		const float dx = across.x * NEO_DUCK_LANE_REACH;
		const float dy = across.y * NEO_DUCK_LANE_REACH;
		const Vector a = NeoLaneEnd( from, x - dx, y - dy, vecMins, vecMaxs );
		const Vector b = NeoLaneEnd( to, x + dx, y + dy, vecMins, vecMaxs );
		if ( NeoLaneClear( a, b, vecMins, vecMaxs ) )
		{
			return true;
		}
	}

	return false;
}

// Is a level step into or out of a crouch area open to this ducked hull? Climbs and drops are not tested
static bool NeoDuckLaneOpen( const CNavArea *from, const CNavArea *to, const Vector &vecMins, const Vector &vecMaxs,
	float flStepHeight )
{
	if ( !from->HasAttributes( NAV_MESH_CROUCH ) && !to->HasAttributes( NAV_MESH_CROUCH ) )
	{
		return true;
	}

	if ( fabs( from->ComputeAdjacentConnectionHeightChange( to ) ) > flStepHeight )
	{
		return true;
	}

	if ( TheNavMesh->GetNavAreaCount() != s_nDuckLaneAreaCount )
	{
		s_duckLaneVerdicts.RemoveAll();
		s_nDuckLaneAreaCount = TheNavMesh->GetNavAreaCount();
	}

	const float flHeight = vecMaxs.z - vecMins.z;
	const NeoDuckLaneKey_t key = { from->GetID(), to->GetID(), (int)flHeight };
	const auto idx = s_duckLaneVerdicts.Find( key );
	if ( idx != s_duckLaneVerdicts.InvalidIndex() )
	{
		return s_duckLaneVerdicts[idx];
	}

	const bool bOpen = NeoDuckLaneFits( from, to, vecMins, vecMaxs );
	s_duckLaneVerdicts.Insert( key, bOpen );
	return bOpen;
}

//-------------------------------------------------------------------------------------------------
// Direction-aware exposure: a bot walking into an area faces the way it travels,
// so the areas behind it are not where it expects to be shot from

enum NeoExposureDirMode
{
	NEO_EXPOSURE_DIR_OFF = 0,
	NEO_EXPOSURE_DIR_NOT_BEHIND,
	NEO_EXPOSURE_DIR_HALF_AHEAD,
	NEO_EXPOSURE_DIR_CONE_AHEAD,

	NEO_EXPOSURE_DIR_MODE_COUNT
};

// Weight of each compass sector by its offset from the sector ahead (offset 4 is straight behind);
// a half weight takes the half of a sector that lies inside the counted span
static constexpr float NEO_EXPOSURE_SECTOR_WEIGHTS[NEO_EXPOSURE_DIR_MODE_COUNT][NAV_VISIBLE_SECTOR_COUNT] =
{
	{ 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
	{ 1.0f, 1.0f, 1.0f, 0.5f, 0.0f, 0.5f, 1.0f, 1.0f },
	{ 1.0f, 1.0f, 0.5f, 0.0f, 0.0f, 0.0f, 0.5f, 1.0f },
	{ 1.0f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.5f },
};

// The visible-area count seen from the counted span around one sector, scaled up by the share of the
// compass that span covers, so averaged over every direction of travel it matches the plain count
static float NeoSectorExposure( const CNavArea *area, int aheadSector, int mode )
{
	const float *weights = NEO_EXPOSURE_SECTOR_WEIGHTS[mode];
	float tally = 0.0f;
	float weightSum = 0.0f;
	for ( int offset = 0; offset < NAV_VISIBLE_SECTOR_COUNT; ++offset )
	{
		const int sector = ( aheadSector + offset ) % NAV_VISIBLE_SECTOR_COUNT;
		tally += weights[offset] * area->GetVisibleSectorTally( sector );
		weightSum += weights[offset];
	}

	// at least one area, as the plain count is, so the inverse penalties stay bounded
	return Max( 1.0f, tally * NAV_VISIBLE_SECTOR_COUNT / weightSum );
}

// The exposure of a step from fromArea into area; a step with no sideways travel, such as a straight
// drop or a ladder climb, keeps the plain count
static float NeoDirectionalExposure( const CNavArea *area, const CNavArea *fromArea, int mode )
{
	const float dx = area->GetCenter().x - fromArea->GetCenter().x;
	const float dy = area->GetCenter().y - fromArea->GetCenter().y;
	constexpr float flMinTravelSqr = 1.0f;
	if ( mode == NEO_EXPOSURE_DIR_OFF || dx * dx + dy * dy < flMinTravelSqr )
	{
		return (float)area->GetPotentiallyVisibleAreaCount();
	}

	return NeoSectorExposure( area, NavVisibleSector( dx, dy ), mode );
}

// Prints the direction tallies of one nav area, to check them against the map
CON_COMMAND_F( neo_bot_research_exposure_dump, "Research: print the visible-area tallies by direction of the nav area "
	"with the given ID, or nearest the given x y z, or nearest the calling player", FCVAR_GAMEDLL | FCVAR_CHEAT )
{
	if ( !UTIL_IsCommandIssuedByServerAdmin() )
	{
		return;
	}

	const CNavArea *area = nullptr;
	if ( args.ArgC() == 2 )
	{
		area = TheNavMesh->GetNavAreaByID( V_atoi( args[1] ) );
	}
	else if ( args.ArgC() == 4 )
	{
		const Vector pos( V_atof( args[1] ), V_atof( args[2] ), V_atof( args[3] ) );
		area = TheNavMesh->GetNearestNavArea( pos );
	}
	else if ( CBasePlayer *player = UTIL_GetCommandClient() )
	{
		area = TheNavMesh->GetNearestNavArea( player->GetAbsOrigin() );
	}

	if ( !area )
	{
		Msg( "neo_bot_research_exposure_dump: no nav area found\n" );
		return;
	}

	// sector 0 is +x, counting toward +y
	static const char *const sectorNames[NAV_VISIBLE_SECTOR_COUNT] = { "+x", "+x+y", "+y", "-x+y", "-x", "-x-y", "-y", "+x-y" };

	const Vector &center = area->GetCenter();
	Msg( "EXPOSURE_DUMP area %u at (%.0f %.0f %.0f) visible %d\n", area->GetID(), center.x, center.y, center.z,
		area->GetPotentiallyVisibleAreaCount() );
	for ( int sector = 0; sector < NAV_VISIBLE_SECTOR_COUNT; ++sector )
	{
		Msg( "EXPOSURE_DUMP  %-5s tally %7.1f | walking %-5s: not_behind %7.1f half_ahead %7.1f cone_ahead %7.1f\n",
			sectorNames[sector], area->GetVisibleSectorTally( sector ), sectorNames[sector],
			NeoSectorExposure( area, sector, NEO_EXPOSURE_DIR_NOT_BEHIND ),
			NeoSectorExposure( area, sector, NEO_EXPOSURE_DIR_HALF_AHEAD ),
			NeoSectorExposure( area, sector, NEO_EXPOSURE_DIR_CONE_AHEAD ) );
	}
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

	// Support and Juggernaut duck too tall for some crouch gaps
	if ( m_bDucksAboveCrouchHeight && !ladder && !elevator
		&& !NeoDuckLaneOpen( fromArea, area, m_vecDuckHullMins, m_vecDuckHullMaxs, m_stepHeight ) )
	{
		return -1.0f;
	}

	float hazardMultiplier = 1.0f;
	if ( !m_bIgnoreHazards && ( m_routeType == DEFAULT_ROUTE ) && CNEOBotPathReservations()->IsAreaHazardous(area->GetID(), m_me) )
	{
		// a hurrying bot pushes through a hazard that is not deadly, such as smoke, when no detour exists,
		// instead of stalling at its edge, but deadly hazards such as frag grenades stay impassable
		if ( m_bInAHurry && !CNEOBotPathReservations()->IsAreaDeadlyHazard(area->GetID(), m_me) )
		{
			hazardMultiplier = neo_bot_path_penalty_hurry_hazard_multiplier.GetFloat();
		}
		else
		{
			// attempt to route around hazards
			// if blocked, assumes includeGoalIfPathFails=true
			// to partially path up to boundary of hazard area
			return -1.0f;
		}

		// an enemy detpack's blast is lethal on the fastest and safest routes too,
		// but a bot already inside the blast radius may still walk out through it
		const bool bDetpackRoute = ( m_routeType == FASTEST_ROUTE ) || ( m_routeType == SAFEST_ROUTE );
		const int iTeam = m_me->GetTeamNumber();
		if ( bDetpackRoute && CNEOBotPathReservations()->IsAreaDetpackHazard( area->GetID(), iTeam )
			&& !CNEOBotPathReservations()->IsAreaDetpackHazard( fromArea->GetID(), iTeam ) )
		{
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
	}
	else if (length > 0.0)
	{
		dist = length;
	}
	else
	{
		dist = (area->GetCenter() - fromArea->GetCenter()).Length();
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

	// ------------------------------------------------------------------------------------------------
	// New path reservation related cost adjustments
	if ( !m_bIgnoreReservations && (m_routeType != FASTEST_ROUTE) )
	{
		cost += CNEOBotPathReservations()->GetAreaPenalty(area->GetID()).GetPathCost();

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
				if (area->GetPotentiallyVisibleAreaCount() > 0)
				{
					const float visibleAreaCount = NeoDirectionalExposure( area, fromArea, m_exposureDirMode );

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

	if (area->HasAttributes(NAV_MESH_AVOID) || (ladder && fromArea->HasAttributes(NAV_MESH_AVOID)))
	{
		cost *= neo_bot_path_penalty_avoid_multiplier.GetFloat();
	}

	cost *= hazardMultiplier;
	// ------------------------------------------------------------------------------------------------

	if (area->HasAttributes(NAV_MESH_FUNC_COST))
	{
		cost *= area->ComputeFuncNavCost(m_me);
		DebuggerBreakOnNaN_StagingOnly(cost);
	}

	return cost + fromArea->GetCostSoFar();
}
