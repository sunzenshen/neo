#include "cbase.h"
#include "neo_player.h"
#include "neo_gamerules.h"
#include "bot/neo_bot.h"
#include "bot/behavior/neo_bot_ctg_seek.h"
#include "bot/behavior/neo_bot_ctg_lone_wolf.h"
#include "bot/behavior/neo_bot_ctg_escort.h"
#include "bot/behavior/neo_bot_ctg_enemy.h"
#include "bot/behavior/neo_bot_ctg_enemy_intercept_cap_path.h"
#include "bot/behavior/neo_bot_ctg_carrier.h"
#include "bot/behavior/neo_bot_ctg_capture.h"
#include "bot/neo_bot_path_compute.h"
#include "neo_enums.h"
#include "weapon_ghost.h"

ConVar sv_neo_bot_ctg_goalie_depth( "sv_neo_bot_ctg_goalie_depth", "600", FCVAR_CHEAT,
	"CTG: how far (units) back from the threatened enemy cap, along the predicted ghost->cap route, "
	"a freezetime Support-class 'goalie' holds. 0 disables the freezetime goalie entirely.",
	true, 0.0f, false, 0.0f );

ConVar sv_neo_bot_ctg_assault_intercept( "sv_neo_bot_ctg_assault_intercept", "0", FCVAR_CHEAT,
	"CTG: when 1, Assault-class defenders also pre-plan a ghost cut-off during freezetime instead of "
	"racing for the loose ghost. Recon always races; Support always goalies (sv_neo_bot_ctg_goalie_depth)." );

//---------------------------------------------------------------------------------------------
// During freezetime, before anyone holds the ghost, split the defence by class:
//   - Recon (fastest) races the loose ghost, as before - fall through, no suspend here.
//   - Support (slowest) sets up as a goalie on the threatened wing cap's approach throat.
//   - Assault joins the goalie's forward cut-off only when sv_neo_bot_ctg_assault_intercept is on.
// Returns a non-Continue result when it has taken over, Continue() to let the caller carry on.
ActionResult< CNEOBot > CNEOBotCtgSeek::ConsiderFreezetimeIntercept( CNEOBot *me )
{
	if ( NEORules()->GetRemainingPreRoundFreezeTime( true ) <= 0.0f || !NEORules()->GhostExists() )
	{
		return Continue();
	}

	const int iClass = me->GetClass();

	if ( iClass == NEO_CLASS_SUPPORT )
	{
		const float flDepth = sv_neo_bot_ctg_goalie_depth.GetFloat();
		if ( flDepth <= 0.0f )
		{
			return Continue();
		}

		CNEOBotCtgEnemy::CutOff hold;
		CNEOBotPredictedRoute route;
		if ( CNEOBotCtgEnemy::FindGoalieHold( me, flDepth, hold, &route ) )
		{
			return SuspendFor( new CNEOBotCtgEnemyInterceptCapPath( hold, route, true, flDepth ),
				"Freezetime: setting up as goalie on the threatened cap" );
		}
	}
	else if ( iClass == NEO_CLASS_ASSAULT && sv_neo_bot_ctg_assault_intercept.GetBool() )
	{
		CNEOBotCtgEnemy::CutOff cutOff;
		CNEOBotPredictedRoute route;
		if ( CNEOBotCtgEnemy::FindCutOff( me, cutOff, &route ) && cutOff.pArea != me->GetLastKnownArea() )
		{
			return SuspendFor( new CNEOBotCtgEnemyInterceptCapPath( cutOff, route ),
				"Freezetime: pre-planning a ghost cut-off" );
		}
	}

	return Continue();
}

//---------------------------------------------------------------------------------------------
ActionResult< CNEOBot > CNEOBotCtgSeek::Update( CNEOBot *me, float interval )
{
	if (NEORules()->GetGameType() != NEO_GAME_TYPE_CTG)
	{
		return Done( "Game mode is no longer CTG" );
	}

	{
		ActionResult< CNEOBot > freezeResult = ConsiderFreezetimeIntercept( me );
		if ( freezeResult.IsRequestingChange() || freezeResult.IsDone() )
		{
			return freezeResult;
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
	if ( CNEOBotCtgEnemy::EnemyGhostCarrier( me ) )
	{
		return SuspendFor( new CNEOBotCtgEnemy, "Stopping the ghost carrier!" );
	}

	ActionResult< CNEOBot > result = UpdateCommon( me, interval );
	if ( result.IsRequestingChange() || result.IsDone() )
	{
		return result;
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
