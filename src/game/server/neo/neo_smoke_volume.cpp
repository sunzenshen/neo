// NEO volumetric smoke - the server's copy of a grenade's smoke field.
// See neo_smoke_volume.h.

#include "cbase.h"
#include "neo_smoke_volume.h"
#include "neo_smoke_fill.h"
#include "neo_smoke_shared.h"
#include "neo_smoke_world.h"
#include "neo_smoke_jobs.h"
#include "vstdlib/jobthread.h"
#include "igamesystem.h"

// The networked half of a smoke field: what a client needs to fill the same
// field and draw it at the same age, sent to every client so smoke around a
// corner draws too. The origin is sent unscaled, so it arrives bit-exact.
class CNEOSmokeVolumeEnt : public CBaseEntity
{
	DECLARE_CLASS( CNEOSmokeVolumeEnt, CBaseEntity );
	DECLARE_SERVERCLASS();

public:
	virtual void Spawn() override;
	virtual int UpdateTransmitState() override { return SetTransmitState( FL_EDICT_ALWAYS ); }

	void SetStartTime( float time ) { m_flStartTime = time; }
	void SetFieldHash( unsigned long long hash );

private:
	CNetworkVar( float, m_flStartTime );
	CNetworkVar( int, m_nFieldHashLo );
	CNetworkVar( int, m_nFieldHashHi );
};

IMPLEMENT_SERVERCLASS_ST( CNEOSmokeVolumeEnt, DT_NEOSmokeVolumeEnt )
	SendPropTime( SENDINFO( m_flStartTime ) ),
	SendPropInt( SENDINFO( m_nFieldHashLo ), 32, SPROP_UNSIGNED ),
	SendPropInt( SENDINFO( m_nFieldHashHi ), 32, SPROP_UNSIGNED ),
END_SEND_TABLE()

LINK_ENTITY_TO_CLASS( neo_smoke_volume, CNEOSmokeVolumeEnt );

void CNEOSmokeVolumeEnt::Spawn()
{
	BaseClass::Spawn();
	SetMoveType( MOVETYPE_NONE );
	SetSolid( SOLID_NONE );
	AddEffects( EF_NODRAW );
}

// Zero until the field is filled.
void CNEOSmokeVolumeEnt::SetFieldHash( unsigned long long hash )
{
	m_nFieldHashLo = (int)( hash & 0xffffffffull );
	m_nFieldHashHi = (int)( hash >> 32 );
}

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

// Fills run on worker threads, one job per fill, oldest first, so a tick
// never pays for one; the main thread only starts jobs and collects the
// finished fields. Few workers: the server shares the machine with other
// servers or a client, and a fill still ends long before the bloom.
static constexpr int NEO_SMOKE_SERVER_FILL_MAX_WORKERS = 2;

// Without workers, fill work a tick, in finalized cells: each pending fill
// may take up to PER_FILL, oldest first, until the tick's total reaches the
// cap. A slice also stops after this many world queries per cell, which
// bounds a slice along walls, where a cell costs the most.
static constexpr int NEO_SMOKE_SERVER_CELLS_PER_FILL = 700;
static constexpr int NEO_SMOKE_SERVER_CELLS_PER_TICK = 2800;
static constexpr int NEO_SMOKE_SERVER_QUERIES_PER_CELL = 8;

// The client's step for its CPU smoke path; the same step gives the same
// sum.
static constexpr float NEO_SMOKE_PATH_STEP = 16.0f;

// A cell beside the gas without a gas neighbor reads this path length, as
// the client's empty texel does.
static constexpr float NEO_SMOKE_DIST_RANGE = 64 * CNeoSmokeFill::CELL_SIZE;

class CNeoSmokeVolume
{
public:
	CNeoSmokeVolume( const Vector &origin, CBaseEntity *pGrenade );
	~CNeoSmokeVolume();

	// Fills up to `cells` cells; returns what the slice takes from the budget.
	int Step( int cells );

	bool WantsFill() const { return !m_Fill.IsDone() && !m_pFillJob; }
	bool IsFilling() const { return m_pFillJob != NULL; }
	void StartJob( IThreadPool *pPool );

