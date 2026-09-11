#pragma once

#include "Path/NextBotPathFollow.h"

#include "../neo_bot_contextual_query_interface.h"

class CNEOBotMainAction : public Action< CNEOBot >, public CNEOBotContextualQueryInterface
{
public:
	virtual Action< CNEOBot > *InitialContainedAction( CNEOBot *me );

	virtual ActionResult< CNEOBot >	OnStart( CNEOBot *me, Action< CNEOBot > *priorAction );
	virtual ActionResult< CNEOBot >	Update( CNEOBot *me, float interval );

	virtual EventDesiredResult< CNEOBot > OnKilled( CNEOBot *me, const CTakeDamageInfo &info );
	virtual EventDesiredResult< CNEOBot > OnInjured( CNEOBot *me, const CTakeDamageInfo &info );
	virtual EventDesiredResult< CNEOBot > OnContact( CNEOBot *me, CBaseEntity *other, CGameTrace *result = NULL );
	virtual EventDesiredResult< CNEOBot > OnStuck( CNEOBot *me );

	virtual EventDesiredResult< CNEOBot > OnOtherKilled( CNEOBot *me, CBaseCombatCharacter *victim, const CTakeDamageInfo &info );

	// IContextualQuery implementation
	virtual QueryResultType ShouldAttack( const INextBot *me, const CKnownEntity *them ) const;
	virtual QueryResultType	ShouldRetreat( const INextBot *me ) const;							// is it time to retreat?
	virtual QueryResultType	ShouldHurry( const INextBot *me ) const;							// are we in a hurry?
	
	// CNEOBotContextualQueryInterface implementation
	QueryResultType ShouldWalk(const CNEOBot *me, const QueryResultType qShouldAimQuery) const final;
	QueryResultType ShouldAim(const CNEOBot *me, const bool bWepHasClip) const final;

	virtual Vector SelectTargetPoint( const INextBot *me, const CBaseCombatCharacter *subject ) const;		// given a subject, return the world space position we should aim at
	virtual QueryResultType IsPositionAllowed( const INextBot *me, const Vector &pos ) const;

	virtual const CKnownEntity *	SelectMoreDangerousThreat( const INextBot *me, 
															   const CBaseCombatCharacter *subject,
															   const CKnownEntity *threat1, 
															   const CKnownEntity *threat2 ) const;	// return the more dangerous of the two threats to 'subject', or NULL if we have no opinion

	virtual const char *GetName( void ) const	{ return "MainAction"; };

private:
	CountdownTimer m_reloadTimer;
	CountdownTimer m_reconSuperJumpPathCheckTimer;
	mutable CountdownTimer m_aimAdjustTimer;
	mutable float m_aimErrorRadius;
	mutable float m_aimErrorAngle;

	float m_yawRate;
	float m_priorYaw;
	IntervalTimer m_steadyTimer;

	// NEO-HARNESS-TEMP (proposal 0019): head-motion proxy telemetry, gated by
	// sv_neo_bot_look_telemetry. Strip before PR.
	float m_htPrevYaw = 0.0f;
	float m_htPrevDYaw = 0.0f;
	int m_htTicks = 0;
	int m_htNoThreatTicks = 0;
	float m_htNoThreatYawTravel = 0.0f;
	int m_htReversals = 0;
	float m_htMaxRate = 0.0f;
	int m_htRateBins[8] = {};
	CountdownTimer m_htDumpTimer;
	CountdownTimer m_htGazeTimer;
	// reaction-latency tracking: from "threat first known" to on-target / first shot
	EHANDLE m_htReactThreat;
	float m_htReactT0 = 0.0f;
	float m_htReactInitErr = 0.0f;
	bool m_htReactAimed = false;
	bool m_htReactFired = false;
	bool m_htReactVis = false;

	bool m_isWaitingForFullReload;

	void FireWeaponAtEnemy( CNEOBot *me );
	void FireBalcAtEnemy( CNEOBot *me, CNEOBaseCombatWeapon *myWeapon, const CKnownEntity *threat, float threatRange );
	float GetFireDurationByDifficulty( CNEOBot *me ) const;

	CHandle< CBaseEntity > m_lastTouch;
	float m_lastTouchTime;

	bool IsImmediateThreat( const CBaseCombatCharacter *subject, const CKnownEntity *threat ) const;
	const CKnownEntity *SelectCloserThreat( CNEOBot *me, const CKnownEntity *threat1, const CKnownEntity *threat2 ) const;
	const CKnownEntity *GetHealerOfThreat( const CKnownEntity *threat ) const;

	const CKnownEntity *SelectMoreDangerousThreatInternal( const INextBot *me, 
														   const CBaseCombatCharacter *subject,
														   const CKnownEntity *threat1, 
														   const CKnownEntity *threat2 ) const;


	void Dodge( CNEOBot *me );
	void ReconConsiderSuperJump(CNEOBot *me);

	IntervalTimer m_undergroundTimer;
};
