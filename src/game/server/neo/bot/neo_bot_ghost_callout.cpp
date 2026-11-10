#include "cbase.h"
#include "bot/neo_bot_ghost_callout.h"
#include "bot/neo_bot.h"
#include "neo_gamerules.h"
#include "neo_player.h"
#include "weapon_ghost.h"
#include "nav_mesh.h"
#include <utlhashtable.h>

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

ConVar sv_neo_bot_ghost_callout_interval( "sv_neo_bot_ghost_callout_interval", "1.5", FCVAR_CHEAT,
	"How often bots learn the enemy at the center of their ghost carrier's aim.", true, 0.1f, false, 0 );

// NEO TODO: research gates for the CTG team-play A/B, the PR carries the chosen behavior without them
ConVar neo_bot_research_team_arm( "neo_bot_research_team_arm", "0", FCVAR_CHEAT,
	"Research: 0 = the research cvars apply to every bot, 2 or 3 = only to that team's bots", true, 0, true, 3 );

ConVar neo_bot_research_callouts( "neo_bot_research_callouts", "0", FCVAR_CHEAT,
	"Research: 0 = off, 1 = a bot ghost carrier reports the nearest enemies its ghost shows and its team's CTG routes "
	"avoid the areas those enemies see, 2 = as 1, and teammates also learn each reported enemy's position", true, 0, true, 2 );

ConVar neo_bot_research_callouts_log( "neo_bot_research_callouts_log", "0", FCVAR_CHEAT,
	"Research: print a NEO_CALLOUT line for each ghost carrier report its team hears", true, 0, true, 1 );

namespace NEOBotGhostCallout
{
	// A carrier calls out an enemy within this angle of the center of their view
	static constexpr float CALLOUT_MAX_ANGLE_DEG = 30.0f;

	// Time a carrier takes to turn onto a new enemy before a callout can read its aim
	static constexpr float AIM_TURN_TIME = 0.5f;

	static float s_flNextCalloutTime = 0.0f;

	// The carrier whose ghost has shown beacons during this carry, so they know where enemies are
	static int s_iBootedCarrier = 0;

	// A bot carrier reports what its ghost shows at a talking pace,
	static constexpr float REPORT_INTERVAL = 2.0f;
	// naming the nearest few enemies,
	static constexpr int REPORT_MAX_ENEMIES = 3;
	// and its team knows once the report is spoken
	static constexpr float REPORT_SPEAK_DELAY = 0.5f;

	// A reported enemy exposes the areas it may see from its own area within rifle range,
	static constexpr float REPORT_EXPOSURE_RANGE = 2000.0f;
	// and the team trusts that for a while after the last report naming that area
	static constexpr float REPORT_DECAY_TIME = 8.0f;

	// A freshly exposed area costs a route up to this many times its length more
	static constexpr float REPORT_EXPOSURE_COST = 3.0f;

	struct Report_t
	{
		int m_iTeam;	// 0 while no report waits to be spoken
		float m_flSpokenTime;
		int m_nEnemies;
		int m_iEnemyIndex[ REPORT_MAX_ENEMIES ];
		unsigned int m_iAreaID[ REPORT_MAX_ENEMIES ];
		int m_nRelayed;
	};

	static Report_t s_pendingReport = {};
	static float s_flNextReportTime = 0.0f;

	// For each team, a nav area id and when the newest report exposing it was spoken
	static CUtlHashtable< unsigned int, float > s_exposedAreas[ TEAM__TOTAL ];

	// For the log: path cost evaluations against the team's exposed areas, and the penalized ones, since its last report
	static int s_nCostQueries[ TEAM__TOTAL ] = {};
	static int s_nCostHits[ TEAM__TOTAL ] = {};

	// The bot whose CTG carrier or escort route is being computed
	static const CNEOBot *s_pExposureScopeBot = nullptr;

	static bool IsResearchTeam( int iTeam )
	{
		if ( iTeam < 0 || iTeam >= TEAM__TOTAL || neo_bot_research_callouts.GetInt() == 0 )
		{
			return false;
		}

		const int iArm = neo_bot_research_team_arm.GetInt();
		return ( iArm == 0 ) || ( iArm == iTeam );
	}

	// Marks the areas a called-out enemy may see as exposed when the report was spoken
	class CMarkExposedAreas
	{
	public:
		CMarkExposedAreas( CUtlHashtable< unsigned int, float > &exposed, const Vector &vecFrom, float flSpokenTime ) :
			m_exposed( exposed ),
			m_vecFrom( vecFrom ),
			m_flSpokenTime( flSpokenTime ),
			m_nMarked( 0 )
		{
		}

