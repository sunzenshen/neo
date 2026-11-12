#include "cbase.h"
#include "neo_smokegrenade.h"
#include "neo_smokelineofsightblocker.h"
#include "neo_smoke_volume.h"
#include "neo_smoke_shared.h"

#include "neo_tracefilter_collisiongroupdelta.h"
#include "particle_smokegrenade.h"
#include "tempentity.h"

#include "mathlib/vector.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

#define NADE_SOLID_TYPE SolidType_t::SOLID_BBOX

LINK_ENTITY_TO_CLASS(neo_grenade_smoke, CNEOGrenadeSmoke);

BEGIN_DATADESC(CNEOGrenadeSmoke)
// Fields
DEFINE_FIELD(m_inSolid, FIELD_BOOLEAN),
DEFINE_FIELD(m_punted, FIELD_BOOLEAN),
DEFINE_FIELD(m_hasSettled, FIELD_BOOLEAN),

// Function Pointers
DEFINE_THINKFUNC(DelayThink),

// Inputs
DEFINE_INPUTFUNC(FIELD_FLOAT, "SetTimer", InputSetTimer),
END_DATADESC()

ConVar sv_neo_smoke_bloom_duration("sv_neo_smoke_bloom_duration", "25", FCVAR_CHEAT, "How long should the smoke bloom be up, in seconds.", true, 0.0, true, 60.0);
ConVar sv_neo_smoke_bloom_layers("sv_neo_smoke_bloom_layers", "2", FCVAR_CHEAT, "Amount of smoke layers atop each other.", true, 0.0, true, 32.0);
ConVar sv_neo_smoke_blocker_size("sv_neo_smoke_blocker_size", "140", FCVAR_CHEAT, "Size of each LOS blocker per smoke particle.", true, 0.0, true, 200.0);
ConVar sv_neo_smoke_blocker_delay("sv_neo_smoke_blocker_delay", "1.0", FCVAR_CHEAT, "Time after spawn before LOS blocker activates LOS blocking", true, 0.0, true, 10.0);
// Depth below the water surface where a smoke grenade stops and detonates.
// Deep enough that the whole grenade is under water.
static constexpr float NEO_SMOKE_WATER_HOLD_DEPTH = 6.0f;
// How far above the grenade to look for the water surface.
static constexpr float NEO_SMOKE_WATER_SURFACE_SEARCH = 32.0f;

extern ConVar sv_neo_grenade_cor;
extern ConVar sv_neo_grenade_gravity;
extern ConVar sv_neo_grenade_friction;
void CNEOGrenadeSmoke::Spawn(void)
{
	Precache();
	SetModel(NEO_SMOKE_GRENADE_MODEL);
	BaseClass::Spawn();

	m_flDamage = 0;
	m_DmgRadius = 0;
	m_takedamage = DAMAGE_YES;
	m_iHealth = 1;

	m_punted = false;
	m_hasSettled = false;
	m_hasBeenMadeNonSolid = false;
	m_lastPos = GetAbsOrigin();

	SetElasticity(sv_neo_grenade_cor.GetFloat());
	SetGravity(sv_neo_grenade_gravity.GetFloat());
	SetFriction(sv_neo_grenade_friction.GetFloat());
	SetCollisionGroup(COLLISION_GROUP_PROJECTILE);
	SetDetonateTimerLength(FLT_MAX);

	SetThink(&CNEOGrenadeSmoke::DelayThink);
	SetNextThink(gpGlobals->curtime);
}

void CNEOGrenadeSmoke::OnPhysGunPickup(CBasePlayer* pPhysGunUser, PhysGunPickup_t reason)
{
	SetThrower(pPhysGunUser);
	m_bHasWarnedAI = true;

	BaseClass::OnPhysGunPickup(pPhysGunUser, reason);
}

int CNEOGrenadeSmoke::OnTakeDamage(const CTakeDamageInfo& inputInfo)
{
	// NEO smokes can't be detonated by damage.
	return 0;
}

