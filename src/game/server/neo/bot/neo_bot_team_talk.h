#pragma once

class CNEOBot;
class CNavArea;
class CTakeDamageInfo;

// Bots tell their team what players say on voice: who killed them and where.
// The team learns a position snapshot after a speaking delay, never a live position or line of sight.
namespace NEOBotTeamTalk
{
	// A dying bot calls out its killer and marks where it fell
	void OnBotKilled( CNEOBot *pVictim, const CTakeDamageInfo &info );

	// Extra path cost of an area where a teammate died recently
	float GetFreshDeathPathCost( const CNEOBot *me, const CNavArea *area );

	// Delivers callouts whose speaking delay has passed
	void Update();

	// Forget every callout and mark, e.g. on a map change where curtime restarts
	void Reset();
}
