// NEO volumetric smoke - the server's copy of a grenade's smoke field.
// See neo_smoke_volume.h.

#include "cbase.h"
#include "neo_smoke_volume.h"
#include "neo_smoke_fill.h"
#include "neo_smoke_shared.h"
#include "neo_smoke_world.h"
#include "igamesystem.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

// Fill work a tick, in finalized cells: each pending fill may take up to
// PER_FILL, oldest first, until the tick's total reaches the cap. A lone open
// dome then fills in about 44 ticks; many grenades at once queue behind the
// cap instead of stretching one tick. A slice also stops after this many
// world queries per cell, which bounds a slice along walls, where a cell
// costs the most.
static constexpr int NEO_SMOKE_SERVER_CELLS_PER_FILL = 700;
static constexpr int NEO_SMOKE_SERVER_CELLS_PER_TICK = 2800;
static constexpr int NEO_SMOKE_SERVER_QUERIES_PER_CELL = 8;

class CNeoSmokeVolume
{
public:
	CNeoSmokeVolume( const Vector &origin, CBaseEntity *pGrenade );
	~CNeoSmokeVolume() { delete m_pWorld; }

	// Fills up to `cells` cells; returns what the slice takes from the budget.
	int Step( int cells );

	bool IsDone() const { return m_Fill.IsDone(); }
	bool Expired() const;
	unsigned long long LatticeHash() const;
	void Print() const;

private:
	int m_nId;
	Vector m_vecOrigin;
	float m_flStartTime;
	EHANDLE m_hGrenade;
	CNeoSmokeFill m_Fill;
	CNeoSmokeWorld *m_pWorld; // freed once the fill is done
	int m_nTraces;
	double m_flFillSeconds;
	double m_flWorstSlice;
	int m_nSlices;
	float m_flDoneAge;
	int m_nPeakBytes;
	int m_nWorldBytes;

	static int s_nNextId;
};

int CNeoSmokeVolume::s_nNextId = 0;

static CUtlVector< CNeoSmokeVolume * > s_SmokeVolumes;

// Fill time a tick over every volume: the worst tick, and the sum and count
// of the ticks that did fill work, since the last print.
static double s_flWorstTick = 0.0;
static double s_flTickSum = 0.0;
static int s_nFillTicks = 0;

CNeoSmokeVolume::CNeoSmokeVolume( const Vector &origin, CBaseEntity *pGrenade )
	: m_nId( s_nNextId++ ), m_vecOrigin( origin ), m_flStartTime( gpGlobals->curtime ), m_hGrenade( pGrenade ), m_pWorld( NULL ),
	  m_nTraces( 0 ), m_flFillSeconds( 0.0 ), m_flWorstSlice( 0.0 ), m_nSlices( 0 ), m_flDoneAge( -1.0f ), m_nPeakBytes( 0 ), m_nWorldBytes( 0 )
{
	// The same box and lattice as the client's volume, so the queries and
	// the field are the same.
	int box[3];
	int seedCorner[3];
	m_Fill.BoxSize( box );
	m_Fill.BoxSeedCorner( seedCorner );

	Vector gridMin;
	const float cell = CNeoSmokeFill::CELL_SIZE;
	for ( int a = 0; a < 3; ++a )
	{
		gridMin[a] = origin[a] - seedCorner[a] * cell;
	}

	int latticeLo[3];
	int latticeHi[3];
	m_Fill.LatticeBounds( latticeLo, latticeHi );
	m_pWorld = new CNeoSmokeWorld( gridMin, box, seedCorner, latticeLo, latticeHi );
	m_Fill.Start( m_pWorld );
}

int CNeoSmokeVolume::Step( int cells )
{
	const double start = Plat_FloatTime();
	const int cellsBefore = m_Fill.LatticeCount();
	const int queriesBefore = m_Fill.WorldQueries();
	m_Fill.Step( cells, cells * NEO_SMOKE_SERVER_QUERIES_PER_CELL );
	const double spent = Plat_FloatTime() - start;

	m_flFillSeconds += spent;
	m_flWorstSlice = MAX( m_flWorstSlice, spent );
	++m_nSlices;
	m_nPeakBytes = MAX( m_nPeakBytes, m_Fill.MemoryBytes() + m_pWorld->MemoryBytes() );

	// A slice cut short by its queries is charged for them, so a tick of
	// fills along walls costs no more than a tick in the open.
	const int queryCells = ( m_Fill.WorldQueries() - queriesBefore ) / NEO_SMOKE_SERVER_QUERIES_PER_CELL;
	const int taken = MAX( 1, MAX( m_Fill.LatticeCount() - cellsBefore, queryCells ) );
	if ( !m_Fill.IsDone() )
	{
		return taken;
	}

	// The query reads the finished field only.
	m_nTraces = m_pWorld->Traces();
	m_nWorldBytes = m_pWorld->MemoryBytes();
	m_nPeakBytes = MAX( m_nPeakBytes, m_Fill.PeakMemoryBytes() + m_nWorldBytes );
	delete m_pWorld;
	m_pWorld = NULL;
	m_flDoneAge = gpGlobals->curtime - m_flStartTime;
	return taken;
}

