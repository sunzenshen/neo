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
#include "smoke_fog_overlay.h"
#include "dlight.h"
#include "iefx.h"
#include "cdll_util.h"
#include "c_neo_player.h"
#include "vstdlib/jobthread.h"
#include "neo_smoke_shared.h"
#include "neo_smoke_world.h"
#include "neo_smoke_jobs.h"

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
static ConVar neo_volsmoke_slabs( "neo_volsmoke_slabs", "1", FCVAR_CHEAT, "Test: 0 one march clipped by dest-alpha depth only, 1 far slabs depth-tested at their middle, 2 at their far end, 3 as 1 without the opaque-depth cull." );
static ConVar neo_volsmoke_draw( "neo_volsmoke_draw", "1", FCVAR_CHEAT, "Draw smoke volumes." );
static ConVar neo_volsmoke_fill_audit( "neo_volsmoke_fill_audit", "0", FCVAR_CHEAT, "Test: also ask the point and segment queries where a block query answered, keep their answer and count disagreements." );

// ---------------------------------------------------------------------------
// Fixed values of the drawing. The gameplay values, which decide where the
// smoke blocks sight, are in neo_smoke_shared.h.
// ---------------------------------------------------------------------------

// Atlas texel bytes in BGRA order. The atlas holds what does not change with
// time: the gas path length, the cell weight (variation and carve hole) and
// the light. The shader applies the bloom front and the fade.
enum VolSmokeAtlasByte_t
{
	VOLSMOKE_BYTE_DIST = 0,   // B: path length, 0..the volume's distance range
	VOLSMOKE_BYTE_LIGHT = 1,  // G: transmittance toward the light
	VOLSMOKE_BYTE_WEIGHT = 2, // R: density of a cell inside the front
	VOLSMOKE_BYTE_ALPHA = 3,  // A: distance to the wall beside the gas
};

// The look noise turns about a point this far above the grenade, near the
// middle of a cloud on open floor.
static constexpr float NEO_VOLSMOKE_SPIN_HEIGHT = 8 * CNeoSmokeFill::CELL_SIZE;

// Path lengths are stored up to this length, or up to the front when it is
// farther, so a short field keeps its precision.
static constexpr float NEO_VOLSMOKE_DIST_RANGE = 64 * CNeoSmokeFill::CELL_SIZE;

// The atlas changes only while the fill, the bloom light or a regrow runs;
// it is uploaded then, at most this often, and never after.
static constexpr float NEO_VOLSMOKE_UPLOAD_HZ = 15.0f;

// Fills run on worker threads, one job per fill, so a frame never pays for
// one. The main thread keeps a physical core of its own; the cap bounds the
// fill memory held at once. Without a worker pool the
// frame takes a fixed slice of fill work instead, in small chunks so it
// overruns by at most one chunk. A slow machine blooms later, never
// differently.
static constexpr int NEO_VOLSMOKE_FILL_MAX_WORKERS = 8;
static constexpr double NEO_VOLSMOKE_FILL_BUDGET = 0.0015; // seconds a frame, without workers
static constexpr float NEO_VOLSMOKE_FILL_SAME_AGE = 0.25f; // seconds apart that count as one age

// The near pass clips against the engine's dest-alpha depth, which saturates
// about 194 u from the eye. It marches view depths up to NEAR_RANGE, kept
// below that with a margin; depth-tested slabs draw the rest.
static constexpr float NEO_VOLSMOKE_NEAR_RANGE = 172.0f;
static constexpr float NEO_VOLSMOKE_SLAB_MIN = 24.0f;
static constexpr float NEO_VOLSMOKE_SLAB_GROWTH = 0.08f; // of the slab's view depth
static constexpr int NEO_VOLSMOKE_MAX_SLABS = 32;

// Far slab cull: a coarse grid of eye rays finds the view depth past which
// the smoke is certainly opaque, and slab parts behind it are not drawn.
static constexpr float NEO_VOLSMOKE_CULL_TILE_PX = 64.0f;
static constexpr int NEO_VOLSMOKE_CULL_MAX_COLS = 24;
static constexpr int NEO_VOLSMOKE_CULL_MAX_ROWS = 14;

// Coarser than the shader's step to keep the CPU cost low; the opaque
// margin below covers the difference in sampling.
static constexpr float NEO_VOLSMOKE_CULL_STEP = 16.0f;

// Optical depth taken as opaque: transmittance 0.001, below the shader's
// cutoff, so its different step phase cannot let hidden smoke show.
static constexpr float NEO_VOLSMOKE_CULL_OPAQUE = 6.9f;

// The shader's look noise moves the front in by up to this, times its
// strength; the cull assumes the most.
static constexpr float NEO_VOLSMOKE_PUFF_BITE = 28.0f;

// The grid covers the view frustum with this margin, and starts this far in
// front of the eye so a box around the camera keeps a finite screen size.
static constexpr float NEO_VOLSMOKE_CULL_FRUSTUM_MARGIN = 1.02f;
static constexpr float NEO_VOLSMOKE_CULL_MIN_DEPTH = 1.0f;

// View depth range of a whole-volume march; farther than any volume is drawn.
static constexpr float NEO_VOLSMOKE_MAX_DEPTH = 65536.0f;

// All smoke lies inside a sphere about the gas seed this much wider than the
// front: a gas path is never shorter than the straight line from the seed,
// and a trilinear lookup spreads a cell's path length up to sqrt(3) cells.
// One more cell covers the atlas rounding and the clamp at the grid's z ends,
// and one march step keeps the cells the shader clips at the sphere empty.
static constexpr float NEO_VOLSMOKE_MARCH_STEP = 8.0f; // the shader's STEP_LEN
static constexpr float NEO_VOLSMOKE_SPHERE_MARGIN = ( 1.7321f + 1.0f ) * CNeoSmokeFill::CELL_SIZE + NEO_VOLSMOKE_MARCH_STEP;

// The atlas's fourth byte holds the distance to the wall beside the gas, in
// world units: positive on the gas side, negative past the wall's face. The
// trilinear lookup of a plane's distance is exact, so the shader finds the
// face inside a cell and draws no smoke past it. WALL_CAP is below the least
// distance at which a face can miss all six links of a cell.
//
// A far slab has no scene depth to clip its march, so it stops at the first
// negative distance instead: the cells without gas up to WALL_REACH cells from
// the gas carry it, which is deeper than one march step can skip.
static constexpr float NEO_VOLSMOKE_WALL_CAP = 5.0f;
static constexpr int NEO_VOLSMOKE_WALL_REACH = 2;
static constexpr float NEO_VOLSMOKE_WALL_STEPS_PER_UNIT = 12.0f;
static constexpr int NEO_VOLSMOKE_WALL_UNSET = 255; // no gas cell has written this cell
static constexpr int NEO_VOLSMOKE_WALL_ZERO_BYTE = 255 - (int)( NEO_VOLSMOKE_WALL_CAP * NEO_VOLSMOKE_WALL_STEPS_PER_UNIT );

// The shader draws full density this far inside the face and none at it; the
// margin covers the rounding of the hardware's bilinear weights.
static constexpr float NEO_VOLSMOKE_WALL_MARGIN = 0.25f;

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

// Below the knee the cloud darkens with its surroundings, as a grey wall
// would, instead of keeping the legacy color's floor. The floor keeps a
// cloud in a black room faintly visible, so a throw stays readable.
static constexpr float NEO_VOLSMOKE_DARK_KNEE = 0.15f;
static constexpr float NEO_VOLSMOKE_DARK_FLOOR = 0.03f;
static const Vector NEO_VOLSMOKE_LUMINANCE( 0.2126f, 0.7152f, 0.0722f ); // Rec. 709 weights

// Shade from a top-down light sweep: the top keeps full color, fully shaded
// smoke keeps the ambient share, so nothing goes black. The legacy sprites
// shade their base only a little.
static constexpr float NEO_VOLSMOKE_AMBIENT_SHARE = 0.8f;
static constexpr float NEO_VOLSMOKE_SWEEP_EXTINCTION = 0.012f;

// Light-grid samples: the grenade and four points at half the dome radius.
static constexpr float NEO_VOLSMOKE_LIGHT_SAMPLE_UP = 32.0f;
static constexpr float NEO_VOLSMOKE_LIGHT_SAMPLE_OUT = 0.5f * CNeoSmokeFill::DOME_RADIUS;

// The inside overlay averages the density over a cube of samples around the
// camera, so it ramps in over about the cube's size while walking in, as the
// legacy overlay ramps with the distance to the grenade.
static constexpr float NEO_VOLSMOKE_OVERLAY_REACH = 64.0f;
static constexpr int NEO_VOLSMOKE_OVERLAY_SIDE = 3; // samples per axis

// Average density at which the overlay covers the view completely. Below the
// smallest cell variation, so the core hides the viewmodel as legacy does.
static constexpr float NEO_VOLSMOKE_OVERLAY_FULL = 0.5f;

// Dynamic lights (muzzle flashes, explosions) light the smoke as they light
// the legacy particles, fading to nothing a particle radius past the light's
// own radius. The shader adds the light in linear, after the darkness factor,
// so a flash reads stronger in a dark cloud than in a lit one; the linear
// scale matches the legacy flash on a cloud in a lit room. The shader takes
// the brightest few that reach the smoke.
static constexpr float NEO_VOLSMOKE_DLIGHT_SHARE = 0.1f;
static constexpr float NEO_VOLSMOKE_DLIGHT_LINEAR = 1.9f;
static constexpr float NEO_VOLSMOKE_DLIGHT_MARGIN = SMOKEPARTICLE_SIZE;
static constexpr int NEO_VOLSMOKE_MAX_DLIGHTS = 2;

// A muzzle flash lights only the smoke around the gun: hot at the muzzle and
// gone a little inside the range the legacy flash visibly lit its cloud, so
// it reads as a flash rather than a lit cloud. The gain makes up for the
// smaller lit area at the core.
static constexpr float NEO_VOLSMOKE_FLASH_REACH = 96.0f;
static constexpr float NEO_VOLSMOKE_FLASH_GAIN = 2.0f;

static bool IsMuzzleFlashLight( const dlight_t *pLight )
{
	return pLight->key >= LIGHT_INDEX_MUZZLEFLASH && pLight->key < LIGHT_INDEX_MUZZLEFLASH + MAX_EDICTS;
}

// A muzzle flash is far brighter than the display range, so it shows through
// smoke that hides a surface: it sees this share of the smoke's optical depth.
// The flash point and the firing player's silhouette draw after the volume,
// scaled by what gets through, so they fade with depth into the cloud instead
// of switching at a plane as the legacy draw order does.
static constexpr float NEO_VOLSMOKE_FLASH_DEPTH_SHARE = 0.15f;
// Below this share a flash no longer shows, as the march stops at a small
// transmittance: what the smoke reveals fades to nothing before the far side.
static constexpr float NEO_VOLSMOKE_FLASH_CUTOFF = 0.05f;
static constexpr float NEO_VOLSMOKE_FLASH_LIFE = 0.05f; // the weapon's muzzle light
// The flash point fades smoothly with the smoke between it and the eye:
// a clear point in the near half, dimmer in the far half. An unsuppressed
// shot must stay findable from outside the cloud, so the point keeps a
// floor through about one cloud of smoke and fades out over the depth of
// a path through the whole cloud: a shooter in clear air behind the cloud
// stays hidden.
static constexpr float NEO_VOLSMOKE_FLASH_POINT_DEPTH = 16.0f;
static constexpr float NEO_VOLSMOKE_FLASH_POINT_FALLOFF = 2.0f;
static constexpr float NEO_VOLSMOKE_FLASH_POINT_FLOOR = 0.45f;
static constexpr float NEO_VOLSMOKE_FLASH_POINT_GONE = 22.0f;
static constexpr float NEO_VOLSMOKE_FLASH_POINT_RADIUS = 12.0f;
// The muzzle flash sprite's orange, as the legacy sprite shows it.
static const Vector NEO_VOLSMOKE_FLASH_POINT_TINT( 1.0f, 0.84f, 0.5f );
static constexpr float NEO_VOLSMOKE_FLASH_MIN = 1.0f / 255.0f;
static constexpr float NEO_VOLSMOKE_PATH_STEP = 16.0f;

// Silhouette strength of a player the smoke hides: while a muzzle flash
// lights them, and for a cloaked body the smoke flows around.
static constexpr float NEO_VOLSMOKE_FIRE_SILHOUETTE = 0.15f;
static constexpr float NEO_VOLSMOKE_CLOAK_SILHOUETTE = 0.15f;

static int VolSmokePow2( int n )
{
	int p = 1;
	while ( p < n )
	{
		p <<= 1;
	}

	return p;
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
	VOLSMOKE_PERF_CULL,
	VOLSMOKE_PERF_STAGES,
};

static const char *const s_VolSmokePerfNames[VOLSMOKE_PERF_STAGES] = { "fill", "density", "sweep", "pack", "download", "cull" };

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
// The fill's world queries (shared with the server), plus what only the
// client draws: the walls beside the gas.
// ---------------------------------------------------------------------------
class CNeoVolSmokeWorld final : public CNeoSmokeWorld
{
public:
	CNeoVolSmokeWorld( const Vector &gridMin, const int boxSize[3], const int seedCorner[3], const int latticeLo[3], const int latticeHi[3] )
		: CNeoSmokeWorld( gridMin, boxSize, seedCorner, latticeLo, latticeHi )
	{
	}

