// NEO volumetric smoke - the client's cloud of a smoke grenade.
//
// The server networks each grenade's smoke field (C_NEOSmokeVolumeEnt). The
// client fills the same field from the same origin with the shared fill
// (CNEOSmokeFill), so the cloud has the shape that the server's sight test
// reads. A top-down sweep shades the field, and its cells go to a 2D slice
// atlas in a procedural texture. The atlas holds path lengths, not densities:
// the shader applies the bloom front and the fade, so a grown cloud uploads
// nothing.
//
// The Neo_VolSmoke shader raymarches one box per volume in two parts: far
// slabs that use the hardware depth test, and a near pass on the box's far
// faces that clips in the shader. One box per volume needs no sorting inside it.

#ifndef C_NEO_VOLSMOKE_H
#define C_NEO_VOLSMOKE_H
#ifdef _WIN32
#pragma once
#endif

#include "c_baseentity.h"
#include "materialsystem/itexture.h"
#include "materialsystem/MaterialSystemUtil.h"
#include "utlvector.h"
#include "tier0/threadtools.h"
#include "neo_smoke_fill.h"
#include "neo_volsmoke_cover.h"

class CNeoVolSmokeRegenerator;
class IMatRenderContext;
class IMaterial;
class IWorldRenderList;
class CNeoVolSmokeWorld;
class CJob;
struct VolSmokeCullGrid_t;
struct VolSmokeSheetVolume_t;

// A glass pane that crosses a volume's box: a rectangle in the pane's middle
// plane, where |dot( p, u ) + u0| <= 1 and |dot( p, v ) + v0| <= 1, and the
// leaves beside each face that hold the pane's translucent surfaces.
static constexpr int NEO_VOLSMOKE_MAX_PANES = 4;
static constexpr int NEO_VOLSMOKE_PANE_LEAVES = 8;
struct VolSmokePane_t
{
	Vector normal;
	float  dist;
	Vector u;
	float  u0;
	Vector v;
	float  v0;
	int    leaves[2][NEO_VOLSMOKE_PANE_LEAVES]; // [0] on the side the normal points to
	int    nLeaves[2];
};

// A glass face that a ray of the pane sweep met.
struct VolSmokePaneHit_t
{
	Vector normal;
	float  dist;
	Vector pos;
};

class C_NeoVolSmoke : public C_BaseEntity
{
	DECLARE_CLASS( C_NeoVolSmoke, C_BaseEntity );

public:
	C_NeoVolSmoke();
	virtual ~C_NeoVolSmoke();

	// Spawns a client-only volume for a grenade resting at `origin` that
	// detonated at `spawnTime` (server time), or none once its time is over.
	// Bloom and fade follow that time. The volume dies with `pGrenade`,
	// as the legacy particles do, so a round restart that removes the grenade
	// also removes its smoke.
	static C_NeoVolSmoke *CreateForGrenade( const Vector &origin, float spawnTime, C_BaseEntity *pGrenade );

	// Cancels and joins every running fill job.
	static void StopAllFills();

	// Releases the volumes whose time is over or whose grenade is gone. Called
	// once a frame, outside the client think, which cannot release the entity
	// it runs.
	static void RemoveExpired();

	// The translucent sort key of a volume: the view depth of the nearest
	// corner of its smoke box. False when the renderable is not a volume.
	static bool SortDepth( IClientRenderable *pRenderable, const Vector &viewOrigin, const Vector &viewForward, float &depth );

	// The share of a point's light that reaches the eye through every
	// volume. A translucent effect drawn after the smoke, which writes no
	// depth, can fade itself by this to stay hidden behind it.
	static float Transmittance( const Vector &eye, const Vector &point );

	// Glass inside a volume's box. The translucent world walk calls these:
	// a leaf that holds a pane's camera-side faces is drawn after the volume
	// rather than in its own place, so the pane shows over the smoke.
	static void BeginGlassWalk( bool bMainView );
	static bool HoldsGlass( int leaf, IWorldRenderList *pList, int sortIndex, unsigned long flags );
	static void DrawHeldGlass();
	static void EndGlassWalk();

	// C_BaseEntity
	virtual void ClientThink() override;
	virtual bool ShouldDraw() override { return true; }
	virtual bool IsTransparent() override { return true; }
	virtual RenderGroup_t GetRenderGroup() override { return RENDER_GROUP_TRANSLUCENT_ENTITY; }
	virtual void GetRenderBounds( Vector &mins, Vector &maxs ) override;
	virtual int  DrawModel( int flags ) override;

private:
	static C_NeoVolSmoke *Create( const Vector &origin, float spawnTime );
	void Init( const Vector &origin, float spawnTime );
	void Shutdown();

	// Simulation
	static void StepFills();
	static C_NeoVolSmoke *UploadTurn();
	void StepFill( double budget );
	void RunFill();
	void BuildCellBytes();
	float DistRange() const;
	void StopFill();
	bool FillDone() const { return m_bFillDone; }
	bool WantsFill() const { return !m_bFillDone && !m_pFillJob && m_nFillCancel == 0; }
	void FitBoxToField();
	void FitToField();
	void CreateAtlas();
	Vector CellCenter( const int c[3] ) const;

	// Glass panes inside the box
	void FindPanes();
	bool PaneRay( int ray, Vector &start, Vector &end ) const;
	void TracePaneRay( const Vector &start, const Vector &end );
	void BuildPanes();
	bool DrawsShaderCloud();
	void ReleaseHeldGlass();
	void SetPaneLayers();
	void DrawNextLayer();

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
	float CellWeight( int idx ) const;
	float DensityAt( const Vector &pos, float front ) const;
	void AddInsideOverlay( float front, float now );
	Vector DisplayColor() const;
	void UpdateBounds( float front );

