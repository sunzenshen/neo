#pragma once

#include "NextBot/Player/NextBotPlayerLocomotion.h"
#include "neo_player_shared.h"
#include "../neo_player.h"

//----------------------------------------------------------------------------
class CNEOBotLocomotion : public PlayerLocomotion
{
public:
	DECLARE_CLASS( CNEOBotLocomotion, PlayerLocomotion );

	CNEOBotLocomotion( INextBot *bot ) : PlayerLocomotion( bot )
	{
	}

	virtual ~CNEOBotLocomotion() { }

	virtual void Update( void );								// (EXTEND) update internal state

	virtual void Approach( const Vector &pos, float goalWeight = 1.0f );	// move directly towards the given position

	virtual float GetMaxJumpHeight( void ) const;				// return maximum height of a jump
	virtual float GetDeathDropHeight( void ) const;			// distance at which we will die if we fall

	virtual float GetRunSpeed( void ) const override;			// get maximum running speed
	virtual float GetWalkSpeed( void ) const override;			// get maximum walking speed
	virtual bool IsRunning( void ) const override;

	virtual bool IsAreaTraversable( const CNavArea *baseArea ) const;	// return true if given area can be used for navigation
	virtual bool IsEntityTraversable( CBaseEntity *obstacle, TraverseWhenType when = EVENTUALLY ) const;

	mutable bool m_bBreakBreakableInPath = false;

protected:
	virtual void AdjustPosture( const Vector &moveGoal ) { }	// never crouch to navigate
};

inline float CNEOBotLocomotion::GetMaxJumpHeight( void ) const
{
	auto me = (CNEO_Player*)GetBot()->GetEntity();
	switch (me->GetClass())
	{
		case NEO_CLASS_RECON:
			return NEO_JUMP_HEIGHT_RECON;
		case NEO_CLASS_ASSAULT:
			return NEO_JUMP_HEIGHT_ASSAULT;
		case NEO_CLASS_SUPPORT:
			return NEO_JUMP_HEIGHT_SUPPORT;
		case NEO_CLASS_VIP:
			return NEO_JUMP_HEIGHT_VIP;
		case NEO_CLASS_JUGGERNAUT:
			return NEO_JUMP_HEIGHT_JUGGERNAUT;
		default:
			Assert(false);
			return 0.f;
	}
}
