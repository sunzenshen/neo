#pragma once

class CNEOBot;
class CBaseCombatCharacter;
class CTakeDamageInfo;
class CNEOBotTradeResponse;

// When a teammate is killed, the nearest teammates that could notice it go after the killer
// for a few seconds, so the team answers a lost fight with a trade instead of walking on
namespace NEOBotTrade
{
	// How long a responder answers a death: long enough to cross a room, short enough not to drop the round plan
	static constexpr float RESPONSE_WINDOW = 5.0f;

	// Picks the responders to a player's death; every bot gets the same death event, so only the first call works
	void OnPlayerKilled( CBaseCombatCharacter *pVictim, const CTakeDamageInfo &info );

	// The trade response this bot should start now, or nullptr; taking it clears the order
	CNEOBotTradeResponse *TakeResponse( CNEOBot *me );

	// Bookkeeping for the cap on how many teammates respond at once
	void OnResponseStart( const CNEOBot *me, float flEndTime );
	void OnResponseEnd( const CNEOBot *me );

	bool IsLogging();

	// Research arm 2: also answer while chasing or cutting off the enemy ghost carrier
	bool RespondsDuringEnemyCarry();
}
