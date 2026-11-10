#include "cbase.h"
#include "bot/neo_bot.h"
#include "bot/neo_bot_hearing.h"
#include "nav_mesh.h"
#include "soundent.h"
#include "vstdlib/random.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

ConVar neo_bot_research_lonewolf_hearing( "neo_bot_research_lonewolf_hearing", "0", FCVAR_CHEAT,
	"Research: a CTG lone wolf turns toward, ambushes or hunts the enemy it hears (0 off, 1 on)", true, 0, true, 1 );
ConVar neo_bot_research_lonewolf_hearing_debug( "neo_bot_research_lonewolf_hearing_debug", "0", FCVAR_CHEAT,
	"Research: print NEO_LW_HEARD for every heard noise a lone wolf acts on", true, 0, true, 1 );

extern ConVar neo_bot_research_team_arm;

namespace NEOBotHearing
{
	// A noise is worth acting on for this long after it was made
	static constexpr float NOISE_MEMORY_TIME = 4.0f;
	// A running step (volume 0.5) carries this far times two; crouching and walking scale it down
	static constexpr float FOOTSTEP_RADIUS_PER_VOLUME = 1600.0f;
	// Combat sounds at or below the dry fire click's radius are not gunshots
	static constexpr float GUNFIRE_MIN_RADIUS = SOUNDENT_VOLUME_EMPTY;
	// Walls between ear and noise muffle it to this share of its radius
	static constexpr float OCCLUDED_RADIUS_SCALE = 0.5f;
	// Hearing gives a direction within this many degrees either way
	static constexpr float BEARING_ERROR_DEG = 20.0f;
	// and a range within this share either way
	static constexpr float RANGE_ERROR = 0.3f;
	// The guess is moved onto the nav mesh from no further than this
	static constexpr float GUESS_SNAP_DIST = 512.0f;
	// Trace to the noise from this high above its origin, so a step is not occluded by the floor
	static constexpr float NOISE_TRACE_HEIGHT = 16.0f;

	struct Noise
	{
		Vector pos = vec3_origin;
		float flWhen = -FLT_MAX;
		float flRadius = 0.0f;
		float flCombatExpire = -FLT_MAX;	// tells a new combat sound from the one already sampled
		int iTeam = TEAM_UNASSIGNED;
		NoiseType eType = NOISE_NONE;
	};

	// Indexed by player entindex
	static Noise s_noises[ MAX_PLAYERS + 1 ];

	static bool IsRecording()
	{
		return neo_bot_research_lonewolf_hearing.GetBool();
	}

	bool IsEnabledFor( const CNEOBot *me )
	{
		if ( !IsRecording() )
		{
			return false;
		}

		const int teamArm = neo_bot_research_team_arm.GetInt();
		return teamArm == 0 || me->GetTeamNumber() == teamArm;
	}

	bool IsDebugEnabled()
	{
		return neo_bot_research_lonewolf_hearing_debug.GetBool();
	}

	static void RecordNoise( CBaseEntity *pSource, const Vector &pos, float flRadius, NoiseType eType )
	{
		const int iEnt = pSource->entindex();
		if ( iEnt < 1 || iEnt > MAX_PLAYERS )
		{
			return;
		}

		Noise &noise = s_noises[ iEnt ];
		noise.pos = pos;
		noise.flWhen = gpGlobals->curtime;
		noise.flRadius = flRadius;
		noise.iTeam = pSource->GetTeamNumber();
		noise.eType = eType;
	}

	void OnFootstep( CBasePlayer *pPlayer, const Vector &vecFeet, float flVolume )
	{
		if ( !IsRecording() || !pPlayer )
		{
			return;
		}

		RecordNoise( pPlayer, vecFeet, flVolume * FOOTSTEP_RADIUS_PER_VOLUME, NOISE_FOOTSTEP );
	}

	void Update()
	{
		if ( !IsRecording() )
		{
			return;
		}

		CSound *pSound = nullptr;
		for ( int i = CSoundEnt::ActiveList(); i != SOUNDLIST_EMPTY; i = pSound->NextSound() )
		{
			pSound = CSoundEnt::SoundPointerForIndex( i );
			if ( !pSound )
			{
				break;
			}

			if ( !( pSound->SoundType() & SOUND_COMBAT ) || !pSound->DoesSoundExpire() )
			{
				continue;
			}

			CBaseEntity *pOwner = pSound->m_hOwner.Get();
			if ( !pOwner || !pOwner->IsPlayer() )
			{
				continue;
			}

			const int iEnt = pOwner->entindex();
			if ( iEnt < 1 || iEnt > MAX_PLAYERS )
			{
				continue;
			}

			// The same combat sound stays listed for a few ticks: take it once
			if ( s_noises[ iEnt ].flCombatExpire == pSound->SoundExpirationTime() )
			{
				continue;
			}

			const float flRadius = (float)pSound->Volume();
			RecordNoise( pOwner, pSound->GetSoundOrigin(), flRadius, ( flRadius > GUNFIRE_MIN_RADIUS ) ? NOISE_GUNFIRE : NOISE_OTHER );
			s_noises[ iEnt ].flCombatExpire = pSound->SoundExpirationTime();
		}
	}