	// Picks up the job once it is finished; returns whether it was.
	bool CollectJob();

	bool IsDone() const { return !m_pFillJob && m_Fill.IsDone(); }
	bool Expired() const;
	unsigned long long LatticeHash() const;
	void Print() const;

	float PathOpticalDepth( const Vector &from, const Vector &to ) const;

	// Whether the segment crosses the box that can hold smoke now.
	bool SegmentCrossesBox( const Vector &from, const Vector &to ) const;

	bool BelongsTo( const CBaseEntity *pGrenade ) const { return m_hGrenade.Get() == pGrenade; }

	// The box that holds the smoke; false until the field is filled.
	bool SmokeBox( Vector &mins, Vector &maxs ) const;

private:
	// The box that can hold smoke at this age; false when there is none.
	bool CurrentBox( Vector &boxMin, Vector &boxMax, float &front ) const;

	void RunFill();
	void PrepareField();
	void FinishFill();
	float Density( const Vector &pos, float front ) const;
	float DomeDensity( const Vector &pos, float front ) const;
	void CornerValue( int x, int y, int z, float &dist, float &weight ) const;

	int m_nId;
	Vector m_vecOrigin;
	float m_flStartTime;
	EHANDLE m_hGrenade;
	CHandle< CNEOSmokeVolumeEnt > m_hNetworked;
	CNeoSmokeFill m_Fill;
	CNeoSmokeWorld *m_pWorld; // freed once the fill is done
	int m_nTraces;
	double m_flFillSeconds;
	double m_flWorstSlice;
	int m_nSlices;
	float m_flDoneAge;
	int m_nPeakBytes;
	int m_nWorldBytes;

	// The job owns the fill, its world and m_flJobSeconds while it runs.
	CJob *m_pFillJob;
	CInterlockedInt m_nFillCancel;
	double m_flJobSeconds;

	// The finished field's box, as the client lays it out, and the box that
	// holds its smoke.
	int m_nBox[3];
	int m_nSeedCorner[3];
	Vector m_vecGridMin;
	Vector m_vecSmokeMin;
	Vector m_vecSmokeMax;
	float m_flFrontUnits;
	float m_flDistRange;
	unsigned long long m_nFieldHash;

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
	  m_nTraces( 0 ), m_flFillSeconds( 0.0 ), m_flWorstSlice( 0.0 ), m_nSlices( 0 ), m_flDoneAge( -1.0f ), m_nPeakBytes( 0 ), m_nWorldBytes( 0 ),
	  m_pFillJob( NULL ), m_flJobSeconds( 0.0 ), m_flFrontUnits( 0.0f ), m_flDistRange( 0.0f ), m_nFieldHash( 0 )
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

	CNEOSmokeVolumeEnt *pEnt = static_cast< CNEOSmokeVolumeEnt * >( CreateEntityByName( "neo_smoke_volume" ) );
	if ( pEnt )
	{
		pEnt->SetAbsOrigin( origin );
		pEnt->SetStartTime( m_flStartTime );
		DispatchSpawn( pEnt );
		m_hNetworked = pEnt;
	}
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
	if ( m_Fill.IsDone() )
	{
		PrepareField();
		FinishFill();
	}

	return taken;
}

CNeoSmokeVolume::~CNeoSmokeVolume()
{
	// The job uses the fill, its world and the map; it ends before they go.
	m_nFillCancel = 1;
	NEOSmokeFillJoin( m_pFillJob );
	delete m_pWorld;

	if ( m_hNetworked.Get() )
	{
		UTIL_Remove( m_hNetworked.Get() );
	}
}

void CNeoSmokeVolume::StartJob( IThreadPool *pPool )
{
	m_pFillJob = pPool->QueueCall( this, &CNeoSmokeVolume::RunFill );
}

// The fill job, on a worker.
void CNeoSmokeVolume::RunFill()
{
	const double start = Plat_FloatTime();
	NEOSmokeFillRun( m_Fill, m_nFillCancel );
	if ( m_Fill.IsDone() )
	{
		PrepareField();
	}

	m_flJobSeconds += Plat_FloatTime() - start;
}

