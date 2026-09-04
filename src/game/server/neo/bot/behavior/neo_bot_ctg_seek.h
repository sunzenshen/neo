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
	// Freezetime only: split the defence by class so a slow goalie pre-covers the threatened wing
	// cap while the fast class still races the loose ghost. Returns a change/suspend result when it
	// has taken over, Continue() otherwise.
	ActionResult< CNEOBot > ConsiderFreezetimeIntercept( CNEOBot *me );
};
