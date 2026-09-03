#include "cbase.h"
#include "neo_player.h"
#include "neo_gamerules.h"
#include "bot/neo_bot.h"
#include "bot/behavior/neo_bot_ctg_seek.h"
#include "bot/behavior/neo_bot_ctg_lone_wolf.h"
#include "bot/behavior/neo_bot_ctg_escort.h"
#include "bot/behavior/neo_bot_ctg_enemy.h"
#include "bot/behavior/neo_bot_ctg_carrier.h"
#include "bot/behavior/neo_bot_ctg_capture.h"
#include "bot/neo_bot_path_compute.h"
#include "weapon_ghost.h"
#include "neo_ghost_cap_point.h"
#include "nav_mesh.h"
#include "nav_pathfind.h"

// NEO-HARNESS-TEMP: positive control for re-measuring the pre-fix dispatch order at a larger
// sample than the original baseline arm (P0, n=24) had. When set, the enemy-carrier check moves
// back below UpdateCommon, exactly where it sat before this branch. Never part of the PR -- see
// harness/patches/README.md.
ConVar sv_neo_bot_ctg_enemy_priority_off( "sv_neo_bot_ctg_enemy_priority_off", "0", FCVAR_CHEAT,
	"NEO harness debug: 1 = restore the pre-fix dispatch order (combat suspend before the ghost "
	"decision), for re-measuring the baseline at higher n." );

ConVar sv_neo_bot_ctg_seek_preposition_ratio( "sv_neo_bot_ctg_seek_preposition_ratio", "0", FCVAR_CHEAT,
	"CTG: before the ghost is picked up, compare a rough path length from this bot to the ghost "
	"against the same from the enemy's spawn. If the enemy's share of the combined length is at or "
	"below this ratio (0.4 means its path is 4 of a 10-unit total, i.e. clearly shorter), stop "
	"racing for the ghost and pre-position at the zone the enemy would threaten instead. 0 = off.",
	true, 0.0f, true, 0.5f );

//---------------------------------------------------------------------------------------------
// The first spawn point entity belonging to the *other* team this round - public map geometry
// (which classname is whose alternates with roundNumberIsEven(), exactly as
// CNEO_Player::EntSelectSpawnPoint resolves its own), never a live enemy position. "First found"
// rather than nearest-to-anything: NT;RE maps put a team's whole spawn set in one place, so which
// one of that set is returned does not change the answer by more than the width of a spawn room.
static CBaseEntity *FirstEnemySpawnPoint( int iMyTeam )
{
	const bool bAlternate = NEORules()->roundNumberIsEven();
	const char *pMyClassname =
		( iMyTeam == TEAM_JINRAI ) ? ( bAlternate ? "info_player_attacker" : "info_player_defender" ) :
		( iMyTeam == TEAM_NSF )    ? ( bAlternate ? "info_player_defender" : "info_player_attacker" ) :
		nullptr;
	if ( !pMyClassname )
	{
		return nullptr;
	}

	const char *pEnemyClassname = FStrEq( pMyClassname, "info_player_attacker" )
		? "info_player_defender" : "info_player_attacker";
	return gEntList.FindEntityByClassname( nullptr, pEnemyClassname );
}