		bool operator()( CNavArea *pArea )
		{
			if ( pArea->GetCenter().DistToSqr( m_vecFrom ) > Square( REPORT_EXPOSURE_RANGE ) )
			{
				return true;
			}

			m_exposed[ m_exposed.Insert( pArea->GetID(), m_flSpokenTime ) ] = m_flSpokenTime;
			++m_nMarked;
			return true;
		}

		int GetMarkedCount() const { return m_nMarked; }

	private:
		CUtlHashtable< unsigned int, float > &m_exposed;
		Vector m_vecFrom;
		float m_flSpokenTime;
		int m_nMarked;
	};

	// The carrier names the nearest enemies its booted ghost shows, by the area each stands in
	static void SampleReport( CNEOBot *pCarrier )
	{
		struct Candidate_t
		{
			CNEO_Player *m_pEnemy;
			float m_flDistSq;
		};

		Candidate_t nearest[ REPORT_MAX_ENEMIES ];
		int nFound = 0;
		const Vector &vecCarrier = pCarrier->GetAbsOrigin();
		for ( int i = 1; i <= gpGlobals->maxClients; ++i )
		{
			CNEO_Player *pEnemy = ToNEOPlayer( UTIL_PlayerByIndex( i ) );
			if ( !pEnemy || !pEnemy->IsAlive() || pCarrier->InSameTeam( pEnemy ) )
			{
				continue;
			}

			if ( !pCarrier->IsRevealedByMyGhost( pEnemy ) || !pEnemy->GetLastKnownArea() )
			{
				continue;
			}

			// Insert in distance order, dropping the farthest once the report is full
			const float flDistSq = vecCarrier.DistToSqr( pEnemy->GetAbsOrigin() );
			int iSlot = nFound;
			while ( iSlot > 0 && nearest[ iSlot - 1 ].m_flDistSq > flDistSq )
			{
				if ( iSlot < REPORT_MAX_ENEMIES )
				{
					nearest[ iSlot ] = nearest[ iSlot - 1 ];
				}

				--iSlot;
			}

			if ( iSlot < REPORT_MAX_ENEMIES )
			{
				nearest[ iSlot ].m_pEnemy = pEnemy;
				nearest[ iSlot ].m_flDistSq = flDistSq;
				nFound = Min( nFound + 1, REPORT_MAX_ENEMIES );
			}
		}

		if ( nFound == 0 )
		{
			return;
		}

		Report_t &report = s_pendingReport;
		report.m_iTeam = pCarrier->GetTeamNumber();
		report.m_flSpokenTime = gpGlobals->curtime + REPORT_SPEAK_DELAY;
		report.m_nEnemies = nFound;
		report.m_nRelayed = 0;
		for ( int k = 0; k < nFound; ++k )
		{
			report.m_iEnemyIndex[ k ] = nearest[ k ].m_pEnemy->entindex();
			report.m_iAreaID[ k ] = nearest[ k ].m_pEnemy->GetLastKnownArea()->GetID();
		}

		if ( neo_bot_research_callouts.GetInt() < 2 )
		{
			return;
		}

		// A position snapshot as of this look, as master's aim callout gives
		for ( int i = 1; i <= gpGlobals->maxClients; ++i )
		{
			CNEOBot *pMate = ToNEOBot( UTIL_PlayerByIndex( i ) );
			if ( !pMate || !pMate->IsAlive() || pMate == pCarrier || !pMate->InSameTeam( pCarrier ) )
			{
				continue;
			}

			for ( int k = 0; k < nFound; ++k )
			{
				pMate->GetVisionInterface()->UpdateKnownEntityPosition( nearest[ k ].m_pEnemy );
				++report.m_nRelayed;
			}
		}
	}

	// The team hears the report: the areas each named enemy may see become exposed
	static void SpeakReport()
	{
		Report_t &report = s_pendingReport;
		CUtlHashtable< unsigned int, float > &exposed = s_exposedAreas[ report.m_iTeam ];

		int nMarked = 0;
		char szEnemies[ 64 ] = "";
		for ( int k = 0; k < report.m_nEnemies; ++k )
		{
			const int nLen = V_strlen( szEnemies );
			V_snprintf( szEnemies + nLen, sizeof( szEnemies ) - nLen, "%s%d", k ? "," : "", report.m_iEnemyIndex[ k ] );

			CNavArea *pCalled = TheNavMesh->GetNavAreaByID( report.m_iAreaID[ k ] );
			if ( !pCalled )
			{
				continue;
			}

			CMarkExposedAreas mark( exposed, pCalled->GetCenter(), report.m_flSpokenTime );
			mark( pCalled );
			pCalled->ForAllPotentiallyVisibleAreas( mark );
			nMarked += mark.GetMarkedCount();
		}

		if ( neo_bot_research_callouts_log.GetBool() )
		{
			Msg( "NEO_CALLOUT report t=%.1f team=%d n=%d enemies=%s marked=%d board=%d queries=%d hits=%d relayed=%d\n",
				gpGlobals->curtime, report.m_iTeam, report.m_nEnemies, szEnemies, nMarked, exposed.Count(),
				s_nCostQueries[ report.m_iTeam ], s_nCostHits[ report.m_iTeam ], report.m_nRelayed );
		}

		s_nCostQueries[ report.m_iTeam ] = 0;
		s_nCostHits[ report.m_iTeam ] = 0;
		report.m_iTeam = 0;
	}

