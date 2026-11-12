// NEO volumetric smoke - the fill's world queries, on the engine's world
// trace. The client and the server fill the same field from them.
//
// One box query answers a whole block of cells: when the block is empty,
// every cell center in it is open, and so is every link inside it or into
// another empty block, since the segment stays inside the two boxes. A block
// that touches the world falls to the fine blocks inside it, then to the point
// and segment queries. The coarse blocks cover the fill's lattice; the fine
// ones are kept only for coarse blocks that touch the world.

#ifndef NEO_SMOKE_WORLD_H
#define NEO_SMOKE_WORLD_H
#ifdef _WIN32
#pragma once
#endif

#include "neo_smoke_fill.h"
#include "utlvector.h"

class CNEOSmokeWorld : public INEOSmokeFillWorld
{
public:
	CNEOSmokeWorld( const Vector &gridMin, const int boxSize[3], const int seedCorner[3], const int latticeLo[3], const int latticeHi[3] );
	virtual ~CNEOSmokeWorld() {}

	static bool PointIn( const Vector &pos, int mask );
	static bool PointSolid( const Vector &pos );

	// INEOSmokeFillWorld
	virtual bool CellSolid( int x, int y, int z ) override;
	virtual bool LinkBlocked( int x, int y, int z, int axis ) override;
	virtual bool SeedBlocked( int x, int y, int z ) override;
	virtual bool SeedInWater() override;
	virtual bool CellWater( int x, int y, int z ) override;

protected:
	bool InBox( int x, int y, int z ) const
	{
		return x >= 0 && y >= 0 && z >= 0 && x < m_nBoxSize[0] && y < m_nBoxSize[1] && z < m_nBoxSize[2];
	}

	bool InEmptyBlock( int x, int y, int z );

	Vector Center( int x, int y, int z ) const
	{
		const float c = CNEOSmokeFill::CELL_SIZE;
		return m_vecGridMin + Vector( ( x + 0.5f ) * c, ( y + 0.5f ) * c, ( z + 0.5f ) * c );
	}

	Vector m_vecGridMin;
	int m_nBoxSize[3];      // the drawn box, from m_nDrawOffset
	int m_nDrawOffset[3];

private:
	bool InLattice( int x, int y, int z ) const
	{
		return x >= m_nLatticeLo[0] && y >= m_nLatticeLo[1] && z >= m_nLatticeLo[2]
			&& x < m_nLatticeHi[0] && y < m_nLatticeHi[1] && z < m_nLatticeHi[2];
	}

	bool BoxTouchesWorld( int x, int y, int z, int b );

	Vector Grenade() const
	{
		const float c = CNEOSmokeFill::CELL_SIZE;
		return m_vecGridMin + Vector( m_nSeedCorner[0] * c, m_nSeedCorner[1] * c, m_nSeedCorner[2] * c );
	}

	int m_nSeedCorner[3];
	int m_nLatticeLo[3];
	int m_nLatticeHi[3];
	int m_nCoarse[3];
	CUtlVector<int> m_Coarse;          // NEO_SMOKE_COARSE_* or fine offset + 1
	CUtlVector<unsigned char> m_Fine;  // NEOSmokeBlockState_t
};

#endif // NEO_SMOKE_WORLD_H
