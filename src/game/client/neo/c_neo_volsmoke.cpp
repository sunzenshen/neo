// NEO volumetric smoke - client-side voxel smoke volume (demo).
// See c_neo_volsmoke.h for the overview.

#include "cbase.h"
#include "c_neo_volsmoke.h"

#include "materialsystem/imaterialsystem.h"
#include "materialsystem/imaterialvar.h"
#include "materialsystem/imesh.h"
#include "materialsystem/itexture.h"
#include "pixelwriter.h"
#include "vtf/vtf.h"
#include "engine/IEngineTrace.h"
#include "collisionutils.h"
#include "c_baseplayer.h"
#include "mathlib/mathlib.h"
#include "viewrender.h"
#include "tier1/KeyValues.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

// ---------------------------------------------------------------------------
// ConVars
// ---------------------------------------------------------------------------
static ConVar neo_volsmoke_regrow( "neo_volsmoke_regrow", "4.0", FCVAR_CHEAT, "Seconds for a carved cell to refill.", true, 0.1f, true, 60.0f );
static ConVar neo_volsmoke_carve_radius( "neo_volsmoke_carve_radius", "24", FCVAR_CHEAT, "Capsule radius carved by neo_volsmoke_carve.", true, 1.0f, true, 512.0f );
static ConVar neo_volsmoke_explode_radius( "neo_volsmoke_explode_radius", "110", FCVAR_CHEAT, "Sphere radius carved by neo_volsmoke_explode.", true, 1.0f, true, 1024.0f );
static ConVar neo_volsmoke_steps( "neo_volsmoke_steps", "64", FCVAR_CHEAT, "Raymarch steps across the box.", true, 4.0f, true, 64.0f );
static ConVar neo_volsmoke_noise_amp( "neo_volsmoke_noise_amp", "0.35", FCVAR_CHEAT, "Edge erosion noise amplitude.", true, 0.0f, true, 2.0f );
static ConVar neo_volsmoke_noise_scale( "neo_volsmoke_noise_scale", "0.045", FCVAR_CHEAT, "Noise frequency (1/world units).", true, 0.0f, true, 1.0f );
static ConVar neo_volsmoke_jitter( "neo_volsmoke_jitter", "1", FCVAR_CHEAT, "Per-pixel step jitter (grain) instead of banding." );
static ConVar neo_volsmoke_debug( "neo_volsmoke_debug", "0", FCVAR_CHEAT, "0 off, 1 coverage, 2 scene depth as read by the shader, 3 box hit." );
static ConVar neo_volsmoke_draw( "neo_volsmoke_draw", "1", FCVAR_CHEAT, "Draw smoke volumes." );

// ---------------------------------------------------------------------------
// Fixed gameplay values: they decide where the smoke blocks sight, so they are
// the same for every client.
// ---------------------------------------------------------------------------

// Grid footprint around the grenade: centered in x and y, mostly above it.
static constexpr float NEO_VOLSMOKE_HALF_XY = 0.5f * CNeoSmokeFill::GRID_XY * CNeoSmokeFill::CELL_SIZE;
static constexpr float NEO_VOLSMOKE_BELOW = CNeoSmokeFill::SEED_CELL_Z * CNeoSmokeFill::CELL_SIZE;
static constexpr float NEO_VOLSMOKE_ABOVE = ( CNeoSmokeFill::GRID_Z - CNeoSmokeFill::SEED_CELL_Z ) * CNeoSmokeFill::CELL_SIZE;

// Atlas: one GRID_XY square tile per z slice, power-of-two in both directions.
static constexpr int NEO_VOLSMOKE_ATLAS_COLS = 8;
static constexpr int NEO_VOLSMOKE_ATLAS_ROWS = CNeoSmokeFill::GRID_Z / NEO_VOLSMOKE_ATLAS_COLS;
static constexpr int NEO_VOLSMOKE_ATLAS_W = NEO_VOLSMOKE_ATLAS_COLS * CNeoSmokeFill::GRID_XY;
static constexpr int NEO_VOLSMOKE_ATLAS_H = NEO_VOLSMOKE_ATLAS_ROWS * CNeoSmokeFill::GRID_XY;

// Fill work per frame. A slow machine blooms later, never differently.
static constexpr int NEO_VOLSMOKE_FILL_CELLS_PER_FRAME = 1024;
static constexpr int NEO_VOLSMOKE_FILL_QUERIES_PER_FRAME = 4096;