	// A bot carrier with a booted ghost reports on a timer, and a report reaches its team once spoken
	static void UpdateReports( CNEO_Player *pCarrier )
	{
		if ( s_pendingReport.m_iTeam != 0 && gpGlobals->curtime >= s_pendingReport.m_flSpokenTime )
		{
			SpeakReport();
		}

		if ( gpGlobals->curtime < s_flNextReportTime )
		{
			return;
		}

		CNEOBot *pBot = ToNEOBot( pCarrier );
		if ( !pBot || !pBot->IsAlive() || !IsResearchTeam( pBot->GetTeamNumber() ) || !pBot->GetBeaconingGhost() )
		{
			return;
		}

		s_flNextReportTime = gpGlobals->curtime + REPORT_INTERVAL;
		SampleReport( pBot );
	}

	// Remember the carrier once their ghost shows beacons, by the beacon HUD's rule
	static void UpdateBootedCarrier( CNEO_Player *pCarrier )
	{
		if ( s_iBootedCarrier != pCarrier->entindex() )
		{
			s_iBootedCarrier = pCarrier->GetBeaconingGhost() ? pCarrier->entindex() : 0;
		}
	}

	// The enemy within the ghost's reveal range that is closest to the center of the carrier's view.
	// No line of sight test: the ghost reveals enemies through walls.
	static CNEO_Player *FindAimedEnemy( CNEO_Player *pCarrier, const CWeaponGhost *pGhost )
	{
		Vector vecForward;
		pCarrier->EyeVectors( &vecForward );
		const Vector vecEye = pCarrier->EyePosition();

		CNEO_Player *pBest = nullptr;
		float flBestDot = cosf( DEG2RAD( CALLOUT_MAX_ANGLE_DEG ) );
		for ( int i = 1; i <= gpGlobals->maxClients; ++i )
		{
			CNEO_Player *pEnemy = ToNEOPlayer( UTIL_PlayerByIndex( i ) );
			if ( !pEnemy || pCarrier->InSameTeam( pEnemy ) )
			{
				continue;
			}

			float flDistIgnored;
			if ( !pGhost->BeaconRange( pEnemy, flDistIgnored ) )
			{
				continue;
			}

			Vector vecToEnemy = pEnemy->WorldSpaceCenter() - vecEye;
			vecToEnemy.NormalizeInPlace();

			const float flDot = DotProduct( vecToEnemy, vecForward );
			if ( flDot > flBestDot )
			{
				flBestDot = flDot;
				pBest = pEnemy;
			}
		}

		return pBest;
	}

	// The enemy a bot carrier is fighting and sees now, when it is within the ghost's reveal range
	static CNEO_Player *FindFoughtEnemy( CNEOBot *pBot, const CWeaponGhost *pGhost )
	{
		// In sight now, not seen moments ago: the carrier does not know where that enemy is now
		const CKnownEntity *pThreat = pBot->GetVisionInterface()->GetPrimaryKnownThreat( true );
		if ( !pThreat || !pThreat->IsVisibleInFOVNow() )
		{
			return nullptr;
		}

		CNEO_Player *pEnemy = ToNEOPlayer( pThreat->GetEntity() );
		float flDistIgnored;
		if ( !pEnemy || !pGhost->BeaconRange( pEnemy, flDistIgnored ) )
		{
			return nullptr;
		}

		// and not a beacon through a wall, which counts as seen with sv_neo_ctg_ghost_beacons_when_inactive
		if ( !pBot->GetVisionInterface()->IsLineOfSightClear( pEnemy->WorldSpaceCenter() ) )
		{
			return nullptr;
		}

		return pEnemy;
	}

	// Mark the called-out enemy on the compass of the carrier's human teammates
	static void FireCompassCallout( CNEO_Player *pCarrier, CNEO_Player *pTarget )
	{
		IGameEvent *event = gameeventmanager->CreateEvent( "ghost_enemy_callout" );
		if ( !event )
		{
			return;
		}

		event->SetInt( "userid", pCarrier->GetUserID() );
		event->SetInt( "team", pCarrier->GetTeamNumber() );
		event->SetInt( "targetid", pTarget->entindex() );
		const Vector &vecPos = pTarget->GetAbsOrigin();
		event->SetInt( "targetx", vecPos.x );
		event->SetInt( "targety", vecPos.y );
		event->SetInt( "targetz", vecPos.z );
		gameeventmanager->FireEvent( event );
	}

