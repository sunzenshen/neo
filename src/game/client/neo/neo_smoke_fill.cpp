// NEO volumetric smoke - deterministic gas fill. See neo_smoke_fill.h.

#include "cbase.h"
#include "neo_smoke_fill.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

// The legacy smoke is a dome of this radius on a floor; the gas budget is
// that dome's volume in cells.
static constexpr double NEO_SMOKE_FILL_DOME_RADIUS = 165.0;
static constexpr double NEO_SMOKE_FILL_PI = 3.14159265358979323846;
static constexpr int NEO_SMOKE_FILL_BUDGET = (int)( ( 2.0 / 3.0 ) * NEO_SMOKE_FILL_PI
	* NEO_SMOKE_FILL_DOME_RADIUS * NEO_SMOKE_FILL_DOME_RADIUS * NEO_SMOKE_FILL_DOME_RADIUS
	/ ( (double)CNeoSmokeFill::CELL_SIZE * CNeoSmokeFill::CELL_SIZE * CNeoSmokeFill::CELL_SIZE ) + 0.5 );

// Longest path the gas may take. Keeps a narrow hall inside the grid.
static constexpr int NEO_SMOKE_FILL_REACH_CAP = (int)( 270.0 * CNeoSmokeFill::DIST_SCALE / CNeoSmokeFill::CELL_SIZE );

// A grenade resting in a corner can put the first cell inside the floor.
static constexpr int NEO_SMOKE_FILL_SEED_LIFT = 4;

// Step costs in 1/DIST_SCALE cells: face, edge and corner neighbor.
static constexpr int NEO_SMOKE_FILL_STEP_FACE = 16;
static constexpr int NEO_SMOKE_FILL_STEP_EDGE = 23;
static constexpr int NEO_SMOKE_FILL_STEP_CORNER = 28;

static constexpr int NEO_SMOKE_FILL_UNREACHED = 0x7fffffff;

// Occupancy cache: two bits per question, the cell and its +x, +y, +z links.
enum NeoSmokeOcc_t
{
	NEO_SMOKE_OCC_UNKNOWN = 0,
	NEO_SMOKE_OCC_OPEN = 1,
	NEO_SMOKE_OCC_BLOCKED = 2,
	NEO_SMOKE_OCC_MASK = 3,
};

static const int NEO_SMOKE_OCC_CELL_SHIFT = 0;

static int NeoSmokeOccLinkShift( int axis )
{
	return 2 + 2 * axis;
}

// Floor of the square root, exact for any non-negative int.
static int NeoSmokeISqrt( int v )
{
	int r = 0;
	int bit = 1 << 30;
	while ( bit > v )
	{
		bit >>= 2;
	}

	while ( bit != 0 )
	{
		if ( v >= r + bit )
		{
			v -= r + bit;
			r = ( r >> 1 ) + bit;
		}
		else
		{
			r >>= 1;
		}
		bit >>= 2;
	}

	return r;
}

CNeoSmokeFill::CNeoSmokeFill()
	: m_pWorld( NULL ), m_nHeapCount( 0 ), m_nFinalized( 0 ), m_nFrontDist( 0 ), m_nQueries( 0 ), m_bDone( true )
{
	m_pOcc = new unsigned char[CELLS];
	m_pState = new unsigned char[CELLS];
	m_pDist = new int[CELLS];
	m_pAnchor = new int[CELLS];
	m_pHeap = new int[CELLS];
	m_pHeapPos = new int[CELLS];
	m_pOrder = new int[NEO_SMOKE_FILL_BUDGET];
}

CNeoSmokeFill::~CNeoSmokeFill()
{
	delete[] m_pOcc;
	delete[] m_pState;
	delete[] m_pDist;
	delete[] m_pAnchor;
	delete[] m_pHeap;
	delete[] m_pHeapPos;
	delete[] m_pOrder;
}

void CNeoSmokeFill::Start( INeoSmokeFillWorld *pWorld )
{
	m_pWorld = pWorld;
	m_nHeapCount = 0;
	m_nFinalized = 0;
	m_nFrontDist = 0;
	m_nQueries = 0;
	m_bDone = false;

	for ( int i = 0; i < CELLS; ++i )
	{
		m_pOcc[i] = 0;
		m_pState[i] = STATE_NONE;
		m_pDist[i] = NEO_SMOKE_FILL_UNREACHED;
		m_pAnchor[i] = -1;
		m_pHeapPos[i] = -1;
	}

	const int sx = GRID_XY / 2;
	const int sy = GRID_XY / 2;
	int sz = SEED_CELL_Z;
	for ( int lift = 0; lift < NEO_SMOKE_FILL_SEED_LIFT && !CellOpen( sx, sy, sz ); ++lift )
	{
		++sz;
	}

	if ( !CellOpen( sx, sy, sz ) )
	{
		m_bDone = true;
		return;
	}

	const int seed = Index( sx, sy, sz );
	m_pDist[seed] = 0;
	m_pAnchor[seed] = seed;
	HeapPush( seed );
}

