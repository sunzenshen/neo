#include "cbase.h"

#include "bot/neo_bot.h"
#include "bot/behavior/neo_bot_path_clear_breakable.h"
#include "neo_gamerules.h"

extern ConVar neo_bot_fire_weapon_allowed;
extern ConVar sv_neo_forensic_log;

ConVar neo_bot_fire_at_breakable_weapon_min_time( "neo_bot_fire_at_breakable_weapon_min_time", "0.2", FCVAR_CHEAT,
	"Minimum time to fire at breakables", true, 0.0f, true, 60.0f );

// Research arm (2026-10-07, marketa window): 1 = a ghost carrier clears a breakable in its path with a threat in sight
ConVar neo_bot_carrier_clear_breakable( "neo_bot_carrier_clear_breakable", "0", FCVAR_CHEAT,
	"Research: 1 = a ghost carrier clears a breakable in its path even while it sees a threat, 0 = it waits for no threat in sight" );

// NEO-HARNESS-TEMP forensic instrumentation (2026-10-07): NEO_FORENSIC_BRK - a breakable in the path held back by a threat,
// and each clearing attempt's start and end. Never part of a PR.
static float s_flNeoBrkGateLogged[ MAX_PLAYERS + 1 ];
static void NeoLogBreakable( CNEOBot *me, const char *pszEvent, CBaseEntity *breakable, float flRateLimit )
{
	const int idx = me->entindex();
	if ( !sv_neo_forensic_log.GetBool() || idx <= 0 || idx > MAX_PLAYERS )
	{
		return;
	}

	if ( flRateLimit > 0.0f && gpGlobals->curtime - s_flNeoBrkGateLogged[idx] < flRateLimit && s_flNeoBrkGateLogged[idx] <= gpGlobals->curtime )
	{
		return;
	}
	s_flNeoBrkGateLogged[idx] = gpGlobals->curtime;

	CBaseCombatWeapon *weapon = me->GetActiveWeapon();
	const CKnownEntity *threat = me->GetVisionInterface()->GetPrimaryKnownThreat();
	const Vector &pos = me->GetAbsOrigin();
	Msg( "NEO_FORENSIC_BRK t=%.2f p=%d event=%s pos=%.0f,%.0f,%.0f carrier=%d wep=%s brk=%d hp=%d threatvis=%d\n", gpGlobals->curtime, idx,
		pszEvent, pos.x, pos.y, pos.z, me->IsCarryingGhost() ? 1 : 0, weapon ? weapon->GetClassname() : "-",
		breakable ? breakable->entindex() : -1, breakable ? breakable->GetHealth() : 0,
		( threat && threat->IsVisibleRecently() ) ? 1 : 0 );
}

//--------------------------------------------------------------------------------------------------------
// A threat in sight holds a bot back from a breakable, since it fights the threat first,
// but a ghost carrier does not fight it, so waiting for it to go keeps the carrier at the breakable
static bool IsHeldBackByThreat( CNEOBot *me )
{
	const CKnownEntity *threat = me->GetVisionInterface()->GetPrimaryKnownThreat();
	if ( !threat || !threat->GetEntity() || !threat->IsVisibleRecently() )
	{
		return false;
	}

	return !( neo_bot_carrier_clear_breakable.GetBool() && me->IsCarryingGhost() );
}

//--------------------------------------------------------------------------------------------------------
CBaseEntity *CNEOBotPathClearBreakable::GetBreakableInPath( CNEOBot *me )
{
	if ( !me || !me->IsAlive() )
	{
		return nullptr;
	}

	if ( me->GetNeoFlags() & NEO_FL_FREEZETIME )
	{
		return nullptr;
	}

	if ( !neo_bot_fire_weapon_allowed.GetBool() )
	{
		return nullptr;
	}

	const PathFollower *path = me->GetCurrentPath();
	if ( !path || !path->IsValid() )
	{
		return nullptr;
	}

	// the path follower's look ahead for props also finds breakables in the way
	CBaseEntity *breakable = path->GetBreakableInWay();
	if ( !breakable || !breakable->IsAlive() || breakable->GetHealth() <= 0 )
	{
		return nullptr;
	}

	// Only enter this behavior if there is no visible threat
	if ( IsHeldBackByThreat( me ) )
	{
		NeoLogBreakable( me, "held_by_threat", breakable, 1.0f );
		return nullptr;
	}

	return breakable;
}


