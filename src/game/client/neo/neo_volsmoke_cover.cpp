// NEO volumetric smoke - the opaque boxes a client without the smoke shader
// draws. See neo_volsmoke_cover.h.

#include "cbase.h"
#include "neo_volsmoke_cover.h"
#include "neo_smoke_fill.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

static int CoverIndex( const int size[3], int x, int y, int z )
{
	return ( z * size[1] + y ) * size[0] + x;
}

static bool CoverBoxEmpty( const VolSmokeCoverBox_t &box )
{
	return box.hi[0] < box.lo[0];
}

static int CoverBoxVolume( const VolSmokeCoverBox_t &box )
{
	if ( CoverBoxEmpty( box ) )
	{
		return 0;
	}

	return ( box.hi[0] - box.lo[0] + 1 ) * ( box.hi[1] - box.lo[1] + 1 ) * ( box.hi[2] - box.lo[2] + 1 );
}

static VolSmokeCoverBox_t CoverBoxUnion( const VolSmokeCoverBox_t &a, const VolSmokeCoverBox_t &b )
{
	if ( CoverBoxEmpty( a ) )
	{
		return b;
	}

	if ( CoverBoxEmpty( b ) )
	{
		return a;
	}

	VolSmokeCoverBox_t box;
	for ( int k = 0; k < 3; ++k )
	{
		box.lo[k] = Min( a.lo[k], b.lo[k] );
		box.hi[k] = Max( a.hi[k], b.hi[k] );
	}

	return box;
}

// The smallest box that holds the marked cells inside `in`; empty without one.
static VolSmokeCoverBox_t CoverTightBox( const unsigned char *pMask, const int size[3], const VolSmokeCoverBox_t &in )
{
	VolSmokeCoverBox_t box;
	for ( int k = 0; k < 3; ++k )
	{
		box.lo[k] = in.hi[k] + 1;
		box.hi[k] = -1;
	}

	for ( int z = in.lo[2]; z <= in.hi[2]; ++z )
	{
		for ( int y = in.lo[1]; y <= in.hi[1]; ++y )
		{
			for ( int x = in.lo[0]; x <= in.hi[0]; ++x )
			{
				if ( !pMask[CoverIndex( size, x, y, z )] )
				{
					continue;
				}

				const int c[3] = { x, y, z };
				for ( int k = 0; k < 3; ++k )
				{
					box.lo[k] = Min( box.lo[k], c[k] );
					box.hi[k] = Max( box.hi[k], c[k] );
				}
			}
		}
	}

	return box;
}

// The single axis cut of a box that saves the most volume once both halves
// shrink to their cells. Returns the volume saved, 0 when no cut saves any.
// pScratch holds three boxes per cell of the box's longest side.
static int CoverBestCut( const unsigned char *pMask, const int size[3], const VolSmokeCoverBox_t &box,
	VolSmokeCoverBox_t *pScratch, int maxSide, VolSmokeCoverBox_t &cutA, VolSmokeCoverBox_t &cutB )
{
	const int volume = CoverBoxVolume( box );
	int best = volume;
	VolSmokeCoverBox_t *pSlices = pScratch;
	VolSmokeCoverBox_t *pBefore = pScratch + maxSide;
	VolSmokeCoverBox_t *pAfter = pScratch + 2 * maxSide;
	for ( int axis = 0; axis < 3; ++axis )
	{
		const int n = box.hi[axis] - box.lo[axis] + 1;
		if ( n < 2 )
		{
			continue;
		}

		for ( int i = 0; i < n; ++i )
		{
			VolSmokeCoverBox_t slice = box;
			slice.lo[axis] = slice.hi[axis] = box.lo[axis] + i;
			pSlices[i] = CoverTightBox( pMask, size, slice );
		}

		pBefore[0] = pSlices[0];
		for ( int i = 1; i < n; ++i )
		{
			pBefore[i] = CoverBoxUnion( pBefore[i - 1], pSlices[i] );
		}

		pAfter[n - 1] = pSlices[n - 1];
		for ( int i = n - 2; i >= 0; --i )
		{
			pAfter[i] = CoverBoxUnion( pAfter[i + 1], pSlices[i] );
		}

		for ( int i = 0; i + 1 < n; ++i )
		{
			const int split = CoverBoxVolume( pBefore[i] ) + CoverBoxVolume( pAfter[i + 1] );
			if ( split < best )
			{
				best = split;
				cutA = pBefore[i];
				cutB = pAfter[i + 1];
			}
		}
	}

	return volume - best;
}

