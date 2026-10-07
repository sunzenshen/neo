#include "cbase.h"
#include "bot/behavior/neo_bot_attack.h"
#include "bot/behavior/neo_bot_ladder_climb.h"
#include "nav_ladder.h"
#include "NextBot/Path/NextBotPathFollow.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

extern bool NEOHarnessLadderDescentOff( void );	// NEO-HARNESS-TEMP: #2176's descent half off

// NEO-HARNESS-TEMP forensic instrumentation (2026-09-24): one NEO_FORENSIC_LADDERBEH line per ladder behaviour ending,
// with its reason, so an offline tool can count how climbs end (see harness/patches/README.md). Never part of a PR.
extern ConVar sv_neo_forensic_log;
static float s_flNeoClimbStart[ MAX_PLAYERS + 1 ];	// patch 100: when each bot's current climb began
static const float NEO_LADDER_TICK_TRACE_TIME = 1.2f;
extern ConVar neo_harness_ladder_pr_off;	// NEO-HARNESS-TEMP A/B switch (NextBotPlayerLocomotion.cpp)
static void NeoLogLadderBeh( CNEOBot *me, const char *beh, const CNavLadder *ladder, bool goingUp, const char *reason )
{
	if ( !sv_neo_forensic_log.GetBool() || !me )
	{
		return;
	}
	const Vector &feet = me->GetLocomotionInterface()->GetFeet();
	Msg( "NEO_FORENSIC_LADDERBEH t=%.2f p=%d beh=%s dir=%s ladder=%d pos=%.0f,%.0f,%.0f ladderbottom=%.0f laddertop=%.0f movetype=%d onground=%d reason=%s\n",
		gpGlobals->curtime, me->entindex(), beh, goingUp ? "up" : "down", ladder ? ladder->GetID() : -1,
		feet.x, feet.y, feet.z, ladder ? ladder->m_bottom.z : 0.0f, ladder ? ladder->m_top.z : 0.0f,
		(int)me->GetMoveType(), me->GetGroundEntity() ? 1 : 0, reason );
}
#define NEO_LADDER_DONE( reason ) ( NeoLogLadderBeh( me, "climb", m_ladder, m_bGoingUp, reason ), Done( reason ) )
static void NeoLogLadderEvent( CNEOBot *me, const char *beh, const CNavLadder *ladder, bool goingUp, PRINTF_FORMAT_STRING const char *fmt, ... )
{
	if ( !sv_neo_forensic_log.GetBool() )
	{
		return;
	}
	char buf[256];
	va_list args;
	va_start( args, fmt );
	V_vsnprintf( buf, sizeof( buf ), fmt, args );
	va_end( args );
	NeoLogLadderBeh( me, beh, ladder, goingUp, buf );
}

//---------------------------------------------------------------------------------------------
// NEO-HARNESS-TEMP research arm (2026-09-23, patch 52), on by default since 2026-10-07 (ridgeline ladder 1: top falls)
ConVar neo_bot_ladder_exit_closest( "neo_bot_ladder_exit_closest", "1", FCVAR_CHEAT,
	"Research: dismount towards the exit area's point nearest the ladder end instead of its centre" );

// NEO-HARNESS-TEMP research arm (neo_bot_ladder_exit_closest v2): an exit area narrower than half a hull is a wall cap
// the climb crosses, not a landing to stand on (dawn and dusk ladder 1: 7 u deep), so it keeps the center and the old finish
static bool NeoIsThinExit( const CNavArea *area, CNEOBot *me )
{
	const float flHalfHull = me->GetBodyInterface()->GetHullWidth() * 0.5f;
	return area && MIN( area->GetSizeX(), area->GetSizeY() ) < flHalfHull;
}

// NEO-HARNESS-TEMP research arm (2026-09-24, patch 71): tell the locomotion's ladder state machine that this ladder is
// wanted, and which way. Without it PlayerLocomotion sees a bot on a ladder it never asked for: TraverseLadder() forces
// the move type back to walking for up to half a second (LADDER_ADOPT_TIME) and then adopts the ladder by its NEARER end
// - at a ladder's foot that is the bottom, so a bot the behaviour just mounted to climb gets driven down and off
// (fc3: 2,047 climbs began through that adoption against 270 through ClimbLadder). v2 claims only once the bot is on the
// ladder (LadderClimb::OnStart): v1 also claimed during the approach, and on a ladder whose foot hangs above the floor the
// locomotion's APPROACHING state (which has no timeout) then pinned the bot under it for good (lc1: x1.20, one subsurface
// spot 619 events).
ConVar neo_bot_ladder_claim( "neo_bot_ladder_claim", "0", FCVAR_CHEAT,
	"Research: the ladder behaviours call ClimbLadder / DescendLadder so the locomotion treats the ladder as wanted" );

static const CNavArea *NeoLadderExitArea( const CNavLadder *ladder, bool goingUp )
{
	if ( !ladder )
	{
		return NULL;
	}
	if ( !goingUp )
	{
		return ladder->m_bottomArea;
	}
	return ladder->m_topForwardArea ? ladder->m_topForwardArea :
		( ladder->m_topLeftArea ? ladder->m_topLeftArea : ( ladder->m_topRightArea ? ladder->m_topRightArea : ladder->m_topBehindArea ) );
}

static void NeoClaimLadder( CNEOBot *me, const CNavLadder *ladder, bool goingUp, const CNavArea *exitArea )
{
	if ( !neo_bot_ladder_claim.GetBool() || !me || !ladder )
	{
		return;
	}
	ILocomotion *mover = me->GetLocomotionInterface();
	if ( mover->IsUsingLadder() )
	{
		return;
	}
	if ( !exitArea )
	{
		exitArea = NeoLadderExitArea( ladder, goingUp );
	}
	if ( goingUp )
	{
		mover->ClimbLadder( ladder, exitArea );
	}
	else
	{
		mover->DescendLadder( ladder, exitArea );
	}
}

// NEO-HARNESS-TEMP research arm (2026-09-24, patch 72): a climb that stops rising part-way is usually snagged on
// something at the ladder's edge rather than blocked outright - subsurface ladder 1's landing plate overlaps the column
// by ~1 u, so a bot centred on the nav ladder heads into its underside at the same height every time while one 2 u off
// centre climbs past (lc2: 169 of 198 mid-climb stall endings there). Before giving up, shimmy sideways - right, then
// back left past the start, then right again - and only jump off if none of that restores the climb.
ConVar neo_bot_ladder_stall_nudge( "neo_bot_ladder_stall_nudge", "0", FCVAR_CHEAT,
	"Research: a ladder climb that stalls part-way shimmies sideways before jumping off" );

// NEO-HARNESS-TEMP research arm (2026-09-24, patch 74): a threat seen while climbing makes the bot jump backwards off the
// ladder to fight - also at the very top and while stepping off onto the landing, where the jump drops it back down the
// shaft and the traversal is lost (lcf: bullet ladders 4 / 3 and dawn ladder 2 ended 46 / 36 / 22 up-climbs that way at the
// top). Past halfway, or once dismounting, finish the ladder and fight from the landing.
ConVar neo_bot_ladder_commit( "neo_bot_ladder_commit", "0", FCVAR_CHEAT,
	"Research: past halfway up or down a ladder (or dismounting), a threat does not make the bot jump off" );

