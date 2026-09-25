#include "cbase.h"
#include "fmtstr.h"

#include "neo_gamerules.h"
#include "nav_ladder.h"
#include "NextBot/NavMeshEntities/func_nav_prerequisite.h"

#include "bot/neo_bot.h"
#include "bot/neo_bot_manager.h"
#include "bot/neo_bot_path_reservation.h"

#include "bot/behavior/neo_bot_detpack_trigger.h"
#include "bot/behavior/neo_bot_tactical_monitor.h"
#include "bot/behavior/neo_bot_scenario_monitor.h"

#include "bot/behavior/neo_bot_seek_and_destroy.h"
#include "bot/behavior/neo_bot_seek_weapon.h"
#include "bot/behavior/neo_bot_retreat_to_cover.h"
#include "bot/behavior/neo_bot_retreat_from_grenade.h"
#include "bot/behavior/neo_bot_retreat_from_hazard_area.h"
#include "bot/behavior/neo_bot_ladder_approach.h"
#include "bot/behavior/neo_bot_ladder_climb.h"
#include "bot/behavior/neo_bot_path_clear_breakable.h"
#include "bot/behavior/neo_bot_pause.h"
#if 0 // NEO TODO (Adam) Fix picking up weapons, search for dropped weapons to pick up ammo
#include "bot/behavior/neo_bot_get_ammo.h"
#endif
#include "bot/behavior/nav_entities/neo_bot_nav_ent_destroy_entity.h"
#include "bot/behavior/nav_entities/neo_bot_nav_ent_move_to.h"
#include "bot/behavior/nav_entities/neo_bot_nav_ent_wait.h"
#include "neo/neo_player_shared.h"
#include "neo_detpack.h"
#include "weapon_detpack.h"
#include "weapon_ghost.h"
#include "nav_mesh.h"

ConVar neo_bot_force_jump( "neo_bot_force_jump", "0", FCVAR_CHEAT, "Force bots to continuously jump" );

ConVar neo_bot_scavenge_upgrade_delay( "neo_bot_scavenge_upgrade_delay", "4", FCVAR_GAMEDLL,
	"Delay in seconds between checking for a better weapon if the bot already has a primary weapon.", true, 1, false, 0 );

////////////////////////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////////////////////////
// Attempts to kick/despawn the bot in the Update()

class CNEODespawn : public Action< CNEOBot >
{
public:
	virtual ActionResult< CNEOBot >	Update( CNEOBot* me, float interval );
	virtual const char* GetName( void ) const { return "Despawn"; };
};


ActionResult< CNEOBot > CNEODespawn::Update( CNEOBot* me, float interval )
{
	// players need to be kicked, not deleted
	if ( me->GetEntity()->IsPlayer() )
	{
		CBasePlayer* player = static_cast< CBasePlayer* >( me->GetEntity() );
		engine->ServerCommand( UTIL_VarArgs( "kickid %d\n", player->GetUserID() ) );
	}
	else
	{
		UTIL_Remove( me->GetEntity() );
	}
	return Continue();
}

////////////////////////////////////////////////////////////////////////////////////////////////////

CNEOBotTacticalMonitor::CNEOBotTacticalMonitor()
{
	m_pIgnoredWeapons = std::make_unique<CNEOIgnoredWeaponsCache>();
}

CNEOBotTacticalMonitor::~CNEOBotTacticalMonitor() = default;


Action< CNEOBot > *CNEOBotTacticalMonitor::InitialContainedAction( CNEOBot *me )
{
	return new CNEOBotScenarioMonitor;
}


//-----------------------------------------------------------------------------------------
ActionResult< CNEOBot >	CNEOBotTacticalMonitor::OnStart( CNEOBot *me, Action< CNEOBot > *priorAction )
{
	m_pIgnoredWeapons->Reset();
	m_hazardCheckTimer.Start( 0.5f );
	return Continue();
}


