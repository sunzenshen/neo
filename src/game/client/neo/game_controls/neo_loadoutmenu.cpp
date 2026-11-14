#include "cbase.h"
#include "neo_loadoutmenu.h"

#include "vgui_controls/Label.h"
#include "vgui_controls/ImagePanel.h"
#include "vgui_controls/Button.h"
#include "neo/game_controls/neo_button.h"
#include "neo/game_controls/neo_image_button.h"

#include <vgui/ILocalize.h>
#include <vgui/ISurface.h>

#include "ienginevgui.h"

#include "c_neo_player.h"
#include "weapon_neobasecombatweapon.h"
#include "neo_weapon_loadout.h"
#include "neo_gamerules.h"
#include "ui/neo_root.h"
#include "IGameUIFuncs.h" // for key bindings
#include "weapon_parse.h"
#include "neo_weapon_parse.h"
#include <vgui_controls/Tooltip.h>

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

using namespace vgui;

// These are defined in the .res file
#define CONTROL_BUTTON1 "Button1"
#define CONTROL_BUTTON2 "Button2"
#define CONTROL_BUTTON3 "Button3"
#define CONTROL_BUTTON4 "Button4"
#define CONTROL_BUTTON5 "Button5"
#define CONTROL_BUTTON6 "Button6"
#define CONTROL_BUTTON7 "Button7"
#define CONTROL_BUTTON8 "Button8"
#define CONTROL_BUTTON9 "Button9"
#define CONTROL_BUTTON10 "Button10"
#define CONTROL_BUTTON11 "Button11"
#define CONTROL_BUTTON12 "Button12"

#define concat(first, second) first second

static const char *szButtons[] = {
    CONTROL_BUTTON1,
    CONTROL_BUTTON2,
    CONTROL_BUTTON3,
    CONTROL_BUTTON4,
    CONTROL_BUTTON5,
    CONTROL_BUTTON6,
    CONTROL_BUTTON7,
    CONTROL_BUTTON8,
    CONTROL_BUTTON9,
    CONTROL_BUTTON10,
    CONTROL_BUTTON11,
    CONTROL_BUTTON12,
};

// Tooltip field labels, kept in one place for localization
static constexpr const char *TOOLTIP_LABEL_TYPE = "Type";
static constexpr const char *TOOLTIP_LABEL_CLIP = "Clip";
static constexpr const char *TOOLTIP_LABEL_RESERVE = "Reserve";
static constexpr const char *TOOLTIP_LABEL_AMMO = "Ammo";
static constexpr const char *TOOLTIP_LABEL_DAMAGE = "Damage";
static constexpr const char *TOOLTIP_LABEL_FIRE_RATE = "Fire rate";
static constexpr const char *TOOLTIP_UNIT_FIRE_RATE = "rounds/min";
static constexpr const char *TOOLTIP_LABEL_PENETRATION = "Penetration";
static constexpr const char *TOOLTIP_LABEL_REQUIRES = "Requires";

static constexpr const char *WEAPON_TYPE_NAMES[] = {
	"",				// WEP_TYPE_NIL
	"Throwable",
	"Pistol",
	"SMG",
	"Shotgun",
	"Rifle",
	"Machine gun",
	"Sniper rifle",
};
static_assert(ARRAYSIZE(WEAPON_TYPE_NAMES) == WEP_TYPE__TOTAL);

static constexpr float SECONDS_PER_MINUTE = 60.f;

static ENeoWeaponType LoadoutWeaponType(const NEO_WEP_BITS_UNDERLYING_TYPE iWepBit)
{
	for (int i = NEO_WIDX_INVALID + 1; i < NEO_WIDX__TOTAL; ++i)
	{
		if (iWepBit == (static_cast<NEO_WEP_BITS_UNDERLYING_TYPE>(1) << (i - 1)))
		{
			return NEO_WEAPON_TYPE[i];
		}
	}
	return WEP_TYPE_NIL;
}