// NEO-HARNESS-TEMP research arm (2026-09-24, patch 75): bots drift sideways off a ladder's edge and slip off part-way -
// on the 16 u ladders (sentinel 7, apparatus 22) 14 of 16 and 8 of 14 mid-climb slips happened beyond the half-width, and
// patch 72's timed strafes overshot subsurface ladder 1 by ~21 u. Hold the bot across the ladder: steer it back towards a
// lateral target inside the ladder's width (the centre; with patch 72, its shimmy moves the target to +4 / -4 / +8 u).
ConVar neo_bot_ladder_lateral_hold( "neo_bot_ladder_lateral_hold", "0", FCVAR_CHEAT,
	"Research: keep a climbing bot within the ladder's width (and make the stall shimmy move to fixed offsets)" );

// NEO-HARNESS-TEMP research arm (2026-09-24, patch 76): the climb presses MoveUp / MoveDown so that NEO's ladder movement
// (gamemovement LadderMove, upstream #1802) climbs straight along the ladder whatever the bot is looking at - the intent of
// upstream #1830, "decouple climbing with look direction". But PressMoveUpButton() / PressMoveDownButton() take a timer
// duration defaulting to -1, and unlike the other buttons they set no input bit of their own: the timer has expired
// before the user command is built, so the press never happens and bots climb on forward + view pitch, which drifts them
// sideways or off the face whenever the view is not square to the ladder. Hold the press past the next bot update
// (updates come every ~0.13 s), and let go on ticks that strafe (patches 72 / 75 - LadderMove ignores strafing while
// upmove is set) and before the dismount, whose forward step over the top upmove would also override.
ConVar neo_bot_ladder_moveup( "neo_bot_ladder_moveup", "0", FCVAR_CHEAT,
	"Research: the ladder climb's MoveUp / MoveDown presses actually reach the user command (climb along the ladder, not the view)" );

// NEO-HARNESS-TEMP research arm (2026-09-25, patch 101): LadderMove turns MoveDown into a move out along the face normal
// (NEO's upmove branch) and, for a player standing on a floor, adds a push off the ladder for any move away from the face -
// so a descent that holds MoveDown while the bot's feet still touch the landing or a hatch rim is pushed off the top
// (saitama 2: 11-14 of the descents st1 traced popped out within one update, every one with upmove -230). While there is
// floor under the feet, descend with the plain forward-into-the-face press and the steep look instead, which never counts
// as moving away.
ConVar neo_bot_ladder_descend_forward( "neo_bot_ladder_descend_forward", "0", FCVAR_CHEAT,
	"Research: a descent does not press MoveDown while the bot's feet are on a floor (forward into the face only)" );
static constexpr float NEO_LADDER_STEEP_PITCH = 50.0f;	// forward into the face descends only past 45 deg of pitch

static bool NeoFloorUnder( CNEOBot *me )
{
	// as LadderMove decides onFloor: a ground entity, or solid 1 u under the feet
	const Vector feet = me->GetLocomotionInterface()->GetFeet();
	return me->GetGroundEntity() != NULL || ( UTIL_PointContents( feet - Vector( 0.0f, 0.0f, 1.0f ) ) & CONTENTS_SOLID ) != 0;
}

// NEO-HARNESS-TEMP research arm (2026-09-25, patch 102): the descent's grab tick is the hazard (patch 100's traces: MoveDown
// or IN_BACK from a bot still on the rim, or forward with the view pointed up, and the bot pops out of the top). Settle
// first: press nothing until the view is steep and square to the face (phase 1), then forward only - into the face, which
// both grabs the ladder and moves down, and never counts as moving away - until the bot is on the ladder and off the
// floor (phase 2); then the usual descent. NextBotPlayer's user-command build enforces the same limits every tick.
ConVar neo_bot_ladder_descent_settle( "neo_bot_ladder_descent_settle", "0", FCVAR_CHEAT,
	"Research: a descent presses nothing until its view is steep and square to the ladder, then forward only until it is on the ladder off the floor" );
static constexpr float NEO_LADDER_SETTLE_YAW = 30.0f;		// deg off the face's inward normal
static constexpr float NEO_LADDER_SETTLE_TIMEOUT = 1.2f;	// give up settling after this long
static constexpr float NEO_LADDER_SETTLE_LOOK_DROP = 150.0f;	// settle looking at the face this far below the feet

// NEO-HARNESS-TEMP research arm (2026-09-25, patch 104): the dismount kick sends the bot at a fixed 150 u/s, some 80 u through
// the air whatever the distance to the exit point; off a short side landing that is past the landing, into the corner
// beyond (sentinel_jgr ladder 6: 13 of 72 climbs up ended in the elevator roof's far corner, 78 stuck events in 8 matches).
// Scale the kick to reach the exit point.
ConVar neo_bot_ladder_kick_to_exit( "neo_bot_ladder_kick_to_exit", "0", FCVAR_CHEAT,
	"Research: the dismount kick carries the bot as far as the exit point instead of a fixed 150 u/s" );
static constexpr float NEO_LADDER_KICK_SPEED = 150.0f;	// the stock kick
static constexpr float NEO_LADDER_KICK_AIRTIME = 0.45f;	// about how long its 150 u/s up keeps the bot in the air
static constexpr float NEO_LADDER_KICK_MIN = 50.0f;		// enough to leave the ladder

// patch 104 v2: the kick aims where the whole hull stands on the exit area - to its nearest point the bot's centre lands
// on the area's edge with half its hull over whatever lies between (sentinel 1: the top of the wall before its landing)
static Vector NeoHullLandingPoint( const CNavArea *area, const Vector &from, float flHalfHull )
{
	const Vector lo = area->GetCorner( NORTH_WEST );
	const Vector hi = area->GetCorner( SOUTH_EAST );
	float xlo = lo.x + flHalfHull, xhi = hi.x - flHalfHull;
	float ylo = lo.y + flHalfHull, yhi = hi.y - flHalfHull;
	if ( xlo > xhi )
	{
		xlo = xhi = 0.5f * ( lo.x + hi.x );
	}
	if ( ylo > yhi )
	{
		ylo = yhi = 0.5f * ( lo.y + hi.y );
	}

	Vector vecLand( Clamp( from.x, xlo, xhi ), Clamp( from.y, ylo, yhi ), from.z );
	vecLand.z = area->GetZ( vecLand );
	return vecLand;
}
static int s_nNeoLadderSettle[ MAX_PLAYERS + 1 ];			// 0 none, 1 aiming, 2 pushing into the face
static float s_flNeoLadderSettleStart[ MAX_PLAYERS + 1 ];

int NeoBotLadderSettlePhase( int iEntIndex )
{
	return ( iEntIndex > 0 && iEntIndex <= MAX_PLAYERS ) ? s_nNeoLadderSettle[ iEntIndex ] : 0;
}