	// Atlas
	int  AtlasOffset( int x, int y, int z ) const;
	int  AtlasOffset( int idx ) const;
	void ClearAtlas();
	bool ShapeChanged() const;
	bool PackNewCells();
	void SweepLight();
	Vector SampleColor() const;
	bool GasReached( const Vector &pos ) const;

	// Rendering
	float DrawExtinction() const;
	float SmokeSphere( Vector &center ) const;
	void SetDrawParams( IMaterial *pMat ) const;
	void SampleAtlas( const Vector &pos, float &dist, float &weight, float &wallCut ) const;
	float DensityLowerBound( const Vector &pos, float front, float bite ) const;
	bool BuildCullGrid( VolSmokeCullGrid_t &grid ) const;
	bool StartsRender();
	static float SheetOpaqueDepth( const VolSmokeSheetVolume_t *vols, int nVols, const Vector &eye, const Vector &dir, float *own );
	static void BuildOpaqueSheet( IMatRenderContext *pRenderContext );
	void DrawFarSlabs( IMatRenderContext *pRenderContext );
	void DrawNearPass( IMatRenderContext *pRenderContext );
	static bool UsesCoverBoxes();
	void BuildCoverReach();
	void UpdateCoverBoxes( float now );
	void UpdateInterimCover( float now );
	void AddCoverOverlay();
	bool InCoverBox( const Vector &point, float margin ) const;
	void DrawCoverBoxes( IMatRenderContext *pRenderContext );

	// What shows through the smoke after the volume draws
	static float OpticalDepth( const Vector &eye, const Vector &point );
	float PathOpticalDepth( const Vector &eye, const Vector &point ) const;
	static C_NeoVolSmoke *Owner( const Vector &point );
	void DrawRevealed( IMatRenderContext *pRenderContext );

private:
	static CUtlVector< C_NeoVolSmoke * > s_Volumes;
	static int s_nNextId;

	int    m_nId;
	EHANDLE m_hGrenade;        // the grenade entity the volume belongs to
	int    m_nBox[3];          // the volume's box of cells, from the fill
	int    m_nSeedCorner[3];   // the cell corner the grenade sits on, in the box
	int    m_nBoxOffset[3];    // the box's low corner in the fill's query coordinates
	bool   m_bBoxFitted;       // the fill is done and the box fits its field
	bool   m_bFitted;          // the box has its atlas and materials
	Vector m_vecQueryMin;      // corner of the box the fill's world queries are in
	Vector m_vecGridMin;
	Vector m_vecGridMax;
	Vector m_vecSpawn;
	Vector m_vecSmokeMin;      // bounds of the cells that hold smoke, padded; drawn instead of the grid
	Vector m_vecSmokeMax;

	float  m_flSpawnTime;
	Vector m_vecColor;         // linear smoke color, sampled once

	// Gas fill and the world it queries
	CNEOSmokeFill      m_Fill;
	CNeoVolSmokeWorld *m_pWorld;
	// While a job runs, the worker owns m_Fill and m_pWorld; the main thread
	// reads them only after the job is finished.
	CJob  *m_pFillJob;
	CInterlockedInt m_nFillCancel;
	bool   m_bFillDone;        // the fill is done and the main thread has seen it

	// The atlas's path length and wall distance per box cell, built by the
	// fill job; kept until the first pack copies them.
	CUtlVector< unsigned char > m_CellBytes;
	bool   m_bCellBytes;

	// Per-cell state
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
	float m_flUploadedFront;   // path length of the last cell the uploaded atlas holds
	bool  m_bUploadPending;    // atlas changed, upload waits for a free frame
	int   m_nRenderSerial;     // the render this volume last drew in

	// Without the shader: opaque boxes over the cells where the server's
	// smoke density can be above zero, rebuilt while the cloud blooms.
	CUtlVector< float > m_CoverReach; // per finalized cell, from VolSmokeCoverReach
	Vector m_vecCoverMin[NEO_VOLSMOKE_COVER_BOXES];
	Vector m_vecCoverMax[NEO_VOLSMOKE_COVER_BOXES];
	int    m_nCoverBoxes;
	float  m_flNextCover;
	bool   m_bCoverFinal;      // the cloud is full grown and the boxes hold all its cells
	bool   m_bCoverBoxes;      // draws the cover boxes, not the shader cloud

	// Glass panes that cross the fitted box, found once a few rays a frame
	CUtlVector< VolSmokePaneHit_t > m_PaneHits;
	VolSmokePane_t m_Panes[NEO_VOLSMOKE_MAX_PANES];
	int    m_nPanes;
	int    m_nPaneRay;         // the next ray of the sweep
	bool   m_bPanesFound;

	// This volume's part in the current translucent walk
	int    m_nGlassWalk;       // the walk the fields below belong to
	int    m_nPaneHeld[NEO_VOLSMOKE_MAX_PANES]; // the pane's last held leaf in the walk's queue, or -1
	bool   m_bGlassDrawn;      // the volume has drawn in this walk
	CUtlVector< int > m_GlassWaits; // the held leaves that wait on this volume
	int    m_nLayerAfter[NEO_VOLSMOKE_MAX_PANES]; // per layer from the second, the held leaf it draws after
	int    m_nGlassLayers;     // layers after the first in this walk
	int    m_nLayersDrawn;     // of those
};

#endif // C_NEO_VOLSMOKE_H