// The weapon may not have been precached yet in this session, so read its script here
static const CNEOWeaponInfo *LoadoutWeaponScript(const char *szWeaponEntityName)
{
	WEAPON_FILE_INFO_HANDLE hWeaponInfo;
	const unsigned char *pICEKey = g_pGameRules ? g_pGameRules->GetEncryptionKey() : nullptr;
	if (!ReadWeaponDataFromFileForSlot(filesystem, szWeaponEntityName, &hWeaponInfo, pICEKey))
	{
		return nullptr;
	}

	return static_cast<const CNEOWeaponInfo *>(GetFileWeaponInfoFromHandle(hWeaponInfo));
}

static void AppendTooltipLine(char *szText, const int iTextSize, const char *szLabel, const char *szValue)
{
	char szLine[128];
	V_sprintf_safe(szLine, "\n%s: %s", szLabel, szValue);
	V_strncat(szText, szLine, iTextSize);
}

// All of the tooltip text is built here, so localization has one function to change
static void LoadoutWeaponTooltip(const CLoadoutWeapon &weapon, char *szText, const int iTextSize, const bool bLocked)
{
	V_strncpy(szText, weapon.info.m_szWeaponName, iTextSize);
	char szValue[64];

	const ENeoWeaponType eType = LoadoutWeaponType(weapon.info.m_iWepBit);
	if (eType != WEP_TYPE_NIL)
	{
		AppendTooltipLine(szText, iTextSize, TOOLTIP_LABEL_TYPE, WEAPON_TYPE_NAMES[eType]);
	}

	const CNEOWeaponInfo *pScript = LoadoutWeaponScript(weapon.info.m_szWeaponEntityName);
	if (pScript)
	{
		if (pScript->iMaxClip1 > 0)
		{
			V_sprintf_safe(szValue, "%d", pScript->iMaxClip1);
			AppendTooltipLine(szText, iTextSize, TOOLTIP_LABEL_CLIP, szValue);
		}

		if (pScript->iDefaultClip1 >= 0)
		{
			V_sprintf_safe(szValue, "%d", pScript->iDefaultClip1);
			AppendTooltipLine(szText, iTextSize, pScript->iMaxClip1 > 0 ? TOOLTIP_LABEL_RESERVE : TOOLTIP_LABEL_AMMO, szValue);
		}

		if (pScript->m_iPlayerDamage > 0)
		{
			if (pScript->m_iBullets > 1)
			{
				V_sprintf_safe(szValue, "%d x %d", pScript->m_iPlayerDamage, pScript->m_iBullets);
			}
			else
			{
				V_sprintf_safe(szValue, "%d", pScript->m_iPlayerDamage);
			}
			AppendTooltipLine(szText, iTextSize, TOOLTIP_LABEL_DAMAGE, szValue);
		}

		if (pScript->m_flCycleTime > 0.f)
		{
			V_sprintf_safe(szValue, "%d %s", RoundFloatToInt(SECONDS_PER_MINUTE / pScript->m_flCycleTime), TOOLTIP_UNIT_FIRE_RATE);
			AppendTooltipLine(szText, iTextSize, TOOLTIP_LABEL_FIRE_RATE, szValue);
		}

		if (pScript->m_flPenetration > 0.f)
		{
			V_sprintf_safe(szValue, "%g", pScript->m_flPenetration);
			AppendTooltipLine(szText, iTextSize, TOOLTIP_LABEL_PENETRATION, szValue);
		}
	}

	if (bLocked)
	{
		AppendTooltipLine(szText, iTextSize, TOOLTIP_LABEL_REQUIRES, GetRankName(weapon.m_iWeaponPrice));
	}
}

// The built-in vgui text tooltip takes its colors from the engine's default scheme,
// so the menu shows its own label, styled by the client scheme
class CNeoTooltipLabel : public vgui::Label
{
	DECLARE_CLASS_SIMPLE(CNeoTooltipLabel, vgui::Label);

public:
	CNeoTooltipLabel(vgui::Panel *pParent)
		: BaseClass(pParent, "WeaponTooltip", "")
	{
	}

protected:
	void ApplySchemeSettings(vgui::IScheme *pScheme) override
	{
		BaseClass::ApplySchemeSettings(pScheme);
		SetFont(pScheme->GetFont("DefaultSmall", IsProportional()));
		SetFgColor(GetSchemeColor("Tooltip.TextColor", pScheme));
		SetBgColor(GetSchemeColor("Tooltip.BgColor", pScheme));
		SetBorder(pScheme->GetBorder("ToolTipBorder"));
		SetPaintBackgroundEnabled(true);
	}
};