static void NeoSetLadderSettle( CNEOBot *me, int nPhase )
{
	const int idx = me->entindex();
	if ( idx <= 0 || idx > MAX_PLAYERS )
	{
		return;
	}

	if ( nPhase == 1 && s_nNeoLadderSettle[idx] == 0 )
	{
		s_flNeoLadderSettleStart[idx] = gpGlobals->curtime;
	}
	s_nNeoLadderSettle[idx] = nPhase;
}
static constexpr float NEO_LADDER_VERTICAL_PRESS = 0.25f;

// NEO-HARNESS-TEMP research arm (2026-09-24, patch 77): OnStart's teleport fallback leaves the hull 2 u off the ladder face
// - exactly CGameMovement::LadderDistance(), so the engine's next ladder trace ends on the face, misses, and drops the bot
// to walking before its first climbing update (lcg: teleported mounts go ascend -> none 0.14 s later, bullet ladder 2 97
// of 301). Place it half the grab distance off the face instead.
ConVar neo_bot_ladder_teleport_grab( "neo_bot_ladder_teleport_grab", "0", FCVAR_CHEAT,
	"Research: the ladder climb's teleport fallback places the bot inside the engine's ladder grab distance" );
static constexpr float NEO_LADDER_GRAB_DIST = 2.0f;	// CGameMovement::LadderDistance()

// NEO-HARNESS-TEMP research arm (2026-10-07, on by default): on a turned ladder brush a half width off the line leaves the hull's corner in it
// (ridgeline ladder 1: climbs stuck at the foot), so clear the corner, then slide in until the hull touches the face
ConVar neo_bot_ladder_teleport_reach( "neo_bot_ladder_teleport_reach", "1", FCVAR_CHEAT,
	"Research: the ladder climb's teleport fallback clears the hull's corners off a turned ladder face and then touches the face" );
// NEO-HARNESS-TEMP research arm (2026-10-07, ntre notes/ridgeline-todo8-2026-10-07.md): ridgeline ladder 1 foot bounces
ConVar neo_bot_ladder_teleport_view( "neo_bot_ladder_teleport_view", "0", FCVAR_CHEAT,
	"Research: the ladder climb's teleport fallback also turns the view square to the ladder face" );
static constexpr float NEO_LADDER_TELEPORT_SEEK = 16.0f;	// how far towards the face the teleported hull is slid to find it

// NEO-HARNESS-TEMP research arm (2026-09-24, patch 78): the locomotion drops its claim the moment the engine reports the bot
// off the ladder, even for a tick; when the climb carries on and the engine re-attaches it, the locomotion sees a ladder it
// never asked for, forces walking every update and after half a second adopts the ladder by its nearer end - mid-climb
// often the bottom (lcg: subsurface ladder 1, 33 of its 41 such climbs failed). Re-claim while climbing.
ConVar neo_bot_ladder_reclaim( "neo_bot_ladder_reclaim", "0", FCVAR_CHEAT,
	"Research: a climbing bot the engine has on the ladder re-claims it when the locomotion has let go" );

// NEO-HARNESS-TEMP research arm (2026-09-24, patch 81): going down, the early drop-off fires as soon as the exit is less than
// SAFE_FALL_DIST below - on most ladders at the very top - and its kick (150 u/s back from the face and 150 u/s up) lands the
// bot back on the floor it came down from: through a hatch, across a narrow shaft (lch C: 326 of 411 descents ended at the
// top; saitama ladder 2's slab is the map's worst stuck spot). Drop early only once the bot is a standing height below the top.
ConVar neo_bot_ladder_descent_clear( "neo_bot_ladder_descent_clear", "0", FCVAR_CHEAT,
	"Research: a descending bot drops off the ladder early only once it is a standing height below the ladder's top" );

// NEO-HARNESS-TEMP research arm (2026-09-24, patch 83): the climb's first Update comes one bot update (~0.13 s) after the
// mount, and until then the bot holds the approach's forward press - which, facing the ladder, LadderMove turns into climbing
// UP: a descent mounted at the top goes over the top edge and lands back on the floor it started from (lci C: 50 of 93
// descents). Press the climb's direction (patch 76's MoveUp / MoveDown, which override forward and strafe) at the mount.
ConVar neo_bot_ladder_press_on_start( "neo_bot_ladder_press_on_start", "0", FCVAR_CHEAT,
	"Research: the ladder climb presses its vertical direction when it starts, instead of at its first update" );

CNEOBotLadderClimb::CNEOBotLadderClimb( const CNavLadder *ladder, bool goingUp )
	: m_ladder( ladder ), m_bGoingUp( goingUp ), m_flLastZ( 0.0f ),
	m_bDismountPhase( false ), m_bJumpedOffLadder( false ), m_pExitArea( nullptr )
{
	m_exitAreaCenter = vec3_origin;
	m_ladderForward = ladder ? -ladder->GetNormal() : vec3_origin;
}

