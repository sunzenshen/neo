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

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

// ---------------------------------------------------------------------------
// ConVars
// ---------------------------------------------------------------------------
static ConVar neo_volsmoke_grid( "neo_volsmoke_grid", "32", FCVAR_CHEAT, "Cells per axis (16, 32 or 64).", true, 16.0f, true, 64.0f );
static ConVar neo_volsmoke_extent( "neo_volsmoke_extent", "192", FCVAR_CHEAT, "Half-size of a smoke volume's AABB in world units.", true, 32.0f, true, 1024.0f );
static ConVar neo_volsmoke_lifetime( "neo_volsmoke_lifetime", "25", FCVAR_CHEAT, "Seconds until the volume has fully dissipated.", true, 1.0f, true, 120.0f );
static ConVar neo_volsmoke_fill_rate( "neo_volsmoke_fill_rate", "300", FCVAR_CHEAT, "Max cells reached by the flood fill per frame (bloom speed).", true, 1.0f, true, 100000.0f );
static ConVar neo_volsmoke_fill_fraction( "neo_volsmoke_fill_fraction", "1.0", FCVAR_CHEAT, "Gas volume as a fraction of the inscribed sphere. In tight spaces the smoke spreads further instead.", true, 0.05f, true, 4.0f );
static ConVar neo_volsmoke_ramp( "neo_volsmoke_ramp", "1.5", FCVAR_CHEAT, "Seconds for a reached cell to ramp to full density.", true, 0.05f, true, 10.0f );
static ConVar neo_volsmoke_dissipate( "neo_volsmoke_dissipate", "5.0", FCVAR_CHEAT, "Seconds of fade-out at the end of life.", true, 0.1f, true, 30.0f );
static ConVar neo_volsmoke_regrow( "neo_volsmoke_regrow", "4.0", FCVAR_CHEAT, "Seconds for a carved cell to refill.", true, 0.1f, true, 60.0f );
static ConVar neo_volsmoke_carve_radius( "neo_volsmoke_carve_radius", "24", FCVAR_CHEAT, "Capsule radius carved by neo_volsmoke_carve.", true, 1.0f, true, 512.0f );
static ConVar neo_volsmoke_explode_radius( "neo_volsmoke_explode_radius", "110", FCVAR_CHEAT, "Sphere radius carved by neo_volsmoke_explode.", true, 1.0f, true, 1024.0f );
static ConVar neo_volsmoke_lightdir( "neo_volsmoke_lightdir", "0.3 0.2 1.0", FCVAR_CHEAT, "Direction *toward* the light used for the baked light sweep." );
static ConVar neo_volsmoke_light_k( "neo_volsmoke_light_k", "0.05", FCVAR_CHEAT, "Extinction per world unit used by the light sweep.", true, 0.0f, true, 1.0f );
static ConVar neo_volsmoke_light_sweep_hz( "neo_volsmoke_light_sweep_hz", "15", FCVAR_CHEAT, "How often per second to recompute the light sweep.", true, 1.0f, true, 120.0f );
static ConVar neo_volsmoke_ambient_scale( "neo_volsmoke_ambient_scale", "0.9", FCVAR_CHEAT, "Multiplier on sampled ambient light.", true, 0.0f, true, 8.0f );
static ConVar neo_volsmoke_light_scale( "neo_volsmoke_light_scale", "0.8", FCVAR_CHEAT, "Multiplier on the directional light term.", true, 0.0f, true, 8.0f );
static ConVar neo_volsmoke_extinction( "neo_volsmoke_extinction", "0.06", FCVAR_CHEAT, "Shader extinction per world unit at density 1.", true, 0.0f, true, 2.0f );
static ConVar neo_volsmoke_steps( "neo_volsmoke_steps", "32", FCVAR_CHEAT, "Raymarch steps across the box.", true, 4.0f, true, 64.0f );
static ConVar neo_volsmoke_noise_amp( "neo_volsmoke_noise_amp", "0.35", FCVAR_CHEAT, "Edge erosion noise amplitude.", true, 0.0f, true, 2.0f );
static ConVar neo_volsmoke_noise_scale( "neo_volsmoke_noise_scale", "0.045", FCVAR_CHEAT, "Noise frequency (1/world units).", true, 0.0f, true, 1.0f );
static ConVar neo_volsmoke_jitter( "neo_volsmoke_jitter", "1", FCVAR_CHEAT, "Per-pixel step jitter (grain) instead of banding." );
static ConVar neo_volsmoke_debug( "neo_volsmoke_debug", "0", FCVAR_CHEAT, "0 off, 1 coverage, 2 scene depth as read by the shader, 3 box hit." );
static ConVar neo_volsmoke_draw( "neo_volsmoke_draw", "1", FCVAR_CHEAT, "Draw smoke volumes." );
static ConVar neo_volsmoke_material( "neo_volsmoke_material", "dev/neo_volsmoke", FCVAR_CHEAT, "Material using the Neo_VolSmoke shader." );

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

