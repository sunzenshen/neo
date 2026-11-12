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
// finished fields. The workers run at low priority, so the main thread
// keeps its core.

// Game time after detonation at which the queries switch from the nominal
// dome to the field. A fill done by then waits for this tick, so sight
// does not depend on when a worker finished: a fill normally ends long
// before it, so in normal play every field switches at this tick and a
// match replays the same way. A tick never waits for a fill, so a fill
// still running then (many grenades at once, a loaded machine, a high
// host_timescale) switches on the first tick after it ends, and the switch
// then depends on the machine. The delay leaves a fill time to end even at
// a high host_timescale, and the dome front is still small at this age.
static constexpr float NEO_SMOKE_FIELD_LIVE_DELAY = 1.0f;

// Without workers, fill work a tick, in finalized cells: each pending fill
// may take up to PER_FILL, oldest first, until the tick's total reaches the
// cap. A slice also stops after this many world queries per cell, which
// bounds a slice along walls, where a cell costs the most.
static constexpr int NEO_SMOKE_SERVER_CELLS_PER_FILL = 700;
static constexpr int NEO_SMOKE_SERVER_CELLS_PER_TICK = 2800;
static constexpr int NEO_SMOKE_SERVER_QUERIES_PER_CELL = 8;

class CNEOSmokeVolume;

// Where a sight line runs through one volume's box, and what scales the
// volume's density there at its age.
struct NEOSmokeSightSpan_t
{
	const CNEOSmokeVolume *pVolume;
	float t0;
	float t1;
	float front;
	float scale;
	bool filled;
};

class CNEOSmokeVolume
{
public:
	CNEOSmokeVolume( const Vector &origin, CBaseEntity *pGrenade );
	~CNEOSmokeVolume();

	// Fills up to `cells` cells; returns what the slice takes from the budget.
	int Step( int cells );

	bool WantsFill() const { return !m_pFillJob && !m_Fill.IsDone(); }
	bool IsFilling() const { return m_pFillJob != nullptr; }
	void StartJob( IThreadPool *pPool );

	// Picks up the job once it is finished.
	void CollectJob();

	bool IsDone() const { return !m_pFillJob && m_Fill.IsDone(); }

	// Whether the queries read the field: from a fixed tick on, once the
	// fill is done.
	bool IsLive() const { return m_bLive; }
	bool LiveDue() const { return !m_bLive && gpGlobals->tickcount >= m_nLiveTick && IsDone(); }
	void GoLive();
	bool Expired() const;
	unsigned long long LatticeHash() const;
	void Print() const;

	float PathOpticalDepth( const Vector &from, const Vector &to ) const;

	// The part of the segment from `from` along `unit` (length `len`) inside
	// the box that can hold smoke now; false when it misses.
	bool SightSpan( const Vector &from, const Vector &unit, float len, NEOSmokeSightSpan_t &span ) const;

	// Extinction at a point of the span, in optical depth a unit.
	float SpanExtinction( const Vector &pos, const NEOSmokeSightSpan_t &span ) const;

	bool BelongsTo( const CBaseEntity *pGrenade ) const { return m_hGrenade.Get() == pGrenade; }

	// The box that holds the smoke; false until the field is filled.
	bool SmokeBox( Vector &mins, Vector &maxs ) const;

private:
	// The box that can hold smoke at this age; false when there is none.
	bool CurrentBox( Vector &boxMin, Vector &boxMax, float &front ) const;

	void RunFill();
	void PrepareField();
	void ReleaseWorld();
	void FinishFill();
	float Density( const Vector &pos, float front ) const;
	float DomeDensity( const Vector &pos, float front ) const;
	void CornerValue( int x, int y, int z, float &dist, float &weight ) const;

	// Age in whole ticks, so a depth at a given age does not depend on when
	// the grenade went off.
	float Age() const { return TICKS_TO_TIME( gpGlobals->tickcount - m_nStartTick ); }

	int m_nId;
	Vector m_vecOrigin;
	float m_flStartTime;
	int m_nStartTick;
	EHANDLE m_hGrenade;
	CHandle< CNEOSmokeVolumeEnt > m_hNetworked;
	CNEOSmokeFill m_Fill;
	CNEOSmokeWorld *m_pWorld; // freed once the fill is done
	int m_nLiveTick;
	bool m_bLive;
	float m_flLiveAge;

