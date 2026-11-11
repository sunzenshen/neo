// NEO volumetric smoke - deterministic gas fill on a voxel grid.
//
// A wavefront grows from the grenade through open cells in order of path
// length and stops when the gas budget is spent, so blocked gas reaches
// further down halls and through doors. The arithmetic is integer and the
// world is queried through an interface, so every client grows the same
// field and the core runs without the engine.

#ifndef NEO_SMOKE_FILL_H
#define NEO_SMOKE_FILL_H
#ifdef _WIN32
#pragma once
#endif

// Answers occupancy questions in grid coordinates. Implementations must see
// only data every client shares (the world BSP).
class INeoSmokeFillWorld
{
public:
	virtual bool CellSolid( int x, int y, int z ) = 0;

	// Is the way from cell (x, y, z) to its neighbor one step along +axis blocked?
	virtual bool LinkBlocked( int x, int y, int z, int axis ) = 0;

	// Is the straight way from the grenade to the center of cell (x, y, z)
	// blocked? The grenade sits on the corner shared by the seed candidates.
	virtual bool SeedBlocked( int x, int y, int z ) = 0;

	// Water bounds the gas like a floor. Is the grenade itself under water, and
	// is the center of cell (x, y, z) under water?
	virtual bool SeedInWater() = 0;
	virtual bool CellWater( int x, int y, int z ) = 0;
};

class CNeoSmokeFill
{
public:
	// The box of cells handed to the client, seed-centered inside the larger
	// work lattice. Cells the gas reaches outside it are filled but not
	// listed, until the client draws a box fitted to the gas.
	static constexpr int GRID_XY = 128;
	static constexpr int GRID_Z = 32;
	static constexpr int CELLS = GRID_XY * GRID_XY * GRID_Z;
	static constexpr float CELL_SIZE = 9.0f;

	// Open-floor radius of the cloud: the legacy smoke's drawn dome, measured
	// to the outer edge of its sprites. The gas budget is this dome's volume.
	static constexpr float DOME_RADIUS = 220.0f;

	// The grenade sits on the floor, so most of the grid is above it.
	static constexpr int SEED_CELL_Z = 8;

	// Path lengths are integers in 1/DIST_SCALE of a cell.
	static constexpr int DIST_SCALE = 16;

	CNeoSmokeFill();
	~CNeoSmokeFill();
	CNeoSmokeFill( const CNeoSmokeFill & ) = delete;
	CNeoSmokeFill &operator=( const CNeoSmokeFill & ) = delete;

	void Start( INeoSmokeFillWorld *pWorld );

	// Finalizes up to maxCells cells, stopping early once the slice has made
	// maxQueries world queries. Returns true when the fill is complete. The
	// field does not depend on how the work is sliced.
	bool Step( int maxCells, int maxQueries );

	bool IsDone() const { return m_bDone; }

	// The finalized cells inside the box, in fill order, as box indices.
	int  FinalizedCount() const { return m_nBoxFinalized; }
	int  FinalizedCell( int i ) const { return m_pBoxOrder[i]; }
	bool IsFinalized( int idx ) const;

	// Path length of a finalized cell, in 1/DIST_SCALE cells.
	int  Dist( int idx ) const;

	// Once done: the position of a finalized cell among the finalized cells
	// in grid order, 0 to FinalizedCount() - 1.
	int  FinalRank( int idx ) const;

	// Heap bytes held now, and the most held at once while filling. The work
	// arrays are freed when the fill is done.
	int  MemoryBytes() const;
	int  PeakMemoryBytes() const { return m_nPeakBytes; }

	// Path length of the last finalized cell in the box (the final radius
	// once done).
	int  FrontDist() const { return m_nBoxFront; }