//---------------------------------------------------------------------------------------------
ActionResult<CNEOBot> CNEOBotLadderClimb::OnStart( CNEOBot *me, Action<CNEOBot> *priorAction )
{
	if ( !m_ladder )
	{
		return Done( "No ladder specified" );
	}

	// Don't interfere with look direction
	// Can exit out of behavior if threat is present
	me->StopLookingAroundForEnemies();
	me->SetAttribute( CNEOBot::IGNORE_ENEMIES );

	// Timeout based on ladder length
	float estimatedClimbTime = m_ladder->m_length / MAX_CLIMB_SPEED + 3.0f;
	m_timeoutTimer.Start( estimatedClimbTime );

	// patch 100
	if ( me->entindex() > 0 && me->entindex() <= MAX_PLAYERS )
	{
		s_flNeoClimbStart[ me->entindex() ] = gpGlobals->curtime;
	}

	ILocomotion *mover = me->GetLocomotionInterface();
	m_flLastZ = mover->GetFeet().z;
	m_stuckTimer.Start( STUCK_CHECK_INTERVAL );

	bool bTeleported = false;
	const Vector vecStartVel = me->GetAbsVelocity();
	if ( m_bGoingUp )
	{
		// Hull trace check: Ensure clear path to climb in the intended direction.
		Vector traceStart = mover->GetFeet();

		// Trace upwards from feet to check the space the bot would occupy.
		// e.g. ladder going through opening in ceiling
		float targetZ = m_ladder->m_top.z;
		Vector traceEnd = traceStart;
		traceEnd.z = MIN( targetZ, traceStart.z + 100.0f );

		trace_t tr;
		UTIL_TraceHull( traceStart, traceEnd, me->WorldAlignMins(), me->WorldAlignMaxs(), MASK_NPCSOLID, me, COLLISION_GROUP_NONE, &tr );

		// If vertical path blocked by world geometry due to failed approach alignment,
		// teleport bot to ideal position, assuming approach placed bot close enough
		// Ideally we only transition into this behavior when bot has attached to ladder
		if ( tr.DidHit() && tr.fraction < 1.0f )
		{
			if ( me->IsDebugging( NEXTBOT_PATH ) )
			{
				DevMsg( "%s: Ladder climb path obstructed (fraction %.2f), teleporting to ideal position\n",
					me->GetDebugIdentifier(), tr.fraction );
				NDebugOverlay::Box( traceStart, me->WorldAlignMins(), me->WorldAlignMaxs(), 255, 0, 0, 0, 2.0f );
				NDebugOverlay::Box( traceEnd, me->WorldAlignMins(), me->WorldAlignMaxs(), 255, 0, 0, 0, 2.0f );
			}

			// Fallback: Teleport to a center position on the ladder.
			bTeleported = true;
			mover->Reset(); // clear velocity cache in locomotion interface
			me->SetAbsVelocity( vec3_origin );

			Vector idealPos = m_ladder->GetPosAtHeight( m_flLastZ );
			// Offset slightly from the ladder surface based on the bot's collision box
			// (patch 77: inside the engine's grab distance, see neo_bot_ladder_teleport_grab)
			const float flFaceGap = neo_bot_ladder_teleport_grab.GetBool() ? NEO_LADDER_GRAB_DIST * 0.5f : 2.0f;
			float offsetDist = me->CollisionProp()->OBBSize().x / 2.0f + flFaceGap;
			idealPos += m_ladder->GetNormal() * offsetDist;
			idealPos.z = m_flLastZ;

			// NEO-HARNESS-TEMP research arm (neo_bot_ladder_teleport_reach)
			if ( neo_bot_ladder_teleport_reach.GetBool() )
			{
				const Vector &normal = m_ladder->GetNormal();
				const float flHalf = me->CollisionProp()->OBBSize().x / 2.0f;
				const float flReach = flHalf * ( fabsf( normal.x ) + fabsf( normal.y ) );
				Vector clearPos = m_ladder->GetPosAtHeight( m_flLastZ ) + normal * ( flReach + flFaceGap );
				clearPos.z = m_flLastZ;

				trace_t trFace;
				UTIL_TraceHull( clearPos, clearPos - normal * NEO_LADDER_TELEPORT_SEEK, me->WorldAlignMins(), me->WorldAlignMaxs(),
					MASK_PLAYERSOLID, me, COLLISION_GROUP_PLAYER_MOVEMENT, &trFace );
				if ( !trFace.startsolid )
				{
					idealPos = trFace.DidHit() ? trFace.endpos + normal * ( NEO_LADDER_GRAB_DIST * 0.5f ) : clearPos;
				}
			}

			// Face perpendicularly straight on to the ladder (-normal)
			Vector idealLookDir = -m_ladder->GetNormal();
			QAngle idealAngles;
			VectorAngles( idealLookDir, idealAngles );

			// Teleport the bot
			me->SetAbsOrigin( idealPos );
			me->SetAbsAngles( idealAngles );
			// NEO-HARNESS-TEMP research arm (2026-10-07, neo_bot_ladder_teleport_view): SetAbsAngles leaves the view,
			// and a forward press with the view off the face's normal slides the bot off the side of a narrow ladder
			if ( neo_bot_ladder_teleport_view.GetBool() )
			{
				me->SnapEyeAngles( idealAngles );
			}

			// Update mover feet to new teleported position for stuck checking
			m_flLastZ = idealPos.z;
		}
		else if ( me->IsDebugging( NEXTBOT_PATH ) )
		{
			NDebugOverlay::Line( traceStart, traceEnd, 0, 255, 0, true, 2.0f );
		}
	}

	if ( me->IsDebugging( NEXTBOT_PATH ) )
	{
		DevMsg( "%s: Starting ladder climb (%s), length %.1f\n",
			me->GetDebugIdentifier(),
			m_bGoingUp ? "up" : "down",
			m_ladder->m_length );
	}

	// Try to resolve the exit area from the current path early
	ResolveExitArea( me );

	ClaimLadder( me );
	// patch 71: the locomotion must know this ladder is wanted (see neo_bot_ladder_claim)
	NeoClaimLadder( me, m_ladder, m_bGoingUp, m_pExitArea );

	// patch 83: climb the right way from the first tick, not with the approach's forward press
	if ( neo_bot_ladder_press_on_start.GetBool() && neo_bot_ladder_moveup.GetBool() )
	{
		me->ReleaseForwardButton();
		me->ReleaseLeftButton();
		me->ReleaseRightButton();
		if ( m_bGoingUp )
		{
			me->PressMoveUpButton( NEO_LADDER_VERTICAL_PRESS );
		}
		else if ( !( neo_bot_ladder_descend_forward.GetBool() && NeoFloorUnder( me ) ) && !neo_bot_ladder_descent_settle.GetBool() )	// patches 101 / 102
		{
			me->PressMoveDownButton( NEO_LADDER_VERTICAL_PRESS );
		}
	}

	// patch 102: a descent settles before it presses anything
	NeoSetLadderSettle( me, ( neo_bot_ladder_descent_settle.GetBool() && !m_bGoingUp ) ? 1 : 0 );

	NeoLogLadderEvent( me, "climbstart", m_ladder, m_bGoingUp, "tele=%d exit=%d exitz=%.0f vel=%.0f,%.0f,%.0f ang=%.0f,%.0f",
		bTeleported ? 1 : 0, m_pExitArea ? m_pExitArea->GetID() : -1, m_pExitArea ? m_exitAreaCenter.z : 0.0f,
		vecStartVel.x, vecStartVel.y, vecStartVel.z, me->EyeAngles().x, me->EyeAngles().y );

	return Continue();
}

//---------------------------------------------------------------------------------------------
// PlayerLocomotion lets go of a ladder it was never asked to use, and once the contact persists
// takes it over by the nearer end - at the foot, the bottom. Tell it this climb is wanted.
void CNEOBotLadderClimb::ClaimLadder( CNEOBot *me ) const
{
	ILocomotion *mover = me->GetLocomotionInterface();
	if ( mover->IsUsingLadder() || neo_harness_ladder_pr_off.GetBool() )	// NEO-HARNESS-TEMP: off = no claim, as upstream
	{
		return;
	}

	const CNavArea *pExitArea = m_pExitArea;
	if ( !pExitArea )
	{
		pExitArea = m_bGoingUp ? m_ladder->m_topForwardArea : m_ladder->m_bottomArea;
	}

	// The generator fills whichever top slot the landing is in, often not the forward one
	if ( !pExitArea && m_bGoingUp )
	{
		pExitArea = m_ladder->m_topLeftArea ? m_ladder->m_topLeftArea :
			( m_ladder->m_topRightArea ? m_ladder->m_topRightArea : m_ladder->m_topBehindArea );
	}

	// The locomotion's dismount walks to this area, so there is nothing to claim with without one
	if ( !pExitArea )
	{
		return;
	}

	if ( m_bGoingUp )
	{
		mover->ClimbLadder( m_ladder, pExitArea );
	}
	else
	{
		mover->DescendLadder( m_ladder, pExitArea );
	}
}

