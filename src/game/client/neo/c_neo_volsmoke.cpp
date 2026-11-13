// NEO volumetric smoke - the client's cloud of a smoke grenade.
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
// Fixed values of the drawing. The gameplay values, which decide where the
// smoke blocks sight, are in neo_smoke_shared.h.
// ---------------------------------------------------------------------------

// Atlas texel bytes in BGRA order. The atlas holds what does not change with
// time: the gas path length, the cell weight (its variation) and
// the light. The shader applies the bloom front and the fade.
enum VolSmokeAtlasByte_t
{
	VOLSMOKE_BYTE_DIST = 0,   // B: path length, 0..the volume's distance range
	VOLSMOKE_BYTE_LIGHT = 1,  // G: transmittance toward the light
	VOLSMOKE_BYTE_WEIGHT = 2, // R: density of a cell inside the front
	VOLSMOKE_BYTE_ALPHA = 3,  // A: distance to the wall beside the gas
};

// The bytes the fill job builds per box cell for the first pack.
enum VolSmokeCellByte_t
{
	VOLSMOKE_CELL_DIST = 0,
	VOLSMOKE_CELL_WALL,
	VOLSMOKE_CELL_BYTES,
};

// The look noise turns about a point this far above the grenade, near the
// middle of a cloud on open floor.
static constexpr float NEO_VOLSMOKE_SPIN_HEIGHT = 8 * CNEOSmokeFill::CELL_SIZE;

// The atlas changes only while the fill or the bloom light runs;
// it is uploaded then, at most this often, and never after.
static constexpr float NEO_VOLSMOKE_UPLOAD_HZ = 15.0f;

// Fills run on worker threads, one job per fill, so a frame never pays for
// one. Without a worker pool the frame takes a fixed slice of fill work
// instead, in small chunks so it overruns by at most one chunk. A slow
// machine blooms later, never differently.
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

// The shader's look noise moves the front in by up to this; the cull assumes
// the most.
static constexpr float NEO_VOLSMOKE_PUFF_BITE = 28.0f;

// The grid covers the view frustum with this margin, and starts this far in
// front of the eye so a box around the camera keeps a finite screen size.
static constexpr float NEO_VOLSMOKE_CULL_FRUSTUM_MARGIN = 1.02f;
static constexpr float NEO_VOLSMOKE_CULL_MIN_DEPTH = 1.0f;

// All smoke lies inside a sphere about the gas seed this much wider than the
// front: a gas path is never shorter than the straight line from the seed,
// and a trilinear lookup spreads a cell's path length up to sqrt(3) cells.
// One more cell covers the atlas rounding and the clamp at the grid's z ends,
// and one march step keeps the cells the shader clips at the sphere empty.
static constexpr float NEO_VOLSMOKE_MARCH_STEP = 8.0f; // the shader's STEP_LEN
static constexpr float NEO_VOLSMOKE_SPHERE_MARGIN = ( 1.7321f + 1.0f ) * CNEOSmokeFill::CELL_SIZE + NEO_VOLSMOKE_MARCH_STEP;

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
static constexpr float NEO_VOLSMOKE_NOMINAL_RADIUS = CNEOSmokeFill::DOME_RADIUS;

// Look (cosmetic, may differ between clients).
// The legacy particle color: a 0.5-0.6 gray times the light, desaturated
// halfway to mid gray. It is a display value; the shader works in linear.
static constexpr float NEO_VOLSMOKE_GRAY = 0.55f;
static constexpr float NEO_VOLSMOKE_DESATURATE = 0.5f;
static constexpr float NEO_VOLSMOKE_DISPLAY_GAMMA = 2.2f;

// Below the knee the cloud darkens with its surroundings, as a gray wall
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
static constexpr float NEO_VOLSMOKE_LIGHT_SAMPLE_OUT = 0.5f * CNEOSmokeFill::DOME_RADIUS;

// The inside overlay averages the density over a cube of samples around the
// camera, so it ramps in over about the cube's size while walking in, as the
// legacy overlay ramps with the distance to the grenade.
static constexpr float NEO_VOLSMOKE_OVERLAY_REACH = 64.0f;
static constexpr int NEO_VOLSMOKE_OVERLAY_SIDE = 3; // samples per axis

// Average density at which the overlay covers the view completely. Below the
// smallest cell variation, so the core hides the viewmodel as legacy does.
static constexpr float NEO_VOLSMOKE_OVERLAY_FULL = 0.5f;

// Without the shader, opaque boxes stand in for the cloud. While the cloud
// blooms they are rebuilt this often, each time for the front this far
// ahead, so the boxes stay ahead of the server's front between rebuilds and
// across the offset between the two clocks.
static constexpr float NEO_VOLSMOKE_COVER_PERIOD = 0.25f;
static constexpr float NEO_VOLSMOKE_COVER_LEAD = 0.35f;

// Clouds whose fills end in one frame would rebuild in the same frames. A
// volume's first period is cut to one of this many shares, in turn by the
// order of first builds, so their rebuilds spread over the period. A
// shorter period only rebuilds sooner.
static constexpr int NEO_VOLSMOKE_COVER_PHASES = 8;

// The lookup spreads a cell's smoke half a cell past it.
static constexpr float NEO_VOLSMOKE_COVER_SKIN = 0.5f * CNEOSmokeFill::CELL_SIZE;

// The near plane clips a box face this close to the eye, so the full inside
// overlay covers the view there.
static constexpr float NEO_VOLSMOKE_COVER_NEAR_MARGIN = 16.0f;

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


// Opaque sheet: the render the last sheet was drawn in.
static int s_nSheetSerial = 0;
static int s_nSheetFrame = -1;

// The shared ray grid of the last sheet march, with each volume's own opaque
// depth at every grid corner, for the per-volume slab cull.
static int s_nCullSerial = -1;
static int s_nCullCols = 0;
static int s_nCullRows = 0;
static float s_flCullXs[NEO_VOLSMOKE_CULL_MAX_COLS + 1];
static float s_flCullYs[NEO_VOLSMOKE_CULL_MAX_ROWS + 1];
static CUtlVector< const C_NeoVolSmoke * > s_CullVolumes;
static CUtlVector< float > s_flCullCorner;
static CUtlVector< float > s_flCenterOwn;

// The sheet's view depth on each grid tile, FLT_MAX where the tile is not
// opaque: every volume's slab cull hides its tiles past it.
static float s_flSheetDepth[NEO_VOLSMOKE_CULL_MAX_ROWS][NEO_VOLSMOKE_CULL_MAX_COLS];

// ---------------------------------------------------------------------------
// The fill's world queries (shared with the server), plus what only the
// client draws: the walls beside the gas.
// ---------------------------------------------------------------------------
class CNeoVolSmokeWorld final : public CNEOSmokeWorld
{
public:
	CNeoVolSmokeWorld( const Vector &gridMin, const int boxSize[3], const int seedCorner[3], const int latticeLo[3], const int latticeHi[3] )
		: CNEOSmokeWorld( gridMin, boxSize, seedCorner, latticeLo, latticeHi )
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

		Ray_t ray;
		ray.Init( Center( x, y, z ), Center( n[0], n[1], n[2] ) );
		CTraceFilterWorldOnly filter;
		trace_t tr;
		enginetrace->TraceRay( ray, NEO_VOLSMOKE_FILL_MASK, &filter, &tr );
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
		{
			return;
		}

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
// NEO_VOLSMOKE_NOISE_SHIFT_X / _Y texels away. Must match NOISE_SIZE and
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

	s_VolSmokeNoise->SetTextureRegenerator( nullptr );
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
	CNeoVolSmokeWorld *pWorld; // the fill's world cache, large enough to stall a frame when many are freed
	int        nFrame;
};

static CUtlVector< VolSmokeRetired_t > s_VolSmokeRetired;

static void VolSmokeDeleteRetired( const VolSmokeRetired_t &retired )
{
	delete retired.pWorld;

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
		retired.pAtlas->SetTextureRegenerator( nullptr ); // calls Release() on the regenerator
		retired.pAtlas->DecrementReferenceCount();
		retired.pAtlas->DeleteIfUnreferenced();
	}
}

enum VolSmokeRetireWait_t
{
	VOLSMOKE_RETIRE_AFTER_FRAMES,
	VOLSMOKE_RETIRE_NOW,
};

// Deleting a volume's materials and atlas is the heavy part of its teardown;
// a round restart retires many at once, so a frame deletes only
// one of them and the rest wait for later frames.
static constexpr int NEO_VOLSMOKE_RETIRE_PER_FRAME = 1;