	// The job owns the fill and its world while it runs.
	CJob *m_pFillJob;
	CInterlockedInt m_nFillCancel;

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

int CNEOSmokeVolume::s_nNextId = 0;

static CUtlVector< CNEOSmokeVolume * > s_SmokeVolumes;

// Fields that went live after their tick, since the last print.
static int s_nLateLive = 0;

// Bumped whenever the set of fields or a field's shape changes, so a
// remembered sight result is not reused across such a change.
static int s_nVolumeGeneration = 0;

// Sight queries and the marches they ran, since the last print.
static int s_nSightQueries = 0;
static int s_nSightMarches = 0;

CNEOSmokeVolume::CNEOSmokeVolume( const Vector &origin, CBaseEntity *pGrenade )
	: m_nId( s_nNextId++ ), m_vecOrigin( origin ), m_flStartTime( gpGlobals->curtime ), m_nStartTick( gpGlobals->tickcount ), m_hGrenade( pGrenade ), m_pWorld( nullptr ),
	  m_nLiveTick( gpGlobals->tickcount + TIME_TO_TICKS( NEO_SMOKE_FIELD_LIVE_DELAY ) ), m_bLive( false ), m_flLiveAge( -1.0f ),
	  m_pFillJob( nullptr ), m_flFrontUnits( 0.0f ), m_flDistRange( 0.0f ), m_nFieldHash( 0 )
{
	// The same box and lattice as the client's volume, so the queries and
	// the field are the same.
	int box[3];
	int seedCorner[3];
	m_Fill.BoxSize( box );
	m_Fill.BoxSeedCorner( seedCorner );

	Vector gridMin;
	const float cell = CNEOSmokeFill::CELL_SIZE;
	for ( int a = 0; a < 3; ++a )
	{
		gridMin[a] = origin[a] - seedCorner[a] * cell;
	}

	int latticeLo[3];
	int latticeHi[3];
	m_Fill.LatticeBounds( latticeLo, latticeHi );
	m_pWorld = new CNEOSmokeWorld( gridMin, box, seedCorner, latticeLo, latticeHi );
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

int CNEOSmokeVolume::Step( int cells )
{
	const int cellsBefore = m_Fill.LatticeCount();
	const int queriesBefore = m_Fill.WorldQueries();
	m_Fill.Step( cells, cells * NEO_SMOKE_SERVER_QUERIES_PER_CELL );

	// A slice cut short by its queries is charged for them, so a tick of
	// fills along walls costs no more than a tick in the open.
	const int queryCells = ( m_Fill.WorldQueries() - queriesBefore ) / NEO_SMOKE_SERVER_QUERIES_PER_CELL;
	const int taken = Max( 1, Max( m_Fill.LatticeCount() - cellsBefore, queryCells ) );
	if ( m_Fill.IsDone() )
	{
		PrepareField();
		FinishFill();
	}

	return taken;
}

CNEOSmokeVolume::~CNEOSmokeVolume()
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

void CNEOSmokeVolume::StartJob( IThreadPool *pPool )
{
	m_pFillJob = pPool->QueueCall( this, &CNEOSmokeVolume::RunFill );
}

// The fill job, on a worker.
void CNEOSmokeVolume::RunFill()
{
	NEOSmokeFillRun( m_Fill, m_nFillCancel );
	if ( m_Fill.IsDone() )
	{
		PrepareField();
		ReleaseWorld();
	}
}

void CNEOSmokeVolume::CollectJob()
{
	if ( !m_pFillJob || !m_pFillJob->IsFinished() )
	{
		return;
	}

	m_pFillJob->Release();
	m_pFillJob = nullptr;
	if ( m_Fill.IsDone() )
	{
		FinishFill();
	}

}

void CNEOSmokeVolume::GoLive()
{
	m_bLive = true;
	m_flLiveAge = Age();
	++s_nVolumeGeneration;
	if ( gpGlobals->tickcount > m_nLiveTick )
	{
		++s_nLateLive;
	}
}

// The query reads the finished field only, so the world cache goes. A job
// frees it on its worker, since freeing a large cache shows in a tick.
void CNEOSmokeVolume::ReleaseWorld()
{
	delete m_pWorld;
	m_pWorld = nullptr;
}

void CNEOSmokeVolume::FinishFill()
{
	ReleaseWorld();
	if ( m_hNetworked.Get() )
	{
		m_hNetworked->SetFieldHash( m_nFieldHash );
	}
}

// What the query and the clients need from the finished field. Runs on the
// fill's worker, so the tick does not pay for the scan of the field.
void CNEOSmokeVolume::PrepareField()
{
	m_Fill.BoxSize( m_nBox );
	m_Fill.BoxSeedCorner( m_nSeedCorner );
	m_flFrontUnits = CNEOSmokeFill::DistToUnits( m_Fill.FrontDist() );
	m_flDistRange = Max( NEO_VOLSMOKE_DIST_RANGE, m_flFrontUnits );

	int lo[3] = { m_nBox[0], m_nBox[1], m_nBox[2] };
	int hi[3] = { -1, -1, -1 };
	for ( int i = 0; i < m_Fill.FinalizedCount(); ++i )
	{
		const int idx = m_Fill.FinalizedCell( i );
		const int c[3] = { idx % m_nBox[0], ( idx / m_nBox[0] ) % m_nBox[1], idx / ( m_nBox[0] * m_nBox[1] ) };
		for ( int a = 0; a < 3; ++a )
		{
			lo[a] = Min( lo[a], c[a] );
			hi[a] = Max( hi[a], c[a] );
		}
	}

	// One cell of margin: the trilinear lookup spreads a cell's smoke half a
	// cell past it.
	const float cell = CNEOSmokeFill::CELL_SIZE;
	for ( int a = 0; a < 3; ++a )
	{
		m_vecGridMin[a] = m_vecOrigin[a] - m_nSeedCorner[a] * cell;
		const float gridMax = m_vecGridMin[a] + m_nBox[a] * cell;
		m_vecSmokeMin[a] = Max( m_vecGridMin[a], m_vecGridMin[a] + ( lo[a] - 1 ) * cell );
		m_vecSmokeMax[a] = Min( gridMax, m_vecGridMin[a] + ( hi[a] + 2 ) * cell );
	}

	m_nFieldHash = LatticeHash();
}

// A box cell's path length and weight as the client's atlas holds them: a
// cell without gas has no weight and takes the shortest path length of its
// gas neighbors, so the lookup fades the edge instead of cutting it.
void CNEOSmokeVolume::CornerValue( int x, int y, int z, float &dist, float &weight ) const
{
	const int idx = ( z * m_nBox[1] + y ) * m_nBox[0] + x;
	if ( m_Fill.IsFinalized( idx ) )
	{
		dist = CNEOSmokeFill::DistToUnits( m_Fill.Dist( idx ) );
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
					dist = Min( dist, CNEOSmokeFill::DistToUnits( m_Fill.Dist( nIdx ) ) );
				}
			}
		}
	}
}