//---------------------------------------------------------------------------------------------
void CNEOBotLadderClimb::ResolveExitArea( CNEOBot *me )
{
	// Clear potentially stale exit area
	m_pExitArea = nullptr;

	const PathFollower *path = me->GetCurrentPath();
	if ( path && path->IsValid() )
	{
		const Path::Segment *seg = path->GetCurrentGoal();
		if ( !seg )
		{
			return;
		}

		constexpr int MAX_PATH_SEARCH_STEPS = 100;

		// The path's current goal can still be the area at the ladder's foot when the climb reaches the
		// top, and that area is no exit: the exit is the first area after this ladder
		const Path::Segment *ladderSeg = neo_harness_ladder_pr_off.GetBool() ? NULL : seg;	// NEO-HARNESS-TEMP: off = upstream's scan
		for ( int i = 0; ladderSeg && ladderSeg->ladder != m_ladder && i < MAX_PATH_SEARCH_STEPS; ++i )
		{
			ladderSeg = path->NextSegment( ladderSeg );
		}

		if ( ladderSeg && ladderSeg->ladder == m_ladder )
		{
			seg = ladderSeg;
		}

		int safetyCounter = 0;
		while ( seg && safetyCounter < MAX_PATH_SEARCH_STEPS )
		{
			if ( !seg->ladder )
			{
				break;
			}

			seg = path->NextSegment( seg );
			safetyCounter++;
		}
		Assert( safetyCounter < MAX_PATH_SEARCH_STEPS );

		if ( seg && !seg->ladder && seg->area )
		{
			m_pExitArea = seg->area;
			m_exitAreaCenter = m_pExitArea->GetCenter();

			// NEO-HARNESS-TEMP research arm (2026-09-23, patch 52): step off towards where the exit area
			// meets this end of the ladder, not towards its centre - on a long, narrow landing (ghost's
			// ladder-1 catwalk, 25 x 125 u) the centre lies off to one side, and the dismount kick
			// along it carries the bot past the landing's edge
			if ( neo_bot_ladder_exit_closest.GetBool() )
			{
				// The exit is the area this ladder lands on, not whatever the path follower has moved on to:
				// by the time the dismount starts the path's current goal can already be the area after the
				// landing (ghost ladder 1: 1499, south along the catwalk, instead of the landing 930)
				CNavArea *ends[4] = { m_ladder->m_topForwardArea, m_ladder->m_topLeftArea, m_ladder->m_topRightArea, m_ladder->m_topBehindArea };
				int nEnds = 4;
				if ( !m_bGoingUp )
				{
					ends[0] = m_ladder->m_bottomArea;
					nEnds = 1;
				}
				bool bIsLadderEnd = false;
				CNavArea *pFirstEnd = NULL;
				for ( int e = 0; e < nEnds; ++e )
				{
					bIsLadderEnd |= ( ends[e] == m_pExitArea );
					if ( !pFirstEnd && ends[e] )
						pFirstEnd = ends[e];
				}
				if ( !bIsLadderEnd && pFirstEnd )
				{
					// prefer a landing the path passes through
					CNavArea *pOnPath = NULL;
					for ( const Path::Segment *s = path->FirstSegment(); s && !pOnPath; s = path->NextSegment( s ) )
						for ( int e = 0; e < nEnds && !pOnPath; ++e )
							if ( ends[e] && s->area == ends[e] )
								pOnPath = ends[e];
					m_pExitArea = pOnPath ? pOnPath : pFirstEnd;
					m_exitAreaCenter = m_pExitArea->GetCenter();
				}

				if ( NeoIsThinExit( m_pExitArea, me ) )
				{
					return;
				}

				const Vector &ladderEnd = m_bGoingUp ? m_ladder->m_top : m_ladder->m_bottom;
				Vector closest;
				m_pExitArea->GetClosestPointOnArea( ladderEnd, &closest );
				// into the wall at the top (where the landing is), away from it at the bottom
				Vector inward = m_bGoingUp ? m_ladderForward : -m_ladderForward; inward.z = 0.0f;
				if ( inward.NormalizeInPlace() > 0.0f )
				{
					closest += inward * 16.0f;
				}
				m_pExitArea->GetClosestPointOnArea( closest, &m_exitAreaCenter );
			}
		}
	}
}


//---------------------------------------------------------------------------------------------
// Implementation based on ladder climbing logic in https://github.com/Dragoteryx/drgbase/
// NEO-HARNESS-TEMP forensic instrumentation (2026-09-25, patch 100): NEO_FORENSIC_LADDERTICK - every tick of a descent's
// first 0.6 s, what the engine was last told to do (the user command) and what it did, for the descents that pop back
// out of the top (saitama 2: 49 %). Never part of a PR.

// patch 100b: the same trace for climbs up (leaning sentinel ladder 7: 27 % of up-climbs slip off near the foot)
ConVar neo_bot_ladder_tick_trace_up( "neo_bot_ladder_tick_trace_up", "0", FCVAR_CHEAT, "Harness: NEO_FORENSIC_LADDERTICK for climbs up too" );

static void NeoLogLadderTick( CNEOBot *me, const CNavLadder *ladder, bool bGoingUp )
{
	const int idx = me->entindex();
	if ( !sv_neo_forensic_log.GetBool() || ( bGoingUp && !neo_bot_ladder_tick_trace_up.GetBool() ) || !ladder || idx <= 0 || idx > MAX_PLAYERS
		|| gpGlobals->curtime - s_flNeoClimbStart[idx] > NEO_LADDER_TICK_TRACE_TIME )
	{
		return;
	}

	const Vector &feet = me->GetLocomotionInterface()->GetFeet();
	const Vector &vel = me->GetAbsVelocity();
	const CUserCmd *pCmd = me->GetLastUserCommand();
	const QAngle ang = me->EyeAngles();
	Msg( "NEO_FORENSIC_LADDERTICK t=%.3f p=%d dir=%s ladder=%d pos=%.0f,%.0f,%.1f top=%.0f vel=%.0f,%.0f,%.0f mt=%d gnd=%d btn=%x "
		"fm=%.0f sm=%.0f um=%.0f cmdbtn=%x ang=%.0f,%.0f using=%d\n",
		gpGlobals->curtime, idx, bGoingUp ? "up" : "down", ladder->GetID(), feet.x, feet.y, feet.z, ladder->m_top.z, vel.x, vel.y, vel.z,
		(int)me->GetMoveType(), me->GetGroundEntity() ? 1 : 0, (unsigned)me->m_nButtons,
		pCmd ? pCmd->forwardmove : 0.0f, pCmd ? pCmd->sidemove : 0.0f, pCmd ? pCmd->upmove : 0.0f, pCmd ? (unsigned)pCmd->buttons : 0u,
		ang.x, ang.y, me->GetLocomotionInterface()->IsUsingLadder() ? 1 : 0 );
}

