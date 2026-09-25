#include "cbase.h"

#include "neo_bot.h"
#include "neo_bot_locomotion.h"
#include "particle_parse.h"

extern ConVar falldamage;

// NEO-HARNESS-TEMP research arm (2026-09-23, patch 54)
ConVar neo_bot_crouch_lookahead( "neo_bot_crouch_lookahead", "0", FCVAR_CHEAT,
	"Research: duck when a crouch area on the path starts within this many units (0 = only inside one)" );

// NEO-HARNESS-TEMP research arm (2026-09-25, patch 93): a bot ducks once its last known area is CROUCH, i.e. once its centre is
// in the crouch strip; a strip that starts at the obstacle's edge lets the standing hull's front meet the obstacle first
// (isolation's hut eave, decom's pipes, vtol's shelf: scratch/rr/roundrobin.md). Duck when the point a half hull ahead in
// the direction of travel is over a crouch area.
ConVar neo_bot_crouch_leading_edge( "neo_bot_crouch_leading_edge", "0", FCVAR_CHEAT,
	"Research: duck when the leading edge of the hull, not only its centre, is over a NAV_MESH_CROUCH area" );

// patch 93: the nav area under the point a half hull (plus a margin) ahead of the bot, along its horizontal velocity
static bool NeoCrouchAhead( CNEOBot *me, const Vector &feet, float flStep )
{
	Vector vel = me->GetAbsVelocity();
	vel.z = 0.0f;
	if ( vel.Length2DSqr() < Square( 30.0f ) )
	{
		return false;
	}

	vel.NormalizeInPlace();
	const float flAhead = me->GetBodyInterface()->GetHullWidth() * 0.5f + 12.0f;
	const Vector probe = feet + vel * flAhead + Vector( 0.0f, 0.0f, flStep );
	const CNavArea *pAhead = TheNavMesh->GetNavArea( probe, 60.0f );
	return pAhead && pAhead->HasAttributes( NAV_MESH_CROUCH );
}

//-----------------------------------------------------------------------------------------
void CNEOBotLocomotion::Update( void )
{
	CNEOBot* me = ToNEOBot( GetBot()->GetEntity() );
	if ( !me || me->GetNeoFlags() & NEO_FL_FREEZETIME)
	{
		return;
	}

	BaseClass::Update();

	// always 'crouch jump'
	if ( IsOnGround() )
	{
		CNavArea* currentArea = me->GetLastKnownArea();
		if (currentArea && (currentArea->GetAttributes() & NAV_MESH_CROUCH))
		{
			me->PressCrouchButton( 0.3f );
		}
		else if ( neo_bot_crouch_leading_edge.GetBool() && NeoCrouchAhead( me, GetFeet(), GetStepHeight() ) )
		{
			me->PressCrouchButton( 0.3f );
		}
		else if ( neo_bot_crouch_lookahead.GetFloat() > 0.0f )
		{
			// NEO-HARNESS-TEMP research arm (2026-09-23, patch 54): duck before the hull reaches a crouch
			// space, not once the bot's centre is inside it. A crouch area is often only as deep as the
			// low ceiling itself (terminal's spawn exit: a 25 u strip under an 8 u lintel), so a standing
			// hull hits the lintel while the bot is still in the area before it and never ducks
			const PathFollower *path = me->GetCurrentPath();
			if ( path && path->IsValid() )
			{
				const float flLook = neo_bot_crouch_lookahead.GetFloat();
				const Vector &feet = GetFeet();
				int nChecked = 0;
				for ( const Path::Segment *seg = path->GetCurrentGoal(); seg && nChecked < 4; seg = path->NextSegment( seg ), ++nChecked )
				{
					if ( ( seg->pos - feet ).Length2D() > flLook )
						break;
					if ( seg->area && seg->area->HasAttributes( NAV_MESH_CROUCH ) )
					{
						me->PressCrouchButton( 0.3f );
						break;
					}
				}
			}
		}
#ifdef NEO
		// NEO JANK resetting of crouch timer moved to NextBotPlayer::PressJumpButton
		// so far crouch jump seems to still be working, but watch out for a regression
		// disabled:
#else
		me->ReleaseCrouchButton();
#endif
	}
	else
	{
#ifdef NEO
		// Don't try to crouch jump if we are climbing a ladder
		if (!me->IsBotOnLadder())
		{
			me->PressCrouchButton( 0.3f );
		}
#else
		me->PressCrouchButton( 0.3f );
#endif
	}
}


