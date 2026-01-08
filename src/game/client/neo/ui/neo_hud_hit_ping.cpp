#include "neo_hud_hit_ping.h"

#include "c_neo_player.h"
#include "neo_gamerules.h"
#include "neo_hud_game_event.h"
#include "c_playerresource.h"

#include "iclientmode.h"
#include <vgui/ILocalize.h>
#include <vgui/ISurface.h>
#include "engine/IEngineSound.h"
#include "voice_status.h"
#include "hud_chat.h"

#include "ienginevgui.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

DECLARE_NAMED_HUDELEMENT(CNEOHud_HitPing, NHudHitPing);

static ConVar cl_neo_hud_hit_ping_update_freq("cl_neo_hud_hit_ping_update_freq", "0.05", FCVAR_CLIENTDLL, "Update frequency for hit pings HUD element");

CNEOHud_HitPing* hitPingHudElement;

extern ConVar cl_neo_player_pings;

CNEOHud_HitPing::CNEOHud_HitPing(const char* pElementName, vgui::Panel* parent)
	: CHudElement(pElementName), Panel(parent, pElementName)
{
	SetAutoDelete(true);
	m_iHideHudElementNumber = NEO_HUD_ELEMENT_PLAYER_PING;

	if (parent)
	{
		SetParent(parent);
	}
	else
	{
		SetParent(g_pClientMode->GetViewport());
	}

	m_hTexture = vgui::surface()->CreateNewTextureID();
	Assert(m_hTexture > 0);

	SetVisible(true);

	hitPingHudElement = this;
}

void CNEOHud_HitPing::ApplySchemeSettings(vgui::IScheme *pScheme)
{
	BaseClass::ApplySchemeSettings(pScheme);

	m_hFontSmall = pScheme->GetFont("NHudOCRSmallerNoAdditive");
	m_iFontTall = vgui::surface()->GetFontTall(m_hFontSmall);
	m_iTexTall = m_iFontTall;

	vgui::surface()->DrawSetTextureFile(m_hTexture, "vgui/hud/ping/ping", 1, false);
	vgui::surface()->DrawGetTextureSize(m_hTexture, m_iTexWidth, m_iTexHeight);

	m_hDeathPingTexture = vgui::surface()->CreateNewTextureID();
	vgui::surface()->DrawSetTextureFile(m_hDeathPingTexture, "vgui/hud/kill_headshot", 1, false);

	vgui::surface()->GetScreenSize(m_iPosX, m_iPosY);
	SetBounds(0, 0, m_iPosX, m_iPosY);

	SetFgColor(COLOR_TRANSPARENT);
	SetBgColor(COLOR_TRANSPARENT);
}

ConVar *CNEOHud_HitPing::GetUpdateFrequencyConVar() const
{
	return &cl_neo_hud_hit_ping_update_freq;
}

