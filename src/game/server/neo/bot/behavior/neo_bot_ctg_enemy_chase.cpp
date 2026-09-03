#include "cbase.h"
#include "neo_player.h"
#include "bot/neo_bot.h"
#include "bot/behavior/neo_bot_ctg_enemy_chase.h"
#include "bot/behavior/neo_bot_ctg_enemy.h"
#include "bot/behavior/neo_bot_attack.h"
#include "bot/behavior/neo_bot_ctg_enemy_intercept_cap_path.h"
#include "bot/neo_bot_path_compute.h"
#include "neo_gamerules.h"
#include "neo_player_shared.h"
#include "vphysics_interface.h"

ConVar sv_neo_bot_ctg_enemy_chase_replan_seconds( "sv_neo_bot_ctg_enemy_chase_replan_seconds", "20", FCVAR_CHEAT,
	"CTG: seconds between a chasing bot re-checking whether to take the fastest or the default route to the enemy ghost carrier.",
	true, 1.0f, false, 0.0f );

ConVar sv_neo_bot_ctg_enemy_endgame_dist( "sv_neo_bot_ctg_enemy_endgame_dist", "0", FCVAR_CHEAT,
	"CTG: once the enemy ghost carrier is within this many Hammer units of its scoring zone, a bot "
	"chasing it stops breaking off to fight anyone who is not the carrier. 0 = always break off.",
	true, 0.0f, false, 0.0f );

ConVar sv_neo_bot_ctg_enemy_redecide( "sv_neo_bot_ctg_enemy_redecide", "0", FCVAR_CHEAT,
	"CTG: 1 = a bot chasing an enemy ghost carrier keeps watching for a cut-off opening up ahead of "
	"it, and takes one when it does. Without this the chase-or-cut-off decision is made once." );

ConVar sv_neo_bot_ctg_enemy_redecide_seconds( "sv_neo_bot_ctg_enemy_redecide_seconds", "3", FCVAR_CHEAT,
	"CTG: seconds between a chasing bot re-checking for a cut-off, when sv_neo_bot_ctg_enemy_redecide is on.",
	true, 0.5f, false, 0.0f );

//---------------------------------------------------------------------------------------------
ActionResult< CNEOBot > CNEOBotCtgEnemyChase::OnStart( CNEOBot *me, Action< CNEOBot > *priorAction )
{
	m_chasePath.SetMinLookAheadDistance( me->GetDesiredPathLookAheadRange() );
	m_routeTypeTimer.Invalidate();
	m_routeType = DEFAULT_ROUTE;

	// Start the re-decide clock rather than invalidating it: arriving here from an interception
	// that just gave up on its cut-off, and immediately re-picking one, is the thrash this timer
	// exists to damp.
	m_redecideTimer.Start( sv_neo_bot_ctg_enemy_redecide_seconds.GetFloat() );

	return Continue();
}

//---------------------------------------------------------------------------------------------
ActionResult< CNEOBot > CNEOBotCtgEnemyChase::Update( CNEOBot *me, float interval )
{
	CNEO_Player *pGhostCarrier = CNEOBotCtgEnemy::EnemyGhostCarrier( me );
	if ( !pGhostCarrier )
	{
		return Done( "No enemy ghost carrier" );
	}

	// Slow re-evaluation of the route type.
	if ( m_routeTypeTimer.IsElapsed() )
	{
		m_routeTypeTimer.Start( sv_neo_bot_ctg_enemy_chase_replan_seconds.GetFloat() );

		// A cut-off exists exactly when there is somewhere on the carrier's route we can still beat
		// it to, which is the same question as "am I ahead of it or behind it".
		CNEOBotCtgEnemy::CutOff cutOff;
		m_routeType = CNEOBotCtgEnemy::FindCutOff( me, pGhostCarrier, cutOff )
			? DEFAULT_ROUTE : FASTEST_ROUTE;
	}

	// The chase-or-cut-off decision was made once, when this action started, against where the
	// carrier was then. It moves, teammates die, and ground that was behind the carrier comes back
	// in front of it - so keep asking.
	//
	// The gate is what stops this thrashing against the interception's own "the carrier is on top
	// of the cut-off, go back to chasing" exit: only a cut-off further along the carrier's route
	// than it can see through walls is worth breaking off a chase for. Anything nearer is a spot
	// the carrier is already looking at, which is what the chase is for.
	if ( sv_neo_bot_ctg_enemy_redecide.GetBool() && m_redecideTimer.IsElapsed() )
	{
		m_redecideTimer.Start( sv_neo_bot_ctg_enemy_redecide_seconds.GetFloat() );

		CNEOBotCtgEnemy::CutOff cutOff;
		CNEOBotPredictedRoute carrierRoute;
		const float flGhostViewUnits = sv_neo_ghost_view_distance.GetFloat() / METERS_PER_INCH;

		if ( CNEOBotCtgEnemy::FindCutOff( me, pGhostCarrier, cutOff, &carrierRoute )
			&& cutOff.pArea != me->GetLastKnownArea()
			&& cutOff.iCarrierRouteIndex > 0
			&& carrierRoute.travel[ cutOff.iCarrierRouteIndex ] > flGhostViewUnits )
		{
			return ChangeTo( new CNEOBotCtgEnemyInterceptCapPath( cutOff, carrierRoute ),
				"A cut-off opened up ahead of the carrier" );
		}
	}

	const CKnownEntity *threat = me->GetVisionInterface()->GetPrimaryKnownThreat( true );
	if ( threat && !threat->IsObsolete() && me->GetIntentionInterface()->ShouldAttack( me, threat ) )
	{
		// Close to the endzone the arithmetic changes. A capture is instantaneous on touching the
		// zone and pays every living winner a rank-up, where an elimination win pays one or two
		// points - so once the carrier is nearly there, stopping to fight anyone who is not the
		// carrier trades the round for a skirmish. Run at it and take the odds instead.
		const float flEndgame = sv_neo_bot_ctg_enemy_endgame_dist.GetFloat();
		const bool bThreatIsTheCarrier = ( threat->GetEntity() == pGhostCarrier );
		const float flCarrierToGoal = ( flEndgame > 0.0f && !bThreatIsTheCarrier )
			? CNEOBotCtgEnemy::CarrierDistToGoal( pGhostCarrier ) : -1.0f;

		if ( flCarrierToGoal < 0.0f || flCarrierToGoal > flEndgame )
		{
			return SuspendFor( new CNEOBotAttack( pGhostCarrier->GetAbsOrigin() ), "Attacking ghoster team" );
		}
	}

	CNEOBotPathUpdateChase( me, m_chasePath, pGhostCarrier, m_routeType );

	return Continue();
}

//---------------------------------------------------------------------------------------------
EventDesiredResult< CNEOBot > CNEOBotCtgEnemyChase::OnStuck( CNEOBot *me )
{
	return TryContinue();
}

//---------------------------------------------------------------------------------------------
EventDesiredResult< CNEOBot > CNEOBotCtgEnemyChase::OnMoveToSuccess( CNEOBot *me, const Path *path )
{
	return TryContinue();
}

//---------------------------------------------------------------------------------------------
EventDesiredResult< CNEOBot > CNEOBotCtgEnemyChase::OnMoveToFailure( CNEOBot *me, const Path *path, MoveToFailureType reason )
{
	return TryContinue();
}