static void VolSmokeDeleteRetired( VolSmokeRetireWait_t wait )
{
	// The list is in retire order, so the oldest go first.
	int deleted = 0;
	for ( int i = 0; i < s_VolSmokeRetired.Count(); )
	{
		if ( wait == VOLSMOKE_RETIRE_AFTER_FRAMES
			&& ( deleted >= NEO_VOLSMOKE_RETIRE_PER_FRAME
				|| gpGlobals->framecount - s_VolSmokeRetired[i].nFrame < NEO_VOLSMOKE_RETIRE_FRAMES ) )
		{
			break;
		}

		VolSmokeDeleteRetired( s_VolSmokeRetired[i] );
		s_VolSmokeRetired.Remove( i );
		++deleted;
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
	: m_nId( s_nNextId++ ), m_bBoxFitted( false ), m_bFitted( false ), m_flSpawnTime( 0 ), m_pWorld( nullptr ), m_pFillJob( nullptr ), m_bFillDone( false ),
	  m_bCellBytes( false ), m_nBounded( 0 ), m_pRegen( nullptr ), m_flDistRange( NEO_VOLSMOKE_DIST_RANGE ), m_nAtlasCols( 0 ),
	  m_nAtlasW( 0 ), m_nAtlasH( 0 ), m_nPacked( 0 ), m_flNextUpload( 0 ), m_flUploadedFront( 0 ), m_bUploadPending( false ),
	  m_nRenderSerial( -1 ), m_nCoverBoxes( 0 ), m_flNextCover( 0 ), m_bCoverFinal( false ), m_bCoverReachOnJob( false )
{
	for ( int a = 0; a < 3; ++a )
	{
		m_nBox[a] = 0;
		m_nSeedCorner[a] = 0;
		m_nBoxOffset[a] = 0;
	}
}

C_NeoVolSmoke::~C_NeoVolSmoke()
{
	StopFill();
	Shutdown();
	s_Volumes.FindAndRemove( this );
}

C_NeoVolSmoke *C_NeoVolSmoke::Create( const Vector &origin, float spawnTime )
{
	C_NeoVolSmoke *pVol = new C_NeoVolSmoke();
	pVol->Init( origin, spawnTime );

	if ( !pVol->InitializeAsClientEntity( nullptr, RENDER_GROUP_TRANSLUCENT_ENTITY ) )
	{
		pVol->Release();
		return nullptr;
	}

	pVol->SetNextClientThink( CLIENT_THINK_ALWAYS );
	s_Volumes.AddToTail( pVol );
	return pVol;
}

C_NeoVolSmoke *C_NeoVolSmoke::CreateForGrenade( const Vector &origin, float spawnTime, C_BaseEntity *pGrenade )
{
	if ( gpGlobals->curtime - spawnTime >= NEO_VOLSMOKE_FADE_END )
	{
		return nullptr;
	}

	// A full update announces the same networked smoke again; a second
	// volume would draw the cloud twice and redo its fill.
	Assert( pGrenade );
	for ( C_NeoVolSmoke *pVol : s_Volumes )
	{
		if ( pVol->m_hGrenade.Get() == pGrenade )
		{
			return pVol;
		}
	}

	C_NeoVolSmoke *pVol = Create( origin, spawnTime );
	if ( pVol )
	{
		pVol->m_hGrenade = pGrenade;
	}

	return pVol;
}

void C_NeoVolSmoke::RemoveExpired()
{
	const float now = gpGlobals->curtime;
	for ( int i = s_Volumes.Count() - 1; i >= 0; --i )
	{
		const C_NeoVolSmoke *pVol = s_Volumes[i];
		const bool grenadeGone = !pVol->m_hGrenade.Get();
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
	const float cell = CNEOSmokeFill::CELL_SIZE;
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

	// The fill job cannot ask the material system.
	m_bCoverReachOnJob = UsesCoverBoxes();

	m_pWorld = new CNeoVolSmokeWorld( m_vecGridMin, m_nBox, seedCorner, latticeLo, latticeHi );
	m_Fill.Start( m_pWorld );

	m_vecQueryMin = m_vecGridMin;
	m_vecColor.Init();
}

// A cell's center, from the corner of the box the fill queries in: the same
// point whatever box the volume is drawn in.
Vector C_NeoVolSmoke::CellCenter( const int c[3] ) const
{
	const float cell = CNEOSmokeFill::CELL_SIZE;
	return m_vecQueryMin + Vector( ( c[0] + m_nBoxOffset[0] + 0.5f ) * cell, ( c[1] + m_nBoxOffset[1] + 0.5f ) * cell,
		( c[2] + m_nBoxOffset[2] + 0.5f ) * cell );
}

// Path length at atlas value 1.
float C_NeoVolSmoke::DistRange() const
{
	return Max( NEO_VOLSMOKE_DIST_RANGE, CNEOSmokeFill::DistToUnits( m_Fill.FrontDist() ) );
}

// Once the fill is done: the box and the bounds fitted to the field.
void C_NeoVolSmoke::FitBoxToField()
{
	m_bBoxFitted = true;
	m_Fill.BoxSize( m_nBox );
	m_Fill.BoxSeedCorner( m_nSeedCorner );
	m_Fill.BoxOffset( m_nBoxOffset );
	const float cell = CNEOSmokeFill::CELL_SIZE;
	for ( int a = 0; a < 3; ++a )
	{
		m_vecGridMin[a] = m_vecSpawn[a] - m_nSeedCorner[a] * cell;
		m_vecGridMax[a] = m_vecSpawn[a] + ( m_nBox[a] - m_nSeedCorner[a] ) * cell;
		m_SmokeLo[a] = m_nBox[a];
		m_SmokeHi[a] = -1;
	}

	SetCollisionBounds( m_vecGridMin - m_vecSpawn, m_vecGridMax - m_vecSpawn );
	m_pWorld->SetDrawBox( m_nBoxOffset, m_nBox );
	m_flDistRange = DistRange();

	// From here the light samples can be kept to where the gas went.
	m_vecColor = SampleColor();
}

// The atlas and materials, whose layout depends on the fitted box. The
// smoke shader draws nothing before them.
void C_NeoVolSmoke::FitToField()
{
	m_bFitted = true;
	CreateAtlas();
}

void C_NeoVolSmoke::CreateAtlas()
{
	// As many tiles to a row as fit the atlas size; both sides a power of two.
	m_nAtlasCols = Max( 1, CNEOSmokeFill::ATLAS_SIZE / m_nBox[0] );
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
	pVMT->SetInt( "$jitter", 1 );
	m_Material.Init( matName, pVMT );

	Q_snprintf( matName, sizeof( matName ), "__neo_volsmoke_%d_slab", m_nId );
	pVMT = new KeyValues( "Neo_VolSmoke" );
	pVMT->SetString( "$basetexture", texName );
	pVMT->SetString( "$noisetexture", NEO_VOLSMOKE_NOISE_NAME );
	pVMT->SetInt( "$jitter", 1 );
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
	m_pRegen = nullptr; // the atlas owns it

	retired.pWorld = m_pWorld;
	m_pWorld = nullptr;

	if ( retired.pMaterials[0] || retired.pMaterials[1] || retired.pAtlas || retired.pWorld )
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
		const float display = ( NEO_VOLSMOKE_GRAY * Clamp( light[k], 0.0f, 1.0f ) + NEO_VOLSMOKE_DESATURATE ) * 0.5f;
		color[k] = powf( display, NEO_VOLSMOKE_DISPLAY_GAMMA );
	}

	const Vector clamped( Clamp( light.x, 0.0f, 1.0f ), Clamp( light.y, 0.0f, 1.0f ), Clamp( light.z, 0.0f, 1.0f ) );
	const float luminance = clamped.Dot( NEO_VOLSMOKE_LUMINANCE );
	const float dark = Clamp( ( luminance + NEO_VOLSMOKE_DARK_FLOOR ) / NEO_VOLSMOKE_DARK_KNEE, 0.0f, 1.0f );
	if ( dark < 1.0f )
	{
		color *= dark;
	}

	return color;
}

// Whether the fill reached the cell holding the point.
bool C_NeoVolSmoke::GasReached( const Vector &pos ) const
{
	const Vector g = ( pos - m_vecGridMin ) / CNEOSmokeFill::CELL_SIZE;
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

	IThreadPool *pPool = NEOSmokeFillPool();
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
		pVol->m_pFillJob = nullptr;
		pVol->m_bFillDone = pVol->m_Fill.IsDone();
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
				oldest = Min( oldest, pVol->m_flSpawnTime );
			}
		}

		C_NeoVolSmoke *pNearest = nullptr;
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
void C_NeoVolSmoke::StepFill( double budget )
{
	const double start = Plat_FloatTime();
	while ( !m_Fill.IsDone() && Plat_FloatTime() - start < budget )
	{
		m_Fill.Step( NEO_SMOKE_FILL_CHUNK_CELLS, NEO_SMOKE_FILL_CHUNK_QUERIES );
	}

	if ( m_Fill.IsDone() )
	{
		m_bFillDone = true;
	}
}

// The fill job, on a worker. It touches only the fill, its world and the
// cover reach; the chunks let StopFill cancel it quickly.
void C_NeoVolSmoke::RunFill()
{
	NEOSmokeFillRun( m_Fill, m_nFillCancel );
	if ( m_Fill.IsDone() )
	{
		BuildCellBytes();
		if ( m_bCoverReachOnJob && m_Fill.FinalizedCount() > 0 )
		{
			BuildCoverReach();
		}
	}
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

// Has the atlas new cells to take, not only new light?
bool C_NeoVolSmoke::ShapeChanged() const
{
	return m_nPacked < m_Fill.FinalizedCount();
}

// The volume whose atlas upload has waited longest, a changed shape before a
// light change. nullptr when none waited at the start of the frame.
C_NeoVolSmoke *C_NeoVolSmoke::UploadTurn()
{
	static int s_nTurnFrame = -1;
	static C_NeoVolSmoke *s_pTurn = nullptr;
	if ( s_nTurnFrame == gpGlobals->framecount )
	{
		return s_pTurn;
	}

	s_nTurnFrame = gpGlobals->framecount;
	s_pTurn = nullptr;
	for ( int i = 0; i < s_Volumes.Count(); ++i )
	{
		C_NeoVolSmoke *pVol = s_Volumes[i];
		if ( !pVol->m_bUploadPending )
		{
			continue;
		}

		if ( s_pTurn && s_pTurn->ShapeChanged() && !pVol->ShapeChanged() )
		{
			continue;
		}

		if ( !s_pTurn || ( pVol->ShapeChanged() && !s_pTurn->ShapeChanged() ) || pVol->m_flNextUpload < s_pTurn->m_flNextUpload )
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
	return Min( BloomRadius( age ), m_flUploadedFront );
}

// The front by the bloom curve alone.
float C_NeoVolSmoke::BloomRadius( float age ) const
{
	const float bloom = VolSmokeBloom( age );
	if ( FillDone() )
	{
		return CNEOSmokeFill::DistToUnits( m_Fill.FrontDist() ) * bloom;
	}

	return NEO_VOLSMOKE_NOMINAL_RADIUS * bloom;
}

// Smoke density at a point: trilinear over the eight nearest cell centers,
// as the shader samples the atlas.
float C_NeoVolSmoke::DensityAt( const Vector &pos, float front ) const
{
	const Vector g = ( pos - m_vecGridMin ) / CNEOSmokeFill::CELL_SIZE - Vector( 0.5f, 0.5f, 0.5f );
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

		const float edge = Clamp( ( front - CNEOSmokeFill::DistToUnits( m_Fill.Dist( i ) ) ) / NEO_VOLSMOKE_EDGE, 0.0f, 1.0f );
		density += w * edge * edge * ( 3.0f - 2.0f * edge ) * CellWeight( i );
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
		sum += DensityAt( eye - reach + offset * spacing, front );
	}

	if ( sum <= 0.0f )
	{
		return;
	}

	const float age = now - m_flSpawnTime;
	const float density = sum / ( side * side * side );
	const float share = Clamp( density / NEO_VOLSMOKE_OVERLAY_FULL, 0.0f, 1.0f );
	AddSmokeFogOverlayColored( share * VolSmokeBloom( age ) * VolSmokeFade( age ), DisplayColor() );
}

// The overlay is a display color: the cloud's own, so the view keeps
// its brightness on walking in, dark in a dark room.
Vector C_NeoVolSmoke::DisplayColor() const
{
	Vector display;
	for ( int k = 0; k < 3; ++k )
	{
		display[k] = powf( m_vecColor[k], 1.0f / NEO_VOLSMOKE_DISPLAY_GAMMA );
	}

	return display;
}

// Density of a cell inside the front: its variation.
float C_NeoVolSmoke::CellWeight( int idx ) const
{
	int c[3];
	CellCoords( idx, c[0], c[1], c[2] );
	return VolSmokeCellVariation( c[0] - m_nSeedCorner[0], c[1] - m_nSeedCorner[1], c[2] - m_nSeedCorner[2] );
}

static unsigned char VolSmokeByte( float v )
{
	return (unsigned char)( Clamp( v, 0.0f, 1.0f ) * 255.0f + 0.5f );
}

static unsigned char VolSmokeWallByte( float distance )
{
	// Rounded down: a face is never placed past where it is.
	const float steps = floorf( distance * NEO_VOLSMOKE_WALL_STEPS_PER_UNIT ) + NEO_VOLSMOKE_WALL_ZERO_BYTE;
	return (unsigned char)Clamp( steps, 0.0f, 255.0f );
}

// Smoke left at a point by the wall distance read from the atlas.
static float VolSmokeWallCut( float wallByte )
{
	const float distance = ( wallByte - NEO_VOLSMOKE_WALL_ZERO_BYTE ) / NEO_VOLSMOKE_WALL_STEPS_PER_UNIT;
	return Clamp( distance - NEO_VOLSMOKE_WALL_MARGIN, 0.0f, 1.0f );
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

// The atlas's path length and wall distance of every cell of the box, from
// the finished fill. A trilinear lookup beside the gas also reads cells
// without gas; those take the path length of their nearest gas neighbor, so
// their zero weight fades the edge instead of a jump in path length cutting
// it off. Only world traces and the fill: the fill job runs it, so the frame
// that first packs the volume copies bytes instead of tracing. A cancelled
// job leaves the bytes unbuilt.
void C_NeoVolSmoke::BuildCellBytes()
{
	int box[3];
	int offset[3];
	m_Fill.BoxSize( box );
	m_Fill.BoxOffset( offset );
	m_pWorld->SetDrawBox( offset, box );

	const float distRange = DistRange();
	const float cell = CNEOSmokeFill::CELL_SIZE;
	const int cells = box[0] * box[1] * box[2];
	m_CellBytes.SetCount( cells * VOLSMOKE_CELL_BYTES );
	Q_memset( m_CellBytes.Base(), 255, m_CellBytes.Count() );
	unsigned char *bytes = m_CellBytes.Base();

	const auto inBox = [&box]( int x, int y, int z )
	{
		return x >= 0 && y >= 0 && z >= 0 && x < box[0] && y < box[1] && z < box[2];
	};

	const int count = m_Fill.FinalizedCount();
	for ( int k = 0; k < count; ++k )
	{
		if ( ( k % NEO_SMOKE_FILL_CHUNK_CELLS ) == 0 && m_nFillCancel != 0 )
		{
			return;
		}

		const int i = m_Fill.FinalizedCell( k );
		const int c[3] = { i % box[0], ( i / box[0] ) % box[1], i / ( box[0] * box[1] ) };
		const unsigned char dist = VolSmokeByte( CNEOSmokeFill::DistToUnits( m_Fill.Dist( i ) ) / distRange );
		bytes[i * VOLSMOKE_CELL_BYTES + VOLSMOKE_CELL_DIST] = dist;

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

		const Vector center = m_vecQueryMin + Vector( ( c[0] + offset[0] + 0.5f ) * cell, ( c[1] + offset[1] + 0.5f ) * cell,
			( c[2] + offset[2] + 0.5f ) * cell );
		float ownWall = NEO_VOLSMOKE_WALL_CAP;
		for ( int w = 0; w < walls; ++w )
		{
			ownWall = Min( ownWall, DotProduct( wallNormal[w], center ) - wallDist[w] );
		}

		bytes[i * VOLSMOKE_CELL_BYTES + VOLSMOKE_CELL_WALL] = VolSmokeWallByte( ownWall );

		for ( int dz = -1; dz <= 1; ++dz )
		{
			for ( int dy = -1; dy <= 1; ++dy )
			{
				for ( int dx = -1; dx <= 1; ++dx )
				{
					if ( !inBox( c[0] + dx, c[1] + dy, c[2] + dz ) )
					{
						continue;
					}

					const int n = ( ( c[2] + dz ) * box[1] + c[1] + dy ) * box[0] + c[0] + dx;
					if ( m_Fill.IsFinalized( n ) )
					{
						continue;
					}

					unsigned char &nDist = bytes[n * VOLSMOKE_CELL_BYTES + VOLSMOKE_CELL_DIST];
					nDist = Min( nDist, dist );
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
					if ( !inBox( c[0] + dx, c[1] + dy, c[2] + dz ) )
					{
						continue;
					}

					const int n = ( ( c[2] + dz ) * box[1] + c[1] + dy ) * box[0] + c[0] + dx;
					if ( m_Fill.IsFinalized( n ) )
					{
						continue;
					}

					float nWall = NEO_VOLSMOKE_WALL_CAP;
					const Vector nCenter = center + Vector( dx * cell, dy * cell, dz * cell );
					for ( int w = 0; w < walls; ++w )
					{
						nWall = Min( nWall, DotProduct( wallNormal[w], nCenter ) - wallDist[w] );
					}

					// A cell past a face stays past it whatever the other gas cells
					// say. Among the faces it is past, the nearest one counts: at an
					// outside corner the farther face is not the one beside the gas.
					const unsigned char newByte = Min<int>( VolSmokeWallByte( nWall ), NEO_VOLSMOKE_WALL_UNSET - 1 );
					unsigned char &nWallByte = bytes[n * VOLSMOKE_CELL_BYTES + VOLSMOKE_CELL_WALL];
					const bool bothPast = nWallByte < NEO_VOLSMOKE_WALL_ZERO_BYTE && newByte < NEO_VOLSMOKE_WALL_ZERO_BYTE;
					if ( nWallByte == NEO_VOLSMOKE_WALL_UNSET )
					{
						nWallByte = newByte;
					}
					else if ( bothPast )
					{
						nWallByte = Max( nWallByte, newByte );
					}
					else
					{
						nWallByte = Min( nWallByte, newByte );
					}
				}
			}
		}
	}

	m_bCellBytes = true;
}

// Writes the finished fill into the atlas, once: the fill is done before the
// first pack. The bytes come from the fill job, or are built here when it was
// cancelled or there are no workers.
bool C_NeoVolSmoke::PackNewCells()
{
	const int count = m_Fill.FinalizedCount();
	if ( m_nPacked >= count )
	{
		return false;
	}

	if ( !m_bCellBytes )
	{
		BuildCellBytes();
		if ( !m_bCellBytes )
		{
			return false;
		}
	}

	unsigned char *bits = m_pRegen->Bits();
	const unsigned char *bytes = m_CellBytes.Base();
	for ( int z = 0; z < m_nBox[2]; ++z )
	{
		for ( int y = 0; y < m_nBox[1]; ++y )
		{
			for ( int x = 0; x < m_nBox[0]; ++x, bytes += VOLSMOKE_CELL_BYTES )
			{
				unsigned char *texel = bits + AtlasOffset( x, y, z );
				texel[VOLSMOKE_BYTE_DIST] = bytes[VOLSMOKE_CELL_DIST];
				texel[VOLSMOKE_BYTE_ALPHA] = bytes[VOLSMOKE_CELL_WALL];
			}
		}
	}

	for ( ; m_nPacked < count; ++m_nPacked )
	{
		const int i = m_Fill.FinalizedCell( m_nPacked );
		bits[AtlasOffset( i ) + VOLSMOKE_BYTE_WEIGHT] = VolSmokeByte( CellWeight( i ) );
	}

	m_CellBytes.Purge();
	return true;
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
		if ( CNEOSmokeFill::DistToUnits( m_Fill.Dist( i ) ) >= front )
		{
			break;
		}

		int c[3];
		CellCoords( i, c[0], c[1], c[2] );
		for ( int a = 0; a < 3; ++a )
		{
			m_SmokeLo[a] = Min( m_SmokeLo[a], c[a] );
			m_SmokeHi[a] = Max( m_SmokeHi[a], c[a] );
		}
		grew = true;
	}

	if ( !grew )
	{
		return;
	}

	// One cell of margin: the shader's trilinear lookup spreads a cell's
	// smoke half a cell past it.
	const float cell = CNEOSmokeFill::CELL_SIZE;
	for ( int a = 0; a < 3; ++a )
	{
		m_vecSmokeMin[a] = Max( m_vecGridMin[a], m_vecGridMin[a] + ( m_SmokeLo[a] - 1 ) * cell );
		m_vecSmokeMax[a] = Min( m_vecGridMax[a], m_vecGridMin[a] + ( m_SmokeHi[a] + 2 ) * cell );
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
void C_NeoVolSmoke::SweepLight()
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
			lo[a] = Min( lo[a], c[a] );
			hi[a] = Max( hi[a], c[a] );
		}
	}

	for ( int a = 0; a < 3; ++a )
	{
		lo[a] = Max( 0, lo[a] - MARGIN );
		hi[a] = Min( size[a] - 1, hi[a] + MARGIN );
	}

	unsigned char *bits = m_pRegen->Bits();
	const float kStep = NEO_VOLSMOKE_SWEEP_EXTINCTION * CNEOSmokeFill::CELL_SIZE;
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

				T *= expf( -kStep * CellWeight( i ) );
			}
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

	StepFills();
	if ( !FillDone() )
	{
		return;
	}

	if ( !m_bBoxFitted )
	{
		FitBoxToField();
	}

	// The cover boxes need only the finished fill, so a box client has no
	// atlas.
	if ( UsesCoverBoxes() )
	{
		UpdateCoverBoxes( now );
		AddCoverOverlay();
		return;
	}

	if ( !m_bFitted )
	{
		// Creating a volume's atlas and materials takes a millisecond or
		// two; clouds that appear together fit one a frame, as they pack.
		static int s_nFitFrame = -1;
		if ( s_nFitFrame == gpGlobals->framecount )
		{
			return;
		}

		s_nFitFrame = gpGlobals->framecount;
		FitToField();
	}

	const float front = FrontRadius( now - m_flSpawnTime );
	UpdateBounds( front );
	AddInsideOverlay( front, now );

	if ( !m_pRegen || !m_Atlas.IsValid() || now < m_flNextUpload )
	{
		return;
	}

	m_bUploadPending = m_bUploadPending || ShapeChanged();

	// One upload a frame across all volumes: an upload can stall the frame.
	// A volume packs and sweeps on its turn only, or many volumes would
	// each redo that work every frame while they wait.
	static int s_nUploadFrame = -1;
	C_NeoVolSmoke *pTurn = UploadTurn();
	if ( !m_bUploadPending || s_nUploadFrame == gpGlobals->framecount || ( pTurn && pTurn != this ) )
	{
		return;
	}

	PackNewCells();
	SweepLight();
	m_Atlas->Download();
	s_nUploadFrame = gpGlobals->framecount;
	m_bUploadPending = false;
	m_flNextUpload = now + 1.0f / NEO_VOLSMOKE_UPLOAD_HZ;
	if ( m_nPacked > 0 )
	{
		m_flUploadedFront = CNEOSmokeFill::DistToUnits( m_Fill.Dist( m_Fill.FinalizedCell( m_nPacked - 1 ) ) );
	}
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
void C_NeoVolSmoke::GetRenderBounds( Vector &mins, Vector &maxs )
{
	// The box that holds smoke, not the whole grid. The engine draws a
	// translucent renderable in the nearest leaf its bounds touch, after the
	// translucent world surfaces of that leaf and of every leaf behind it.
	// Tight bounds keep glass that stands in front of the smoke out of those
	// leaves, so it is drawn over the smoke and not under it.
	Vector lo = m_vecSmokeMin;
	Vector hi = m_vecSmokeMax;

	// Before the first cell fills there is no smoke box. The cover boxes
	// reach past the smoke the shader would draw, and can stand before it.
	const bool smokeBox = m_vecSmokeMax.x > m_vecSmokeMin.x;
	if ( !smokeBox )
	{
		lo = hi = m_vecSpawn;
		if ( m_nCoverBoxes > 0 )
		{
			lo = m_vecCoverMin[0];
			hi = m_vecCoverMax[0];
		}
	}

	mins = lo - GetRenderOrigin();
	maxs = hi - GetRenderOrigin();
	for ( int i = 0; i < m_nCoverBoxes; ++i )
	{
		VectorMin( mins, m_vecCoverMin[i] - GetRenderOrigin(), mins );
		VectorMax( maxs, m_vecCoverMax[i] - GetRenderOrigin(), maxs );
	}
}

// A volume is one renderable that a player can stand inside. Sorted by its
// center, a cloaked player between the camera and the center draws after the
// smoke and its tint outlines it through the smoke in front of it. By the
// nearest corner, everything inside the box draws first and the smoke covers
// it, while a translucent in clear air in front of the box still draws over
// the smoke.
bool C_NeoVolSmoke::SortDepth( IClientRenderable *pRenderable, const Vector &viewOrigin, const Vector &viewForward, float &depth )
{
	if ( s_Volumes.Count() == 0 )
	{
		return false;
	}

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

	const bool drawn = pVolume && ( pVolume->m_vecSmokeMax.x > pVolume->m_vecSmokeMin.x || pVolume->m_nCoverBoxes > 0 );
	if ( !drawn || !pVolume->m_bBoxFitted )
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
	if ( found )
	{
		pVar->SetVecValue( v.x, v.y, v.z );
	}
}

static void SetVec2Var( IMaterial *pMat, const char *name, float x, float y )
{
	bool found = false;
	IMaterialVar *pVar = pMat->FindVar( name, &found, false );
	if ( found )
	{
		pVar->SetVecValue( x, y );
	}
}

static void SetVec4Var( IMaterial *pMat, const char *name, float x, float y, float z, float w )
{
	bool found = false;
	IMaterialVar *pVar = pMat->FindVar( name, &found, false );
	if ( found )
	{
		pVar->SetVecValue( x, y, z, w );
	}
}

static void SetFloatVar( IMaterial *pMat, const char *name, float f )
{
	bool found = false;
	IMaterialVar *pVar = pMat->FindVar( name, &found, false );
	if ( found )
	{
		pVar->SetFloatValue( f );
	}
}

static void SetIntVar( IMaterial *pMat, const char *name, int i )
{
	bool found = false;
	IMaterialVar *pVar = pMat->FindVar( name, &found, false );
	if ( found )
	{
		pVar->SetIntValue( i );
	}
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
		const float brightest = Max( light.color.x, Max( light.color.y, light.color.z ) );
		if ( brightest > 1.0f )
		{
			light.color /= brightest;
		}

		light.color *= NEO_VOLSMOKE_DLIGHT_LINEAR * ( muzzle ? NEO_VOLSMOKE_FLASH_GAIN : 1.0f );
		light.origin = pLight->origin;
		light.invReachSqr = 1.0f / ( reach * reach );
		light.score = Max( light.color.x, Max( light.color.y, light.color.z ) ) * ( 1.0f - distSqr * light.invReachSqr );

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
			nKept = Min( nKept + 1, NEO_VOLSMOKE_MAX_DLIGHTS );
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
	SetFloatVar( pMat, "$age", age );
}

int C_NeoVolSmoke::DrawModel( int flags )
{
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

	// The cover boxes lead the front, so they can draw before the first
	// cell fills.
	if ( UsesCoverBoxes() )
	{
		if ( m_nCoverBoxes == 0 )
		{
			return 0;
		}

		CMatRenderContextPtr pRenderContext( materials );
		DrawCoverBoxes( pRenderContext );
		return 1;
	}

	if ( !m_Atlas.IsValid() )
	{
		return 0;
	}

	if ( !m_Material || m_Material->IsErrorMaterial() || !m_SlabMaterial || m_SlabMaterial->IsErrorMaterial() )
	{
		return 0;
	}

	// Nothing to draw before the first cell fills.
	if ( m_vecSmokeMax.x <= m_vecSmokeMin.x )
	{
		return 0;
	}

	CMatRenderContextPtr pRenderContext( materials );

	SetDrawParams( m_Material );
	SetDrawParams( m_SlabMaterial );

	VolSmokeDLight_t lights[NEO_VOLSMOKE_MAX_DLIGHTS];
	const int nLights = CollectDynamicLights( m_vecSmokeMin, m_vecSmokeMax, lights );
	SetDynamicLightParams( m_Material, lights, nLights );
	SetDynamicLightParams( m_SlabMaterial, lights, nLights );

	pRenderContext->MatrixMode( MATERIAL_MODEL );
	pRenderContext->PushMatrix();
	pRenderContext->LoadIdentity();

	// The sheet is built once a render, before any volume's slabs.
	if ( StartsRender() )
	{
		BuildOpaqueSheet( pRenderContext );
	}

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
		xMin = Min( xMin, p.x / p.z );
		xMax = Max( xMax, p.x / p.z );
		yMin = Min( yMin, p.y / p.z );
		yMax = Max( yMax, p.y / p.z );
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
	const float z0 = Max( lo, center.z - radius );
	const float z1 = Min( hi, center.z + radius );
	if ( z0 > z1 )
	{
		xMin = yMin = FLT_MAX;
		xMax = yMax = -FLT_MAX;
		return;
	}

	// Widest cross-section of the sphere inside the slab.
	const float off = ( center.z < z0 ) ? z0 - center.z : ( ( center.z > z1 ) ? center.z - z1 : 0.0f );
	const float reach = sqrtf( Max( radius * radius - off * off, 0.0f ) );

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
			sxMin = Min( sxMin, xs[k] / z );
			sxMax = Max( sxMax, xs[k] / z );
			syMin = Min( syMin, ys[k] / z );
			syMax = Max( syMax, ys[k] / z );
		}
	}

	xMin = Max( xMin, sxMin );
	xMax = Min( xMax, sxMax );
	yMin = Max( yMin, syMin );
	yMax = Min( yMax, syMax );
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
		const float g = Clamp( ( pos[a] - m_vecGridMin[a] ) / CNEOSmokeFill::CELL_SIZE - 0.5f, 0.0f, (float)( size[a] - 1 ) );
		base[a] = Min( (int)g, size[a] - 2 );
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

	const float edge = Min( depth / NEO_VOLSMOKE_EDGE, 1.0f );
	return weight * edge * edge * ( 3.0f - 2.0f * edge );
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
		t0 = Max( t0, Min( ta, tb ) );
		t1 = Min( t1, Max( ta, tb ) );
	}

	const float front = FrontRadius( gpGlobals->curtime - m_flSpawnTime );
	float path = 0.0f;
	for ( float t = t0; t < t1; t += NEO_VOLSMOKE_PATH_STEP )
	{
		const float step = Min( NEO_VOLSMOKE_PATH_STEP, t1 - t );
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

// How strongly a flash, or what it lights, shows through smoke of optical
// depth tau.
static float FlashShows( float tau )
{
	const float through = expf( -NEO_VOLSMOKE_FLASH_DEPTH_SHARE * tau );
	return Max( through - NEO_VOLSMOKE_FLASH_CUTOFF, 0.0f ) / ( 1.0f - NEO_VOLSMOKE_FLASH_CUTOFF );
}

// How strongly the flash point shows through smoke of optical depth tau.
static float FlashPointShows( float tau )
{
	const float depth = Clamp( tau / NEO_VOLSMOKE_FLASH_POINT_DEPTH, 0.0f, 1.0f );
	const float through = 1.0f - powf( depth, NEO_VOLSMOKE_FLASH_POINT_FALLOFF );
	const float beyond = RemapValClamped( tau, NEO_VOLSMOKE_FLASH_POINT_DEPTH, NEO_VOLSMOKE_FLASH_POINT_GONE, 0.0f, 1.0f );
	const float floorShows = NEO_VOLSMOKE_FLASH_POINT_FLOOR * ( 1.0f - SimpleSpline( beyond ) );
	return Max( through, floorShows );
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
	const float level = 255.0f * Clamp( strength, 0.0f, 1.0f );
	const unsigned char r = (unsigned char)( level * NEO_VOLSMOKE_FLASH_POINT_TINT.x );
	const unsigned char g = (unsigned char)( level * NEO_VOLSMOKE_FLASH_POINT_TINT.y );
	const unsigned char b = (unsigned char)( level * NEO_VOLSMOKE_FLASH_POINT_TINT.z );

	pRenderContext->MatrixMode( MATERIAL_MODEL );
	pRenderContext->PushMatrix();
	pRenderContext->LoadIdentity();
	pRenderContext->Bind( pMat );
	IMesh *pMesh = pRenderContext->GetDynamicMesh( true, nullptr, nullptr, pMat );

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
	render->SetBlend( Clamp( alpha, 0.0f, 1.0f ) );
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
	const float overlayClear = 1.0f - Clamp( g_SmokeFogOverlayAlpha, 0.0f, 1.0f );
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

		const float life = Clamp( ( pLight->die - gpGlobals->curtime ) / NEO_VOLSMOKE_FLASH_LIFE, 0.0f, 1.0f );
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
			silhouette[p] = Max( silhouette[p], NEO_VOLSMOKE_FIRE_SILHOUETTE * shown * hidden );
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
		silhouette[p] = Max( silhouette[p], NEO_VOLSMOKE_CLOAK_SILHOUETTE * shown * hidden );
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

// Opaque sheet: one grid of eye rays sums the smoke of every volume, and the
// view depth where the sum turns opaque hides every volume's slab tiles
// behind it. Overlapping clouds make the sum opaque sooner than any one of
// them. The sheet is not written to the depth buffer: sprites, tracers and
// glass drawn after the smoke would be cut along its tile edges.
struct VolSmokeSheetVolume_t
{
	const C_NeoVolSmoke *pVol;
	Vector boxMin;
	Vector boxMax;
	Vector sphereCenter;
	float sphereRadius;
	float front;
	float bite;
	float extinction;
};

struct VolSmokeSheetSpan_t
{
	int vol;
	float t0;
	float t1;
	float sigma;
};

// The sheet hides everything behind it, so its opaque sum is taken with a
// margin over the slab cull's: the coarse march's midpoint samples can
// overshoot where a grazing ray crosses the bumps at the cloud's edge.
static constexpr float NEO_VOLSMOKE_SHEET_OPAQUE = 2.0f * NEO_VOLSMOKE_CULL_OPAQUE;

// True for the first volume drawn in a render. The frame count stops while
// the game is paused, but each volume draws once a render, so a volume that
// already drew in the current render means a new one has started.
bool C_NeoVolSmoke::StartsRender()
{
	const bool start = m_nRenderSerial == s_nSheetSerial || s_nSheetFrame != gpGlobals->framecount;
	if ( start )
	{
		++s_nSheetSerial;
		s_nSheetFrame = gpGlobals->framecount;
	}

	m_nRenderSerial = s_nSheetSerial;
	return start;
}

// View depth at which the summed lower-bound smoke of all volumes along an
// eye ray becomes opaque, or FLT_MAX if the ray leaves the smoke first. Each
// volume's own depth at the slab cull's sum goes to own[], FLT_MAX where it is
// not reached in front of the sheet (the sheet hides what lies behind it).
float C_NeoVolSmoke::SheetOpaqueDepth( const VolSmokeSheetVolume_t *vols, int nVols, const Vector &eye, const Vector &dir, float *own )
{
	const float len = dir.Length();
	const Vector unit = dir / len;

	CUtlVectorFixedGrowable< VolSmokeSheetSpan_t, 16 > spans;
	float tStart = FLT_MAX;
	float tEnd = 0.0f;
	for ( int v = 0; v < nVols; ++v )
	{
		own[v] = FLT_MAX;

		// No smoke is drawn outside the smoke sphere; a ray that passes over
		// a cloud's top ends its span, and its upper bound, there.
		const Vector rel = vols[v].sphereCenter - eye;
		const float mid = DotProduct( rel, unit );
		const float missSqr = rel.LengthSqr() - mid * mid;
		const float radiusSqr = vols[v].sphereRadius * vols[v].sphereRadius;
		if ( missSqr >= radiusSqr )
		{
			continue;
		}

		const float half = sqrtf( radiusSqr - missSqr );
		float t0 = Max( 0.0f, mid - half );
		float t1 = mid + half;
		for ( int a = 0; a < 3 && t0 < t1; ++a )
		{
			if ( fabsf( unit[a] ) < 1.0e-6f )
			{
				if ( eye[a] < vols[v].boxMin[a] || eye[a] > vols[v].boxMax[a] )
				{
					t1 = 0.0f;
				}
				continue;
			}

			const float ta = ( vols[v].boxMin[a] - eye[a] ) / unit[a];
			const float tb = ( vols[v].boxMax[a] - eye[a] ) / unit[a];
			t0 = Max( t0, Min( ta, tb ) );
			t1 = Min( t1, Max( ta, tb ) );
		}

		if ( t0 < t1 )
		{
			const VolSmokeSheetSpan_t span = { v, t0, t1, 0.0f };
			spans.AddToTail( span );
			tStart = Min( tStart, t0 );
			tEnd = Max( tEnd, t1 );
		}
	}

	float sigma = 0.0f;
	for ( float t = tStart; t < tEnd; t += NEO_VOLSMOKE_CULL_STEP )
	{
		const float tNext = Min( t + NEO_VOLSMOKE_CULL_STEP, tEnd );

		// Cell weights are at most 1, which bounds what the rest of the ray can
		// add, in sum and to each volume.
		float most = sigma;
		bool ownLeft = false;
		for ( const VolSmokeSheetSpan_t &span : spans )
		{
			const float rest = vols[span.vol].extinction * Max( 0.0f, span.t1 - Max( t, span.t0 ) );
			most += rest;
			ownLeft = ownLeft || ( own[span.vol] == FLT_MAX && span.sigma + rest >= NEO_VOLSMOKE_CULL_OPAQUE );
		}
		if ( most < NEO_VOLSMOKE_SHEET_OPAQUE && !ownLeft )
		{
			return FLT_MAX;
		}

		// Each volume adds only the part of the step inside its box.
		for ( VolSmokeSheetSpan_t &span : spans )
		{
			const float a = Max( t, span.t0 );
			const float b = Min( tNext, span.t1 );
			if ( a >= b )
			{
				continue;
			}

			const VolSmokeSheetVolume_t &vol = vols[span.vol];
			const float add = vol.pVol->DensityLowerBound( eye + unit * ( 0.5f * ( a + b ) ), vol.front, vol.bite ) * vol.extinction * ( b - a );
			sigma += add;
			span.sigma += add;
			if ( own[span.vol] == FLT_MAX && span.sigma >= NEO_VOLSMOKE_CULL_OPAQUE )
			{
				own[span.vol] = tNext / len;
			}
		}

		if ( sigma >= NEO_VOLSMOKE_SHEET_OPAQUE )
		{
			// The ray direction has view depth 1 per len along it.
			return tNext / len;
		}
	}

	return FLT_MAX;
}

void C_NeoVolSmoke::BuildOpaqueSheet( IMatRenderContext *pRenderContext )
{
	s_CullVolumes.RemoveAll();

	VMatrix proj;
	pRenderContext->GetMatrix( MATERIAL_PROJECTION, &proj );
	int vx, vy, width, height;
	pRenderContext->GetViewport( vx, vy, width, height );
	if ( proj[0][0] <= 0.0f || proj[1][1] <= 0.0f || width <= 0 || height <= 0 )
	{
		return;
	}

	const Vector &eye = CurrentViewOrigin();
	const Vector &fwd = CurrentViewForward();
	const Vector &right = CurrentViewRight();
	const Vector &up = CurrentViewUp();
	const float tanX = NEO_VOLSMOKE_CULL_FRUSTUM_MARGIN / proj[0][0];
	const float tanY = NEO_VOLSMOKE_CULL_FRUSTUM_MARGIN / proj[1][1];
	const float bite = NEO_VOLSMOKE_PUFF_BITE;

	// The volumes that can add smoke, and the screen rectangle that holds them.
	CUtlVectorFixedGrowable< VolSmokeSheetVolume_t, 16 > vols;
	float most = 0.0f;
	float x0 = FLT_MAX, x1 = -FLT_MAX, y0 = FLT_MAX, y1 = -FLT_MAX;
	for ( const C_NeoVolSmoke *pVol : s_Volumes )
	{
		if ( !pVol->m_pRegen || pVol->m_vecSmokeMax.x <= pVol->m_vecSmokeMin.x )
		{
			continue;
		}

		const Vector &a = pVol->m_vecSmokeMin;
		const Vector &b = pVol->m_vecSmokeMax;
		Vector view[8];
		float zMin = FLT_MAX;
		float zMax = -FLT_MAX;
		for ( int i = 0; i < 8; ++i )
		{
			const Vector corner( ( i & 1 ) ? b.x : a.x, ( i & 2 ) ? b.y : a.y, ( i & 4 ) ? b.z : a.z );
			const Vector rel = corner - eye;
			view[i].Init( DotProduct( rel, right ), DotProduct( rel, up ), DotProduct( rel, fwd ) );
			zMin = Min( zMin, view[i].z );
			zMax = Max( zMax, view[i].z );
		}
		if ( zMax <= NEO_VOLSMOKE_CULL_MIN_DEPTH )
		{
			continue;
		}

		float xMin, xMax, yMin, yMax;
		SlabScreenRect( view, Max( zMin, NEO_VOLSMOKE_CULL_MIN_DEPTH ), zMax, xMin, xMax, yMin, yMax );
		x0 = Min( x0, xMin );
		x1 = Max( x1, xMax );
		y0 = Min( y0, yMin );
		y1 = Max( y1, yMax );

		Vector center;
		const float radius = pVol->SmokeSphere( center );
		const VolSmokeSheetVolume_t vol = { pVol, a, b, center, radius, pVol->FrontRadius( gpGlobals->curtime - pVol->m_flSpawnTime ), bite, pVol->DrawExtinction() };
		vols.AddToTail( vol );
		most += vol.extinction * ( b - a ).Length();
	}

	// Thin smoke (bloom, fade) never reaches the opaque depth.
	if ( most < NEO_VOLSMOKE_CULL_OPAQUE )
	{
		return;
	}
	const bool sheet = most >= NEO_VOLSMOKE_SHEET_OPAQUE;

	x0 = Max( x0, -tanX );
	x1 = Min( x1, tanX );
	y0 = Max( y0, -tanY );
	y1 = Min( y1, tanY );
	if ( x0 >= x1 || y0 >= y1 )
	{
		return;
	}

	const int cols = Clamp( (int)ceilf( ( x1 - x0 ) / ( 2.0f * tanX ) * width / NEO_VOLSMOKE_CULL_TILE_PX ), 1, NEO_VOLSMOKE_CULL_MAX_COLS );
	const int rows = Clamp( (int)ceilf( ( y1 - y0 ) / ( 2.0f * tanY ) * height / NEO_VOLSMOKE_CULL_TILE_PX ), 1, NEO_VOLSMOKE_CULL_MAX_ROWS );
	float xs[NEO_VOLSMOKE_CULL_MAX_COLS + 1];
	float ys[NEO_VOLSMOKE_CULL_MAX_ROWS + 1];
	for ( int i = 0; i <= cols; ++i )
	{
		xs[i] = ( i == cols ) ? x1 : x0 + ( x1 - x0 ) * i / cols;
	}
	for ( int j = 0; j <= rows; ++j )
	{
		ys[j] = ( j == rows ) ? y1 : y0 + ( y1 - y0 ) * j / rows;
	}

	const int nVols = vols.Count();
	s_nCullSerial = s_nSheetSerial;
	s_nCullCols = cols;
	s_nCullRows = rows;
	V_memcpy( s_flCullXs, xs, sizeof( float ) * ( cols + 1 ) );
	V_memcpy( s_flCullYs, ys, sizeof( float ) * ( rows + 1 ) );
	for ( const VolSmokeSheetVolume_t &vol : vols )
	{
		s_CullVolumes.AddToTail( vol.pVol );
	}
	s_flCullCorner.SetCount( ( rows + 1 ) * ( cols + 1 ) * nVols );
	for ( int j = 0; j < rows; ++j )
	{
		for ( int i = 0; i < cols; ++i )
		{
			s_flSheetDepth[j][i] = FLT_MAX;
		}
	}

	float corner[NEO_VOLSMOKE_CULL_MAX_ROWS + 1][NEO_VOLSMOKE_CULL_MAX_COLS + 1];
	for ( int j = 0; j <= rows; ++j )
	{
		for ( int i = 0; i <= cols; ++i )
		{
			float *own = s_flCullCorner.Base() + ( j * ( cols + 1 ) + i ) * nVols;
			corner[j][i] = SheetOpaqueDepth( vols.Base(), nVols, eye, fwd + right * xs[i] + up * ys[j], own );
		}
	}

	if ( !sheet )
	{
		return;
	}

	s_flCenterOwn.SetCount( nVols );

	// Union smoke is not convex: the center ray guards a gap between opaque
	// corners.
	float tileDepth[NEO_VOLSMOKE_CULL_MAX_ROWS][NEO_VOLSMOKE_CULL_MAX_COLS];
	for ( int j = 0; j < rows; ++j )
	{
		for ( int i = 0; i < cols; ++i )
		{
			float depth = Max( Max( corner[j][i], corner[j][i + 1] ), Max( corner[j + 1][i], corner[j + 1][i + 1] ) );
			if ( depth != FLT_MAX )
			{
				const float xc = 0.5f * ( xs[i] + xs[i + 1] );
				const float yc = 0.5f * ( ys[j] + ys[j + 1] );
				depth = Max( depth, SheetOpaqueDepth( vols.Base(), nVols, eye, fwd + right * xc + up * yc, s_flCenterOwn.Base() ) );
			}

			tileDepth[j][i] = depth;
		}
	}

	for ( int j = 0; j < rows; ++j )
	{
		for ( int i = 0; i < cols; ++i )
		{
			const float depth = tileDepth[j][i];
			if ( depth == FLT_MAX )
			{
				continue;
			}

			s_flSheetDepth[j][i] = depth;
		}
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
bool C_NeoVolSmoke::BuildCullGrid( VolSmokeCullGrid_t &grid ) const
{
	if ( s_nCullSerial != s_nSheetSerial )
	{
		return false;
	}

	const int v = s_CullVolumes.Find( this );
	if ( v < 0 )
	{
		return false;
	}

	// Chord length through a convex cloud is smallest at a tile's corners.
	const int nVols = s_CullVolumes.Count();
	const auto own = [&]( int j, int i ) { return s_flCullCorner[( j * ( s_nCullCols + 1 ) + i ) * nVols + v]; };
	bool any = false;
	for ( int j = 0; j < s_nCullRows; ++j )
	{
		for ( int i = 0; i < s_nCullCols; ++i )
		{
			const float ownDepth = Max( Max( own( j, i ), own( j, i + 1 ) ), Max( own( j + 1, i ), own( j + 1, i + 1 ) ) );
			grid.depth[j][i] = Min( ownDepth, s_flSheetDepth[j][i] );
			any = any || grid.depth[j][i] != FLT_MAX;
		}
	}
	if ( !any )
	{
		return false;
	}

	grid.cols = s_nCullCols;
	grid.rows = s_nCullRows;
	V_memcpy( grid.xs, s_flCullXs, sizeof( float ) * ( s_nCullCols + 1 ) );
	V_memcpy( grid.ys, s_flCullYs, sizeof( float ) * ( s_nCullRows + 1 ) );
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
		zMin = Min( zMin, view[i].z );
		zMax = Max( zMax, view[i].z );
	}

	Vector seed;
	const float sphereRadius = SmokeSphere( seed );
	const Vector seedRel = seed - eye;
	const Vector sphereView( DotProduct( seedRel, right ), DotProduct( seedRel, up ), DotProduct( seedRel, fwd ) );

	if ( zMax <= NEO_VOLSMOKE_NEAR_RANGE )
	{
		return;
	}

	// Slab ends are whole units: the shader rounds the range it is given, and
	// the near pass ends at NEAR_RANGE exactly.
	float bounds[NEO_VOLSMOKE_MAX_SLABS + 1];
	int nSlabs = 0;
	bounds[0] = Max( NEO_VOLSMOKE_NEAR_RANGE, floorf( zMin ) );
	const float zEnd = ceilf( zMax );
	while ( bounds[nSlabs] < zEnd )
	{
		const float z = bounds[nSlabs];
		float next = ceilf( z + Max( NEO_VOLSMOKE_SLAB_MIN, NEO_VOLSMOKE_SLAB_GROWTH * z ) );
		if ( nSlabs == NEO_VOLSMOKE_MAX_SLABS - 1 || next > zEnd )
		{
			next = zEnd;
		}

		bounds[++nSlabs] = next;
	}

	VolSmokeCullGrid_t grid;
	const bool cull = BuildCullGrid( grid );

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
		for ( int s = nSlabs - 1; s >= 0; --s )
		{
			const float lo = bounds[s];
			float xMin, xMax, yMin, yMax;
			SlabScreenRect( view, lo, bounds[s + 1], xMin, xMax, yMin, yMax );
			SphereSlabClip( sphereView, sphereRadius, lo, bounds[s + 1], xMin, xMax, yMin, yMax );
			xMin = Max( xMin, -NEO_VOLSMOKE_SLAB_MAX_TAN );
			yMin = Max( yMin, -NEO_VOLSMOKE_SLAB_MAX_TAN );
			xMax = Min( xMax, NEO_VOLSMOKE_SLAB_MAX_TAN );
			yMax = Min( yMax, NEO_VOLSMOKE_SLAB_MAX_TAN );
			if ( !useCull )
			{
				addQuad( xMin, xMax, yMin, yMax, s );
				continue;
			}

			// Each run of kept tiles in a row becomes one quad. The grid covers
			// the visible part of every slab rectangle; the rest is off screen.
			for ( int j = 0; j < grid.rows; ++j )
			{
				const float y0 = Max( grid.ys[j], yMin );
				const float y1 = Min( grid.ys[j + 1], yMax );
				if ( y0 >= y1 )
				{
					continue;
				}

				int runStart = -1;
				for ( int i = 0; i <= grid.cols; ++i )
				{
					const bool keep = i < grid.cols && lo < grid.depth[j][i];

					if ( keep && runStart < 0 )
					{
						runStart = i;
					}
					else if ( !keep && runStart >= 0 )
					{
						addQuad( Max( grid.xs[runStart], xMin ), Min( grid.xs[i], xMax ), y0, y1, s );
						runStart = -1;
					}
				}
			}
		}
	};

	pRenderContext->Bind( m_SlabMaterial, (IClientRenderable *)this );
	IMesh *pMesh = pRenderContext->GetDynamicMesh( true, nullptr, nullptr, m_SlabMaterial );

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
		const float zc = 0.5f * ( lo + hi ); // depth-tested at its middle
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
	IMesh *pMesh = pRenderContext->GetDynamicMesh( true, nullptr, nullptr, m_Material );

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
		meshBuilder.TexCoord2f( 0, 0.0f, NEO_VOLSMOKE_NEAR_RANGE );
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

static IMaterial *VolSmokeCoverMaterial()
{
	static IMaterial *s_pMat = nullptr;
	if ( !s_pMat )
	{
		KeyValues *pVMT = new KeyValues( "UnlitGeneric" );
		pVMT->SetInt( "$vertexcolor", 1 );
		pVMT->SetInt( "$nocull", 1 );
		s_pMat = materials->CreateMaterial( "__neo_volsmoke_cover", pVMT );
	}

	return s_pMat;
}

// Without the shader, the cloud's own boxes say where it can block sight.
// The shader picks its fallback by the hardware alone, so a material that is
// never drawn answers for every volume.
bool C_NeoVolSmoke::UsesCoverBoxes()
{
	static IMaterial *s_pProbe = nullptr;
	if ( !s_pProbe )
	{
		s_pProbe = materials->CreateMaterial( "__neo_volsmoke_probe", new KeyValues( "Neo_VolSmoke" ) );
	}

	return !s_pProbe || V_strcmp( s_pProbe->GetShaderName(), "Neo_VolSmoke" ) != 0;
}

// An opaque box has no face between a camera inside it and the smoke, so the
// full inside overlay hides the view there.
void C_NeoVolSmoke::AddCoverOverlay()
{
	if ( InCoverBox( MainViewOrigin(), NEO_VOLSMOKE_COVER_NEAR_MARGIN ) )
	{
		AddSmokeFogOverlayColored( 1.0f, DisplayColor() );
	}
}

// How far each gas cell's smoke can reach, for the cover boxes. Only the
// fill: the fill job runs it, so clouds whose fills end in one frame do not
// stall that frame.
void C_NeoVolSmoke::BuildCoverReach()
{
	int box[3];
	m_Fill.BoxSize( box );
	CUtlVector< float > cornerDist;
	cornerDist.SetCount( box[0] * box[1] * box[2] );
	m_CoverReach.SetCount( m_Fill.FinalizedCount() );
	VolSmokeCoverReach( m_Fill, DistRange(), m_CoverReach.Base(), cornerDist.Base() );
}

// Rebuilds the cover boxes while the cloud blooms. They hold every gas cell
// that can have smoke near it by the front a little ahead, so they cover
// all the smoke the server's sight test reads. Once the cloud is full grown
// they hold all its cells and stay until the volume is released.
void C_NeoVolSmoke::UpdateCoverBoxes( float now )
{
	if ( m_bCoverFinal || now < m_flNextCover )
	{
		return;
	}

	// A fill without gas leaves the server's field without smoke to block
	// sight, so there is nothing to cover.
	if ( m_Fill.FinalizedCount() == 0 )
	{
		m_nCoverBoxes = 0;
		m_bCoverFinal = true;
		return;
	}

	const int nCells = m_nBox[0] * m_nBox[1] * m_nBox[2];
	if ( m_CoverReach.Count() == 0 )
	{
		BuildCoverReach();
	}

	// The server's front, not FrontRadius: the shader's upload limit does
	// not hold the server back.
	const float age = now - m_flSpawnTime;
	const float front = CNEOSmokeFill::DistToUnits( m_Fill.FrontDist() ) * VolSmokeBloom( age + NEO_VOLSMOKE_COVER_LEAD );

	// The volumes rebuild on the main thread only, so they share the mask.
	static CUtlVector< unsigned char > s_Mask;
	s_Mask.SetCount( nCells );
	VolSmokeCoverMask( m_Fill, m_CoverReach.Base(), front, s_Mask.Base() );

	VolSmokeCoverBox_t boxes[NEO_VOLSMOKE_COVER_BOXES];
	m_nCoverBoxes = VolSmokeCoverSplit( s_Mask.Base(), m_nBox, boxes, NEO_VOLSMOKE_COVER_BOXES );

	const float cell = CNEOSmokeFill::CELL_SIZE;
	const Vector skin( NEO_VOLSMOKE_COVER_SKIN, NEO_VOLSMOKE_COVER_SKIN, NEO_VOLSMOKE_COVER_SKIN );
	for ( int i = 0; i < m_nCoverBoxes; ++i )
	{
		const VolSmokeCoverBox_t &box = boxes[i];
		m_vecCoverMin[i] = m_vecGridMin + Vector( box.lo[0], box.lo[1], box.lo[2] ) * cell - skin;
		m_vecCoverMax[i] = m_vecGridMin + Vector( box.hi[0] + 1, box.hi[1] + 1, box.hi[2] + 1 ) * cell + skin;
	}

	// The first build is the one with no next time set yet.
	float period = NEO_VOLSMOKE_COVER_PERIOD;
	if ( m_flNextCover == 0.0f )
	{
		static int s_nPhase = 0;
		s_nPhase = s_nPhase % NEO_VOLSMOKE_COVER_PHASES + 1;
		period *= static_cast< float >( s_nPhase ) / NEO_VOLSMOKE_COVER_PHASES;
	}

	m_flNextCover = now + period;
	m_bCoverFinal = age >= NEO_VOLSMOKE_BLOOM_TIME;
	if ( m_bCoverFinal )
	{
		m_CoverReach.Purge();
	}

	if ( GetRenderHandle() != INVALID_CLIENT_RENDER_HANDLE )
	{
		ClientLeafSystem()->RenderableChanged( GetRenderHandle() );
	}
}

bool C_NeoVolSmoke::InCoverBox( const Vector &point, float margin ) const
{
	const Vector grow( margin, margin, margin );
	for ( int i = 0; i < m_nCoverBoxes; ++i )
	{
		if ( IsPointInBox( point, m_vecCoverMin[i] - grow, m_vecCoverMax[i] + grow ) )
		{
			return true;
		}
	}

	return false;
}

// Without the shader, opaque boxes over every place the smoke can be. They
// hide more than the cloud would, so a client cannot gain by losing the
// shader. From inside, the full inside overlay hides the view instead.
void C_NeoVolSmoke::DrawCoverBoxes( IMatRenderContext *pRenderContext )
{
	IMaterial *pMat = VolSmokeCoverMaterial();
	if ( !pMat || pMat->IsErrorMaterial() )
	{
		return;
	}

	const Vector display = DisplayColor();
	unsigned char rgb[3];
	for ( int k = 0; k < 3; ++k )
	{
		rgb[k] = VolSmokeByte( display[k] );
	}

	static const unsigned short indices[36] =
	{
		0, 2, 1,  0, 3, 2,
		4, 5, 6,  4, 6, 7,
		0, 1, 5,  0, 5, 4,
		2, 3, 7,  2, 7, 6,
		1, 2, 6,  1, 6, 5,
		3, 0, 4,  3, 4, 7,
	};

	pRenderContext->MatrixMode( MATERIAL_MODEL );
	pRenderContext->PushMatrix();
	pRenderContext->LoadIdentity();
	pRenderContext->Bind( pMat );
	IMesh *pMesh = pRenderContext->GetDynamicMesh( true, nullptr, nullptr, pMat );

	CMeshBuilder meshBuilder;
	meshBuilder.Begin( pMesh, MATERIAL_TRIANGLES, 8 * m_nCoverBoxes, 36 * m_nCoverBoxes );
	for ( int box = 0; box < m_nCoverBoxes; ++box )
	{
		const Vector &a = m_vecCoverMin[box];
		const Vector &b = m_vecCoverMax[box];
		const Vector corners[8] =
		{
			Vector( a.x, a.y, a.z ), Vector( b.x, a.y, a.z ), Vector( b.x, b.y, a.z ), Vector( a.x, b.y, a.z ),
			Vector( a.x, a.y, b.z ), Vector( b.x, a.y, b.z ), Vector( b.x, b.y, b.z ), Vector( a.x, b.y, b.z ),
		};
		for ( int i = 0; i < 8; ++i )
		{
			meshBuilder.Position3fv( corners[i].Base() );
			meshBuilder.Color4ub( rgb[0], rgb[1], rgb[2], 255 );
			meshBuilder.AdvanceVertex();
		}
	}

	for ( int box = 0; box < m_nCoverBoxes; ++box )
	{
		for ( int i = 0; i < 36; ++i )
		{
			meshBuilder.Index( indices[i] + 8 * box );
			meshBuilder.AdvanceIndex();
		}
	}

	meshBuilder.End();
	pMesh->Draw();

	pRenderContext->MatrixMode( MATERIAL_MODEL );
	pRenderContext->PopMatrix();
}
