#include "cbase.h"
#include "weapon_srs.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

static constexpr float SRS_BOLT_REFIRE_TIME = 1.27f;

IMPLEMENT_NETWORKCLASS_ALIASED(WeaponSRS, DT_WeaponSRS)

BEGIN_NETWORK_TABLE(CWeaponSRS, DT_WeaponSRS)
	DEFINE_NEO_BASE_WEP_NETWORK_TABLE
#ifdef CLIENT_DLL
	RecvPropBool(RECVINFO(m_bNeedsBolting)),
#else
	SendPropBool(SENDINFO(m_bNeedsBolting)),
#endif
END_NETWORK_TABLE()

#ifdef CLIENT_DLL
BEGIN_PREDICTION_DATA(CWeaponSRS)
	DEFINE_NEO_BASE_WEP_PREDICTION
	DEFINE_PRED_FIELD(m_bNeedsBolting, FIELD_BOOLEAN, FTYPEDESC_INSENDTABLE),
END_PREDICTION_DATA()
#endif

NEO_IMPLEMENT_ACTTABLE(CWeaponSRS)

LINK_ENTITY_TO_CLASS(weapon_srs, CWeaponSRS);

PRECACHE_WEAPON_REGISTER(weapon_srs);

CWeaponSRS::CWeaponSRS()
{
	m_flSoonestAttack = gpGlobals->curtime;
	m_flAccuracyPenalty = 0;

	m_nNumShotsFired = 0;
	m_bNeedsBolting = false;

	m_weaponSeeds = {
		"srspx",
		"srspy",
		"srsrx",
		"srsry",
	};
}

void CWeaponSRS::PrimaryAttack()
{
	if (ShootingIsPrevented() || !m_iClip1 || !m_bTriggerReset)
	{
		Assert(!(m_iClip1 < 0));
		return BaseClass::PrimaryAttack();
	}

	const bool tryingToShootTooFast =
		(gpGlobals->curtime < m_flLastAttackTime + GetFireRate());
	if (tryingToShootTooFast)
	{
		return;
	}

	m_bNeedsBolting = true;
	BaseClass::PrimaryAttack();
}

void CWeaponSRS::ItemPreFrame()
{
	if (!m_bNeedsBolting)
	{
		return BaseClass::ItemPreFrame();
	}

	const auto* pOwner = ToNEOPlayer(GetOwner());
	if (pOwner && !(pOwner->m_nButtons & IN_ATTACK))
	{
		m_flNextPrimaryAttack = Max(m_flNextPrimaryAttack.Get(), gpGlobals->curtime + SRS_BOLT_REFIRE_TIME);
		SendWeaponAnim(ACT_VM_PULLBACK);
		WeaponSound(SPECIAL1);
		m_bNeedsBolting = false;
	}

	BaseClass::ItemPreFrame();
}

bool CWeaponSRS::Deploy()
{
	// This is on purpose; the player is allowed to cancel the bolting animation
	// with a quick-switch input.
	m_bNeedsBolting = false;

	return BaseClass::Deploy();
}

bool CWeaponSRS::Reload()
{
	// The reloading animation already contains the bolting motion;
	// we don't want to bolt twice.
	m_bNeedsBolting = false;

	return BaseClass::Reload();
}

bool CWeaponSRS::CanBePickedUpByClass(int classId)
{
	return classId != NEO_CLASS_JUGGERNAUT;
}
