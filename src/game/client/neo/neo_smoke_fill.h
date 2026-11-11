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
	int  FinalizedCount() const { return m_nFinalized; }
	int  FinalizedCell( int i ) const { return m_pOrder[i]; }
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

	// Path length of the last finalized cell (the final radius once done).
	int  FrontDist() const { return m_nFrontDist; }
	int  WorldQueries() const { return m_nQueries; }

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
	};

	// The work arrays live in cubes of BRICK_SIZE cells, allocated when a cell
	// in them is first written, so a fill holds memory for the cells it
	// touches, not for the whole grid.
	static constexpr int BRICK_SHIFT = 3;
	static constexpr int BRICK_SIZE = 1 << BRICK_SHIFT;
	static constexpr int BRICK_CELLS = BRICK_SIZE * BRICK_SIZE * BRICK_SIZE;
	static constexpr int BRICKS_XY = GRID_XY / BRICK_SIZE;
	static constexpr int BRICKS_Z = GRID_Z / BRICK_SIZE;
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
	int *m_pOrder;             // finalized cells, in order

	// The finished field: a bit per cell, the finalized cells before each
	// 64-cell word, and the path lengths in grid order.
	unsigned long long *m_pFinalBits;
	int *m_pWordRank;
	unsigned short *m_pFinalDist;

	int  m_nHeapCount;
	int  m_nFinalized;
	int  m_nFrontDist;
	int  m_nQueries;
	int  m_nPeakBytes;
	bool m_bDone;
	bool m_bBegun;     // work arrays allocated and the seed placed
};

#endif // NEO_SMOKE_FILL_H
