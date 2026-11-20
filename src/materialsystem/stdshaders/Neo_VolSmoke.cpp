// NEO volumetric smoke shader.
//
// Draws the AABB of a voxel smoke volume (see client/neo/c_neo_volsmoke.cpp)
// and raymarches the slice atlas in $basetexture. The near pass clips in the
// pixel shader against the engine's depth-in-dest-alpha frame buffer copy, so
// hardware depth test is off and the box is drawn with its far faces only.
// That copy saturates a short way from the eye, so $slab materials draw the
// rest of the volume as view-parallel quads that use the hardware depth test.

#include "BaseVSShader.h"

#include "neo_volsmoke_vs30.inc"
#include "neo_volsmoke_ps30.inc"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

// Glass panes the pixel shader clips the march to.
static constexpr int NEO_VOLSMOKE_SHADER_PANES = 4;

BEGIN_VS_SHADER( Neo_VolSmoke, "NEO raymarched voxel smoke" )

BEGIN_SHADER_PARAMS
	SHADER_PARAM( VOLMIN,     SHADER_PARAM_TYPE_VEC3,    "[0 0 0]",       "World-space AABB min" )
	SHADER_PARAM( VOLMAX,     SHADER_PARAM_TYPE_VEC3,    "[1 1 1]",       "World-space AABB max" )
	SHADER_PARAM( SMOKEMIN,   SHADER_PARAM_TYPE_VEC3,    "[0 0 0]",       "World-space min of the box around the cells that hold smoke" )
	SHADER_PARAM( SMOKEMAX,   SHADER_PARAM_TYPE_VEC3,    "[1 1 1]",       "World-space max of that box" )
	SHADER_PARAM( SMOKESPHERE, SHADER_PARAM_TYPE_VEC4,   "[0 0 0 100000]", "xyz = gas seed, w = radius of the sphere that holds all smoke" )
	SHADER_PARAM( GRIDPARAMS, SHADER_PARAM_TYPE_VEC4,    "[32 8 8 4]",    "x = z slices, y = tiles per atlas row, zw = atlas size in tiles" )
	SHADER_PARAM( TILECELLS,  SHADER_PARAM_TYPE_VEC2,    "[128 128]",     "Cells across one atlas tile: x, y" )
	SHADER_PARAM( SPINCENTER, SHADER_PARAM_TYPE_VEC3,    "[0 0 0]",       "World point the look noise turns about" )
	SHADER_PARAM( AMBIENT,    SHADER_PARAM_TYPE_VEC3,    "[0.6 0.6 0.6]", "Ambient light reaching the smoke (linear)" )
	SHADER_PARAM( LIGHTCOLOR, SHADER_PARAM_TYPE_VEC3,    "[0.8 0.8 0.8]", "Directional light color (linear), scaled by baked per-voxel light" )
	SHADER_PARAM( EXTINCTION, SHADER_PARAM_TYPE_FLOAT,   "0.06",          "Extinction per world unit at density 1" )
	SHADER_PARAM( AGE,        SHADER_PARAM_TYPE_FLOAT,   "0",             "Seconds since the smoke spawned, moves the look noise" )
	SHADER_PARAM( JITTER,     SHADER_PARAM_TYPE_INTEGER, "1",             "Per-pixel step jitter (grain instead of banding)" )
	SHADER_PARAM( DEBUGMODE,  SHADER_PARAM_TYPE_INTEGER, "0",             "0 off, 1 coverage, 2 scene depth, 3 box hit" )
	SHADER_PARAM( SLAB,       SHADER_PARAM_TYPE_INTEGER, "0",             "1 = far slabs: hardware depth test, no culling" )
	SHADER_PARAM( NOISETEXTURE, SHADER_PARAM_TYPE_TEXTURE, "",            "Tiled value noise for the look: R = a lattice layer, G = the next layer up" )
	SHADER_PARAM( DLIGHTCOUNT, SHADER_PARAM_TYPE_INTEGER, "0",            "Dynamic lights that reach the smoke, 0 to 2" )
	SHADER_PARAM( DLIGHT0POS, SHADER_PARAM_TYPE_VEC4,    "[0 0 0 0]",     "xyz = first dynamic light, w = 1 / reach squared" )
	SHADER_PARAM( DLIGHT0COLOR, SHADER_PARAM_TYPE_VEC3,  "[0 0 0]",       "Light the first dynamic light adds at its center (linear)" )
	SHADER_PARAM( DLIGHT1POS, SHADER_PARAM_TYPE_VEC4,    "[0 0 0 0]",     "As $dlight0pos, second light" )
	SHADER_PARAM( DLIGHT1COLOR, SHADER_PARAM_TYPE_VEC3,  "[0 0 0]",       "As $dlight0color, second light" )
	SHADER_PARAM( FRONT,      SHADER_PARAM_TYPE_VEC3,    "[0 288 0.037]", "x = bloom front path length, y = path length at atlas value 1, z = 1 / edge width" )
	SHADER_PARAM( PANECOUNT,  SHADER_PARAM_TYPE_INTEGER, "0",             "Glass panes the smoke draws in layers around, 0 to 4" )
	SHADER_PARAM( PANELAYER,  SHADER_PARAM_TYPE_INTEGER, "0",             "The layer this draw marches" )
	SHADER_PARAM( PANEGROUPS, SHADER_PARAM_TYPE_VEC4,    "[-1 -1 -1 -1]", "Per pane, the layer after which its glass draws" )
	SHADER_PARAM( PANE0PLANE, SHADER_PARAM_TYPE_VEC4,    "[0 0 1 0]",     "First pane: xyz normal, w distance" )
	SHADER_PARAM( PANE0U,     SHADER_PARAM_TYPE_VEC4,    "[0 0 0 0]",     "First pane: in-plane axis over the half width, w offset" )
	SHADER_PARAM( PANE0V,     SHADER_PARAM_TYPE_VEC4,    "[0 0 0 0]",     "First pane: as $pane0u, the other axis" )
	SHADER_PARAM( PANE1PLANE, SHADER_PARAM_TYPE_VEC4,    "[0 0 1 0]",     "As $pane0plane, second pane" )
	SHADER_PARAM( PANE1U,     SHADER_PARAM_TYPE_VEC4,    "[0 0 0 0]",     "As $pane0u, second pane" )
	SHADER_PARAM( PANE1V,     SHADER_PARAM_TYPE_VEC4,    "[0 0 0 0]",     "As $pane0v, second pane" )
	SHADER_PARAM( PANE2PLANE, SHADER_PARAM_TYPE_VEC4,    "[0 0 1 0]",     "As $pane0plane, third pane" )
	SHADER_PARAM( PANE2U,     SHADER_PARAM_TYPE_VEC4,    "[0 0 0 0]",     "As $pane0u, third pane" )
	SHADER_PARAM( PANE2V,     SHADER_PARAM_TYPE_VEC4,    "[0 0 0 0]",     "As $pane0v, third pane" )
	SHADER_PARAM( PANE3PLANE, SHADER_PARAM_TYPE_VEC4,    "[0 0 1 0]",     "As $pane0plane, fourth pane" )
	SHADER_PARAM( PANE3U,     SHADER_PARAM_TYPE_VEC4,    "[0 0 0 0]",     "As $pane0u, fourth pane" )
	SHADER_PARAM( PANE3V,     SHADER_PARAM_TYPE_VEC4,    "[0 0 0 0]",     "As $pane0v, fourth pane" )