	void Reset()
	{
		for ( Noise &noise : s_noises )
		{
			noise = Noise();
		}
	}

	// The same noise always gives the same guess, so the believed spot holds still until a new noise is heard
	static Vector GuessNoiseSpot( const CNEOBot *me, int iSource, const Noise &noise )
	{
		CUniformRandomStream random;
		random.SetSeed( me->entindex() * 7919 + iSource * 131 + (int)( noise.flWhen * 100.0f ) );

		const float flYaw = DEG2RAD( random.RandomFloat( -BEARING_ERROR_DEG, BEARING_ERROR_DEG ) );
		const float flRangeScale = 1.0f + random.RandomFloat( -RANGE_ERROR, RANGE_ERROR );

		// Swing the noise around me and scale its range, keeping its height
		const Vector vecFrom = me->GetAbsOrigin();
		const Vector vecAlong = noise.pos - vecFrom;
		const float flRange = vecAlong.Length2D() * flRangeScale;
		const float flBearing = atan2f( vecAlong.y, vecAlong.x ) + flYaw;
		const Vector vecGuess( vecFrom.x + cosf( flBearing ) * flRange, vecFrom.y + sinf( flBearing ) * flRange, noise.pos.z );

		const CNavArea *pArea = TheNavMesh->GetNearestNavArea( vecGuess, false, GUESS_SNAP_DIST );
		if ( !pArea )
		{
			return vecGuess;
		}

		Vector vecOnMesh;
		pArea->GetClosestPointOnArea( vecGuess, &vecOnMesh );
		return vecOnMesh;
	}

	bool HearLoudestEnemy( CNEOBot *me, HeardNoise &heard )
	{
		const float flNow = gpGlobals->curtime;
		const Vector vecEar = me->EyePosition();

		int iLoudest = 0;
		float flLoudestScore = 0.0f;
		float flLoudestDist = 0.0f;

		for ( int i = 1; i <= MAX_PLAYERS; ++i )
		{
			const Noise &noise = s_noises[ i ];
			const float flAge = flNow - noise.flWhen;
			if ( noise.eType == NOISE_NONE || flAge >= NOISE_MEMORY_TIME || noise.iTeam == me->GetTeamNumber() )
			{
				continue;
			}

			// The kill feed tells everyone who is dead
			CBasePlayer *pSource = UTIL_PlayerByIndex( i );
			if ( !pSource || !pSource->IsAlive() )
			{
				continue;
			}

			const float flDist = vecEar.DistTo( noise.pos );
			if ( flDist >= noise.flRadius )
			{
				continue;
			}

			float flRadius = noise.flRadius;
			trace_t tr;
			UTIL_TraceLine( vecEar, noise.pos + Vector( 0, 0, NOISE_TRACE_HEIGHT ), MASK_SOLID_BRUSHONLY, nullptr, COLLISION_GROUP_NONE, &tr );
			if ( tr.fraction < 1.0f )
			{
				flRadius *= OCCLUDED_RADIUS_SCALE;
				if ( flDist >= flRadius )
				{
					continue;
				}
			}

			const float flScore = ( 1.0f - flDist / flRadius ) * ( 1.0f - flAge / NOISE_MEMORY_TIME );
			if ( flScore > flLoudestScore )
			{
				flLoudestScore = flScore;
				flLoudestDist = flDist;
				iLoudest = i;
			}
		}

		if ( iLoudest == 0 )
		{
			return false;
		}

		const Noise &loudest = s_noises[ iLoudest ];
		heard.vecGuess = GuessNoiseSpot( me, iLoudest, loudest );
		heard.flDist = flLoudestDist;
		heard.flWhen = loudest.flWhen;
		heard.iSource = iLoudest;
		heard.eType = loudest.eType;
		return true;
	}

	const char *GetNoiseName( NoiseType eType )
	{
		switch ( eType )
		{
		case NOISE_FOOTSTEP:
			return "footstep";
		case NOISE_GUNFIRE:
			return "gunfire";
		case NOISE_OTHER:
			return "other";
		default:
			return "none";
		}
	}
}
