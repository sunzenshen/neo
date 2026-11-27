//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose:
//
//=============================================================================//

#include <KeyValues.h>
#include "neo_weapon_parse.h"


FileWeaponInfo_t* CreateWeaponInfo()
{
	return new CNEOWeaponInfo;
}


CNEOWeaponInfo::CNEOWeaponInfo()
{
	m_iBullets = 0;
	m_flCycleTime = 0.f;
	szViewModel2[0] = 0;
	szBulletCharacter[0] = 0;
	szDeathIcon[0] = 0;
	m_flPenetration = 0.f;
	m_bDropOnDeath = true;
	iAimFOV = 0;

	m_flVMFov = m_flVMAimFov = m_flVMAdsFov = 0.f;
	m_bHasAds = false;
	m_flAdsRecoilVertical = m_flAdsRecoilSide = m_flAdsRecoilBack = 1.f;
	m_flAdsRecoilMaxDist = m_flAdsRecoilMaxAngle = 0.f;
	m_vecVMPosOffset = m_vecVMAimPosOffset = m_vecVMAdsPosOffset = vec3_origin;
	m_angVMAngOffset = m_angVMAimAngOffset = m_angVMAdsAngOffset = vec3_angle;
}


// Weapon-script names of the reload phases, by ReloadPhase_t
static constexpr const char *RELOAD_PHASE_NAMES[RELOAD_PHASE_COUNT] = { "", "1_unload", "2_load", "3_cycle" };
// Weapon-script keys of a phase's sound pieces: { sound name, time }
static constexpr const char *RELOAD_SOUND_KEYS[RELOAD_SOUNDS_PER_ROW][2] = {
	{ "sound_name", "sound" }, { "sound2_name", "sound2" }, { "sound3_name", "sound3" }
};

static ReloadPhase_t ReloadPhaseFromName(const char *pszName)
{
	for (int i = RELOAD_PHASE_UNLOADED; i < RELOAD_PHASE_COUNT; ++i)
	{
		if (V_stricmp(pszName, RELOAD_PHASE_NAMES[i]) == 0)
		{
			return static_cast<ReloadPhase_t>(i);
		}
	}
	return RELOAD_PHASE_NONE;
}

// "ReloadPhases" { "<sequence>" { "<phase>" { "ammo" "s" "sound" "s" "sound_name" "name" "resume" "sequence" } ... } ... }
void CNEOWeaponInfo::ParseReloadPhases(KeyValues *pBlock)
{
	m_reloadTables.Purge();
	if (!pBlock)
	{
		return;
	}

	if (!pBlock->GetFirstTrueSubKey())
	{
		Warning("%s: \"ReloadPhases\" is empty (is its neoAssets #base file missing?); reloading without phases\n", szClassName);
		return;
	}

	for (KeyValues *pSeq = pBlock->GetFirstTrueSubKey(); pSeq && m_reloadTables.Count() < RELOAD_PHASE_TABLES_MAX; pSeq = pSeq->GetNextTrueSubKey())
	{
		ReloadPhaseTable_t &table = m_reloadTables[m_reloadTables.AddToTail()];
		V_strncpy(table.szSequence, pSeq->GetName(), sizeof(table.szSequence));
		table.iNumRows = 0;

		for (KeyValues *pRow = pSeq->GetFirstTrueSubKey(); pRow && table.iNumRows < RELOAD_PHASE_ROWS_MAX; pRow = pRow->GetNextTrueSubKey())
		{
			const ReloadPhase_t phase = ReloadPhaseFromName(pRow->GetName());
			if (phase == RELOAD_PHASE_NONE)
			{
				Warning("%s: unknown reload phase \"%s\" in \"%s\"\n", szClassName, pRow->GetName(), table.szSequence);
				continue;
			}

			ReloadPhaseRow_t &row = table.rows[table.iNumRows++];
			row.phase = phase;
			row.flAmmo = pRow->GetFloat("ammo");
			for (int j = 0; j < RELOAD_SOUNDS_PER_ROW; ++j)
			{
				row.sounds[j].flAt = pRow->GetFloat(RELOAD_SOUND_KEYS[j][1]);
				V_strncpy(row.sounds[j].szName, pRow->GetString(RELOAD_SOUND_KEYS[j][0]), sizeof(row.sounds[j].szName));
			}
			V_strncpy(row.szResume, pRow->GetString("resume"), sizeof(row.szResume));
		}
	}
}

int CNEOWeaponInfo::FindReloadTable(const char *pszSequence) const
{
	for (int i = 0; i < m_reloadTables.Count(); ++i)
	{
		if (V_stricmp(m_reloadTables[i].szSequence, pszSequence) == 0)
		{
			return i;
		}
	}
	return -1;
}