// The legacy smoke's time curves: sine ease-out bloom, cosine fade.
static constexpr float NEO_VOLSMOKE_BLOOM_TIME = 5.5f;
static constexpr float NEO_VOLSMOKE_FADE_START = 17.0f;
static constexpr float NEO_VOLSMOKE_FADE_END = 25.0f;

// Width of the soft edge behind the expanding front.
static constexpr float NEO_VOLSMOKE_EDGE = 27.0f;

// Per-cell density variation, from the cell index so it is the same everywhere.
static constexpr float NEO_VOLSMOKE_CELL_VARIATION = 0.15f;

// Extinction per world unit at density 1.
static constexpr float NEO_VOLSMOKE_EXTINCTION = 0.06f;

// Until the fill finishes, the front grows as if toward the open-floor radius.
static constexpr float NEO_VOLSMOKE_NOMINAL_RADIUS = 165.0f;

// Look (cosmetic, may differ between clients).
// The legacy particle color: a 0.5-0.6 grey times the light, desaturated
// halfway to mid grey. It is a display value; the shader works in linear.
static constexpr float NEO_VOLSMOKE_GREY = 0.55f;
static constexpr float NEO_VOLSMOKE_DESATURATE = 0.5f;
static constexpr float NEO_VOLSMOKE_DISPLAY_GAMMA = 2.2f;

// Shade from a top-down light sweep: the top keeps full color, fully shaded
// smoke keeps the ambient share, so nothing goes black.
static constexpr float NEO_VOLSMOKE_AMBIENT_SHARE = 0.55f;
static constexpr float NEO_VOLSMOKE_SWEEP_EXTINCTION = 0.012f;
static constexpr float NEO_VOLSMOKE_SWEEP_HZ = 15.0f;

// Light-grid samples: the grenade and four points at half the dome radius.
static constexpr float NEO_VOLSMOKE_LIGHT_SAMPLE_UP = 32.0f;
static constexpr float NEO_VOLSMOKE_LIGHT_SAMPLE_OUT = 82.0f;

// World-only: no props or entities, so every client sees the same walls.
static constexpr int NEO_VOLSMOKE_FILL_MASK = CONTENTS_SOLID | CONTENTS_WINDOW;

static float VolSmokeBloom( float age )
{
	return sinf( 0.5f * M_PI_F * clamp( age, 0.0f, NEO_VOLSMOKE_BLOOM_TIME ) / NEO_VOLSMOKE_BLOOM_TIME );
}

static float VolSmokeFade( float age )
{
	if ( age <= NEO_VOLSMOKE_FADE_START )
	{
		return 1.0f;
	}

	const float f = clamp( ( age - NEO_VOLSMOKE_FADE_START ) / ( NEO_VOLSMOKE_FADE_END - NEO_VOLSMOKE_FADE_START ), 0.0f, 1.0f );
	return 0.5f + 0.5f * cosf( M_PI_F * f );
}

static float VolSmokeCellVariation( int idx )
{
	unsigned int h = (unsigned int)idx;
	h ^= h >> 16;
	h *= 0x7feb352dU;
	h ^= h >> 15;
	h *= 0x846ca68bU;
	h ^= h >> 16;
	return 1.0f - NEO_VOLSMOKE_CELL_VARIATION * (float)( h & 0xff ) / 255.0f;
}

// ---------------------------------------------------------------------------
// World queries for the fill: cell centers and the segments between them.
// ---------------------------------------------------------------------------
class CNeoVolSmokeWorld final : public INeoSmokeFillWorld
{
public:
	explicit CNeoVolSmokeWorld( const Vector &gridMin ) : m_vecGridMin( gridMin ) {}

	static bool PointSolid( const Vector &pos )
	{
		Ray_t ray;
		ray.Init( pos, pos );
		CTraceFilterWorldOnly filter;
		trace_t tr;
		enginetrace->TraceRay( ray, NEO_VOLSMOKE_FILL_MASK, &filter, &tr );
		return tr.startsolid;
	}

	virtual bool CellSolid( int x, int y, int z ) override
	{
		return PointSolid( Center( x, y, z ) );
	}

