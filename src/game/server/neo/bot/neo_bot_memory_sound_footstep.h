#pragma once

#include "mathlib/vector.h"
#include "soundflags.h"

class CBasePlayer;

// Short-term memory of each player's latest footstep, so a bot can tell when it hears steps near a spot
namespace NEOMemorySoundFootstep
{
	// Record a footstep sound the server emitted for this player
	void OnFootstep( const CBasePlayer *pPlayer, const Vector &vecPos, float flVolume, soundlevel_t soundLevel );

	// Forget every step, e.g. on a map change where curtime restarts
	void Reset();

	// Whether a recent enemy footstep that vEar could hear landed within flRadius of vSpot
	bool HeardEnemyStepNear( const Vector &vEar, int iMyTeam, int iMyEnt, const Vector &vSpot, float flRadius );
}
