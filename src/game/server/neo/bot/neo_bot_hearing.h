#pragma once

#include "mathlib/vector.h"

class CBasePlayer;
class CNEOBot;

// What a bot can hear of the enemy: the last noise each player made, kept for a few seconds.
// A listener gets a direction and a rough distance to a noise, never the exact spot.
namespace NEOBotHearing
{
	enum NoiseType
	{
		NOISE_NONE,
		NOISE_FOOTSTEP,
		NOISE_GUNFIRE,
		NOISE_OTHER,	// dry fire, knife swing, detpack throw
	};

	struct HeardNoise
	{
		Vector vecGuess = vec3_origin;	// where the listener believes the noise came from
		float flDist = 0.0f;		// true distance, for the debug line only
		float flWhen = -1.0f;		// when the noise was made
		int iSource = 0;			// entindex of the player who made it
		NoiseType eType = NOISE_NONE;
	};

	// Research gate: neo_bot_research_lonewolf_hearing and neo_bot_research_team_arm
	bool IsEnabledFor( const CNEOBot *me );
	bool IsDebugEnabled();

	// A footstep sound was played at vecFeet with the given step volume
	void OnFootstep( CBasePlayer *pPlayer, const Vector &vecFeet, float flVolume );

	// Combat sounds stay in the engine's sound list for only ~0.2 s, so sample them every server tick
	void Update();

	// Forget every noise, e.g. on a map change where curtime restarts
	void Reset();

	// The loudest enemy noise I can hear now: closer, louder and newer wins
	bool HearLoudestEnemy( CNEOBot *me, HeardNoise &heard );

	const char *GetNoiseName( NoiseType eType );
}