// The client's smoke density without the look noise: trilinear over the
// eight nearest cell centers, then the soft edge behind the front.
float CNEOSmokeVolume::Density( const Vector &pos, float front ) const
{
	int base[3];
	float frac[3];
	for ( int a = 0; a < 3; ++a )
	{
		const float g = clamp( ( pos[a] - m_vecGridMin[a] ) / CNEOSmokeFill::CELL_SIZE - 0.5f, 0.0f, (float)( m_nBox[a] - 1 ) );
		base[a] = Min( (int)g, m_nBox[a] - 2 );
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

	const float edge = Min( depth / NEO_VOLSMOKE_EDGE, 1.0f );
	return weight * edge * edge * ( 3.0f - 2.0f * edge );
}

// Before the field is filled: the dome the client draws then.
float CNEOSmokeVolume::DomeDensity( const Vector &pos, float front ) const
{
	const float depth = front - pos.DistTo( m_vecOrigin );
	if ( depth <= 0.0f )
	{
		return 0.0f;
	}

	const float edge = Min( depth / NEO_VOLSMOKE_EDGE, 1.0f );
	return edge * edge * ( 3.0f - 2.0f * edge );
}

bool CNEOSmokeVolume::CurrentBox( Vector &boxMin, Vector &boxMax, float &front ) const
{
	const bool filled = IsLive();
	front = ( filled ? m_flFrontUnits : CNEOSmokeFill::DOME_RADIUS ) * VolSmokeBloom( Age() );
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
		t0 = Max( t0, Min( ta, tb ) );
		t1 = Min( t1, Max( ta, tb ) );
	}

	return t0 < t1;
}

bool CNEOSmokeVolume::SightSpan( const Vector &from, const Vector &unit, float len, NEOSmokeSightSpan_t &span ) const
{
	Vector boxMin;
	Vector boxMax;
	if ( !CurrentBox( boxMin, boxMax, span.front ) || !ClipSegmentToBox( from, unit, len, boxMin, boxMax, span.t0, span.t1 ) )
	{
		return false;
	}

	const float age = Age();
	span.pVolume = this;
	span.filled = IsLive();
	span.scale = NEO_VOLSMOKE_EXTINCTION * VolSmokeBloom( age ) * VolSmokeFade( age );
	return span.scale > 0.0f;
}