bool CNeoSmokeVolume::CollectJob()
{
	if ( !m_pFillJob || !m_pFillJob->IsFinished() )
	{
		return false;
	}

	m_pFillJob->Release();
	m_pFillJob = NULL;
	m_flFillSeconds += m_flJobSeconds;
	m_flWorstSlice = MAX( m_flWorstSlice, m_flJobSeconds );
	m_flJobSeconds = 0.0;
	++m_nSlices;
	if ( m_Fill.IsDone() )
	{
		FinishFill();
	}

	return true;
}

// The query reads the finished field only; the world cache goes.
void CNeoSmokeVolume::FinishFill()
{
	m_nTraces = m_pWorld->Traces();
	m_nWorldBytes = m_pWorld->MemoryBytes();
	m_nPeakBytes = MAX( m_nPeakBytes, m_Fill.PeakMemoryBytes() + m_nWorldBytes );
	delete m_pWorld;
	m_pWorld = NULL;
	m_flDoneAge = gpGlobals->curtime - m_flStartTime;
	if ( m_hNetworked.Get() )
	{
		m_hNetworked->SetFieldHash( m_nFieldHash );
	}
}

// What the query and the clients need from the finished field. Runs on the
// fill's worker, so the tick does not pay for the scan of the field.
void CNeoSmokeVolume::PrepareField()
{
	m_Fill.BoxSize( m_nBox );
	m_Fill.BoxSeedCorner( m_nSeedCorner );
	m_flFrontUnits = CNeoSmokeFill::DistToUnits( m_Fill.FrontDist() );
	m_flDistRange = MAX( NEO_SMOKE_DIST_RANGE, m_flFrontUnits );

	int lo[3] = { m_nBox[0], m_nBox[1], m_nBox[2] };
	int hi[3] = { -1, -1, -1 };
	for ( int i = 0; i < m_Fill.FinalizedCount(); ++i )
	{
		const int idx = m_Fill.FinalizedCell( i );
		const int c[3] = { idx % m_nBox[0], ( idx / m_nBox[0] ) % m_nBox[1], idx / ( m_nBox[0] * m_nBox[1] ) };
		for ( int a = 0; a < 3; ++a )
		{
			lo[a] = MIN( lo[a], c[a] );
			hi[a] = MAX( hi[a], c[a] );
		}
	}

	// One cell of margin: the trilinear lookup spreads a cell's smoke half a
	// cell past it.
	const float cell = CNeoSmokeFill::CELL_SIZE;
	for ( int a = 0; a < 3; ++a )
	{
		m_vecGridMin[a] = m_vecOrigin[a] - m_nSeedCorner[a] * cell;
		const float gridMax = m_vecGridMin[a] + m_nBox[a] * cell;
		m_vecSmokeMin[a] = MAX( m_vecGridMin[a], m_vecGridMin[a] + ( lo[a] - 1 ) * cell );
		m_vecSmokeMax[a] = MIN( gridMax, m_vecGridMin[a] + ( hi[a] + 2 ) * cell );
	}

	m_nFieldHash = LatticeHash();
}

// A box cell's path length and weight as the client's atlas holds them: a
// cell without gas has no weight and takes the shortest path length of its
// gas neighbors, so the lookup fades the edge instead of cutting it.
void CNeoSmokeVolume::CornerValue( int x, int y, int z, float &dist, float &weight ) const
{
	const int idx = ( z * m_nBox[1] + y ) * m_nBox[0] + x;
	if ( m_Fill.IsFinalized( idx ) )
	{
		dist = CNeoSmokeFill::DistToUnits( m_Fill.Dist( idx ) );
		weight = VolSmokeCellVariation( x - m_nSeedCorner[0], y - m_nSeedCorner[1], z - m_nSeedCorner[2] );
		return;
	}

	weight = 0.0f;
	dist = m_flDistRange;
	for ( int dz = -1; dz <= 1; ++dz )
	{
		for ( int dy = -1; dy <= 1; ++dy )
		{
			for ( int dx = -1; dx <= 1; ++dx )
			{
				const int n[3] = { x + dx, y + dy, z + dz };
				if ( n[0] < 0 || n[1] < 0 || n[2] < 0 || n[0] >= m_nBox[0] || n[1] >= m_nBox[1] || n[2] >= m_nBox[2] )
				{
					continue;
				}

				const int nIdx = ( n[2] * m_nBox[1] + n[1] ) * m_nBox[0] + n[0];
				if ( m_Fill.IsFinalized( nIdx ) )
				{
					dist = MIN( dist, CNeoSmokeFill::DistToUnits( m_Fill.Dist( nIdx ) ) );
				}
			}
		}
	}
}

