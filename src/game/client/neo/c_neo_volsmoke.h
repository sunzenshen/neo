// NEO volumetric smoke - client-side voxel smoke volume (demo).
//
// A CPU-simulated density grid that fills through world collision from a
// spawn point (CNeoSmokeFill), is shaded by a top-down sweep, can be carved by
// bullets/explosions (with regrow), and is uploaded as a 2D slice atlas to a
// procedural texture. The atlas holds path lengths, not densities: the shader
// applies the bloom front and the fade, so a grown volume uploads nothing. Rendering is a single raymarched box per volume via the
// Neo_VolSmoke shader, so there is no intra-volume sorting and no OIT issue.
//
// This is a client-only prototype: spawn with `neo_volsmoke_spawn`, carve with
// `neo_volsmoke_carve`, inspect with `neo_volsmoke_debug`.

#ifndef C_NEO_VOLSMOKE_H
#define C_NEO_VOLSMOKE_H
#ifdef _WIN32
#pragma once
#endif

#include "c_baseentity.h"
#include "materialsystem/itexture.h"
#include "materialsystem/MaterialSystemUtil.h"
#include "utlvector.h"
#include "neo_smoke_fill.h"

class CNeoVolSmokeRegenerator;
class IMatRenderContext;
class IMaterial;
class CNeoVolSmokeWorld;
struct VolSmokeCullGrid_t;

class C_NeoVolSmoke : public C_BaseEntity
{
	DECLARE_CLASS( C_NeoVolSmoke, C_BaseEntity );

public:
	C_NeoVolSmoke();
	virtual ~C_NeoVolSmoke();

	// Spawns a client-only volume for a grenade resting at `origin` that
	// detonated at `spawnTime` (server time). Bloom and fade follow that time.
	static C_NeoVolSmoke *Create( const Vector &origin, float spawnTime );

	// As Create, but at most one volume per grenade, and none once its time is over.
	static C_NeoVolSmoke *CreateForGrenade( const Vector &origin, float spawnTime );

	// Carves a capsule out of every live volume (bullet trace / explosion).
	static void CarveAll( const Vector &start, const Vector &end, float radius );

	static void RemoveAll();

	// Releases the volumes whose time is over. Called once a frame, outside
	// the client think, which cannot release the entity it runs.
	static void RemoveExpired();

	static void PrintAllStats();

	// The translucent sort key of a volume: the view depth of the nearest
	// corner of its smoke box. False when the renderable is not a volume.
	static bool SortDepth( IClientRenderable *pRenderable, const Vector &viewOrigin, const Vector &viewForward, float &depth );

	// The share of a bright flash at a point that the smoke of every volume
	// between the eye and the point lets through: 1 in clear air, toward 0
	// deep behind smoke.
	static float FlashFactor( const Vector &eye, const Vector &point );

	static int  LiveCount() { return s_Volumes.Count(); }
	static int  NextId() { return s_nNextId; }

	// C_BaseEntity
	virtual void ClientThink() override;
	virtual bool ShouldDraw() override { return true; }
	virtual bool IsTransparent() override { return true; }
	virtual RenderGroup_t GetRenderGroup() override { return RENDER_GROUP_TRANSLUCENT_ENTITY; }
	virtual void GetRenderBounds( Vector &mins, Vector &maxs ) override;
	virtual int  DrawModel( int flags ) override;

	void PrintStats() const;
	unsigned long long ClientFieldHash() const;
	unsigned long long LatticeFieldHash() const;
	unsigned long long AtlasHash() const;

private:
	void Init( const Vector &origin, float spawnTime );
	void Shutdown();

	// Simulation
	static void StepFills();
	static C_NeoVolSmoke *UploadTurn();
	double StepFill( double budget );
	void FitToField();
	void CreateAtlas();
	Vector CellCenter( const int c[3] ) const;
	float CarveTime( int idx ) const { return m_CarveTime[m_Fill.FinalRank( idx )]; }

	// Cells of the volume's box
	int  CellIndex( int x, int y, int z ) const { return ( z * m_nBox[1] + y ) * m_nBox[0] + x; }
	void CellCoords( int idx, int &x, int &y, int &z ) const
	{
		x = idx % m_nBox[0];
		y = ( idx / m_nBox[0] ) % m_nBox[1];
		z = idx / ( m_nBox[0] * m_nBox[1] );
	}
	bool InBox( int x, int y, int z ) const
	{
		return x >= 0 && y >= 0 && z >= 0 && x < m_nBox[0] && y < m_nBox[1] && z < m_nBox[2];
	}
	float FrontRadius( float age ) const;
	float BloomRadius( float age ) const;
	float CellWeight( int idx, float now ) const;
	float DensityAt( const Vector &pos, float front, float now ) const;
	void AddInsideOverlay( float front, float now );
	void UpdateBounds( float front );

