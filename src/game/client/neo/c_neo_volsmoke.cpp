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
static ConVar neo_volsmoke_noise_amp( "neo_volsmoke_noise_amp", "1", FCVAR_CHEAT, "Test: look noise strength, 0 (smooth) to 1.", true, 0.0f, true, 1.0f );
static ConVar neo_volsmoke_jitter( "neo_volsmoke_jitter", "1", FCVAR_CHEAT, "Per-pixel step jitter (grain) instead of banding." );
static ConVar neo_volsmoke_debug( "neo_volsmoke_debug", "0", FCVAR_CHEAT, "0 off, 1 coverage, 2 scene depth as read by the shader, 3 box hit." );
static ConVar neo_volsmoke_slabs( "neo_volsmoke_slabs", "1", FCVAR_CHEAT, "Test: 0 one march clipped by dest-alpha depth only, 1 far slabs depth-tested at their middle, 2 at their far end." );
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

// Atlas texel bytes in BGRA order. The atlas holds what does not change with
// time: the gas path length, the cell weight (variation and carve hole) and
// the light. The shader applies the bloom front and the fade.
enum VolSmokeAtlasByte_t
{
	VOLSMOKE_BYTE_DIST = 0,   // B: path length, 0..NEO_VOLSMOKE_DIST_RANGE
	VOLSMOKE_BYTE_LIGHT = 1,  // G: transmittance toward the light
	VOLSMOKE_BYTE_WEIGHT = 2, // R: density of a cell inside the front
	VOLSMOKE_BYTE_ALPHA = 3,
};

// Path lengths are stored up to the grid's half width, past any front.
static constexpr float NEO_VOLSMOKE_DIST_RANGE = 0.5f * CNeoSmokeFill::GRID_XY * CNeoSmokeFill::CELL_SIZE;

// The atlas changes only while the fill, the bloom light or a regrow runs;
// it is uploaded then, at most this often, and never after.
static constexpr float NEO_VOLSMOKE_UPLOAD_HZ = 15.0f;

// Fill work per frame, shared by all volumes, in small chunks so a frame
// overruns the budget by at most one chunk. A slow machine blooms later,
// never differently.
static constexpr double NEO_VOLSMOKE_FILL_BUDGET = 0.0015; // seconds
static constexpr int NEO_VOLSMOKE_FILL_CHUNK_CELLS = 64;
static constexpr int NEO_VOLSMOKE_FILL_CHUNK_QUERIES = 128;

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

// The near pass clips against the engine's dest-alpha depth, which saturates
// about 194 u from the eye. It marches view depths up to NEAR_RANGE, kept
// below that with a margin; depth-tested slabs draw the rest.
static constexpr float NEO_VOLSMOKE_NEAR_RANGE = 172.0f;
static constexpr float NEO_VOLSMOKE_SLAB_MIN = 24.0f;
static constexpr float NEO_VOLSMOKE_SLAB_GROWTH = 0.08f; // of the slab's view depth
static constexpr int NEO_VOLSMOKE_MAX_SLABS = 24;

// View depth range of a whole-volume march; farther than any volume is drawn.
static constexpr float NEO_VOLSMOKE_MAX_DEPTH = 65536.0f;

// Bounds of a slab quad in view-space x / z and y / z: past any field of view.
static constexpr float NEO_VOLSMOKE_SLAB_MAX_TAN = 4.0f;

// Until the fill finishes, the front grows as if toward the open-floor radius.
static constexpr float NEO_VOLSMOKE_NOMINAL_RADIUS = CNeoSmokeFill::DOME_RADIUS;

// Look (cosmetic, may differ between clients).
// The legacy particle color: a 0.5-0.6 grey times the light, desaturated
// halfway to mid grey. It is a display value; the shader works in linear.
static constexpr float NEO_VOLSMOKE_GREY = 0.55f;
static constexpr float NEO_VOLSMOKE_DESATURATE = 0.5f;
static constexpr float NEO_VOLSMOKE_DISPLAY_GAMMA = 2.2f;

// Shade from a top-down light sweep: the top keeps full color, fully shaded
// smoke keeps the ambient share, so nothing goes black. The legacy sprites
// shade their base only a little.
static constexpr float NEO_VOLSMOKE_AMBIENT_SHARE = 0.8f;
static constexpr float NEO_VOLSMOKE_SWEEP_EXTINCTION = 0.012f;