// The client's smoke density without the look noise: trilinear over the
// eight nearest cell centers, then the soft edge behind the front.
float CNeoSmokeVolume::Density( const Vector &pos, float front ) const
{
	int base[3];
	float frac[3];
	for ( int a = 0; a < 3; ++a )
	{
		const float g = clamp( ( pos[a] - m_vecGridMin[a] ) / CNeoSmokeFill::CELL_SIZE - 0.5f, 0.0f, (float)( m_nBox[a] - 1 ) );
		base[a] = MIN( (int)g, m_nBox[a] - 2 );
		frac[a] = g - base[a];
	}

	float dist = 0.0f;
	float weight = 0.0f;
	for ( int corner = 0; corner < 8; ++corner )
	{
		float w = 1.0f;
		int c[3];
		for ( int a = 0; a < 3; ++a )
		{
			const int bit = ( corner >> a ) & 1;
			c[a] = base[a] + bit;
			w *= bit ? frac[a] : 1.0f - frac[a];
		}

		if ( w <= 0.0f )
		{
			continue;
		}

		float cornerDist;
		float cornerWeight;
		CornerValue( c[0], c[1], c[2], cornerDist, cornerWeight );
		dist += w * cornerDist;
		weight += w * cornerWeight;
	}

	const float depth = front - dist;
	if ( weight <= 0.001f || depth <= 0.0f )
	{
		return 0.0f;
	}

	const float edge = MIN( depth / NEO_VOLSMOKE_EDGE, 1.0f );
	return weight * edge * edge * ( 3.0f - 2.0f * edge );
}

// Before the field is filled: the dome the client draws then.
float CNeoSmokeVolume::DomeDensity( const Vector &pos, float front ) const
{
	const float depth = front - pos.DistTo( m_vecOrigin );
	if ( depth <= 0.0f )
	{
		return 0.0f;
	}

	const float edge = MIN( depth / NEO_VOLSMOKE_EDGE, 1.0f );
	return edge * edge * ( 3.0f - 2.0f * edge );
}

bool CNeoSmokeVolume::CurrentBox( Vector &boxMin, Vector &boxMax, float &front ) const
{
	const bool filled = IsDone();
	front = ( filled ? m_flFrontUnits : CNeoSmokeFill::DOME_RADIUS ) * VolSmokeBloom( gpGlobals->curtime - m_flStartTime );
	if ( front <= 0.0f )
	{
		return false;
	}

	boxMin = filled ? m_vecSmokeMin : m_vecOrigin - Vector( front, front, front );
	boxMax = filled ? m_vecSmokeMax : m_vecOrigin + Vector( front, front, front );
	return true;
}

// Clips the segment from `from` along `unit` (length `len`) to the box;
// false when it misses.
static bool ClipSegmentToBox( const Vector &from, const Vector &unit, float len, const Vector &boxMin, const Vector &boxMax, float &t0, float &t1 )
{
	t0 = 0.0f;
	t1 = len;
	for ( int a = 0; a < 3; ++a )
	{
		if ( fabsf( unit[a] ) < 1.0e-6f )
		{
			if ( from[a] < boxMin[a] || from[a] > boxMax[a] )
			{
				return false;
			}
			continue;
		}

		const float ta = ( boxMin[a] - from[a] ) / unit[a];
		const float tb = ( boxMax[a] - from[a] ) / unit[a];
		t0 = MAX( t0, MIN( ta, tb ) );
		t1 = MIN( t1, MAX( ta, tb ) );
	}

	return t0 < t1;
}

