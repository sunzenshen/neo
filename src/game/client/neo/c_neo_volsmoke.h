// NEO volumetric smoke - client-side voxel smoke volume (demo).
//
// A CPU-simulated density grid that flood-fills through world collision from
// a spawn point, is lit by a sweep toward a light direction, can be carved by
// bullets/explosions (with regrow), and is uploaded as a 2D slice atlas to a
// procedural texture. Rendering is a single raymarched box per volume via the
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

class CNeoVolSmokeRegenerator;

class C_NeoVolSmoke : public C_BaseEntity
{
	DECLARE_CLASS( C_NeoVolSmoke, C_BaseEntity );

public:
	C_NeoVolSmoke();
	virtual ~C_NeoVolSmoke();

	// Spawns a client-only volume centred on `origin`. `extent` is the half
	// size of the AABB in world units; `lifetime` seconds until dissipation.
	static C_NeoVolSmoke *Create( const Vector &origin, float extent, float lifetime );

	// Carves a capsule out of every live volume (bullet trace / explosion).
	static void CarveAll( const Vector &start, const Vector &end, float radius );

	static void RemoveAll();
	static int  LiveCount() { return s_Volumes.Count(); }

	// C_BaseEntity
	virtual void ClientThink() override;
	virtual bool ShouldDraw() override { return true; }
	virtual bool IsTransparent() override { return true; }
	virtual RenderGroup_t GetRenderGroup() override { return RENDER_GROUP_TRANSLUCENT_ENTITY; }
	virtual void GetRenderBounds( Vector &mins, Vector &maxs ) override;
	virtual int  DrawModel( int flags ) override;

	// Fills the slice atlas; called by the texture regenerator.
	void FillAtlas( unsigned char *pDst, int pitch, int width, int height ) const;

	int  GridN() const { return m_nN; }
	int  AtlasCols() const { return m_nAtlasCols; }
	int  AtlasRows() const { return m_nAtlasRows; }

private:
	void Init( const Vector &origin, float extent, float lifetime );
	void Shutdown();

	// Simulation
	void StepFloodFill( int maxCells );
	void UpdateDensities();
	void SweepLight();
	void Carve( const Vector &start, const Vector &end, float radius );

	// Grid helpers
	inline int    Idx( int x, int y, int z ) const { return ( z * m_nN + y ) * m_nN + x; }
	inline bool   InGrid( int x, int y, int z ) const { return x >= 0 && y >= 0 && z >= 0 && x < m_nN && y < m_nN && z < m_nN; }
	inline Vector CellCenter( int x, int y, int z ) const
	{
		return m_vecGridMin + Vector( ( x + 0.5f ) * m_flCellSize, ( y + 0.5f ) * m_flCellSize, ( z + 0.5f ) * m_flCellSize );
	}
	bool CellOpen( const Vector &from, const Vector &to ) const;

	float SampleLight( float fx, float fy, int zSlab, int axis ) const;

private:
	static CUtlVector< C_NeoVolSmoke * > s_Volumes;
	static int s_nNextId;

	int    m_nId;
	int    m_nN;            // cells per axis
	int    m_nAtlasCols;
	int    m_nAtlasRows;
	float  m_flCellSize;
	Vector m_vecGridMin;
	Vector m_vecGridMax;
	Vector m_vecSpawn;

	float  m_flSpawnTime;
	float  m_flLifetime;

	// Per-cell state
	CUtlVector< unsigned char > m_Reach;      // BFS depth from spawn, 255 = unreached
	CUtlVector< float >         m_FillTime;   // curtime when the cell was reached
	CUtlVector< float >         m_CarveTime;  // curtime of last carve, <0 = never
	CUtlVector< unsigned char > m_Noise;      // static per-cell variation
	CUtlVector< float >         m_Density;    // 0..1 current density
	CUtlVector< float >         m_Light;      // 0..1 transmittance toward light

	// Flood fill frontier (ring buffer of cell indices)
	CUtlVector< int > m_Frontier;
	int   m_nFrontierHead;
	int   m_nFilledCells;
	int   m_nMaxCells;
	int   m_nMaxDepth;
	bool  m_bFillDone;

	// GPU side
	CTextureReference        m_Atlas;
	CNeoVolSmokeRegenerator *m_pRegen;
	CMaterialReference       m_Material;
	bool  m_bAtlasDirty;
	float m_flNextLightSweep;
};

#endif // C_NEO_VOLSMOKE_H