	// The enemy the carrier calls out now, by the weapon they hold
	static CNEO_Player *FindCalloutTarget( CNEO_Player *pCarrier, const CWeaponGhost *pGhost )
	{
		// With the ghost out and booted, the carrier calls out the beacon at the center of their view
		if ( pCarrier->GetActiveWeapon() == pGhost )
		{
			return pCarrier->GetBeaconingGhost() ? FindAimedEnemy( pCarrier, pGhost ) : nullptr;
		}

		// With another weapon out, a human's aim may rest on a wall with an unrelated beacon behind it,
		// so only a bot carrier calls out then, and only the enemy it is fighting
		CNEOBot *pBot = ToNEOBot( pCarrier );
		return pBot ? FindFoughtEnemy( pBot, pGhost ) : nullptr;
	}

	void Update()
	{
		if ( !NEORules()->IsTeamplay() )
		{
			return;
		}

		CNEO_Player *pCarrier = ToNEOPlayer( UTIL_PlayerByIndex( NEORules()->GetGhosterPlayer() ) );
		UpdateReports( pCarrier );

		if ( !pCarrier || !pCarrier->IsAlive() )
		{
			s_iBootedCarrier = 0;
			return;
		}

		// A carrier knows where enemies are once the ghost has shown beacons during this carry
		UpdateBootedCarrier( pCarrier );
		if ( s_iBootedCarrier != pCarrier->entindex() || gpGlobals->curtime < s_flNextCalloutTime )
		{
			return;
		}

		const CWeaponGhost *pGhost = assert_cast<const CWeaponGhost *>( GetNeoWepWithBits( pCarrier, NEO_WEP_GHOST ) );
		if ( !pGhost )
		{
			return;
		}

		s_flNextCalloutTime = gpGlobals->curtime + sv_neo_bot_ghost_callout_interval.GetFloat();

		CNEO_Player *pTarget = FindCalloutTarget( pCarrier, pGhost );
		if ( !pTarget )
		{
			return;
		}

		// A human carrier speaks for themselves, so only a bot carrier's callouts reach the compass
		if ( pCarrier->IsBot() )
		{
			FireCompassCallout( pCarrier, pTarget );
		}

		for ( int i = 1; i <= gpGlobals->maxClients; ++i )
		{
			CNEOBot *pMate = ToNEOBot( UTIL_PlayerByIndex( i ) );
			if ( !pMate || !pMate->IsAlive() || pMate == pCarrier || !pMate->InSameTeam( pCarrier ) )
			{
				continue;
			}

			// A position snapshot, as a voice callout gives
			pMate->GetVisionInterface()->UpdateKnownEntityPosition( pTarget );
		}
	}

	float GetMinAimHoldTime()
	{
		return sv_neo_bot_ghost_callout_interval.GetFloat() + AIM_TURN_TIME;
	}

	void Reset()
	{
		s_flNextCalloutTime = 0.0f;
		s_iBootedCarrier = 0;

		s_pendingReport.m_iTeam = 0;
		s_flNextReportTime = 0.0f;
		for ( int iTeam = 0; iTeam < TEAM__TOTAL; ++iTeam )
		{
			s_exposedAreas[ iTeam ].RemoveAll();
			s_nCostQueries[ iTeam ] = 0;
			s_nCostHits[ iTeam ] = 0;
		}
	}

	float GetCalledExposureCost( const CNEOBot *pBot, const CNavArea *pArea )
	{
		if ( !pBot || pBot != s_pExposureScopeBot )
		{
			return 0.0f;
		}

		const int iTeam = pBot->GetTeamNumber();
		if ( !IsResearchTeam( iTeam ) || s_exposedAreas[ iTeam ].Count() == 0 )
		{
			return 0.0f;
		}

		++s_nCostQueries[ iTeam ];
		const CUtlHashtable< unsigned int, float > &exposed = s_exposedAreas[ iTeam ];
		const UtlHashHandle_t hArea = exposed.Find( pArea->GetID() );
		if ( hArea == exposed.InvalidHandle() )
		{
			return 0.0f;
		}

		const float flAge = gpGlobals->curtime - exposed[ hArea ];
		if ( flAge >= REPORT_DECAY_TIME )
		{
			return 0.0f;
		}

		++s_nCostHits[ iTeam ];
		return REPORT_EXPOSURE_COST * ( 1.0f - flAge / REPORT_DECAY_TIME );
	}

	CCalledExposureScope::CCalledExposureScope( const CNEOBot *pBot ) :
		m_pPrevBot( s_pExposureScopeBot )
	{
		s_pExposureScopeBot = pBot;
	}

	CCalledExposureScope::~CCalledExposureScope()
	{
		s_pExposureScopeBot = m_pPrevBot;
	}
}