bool CNeoSmokeVolume::SegmentCrossesBox( const Vector &from, const Vector &to ) const
{
	Vector boxMin;
	Vector boxMax;
	float front;
	const Vector delta = to - from;
	const float len = delta.Length();
	if ( len < 1.0f || !CurrentBox( boxMin, boxMax, front ) )
	{
		return false;
	}

	float t0;
	float t1;
	return ClipSegmentToBox( from, delta / len, len, boxMin, boxMax, t0, t1 );
}

float CNeoSmokeVolume::PathOpticalDepth( const Vector &from, const Vector &to ) const
{
	const float age = gpGlobals->curtime - m_flStartTime;
	const bool filled = IsDone();
	Vector boxMin;
	Vector boxMax;
	float front;
	const Vector delta = to - from;
	const float len = delta.Length();
	if ( len < 1.0f || !CurrentBox( boxMin, boxMax, front ) )
	{
		return 0.0f;
	}

	const Vector unit = delta / len;
	float t0;
	float t1;
	if ( !ClipSegmentToBox( from, unit, len, boxMin, boxMax, t0, t1 ) )
	{
		return 0.0f;
	}

	float path = 0.0f;
	for ( float t = t0; t < t1; t += NEO_SMOKE_PATH_STEP )
	{
		const float step = MIN( NEO_SMOKE_PATH_STEP, t1 - t );
		const Vector pos = from + unit * ( t + 0.5f * step );
		path += ( filled ? Density( pos, front ) : DomeDensity( pos, front ) ) * step;
	}

	return path * NEO_VOLSMOKE_EXTINCTION * VolSmokeBloom( age ) * VolSmokeFade( age );
}

bool CNeoSmokeVolume::SmokeBox( Vector &mins, Vector &maxs ) const
{
	if ( !IsDone() )
	{
		return false;
	}

	mins = m_vecSmokeMin;
	maxs = m_vecSmokeMax;
	return true;
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
		Msg( "neo_smoke_server_stats: volume %d age %.2f filling%s\n", m_nId, gpGlobals->curtime - m_flStartTime,
			m_pFillJob ? " on a worker" : "" );
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
		IThreadPool *pPool = NEOSmokeFillPool( NEO_SMOKE_SERVER_FILL_MAX_WORKERS );
		const bool worked = pPool ? StepJobs( pPool ) : StepOnMainThread();

		if ( !worked )
		{
			return;
		}

		const double spent = Plat_FloatTime() - start;
		s_flWorstTick = MAX( s_flWorstTick, spent );
		s_flTickSum += spent;
		++s_nFillTicks;
	}

	// The fill jobs trace the map, so they end before it unloads.
	virtual void LevelInitPreEntity() override { NEOSmokeVolume_RemoveAll(); }
	virtual void LevelShutdownPreEntity() override { NEOSmokeVolume_RemoveAll(); }
	virtual void Shutdown() override
	{
		NEOSmokeVolume_RemoveAll();
		NEOSmokeFillPoolShutdown();
	}

private:
	// Collects the finished jobs and starts one job a free worker, the oldest
	// fill first. The list is in creation order. Returns whether a fill ran.
	static bool StepJobs( IThreadPool *pPool )
	{
		bool worked = false;
		int running = 0;
		for ( CNeoSmokeVolume *pVol : s_SmokeVolumes )
		{
			worked |= pVol->CollectJob();
			running += pVol->IsFilling() ? 1 : 0;
		}

		for ( int i = 0; i < s_SmokeVolumes.Count() && running < pPool->NumThreads(); ++i )
		{
			CNeoSmokeVolume *pVol = s_SmokeVolumes[i];
			if ( !pVol->WantsFill() )
			{
				continue;
			}

			pVol->StartJob( pPool );
			++running;
			worked = true;
		}

		return worked || running > 0;
	}

	static bool StepOnMainThread()
	{
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

		return worked;
	}
};

static CNeoSmokeVolumeSystem s_SmokeVolumeSystem;

float NEOSmoke_OpticalDepth( const Vector &from, const Vector &to )
{
	float tau = 0.0f;
	for ( const CNeoSmokeVolume *pVol : s_SmokeVolumes )
	{
		tau += pVol->PathOpticalDepth( from, to );
	}

	return tau;
}