ActionResult<CNEOBot> CNEOBotLadderClimb::Update( CNEOBot *me, float /*interval*/ )
{
	// patch 100
	NeoLogLadderTick( me, m_ladder, m_bGoingUp );

	if ( m_timeoutTimer.IsElapsed() )
	{
		return NEO_LADDER_DONE( "Ladder climb timeout" );
	}

	const CKnownEntity *threat = me->GetVisionInterface()->GetPrimaryKnownThreat(true);
	// patch 74: past halfway (or dismounting) the ladder is finished first
	bool bCommitted = false;
	if ( threat && neo_bot_ladder_commit.GetBool() && m_ladder->m_top.z > m_ladder->m_bottom.z )
	{
		const float flFrac = ( me->GetLocomotionInterface()->GetFeet().z - m_ladder->m_bottom.z ) / ( m_ladder->m_top.z - m_ladder->m_bottom.z );
		bCommitted = m_bDismountPhase || ( m_bGoingUp ? flFrac > 0.5f : flFrac < 0.5f );
	}

	if ( threat && !bCommitted )
	{
		if ( me->IsDebugging( NEXTBOT_PATH ) )
		{
			DevMsg( "%s: Threat detected during ladder climb - engaging\n", me->GetDebugIdentifier() );
		}
		// Detach from ladder to ready weapon
		me->PressJumpButton();
		me->PressBackwardButton(0.1f);
		// ChangeTo: We may move away from ladder when fighting, reevaluate later
		NeoLogLadderBeh( me, "climb", m_ladder, m_bGoingUp, "threat" );
		return ChangeTo( new CNEOBotAttack, "Interrupting climb to engage enemy" );
	}

	ILocomotion *mover = me->GetLocomotionInterface();
	IBody *body = me->GetBodyInterface();
	const Vector& myPos = mover->GetFeet();
	bool bExitIsBehind = false;
	Vector toExit = vec3_origin;

	if ( m_pExitArea )
	{
		toExit = m_exitAreaCenter - myPos;
		toExit.z = 0.0f;
		if ( toExit.Length2DSqr() > 0.01f )
		{
			toExit.NormalizeInPlace();
		}
		else
		{
			toExit = m_ladderForward; // Fallback direction
		}

		if ( DotProduct( toExit, m_ladderForward ) < 0.0f )
		{
			bExitIsBehind = true;
		}
	}

	// Check if we're on the ladder (MOVETYPE_LADDER or locomotion says so)
	bool onLadder = me->IsBotOnLadder();

	if ( !onLadder )
	{
		if ( mover->IsOnGround() )
		{
			return NEO_LADDER_DONE( "Reached the ground, ending climb" );
		}
		
		if ( !m_bDismountPhase && !m_bJumpedOffLadder )
		{
			Vector ladderClosestPoint;
			CalcClosestPointOnLineSegment( myPos, m_ladder->m_bottom, m_ladder->m_top, ladderClosestPoint );
			if ( myPos.DistToSqr( ladderClosestPoint ) > Square( MAX_DEVIATION_DIST ) )
			{
				return NEO_LADDER_DONE( "Fallen too far from ladder, resetting" );
			}
		}
	}

	//------------------------------------------------------------
	// Ladder climbing phase
	//------------------------------------------------------------
	if ( !m_bDismountPhase )
	{
		// The locomotion drops its claim whenever the engine lets go of the bot, even for a tick
		if ( me->GetMoveType() == MOVETYPE_LADDER )
		{
			ClaimLadder( me );
		}

		float currentZ = myPos.z;
		float targetZ = m_bGoingUp ? m_ladder->m_top.z : m_ladder->m_bottom.z;

		// Going down, the bot holds still until its view makes forward take it down: not a stall
		const bool bHoldForView = !m_bGoingUp && !NEOHarnessLadderDescentOff()	// NEO-HARNESS-TEMP: off = never hold, as upstream
			&& !me->GetLocomotionInterface()->IsForwardDownLadder( m_ladder );
		if ( bHoldForView )
		{
			m_flLastZ = currentZ;
			m_stuckTimer.Start( STUCK_CHECK_INTERVAL );
		}

		// patch 102: advance the settle - aim, then push into the face, then climb as usual
		int nSettle = NeoBotLadderSettlePhase( me->entindex() );
		if ( nSettle > 0 )
		{
			Vector2D vecIn = -m_ladder->GetNormal().AsVector2D();
			vecIn.NormalizeInPlace();
			Vector vecEyeFwd;
			AngleVectors( me->EyeAngles(), &vecEyeFwd );
			Vector2D vecEye2D = vecEyeFwd.AsVector2D();
			vecEye2D.NormalizeInPlace();
			const bool bSquare = DotProduct2D( vecEye2D, vecIn ) > cosf( DEG2RAD( NEO_LADDER_SETTLE_YAW ) );
			const bool bSteep = me->EyeAngles().x >= NEO_LADDER_STEEP_PITCH;
			if ( gpGlobals->curtime - s_flNeoLadderSettleStart[ me->entindex() ] > NEO_LADDER_SETTLE_TIMEOUT )
			{
				nSettle = 0;
			}
			else if ( nSettle == 1 && bSquare && bSteep )
			{
				nSettle = 2;
			}
			else if ( nSettle == 2 && onLadder && !NeoFloorUnder( me ) )
			{
				nSettle = 0;
			}
			NeoSetLadderSettle( me, nSettle );
			if ( nSettle > 0 )
			{
				m_flLastZ = currentZ;	// no stall while settling
				m_stuckTimer.Start( STUCK_CHECK_INTERVAL );
			}
		}

		// Stuck detection: if we haven't made vertical progress, bail out gracefully
		if ( m_stuckTimer.IsElapsed() )
		{
			float verticalDelta = fabsf( currentZ - m_flLastZ );
			if ( verticalDelta < STUCK_Z_TOLERANCE )
			{
				// No vertical progress - if we're close enough to the target, consider it done
				float distToTarget = fabsf( currentZ - targetZ );
				if ( distToTarget < mover->GetStepHeight() * 2.0f )
				{
					EnterDismountPhase( me, "stall-near-end" );
					return Continue();
				}
				else if ( neo_bot_ladder_stall_nudge.GetBool() && m_nNudges < 3 )
				{
					// patch 72: snagged part-way - shimmy sideways (right, left past the start, right) before giving up
					++m_nNudges;
					if ( neo_bot_ladder_lateral_hold.GetBool() )
					{
						// patch 75: to fixed offsets across the ladder rather than timed strafes
						m_flLateralTarget = ( m_nNudges == 1 ) ? 4.0f : ( m_nNudges == 2 ? -4.0f : 8.0f );
					}
					else
					{
						m_nudgeTimer.Start( 0.3f * m_nNudges );
					}
					m_flLastZ = currentZ;
					m_stuckTimer.Start( STUCK_CHECK_INTERVAL + 0.3f * m_nNudges );
				}
				else
				{
					// We are stuck mid-climb. Reset scenario by jumping backwards and ending condition
					if ( me->IsDebugging( NEXTBOT_PATH ) )
					{
						DevMsg( "%s: Ladder climb stuck - jumping backwards to reset (delta %.1f)\n",
							me->GetDebugIdentifier(), verticalDelta );
					}
					me->PressJumpButton();
					me->PressBackwardButton(0.1f);
					return NEO_LADDER_DONE( "Got stuck on something climbing the ladder, jumping off to reset." );
				}
			}
			m_flLastZ = currentZ;
			m_stuckTimer.Start( STUCK_CHECK_INTERVAL );
		}

		// Early jump-off. Going down, only once a standing height below the top: from any higher, the
		// kick towards the exit lands the bot back on the floor the descent started from.
		bool bWantsDismount = false;
		bool bEarlyDismount = false;
		if ( m_pExitArea )
		{
			float zDistToExit = currentZ - m_exitAreaCenter.z;
			const bool bBelowTopFloor = m_bGoingUp || NEOHarnessLadderDescentOff()	// NEO-HARNESS-TEMP: off = upstream's jump-off
				|| currentZ < m_ladder->m_top.z - body->GetStandHullHeight();

			if ( zDistToExit > 0.0f && zDistToExit <= SAFE_FALL_DIST && bBelowTopFloor )
			{
				bWantsDismount = true;
				bEarlyDismount = true;
			}
		}

		if ( m_bGoingUp )
		{
			body->SetDesiredPosture( IBody::STAND );
		}

		float dismountZ = targetZ;
		if ( m_pExitArea )
		{
			if ( m_bGoingUp )
			{
				if ( bExitIsBehind )
				{
					dismountZ = Min( m_exitAreaCenter.z, targetZ );
				}
			}
			else
			{
				// Allow early drop-off at intermediate floors
				dismountZ = Max( m_exitAreaCenter.z, targetZ );
			}
		}

		// Check if we've reached the target exit height
		if ( m_bGoingUp ? ( currentZ >= dismountZ ) : ( currentZ <= dismountZ ) )
		{
			bWantsDismount = true;
			bEarlyDismount = false;
		}

		// A parapet between the ladder and a roof exit blocks the step off at floor height:
		// keep climbing until the hull clears it (stuck detection ends it if nothing does)
		const bool bLipInTheWay = bWantsDismount && m_bGoingUp && onLadder
			&& IsDismountBlocked( me, m_pExitArea ? toExit : m_ladderForward );

		if ( bWantsDismount && !bLipInTheWay )
		{
			EnterDismountPhase( me, bEarlyDismount ? "early" : "reached" );
			return Continue();
		}

		// Adjust vertical height based on target exit area
		bool bIsClimbingDown = false;
		if ( onLadder )
		{
			bool bShouldGoUp = m_bGoingUp;
			if ( m_pExitArea )
			{
				bShouldGoUp = ( currentZ < m_exitAreaCenter.z );
			}

			if ( bLipInTheWay )
			{
				bShouldGoUp = true;
			}

			// patch 76: a duration that reaches the user command (see neo_bot_ladder_moveup); -1 is the stock no-op
			const float flVerticalPress = neo_bot_ladder_moveup.GetBool() ? NEO_LADDER_VERTICAL_PRESS : -1.0f;
			if ( nSettle > 0 )
			{
				me->ReleaseMoveUpButton();	// patch 102: settling
				me->ReleaseMoveDownButton();
			}
			else if ( bShouldGoUp )
			{
				me->PressMoveUpButton( flVerticalPress );
			}
			else
			{
				// patch 101: not while a floor is under the feet - LadderMove would push the bot off the ladder
				if ( neo_bot_ladder_descend_forward.GetBool() && NeoFloorUnder( me ) )
				{
					me->ReleaseMoveDownButton();
				}
				else
				{
					me->PressMoveDownButton( flVerticalPress );
				}
				bIsClimbingDown = true;
			}
		}


		// A descent the locomotion has claimed is aimed by its DescendLadder();
		// a second MANDATORY aim here would hold the view back from it
		if ( m_bGoingUp || !mover->IsUsingLadder() || nSettle > 0 || NEOHarnessLadderDescentOff() )	// NEO-HARNESS-TEMP: off = always aim, as upstream
		{
			// Look at and move to the dismount height, slightly behind the ladder
			Vector lookTarget = m_ladder->GetPosAtHeight( dismountZ );
			lookTarget -= m_ladder->GetNormal() * 50.0f;
			if ( nSettle > 0 )
			{
				// patch 102 v2: settle on a steep point just inside the face (the dismount point can sit only ~48 deg down,
				// short of the 50 the settle waits for)
				lookTarget = m_ladder->GetPosAtHeight( Max( myPos.z - NEO_LADDER_SETTLE_LOOK_DROP, m_ladder->m_bottom.z ) ) - m_ladder->GetNormal() * 8.0f;
			}
			body->AimHeadTowards( lookTarget, IBody::MANDATORY, 0.1f, nullptr,
				m_bGoingUp ? "Climbing up (looking at dismount position)" : "Climbing down (looking at dismount position)" );
		}

		// patch 102: settling - nothing while aiming, forward into the face once square and steep
		if ( nSettle == 1 )
		{
			me->ReleaseForwardButton();
			me->ReleaseBackwardButton();
			me->ReleaseLeftButton();
			me->ReleaseRightButton();
		}
		else if ( nSettle == 2 )
		{
			me->PressForwardButton( 0.1f );
		}
		// patch 101: off a floor, forward into the face climbs down only once the view is steep - until then it climbs up
		else if ( bHoldForView || ( neo_bot_ladder_descend_forward.GetBool() && !m_bGoingUp && onLadder && NeoFloorUnder( me ) && me->EyeAngles().x < NEO_LADDER_STEEP_PITCH ) )
		{
			me->ReleaseForwardButton();
		}
		else
		{
			me->PressForwardButton(0.1f);
		}

		// patch 75: hold the bot across the ladder, at m_flLateralTarget inside its width
		bool bStrafing = false;
		if ( nSettle > 0 )
		{
			// patch 102: no sideways moves while settling
		}
		else if ( onLadder && neo_bot_ladder_lateral_hold.GetBool() )
		{
			const Vector normal = m_ladder->GetNormal();
			const Vector lateral( -normal.y, normal.x, 0.0f );	// the right hand of a bot facing into the ladder
			const Vector center = m_ladder->GetPosAtHeight( myPos.z );
			const float flLat = DotProduct( myPos - center, lateral );
			const float flHalf = MAX( 0.0f, m_ladder->m_width * 0.5f - 2.0f );
			const float flErr = flLat - Clamp( m_flLateralTarget, -flHalf, flHalf );
			if ( flErr > 3.0f )
			{
				me->PressLeftButton( 0.1f );
				bStrafing = true;
			}
			else if ( flErr < -3.0f )
			{
				me->PressRightButton( 0.1f );
				bStrafing = true;
			}
		}
		// patch 72: shimmy sideways while a nudge is running
		else if ( onLadder && m_nudgeTimer.HasStarted() && !m_nudgeTimer.IsElapsed() )
		{
			if ( m_nNudges % 2 )
			{
				me->PressRightButton( 0.1f );
			}
			else
			{
				me->PressLeftButton( 0.1f );
			}
			bStrafing = true;
		}

		// patch 76: LadderMove drops strafing while upmove is set - let go of MoveUp / MoveDown while moving sideways
		if ( bStrafing && neo_bot_ladder_moveup.GetBool() )
		{
			me->ReleaseMoveUpButton();
			me->ReleaseMoveDownButton();
		}
	}

	//------------------------------------------------------------
	// Ladder dismount phase
	//------------------------------------------------------------
	if ( m_bDismountPhase )
	{
		// Reached the target NavArea after the ladder
		// NEO-HARNESS-TEMP research arm (patch 52): the last known area switches to the exit area while
		// the bot is still in the air beside it (ghost ladder 1: 29 u off the catwalk's edge, over a
		// drop); only finish once it stands on the exit area itself
		const bool bOnExit = !neo_bot_ladder_exit_closest.GetBool() || NeoIsThinExit( m_pExitArea, me )
			|| ( mover->IsOnGround() && m_pExitArea && m_pExitArea->IsOverlapping( myPos, 0.0f ) );
		if ( m_pExitArea && me->GetLastKnownArea() == m_pExitArea && bOnExit )
		{
			return NEO_LADDER_DONE( "Reached next NavArea after dismount" );
		}

		// Safety timeout
		if ( m_dismountTimer.IsElapsed() )
		{
			return NEO_LADDER_DONE( "Dismount walk timed out" );
		}

		// Build look target toward exit area center with vertical bias preserved
		if ( m_pExitArea )
		{
			bool bDroppingEarly = ( myPos.z >= m_exitAreaCenter.z ); 
			// Maintain Z-height while on the ladder in the dismount phase
			if ( onLadder )
			{
				if ( myPos.z < m_exitAreaCenter.z )
				{
					me->PressMoveUpButton();
				}
				else
				{
					me->PressMoveDownButton();
				}
			}
			else
			{
				bDroppingEarly = true;
			}

			body->AimHeadTowards( m_exitAreaCenter, IBody::MANDATORY, 0.1f, nullptr, "Walking to exit area" );

			// Jump to detach from ladder if exit is not straight ahead, or if we have reached the exit height
			float dot = DotProduct( toExit, m_ladderForward );

			if ( dot < 0.5f || bDroppingEarly )
			{
				// Velocity kick to simulate a jump to the next NavArea
				if ( !m_bJumpedOffLadder )
				{
					NeoLogLadderEvent( me, "climbkick", m_ladder, m_bGoingUp, "dot=%.2f early=%d onladder=%d exit=%d exitz=%.0f",
						dot, bDroppingEarly ? 1 : 0, onLadder ? 1 : 0, m_pExitArea->GetID(), m_exitAreaCenter.z );
					me->PressJumpButton(); // mostly to trigger animation if possible

					mover->Reset(); // clear velocity cache in locomotion interface

					// patch 104: as far as the exit area, not a fixed ~80 u through the air (v2: to where the hull lands on it)
					float flKick = NEO_LADDER_KICK_SPEED;
					Vector vecKickDir = toExit;
					if ( neo_bot_ladder_kick_to_exit.GetBool() )
					{
						Vector vecToLand = NeoHullLandingPoint( m_pExitArea, myPos, body->GetHullWidth() * 0.5f ) - myPos;
						vecToLand.z = 0.0f;
						const float flDist = vecToLand.NormalizeInPlace();
						if ( flDist > 0.0f )
						{
							vecKickDir = vecToLand;
						}
						flKick = Clamp( flDist / NEO_LADDER_KICK_AIRTIME, NEO_LADDER_KICK_MIN, NEO_LADDER_KICK_SPEED );
					}

					Vector jumpVelocity = vecKickDir * flKick;
					jumpVelocity.z = 150.0f - me->GetAbsVelocity().z;

					me->ApplyAbsVelocityImpulse( jumpVelocity );
					m_bJumpedOffLadder = true;
				}
				else if ( !onLadder )
				{
					me->PressForwardButton();
				}
			}
			else
			{
				// Forward exit, just clamber over
				me->PressForwardButton();
			}

			if ( me->IsDebugging( NEXTBOT_PATH ) )
			{
				NDebugOverlay::Line( myPos, m_exitAreaCenter, 0, 255, 255, true, 0.1f );
			}
		}
		else
		{
			// No exit area resolved - just push forward along the ladder normal
			Vector pushDir = m_ladderForward;
			Vector pushTarget = myPos + 100.0f * pushDir;
			body->AimHeadTowards( pushTarget, IBody::MANDATORY, 0.1f, nullptr, "Dismount push forward" );
			me->PressForwardButton();
			
			if ( mover->IsOnGround() )
			{
				return NEO_LADDER_DONE( "Dismounted to ground" );
			}
		}
	}

	return Continue();
}