class CNeoWeaponTooltip : public vgui::BaseTooltip
{
public:
	CNeoWeaponTooltip(vgui::Panel *pParent)
		: BaseTooltip(pParent)
		, m_pLabel(new CNeoTooltipLabel(pParent))
	{
		// A popup is not clipped by the menu and does not take focus from it
		m_pLabel->MakePopup(false, true);
		m_pLabel->SetMouseInputEnabled(false);
		m_pLabel->SetKeyBoardInputEnabled(false);
		m_pLabel->SetTextInset(TEXT_INSET, TEXT_INSET);
		m_pLabel->SetVisible(false);
	}

	void SetText(const char *szText) override
	{
		BaseTooltip::SetText(szText);
		m_pLabel->SetText(szText);
	}

	void HideTooltip() override
	{
		m_pLabel->SetVisible(false);
		BaseTooltip::HideTooltip();
	}

	void PerformLayout() override
	{
		if (!ShouldLayout())
		{
			return;
		}

		_isDirty = false;

		// The content size holds the inset on the leading side only
		int iWide, iTall;
		m_pLabel->GetContentSize(iWide, iTall);
		m_pLabel->SetSize(iWide + TEXT_INSET, iTall + TEXT_INSET);
		PositionWindow(m_pLabel);
		m_pLabel->SetVisible(true);
		m_pLabel->MoveToFront();
	}

private:
	static constexpr int TEXT_INSET = 4;

	CNeoTooltipLabel *m_pLabel;
};

static void SetWeaponTooltip(CNeoImageButton *button, BaseTooltip *pTooltip, const CLoadoutWeapon &weapon, const bool bLocked)
{
	if (weapon.m_iWeaponPrice >= XP_EMPTY)
	{
		// Empty slots show no tooltip
		button->SetTooltip(nullptr, nullptr);
		return;
	}

	char szTooltip[512];
	LoadoutWeaponTooltip(weapon, szTooltip, sizeof(szTooltip), bLocked);
	button->SetTooltip(pTooltip, szTooltip);
}

Panel *NeoLoadout_Factory()
{
	return new CNeoLoadoutMenu(gViewPortInterface);
}

DECLARE_BUILD_FACTORY_CUSTOM(CNeoLoadoutMenu, NeoLoadout_Factory);

CNeoLoadoutMenu::CNeoLoadoutMenu(IViewPort *pViewPort)
	: BaseClass(NULL, PANEL_NEO_LOADOUT)
{
	Assert(pViewPort);
	// Quiet "parent not sized yet" spew
	SetSize(10, 10);

	m_pViewPort = pViewPort;

	m_bLoadoutMenu = false;
	m_pWeaponTooltip = new CNeoWeaponTooltip(this);

	LoadControlSettings(GetResFile());

	SetVisible(false);
	SetProportional(false);
	SetMouseInputEnabled(true);
	//SetKeyBoardInputEnabled(true); // Leaving here to highlight menu navigation with keyboard is possible atm
	SetTitleBarVisible(false);

	m_iLoadoutNone = surface()->DrawGetTextureId("vgui/loadout/loadout_none");
	if (m_iLoadoutNone == -1)
	{
		m_iLoadoutNone = surface()->CreateNewTextureID();
		surface()->DrawSetTextureFile(m_iLoadoutNone, "vgui/loadout/loadout_none", true, false);
	}
	FindButtons();
}

CNeoLoadoutMenu::~CNeoLoadoutMenu()
{
	delete m_pWeaponTooltip;
}

