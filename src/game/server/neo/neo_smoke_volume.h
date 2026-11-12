// NEO volumetric smoke - the server's copy of a grenade's smoke field.
//
// The server fills the same field as the clients, from the same shared core
// and world queries, so line of sight can follow the drawn smoke. The fill
// runs on worker threads; the field does not depend on how its work is
// split, and the queries switch to it at a fixed tick when the fill is done
// by then, as in normal play, so sight does not depend on machine speed or
// host_timescale. Bot sight reads it through the smoke line-of-sight
// blocker.

#ifndef NEO_SMOKE_VOLUME_H
#define NEO_SMOKE_VOLUME_H
#ifdef _WIN32
#pragma once
#endif

class CBaseEntity;

// Starts the field of a smoke grenade that detonated now at `origin`. The
// field lives until the smoke has faded or the grenade entity is removed,
// as at a round restart.
void NEOSmokeVolume_Create( const Vector &origin, CBaseEntity *pGrenade );

// Optical depth of all smoke on the segment, as the clients draw it without
// the look noise, at the smoke's current age. Until a field is filled its
// smoke is the nominal dome.
float NEOSmoke_OpticalDepth( const Vector &from, const Vector &to );

// Optical depth at which a human stops seeing a player behind smoke in a lit scene.
static constexpr float NEO_SMOKE_SIGHT_BLOCK_DEPTH = 4.4f;

// The share of the segment from `from` that stays in sight: 1 when the smoke
// on it stays under NEO_SMOKE_SIGHT_BLOCK_DEPTH.
float NEOSmoke_SightBlockFraction( const Vector &from, const Vector &to );

// The box that holds the smoke of `pGrenade`'s field; false while the field
// is filling or when the grenade has none.
bool NEOSmokeVolume_SmokeBox( const CBaseEntity *pGrenade, Vector &mins, Vector &maxs );

#endif // NEO_SMOKE_VOLUME_H
