#include "cbase.h"
#include "neo_gamerules.h"
#include "neo_player.h"
#include "bot/neo_bot.h"
#include "bot/behavior/neo_bot_grenade_dispatch.h"
#include "bot/behavior/neo_bot_grenade_throw_frag.h"
#include "bot/behavior/neo_bot_grenade_throw_smoke.h"
#include "neo/neo_smokelineofsightblocker.h"
#include "nav_mesh.h"
#include "weapon_neobasecombatweapon.h"
#include "weapon_grenade.h"
#include "weapon_smokegrenade.h"

ConVar sv_neo_bot_grenade_polite_frag("sv_neo_bot_grenade_polite_frag", "1", FCVAR_NONE, "Force bots to consider teammates when throwing frag grenades.", true, 0, true, 1);
ConVar sv_neo_bot_grenade_polite_smoke("sv_neo_bot_grenade_polite_smoke", "1", FCVAR_NONE, "Force bots to consider teammates when throwing smoke grenades.", true, 0, true, 1);
ConVar sv_neo_bot_grenade_use_frag("sv_neo_bot_grenade_use_frag", "1", FCVAR_NONE, "Allow bots to use frag grenades", true, 0, true, 1);
ConVar sv_neo_bot_grenade_use_smoke("sv_neo_bot_grenade_use_smoke", "1", FCVAR_NONE, "Allow bots to use smoke grenades", true, 0, true, 1);
ConVar sv_neo_bot_grenade_throw_cooldown("sv_neo_bot_grenade_throw_cooldown", "10", FCVAR_NONE, "Cooldown in seconds between grenade throws for bots");

extern ConVar neo_bot_fire_weapon_allowed;

namespace
{
// Trace to the torso, so a low ledge in front of the feet does not count as cover
constexpr float SMOKE_FRAG_TORSO_HEIGHT = 36.0f;
}

//---------------------------------------------------------------------------------------------
bool CNEOBotGrenadeDispatch::GetSmokeHiddenThreatPos( const CNEOBot *me, const CKnownEntity *threat, Vector &pos )
{
	// Support thermal vision sees through smoke.
	if ( me->GetClass() == NEO_CLASS_SUPPORT )
	{
		return false;
	}

	if ( !threat || !threat->GetEntity() || threat->IsVisibleInFOVNow() )
	{
		return false;
	}

	// Throw planning needs a nav area, which the rough position from suppression may lack.
	Vector vecBelieved = threat->GetLastKnownPosition();
	Vector vecHeard;
	if ( me->GetSuppressiveFire()->GetBelievedPos( threat->GetEntity(), vecHeard ) && TheNavMesh->GetNavArea( vecHeard ) )
	{
		vecBelieved = vecHeard;
	}

	const Vector vecEyes = me->GetBodyInterface()->GetEyePosition();
	const Vector vecTorso = vecBelieved + Vector( 0, 0, SMOKE_FRAG_TORSO_HEIGHT );

	NextBotTraceFilterIgnoreActors filter( nullptr, COLLISION_GROUP_NONE );
	trace_t trWorld;
	UTIL_TraceLine( vecEyes, vecTorso, MASK_SOLID, &filter, &trWorld );
	if ( trWorld.DidHit() )
	{
		return false;
	}

	trace_t trSmoke;
	{
		ScopedSmokeLOS smokeGuard( false );
		// The trace filter needs CONTENTS_MONSTER to hit the smoke blocker's bounding box.
		UTIL_TraceLine( vecEyes, vecTorso, MASK_BLOCKLOS_AND_NPCS, &filter, &trSmoke );
	}

	if ( !trSmoke.DidHit() || !trSmoke.m_pEnt || !FStrEq( trSmoke.m_pEnt->GetClassname(), SMOKELINEOFSIGHTBLOCKER_ENTITYNAME ) )
	{
		return false;
	}

	pos = vecBelieved;
	return true;
}