	// The whole field, in fill order, in box coordinates (which may lie
	// outside the box), and the cells the atlas fit dropped.
	int  LatticeCount() const { return m_nFinalized; }
	void LatticeCell( int i, int &x, int &y, int &z ) const;
	int  LatticeDist( int i ) const { return m_pOrderDist[i]; }
	int  LatticeFrontDist() const { return m_nFrontDist; }
	int  DroppedCount() const { return m_nDropped; }
	int  WorldQueries() const { return m_nQueries; }

	// The box the finalized cells are listed in: its size in cells and the
	// cell corner the grenade sits on. World queries use its coordinates.
	void BoxSize( int size[3] ) const
	{
		size[0] = GRID_XY;
		size[1] = GRID_XY;
		size[2] = GRID_Z;
	}
	void BoxSeedCorner( int corner[3] ) const
	{
		corner[0] = GRID_XY / 2;
		corner[1] = GRID_XY / 2;
		corner[2] = SEED_CELL_Z;
	}

	// The cells the fill may query, in box coordinates: lo inclusive, hi
	// exclusive. Both are multiples of BLOCK_ALIGN.
	static constexpr int BLOCK_ALIGN = 8;
	void LatticeBounds( int lo[3], int hi[3] ) const
	{
		lo[0] = -BOX_SHIFT_XY;
		lo[1] = -BOX_SHIFT_XY;
		lo[2] = -BOX_SHIFT_Z;
		hi[0] = LATTICE_XY - BOX_SHIFT_XY;
		hi[1] = LATTICE_XY - BOX_SHIFT_XY;
		hi[2] = LATTICE_Z - BOX_SHIFT_Z;
	}

	static float DistToUnits( int dist ) { return (float)dist * ( CELL_SIZE / (float)DIST_SCALE ); }

	static int Index( int x, int y, int z ) { return ( z * GRID_XY + y ) * GRID_XY + x; }
	static void Coords( int idx, int &x, int &y, int &z )
	{
		x = idx % GRID_XY;
		y = ( idx / GRID_XY ) % GRID_XY;
		z = idx / ( GRID_XY * GRID_XY );
	}
	static bool InGrid( int x, int y, int z )
	{
		return x >= 0 && y >= 0 && z >= 0 && x < GRID_XY && y < GRID_XY && z < GRID_Z;
	}

private:
	enum CellState_t
	{
		STATE_NONE = 0,
		STATE_QUEUED,
		STATE_FINAL,
		STATE_DROPPED, // popped, but the gas box would no longer fit the atlas
	};

	// The work lattice: the grenade on the same corner as in the box, 432 u
	// below it and 720 u above, 2,304 u to each side.
	static constexpr int LATTICE_LOG2_XY = 9;
	static constexpr int LATTICE_XY = 1 << LATTICE_LOG2_XY;
	static constexpr int LATTICE_Z = 128;
	static constexpr int LATTICE_SEED_Z = 48;
	static constexpr int BOX_SHIFT_XY = LATTICE_XY / 2 - GRID_XY / 2;
	static constexpr int BOX_SHIFT_Z = LATTICE_SEED_Z - SEED_CELL_Z;
	static_assert( BOX_SHIFT_XY % BLOCK_ALIGN == 0 && BOX_SHIFT_Z % BLOCK_ALIGN == 0, "box offset must keep block alignment" );
	static_assert( LATTICE_XY % BLOCK_ALIGN == 0 && LATTICE_Z % BLOCK_ALIGN == 0, "lattice must hold whole blocks" );

	static int  LatticeIndex( int x, int y, int z ) { return ( z * LATTICE_XY + y ) * LATTICE_XY + x; }
	static void LatticeCoords( int idx, int &x, int &y, int &z )
	{
		x = idx & ( LATTICE_XY - 1 );
		y = ( idx >> LATTICE_LOG2_XY ) & ( LATTICE_XY - 1 );
		z = idx >> ( 2 * LATTICE_LOG2_XY );
	}
	static bool InLattice( int x, int y, int z )
	{
		return x >= 0 && y >= 0 && z >= 0 && x < LATTICE_XY && y < LATTICE_XY && z < LATTICE_Z;
	}
	static int  BoxToLattice( int idx );
	static int  LatticeToBox( int cell );