	virtual bool LinkBlocked( int x, int y, int z, int axis ) override
	{
		int n[3] = { x, y, z };
		++n[axis];

		Ray_t ray;
		ray.Init( Center( x, y, z ), Center( n[0], n[1], n[2] ) );
		CTraceFilterWorldOnly filter;
		trace_t tr;
		enginetrace->TraceRay( ray, NEO_VOLSMOKE_FILL_MASK, &filter, &tr );
		return tr.startsolid || tr.fraction < 1.0f;
	}

private:
	Vector Center( int x, int y, int z ) const
	{
		const float c = CNeoSmokeFill::CELL_SIZE;
		return m_vecGridMin + Vector( ( x + 0.5f ) * c, ( y + 0.5f ) * c, ( z + 0.5f ) * c );
	}

	Vector m_vecGridMin;
};

// ---------------------------------------------------------------------------
// Texture regenerator: owns a CPU copy of the atlas so the material system
// can pull bits whenever it likes without touching the simulation arrays.
// ---------------------------------------------------------------------------
class CNeoVolSmokeRegenerator final : public ITextureRegenerator
{
public:
	CNeoVolSmokeRegenerator( int width, int height )
		: m_nWidth( width ), m_nHeight( height )
	{
		m_Bits.SetCount( width * height * 4 );
		Q_memset( m_Bits.Base(), 0, m_Bits.Count() );
	}

	unsigned char *Bits() { return m_Bits.Base(); }
	int Pitch() const { return m_nWidth * 4; }

	virtual void RegenerateTextureBits( ITexture *pTexture, IVTFTexture *pVTFTexture, Rect_t *pRect ) override
	{
		const int w = pVTFTexture->Width();
		const int h = pVTFTexture->Height();
		if ( w != m_nWidth || h != m_nHeight )
			return;

		CPixelWriter writer;
		writer.SetPixelMemory( pVTFTexture->Format(), pVTFTexture->ImageData( 0, 0, 0 ), pVTFTexture->RowSizeInBytes( 0 ) );

		const unsigned char *src = m_Bits.Base();
		for ( int y = 0; y < h; ++y )
		{
			writer.Seek( 0, y );
			for ( int x = 0; x < w; ++x, src += 4 )
			{
				// Our buffer is R,G,B,A; CPixelWriter handles the target format.
				writer.WritePixel( src[0], src[1], src[2], src[3] );
			}
		}
	}

	virtual void Release() override { delete this; }

private:
	int m_nWidth;
	int m_nHeight;
	CUtlVector< unsigned char > m_Bits;
};

// ---------------------------------------------------------------------------
// Statics
// ---------------------------------------------------------------------------
CUtlVector< C_NeoVolSmoke * > C_NeoVolSmoke::s_Volumes;
int C_NeoVolSmoke::s_nNextId = 0;

// ---------------------------------------------------------------------------
C_NeoVolSmoke::C_NeoVolSmoke()
	: m_nId( s_nNextId++ ), m_flSpawnTime( 0 ), m_pWorld( NULL ), m_flFillSeconds( 0 ), m_flFillPeakFrame( 0 ),
	  m_pRegen( NULL ), m_bAtlasDirty( true ), m_flNextLightSweep( 0 )
{
}

C_NeoVolSmoke::~C_NeoVolSmoke()
{
	Shutdown();
	delete m_pWorld;
	s_Volumes.FindAndRemove( this );
}

C_NeoVolSmoke *C_NeoVolSmoke::Create( const Vector &origin, float spawnTime )
{
	C_NeoVolSmoke *pVol = new C_NeoVolSmoke();
	pVol->Init( origin, spawnTime );

	if ( !pVol->InitializeAsClientEntity( NULL, RENDER_GROUP_TRANSLUCENT_ENTITY ) )
	{
		pVol->Release();
		return NULL;
	}

	pVol->SetNextClientThink( CLIENT_THINK_ALWAYS );
	s_Volumes.AddToTail( pVol );
	return pVol;
}

void C_NeoVolSmoke::CarveAll( const Vector &start, const Vector &end, float radius )
{
	for ( int i = 0; i < s_Volumes.Count(); ++i )
	{
		s_Volumes[i]->Carve( start, end, radius );
	}
}

void C_NeoVolSmoke::PrintAllStats()
{
	for ( int i = 0; i < s_Volumes.Count(); ++i )
	{
		s_Volumes[i]->PrintStats();
	}
}

void C_NeoVolSmoke::RemoveAll()
{
	// Release() removes the entity from the list via the destructor.
	while ( s_Volumes.Count() > 0 )
	{
		s_Volumes[0]->Release();
	}
}