//---------------------------------------------------------------------------------------------
bool CNEOBotLadderClimb::IsDismountBlocked( CNEOBot *me, const Vector &toExit ) const
{
	const Vector &feet = me->GetLocomotionInterface()->GetFeet();
	const float hullWidth = me->GetBodyInterface()->GetHullWidth();
	const Vector traceEnd = feet + toExit * ( DISMOUNT_CLEARANCE_HULLS * hullWidth );

	trace_t tr;
	UTIL_TraceHull( feet, traceEnd, me->WorldAlignMins(), me->WorldAlignMaxs(), MASK_NPCSOLID, me, COLLISION_GROUP_NONE, &tr );

	return tr.DidHit();
}
//---------------------------------------------------------------------------------------------
void CNEOBotLadderClimb::EnterDismountPhase( CNEOBot *me, const char *why )
{
	me->SetAbsVelocity( vec3_origin ); // stop momentum
	m_bDismountPhase = true;
	m_dismountTimer.Start( DISMOUNT_TIMEOUT );
	ResolveExitArea( me );

	// patch 76: the step off over the top is a forward move, which upmove would override on the ladder
	if ( neo_bot_ladder_moveup.GetBool() )
	{
		me->ReleaseMoveUpButton();
		me->ReleaseMoveDownButton();
	}
	NeoLogLadderEvent( me, "climbdismount", m_ladder, m_bGoingUp, "why=%s exit=%d exitz=%.0f",
		why, m_pExitArea ? m_pExitArea->GetID() : -1, m_pExitArea ? m_exitAreaCenter.z : 0.0f );

	if ( me->IsDebugging( NEXTBOT_PATH ) )
	{
		DevMsg( "%s: Entering dismount phase (exit area %s)\n",
			me->GetDebugIdentifier(),
			m_pExitArea ? "found" : "NOT found" );
	}
}

