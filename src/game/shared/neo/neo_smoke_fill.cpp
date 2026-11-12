// NEO volumetric smoke - deterministic gas fill. See neo_smoke_fill.h.

#include "cbase.h"
#include "neo_smoke_fill.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

static constexpr double NEO_SMOKE_FILL_DOME_RADIUS = CNEOSmokeFill::DOME_RADIUS;
static constexpr double NEO_SMOKE_FILL_PI = 3.14159265358979323846;
static constexpr int NEO_SMOKE_FILL_BUDGET = (int)( ( 2.0 / 3.0 ) * NEO_SMOKE_FILL_PI
	* NEO_SMOKE_FILL_DOME_RADIUS * NEO_SMOKE_FILL_DOME_RADIUS * NEO_SMOKE_FILL_DOME_RADIUS
	/ ( (double)CNEOSmokeFill::CELL_SIZE * CNEOSmokeFill::CELL_SIZE * CNEOSmokeFill::CELL_SIZE ) + 0.5 );

// The gas box, padded by a cell on each side, must fit a square atlas of
// this many texels a side in z slices, or the cell that would grow it past
// that is dropped.
static constexpr int NEO_SMOKE_FILL_ATLAS_SIZE = CNEOSmokeFill::ATLAS_SIZE;

// Finished path lengths are 16 bits; a longer path, possible only in a
// winding duct, is stored as the longest.
static constexpr int NEO_SMOKE_FILL_MAX_FINAL_DIST = 0xffff;

// A grenade resting in a corner can put the first cell inside the floor.
static constexpr int NEO_SMOKE_FILL_SEED_LIFT = 4;

// The grenade sits on a grid corner. The cell above and toward +x +y is the
// usual seed; the other seven touch the same corner, so one of them is on the
// grenade's side of a wall it rests against.
static constexpr int NEO_SMOKE_FILL_SEED_CORNERS = 8;

// Step costs in 1/DIST_SCALE cells: face, edge and corner neighbor.
static constexpr int NEO_SMOKE_FILL_STEP_FACE = 16;
static constexpr int NEO_SMOKE_FILL_STEP_EDGE = 23;
static constexpr int NEO_SMOKE_FILL_STEP_CORNER = 28;

static constexpr int NEO_SMOKE_FILL_UNREACHED = 0x7fffffff;

// Occupancy cache: two bits per question, the cell and its +x, +y, +z links.
enum NEOSmokeOcc_t
{
	NEO_SMOKE_OCC_UNKNOWN = 0,
	NEO_SMOKE_OCC_OPEN = 1,
	NEO_SMOKE_OCC_BLOCKED = 2,
	NEO_SMOKE_OCC_MASK = 3,
};

static constexpr int NEO_SMOKE_OCC_CELL_SHIFT = 0;

// Move code ( dx + 1 ) + 3 * ( dy + 1 ) + 9 * ( dz + 1 ) of no move. A move and
// its reverse sum to twice this.
static constexpr int NEO_SMOKE_MOVE_SELF = 13;

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

// Unless the fill spends its budget early, most of the cells it queues stay
// in the heap, so it starts big enough for an open dome.
static constexpr int NEO_SMOKE_FILL_HEAP_START = 8192;

// Bricks a fill usually touches; the list grows past this as needed.
static constexpr int NEO_SMOKE_FILL_BRICKS_START = 256;

CNEOSmokeFill::CNEOSmokeFill()
	: m_pWorld( nullptr ), m_pBrickDir( nullptr ), m_ppBricks( nullptr ), m_nBricks( 0 ), m_nBrickCap( 0 ), m_pHeap( nullptr ),
	  m_nHeapCap( 0 ), m_pOrder( nullptr ), m_pOrderDist( nullptr ), m_pBoxOrder( nullptr ), m_pFinalBits( nullptr ), m_pWordRank( nullptr ), m_pFinalDist( nullptr ), m_nHeapCount( 0 ),
	  m_nFinalized( 0 ), m_nBoxFinalized( 0 ), m_nBoxFront( 0 ), m_nWords( 0 ), m_nQueries( 0 ), m_bDone( true ), m_bBegun( false )
{
}