void CNeoLoadoutMenu::FindButtons()
{
	m_pButton1 = FindControl<CNeoImageButton>(CONTROL_BUTTON1);
	m_pButton2 = FindControl<CNeoImageButton>(CONTROL_BUTTON2);
	m_pButton3 = FindControl<CNeoImageButton>(CONTROL_BUTTON3);
	m_pButton4 = FindControl<CNeoImageButton>(CONTROL_BUTTON4);
	m_pButton5 = FindControl<CNeoImageButton>(CONTROL_BUTTON5);
	m_pButton6 = FindControl<CNeoImageButton>(CONTROL_BUTTON6);
	m_pButton7 = FindControl<CNeoImageButton>(CONTROL_BUTTON7);
	m_pButton8 = FindControl<CNeoImageButton>(CONTROL_BUTTON8);
	m_pButton9 = FindControl<CNeoImageButton>(CONTROL_BUTTON9);
	m_pButton10 = FindControl<CNeoImageButton>(CONTROL_BUTTON10);
	m_pButton11 = FindControl<CNeoImageButton>(CONTROL_BUTTON11);
	m_pButton12 = FindControl<CNeoImageButton>(CONTROL_BUTTON12);

	for (int i = 0; i < MAX_WEAPON_LOADOUTS; i++)
	{
		auto button = FindControl<CNeoImageButton>(szButtons[i]); // Duplicate FindControl NEO FIXME

		if (!button)
		{
			Assert(false);
			Warning("Button was null on CNeoLoadoutMenu\n");
			continue;
		}

		button->SetMouseInputEnabled(true);
		button->SetButtonTextureID(m_iLoadoutNone);
	}

	returnButton = FindControl<CNeoButton>("ReturnButton");
	returnButton->SetMouseInputEnabled(true);
	if (!returnButton)
	{
		Assert(false);
	}
}

void CNeoLoadoutMenu::CommandCompletion()
{
    for (int i = 0; i < MAX_WEAPON_LOADOUTS; i++)
	{
        auto button = FindControl<Button>(szButtons[i]);

		if (!button)
		{
			Assert(false);
			Warning("Button was null on CNeoLoadoutMenu\n");
			continue;
		}

		button->SetEnabled(false);
	}
	
	returnButton->SetEnabled(false);

	SetVisible(false);
	SetEnabled(false);

	SetMouseInputEnabled(false);
}

void CNeoLoadoutMenu::ShowPanel(bool bShow)
{
	//gViewPortInterface->ShowPanel(PANEL_NEO_LOADOUT, bShow);

	if (bShow && !IsVisible())
	{
		m_bLoadoutMenu = false;
	}

	SetVisible(bShow);

	if (!bShow && m_bLoadoutMenu)
	{
		gViewPortInterface->ShowPanel(PANEL_NEO_LOADOUT, false);
	}
}

void CNeoLoadoutMenu::OnMessage(const KeyValues* params, vgui::VPANEL fromPanel)
{
	BaseClass::OnMessage(params, fromPanel);
}

void CNeoLoadoutMenu::OnThink()
{
	BaseClass::OnThink();
}

void CNeoLoadoutMenu::OnMousePressed(vgui::MouseCode code)
{
	BaseClass::OnMousePressed(code);
}

extern ConCommand loadoutmenu;

extern ConVar sv_neo_dev_loadout;

void CNeoLoadoutMenu::OnClose()
{
	CommandCompletion();
	BaseClass::OnClose();
}

void CNeoLoadoutMenu::OnCommand(const char* command)
{
	BaseClass::OnCommand(command);

	if (command == nullptr || *command == '\0')
	{
		return;
	}

	if (Q_stristr(command, "classmenu"))
	{ // return to class selection
		ChangeMenu("classmenu");
		return;
	}

	if (Q_stristr(command, "loadout "))
	{ // set player loadout
		CUtlStringList loadoutArgs;
		V_SplitString(command, " ", loadoutArgs);

		if (loadoutArgs.Size() != 2) {return;}

		Q_StripPrecedingAndTrailingWhitespace(loadoutArgs[1]);
		const int choiceNum = atoi(loadoutArgs[1]);

		auto localPlayer = C_NEO_Player::GetLocalNEOPlayer();
		if (!localPlayer) { return; }

		int currentXP = CNEOWeaponLoadout::GetEffectiveXP(localPlayer->m_iXP.Get());
		int currentClass = localPlayer->m_iNextSpawnClassChoice.Get() != NEO_CLASS_RANDOM ? localPlayer->m_iNextSpawnClassChoice.Get() : localPlayer->m_iNeoClass.Get();
		int numWeapons = CNEOWeaponLoadout::GetNumberOfLoadoutWeapons(currentXP,
				sv_neo_dev_loadout.GetBool() ? NEO_LOADOUT_DEV : currentClass);
			
		if (choiceNum+1 > numWeapons)
		{
#define INSUFFICIENT_LOADOUT_XP_MSG "Insufficient XP for equipping this loadout!\n"
			Msg(INSUFFICIENT_LOADOUT_XP_MSG);
			engine->Con_NPrintf(0, INSUFFICIENT_LOADOUT_XP_MSG);
			engine->ClientCmd(loadoutmenu.GetName());
		}else
		{
			engine->ClientCmd(command);
		}

	}
	CommandCompletion();
}