//-----------------------------------------------------------------------------------------
ActionResult< CNEOBot > CNEOBotTacticalMonitor::MonitorArmedDetpack( CNEOBot *me )
{
	if ( !m_detpackCheckTimer.IsElapsed() )
	{
		return Continue();
	}

	CWeaponDetpack *pDetWeapon = assert_cast<CWeaponDetpack*>( me->Weapon_OwnsThisType( "weapon_remotedet" ) );
	if ( !pDetWeapon || pDetWeapon->m_bRemoteHasBeenTriggered )
	{
		// Longer delay assuming that a detpack pickup would be rare
		// Players usually don't realize they picked up a detpack anyway
		m_detpackCheckTimer.Start( 10.0f );
		return Continue();
	}

	if ( !pDetWeapon->m_bThisDetpackHasBeenThrown )
	{
		// Based on GetArmingTime plus need to get away
		m_detpackCheckTimer.Start( 3.0f );
		return Continue();
	}
	m_detpackCheckTimer.Start( 0.5f );

	CBaseEntity *pDetpackEnt = pDetWeapon->GetDetpackEntity();
	if ( !pDetpackEnt )
	{
		return Continue();
	}

	const Vector& vecDetpackPos = pDetpackEnt->GetAbsOrigin();

	// Check if I am too close to the detpack
	if ( me->GetAbsOrigin().DistToSqr( vecDetpackPos ) <= Square( NEO_DETPACK_DAMAGE_RADIUS ) )
	{
		return Continue();
	}

	float flThresholdMultiplier;
	switch ( me->GetDifficulty() )
	{
	case CNEOBot::EASY:
		flThresholdMultiplier = 1.05f;
		break;
	case CNEOBot::NORMAL:
		flThresholdMultiplier = 0.95f;
		break;
	case CNEOBot::HARD:
		flThresholdMultiplier = 0.85f;
		break;
	case CNEOBot::EXPERT:
		flThresholdMultiplier = 0.75f;
		break;
	default:
		flThresholdMultiplier = 0.95f;
		break;
	}

	const float flMaxRadiusSq = Square( NEO_DETPACK_DAMAGE_RADIUS * flThresholdMultiplier );
	bool bShouldDetonate = false;

	// Check if any known threat or teammate is in range
	CUtlVector< CKnownEntity > knownVector;
	me->GetVisionInterface()->CollectKnownEntities( &knownVector );
	bool bIsTeamplay = NEORules()->IsTeamplay();

	for ( int i = 0; i < knownVector.Count(); ++i )
	{
		if ( knownVector[i].IsObsolete() )
		{
			continue;
		}

		CBaseEntity *pEntity = knownVector[i].GetEntity();
		if ( !pEntity )
		{
			continue;
		}

		if ( vecDetpackPos.DistToSqr( knownVector[i].GetLastKnownPosition() ) <= flMaxRadiusSq )
		{
			if ( bIsTeamplay && me->InSameTeam( pEntity ) )
			{
				// Teammate in blast radius
				return Continue();
			}

			// Enemy in range.
			bShouldDetonate = true;
		}
	}

	// Check if ghost carrier is in range
	if ( !bShouldDetonate )
	{
		if ( CWeaponGhost *pGhost = NEORules()->m_pGhost )
		{
			CBaseCombatCharacter *pGhostOwner = pGhost->GetOwner();
			if ( pGhostOwner && !me->InSameTeam( pGhostOwner ) )
			{
				if ( vecDetpackPos.DistToSqr( pGhostOwner->GetAbsOrigin() ) <= flMaxRadiusSq )
				{
					bShouldDetonate = true;
				}
			}
		}
	}

	if ( bShouldDetonate )
	{
		return SuspendFor( new CNEOBotDetpackTrigger(), "Triggering detpack!" );
	}

	return Continue();
}