void CNEOWeaponInfo::Parse( KeyValues *pKeyValuesData, const char *szWeaponName )
{
	BaseClass::Parse( pKeyValuesData, szWeaponName );

	m_iPlayerDamage = pKeyValuesData->GetInt( "Damage", 42 ); // Douglas Adams 1952 - 2001
	m_iBullets = pKeyValuesData->GetInt( "Bullets", 1 );
	m_flCycleTime = pKeyValuesData->GetFloat( "CycleTime", 0.15 );
	m_flReloadReadyBeforeEnd = pKeyValuesData->GetFloat( "ReloadReadyBeforeEnd", 0.0f );
	ParseReloadPhases(pKeyValuesData->FindKey("ReloadPhases"));

	const char *notFoundStr = "notfound";
	Q_strncpy(szViewModel2, pKeyValuesData->GetString("team2viewmodel", notFoundStr), MAX_WEAPON_STRING);
	// If there was no NSF viewmodel specified, fall back to Source's default "viewmodel" to ensure we have something sensible available.
	// This might happen when attempting to equip a non-NT weapon.
	if (Q_strcmp(szViewModel2, notFoundStr) == 0)
	{
		Q_strncpy(szViewModel2, pKeyValuesData->GetString("viewmodel"), MAX_WEAPON_STRING);
	}

	Q_strncpy( szBulletCharacter, pKeyValuesData->GetString("BulletCharacter", "a"), MAX_BULLET_CHARACTER);
	Q_strncpy( szDeathIcon, pKeyValuesData->GetString("iDeathIcon", ""), MAX_BULLET_CHARACTER);
	m_flPenetration = pKeyValuesData->GetFloat("Penetration", 0);
	m_bDropOnDeath = pKeyValuesData->GetBool("DropOnDeath", true);
	iAimFOV = pKeyValuesData->GetInt("AimFov", 45);

	KeyValues *pViewModel = pKeyValuesData->FindKey("ViewModelOffset");
	if (pViewModel)
	{
		m_flVMFov = pKeyValuesData->GetFloat("VMFov", 60);

		m_vecVMPosOffset.x = pViewModel->GetFloat("forward", 0);
		m_vecVMPosOffset.y = pViewModel->GetFloat("right", 0);
		m_vecVMPosOffset.z = pViewModel->GetFloat("up", 0);

		m_angVMAngOffset[PITCH] = pViewModel->GetFloat("pitch", 0);
		m_angVMAngOffset[YAW] = pViewModel->GetFloat("yaw", 0);
		m_angVMAngOffset[ROLL] = pViewModel->GetFloat("roll", 0);
	}

	// ZoomOffset = Traditional NT aim offset
	// AimOffset = ADS offset. Many scripts carry an untuned one, so it counts only with "enabled" "1".
	if (KeyValues* pZoomOffset = pKeyValuesData->FindKey("ZoomOffset"))
	{
		m_flVMAimFov = pZoomOffset->GetFloat("fov", 55);

		m_vecVMAimPosOffset.x = pZoomOffset->GetFloat("forward", 0);
		m_vecVMAimPosOffset.y = pZoomOffset->GetFloat("right", 0);
		m_vecVMAimPosOffset.z = pZoomOffset->GetFloat("up", 0);

		m_angVMAimAngOffset[PITCH] = pZoomOffset->GetFloat("pitch", 0);
		m_angVMAimAngOffset[YAW] = pZoomOffset->GetFloat("yaw", 0);
		m_angVMAimAngOffset[ROLL] = pZoomOffset->GetFloat("roll", 0);
	}

	m_bHasAds = false;
	if (KeyValues* pAdsOffset = pKeyValuesData->FindKey("AimOffset"))
	{
		m_bHasAds = pAdsOffset->GetBool("enabled", false);
		m_flVMAdsFov = pAdsOffset->GetFloat("fov", 55);

		m_vecVMAdsPosOffset.x = pAdsOffset->GetFloat("forward", 0);
		m_vecVMAdsPosOffset.y = pAdsOffset->GetFloat("right", 0);
		m_vecVMAdsPosOffset.z = pAdsOffset->GetFloat("up", 0);

		m_angVMAdsAngOffset[PITCH] = pAdsOffset->GetFloat("pitch", 0);
		m_angVMAdsAngOffset[YAW] = pAdsOffset->GetFloat("yaw", 0);
		m_angVMAdsAngOffset[ROLL] = pAdsOffset->GetFloat("roll", 0);
	}

	KeyValues* pAdsRecoil = pKeyValuesData->FindKey("AdsRecoil");
	m_flAdsRecoilVertical = pAdsRecoil ? pAdsRecoil->GetFloat("vertical", 1) : 1.f;
	m_flAdsRecoilSide = pAdsRecoil ? pAdsRecoil->GetFloat("side", 1) : 1.f;
	m_flAdsRecoilBack = pAdsRecoil ? pAdsRecoil->GetFloat("back", 1) : 1.f;
	m_flAdsRecoilMaxDist = pAdsRecoil ? pAdsRecoil->GetFloat("max_dist", 0) : 0.f;
	m_flAdsRecoilMaxAngle = pAdsRecoil ? pAdsRecoil->GetFloat("max_angle", 0) : 0.f;
}