//---------------------------------------------------------------------------------------------
Action< CNEOBot > *CNEOBotGrenadeDispatch::ChooseGrenadeThrowBehavior( const CNEOBot *me, const CKnownEntity *threat )
{
	if ( !neo_bot_fire_weapon_allowed.GetBool() )
	{
		return nullptr;
	}

	if (!sv_neo_bot_grenade_use_frag.GetBool() && !sv_neo_bot_grenade_use_smoke.GetBool())
	{
		return nullptr;
	}

	if ( !threat || !threat->GetEntity() || !threat->GetEntity()->IsPlayer() || !threat->GetEntity()->IsAlive() )
	{
		return nullptr;
	}

	Vector vecSmokeTarget;
	const bool bBehindSmoke = GetSmokeHiddenThreatPos( me, threat, vecSmokeTarget );

	// A clear line of fire through smoke does not give us a visible target.
	if ( !bBehindSmoke && me->IsLineOfFireClear( threat->GetEntity(), CNEOBot::LINE_OF_FIRE_FLAGS_DEFAULT ) )
	{
		return nullptr;
	}

	CWeaponGrenade *pFragGrenade = nullptr;
	CWeaponSmokeGrenade *pSmokeGrenade = nullptr;

	int iWeaponCount = me->WeaponCount();
	for ( int i=0; i<iWeaponCount; ++i )
	{
		CBaseCombatWeapon *pWep = me->GetWeapon( i );
		if ( !pWep )
		{
			continue;
		}

		CNEOBaseCombatWeapon *pNeoWep = static_cast< CNEOBaseCombatWeapon * >( pWep );

		if ( !pNeoWep->HasPrimaryAmmo() )
		{
			continue;
		}

		auto bits = pNeoWep->GetNeoWepBits();
		if ( sv_neo_bot_grenade_use_frag.GetBool() && (bits & NEO_WEP_FRAG_GRENADE) )
		{
			pFragGrenade = static_cast< CWeaponGrenade * >( pNeoWep );
			if ( pSmokeGrenade || (me->GetClass() != NEO_CLASS_SUPPORT) )
			{
				break; // done searching
			}
		}
		else if ( sv_neo_bot_grenade_use_smoke.GetBool() && (bits & NEO_WEP_SMOKE_GRENADE) )
		{
			pSmokeGrenade = static_cast< CWeaponSmokeGrenade * >( pNeoWep );
			if ( pFragGrenade )
			{
				break; // done searching
			}
		}
	}

	if ( !pFragGrenade && !pSmokeGrenade )
	{
		return nullptr;
	}

	// Should I toss a smoke grenade?
	if ( pSmokeGrenade && (me->GetClass() == NEO_CLASS_SUPPORT) )
	{
		if ( sv_neo_bot_grenade_polite_smoke.GetBool() )
		{
			for ( int i = 1; i <= gpGlobals->maxClients; i++ )
			{
				CNEO_Player *pPlayer = ToNEOPlayer( UTIL_PlayerByIndex( i ) );
				if ( !pPlayer || !pPlayer->IsAlive() || pPlayer == me )
				{
					continue;
				}

				if ( !me->IsEnemy( pPlayer ) )
				{
					if ( !pPlayer->IsBot() && pPlayer->GetClass() != NEO_CLASS_SUPPORT )
					{
						// Avoid blocking the vision of a friendly human
						// (Bots benefit from concealment without the disorientation)
						return nullptr;
					}
				}
				else
				{
					if ( pPlayer->GetClass() == NEO_CLASS_SUPPORT )
					{
						// Avoid giving an enemy with thermal vision a free smoke screen
						return nullptr;
					}
				}
			}
		}

		return new CNEOBotGrenadeThrowSmoke( pSmokeGrenade, threat );
	}

	// Should I toss a frag grenade? 
	if ( pFragGrenade )
	{
		const Vector vecFragTarget = bBehindSmoke ? vecSmokeTarget : threat->GetLastKnownPosition();
		const bool bFragSafe = sv_neo_bot_grenade_polite_frag.GetBool()
			? CNEOBotGrenadeThrowFrag::IsFragSafe( me, vecFragTarget )
			: CNEOBotGrenadeThrowFrag::IsFragPolite( me, vecFragTarget );
		if ( !bFragSafe )
		{
			return nullptr;
		}

		if ( bBehindSmoke )
		{
			return new CNEOBotGrenadeThrowFrag( pFragGrenade, threat, vecSmokeTarget );
		}

		return new CNEOBotGrenadeThrowFrag( pFragGrenade, threat );
	}

	return nullptr;
}