// Light-grid samples: the grenade and four points at half the dome radius.
static constexpr float NEO_VOLSMOKE_LIGHT_SAMPLE_UP = 32.0f;
static constexpr float NEO_VOLSMOKE_LIGHT_SAMPLE_OUT = 0.5f * CNeoSmokeFill::DOME_RADIUS;

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
// CPU cost readout (neo_volsmoke_perf): time per stage, summed over volumes.
// ---------------------------------------------------------------------------
enum VolSmokePerfStage_t
{
	VOLSMOKE_PERF_FILL = 0,
	VOLSMOKE_PERF_DENSITY,
	VOLSMOKE_PERF_SWEEP,
	VOLSMOKE_PERF_PACK,
	VOLSMOKE_PERF_DOWNLOAD,
	VOLSMOKE_PERF_STAGES,
};

static const char *const s_VolSmokePerfNames[VOLSMOKE_PERF_STAGES] = { "fill", "density", "sweep", "pack", "download" };

struct VolSmokePerf_t
{
	double stage[VOLSMOKE_PERF_STAGES];
	double stagePeak[VOLSMOKE_PERF_STAGES];
	double frameTotal;     // this frame so far, all stages and volumes
	double framePeak;
	double frameTime;      // engine frame time summed over counted frames
	int    frames;
	int    lastFrame;
	int    uploads;
	double start;
};

static VolSmokePerf_t s_VolSmokePerf;

static void VolSmokePerfReset()
{
	Q_memset( &s_VolSmokePerf, 0, sizeof( s_VolSmokePerf ) );
	s_VolSmokePerf.lastFrame = -1;
	s_VolSmokePerf.start = Plat_FloatTime();
}

// Opens a frame the first time any volume thinks in it.
static void VolSmokePerfFrame()
{
	VolSmokePerf_t &p = s_VolSmokePerf;
	if ( p.lastFrame == gpGlobals->framecount )
	{
		return;
	}

	p.framePeak = MAX( p.framePeak, p.frameTotal );
	p.frameTotal = 0.0;
	p.lastFrame = gpGlobals->framecount;
	p.frameTime += gpGlobals->absoluteframetime;
	++p.frames;
}

