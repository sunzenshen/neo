#pragma once

#include "NextBotBehavior.h"
#include "bot/neo_bot.h"

class CNavLadder;
class PathFollower;

//----------------------------------------------------------------------------------------------------------------
/**
 * Implementation based on ladder climbing logic in https://github.com/Dragoteryx/drgbase/
 * Behavior that handles the actual ladder climbing.
 * DOES disable enemy awareness while climbing.
 * Checks if still on ladder - returns Done if knocked off to reevaluate situation.
 */
class CNEOBotLadderClimb : public Action<CNEOBot>
{
public:
	CNEOBotLadderClimb( const CNavLadder *ladder, bool goingUp );
	virtual ~CNEOBotLadderClimb() = default;

	virtual const char *GetName() const override { return "LadderClimb"; }

	virtual ActionResult<CNEOBot> OnStart( CNEOBot *me, Action<CNEOBot> *priorAction ) override;
	virtual ActionResult<CNEOBot> Update( CNEOBot *me, float interval ) override;
	virtual void OnEnd( CNEOBot *me, Action<CNEOBot> *nextAction ) override;
	virtual ActionResult<CNEOBot> OnSuspend( CNEOBot *me, Action<CNEOBot> *interruptingAction ) override;
	virtual ActionResult<CNEOBot> OnResume( CNEOBot *me, Action<CNEOBot> *interruptingAction ) override;

	static bool TryStuckRecovery( CNEOBot *me, INextBotEventResponder *behavior );

	enum RecoveryWatchPhase
	{
		RECOVERY_APPROACH,
		RECOVERY_CLIMB
	};
	static void BeginRecoveryWatch( CNEOBot *me, const CNavLadder *ladder, bool goingUp, const void *owner );
	static void EndRecoveryWatch( CNEOBot *me, const void *owner );
	static void UpdateRecoveryWatch( CNEOBot *me, const void *owner, const CNavArea *exitArea, RecoveryWatchPhase phase );
	static void LatchRecoveryDismount( CNEOBot *me, const void *owner );
	static void UpdateRecovery( CNEOBot *me );

	enum ExpiryTimer
	{
		EXPIRY_CLIMB,
		EXPIRY_DISMOUNT,
		EXPIRY_STALL,
		EXPIRY_APPROACH,
		EXPIRY_MISS
	};
	static bool TryExpiryTeleport( CNEOBot *me, const CNavLadder *ladder, bool goingUp,
		const CNavArea *pathExit, const Vector *dismountPos, ExpiryTimer timer );

private:
	friend class CNEOBot;
	enum RecoveryTrigger
	{
		RECOVERY_SLIP = 1 << 0,
		RECOVERY_BOUNCE = 1 << 1,
		RECOVERY_STALL = 1 << 2,
		RECOVERY_STUCK = 1 << 3,
		RECOVERY_EXIT_TIMEOUT = 1 << 4,
		RECOVERY_BOUNCE_LANDING = 1 << 5
	};
	static bool IsRecoveryOwner( CNEOBot *me, const void *owner );
	static bool HasRecoveryIntent( CNEOBot *me );
	static void RecoveryDecision( CNEOBot *me, RecoveryTrigger trigger, RecoveryWatchPhase phase, const char *gate );
	struct LandingCandidate
	{
		Vector position;
		float distanceSqr;
	};
	struct LandingProbe
	{
		const char *gate = "none";
		int hit = -1;
	};
	static bool ResolveRecoveryExit( CNEOBot *me );
	enum LandingSearchPurpose
	{
		WATCH_GEOMETRY,
		RECOVERY_PLACEMENT,
		RECOVERY_HANDOFF
	};
	static void FindRecoveryLandings( CNEOBot *me, LandingSearchPurpose purpose );
	static bool ValidateLanding( CNEOBot *me, const Vector &point, Vector *landing, LandingProbe *probe, LandingSearchPurpose purpose );
	static bool CheckLandingSupport( CNEOBot *me, const Vector &sample, float floorZ, LandingProbe *probe, LandingSearchPurpose purpose );
	static void SaveLandingProbe( LandingProbe *probe, const trace_t &trace, const char *gate );
	static int CompareLandings( const LandingCandidate *left, const LandingCandidate *right );
	static bool IsRecoveryCorridor( CNEOBot *me );
	static bool StartRecoveryC( CNEOBot *me );
	static bool StartRecoveryA( CNEOBot *me, RecoveryWatchPhase phase, bool slide );
	static void UpdateRecoverySlide( CNEOBot *me );
	static bool FindRecoveryLaunch( CNEOBot *me, RecoveryWatchPhase phase );
	static bool TryRecoveryLaunchAt( CNEOBot *me, float height, const Vector &normal );
	static bool FindLaunchContact( CNEOBot *me, float height, const Vector &normal, Vector *point, trace_t *face );
	static bool TraceLaunchFace( CNEOBot *me, const Vector &point, const Vector &normal, trace_t *face );
	static bool SolveRecoveryFlight( CNEOBot *me, const Vector &start, Vector *velocity, float *flightTime );
	static bool CheckRecoveryArc( CNEOBot *me, const Vector &start, const Vector &velocity, float flightTime );
	static bool PlaceRecoveryLaunch( CNEOBot *me );
	static void LaunchRecovery( CNEOBot *me );
	static void UpdateRecoveryFlight( CNEOBot *me );
	static void FallbackRecoveryC( CNEOBot *me, const char *gate );
	static bool CheckRecoverySource( CNEOBot *me );
	static bool PlaceRecoveryC( CNEOBot *me );
	static void StopRecovery( CNEOBot *me, const char *result, const char *gate );
	static bool FindRecoveryHandoff( CNEOBot *me );
	static bool CheckRecoveryWalk( CNEOBot *me, const Vector &target );
	static bool RecomputeRecoveryPath( CNEOBot *me );
	static void BlockRecoveryLadder( CNEOBot *me, float duration );
	static void ClearRecoveryInputs( CNEOBot *me );
	static void TryRecoveryReturn( CNEOBot *me );

