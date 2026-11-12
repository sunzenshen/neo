// NEO volumetric smoke - the fill's world queries. See neo_smoke_world.h.

#include "cbase.h"
#include "neo_smoke_world.h"
#include "neo_smoke_shared.h"
#include "engine/IEngineTrace.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

static constexpr int NEO_SMOKE_COARSE_BLOCK = CNEOSmokeFill::BLOCK_ALIGN; // cells
static constexpr int NEO_SMOKE_FINE_BLOCK = 2;                            // cells
static constexpr int NEO_SMOKE_FINE_PER_AXIS = NEO_SMOKE_COARSE_BLOCK / NEO_SMOKE_FINE_BLOCK;
static constexpr int NEO_SMOKE_FINE_PER_COARSE = NEO_SMOKE_FINE_PER_AXIS * NEO_SMOKE_FINE_PER_AXIS * NEO_SMOKE_FINE_PER_AXIS;
static_assert( NEO_SMOKE_COARSE_BLOCK % NEO_SMOKE_FINE_BLOCK == 0, "fine blocks must tile a coarse block" );

// Coarse block directory entries; a value above zero is a touched block whose
// fine states start at ( value - 1 ) * NEO_SMOKE_FINE_PER_COARSE.
static constexpr int NEO_SMOKE_COARSE_UNKNOWN = 0;
static constexpr int NEO_SMOKE_COARSE_EMPTY = -1;

static constexpr int NEO_SMOKE_AUDIT_MAX_WARNINGS = 10; // per volume

enum NEOSmokeBlockState_t
{
	NEO_SMOKE_BLOCK_UNKNOWN = 0,
	NEO_SMOKE_BLOCK_EMPTY,
	NEO_SMOKE_BLOCK_TOUCHED,
};

CNEOSmokeWorld::CNEOSmokeWorld( const Vector &gridMin, const int boxSize[3], const int seedCorner[3], const int latticeLo[3], const int latticeHi[3] )
	: m_vecGridMin( gridMin ), m_nTraces( 0 ), m_nAuditChecks( 0 ), m_nAuditDisagree( 0 )
{
	int blocks = 1;
	for ( int a = 0; a < 3; ++a )
	{
		m_nBoxSize[a] = boxSize[a];
		m_nDrawOffset[a] = 0;
		m_nSeedCorner[a] = seedCorner[a];
		m_nLatticeLo[a] = latticeLo[a];
		m_nLatticeHi[a] = latticeHi[a];
		m_nCoarse[a] = ( latticeHi[a] - latticeLo[a] ) / NEO_SMOKE_COARSE_BLOCK;
		blocks *= m_nCoarse[a];
	}

	m_Coarse.SetCount( blocks );
	Q_memset( m_Coarse.Base(), 0, m_Coarse.Count() * sizeof( int ) );
}

bool CNEOSmokeWorld::PointIn( const Vector &pos, int mask )
{
	Ray_t ray;
	ray.Init( pos, pos );
	CTraceFilterWorldOnly filter;
	trace_t tr;
	enginetrace->TraceRay( ray, mask, &filter, &tr );
	return tr.startsolid;
}

bool CNEOSmokeWorld::PointSolid( const Vector &pos )
{
	return PointIn( pos, NEO_VOLSMOKE_FILL_MASK );
}

bool CNEOSmokeWorld::CellSolid( int x, int y, int z )
{
	bool solid = false;
	const bool inEmpty = InEmptyBlock( x, y, z );
	if ( !inEmpty || Auditing() )
	{
		++m_nTraces;
		solid = PointSolid( Center( x, y, z ) );
	}

	if ( inEmpty && Auditing() )
	{
		Audit( solid, x, y, z, -1 );
	}

	return solid;
}

bool CNEOSmokeWorld::LinkBlocked( int x, int y, int z, int axis )
{
	int n[3] = { x, y, z };
	++n[axis];

	const bool inEmpty = InEmptyBlock( x, y, z ) && InEmptyBlock( n[0], n[1], n[2] );
	bool blocked = false;
	if ( !inEmpty || Auditing() )
	{
		++m_nTraces;
		Ray_t ray;
		ray.Init( Center( x, y, z ), Center( n[0], n[1], n[2] ) );
		CTraceFilterWorldOnly filter;
		trace_t tr;
		enginetrace->TraceRay( ray, NEO_VOLSMOKE_FILL_MASK, &filter, &tr );
		blocked = tr.startsolid || tr.fraction < 1.0f;
	}

	if ( inEmpty && Auditing() )
	{
		Audit( blocked, x, y, z, axis );
	}

	return blocked;
}

bool CNEOSmokeWorld::SeedBlocked( int x, int y, int z )
{
	++m_nTraces;
	Ray_t ray;
	ray.Init( Grenade(), Center( x, y, z ) );
	CTraceFilterWorldOnly filter;
	trace_t tr;
	enginetrace->TraceRay( ray, NEO_VOLSMOKE_FILL_MASK, &filter, &tr );
	return tr.startsolid || tr.fraction < 1.0f;
}

