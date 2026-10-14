#include "cbase.h"
#include "bot/neo_bot_memory_sound_footstep.h"
#include "neo_gamerules.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

namespace NEOMemorySoundFootstep
{

	// A running player steps about every 0.3 s and the detpack check runs every 0.5 s,
	// so a step stays heard long enough for the next check to catch it
	static constexpr float FOOTSTEP_MEMORY_TIME = 0.6f;

	// A step is heard as far as Valve's audible distance for its sound level (recipientfilter.cpp) at the stock clip distance,
	// scaled by its volume: a running step on concrete (volume 0.5, 75 dB) carries 1250 u, a walking one 500 u
	static constexpr float FOOTSTEP_HEARING_CLIP_DIST = 1000.0f;

	struct Step
	{
		Vector pos = vec3_origin;
		float  time = -FLT_MAX;
		float  range = 0.0f;	// how far this step could be heard
		int    team = 0;
	};

	// Indexed by player entindex
	static Step s_steps[ MAX_PLAYERS + 1 ];

	void OnFootstep( const CBasePlayer *pPlayer, const Vector &vecPos, float flVolume, soundlevel_t soundLevel )
	{
		const int iEnt = pPlayer->entindex();
		if ( iEnt < 1 || iEnt > MAX_PLAYERS )
		{
			return;
		}

		Step &s = s_steps[ iEnt ];
		s.pos = vecPos;
		s.time = gpGlobals->curtime;
		s.range = flVolume * 2.0f * FOOTSTEP_HEARING_CLIP_DIST / SNDLVL_TO_ATTN( soundLevel );
		s.team = pPlayer->GetTeamNumber();
	}

	void Reset()
	{
		for ( Step &s : s_steps )
		{
			s = Step();
		}
	}

	bool HeardEnemyStepNear( const Vector &vEar, int iMyTeam, int iMyEnt, const Vector &vSpot, float flRadius )
	{
		const float flOldest = gpGlobals->curtime - FOOTSTEP_MEMORY_TIME;
		const bool bTeamplay = NEORules() && NEORules()->IsTeamplay();

		for ( int i = 1; i <= MAX_PLAYERS; ++i )
		{
			const Step &s = s_steps[ i ];
			if ( i == iMyEnt || s.time < flOldest )
			{
				continue;
			}

			if ( bTeamplay && s.team == iMyTeam )
			{
				continue;
			}

			if ( vEar.DistToSqr( s.pos ) > Square( s.range ) )
			{
				continue;
			}

			if ( vSpot.DistToSqr( s.pos ) <= Square( flRadius ) )
			{
				return true;
			}
		}

		return false;
	}

}