//-----------------------------------------------------------------------------------------
// Move directly towards the given position
void CNEOBotLocomotion::Approach( const Vector& pos, float goalWeight )
{
	BaseClass::Approach( pos, goalWeight );
}


//-----------------------------------------------------------------------------------------
// Distance at which we will die if we fall
float CNEOBotLocomotion::GetDeathDropHeight( void ) const
{
	CNEOBot* me = ( CNEOBot* )GetBot()->GetEntity();

	// misyl: Fall damage only deals 10 health otherwise. 
	if ( falldamage.GetInt() != 1 )
	{
		if ( me->GetHealth() > 10.0f )
			return MAX_COORD_FLOAT;

		// #define PLAYER_MAX_SAFE_FALL_SPEED	526.5f // approx 20 feet sqrt( 2 * gravity * 20 * 12 )
		return 240.0f;
	}
	else
	{
		return 1000.0f;
	}
}


//-----------------------------------------------------------------------------------------
// Get maximum running speed
float CNEOBotLocomotion::GetRunSpeed( void ) const
{
	CNEOBot *me = (CNEOBot *)GetBot()->GetEntity();
	return me->GetSprintSpeed_WithActiveWepEncumberment();
}

float CNEOBotLocomotion::GetWalkSpeed( void ) const
{
	CNEOBot *me = (CNEOBot *)GetBot()->GetEntity();
	return me->GetNormSpeed_WithActiveWepEncumberment();
}

bool CNEOBotLocomotion::IsRunning( void ) const
{
	return GetSpeed() > GetWalkSpeed();
}


//-----------------------------------------------------------------------------------------
// Return true if given area can be used for navigation
bool CNEOBotLocomotion::IsAreaTraversable( const CNavArea* area ) const
{
	CNEOBot* me = ( CNEOBot* )GetBot()->GetEntity();

	if ( area->IsBlocked( me->GetTeamNumber() ) )
	{
		return false;
	}

	return true;
}


//-----------------------------------------------------------------------------------------
bool CNEOBotLocomotion::IsEntityTraversable( CBaseEntity* obstacle, TraverseWhenType when ) const
{
	// assume all players are "traversable" in that they will move or can be killed
	if ( obstacle && obstacle->IsPlayer() )
	{
		return true;
	}

	if ( obstacle )
	{
		// misyl:
		// override the base brush logic here to work better for hl2mp (w/ solid flags)
		// changing this for TF would be scary.
		//
		// if we hit a clip brush, ignore it if it is not BRUSHSOLID_ALWAYS
		if ( FClassnameIs( obstacle, "func_brush" ) )
		{
			CFuncBrush* brush = ( CFuncBrush* )obstacle;

			switch ( brush->m_iSolidity )
			{
			case CFuncBrush::BRUSHSOLID_ALWAYS:
				return false;
			case CFuncBrush::BRUSHSOLID_NEVER:
				return true;
			case CFuncBrush::BRUSHSOLID_TOGGLE:
				return brush->GetSolidFlags() & FSOLID_NOT_SOLID;
			}
		}
	}

	const bool bIsTraversable = PlayerLocomotion::IsEntityTraversable( obstacle, when );
	// NEO-HARNESS-TEMP research arm: with neo_bot_avoid_prop_entities, a breakable is only traversable
	// EVENTUALLY (base ILocomotion's rule), so avoidance steers round it
	extern ConVar neo_bot_avoid_prop_entities;
	const bool bBreakNow = ( when == EVENTUALLY ) || !neo_bot_avoid_prop_entities.GetBool();
	return bIsTraversable || ( bBreakNow && GetBot()->IsAbleToBreak(obstacle) );
}