//--------------------------------------------------------------------------------------------------------
CNEOBotPathClearBreakable::CNEOBotPathClearBreakable( CBaseEntity *breakable )
{
	m_hBreakable = breakable;
}


//--------------------------------------------------------------------------------------------------------
ActionResult< CNEOBot > CNEOBotPathClearBreakable::OnStart( CNEOBot *me, Action< CNEOBot > *priorAction )
{
	m_isWaitingForFullReload = false;
	m_bDidSwitchWeapon = false;
	m_bSkipKnife = false;
	m_bStrafeRight = RandomInt( 0, 1 ) == 0;
	m_giveUpTimer.Start( 10.0f );
	m_meleeTimeoutTimer.Start( 2.0f );

	if ( !m_hBreakable.Get() )
	{
		return Done( "No breakable target" );
	}

	NeoLogBreakable( me, "start", m_hBreakable.Get(), 0.0f );

	// Prioritize weapons for clearing breakables:
	// 1. Knife (Slot 2)
	// 2. Secondary (Slot 1) if it has ammo
	// 3. Current/Primary (Slot 0) as fallback

	auto *myWeapon = static_cast<CNEOBaseCombatWeapon *>( me->GetActiveWeapon() );
	auto *knife = static_cast<CNEOBaseCombatWeapon *>( me->Weapon_GetSlot( 2 ) );
	auto *secondaryWep = static_cast<CNEOBaseCombatWeapon *>( me->Weapon_GetSlot( 1 ) );

	if ( !m_bSkipKnife && knife && knife != myWeapon )
	{
		me->Weapon_Switch( knife );
		m_bDidSwitchWeapon = true;
	}
	else if ( secondaryWep && secondaryWep != myWeapon &&
			( ( secondaryWep->Clip1() + secondaryWep->m_iPrimaryAmmoCount ) > 0 ) )
	{
		me->Weapon_Switch( secondaryWep );
		m_bDidSwitchWeapon = true;
	}

	return Continue();
}


