#pragma once

class CNEOBot;
class CNEO_Player;
template < typename Actor > class Action;

// A bot that sees more enemies than it has support falls back toward its teammates
// instead of fighting alone, since the side with more players at a fight wins most fights
namespace NEOBotFallback
{
	enum ETrigger
	{
		TRIGGER_OUTNUMBERED,		// more enemies seen than the bot and its nearby teammates
		TRIGGER_UNSEEN_ALONE,		// alone, and saw an enemy that has not noticed the bot

		TRIGGER_COUNT
	};

	// The fall back to start now, or NULL when the bot should fight where it is
	Action< CNEOBot > *StartFallBack( CNEOBot *me );

	// Teammates within support range (not counting the bot) and enemies the bot saw recently
	void CountLocalSides( CNEOBot *me, int *pMates, int *pEnemies );

	// True when the bot has noticed it was shot at or that the enemy it watches is aiming at it
	bool IsSpotted( CNEOBot *me );

	// Keeps the bot from opening fire for a short while; refreshed every update by the caller
	void HoldFire( CNEOBot *me );
	bool HoldsFire( CNEOBot *me );

	// Logs the end of a fall back toward a teammate
	void OnFallBackEnd( CNEOBot *me, ETrigger trigger, const char *pszReason, float flDuration );
}