//-----------------------------------------------------------------------------------------
// NEO-HARNESS-TEMP research arm (2026-09-24, patch 60). Players never collide in NEO (CNEORules::ShouldCollide), so
// this reflex only spaces bots out - and when several spawn on the same point (30-bot matches: a dozen per team
// stacked on one spawn) the "away" vector is zero, every stacked bot drops its movement keys every tick, and the
// team stands still until the stuck monitor jumps it free ~4.5 s later.
// 1 = stacked bots step off in a direction of their own; 2 = no avoidance at all.
ConVar neo_bot_avoid_friends( "neo_bot_avoid_friends", "0", FCVAR_CHEAT,
	"Research: 0 = stock AvoidBumpingFriends, 1 = stacked bots separate along a per-bot direction, 2 = off" );

extern bool NeoBotOnPreciseArea( CNEOBot *me );

void CNEOBotTacticalMonitor::AvoidBumpingFriends( CNEOBot *me )
{
	const float avoidRange = 32.0f;

	if ( neo_bot_avoid_friends.GetInt() >= 2 )
	{
		return;
	}

	// patch 64: stepping straight away from a teammate on a PRECISE area (a ledge) can step off it
	if ( NeoBotOnPreciseArea( me ) )
	{
		return;
	}

	if ( !NEORules()->IsTeamplay() )
	{
		return;
	}

	CUtlVector< CNEO_Player * > friendVector;
	CollectPlayers( &friendVector, me->GetTeamNumber(), COLLECT_ONLY_LIVING_PLAYERS );

	CNEO_Player *closestFriend = NULL;
	float closestRangeSq = avoidRange * avoidRange;

	for( int i=0; i<friendVector.Count(); ++i )
	{
		CNEO_Player *friendly = friendVector[i];

		if ( friendly == me->GetEntity() )
			continue;

		float rangeSq = ( friendly->GetAbsOrigin() - me->GetAbsOrigin() ).LengthSqr();
		if ( rangeSq < closestRangeSq )
		{
			closestFriend = friendly;
			closestRangeSq = rangeSq;
		}
	}

	if ( !closestFriend )
		return;

	// avoid unless hindrance returns a definitive "no"
	if ( me->GetIntentionInterface()->IsHindrance( me, closestFriend ) == ANSWER_UNDEFINED )
	{
		me->ReleaseForwardButton();
		me->ReleaseLeftButton();
		me->ReleaseRightButton();
		me->ReleaseBackwardButton();

		Vector away = me->GetAbsOrigin() - closestFriend->GetAbsOrigin();
		if ( neo_bot_avoid_friends.GetInt() == 1 && away.AsVector2D().IsLengthLessThan( 1.0f ) )
		{
			// stacked on the same spot: a direction of our own, so the stack fans out instead of freezing
			const float yaw = DEG2RAD( ( me->entindex() * 137.508f ) );
			away.Init( 32.0f * cosf( yaw ), 32.0f * sinf( yaw ), 0.0f );
		}

		me->GetLocomotionInterface()->Approach( me->GetLocomotionInterface()->GetFeet() + away );
	}
}



//-----------------------------------------------------------------------------------------
// NEO-HARNESS-TEMP research arm (2026-09-25, patch 88): the ladder approach steers straight at the mount point, so starting it
// by distance alone sends a bot on the far side of a wall into the wall (bullet ladder 4: 21 of 28 up-approach timeouts from
// behind the pillar the ladder hangs on, 55 u through it; scratch/rr/ladder_audit.md 3.7). Start it only when the way to the
// mount point is clear or the bot already stands in the ladder's own end area.
ConVar neo_bot_ladder_approach_clear( "neo_bot_ladder_approach_clear", "0", FCVAR_CHEAT,
	"Research: start a ladder approach only with a clear hull path to the mount point (or from the ladder's end area)" );