void CNEOGrenadeSmoke::Splash()
{
	// Without this the grenade sinks to the pool floor and the smoke starts there.
	// Holding it just under the surface keeps the cloud where the grenade went in.
	const Vector vecOrigin = GetAbsOrigin();

	// Splash() runs as soon as the bottom of the grenade is wet, while its center can still be dry,
	// so the surface search starts at the bottom: a dry start would be taken as the surface.
	Vector vecBottom;
	CollisionProp()->NormalizedToWorldSpace(Vector(0.5f, 0.5f, 0.0f), &vecBottom);
	const float flSurfaceZ = UTIL_WaterLevel(vecBottom, vecBottom.z, vecBottom.z + NEO_SMOKE_WATER_SURFACE_SEARCH);
	const Vector vecHold(vecOrigin.x, vecOrigin.y, flSurfaceZ - NEO_SMOKE_WATER_HOLD_DEPTH);

	// Shallow water: the floor stops the grenade above the hold depth.
	trace_t tr;
	UTIL_TraceEntity(this, vecOrigin, vecHold, MASK_SOLID, &tr);
	SetAbsOrigin(tr.endpos);

	SetAbsVelocity(vec3_origin);
	SetLocalAngularVelocity(vec3_angle);
	SetMoveType(MOVETYPE_NONE);
}

void CNEOGrenadeSmoke::DelayThink()
{
	if (TryDetonate())
	{
		return;
	}

	if (!m_bHasWarnedAI && gpGlobals->curtime >= m_flWarnAITime)
	{
#if !defined( CLIENT_DLL )
		CSoundEnt::InsertSound(SOUND_DANGER, GetAbsOrigin(), 400, 1.5, this);
#endif
		m_bHasWarnedAI = true;
	}

	m_hasSettled = CloseEnough(m_lastPos, GetAbsOrigin(), 0.1f);
	m_lastPos = GetAbsOrigin();

	SetNextThink(gpGlobals->curtime + 0.1);
}


bool CNEOGrenadeSmoke::TryDetonate(void)
{
	if (m_hasSettled && (gpGlobals->curtime > GetDetonateTimerLength()))
	{
		Detonate();
		return true;
	}

	return false;
}

void CNEOGrenadeSmoke::Detonate(void)
{
#if(0)
	Vector randVec;
	randVec.Random((-sv_neo_smoke_bloom_radius.GetFloat() * 0.05f), (sv_neo_smoke_bloom_radius.GetFloat() * 0.05f));
	UTIL_Smoke(GetAbsOrigin() + randVec, sv_neo_smoke_bloom_radius.GetFloat(), 1.0f);
#endif

	if (!m_hasBeenMadeNonSolid)
	{
		m_hasBeenMadeNonSolid = true;

		SetupParticles(sv_neo_smoke_bloom_layers.GetInt());
		m_flSmokeBloomTime = gpGlobals->curtime;
		if (sv_neo_smoke_volumetric.GetBool())
		{
			NEOSmokeVolume_Create(GetAbsOrigin(), this);
		}

		CRecipientFilter filter;
		filter.AddRecipientsByPAS(GetAbsOrigin());
		EmitSound_t type;
		type.m_pSoundName = "Smoke.Explode";
		EmitSound(filter, edict()->m_EdictIndex, type);

		SetTouch(NULL);
		SetSolid(SOLID_NONE);
		SetAbsVelocity(vec3_origin);
		SetMoveType(MOVETYPE_NONE);
	}
	else if (gpGlobals->curtime - m_flSmokeBloomTime >= sv_neo_smoke_bloom_duration.GetFloat())
	{
		BecomeDebris();
		return;
	}
	
	SetNextThink(gpGlobals->curtime + 1.0f);
}