void CNEOHud_HitPing::UpdateStateForNeoHudElementDraw()
{
	auto *localPlayer = C_NEO_Player::GetLocalNEOPlayer();
	if (!localPlayer) return;

	const int playerTeam = localPlayer->GetTeamNumber();
	for (int playerSlot = 0; playerSlot < gpGlobals->maxClients; playerSlot++)
	{
		for (int hitIndex = 0; hitIndex < 10; hitIndex++)
		{
			if (gpGlobals->curtime < m_iEnemyHitPings[playerSlot][hitIndex].deathTime && (playerTeam == TEAM_SPECTATOR || m_iEnemyHitPings[playerSlot][hitIndex].team == playerTeam))
			{
				UpdateDistanceToPlayer(localPlayer, playerSlot, false, hitIndex);
			}
		}

		if (gpGlobals->curtime < m_iDeathPings[playerSlot].deathTime)
		{
			UpdateDistanceToPlayer(localPlayer, playerSlot, true);
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: Register game events
//-----------------------------------------------------------------------------
void CNEOHud_HitPing::Init(void)
{
	ListenForGameEvent("hit_ping");
	ListenForGameEvent("round_start");
	ListenForGameEvent("player_team");
	ListenForGameEvent("player_death");
}

//-----------------------------------------------------------------------------
// Purpose: Player created a ping
//-----------------------------------------------------------------------------
void CNEOHud_HitPing::FireGameEvent(IGameEvent* event)
{
	auto eventName = event->GetName();
	if (!Q_stricmp(eventName, "hit_ping"))
	{
		if (!cl_neo_player_pings.GetBool())
		{
			return;
		}

		const int localTeam = GetLocalPlayerTeam();
		const int attackerTeam = event->GetInt("playerteam"); // This is the team of the attacker (pinger)
		
		// Logic: We only show pings from teammates (or if we are spectator)
		if (localTeam != TEAM_SPECTATOR && attackerTeam != localTeam)
		{
			return;
		}

		// In hit_ping, userid is the attacker.
		const int attackerUserID = event->GetInt("userid");
		const int attackerIndex = engine->GetPlayerForUserID(attackerUserID); // 1-based index
		
		int shotUserID = event->GetInt("shotuserid");
		bool isDeathPing = event->GetBool("deathping");
		
		if (!shotUserID && !isDeathPing) 
		{
			// Should be valid for hit ping
			return; 
		}
		
		// shotUserID is the VICTIM.
		// attackerIndex is the ATTACKER.

		if (GetClientVoiceMgr()->IsPlayerBlocked(attackerIndex))
		{
			return;
		}

		const Vector worldpos = Vector(event->GetInt("pingx"), event->GetInt("pingy"), event->GetInt("pingz"));

		int storageIndex = -1;
		int targetIndex; 
		if (shotUserID != 0) {
			targetIndex = engine->GetPlayerForUserID(shotUserID) - 1;
		} else {
			targetIndex = attackerIndex - 1;
		}

		int damage = event->GetInt("damage", 0);
		SetPos(targetIndex, attackerTeam, worldpos, isDeathPing, shotUserID, damage);
	}
	else if (!Q_stricmp(eventName, "round_start"))
	{
		HideAllPings();
	}
	else if (!Q_stricmp(eventName, "player_team"))
	{
		auto player = UTIL_PlayerByUserId(event->GetInt("userid"));
		if (player && player->IsLocalPlayer())
		{
			HideAllPings();
		}
	}
	else if (!Q_stricmp(eventName, "player_death"))
	{
		const int dead_player_userid = event->GetInt("userid");
		for (int i = 0; i < gpGlobals->maxClients; i++)
		{
			if (m_iEnemyHitPings[i][0].victimUserID == dead_player_userid) // Check 0th index as victim user ID should be same for all
			{
				for (int j = 0; j < 10; j++)
				{
					m_iEnemyHitPings[i][j].deathTime = 0;
				}
			}
		}
	}

}

void CNEOHud_HitPing::LevelShutdown(void)
{
	HideAllPings();
}

extern ConVar cl_neo_player_pings_in_spectate;

enum NeoPlayerPingsInSpectate
{
	NEO_SPECTATE_PINGS_DISABLED	= 0,
	NEO_SPECTATE_PINGS_TARGET,
	NEO_SPECTATE_PINGS_ALL,

	NEO_SPECTATE_PINGS_LAST_VALUE =	NEO_SPECTATE_PINGS_ALL
};

void CNEOHud_HitPing::DrawHitPings(int localPlayerTeam, int spectateTargetTeam, int playerPingsInSpectate)
{
	// Similar to Copy, but we iterate m_iEnemyHitPings
	int x, y, x2, y2;
	vgui::surface()->DrawSetTexture(m_hTexture);

	for (int i = 0; i < gpGlobals->maxClients; i++)
	{
		for (int j = 0; j < 10; j++)
		{
			const hitPing& ping = m_iEnemyHitPings[i][j];

			if (gpGlobals->curtime >= ping.deathTime)
			{
				continue;
			}
			
			Vector offset = Vector(0, 0, 32);
			if (!GetVectorInScreenSpace(ping.worldPos, x, y) || !GetVectorInScreenSpace(ping.worldPos, x2, y2, &offset))
			{
				continue;
			}

			if (localPlayerTeam == TEAM_SPECTATOR)
			{
				if (playerPingsInSpectate == NEO_SPECTATE_PINGS_DISABLED)
				{
					continue;
				}
				// If not disabled, spectators see ALL hit pings regardless of target
			}
			else if (ping.team != localPlayerTeam)
			{
				continue;
			}

			constexpr float PING_NORMAL_OPACITY = 200;
			constexpr float PING_OBSTRUCTED_OPACITY = 80;
			float opacity = ping.noLineOfSight ? PING_OBSTRUCTED_OPACITY : PING_NORMAL_OPACITY;

			constexpr float PING_FADE_START = 0.25f;
			const float timeTillDeath = ping.deathTime - gpGlobals->curtime;
			if (timeTillDeath < PING_FADE_START)
			{
				opacity *= timeTillDeath / PING_FADE_START;
			}

			// Land animation
			constexpr float HIT_PING_LIFETIME = 0.5f;
			constexpr float PING_PLACE_ANIMATION_DURATION = 0.5;
			if (timeTillDeath > HIT_PING_LIFETIME - PING_PLACE_ANIMATION_DURATION)
			{
				constexpr float PING_PLACE_ANIMATION_MAX_OFFSET = 0.25;
				y2 += ((y - y2) * PING_PLACE_ANIMATION_MAX_OFFSET) * sin(M_PI * (timeTillDeath - (HIT_PING_LIFETIME - PING_PLACE_ANIMATION_DURATION)) * (1 / PING_PLACE_ANIMATION_DURATION));
			}

			const int halfTexture = (int)((float)m_iTexTall * 0.5f);
			Color color = COLOR_RED; 
			if (localPlayerTeam == TEAM_SPECTATOR)
			{
				if (ping.team == TEAM_JINRAI)
				{
					color = COLOR_JINRAI;
				}
				else if (ping.team == TEAM_NSF)
				{
					color = COLOR_NSF;
				}
			}
			
			color.SetColor(color.r(), color.g(), color.b(), opacity);
			vgui::surface()->DrawSetColor(color);

			int drawX = x;
			int drawY = y;

			vgui::surface()->DrawTexturedRect(
				drawX - halfTexture,
				drawY - halfTexture,
				drawX + halfTexture,
				drawY + halfTexture);

			vgui::surface()->DrawSetTextFont(m_hFontSmall);
			char m_szMarkerText[4 + 1] = {};
			wchar_t m_wszMarkerTextUnicode[4 + 1] = {};
			V_snprintf(m_szMarkerText, sizeof(m_szMarkerText), "%i", ping.damage);
			g_pVGuiLocalize->ConvertANSIToUnicode(m_szMarkerText, m_wszMarkerTextUnicode, sizeof(m_wszMarkerTextUnicode));
			const int halfTextWidth = 0.5 * GetStringPixelWidth(m_wszMarkerTextUnicode, m_hFontSmall);

			int textDrawX = drawX;
			int textDrawY = drawY;

			color.SetColor(COLOR_BLACK.r(), COLOR_BLACK.g(), COLOR_BLACK.b(), opacity);
			vgui::surface()->DrawSetTextColor(color);
			vgui::surface()->DrawSetTextPos(textDrawX - halfTextWidth, textDrawY + 1 - m_iTexTall - m_iFontTall);
			vgui::surface()->DrawPrintText(m_wszMarkerTextUnicode, 5);

			color.SetColor(COLOR_WHITE.r(), COLOR_WHITE.g(), COLOR_WHITE.b(), opacity);
			vgui::surface()->DrawSetTextColor(color);
			vgui::surface()->DrawSetTextPos(textDrawX - 1 - halfTextWidth, textDrawY - m_iTexTall - m_iFontTall);
			vgui::surface()->DrawPrintText(m_wszMarkerTextUnicode, 5);
		}
	}
}

void CNEOHud_HitPing::DrawDeathPings(int localPlayerTeam, int spectateTargetTeam, int playerPingsInSpectate)
{
    DrawPings(m_iDeathPings, localPlayerTeam, spectateTargetTeam, playerPingsInSpectate, true);
}

void CNEOHud_HitPing::DrawPings(hitPing* pings, int localPlayerTeam, int spectateTargetTeam, int playerPingsInSpectate, bool isDeathPing)
{
	int x, y, x2, y2;
	vgui::surface()->DrawSetTexture(isDeathPing ? m_hDeathPingTexture : m_hTexture);

	for (int i = 0; i < gpGlobals->maxClients; i++)
	{
		Vector offset = Vector(0, 0, 32);
		if (gpGlobals->curtime >= pings[i].deathTime || !GetVectorInScreenSpace(pings[i].worldPos, x, y) || !GetVectorInScreenSpace(pings[i].worldPos, x2, y2, &offset))
		{
			continue;
		}

		if (localPlayerTeam == TEAM_SPECTATOR && playerPingsInSpectate == 1 && pings[i].team != spectateTargetTeam)
		{
			continue;
		}

		constexpr float PING_NORMAL_OPACITY = 200;
		constexpr float PING_OBSTRUCTED_OPACITY = 80;
		float opacity = pings[i].noLineOfSight ? PING_OBSTRUCTED_OPACITY : PING_NORMAL_OPACITY;

		constexpr float PING_FADE_START = 1.f;
		const float timeTillDeath = pings[i].deathTime - gpGlobals->curtime;
		if (timeTillDeath < PING_FADE_START)
		{
			opacity *= timeTillDeath / PING_FADE_START;
		}

		// Land animation
		constexpr float DEATH_PING_LIFETIME = 2.0f;
		
		const float lifetime = DEATH_PING_LIFETIME;

		constexpr float PING_PLACE_ANIMATION_DURATION = 0.5;
		if (timeTillDeath > lifetime - PING_PLACE_ANIMATION_DURATION)
		{
			constexpr float PING_PLACE_ANIMATION_MAX_OFFSET = 0.25;
			y2 += ((y - y2) * PING_PLACE_ANIMATION_MAX_OFFSET) * sin(M_PI * (timeTillDeath - (lifetime - PING_PLACE_ANIMATION_DURATION)) * (1 / PING_PLACE_ANIMATION_DURATION));
		}

		// Draw Ping Shape
		const int halfTexture = (int)((float)m_iTexTall * 0.5f);
		Color color = COLOR_RED;
		
		if (isDeathPing && pings[i].team == localPlayerTeam)
		{
			color = (pings[i].team == TEAM_JINRAI) ? COLOR_JINRAI : COLOR_NSF;
		}

		color.SetColor(color.r(), color.g(), color.b(), opacity);
		vgui::surface()->DrawSetColor(color);

		int drawX = x;
		int drawY = y;

		vgui::surface()->DrawTexturedRect(
			drawX - halfTexture,
			drawY - halfTexture,
			drawX + halfTexture,
			drawY + halfTexture);

		// Draw Text (Damage)
		vgui::surface()->DrawSetTextFont(m_hFontSmall);
		char m_szMarkerText[4 + 1] = {};
		wchar_t m_wszMarkerTextUnicode[4 + 1] = {};
		if (isDeathPing)
		{
			V_snprintf(m_szMarkerText, sizeof(m_szMarkerText), "%i", pings[i].damage);
		}
		
		g_pVGuiLocalize->ConvertANSIToUnicode(m_szMarkerText, m_wszMarkerTextUnicode, sizeof(m_wszMarkerTextUnicode));
		const int halfTextWidth = 0.5 * GetStringPixelWidth(m_wszMarkerTextUnicode, m_hFontSmall);

		int textDrawX = drawX;
		int textDrawY = drawY;

		color.SetColor(COLOR_BLACK.r(), COLOR_BLACK.g(), COLOR_BLACK.b(), opacity);
		vgui::surface()->DrawSetTextColor(color);
		vgui::surface()->DrawSetTextPos(textDrawX - halfTextWidth, textDrawY + 1 - m_iTexTall - m_iFontTall);
		vgui::surface()->DrawPrintText(m_wszMarkerTextUnicode, 5);

		color.SetColor(COLOR_WHITE.r(), COLOR_WHITE.g(), COLOR_WHITE.b(), opacity);
		vgui::surface()->DrawSetTextColor(color);
		vgui::surface()->DrawSetTextPos(textDrawX - 1 - halfTextWidth, textDrawY - m_iTexTall - m_iFontTall);
		vgui::surface()->DrawPrintText(m_wszMarkerTextUnicode, 5);
	}
}

void CNEOHud_HitPing::DrawNeoHudElement()
{
	if (!ShouldDraw() || !NEORules()->IsTeamplay())
	{
		return;
	}
	
	C_BasePlayer *pLocalPlayer = C_BasePlayer::GetLocalPlayer();
	if (!pLocalPlayer)
	{
		return;
	}

	const int localPlayerTeam = pLocalPlayer->GetTeamNumber();
	const int playerPingsInSpectate = cl_neo_player_pings_in_spectate.GetInt();
	if (localPlayerTeam == TEAM_SPECTATOR && playerPingsInSpectate == NEO_SPECTATE_PINGS_DISABLED)
	{
		return;
	}

	int spectateTargetTeam = TEAM_UNASSIGNED;
	if (auto observerTarget = pLocalPlayer->GetObserverTarget())
	{
		spectateTargetTeam = observerTarget->GetTeamNumber();
	};
	
	DrawHitPings(localPlayerTeam, spectateTargetTeam, playerPingsInSpectate);
	DrawDeathPings(localPlayerTeam, spectateTargetTeam, playerPingsInSpectate);
}

void CNEOHud_HitPing::Paint()
{
	BaseClass::Paint();
	PaintNeoElement();
}

int CNEOHud_HitPing::GetStringPixelWidth(wchar_t* pString, vgui::HFont hFont)
{
	int iLength = 0;

	for (wchar_t* wch = pString; *wch != 0; wch++)
	{
		iLength += vgui::surface()->GetCharacterWidth(hFont, *wch);
	}

	return iLength;
}

void CNEOHud_HitPing::HideAllPings()
{
	for (int i = 0; i < gpGlobals->maxClients; i++)
	{
		m_iDeathPings[i].deathTime = 0;
		for (int j = 0; j < 10; j++)
		{
			m_iEnemyHitPings[i][j].deathTime = 0;
		}
	}
}

void CNEOHud_HitPing::UpdateDistanceToPlayer(C_BasePlayer* player, const int playerSlot, bool isDeathPing, int hitIndex)
{
	hitPing* pings = nullptr;
	if (isDeathPing) {
		pings = &m_iDeathPings[playerSlot];
	} else {
		if (hitIndex < 0 || hitIndex >= 10) return;
		pings = &m_iEnemyHitPings[playerSlot][hitIndex];
	}

	pings->distance = METERS_PER_INCH * player->GetAbsOrigin().DistTo(pings->worldPos);

	trace_t tr;
	UTIL_TraceLine(player->EyePosition(), pings->worldPos, MASK_VISIBLE_AND_NPCS, player, COLLISION_GROUP_NONE, &tr);
	pings->noLineOfSight = tr.fraction < 0.999;
}

void CNEOHud_HitPing::SetPos(const int playerSlot, const int playerTeam, const Vector& pos, bool isDeathPing, int shotUserID, int damage) {
	constexpr float HIT_PING_LIFETIME = 0.5f;

	auto localPlayer = C_NEO_Player::GetLocalNEOPlayer();
	if (!localPlayer) { return; }

	hitPing* ping = nullptr;
	int hitIndex = -1;

	if (isDeathPing)
	{
		ping = &m_iDeathPings[playerSlot];
	}
	else
	{
		hitIndex = m_iEnemyHitPingIndex[playerSlot];
		ping = &m_iEnemyHitPings[playerSlot][hitIndex];
		m_iEnemyHitPingIndex[playerSlot] = (m_iEnemyHitPingIndex[playerSlot] + 1) % 10;
	}

	ping->worldPos = pos;
	ping->deathTime = gpGlobals->curtime + (isDeathPing ? 2.0f : HIT_PING_LIFETIME);
	ping->team = playerTeam;
	ping->victimUserID = shotUserID;
	ping->damage = damage;

	UpdateDistanceToPlayer(localPlayer, playerSlot, isDeathPing, hitIndex);
}
