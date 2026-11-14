// NEO volumetric smoke - gameplay values shared by the client and the server.
//
// They decide where the smoke is and how much sight it takes, so the server's
// line-of-sight field and every client's drawn cloud must use the same ones.

#ifndef NEO_SMOKE_SHARED_H
#define NEO_SMOKE_SHARED_H
#ifdef _WIN32
#pragma once
#endif

#include "mathlib/mathlib.h"
#include "neo_smoke_fill.h"

#include "tier1/convar.h"

// The smoke every player faces: legacy particles and cube blocker, or the
// volumetric field. Read when a grenade detonates.
extern ConVar sv_neo_smoke_volumetric;

enum NeoSmokeMode_t
{
	NEO_SMOKE_MODE_COVER_BOXES = -1,
	NEO_SMOKE_MODE_LEGACY = 0,
	NEO_SMOKE_MODE_VOLUMETRIC = 1,
};

inline NeoSmokeMode_t NEOSmokeMode()
{
	return static_cast< NeoSmokeMode_t >( sv_neo_smoke_volumetric.GetInt() );
}

// The cover box mode changes only what a client draws, so the server's
// field and sight are the same as in the volumetric mode.
inline bool NEOSmokeIsVolumetric()
{
	return NEOSmokeMode() != NEO_SMOKE_MODE_LEGACY;
}

// The legacy smoke's time curves: sine ease-out bloom, cosine fade.
static constexpr float NEO_VOLSMOKE_BLOOM_TIME = 5.5f;
static constexpr float NEO_VOLSMOKE_FADE_START = 17.0f;
static constexpr float NEO_VOLSMOKE_FADE_END = 25.0f;

// Game time after detonation at which the server's queries switch from the
// nominal dome to the field. A fill done by then waits for this tick, so
// sight does not depend on when a worker finished: a fill normally ends
// long before it, so in normal play every field switches at this tick and a
// match replays the same way. A tick never waits for a fill, so a fill
// still running then (many grenades at once, a loaded machine, a high
// host_timescale) switches on the first tick after it ends, and the switch
// then depends on the machine. The delay leaves a fill time to end even at
// a high host_timescale, and the dome front is still small at this age.
static constexpr float NEO_SMOKE_FIELD_LIVE_DELAY = 1.0f;

// Step of the CPU smoke path. The server's sight and the client's path
// sample alike, so the two sums are equal.
static constexpr float NEO_VOLSMOKE_PATH_STEP = 16.0f;

// Path lengths are stored up to this length, or up to the front when it is
// farther, so a short field keeps its precision.
static constexpr float NEO_VOLSMOKE_DIST_RANGE = 64 * CNEOSmokeFill::CELL_SIZE;

// Width of the soft edge behind the expanding front.
static constexpr float NEO_VOLSMOKE_EDGE = 27.0f;

// Per-cell density variation, from the cell index so it is the same everywhere.
static constexpr float NEO_VOLSMOKE_CELL_VARIATION = 0.15f;

// Extinction per world unit at density 1.
static constexpr float NEO_VOLSMOKE_EXTINCTION = 0.06f;

// World-only: no props or entities, so every client sees the same walls.
// Gas does not enter water; it spreads over the surface as over a floor.
static constexpr int NEO_VOLSMOKE_WATER_MASK = CONTENTS_WATER | CONTENTS_SLIME;
static constexpr int NEO_VOLSMOKE_FILL_MASK = CONTENTS_SOLID | CONTENTS_WINDOW | NEO_VOLSMOKE_WATER_MASK;

inline float VolSmokeBloom( float age )
{
	return sinf( 0.5f * M_PI_F * Clamp( age, 0.0f, NEO_VOLSMOKE_BLOOM_TIME ) / NEO_VOLSMOKE_BLOOM_TIME );
}

inline float VolSmokeFade( float age )
{
	if ( age <= NEO_VOLSMOKE_FADE_START )
	{
		return 1.0f;
	}

	const float f = Clamp( ( age - NEO_VOLSMOKE_FADE_START ) / ( NEO_VOLSMOKE_FADE_END - NEO_VOLSMOKE_FADE_START ), 0.0f, 1.0f );
	return 0.5f + 0.5f * cosf( M_PI_F * f );
}

// The variation is keyed by a cell's place relative to the grenade, laid out
// as the cell index of a fixed grid around it, so it does not move with the
// box.
static constexpr int NEO_VOLSMOKE_VARIATION_SPAN = 128;
static constexpr int NEO_VOLSMOKE_VARIATION_BELOW = 8;

inline float VolSmokeCellVariation( int x, int y, int z )
{
	const int half = NEO_VOLSMOKE_VARIATION_SPAN / 2;
	const int key = ( ( z + NEO_VOLSMOKE_VARIATION_BELOW ) * NEO_VOLSMOKE_VARIATION_SPAN + y + half ) * NEO_VOLSMOKE_VARIATION_SPAN + x + half;
	unsigned int h = (unsigned int)key;
	h ^= h >> 16;
	h *= 0x7feb352dU;
	h ^= h >> 15;
	h *= 0x846ca68bU;
	h ^= h >> 16;
	return 1.0f - NEO_VOLSMOKE_CELL_VARIATION * (float)( h & 0xff ) / 255.0f;
}

#endif // NEO_SMOKE_SHARED_H