// ---------------------------------------------------------------------------
void C_NeoVolSmoke::Init( const Vector &origin, float spawnTime )
{
	m_vecSpawn    = origin;
	m_vecGridMin  = origin - Vector( NEO_VOLSMOKE_HALF_XY, NEO_VOLSMOKE_HALF_XY, NEO_VOLSMOKE_BELOW );
	m_vecGridMax  = origin + Vector( NEO_VOLSMOKE_HALF_XY, NEO_VOLSMOKE_HALF_XY, NEO_VOLSMOKE_ABOVE );
	m_flSpawnTime = spawnTime;

	SetAbsOrigin( origin );
	SetCollisionBounds( m_vecGridMin - origin, m_vecGridMax - origin );

	const int cells = CNeoSmokeFill::CELLS;
	m_CarveTime.SetCount( cells );
	m_Density.SetCount( cells );
	m_Light.SetCount( cells );
	for ( int i = 0; i < cells; ++i )
	{
		m_CarveTime[i] = -1.0f;
		m_Density[i]   = 0.0f;
		m_Light[i]     = 1.0f;
	}

	m_pWorld = new CNeoVolSmokeWorld( m_vecGridMin );
	m_Fill.Start( m_pWorld );

	m_vecColor = SampleColor( origin );

	// GPU resources
	const int atlasW = NEO_VOLSMOKE_ATLAS_W;
	const int atlasH = NEO_VOLSMOKE_ATLAS_H;

	char texName[64];
	Q_snprintf( texName, sizeof( texName ), "_neo_volsmoke_%d", m_nId );

	m_Atlas.InitProceduralTexture( texName, TEXTURE_GROUP_OTHER, atlasW, atlasH, IMAGE_FORMAT_BGRA8888,
		TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT | TEXTUREFLAGS_NOMIP | TEXTUREFLAGS_NOLOD | TEXTUREFLAGS_PROCEDURAL | TEXTUREFLAGS_SINGLECOPY );
	if ( !m_Atlas.IsValid() || IsErrorTexture( m_Atlas ) )
	{
		Warning( "C_NeoVolSmoke: failed to create procedural texture %s\n", texName );
		m_Atlas.Shutdown( true );
		return;
	}

	m_pRegen = new CNeoVolSmokeRegenerator( atlasW, atlasH );
	m_Atlas->SetTextureRegenerator( m_pRegen );

	// One material per volume: a shared material would carry one volume's
	// params and atlas into another volume's draw.
	char matName[64];
	Q_snprintf( matName, sizeof( matName ), "__neo_volsmoke_%d", m_nId );

	KeyValues *pVMT = new KeyValues( "Neo_VolSmoke" );
	pVMT->SetString( "$basetexture", texName );
	m_Material.Init( matName, pVMT );
	if ( !m_Material || m_Material->IsErrorMaterial() )
	{
		Warning( "C_NeoVolSmoke: material %s failed (needs the Neo_VolSmoke shader)\n", matName );
	}
	else
	{
		// Precaches the material. Binding an uncached material makes the
		// material system add a reference of its own, which would keep the
		// material and its atlas alive after the volume dies.
		m_Material->GetNumPasses();
	}

	m_bAtlasDirty = true;
	m_flNextLightSweep = 0.0f;
}

void C_NeoVolSmoke::Shutdown()
{
	// The material references the atlas, so it goes first.
	IMaterial *pMat = m_Material;
	m_Material.Shutdown();
	if ( pMat )
	{
		pMat->DeleteIfUnreferenced();
	}

	if ( m_Atlas.IsValid() )
	{
		m_Atlas->SetTextureRegenerator( NULL ); // calls Release() on m_pRegen
		m_pRegen = NULL;
		m_Atlas.Shutdown( true );
	}
}

// ---------------------------------------------------------------------------
// Simulation
// ---------------------------------------------------------------------------
Vector C_NeoVolSmoke::SampleColor( const Vector &origin )
{
	const float out = NEO_VOLSMOKE_LIGHT_SAMPLE_OUT;
	const Vector offsets[] =
	{
		Vector( 0, 0, 0 ), Vector( out, 0, 0 ), Vector( -out, 0, 0 ), Vector( 0, out, 0 ), Vector( 0, -out, 0 ),
	};

	Vector light( 0, 0, 0 );
	int samples = 0;
	for ( int i = 0; i < ARRAYSIZE( offsets ); ++i )
	{
		const Vector pos = origin + offsets[i] + Vector( 0, 0, NEO_VOLSMOKE_LIGHT_SAMPLE_UP );
		if ( i > 0 && CNeoVolSmokeWorld::PointSolid( pos ) )
		{
			continue;
		}

		light += engine->GetLightForPoint( pos, true );
		++samples;
	}
	light /= (float)samples;

	Vector color;
	for ( int k = 0; k < 3; ++k )
	{
		const float display = ( NEO_VOLSMOKE_GREY * clamp( light[k], 0.0f, 1.0f ) + NEO_VOLSMOKE_DESATURATE ) * 0.5f;
		color[k] = powf( display, NEO_VOLSMOKE_DISPLAY_GAMMA );
	}

	return color;
}