CNEOSmokeFill::~CNEOSmokeFill()
{
	FreeWork();
	delete[] m_pOrder;
	delete[] m_pOrderDist;
	delete[] m_pBoxOrder;
	delete[] m_pFinalBits;
	delete[] m_pWordRank;
	delete[] m_pFinalDist;
}

void CNEOSmokeFill::AllocWork()
{
	m_pBrickDir = new int[BRICKS];
	memset( m_pBrickDir, 0, BRICKS * sizeof( int ) );
	m_nBrickCap = NEO_SMOKE_FILL_BRICKS_START;
	m_ppBricks = new Brick_t *[m_nBrickCap];
	m_nBricks = 0;
	m_nHeapCap = NEO_SMOKE_FILL_HEAP_START;
	m_pHeap = new HeapEntry_t[m_nHeapCap];
}

void CNEOSmokeFill::FreeWork()
{
	for ( int i = 0; i < m_nBricks; ++i )
	{
		delete m_ppBricks[i];
	}

	delete[] m_ppBricks;
	delete[] m_pBrickDir;
	delete[] m_pHeap;
	m_ppBricks = nullptr;
	m_pBrickDir = nullptr;
	m_pHeap = nullptr;
	m_nBricks = 0;
	m_nBrickCap = 0;
	m_nHeapCap = 0;
}

int CNEOSmokeFill::BrickOf( int cell )
{
	int x;
	int y;
	int z;
	LatticeCoords( cell, x, y, z );
	return ( ( z >> BRICK_SHIFT ) * BRICKS_XY + ( y >> BRICK_SHIFT ) ) * BRICKS_XY + ( x >> BRICK_SHIFT );
}

int CNEOSmokeFill::BrickSlot( int cell )
{
	int x;
	int y;
	int z;
	LatticeCoords( cell, x, y, z );
	const int mask = BRICK_SIZE - 1;
	return ( ( ( ( z & mask ) << BRICK_SHIFT ) + ( y & mask ) ) << BRICK_SHIFT ) + ( x & mask );
}

const CNEOSmokeFill::Brick_t *CNEOSmokeFill::FindBrick( int cell ) const
{
	const int n = m_pBrickDir[BrickOf( cell )];
	return n ? m_ppBricks[n - 1] : nullptr;
}

CNEOSmokeFill::Brick_t &CNEOSmokeFill::TouchBrick( int cell )
{
	int &n = m_pBrickDir[BrickOf( cell )];
	if ( n )
	{
		return *m_ppBricks[n - 1];
	}

	if ( m_nBricks == m_nBrickCap )
	{
		Brick_t **ppGrown = new Brick_t *[2 * m_nBrickCap];
		memcpy( ppGrown, m_ppBricks, m_nBricks * sizeof( Brick_t * ) );
		delete[] m_ppBricks;
		m_ppBricks = ppGrown;
		m_nBrickCap *= 2;
	}

	Brick_t *pBrick = new Brick_t;
	for ( int i = 0; i < BRICK_CELLS; ++i )
	{
		pBrick->dist[i] = NEO_SMOKE_FILL_UNREACHED;
		pBrick->anchor[i] = -1;
		pBrick->heapPos[i] = -1;
	}
	memset( pBrick->moveMemo, 0, sizeof( pBrick->moveMemo ) );
	memset( pBrick->occ, 0, sizeof( pBrick->occ ) );
	memset( pBrick->state, STATE_NONE, sizeof( pBrick->state ) );

	m_ppBricks[m_nBricks++] = pBrick;
	n = m_nBricks;
	return *pBrick;
}

int CNEOSmokeFill::CellState( int cell ) const
{
	const Brick_t *pBrick = FindBrick( cell );
	return pBrick ? pBrick->state[BrickSlot( cell )] : (int)STATE_NONE;
}

int CNEOSmokeFill::CellDist( int cell ) const
{
	const Brick_t *pBrick = FindBrick( cell );
	return pBrick ? pBrick->dist[BrickSlot( cell )] : NEO_SMOKE_FILL_UNREACHED;
}

static int NeoSmokePopCount( unsigned long long v )
{
	v = v - ( ( v >> 1 ) & 0x5555555555555555ull );
	v = ( v & 0x3333333333333333ull ) + ( ( v >> 2 ) & 0x3333333333333333ull );
	v = ( v + ( v >> 4 ) ) & 0x0f0f0f0f0f0f0f0full;
	return (int)( ( v * 0x0101010101010101ull ) >> 56 );
}