void CNEOGrenadeSmoke::BecomeDebris()
{
	// The spent shell would otherwise stay networked and drawn until the round ends.
	// Clients replace it with a local physics prop, so it costs no edict.
	CPVSFilter filter(GetAbsOrigin());
	te->PhysicsProp(filter, 0.0f, GetModelIndex(), m_nSkin, GetAbsOrigin(), GetAbsAngles(), vec3_origin,
		TE_PHYSPROPFLAG_DEBRIS, 0);

	UTIL_Remove(this);
}

void CNEOGrenadeSmoke::SetupParticles(size_t number)
{
	// In volumetric mode the server's smoke volume draws the cloud.
	const bool legacyParticles = !sv_neo_smoke_volumetric.GetBool();
	for (size_t i = 0; i < number; ++i)
	{
		ParticleSmokeGrenade *ptr = nullptr;
		if (legacyParticles)
		{
			ptr = dynamic_cast<ParticleSmokeGrenade*>(CreateEntityByName(PARTICLESMOKEGRENADE_ENTITYNAME));
			Assert(ptr);
		}
		if (ptr)
		{
			Vector vForward;
			AngleVectors(GetLocalAngles(), &vForward);
			vForward.z = 0;
			VectorNormalize(vForward);

			ptr->SetLocalOrigin(GetLocalOrigin());
			constexpr int smokeFadeStartTime = 17;
			constexpr int smokeFadeEndTime = 25;
			ptr->SetFadeTime(smokeFadeStartTime, smokeFadeEndTime);
			ptr->Activate();
			ptr->SetLifetime(sv_neo_smoke_bloom_duration.GetFloat()); // This call passes on responsibility for the memory to the particle thinkfunc
			ptr->FillVolume();
		}

		// Currently redundant to create a blocker for each particle, but will be handy for dynamic flood fill later
		auto blocker = static_cast<CNEOSmokeLineOfSightBlocker*>(CreateEntityByName(SMOKELINEOFSIGHTBLOCKER_ENTITYNAME));
		if (blocker) {
			// Use `ai_debug_los 2` after spawn to visualize relative blocker size
			int blockerSize = sv_neo_smoke_blocker_size.GetInt();
			// Overlap with each particle's position
			blocker->SetAbsOrigin(GetAbsOrigin());
			blocker->SetBounds(Vector(-blockerSize), Vector(blockerSize));
			// Schedule activation of LOS blocking
			blocker->SetThink(&CNEOSmokeLineOfSightBlocker::ActivateLOSBlocker);
			blocker->SetNextThink(gpGlobals->curtime + sv_neo_smoke_blocker_delay.GetFloat());
		}
	}
}

void CNEOGrenadeSmoke::InputSetTimer(inputdata_t& inputdata)
{
	SetDetonateTimerLength(inputdata.value.Float());
}

CBaseGrenadeProjectile* NEOSmokegrenade_Create(const Vector& position, const QAngle& angles, const Vector& velocity,
	const AngularImpulse& angVelocity, CBaseEntity* pOwner)
{
	// Don't set the owner here, or the player can't interact with grenades he's thrown
	CNEOGrenadeSmoke* pGrenade = (CNEOGrenadeSmoke*)CBaseEntity::Create("neo_grenade_smoke", position, angles, pOwner);

	pGrenade->SetAbsVelocity(velocity);
	pGrenade->ApplyLocalAngularVelocityImpulse(angVelocity);
	pGrenade->SetupInitialTransmittedGrenadeVelocity(velocity);

	// We won't know when to detonate until the smoke has settled.
	pGrenade->SetNextThink(gpGlobals->curtime);
	pGrenade->SetThrower(ToBaseCombatCharacter(pOwner));
	if (pOwner) pGrenade->ChangeTeam(pOwner->GetTeamNumber());
	pGrenade->m_takedamage = DAMAGE_EVENTS_ONLY;

	return pGrenade;
}

bool NEOSmokegrenade_WasPunted(const CBaseEntity* pEntity)
{
	auto pSmoke = dynamic_cast<const CNEOGrenadeSmoke*>(pEntity);
	if (pSmoke)
	{
		return pSmoke->WasPunted();
	}

	return false;
}
