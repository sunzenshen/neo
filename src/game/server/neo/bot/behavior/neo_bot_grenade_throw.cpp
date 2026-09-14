#include "cbase.h"
#include "neo_player.h"
#include "bot/neo_bot.h"
#include "bot/neo_bot_path_compute.h"
#include "bot/behavior/neo_bot_grenade_throw.h"
#include "bot/behavior/neo_bot_retreat_from_grenade.h"
#include "weapon_neobasecombatweapon.h"
#include "weapon_grenade.h"
#include "weapon_smokegrenade.h"
#include "movevars_shared.h"
#include "nav_mesh.h"
#include "nav_area.h"

extern ConVar sv_neo_bot_grenade_frag_safety_range_multiplier;
extern ConVar sv_neo_grenade_blast_radius;
extern ConVar sv_neo_grenade_gravity;
extern ConVar sv_neo_grenade_throw_intensity;
extern ConVar sv_neo_grenade_fuse_timer;
extern ConVar sv_neo_grenade_cor;

namespace
{
// CWeaponGrenade::ThrowGrenade derives both launch pitch and launch speed from eye pitch
constexpr float kThrowPitchBias = -10.0f;
constexpr float kThrowSlopeDown = 100.0f / 90.0f;
constexpr float kThrowSlopeUp = 80.0f / 90.0f;
constexpr float kThrowSpeedPerDegree = 6.0f;
constexpr float kThrowForwardOffset = 16.0f; // CNEOBaseProjectile::GetThrowPos when unobstructed
constexpr float kMaxElasticity = 0.9f; // CBaseGrenadeProjectile::ResolveFlyCollisionCustom clamp

constexpr float kSolvePitchUp = -39.0f; // about a 45 degree lob, the longest throw
constexpr float kSolvePitchDown = 60.0f; // short underhand drop
constexpr int kSolveBisections = 8;
constexpr int kSolvePasses = 2;
constexpr int kMaxModelledBounces = 8;
constexpr float kMinSolveDist = 64.0f;
constexpr float kCloseThrowAimCone = 0.98f;
constexpr float kUnreachableMiss = 135.0f; // half the default frag blast radius
constexpr float kThrowLookAtDist = 4096.0f; // far enough that walking barely turns the view off it

struct GrenadeLaunch
{
	Vector vecPos;
	Vector vecVel;
};

GrenadeLaunch ComputeLaunch( const Vector &vecEye, const QAngle &angEye, const Vector &vecOwnerVel )
{
	const float flSlope = ( angEye.x >= 0.0f ) ? kThrowSlopeDown : kThrowSlopeUp;
	const QAngle angThrow( kThrowPitchBias + angEye.x * flSlope, angEye.y, 0.0f );
	Vector vecFwd;
	AngleVectors( angThrow, &vecFwd );
	const float flSpeed = MIN( ( 90.0f - angThrow.x ) * kThrowSpeedPerDegree, sv_neo_grenade_throw_intensity.GetFloat() );

	GrenadeLaunch launch;
	launch.vecPos = vecEye + vecFwd * kThrowForwardOffset;
	launch.vecVel = vecFwd * flSpeed + vecOwnerVel;
	return launch;
}

// Seconds of launch horizontal speed travelled after the first floor impact: every bounce keeps
// elasticity^n of both speed components, until the fuse runs out.
float BounceTravelTime( float flImpactVz, float flFuseLeft, float flGravity )
{
	const float flElasticity = MIN( sv_neo_grenade_cor.GetFloat(), kMaxElasticity );
	float flScale = 1.0f;
	float flTravel = 0.0f;
	for ( int i = 0; i < kMaxModelledBounces && flFuseLeft > 0.0f; ++i )
	{
		flScale *= flElasticity;
		const float flHop = MIN( 2.0f * flImpactVz * flScale / flGravity, flFuseLeft );
		flTravel += flScale * flHop;
		flFuseLeft -= flHop;
	}
	return flTravel;
}

// Where the grenade is when the fuse ends, assuming flat floor at flFloorZ and no walls
bool PredictDetonation( const GrenadeLaunch &launch, float flFloorZ, Vector2D &vecOut )
{
	const float flGravity = GetCurrentGravity() * sv_neo_grenade_gravity.GetFloat();
	const float flDiscriminant = launch.vecVel.z * launch.vecVel.z - 2.0f * flGravity * ( flFloorZ - launch.vecPos.z );
	if ( flGravity <= 0.0f || flDiscriminant < 0.0f )
	{
		return false;
	}

	const float flImpactVz = sqrt( flDiscriminant );
	const float flFlightTime = ( launch.vecVel.z + flImpactVz ) / flGravity;
	if ( flFlightTime <= 0.0f )
	{
		return false;
	}

	const float flFuse = sv_neo_grenade_fuse_timer.GetFloat();
	const float flTravelTime = ( flFlightTime >= flFuse ) ? flFuse : flFlightTime + BounceTravelTime( flImpactVz, flFuse - flFlightTime, flGravity );
	vecOut.x = launch.vecPos.x + launch.vecVel.x * flTravelTime;
	vecOut.y = launch.vecPos.y + launch.vecVel.y * flTravelTime;
	return true;
}

float PredictMiss( const Vector &vecEye, const QAngle &angEye, const Vector &vecOwnerVel, const Vector &vecTarget )
{
	Vector2D vecDetonation;
	if ( !PredictDetonation( ComputeLaunch( vecEye, angEye, vecOwnerVel ), vecTarget.z, vecDetonation ) )
	{
		return FLT_MAX;
	}
	return ( vecDetonation - vecTarget.AsVector2D() ).Length();
}

// Bisect eye pitch for range along the aim yaw; the second pass shifts the aim by the remaining
// miss, which absorbs the thrower's own velocity. False if no arc reaches the target's height.
bool SolveThrowAngles( const Vector &vecEye, const Vector &vecOwnerVel, const Vector &vecTarget, QAngle &angOut )
{
	const Vector2D vecToTarget = vecTarget.AsVector2D() - vecEye.AsVector2D();
	Vector2D vecAim = vecToTarget;
	angOut.Init();

	for ( int iPass = 0; iPass < kSolvePasses; ++iPass )
	{
		angOut.y = RAD2DEG( atan2( vecAim.y, vecAim.x ) );
		const Vector2D vecDir( cos( DEG2RAD( angOut.y ) ), sin( DEG2RAD( angOut.y ) ) );
		const float flWantRange = vecToTarget.Dot( vecDir );

		float flUp = kSolvePitchUp;
		float flDown = kSolvePitchDown;
		for ( int i = 0; i < kSolveBisections; ++i )
		{
			angOut.x = 0.5f * ( flUp + flDown );
			Vector2D vecDetonation;
			if ( !PredictDetonation( ComputeLaunch( vecEye, angOut, vecOwnerVel ), vecTarget.z, vecDetonation )
				|| ( vecDetonation - vecEye.AsVector2D() ).Dot( vecDir ) < flWantRange )
			{
				flDown = angOut.x; // short: look further up
			}
			else
			{
				flUp = angOut.x;
			}
		}

		Vector2D vecDetonation;
		if ( !PredictDetonation( ComputeLaunch( vecEye, angOut, vecOwnerVel ), vecTarget.z, vecDetonation ) )
		{
			return false;
		}
		vecAim += vecTarget.AsVector2D() - vecDetonation;
	}
	return true;
}

// Predicted miss at which a bot lets go
float GetThrowTolerance( const CNEOBot *me )
{
	const float flBlastRadius = sv_neo_grenade_blast_radius.GetFloat();
	switch ( me->GetDifficulty() )
	{
	case CNEOBot::EXPERT:
		return 0.4f * flBlastRadius;
	case CNEOBot::HARD:
		return 0.6f * flBlastRadius;
	case CNEOBot::NORMAL:
		return 0.8f * flBlastRadius;
	case CNEOBot::EASY:
	default:
		return flBlastRadius;
	}
}
} // namespace