static void ShrinkToCount( int *&pArray, int count )
{
	int *pShrunk = new int[Max( count, 1 )];
	memcpy( pShrunk, pArray, count * sizeof( int ) );
	delete[] pArray;
	pArray = pShrunk;
}

// The listed box: the gas box with a cell of margin, so a trilinear lookup
// beside the gas stays inside it.
void CNEOSmokeFill::FitBox()
{
	const int latticeHi[3] = { LATTICE_XY - 1, LATTICE_XY - 1, LATTICE_Z - 1 };
	const int seed[3] = { LATTICE_XY / 2, LATTICE_XY / 2, LATTICE_SEED_Z };
	for ( int k = 0; k < 3; ++k )
	{
		int lo = seed[k] - 1;
		int hi = seed[k];
		if ( m_nFinalized > 0 )
		{
			lo = m_nLo[k] - 1;
			hi = m_nHi[k] + 1;
		}

		lo = Max( lo, 0 );
		hi = Min( hi, latticeHi[k] );
		m_nBoxLo[k] = lo;
		m_nBoxSize[k] = hi - lo + 1;
	}

	m_nBoxFinalized = 0;
	m_nBoxFront = 0;
	for ( int i = 0; i < m_nFinalized; ++i )
	{
		int c[3];
		LatticeCoords( m_pOrder[i], c[0], c[1], c[2] );
		bool inside = true;
		for ( int k = 0; k < 3; ++k )
		{
			c[k] -= m_nBoxLo[k];
			inside = inside && c[k] >= 0 && c[k] < m_nBoxSize[k];
		}

		if ( !inside )
		{
			continue;
		}

		m_pBoxOrder[m_nBoxFinalized++] = BoxIndex( c[0], c[1], c[2] );
		m_nBoxFront = m_pOrderDist[i];
	}
}

// Keeps what the finished field needs and frees the work arrays.
void CNEOSmokeFill::Finish()
{
	FitBox();

	m_nWords = ( m_nBoxSize[0] * m_nBoxSize[1] * m_nBoxSize[2] + 63 ) / 64;
	m_pFinalBits = new unsigned long long[m_nWords];
	m_pWordRank = new int[m_nWords];
	m_pFinalDist = new unsigned short[Max( m_nBoxFinalized, 1 )];
	memset( m_pFinalBits, 0, m_nWords * sizeof( *m_pFinalBits ) );

	for ( int i = 0; i < m_nBoxFinalized; ++i )
	{
		const int idx = m_pBoxOrder[i];
		m_pFinalBits[idx >> 6] |= 1ull << ( idx & 63 );
	}

	int rank = 0;
	for ( int w = 0; w < m_nWords; ++w )
	{
		m_pWordRank[w] = rank;
		rank += NeoSmokePopCount( m_pFinalBits[w] );
	}

	for ( int i = 0; i < m_nBoxFinalized; ++i )
	{
		const int idx = m_pBoxOrder[i];
		const int x = idx % m_nBoxSize[0];
		const int y = ( idx / m_nBoxSize[0] ) % m_nBoxSize[1];
		const int z = idx / ( m_nBoxSize[0] * m_nBoxSize[1] );
		const int cell = LatticeIndex( x + m_nBoxLo[0], y + m_nBoxLo[1], z + m_nBoxLo[2] );
		m_pFinalDist[FinalRank( idx )] = (unsigned short)Min( CellDist( cell ), NEO_SMOKE_FILL_MAX_FINAL_DIST );
	}

	ShrinkToCount( m_pOrder, m_nFinalized );
	ShrinkToCount( m_pOrderDist, m_nFinalized );
	ShrinkToCount( m_pBoxOrder, m_nBoxFinalized );

	FreeWork();
}

void CNEOSmokeFill::BoxSize( int size[3] ) const
{
	const int frame[3] = { GRID_XY, GRID_XY, GRID_Z };
	for ( int k = 0; k < 3; ++k )
	{
		size[k] = m_pFinalBits ? m_nBoxSize[k] : frame[k];
	}
}

