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

class CNeoVolSmokeRegenerator;
class IMatRenderContext;
class IMaterial;
class CNeoVolSmokeWorld;
class CJob;
struct VolSmokeCullGrid_t;
struct VolSmokeSheetVolume_t;

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
	void FitToField();
	void CreateAtlas();
	Vector CellCenter( const int c[3] ) const;

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
	bool ShaderDraws();
	void DrawCoverBox( IMatRenderContext *pRenderContext );

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
};

#endif // C_NEO_VOLSMOKE_H