//---------------------------------------------------------------------------------------------
ActionResult< CNEOBot > CNEOBotCtgSeek::Update( CNEOBot *me, float interval )
{
	if (NEORules()->GetGameType() != NEO_GAME_TYPE_CTG)
	{
		return Done( "Game mode is no longer CTG" );
	}

	if ( sv_neo_bot_ctg_enemy_priority_off.GetBool() )
	{
		ActionResult< CNEOBot > result = UpdateCommon( me, interval );
		if ( result.IsRequestingChange() || result.IsDone() )
		{
			return result;
		}
	}

	// The objective outranks the shooting when the *enemy* has the ghost. UpdateCommon suspends for
	// CNEOBotAttack on any visible threat and runs first, so a defender that can see an escort
	// never reaches the ghost decision below - measured over a 24-round arm, only 1.7 of 5
	// defenders per round ever entered CNEOBotCtgEnemy at all, while the ghost was walked in. The
	// escorts screen the carrier and the defence spends the round fighting the screen.
	//
	// This does not stop the bot fighting: CNEOBotCtgEnemyChase suspends for CNEOBotAttack aimed at
	// the carrier the moment it sees a threat, and a bot holding a cut-off shoots what walks into
	// it. What changes is that the fight is anchored to the carrier's route instead of to wherever
	// the first escort happened to appear.
	if ( NEORules()->GhostExists() )
	{
		const int iGhosterPlayer = NEORules()->GetGhosterPlayer();
		if ( iGhosterPlayer > 0 && iGhosterPlayer <= gpGlobals->maxClients )
		{
			CNEO_Player *pGhostCarrier = ToNEOPlayer( UTIL_PlayerByIndex( iGhosterPlayer ) );
			if ( pGhostCarrier && pGhostCarrier != me && pGhostCarrier->IsAlive()
				&& pGhostCarrier->GetTeamNumber() != me->GetTeamNumber() )
			{
				return SuspendFor( new CNEOBotCtgEnemy, "Stopping the ghost carrier!" );
			}
		}
	}

	// Already ran once above when the control cvar restores the pre-fix order; running it again
	// here would call it twice a tick for that arm, which the shipping order never does.
	if ( !sv_neo_bot_ctg_enemy_priority_off.GetBool() )
	{
		ActionResult< CNEOBot > result = UpdateCommon( me, interval );
		if ( result.IsRequestingChange() || result.IsDone() )
		{
			return result;
		}
	}

	int team_members = 0;
	for ( int i = 1; i <= gpGlobals->maxClients; i++ )
	{
		CNEO_Player *pPlayer = ToNEOPlayer( UTIL_PlayerByIndex( i ) );
		if ( pPlayer && pPlayer->IsAlive() && pPlayer->GetTeamNumber() == me->GetTeamNumber() )
		{
			team_members++;
		}
	}

	if ( team_members == 1 )
	{
		return SuspendFor( new CNEOBotCtgLoneWolf, "I'm the last one on my team!" );
	}

	// The enemy-carrier case is handled above, before the combat suspend; these two are the ones
	// where the ghost is on our own side, and they stay below it deliberately. Raising the escort
	// would do the *attacking* team the same favour, which is the opposite of the intent.
	if (NEORules()->GhostExists())
	{
		int iGhosterPlayer = NEORules()->GetGhosterPlayer();
		if (iGhosterPlayer > 0 && iGhosterPlayer <= gpGlobals->maxClients)
		{
			CNEO_Player* pGhostCarrier = ToNEOPlayer(UTIL_PlayerByIndex(iGhosterPlayer));
			if (pGhostCarrier && pGhostCarrier != me
				&& pGhostCarrier->GetTeamNumber() == me->GetTeamNumber())
			{
				return SuspendFor(new CNEOBotCtgEscort, "Protecting the ghost carrier!");
			}

			// If I have the ghost, switch to ghost behavior
			if (me->IsCarryingGhost())
			{
				return SuspendFor(new CNEOBotCtgCarrier, "I am the ghost carrier!");
			}
		}
	}

	// Ghost capture logic
	if (m_bGoingToTargetEntity && m_hTargetEntity)
	{
		const float useRangeSq = 100.0f * 100.0f;
		if ( me->GetAbsOrigin().DistToSqr( m_hTargetEntity->GetAbsOrigin() ) < useRangeSq ) 
		{
			if ( !m_hTargetEntity->IsPlayer() )
			{
				if ( me->IsLineOfSightClear( m_hTargetEntity, CBaseCombatCharacter::IGNORE_ACTORS ) )
				{
					const char *classname = m_hTargetEntity->GetClassname();
					if ( FStrEq( classname, "weapon_ghost" ) )
					{
						CBaseCombatWeapon* pWeapon = m_hTargetEntity->MyCombatWeaponPointer();
						if ( pWeapon && !pWeapon->GetOwner() )
						{
							CWeaponGhost *pGhost = assert_cast<CWeaponGhost*>( m_hTargetEntity.Get() );
							return SuspendFor( new CNEOBotCtgCapture( pGhost ), "Capturing Ghost" );
						}
					}
				}
				else
				{
					return SuspendFor(new CNEOBotCtgLoneWolf, "Capture target is blocked by some other entity, searching around the nearest areas");
				}
			}
		}
	}

	return Continue();
}