ConVar sv_neo_bot_grenade_debug_behavior("sv_neo_bot_grenade_debug_behavior", "0", FCVAR_CHEAT,
	"Draw debug overlays for bot grenade behavior", true, 0, true, 1);

ConVar sv_neo_bot_grenade_give_up_time("sv_neo_bot_grenade_give_up_time", "5.0", FCVAR_NONE,
	"Time in seconds before bot gives up on grenade throw", true, 1, false, 0);

//---------------------------------------------------------------------------------------------
CNEOBotGrenadeThrow::CNEOBotGrenadeThrow( CNEOBaseCombatWeapon *pWeapon, const CKnownEntity *threat )
{
	m_bFocusedOnThrow = false;
	m_bPinPulled = false;
	m_hGrenadeWeapon = pWeapon;
	m_bVantagePointBlocked = false;
	m_vantageArea = nullptr;
	m_vecTarget = vec3_invalid;
	m_vecThrowLookAt = vec3_invalid;
	m_angThrowSolved.Init( FLT_MAX, 0.0f, 0.0f );

	if ( threat )
	{
		m_hThreatGrenadeTarget = threat->GetEntity();
		m_vecThreatLastKnownPos = threat->GetLastKnownPosition();
	}
	else
	{
		m_hThreatGrenadeTarget = nullptr;
		m_vecThreatLastKnownPos = vec3_invalid;
	}
}

