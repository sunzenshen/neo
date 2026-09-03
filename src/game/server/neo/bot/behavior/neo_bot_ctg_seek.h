#pragma once

#include "bot/behavior/neo_bot_seek_and_destroy.h"

//
// CTG game mode behavior dispatcher
//
class CNEOBotCtgSeek : public CNEOBotSeekAndDestroy
{
public:
	CNEOBotCtgSeek( float duration = -1.0f ) : CNEOBotSeekAndDestroy( duration ) { }

	virtual ActionResult< CNEOBot >	Update( CNEOBot *me, float interval ) override;
	virtual const char *GetName( void ) const override { return "ctgSeek"; };

protected:
	virtual void RecomputeSeekPath( CNEOBot *me ) override;

private:
	// The pre-positioning race estimate (sv_neo_bot_ctg_seek_preposition_ratio) is decided once
	// per round, not re-litigated on every RecomputeSeekPath - a bot commits to either racing for
	// the ghost or covering the threatened zone, and any later repath (stuck, blocked, arrived)
	// just falls back to plain ghost-seeking rather than re-running the comparison. This instance
	// can outlive a single round (nothing in Update() ends it at a round boundary), so the decision
	// is tagged with the round it was made for rather than a plain "already decided" flag.
	int m_iPrepositionDecidedRound = -1;
};