//---------------------------------------------------------------------------------------------
// True, and vecOutGoal set, if the enemy is honestly closer to the ghost than this bot is by at
// least flRatio's margin - in which case the goal is the zone the enemy would threaten once it
// wins that race, not the ghost itself. Both path lengths are the same naive area-centre-summed
// shortest path FindCutOff uses for the equivalent comparison later, so this is not a new measure,
// just an earlier use of the one already trusted for this purpose. Pure lookup - the caller is the
// one with access to this bot's path state, so it does the actual pathing and bookkeeping.
static bool FindPrepositionGoal( CNEOBot *me, float flRatio, Vector &vecOutGoal )
{
	CNavArea *pMyArea = me->GetLastKnownArea();
	CBaseEntity *pEnemySpawn = FirstEnemySpawnPoint( me->GetTeamNumber() );
	if ( !pMyArea || !pEnemySpawn )
	{
		return false;
	}

	const Vector vecGhost = NEORules()->GetGhostPos();
	CNavArea *pGhostArea = TheNavMesh->GetNearestNavArea( vecGhost );
	CNavArea *pEnemyArea = TheNavMesh->GetNearestNavArea( pEnemySpawn->GetAbsOrigin() );
	if ( !pGhostArea || !pEnemyArea )
	{
		return false;
	}

	ShortestPathCost myCost;
	const float flMyLen = NavAreaTravelDistance( pMyArea, pGhostArea, myCost );
	ShortestPathCost enemyCost;
	const float flEnemyLen = NavAreaTravelDistance( pEnemyArea, pGhostArea, enemyCost );
	if ( flMyLen < 0.0f || flEnemyLen < 0.0f )
	{
		return false;
	}

	const float flTotal = flMyLen + flEnemyLen;
	if ( flTotal <= 0.0f || ( flEnemyLen / flTotal ) > flRatio )
	{
		// Close enough to be worth contesting - race for the ghost as normal.
		return false;
	}

	const int iEnemyTeam = ( me->GetTeamNumber() == TEAM_JINRAI ) ? TEAM_NSF : TEAM_JINRAI;
	CNEOGhostCapturePoint *pThreatened = CNEOBotCtgEnemy::NearestCapForTeam( iEnemyTeam, vecGhost );
	if ( !pThreatened )
	{
		return false;
	}

	vecOutGoal = pThreatened->GetAbsOrigin();
	return true;
}

//---------------------------------------------------------------------------------------------
void CNEOBotCtgSeek::RecomputeSeekPath( CNEOBot *me )
{
	if (NEORules()->GetGameType() != NEO_GAME_TYPE_CTG)
	{
		// Wait until next tick to exit behavior
		return;
	}

	m_hTargetEntity = nullptr;
	m_bGoingToTargetEntity = false;
	m_vGoalPos = vec3_origin;

	if (NEORules()->GhostExists())
	{
		const int iGhosterPlayer = NEORules()->GetGhosterPlayer();

		if (iGhosterPlayer > 0)
		{
			// Get ready to transition into CTG specific role next tick
			m_path.Invalidate();
			return;
		}
		else
		{
			// Before racing for the ghost, ask whether the enemy is honestly closer to it than we
			// are. If so by a wide enough margin, do not bother - go cover the zone it would
			// threaten instead, on the unhurried DEFAULT_ROUTE, so the team is already regrouped
			// there by the time the ghost is actually picked up and CNEOBotCtgEnemy takes over
			// (see FindCutOff). The comparison plans from public information only: the enemy's
			// side uses the map's own spawn point geometry, never a live enemy position.
			const float flRatio = sv_neo_bot_ctg_seek_preposition_ratio.GetFloat();
			Vector vecPreposition;
			if ( flRatio > 0.0f && FindPrepositionGoal( me, flRatio, vecPreposition ) )
			{
				m_vGoalPos = vecPreposition;
				m_bGoingToTargetEntity = false;	// a position to hold, not something to pick up
				m_hTargetEntity = nullptr;

				if ( CNEOBotPathCompute( me, m_path, m_vGoalPos, DEFAULT_ROUTE )
					&& m_path.IsValid() && m_path.GetResult() == Path::COMPLETE_PATH )
				{
					return;
				}
				// No path to the threatened zone - fall through and race for the ghost instead.
			}

			// Search for ghost on the ground
			CBaseEntity* pGhost = gEntList.FindEntityByClassname( nullptr, "weapon_ghost" );
			if ( pGhost )
			{
				m_vGoalPos = pGhost->WorldSpaceCenter();
				m_bGoingToTargetEntity = true;
				m_hTargetEntity = pGhost;

				if ( CNEOBotPathCompute( me, m_path, m_vGoalPos, DEFAULT_ROUTE ) && m_path.IsValid() && m_path.GetResult() == Path::COMPLETE_PATH )
				{
					return;
				}
			}
		}
	}

	// Fallback to base behavior (roaming spawn points)
	CNEOBotSeekAndDestroy::RecomputeSeekPath( me );
}