//---------------------------------------------------------------------------------------------
// Search for the nearest area that has a potential view of the targeted grenade area.
// Returns nullptr if the targeted position is off the nav mesh or if no visible area exists.
CNavArea *CNEOBotGrenadeThrow::FindVantageArea( CNEOBot *me )
{
	CNavArea *targetArea = TheNavMesh->GetNearestNavArea( m_vecThreatLastKnownPos );
	if ( !targetArea )
	{
		return nullptr;
	}

	CNEOFindClosestPotentiallyVisibleAreaToPos find( me->GetAbsOrigin() );
	targetArea->ForAllPotentiallyVisibleAreas( find );
	return find.m_closeArea;
}

//---------------------------------------------------------------------------------------------
// Aim so the grenade detonates near m_vecTarget, allowing for the launch coupling, own velocity,
// bounces and the fuse. Throws closer than kMinSolveDist just look at the target.
CNEOBotGrenadeThrow::ThrowAimResult CNEOBotGrenadeThrow::UpdateThrowAim( CNEOBot *me )
{
	const Vector vecEye = me->EyePosition();
	if ( ( m_vecTarget - vecEye ).Length2D() < kMinSolveDist )
	{
		me->GetBodyInterface()->AimHeadTowards( m_vecTarget, IBody::MANDATORY, 0.2f, nullptr, "Aiming grenade" );

		Vector vecForward;
		me->EyeVectors( &vecForward );
		Vector vecToTarget = m_vecTarget - vecEye;
		vecToTarget.NormalizeInPlace();
		return ( vecForward.Dot( vecToTarget ) >= kCloseThrowAimCone ) ? THROW_AIM_READY : THROW_AIM_TURNING;
	}

	const float flTolerance = GetThrowTolerance( me );
	const Vector vecVel = me->GetAbsVelocity();

	// Re-solve only once the solution drifts: AimHeadTowards ignores a new point mid-turn
	if ( m_angThrowSolved.x == FLT_MAX || PredictMiss( vecEye, m_angThrowSolved, vecVel, m_vecTarget ) > 0.5f * flTolerance )
	{
		if ( !SolveThrowAngles( vecEye, vecVel, m_vecTarget, m_angThrowSolved )
			|| PredictMiss( vecEye, m_angThrowSolved, vecVel, m_vecTarget ) > MAX( flTolerance, kUnreachableMiss ) )
		{
			m_angThrowSolved.x = FLT_MAX;
			return THROW_AIM_UNREACHABLE;
		}

		Vector vecDir;
		AngleVectors( m_angThrowSolved, &vecDir );
		m_vecThrowLookAt = vecEye + vecDir * kThrowLookAtDist;
	}

	me->GetBodyInterface()->AimHeadTowards( m_vecThrowLookAt, IBody::MANDATORY, 0.2f, nullptr, "Aiming grenade" );

	// Release on where the current eye angles and velocity would put the grenade, not on angle error
	return ( PredictMiss( vecEye, me->LocalEyeAngles(), vecVel, m_vecTarget ) <= flTolerance ) ? THROW_AIM_READY : THROW_AIM_TURNING;
}

//---------------------------------------------------------------------------------------------
// Take over the bot's weapon handling and looking while the throw is in progress.
// Paired with EndThrowFocus - anything added here needs its inverse added there.
void CNEOBotGrenadeThrow::BeginThrowFocus( CNEOBot *me )
{
	m_bFocusedOnThrow = true;

	// Swap to the grenade
	me->PushRequiredWeapon( m_hGrenadeWeapon );

	// Stop distracting looking or weapon handling behavior that could interrupt the throw
	me->StopLookingAroundForEnemies(); // reduce distractions for manual look aiming
	me->SetAttribute( CNEOBot::IGNORE_ENEMIES ); // suppress reaction to swap back to firearm
}