	// The work arrays live in cubes of BRICK_SIZE cells, allocated when a cell
	// in them is first written, so a fill holds memory for the cells it
	// touches, not for the whole grid.
	static constexpr int BRICK_SHIFT = 3;
	static constexpr int BRICK_SIZE = 1 << BRICK_SHIFT;
	static constexpr int BRICK_CELLS = BRICK_SIZE * BRICK_SIZE * BRICK_SIZE;
	static constexpr int BRICKS_XY = LATTICE_XY / BRICK_SIZE;
	static constexpr int BRICKS_Z = LATTICE_Z / BRICK_SIZE;
	static constexpr int BRICKS = BRICKS_XY * BRICKS_XY * BRICKS_Z;

	struct Brick_t
	{
		int dist[BRICK_CELLS];
		int anchor[BRICK_CELLS];    // a cell with an open straight line to this one
		int heapPos[BRICK_CELLS];
		unsigned int moveMemo[BRICK_CELLS]; // cached BlockOpen answers, per cell and move
		unsigned char occ[BRICK_CELLS];     // cached world answers: cell and +x/+y/+z links
		unsigned char state[BRICK_CELLS];   // CellState_t
	};

	// The heap keeps each cell's distance beside it, so ordering reads no brick.
	struct HeapEntry_t
	{
		int dist;
		int cell;
	};

	static int BrickOf( int cell );
	static int BrickSlot( int cell );
	const Brick_t *FindBrick( int cell ) const;
	Brick_t &TouchBrick( int cell );
	int  CellState( int cell ) const;
	int  CellDist( int cell ) const;
	void NoteMemory();

	bool FitsAtlas( int cell );
	bool CellOpen( int x, int y, int z );
	bool LinkOpen( int x, int y, int z, int axis );
	bool BlockOpen( const int p[3], const int q[3] );
	bool BlockOpenUncached( const int p[3], const int q[3] );
	bool LineOpen( int from, int to );
	bool SeedCellOk( int x, int y, int z );
	int  FindSeed();
	int  FindSeedAboveWater();

	void Begin();
	void Relax( int cell );
	void AllocWork();
	void FreeWork();
	void Finish();

	static bool HeapLess( const HeapEntry_t &a, const HeapEntry_t &b );
	void HeapPush( int cell, int dist );
	void HeapUpdate( int cell, int dist );
	void HeapPop();
	void HeapPlace( int pos, const HeapEntry_t &entry );
	void SiftUp( int pos );
	void SiftDown( int pos );

	INeoSmokeFillWorld *m_pWorld;

	int *m_pBrickDir;          // brick number + 1 per brick of the grid, 0 = none
	Brick_t **m_ppBricks;
	int m_nBricks;
	int m_nBrickCap;

	HeapEntry_t *m_pHeap;
	int m_nHeapCap;
	int *m_pOrder;             // finalized lattice cells, in order
	int *m_pOrderDist;
	int *m_pBoxOrder;          // the same cells inside the box, as box indices
	int  m_nLo[3];             // box of the finalized cells, in lattice cells
	int  m_nHi[3];

	// The finished field: a bit per cell, the finalized cells before each
	// 64-cell word, and the path lengths in grid order.
	unsigned long long *m_pFinalBits;
	int *m_pWordRank;
	unsigned short *m_pFinalDist;

	int  m_nHeapCount;
	int  m_nFinalized;
	int  m_nFrontDist;
	int  m_nBoxFinalized;
	int  m_nBoxFront;
	int  m_nDropped;
	int  m_nQueries;
	int  m_nPeakBytes;
	bool m_bDone;
	bool m_bBegun;     // work arrays allocated and the seed placed
};

#endif // NEO_SMOKE_FILL_H
