#pragma once

#include "neo_hud_childelement.h"
#include "hudelement.h"
#include <vgui_controls/Panel.h>

struct hitPing
{
	Vector worldPos;
	float deathTime;
	float distance;
	int team;
	bool noLineOfSight;
	int victimUserID;
	int damage;
};

class CNEOHud_HitPing : public CNEOHud_ChildElement, public CHudElement, public vgui::Panel
{
	DECLARE_CLASS_SIMPLE(CNEOHud_HitPing, Panel)

public:
	CNEOHud_HitPing(const char *pElementName, vgui::Panel *parent = nullptr);

	virtual void Init(void) override;
	virtual void ApplySchemeSettings(vgui::IScheme *pScheme) override;
	virtual void Paint() override;
	virtual void UpdateStateForNeoHudElementDraw() override;
	virtual void DrawNeoHudElement() override;
	virtual ConVar *GetUpdateFrequencyConVar() const override;
	virtual void LevelShutdown(void) override;

	void HideAllPings();
protected:
	virtual void FireGameEvent(IGameEvent* event) override;

private:
	void DrawHitPings(int localPlayerTeam, int spectateTargetTeam, int playerPingsInSpectate);
	void DrawDeathPings(int localPlayerTeam, int spectateTargetTeam, int playerPingsInSpectate);
	void DrawPings(hitPing* pings, int localPlayerTeam, int spectateTargetTeam, int playerPingsInSpectate, bool isDeathPing);

	int GetStringPixelWidth(wchar_t* pString, vgui::HFont hFont);
	void UpdateDistanceToPlayer(C_BasePlayer* player, const int pingIndex, bool isDeathPing, int hitIndex = -1);
	void SetPos(const int index, const int playerTeam, const Vector& pos, bool isDeathPing, int shotUserID, int damage);

private:
	hitPing m_iEnemyHitPings[MAX_PLAYERS][10] = {}; // [Victim][HistoryIndex]
	int m_iEnemyHitPingIndex[MAX_PLAYERS] = {}; // Circular buffer index for each victim
	hitPing m_iDeathPings[MAX_PLAYERS] = {};

	int m_iPosX, m_iPosY;

	vgui::HFont m_hFontSmall = 0UL;
	int m_iFontTall;

	vgui::HTexture m_hTexture = 0UL; // Hit ping texture
	vgui::HTexture m_hDeathPingTexture = 0UL; // Death ping texture
	int m_iTexWidth, m_iTexHeight;
	int m_iTexTall;
};