//---------------------------------------------------------------------------------------------
// Undo BeginThrowFocus. Safe to call unconditionally, because the behavior framework
// runs OnEnd even when OnStart returns Done before ever committing to the throw.
void CNEOBotGrenadeThrow::EndThrowFocus( CNEOBot *me )
{
	if ( !m_bFocusedOnThrow )
	{
		return;
	}
	m_bFocusedOnThrow = false;

	// Restore looking and weapon handling behaviors
	me->PopRequiredWeapon();
	me->StartLookingAroundForEnemies();
	me->ClearAttribute( CNEOBot::IGNORE_ENEMIES );
	const CKnownEntity *threat = me->GetVisionInterface()->GetPrimaryKnownThreat();
	me->EquipBestWeaponForThreat( threat );
}

//---------------------------------------------------------------------------------------------
ActionResult< CNEOBot >	CNEOBotGrenadeThrow::OnStart( CNEOBot *me, Action< CNEOBot > *priorAction )
{
	CNEOBaseCombatWeapon *pWep = m_hGrenadeWeapon.Get();
	if ( !pWep )
	{
		return Done( "Grenade input invalid" );
	}
	Assert( ( pWep->GetNeoWepBits() & NEO_WEP_FRAG_GRENADE ) || ( pWep->GetNeoWepBits() & NEO_WEP_SMOKE_GRENADE ) );

	if ( !m_hThreatGrenadeTarget.Get() )
	{
		return Done( "Targeted Threat is null" );
	}

	if ( !m_hThreatGrenadeTarget->IsAlive() )
	{
		return Done( "Targeted threat is dead" );
	}

	if ( m_vecThreatLastKnownPos == vec3_invalid )
	{
		return Done( "Targeted threat position invalid" );
	}

	// Smoke grenades don't need vantage point planning since they don't have unsafe radius.
	if ( !( pWep->GetNeoWepBits() & NEO_WEP_SMOKE_GRENADE ) )
	{
		m_vantageArea = FindVantageArea( me );
		if ( !m_vantageArea )
		{
			return Done( "No viable vantage point for grenade throw" );
		}
	}

	// We have decided to commit to the throw by this point
	BeginThrowFocus( me );

	m_giveUpTimer.Start( sv_neo_bot_grenade_give_up_time.GetFloat() );
	m_bPinPulled = false;

	m_PathFollower.SetMinLookAheadDistance( me->GetDesiredPathLookAheadRange() );
	CNEOBotPathCompute( me, m_PathFollower, m_vantageArea ? m_vantageArea->GetCenter() : m_vecThreatLastKnownPos, FASTEST_ROUTE );

	return Continue();
}