bool CNeoSmokeFill::Step( int maxCells, int maxQueries )
{
	const int queryLimit = m_nQueries + maxQueries;
	for ( int processed = 0; processed < maxCells && m_nQueries < queryLimit; ++processed )
	{
		if ( m_bDone )
		{
			break;
		}

		if ( m_nHeapCount == 0 || m_nFinalized >= NEO_SMOKE_FILL_BUDGET )
		{
			m_bDone = true;
			break;
		}

		const int cell = m_pHeap[0];
		if ( m_pDist[cell] > NEO_SMOKE_FILL_REACH_CAP )
		{
			m_bDone = true;
			break;
		}

		HeapPop();
		m_pState[cell] = STATE_FINAL;
		m_pOrder[m_nFinalized++] = cell;
		m_nFrontDist = m_pDist[cell];

		Relax( cell );
	}

	if ( m_nFinalized >= NEO_SMOKE_FILL_BUDGET )
	{
		m_bDone = true;
	}

	return m_bDone;
}

void CNeoSmokeFill::Relax( int cell )
{
	int c[3];
	Coords( cell, c[0], c[1], c[2] );

	for ( int dx = -1; dx <= 1; ++dx )
	{
		for ( int dy = -1; dy <= 1; ++dy )
		{
			for ( int dz = -1; dz <= 1; ++dz )
			{
				const int axes = ( dx != 0 ) + ( dy != 0 ) + ( dz != 0 );
				if ( axes == 0 )
				{
					continue;
				}

				const int n[3] = { c[0] + dx, c[1] + dy, c[2] + dz };
				if ( !InGrid( n[0], n[1], n[2] ) )
				{
					continue;
				}

				const int next = Index( n[0], n[1], n[2] );
				if ( m_pState[next] == STATE_FINAL )
				{
					continue;
				}

				// No corner cutting: the whole block the move spans must be open.
				if ( !BlockOpen( c, n ) )
				{
					continue;
				}

				// Measure straight from the anchor while it can see the cell, so
				// open space fills as a sphere. Behind a corner the corner cell
				// becomes the new anchor and the gas turns it as a sphere section.
				int dist;
				int anchor;
				const int a = m_pAnchor[cell];
				if ( LineOpen( a, next ) )
				{
					int ac[3];
					Coords( a, ac[0], ac[1], ac[2] );
					const int ex = ac[0] - n[0];
					const int ey = ac[1] - n[1];
					const int ez = ac[2] - n[2];
					dist = m_pDist[a] + NeoSmokeISqrt( DIST_SCALE * DIST_SCALE * ( ex * ex + ey * ey + ez * ez ) );
					anchor = a;
				}
				else
				{
					const int step = ( axes == 1 ) ? NEO_SMOKE_FILL_STEP_FACE
						: ( ( axes == 2 ) ? NEO_SMOKE_FILL_STEP_EDGE : NEO_SMOKE_FILL_STEP_CORNER );
					dist = m_pDist[cell] + step;
					anchor = cell;
				}

				if ( dist >= m_pDist[next] )
				{
					continue;
				}

				m_pDist[next] = dist;
				m_pAnchor[next] = anchor;
				if ( m_pState[next] == STATE_QUEUED )
				{
					HeapUpdate( next );
				}
				else
				{
					HeapPush( next );
				}
			}
		}
	}
}

// ---------------------------------------------------------------------------
// World queries, cached
// ---------------------------------------------------------------------------
bool CNeoSmokeFill::CellOpen( int x, int y, int z )
{
	if ( !InGrid( x, y, z ) )
	{
		return false;
	}

	unsigned char &occ = m_pOcc[Index( x, y, z )];
	int state = ( occ >> NEO_SMOKE_OCC_CELL_SHIFT ) & NEO_SMOKE_OCC_MASK;
	if ( state == NEO_SMOKE_OCC_UNKNOWN )
	{
		++m_nQueries;
		state = m_pWorld->CellSolid( x, y, z ) ? NEO_SMOKE_OCC_BLOCKED : NEO_SMOKE_OCC_OPEN;
		occ |= (unsigned char)( state << NEO_SMOKE_OCC_CELL_SHIFT );
	}

	return state == NEO_SMOKE_OCC_OPEN;
}

bool CNeoSmokeFill::LinkOpen( int x, int y, int z, int axis )
{
	unsigned char &occ = m_pOcc[Index( x, y, z )];
	const int shift = NeoSmokeOccLinkShift( axis );
	int state = ( occ >> shift ) & NEO_SMOKE_OCC_MASK;
	if ( state == NEO_SMOKE_OCC_UNKNOWN )
	{
		++m_nQueries;
		state = m_pWorld->LinkBlocked( x, y, z, axis ) ? NEO_SMOKE_OCC_BLOCKED : NEO_SMOKE_OCC_OPEN;
		occ |= (unsigned char)( state << shift );
	}

	return state == NEO_SMOKE_OCC_OPEN;
}