void CNEOSmokeFill::BoxSeedCorner( int corner[3] ) const
{
	int offset[3];
	BoxOffset( offset );
	corner[0] = GRID_XY / 2 - offset[0];
	corner[1] = GRID_XY / 2 - offset[1];
	corner[2] = SEED_CELL_Z - offset[2];
}

void CNEOSmokeFill::BoxOffset( int offset[3] ) const
{
	const int shift[3] = { BOX_SHIFT_XY, BOX_SHIFT_XY, BOX_SHIFT_Z };
	for ( int k = 0; k < 3; ++k )
	{
		offset[k] = m_pFinalBits ? m_nBoxLo[k] - shift[k] : 0;
	}
}

bool CNEOSmokeFill::IsFinalized( int idx ) const
{
	return m_pFinalBits && ( ( m_pFinalBits[idx >> 6] >> ( idx & 63 ) ) & 1 ) != 0;
}

int CNEOSmokeFill::Dist( int idx ) const
{
	return m_pFinalDist[FinalRank( idx )];
}

int CNEOSmokeFill::FinalRank( int idx ) const
{
	const int w = idx >> 6;
	const unsigned long long below = ( 1ull << ( idx & 63 ) ) - 1;
	return m_pWordRank[w] + NeoSmokePopCount( m_pFinalBits[w] & below );
}

void CNEOSmokeFill::Start( INEOSmokeFillWorld *pWorld )
{
	FreeWork();
	delete[] m_pOrder;
	delete[] m_pOrderDist;
	delete[] m_pBoxOrder;
	delete[] m_pFinalBits;
	delete[] m_pWordRank;
	delete[] m_pFinalDist;
	m_pOrder = nullptr;
	m_pOrderDist = nullptr;
	m_pBoxOrder = nullptr;
	m_pFinalBits = nullptr;
	m_pWordRank = nullptr;
	m_pFinalDist = nullptr;

	m_pWorld = pWorld;
	m_nHeapCount = 0;
	m_nFinalized = 0;
	m_nBoxFinalized = 0;
	m_nBoxFront = 0;
	m_nQueries = 0;
	m_bDone = false;
	m_bBegun = false;
}

// The work arrays are allocated by the first step, not by Start: fills run
// one at a time, so many waiting fills then hold one work set, not one each.
void CNEOSmokeFill::Begin()
{
	m_bBegun = true;
	AllocWork();
	m_pOrder = new int[NEO_SMOKE_FILL_BUDGET];
	m_pOrderDist = new int[NEO_SMOKE_FILL_BUDGET];
	m_pBoxOrder = new int[NEO_SMOKE_FILL_BUDGET];
	for ( int k = 0; k < 3; ++k )
	{
		m_nLo[k] = LATTICE_XY;
		m_nHi[k] = -1;
	}

	const int seed = FindSeed();
	if ( seed < 0 )
	{
		m_bDone = true;
		Finish();
		return;
	}

	Brick_t &brick = TouchBrick( seed );
	const int slot = BrickSlot( seed );
	brick.dist[slot] = 0;
	brick.anchor[slot] = seed;
	HeapPush( seed, 0 );
}

bool CNEOSmokeFill::SeedCellOk( int x, int y, int z )
{
	if ( !CellOpen( x, y, z ) )
	{
		return false;
	}

	++m_nQueries;
	return !m_pWorld->SeedBlocked( x - BOX_SHIFT_XY, y - BOX_SHIFT_XY, z - BOX_SHIFT_Z );
}

// The seed must be open and in plain sight of the grenade, or a grenade
// resting against a thin wall or under a slab starts the gas on the far side.
int CNEOSmokeFill::FindSeed()
{
	const int cx = LATTICE_XY / 2;
	const int cy = LATTICE_XY / 2;
	const int cz = LATTICE_SEED_Z;
	for ( int i = 0; i < NEO_SMOKE_FILL_SEED_CORNERS; ++i )
	{
		const int x = cx - ( i & 1 );
		const int y = cy - ( ( i >> 1 ) & 1 );
		const int z = cz - ( ( i >> 2 ) & 1 );
		if ( SeedCellOk( x, y, z ) )
		{
			return LatticeIndex( x, y, z );
		}
	}

	for ( int lift = 1; lift <= NEO_SMOKE_FILL_SEED_LIFT; ++lift )
	{
		for ( int i = 0; i < NEO_SMOKE_FILL_SEED_CORNERS / 2; ++i )
		{
			const int x = cx - ( i & 1 );
			const int y = cy - ( ( i >> 1 ) & 1 );
			if ( SeedCellOk( x, y, cz + lift ) )
			{
				return LatticeIndex( x, y, cz + lift );
			}
		}
	}

	return FindSeedAboveWater();
}