void CNeoLoadoutMenu::ChangeMenu(const char* menuName = NULL)
{
	CommandCompletion();
	ShowPanel(false);
	m_bLoadoutMenu = false;
	if (menuName == NULL)
	{
		return;
	}
	if (Q_stricmp(menuName, "classmenu") == 0 && NEORules()->GetForcedClass() < 0)
	{
		engine->ClientCmd(menuName);
	}
}

void CNeoLoadoutMenu::OnKeyCodeReleased(vgui::KeyCode code)
{
	if (code == gameuifuncs->GetButtonCodeForBind("loadoutmenu"))
	{
		ChangeMenu(NULL);
		return;
	}

	switch (code) {
	case KEY_SPACE: // Continue with the currently selected weapon
		ChangeMenu(NULL);
		break;
	default:
		// Ignore other key presses
		break;
	}
}

void CNeoLoadoutMenu::OnButtonPressed(KeyValues *data)
{
}

void CNeoLoadoutMenu::ApplySchemeSettings(vgui::IScheme *pScheme)
{
	BaseClass::ApplySchemeSettings(pScheme);
	LoadControlSettings(GetResFile());
	SetPaintBorderEnabled(false);
	SetPaintBackgroundEnabled(false);

	auto localPlayer = C_NEO_Player::GetLocalNEOPlayer();
	if (!localPlayer) { return; }

	const int currentXP = CNEOWeaponLoadout::GetEffectiveXP(localPlayer->m_iXP.Get());
	const int currentClass = localPlayer->m_iNextSpawnClassChoice.Get() != NEO_CLASS_RANDOM ? localPlayer->m_iNextSpawnClassChoice.Get() : localPlayer->m_iNeoClass.Get();

	int iLoadout = sv_neo_dev_loadout.GetBool() ? NEO_LOADOUT_DEV : currentClass;
	if (IN_BETWEEN_AR(0, iLoadout, NEO_LOADOUT__COUNT) == false)
	{
		iLoadout = NEO_LOADOUT_INVALID;
	}

	const auto &loadout = CNEOWeaponLoadout::s_LoadoutWeapons[iLoadout];
	for (int i = 0; i < MAX_WEAPON_LOADOUTS; ++i)
	{
		const int iWepPrice = loadout[i].m_iWeaponPrice;
		auto button = FindControl<CNeoImageButton>(szButtons[i]);

		SetWeaponTooltip(button, m_pWeaponTooltip, loadout[i], iWepPrice > currentXP);

		if (iWepPrice <= currentXP)
		{
			// Available weapons
			button->SetButtonTexture(loadout[i].info.m_szVguiImage);
			button->SetMouseInputEnabled(true);
			continue;
		}

		button->SetEnabled(false);
		if (iWepPrice < XP_EMPTY)
		{
			// Locked weapons, still hoverable for their tooltip
			button->SetButtonTexture(loadout[i].info.m_szVguiImageNo);
			button->SetMouseInputEnabled(true);
		}
		else
		{
			// Dummy locked weapon slots
			button->SetButtonTextureID(m_iLoadoutNone);
		}
	}

	returnButton = FindControl<CNeoButton>("ReturnButton");
	returnButton->SetMouseInputEnabled(true);
	InvalidateLayout();
}