float CNEOSmokeVolume::SpanExtinction( const Vector &pos, const NEOSmokeSightSpan_t &span ) const
{
	const float density = span.filled ? Density( pos, span.front ) : DomeDensity( pos, span.front );
	return density * span.scale;
}

float CNEOSmokeVolume::PathOpticalDepth( const Vector &from, const Vector &to ) const
{
	const Vector delta = to - from;
	const float len = delta.Length();
	if ( len < 1.0f )
	{
		return 0.0f;
	}

	const Vector unit = delta / len;
	NEOSmokeSightSpan_t span;
	if ( !SightSpan( from, unit, len, span ) )
	{
		return 0.0f;
	}

	float tau = 0.0f;
	for ( float t = span.t0; t < span.t1; t += NEO_VOLSMOKE_PATH_STEP )
	{
		const float step = Min( NEO_VOLSMOKE_PATH_STEP, span.t1 - t );
		tau += SpanExtinction( from + unit * ( t + 0.5f * step ), span ) * step;
	}

	return tau;
}

bool CNEOSmokeVolume::SmokeBox( Vector &mins, Vector &maxs ) const
{
	if ( !IsLive() )
	{
		return false;
	}

	mins = m_vecSmokeMin;
	maxs = m_vecSmokeMax;
	return true;
}

bool CNEOSmokeVolume::Expired() const
{
	return !m_hGrenade.Get() || Age() > NEO_VOLSMOKE_FADE_END;
}

// The client's lattice hash (neo_volsmoke_stats): cells relative to the
// grenade's cell corner, so the two prints of one field are equal.
unsigned long long CNEOSmokeVolume::LatticeHash() const
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

void CNEOSmokeVolume::Print() const
{
	Msg( "neo_smoke_server_stats: volume %d origin (%.9g %.9g %.9g) age %.2f live at age %.2f lattice %016llx (%d cells, front %.1f)\n",
		m_nId, m_vecOrigin.x, m_vecOrigin.y, m_vecOrigin.z, Age(), m_flLiveAge,
		IsDone() ? LatticeHash() : 0ull, IsDone() ? m_Fill.LatticeCount() : 0, IsDone() ? CNEOSmokeFill::DistToUnits( m_Fill.LatticeFrontDist() ) : 0.0f );
}

void NEOSmokeVolume_Create( const Vector &origin, CBaseEntity *pGrenade )
{
	s_SmokeVolumes.AddToTail( new CNEOSmokeVolume( origin, pGrenade ) );
	++s_nVolumeGeneration;
}

static void NEOSmokeVolume_RemoveAll()
{
	s_SmokeVolumes.PurgeAndDeleteElements();
	++s_nVolumeGeneration;
}

// Steps the pending fills once a tick and removes the fields whose smoke is
// gone.
class CNEOSmokeVolumeSystem final : public CAutoGameSystemPerFrame
{
public:
	CNEOSmokeVolumeSystem() : CAutoGameSystemPerFrame( "CNEOSmokeVolumeSystem" ) {}