//---------------------------------------------------------------------------------------------
void CNEOBotLadderClimb::OnEnd( CNEOBot *me, Action<CNEOBot> *nextAction )
{
	NeoSetLadderSettle( me, 0 );	// patch 102
	NeoLogLadderBeh( me, "climb", m_ladder, m_bGoingUp, "end" );

	me->StartLookingAroundForEnemies();
	me->ClearAttribute( CNEOBot::IGNORE_ENEMIES );

	// patch 76: do not carry a held MoveUp / MoveDown into whatever comes next
	if ( neo_bot_ladder_moveup.GetBool() )
	{
		me->ReleaseMoveUpButton();
		me->ReleaseMoveDownButton();
	}

	if ( me->IsDebugging( NEXTBOT_PATH ) )
	{
		DevMsg( "%s: Finished ladder climb\n", me->GetDebugIdentifier() );
	}
}

//---------------------------------------------------------------------------------------------
ActionResult<CNEOBot> CNEOBotLadderClimb::OnSuspend( CNEOBot *me, Action<CNEOBot> *interruptingAction )
{
	return Done( "OnSuspend: Cancel out of ladder climb, situation will likely become stale." );
}

//---------------------------------------------------------------------------------------------
ActionResult<CNEOBot> CNEOBotLadderClimb::OnResume( CNEOBot *me, Action<CNEOBot> *interruptingAction )
{
	return Done( "OnResume: Cancel out of ladder climb, situation is likely stale." );
}
