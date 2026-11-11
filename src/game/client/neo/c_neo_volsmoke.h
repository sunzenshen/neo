// NEO volumetric smoke - client-side voxel smoke volume (demo).
//
// A CPU-simulated density grid that fills through world collision from a
// spawn point (CNeoSmokeFill), is shaded by a top-down sweep, can be carved by
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
#include "neo_smoke_fill.h"

class CNeoVolSmokeRegenerator;
class CNeoVolSmokeWorld;

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
	static void PrintAllStats();
	static int  LiveCount() { return s_Volumes.Count(); }
	static int  NextId() { return s_nNextId; }

	// C_BaseEntity
	virtual void ClientThink() override;
	virtual bool ShouldDraw() override { return true; }
	virtual bool IsTransparent() override { return true; }
	virtual RenderGroup_t GetRenderGroup() override { return RENDER_GROUP_TRANSLUCENT_ENTITY; }
	virtual void GetRenderBounds( Vector &mins, Vector &maxs ) override;
	virtual int  DrawModel( int flags ) override;

	// Fills the slice atlas; called by the texture regenerator.
	void FillAtlas( unsigned char *pDst, int pitch ) const;

	void PrintStats() const;

private:
	void Init( const Vector &origin, float spawnTime );
	void Shutdown();

	// Simulation
	void StepFill();
	float FrontRadius( float age ) const;
	void UpdateDensities();
	void SweepLight();
	static Vector SampleColor( const Vector &origin );
	void Carve( const Vector &start, const Vector &end, float radius );

private:
	static CUtlVector< C_NeoVolSmoke * > s_Volumes;
	static int s_nNextId;

	int    m_nId;
	Vector m_vecGridMin;
	Vector m_vecGridMax;
	Vector m_vecSpawn;

	float  m_flSpawnTime;
	Vector m_vecColor;         // linear smoke color, sampled once

	// Gas fill and the world it queries
	CNeoSmokeFill      m_Fill;
	CNeoVolSmokeWorld *m_pWorld;
	double m_flFillSeconds;    // CPU time spent in the fill
	double m_flFillPeakFrame;  // longest single fill slice

	// Per-cell state
	CUtlVector< float >         m_CarveTime;  // curtime of last carve, <0 = never
	CUtlVector< float >         m_Density;    // 0..1 current density
	CUtlVector< float >         m_Light;      // 0..1 transmittance toward light

	// GPU side
	CTextureReference        m_Atlas;
	CNeoVolSmokeRegenerator *m_pRegen;
	CMaterialReference       m_Material;
	bool  m_bAtlasDirty;
	float m_flNextLightSweep;
};

#endif // C_NEO_VOLSMOKE_H