void C_NeoVolSmoke::StepFill()
{
	if ( m_Fill.IsDone() )
	{
		return;
	}

	const double start = Plat_FloatTime();
	m_Fill.Step( NEO_VOLSMOKE_FILL_CELLS_PER_FRAME, NEO_VOLSMOKE_FILL_QUERIES_PER_FRAME );
	const double spent = Plat_FloatTime() - start;

	m_flFillSeconds += spent;
	m_flFillPeakFrame = MAX( m_flFillPeakFrame, spent );
}

// Distance of the expanding front from the grenade, along the gas path.
float C_NeoVolSmoke::FrontRadius( float age ) const
{
	const float bloom = VolSmokeBloom( age );
	if ( m_Fill.IsDone() )
	{
		return CNeoSmokeFill::DistToUnits( m_Fill.FrontDist() ) * bloom;
	}

	return NEO_VOLSMOKE_NOMINAL_RADIUS * bloom;
}

void C_NeoVolSmoke::UpdateDensities()
{
	const float now    = gpGlobals->curtime;
	const float front  = FrontRadius( now - m_flSpawnTime );
	const float regrow = neo_volsmoke_regrow.GetFloat();

	// Only finalized cells carry smoke; the rest stay at zero.
	const int count = m_Fill.FinalizedCount();
	for ( int k = 0; k < count; ++k )
	{
		const int i = m_Fill.FinalizedCell( k );
		const float dist = CNeoSmokeFill::DistToUnits( m_Fill.Dist( i ) );
		const float edge = clamp( ( front - dist ) / NEO_VOLSMOKE_EDGE, 0.0f, 1.0f );
		const float body = edge * edge * ( 3.0f - 2.0f * edge );

		float hole = 1.0f;
		if ( m_CarveTime[i] >= 0.0f )
		{
			hole = clamp( ( now - m_CarveTime[i] ) / regrow, 0.0f, 1.0f );
			hole = hole * hole; // slow start, then refill
		}

		m_Density[i] = body * VolSmokeCellVariation( i ) * hole;
	}
}

// Top-down sweep: each cell gets the transmittance of the smoke above it.
void C_NeoVolSmoke::SweepLight()
{
	const float kStep = NEO_VOLSMOKE_SWEEP_EXTINCTION * CNeoSmokeFill::CELL_SIZE;
	for ( int y = 0; y < CNeoSmokeFill::GRID_XY; ++y )
	{
		for ( int x = 0; x < CNeoSmokeFill::GRID_XY; ++x )
		{
			float T = 1.0f;
			for ( int z = CNeoSmokeFill::GRID_Z - 1; z >= 0; --z )
			{
				const int i = CNeoSmokeFill::Index( x, y, z );
				m_Light[i] = T;
				T *= expf( -kStep * m_Density[i] );
			}
		}
	}
}