//--------------------------------------------------------------------------------------------------------
ActionResult< CNEOBot > CNEOBotPathClearBreakable::Update( CNEOBot *me, float interval )
{
	CBaseEntity *breakable = m_hBreakable.Get();

	// If the breakable is destroyed or invalid, we're done
	if ( !breakable || !breakable->IsAlive() || breakable->GetHealth() <= 0 )
	{
		return Done( "Path is clear" );
	}

	if ( m_giveUpTimer.IsElapsed() )
	{
		return Done( "Give up timer elapsed" );
	}

	// Double check distance - if we've moved too far away or the breakable is too far, stop
	if ( me->GetRangeSquaredTo( breakable ) > Square( 500.0f ) )
	{
		return Done( "Breakable is too far" );
	}

	// If freeze time started, bail out
	if ( me->GetNeoFlags() & NEO_FL_FREEZETIME )
	{
		return Done( "Freeze time" );
	}

	// If a visible threat appeared, bail out and let the normal combat system handle it
	if ( IsHeldBackByThreat( me ) )
	{
		return Done( "Threat appeared" );
	}

	auto *myWeapon = static_cast<CNEOBaseCombatWeapon *>( me->GetActiveWeapon() );
	if ( !myWeapon )
	{
		return Done( "No weapon" );
	}

	// Aim at the breakable
	me->GetBodyInterface()->AimHeadTowards(
		breakable->WorldSpaceCenter(), IBody::MANDATORY, 1.0f, nullptr,
		"Attacking breakable in path" );

	// Handle reloading if clip is empty
	bool bUsesClips = myWeapon->UsesClipsForAmmo1();
	if ( bUsesClips && myWeapon->Clip1() <= 0 )
	{
		me->ReleaseFireButton();
		me->PressReloadButton();
		m_isWaitingForFullReload = true;
	}

	if ( m_isWaitingForFullReload )
	{
		m_isWaitingForFullReload = ( bUsesClips && myWeapon->Clip1() < myWeapon->GetMaxClip1() );
	}

	// Fire at the breakable if aim is ready
	if ( !m_isWaitingForFullReload && me->GetBodyInterface()->IsHeadAimingOnTarget() )
	{
		// First check if friendly in line of fire
		trace_t aimTr;
		Vector vForward;
		me->EyeVectors( &vForward );
		Vector vStart = me->EyePosition();
		Vector vEnd = vStart + vForward * 500.0f;
		UTIL_TraceLine( vStart, vEnd, MASK_SHOT_HULL, me->GetEntity(), COLLISION_GROUP_NONE, &aimTr );

		if ( aimTr.m_pEnt )
		{
			if ( aimTr.m_pEnt->IsPlayer() )
			{
				CNEO_Player *pPlayer = ToNEOPlayer( aimTr.m_pEnt );
				if ( pPlayer && pPlayer->InSameTeam( me->GetEntity() ) )
				{
					// Avoid friendly and allow them to break breakable if same path
					me->ReleaseFireButton();
					me->ReleaseForwardButton();
					me->PressBackwardButton();
					if ( m_bStrafeRight )
					{
						me->PressRightButton();
					}
					else
					{
						me->PressLeftButton();
					}

					return Continue();
				}
			}

			// Distance check for knife prioritization
			if ( aimTr.m_pEnt == breakable )
			{
				float flDist = ( aimTr.endpos - vStart ).Length();
				if ( flDist > 50.0f ) // NEO_WEP_KNIFE_RANGE
				{
					m_bSkipKnife = true;
				}
			}
		}

		// If we've been trying to melee for too long, or the target is too far for a knife
		if ( ( m_meleeTimeoutTimer.IsElapsed() || ( m_bSkipKnife && ( myWeapon->GetNeoWepBits() & NEO_WEP_KNIFE ) ) ) &&
			 ( myWeapon->GetNeoWepBits() & NEO_WEP_KNIFE ) )
		{
			auto *secondaryWep = static_cast<CNEOBaseCombatWeapon *>( me->Weapon_GetSlot( 1 ) );
			auto *primaryWeapon = static_cast<CNEOBaseCombatWeapon *>( me->Weapon_GetSlot( 0 ) );

			if ( secondaryWep && ( ( secondaryWep->Clip1() + secondaryWep->m_iPrimaryAmmoCount ) > 0 ) )
			{
				me->Weapon_Switch( secondaryWep );
				m_bDidSwitchWeapon = true;
			}
			else if ( primaryWeapon && ( ( primaryWeapon->Clip1() + primaryWeapon->m_iPrimaryAmmoCount ) > 0 ) )
			{
				me->Weapon_Switch( primaryWeapon );
				m_bDidSwitchWeapon = true;
			}
		}

		// Prevent bot from missing breakable because of leaning
		me->ReleaseLeanLeftButton();
		me->ReleaseLeanRightButton();

		if ( me->IsContinuousFireWeapon( myWeapon ) )
		{
			me->PressFireButton( neo_bot_fire_at_breakable_weapon_min_time.GetFloat() );
		}
		else
		{
			if ( me->m_nButtons & IN_ATTACK )
			{
				me->ReleaseFireButton();
			}
			else
			{
				me->PressFireButton();
			}
		}
	}

	return Continue();
}


//--------------------------------------------------------------------------------------------------------
void CNEOBotPathClearBreakable::OnEnd( CNEOBot *me, Action< CNEOBot > *nextAction )
{
	NeoLogBreakable( me, "end", m_hBreakable.Get(), 0.0f );

	// If we switched weapon, try to switch back to primary
	if ( m_bDidSwitchWeapon )
	{
		auto *myWeapon = static_cast<CNEOBaseCombatWeapon *>( me->GetActiveWeapon() );
		if ( !myWeapon ||
				( myWeapon->GetNeoWepBits() & ( NEO_WEP_MILSO | NEO_WEP_TACHI | NEO_WEP_KYLA | NEO_WEP_KNIFE | NEO_WEP_THROWABLE ) ) )
		{
			auto *primaryWeapon = static_cast<CNEOBaseCombatWeapon *>( me->Weapon_GetSlot( 0 ) );
			if ( primaryWeapon && ( primaryWeapon->Clip1() + primaryWeapon->m_iPrimaryAmmoCount ) > 0 )
			{
				me->Weapon_Switch( primaryWeapon );
			}
		}
	}
}
