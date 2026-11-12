// NEO volumetric smoke - the server's copy of a grenade's smoke field.
//
// The server fills the same field as the clients, from the same shared core
// and world queries, so line of sight can follow the drawn smoke. The fill is
// sliced over ticks by a cell count, never by time, so a match replays the
// same way on any machine. Not used by gameplay yet.

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

#endif // NEO_SMOKE_VOLUME_H