static bool NeoLadderEntryClear( CNEOBot *me, const CNavLadder *ladder, bool bGoingUp )
{
	const CNavArea *pArea = me->GetLastKnownArea();
	if ( pArea && ( bGoingUp ? pArea == ladder->m_bottomArea
		: ( pArea == ladder->m_topForwardArea || pArea == ladder->m_topLeftArea || pArea == ladder->m_topRightArea || pArea == ladder->m_topBehindArea ) ) )
	{
		return true;
	}

	// at the bot's own height, a step up: in front of the face going up, the ladder's top going down
	ILocomotion *mover = me->GetLocomotionInterface();
	const float flOut = me->GetBodyInterface()->GetHullWidth() * 0.5f + 2.0f;
	Vector start = mover->GetFeet();
	start.z += mover->GetStepHeight();
	Vector entry = bGoingUp ? ladder->m_bottom + ladder->GetNormal() * flOut : ladder->m_top;
	entry.z = start.z;

	// right at the mount point the trace only meets the ladder's own surroundings (la1): let the approach start
	const float flNearEnough = 48.0f;
	if ( ( entry - start ).AsVector2D().IsLengthLessThan( flNearEnough ) )
	{
		return true;
	}

	const Vector vecMins( me->WorldAlignMins().x, me->WorldAlignMins().y, 0.0f );
	const Vector vecMaxs( me->WorldAlignMaxs().x, me->WorldAlignMaxs().y, me->GetBodyInterface()->GetCrouchHullHeight() - mover->GetStepHeight() );
	trace_t tr;
	UTIL_TraceHull( start, entry, vecMins, vecMaxs, MASK_PLAYERSOLID, me, COLLISION_GROUP_PLAYER_MOVEMENT, &tr );
	return !tr.startsolid && tr.fraction >= 1.0f;
}

// NEO-HARNESS-TEMP research arm (2026-09-25, patch 94): a bot the engine holds on a ladder while no ladder behaviour runs -
// no path, or a path whose goal is not a ladder - hangs there: nothing below drives it (rg2: 50-60 s episodes at the
// restored shaft tops of sentinel ctg / jgr and half way up skyline 1, goal -1; scratch/rr/ladder_audit.md 3.4). Climb it
// to the nearer end.
ConVar neo_bot_ladder_orphan_climb( "neo_bot_ladder_orphan_climb", "0", FCVAR_CHEAT,
	"Research: a bot left on a ladder with no ladder behaviour climbs it to the nearer end" );

static const CNavLadder *NeoLadderUnderBot( CNEOBot *me )
{
	const Vector &feet = me->GetLocomotionInterface()->GetFeet();
	const CNavLadder *best = nullptr;
	float flBest = Square( 48.0f );
	const NavLadderVector &ladders = TheNavMesh->GetLadders();
	FOR_EACH_VEC( ladders, i )
	{
		const CNavLadder *ladder = ladders[i];
		if ( feet.z < ladder->m_bottom.z - 24.0f || feet.z > ladder->m_top.z + 24.0f )
		{
			continue;
		}

		const float flDistSq = ( ladder->GetPosAtHeight( feet.z ) - feet ).AsVector2D().LengthSqr();
		if ( flDistSq < flBest )
		{
			flBest = flDistSq;
			best = ladder;
		}
	}
	return best;
}

