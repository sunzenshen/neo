// NEO volumetric smoke - the opaque boxes a client without the smoke shader
// draws in place of the cloud.
//
// The boxes cover every point where the server's smoke density can be above
// zero: within half a cell of a gas cell whose neighborhood holds a path
// length below the front. So every line of sight the server blocks touches
// a box.

#ifndef NEO_VOLSMOKE_COVER_H
#define NEO_VOLSMOKE_COVER_H
#ifdef _WIN32
#pragma once
#endif

class CNEOSmokeFill;

// The most boxes a cloud is split into.
static constexpr int NEO_VOLSMOKE_COVER_BOXES = 16;

// A box of cells, inclusive bounds.
struct VolSmokeCoverBox_t
{
	int lo[3];
	int hi[3];
};

// Per finalized cell, in fill order: the shortest corner path length in its
// 3x3x3 block. A gas cell's corner path length is its own; a cell without
// gas takes the shortest of its gas neighbors, or distRange without one, as
// the server's sight field does. pReach holds FinalizedCount() values and
// pScratch one value per cell of the fill's box.
void VolSmokeCoverReach( const CNEOSmokeFill &fill, float distRange, float *pReach, float *pScratch );

// Marks the gas cells whose reach is below the front. pMask holds one byte
// per cell of the fill's box.
void VolSmokeCoverMask( const CNEOSmokeFill &fill, const float *pReach, float front, unsigned char *pMask );

// Splits the marked cells into at most maxBoxes disjoint boxes that hold
// them all: from the tight box of the cells, it keeps applying the single
// axis cut that saves the most volume. Returns the number of boxes.
int VolSmokeCoverSplit( const unsigned char *pMask, const int size[3], VolSmokeCoverBox_t *pBoxes, int maxBoxes );

#endif // NEO_VOLSMOKE_COVER_H