void VolSmokeCoverReach( const CNEOSmokeFill &fill, float distRange, float *pReach, float *pScratch )
{
	int size[3];
	fill.BoxSize( size );
	const int nCells = size[0] * size[1] * size[2];
	const int count = fill.FinalizedCount();
	for ( int i = 0; i < nCells; ++i )
	{
		pScratch[i] = distRange;
	}

	// Corner path lengths: a gas cell's own, else the shortest of its gas
	// neighbors.
	for ( int k = 0; k < count; ++k )
	{
		const int idx = fill.FinalizedCell( k );
		pScratch[idx] = CNEOSmokeFill::DistToUnits( fill.Dist( idx ) );
	}

	for ( int k = 0; k < count; ++k )
	{
		const int idx = fill.FinalizedCell( k );
		const float dist = pScratch[idx];
		const int c[3] = { idx % size[0], ( idx / size[0] ) % size[1], idx / ( size[0] * size[1] ) };
		for ( int dz = -1; dz <= 1; ++dz )
		{
			for ( int dy = -1; dy <= 1; ++dy )
			{
				for ( int dx = -1; dx <= 1; ++dx )
				{
					const int x = c[0] + dx;
					const int y = c[1] + dy;
					const int z = c[2] + dz;
					if ( x < 0 || y < 0 || z < 0 || x >= size[0] || y >= size[1] || z >= size[2] )
					{
						continue;
					}

					const int n = CoverIndex( size, x, y, z );
					if ( !fill.IsFinalized( n ) )
					{
						pScratch[n] = Min( pScratch[n], dist );
					}
				}
			}
		}
	}

	for ( int k = 0; k < count; ++k )
	{
		const int idx = fill.FinalizedCell( k );
		const int c[3] = { idx % size[0], ( idx / size[0] ) % size[1], idx / ( size[0] * size[1] ) };
		float reach = distRange;
		for ( int dz = -1; dz <= 1; ++dz )
		{
			for ( int dy = -1; dy <= 1; ++dy )
			{
				for ( int dx = -1; dx <= 1; ++dx )
				{
					const int x = c[0] + dx;
					const int y = c[1] + dy;
					const int z = c[2] + dz;
					if ( x < 0 || y < 0 || z < 0 || x >= size[0] || y >= size[1] || z >= size[2] )
					{
						continue;
					}

					reach = Min( reach, pScratch[CoverIndex( size, x, y, z )] );
				}
			}
		}

		pReach[k] = reach;
	}
}

void VolSmokeCoverMask( const CNEOSmokeFill &fill, const float *pReach, float front, unsigned char *pMask )
{
	int size[3];
	fill.BoxSize( size );
	memset( pMask, 0, size[0] * size[1] * size[2] );

	const int count = fill.FinalizedCount();
	for ( int k = 0; k < count; ++k )
	{
		if ( pReach[k] < front )
		{
			pMask[fill.FinalizedCell( k )] = 1;
		}
	}
}

int VolSmokeCoverSplit( const unsigned char *pMask, const int size[3], VolSmokeCoverBox_t *pBoxes, int maxBoxes )
{
	Assert( maxBoxes >= 1 );
	const VolSmokeCoverBox_t all = { { 0, 0, 0 }, { size[0] - 1, size[1] - 1, size[2] - 1 } };
	pBoxes[0] = CoverTightBox( pMask, size, all );
	if ( CoverBoxEmpty( pBoxes[0] ) )
	{
		return 0;
	}

	const int maxSide = Max( size[0], Max( size[1], size[2] ) );
	CUtlVectorFixedGrowable< VolSmokeCoverBox_t, 3 * CNEOSmokeFill::GRID_XY > scratch;
	CUtlVectorFixedGrowable< VolSmokeCoverBox_t, 2 * NEO_VOLSMOKE_COVER_BOXES > cuts;
	CUtlVectorFixedGrowable< int, NEO_VOLSMOKE_COVER_BOXES > gain;
	scratch.SetCount( 3 * maxSide );
	cuts.SetCount( 2 * maxBoxes );
	gain.SetCount( maxBoxes );
	VolSmokeCoverBox_t *pScratch = scratch.Base();
	VolSmokeCoverBox_t *pCuts = cuts.Base();
	int *pGain = gain.Base();

	int nBoxes = 1;
	pGain[0] = CoverBestCut( pMask, size, pBoxes[0], pScratch, maxSide, pCuts[0], pCuts[1] );
	while ( nBoxes < maxBoxes )
	{
		int bestBox = -1;
		int bestGain = 0;
		for ( int i = 0; i < nBoxes; ++i )
		{
			if ( pGain[i] > bestGain )
			{
				bestGain = pGain[i];
				bestBox = i;
			}
		}

		if ( bestBox < 0 )
		{
			break;
		}

		const VolSmokeCoverBox_t second = pCuts[2 * bestBox + 1];
		pBoxes[bestBox] = pCuts[2 * bestBox];
		pGain[bestBox] = CoverBestCut( pMask, size, pBoxes[bestBox], pScratch, maxSide, pCuts[2 * bestBox], pCuts[2 * bestBox + 1] );

		// A cut of a tight box leaves gas on both sides.
		Assert( !CoverBoxEmpty( second ) );
		pBoxes[nBoxes] = second;
		pGain[nBoxes] = CoverBestCut( pMask, size, second, pScratch, maxSide, pCuts[2 * nBoxes], pCuts[2 * nBoxes + 1] );
		++nBoxes;
	}

	return nBoxes;
}