ActionResult< CNEOBot > CNEOBotTacticalMonitor::WatchForLadders( CNEOBot *me )
{
	// Check if our current path has an approaching ladder segment
	const PathFollower *path = me->GetCurrentPath();

	// patch 94: on a ladder with nothing to climb it for
	if ( neo_bot_ladder_orphan_climb.GetBool() && me->IsOnLadder() )
	{
		const Path::Segment *pathGoal = ( path && path->IsValid() ) ? path->GetCurrentGoal() : nullptr;
		const CNavLadder *ladder = NeoLadderUnderBot( me );
		if ( ladder && !( pathGoal && pathGoal->ladder == ladder ) )
		{
			const float z = me->GetLocomotionInterface()->GetFeet().z;
			const bool bUp = ( ladder->m_top.z - z ) < ( z - ladder->m_bottom.z );
			return SuspendFor( new CNEOBotLadderClimb( ladder, bUp ), bUp ? "Left on a ladder: climbing to the top" : "Left on a ladder: climbing down" );
		}
	}

	if ( !path || !path->IsValid() )
	{
		return Continue();
	}

	const Path::Segment *goal = path->GetCurrentGoal();
	if ( !goal || !goal->ladder )
	{
		return Continue();
	}

	// We're approaching a ladder - check distance
	const float ladderApproachRange = CNEOBotLadderApproach::ALIGN_RANGE;
	bool goingUp = (goal->how == GO_LADDER_UP);
	Vector ladderPos = (goal->how == GO_LADDER_UP) 
		? goal->ladder->m_bottom 
		: goal->ladder->m_top;

	// Sometimes we accidentally run into a ladder without expecting to
	if ( me->IsOnLadder() )
	{
		return SuspendFor(
			new CNEOBotLadderClimb( goal->ladder, goingUp ),
			goingUp ? "Encountered ladder up" : "Encountered ladder down" 
		);
	}

	if ( me->GetAbsOrigin().DistToSqr( ladderPos ) < Square(ladderApproachRange) )
	{
		// patch 88: not through a wall - keep following the path round it first
		if ( neo_bot_ladder_approach_clear.GetBool() && !NeoLadderEntryClear( me, goal->ladder, goingUp ) )
		{
			return Continue();
		}

		return SuspendFor( 
			new CNEOBotLadderApproach( goal->ladder, goingUp ), 
			goingUp ? "Approaching ladder up" : "Approaching ladder down" 
		);
	}

	return Continue();
}


//-----------------------------------------------------------------------------------------
ActionResult< CNEOBot >	CNEOBotTacticalMonitor::Update( CNEOBot *me, float interval )
{
	if ( neo_bot_force_jump.GetBool() )
	{
		if ( !me->GetLocomotionInterface()->IsClimbingOrJumping() )
		{
			me->GetLocomotionInterface()->Jump();
		}
	}

	CBaseEntity *dangerousGrenade = CNEOBotRetreatFromGrenade::FindDangerousGrenade( me );
	if ( dangerousGrenade )
	{
		return SuspendFor( new CNEOBotRetreatFromGrenade( dangerousGrenade ), "Fleeing from grenade!" );
	}

	ActionResult< CNEOBot > result = WatchForLadders( me );
	if ( result.IsRequestingChange() )
	{
		return result;
	}

	if ( CBaseEntity *breakable = CNEOBotPathClearBreakable::GetBreakableInPath( me ) )
	{
		return SuspendFor( new CNEOBotPathClearBreakable( breakable ), "Clearing breakable in path" );
	}

	const bool bInAHurry = ( me->GetIntentionInterface()->ShouldHurry( me ) == ANSWER_YES );

	if ( !bInAHurry )
	{
		// check if we need to get to cover
		QueryResultType shouldRetreat = me->GetIntentionInterface()->ShouldRetreat( me );

		if ( shouldRetreat == ANSWER_YES )
		{
			return SuspendFor( new CNEOBotRetreatToCover, "Backing off" );
		}
		else if ( shouldRetreat != ANSWER_NO )
		{
			// retreat if we need to do a full reload
			if ( me->IsDifficulty( CNEOBot::HARD ) || me->IsDifficulty( CNEOBot::EXPERT ) )
			{
				CNEOBaseCombatWeapon *weapon = (CNEOBaseCombatWeapon*) me->GetActiveWeapon();
				if ( weapon && weapon->GetPrimaryAmmoCount() > 0 && me->IsBarrageAndReloadWeapon(weapon))
				{
					if ( weapon->Clip1() <= 1 )
					{
						return SuspendFor( new CNEOBotRetreatToCover, "Moving to cover to reload" );
					}
				}
			}
		}

		// Don't want to interfere with human squad leader's control just to ignore hazards
		// Might be annoying if bots ignored following or waypoints (e.g. smoke sightlines avoidance)
		if ( !me->m_hCommandingPlayer.Get() && m_hazardCheckTimer.IsElapsed() )
		{
			m_hazardCheckTimer.Start( 0.5f );
			CNavArea *myArea = me->GetLastKnownArea();
			if ( myArea )
			{
				if ( CNEOBotPathReservations()->IsAreaHazardous( myArea->GetID(), me ) )
				{
					return SuspendFor( new CNEOBotRetreatFromHazardArea( ), "Avoiding hazard area" );
				}
			}
		}

		ActionResult< CNEOBot > scavengeResult = ScavengeForPrimaryWeapon( me );
		if ( scavengeResult.IsRequestingChange() )
		{
			return scavengeResult;
		}
	}

#if 0 // NEO TODO (Adam) search for dropped weapons to resupply ammunition
	bool isAvailable = ( me->GetIntentionInterface()->ShouldHurry( me ) != ANSWER_YES );

	// collect ammo and health kits, unless we're in a big hurry
	if ( isAvailable )
	{
		if ( m_maintainTimer.IsElapsed() )
		{
			m_maintainTimer.Start( RandomFloat( 0.3f, 0.5f ) );

			if ( me->IsAmmoLow() && CNEOBotGetAmmo::IsPossible( me ) )
			{
				return SuspendFor( new CNEOBotGetAmmo, "Grabbing nearby ammo" );
			}
		}
	}
#endif

	ActionResult< CNEOBot > detpackResult = MonitorArmedDetpack( me );
	if ( detpackResult.IsRequestingChange() )
	{
		return detpackResult;
	}

	if ( !(me->m_nButtons & (IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT | IN_USE)) )
	{
		AvoidBumpingFriends( me );
	}

	const CKnownEntity *threat = me->GetVisionInterface()->GetPrimaryKnownThreat();
	if ( !threat )
	{
		me->ReloadIfLowClip();
	}

	me->UpdateDelayedThreatNotices();

	return Continue();
}


