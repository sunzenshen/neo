// NEO volumetric smoke shader.
//
// Draws the AABB of a voxel smoke volume (see client/neo/c_neo_volsmoke.cpp)
// and raymarches the slice atlas in $basetexture. Depth test is done in the
// pixel shader against the engine's depth-in-dest-alpha frame buffer copy, so
// hardware depth test is off and the box can be drawn with its far faces only.

#include "BaseVSShader.h"

#include "neo_volsmoke_vs30.inc"
#include "neo_volsmoke_ps30.inc"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

BEGIN_VS_SHADER( Neo_VolSmoke, "NEO raymarched voxel smoke" )

BEGIN_SHADER_PARAMS
	SHADER_PARAM( VOLMIN,     SHADER_PARAM_TYPE_VEC3,    "[0 0 0]",       "World-space AABB min" )
	SHADER_PARAM( VOLMAX,     SHADER_PARAM_TYPE_VEC3,    "[1 1 1]",       "World-space AABB max" )
	SHADER_PARAM( GRIDPARAMS, SHADER_PARAM_TYPE_VEC4,    "[32 8 4 0.03125]", "x = N, y = atlas cols, z = atlas rows, w = 1/N" )
	SHADER_PARAM( AMBIENT,    SHADER_PARAM_TYPE_VEC3,    "[0.6 0.6 0.6]", "Ambient light reaching the smoke (linear)" )
	SHADER_PARAM( LIGHTCOLOR, SHADER_PARAM_TYPE_VEC3,    "[0.8 0.8 0.8]", "Directional light colour (linear), scaled by baked per-voxel light" )
	SHADER_PARAM( EXTINCTION, SHADER_PARAM_TYPE_FLOAT,   "0.06",          "Extinction per world unit at density 1" )
	SHADER_PARAM( NOISEAMP,   SHADER_PARAM_TYPE_FLOAT,   "0.35",          "Edge erosion noise amplitude" )
	SHADER_PARAM( NOISESCALE, SHADER_PARAM_TYPE_FLOAT,   "0.045",         "Noise frequency (1/world units)" )
	SHADER_PARAM( JITTER,     SHADER_PARAM_TYPE_INTEGER, "1",             "Per-pixel step jitter (grain instead of banding)" )
	SHADER_PARAM( DEBUGMODE,  SHADER_PARAM_TYPE_INTEGER, "0",             "0 off, 1 coverage, 2 scene depth, 3 box hit" )
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
}

SHADER_FALLBACK
{
	if ( g_pHardwareConfig->GetDXSupportLevel() < 95 || !g_pHardwareConfig->SupportsShaderModel_3_0() )
	{
		// No ps_3_0: draw nothing rather than something wrong. The caller can
		// fall back to the legacy particle smoke when this material is in
		// fallback (IMaterial::GetShaderName() != "Neo_VolSmoke").
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

		// Manual depth test in the PS; never touch hardware depth.
		pShaderShadow->EnableDepthWrites( false );
		pShaderShadow->EnableDepthTest( false );

		// Dest alpha holds scene depth for later translucents - don't clobber it.
		pShaderShadow->EnableAlphaWrites( false );

		// Premultiplied alpha.
		pShaderShadow->EnableBlending( true );
		pShaderShadow->BlendFunc( SHADER_BLEND_ONE, SHADER_BLEND_ONE_MINUS_SRC_ALPHA );

		// Culling is driven by the caller (far faces only, see client code).
		pShaderShadow->EnableCulling( true );

		pShaderShadow->EnableTexture( SHADER_SAMPLER0, true ); // atlas (data, not sRGB)
		pShaderShadow->EnableTexture( SHADER_SAMPLER1, true ); // frame buffer / depth
		pShaderShadow->EnableSRGBRead( SHADER_SAMPLER0, false );
		pShaderShadow->EnableSRGBRead( SHADER_SAMPLER1, false );
		pShaderShadow->EnableSRGBWrite( true );

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

		// c2: volume max
		params[VOLMAX]->GetVecValue( c, 3 );
		c[3] = 0.0f;
		pShaderAPI->SetPixelShaderConstant( 2, c );

		// c3: grid layout
		params[GRIDPARAMS]->GetVecValue( c, 4 );
		pShaderAPI->SetPixelShaderConstant( 3, c );

		// c4: ambient + jitter flag
		params[AMBIENT]->GetVecValue( c, 3 );
		c[3] = params[JITTER]->GetIntValue() ? 1.0f : 0.0f;
		pShaderAPI->SetPixelShaderConstant( 4, c );

		// c5: directional light colour + time
		params[LIGHTCOLOR]->GetVecValue( c, 3 );
		c[3] = pShaderAPI->CurrentTime();
		pShaderAPI->SetPixelShaderConstant( 5, c );

		// c6: noise + debug
		c[0] = params[NOISEAMP]->GetFloatValue();
		c[1] = params[NOISESCALE]->GetFloatValue();
		c[2] = 0.0f;
		c[3] = (float)params[DEBUGMODE]->GetIntValue();
		pShaderAPI->SetPixelShaderConstant( 6, c );

		// c7: half-texel offset for the frame buffer lookup
		int w, h;
		pShaderAPI->GetBackBufferDimensions( w, h );
		c[0] = 0.5f / (float)MAX( w, 1 );
		c[1] = 0.5f / (float)MAX( h, 1 );
		c[2] = 0.0f; c[3] = 0.0f;
		pShaderAPI->SetPixelShaderConstant( 7, c );

		DECLARE_DYNAMIC_VERTEX_SHADER( neo_volsmoke_vs30 );
		SET_DYNAMIC_VERTEX_SHADER( neo_volsmoke_vs30 );

		DECLARE_DYNAMIC_PIXEL_SHADER( neo_volsmoke_ps30 );
		SET_DYNAMIC_PIXEL_SHADER( neo_volsmoke_ps30 );
	}

	Draw();
}

END_SHADER