bool CNeoSmokeVolume::Expired() const
{
	return !m_hGrenade.Get() || gpGlobals->curtime - m_flStartTime > NEO_VOLSMOKE_FADE_END;
}

// The client's lattice hash (neo_volsmoke_stats): cells relative to the
// grenade's cell corner, so the two prints of one field are equal.
unsigned long long CNeoSmokeVolume::LatticeHash() const
{
	int seedCorner[3];
	int offset[3];
	m_Fill.BoxSeedCorner( seedCorner );
	m_Fill.BoxOffset( offset );

	unsigned long long hash = 0;
	for ( int i = 0; i < m_Fill.LatticeCount(); ++i )
	{
		int c[3];
		m_Fill.LatticeCell( i, c[0], c[1], c[2] );
		for ( int a = 0; a < 3; ++a )
		{
			c[a] -= seedCorner[a] + offset[a];
		}

		hash += VolSmokeCellKey( c[0], c[1], c[2], (unsigned int)m_Fill.LatticeDist( i ) );
	}

	return hash;
}

void CNeoSmokeVolume::Print() const
{
	if ( !IsDone() )
	{
		Msg( "neo_smoke_server_stats: volume %d age %.2f filling, %d cells, %d slices, %.1f ms so far\n",
			m_nId, gpGlobals->curtime - m_flStartTime, m_Fill.LatticeCount(), m_nSlices, m_flFillSeconds * 1000.0 );
		return;
	}

	Msg( "neo_smoke_server_stats: volume %d origin (%.9g %.9g %.9g) age %.2f done at age %.2f lattice %016llx (%d cells, front %.1f) "
		"fill %.1f ms in %d slices (worst %.2f ms), traces %d, memory %d KB now, %d KB peak (world cache %d KB)\n",
		m_nId, m_vecOrigin.x, m_vecOrigin.y, m_vecOrigin.z, gpGlobals->curtime - m_flStartTime, m_flDoneAge, LatticeHash(),
		m_Fill.LatticeCount(), CNeoSmokeFill::DistToUnits( m_Fill.LatticeFrontDist() ), m_flFillSeconds * 1000.0, m_nSlices,
		m_flWorstSlice * 1000.0, m_nTraces, m_Fill.MemoryBytes() / 1024, m_nPeakBytes / 1024, m_nWorldBytes / 1024 );
}

void NEOSmokeVolume_Create( const Vector &origin, CBaseEntity *pGrenade )
{
	s_SmokeVolumes.AddToTail( new CNeoSmokeVolume( origin, pGrenade ) );
}

static void NEOSmokeVolume_RemoveAll()
{
	s_SmokeVolumes.PurgeAndDeleteElements();
}

// Steps the pending fills once a tick and removes the fields whose smoke is
// gone.
class CNeoSmokeVolumeSystem final : public CAutoGameSystemPerFrame
{
public:
	CNeoSmokeVolumeSystem() : CAutoGameSystemPerFrame( "CNeoSmokeVolumeSystem" ) {}

	virtual void FrameUpdatePostEntityThink() override
	{
		for ( int i = s_SmokeVolumes.Count() - 1; i >= 0; --i )
		{
			if ( s_SmokeVolumes[i]->Expired() )
			{
				delete s_SmokeVolumes[i];
				s_SmokeVolumes.Remove( i );
			}
		}

		const double start = Plat_FloatTime();
		int budget = NEO_SMOKE_SERVER_CELLS_PER_TICK;
		bool worked = false;
		for ( int i = 0; i < s_SmokeVolumes.Count() && budget > 0; ++i )
		{
			CNeoSmokeVolume *pVol = s_SmokeVolumes[i];
			if ( pVol->IsDone() )
			{
				continue;
			}

			budget -= pVol->Step( MIN( budget, NEO_SMOKE_SERVER_CELLS_PER_FILL ) );
			worked = true;
		}

		if ( !worked )
		{
			return;
		}

		const double spent = Plat_FloatTime() - start;
		s_flWorstTick = MAX( s_flWorstTick, spent );
		s_flTickSum += spent;
		++s_nFillTicks;
	}

	virtual void LevelShutdownPostEntity() override { NEOSmokeVolume_RemoveAll(); }
};

static CNeoSmokeVolumeSystem s_SmokeVolumeSystem;

CON_COMMAND_F( neo_smoke_server_stats, "Print the server's smoke fields, and the fill time a tick since the last print.", FCVAR_CHEAT )
{
	if ( !UTIL_IsCommandIssuedByServerAdmin() )
	{
		return;
	}

	for ( const CNeoSmokeVolume *pVol : s_SmokeVolumes )
	{
		pVol->Print();
	}

	Msg( "neo_smoke_server_stats: live %d; fill ticks %d, mean %.3f ms, worst %.3f ms (budget %d cells a tick, %d a fill)\n",
		s_SmokeVolumes.Count(), s_nFillTicks, s_nFillTicks > 0 ? s_flTickSum * 1000.0 / s_nFillTicks : 0.0,
		s_flWorstTick * 1000.0, NEO_SMOKE_SERVER_CELLS_PER_TICK, NEO_SMOKE_SERVER_CELLS_PER_FILL );
	s_flWorstTick = 0.0;
	s_flTickSum = 0.0;
	s_nFillTicks = 0;
}