static int RoundGrid( int n )
{
	if ( n <= 16 ) return 16;
	if ( n <= 32 ) return 32;
	return 64;
}

static void AtlasLayout( int n, int &cols, int &rows )
{
	// Keep both dimensions a power of two: 16 -> 4x4, 32 -> 8x4, 64 -> 8x8.
	cols = ( n >= 64 ) ? 8 : ( n >= 32 ? 8 : 4 );
	rows = n / cols;
}

// ---------------------------------------------------------------------------
C_NeoVolSmoke::C_NeoVolSmoke()
	: m_nId( s_nNextId++ ), m_nN( 0 ), m_nAtlasCols( 0 ), m_nAtlasRows( 0 ), m_flCellSize( 1.0f ),
	  m_flSpawnTime( 0 ), m_flLifetime( 1 ), m_nFrontierHead( 0 ), m_nFilledCells( 0 ),
	  m_nMaxCells( 0 ), m_nMaxDepth( 0 ), m_bFillDone( false ), m_pRegen( NULL ),
	  m_bAtlasDirty( true ), m_flNextLightSweep( 0 )
{
}

C_NeoVolSmoke::~C_NeoVolSmoke()
{
	Shutdown();
	s_Volumes.FindAndRemove( this );
}

C_NeoVolSmoke *C_NeoVolSmoke::Create( const Vector &origin, float extent, float lifetime )
{
	C_NeoVolSmoke *pVol = new C_NeoVolSmoke();
	pVol->Init( origin, extent, lifetime );

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

void C_NeoVolSmoke::RemoveAll()
{
	// Release() removes the entity from the list via the destructor.
	while ( s_Volumes.Count() > 0 )
	{
		s_Volumes[0]->Release();
	}
}

// ---------------------------------------------------------------------------
void C_NeoVolSmoke::Init( const Vector &origin, float extent, float lifetime )
{
	m_nN = RoundGrid( neo_volsmoke_grid.GetInt() );
	AtlasLayout( m_nN, m_nAtlasCols, m_nAtlasRows );

	m_vecSpawn    = origin;
	m_vecGridMin  = origin - Vector( extent, extent, extent );
	m_vecGridMax  = origin + Vector( extent, extent, extent );
	m_flCellSize  = ( 2.0f * extent ) / (float)m_nN;
	m_flSpawnTime = gpGlobals->curtime;
	m_flLifetime  = lifetime;

	SetAbsOrigin( origin );
	SetCollisionBounds( Vector( -extent, -extent, -extent ), Vector( extent, extent, extent ) );

	const int cells = m_nN * m_nN * m_nN;
	m_Reach.SetCount( cells );
	m_FillTime.SetCount( cells );
	m_CarveTime.SetCount( cells );
	m_Noise.SetCount( cells );
	m_Density.SetCount( cells );
	m_Light.SetCount( cells );

	for ( int i = 0; i < cells; ++i )
	{
		m_Reach[i]     = 255;
		m_FillTime[i]  = 0.0f;
		m_CarveTime[i] = -1.0f;
		m_Noise[i]     = (unsigned char)RandomInt( 0, 255 );
		m_Density[i]   = 0.0f;
		m_Light[i]     = 1.0f;
	}

	// Gas budget: fraction of the inscribed sphere, in cells. In corridors the
	// same budget gets spent on reaching further.
	const float r = 0.5f * (float)m_nN;
	m_nMaxDepth = MAX( 1, (int)r - 1 );
	m_nMaxCells = (int)( neo_volsmoke_fill_fraction.GetFloat() * ( 4.0f / 3.0f ) * M_PI_F * r * r * r );
	m_nMaxCells = clamp( m_nMaxCells, 1, cells );

	// Seed cell: the cell containing the spawn point. If that is inside a
	// solid (grenade resting against a wall), walk upward a little.
	int sx = m_nN / 2, sy = m_nN / 2, sz = m_nN / 2;
	for ( int tries = 0; tries < 4; ++tries )
	{
		if ( !( enginetrace->GetPointContents( CellCenter( sx, sy, sz ) ) & CONTENTS_SOLID ) )
			break;
		sz = MIN( sz + 1, m_nN - 1 );
	}
	const int seed = Idx( sx, sy, sz );
	m_Reach[seed]    = 0;
	m_FillTime[seed] = gpGlobals->curtime;
	m_Frontier.AddToTail( seed );
	m_nFrontierHead = 0;
	m_nFilledCells  = 1;
	m_bFillDone     = false;

	// GPU resources
	const int atlasW = m_nAtlasCols * m_nN;
	const int atlasH = m_nAtlasRows * m_nN;

	char texName[64];
	Q_snprintf( texName, sizeof( texName ), "_neo_volsmoke_%d", m_nId );

	ITexture *pTex = materials->CreateProceduralTexture( texName, TEXTURE_GROUP_OTHER, atlasW, atlasH, IMAGE_FORMAT_BGRA8888,
		TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT | TEXTUREFLAGS_NOMIP | TEXTUREFLAGS_NOLOD | TEXTUREFLAGS_PROCEDURAL | TEXTUREFLAGS_SINGLECOPY );
	if ( pTex && !IsErrorTexture( pTex ) )
	{
		m_pRegen = new CNeoVolSmokeRegenerator( atlasW, atlasH );
		pTex->SetTextureRegenerator( m_pRegen );
		m_Atlas.Init( pTex );
	}
	else
	{
		Warning( "C_NeoVolSmoke: failed to create procedural texture %s\n", texName );
	}

	m_Material.Init( neo_volsmoke_material.GetString(), TEXTURE_GROUP_OTHER );
	if ( !m_Material || m_Material->IsErrorMaterial() )
	{
		Warning( "C_NeoVolSmoke: material %s missing (needs the Neo_VolSmoke shader)\n", neo_volsmoke_material.GetString() );
	}

	m_bAtlasDirty = true;
	m_flNextLightSweep = 0.0f;
}

void C_NeoVolSmoke::Shutdown()
{
	if ( m_Atlas.IsValid() )
	{
		m_Atlas->SetTextureRegenerator( NULL ); // calls Release() on m_pRegen
		m_pRegen = NULL;
		m_Atlas.Shutdown();
	}
	m_Material.Shutdown();
}

// ---------------------------------------------------------------------------
// Simulation
// ---------------------------------------------------------------------------
bool C_NeoVolSmoke::CellOpen( const Vector &from, const Vector &to ) const
{
	if ( enginetrace->GetPointContents( to ) & CONTENTS_SOLID )
		return false;

	trace_t tr;
	UTIL_TraceLine( from, to, MASK_SOLID_BRUSHONLY, NULL, COLLISION_GROUP_NONE, &tr );
	return !tr.startsolid && tr.fraction >= 1.0f;
}

void C_NeoVolSmoke::StepFloodFill( int maxCells )
{
	static const int offs[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };

	int processed = 0;
	while ( m_nFrontierHead < m_Frontier.Count() && processed < maxCells && m_nFilledCells < m_nMaxCells )
	{
		const int cur = m_Frontier[m_nFrontierHead++];
		const int cz  = cur / ( m_nN * m_nN );
		const int cy  = ( cur / m_nN ) % m_nN;
		const int cx  = cur % m_nN;
		const int depth = m_Reach[cur];

		if ( depth + 1 > m_nMaxDepth )
			continue;

		const Vector curCenter = CellCenter( cx, cy, cz );

		for ( int k = 0; k < 6 && m_nFilledCells < m_nMaxCells; ++k )
		{
			const int nx = cx + offs[k][0], ny = cy + offs[k][1], nz = cz + offs[k][2];
			if ( !InGrid( nx, ny, nz ) )
				continue;

			const int ni = Idx( nx, ny, nz );
			if ( m_Reach[ni] != 255 )
				continue;

			if ( !CellOpen( curCenter, CellCenter( nx, ny, nz ) ) )
				continue;

			m_Reach[ni]    = (unsigned char)( depth + 1 );
			m_FillTime[ni] = gpGlobals->curtime;
			m_Frontier.AddToTail( ni );
			++m_nFilledCells;
			++processed;
		}
	}

	if ( m_nFrontierHead >= m_Frontier.Count() || m_nFilledCells >= m_nMaxCells )
	{
		m_bFillDone = true;
		m_Frontier.Purge();
		m_nFrontierHead = 0;
	}
}

void C_NeoVolSmoke::UpdateDensities()
{
	const float now     = gpGlobals->curtime;
	const float age     = now - m_flSpawnTime;
	const float ramp    = neo_volsmoke_ramp.GetFloat();
	const float regrow  = neo_volsmoke_regrow.GetFloat();
	const float dissip  = neo_volsmoke_dissipate.GetFloat();
	const float lifeFade = clamp( ( m_flLifetime - age ) / dissip, 0.0f, 1.0f );
	const float invMaxDepth = 1.0f / (float)MAX( 1, m_nMaxDepth );

	const int cells = m_nN * m_nN * m_nN;
	for ( int i = 0; i < cells; ++i )
	{
		const int reach = m_Reach[i];
		if ( reach == 255 )
		{
			m_Density[i] = 0.0f;
			continue;
		}

		// Thinner toward the fringe, with a bit of static per-cell variation.
		const float f    = (float)reach * invMaxDepth;
		float base       = 1.0f - 0.65f * f * f;
		base            *= 0.7f + 0.3f * ( (float)m_Noise[i] / 255.0f );

		const float fill = clamp( ( now - m_FillTime[i] ) / ramp, 0.0f, 1.0f );
		float hole = 1.0f;
		if ( m_CarveTime[i] >= 0.0f )
		{
			hole = clamp( ( now - m_CarveTime[i] ) / regrow, 0.0f, 1.0f );
			hole = hole * hole; // slow start, then refill
		}

		m_Density[i] = base * fill * hole * lifeFade;
	}
}

// Bilinear sample of m_Light within slab `zSlab` of the given major axis, at
// fractional coordinates (fx, fy) along the other two axes (cell units).
float C_NeoVolSmoke::SampleLight( float fx, float fy, int zSlab, int axis ) const
{
	if ( zSlab < 0 || zSlab >= m_nN )
		return 1.0f;

	const int   x0 = (int)floorf( fx ), y0 = (int)floorf( fy );
	const float tx = fx - (float)x0,   ty = fy - (float)y0;

	float result = 0.0f;
	for ( int dy = 0; dy < 2; ++dy )
	{
		for ( int dx = 0; dx < 2; ++dx )
		{
			const int x = x0 + dx, y = y0 + dy;
			const float w = ( dx ? tx : 1.0f - tx ) * ( dy ? ty : 1.0f - ty );
			float v = 1.0f;
			if ( x >= 0 && y >= 0 && x < m_nN && y < m_nN )
			{
				int ix, iy, iz;
				switch ( axis )
				{
				case 0:  ix = zSlab; iy = x; iz = y; break;
				case 1:  ix = x; iy = zSlab; iz = y; break;
				default: ix = x; iy = y; iz = zSlab; break;
				}
				v = m_Light[Idx( ix, iy, iz )];
			}
			result += w * v;
		}
	}
	return result;
}

// One slab-ordered sweep from the lit side: T(cell) = T(parent) * exp(-k*d*cellsize)
// where parent is one slab toward the light along the light direction.
void C_NeoVolSmoke::SweepLight()
{
	Vector L( 0.3f, 0.2f, 1.0f );
	sscanf( neo_volsmoke_lightdir.GetString(), "%f %f %f", &L.x, &L.y, &L.z );
	if ( L.LengthSqr() < 1e-6f )
		L = Vector( 0, 0, 1 );
	L.NormalizeInPlace();

	int axis = 2;
	if ( fabsf( L.x ) >= fabsf( L.y ) && fabsf( L.x ) >= fabsf( L.z ) ) axis = 0;
	else if ( fabsf( L.y ) >= fabsf( L.z ) ) axis = 1;

	const float major = L[axis];
	const int   dir   = major > 0.0f ? 1 : -1;      // slab index change toward the light
	const Vector step = L * ( m_flCellSize / fabsf( major ) ); // world step to the parent slab
	const float kStep = neo_volsmoke_light_k.GetFloat() * m_flCellSize;

	// Parent offset in cell units along the two minor axes.
	const int a0 = ( axis == 0 ) ? 1 : 0;
	const int a1 = ( axis == 2 ) ? 1 : 2;
	const float ofs0 = step[a0] / m_flCellSize;
	const float ofs1 = step[a1] / m_flCellSize;

	// Iterate slabs starting at the one nearest the light.
	const int zStart = dir > 0 ? m_nN - 1 : 0;
	for ( int n = 0; n < m_nN; ++n )
	{
		const int z = zStart - dir * n;
		const int zParent = z + dir;

		for ( int v = 0; v < m_nN; ++v )
		{
			for ( int u = 0; u < m_nN; ++u )
			{
				int ix, iy, iz;
				switch ( axis )
				{
				case 0:  ix = z; iy = u; iz = v; break;
				case 1:  ix = u; iy = z; iz = v; break;
				default: ix = u; iy = v; iz = z; break;
				}
				const int i = Idx( ix, iy, iz );

				const float parentT = SampleLight( (float)u + ofs0, (float)v + ofs1, zParent, axis );
				m_Light[i] = parentT * expf( -kStep * m_Density[i] );
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
	int x0 = clamp( (int)floorf( ( cmin.x - m_vecGridMin.x ) / m_flCellSize ), 0, m_nN - 1 );
	int y0 = clamp( (int)floorf( ( cmin.y - m_vecGridMin.y ) / m_flCellSize ), 0, m_nN - 1 );
	int z0 = clamp( (int)floorf( ( cmin.z - m_vecGridMin.z ) / m_flCellSize ), 0, m_nN - 1 );
	int x1 = clamp( (int)floorf( ( cmax.x - m_vecGridMin.x ) / m_flCellSize ), 0, m_nN - 1 );
	int y1 = clamp( (int)floorf( ( cmax.y - m_vecGridMin.y ) / m_flCellSize ), 0, m_nN - 1 );
	int z1 = clamp( (int)floorf( ( cmax.z - m_vecGridMin.z ) / m_flCellSize ), 0, m_nN - 1 );

	bool touched = false;
	for ( int z = z0; z <= z1; ++z )
	{
		for ( int y = y0; y <= y1; ++y )
		{
			for ( int x = x0; x <= x1; ++x )
			{
				const int i = Idx( x, y, z );
				if ( m_Reach[i] == 255 )
					continue;

				const Vector c = CellCenter( x, y, z );
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
void C_NeoVolSmoke::FillAtlas( unsigned char *pDst, int pitch, int width, int height ) const
{
	for ( int z = 0; z < m_nN; ++z )
	{
		const int tileX = ( z % m_nAtlasCols ) * m_nN;
		const int tileY = ( z / m_nAtlasCols ) * m_nN;
		for ( int y = 0; y < m_nN; ++y )
		{
			unsigned char *row = pDst + ( tileY + y ) * pitch + tileX * 4;
			for ( int x = 0; x < m_nN; ++x )
			{
				const int i = Idx( x, y, z );
				row[x * 4 + 0] = (unsigned char)( clamp( m_Density[i], 0.0f, 1.0f ) * 255.0f + 0.5f ); // R density
				row[x * 4 + 1] = (unsigned char)( clamp( m_Light[i],   0.0f, 1.0f ) * 255.0f + 0.5f ); // G light
				row[x * 4 + 2] = 0;
				row[x * 4 + 3] = 255;
			}
		}
	}
}

void C_NeoVolSmoke::ClientThink()
{
	const float now = gpGlobals->curtime;

	if ( now - m_flSpawnTime > m_flLifetime )
	{
		Release();
		return;
	}

	if ( !m_bFillDone )
	{
		StepFloodFill( neo_volsmoke_fill_rate.GetInt() );
	}

	// Densities animate every frame (ramp, regrow, dissipation).
	UpdateDensities();

	if ( now >= m_flNextLightSweep )
	{
		SweepLight();
		m_flNextLightSweep = now + 1.0f / neo_volsmoke_light_sweep_hz.GetFloat();
	}

	if ( m_pRegen && m_Atlas.IsValid() )
	{
		FillAtlas( m_pRegen->Bits(), m_pRegen->Pitch(), m_nAtlasCols * m_nN, m_nAtlasRows * m_nN );
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

	IMaterial *pMat = m_Material;

	// Lighting: ambient from the light grid at the spawn point (gamma -> linear).
	Vector amb = engine->GetLightForPoint( m_vecSpawn + Vector( 0, 0, 32.0f ), true );
	amb.x = powf( MAX( amb.x, 0.0f ), 2.2f );
	amb.y = powf( MAX( amb.y, 0.0f ), 2.2f );
	amb.z = powf( MAX( amb.z, 0.0f ), 2.2f );
	amb *= neo_volsmoke_ambient_scale.GetFloat();
	// Directional term scales with the scene brightness so indoor smoke stays dim.
	const float brightness = clamp( ( amb.x + amb.y + amb.z ) / 3.0f, 0.02f, 1.0f );
	const Vector lightCol = Vector( 1, 1, 1 ) * ( brightness * neo_volsmoke_light_scale.GetFloat() );

	bool found = false;
	IMaterialVar *pBase = pMat->FindVar( "$basetexture", &found, false );
	if ( found ) pBase->SetTextureValue( m_Atlas );

	SetVec3Var( pMat, "$volmin", m_vecGridMin );
	SetVec3Var( pMat, "$volmax", m_vecGridMax );
	SetVec4Var( pMat, "$gridparams", (float)m_nN, (float)m_nAtlasCols, (float)m_nAtlasRows, 1.0f / (float)m_nN );
	SetVec3Var( pMat, "$ambient", amb );
	SetVec3Var( pMat, "$lightcolor", lightCol );
	SetFloatVar( pMat, "$extinction", neo_volsmoke_extinction.GetFloat() );
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

CON_COMMAND_F( neo_volsmoke_spawn, "Spawn a volumetric smoke at the crosshair. Args: [extent] [lifetime]", FCVAR_CHEAT )
{
	trace_t tr;
	if ( !ViewTrace( tr, 2048.0f ) )
		return;

	const float extent   = args.ArgC() > 1 ? atof( args[1] ) : neo_volsmoke_extent.GetFloat();
	const float lifetime = args.ArgC() > 2 ? atof( args[2] ) : neo_volsmoke_lifetime.GetFloat();

	// Lift off the surface a little so the seed cell is not inside it.
	Vector origin = tr.endpos + tr.plane.normal * 24.0f;
	if ( tr.fraction >= 1.0f )
		origin = tr.endpos;

	C_NeoVolSmoke *p = C_NeoVolSmoke::Create( origin, extent, lifetime );
	Msg( "neo_volsmoke: %s at (%.0f %.0f %.0f), extent %.0f, %d live\n", p ? "spawned" : "FAILED", origin.x, origin.y, origin.z, extent, C_NeoVolSmoke::LiveCount() );
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
