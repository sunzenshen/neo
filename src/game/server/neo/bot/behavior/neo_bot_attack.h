#pragma once

#include "NextBotBehavior.h"
#include "Path/NextBotChasePath.h"

class CNEOBot;


//-------------------------------------------------------------------------------
class CNEOBotAttack : public Action< CNEOBot >
{
public:
	CNEOBotAttack( void );
	CNEOBotAttack( const Vector &goalPosition );
	virtual ~CNEOBotAttack() { }

	virtual ActionResult< CNEOBot >	OnStart( CNEOBot *me, Action< CNEOBot > *priorAction );
	virtual ActionResult< CNEOBot >	Update( CNEOBot *me, float interval );

	virtual EventDesiredResult< CNEOBot > OnStuck( CNEOBot *me );
	virtual EventDesiredResult< CNEOBot > OnMoveToSuccess( CNEOBot *me, const Path *path );
	virtual EventDesiredResult< CNEOBot > OnMoveToFailure( CNEOBot *me, const Path *path, MoveToFailureType reason );

	virtual QueryResultType	ShouldRetreat( const INextBot *me ) const;							// is it time to retreat?
	virtual QueryResultType ShouldHurry( const INextBot *me ) const;					// are we in a hurry?

	virtual const char *GetName( void ) const	{ return "Attack"; };

	// True when a bot already engaging a threat should drop into CNEOBotKnifeRush: it owns a knife
	// and can see a living enemy, and either sv_neo_bot_force_knife_fight is set (debug / reviewer
	// evaluation) or it is completely out of bullets - both primary and secondary weapon absent, or
	// with clip and reserve at zero. Checked before the "too close, make space" range test so a bot
	// that still has ammo never takes the knife path.
	static bool ShouldKnifeRush( CNEOBot *me );

private:
	bool m_bSawEnemySinceLastPathCompute; // throttles m_attackCoverArea search
	const CNavArea *m_attackCoverArea; // attempting to advance towards this cover area
	const CNavArea *m_goalArea; // if set, engage enemies while moving towards this destination
	PathFollower m_path;
	ChasePath m_chasePath;
	CountdownTimer m_grenadeThrowCooldownTimer;
};
