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

	void Start( INeoSmokeFillWorld *pWorld );

	// Finalizes up to maxCells cells, stopping early once the slice has made
	// maxQueries world queries. Returns true when the fill is complete. The
	// field does not depend on how the work is sliced.
	bool Step( int maxCells, int maxQueries );

	bool IsDone() const { return m_bDone; }
	int  FinalizedCount() const { return m_nFinalized; }
	int  FinalizedCell( int i ) const { return m_pOrder[i]; }
	bool IsFinalized( int idx ) const { return m_pState[idx] == STATE_FINAL; }

	// Path length of a finalized cell, in 1/DIST_SCALE cells.
	int  Dist( int idx ) const { return m_pDist[idx]; }

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

	bool CellOpen( int x, int y, int z );
	bool LinkOpen( int x, int y, int z, int axis );
	bool BlockOpen( const int p[3], const int q[3] );
	bool BlockOpenUncached( const int p[3], const int q[3] );
	bool LineOpen( int from, int to );

	void Relax( int cell );

	bool HeapLess( int a, int b ) const;
	void HeapPush( int cell );
	void HeapUpdate( int cell );
	int  HeapPop();
	void SiftUp( int pos );
	void SiftDown( int pos );

	INeoSmokeFillWorld *m_pWorld;

	unsigned char *m_pOcc;     // cached world answers: cell and +x/+y/+z links
	unsigned int *m_pMoveMemo; // cached BlockOpen answers, per cell and move
	unsigned char *m_pState;   // CellState_t
	int *m_pDist;
	int *m_pAnchor;            // a cell with an open straight line to this one
	int *m_pHeap;
	int *m_pHeapPos;
	int *m_pOrder;             // finalized cells, in order

	int  m_nHeapCount;
	int  m_nFinalized;
	int  m_nFrontDist;
	int  m_nQueries;
	bool m_bDone;
};

#endif // NEO_SMOKE_FILL_H