// A grenade under water starts the gas at the first air cell straight above
// it, so it still smokes from the surface instead of making nothing.
int CNEOSmokeFill::FindSeedAboveWater()
{
	++m_nQueries;
	if ( !m_pWorld->SeedInWater() )
	{
		return -1;
	}

	const int x = LATTICE_XY / 2;
	const int y = LATTICE_XY / 2;
	int z = LATTICE_SEED_Z;
	while ( z < LATTICE_Z )
	{
		++m_nQueries;
		if ( !m_pWorld->CellWater( x - BOX_SHIFT_XY, y - BOX_SHIFT_XY, z - BOX_SHIFT_Z ) )
		{
			break;
		}

		++z;
	}

	if ( z >= LATTICE_Z || !CellOpen( x, y, z ) )
	{
		return -1;
	}

	return LatticeIndex( x, y, z );
}

bool CNEOSmokeFill::Step( int maxCells, int maxQueries )
{
	if ( !m_bBegun && !m_bDone )
	{
		Begin();
	}

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

		const HeapEntry_t top = m_pHeap[0];
		HeapPop();
		const int cell = top.cell;
		if ( !FitsAtlas( cell ) )
		{
			TouchBrick( cell ).state[BrickSlot( cell )] = STATE_DROPPED;
			continue;
		}

		TouchBrick( cell ).state[BrickSlot( cell )] = STATE_FINAL;
		m_pOrder[m_nFinalized] = cell;
		m_pOrderDist[m_nFinalized] = top.dist;
		++m_nFinalized;

		Relax( cell );
	}

	if ( m_nFinalized >= NEO_SMOKE_FILL_BUDGET )
	{
		m_bDone = true;
	}

	if ( m_bDone && m_pBrickDir )
	{
		Finish();
	}

	return m_bDone;
}

// Grows the gas box by the cell when the padded box still fits the atlas.
// Integer only and decided in pop order, so every client drops the same cells.
bool CNEOSmokeFill::FitsAtlas( int cell )
{
	int c[3];
	LatticeCoords( cell, c[0], c[1], c[2] );

	int lo[3];
	int hi[3];
	for ( int k = 0; k < 3; ++k )
	{
		lo[k] = Min( m_nLo[k], c[k] );
		hi[k] = Max( m_nHi[k], c[k] );
	}

	const int w = hi[0] - lo[0] + 3;
	const int h = hi[1] - lo[1] + 3;
	const int d = hi[2] - lo[2] + 3;
	if ( ( NEO_SMOKE_FILL_ATLAS_SIZE / w ) * ( NEO_SMOKE_FILL_ATLAS_SIZE / h ) < d )
	{
		return false;
	}

	for ( int k = 0; k < 3; ++k )
	{
		m_nLo[k] = lo[k];
		m_nHi[k] = hi[k];
	}

	return true;
}

