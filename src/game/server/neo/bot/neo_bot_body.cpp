#include "cbase.h"

#include "neo_bot.h"
#include "neo_bot_body.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

ConVar nb_bot_scan_head_speed( "nb_bot_scan_head_speed", "150", FCVAR_CHEAT,
	"Max head turn rate (deg/s) for a bot's non-threat scanning looks - keeps the "
	"spectator view from being whipped around. Threat tracking still uses nb_saccade_speed." );

//
// Return how often we should sample our target's position and
// velocity to update our aim tracking, to allow realistic slop in tracking
//
float CNEOBotBody::GetHeadAimTrackingInterval() const
{
	CNEOBot *me = (CNEOBot *)GetBot();

	switch( me->GetDifficulty() )
	{
	case CNEOBot::EXPERT:
		return 0.05f;

	case CNEOBot::HARD:
		return 0.1f;

	case CNEOBot::NORMAL:
		return 0.25f;

	case CNEOBot::EASY:
		return 1.0f;
	}

	return 0.0f;
}

//
// A scanning look has no entity subject and a priority below CRITICAL. Turn the head
// gently for those so a spectator riding this bot's eyes doesn't get motion sick from
// the whole view whipping around. Active threat tracking keeps PlayerBody's fast saccade.
//
float CNEOBotBody::GetMaxHeadAngularVelocity() const
{
	if ( GetLookAtSubject() == NULL && m_lookAtPriority < IBody::CRITICAL )
	{
		if ( m_scanSpeedOverride > 0.0f )
		{
			return m_scanSpeedOverride;
		}

		return nb_bot_scan_head_speed.GetFloat();
	}

	return PlayerBody::GetMaxHeadAngularVelocity();
}

CNEO_Player *CNEOBotBody::NEOPlayerEnt() const
{
	return ToNEOPlayer(GetBot()->GetEntity());
}

float CNEOBotBody::GetCloakPower() const
{
	auto me = NEOPlayerEnt();
	return me ? me->CloakPower_Get() : 0.0f;
}

bool CNEOBotBody::IsCloakEnabled() const
{
	// used for determining if bot needs to press thermoptic button
	// we are only interested in the toggle state not visibility in this context
	// so do not use GetBotCloakStateDisrupted() here
	auto me = NEOPlayerEnt();
	return me && me->GetInThermOpticCamo();
}

float CNEOBotBody::GetHullWidth() const
{
	const auto *neoPlayer = NEOPlayerEnt();
	return VEC_HULL_MAX_SCALED(neoPlayer).x - VEC_HULL_MIN_SCALED(neoPlayer).x;
}

float CNEOBotBody::GetStandHullHeight() const
{
	const auto *neoPlayer = NEOPlayerEnt();
	return VEC_HULL_MAX_SCALED(neoPlayer).z - VEC_HULL_MIN_SCALED(neoPlayer).z;
}

float CNEOBotBody::GetCrouchHullHeight() const
{
	const auto *neoPlayer = NEOPlayerEnt();
	return VEC_DUCK_HULL_MAX_SCALED(neoPlayer).z - VEC_DUCK_HULL_MIN_SCALED(neoPlayer).z;
}

const Vector &CNEOBotBody::GetHullMins() const
{
	const auto *neoPlayer = NEOPlayerEnt();

	if ( m_posture == CROUCH )
	{
		m_hullMins = VEC_DUCK_HULL_MIN_SCALED(neoPlayer);
	}
	else
	{
		m_hullMins = VEC_HULL_MIN_SCALED(neoPlayer);
	}

	return m_hullMins;
}

const Vector &CNEOBotBody::GetHullMaxs() const
{
	const auto *neoPlayer = NEOPlayerEnt();

	if ( m_posture == CROUCH )
	{
		m_hullMaxs = VEC_DUCK_HULL_MAX_SCALED(neoPlayer);
	}
	else
	{
		m_hullMaxs = VEC_HULL_MAX_SCALED(neoPlayer);
	}

	return m_hullMaxs;	
}

