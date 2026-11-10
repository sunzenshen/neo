#pragma once

class CNEOBot;
class CNavArea;

// A ghost carrier's teammates learn the position of the enemy the carrier is looking at,
// as a human carrier's voice callout would tell them
namespace NEOBotGhostCallout
{
	// Checks the carrier's aim once per callout interval
	void Update();

	// How long a carrier must hold its aim on an enemy for one callout to read it
	float GetMinAimHoldTime();

	// Forget the callout time and booted carry, e.g. on a map change where curtime restarts
	void Reset();

	// NEO TODO: research for the CTG team-play A/B (neo_bot_research_callouts), the PR carries the chosen behavior without it
	// Extra path cost per unit of length through an area the bot's team was told is exposed to an enemy
	float GetCalledExposureCost( const CNEOBot *pBot, const CNavArea *pArea );

	// Path costs computed while this lives avoid the areas the carrier's reports expose,
	// so only the CTG carrier and escort routes change, not an attack on an enemy
	class CCalledExposureScope
	{
	public:
		explicit CCalledExposureScope( const CNEOBot *pBot );
		~CCalledExposureScope();

	private:
		const CNEOBot *m_pPrevBot;
	};
}