class CVolSmokePerfScope
{
public:
	explicit CVolSmokePerfScope( VolSmokePerfStage_t stage ) : m_Stage( stage ), m_flStart( Plat_FloatTime() ) {}
	~CVolSmokePerfScope()
	{
		const double spent = Plat_FloatTime() - m_flStart;
		VolSmokePerf_t &p = s_VolSmokePerf;
		p.stage[m_Stage] += spent;
		p.stagePeak[m_Stage] = MAX( p.stagePeak[m_Stage], spent );
		p.frameTotal += spent;
	}

private:
	VolSmokePerfStage_t m_Stage;
	double m_flStart;
};

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
// Texture regenerator: owns the CPU copy of the atlas, in the texture's own
// BGRA byte order, so the material system can pull it whenever it likes.
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

		unsigned char *pDst = pVTFTexture->ImageData( 0, 0, 0 );
		const int dstPitch = pVTFTexture->RowSizeInBytes( 0 );
		if ( pVTFTexture->Format() == IMAGE_FORMAT_BGRA8888 && dstPitch == Pitch() )
		{
			Q_memcpy( pDst, m_Bits.Base(), m_Bits.Count() );
			return;
		}

		CPixelWriter writer;
		writer.SetPixelMemory( pVTFTexture->Format(), pDst, dstPitch );

		const unsigned char *src = m_Bits.Base();
		for ( int y = 0; y < h; ++y )
		{
			writer.Seek( 0, y );
			for ( int x = 0; x < w; ++x, src += 4 )
			{
				writer.WritePixel( src[2], src[1], src[0], src[3] );
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
	: m_nId( s_nNextId++ ), m_flSpawnTime( 0 ), m_pWorld( NULL ), m_flFillSeconds( 0 ), m_flFillPeakFrame( 0 ), m_flFillDoneAge( -1.0f ), m_nLagFrames( 0 ),
	  m_nBounded( 0 ), m_pRegen( NULL ), m_nPacked( 0 ), m_flNextUpload( 0 ), m_flRegrowEnd( -1.0f ), m_flSweptFront( -1.0f )
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

// The server spawns several particle entities per grenade at the same spot
// and time; they share one volume. The client can see their origins a
// little apart, so the match allows less than two cells.
static constexpr float NEO_VOLSMOKE_SAME_GRENADE_DIST = 16.0f;
static constexpr float NEO_VOLSMOKE_SAME_GRENADE_TIME = 0.1f;

C_NeoVolSmoke *C_NeoVolSmoke::CreateForGrenade( const Vector &origin, float spawnTime )
{
	if ( gpGlobals->curtime - spawnTime >= NEO_VOLSMOKE_FADE_END )
	{
		return NULL;
	}

	for ( int i = 0; i < s_Volumes.Count(); ++i )
	{
		const C_NeoVolSmoke *pVol = s_Volumes[i];
		if ( fabsf( pVol->m_flSpawnTime - spawnTime ) <= NEO_VOLSMOKE_SAME_GRENADE_TIME
			&& pVol->m_vecSpawn.DistToSqr( origin ) <= NEO_VOLSMOKE_SAME_GRENADE_DIST * NEO_VOLSMOKE_SAME_GRENADE_DIST )
		{
			DevMsg( "neo_volsmoke: grenade at (%.1f %.1f %.1f) shares volume %d (%.1f u apart)\n",
				origin.x, origin.y, origin.z, pVol->m_nId, pVol->m_vecSpawn.DistTo( origin ) );
			return NULL;
		}
	}

	C_NeoVolSmoke *pVol = Create( origin, spawnTime );
	DevMsg( "neo_volsmoke: grenade at (%.1f %.1f %.1f) spawned volume %d, age %.2f\n",
		origin.x, origin.y, origin.z, pVol ? pVol->m_nId : -1, gpGlobals->curtime - spawnTime );
	return pVol;
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
	m_vecSmokeMin = origin;
	m_vecSmokeMax = origin;
	m_vecGridMin  = origin - Vector( NEO_VOLSMOKE_HALF_XY, NEO_VOLSMOKE_HALF_XY, NEO_VOLSMOKE_BELOW );
	m_vecGridMax  = origin + Vector( NEO_VOLSMOKE_HALF_XY, NEO_VOLSMOKE_HALF_XY, NEO_VOLSMOKE_ABOVE );
	m_flSpawnTime = spawnTime;

	SetAbsOrigin( origin );
	SetCollisionBounds( m_vecGridMin - origin, m_vecGridMax - origin );

	const int cells = CNeoSmokeFill::CELLS;
	m_CarveTime.SetCount( cells );
	for ( int i = 0; i < cells; ++i )
	{
		m_CarveTime[i] = -1.0f;
	}

	for ( int a = 0; a < 3; ++a )
	{
		m_SmokeLo[a] = CNeoSmokeFill::GRID_XY;
		m_SmokeHi[a] = -1;
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
	ClearAtlas();
	m_Atlas->SetTextureRegenerator( m_pRegen );

	// One material per volume: a shared material would carry one volume's
	// params and atlas into another volume's draw.
	char matName[64];
	Q_snprintf( matName, sizeof( matName ), "__neo_volsmoke_%d", m_nId );

	KeyValues *pVMT = new KeyValues( "Neo_VolSmoke" );
	pVMT->SetString( "$basetexture", texName );
	m_Material.Init( matName, pVMT );

	Q_snprintf( matName, sizeof( matName ), "__neo_volsmoke_%d_slab", m_nId );
	pVMT = new KeyValues( "Neo_VolSmoke" );
	pVMT->SetString( "$basetexture", texName );
	pVMT->SetInt( "$slab", 1 );
	m_SlabMaterial.Init( matName, pVMT );

	CMaterialReference *pMaterials[] = { &m_Material, &m_SlabMaterial };
	for ( CMaterialReference *pRef : pMaterials )
	{
		IMaterial *pMat = *pRef;
		if ( !pMat || pMat->IsErrorMaterial() )
		{
			Warning( "C_NeoVolSmoke: material %s failed (needs the Neo_VolSmoke shader)\n", pMat ? pMat->GetName() : "?" );
			continue;
		}

		// Precaches the material. Binding an uncached material makes the
		// material system add a reference of its own, which would keep the
		// material and its atlas alive after the volume dies.
		pMat->GetNumPasses();
	}

}

void C_NeoVolSmoke::Shutdown()
{
	// The materials reference the atlas, so they go first.
	CMaterialReference *pMaterials[] = { &m_Material, &m_SlabMaterial };
	for ( CMaterialReference *pRef : pMaterials )
	{
		IMaterial *pMat = *pRef;
		pRef->Shutdown();
		if ( pMat )
		{
			pMat->DeleteIfUnreferenced();
		}
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

	static int s_nBudgetFrame = -1;
	static double s_flBudgetSpent = 0.0;
	if ( s_nBudgetFrame != gpGlobals->framecount )
	{
		s_nBudgetFrame = gpGlobals->framecount;
		s_flBudgetSpent = 0.0;
	}

	const double start = Plat_FloatTime();
	double spent = 0.0;
	while ( !m_Fill.IsDone() && s_flBudgetSpent + spent < NEO_VOLSMOKE_FILL_BUDGET )
	{
		m_Fill.Step( NEO_VOLSMOKE_FILL_CHUNK_CELLS, NEO_VOLSMOKE_FILL_CHUNK_QUERIES );
		spent = Plat_FloatTime() - start;
	}

	s_flBudgetSpent += spent;
	m_flFillSeconds += spent;
	m_flFillPeakFrame = MAX( m_flFillPeakFrame, spent );
	if ( m_Fill.IsDone() )
	{
		m_flFillDoneAge = gpGlobals->curtime - m_flSpawnTime;
	}
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

// Density of a cell inside the front: its variation, times the carve hole
// while it refills.
float C_NeoVolSmoke::CellWeight( int idx, float now ) const
{
	float hole = 1.0f;
	if ( m_CarveTime[idx] >= 0.0f )
	{
		hole = clamp( ( now - m_CarveTime[idx] ) / neo_volsmoke_regrow.GetFloat(), 0.0f, 1.0f );
		hole = hole * hole; // slow start, then refill
	}

	return VolSmokeCellVariation( idx ) * hole;
}

static unsigned char VolSmokeByte( float v )
{
	return (unsigned char)( clamp( v, 0.0f, 1.0f ) * 255.0f + 0.5f );
}

// Byte offset of a cell's texel: slice z lives in tile ( z % cols, z / cols ).
static int VolSmokeAtlasOffset( int idx )
{
	int x, y, z;
	CNeoSmokeFill::Coords( idx, x, y, z );
	const int tileX = ( z % NEO_VOLSMOKE_ATLAS_COLS ) * CNeoSmokeFill::GRID_XY;
	const int tileY = ( z / NEO_VOLSMOKE_ATLAS_COLS ) * CNeoSmokeFill::GRID_XY;
	return ( ( tileY + y ) * NEO_VOLSMOKE_ATLAS_W + tileX + x ) * 4;
}

void C_NeoVolSmoke::ClearAtlas()
{
	unsigned char *bits = m_pRegen->Bits();
	for ( int i = 0; i < NEO_VOLSMOKE_ATLAS_W * NEO_VOLSMOKE_ATLAS_H; ++i )
	{
		unsigned char *texel = bits + i * 4;
		texel[VOLSMOKE_BYTE_DIST] = 255;
		texel[VOLSMOKE_BYTE_LIGHT] = 255;
		texel[VOLSMOKE_BYTE_WEIGHT] = 0;
		texel[VOLSMOKE_BYTE_ALPHA] = 255;
	}
}

// Writes the cells the fill finalized since the last call. A trilinear lookup
// beside the gas also reads cells without gas; those take the path length of
// their nearest gas neighbor, so their zero weight fades the edge as before
// instead of a jump in path length cutting it off.
bool C_NeoVolSmoke::PackNewCells( float now )
{
	unsigned char *bits = m_pRegen->Bits();
	const int count = m_Fill.FinalizedCount();
	const bool packed = m_nPacked < count;
	for ( ; m_nPacked < count; ++m_nPacked )
	{
		const int i = m_Fill.FinalizedCell( m_nPacked );
		const unsigned char dist = VolSmokeByte( CNeoSmokeFill::DistToUnits( m_Fill.Dist( i ) ) / NEO_VOLSMOKE_DIST_RANGE );
		unsigned char *texel = bits + VolSmokeAtlasOffset( i );
		texel[VOLSMOKE_BYTE_DIST] = dist;
		texel[VOLSMOKE_BYTE_WEIGHT] = VolSmokeByte( CellWeight( i, now ) );

		int c[3];
		CNeoSmokeFill::Coords( i, c[0], c[1], c[2] );
		for ( int dz = -1; dz <= 1; ++dz )
		{
			for ( int dy = -1; dy <= 1; ++dy )
			{
				for ( int dx = -1; dx <= 1; ++dx )
				{
					if ( !CNeoSmokeFill::InGrid( c[0] + dx, c[1] + dy, c[2] + dz ) )
					{
						continue;
					}

					const int n = CNeoSmokeFill::Index( c[0] + dx, c[1] + dy, c[2] + dz );
					if ( m_Fill.IsFinalized( n ) )
					{
						continue;
					}

					unsigned char &nDist = bits[VolSmokeAtlasOffset( n ) + VOLSMOKE_BYTE_DIST];
					nDist = MIN( nDist, dist );
				}
			}
		}
	}

	return packed;
}

// Rewrites every cell's weight while carved cells refill.
void C_NeoVolSmoke::PackWeights( float now )
{
	unsigned char *bits = m_pRegen->Bits();
	for ( int k = 0; k < m_nPacked; ++k )
	{
		const int i = m_Fill.FinalizedCell( k );
		bits[VolSmokeAtlasOffset( i ) + VOLSMOKE_BYTE_WEIGHT] = VolSmokeByte( CellWeight( i, now ) );
	}
}

// Grows the drawn box over the finalized cells inside the front. Cells are
// finalized in path length order, so the cells inside are a prefix.
void C_NeoVolSmoke::UpdateBounds( float front )
{
	const int count = m_Fill.FinalizedCount();
	bool grew = false;
	for ( ; m_nBounded < count; ++m_nBounded )
	{
		const int i = m_Fill.FinalizedCell( m_nBounded );
		if ( CNeoSmokeFill::DistToUnits( m_Fill.Dist( i ) ) >= front )
		{
			break;
		}

		int c[3];
		CNeoSmokeFill::Coords( i, c[0], c[1], c[2] );
		for ( int a = 0; a < 3; ++a )
		{
			m_SmokeLo[a] = MIN( m_SmokeLo[a], c[a] );
			m_SmokeHi[a] = MAX( m_SmokeHi[a], c[a] );
		}
		grew = true;
	}

	if ( !grew )
	{
		return;
	}

	// One cell of margin: the shader's trilinear lookup spreads a cell's
	// smoke half a cell past it.
	const float cell = CNeoSmokeFill::CELL_SIZE;
	for ( int a = 0; a < 3; ++a )
	{
		m_vecSmokeMin[a] = MAX( m_vecGridMin[a], m_vecGridMin[a] + ( m_SmokeLo[a] - 1 ) * cell );
		m_vecSmokeMax[a] = MIN( m_vecGridMax[a], m_vecGridMin[a] + ( m_SmokeHi[a] + 2 ) * cell );
	}
}

// Top-down sweep: each cell gets the transmittance of the smoke above it.
// Only the columns and heights a lookup near the smoke can read are swept;
// the rest keep full light.
void C_NeoVolSmoke::SweepLight( float front, float now )
{
	if ( m_SmokeHi[0] < 0 )
	{
		return;
	}

	static constexpr int MARGIN = 2;
	int lo[3];
	int hi[3];
	const int size[3] = { CNeoSmokeFill::GRID_XY, CNeoSmokeFill::GRID_XY, CNeoSmokeFill::GRID_Z };
	for ( int a = 0; a < 3; ++a )
	{
		lo[a] = MAX( 0, m_SmokeLo[a] - MARGIN );
		hi[a] = MIN( size[a] - 1, m_SmokeHi[a] + MARGIN );
	}

	unsigned char *bits = m_pRegen->Bits();
	const float kStep = NEO_VOLSMOKE_SWEEP_EXTINCTION * CNeoSmokeFill::CELL_SIZE;
	for ( int y = lo[1]; y <= hi[1]; ++y )
	{
		for ( int x = lo[0]; x <= hi[0]; ++x )
		{
			float T = 1.0f;
			for ( int z = hi[2]; z >= lo[2]; --z )
			{
				const int i = CNeoSmokeFill::Index( x, y, z );
				bits[VolSmokeAtlasOffset( i ) + VOLSMOKE_BYTE_LIGHT] = VolSmokeByte( T );
				if ( !m_Fill.IsFinalized( i ) )
				{
					continue;
				}

				const float edge = clamp( ( front - CNeoSmokeFill::DistToUnits( m_Fill.Dist( i ) ) ) / NEO_VOLSMOKE_EDGE, 0.0f, 1.0f );
				const float body = edge * edge * ( 3.0f - 2.0f * edge );
				T *= expf( -kStep * body * CellWeight( i, now ) );
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
	{
		m_flRegrowEnd = MAX( m_flRegrowEnd, now + neo_volsmoke_regrow.GetFloat() );
	}
}

// ---------------------------------------------------------------------------
void C_NeoVolSmoke::PrintStats() const
{
	Msg( "neo_volsmoke_stats: volume %d age %.2f done %d (at age %.2f) cells %d radius %.1f queries %d fill %.1f ms (peak frame %.2f ms), front ahead of the fill in %d frames\n",
		m_nId, gpGlobals->curtime - m_flSpawnTime, m_Fill.IsDone() ? 1 : 0, m_flFillDoneAge, m_Fill.FinalizedCount(),
		CNeoSmokeFill::DistToUnits( m_Fill.FrontDist() ), m_Fill.WorldQueries(),
		m_flFillSeconds * 1000.0, m_flFillPeakFrame * 1000.0, m_nLagFrames );
}

void C_NeoVolSmoke::ClientThink()
{
	const float now = gpGlobals->curtime;

	if ( now - m_flSpawnTime > NEO_VOLSMOKE_FADE_END )
	{
		Release();
		return;
	}

	VolSmokePerfFrame();

	{
		CVolSmokePerfScope perf( VOLSMOKE_PERF_FILL );
		StepFill();
	}

	const float front = FrontRadius( now - m_flSpawnTime );
	if ( !m_Fill.IsDone() && CNeoSmokeFill::DistToUnits( m_Fill.FrontDist() ) < front )
	{
		++m_nLagFrames;
	}

	{
		CVolSmokePerfScope perf( VOLSMOKE_PERF_DENSITY );
		UpdateBounds( front );
	}

	if ( !m_pRegen || !m_Atlas.IsValid() || now < m_flNextUpload )
	{
		return;
	}

	// A grown volume with no carve refilling changes nothing.
	bool changed = false;
	{
		CVolSmokePerfScope perf( VOLSMOKE_PERF_PACK );
		changed = PackNewCells( now );
		if ( now < m_flRegrowEnd + 1.0f / NEO_VOLSMOKE_UPLOAD_HZ )
		{
			PackWeights( now );
			changed = true;
		}
	}

	if ( changed || front != m_flSweptFront )
	{
		CVolSmokePerfScope perf( VOLSMOKE_PERF_SWEEP );
		SweepLight( front, now );
		m_flSweptFront = front;
		changed = true;
	}

	if ( !changed )
	{
		return;
	}

	CVolSmokePerfScope perf( VOLSMOKE_PERF_DOWNLOAD );
	m_Atlas->Download();
	++s_VolSmokePerf.uploads;
	m_flNextUpload = now + 1.0f / NEO_VOLSMOKE_UPLOAD_HZ;
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

void C_NeoVolSmoke::SetDrawParams( IMaterial *pMat ) const
{
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
	SetVec3Var( pMat, "$front", Vector( FrontRadius( age ), NEO_VOLSMOKE_DIST_RANGE, 1.0f / NEO_VOLSMOKE_EDGE ) );
	SetFloatVar( pMat, "$noiseamp", neo_volsmoke_noise_amp.GetFloat() );
	SetFloatVar( pMat, "$age", age );
	SetIntVar( pMat, "$jitter", neo_volsmoke_jitter.GetInt() );
	SetIntVar( pMat, "$debugmode", neo_volsmoke_debug.GetInt() );
}

int C_NeoVolSmoke::DrawModel( int flags )
{
	if ( !neo_volsmoke_draw.GetBool() || !m_Atlas.IsValid() )
	{
		return 0;
	}

	if ( !m_Material || m_Material->IsErrorMaterial() || !m_SlabMaterial || m_SlabMaterial->IsErrorMaterial() )
	{
		return 0;
	}

	// The scene depth the shader clips against is only current in the main view.
	if ( CurrentViewID() != VIEW_MAIN )
	{
		return 0;
	}

	// Nothing to draw before the first cell fills.
	if ( m_vecSmokeMax.x <= m_vecSmokeMin.x )
	{
		return 0;
	}

	SetDrawParams( m_Material );
	SetDrawParams( m_SlabMaterial );

	CMatRenderContextPtr pRenderContext( materials );

	pRenderContext->MatrixMode( MATERIAL_MODEL );
	pRenderContext->PushMatrix();
	pRenderContext->LoadIdentity();

	// Back to front: the far slabs, then the near pass in front of them.
	DrawFarSlabs( pRenderContext );
	DrawNearPass( pRenderContext );

	pRenderContext->MatrixMode( MATERIAL_MODEL );
	pRenderContext->PopMatrix();

	return 1;
}

// The part of the volume beyond NEAR_RANGE, as view-parallel quads at the
// middle depth of each slab. The depth test hides a slab behind geometry, so
// smoke behind a surface the dest-alpha depth cannot see is wrong by at most
// half a slab along the ray.
void C_NeoVolSmoke::DrawFarSlabs( IMatRenderContext *pRenderContext )
{
	const Vector &eye = CurrentViewOrigin();
	const Vector &fwd = CurrentViewForward();
	const Vector &right = CurrentViewRight();
	const Vector &up = CurrentViewUp();

	const Vector &a = m_vecSmokeMin;
	const Vector &b = m_vecSmokeMax;

	// View depth range of the box, and its screen rectangle as view-space
	// x / z and y / z. A corner at or behind the eye makes the rectangle the
	// whole view.
	float zMin = FLT_MAX;
	float zMax = -FLT_MAX;
	float xMin = NEO_VOLSMOKE_SLAB_MAX_TAN;
	float xMax = -NEO_VOLSMOKE_SLAB_MAX_TAN;
	float yMin = NEO_VOLSMOKE_SLAB_MAX_TAN;
	float yMax = -NEO_VOLSMOKE_SLAB_MAX_TAN;
	bool bWholeView = false;
	for ( int i = 0; i < 8; ++i )
	{
		const Vector corner( ( i & 1 ) ? b.x : a.x, ( i & 2 ) ? b.y : a.y, ( i & 4 ) ? b.z : a.z );
		const Vector rel = corner - eye;
		const float z = DotProduct( rel, fwd );
		zMin = MIN( zMin, z );
		zMax = MAX( zMax, z );

		if ( z < 1.0f )
		{
			bWholeView = true;
			continue;
		}

		const float x = DotProduct( rel, right ) / z;
		const float y = DotProduct( rel, up ) / z;
		xMin = MIN( xMin, x );
		xMax = MAX( xMax, x );
		yMin = MIN( yMin, y );
		yMax = MAX( yMax, y );
	}

	if ( zMax <= NEO_VOLSMOKE_NEAR_RANGE || neo_volsmoke_slabs.GetInt() == 0 )
	{
		return;
	}
	const float testAt = neo_volsmoke_slabs.GetInt() == 2 ? 1.0f : 0.5f;

	if ( bWholeView )
	{
		xMin = yMin = -NEO_VOLSMOKE_SLAB_MAX_TAN;
		xMax = yMax = NEO_VOLSMOKE_SLAB_MAX_TAN;
	}
	xMin = MAX( xMin, -NEO_VOLSMOKE_SLAB_MAX_TAN );
	yMin = MAX( yMin, -NEO_VOLSMOKE_SLAB_MAX_TAN );
	xMax = MIN( xMax, NEO_VOLSMOKE_SLAB_MAX_TAN );
	yMax = MIN( yMax, NEO_VOLSMOKE_SLAB_MAX_TAN );

	// Slab ends are whole units: the shader rounds the range it is given, and
	// the near pass ends at NEAR_RANGE exactly.
	float bounds[NEO_VOLSMOKE_MAX_SLABS + 1];
	int nSlabs = 0;
	bounds[0] = MAX( NEO_VOLSMOKE_NEAR_RANGE, floorf( zMin ) );
	const float zEnd = ceilf( zMax );
	while ( bounds[nSlabs] < zEnd )
	{
		const float z = bounds[nSlabs];
		float next = ceilf( z + MAX( NEO_VOLSMOKE_SLAB_MIN, NEO_VOLSMOKE_SLAB_GROWTH * z ) );
		if ( nSlabs == NEO_VOLSMOKE_MAX_SLABS - 1 || next > zEnd )
		{
			next = zEnd;
		}

		bounds[++nSlabs] = next;
	}

	pRenderContext->Bind( m_SlabMaterial, (IClientRenderable *)this );
	IMesh *pMesh = pRenderContext->GetDynamicMesh( true, NULL, NULL, m_SlabMaterial );

	CMeshBuilder meshBuilder;
	meshBuilder.Begin( pMesh, MATERIAL_TRIANGLES, 4 * nSlabs, 6 * nSlabs );
	int nVert = 0;
	for ( int s = nSlabs - 1; s >= 0; --s )
	{
		const float lo = bounds[s];
		const float hi = bounds[s + 1];
		const float zc = lo + testAt * ( hi - lo );
		const Vector center = eye + fwd * zc;
		const float xs[4] = { xMin, xMax, xMax, xMin };
		const float ys[4] = { yMin, yMin, yMax, yMax };
		for ( int i = 0; i < 4; ++i )
		{
			const Vector pos = center + right * ( xs[i] * zc ) + up * ( ys[i] * zc );
			meshBuilder.Position3fv( pos.Base() );
			meshBuilder.TexCoord2f( 0, lo, hi );
			meshBuilder.AdvanceVertex();
		}

		static const int quad[6] = { 0, 1, 2, 0, 2, 3 };
		for ( int i = 0; i < 6; ++i )
		{
			meshBuilder.Index( nVert + quad[i] );
			meshBuilder.AdvanceIndex();
		}
		nVert += 4;
	}
	meshBuilder.End();
	pMesh->Draw();
}

// The box's far faces, marching view depths up to NEAR_RANGE: exactly one
// fragment per pixel, and still correct with the camera inside the volume.
// The pixel shader does the depth test against the scene itself.
void C_NeoVolSmoke::DrawNearPass( IMatRenderContext *pRenderContext )
{
	pRenderContext->CullMode( MATERIAL_CULLMODE_CCW );

	pRenderContext->Bind( m_Material, (IClientRenderable *)this );
	IMesh *pMesh = pRenderContext->GetDynamicMesh( true, NULL, NULL, m_Material );

	const Vector &a = m_vecSmokeMin;
	const Vector &b = m_vecSmokeMax;
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
		meshBuilder.TexCoord2f( 0, 0.0f, neo_volsmoke_slabs.GetInt() == 0 ? NEO_VOLSMOKE_MAX_DEPTH : NEO_VOLSMOKE_NEAR_RANGE );
		meshBuilder.AdvanceVertex();
	}
	for ( int i = 0; i < 36; ++i )
	{
		meshBuilder.Index( indices[i] );
		meshBuilder.AdvanceIndex();
	}
	meshBuilder.End();
	pMesh->Draw();
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

CON_COMMAND_F( neo_volsmoke_perf, "Print the smoke's CPU time per frame by stage since the last reset. Args: [reset]", FCVAR_CHEAT )
{
	VolSmokePerf_t &p = s_VolSmokePerf;
	if ( args.ArgC() > 1 && !Q_stricmp( args[1], "reset" ) )
	{
		VolSmokePerfReset();
		Msg( "neo_volsmoke_perf: reset\n" );
		return;
	}

	// The frame in progress counts toward the peak.
	const double framePeak = MAX( p.framePeak, p.frameTotal );
	const int frames = MAX( p.frames, 1 );
	const double seconds = MAX( Plat_FloatTime() - p.start, 1.0e-3 );

	double total = 0.0;
	for ( int s = 0; s < VOLSMOKE_PERF_STAGES; ++s )
	{
		total += p.stage[s];
	}

	Msg( "neo_volsmoke_perf: live %d frames %d over %.2f s, engine frame %.3f ms, smoke cpu %.3f ms/frame (peak frame %.2f ms), uploads %d (%.1f/s)\n",
		C_NeoVolSmoke::LiveCount(), p.frames, seconds, 1000.0 * p.frameTime / frames, 1000.0 * total / frames,
		1000.0 * framePeak, p.uploads, p.uploads / seconds );
	for ( int s = 0; s < VOLSMOKE_PERF_STAGES; ++s )
	{
		Msg( "neo_volsmoke_perf:   %-8s %.3f ms/frame (peak %.2f ms)\n", s_VolSmokePerfNames[s],
			1000.0 * p.stage[s] / frames, 1000.0 * p.stagePeak[s] );
	}
}