	// The client's box, in query coordinates, for LinkWall.
	void SetDrawBox( const int offset[3], const int size[3] )
	{
		for ( int a = 0; a < 3; ++a )
		{
			m_nDrawOffset[a] = offset[a];
			m_nBoxSize[a] = size[a];
		}
	}

	// The world face that blocks the way from an open cell of the client's
	// box to its neighbor dn, as a plane whose normal points back at the cell.
	// Not part of the fill.
	bool LinkWall( const int c[3], const int dn[3], Vector &normal, float &planeDist )
	{
		if ( !InBox( dn[0], dn[1], dn[2] ) )
		{
			return false;
		}

		const int x = c[0] + m_nDrawOffset[0];
		const int y = c[1] + m_nDrawOffset[1];
		const int z = c[2] + m_nDrawOffset[2];
		const int n[3] = { dn[0] + m_nDrawOffset[0], dn[1] + m_nDrawOffset[1], dn[2] + m_nDrawOffset[2] };
		if ( InEmptyBlock( x, y, z ) && InEmptyBlock( n[0], n[1], n[2] ) )
		{
			return false;
		}

		const double start = Plat_FloatTime();
		++m_nTraces;
		Ray_t ray;
		ray.Init( Center( x, y, z ), Center( n[0], n[1], n[2] ) );
		CTraceFilterWorldOnly filter;
		trace_t tr;
		enginetrace->TraceRay( ray, NEO_VOLSMOKE_FILL_MASK, &filter, &tr );
		m_flQuerySeconds += Plat_FloatTime() - start;
		if ( tr.startsolid || tr.fraction >= 1.0f )
		{
			return false;
		}

		// Smoke shows through glass and grates, so they do not end it. A water
		// surface does, though it may be translucent: no gas is under it.
		const bool water = ( tr.contents & NEO_VOLSMOKE_WATER_MASK ) != 0;
		if ( !water && ( ( tr.contents & ( CONTENTS_WINDOW | CONTENTS_GRATE ) ) || ( tr.surface.flags & SURF_TRANS ) ) )
		{
			return false;
		}

		normal = tr.plane.normal;
		planeDist = DotProduct( normal, tr.endpos );
		return true;
	}

	void PrintAudit( int volumeId ) const
	{
		for ( const NeoSmokeAuditCell_t &cell : m_AuditCells )
		{
			const Vector pos = Center( cell.nCell[0], cell.nCell[1], cell.nCell[2] );
			Warning( "neo_volsmoke_fill_audit: volume %d block says open, %s says solid at cell (%d %d %d) axis %d, world (%.1f %.1f %.1f)\n",
				volumeId, cell.nAxis < 0 ? "point" : "segment", cell.nCell[0], cell.nCell[1], cell.nCell[2], cell.nAxis,
				pos.x, pos.y, pos.z );
		}
	}

private:
	virtual bool Auditing() const override { return neo_volsmoke_fill_audit.GetBool(); }
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
	int MemoryBytes() const { return m_Bits.Count(); }
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
// Look noise texture, shared by all volumes: random values on a lattice that
// wraps, R one layer and G the layer above it, which the shader finds
// NEO_VOLSMOKE_NOISE_SHIFT texels away. Must match NOISE_SIZE and
// NOISE_Z_SHIFT in neo_volsmoke_ps30.fxc.
// ---------------------------------------------------------------------------
static constexpr int NEO_VOLSMOKE_NOISE_SIZE = 64;
static constexpr int NEO_VOLSMOKE_NOISE_SHIFT_X = 37;
static constexpr int NEO_VOLSMOKE_NOISE_SHIFT_Y = 17;

// Integer hash (lowbias32): the same values on every client.
static unsigned int VolSmokeNoiseHash( unsigned int x )
{
	x ^= x >> 16;
	x *= 0x7feb352dU;
	x ^= x >> 15;
	x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}

static unsigned char VolSmokeNoiseValue( int x, int y )
{
	const int size = NEO_VOLSMOKE_NOISE_SIZE;
	const int wx = ( ( x % size ) + size ) % size;
	const int wy = ( ( y % size ) + size ) % size;
	return (unsigned char)( VolSmokeNoiseHash( (unsigned int)( wy * size + wx ) ) >> 24 );
}

class CNeoVolSmokeNoiseRegenerator final : public ITextureRegenerator
{
public:
	virtual void RegenerateTextureBits( ITexture *pTexture, IVTFTexture *pVTFTexture, Rect_t *pRect ) override
	{
		const int size = NEO_VOLSMOKE_NOISE_SIZE;
		if ( pVTFTexture->Width() != size || pVTFTexture->Height() != size )
		{
			return;
		}

		CPixelWriter writer;
		writer.SetPixelMemory( pVTFTexture->Format(), pVTFTexture->ImageData( 0, 0, 0 ), pVTFTexture->RowSizeInBytes( 0 ) );
		for ( int y = 0; y < size; ++y )
		{
			writer.Seek( 0, y );
			for ( int x = 0; x < size; ++x )
			{
				const unsigned char layer = VolSmokeNoiseValue( x, y );
				const unsigned char above = VolSmokeNoiseValue( x + NEO_VOLSMOKE_NOISE_SHIFT_X, y + NEO_VOLSMOKE_NOISE_SHIFT_Y );
				writer.WritePixel( layer, above, 0, 255 );
			}
		}
	}

	virtual void Release() override {}
};

static CNeoVolSmokeNoiseRegenerator s_VolSmokeNoiseRegen;
static CTextureReference s_VolSmokeNoise;
static const char *const NEO_VOLSMOKE_NOISE_NAME = "_neo_volsmoke_noise";

static bool VolSmokeNoiseInit()
{
	if ( s_VolSmokeNoise.IsValid() )
	{
		return true;
	}

	s_VolSmokeNoise.InitProceduralTexture( NEO_VOLSMOKE_NOISE_NAME, TEXTURE_GROUP_OTHER, NEO_VOLSMOKE_NOISE_SIZE, NEO_VOLSMOKE_NOISE_SIZE,
		IMAGE_FORMAT_BGRA8888, TEXTUREFLAGS_NOMIP | TEXTUREFLAGS_NOLOD | TEXTUREFLAGS_PROCEDURAL | TEXTUREFLAGS_SINGLECOPY );
	if ( !s_VolSmokeNoise.IsValid() || IsErrorTexture( s_VolSmokeNoise ) )
	{
		Warning( "C_NeoVolSmoke: failed to create procedural texture %s\n", NEO_VOLSMOKE_NOISE_NAME );
		s_VolSmokeNoise.Shutdown( true );
		return false;
	}

	s_VolSmokeNoise->SetTextureRegenerator( &s_VolSmokeNoiseRegen );
	s_VolSmokeNoise->Download();
	return true;
}

static void VolSmokeNoiseShutdown()
{
	if ( !s_VolSmokeNoise.IsValid() )
	{
		return;
	}

	s_VolSmokeNoise->SetTextureRegenerator( NULL );
	s_VolSmokeNoise.Shutdown( true );
}

// ---------------------------------------------------------------------------
// Retired GPU resources
// ---------------------------------------------------------------------------
// The queued material system can still hold a frame that draws a removed
// volume. While it does, its materials stay referenced, DeleteIfUnreferenced
// does nothing, and they leak until the material system shuts down, which
// can crash the client at quit. The volume therefore hands its materials and
// atlas over here, and they are deleted once the render thread is past them.
static constexpr int NEO_VOLSMOKE_RETIRE_FRAMES = 3;

struct VolSmokeRetired_t
{
	IMaterial *pMaterials[2];
	ITexture  *pAtlas;
	int        nFrame;
};

static CUtlVector< VolSmokeRetired_t > s_VolSmokeRetired;

static void VolSmokeDeleteRetired( const VolSmokeRetired_t &retired )
{
	// The materials reference the atlas, so they go first.
	for ( IMaterial *pMat : retired.pMaterials )
	{
		if ( pMat )
		{
			pMat->DecrementReferenceCount();
			pMat->DeleteIfUnreferenced();
		}
	}

	if ( retired.pAtlas )
	{
		retired.pAtlas->SetTextureRegenerator( NULL ); // calls Release() on the regenerator
		retired.pAtlas->DecrementReferenceCount();
		retired.pAtlas->DeleteIfUnreferenced();
	}
}

enum VolSmokeRetireWait_t
{
	VOLSMOKE_RETIRE_AFTER_FRAMES,
	VOLSMOKE_RETIRE_NOW,
};

static void VolSmokeDeleteRetired( VolSmokeRetireWait_t wait )
{
	for ( int i = s_VolSmokeRetired.Count() - 1; i >= 0; --i )
	{
		if ( wait == VOLSMOKE_RETIRE_AFTER_FRAMES
			&& gpGlobals->framecount - s_VolSmokeRetired[i].nFrame < NEO_VOLSMOKE_RETIRE_FRAMES )
		{
			continue;
		}

		VolSmokeDeleteRetired( s_VolSmokeRetired[i] );
		s_VolSmokeRetired.Remove( i );
	}
}

// Deletes retired resources each frame, and all of them when the level or the
// client shuts down (no frame is queued then).
class CNeoVolSmokeRetireSystem final : public CAutoGameSystemPerFrame
{
public:
	CNeoVolSmokeRetireSystem() : CAutoGameSystemPerFrame( "CNeoVolSmokeRetireSystem" ) {}

	virtual void Update( float frametime ) override
	{
		C_NeoVolSmoke::RemoveExpired();
		VolSmokeDeleteRetired( VOLSMOKE_RETIRE_AFTER_FRAMES );
	}

