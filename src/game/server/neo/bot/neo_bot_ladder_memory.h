#ifndef NEO_BOT_LADDER_MEMORY_H
#define NEO_BOT_LADDER_MEMORY_H

#include "nav_ladder.h"

// NEO-HARNESS-TEMP research arm (2026-10-07, patch 134): neo_bot_ladder_fail_skip.
// A bot remembers the ladder climbs it failed itself, so one that fails the same climb twice
// routes around that ladder for a while instead of mounting it again (apparatus ladder 13 up:
// 41 mounts, 39 of them stuck under the landing slab).
class CNEOBotLadderMemory
{
public:
	CNEOBotLadderMemory()
	{
		Reset();
	}

	void Reset()
	{
		for ( int i = 0; i < MAX_ENTRIES; ++i )
		{
			m_entries[i].ladderID = INVALID_ID;
			m_entries[i].nFails = 0;
			m_entries[i].flLastFail = 0.0f;
			m_entries[i].flSkipUntil = 0.0f;
		}
	}

	// Returns true when this failure starts a skip
	bool OnClimbFailed( const CNavLadder *ladder, bool bGoingUp, float flSkipTime )
	{
		const float now = gpGlobals->curtime;
		Entry *entry = Find( ladder, bGoingUp );
		if ( !entry )
		{
			entry = Oldest();
			entry->ladderID = ladder->GetID();
			entry->bGoingUp = bGoingUp;
			entry->nFails = 0;
			entry->flSkipUntil = 0.0f;
		}

		// a failure long ago, or before a map change reset the clock, no longer counts
		if ( entry->nFails > 0 && ( now - entry->flLastFail > FAIL_WINDOW || entry->flLastFail > now ) )
		{
			entry->nFails = 0;
		}

		++entry->nFails;
		entry->flLastFail = now;
		if ( entry->nFails < FAILS_TO_SKIP )
		{
			return false;
		}

		entry->nFails = 0;
		entry->flSkipUntil = now + flSkipTime;
		return true;
	}

	bool IsSkipped( const CNavLadder *ladder, bool bGoingUp ) const
	{
		const float now = gpGlobals->curtime;
		const Entry *entry = Find( ladder, bGoingUp );
		return entry && now < entry->flSkipUntil && entry->flLastFail <= now;
	}

private:
	static constexpr unsigned int INVALID_ID = 0xFFFFFFFF;
	static constexpr int MAX_ENTRIES = 4;
	// two failed climbs of the same ladder and direction,
	static constexpr int FAILS_TO_SKIP = 2;
	// at most this many seconds apart, start a skip
	static constexpr float FAIL_WINDOW = 30.0f;

	struct Entry
	{
		unsigned int ladderID;
		bool bGoingUp;
		int nFails;
		float flLastFail;
		float flSkipUntil;
	};

	Entry *Find( const CNavLadder *ladder, bool bGoingUp )
	{
		return const_cast< Entry * >( static_cast< const CNEOBotLadderMemory * >( this )->Find( ladder, bGoingUp ) );
	}

	const Entry *Find( const CNavLadder *ladder, bool bGoingUp ) const
	{
		for ( int i = 0; i < MAX_ENTRIES; ++i )
		{
			if ( m_entries[i].ladderID == ladder->GetID() && m_entries[i].bGoingUp == bGoingUp )
			{
				return &m_entries[i];
			}
		}
		return nullptr;
	}

	Entry *Oldest()
	{
		Entry *oldest = &m_entries[0];
		for ( int i = 0; i < MAX_ENTRIES; ++i )
		{
			if ( m_entries[i].ladderID == INVALID_ID )
			{
				return &m_entries[i];
			}

			if ( m_entries[i].flLastFail < oldest->flLastFail )
			{
				oldest = &m_entries[i];
			}
		}
		return oldest;
	}

	Entry m_entries[ MAX_ENTRIES ];
};

#endif // NEO_BOT_LADDER_MEMORY_H
