// ALL SKIP STATEMENTS THAT AFFECT THIS SHADER!!!
// defined $HDRTYPE && defined $HDRENABLED && !$HDRTYPE && $HDRENABLED
// defined $PIXELFOGTYPE && defined $WRITEWATERFOGTODESTALPHA && ( $PIXELFOGTYPE != 1 ) && $WRITEWATERFOGTODESTALPHA
// defined $LIGHTING_PREVIEW && defined $HDRTYPE && $LIGHTING_PREVIEW && $HDRTYPE != 0
// defined $LIGHTING_PREVIEW && defined $FASTPATHENVMAPTINT && $LIGHTING_PREVIEW && $FASTPATHENVMAPTINT
// defined $LIGHTING_PREVIEW && defined $FASTPATHENVMAPCONTRAST && $LIGHTING_PREVIEW && $FASTPATHENVMAPCONTRAST
// defined $LIGHTING_PREVIEW && defined $FASTPATH && $LIGHTING_PREVIEW && $FASTPATH
// ($FLASHLIGHT || $FLASHLIGHTSHADOWS) && $LIGHTING_PREVIEW

#pragma once
#include "shaderlib/cshader.h"
class neo_volsmoke_ps30_Static_Index
{
	unsigned int m_nCONVERT_TO_SRGB : 2;
public:
	void SetCONVERT_TO_SRGB( int i )
	{
		Assert( i >= 0 && i <= 1 );
		m_nCONVERT_TO_SRGB = i;
	}

	neo_volsmoke_ps30_Static_Index(  )
	{
		m_nCONVERT_TO_SRGB = g_pHardwareConfig->NeedsShaderSRGBConversion();
	}

	int GetIndex() const
	{
		return ( 3 * m_nCONVERT_TO_SRGB ) + 0;
	}
};

#define shaderStaticTest_neo_volsmoke_ps30 1


class neo_volsmoke_ps30_Dynamic_Index
{
	unsigned int m_nDLIGHTS : 2;
#ifdef _DEBUG
	bool m_bDLIGHTS : 1;
#endif	// _DEBUG
public:
	void SetDLIGHTS( int i )
	{
		Assert( i >= 0 && i <= 2 );
		m_nDLIGHTS = i;
#ifdef _DEBUG
		m_bDLIGHTS = true;
#endif	// _DEBUG
	}

	neo_volsmoke_ps30_Dynamic_Index(  )
	{
		m_nDLIGHTS = 0;
#ifdef _DEBUG
		m_bDLIGHTS = false;
#endif	// _DEBUG
	}

	int GetIndex() const
	{
		Assert( m_bDLIGHTS );
		return ( 1 * m_nDLIGHTS ) + 0;
	}
};

#define shaderDynamicTest_neo_volsmoke_ps30 psh_forgot_to_set_dynamic_DLIGHTS