	// The fill jobs trace the map, so they stop before it unloads. The level
	// init stop guards a path that skips the client level shutdown.
	virtual void LevelInitPreEntity() override { C_NeoVolSmoke::StopAllFills(); }
	virtual void LevelShutdownPreEntity() override { C_NeoVolSmoke::StopAllFills(); }
	virtual void LevelShutdownPostEntity() override { VolSmokeDeleteRetired( VOLSMOKE_RETIRE_NOW ); }
	virtual void Shutdown() override
	{
		C_NeoVolSmoke::StopAllFills();
		NEOSmokeFillPoolShutdown();

		// The retired materials reference the noise texture.
		VolSmokeDeleteRetired( VOLSMOKE_RETIRE_NOW );
		VolSmokeNoiseShutdown();
	}
};

static CNeoVolSmokeRetireSystem s_VolSmokeRetireSystem;

// ---------------------------------------------------------------------------
// Statics
// ---------------------------------------------------------------------------
CUtlVector< C_NeoVolSmoke * > C_NeoVolSmoke::s_Volumes;
int C_NeoVolSmoke::s_nNextId = 0;

// ---------------------------------------------------------------------------
C_NeoVolSmoke::C_NeoVolSmoke()
	: m_nId( s_nNextId++ ), m_bHasGrenade( false ), m_flSpawnTime( 0 ), m_pWorld( NULL ), m_pFillJob( NULL ), m_bFillDone( false ), m_flFillSeconds( 0 ), m_flFillPeakFrame( 0 ), m_flFillDoneAge( -1.0f ), m_nLagFrames( 0 ),
	  m_nBounded( 0 ), m_pRegen( NULL ), m_nPacked( 0 ), m_flNextUpload( 0 ), m_flRegrowEnd( -1.0f ), m_flUploadedFront( 0 ), m_bUploadPending( false ), m_nSlabTiles( 0 ), m_nSlabTilesCulled( 0 ), m_flDistRange( NEO_VOLSMOKE_DIST_RANGE ), m_nAtlasCols( 0 ), m_nAtlasW( 0 ), m_nAtlasH( 0 )
{
	for ( int a = 0; a < 3; ++a )
	{
		m_nBox[a] = 0;
		m_nSeedCorner[a] = 0;
		m_nBoxOffset[a] = 0;
	}

	m_bFitted = false;
}

C_NeoVolSmoke::~C_NeoVolSmoke()
{
	StopFill();
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

C_NeoVolSmoke *C_NeoVolSmoke::CreateForGrenade( const Vector &origin, float spawnTime, C_BaseEntity *pGrenade )
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
	if ( pVol )
	{
		pVol->m_hGrenade = pGrenade;
		pVol->m_bHasGrenade = true;
	}

	DevMsg( "neo_volsmoke: grenade at (%.9g %.9g %.9g) spawned volume %d, age %.2f\n",
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

void C_NeoVolSmoke::RemoveExpired()
{
	const float now = gpGlobals->curtime;
	for ( int i = s_Volumes.Count() - 1; i >= 0; --i )
	{
		const C_NeoVolSmoke *pVol = s_Volumes[i];
		const bool grenadeGone = pVol->m_bHasGrenade && !pVol->m_hGrenade.Get();
		if ( grenadeGone || now - pVol->m_flSpawnTime > NEO_VOLSMOKE_FADE_END )
		{
			s_Volumes[i]->Release();
		}
	}
}

// ---------------------------------------------------------------------------
void C_NeoVolSmoke::Init( const Vector &origin, float spawnTime )
{
	m_vecSpawn    = origin;
	m_vecSmokeMin = origin;
	m_vecSmokeMax = origin;
	m_flSpawnTime = spawnTime;

	// The box hangs from the cell corner the grenade sits on.
	int *seedCorner = m_nSeedCorner;
	m_Fill.BoxSize( m_nBox );
	m_Fill.BoxSeedCorner( seedCorner );
	const float cell = CNeoSmokeFill::CELL_SIZE;
	for ( int a = 0; a < 3; ++a )
	{
		m_vecGridMin[a] = origin[a] - seedCorner[a] * cell;
		m_vecGridMax[a] = origin[a] + ( m_nBox[a] - seedCorner[a] ) * cell;
	}

	SetAbsOrigin( origin );
	SetCollisionBounds( m_vecGridMin - origin, m_vecGridMax - origin );

	for ( int a = 0; a < 3; ++a )
	{
		m_SmokeLo[a] = m_nBox[a];
		m_SmokeHi[a] = -1;
	}

	int latticeLo[3];
	int latticeHi[3];
	m_Fill.LatticeBounds( latticeLo, latticeHi );
	m_pWorld = new CNeoVolSmokeWorld( m_vecGridMin, m_nBox, seedCorner, latticeLo, latticeHi );
	m_Fill.Start( m_pWorld );

	m_vecQueryMin = m_vecGridMin;
	m_vecColor.Init();
}

// A cell's center, from the corner of the box the fill queries in: the same
// point whatever box the volume is drawn in.
Vector C_NeoVolSmoke::CellCenter( const int c[3] ) const
{
	const float cell = CNeoSmokeFill::CELL_SIZE;
	return m_vecQueryMin + Vector( ( c[0] + m_nBoxOffset[0] + 0.5f ) * cell, ( c[1] + m_nBoxOffset[1] + 0.5f ) * cell,
		( c[2] + m_nBoxOffset[2] + 0.5f ) * cell );
}

// Once the fill is done: the box, the bounds and the atlas fitted to the field.
// Nothing is drawn before, because the atlas layout depends on the box.
void C_NeoVolSmoke::FitToField()
{
	m_bFitted = true;
	m_Fill.BoxSize( m_nBox );
	m_Fill.BoxSeedCorner( m_nSeedCorner );
	m_Fill.BoxOffset( m_nBoxOffset );
	const float cell = CNeoSmokeFill::CELL_SIZE;
	for ( int a = 0; a < 3; ++a )
	{
		m_vecGridMin[a] = m_vecSpawn[a] - m_nSeedCorner[a] * cell;
		m_vecGridMax[a] = m_vecSpawn[a] + ( m_nBox[a] - m_nSeedCorner[a] ) * cell;
		m_SmokeLo[a] = m_nBox[a];
		m_SmokeHi[a] = -1;
	}

	SetCollisionBounds( m_vecGridMin - m_vecSpawn, m_vecGridMax - m_vecSpawn );
	m_pWorld->SetDrawBox( m_nBoxOffset, m_nBox );
	m_flDistRange = MAX( NEO_VOLSMOKE_DIST_RANGE, CNeoSmokeFill::DistToUnits( m_Fill.FrontDist() ) );

	m_CarveTime.SetCount( m_Fill.FinalizedCount() );
	for ( int k = 0; k < m_CarveTime.Count(); ++k )
	{
		m_CarveTime[k] = -1.0f;
	}

	// Nothing draws before the fit, and from here the light samples can be
	// kept to where the gas went.
	m_vecColor = SampleColor();
	CreateAtlas();
}

void C_NeoVolSmoke::CreateAtlas()
{
	// As many tiles to a row as fit the atlas size; both sides a power of two.
	m_nAtlasCols = MAX( 1, CNeoSmokeFill::ATLAS_SIZE / m_nBox[0] );
	const int rows = ( m_nBox[2] + m_nAtlasCols - 1 ) / m_nAtlasCols;
	m_nAtlasW = VolSmokePow2( m_nAtlasCols * m_nBox[0] );
	m_nAtlasH = VolSmokePow2( rows * m_nBox[1] );
	const int atlasW = m_nAtlasW;
	const int atlasH = m_nAtlasH;

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

	if ( !VolSmokeNoiseInit() )
	{
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
	pVMT->SetString( "$noisetexture", NEO_VOLSMOKE_NOISE_NAME );
	m_Material.Init( matName, pVMT );

	Q_snprintf( matName, sizeof( matName ), "__neo_volsmoke_%d_slab", m_nId );
	pVMT = new KeyValues( "Neo_VolSmoke" );
	pVMT->SetString( "$basetexture", texName );
	pVMT->SetString( "$noisetexture", NEO_VOLSMOKE_NOISE_NAME );
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
	// Hand this volume's references over to the retire list.
	VolSmokeRetired_t retired = {};
	retired.nFrame = gpGlobals->framecount;

	CMaterialReference *pMaterials[] = { &m_Material, &m_SlabMaterial };
	for ( int i = 0; i < ARRAYSIZE( pMaterials ); ++i )
	{
		IMaterial *pMat = *pMaterials[i];
		if ( pMat )
		{
			pMat->IncrementReferenceCount();
		}
		retired.pMaterials[i] = pMat;
		pMaterials[i]->Shutdown();
	}

	if ( m_Atlas.IsValid() )
	{
		retired.pAtlas = m_Atlas;
		retired.pAtlas->IncrementReferenceCount();
		m_Atlas.Shutdown( false );
	}
	m_pRegen = NULL; // the atlas owns it

	if ( retired.pMaterials[0] || retired.pMaterials[1] || retired.pAtlas )
	{
		s_VolSmokeRetired.AddToTail( retired );
	}
}

// ---------------------------------------------------------------------------
// Simulation
// ---------------------------------------------------------------------------
Vector C_NeoVolSmoke::SampleColor() const
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
		Vector pos = m_vecSpawn + offsets[i] + Vector( 0, 0, NEO_VOLSMOKE_LIGHT_SAMPLE_UP );
		if ( i == 0 && CNeoVolSmokeWorld::PointSolid( pos ) )
		{
			pos = m_vecSpawn;
		}

		// Only where the gas went: a point past a thin wall sees the light
		// of a room the smoke never reaches.
		if ( i > 0 && ( CNeoVolSmokeWorld::PointSolid( pos ) || !GasReached( pos ) ) )
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

	const Vector clamped( clamp( light.x, 0.0f, 1.0f ), clamp( light.y, 0.0f, 1.0f ), clamp( light.z, 0.0f, 1.0f ) );
	const float luminance = clamped.Dot( NEO_VOLSMOKE_LUMINANCE );
	const float dark = clamp( ( luminance + NEO_VOLSMOKE_DARK_FLOOR ) / NEO_VOLSMOKE_DARK_KNEE, 0.0f, 1.0f );
	if ( dark < 1.0f )
	{
		color *= dark;
	}

	return color;
}

// Whether the fill reached the cell holding the point.
bool C_NeoVolSmoke::GasReached( const Vector &pos ) const
{
	const Vector g = ( pos - m_vecGridMin ) / CNeoSmokeFill::CELL_SIZE;
	const int c[3] = { (int)floorf( g.x ), (int)floorf( g.y ), (int)floorf( g.z ) };
	return InBox( c[0], c[1], c[2] ) && m_Fill.IsFinalized( CellIndex( c[0], c[1], c[2] ) );
}

// Picks up finished fill jobs and starts new ones, once a frame. One job per
// worker, the oldest fill first so no volume waits without bound, and among
// volumes of one age the nearest to the camera first.
void C_NeoVolSmoke::StepFills()
{
	static int s_nFillFrame = -1;
	if ( s_nFillFrame == gpGlobals->framecount )
	{
		return;
	}

	s_nFillFrame = gpGlobals->framecount;

	IThreadPool *pPool = NEOSmokeFillPool( NEO_VOLSMOKE_FILL_MAX_WORKERS );
	int running = 0;
	for ( C_NeoVolSmoke *pVol : s_Volumes )
	{
		if ( !pVol->m_pFillJob )
		{
			continue;
		}

		if ( !pVol->m_pFillJob->IsFinished() )
		{
			++running;
			continue;
		}

		pVol->m_pFillJob->Release();
		pVol->m_pFillJob = NULL;
		pVol->m_bFillDone = pVol->m_Fill.IsDone();
		pVol->m_flFillDoneAge = gpGlobals->curtime - pVol->m_flSpawnTime;
	}

	const int slots = pPool ? pPool->NumThreads() : 1;
	const Vector eye = MainViewOrigin();
	while ( running < slots )
	{
		float oldest = gpGlobals->curtime;
		for ( const C_NeoVolSmoke *pVol : s_Volumes )
		{
			if ( pVol->WantsFill() )
			{
				oldest = MIN( oldest, pVol->m_flSpawnTime );
			}
		}

		C_NeoVolSmoke *pNearest = NULL;
		float nearestSqr = 0.0f;
		for ( C_NeoVolSmoke *pVol : s_Volumes )
		{
			if ( !pVol->WantsFill() || pVol->m_flSpawnTime > oldest + NEO_VOLSMOKE_FILL_SAME_AGE )
			{
				continue;
			}

			const float distSqr = pVol->m_vecSpawn.DistToSqr( eye );
			if ( !pNearest || distSqr < nearestSqr )
			{
				pNearest = pVol;
				nearestSqr = distSqr;
			}
		}

		if ( !pNearest )
		{
			break;
		}

		if ( !pPool )
		{
			pNearest->StepFill( NEO_VOLSMOKE_FILL_BUDGET );
			break;
		}

		pNearest->m_pFillJob = pPool->QueueCall( pNearest, &C_NeoVolSmoke::RunFill );
		++running;
	}
}

// Works on this volume's fill on the main thread for up to `budget` seconds.
// Returns the time spent.
double C_NeoVolSmoke::StepFill( double budget )
{
	const double start = Plat_FloatTime();
	double spent = 0.0;
	while ( !m_Fill.IsDone() && spent < budget )
	{
		m_Fill.Step( NEO_SMOKE_FILL_CHUNK_CELLS, NEO_SMOKE_FILL_CHUNK_QUERIES );
		spent = Plat_FloatTime() - start;
	}

	m_flFillSeconds += spent;
	m_flFillPeakFrame = MAX( m_flFillPeakFrame, spent );
	if ( m_Fill.IsDone() )
	{
		m_bFillDone = true;
		m_flFillDoneAge = gpGlobals->curtime - m_flSpawnTime;
	}

	return spent;
}

// The fill job, on a worker. It touches only the fill, its world and the
// fill time; the chunks let StopFill cancel it quickly.
void C_NeoVolSmoke::RunFill()
{
	const double start = Plat_FloatTime();
	NEOSmokeFillRun( m_Fill, m_nFillCancel );
	m_flFillSeconds += Plat_FloatTime() - start;
}

// Cancels this volume's fill job and waits for it, so the fill, its world and
// the map outlive it. A cancelled fill is never shown.
void C_NeoVolSmoke::StopFill()
{
	if ( !m_pFillJob )
	{
		return;
	}

	m_nFillCancel = 1;
	NEOSmokeFillJoin( m_pFillJob );
}

void C_NeoVolSmoke::StopAllFills()
{
	for ( C_NeoVolSmoke *pVol : s_Volumes )
	{
		pVol->StopFill();
	}
}

bool C_NeoVolSmoke::IsRegrowing( float now ) const
{
	return now < m_flRegrowEnd + 1.0f / NEO_VOLSMOKE_UPLOAD_HZ;
}

// Has the atlas new cells or carve weights to take, not only new light?
bool C_NeoVolSmoke::ShapeChanged( float now ) const
{
	return m_nPacked < m_Fill.FinalizedCount() || IsRegrowing( now );
}

// The volume whose atlas upload has waited longest, a changed shape before a
// light change. NULL when none waited at the start of the frame.
C_NeoVolSmoke *C_NeoVolSmoke::UploadTurn()
{
	static int s_nTurnFrame = -1;
	static C_NeoVolSmoke *s_pTurn = NULL;
	if ( s_nTurnFrame == gpGlobals->framecount )
	{
		return s_pTurn;
	}

	s_nTurnFrame = gpGlobals->framecount;
	s_pTurn = NULL;
	for ( int i = 0; i < s_Volumes.Count(); ++i )
	{
		C_NeoVolSmoke *pVol = s_Volumes[i];
		if ( !pVol->m_bUploadPending )
		{
			continue;
		}

		const float now = gpGlobals->curtime;
		if ( s_pTurn && s_pTurn->ShapeChanged( now ) && !pVol->ShapeChanged( now ) )
		{
			continue;
		}

		if ( !s_pTurn || ( pVol->ShapeChanged( now ) && !s_pTurn->ShapeChanged( now ) ) || pVol->m_flNextUpload < s_pTurn->m_flNextUpload )
		{
			s_pTurn = pVol;
		}
	}

	return s_pTurn;
}

// Distance of the expanding front from the grenade, along the gas path.
// Never past the cells the shader has: a volume whose fill or upload is late
// grows as far as it has filled instead of showing a cut-off shape.
float C_NeoVolSmoke::FrontRadius( float age ) const
{
	return MIN( BloomRadius( age ), m_flUploadedFront );
}

// The front by the bloom curve alone.
float C_NeoVolSmoke::BloomRadius( float age ) const
{
	const float bloom = VolSmokeBloom( age );
	if ( FillDone() )
	{
		return CNeoSmokeFill::DistToUnits( m_Fill.FrontDist() ) * bloom;
	}

	return NEO_VOLSMOKE_NOMINAL_RADIUS * bloom;
}

// Smoke density at a point: trilinear over the eight nearest cell centers,
// as the shader samples the atlas.
float C_NeoVolSmoke::DensityAt( const Vector &pos, float front, float now ) const
{
	const Vector g = ( pos - m_vecGridMin ) / CNeoSmokeFill::CELL_SIZE - Vector( 0.5f, 0.5f, 0.5f );
	const int base[3] = { (int)floorf( g.x ), (int)floorf( g.y ), (int)floorf( g.z ) };
	const float frac[3] = { g.x - base[0], g.y - base[1], g.z - base[2] };

	float density = 0.0f;
	for ( int corner = 0; corner < 8; ++corner )
	{
		int c[3];
		float w = 1.0f;
		for ( int a = 0; a < 3; ++a )
		{
			const int bit = ( corner >> a ) & 1;
			c[a] = base[a] + bit;
			w *= bit ? frac[a] : 1.0f - frac[a];
		}

		if ( !InBox( c[0], c[1], c[2] ) )
		{
			continue;
		}

		const int i = CellIndex( c[0], c[1], c[2] );
		if ( !m_Fill.IsFinalized( i ) )
		{
			continue;
		}

		const float edge = clamp( ( front - CNeoSmokeFill::DistToUnits( m_Fill.Dist( i ) ) ) / NEO_VOLSMOKE_EDGE, 0.0f, 1.0f );
		density += w * edge * edge * ( 3.0f - 2.0f * edge ) * CellWeight( i, now );
	}

	if ( density <= 0.0f || !m_pRegen )
	{
		return density;
	}

	// No smoke past a wall's face, as in the shader.
	float dist, weight, wallCut;
	SampleAtlas( pos, dist, weight, wallCut );
	return density * wallCut;
}

// From inside the smoke the march leaves the nearest units clear. Like the
// legacy grenade, add the camera's share to the fullscreen smoke overlay,
// which the HUD also reads to hide names and hints.
void C_NeoVolSmoke::AddInsideOverlay( float front, float now )
{
	const Vector eye = MainViewOrigin();
	const Vector reach( NEO_VOLSMOKE_OVERLAY_REACH, NEO_VOLSMOKE_OVERLAY_REACH, NEO_VOLSMOKE_OVERLAY_REACH );
	if ( !IsPointInBox( eye, m_vecSmokeMin - reach, m_vecSmokeMax + reach ) )
	{
		return;
	}

	const int side = NEO_VOLSMOKE_OVERLAY_SIDE;
	const float spacing = 2.0f * NEO_VOLSMOKE_OVERLAY_REACH / ( side - 1 );
	float sum = 0.0f;
	for ( int i = 0; i < side * side * side; ++i )
	{
		const Vector offset( i % side, ( i / side ) % side, i / ( side * side ) );
		sum += DensityAt( eye - reach + offset * spacing, front, now );
	}

	if ( sum <= 0.0f )
	{
		return;
	}

	const float age = now - m_flSpawnTime;
	const float density = sum / ( side * side * side );
	const float share = clamp( density / NEO_VOLSMOKE_OVERLAY_FULL, 0.0f, 1.0f );
	// The overlay is a display color: the cloud's own, so the view keeps
	// its brightness on walking in, dark in a dark room.
	Vector display;
	for ( int k = 0; k < 3; ++k )
	{
		display[k] = powf( m_vecColor[k], 1.0f / NEO_VOLSMOKE_DISPLAY_GAMMA );
	}

	AddSmokeFogOverlayColored( share * VolSmokeBloom( age ) * VolSmokeFade( age ), display );
}

// Density of a cell inside the front: its variation, times the carve hole
// while it refills.
float C_NeoVolSmoke::CellWeight( int idx, float now ) const
{
	float hole = 1.0f;
	const float carveTime = CarveTime( idx );
	if ( carveTime >= 0.0f )
	{
		hole = clamp( ( now - carveTime ) / neo_volsmoke_regrow.GetFloat(), 0.0f, 1.0f );
		hole = hole * hole; // slow start, then refill
	}

	int c[3];
	CellCoords( idx, c[0], c[1], c[2] );
	return VolSmokeCellVariation( c[0] - m_nSeedCorner[0], c[1] - m_nSeedCorner[1], c[2] - m_nSeedCorner[2] ) * hole;
}

static unsigned char VolSmokeByte( float v )
{
	return (unsigned char)( clamp( v, 0.0f, 1.0f ) * 255.0f + 0.5f );
}

static unsigned char VolSmokeWallByte( float distance )
{
	// Rounded down: a face is never placed past where it is.
	const float steps = floorf( distance * NEO_VOLSMOKE_WALL_STEPS_PER_UNIT ) + NEO_VOLSMOKE_WALL_ZERO_BYTE;
	return (unsigned char)clamp( steps, 0.0f, 255.0f );
}

// Smoke left at a point by the wall distance read from the atlas.
static float VolSmokeWallCut( float wallByte )
{
	const float distance = ( wallByte - NEO_VOLSMOKE_WALL_ZERO_BYTE ) / NEO_VOLSMOKE_WALL_STEPS_PER_UNIT;
	return clamp( distance - NEO_VOLSMOKE_WALL_MARGIN, 0.0f, 1.0f );
}

// Byte offset of a cell's texel: slice z lives in tile ( z % cols, z / cols ).
int C_NeoVolSmoke::AtlasOffset( int x, int y, int z ) const
{
	const int tileX = ( z % m_nAtlasCols ) * m_nBox[0];
	const int tileY = ( z / m_nAtlasCols ) * m_nBox[1];
	return ( ( tileY + y ) * m_nAtlasW + tileX + x ) * 4;
}

int C_NeoVolSmoke::AtlasOffset( int idx ) const
{
	int x, y, z;
	CellCoords( idx, x, y, z );
	return AtlasOffset( x, y, z );
}

void C_NeoVolSmoke::ClearAtlas()
{
	unsigned char *bits = m_pRegen->Bits();
	for ( int i = 0; i < m_nAtlasW * m_nAtlasH; ++i )
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
		const unsigned char dist = VolSmokeByte( CNeoSmokeFill::DistToUnits( m_Fill.Dist( i ) ) / m_flDistRange );
		unsigned char *texel = bits + AtlasOffset( i );
		texel[VOLSMOKE_BYTE_DIST] = dist;
		texel[VOLSMOKE_BYTE_WEIGHT] = VolSmokeByte( CellWeight( i, now ) );

		int c[3];
		CellCoords( i, c[0], c[1], c[2] );

		// The faces that block this cell's six links. A cell without gas beside
		// it takes its distance to them, so the lookup crosses zero at the face.
		Vector wallNormal[6];
		float wallDist[6];
		int walls = 0;
		for ( int link = 0; link < 6; ++link )
		{
			int n[3] = { c[0], c[1], c[2] };
			n[link >> 1] += ( link & 1 ) ? 1 : -1;
			if ( m_pWorld->LinkWall( c, n, wallNormal[walls], wallDist[walls] ) )
			{
				++walls;
			}
		}

		const float cell = CNeoSmokeFill::CELL_SIZE;
		const Vector center = CellCenter( c );
		float ownWall = NEO_VOLSMOKE_WALL_CAP;
		for ( int w = 0; w < walls; ++w )
		{
			ownWall = MIN( ownWall, DotProduct( wallNormal[w], center ) - wallDist[w] );
		}

		texel[VOLSMOKE_BYTE_ALPHA] = VolSmokeWallByte( ownWall );

		for ( int dz = -1; dz <= 1; ++dz )
		{
			for ( int dy = -1; dy <= 1; ++dy )
			{
				for ( int dx = -1; dx <= 1; ++dx )
				{
					if ( !InBox( c[0] + dx, c[1] + dy, c[2] + dz ) )
					{
						continue;
					}

					const int n = CellIndex( c[0] + dx, c[1] + dy, c[2] + dz );
					if ( m_Fill.IsFinalized( n ) )
					{
						continue;
					}

					unsigned char &nDist = bits[AtlasOffset( n ) + VOLSMOKE_BYTE_DIST];
					nDist = MIN( nDist, dist );
				}
			}
		}

		if ( walls == 0 )
		{
			continue;
		}

		const int reach = NEO_VOLSMOKE_WALL_REACH;
		for ( int dz = -reach; dz <= reach; ++dz )
		{
			for ( int dy = -reach; dy <= reach; ++dy )
			{
				for ( int dx = -reach; dx <= reach; ++dx )
				{
					if ( !InBox( c[0] + dx, c[1] + dy, c[2] + dz ) )
					{
						continue;
					}

					const int n = CellIndex( c[0] + dx, c[1] + dy, c[2] + dz );
					if ( m_Fill.IsFinalized( n ) )
					{
						continue;
					}

					float nWall = NEO_VOLSMOKE_WALL_CAP;
					const Vector nCenter = center + Vector( dx * cell, dy * cell, dz * cell );
					for ( int w = 0; w < walls; ++w )
					{
						nWall = MIN( nWall, DotProduct( wallNormal[w], nCenter ) - wallDist[w] );
					}

					// A cell past a face stays past it whatever the other gas cells
					// say. Among the faces it is past, the nearest one counts: at an
					// outside corner the farther face is not the one beside the gas.
					const unsigned char newByte = MIN( VolSmokeWallByte( nWall ), NEO_VOLSMOKE_WALL_UNSET - 1 );
					unsigned char &nWallByte = bits[AtlasOffset( n ) + VOLSMOKE_BYTE_ALPHA];
					const bool bothPast = nWallByte < NEO_VOLSMOKE_WALL_ZERO_BYTE && newByte < NEO_VOLSMOKE_WALL_ZERO_BYTE;
					if ( nWallByte == NEO_VOLSMOKE_WALL_UNSET )
					{
						nWallByte = newByte;
					}
					else if ( bothPast )
					{
						nWallByte = MAX( nWallByte, newByte );
					}
					else
					{
						nWallByte = MIN( nWallByte, newByte );
					}
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
		bits[AtlasOffset( i ) + VOLSMOKE_BYTE_WEIGHT] = VolSmokeByte( CellWeight( i, now ) );
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
		CellCoords( i, c[0], c[1], c[2] );
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

	// The render bounds follow the smoke, so the leaves it draws in do too.
	if ( GetRenderHandle() != INVALID_CLIENT_RENDER_HANDLE )
	{
		ClientLeafSystem()->RenderableChanged( GetRenderHandle() );
	}
}

// Top-down sweep: each cell gets the transmittance of the smoke above it.
// Only the columns and heights a lookup near the smoke can read are swept;
// the rest keep full light. The sweep sees every packed cell, not only
// those inside the bloom front: the light then changes with the shape
// alone, so a bloom costs an upload per fill stage, not one per frame.
void C_NeoVolSmoke::SweepLight( float now )
{
	const int count = m_Fill.FinalizedCount();
	if ( count == 0 )
	{
		return;
	}

	// The bounds of all finalized cells: the bloom bounds grow later
	// without a new sweep.
	static constexpr int MARGIN = 2;
	const int *size = m_nBox;
	int lo[3] = { size[0], size[1], size[2] };
	int hi[3] = { -1, -1, -1 };
	for ( int k = 0; k < count; ++k )
	{
		int c[3];
		CellCoords( m_Fill.FinalizedCell( k ), c[0], c[1], c[2] );
		for ( int a = 0; a < 3; ++a )
		{
			lo[a] = MIN( lo[a], c[a] );
			hi[a] = MAX( hi[a], c[a] );
		}
	}

	for ( int a = 0; a < 3; ++a )
	{
		lo[a] = MAX( 0, lo[a] - MARGIN );
		hi[a] = MIN( size[a] - 1, hi[a] + MARGIN );
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
				const int i = CellIndex( x, y, z );
				bits[AtlasOffset( i ) + VOLSMOKE_BYTE_LIGHT] = VolSmokeByte( T );
				if ( !m_Fill.IsFinalized( i ) )
				{
					continue;
				}

				T *= expf( -kStep * CellWeight( i, now ) );
			}
		}
	}
}

void C_NeoVolSmoke::Carve( const Vector &start, const Vector &end, float radius )
{
	// Before the field is fitted nothing is drawn, so there is nothing to carve.
	if ( !m_bFitted )
	{
		return;
	}

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
	int x0 = clamp( (int)floorf( ( cmin.x - m_vecGridMin.x ) / cell ), 0, m_nBox[0] - 1 );
	int y0 = clamp( (int)floorf( ( cmin.y - m_vecGridMin.y ) / cell ), 0, m_nBox[1] - 1 );
	int z0 = clamp( (int)floorf( ( cmin.z - m_vecGridMin.z ) / cell ), 0, m_nBox[2] - 1 );
	int x1 = clamp( (int)floorf( ( cmax.x - m_vecGridMin.x ) / cell ), 0, m_nBox[0] - 1 );
	int y1 = clamp( (int)floorf( ( cmax.y - m_vecGridMin.y ) / cell ), 0, m_nBox[1] - 1 );
	int z1 = clamp( (int)floorf( ( cmax.z - m_vecGridMin.z ) / cell ), 0, m_nBox[2] - 1 );

	bool touched = false;
	for ( int z = z0; z <= z1; ++z )
	{
		for ( int y = y0; y <= y1; ++y )
		{
			for ( int x = x0; x <= x1; ++x )
			{
				const int i = CellIndex( x, y, z );
				if ( !m_Fill.IsFinalized( i ) )
					continue;

				const Vector c = m_vecGridMin + Vector( ( x + 0.5f ) * cell, ( y + 0.5f ) * cell, ( z + 0.5f ) * cell );
				float t = 0.0f;
				if ( segLenSq > 1e-6f )
					t = clamp( DotProduct( c - start, seg ) / segLenSq, 0.0f, 1.0f );
				const Vector closest = start + seg * t;
				if ( ( c - closest ).LengthSqr() <= r2 )
				{
					m_CarveTime[m_Fill.FinalRank( i )] = now;
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
// Field and atlas hashes for neo_volsmoke_stats. Each is a sum over cells of a
// mix of the cell's place relative to the grenade and its value, so it does
// not depend on the box or the atlas layout: equal fields give equal hashes
// across builds. The key is the offline fill prototype's (VolSmokeCellKey).

unsigned long long C_NeoVolSmoke::ClientFieldHash() const
{
	unsigned long long hash = 0;
	for ( int i = 0; i < m_Fill.FinalizedCount(); ++i )
	{
		const int cell = m_Fill.FinalizedCell( i );
		int c[3];
		CellCoords( cell, c[0], c[1], c[2] );
		hash += VolSmokeCellKey( c[0] - m_nSeedCorner[0], c[1] - m_nSeedCorner[1], c[2] - m_nSeedCorner[2], (unsigned int)m_Fill.Dist( cell ) );
	}

	return hash;
}

unsigned long long C_NeoVolSmoke::LatticeFieldHash() const
{
	unsigned long long hash = 0;
	for ( int i = 0; i < m_Fill.LatticeCount(); ++i )
	{
		int c[3];
		m_Fill.LatticeCell( i, c[0], c[1], c[2] );
		for ( int a = 0; a < 3; ++a )
		{
			c[a] -= m_nSeedCorner[a] + m_nBoxOffset[a];
		}

		hash += VolSmokeCellKey( c[0], c[1], c[2], (unsigned int)m_Fill.LatticeDist( i ) );
	}

	return hash;
}

// Over the texels a lookup near the gas reads (the gas cells and one cell
// around them) that differ from a cleared one.
unsigned long long C_NeoVolSmoke::AtlasHash() const
{
	if ( !m_pRegen || m_Fill.FinalizedCount() == 0 )
	{
		return 0;
	}

	int lo[3] = { m_nBox[0], m_nBox[1], m_nBox[2] };
	int hi[3] = { -1, -1, -1 };
	for ( int k = 0; k < m_Fill.FinalizedCount(); ++k )
	{
		int c[3];
		CellCoords( m_Fill.FinalizedCell( k ), c[0], c[1], c[2] );
		for ( int a = 0; a < 3; ++a )
		{
			lo[a] = MIN( lo[a], c[a] );
			hi[a] = MAX( hi[a], c[a] );
		}
	}

	for ( int a = 0; a < 3; ++a )
	{
		lo[a] = MAX( 0, lo[a] - 1 );
		hi[a] = MIN( m_nBox[a] - 1, hi[a] + 1 );
	}

	const unsigned char *bits = m_pRegen->Bits();
	unsigned long long hash = 0;
	for ( int z = lo[2]; z <= hi[2]; ++z )
	{
		for ( int y = lo[1]; y <= hi[1]; ++y )
		{
			for ( int x = lo[0]; x <= hi[0]; ++x )
			{
				const unsigned char *t = bits + AtlasOffset( x, y, z );
				const unsigned int texel = (unsigned int)t[0] | ( (unsigned int)t[1] << 8 ) | ( (unsigned int)t[2] << 16 ) | ( (unsigned int)t[3] << 24 );
				const bool cleared = t[VOLSMOKE_BYTE_DIST] == 255 && t[VOLSMOKE_BYTE_LIGHT] == 255 && t[VOLSMOKE_BYTE_WEIGHT] == 0 && t[VOLSMOKE_BYTE_ALPHA] == 255;
				if ( cleared )
				{
					continue;
				}

				hash += VolSmokeCellKey( x - m_nSeedCorner[0], y - m_nSeedCorner[1], z - m_nSeedCorner[2], texel );
			}
		}
	}

	return hash;
}

// ---------------------------------------------------------------------------
void C_NeoVolSmoke::PrintStats() const
{
	if ( m_pFillJob )
	{
		Msg( "neo_volsmoke_stats: volume %d age %.2f done 0 (filling on a worker)\n", m_nId, gpGlobals->curtime - m_flSpawnTime );
		return;
	}

	// FNV-1a over the finalized cells and their path lengths: equal hashes mean
	// equal fields.
	unsigned int hash = 2166136261u;
	for ( int i = 0; i < m_Fill.FinalizedCount(); ++i )
	{
		const int cell = m_Fill.FinalizedCell( i );
		const unsigned int words[2] = { (unsigned int)cell, (unsigned int)m_Fill.Dist( cell ) };
		for ( unsigned int word : words )
		{
			hash = ( hash ^ word ) * 16777619u;
		}
	}

	const double queryMs = m_pWorld ? m_pWorld->QuerySeconds() * 1000.0 : 0.0;
	const int traces = m_pWorld ? m_pWorld->Traces() : 0;
	Msg( "neo_volsmoke_stats: volume %d age %.2f done %d (at age %.2f) cells %d radius %.1f queries %d traces %d fill %.1f ms (world %.1f ms, peak frame %.2f ms), hash %08x, front ahead of the fill in %d frames\n",
		m_nId, gpGlobals->curtime - m_flSpawnTime, FillDone() ? 1 : 0, m_flFillDoneAge, m_Fill.FinalizedCount(),
		CNeoSmokeFill::DistToUnits( m_Fill.FrontDist() ), m_Fill.WorldQueries(), traces,
		m_flFillSeconds * 1000.0, queryMs, m_flFillPeakFrame * 1000.0, hash, m_nLagFrames );
	const int fillBytes = m_Fill.MemoryBytes();
	const int carveBytes = m_CarveTime.Count() * (int)sizeof( float );
	const int worldBytes = m_pWorld ? m_pWorld->MemoryBytes() : 0;
	const int atlasBytes = m_pRegen ? m_pRegen->MemoryBytes() : 0;
	Msg( "neo_volsmoke_stats: volume %d field %016llx lattice %016llx (%d cells, front %.1f, dropped %d) atlas %016llx\n",
		m_nId, ClientFieldHash(), LatticeFieldHash(), m_Fill.LatticeCount(), CNeoSmokeFill::DistToUnits( m_Fill.LatticeFrontDist() ),
		m_Fill.DroppedCount(), AtlasHash() );
	Msg( "neo_volsmoke_stats: volume %d memory %d KB (fill %d KB, carve times %d KB, world cache %d KB, atlas copy %d KB)\n",
		m_nId, ( fillBytes + carveBytes + worldBytes + atlasBytes ) / 1024, fillBytes / 1024, carveBytes / 1024,
		worldBytes / 1024, atlasBytes / 1024 );
	Msg( "neo_volsmoke_stats: volume %d far slab tiles last frame %d, behind opaque smoke %d\n", m_nId, m_nSlabTiles, m_nSlabTilesCulled );
	if ( m_pWorld && m_pWorld->AuditChecks() > 0 )
	{
		Msg( "neo_volsmoke_stats: volume %d audit: %d block answers checked, %d disagreements\n",
			m_nId, m_pWorld->AuditChecks(), m_pWorld->AuditDisagreements() );
		if ( m_bFillDone )
		{
			m_pWorld->PrintAudit( m_nId );
		}
	}
}

void C_NeoVolSmoke::ClientThink()
{
	const float now = gpGlobals->curtime;

	// RemoveExpired releases the volume later in the frame.
	if ( now - m_flSpawnTime > NEO_VOLSMOKE_FADE_END )
	{
		return;
	}

	VolSmokePerfFrame();

	{
		CVolSmokePerfScope perf( VOLSMOKE_PERF_FILL );
		StepFills();
		if ( !m_bFitted )
		{
			if ( !FillDone() )
			{
				return;
			}

			FitToField();
		}
	}

	const float front = FrontRadius( now - m_flSpawnTime );
	if ( front < BloomRadius( now - m_flSpawnTime ) )
	{
		++m_nLagFrames;
	}

	{
		CVolSmokePerfScope perf( VOLSMOKE_PERF_DENSITY );
		UpdateBounds( front );
		AddInsideOverlay( front, now );
	}

	if ( !m_pRegen || !m_Atlas.IsValid() || now < m_flNextUpload )
	{
		return;
	}

	// A grown volume with no carve refilling changes nothing.
	const bool regrow = IsRegrowing( now );
	m_bUploadPending = m_bUploadPending || ShapeChanged( now );

	// One upload a frame across all volumes: an upload can stall the frame.
	// A volume packs and sweeps on its turn only, or many volumes would
	// each redo that work every frame while they wait.
	static int s_nUploadFrame = -1;
	C_NeoVolSmoke *pTurn = UploadTurn();
	if ( !m_bUploadPending || s_nUploadFrame == gpGlobals->framecount || ( pTurn && pTurn != this ) )
	{
		return;
	}

	{
		CVolSmokePerfScope perf( VOLSMOKE_PERF_PACK );
		PackNewCells( now );
		if ( regrow )
		{
			PackWeights( now );
		}
	}

	{
		CVolSmokePerfScope perf( VOLSMOKE_PERF_SWEEP );
		SweepLight( now );
	}

	CVolSmokePerfScope perf( VOLSMOKE_PERF_DOWNLOAD );
	m_Atlas->Download();
	++s_VolSmokePerf.uploads;
	s_nUploadFrame = gpGlobals->framecount;
	m_bUploadPending = false;
	m_flNextUpload = now + 1.0f / NEO_VOLSMOKE_UPLOAD_HZ;
	if ( m_nPacked > 0 )
	{
		m_flUploadedFront = CNeoSmokeFill::DistToUnits( m_Fill.Dist( m_Fill.FinalizedCell( m_nPacked - 1 ) ) );
	}
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
void C_NeoVolSmoke::GetRenderBounds( Vector &mins, Vector &maxs )
{
	// Before the first cell fills there is nothing to place.
	if ( m_vecSmokeMax.x <= m_vecSmokeMin.x )
	{
		mins = maxs = m_vecSpawn - GetRenderOrigin();
		return;
	}

	// The box that holds smoke, not the whole grid. The engine draws a
	// translucent renderable in the nearest leaf its bounds touch, after the
	// translucent world surfaces of that leaf and of every leaf behind it.
	// Tight bounds keep glass that stands in front of the smoke out of those
	// leaves, so it is drawn over the smoke and not under it.
	mins = m_vecSmokeMin - GetRenderOrigin();
	maxs = m_vecSmokeMax - GetRenderOrigin();
}

// A volume is one renderable that a player can stand inside. Sorted by its
// center, a cloaked player between the camera and the center draws after the
// smoke and its tint outlines it through the smoke in front of it. By the
// nearest corner, everything inside the box draws first and the smoke covers
// it, while a translucent in clear air in front of the box still draws over
// the smoke.
bool C_NeoVolSmoke::SortDepth( IClientRenderable *pRenderable, const Vector &viewOrigin, const Vector &viewForward, float &depth )
{
	IClientUnknown *pUnknown = pRenderable->GetIClientUnknown();
	C_BaseEntity *pEntity = pUnknown ? pUnknown->GetBaseEntity() : nullptr;
	if ( !pEntity )
	{
		return false;
	}

	const C_NeoVolSmoke *pVolume = nullptr;
	for ( const C_NeoVolSmoke *p : s_Volumes )
	{
		if ( p == pEntity )
		{
			pVolume = p;
			break;
		}
	}

	if ( !pVolume || !pVolume->m_bFitted || pVolume->m_vecSmokeMax.x <= pVolume->m_vecSmokeMin.x )
	{
		return false;
	}

	// The nearest corner along the view direction: per axis, the box side
	// that faces the camera. The box fitted to the whole field, not the one
	// growing with the bloom, so two volumes keep their order as they grow.
	Vector nearest;
	for ( int axis = 0; axis < 3; ++axis )
	{
		nearest[axis] = viewForward[axis] >= 0.0f ? pVolume->m_vecGridMin[axis] : pVolume->m_vecGridMax[axis];
	}

	depth = DotProduct( nearest - viewOrigin, viewForward );
	return true;
}

static void SetVec3Var( IMaterial *pMat, const char *name, const Vector &v )
{
	bool found = false;
	IMaterialVar *pVar = pMat->FindVar( name, &found, false );
	if ( found ) pVar->SetVecValue( v.x, v.y, v.z );
}

static void SetVec2Var( IMaterial *pMat, const char *name, float x, float y )
{
	bool found = false;
	IMaterialVar *pVar = pMat->FindVar( name, &found, false );
	if ( found ) pVar->SetVecValue( x, y );
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

// The legacy smoke scales its alpha by the bloom and the fade the same way.
float C_NeoVolSmoke::DrawExtinction() const
{
	const float age = gpGlobals->curtime - m_flSpawnTime;
	return NEO_VOLSMOKE_EXTINCTION * VolSmokeBloom( age ) * VolSmokeFade( age );
}

// The sphere about the gas seed that holds all smoke. Returns its radius.
float C_NeoVolSmoke::SmokeSphere( Vector &center ) const
{
	int c[3];
	CellCoords( m_Fill.FinalizedCell( 0 ), c[0], c[1], c[2] );
	center = CellCenter( c );
	return FrontRadius( gpGlobals->curtime - m_flSpawnTime ) + NEO_VOLSMOKE_SPHERE_MARGIN;
}

struct VolSmokeDLight_t
{
	Vector origin;
	float invReachSqr;
	Vector color;
	float score;
};

// The dynamic lights that reach the box, brightest at the box first. Returns
// how many were kept.
static int CollectDynamicLights( const Vector &boxMin, const Vector &boxMax, VolSmokeDLight_t out[NEO_VOLSMOKE_MAX_DLIGHTS] )
{
	dlight_t *lights[MAX_DLIGHTS];
	const int nLights = effects->CL_GetActiveDLights( lights );

	int nKept = 0;
	for ( int i = 0; i < nLights; ++i )
	{
		const dlight_t *pLight = lights[i];
		if ( pLight->color.exponent == 0 || ( pLight->color.r == 0 && pLight->color.g == 0 && pLight->color.b == 0 ) )
		{
			continue;
		}

		const bool muzzle = IsMuzzleFlashLight( pLight );
		const float reach = muzzle ? NEO_VOLSMOKE_FLASH_REACH : pLight->radius + NEO_VOLSMOKE_DLIGHT_MARGIN;
		const float distSqr = CalcSqrDistanceToAABB( boxMin, boxMax, pLight->origin );
		if ( distSqr >= reach * reach )
		{
			continue;
		}

		VolSmokeDLight_t light;
		ColorRGBExp32ToVector( pLight->color, light.color );

		// Legacy adds a share of the light and scales a channel past white
		// back down with the others; cap the light the same way, keeping
		// its tint, then add it in linear.
		light.color *= NEO_VOLSMOKE_DLIGHT_SHARE / 255.0f;
		const float brightest = MAX( light.color.x, MAX( light.color.y, light.color.z ) );
		if ( brightest > 1.0f )
		{
			light.color /= brightest;
		}

		light.color *= NEO_VOLSMOKE_DLIGHT_LINEAR * ( muzzle ? NEO_VOLSMOKE_FLASH_GAIN : 1.0f );
		light.origin = pLight->origin;
		light.invReachSqr = 1.0f / ( reach * reach );
		light.score = MAX( light.color.x, MAX( light.color.y, light.color.z ) ) * ( 1.0f - distSqr * light.invReachSqr );

		// Insert by score; a light weaker than all kept ones falls off the end.
		int slot = nKept;
		while ( slot > 0 && out[slot - 1].score < light.score )
		{
			if ( slot < NEO_VOLSMOKE_MAX_DLIGHTS )
			{
				out[slot] = out[slot - 1];
			}
			--slot;
		}

		if ( slot < NEO_VOLSMOKE_MAX_DLIGHTS )
		{
			out[slot] = light;
			nKept = MIN( nKept + 1, NEO_VOLSMOKE_MAX_DLIGHTS );
		}
	}

	return nKept;
}

static void SetDynamicLightParams( IMaterial *pMat, const VolSmokeDLight_t lights[NEO_VOLSMOKE_MAX_DLIGHTS], int nLights )
{
	SetIntVar( pMat, "$dlightcount", nLights );
	if ( nLights > 0 )
	{
		SetVec4Var( pMat, "$dlight0pos", lights[0].origin.x, lights[0].origin.y, lights[0].origin.z, lights[0].invReachSqr );
		SetVec3Var( pMat, "$dlight0color", lights[0].color );
	}

	if ( nLights > 1 )
	{
		SetVec4Var( pMat, "$dlight1pos", lights[1].origin.x, lights[1].origin.y, lights[1].origin.z, lights[1].invReachSqr );
		SetVec3Var( pMat, "$dlight1color", lights[1].color );
	}
}

void C_NeoVolSmoke::SetDrawParams( IMaterial *pMat ) const
{
	const Vector amb = m_vecColor * NEO_VOLSMOKE_AMBIENT_SHARE;
	const Vector lightCol = m_vecColor * ( 1.0f - NEO_VOLSMOKE_AMBIENT_SHARE );

	SetVec3Var( pMat, "$volmin", m_vecGridMin );
	SetVec3Var( pMat, "$volmax", m_vecGridMax );
	SetVec3Var( pMat, "$smokemin", m_vecSmokeMin );
	SetVec3Var( pMat, "$smokemax", m_vecSmokeMax );
	Vector seed;
	const float sphereRadius = SmokeSphere( seed );
	SetVec4Var( pMat, "$smokesphere", seed.x, seed.y, seed.z, sphereRadius );
	// Slices along z, one tile of the box's x by y cells each.
	SetVec4Var( pMat, "$gridparams", (float)m_nBox[2], (float)m_nAtlasCols, (float)m_nAtlasW / (float)m_nBox[0], (float)m_nAtlasH / (float)m_nBox[1] );
	SetVec2Var( pMat, "$tilecells", (float)m_nBox[0], (float)m_nBox[1] );
	SetVec3Var( pMat, "$spincenter", m_vecSpawn + Vector( 0.0f, 0.0f, NEO_VOLSMOKE_SPIN_HEIGHT ) );
	SetVec3Var( pMat, "$ambient", amb );
	SetVec3Var( pMat, "$lightcolor", lightCol );
	const float age = gpGlobals->curtime - m_flSpawnTime;
	SetFloatVar( pMat, "$extinction", DrawExtinction() );
	SetVec3Var( pMat, "$front", Vector( FrontRadius( age ), m_flDistRange, 1.0f / NEO_VOLSMOKE_EDGE ) );
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

	// Thermal vision sees through smoke. Any smoke drawn over a hot body
	// lowers its brightness, which the thermal color ramp turns from white
	// to yellow.
	if ( g_SmokeFogOverlayThermalOverride )
	{
		return 0;
	}

	// An opaque inside overlay hides the whole view, so the march would only
	// cost frame time.
	if ( SmokeFogOverlayCoversView() )
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

	VolSmokeDLight_t lights[NEO_VOLSMOKE_MAX_DLIGHTS];
	const int nLights = CollectDynamicLights( m_vecSmokeMin, m_vecSmokeMax, lights );
	SetDynamicLightParams( m_Material, lights, nLights );
	SetDynamicLightParams( m_SlabMaterial, lights, nLights );

	CMatRenderContextPtr pRenderContext( materials );

	pRenderContext->MatrixMode( MATERIAL_MODEL );
	pRenderContext->PushMatrix();
	pRenderContext->LoadIdentity();

	// Back to front: the far slabs, then the near pass in front of them.
	DrawFarSlabs( pRenderContext );
	DrawNearPass( pRenderContext );

	pRenderContext->MatrixMode( MATERIAL_MODEL );
	pRenderContext->PopMatrix();

	DrawRevealed( pRenderContext );

	return 1;
}

// Screen rectangle, as view-space x / z and y / z, of the part of a box
// between view depths lo and hi: the box corners inside that range and the
// points where the box edges cross its two ends. Rays outside it meet no
// smoke in that slab.
static void SlabScreenRect( const Vector view[8], float lo, float hi, float &xMin, float &xMax, float &yMin, float &yMax )
{
	xMin = yMin = FLT_MAX;
	xMax = yMax = -FLT_MAX;

	const auto addPoint = [&]( const Vector &p )
	{
		xMin = MIN( xMin, p.x / p.z );
		xMax = MAX( xMax, p.x / p.z );
		yMin = MIN( yMin, p.y / p.z );
		yMax = MAX( yMax, p.y / p.z );
	};

	for ( int i = 0; i < 8; ++i )
	{
		if ( view[i].z >= lo && view[i].z <= hi )
		{
			addPoint( view[i] );
		}

		// Each edge once: from corner i to the corner that differs in one bit.
		for ( int bit = 1; bit < 8; bit <<= 1 )
		{
			const int j = i | bit;
			if ( j == i )
			{
				continue;
			}

			const Vector &a = view[i];
			const Vector &b = view[j];
			const float ends[2] = { lo, hi };
			for ( const float z : ends )
			{
				if ( ( a.z - z ) * ( b.z - z ) > 0.0f || a.z == b.z )
				{
					continue;
				}

				addPoint( a + ( b - a ) * ( ( z - a.z ) / ( b.z - a.z ) ) );
			}
		}
	}
}

// Shrinks a slab's screen rectangle (view-space x / z and y / z) to the part
// of a view-space sphere between view depths lo and hi. That part lies in a
// view-aligned box, whose screen bound comes from its corners.
static void SphereSlabClip( const Vector &center, float radius, float lo, float hi, float &xMin, float &xMax, float &yMin, float &yMax )
{
	const float z0 = MAX( lo, center.z - radius );
	const float z1 = MIN( hi, center.z + radius );
	if ( z0 > z1 )
	{
		xMin = yMin = FLT_MAX;
		xMax = yMax = -FLT_MAX;
		return;
	}

	// Widest cross-section of the sphere inside the slab.
	const float off = ( center.z < z0 ) ? z0 - center.z : ( ( center.z > z1 ) ? center.z - z1 : 0.0f );
	const float reach = sqrtf( MAX( radius * radius - off * off, 0.0f ) );

	const float xs[2] = { center.x - reach, center.x + reach };
	const float ys[2] = { center.y - reach, center.y + reach };
	const float zs[2] = { z0, z1 };
	float sxMin = FLT_MAX;
	float sxMax = -FLT_MAX;
	float syMin = FLT_MAX;
	float syMax = -FLT_MAX;
	for ( const float z : zs )
	{
		for ( int k = 0; k < 2; ++k )
		{
			sxMin = MIN( sxMin, xs[k] / z );
			sxMax = MAX( sxMax, xs[k] / z );
			syMin = MIN( syMin, ys[k] / z );
			syMax = MAX( syMax, ys[k] / z );
		}
	}

	xMin = MAX( xMin, sxMin );
	xMax = MIN( xMax, sxMax );
	yMin = MAX( yMin, syMin );
	yMax = MIN( yMax, syMax );
}

// The atlas's path length (0..1 of DIST_RANGE) and weight at a point,
// filtered like the shader's lookup: clamped to the grid, trilinear.
void C_NeoVolSmoke::SampleAtlas( const Vector &pos, float &dist, float &weight, float &wallCut ) const
{
	const int *size = m_nBox;
	int base[3];
	float frac[3];
	for ( int a = 0; a < 3; ++a )
	{
		const float g = clamp( ( pos[a] - m_vecGridMin[a] ) / CNeoSmokeFill::CELL_SIZE - 0.5f, 0.0f, (float)( size[a] - 1 ) );
		base[a] = MIN( (int)g, size[a] - 2 );
		frac[a] = g - base[a];
	}

	// Neighbors in x and y are one texel and one atlas row apart within a slice.
	const unsigned char *bits = m_pRegen->Bits();
	const int dx = 4;
	const int dy = 4 * m_nAtlasW;
	dist = 0.0f;
	weight = 0.0f;
	float wall = 0.0f;
	for ( int bz = 0; bz < 2; ++bz )
	{
		const unsigned char *t = bits + AtlasOffset( base[0], base[1], base[2] + bz );
		const float wz = bz ? frac[2] : 1.0f - frac[2];
		const float w00 = wz * ( 1.0f - frac[0] ) * ( 1.0f - frac[1] );
		const float w10 = wz * frac[0] * ( 1.0f - frac[1] );
		const float w01 = wz * ( 1.0f - frac[0] ) * frac[1];
		const float w11 = wz * frac[0] * frac[1];
		dist += w00 * t[VOLSMOKE_BYTE_DIST] + w10 * t[dx + VOLSMOKE_BYTE_DIST]
			+ w01 * t[dy + VOLSMOKE_BYTE_DIST] + w11 * t[dx + dy + VOLSMOKE_BYTE_DIST];
		weight += w00 * t[VOLSMOKE_BYTE_WEIGHT] + w10 * t[dx + VOLSMOKE_BYTE_WEIGHT]
			+ w01 * t[dy + VOLSMOKE_BYTE_WEIGHT] + w11 * t[dx + dy + VOLSMOKE_BYTE_WEIGHT];
		wall += w00 * t[VOLSMOKE_BYTE_ALPHA] + w10 * t[dx + VOLSMOKE_BYTE_ALPHA]
			+ w01 * t[dy + VOLSMOKE_BYTE_ALPHA] + w11 * t[dx + dy + VOLSMOKE_BYTE_ALPHA];
	}

	dist /= 255.0f;
	weight /= 255.0f;
	wallCut = VolSmokeWallCut( wall );
}

// The shader's density at a point with the front moved in by the most the
// look noise can bite: never more than what the shader draws.
float C_NeoVolSmoke::DensityLowerBound( const Vector &pos, float front, float bite ) const
{
	float dist, weight, wallCut;
	SampleAtlas( pos, dist, weight, wallCut );
	weight *= wallCut;

	const float depth = front - dist * m_flDistRange - bite;
	if ( weight <= 0.001f || depth <= 0.0f )
	{
		return 0.0f;
	}

	const float edge = MIN( depth / NEO_VOLSMOKE_EDGE, 1.0f );
	return weight * edge * edge * ( 3.0f - 2.0f * edge );
}

// View depth at which the lower-bound smoke along an eye ray becomes opaque,
// or FLT_MAX if the ray leaves the smoke first. The ray ignores the world:
// a wall in front of that depth hides everything behind it anyway.
float C_NeoVolSmoke::OpaqueDepth( const Vector &eye, const Vector &dir, float front, float bite, float extinction ) const
{
	const float len = dir.Length();
	const Vector unit = dir / len;

	float t0 = 0.0f;
	float t1 = FLT_MAX;
	for ( int a = 0; a < 3; ++a )
	{
		if ( fabsf( unit[a] ) < 1.0e-6f )
		{
			if ( eye[a] < m_vecSmokeMin[a] || eye[a] > m_vecSmokeMax[a] )
			{
				return FLT_MAX;
			}
			continue;
		}

		const float ta = ( m_vecSmokeMin[a] - eye[a] ) / unit[a];
		const float tb = ( m_vecSmokeMax[a] - eye[a] ) / unit[a];
		t0 = MAX( t0, MIN( ta, tb ) );
		t1 = MIN( t1, MAX( ta, tb ) );
	}

	// Cell weights are at most 1, which bounds what the rest of the ray can add.
	float sigma = 0.0f;
	for ( float t = t0; t < t1; t += NEO_VOLSMOKE_CULL_STEP )
	{
		if ( sigma + extinction * ( t1 - t ) < NEO_VOLSMOKE_CULL_OPAQUE )
		{
			return FLT_MAX;
		}

		const float step = MIN( NEO_VOLSMOKE_CULL_STEP, t1 - t );
		sigma += DensityLowerBound( eye + unit * ( t + 0.5f * step ), front, bite ) * extinction * step;
		if ( sigma >= NEO_VOLSMOKE_CULL_OPAQUE )
		{
			// The ray direction has view depth 1 per len along it.
			return ( t + step ) / len;
		}
	}

	return FLT_MAX;
}

// Optical depth of this volume's smoke on the segment from the eye to a
// point, without the look noise, at the strength the volume draws with.
float C_NeoVolSmoke::PathOpticalDepth( const Vector &eye, const Vector &point ) const
{
	if ( !m_pRegen || m_vecSmokeMax.x <= m_vecSmokeMin.x )
	{
		return 0.0f;
	}

	const Vector delta = point - eye;
	const float len = delta.Length();
	if ( len < 1.0f )
	{
		return 0.0f;
	}

	const Vector unit = delta / len;
	float t0 = 0.0f;
	float t1 = len;
	for ( int a = 0; a < 3; ++a )
	{
		if ( fabsf( unit[a] ) < 1.0e-6f )
		{
			if ( eye[a] < m_vecSmokeMin[a] || eye[a] > m_vecSmokeMax[a] )
			{
				return 0.0f;
			}
			continue;
		}

		const float ta = ( m_vecSmokeMin[a] - eye[a] ) / unit[a];
		const float tb = ( m_vecSmokeMax[a] - eye[a] ) / unit[a];
		t0 = MAX( t0, MIN( ta, tb ) );
		t1 = MIN( t1, MAX( ta, tb ) );
	}

	const float front = FrontRadius( gpGlobals->curtime - m_flSpawnTime );
	float path = 0.0f;
	for ( float t = t0; t < t1; t += NEO_VOLSMOKE_PATH_STEP )
	{
		const float step = MIN( NEO_VOLSMOKE_PATH_STEP, t1 - t );
		path += DensityLowerBound( eye + unit * ( t + 0.5f * step ), front, 0.0f ) * step;
	}

	return path * DrawExtinction();
}

float C_NeoVolSmoke::OpticalDepth( const Vector &eye, const Vector &point )
{
	float tau = 0.0f;
	for ( const C_NeoVolSmoke *pVol : s_Volumes )
	{
		tau += pVol->PathOpticalDepth( eye, point );
	}

	return tau;
}

void C_NeoVolSmoke::PrintOpticalDepth( const Vector &from, const Vector &to )
{
	Msg( "neo_volsmoke_depth: (%g %g %g) -> (%g %g %g) depth %.4f (%d volumes)\n",
		from.x, from.y, from.z, to.x, to.y, to.z, OpticalDepth( from, to ), s_Volumes.Count() );
}

float C_NeoVolSmoke::FlashFactor( const Vector &eye, const Vector &point )
{
	return expf( -NEO_VOLSMOKE_FLASH_DEPTH_SHARE * OpticalDepth( eye, point ) );
}

// How strongly a flash, or what it lights, shows through smoke of optical
// depth tau.
static float FlashShows( float tau )
{
	const float through = expf( -NEO_VOLSMOKE_FLASH_DEPTH_SHARE * tau );
	return MAX( through - NEO_VOLSMOKE_FLASH_CUTOFF, 0.0f ) / ( 1.0f - NEO_VOLSMOKE_FLASH_CUTOFF );
}

// How strongly the flash point shows through smoke of optical depth tau.
static float FlashPointShows( float tau )
{
	const float depth = clamp( tau / NEO_VOLSMOKE_FLASH_POINT_DEPTH, 0.0f, 1.0f );
	const float through = 1.0f - powf( depth, NEO_VOLSMOKE_FLASH_POINT_FALLOFF );
	const float beyond = RemapValClamped( tau, NEO_VOLSMOKE_FLASH_POINT_DEPTH, NEO_VOLSMOKE_FLASH_POINT_GONE, 0.0f, 1.0f );
	const float floorShows = NEO_VOLSMOKE_FLASH_POINT_FLOOR * ( 1.0f - SimpleSpline( beyond ) );
	return MAX( through, floorShows );
}

// The volume that draws what the smoke reveals at a point: the one whose
// smoke box is nearest, so a flash or a player draws once per frame.
C_NeoVolSmoke *C_NeoVolSmoke::Owner( const Vector &point )
{
	C_NeoVolSmoke *pBest = nullptr;
	float bestSqr = FLT_MAX;
	for ( C_NeoVolSmoke *pVol : s_Volumes )
	{
		if ( pVol->m_vecSmokeMax.x <= pVol->m_vecSmokeMin.x )
		{
			continue;
		}

		const float distSqr = CalcSqrDistanceToAABB( pVol->m_vecSmokeMin, pVol->m_vecSmokeMax, point );
		if ( distSqr < bestSqr )
		{
			bestSqr = distSqr;
			pBest = pVol;
		}
	}

	return pBest;
}

static IMaterial *VolSmokeFlashMaterial()
{
	static IMaterial *s_pMat = nullptr;
	if ( !s_pMat )
	{
		KeyValues *pVMT = new KeyValues( "UnlitGeneric" );
		pVMT->SetString( "$basetexture", "effects/muzzleflash1" );
		pVMT->SetInt( "$additive", 1 );
		pVMT->SetInt( "$vertexcolor", 1 );
		pVMT->SetInt( "$vertexalpha", 1 );
		pVMT->SetInt( "$nocull", 1 );
		s_pMat = materials->CreateMaterial( "__neo_volsmoke_flash", pVMT );
	}

	return s_pMat;
}

static IMaterial *VolSmokeSilhouetteMaterial()
{
	static IMaterial *s_pMat = nullptr;
	if ( !s_pMat )
	{
		KeyValues *pVMT = new KeyValues( "UnlitGeneric" );
		pVMT->SetInt( "$model", 1 );
		pVMT->SetInt( "$translucent", 1 );
		s_pMat = materials->CreateMaterial( "__neo_volsmoke_silhouette", pVMT );
	}

	return s_pMat;
}

static void DrawFlashPoint( IMatRenderContext *pRenderContext, const Vector &origin, float strength )
{
	IMaterial *pMat = VolSmokeFlashMaterial();
	if ( !pMat || pMat->IsErrorMaterial() )
	{
		return;
	}

	const Vector right = CurrentViewRight() * NEO_VOLSMOKE_FLASH_POINT_RADIUS;
	const Vector up = CurrentViewUp() * NEO_VOLSMOKE_FLASH_POINT_RADIUS;
	const float level = 255.0f * clamp( strength, 0.0f, 1.0f );
	const unsigned char r = (unsigned char)( level * NEO_VOLSMOKE_FLASH_POINT_TINT.x );
	const unsigned char g = (unsigned char)( level * NEO_VOLSMOKE_FLASH_POINT_TINT.y );
	const unsigned char b = (unsigned char)( level * NEO_VOLSMOKE_FLASH_POINT_TINT.z );

	pRenderContext->MatrixMode( MATERIAL_MODEL );
	pRenderContext->PushMatrix();
	pRenderContext->LoadIdentity();
	pRenderContext->Bind( pMat );
	IMesh *pMesh = pRenderContext->GetDynamicMesh( true, NULL, NULL, pMat );

	const Vector corners[4] = { origin - right - up, origin + right - up, origin + right + up, origin - right + up };
	static const float us[4] = { 0.0f, 1.0f, 1.0f, 0.0f };
	static const float vs[4] = { 1.0f, 1.0f, 0.0f, 0.0f };
	CMeshBuilder meshBuilder;
	meshBuilder.Begin( pMesh, MATERIAL_QUADS, 1 );
	for ( int i = 0; i < 4; ++i )
	{
		meshBuilder.Position3fv( corners[i].Base() );
		meshBuilder.Color4ub( r, g, b, 255 );
		meshBuilder.TexCoord2f( 0, us[i], vs[i] );
		meshBuilder.AdvanceVertex();
	}
	meshBuilder.End();
	pMesh->Draw();

	pRenderContext->MatrixMode( MATERIAL_MODEL );
	pRenderContext->PopMatrix();
}

// Flat dark redraw of a player over the smoke in front of them.
static void DrawSilhouette( C_BasePlayer *pPlayer, float alpha )
{
	IMaterial *pMat = VolSmokeSilhouetteMaterial();
	if ( !pMat || pMat->IsErrorMaterial() )
	{
		return;
	}

	// The model draw sets the material's color and alpha from these.
	static const float black[3] = { 0.0f, 0.0f, 0.0f };
	float oldColor[3];
	render->GetColorModulation( oldColor );
	const float oldBlend = render->GetBlend();
	render->SetColorModulation( black );
	render->SetBlend( clamp( alpha, 0.0f, 1.0f ) );
	modelrender->ForcedMaterialOverride( pMat );
	pPlayer->DrawModel( STUDIO_RENDER | STUDIO_IGNORE_NEO_EFFECTS );

	// Weapons in hand or carried on the body are child entities: draw them
	// with the same alpha so they blend evenly into the body.
	for ( C_BaseEntity *pChild = pPlayer->FirstMoveChild(); pChild; pChild = pChild->NextMovePeer() )
	{
		if ( pChild->ShouldDraw() )
		{
			pChild->DrawModel( STUDIO_RENDER | STUDIO_IGNORE_NEO_EFFECTS );
		}
	}

	modelrender->ForcedMaterialOverride( nullptr );
	render->SetBlend( oldBlend );
	render->SetColorModulation( oldColor );
}

// The player a muzzle-flash light belongs to, or null for another light or
// the local view model's flash.
static C_BasePlayer *MuzzleFlashOwner( const dlight_t *pLight )
{
	if ( !IsMuzzleFlashLight( pLight ) )
	{
		return nullptr;
	}

	C_BaseEntity *pEnt = ClientEntityList().GetBaseEntity( pLight->key - LIGHT_INDEX_MUZZLEFLASH );
	C_BaseCombatWeapon *pWeapon = pEnt ? pEnt->MyCombatWeaponPointer() : nullptr;
	if ( !pWeapon )
	{
		return nullptr;
	}

	return ToBasePlayer( pWeapon->GetOwner() );
}

// What the smoke reveals, drawn over the volume: the point of each muzzle
// flash, the players the flash lights, and cloaked bodies. Flash and players
// scale with the smoke between them and the eye, so they fade with depth.
void C_NeoVolSmoke::DrawRevealed( IMatRenderContext *pRenderContext )
{
	const Vector &eye = CurrentViewOrigin();
	const float overlayClear = 1.0f - clamp( g_SmokeFogOverlayAlpha, 0.0f, 1.0f );
	float silhouette[MAX_PLAYERS + 1] = {};
	bool any = false;

	dlight_t *lights[MAX_DLIGHTS];
	const int nLights = effects->CL_GetActiveDLights( lights );
	for ( int i = 0; i < nLights; ++i )
	{
		const dlight_t *pLight = lights[i];
		C_BasePlayer *pShooter = MuzzleFlashOwner( pLight );
		if ( !pShooter || Owner( pLight->origin ) != this )
		{
			continue;
		}

		const float life = clamp( ( pLight->die - gpGlobals->curtime ) / NEO_VOLSMOKE_FLASH_LIFE, 0.0f, 1.0f );
		const float point = FlashPointShows( OpticalDepth( eye, pLight->origin ) ) * overlayClear * life;
		if ( point >= NEO_VOLSMOKE_FLASH_MIN )
		{
			DrawFlashPoint( pRenderContext, pLight->origin, point );
		}

		const float reachSqr = Square( NEO_VOLSMOKE_FLASH_REACH );
		for ( int p = 1; p <= gpGlobals->maxClients && p <= MAX_PLAYERS; ++p )
		{
			C_BasePlayer *pPlayer = UTIL_PlayerByIndex( p );
			if ( !pPlayer || ( pPlayer != pShooter && pPlayer->WorldSpaceCenter().DistToSqr( pLight->origin ) > reachSqr ) )
			{
				continue;
			}

			const float tau = OpticalDepth( eye, pPlayer->WorldSpaceCenter() );
			const float hidden = 1.0f - expf( -tau );
			const float shown = FlashShows( tau ) * overlayClear * life;
			silhouette[p] = MAX( silhouette[p], NEO_VOLSMOKE_FIRE_SILHOUETTE * shown * hidden );
			any = true;
		}
	}

	// Smoke flows around a cloaked body: show a faint shape, the same rule at
	// every depth and side, weaker than any flash.
	for ( int p = 1; p <= gpGlobals->maxClients && p <= MAX_PLAYERS; ++p )
	{
		C_NEO_Player *pPlayer = ToNEOPlayer( UTIL_PlayerByIndex( p ) );
		if ( !pPlayer || !pPlayer->IsCloaked() || Owner( pPlayer->WorldSpaceCenter() ) != this )
		{
			continue;
		}

		const float tau = OpticalDepth( eye, pPlayer->WorldSpaceCenter() );
		if ( tau <= 0.0f )
		{
			continue;
		}

		const float hidden = 1.0f - expf( -tau );
		const float shown = FlashShows( tau ) * overlayClear;
		silhouette[p] = MAX( silhouette[p], NEO_VOLSMOKE_CLOAK_SILHOUETTE * shown * hidden );
		any = true;
	}

	if ( !any )
	{
		return;
	}

	for ( int p = 1; p <= gpGlobals->maxClients && p <= MAX_PLAYERS; ++p )
	{
		C_BasePlayer *pPlayer = UTIL_PlayerByIndex( p );
		if ( silhouette[p] < NEO_VOLSMOKE_FLASH_MIN || !pPlayer || pPlayer->IsDormant() || !pPlayer->IsAlive() || !pPlayer->ShouldDraw() )
		{
			continue;
		}

		DrawSilhouette( pPlayer, silhouette[p] );
	}
}

// Screen tiles over the smoke, as view-space x / z and y / z, each with the
// view depth past which all of it is opaque.
struct VolSmokeCullGrid_t
{
	int cols;
	int rows;
	float xs[NEO_VOLSMOKE_CULL_MAX_COLS + 1];
	float ys[NEO_VOLSMOKE_CULL_MAX_ROWS + 1];
	float depth[NEO_VOLSMOKE_CULL_MAX_ROWS][NEO_VOLSMOKE_CULL_MAX_COLS];
};

// False when every slab is to be drawn whole.
bool C_NeoVolSmoke::BuildCullGrid( IMatRenderContext *pRenderContext, const Vector view[8], float zMin, float zMax, VolSmokeCullGrid_t &grid ) const
{
	if ( neo_volsmoke_slabs.GetInt() == 3 || !m_pRegen )
	{
		return false;
	}

	// A carved hole is narrower than a tile: its corner rays can be opaque
	// while the hole is not.
	if ( gpGlobals->curtime < m_flRegrowEnd )
	{
		return false;
	}

	// Thin smoke (bloom, fade) never reaches the opaque depth.
	const float extinction = DrawExtinction();
	if ( extinction * ( m_vecSmokeMax - m_vecSmokeMin ).Length() < NEO_VOLSMOKE_CULL_OPAQUE )
	{
		return false;
	}

	VMatrix proj;
	pRenderContext->GetMatrix( MATERIAL_PROJECTION, &proj );
	int vx, vy, width, height;
	pRenderContext->GetViewport( vx, vy, width, height );
	if ( proj[0][0] <= 0.0f || proj[1][1] <= 0.0f || width <= 0 || height <= 0 )
	{
		return false;
	}

	const float tanX = NEO_VOLSMOKE_CULL_FRUSTUM_MARGIN / proj[0][0];
	const float tanY = NEO_VOLSMOKE_CULL_FRUSTUM_MARGIN / proj[1][1];
	float x0, x1, y0, y1;
	SlabScreenRect( view, MAX( zMin, NEO_VOLSMOKE_CULL_MIN_DEPTH ), zMax, x0, x1, y0, y1 );
	x0 = MAX( x0, -tanX );
	x1 = MIN( x1, tanX );
	y0 = MAX( y0, -tanY );
	y1 = MIN( y1, tanY );
	if ( x0 >= x1 || y0 >= y1 )
	{
		return false;
	}

	const float pxX = ( x1 - x0 ) / ( 2.0f * tanX ) * width;
	const float pxY = ( y1 - y0 ) / ( 2.0f * tanY ) * height;
	grid.cols = clamp( (int)ceilf( pxX / NEO_VOLSMOKE_CULL_TILE_PX ), 1, NEO_VOLSMOKE_CULL_MAX_COLS );
	grid.rows = clamp( (int)ceilf( pxY / NEO_VOLSMOKE_CULL_TILE_PX ), 1, NEO_VOLSMOKE_CULL_MAX_ROWS );
	for ( int i = 0; i <= grid.cols; ++i )
	{
		grid.xs[i] = ( i == grid.cols ) ? x1 : x0 + ( x1 - x0 ) * i / grid.cols;
	}
	for ( int j = 0; j <= grid.rows; ++j )
	{
		grid.ys[j] = ( j == grid.rows ) ? y1 : y0 + ( y1 - y0 ) * j / grid.rows;
	}

	const Vector &eye = CurrentViewOrigin();
	const Vector &fwd = CurrentViewForward();
	const Vector &right = CurrentViewRight();
	const Vector &up = CurrentViewUp();
	const float age = gpGlobals->curtime - m_flSpawnTime;
	const float front = FrontRadius( age );
	const float bite = NEO_VOLSMOKE_PUFF_BITE * neo_volsmoke_noise_amp.GetFloat();

	float corner[NEO_VOLSMOKE_CULL_MAX_ROWS + 1][NEO_VOLSMOKE_CULL_MAX_COLS + 1];
	for ( int j = 0; j <= grid.rows; ++j )
	{
		for ( int i = 0; i <= grid.cols; ++i )
		{
			const Vector dir = fwd + right * grid.xs[i] + up * grid.ys[j];
			corner[j][i] = OpaqueDepth( eye, dir, front, bite, extinction );
		}
	}

	// Chord length through a convex cloud is smallest at a tile's corners.
	for ( int j = 0; j < grid.rows; ++j )
	{
		for ( int i = 0; i < grid.cols; ++i )
		{
			grid.depth[j][i] = MAX( MAX( corner[j][i], corner[j][i + 1] ), MAX( corner[j + 1][i], corner[j + 1][i + 1] ) );
		}
	}

	return true;
}

// The part of the volume beyond NEAR_RANGE, as view-parallel quads at the
// middle depth of each slab. The depth test hides a slab behind geometry, so
// smoke behind a surface the dest-alpha depth cannot see is wrong by at most
// half a slab along the ray. A slab's tiles behind opaque smoke are left out.
void C_NeoVolSmoke::DrawFarSlabs( IMatRenderContext *pRenderContext )
{
	const Vector &eye = CurrentViewOrigin();
	const Vector &fwd = CurrentViewForward();
	const Vector &right = CurrentViewRight();
	const Vector &up = CurrentViewUp();

	const Vector &a = m_vecSmokeMin;
	const Vector &b = m_vecSmokeMax;

	// The box corners in view space: x right, y up, z depth.
	Vector view[8];
	float zMin = FLT_MAX;
	float zMax = -FLT_MAX;
	for ( int i = 0; i < 8; ++i )
	{
		const Vector corner( ( i & 1 ) ? b.x : a.x, ( i & 2 ) ? b.y : a.y, ( i & 4 ) ? b.z : a.z );
		const Vector rel = corner - eye;
		view[i].Init( DotProduct( rel, right ), DotProduct( rel, up ), DotProduct( rel, fwd ) );
		zMin = MIN( zMin, view[i].z );
		zMax = MAX( zMax, view[i].z );
	}

	Vector seed;
	const float sphereRadius = SmokeSphere( seed );
	const Vector seedRel = seed - eye;
	const Vector sphereView( DotProduct( seedRel, right ), DotProduct( seedRel, up ), DotProduct( seedRel, fwd ) );

	m_nSlabTiles = 0;
	m_nSlabTilesCulled = 0;
	if ( zMax <= NEO_VOLSMOKE_NEAR_RANGE || neo_volsmoke_slabs.GetInt() == 0 )
	{
		return;
	}
	const float testAt = neo_volsmoke_slabs.GetInt() == 2 ? 1.0f : 0.5f;

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

	VolSmokeCullGrid_t grid;
	bool cull;
	{
		CVolSmokePerfScope perf( VOLSMOKE_PERF_CULL );
		cull = BuildCullGrid( pRenderContext, view, zMin, zMax, grid );
	}

	struct SlabQuad_t
	{
		float xMin, xMax, yMin, yMax;
		int slab;
	};
	CUtlVectorFixedGrowable< SlabQuad_t, 64 > quads;
	const auto addQuad = [&]( float x0, float x1, float y0, float y1, int slab )
	{
		if ( x0 < x1 && y0 < y1 )
		{
			const SlabQuad_t q = { x0, x1, y0, y1, slab };
			quads.AddToTail( q );
		}
	};

	const auto collect = [&]( bool useCull )
	{
		quads.RemoveAll();
		m_nSlabTiles = 0;
		m_nSlabTilesCulled = 0;
		for ( int s = nSlabs - 1; s >= 0; --s )
		{
			const float lo = bounds[s];
			float xMin, xMax, yMin, yMax;
			SlabScreenRect( view, lo, bounds[s + 1], xMin, xMax, yMin, yMax );
			SphereSlabClip( sphereView, sphereRadius, lo, bounds[s + 1], xMin, xMax, yMin, yMax );
			xMin = MAX( xMin, -NEO_VOLSMOKE_SLAB_MAX_TAN );
			yMin = MAX( yMin, -NEO_VOLSMOKE_SLAB_MAX_TAN );
			xMax = MIN( xMax, NEO_VOLSMOKE_SLAB_MAX_TAN );
			yMax = MIN( yMax, NEO_VOLSMOKE_SLAB_MAX_TAN );
			if ( !useCull )
			{
				addQuad( xMin, xMax, yMin, yMax, s );
				continue;
			}

			// Each run of kept tiles in a row becomes one quad. The grid covers
			// the visible part of every slab rectangle; the rest is off screen.
			for ( int j = 0; j < grid.rows; ++j )
			{
				const float y0 = MAX( grid.ys[j], yMin );
				const float y1 = MIN( grid.ys[j + 1], yMax );
				if ( y0 >= y1 )
				{
					continue;
				}

				int runStart = -1;
				for ( int i = 0; i <= grid.cols; ++i )
				{
					const bool keep = i < grid.cols && lo < grid.depth[j][i];
					if ( i < grid.cols && grid.xs[i + 1] > xMin && grid.xs[i] < xMax )
					{
						++m_nSlabTiles;
						m_nSlabTilesCulled += keep ? 0 : 1;
					}

					if ( keep && runStart < 0 )
					{
						runStart = i;
					}
					else if ( !keep && runStart >= 0 )
					{
						addQuad( MAX( grid.xs[runStart], xMin ), MIN( grid.xs[i], xMax ), y0, y1, s );
						runStart = -1;
					}
				}
			}
		}
	};

	pRenderContext->Bind( m_SlabMaterial, (IClientRenderable *)this );
	IMesh *pMesh = pRenderContext->GetDynamicMesh( true, NULL, NULL, m_SlabMaterial );

	collect( cull );
	int maxVerts, maxIndices;
	pRenderContext->GetMaxToRender( pMesh, false, &maxVerts, &maxIndices );
	if ( 4 * quads.Count() > maxVerts || 6 * quads.Count() > maxIndices )
	{
		collect( false );
	}

	if ( quads.Count() == 0 )
	{
		return;
	}

	CMeshBuilder meshBuilder;
	meshBuilder.Begin( pMesh, MATERIAL_TRIANGLES, 4 * quads.Count(), 6 * quads.Count() );
	int nVert = 0;
	for ( const SlabQuad_t &q : quads )
	{
		const float lo = bounds[q.slab];
		const float hi = bounds[q.slab + 1];
		const float zc = lo + testAt * ( hi - lo );
		const Vector center = eye + fwd * zc;

		const float xs[4] = { q.xMin, q.xMax, q.xMax, q.xMin };
		const float ys[4] = { q.yMin, q.yMin, q.yMax, q.yMax };
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

CON_COMMAND_F( neo_volsmoke_depth, "Print the smoke optical depth on a segment, as the flash and silhouette read it. Args: x1 y1 z1 x2 y2 z2", FCVAR_CHEAT )
{
	if ( args.ArgC() < 7 )
	{
		return;
	}

	const Vector from( atof( args[1] ), atof( args[2] ), atof( args[3] ) );
	const Vector to( atof( args[4] ), atof( args[5] ), atof( args[6] ) );
	C_NeoVolSmoke::PrintOpticalDepth( from, to );
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
