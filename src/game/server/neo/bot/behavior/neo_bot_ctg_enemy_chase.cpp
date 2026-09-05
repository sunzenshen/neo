#include "cbase.h"
#include "neo_player.h"
#include "bot/neo_bot.h"
#include "bot/behavior/neo_bot_ctg_enemy_chase.h"
#include "bot/behavior/neo_bot_ctg_enemy.h"
#include "bot/behavior/neo_bot_attack.h"
#include "bot/neo_bot_path_compute.h"
#include "neo_gamerules.h"

//---------------------------------------------------------------------------------------------
ActionResult< CNEOBot > CNEOBotCtgEnemyChase::OnStart( CNEOBot *me, Action< CNEOBot > *priorAction )
{
	m_chasePath.SetMinLookAheadDistance( me->GetDesiredPathLookAheadRange() );

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

	const CKnownEntity *threat = me->GetVisionInterface()->GetPrimaryKnownThreat( true );
	if ( threat && !threat->IsObsolete() && me->GetIntentionInterface()->ShouldAttack( me, threat ) )
	{
		return SuspendFor( new CNEOBotAttack( pGhostCarrier->GetAbsOrigin() ), "Attacking ghoster team" );
	}

	CNEOBotPathUpdateChase( me, m_chasePath, pGhostCarrier, FASTEST_ROUTE );

	return Continue();
}