// p and q differ by at most one cell per axis. Every cell of the block they
// span must be open, and so must every link inside it, so gas cannot slip
// through a wall thinner than a cell or around a corner.
bool CNeoSmokeFill::BlockOpen( const int p[3], const int q[3] )
{
	int lo[3];
	int hi[3];
	for ( int k = 0; k < 3; ++k )
	{
		lo[k] = MIN( p[k], q[k] );
		hi[k] = MAX( p[k], q[k] );
	}

	for ( int z = lo[2]; z <= hi[2]; ++z )
	{
		for ( int y = lo[1]; y <= hi[1]; ++y )
		{
			for ( int x = lo[0]; x <= hi[0]; ++x )
			{
				if ( !CellOpen( x, y, z ) )
				{
					return false;
				}
			}
		}
	}

	for ( int z = lo[2]; z <= hi[2]; ++z )
	{
		for ( int y = lo[1]; y <= hi[1]; ++y )
		{
			for ( int x = lo[0]; x <= hi[0]; ++x )
			{
				const int cell[3] = { x, y, z };
				for ( int k = 0; k < 3; ++k )
				{
					if ( hi[k] == lo[k] || cell[k] != lo[k] )
					{
						continue;
					}

					if ( !LinkOpen( x, y, z, k ) )
					{
						return false;
					}
				}
			}
		}
	}

	return true;
}

// Walks the straight line between two cell centers in half-cell steps; each
// step must pass BlockOpen. Integer only, so the answer is the same on every
// compiler.
bool CNeoSmokeFill::LineOpen( int from, int to )
{
	int a[3];
	int b[3];
	Coords( from, a[0], a[1], a[2] );
	Coords( to, b[0], b[1], b[2] );

	int span = 0;
	for ( int k = 0; k < 3; ++k )
	{
		span = MAX( span, abs( b[k] - a[k] ) );
	}

	const int samples = 2 * span;
	int prev[3] = { a[0], a[1], a[2] };
	for ( int i = 1; i <= samples; ++i )
	{
		// floor( a + 0.5 + ( b - a ) * i / samples ); the numerator is never negative.
		int p[3];
		for ( int k = 0; k < 3; ++k )
		{
			p[k] = ( ( 2 * a[k] + 1 ) * samples + 2 * ( b[k] - a[k] ) * i ) / ( 2 * samples );
		}

		if ( p[0] == prev[0] && p[1] == prev[1] && p[2] == prev[2] )
		{
			continue;
		}

		if ( !BlockOpen( prev, p ) )
		{
			return false;
		}

		prev[0] = p[0];
		prev[1] = p[1];
		prev[2] = p[2];
	}

	return true;
}

// ---------------------------------------------------------------------------
// Indexed binary heap keyed by (distance, cell index): a total order, so the
// fill is the same on every machine.
// ---------------------------------------------------------------------------
bool CNeoSmokeFill::HeapLess( int a, int b ) const
{
	if ( m_pDist[a] != m_pDist[b] )
	{
		return m_pDist[a] < m_pDist[b];
	}

	return a < b;
}

void CNeoSmokeFill::HeapPush( int cell )
{
	m_pState[cell] = STATE_QUEUED;
	m_pHeap[m_nHeapCount] = cell;
	m_pHeapPos[cell] = m_nHeapCount;
	SiftUp( m_nHeapCount++ );
}

// The cell's distance only ever drops while it is queued.
void CNeoSmokeFill::HeapUpdate( int cell )
{
	SiftUp( m_pHeapPos[cell] );
}

int CNeoSmokeFill::HeapPop()
{
	const int top = m_pHeap[0];
	m_pHeapPos[top] = -1;

	--m_nHeapCount;
	if ( m_nHeapCount > 0 )
	{
		m_pHeap[0] = m_pHeap[m_nHeapCount];
		m_pHeapPos[m_pHeap[0]] = 0;
		SiftDown( 0 );
	}

	return top;
}

void CNeoSmokeFill::SiftUp( int pos )
{
	const int cell = m_pHeap[pos];
	while ( pos > 0 )
	{
		const int parent = ( pos - 1 ) / 2;
		if ( !HeapLess( cell, m_pHeap[parent] ) )
		{
			break;
		}

		m_pHeap[pos] = m_pHeap[parent];
		m_pHeapPos[m_pHeap[pos]] = pos;
		pos = parent;
	}

	m_pHeap[pos] = cell;
	m_pHeapPos[cell] = pos;
}

void CNeoSmokeFill::SiftDown( int pos )
{
	const int cell = m_pHeap[pos];
	for ( ;; )
	{
		const int left = 2 * pos + 1;
		if ( left >= m_nHeapCount )
		{
			break;
		}

		int child = left;
		const int right = left + 1;
		if ( right < m_nHeapCount && HeapLess( m_pHeap[right], m_pHeap[left] ) )
		{
			child = right;
		}

		if ( !HeapLess( m_pHeap[child], cell ) )
		{
			break;
		}

		m_pHeap[pos] = m_pHeap[child];
		m_pHeapPos[m_pHeap[pos]] = pos;
		pos = child;
	}

	m_pHeap[pos] = cell;
	m_pHeapPos[cell] = pos;
}