// Halvings of the segment to find where sight is lost: 1/64 of the segment.
static constexpr int NEO_SMOKE_SIGHT_SEARCH_STEPS = 6;

// Only the volumes whose box the whole segment crosses can add depth to a
// part of it, so the search sums just those.
typedef CUtlVectorFixedGrowable< const CNeoSmokeVolume *, 16 > CrossedVolumes_t;

static float SightDepth( const CrossedVolumes_t &volumes, const Vector &from, const Vector &to )
{
	float tau = 0.0f;
	for ( const CNeoSmokeVolume *pVol : volumes )
	{
		tau += pVol->PathOpticalDepth( from, to );
	}

	return tau;
}

float NEOSmoke_SightBlockFraction( const Vector &from, const Vector &to )
{
	CrossedVolumes_t crossed;
	for ( const CNeoSmokeVolume *pVol : s_SmokeVolumes )
	{
		if ( pVol->SegmentCrossesBox( from, to ) )
		{
			crossed.AddToTail( pVol );
		}
	}

	if ( crossed.IsEmpty() || SightDepth( crossed, from, to ) < NEO_SMOKE_SIGHT_BLOCK_DEPTH )
	{
		return 1.0f;
	}

	const Vector delta = to - from;
	float clear = 0.0f;
	float blocked = 1.0f;
	for ( int i = 0; i < NEO_SMOKE_SIGHT_SEARCH_STEPS; ++i )
	{
		const float mid = 0.5f * ( clear + blocked );
		if ( SightDepth( crossed, from, from + delta * mid ) >= NEO_SMOKE_SIGHT_BLOCK_DEPTH )
		{
			blocked = mid;
		}
		else
		{
			clear = mid;
		}
	}

	return blocked;
}

bool NEOSmokeVolume_SmokeBox( const CBaseEntity *pGrenade, Vector &mins, Vector &maxs )
{
	for ( const CNeoSmokeVolume *pVol : s_SmokeVolumes )
	{
		if ( pVol->BelongsTo( pGrenade ) )
		{
			return pVol->SmokeBox( mins, maxs );
		}
	}

	return false;
}

static constexpr int NEO_SMOKE_DEPTH_BENCH_RUNS = 1000;

CON_COMMAND_F( neo_smoke_server_depth, "Print the server's smoke optical depth on a segment and its cost. Args: x1 y1 z1 x2 y2 z2", FCVAR_CHEAT )
{
	if ( !UTIL_IsCommandIssuedByServerAdmin() || args.ArgC() < 7 )
	{
		return;
	}

	const Vector from( atof( args[1] ), atof( args[2] ), atof( args[3] ) );
	const Vector to( atof( args[4] ), atof( args[5] ), atof( args[6] ) );
	const float tau = NEOSmoke_OpticalDepth( from, to );

	const double start = Plat_FloatTime();
	float sink = 0.0f;
	for ( int i = 0; i < NEO_SMOKE_DEPTH_BENCH_RUNS; ++i )
	{
		sink += NEOSmoke_OpticalDepth( from, to );
	}
	const double each = ( Plat_FloatTime() - start ) / NEO_SMOKE_DEPTH_BENCH_RUNS;

	Msg( "neo_smoke_server_depth: (%g %g %g) -> (%g %g %g) depth %.4f, %.2f us a query (%d volumes, check %.1f)\n",
		from.x, from.y, from.z, to.x, to.y, to.z, tau, each * 1.0e6, s_SmokeVolumes.Count(), sink / NEO_SMOKE_DEPTH_BENCH_RUNS );
}

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

	IThreadPool *pPool = NEOSmokeFillPool( NEO_SMOKE_SERVER_FILL_MAX_WORKERS );
	Msg( "neo_smoke_server_stats: live %d; fill ticks %d, mean %.3f ms, worst %.3f ms (%d fill workers)\n",
		s_SmokeVolumes.Count(), s_nFillTicks, s_nFillTicks > 0 ? s_flTickSum * 1000.0 / s_nFillTicks : 0.0,
		s_flWorstTick * 1000.0, pPool ? pPool->NumThreads() : 0 );
	s_flWorstTick = 0.0;
	s_flTickSum = 0.0;
	s_nFillTicks = 0;
}
