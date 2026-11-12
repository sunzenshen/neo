// NEO volumetric smoke - the networked half of a server smoke field. The
// client fills its own copy of the field from the exact origin and draws it
// from the server's start time, so a client that joins late draws the same
// cloud at the same age. The field hash checks that the copies agree.

#include "cbase.h"
#include "c_neo_volsmoke.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

class C_NEOSmokeVolumeEnt : public C_BaseEntity
{
	DECLARE_CLASS( C_NEOSmokeVolumeEnt, C_BaseEntity );
	DECLARE_CLIENTCLASS();

public:
	C_NEOSmokeVolumeEnt() : m_flStartTime( 0.0f ), m_nFieldHashLo( 0 ), m_nFieldHashHi( 0 ), m_bHashChecked( false ) {}

	virtual void OnDataChanged( DataUpdateType_t updateType ) override;
	virtual void ClientThink() override;

private:
	unsigned long long FieldHash() const { return ( (unsigned long long)(unsigned int)m_nFieldHashHi << 32 ) | (unsigned int)m_nFieldHashLo; }

	float m_flStartTime;
	int m_nFieldHashLo;
	int m_nFieldHashHi;
	CHandle< C_NeoVolSmoke > m_hVolume;
	bool m_bHashChecked;
};

IMPLEMENT_CLIENTCLASS_DT( C_NEOSmokeVolumeEnt, DT_NEOSmokeVolumeEnt, CNEOSmokeVolumeEnt )
	RecvPropTime( RECVINFO( m_flStartTime ) ),
	RecvPropInt( RECVINFO( m_nFieldHashLo ) ),
	RecvPropInt( RECVINFO( m_nFieldHashHi ) ),
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
	m_hVolume = C_NeoVolSmoke::CreateForGrenade( GetNetworkOrigin(), m_flStartTime, this );
	SetNextClientThink( CLIENT_THINK_ALWAYS );
}

// Compares the fields once both are filled.
void C_NEOSmokeVolumeEnt::ClientThink()
{
	const C_NeoVolSmoke *pVol = m_hVolume.Get();
	if ( m_bHashChecked || !pVol )
	{
		SetNextClientThink( CLIENT_THINK_NEVER );
		return;
	}

	const unsigned long long serverHash = FieldHash();
	if ( serverHash == 0 || !pVol->CheckServerHash( serverHash ) )
	{
		return;
	}

	m_bHashChecked = true;
	SetNextClientThink( CLIENT_THINK_NEVER );
}
