// NEO volumetric smoke - the networked half of a server smoke field. The
// client fills its own copy of the field from the exact origin and draws it
// from the server's start time, so a client that joins late draws the same
// cloud at the same age.

#include "cbase.h"
#include "c_neo_volsmoke.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

class C_NEOSmokeVolumeEnt : public C_BaseEntity
{
	DECLARE_CLASS( C_NEOSmokeVolumeEnt, C_BaseEntity );
	DECLARE_CLIENTCLASS();

public:
	C_NEOSmokeVolumeEnt() : m_flStartTime( 0.0f ) {}

	virtual void OnDataChanged( DataUpdateType_t updateType ) override;

private:
	float m_flStartTime;
};

IMPLEMENT_CLIENTCLASS_DT( C_NEOSmokeVolumeEnt, DT_NEOSmokeVolumeEnt, CNEOSmokeVolumeEnt )
	RecvPropTime( RECVINFO( m_flStartTime ) ),
END_RECV_TABLE()

void C_NEOSmokeVolumeEnt::OnDataChanged( DataUpdateType_t updateType )
{
	BaseClass::OnDataChanged( updateType );

	if ( updateType != DATA_UPDATE_CREATED )
	{
		return;
	}

	// The cloud dies with this entity, which the server removes with the
	// field or with the round.
	C_NeoVolSmoke::CreateForGrenade( GetNetworkOrigin(), m_flStartTime, this );
}