//---------------------------------------------------------------------------------------------
ActionResult< CNEOBot >	CNEOBotGrenadeThrow::Update( CNEOBot *me, float interval )
{
	Assert( me );

	CNEOBaseCombatWeapon *pWep = m_hGrenadeWeapon.Get();
	if ( !pWep )
	{
		return Done( "Do not have grenade" );
	}

	if ( !pWep->HasPrimaryAmmo() )
	{
		return Done( "Weapon slot out of ammo" );
	}

	if ( m_vecThreatLastKnownPos == vec3_invalid )
	{
		return Done( "Targeted threat position invalid" );
	}

	if ( !m_hThreatGrenadeTarget.Get() )
	{
		return Done( "Targeted threat is null" );
	}

	if ( !m_hThreatGrenadeTarget->IsAlive() )
	{
		return Done( "Targeted threat is dead" );
	}

	if ( m_giveUpTimer.IsElapsed() )
	{
		return Done( "Grenade throw timed out" );
	}

	if ( gpGlobals->curtime - me->GetLastDamageTime() < 0.5f )
	{
		return Done( "Actively getting hurt, too dangerous to throw grenade" );
	}

	if ( sv_neo_bot_grenade_debug_behavior.GetBool() && m_hThreatGrenadeTarget.Get() )
	{
		// DEBUG Colors: Red = Frag, Gray = Smoke, Purple = Unknown
		const Vector& vecStart = me->WorldSpaceCenter();
		const Vector& vecVictim = m_hThreatGrenadeTarget->GetAbsOrigin();
		int r = 255, g = 0, b = 255; // Default purple just in case
		if ( pWep->GetNeoWepBits() & NEO_WEP_FRAG_GRENADE )
		{
			r = 255; g = 0; b = 0; // Red
		}
		else if ( pWep->GetNeoWepBits() & NEO_WEP_SMOKE_GRENADE )
		{
			r = 128; g = 128; b = 128; // Gray
		}
		// Draw box around the bot that is considering the throw
		NDebugOverlay::Box( me->GetAbsOrigin(), me->WorldAlignMins(), me->WorldAlignMaxs(), r, g, b, 30, 0.1f );
		// Arrow from bot to threat being targeted
		NDebugOverlay::HorzArrow( vecStart, vecVictim, 2.0f, r, g, b, 255, true, 0.1f );
	}

	me->EnableCloak(3.0f);

	if ( me->GetActiveWeapon() != pWep )
	{
		// Still waiting to switch
		me->Weapon_Switch( pWep );
		return Continue();
	}

	// NEOJANK: The bots struggle to throw grenades with PressFireButton due to control quirks.
	// As a workaround, we decompose the action into different phases of the bot behavior.
	// This also allows us to run aiming logic in parallel with the pin-pull animation.
	if (!m_bPinPulled)
	{
		// Wait for weapon switch to complete fully
		if ( pWep->m_flNextPrimaryAttack > gpGlobals->curtime )
		{
			return Continue();
		}
		// Just play the animation. Do NOT call PrimaryAttack, as that sets up the weapon to 
		// auto-throw via ItemPostFrame, which we want to control manually here.
		pWep->SendWeaponAnim( ACT_VM_PULLPIN );
		pWep->m_flTimeWeaponIdle = FLT_MAX; // Don't let idle anims interrupt us
		m_bPinPulled = true;
	}

	ThrowTargetResult result = UpdateGrenadeTargeting( me, pWep );

	// Subclasses may decide in the last second that a throw is too dangerous
	if ( result == THROW_TARGET_CANCEL )
	{
		return Done( "Grenade throw aborted" );
	}

	bool bAimOnTarget = false;

	if ( result == THROW_TARGET_READY )
	{
		if ( m_vecTarget == vec3_invalid )
		{
			return Done( "Invalid target coordinate" );
		}

		const ThrowAimResult aim = UpdateThrowAim( me );
		if ( aim == THROW_AIM_UNREACHABLE )
		{
			return Done( "Grenade target out of throwing range" );
		}

		if ( aim == THROW_AIM_READY )
		{
			if ( me->IsThrowLineClear( m_vecTarget ) )
			{
				bAimOnTarget = true;
			}
			else
			{
				// Blocked by an obstruction
				if ( sv_neo_bot_grenade_debug_behavior.GetBool() )
				{
					NDebugOverlay::Line( me->EyePosition(), m_vecTarget, 0, 0, 255, true, 0.1f );
				}

				m_PathFollower.Update( me );

				if ( m_repathTimer.IsElapsed() )
				{
					m_repathTimer.Start( RandomFloat( 1.0f, 2.0f ) );
					CNEOBotPathCompute( me, m_PathFollower, m_vecTarget, FASTEST_ROUTE );
				}
			}
		}
	}
	else if ( result == THROW_TARGET_WAIT )
	{
		m_PathFollower.Update( me );

		// Watch for threats emerging from known position
		if ( m_vecThreatLastKnownPos != vec3_invalid )
		{
			me->GetBodyInterface()->AimHeadTowards(
				m_vecThreatLastKnownPos, IBody::IMPORTANT, 0.1f, nullptr,
				"Looking towards last known threat position while moving to vantage point");
		}

		bool bJustArrivedBlocked = false;
		if ( m_vantageArea && me->GetLastKnownArea() == m_vantageArea )
		{
			// Arrived at vantage point but still blocked - fallback to chase now,
			// rather than continuing to wait out the repath timer at the dead end
			m_bVantagePointBlocked = true;
			m_vantageArea = nullptr;
			bJustArrivedBlocked = true;
		}

		if ( m_repathTimer.IsElapsed() || bJustArrivedBlocked )
		{
			m_repathTimer.Start( RandomFloat( 1.0f, 2.0f ) );

			if ( !m_vantageArea && !m_bVantagePointBlocked )
			{
				m_vantageArea = FindVantageArea( me );

				if ( !m_vantageArea )
				{
					return Done( "Failed to find a vantage area to throw grenade from" );
				}
			}

			bool bGoingToVantagePoint = false;
			if ( m_vantageArea )
			{
				bGoingToVantagePoint = CNEOBotPathCompute( me, m_PathFollower, m_vantageArea->GetCenter(), FASTEST_ROUTE );
			}

			if ( !bGoingToVantagePoint )
			{
				// Fallback to direct chase behavior if vantage point is blocked
				if ( m_vantageArea )
				{
					m_vantageArea = nullptr;
					m_bVantagePointBlocked = true;
				}
				CNEOBotPathCompute( me, m_PathFollower, m_vecTarget != vec3_invalid ? m_vecTarget : m_vecThreatLastKnownPos, FASTEST_ROUTE );
			}
		}
	}
	
	// DEBUG: Targeting decisions
	if ( sv_neo_bot_grenade_debug_behavior.GetBool() && m_vecTarget != vec3_invalid )
	{
		// Color: Orange (255, 128, 0) is the final ballistics target
		const Vector& vecStart = me->WorldSpaceCenter();
		NDebugOverlay::HorzArrow( vecStart, m_vecTarget, 2.0f, 255, 128, 0, 255, true, 2.0f );
		NDebugOverlay::Box( m_vecTarget, Vector(-16,-16,-16), Vector(16,16,16), 255, 128, 0, 30, 2.0f );

		if ( m_vantageArea )
		{
			NDebugOverlay::Box( m_vantageArea->GetCenter(), Vector(-16,-16,0), Vector(16,16,16), 0, 255, 0, 30, 0.1f );
			NDebugOverlay::Line( me->EyePosition(), m_vantageArea->GetCenter(), 0, 255, 0, true, 0.1f );
		}
	}

	// NEO JANK: Force the throwing phase if aimed.
	// We ignore bWeaponReady/m_flNextPrimaryAttack because we are manually controlling the throw
	// and want to bypass the weapon's internal scheduled throw to ensure it happens NOW.
	if ( bAimOnTarget )
	{
		OnThrowReleased( me );

		// Play first person throw animation
		pWep->SendWeaponAnim( ACT_VM_THROW );

		if ( pWep->GetNeoWepBits() & NEO_WEP_FRAG_GRENADE )
		{
			static_cast<CWeaponGrenade*>(pWep)->ThrowGrenade( me );
		}
		else if ( pWep->GetNeoWepBits() & NEO_WEP_SMOKE_GRENADE )
		{
			static_cast<CWeaponSmokeGrenade*>(pWep)->ThrowGrenade( me );
		}
		else
		{
			DevWarning( "CNEOBotGrenadeThrow: Unknown grenade type!\n" );
			Assert(0);
		}

		// Would exit immediately for smoke, but enemies tend to frag back so look for one now
		return ChangeTo( new CNEOBotRetreatFromGrenade( nullptr ), "Retreating after throw" );
	}
	
	return Continue();
}

//---------------------------------------------------------------------------------------------
void CNEOBotGrenadeThrow::OnEnd( CNEOBot *me, Action< CNEOBot > *nextAction )
{
	if (!NEORules()) // This can get called on de-init
	{
		return;
	}

	// No-ops if OnStart bailed out before committing to the throw
	EndThrowFocus( me );
}

//---------------------------------------------------------------------------------------------
QueryResultType CNEOBotGrenadeThrow::ShouldRetreat( const INextBot *me ) const
{
	// Avoid tactical monitor interrupting the grenade throw behavior
	return ANSWER_NO;
}

//---------------------------------------------------------------------------------------------
ActionResult<CNEOBot> CNEOBotGrenadeThrow::OnSuspend( CNEOBot *me, Action<CNEOBot> *interruptingAction )
{
	return Done( "OnSuspend: Cancel out of grenade throw behavior, situation will likely become stale." );
	// OnEnd will get called after Done
}

//---------------------------------------------------------------------------------------------
ActionResult<CNEOBot> CNEOBotGrenadeThrow::OnResume( CNEOBot *me, Action<CNEOBot> *interruptingAction )
{
	return Done( "OnResume: Cancel out of grenade throw behavior, situation is likely stale." );
	// OnEnd will get called after Done
}
