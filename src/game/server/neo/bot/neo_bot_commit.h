#pragma once

class CNEOBot;
class CBaseCombatCharacter;
class CTakeDamageInfo;

// A bot that is in a fight its team can win holds its ground instead of backing off,
// since the first fights of a round decide most rounds
namespace NEOBotCommit
{
	// The retreat a caller is about to start; names the suppressed retreat in the log
	enum ERetreatSite
	{
		RETREAT_BACK_OFF,			// tactical monitor: the intention asks to retreat
		RETREAT_RELOAD_COVER,		// tactical monitor: retreat to cover for a full reload
		RETREAT_TOO_CLOSE,			// attack: enemy is closer than the desired range
		RETREAT_AT_GOAL,			// attack: reached the attack area and the enemy is still there
		RETREAT_IN_PROGRESS,		// retreat to cover: already backing off

		RETREAT_SITE_COUNT
	};

	// Every bot hears of every death; a bot remembers a teammate's death it could have noticed
	void OnOtherKilled( CNEOBot *me, CBaseCombatCharacter *pVictim, const CTakeDamageInfo &info );

	// True when the bot should keep fighting instead of starting or continuing the given retreat
	bool HoldsGround( CNEOBot *me, ERetreatSite site );

	// True when an empty primary should give way to a loaded secondary at any range
	bool PrefersSecondaryWhenEmpty( CNEOBot *me );
}