	void EnterDismountPhase( CNEOBot *me );
	void ResolveExitArea( CNEOBot *me );
	bool TeleportOnExpiry( CNEOBot *me, ExpiryTimer timer );
	bool IsMissedLanding( CNEOBot *me ) const;
	const CNavArea *FindLanding( const PathFollower *path, const CNavArea *pathExit ) const;
	const CNavArea *LadderEndArea() const;
	bool IsNarrowLanding( CNEOBot *me ) const;
	Vector GetDismountPos( CNEOBot *me ) const;
	bool IsDismountBlocked( CNEOBot *me, const Vector &toExit ) const;
	Vector GetCrawlDirection( const Vector &toExit ) const;
	float GetCrawlFeetZ() const;
	bool IsCrawlOnlyExit( CNEOBot *me, const Vector &toExit ) const;
	void UpdateCrawlDismount( CNEOBot *me, const Vector &toExit, bool bOnLadder );
	void ClaimLadder( CNEOBot *me ) const;

	const CNavLadder *m_ladder;
	const CNavArea *m_pExitArea;

	bool m_bGoingUp;
	bool m_bDismountPhase;
	bool m_bJumpedOffLadder;
	bool m_bCrawlDismount;	// stepping off crouched, through a gap only a crouched body fits
	bool m_bCrawlPushed;	// set in the gap and pushed off the ladder

	float m_flLastZ;
	Vector m_dismountPos;
	Vector m_ladderForward;

	CountdownTimer m_timeoutTimer;
	CountdownTimer m_stuckTimer;
	CountdownTimer m_dismountTimer;
	CountdownTimer m_nudgeTimer;	// shimmying sideways past a snag
	int m_nNudges;

	static constexpr float STUCK_CHECK_INTERVAL = 0.4f;
	static constexpr float STUCK_Z_TOLERANCE = 2.0f;
	static constexpr float DISMOUNT_TIMEOUT = 3.0f;	// Max time to walk toward exit area after leaving ladder
	static constexpr float SAFE_FALL_DIST = 200.0f;	// Max height to safely drop off a ladder
	static constexpr float MAX_DEVIATION_DIST = 100.0f; // Distance from ladder before canceling
	static constexpr float DISMOUNT_CLEARANCE_HULLS = 1.5f; // Hull widths toward the exit that must be clear to step off
	static constexpr float NARROW_LANDING_HULLS = 0.5f; // Hull widths deep a landing must be to stand on (measured on dawn and dusk L1's 7 u wall cap)
	static constexpr float LANDING_INSET = 16.0f; // How far inside a landing's near edge the dismount aims: half a hull, so the whole hull lands
	static constexpr float CRAWL_FEET_ABOVE_TOP = 4.0f; // Crouched, the feet step off this far above the ladder's top
	static constexpr float CRAWL_DUCK_RANGE = 64.0f; // How far below that height the climb ducks for a crawl exit: a few bot updates' climb
	static constexpr float CRAWL_SNAP_DIST = 24.0f; // How far from that height the bot may be set there: a little over a bot update's climb
	static constexpr float CRAWL_PUSH_SPEED = 150.0f; // How fast the bot is pushed off the ladder into the gap,
	static constexpr float CRAWL_PUSH_RISE_SPEED = 40.0f; // and up, a unit's rise
	static constexpr float CRAWL_CROUCH_HOLD = 0.3f; // Seconds each crouch press lasts, outliving a bot update
	static constexpr int STALL_NUDGES = 3; // Sideways shimmies before a stalled climb jumps off (measured on subsurface L1 and saitama L2)
};