void C_NeoVolSmoke::Carve( const Vector &start, const Vector &end, float radius )
{
	// Quick reject against the AABB (expanded by radius).
	Vector mins = m_vecGridMin - Vector( radius, radius, radius );
	Vector maxs = m_vecGridMax + Vector( radius, radius, radius );
	Ray_t ray;
	ray.Init( start, end );
	if ( !IsBoxIntersectingRay( mins, maxs, ray ) )
		return;

	const Vector seg = end - start;
	const float  segLenSq = seg.LengthSqr();
	const float  r2 = radius * radius;
	const float  now = gpGlobals->curtime;

	// Only visit the cells inside the capsule's bounding box.
	Vector cmin = start.Min( end ) - Vector( radius, radius, radius );
	Vector cmax = start.Max( end ) + Vector( radius, radius, radius );
	const float cell = CNeoSmokeFill::CELL_SIZE;
	const int nxy = CNeoSmokeFill::GRID_XY;
	const int nz = CNeoSmokeFill::GRID_Z;
	int x0 = clamp( (int)floorf( ( cmin.x - m_vecGridMin.x ) / cell ), 0, nxy - 1 );
	int y0 = clamp( (int)floorf( ( cmin.y - m_vecGridMin.y ) / cell ), 0, nxy - 1 );
	int z0 = clamp( (int)floorf( ( cmin.z - m_vecGridMin.z ) / cell ), 0, nz - 1 );
	int x1 = clamp( (int)floorf( ( cmax.x - m_vecGridMin.x ) / cell ), 0, nxy - 1 );
	int y1 = clamp( (int)floorf( ( cmax.y - m_vecGridMin.y ) / cell ), 0, nxy - 1 );
	int z1 = clamp( (int)floorf( ( cmax.z - m_vecGridMin.z ) / cell ), 0, nz - 1 );

	bool touched = false;
	for ( int z = z0; z <= z1; ++z )
	{
		for ( int y = y0; y <= y1; ++y )
		{
			for ( int x = x0; x <= x1; ++x )
			{
				const int i = CNeoSmokeFill::Index( x, y, z );
				if ( !m_Fill.IsFinalized( i ) )
					continue;

				const Vector c = m_vecGridMin + Vector( ( x + 0.5f ) * cell, ( y + 0.5f ) * cell, ( z + 0.5f ) * cell );
				float t = 0.0f;
				if ( segLenSq > 1e-6f )
					t = clamp( DotProduct( c - start, seg ) / segLenSq, 0.0f, 1.0f );
				const Vector closest = start + seg * t;
				if ( ( c - closest ).LengthSqr() <= r2 )
				{
					m_CarveTime[i] = now;
					touched = true;
				}
			}
		}
	}

	if ( touched )
		m_bAtlasDirty = true;
}

// ---------------------------------------------------------------------------
void C_NeoVolSmoke::FillAtlas( unsigned char *pDst, int pitch ) const
{
	const int nxy = CNeoSmokeFill::GRID_XY;
	for ( int z = 0; z < CNeoSmokeFill::GRID_Z; ++z )
	{
		const int tileX = ( z % NEO_VOLSMOKE_ATLAS_COLS ) * nxy;
		const int tileY = ( z / NEO_VOLSMOKE_ATLAS_COLS ) * nxy;
		for ( int y = 0; y < nxy; ++y )
		{
			unsigned char *row = pDst + ( tileY + y ) * pitch + tileX * 4;
			for ( int x = 0; x < nxy; ++x )
			{
				const int i = CNeoSmokeFill::Index( x, y, z );
				row[x * 4 + 0] = (unsigned char)( clamp( m_Density[i], 0.0f, 1.0f ) * 255.0f + 0.5f ); // R density
				row[x * 4 + 1] = (unsigned char)( clamp( m_Light[i],   0.0f, 1.0f ) * 255.0f + 0.5f ); // G light
				row[x * 4 + 2] = 0;
				row[x * 4 + 3] = 255;
			}
		}
	}
}

void C_NeoVolSmoke::PrintStats() const
{
	Msg( "neo_volsmoke_stats: volume %d age %.2f done %d cells %d radius %.1f queries %d fill %.1f ms (peak frame %.2f ms)\n",
		m_nId, gpGlobals->curtime - m_flSpawnTime, m_Fill.IsDone() ? 1 : 0, m_Fill.FinalizedCount(),
		CNeoSmokeFill::DistToUnits( m_Fill.FrontDist() ), m_Fill.WorldQueries(),
		m_flFillSeconds * 1000.0, m_flFillPeakFrame * 1000.0 );
}