//-----------------------------------------------------------------------------------------
ActionResult< CNEOBot > CNEOBotTacticalMonitor::ScavengeForPrimaryWeapon( CNEOBot *me )
{
	if ( !m_maintainTimer.IsElapsed() )
	{
		return Continue();
	}

	// Avoid swapping weapon in the middle of a fight
	if ( me->GetTimeSinceWeaponFired() < 3.0f )
	{
		return Continue();
	}

	CBaseCombatWeapon *pPrimary = me->Weapon_GetSlot( 0 );
	const bool bHasPrimaryAmmo = ( pPrimary != nullptr && pPrimary->HasAnyAmmo() );
	const float flDelay = bHasPrimaryAmmo ? neo_bot_scavenge_upgrade_delay.GetFloat() : 1.0f;
	m_maintainTimer.Start( flDelay );
	
	CBaseEntity *pNearestWeapon = FindNearestPrimaryWeapon( me, false, m_pIgnoredWeapons.get() );
	if ( pNearestWeapon )
	{
		return SuspendFor( new CNEOBotSeekWeapon( pNearestWeapon, m_pIgnoredWeapons.get() ), "Scavenging for a new primary weapon" );
	}

	return Continue();
}


//-----------------------------------------------------------------------------------------
EventDesiredResult< CNEOBot > CNEOBotTacticalMonitor::OnOtherKilled( CNEOBot *me, CBaseCombatCharacter *victim, const CTakeDamageInfo &info )
{
	return TryContinue();
}


//-----------------------------------------------------------------------------------------
EventDesiredResult< CNEOBot > CNEOBotTacticalMonitor::OnNavAreaChanged( CNEOBot *me, CNavArea *newArea, CNavArea *oldArea )
{
	return TryContinue();
}

//-----------------------------------------------------------------------------------------
EventDesiredResult< CNEOBot > CNEOBotTacticalMonitor::OnCommandString( CNEOBot *me, const char *command )
{
	if ( FStrEq( command, "despawn" ) )
	{
		return TrySuspendFor( new CNEODespawn(), RESULT_CRITICAL, "Received command to go to de-spawn" );
	}

	return TryContinue();
}