	virtual void FrameUpdatePostEntityThink() override
	{
		for ( int i = s_SmokeVolumes.Count() - 1; i >= 0; --i )
		{
			if ( s_SmokeVolumes[i]->Expired() )
			{
				delete s_SmokeVolumes[i];
				s_SmokeVolumes.Remove( i );
				++s_nVolumeGeneration;
			}
		}

		IThreadPool *pPool = NEOSmokeFillPool();
		if ( pPool )
		{
			StepJobs( pPool );
		}
		else
		{
			StepOnMainThread();
		}

		for ( CNEOSmokeVolume *pVol : s_SmokeVolumes )
		{
			if ( pVol->LiveDue() )
			{
				pVol->GoLive();
			}
		}
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
	// fill first. The list is in creation order.
	static void StepJobs( IThreadPool *pPool )
	{
		int running = 0;
		for ( CNEOSmokeVolume *pVol : s_SmokeVolumes )
		{
			pVol->CollectJob();
			running += pVol->IsFilling() ? 1 : 0;
		}

		for ( int i = 0; i < s_SmokeVolumes.Count() && running < pPool->NumThreads(); ++i )
		{
			CNEOSmokeVolume *pVol = s_SmokeVolumes[i];
			if ( !pVol->WantsFill() )
			{
				continue;
			}

			pVol->StartJob( pPool );
			++running;
		}
	}

	static void StepOnMainThread()
	{
		int budget = NEO_SMOKE_SERVER_CELLS_PER_TICK;
		for ( int i = 0; i < s_SmokeVolumes.Count() && budget > 0; ++i )
		{
			CNEOSmokeVolume *pVol = s_SmokeVolumes[i];
			if ( pVol->IsDone() )
			{
				continue;
			}

			budget -= pVol->Step( Min( budget, NEO_SMOKE_SERVER_CELLS_PER_FILL ) );
		}
	}
};

static CNEOSmokeVolumeSystem s_SmokeVolumeSystem;

float NEOSmoke_OpticalDepth( const Vector &from, const Vector &to )
{
	float tau = 0.0f;
	for ( const CNEOSmokeVolume *pVol : s_SmokeVolumes )
	{
		tau += pVol->PathOpticalDepth( from, to );
	}

	return tau;
}

typedef CUtlVectorFixedGrowable< NEOSmokeSightSpan_t, 16 > SightSpans_t;

// One march from `from`: the depth adds up step by step and the march stops
// where it reaches the block depth, so a line costs one pass through the
// volumes it crosses.
static float SightBlockMarch( const Vector &from, const Vector &to )
{
	++s_nSightMarches;

	const Vector delta = to - from;
	const float len = delta.Length();
	if ( len < 1.0f )
	{
		return 1.0f;
	}

	const Vector unit = delta / len;
	SightSpans_t spans;
	float start = len;
	float end = 0.0f;
	for ( const CNEOSmokeVolume *pVol : s_SmokeVolumes )
	{
		NEOSmokeSightSpan_t span;
		if ( !pVol->SightSpan( from, unit, len, span ) )
		{
			continue;
		}

		spans.AddToTail( span );
		start = Min( start, span.t0 );
		end = Max( end, span.t1 );
	}

	float tau = 0.0f;
	for ( float t = start; t < end; t += NEO_VOLSMOKE_PATH_STEP )
	{
		const float step = Min( NEO_VOLSMOKE_PATH_STEP, end - t );
		const float mid = t + 0.5f * step;
		const Vector pos = from + unit * mid;

		float extinction = 0.0f;
		for ( const NEOSmokeSightSpan_t &span : spans )
		{
			if ( mid >= span.t0 && mid < span.t1 )
			{
				extinction += span.pVolume->SpanExtinction( pos, span );
			}
		}

		const float stepDepth = extinction * step;
		if ( tau + stepDepth >= NEO_SMOKE_SIGHT_BLOCK_DEPTH )
		{
			// The step's sample stands for the whole step, so the depth grows
			// evenly across it.
			const float blocked = t + step * ( NEO_SMOKE_SIGHT_BLOCK_DEPTH - tau ) / stepDepth;
			return blocked / len;
		}

		tau += stepDepth;
	}

	return 1.0f;
}

// Each blocker whose box the line crosses asks for the same line in the same
// tick, so overlapping clouds share one march. The result does not depend on
// who looks: a viewer who sees through smoke never reaches this query.
float NEOSmoke_SightBlockFraction( const Vector &from, const Vector &to )
{
	static Vector s_vecFrom;
	static Vector s_vecTo;
	static int s_nTick = -1;
	static int s_nGeneration = -1;
	static float s_flFraction = 1.0f;

	++s_nSightQueries;
	if ( s_nTick == gpGlobals->tickcount && s_nGeneration == s_nVolumeGeneration && s_vecFrom == from && s_vecTo == to )
	{
		return s_flFraction;
	}

	s_vecFrom = from;
	s_vecTo = to;
	s_nTick = gpGlobals->tickcount;
	s_nGeneration = s_nVolumeGeneration;
	s_flFraction = SightBlockMarch( from, to );
	return s_flFraction;
}

bool NEOSmokeVolume_SmokeBox( const CBaseEntity *pGrenade, Vector &mins, Vector &maxs )
{
	for ( const CNEOSmokeVolume *pVol : s_SmokeVolumes )
	{
		if ( pVol->BelongsTo( pGrenade ) )
		{
			return pVol->SmokeBox( mins, maxs );
		}
	}

	return false;
}

CON_COMMAND_F( neo_smoke_server_stats, "Print the server's smoke fields.", FCVAR_CHEAT )
{
	if ( !UTIL_IsCommandIssuedByServerAdmin() )
	{
		return;
	}

	for ( const CNEOSmokeVolume *pVol : s_SmokeVolumes )
	{
		pVol->Print();
	}

	Msg( "neo_smoke_server_stats: live %d, late %d, sight queries %d, marches %d\n", s_SmokeVolumes.Count(), s_nLateLive, s_nSightQueries, s_nSightMarches );
	s_nLateLive = 0;
	s_nSightQueries = 0;
	s_nSightMarches = 0;
}