void CNEOSmokeFill::Relax( int cell )
{
	int c[3];
	LatticeCoords( cell, c[0], c[1], c[2] );

	// Measure straight from the anchor while it can see the cell, so open
	// space fills as a sphere. Behind a corner the corner cell becomes the new
	// anchor and the gas turns it as a sphere section.
	const Brick_t &home = *FindBrick( cell );
	const int cellDist = home.dist[BrickSlot( cell )];
	const int a = home.anchor[BrickSlot( cell )];
	const int anchorDist = CellDist( a );
	int ac[3];
	LatticeCoords( a, ac[0], ac[1], ac[2] );

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
				if ( !InLattice( n[0], n[1], n[2] ) )
				{
					continue;
				}

				const int next = LatticeIndex( n[0], n[1], n[2] );
				const Brick_t *pNext = FindBrick( next );
				const int nextSlot = BrickSlot( next );
				const int nextState = pNext ? pNext->state[nextSlot] : (int)STATE_NONE;
				if ( nextState == STATE_FINAL )
				{
					continue;
				}

				const int nextDist = pNext ? pNext->dist[nextSlot] : NEO_SMOKE_FILL_UNREACHED;
				const int ex = ac[0] - n[0];
				const int ey = ac[1] - n[1];
				const int ez = ac[2] - n[2];
				const int lineDist = anchorDist + NeoSmokeISqrt( DIST_SCALE * DIST_SCALE * ( ex * ex + ey * ey + ez * ez ) );
				const int step = ( axes == 1 ) ? NEO_SMOKE_FILL_STEP_FACE
					: ( ( axes == 2 ) ? NEO_SMOKE_FILL_STEP_EDGE : NEO_SMOKE_FILL_STEP_CORNER );
				const int stepDist = cellDist + step;

				// Only these two values can be written, so when neither improves
				// the neighbor no world answer can change the field.
				if ( Min( lineDist, stepDist ) >= nextDist )
				{
					continue;
				}

				// No corner cutting: the whole block the move spans must be open.
				if ( !BlockOpen( c, n ) )
				{
					continue;
				}

				int dist = stepDist;
				int anchor = cell;
				if ( LineOpen( a, next ) )
				{
					dist = lineDist;
					anchor = a;
				}

				if ( dist >= nextDist )
				{
					continue;
				}

				Brick_t &brick = TouchBrick( next );
				brick.dist[nextSlot] = dist;
				brick.anchor[nextSlot] = anchor;
				if ( nextState == STATE_QUEUED )
				{
					HeapUpdate( next, dist );
				}
				else
				{
					HeapPush( next, dist );
				}
			}
		}
	}
}

// ---------------------------------------------------------------------------
// World queries, cached
// ---------------------------------------------------------------------------
bool CNEOSmokeFill::CellOpen( int x, int y, int z )
{
	if ( !InLattice( x, y, z ) )
	{
		return false;
	}

	const int idx = LatticeIndex( x, y, z );
	unsigned char &occ = TouchBrick( idx ).occ[BrickSlot( idx )];
	int state = ( occ >> NEO_SMOKE_OCC_CELL_SHIFT ) & NEO_SMOKE_OCC_MASK;
	if ( state == NEO_SMOKE_OCC_UNKNOWN )
	{
		++m_nQueries;
		state = m_pWorld->CellSolid( x - BOX_SHIFT_XY, y - BOX_SHIFT_XY, z - BOX_SHIFT_Z ) ? NEO_SMOKE_OCC_BLOCKED : NEO_SMOKE_OCC_OPEN;
		occ |= (unsigned char)( state << NEO_SMOKE_OCC_CELL_SHIFT );
	}

	return state == NEO_SMOKE_OCC_OPEN;
}

bool CNEOSmokeFill::LinkOpen( int x, int y, int z, int axis )
{
	const int idx = LatticeIndex( x, y, z );
	unsigned char &occ = TouchBrick( idx ).occ[BrickSlot( idx )];
	const int shift = NeoSmokeOccLinkShift( axis );
	int state = ( occ >> shift ) & NEO_SMOKE_OCC_MASK;
	if ( state == NEO_SMOKE_OCC_UNKNOWN )
	{
		++m_nQueries;
		state = m_pWorld->LinkBlocked( x - BOX_SHIFT_XY, y - BOX_SHIFT_XY, z - BOX_SHIFT_Z, axis ) ? NEO_SMOKE_OCC_BLOCKED : NEO_SMOKE_OCC_OPEN;
		occ |= (unsigned char)( state << shift );
	}

	return state == NEO_SMOKE_OCC_OPEN;
}