END_SHADER_PARAMS

SHADER_INIT_PARAMS()
{
	SET_FLAGS2( MATERIAL_VAR2_NEEDS_FULL_FRAME_BUFFER_TEXTURE );
	SET_FLAGS( MATERIAL_VAR_TRANSLUCENT );
	SET_FLAGS( MATERIAL_VAR_NOFOG );
}

SHADER_INIT
{
	if ( params[BASETEXTURE]->IsDefined() )
	{
		LoadTexture( BASETEXTURE );
	}

	if ( params[NOISETEXTURE]->IsDefined() )
	{
		LoadTexture( NOISETEXTURE );
	}
}

SHADER_FALLBACK
{
	// Keyed on the hardware, not the configured dx level, so a lowered
	// mat_dxlevel on capable hardware still draws the smoke.
	if ( g_pHardwareConfig->GetMaxDXSupportLevel() < 95 )
	{
		// No ps_3_0. The client sees the fallback through
		// IMaterial::GetShaderName() and draws an opaque box instead.
		return "Wireframe";
	}
	return 0;
}

SHADER_DRAW
{
	SHADOW_STATE
	{
		// Texcoord 0 = the view depth range the draw marches.
		int texCoordDims[1] = { 2 };
		pShaderShadow->VertexShaderVertexFormat( VERTEX_POSITION, 1, texCoordDims, 0 );

		// The near pass clips in the PS; slabs sit at their own depth and use
		// the depth buffer. Neither writes depth.
		const bool bSlab = params[SLAB]->GetIntValue() != 0;
		pShaderShadow->EnableDepthWrites( false );
		pShaderShadow->EnableDepthTest( bSlab );

		// Dest alpha holds scene depth for later translucents - don't clobber it.
		pShaderShadow->EnableAlphaWrites( false );

		// Premultiplied alpha.
		pShaderShadow->EnableBlending( true );
		pShaderShadow->BlendFunc( SHADER_BLEND_ONE, SHADER_BLEND_ONE_MINUS_SRC_ALPHA );

		// The near pass culls by the caller's cull mode (far faces only, see
		// client code); a slab quad is one face and may wind either way.
		pShaderShadow->EnableCulling( !bSlab );

		pShaderShadow->EnableTexture( SHADER_SAMPLER0, true ); // atlas (data, not sRGB)
		pShaderShadow->EnableTexture( SHADER_SAMPLER1, true ); // frame buffer / depth
		pShaderShadow->EnableSRGBRead( SHADER_SAMPLER0, false );
		pShaderShadow->EnableTexture( SHADER_SAMPLER2, true ); // look noise (data)
		pShaderShadow->EnableSRGBRead( SHADER_SAMPLER1, false );
		pShaderShadow->EnableSRGBRead( SHADER_SAMPLER2, false );
		// The pixel shader writes gamma itself (see BlendOutput).
		pShaderShadow->EnableSRGBWrite( false );

		DECLARE_STATIC_VERTEX_SHADER( neo_volsmoke_vs30 );
		SET_STATIC_VERTEX_SHADER( neo_volsmoke_vs30 );

		DECLARE_STATIC_PIXEL_SHADER( neo_volsmoke_ps30 );
		SET_STATIC_PIXEL_SHADER( neo_volsmoke_ps30 );

		pShaderShadow->FogMode( SHADER_FOGMODE_DISABLED );
	}

	DYNAMIC_STATE
	{
		BindTexture( SHADER_SAMPLER0, BASETEXTURE );
		pShaderAPI->BindStandardTexture( SHADER_SAMPLER1, TEXTURE_FRAME_BUFFER_FULL_DEPTH );
		BindTexture( SHADER_SAMPLER2, NOISETEXTURE );

		float c[4];

		// c0: eye position
		Vector eye;
		pShaderAPI->GetWorldSpaceCameraPosition( eye.Base() );
		c[0] = eye.x; c[1] = eye.y; c[2] = eye.z; c[3] = 0.0f;
		pShaderAPI->SetPixelShaderConstant( 0, c );

		// c1: volume min + extinction
		params[VOLMIN]->GetVecValue( c, 3 );
		c[3] = params[EXTINCTION]->GetFloatValue();
		pShaderAPI->SetPixelShaderConstant( 1, c );

		// c2: volume max + far slab flag
		params[VOLMAX]->GetVecValue( c, 3 );
		c[3] = params[SLAB]->GetIntValue() != 0 ? 1.0f : 0.0f;
		pShaderAPI->SetPixelShaderConstant( 2, c );

		// c3: grid layout
		params[GRIDPARAMS]->GetVecValue( c, 4 );
		pShaderAPI->SetPixelShaderConstant( 3, c );

		// c4: ambient + jitter flag
		params[AMBIENT]->GetVecValue( c, 3 );
		c[3] = params[JITTER]->GetIntValue() ? 1.0f : 0.0f;
		pShaderAPI->SetPixelShaderConstant( 4, c );

		// c5: directional light color + time
		params[LIGHTCOLOR]->GetVecValue( c, 3 );
		c[3] = pShaderAPI->CurrentTime();
		pShaderAPI->SetPixelShaderConstant( 5, c );

		float front[3];
		params[FRONT]->GetVecValue( front, 3 );

		// c6: layer, age, bloom front, debug
		c[0] = (float)params[PANELAYER]->GetIntValue();
		c[1] = params[AGE]->GetFloatValue();
		c[2] = front[0];
		c[3] = (float)params[DEBUGMODE]->GetIntValue();
		pShaderAPI->SetPixelShaderConstant( 6, c );

		// c7: half-texel offset for the frame buffer lookup, path length decode
		int w, h;
		pShaderAPI->GetBackBufferDimensions( w, h );
		c[0] = 0.5f / (float)Max( w, 1 );
		c[1] = 0.5f / (float)Max( h, 1 );
		c[2] = front[1];
		c[3] = front[2];
		pShaderAPI->SetPixelShaderConstant( 7, c );

		// c8, c9: the box the march is clipped to
		params[SMOKEMIN]->GetVecValue( c, 3 );
		c[3] = 0.0f;
		pShaderAPI->SetPixelShaderConstant( 8, c );

		params[SMOKEMAX]->GetVecValue( c, 3 );
		c[3] = 0.0f;
		pShaderAPI->SetPixelShaderConstant( 9, c );

		// c10: the sphere about the seed that holds all smoke
		params[SMOKESPHERE]->GetVecValue( c, 4 );
		pShaderAPI->SetPixelShaderConstant( 10, c );

		// c11: half a texel of a tile, per axis
		float tile[2];
		params[TILECELLS]->GetVecValue( tile, 2 );
		c[0] = 0.5f / Max( tile[0], 1.0f );
		c[1] = 0.5f / Max( tile[1], 1.0f );
		c[2] = 0.0f;
		c[3] = 0.0f;
		pShaderAPI->SetPixelShaderConstant( 11, c );

		// c12: the point the look noise turns about
		params[SPINCENTER]->GetVecValue( c, 3 );
		c[3] = 0.0f;
		pShaderAPI->SetPixelShaderConstant( 12, c );

		// c13 - c16: the dynamic lights that reach the smoke. Without one the
		// shader runs the combo that has no light term.
		const int nDLights = clamp( params[DLIGHTCOUNT]->GetIntValue(), 0, 2 );
		if ( nDLights > 0 )
		{
			params[DLIGHT0POS]->GetVecValue( c, 4 );
			pShaderAPI->SetPixelShaderConstant( 13, c );

			params[DLIGHT0COLOR]->GetVecValue( c, 3 );
			c[3] = 0.0f;
			pShaderAPI->SetPixelShaderConstant( 14, c );
		}

		if ( nDLights > 1 )
		{
			params[DLIGHT1POS]->GetVecValue( c, 4 );
			pShaderAPI->SetPixelShaderConstant( 15, c );

			params[DLIGHT1COLOR]->GetVecValue( c, 3 );
			c[3] = 0.0f;
			pShaderAPI->SetPixelShaderConstant( 16, c );
		}

		// c17 - c26: the glass panes the smoke draws in layers around, packed
		// as the pixel shader reads them. Without one the shader runs the combo
		// that has no pane clip.
		const int nPanes = clamp( params[PANECOUNT]->GetIntValue(), 0, NEO_VOLSMOKE_SHADER_PANES );
		if ( nPanes > 0 )
		{
			const int paneParams[NEO_VOLSMOKE_SHADER_PANES][3] = { { PANE0PLANE, PANE0U, PANE0V }, { PANE1PLANE, PANE1U, PANE1V },
				{ PANE2PLANE, PANE2U, PANE2V }, { PANE3PLANE, PANE3U, PANE3V } };
			float groups[4];
			params[PANEGROUPS]->GetVecValue( groups, 4 );
			float vScale[4];
			float vOffset[4];
			for ( int p = 0; p < NEO_VOLSMOKE_SHADER_PANES; ++p )
			{
				float plane[4] = { 0.0f, 0.0f, 1.0f, 0.0f };
				float u[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
				float v[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
				const float group = ( p < nPanes ) ? groups[p] : -1.0f;
				if ( p < nPanes )
				{
					params[paneParams[p][0]]->GetVecValue( plane, 4 );
					params[paneParams[p][1]]->GetVecValue( u, 4 );
					params[paneParams[p][2]]->GetVecValue( v, 4 );
				}

				const float scale = group + 2.0f;
				for ( int k = 0; k < 4; ++k )
				{
					plane[k] *= scale;
				}

				const float uLen = sqrtf( u[0] * u[0] + u[1] * u[1] + u[2] * u[2] );
				const float vLen = sqrtf( v[0] * v[0] + v[1] * v[1] + v[2] * v[2] );
				vScale[p] = ( uLen > 0.0f ) ? vLen / ( uLen * scale ) : 0.0f;
				vOffset[p] = v[3];
				pShaderAPI->SetPixelShaderConstant( 17 + p, plane );
				pShaderAPI->SetPixelShaderConstant( 21 + p, u );
			}

			pShaderAPI->SetPixelShaderConstant( 25, vScale );
			pShaderAPI->SetPixelShaderConstant( 26, vOffset );
		}

		DECLARE_DYNAMIC_VERTEX_SHADER( neo_volsmoke_vs30 );
		SET_DYNAMIC_VERTEX_SHADER( neo_volsmoke_vs30 );

		DECLARE_DYNAMIC_PIXEL_SHADER( neo_volsmoke_ps30 );
		SET_DYNAMIC_PIXEL_SHADER_COMBO( DLIGHTS, nDLights );
		SET_DYNAMIC_PIXEL_SHADER_COMBO( PANES, nPanes > 0 ? 1 : 0 );
		SET_DYNAMIC_PIXEL_SHADER( neo_volsmoke_ps30 );
	}

	Draw();
}

END_SHADER