bool CNEOSmokeWorld::SeedInWater()
{
	++m_nTraces;
	return PointIn( Grenade(), NEO_VOLSMOKE_WATER_MASK );
}

bool CNEOSmokeWorld::CellWater( int x, int y, int z )
{
	++m_nTraces;
	return PointIn( Center( x, y, z ), NEO_VOLSMOKE_WATER_MASK );
}

// The block answered "open"; the direct query said otherwise. The direct
// answer is the one returned, so an audited fill grows the old field.
void CNEOSmokeWorld::Audit( bool directSolid, int x, int y, int z, int axis )
{
	++m_nAuditChecks;
	if ( !directSolid )
	{
		return;
	}

	++m_nAuditDisagree;
	if ( m_nAuditDisagree > NEO_SMOKE_AUDIT_MAX_WARNINGS )
	{
		return;
	}

	// May run on a fill worker, so it only records; the owner reports on the
	// main thread.
	const NEOSmokeAuditCell_t cell = { { x, y, z }, axis };
	m_AuditCells.AddToTail( cell );
}

bool CNEOSmokeWorld::InEmptyBlock( int x, int y, int z )
{
	if ( !InLattice( x, y, z ) )
	{
		return false;
	}

	// Offsets from the lattice corner, which lies on a block boundary.
	const int ox = x - m_nLatticeLo[0];
	const int oy = y - m_nLatticeLo[1];
	const int oz = z - m_nLatticeLo[2];
	const int cx = ox / NEO_SMOKE_COARSE_BLOCK;
	const int cy = oy / NEO_SMOKE_COARSE_BLOCK;
	const int cz = oz / NEO_SMOKE_COARSE_BLOCK;
	int &coarse = m_Coarse[( cz * m_nCoarse[1] + cy ) * m_nCoarse[0] + cx];
	if ( coarse == NEO_SMOKE_COARSE_UNKNOWN )
	{
		if ( !BoxTouchesWorld( x - ox % NEO_SMOKE_COARSE_BLOCK, y - oy % NEO_SMOKE_COARSE_BLOCK, z - oz % NEO_SMOKE_COARSE_BLOCK, NEO_SMOKE_COARSE_BLOCK ) )
		{
			coarse = NEO_SMOKE_COARSE_EMPTY;
		}
		else
		{
			coarse = m_Fine.Count() / NEO_SMOKE_FINE_PER_COARSE + 1;
			m_Fine.AddMultipleToTail( NEO_SMOKE_FINE_PER_COARSE );
			Q_memset( m_Fine.Base() + m_Fine.Count() - NEO_SMOKE_FINE_PER_COARSE, NEO_SMOKE_BLOCK_UNKNOWN, NEO_SMOKE_FINE_PER_COARSE );
		}
	}

	if ( coarse == NEO_SMOKE_COARSE_EMPTY )
	{
		return true;
	}

	const int fx = ( ox % NEO_SMOKE_COARSE_BLOCK ) / NEO_SMOKE_FINE_BLOCK;
	const int fy = ( oy % NEO_SMOKE_COARSE_BLOCK ) / NEO_SMOKE_FINE_BLOCK;
	const int fz = ( oz % NEO_SMOKE_COARSE_BLOCK ) / NEO_SMOKE_FINE_BLOCK;
	const int fine = ( coarse - 1 ) * NEO_SMOKE_FINE_PER_COARSE + ( fz * NEO_SMOKE_FINE_PER_AXIS + fy ) * NEO_SMOKE_FINE_PER_AXIS + fx;
	unsigned char &state = m_Fine[fine];
	if ( state == NEO_SMOKE_BLOCK_UNKNOWN )
	{
		const bool touched = BoxTouchesWorld( x - ox % NEO_SMOKE_FINE_BLOCK, y - oy % NEO_SMOKE_FINE_BLOCK, z - oz % NEO_SMOKE_FINE_BLOCK, NEO_SMOKE_FINE_BLOCK );
		state = touched ? NEO_SMOKE_BLOCK_TOUCHED : NEO_SMOKE_BLOCK_EMPTY;
	}

	return state == NEO_SMOKE_BLOCK_EMPTY;
}

// Does the cube of b cells whose low corner is cell (x, y, z) touch the world?
bool CNEOSmokeWorld::BoxTouchesWorld( int x, int y, int z, int b )
{
	++m_nTraces;
	const float c = CNEOSmokeFill::CELL_SIZE;
	const float size = b * c;
	const Vector lo = m_vecGridMin + Vector( x * c, y * c, z * c );
	const Vector half( 0.5f * size, 0.5f * size, 0.5f * size );
	const Vector center = lo + half;

	Ray_t ray;
	ray.Init( center, center, -half, half );
	CTraceFilterWorldOnly filter;
	trace_t tr;
	enginetrace->TraceRay( ray, NEO_VOLSMOKE_FILL_MASK, &filter, &tr );
	return tr.startsolid || tr.allsolid;
}