// p and q differ by at most one cell per axis. The answer depends only on the
// cached occupancy, so it is remembered per cell and move: two bits for each
// of the 13 moves, a move and its reverse sharing one entry.
bool CNEOSmokeFill::BlockOpen( const int p[3], const int q[3] )
{
	int move = ( q[0] - p[0] + 1 ) + 3 * ( q[1] - p[1] + 1 ) + 9 * ( q[2] - p[2] + 1 );
	const int *base = p;
	if ( move < NEO_SMOKE_MOVE_SELF )
	{
		move = 2 * NEO_SMOKE_MOVE_SELF - move;
		base = q;
	}

	const int shift = 2 * ( move - NEO_SMOKE_MOVE_SELF - 1 );
	const int idx = LatticeIndex( base[0], base[1], base[2] );
	const Brick_t *pBase = FindBrick( idx );
	const int known = pBase ? ( pBase->moveMemo[BrickSlot( idx )] >> shift ) & NEO_SMOKE_OCC_MASK : (int)NEO_SMOKE_OCC_UNKNOWN;
	if ( known != NEO_SMOKE_OCC_UNKNOWN )
	{
		return known == NEO_SMOKE_OCC_OPEN;
	}

	const bool open = BlockOpenUncached( p, q );
	unsigned int &memo = TouchBrick( idx ).moveMemo[BrickSlot( idx )];
	memo |= (unsigned int)( open ? NEO_SMOKE_OCC_OPEN : NEO_SMOKE_OCC_BLOCKED ) << shift;
	return open;
}

// Every cell of the block p and q span must be open, and so must every link
// inside it, so gas cannot slip through a wall thinner than a cell or around
// a corner.
bool CNEOSmokeFill::BlockOpenUncached( const int p[3], const int q[3] )
{
	int lo[3];
	int hi[3];
	for ( int k = 0; k < 3; ++k )
	{
		lo[k] = Min( p[k], q[k] );
		hi[k] = Max( p[k], q[k] );
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
bool CNEOSmokeFill::LineOpen( int from, int to )
{
	int a[3];
	int b[3];
	LatticeCoords( from, a[0], a[1], a[2] );
	LatticeCoords( to, b[0], b[1], b[2] );

	int span = 0;
	for ( int k = 0; k < 3; ++k )
	{
		span = Max( span, abs( b[k] - a[k] ) );
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
bool CNEOSmokeFill::HeapLess( const HeapEntry_t &a, const HeapEntry_t &b )
{
	if ( a.dist != b.dist )
	{
		return a.dist < b.dist;
	}

	return a.cell < b.cell;
}

void CNEOSmokeFill::HeapPlace( int pos, const HeapEntry_t &entry )
{
	m_pHeap[pos] = entry;
	TouchBrick( entry.cell ).heapPos[BrickSlot( entry.cell )] = pos;
}

void CNEOSmokeFill::HeapPush( int cell, int dist )
{
	if ( m_nHeapCount == m_nHeapCap )
	{
		HeapEntry_t *pGrown = new HeapEntry_t[2 * m_nHeapCap];
		memcpy( pGrown, m_pHeap, m_nHeapCount * sizeof( HeapEntry_t ) );
		delete[] m_pHeap;
		m_pHeap = pGrown;
		m_nHeapCap *= 2;
	}

	TouchBrick( cell ).state[BrickSlot( cell )] = STATE_QUEUED;
	const HeapEntry_t entry = { dist, cell };
	HeapPlace( m_nHeapCount, entry );
	SiftUp( m_nHeapCount++ );
}

// The cell's distance only ever drops while it is queued.
void CNEOSmokeFill::HeapUpdate( int cell, int dist )
{
	const int pos = FindBrick( cell )->heapPos[BrickSlot( cell )];
	m_pHeap[pos].dist = dist;
	SiftUp( pos );
}

void CNEOSmokeFill::HeapPop()
{
	const int top = m_pHeap[0].cell;
	TouchBrick( top ).heapPos[BrickSlot( top )] = -1;

	--m_nHeapCount;
	if ( m_nHeapCount > 0 )
	{
		HeapPlace( 0, m_pHeap[m_nHeapCount] );
		SiftDown( 0 );
	}
}

void CNEOSmokeFill::SiftUp( int pos )
{
	const HeapEntry_t entry = m_pHeap[pos];
	while ( pos > 0 )
	{
		const int parent = ( pos - 1 ) / 2;
		if ( !HeapLess( entry, m_pHeap[parent] ) )
		{
			break;
		}

		HeapPlace( pos, m_pHeap[parent] );
		pos = parent;
	}

	HeapPlace( pos, entry );
}

void CNEOSmokeFill::SiftDown( int pos )
{
	const HeapEntry_t entry = m_pHeap[pos];
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

		if ( !HeapLess( m_pHeap[child], entry ) )
		{
			break;
		}

		HeapPlace( pos, m_pHeap[child] );
		pos = child;
	}

	HeapPlace( pos, entry );
}