	// Atlas
	int  AtlasOffset( int x, int y, int z ) const;
	int  AtlasOffset( int idx ) const;
	void ClearAtlas();
	bool IsRegrowing( float now ) const;
	bool ShapeChanged( float now ) const;
	bool PackNewCells( float now );
	void PackWeights( float now );
	void SweepLight( float now );
	static Vector SampleColor( const Vector &origin );
	void Carve( const Vector &start, const Vector &end, float radius );

	// Rendering
	float DrawExtinction() const;
	float SmokeSphere( Vector &center ) const;
	void SetDrawParams( IMaterial *pMat ) const;
	void SampleAtlas( const Vector &pos, float &dist, float &weight, float &wallCut ) const;
	float DensityLowerBound( const Vector &pos, float front, float bite ) const;
	float OpaqueDepth( const Vector &eye, const Vector &dir, float front, float bite, float extinction ) const;
	bool BuildCullGrid( IMatRenderContext *pRenderContext, const Vector view[8], float zMin, float zMax, VolSmokeCullGrid_t &grid ) const;
	void DrawFarSlabs( IMatRenderContext *pRenderContext );
	void DrawNearPass( IMatRenderContext *pRenderContext );

	// What shows through the smoke after the volume draws
	static float OpticalDepth( const Vector &eye, const Vector &point );
	float PathOpticalDepth( const Vector &eye, const Vector &point ) const;

private:
	static CUtlVector< C_NeoVolSmoke * > s_Volumes;
	static int s_nNextId;

	int    m_nId;
	int    m_nBox[3];          // the volume's box of cells, from the fill
	int    m_nSeedCorner[3];   // the cell corner the grenade sits on, in the box
	int    m_nBoxOffset[3];    // the box's low corner in the fill's query coordinates
	bool   m_bFitted;          // the fill is done and the box fits its field
	Vector m_vecQueryMin;      // corner of the box the fill's world queries are in
	Vector m_vecGridMin;
	Vector m_vecGridMax;
	Vector m_vecSpawn;
	Vector m_vecSmokeMin;      // bounds of the cells that hold smoke, padded; drawn instead of the grid
	Vector m_vecSmokeMax;

	float  m_flSpawnTime;
	Vector m_vecColor;         // linear smoke color, sampled once

	// Gas fill and the world it queries
	CNeoSmokeFill      m_Fill;
	CNeoVolSmokeWorld *m_pWorld;
	double m_flFillSeconds;    // CPU time spent in the fill
	double m_flFillPeakFrame;  // longest single fill slice
	float  m_flFillDoneAge;    // age when the fill finished, <0 = not yet
	int    m_nLagFrames;       // frames the fill or the upload held the bloom front back

	// Per-cell state
	CUtlVector< float >         m_CarveTime;  // curtime of the last carve by FinalRank, <0 = never
	int    m_SmokeLo[3];       // cell bounds of m_vecSmokeMin / Max
	int    m_SmokeHi[3];
	int    m_nBounded;         // finalized cells inside the drawn box

	// GPU side
	CTextureReference        m_Atlas;
	CNeoVolSmokeRegenerator *m_pRegen;
	CMaterialReference       m_Material;     // near pass: box far faces, clipped in the shader
	CMaterialReference       m_SlabMaterial; // far slabs: hardware depth test
	float m_flDistRange;       // path length at atlas value 1
	int   m_nAtlasCols;        // tiles per atlas row, one tile per z slice
	int   m_nAtlasW;
	int   m_nAtlasH;
	int   m_nPacked;           // finalized cells written to the atlas
	float m_flNextUpload;
	float m_flRegrowEnd;       // curtime when the last carved cell has refilled
	float m_flUploadedFront;   // path length of the last cell the uploaded atlas holds
	bool  m_bUploadPending;    // atlas changed, upload waits for a free frame
	int   m_nSlabTiles;        // far slab tiles last frame, and those behind opaque smoke
	int   m_nSlabTilesCulled;
};

#endif // C_NEO_VOLSMOKE_H