void C_NeoVolSmoke::ClientThink()
{
	const float now = gpGlobals->curtime;

	if ( now - m_flSpawnTime > NEO_VOLSMOKE_FADE_END )
	{
		Release();
		return;
	}

	StepFill();

	// Densities animate every frame (bloom, regrow).
	UpdateDensities();

	if ( now >= m_flNextLightSweep )
	{
		SweepLight();
		m_flNextLightSweep = now + 1.0f / NEO_VOLSMOKE_SWEEP_HZ;
	}

	if ( m_pRegen && m_Atlas.IsValid() )
	{
		FillAtlas( m_pRegen->Bits(), m_pRegen->Pitch() );
		m_Atlas->Download();
		m_bAtlasDirty = false;
	}
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
void C_NeoVolSmoke::GetRenderBounds( Vector &mins, Vector &maxs )
{
	mins = m_vecGridMin - GetRenderOrigin();
	maxs = m_vecGridMax - GetRenderOrigin();
}

static void SetVec3Var( IMaterial *pMat, const char *name, const Vector &v )
{
	bool found = false;
	IMaterialVar *pVar = pMat->FindVar( name, &found, false );
	if ( found ) pVar->SetVecValue( v.x, v.y, v.z );
}

static void SetVec4Var( IMaterial *pMat, const char *name, float x, float y, float z, float w )
{
	bool found = false;
	IMaterialVar *pVar = pMat->FindVar( name, &found, false );
	if ( found ) pVar->SetVecValue( x, y, z, w );
}

static void SetFloatVar( IMaterial *pMat, const char *name, float f )
{
	bool found = false;
	IMaterialVar *pVar = pMat->FindVar( name, &found, false );
	if ( found ) pVar->SetFloatValue( f );
}

static void SetIntVar( IMaterial *pMat, const char *name, int i )
{
	bool found = false;
	IMaterialVar *pVar = pMat->FindVar( name, &found, false );
	if ( found ) pVar->SetIntValue( i );
}

int C_NeoVolSmoke::DrawModel( int flags )
{
	if ( !neo_volsmoke_draw.GetBool() || !m_Material || m_Material->IsErrorMaterial() || !m_Atlas.IsValid() )
		return 0;

	// The scene depth the shader clips against is only current in the main view.
	if ( CurrentViewID() != VIEW_MAIN )
	{
		return 0;
	}

	IMaterial *pMat = m_Material;

	const Vector amb = m_vecColor * NEO_VOLSMOKE_AMBIENT_SHARE;
	const Vector lightCol = m_vecColor * ( 1.0f - NEO_VOLSMOKE_AMBIENT_SHARE );

	SetVec3Var( pMat, "$volmin", m_vecGridMin );
	SetVec3Var( pMat, "$volmax", m_vecGridMax );
	// Slices along z, square GRID_XY tiles.
	SetVec4Var( pMat, "$gridparams", (float)CNeoSmokeFill::GRID_Z, (float)NEO_VOLSMOKE_ATLAS_COLS, (float)NEO_VOLSMOKE_ATLAS_ROWS,
		1.0f / (float)CNeoSmokeFill::GRID_XY );
	SetVec3Var( pMat, "$ambient", amb );
	SetVec3Var( pMat, "$lightcolor", lightCol );
	// The legacy smoke scales its alpha by the bloom and the fade the same way.
	const float age = gpGlobals->curtime - m_flSpawnTime;
	SetFloatVar( pMat, "$extinction", NEO_VOLSMOKE_EXTINCTION * VolSmokeBloom( age ) * VolSmokeFade( age ) );
	SetFloatVar( pMat, "$steps", neo_volsmoke_steps.GetFloat() );
	SetFloatVar( pMat, "$noiseamp", neo_volsmoke_noise_amp.GetFloat() );
	SetFloatVar( pMat, "$noisescale", neo_volsmoke_noise_scale.GetFloat() );
	SetIntVar( pMat, "$jitter", neo_volsmoke_jitter.GetInt() );
	SetIntVar( pMat, "$debugmode", neo_volsmoke_debug.GetInt() );

	CMatRenderContextPtr pRenderContext( materials );

	pRenderContext->MatrixMode( MATERIAL_MODEL );
	pRenderContext->PushMatrix();
	pRenderContext->LoadIdentity();

	// Draw only the far side of the box: exactly one fragment per pixel, and
	// still correct with the camera inside the volume. The pixel shader does
	// the depth test against the scene itself.
	pRenderContext->CullMode( MATERIAL_CULLMODE_CCW );

	pRenderContext->Bind( pMat, (IClientRenderable *)this );
	IMesh *pMesh = pRenderContext->GetDynamicMesh( true, NULL, NULL, pMat );

	const Vector &a = m_vecGridMin;
	const Vector &b = m_vecGridMax;
	const Vector corners[8] =
	{
		Vector( a.x, a.y, a.z ), Vector( b.x, a.y, a.z ), Vector( b.x, b.y, a.z ), Vector( a.x, b.y, a.z ),
		Vector( a.x, a.y, b.z ), Vector( b.x, a.y, b.z ), Vector( b.x, b.y, b.z ), Vector( a.x, b.y, b.z ),
	};
	// Counter-clockwise seen from outside. Source treats clockwise as front
	// facing, so culling counter-clockwise faces keeps only the far side.
	static const unsigned short indices[36] =
	{
		0, 2, 1,  0, 3, 2,   // -z
		4, 5, 6,  4, 6, 7,   // +z
		0, 1, 5,  0, 5, 4,   // -y
		2, 3, 7,  2, 7, 6,   // +y
		1, 2, 6,  1, 6, 5,   // +x
		3, 0, 4,  3, 4, 7,   // -x
	};

	CMeshBuilder meshBuilder;
	meshBuilder.Begin( pMesh, MATERIAL_TRIANGLES, 8, 36 );
	for ( int i = 0; i < 8; ++i )
	{
		meshBuilder.Position3fv( corners[i].Base() );
		meshBuilder.AdvanceVertex();
	}
	for ( int i = 0; i < 36; ++i )
	{
		meshBuilder.Index( indices[i] );
		meshBuilder.AdvanceIndex();
	}
	meshBuilder.End();
	pMesh->Draw();

	pRenderContext->MatrixMode( MATERIAL_MODEL );
	pRenderContext->PopMatrix();

	return 1;
}

// ---------------------------------------------------------------------------
// Console commands
// ---------------------------------------------------------------------------
static bool ViewTrace( trace_t &tr, float dist )
{
	C_BasePlayer *pPlayer = C_BasePlayer::GetLocalPlayer();
	if ( !pPlayer )
		return false;

	Vector fwd;
	pPlayer->EyeVectors( &fwd );
	const Vector start = pPlayer->EyePosition();
	UTIL_TraceLine( start, start + fwd * dist, MASK_SOLID_BRUSHONLY, pPlayer, COLLISION_GROUP_NONE, &tr );
	return true;
}

// About where a resting grenade's origin sits above the surface.
static constexpr float NEO_VOLSMOKE_SPAWN_LIFT = 4.0f;

CON_COMMAND_F( neo_volsmoke_spawn, "Spawn a volumetric smoke at the crosshair, as a grenade resting there. Args: [age in seconds]", FCVAR_CHEAT )
{
	trace_t tr;
	if ( !ViewTrace( tr, 2048.0f ) )
		return;

	const float age = args.ArgC() > 1 ? atof( args[1] ) : 0.0f;

	Vector origin = tr.endpos + tr.plane.normal * NEO_VOLSMOKE_SPAWN_LIFT;
	if ( tr.fraction >= 1.0f )
		origin = tr.endpos;

	C_NeoVolSmoke *p = C_NeoVolSmoke::Create( origin, gpGlobals->curtime - age );
	Msg( "neo_volsmoke: %s at (%.0f %.0f %.0f), age %.1f, %d live\n", p ? "spawned" : "FAILED", origin.x, origin.y, origin.z, age, C_NeoVolSmoke::LiveCount() );
}

CON_COMMAND_F( neo_volsmoke_carve, "Carve a bullet-sized hole through smoke along your view.", FCVAR_CHEAT )
{
	trace_t tr;
	if ( !ViewTrace( tr, 4096.0f ) )
		return;
	C_NeoVolSmoke::CarveAll( tr.startpos, tr.endpos, neo_volsmoke_carve_radius.GetFloat() );
}

CON_COMMAND_F( neo_volsmoke_explode, "Carve an explosion-sized sphere out of smoke at the crosshair.", FCVAR_CHEAT )
{
	trace_t tr;
	if ( !ViewTrace( tr, 4096.0f ) )
		return;
	C_NeoVolSmoke::CarveAll( tr.endpos, tr.endpos, neo_volsmoke_explode_radius.GetFloat() );
}

CON_COMMAND_F( neo_volsmoke_clear, "Remove all volumetric smoke.", FCVAR_CHEAT )
{
	C_NeoVolSmoke::RemoveAll();
}

// A volume material holds its atlas, so a leaked material also shows up here.
CON_COMMAND_F( neo_volsmoke_stats, "Print live volumes and the atlas textures still loaded.", FCVAR_CHEAT )
{
	int textures = 0;
	for ( int id = 0; id < C_NeoVolSmoke::NextId(); ++id )
	{
		char name[64];
		Q_snprintf( name, sizeof( name ), "_neo_volsmoke_%d", id );
		textures += materials->IsTextureLoaded( name ) ? 1 : 0;
	}

	Msg( "neo_volsmoke_stats: live %d, created %d, atlas textures loaded %d\n",
		C_NeoVolSmoke::LiveCount(), C_NeoVolSmoke::NextId(), textures );
	C_NeoVolSmoke::PrintAllStats();
}
