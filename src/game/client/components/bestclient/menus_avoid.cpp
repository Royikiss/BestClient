/* Copyright © 2026 BestProject Team */
#include <engine/client.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/textrender.h>

#include <base/math.h>
#include <base/str.h>

#include <game/client/components/bestclient/avoid.h>
#include <game/client/components/hud_layout.h>
#include <game/client/components/bestclient/ui_theme/style.h>
#include <game/client/components/bestclient/ui_theme/widgets.h>
#include <game/client/components/menus.h>
#include <game/client/gameclient.h>
#include <game/client/ui.h>
#include <game/localization.h>

#include <algorithm>
#include <iterator>

namespace
{
const ColorRGBA AVOID_BOX_BG = ColorRGBA(0.0f, 0.0f, 0.0f, 0.25f);
const ColorRGBA AVOID_BOX_BG_ALT = ColorRGBA(0.10f, 0.13f, 0.20f, 0.45f);
const ColorRGBA AVOID_TEXT_DIM = ColorRGBA(0.62f, 0.66f, 0.74f, 0.95f);
const ColorRGBA AVOID_TEXT_VALUE = ColorRGBA(0.82f, 0.87f, 0.95f, 1.0f);

ColorRGBA AvoidAgentColor(int Agent)
{
	switch(Agent)
	{
	case CAvoid::AGENT_BASIC: return ColorRGBA(0.55f, 0.62f, 0.72f, 1.0f);
	case CAvoid::AGENT_LEGIT: return ColorRGBA(0.35f, 0.85f, 0.55f, 1.0f);
	case CAvoid::AGENT_BLATANT: return ColorRGBA(0.98f, 0.55f, 0.20f, 1.0f);
	case CAvoid::AGENT_FENTBOT: return ColorRGBA(0.72f, 0.48f, 0.98f, 1.0f);
	case CAvoid::AGENT_PILOT: return ColorRGBA(0.35f, 0.72f, 0.98f, 1.0f);
	default: return ColorRGBA(0.6f, 0.6f, 0.6f, 1.0f);
	}
}

ColorRGBA AvoidStateColor(int State)
{
	switch(State)
	{
	case CAvoid::STATE_WATCHING: return ColorRGBA(0.25f, 0.85f, 0.45f, 1.0f);
	case CAvoid::STATE_ASSISTING: return ColorRGBA(0.98f, 0.62f, 0.16f, 1.0f);
	case CAvoid::STATE_NSIF: return ColorRGBA(0.95f, 0.28f, 0.30f, 1.0f);
	case CAvoid::STATE_AFK: return ColorRGBA(0.55f, 0.55f, 0.60f, 1.0f);
	default: return ColorRGBA(0.42f, 0.44f, 0.50f, 1.0f);
	}
}

ColorRGBA AvoidHazardColor(int Flags)
{
	if(Flags & CAvoid::HAZ_DEATH)
		return ColorRGBA(1.00f, 0.36f, 0.38f, 1.0f);
	if(Flags & (CAvoid::HAZ_FREEZE | CAvoid::HAZ_DEEP | CAvoid::HAZ_LIVE))
		return ColorRGBA(0.45f, 0.74f, 1.00f, 1.0f);
	if(Flags & CAvoid::HAZ_UNFREEZE)
		return ColorRGBA(0.35f, 1.00f, 0.85f, 1.0f);
	if(Flags & CAvoid::HAZ_TELE)
		return ColorRGBA(0.80f, 0.45f, 1.00f, 1.0f);
	return AVOID_TEXT_VALUE;
}

void AvoidPanel(CUIRect Box, bool Alt = false)
{
	Box.Draw(Alt ? AVOID_BOX_BG_ALT : AVOID_BOX_BG, IGraphics::CORNER_ALL, 6.0f);
}

void AvoidSectionTitle(CUi *pUi, CUIRect *pRect, const char *pTitle, float Height = 18.0f)
{
	CUIRect Title;
	pRect->HSplitTop(Height, &Title, pRect);
	pUi->DoLabel(&Title, pTitle, 12.0f, TEXTALIGN_ML);
}

void AvoidHint(CUi *pUi, ITextRender *pTextRender, CUIRect Rect, const char *pText, float Size = 10.0f)
{
	pTextRender->TextColor(AVOID_TEXT_DIM);
	pUi->DoLabel(&Rect, pText, Size, TEXTALIGN_ML);
	pTextRender->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
}

void AvoidKeyValue(CUi *pUi, ITextRender *pTextRender, CUIRect *pRect, const char *pKey, const char *pValue, ColorRGBA ValueColor, float RowHeight = 16.0f)
{
	CUIRect Row, KeyRect, ValueRect;
	pRect->HSplitTop(RowHeight, &Row, pRect);
	Row.VSplitLeft(92.0f, &KeyRect, &ValueRect);
	pTextRender->TextColor(AVOID_TEXT_DIM);
	pUi->DoLabel(&KeyRect, pKey, 10.0f, TEXTALIGN_ML);
	pTextRender->TextColor(ValueColor);
	pUi->DoLabel(&ValueRect, pValue, 10.0f, TEXTALIGN_ML);
	pTextRender->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
}

// A greyed out, read-only parameter row, used for the parameters of the agents whose engine is
// not implemented yet. Keeps the page honest: it documents the surface without pretending.
void AvoidPlannedRow(CUi *pUi, ITextRender *pTextRender, CUIRect *pRect, const char *pLabel, float RowHeight = 18.0f)
{
	CUIRect Row, LabelRect, TagRect;
	pRect->HSplitTop(RowHeight, &Row, pRect);
	Row.VSplitRight(58.0f, &LabelRect, &TagRect);
	pTextRender->TextColor(0.48f, 0.50f, 0.56f, 0.95f);
	pUi->DoLabel(&LabelRect, pLabel, 10.0f, TEXTALIGN_ML);
	pTextRender->TextColor(0.42f, 0.44f, 0.50f, 0.95f);
	pUi->DoLabel(&TagRect, BcLocalize("planned"), 9.0f, TEXTALIGN_MR);
	pTextRender->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
}

// ---------------------------------------------------------------------------------------------
// Every agent exposes its own parameter panels. This mirrors the reference client, where the
// options below the agent dropdown change with the selected bot. Panels that describe module
// level settings (which hazard tiles are dangerous) are intentionally shared.
// ---------------------------------------------------------------------------------------------
enum EAvoidPanel
{
	PANEL_BASIC = 0,
	PANEL_ASSIST_LEGIT,
	PANEL_ASSIST_BLATANT,
	PANEL_TUNING_LEGIT,
	PANEL_TUNING_BLATANT,
	PANEL_PRIORITIES,
	PANEL_AIMBOT,
	PANEL_SAFETY,
	PANEL_TILES,
	PANEL_FENT_AVOID,
	PANEL_FENT_CALC,
	PANEL_PILOT_MAIN,
	PANEL_PILOT_SETTINGS,
	PANEL_VISUALS,
};

struct SAvoidPanelDef
{
	int m_Kind;
	const char *m_pName; // English key, resolved through BcLocalize() while drawing
};

const SAvoidPanelDef g_aBasicPanels[] = {
	{PANEL_BASIC, "Basic"},
	{PANEL_TILES, "Tiles"},
};
const SAvoidPanelDef g_aLegitPanels[] = {
	{PANEL_ASSIST_LEGIT, "Assist"},
	{PANEL_TUNING_LEGIT, "Tuning"},
	{PANEL_PRIORITIES, "Priorities"},
	{PANEL_TILES, "Tiles"},
};
const SAvoidPanelDef g_aBlatantPanels[] = {
	{PANEL_ASSIST_BLATANT, "Assist"},
	{PANEL_TUNING_BLATANT, "Tuning"},
	{PANEL_PRIORITIES, "Priorities"},
	{PANEL_AIMBOT, "Aimbot"},
	{PANEL_SAFETY, "Safety"},
	// Blatant needs the tile switches *and* the sensing radius like every other agent; the panel was
	// missing here since stage 1, which left those settings unreachable on this page.
	{PANEL_TILES, "Tiles"},
};
// "Restore defaults" only stores the *script names*, so every value comes from the cvar
// definition in `config_variables_bestclient.h`. There is deliberately no second copy of the
// defaults that could drift away from the real ones.
const char *const g_aBasicDefaultParams[] = {
	// Shown in the Tiles panel.
	"bc_avoid_tile_death",
	"bc_avoid_tile_freeze",
	"bc_avoid_tile_unfreeze",
	"bc_avoid_tile_tele",
	"bc_avoid_unfreeze_ticks",
	"bc_avoid_sensing_radius",
	// Read by Basic although the sliders live in the Legit / Blatant panels.
	"bc_avoid_direction_assist",
	"bc_avoid_check_ticks",
	"bc_avoid_kick_in_ticks",
	"bc_avoid_direction_weight",
	"bc_avoid_life_weight",
	"bc_avoid_afk_protect",
	"bc_avoid_afk_time",
};

const char *const g_aLegitDefaultParams[] = {
	// Assist panel.
	"bc_avoid_direction_assist",
	"bc_avoid_hook_assist",
	"bc_avoid_player_prediction",
	"bc_avoid_afk_protect",
	"bc_avoid_afk_time",
	// Tuning panel.
	"bc_avoid_check_ticks",
	"bc_avoid_quality",
	"bc_avoid_randomness",
	// Priorities panel.
	"bc_avoid_direction_weight",
	"bc_avoid_hook_weight",
	"bc_avoid_life_weight",
	// Read by Legit although the sliders live in the Blatant panels.
	"bc_avoid_kick_in_ticks",
	"bc_avoid_nsif",
	// Tiles panel.
	"bc_avoid_tile_death",
	"bc_avoid_tile_freeze",
	"bc_avoid_tile_unfreeze",
	"bc_avoid_tile_tele",
	"bc_avoid_unfreeze_ticks",
	"bc_avoid_sensing_radius",
};

// Blatant (stage 4) reads the whole assistant / aim / priority / safety surface, so "restore
// defaults" covers all of it - including the parameters whose sliders live in another panel.
const char *const g_aBlatantDefaultParams[] = {
	// Assist panel.
	"bc_avoid_direction_assist",
	"bc_avoid_hook_assist",
	"bc_avoid_track_point",
	"bc_avoid_safe_aim_tracking",
	"bc_avoid_auto_drag",
	"bc_avoid_player_prediction",
	// Tuning panel.
	"bc_avoid_check_ticks",
	"bc_avoid_kick_in_ticks",
	"bc_avoid_quality",
	"bc_avoid_randomness",
	// Priorities panel.
	"bc_avoid_direction_weight",
	"bc_avoid_hook_weight",
	"bc_avoid_life_weight",
	// Aimbot panel.
	"bc_avoid_aimbot",
	"bc_avoid_aimbot_mode",
	"bc_avoid_aimbot_segments",
	"bc_avoid_aimbot_fov",
	// Safety panel.
	"bc_avoid_nsif",
	"bc_avoid_afk_protect",
	"bc_avoid_afk_time",
	// Tiles panel.
	"bc_avoid_tile_death",
	"bc_avoid_tile_freeze",
	"bc_avoid_tile_unfreeze",
	"bc_avoid_tile_tele",
	"bc_avoid_unfreeze_ticks",
	"bc_avoid_sensing_radius",
};

// Fentbot / Pilot get their list when their algorithms land (stage 5/6); until then the button is
// simply not drawn for them, so nothing pretends to reset parameters that do nothing.
const char *const *AvoidDefaultParams(int Agent, int *pCount)
{
	switch(Agent)
	{
	case CAvoid::AGENT_BLATANT:
		*pCount = (int)std::size(g_aBlatantDefaultParams);
		return g_aBlatantDefaultParams;
	case CAvoid::AGENT_LEGIT:
		*pCount = (int)std::size(g_aLegitDefaultParams);
		return g_aLegitDefaultParams;
	case CAvoid::AGENT_BASIC:
		*pCount = (int)std::size(g_aBasicDefaultParams);
		return g_aBasicDefaultParams;
	default:
		*pCount = 0;
		return nullptr;
	}
}

const SAvoidPanelDef g_aFentPanels[] = {
	{PANEL_FENT_AVOID, "Avoid"},
	{PANEL_FENT_CALC, "Calculation"},
	{PANEL_VISUALS, "Visuals"},
	{PANEL_TILES, "Tiles"},
};
const SAvoidPanelDef g_aPilotPanels[] = {
	{PANEL_PILOT_MAIN, "Main"},
	{PANEL_PILOT_SETTINGS, "Settings"},
	{PANEL_VISUALS, "Visuals"},
	{PANEL_TILES, "Tiles"},
};

int AvoidPanelCount(int Agent)
{
	switch(Agent)
	{
	case CAvoid::AGENT_LEGIT: return (int)std::size(g_aLegitPanels);
	case CAvoid::AGENT_BLATANT: return (int)std::size(g_aBlatantPanels);
	case CAvoid::AGENT_FENTBOT: return (int)std::size(g_aFentPanels);
	case CAvoid::AGENT_PILOT: return (int)std::size(g_aPilotPanels);
	case CAvoid::AGENT_BASIC:
	default: return (int)std::size(g_aBasicPanels);
	}
}

const SAvoidPanelDef &AvoidPanelDef(int Agent, int Index)
{
	switch(Agent)
	{
	case CAvoid::AGENT_LEGIT: return g_aLegitPanels[Index];
	case CAvoid::AGENT_BLATANT: return g_aBlatantPanels[Index];
	case CAvoid::AGENT_FENTBOT: return g_aFentPanels[Index];
	case CAvoid::AGENT_PILOT: return g_aPilotPanels[Index];
	case CAvoid::AGENT_BASIC:
	default: return g_aBasicPanels[Index];
	}
}
} // namespace

void CMenus::RenderSettingsAvoid(CUIRect MainView)
{
	CAvoid &Avoid = GameClient()->m_Avoid;
	const CAvoid::STelemetry &T = Avoid.Telemetry();
	const int Agent = Avoid.Agent();
	const bool Enabled = g_Config.m_BcAvoidEnabled != 0;
	const bool Armed = Avoid.IsArmed();

	// Two checkboxes sharing one row. `Id` only has to be unique per row so that the widget
	// containers stay stable across frames.
	auto CheckBoxRow = [&](int Id, CUIRect Row, const char *pLeftText, int *pLeftValue, const char *pRightText, int *pRightValue) {
		static CButtonContainer s_aBoxes[32];
		CUIRect LeftRect, RightRect;
		Row.VSplitMid(&LeftRect, &RightRect, 6.0f);
		if(DoButton_CheckBox(&s_aBoxes[Id * 2], pLeftText, *pLeftValue, &LeftRect))
			*pLeftValue ^= 1;
		if(pRightText && DoButton_CheckBox(&s_aBoxes[Id * 2 + 1], pRightText, *pRightValue, &RightRect))
			*pRightValue ^= 1;
	};

	// =======================================================================================
	// Top status bar: one glance tells you whether protection is on and what it sees.
	// =======================================================================================
	CUIRect StatusBar, LeftColumn, RightColumn;
	MainView.HSplitTop(50.0f, &StatusBar, &MainView);
	MainView.HSplitTop(8.0f, nullptr, &MainView);
	MainView.VSplitMid(&LeftColumn, &RightColumn, 14.0f);

	StatusBar.Draw(ColorRGBA(0.06f, 0.08f, 0.13f, 0.78f), IGraphics::CORNER_ALL, 6.0f);
	{
		CUIRect Inner;
		StatusBar.Margin(8.0f, &Inner);

		CUIRect StateBadge, AgentBadge, Info, Right;
		Inner.VSplitLeft(112.0f, &StateBadge, &Inner);
		Inner.VSplitLeft(6.0f, nullptr, &Inner);
		Inner.VSplitLeft(128.0f, &AgentBadge, &Inner);
		Inner.VSplitLeft(14.0f, nullptr, &Inner);
		Inner.VSplitRight(200.0f, &Info, &Right);

		const char *pStateText = !Enabled ? BcLocalize("DISABLED") : (Armed ? CAvoid::StateName(T.m_State) : BcLocalize("STANDBY"));
		const ColorRGBA StateCol = (Enabled && Armed) ? AvoidStateColor(T.m_State) : ColorRGBA(0.42f, 0.44f, 0.50f, 1.0f);

		StateBadge.Draw(StateCol, IGraphics::CORNER_ALL, 4.0f);
		TextRender()->TextColor(0.05f, 0.05f, 0.05f, 1.0f);
		Ui()->DoLabel(&StateBadge, pStateText, 12.0f, TEXTALIGN_MC);

		AgentBadge.Draw(AvoidAgentColor(Agent), IGraphics::CORNER_ALL, 4.0f);
		char aAgent[64];
		str_format(aAgent, sizeof(aAgent), "AVOID: %s", Avoid.AgentName(Agent));
		Ui()->DoLabel(&AgentBadge, aAgent, 11.0f, TEXTALIGN_MC);
		TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);

		CUIRect Row1, Row2;
		Info.HSplitTop(16.0f, &Row1, &Info);
		Info.HSplitTop(14.0f, &Row2, &Info);

		char aBuf[192];
		if(T.m_ThreatDistanceTiles >= 0.0f)
			str_format(aBuf, sizeof(aBuf), "%s: %s  %.1f %s", BcLocalize("Threat"), CAvoid::HazardName(T.m_ThreatFlags), T.m_ThreatDistanceTiles, BcLocalize("tiles"));
		else
			str_format(aBuf, sizeof(aBuf), "%s: %s", BcLocalize("Threat"), CAvoid::HazardName(T.m_ThreatFlags));
		TextRender()->TextColor(AvoidHazardColor(T.m_ThreatFlags));
		Ui()->DoLabel(&Row1, aBuf, 11.0f, TEXTALIGN_ML);
		TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);

		str_format(aBuf, sizeof(aBuf), "%s: %s  |  %s: %d %s  |  %s %d/%d",
			BcLocalize("Plan"), T.m_aReason[0] ? T.m_aReason : BcLocalize("<none>"),
			BcLocalize("Lookahead"), g_Config.m_BcAvoidCheckTicks, BcLocalize("ticks"),
			BcLocalize("Overrides"), T.m_Overrides, T.m_Decisions);
		TextRender()->TextColor(0.70f, 0.75f, 0.84f, 1.0f);
		Ui()->DoLabel(&Row2, aBuf, 10.0f, TEXTALIGN_ML);
		TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);

		// Right: the two console shortcuts plus the toggle bind hint.
		CUIRect ButtonRow, BindRow;
		Right.HSplitTop(18.0f, &ButtonRow, &Right);
		Right.HSplitTop(12.0f, &BindRow, &Right);

		CUIRect BtnStatus, BtnReset;
		ButtonRow.VSplitMid(&BtnStatus, &BtnReset, 6.0f);
		static CButtonContainer s_StatusBtn;
		if(DoButton_Menu(&s_StatusBtn, BcLocalize("Print status"), 0, &BtnStatus))
			Avoid.PrintStatus();
		static CButtonContainer s_ResetBtn;
		if(DoButton_Menu(&s_ResetBtn, BcLocalize("Reset counters"), 0, &BtnReset))
			Avoid.ResetCounters();

		TextRender()->TextColor(AVOID_TEXT_DIM);
		Ui()->DoLabel(&BindRow, "bind X toggle bc_avoid_active 1 0", 9.0f, TEXTALIGN_MR);
		TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
	}

	// =======================================================================================
	// Left column: switch the mode, watch the state.
	// =======================================================================================
	{
		CUIRect AgentBox, MasterBox, StatusBox;
		LeftColumn.HSplitTop(118.0f, &AgentBox, &LeftColumn);
		LeftColumn.HSplitTop(6.0f, nullptr, &LeftColumn);
		LeftColumn.HSplitTop(122.0f, &MasterBox, &LeftColumn);
		LeftColumn.HSplitTop(6.0f, nullptr, &LeftColumn);
		LeftColumn.HSplitTop(132.0f, &StatusBox, &LeftColumn);

		// --- Assist mode ---------------------------------------------------------------------
		AvoidPanel(AgentBox);
		{
			CUIRect Content;
			AgentBox.Margin(8.0f, &Content);
			AvoidSectionTitle(Ui(), &Content, BcLocalize("Assist mode"));

			CUIRect RowAgent1, RowAgent2, Desc;
			Content.HSplitTop(24.0f, &RowAgent1, &Content);
			Content.HSplitTop(4.0f, nullptr, &Content);
			Content.HSplitTop(24.0f, &RowAgent2, &Content);
			Content.HSplitTop(4.0f, nullptr, &Content);
			Content.HSplitTop(28.0f, &Desc, &Content);

			static CButtonContainer s_aAgentButtons[CAvoid::NUM_AGENTS];

			CUIRect Row = RowAgent1;
			for(int AgentId = CAvoid::AGENT_BASIC; AgentId <= CAvoid::AGENT_BLATANT; ++AgentId)
			{
				CUIRect Button;
				Row.VSplitLeft(Row.w / (float)(CAvoid::AGENT_BLATANT - AgentId + 1), &Button, &Row);
				Button.HMargin(1.0f, &Button);
				if(DoButton_Menu(&s_aAgentButtons[AgentId], Avoid.AgentName(AgentId), AgentId == Agent ? 1 : 0, &Button))
					Avoid.SetAgent(AgentId);
			}
			Row = RowAgent2;
			for(int AgentId = CAvoid::AGENT_FENTBOT; AgentId <= CAvoid::AGENT_PILOT; ++AgentId)
			{
				CUIRect Button;
				Row.VSplitLeft(Row.w / (float)(CAvoid::AGENT_PILOT - AgentId + 1), &Button, &Row);
				Button.HMargin(1.0f, &Button);
				if(DoButton_Menu(&s_aAgentButtons[AgentId], Avoid.AgentName(AgentId), AgentId == Agent ? 1 : 0, &Button))
					Avoid.SetAgent(AgentId);
			}

			const char *pAgentDesc;
			switch(Agent)
			{
			case CAvoid::AGENT_LEGIT:
				pAgentDesc = BcLocalize("Subtle assistance that keeps your movement natural; may still fail on the hardest parts.");
				break;
			case CAvoid::AGENT_BLATANT:
				pAgentDesc = BcLocalize("Safety above all: also steers and uses the hook, meant for extreme Gores maps.");
				break;
			case CAvoid::AGENT_FENTBOT:
				pAgentDesc = BcLocalize("Solves whole parts asynchronously with pathfinding and a genetic search. Planned.");
				break;
			case CAvoid::AGENT_PILOT:
				pAgentDesc = BcLocalize("Navigates the map on its own or follows a target. Planned.");
				break;
			case CAvoid::AGENT_BASIC:
			default:
				pAgentDesc = BcLocalize("Direction keys only, no tuning. The simple default for plain freeze rooms.");
				break;
			}
			AvoidHint(Ui(), TextRender(), Desc, pAgentDesc, 10.0f);
		}

		// --- Master switch, module switches and the pipeline self-test ------------------------
		AvoidPanel(MasterBox, true);
		{
			CUIRect Content;
			MasterBox.Margin(8.0f, &Content);

			CUIRect Button, Hint, Row1, Row2;
			Content.HSplitTop(30.0f, &Button, &Content);
			Content.HSplitTop(4.0f, nullptr, &Content);
			Content.HSplitTop(26.0f, &Hint, &Content);
			Content.HSplitTop(4.0f, nullptr, &Content);
			Content.HSplitTop(20.0f, &Row1, &Content);
			Content.HSplitTop(20.0f, &Row2, &Content);

			static CButtonContainer s_ArmButton;
			if(DoButton_Menu(&s_ArmButton, Armed ? BcLocalize("Disarm Avoid agent") : BcLocalize("Arm Avoid agent"), Armed ? 1 : 0, &Button))
				Avoid.ToggleArmed();

			AvoidHint(Ui(), TextRender(), Hint,
				Armed ? BcLocalize("Armed: the agent inspects every tick and only takes over when your own input would end in a hazard.") : BcLocalize("Disarmed: the sensor keeps running so you can watch threats, but your input is never changed."),
				10.0f);

			CheckBoxRow(0, Row1, BcLocalize("Enable Avoid module"), &g_Config.m_BcAvoidEnabled, BcLocalize("Threat overlay"), &g_Config.m_BcAvoidShowVisuals);

			// The HUD panel is a regular HUD module, so the checkbox mirrors the very same switch
			// the HUD editor shows. Enabling it here also enables it there, and vice versa.
			static CButtonContainer s_CbHud;
			CUIRect HudRow = Row2;
			CUIRect HudBox, SelfTestBox;
			HudRow.VSplitMid(&HudBox, &SelfTestBox, 6.0f);
			const bool HudEnabled = HudLayout::IsEnabled(HudLayout::MODULE_AVOID);
			if(DoButton_CheckBox(&s_CbHud, BcLocalize("Status HUD"), HudEnabled ? 1 : 0, &HudBox))
			{
				HudLayout::SetEnabled(HudLayout::MODULE_AVOID, !HudEnabled);
				g_Config.m_BcAvoidShowHud = !HudEnabled ? 1 : 0;
			}

			static CButtonContainer s_CbSelfTest;
			if(DoButton_CheckBox(&s_CbSelfTest, BcLocalize("Input pipeline self-test"), g_Config.m_BcAvoidDebugOverride, &SelfTestBox))
				g_Config.m_BcAvoidDebugOverride ^= 1;
		}

		// --- Live status ---------------------------------------------------------------------
		AvoidPanel(StatusBox);
		{
			CUIRect Content;
			StatusBox.Margin(8.0f, &Content);
			AvoidSectionTitle(Ui(), &Content, BcLocalize("Live status"));

			char aBuf[128];
			str_format(aBuf, sizeof(aBuf), "%s  (%.1f, %.1f)", CAvoid::HazardName(T.m_ThreatFlags), T.m_PlayerPos.x / 32.0f, T.m_PlayerPos.y / 32.0f);
			AvoidKeyValue(Ui(), TextRender(), &Content, BcLocalize("Tee"), aBuf, AvoidHazardColor(T.m_ThreatFlags));

			if(T.m_ThreatDistanceTiles >= 0.0f)
				str_format(aBuf, sizeof(aBuf), "%.1f %s  (%.1f, %.1f)", T.m_ThreatDistanceTiles, BcLocalize("tiles"), T.m_ThreatPos.x / 32.0f, T.m_ThreatPos.y / 32.0f);
			else
				str_copy(aBuf, BcLocalize("no hazard in range"));
			AvoidKeyValue(Ui(), TextRender(), &Content, BcLocalize("Nearest"), aBuf, AvoidHazardColor(T.m_ThreatFlags));

			str_format(aBuf, sizeof(aBuf), "%d / %d", T.m_HazardTiles, T.m_SensedTiles);
			AvoidKeyValue(Ui(), TextRender(), &Content, BcLocalize("Hazard / sensed"), aBuf, AVOID_TEXT_VALUE);

			str_format(aBuf, sizeof(aBuf), "%d %s  (%.2f %s)", T.m_SafeTicks, BcLocalize("ticks"), T.m_CostMs, BcLocalize("ms"));
			AvoidKeyValue(Ui(), TextRender(), &Content, BcLocalize("Safe ahead"), aBuf, AVOID_TEXT_VALUE);

			str_format(aBuf, sizeof(aBuf), "%d / %d", T.m_Overrides, T.m_Decisions);
			AvoidKeyValue(Ui(), TextRender(), &Content, BcLocalize("Overrides"), aBuf, ColorRGBA(0.95f, 0.75f, 0.35f, 1.0f));

			AvoidHint(Ui(), TextRender(), Content, BcLocalize("The decision engine is not implemented yet - see the delivery document."), 9.0f);
		}
	}

	// =======================================================================================
	// Right column: the parameter panels of the selected agent.
	// =======================================================================================
	{
		static int s_aPanelByAgent[CAvoid::NUM_AGENTS] = {0, 0, 0, 0, 0};
		const int PanelCount = AvoidPanelCount(Agent);
		int &Selected = s_aPanelByAgent[Agent];
		Selected = std::clamp(Selected, 0, PanelCount - 1);

		CUIRect NavBar, Panel;
		RightColumn.HSplitTop(22.0f, &NavBar, &RightColumn);
		RightColumn.HSplitTop(6.0f, nullptr, &RightColumn);
		Panel = RightColumn;

		// A small square button on the right end of the tab bar: put every parameter of the selected
		// agent back to its default. It only appears for the agents that have a parameter list.
		int NumDefaults = 0;
		const char *const *apDefaults = AvoidDefaultParams(Agent, &NumDefaults);
		CUIRect DefaultsRow;
		if(NumDefaults > 0)
		{
			NavBar.VSplitRight(92.0f, &NavBar, &DefaultsRow);
			NavBar.VSplitRight(6.0f, &NavBar, nullptr);
		}

		static CButtonContainer s_aPanelButtons[8];
		for(int i = 0; i < PanelCount; ++i)
		{
			CUIRect Button;
			NavBar.VSplitLeft(NavBar.w / (float)(PanelCount - i), &Button, &NavBar);
			const int Corners = i == 0 ? IGraphics::CORNER_L : (i == PanelCount - 1 ? IGraphics::CORNER_R : IGraphics::CORNER_NONE);
			if(DoButton_MenuTab(&s_aPanelButtons[i], BcLocalize(AvoidPanelDef(Agent, i).m_pName), Selected == i, &Button, Corners))
				Selected = i;
		}

		if(NumDefaults > 0)
		{
			static CButtonContainer s_DefaultsButton;
			static double s_RestoredAt = 0.0;
			const bool JustRestored = s_RestoredAt > 0.0 && Client()->LocalTime() - s_RestoredAt < 2.0;
			if(DoButton_CheckBox_Common(&s_DefaultsButton, JustRestored ? BcLocalize("Restored") : BcLocalize("Defaults"), "", &DefaultsRow, BUTTONFLAG_LEFT))
			{
				if(IConfigManager *pConfigManager = ConfigManager())
				{
					for(int i = 0; i < NumDefaults; i++)
						pConfigManager->Reset(apDefaults[i]);
				}
				s_RestoredAt = Client()->LocalTime();
				GameClient()->Echo(BcLocalize("Avoid: parameters restored to their defaults"));
			}
		}

		AvoidPanel(Panel);
		CUIRect Content;
		Panel.Margin(10.0f, &Content);

		switch(AvoidPanelDef(Agent, Selected).m_Kind)
		{
		// ------------------------------------------------------------------ Basic --------
		case PANEL_BASIC:
		{
			AvoidSectionTitle(Ui(), &Content, BcLocalize("Basic agent"));
			CUIRect Body;
			Content.HSplitTop(52.0f, &Body, &Content);
			AvoidHint(Ui(), TextRender(), Body,
				BcLocalize("Basic takes over the left and right keys only, and it has no options of its own."), 10.0f);
			Content.HSplitTop(10.0f, nullptr, &Content);
			Content.HSplitTop(52.0f, &Body, &Content);
			AvoidHint(Ui(), TextRender(), Body,
				BcLocalize("It is the safe default for plain freeze rooms. Pick Legit for fine tuning or Blatant for maximum safety."), 10.0f);
			break;
		}

		// ------------------------------------------------- Legit assist panel --------
		case PANEL_ASSIST_LEGIT:
		{
			AvoidSectionTitle(Ui(), &Content, BcLocalize("Assistance"));
			CUIRect Row1, Row2, RowAfk;
			Content.HSplitTop(22.0f, &Row1, &Content);
			Content.HSplitTop(8.0f, nullptr, &Content);
			AvoidSectionTitle(Ui(), &Content, BcLocalize("Behaviour"));
			Content.HSplitTop(22.0f, &Row2, &Content);
			Content.HSplitTop(24.0f, &RowAfk, &Content);

			CheckBoxRow(2, Row1, BcLocalize("Direction assistance"), &g_Config.m_BcAvoidDirectionAssist, BcLocalize("Hook assistance"), &g_Config.m_BcAvoidHookAssist);
			CheckBoxRow(3, Row2, BcLocalize("Predict other players"), &g_Config.m_BcAvoidPlayerPrediction, BcLocalize("AFK protection"), &g_Config.m_BcAvoidAfkProtect);
			Ui()->DoScrollbarOption(&g_Config.m_BcAvoidAfkTime, &g_Config.m_BcAvoidAfkTime, &RowAfk, BcLocalize("AFK time"), 5, 600, &CUi::ms_LinearScrollbarScale, 0u, BcLocalize("s"));

			Content.HSplitTop(8.0f, nullptr, &Content);
			AvoidHint(Ui(), TextRender(), Content,
				BcLocalize("Legit only nudges your movement. Turning assistance off keeps that part of your input exactly as you pressed it."), 10.0f);
			break;
		}

		// ----------------------------------------------- Blatant assist panel --------
		case PANEL_ASSIST_BLATANT:
		{
			AvoidSectionTitle(Ui(), &Content, BcLocalize("Assistance"));
			CUIRect Row1, Row2;
			Content.HSplitTop(22.0f, &Row1, &Content);
			Content.HSplitTop(8.0f, nullptr, &Content);
			AvoidSectionTitle(Ui(), &Content, BcLocalize("Aim tracking"));
			Content.HSplitTop(22.0f, &Row2, &Content);
			Content.HSplitTop(8.0f, nullptr, &Content);
			AvoidSectionTitle(Ui(), &Content, BcLocalize("Dragging"));
			CUIRect Row3;
			Content.HSplitTop(22.0f, &Row3, &Content);

			CheckBoxRow(4, Row1, BcLocalize("Direction assistance"), &g_Config.m_BcAvoidDirectionAssist, BcLocalize("Hook assistance"), &g_Config.m_BcAvoidHookAssist);
			CheckBoxRow(5, Row2, BcLocalize("Track point"), &g_Config.m_BcAvoidTrackPoint, BcLocalize("Safe aim tracking"), &g_Config.m_BcAvoidSafeAimTracking);
			CheckBoxRow(6, Row3, BcLocalize("Auto drag"), &g_Config.m_BcAvoidAutoDrag, BcLocalize("Predict other players"), &g_Config.m_BcAvoidPlayerPrediction);

			Content.HSplitTop(8.0f, nullptr, &Content);
			AvoidHint(Ui(), TextRender(), Content,
				BcLocalize("Track point keeps aiming at the last direction that was hookable to solid ground, which is what makes Blatant recover on extreme maps."), 10.0f);
			break;
		}

		// ------------------------------------------------------ Legit tuning --------
		case PANEL_TUNING_LEGIT:
		{
			AvoidSectionTitle(Ui(), &Content, BcLocalize("Lookahead"));
			CUIRect RowCheck;
			Content.HSplitTop(24.0f, &RowCheck, &Content);
			Ui()->DoScrollbarOption(&g_Config.m_BcAvoidCheckTicks, &g_Config.m_BcAvoidCheckTicks, &RowCheck, BcLocalize("Check ticks"), 2, 50, &CUi::ms_LinearScrollbarScale, 0u, BcLocalize("t"));

			Content.HSplitTop(8.0f, nullptr, &Content);
			AvoidSectionTitle(Ui(), &Content, BcLocalize("Search"));
			CUIRect RowQuality, RowRandom;
			Content.HSplitTop(24.0f, &RowQuality, &Content);
			Content.HSplitTop(24.0f, &RowRandom, &Content);
			Ui()->DoScrollbarOption(&g_Config.m_BcAvoidQuality, &g_Config.m_BcAvoidQuality, &RowQuality, BcLocalize("Quality"), 1, 200, &CUi::ms_LinearScrollbarScale, 0u);
			Ui()->DoScrollbarOption(&g_Config.m_BcAvoidRandomness, &g_Config.m_BcAvoidRandomness, &RowRandom, BcLocalize("Randomness"), 0, 200, &CUi::ms_LinearScrollbarScale, 0u);

			Content.HSplitTop(8.0f, nullptr, &Content);
			AvoidHint(Ui(), TextRender(), Content,
				BcLocalize("Check ticks is how far ahead the agent must keep you safe. Quality and Randomness trade CPU time for better plans."), 10.0f);
			Content.HSplitTop(28.0f, nullptr, &Content);
			AvoidHint(Ui(), TextRender(), Content,
				BcLocalize("Lower Quality keeps the frame rate stable when many players are around."), 10.0f);
			break;
		}

		// ---------------------------------------------------- Blatant tuning --------
		case PANEL_TUNING_BLATANT:
		{
			AvoidSectionTitle(Ui(), &Content, BcLocalize("Lookahead"));
			CUIRect RowCheck, RowKick;
			Content.HSplitTop(24.0f, &RowCheck, &Content);
			Content.HSplitTop(24.0f, &RowKick, &Content);
			Ui()->DoScrollbarOption(&g_Config.m_BcAvoidCheckTicks, &g_Config.m_BcAvoidCheckTicks, &RowCheck, BcLocalize("Check ticks"), 2, 50, &CUi::ms_LinearScrollbarScale, 0u, BcLocalize("t"));
			Ui()->DoScrollbarOption(&g_Config.m_BcAvoidKickInTicks, &g_Config.m_BcAvoidKickInTicks, &RowKick, BcLocalize("Kick in ticks"), 0, 50, &CUi::ms_LinearScrollbarScale, 0u, BcLocalize("t"));

			Content.HSplitTop(8.0f, nullptr, &Content);
			AvoidSectionTitle(Ui(), &Content, BcLocalize("Search"));
			CUIRect RowQuality, RowRandom;
			Content.HSplitTop(24.0f, &RowQuality, &Content);
			Content.HSplitTop(24.0f, &RowRandom, &Content);
			Ui()->DoScrollbarOption(&g_Config.m_BcAvoidQuality, &g_Config.m_BcAvoidQuality, &RowQuality, BcLocalize("Quality"), 1, 200, &CUi::ms_LinearScrollbarScale, 0u);
			Ui()->DoScrollbarOption(&g_Config.m_BcAvoidRandomness, &g_Config.m_BcAvoidRandomness, &RowRandom, BcLocalize("Randomness"), 0, 200, &CUi::ms_LinearScrollbarScale, 0u);

			Content.HSplitTop(8.0f, nullptr, &Content);
			AvoidHint(Ui(), TextRender(), Content,
				BcLocalize("Kick in ticks is how late the agent may still stay out of your way. Keep it clearly below the lookahead, for example 20 against 26."), 10.0f);
			break;
		}

		// -------------------------------------------------------- Priorities --------
		case PANEL_PRIORITIES:
		{
			AvoidSectionTitle(Ui(), &Content, BcLocalize("Priority weights"));
			CUIRect RowDir, RowHook, RowLife;
			Content.HSplitTop(24.0f, &RowDir, &Content);
			Content.HSplitTop(24.0f, &RowHook, &Content);
			Content.HSplitTop(24.0f, &RowLife, &Content);
			Ui()->DoScrollbarOption(&g_Config.m_BcAvoidDirectionWeight, &g_Config.m_BcAvoidDirectionWeight, &RowDir, BcLocalize("Direction priority"), 0, 200, &CUi::ms_LinearScrollbarScale, 0u);
			Ui()->DoScrollbarOption(&g_Config.m_BcAvoidHookWeight, &g_Config.m_BcAvoidHookWeight, &RowHook, BcLocalize("Hook priority"), 0, 200, &CUi::ms_LinearScrollbarScale, 0u);
			Ui()->DoScrollbarOption(&g_Config.m_BcAvoidLifeWeight, &g_Config.m_BcAvoidLifeWeight, &RowLife, BcLocalize("Life priority"), 0, 200, &CUi::ms_LinearScrollbarScale, 0u);

			Content.HSplitTop(8.0f, nullptr, &Content);
			AvoidHint(Ui(), TextRender(), Content,
				BcLocalize("Direction and Hook priority decide how much the agent respects what you were trying to do. Life priority decides how much it prefers simply surviving."), 10.0f);
			Content.HSplitTop(28.0f, nullptr, &Content);
			AvoidHint(Ui(), TextRender(), Content,
				BcLocalize("Raise Life priority if the agent feels too timid, lower it if it fights your input too often."), 10.0f);
			break;
		}

		// ------------------------------------------------------------- Aimbot --------
		case PANEL_AIMBOT:
		{
			AvoidSectionTitle(Ui(), &Content, BcLocalize("Internal aim"));

			CUIRect ModeRow;
			Content.HSplitTop(22.0f, &ModeRow, &Content);
			static CButtonContainer s_CbAimbot;
			CUIRect CbRect;
			ModeRow.VSplitLeft(150.0f, &CbRect, &ModeRow);
			if(DoButton_CheckBox(&s_CbAimbot, BcLocalize("Enable aim assist"), g_Config.m_BcAvoidAimbot, &CbRect))
				g_Config.m_BcAvoidAimbot ^= 1;

			static CButtonContainer s_aAimModeButtons[2];
			const char *apAimModes[] = {BcLocalize("Auto aim"), BcLocalize("Aim assist")};
			for(int i = 0; i < 2; ++i)
			{
				CUIRect Button;
				ModeRow.VSplitLeft(ModeRow.w / (float)(2 - i), &Button, &ModeRow);
				Button.HMargin(1.0f, &Button);
				if(DoButton_MenuTab(&s_aAimModeButtons[i], apAimModes[i], g_Config.m_BcAvoidAimbotMode == i, &Button,
					   i == 0 ? IGraphics::CORNER_L : IGraphics::CORNER_R))
					g_Config.m_BcAvoidAimbotMode = i;
			}

			Content.HSplitTop(10.0f, nullptr, &Content);
			CUIRect RowSeg, RowFov;
			Content.HSplitTop(24.0f, &RowSeg, &Content);
			Content.HSplitTop(24.0f, &RowFov, &Content);
			Ui()->DoScrollbarOption(&g_Config.m_BcAvoidAimbotSegments, &g_Config.m_BcAvoidAimbotSegments, &RowSeg, BcLocalize("Scan segments"), 4, 128, &CUi::ms_LinearScrollbarScale, 0u);
			Ui()->DoScrollbarOption(&g_Config.m_BcAvoidAimbotFov, &g_Config.m_BcAvoidAimbotFov, &RowFov, BcLocalize("Field of view"), 10, 180, &CUi::ms_LinearScrollbarScale, 0u, BcLocalize("deg"));

			Content.HSplitTop(8.0f, nullptr, &Content);
			AvoidHint(Ui(), TextRender(), Content,
				BcLocalize("Auto aim scans the whole field of view for the direction that stays safe the longest. Aim assist prefers the safe direction closest to your cursor."), 10.0f);
			break;
		}

		// ------------------------------------------------------------- Safety --------
		case PANEL_SAFETY:
		{
			AvoidSectionTitle(Ui(), &Content, BcLocalize("Recovery"));
			CUIRect RowNsif, RowAfk;
			Content.HSplitTop(22.0f, &RowNsif, &Content);
			Content.HSplitTop(24.0f, &RowAfk, &Content);

			CheckBoxRow(7, RowNsif, BcLocalize("NSIF on no safe input"), &g_Config.m_BcAvoidNsif, BcLocalize("AFK protection"), &g_Config.m_BcAvoidAfkProtect);
			Ui()->DoScrollbarOption(&g_Config.m_BcAvoidAfkTime, &g_Config.m_BcAvoidAfkTime, &RowAfk, BcLocalize("AFK time"), 5, 600, &CUi::ms_LinearScrollbarScale, 0u, BcLocalize("s"));

			Content.HSplitTop(8.0f, nullptr, &Content);
			AvoidHint(Ui(), TextRender(), Content,
				BcLocalize("NSIF kicks in when nothing is fully safe: the agent replays the first step of the safest plan it knows instead of giving up."), 10.0f);
			break;
		}

		// -------------------------------------------------------------- Tiles --------
		case PANEL_TILES:
		{
			AvoidSectionTitle(Ui(), &Content, BcLocalize("Hazard tiles"));
			CUIRect Row1, Row2;
			Content.HSplitTop(22.0f, &Row1, &Content);
			Content.HSplitTop(22.0f, &Row2, &Content);

			// Death and freeze are always dangerous. Unfreeze/teleport are helper tiles and only
			// matter for the agents that actually search around them.
			const bool RichTiles = Agent != CAvoid::AGENT_BASIC;
			CheckBoxRow(8, Row1, BcLocalize("Death tiles"), &g_Config.m_BcAvoidTileDeath, BcLocalize("Freeze tiles"), &g_Config.m_BcAvoidTileFreeze);

			if(RichTiles)
			{
				CheckBoxRow(9, Row2, BcLocalize("Unfreeze tiles"), &g_Config.m_BcAvoidTileUnfreeze, BcLocalize("Teleport tiles"), &g_Config.m_BcAvoidTileTele);

				CUIRect RowUnfreeze;
				Content.HSplitTop(24.0f, &RowUnfreeze, &Content);
				Ui()->DoScrollbarOption(&g_Config.m_BcAvoidUnfreezeTicks, &g_Config.m_BcAvoidUnfreezeTicks, &RowUnfreeze, BcLocalize("Unfreeze lookahead"), 2, 50, &CUi::ms_LinearScrollbarScale, 0u, BcLocalize("t"));
			}
			else if(Agent == CAvoid::AGENT_PILOT)
			{
				CheckBoxRow(9, Row2, BcLocalize("Unfreeze tiles"), &g_Config.m_BcAvoidTileUnfreeze, BcLocalize("Teleport tiles"), &g_Config.m_BcAvoidTileTele);
			}

			Content.HSplitTop(8.0f, nullptr, &Content);
			AvoidSectionTitle(Ui(), &Content, BcLocalize("Sensing"));
			CUIRect RowRadius;
			Content.HSplitTop(24.0f, &RowRadius, &Content);
			// The slider is in half tiles (1 = 0.5 tile, 32 = 16 tiles) so that the low end can be
			// "react almost only when I am already touching it".
			Ui()->DoScrollbarOption(&g_Config.m_BcAvoidSensingRadius, &g_Config.m_BcAvoidSensingRadius, &RowRadius, BcLocalize("Radius (half tiles)"), 1, 32, &CUi::ms_LinearScrollbarScale, 0u);

			Content.HSplitTop(8.0f, nullptr, &Content);
			AvoidHint(Ui(), TextRender(), Content,
				BcLocalize("Black water and freeze are what the agent protects you from. Unfreeze and teleport tiles are optional: turning them on keeps you away from helpers you may actually want."), 10.0f);
			Content.HSplitTop(28.0f, nullptr, &Content);
			AvoidHint(Ui(), TextRender(), Content,
				BcLocalize("The sensing radius is how far ahead the agent may notice a hazard, in half tiles (12 = 6 tiles). Lower it to react later, raise it to react earlier; the scan itself costs almost nothing."), 10.0f);
			break;
		}

		// ------------------------------------------------------- Fentbot avoid --------
		case PANEL_FENT_AVOID:
		{
			AvoidSectionTitle(Ui(), &Content, BcLocalize("Avoid"));
			CUIRect RowPredict;
			Content.HSplitTop(22.0f, &RowPredict, &Content);
			static CButtonContainer s_CbFentPredict;
			if(DoButton_CheckBox(&s_CbFentPredict, BcLocalize("Predict other players"), g_Config.m_BcAvoidPlayerPrediction, &RowPredict))
				g_Config.m_BcAvoidPlayerPrediction ^= 1;

			Content.HSplitTop(8.0f, nullptr, &Content);
			AvoidSectionTitle(Ui(), &Content, BcLocalize("Light tiles"));
			CUIRect RowUnfreeze, RowRadius;
			Content.HSplitTop(22.0f, &RowUnfreeze, &Content);
			CheckBoxRow(10, RowUnfreeze, BcLocalize("Unfreeze tiles"), &g_Config.m_BcAvoidTileUnfreeze, nullptr, nullptr);
			Content.HSplitTop(6.0f, nullptr, &Content);
			Content.HSplitTop(24.0f, &RowRadius, &Content);
			Ui()->DoScrollbarOption(&g_Config.m_BcAvoidUnfreezeTicks, &g_Config.m_BcAvoidUnfreezeTicks, &RowRadius, BcLocalize("Unfreeze lookahead"), 2, 50, &CUi::ms_LinearScrollbarScale, 0u, BcLocalize("t"));

			Content.HSplitTop(8.0f, nullptr, &Content);
			AvoidHint(Ui(), TextRender(), Content,
				BcLocalize("Fentbot may cross freeze tiles when an unfreeze tile is close enough, which is how it solves light freeze segments."), 10.0f);
			break;
		}

		// -------------------------------------------------- Fentbot calculation --------
		case PANEL_FENT_CALC:
		{
			AvoidSectionTitle(Ui(), &Content, BcLocalize("Pathfinding"));
			CUIRect Planned = Content;
			AvoidPlannedRow(Ui(), TextRender(), &Planned, BcLocalize("Quality preset"));
			AvoidPlannedRow(Ui(), TextRender(), &Planned, BcLocalize("Fent ticks"));
			AvoidPlannedRow(Ui(), TextRender(), &Planned, BcLocalize("Tweaker inputs"));
			AvoidPlannedRow(Ui(), TextRender(), &Planned, BcLocalize("Tweaker ticks"));
			AvoidPlannedRow(Ui(), TextRender(), &Planned, BcLocalize("Tweaker dosage"));
			Planned.HSplitTop(6.0f, nullptr, &Planned);
			AvoidHint(Ui(), TextRender(), Planned,
				BcLocalize("Fentbot runs its own asynchronous search, so it needs a calculation panel of its own. Only the menu is reserved for now."), 10.0f);
			break;
		}

		// ------------------------------------------------------- Pilot main --------
		case PANEL_PILOT_MAIN:
		{
			AvoidSectionTitle(Ui(), &Content, BcLocalize("Avoid"));
			CUIRect RowPredict;
			Content.HSplitTop(22.0f, &RowPredict, &Content);
			static CButtonContainer s_CbPilotPredict;
			if(DoButton_CheckBox(&s_CbPilotPredict, BcLocalize("Predict other players"), g_Config.m_BcAvoidPlayerPrediction, &RowPredict))
				g_Config.m_BcAvoidPlayerPrediction ^= 1;

			Content.HSplitTop(8.0f, nullptr, &Content);
			AvoidSectionTitle(Ui(), &Content, BcLocalize("Navigation mode"));
			CUIRect Planned = Content;
			AvoidPlannedRow(Ui(), TextRender(), &Planned, BcLocalize("Autonomous"));
			AvoidPlannedRow(Ui(), TextRender(), &Planned, BcLocalize("Follow cursor"));
			AvoidPlannedRow(Ui(), TextRender(), &Planned, BcLocalize("Follow player"));
			Planned.HSplitTop(6.0f, nullptr, &Planned);
			AvoidHint(Ui(), TextRender(), Planned, BcLocalize("Pilot navigates the map on its own, so its modes are chosen here."), 10.0f);
			break;
		}

		// -------------------------------------------------- Pilot settings --------
		case PANEL_PILOT_SETTINGS:
		{
			AvoidSectionTitle(Ui(), &Content, BcLocalize("Search budget"));
			CUIRect Planned = Content;
			AvoidPlannedRow(Ui(), TextRender(), &Planned, BcLocalize("Population size"));
			AvoidPlannedRow(Ui(), TextRender(), &Planned, BcLocalize("Exploration depth"));
			AvoidPlannedRow(Ui(), TextRender(), &Planned, BcLocalize("Top-K candidates"));
			AvoidPlannedRow(Ui(), TextRender(), &Planned, BcLocalize("Sequence length"));
			Planned.HSplitTop(6.0f, nullptr, &Planned);
			AvoidHint(Ui(), TextRender(), Planned,
				BcLocalize("Pilot searches whole movement sequences instead of single inputs, which is why its budget looks different from Blatant."), 10.0f);
			break;
		}

		// ----------------------------------------------------------- Visuals --------
		case PANEL_VISUALS:
		default:
		{
			AvoidSectionTitle(Ui(), &Content, BcLocalize("Visuals"));
			CUIRect Planned = Content;
			AvoidPlannedRow(Ui(), TextRender(), &Planned, BcLocalize("Render path"));
			AvoidPlannedRow(Ui(), TextRender(), &Planned, BcLocalize("Render pathfinding"));
			if(Agent == CAvoid::AGENT_FENTBOT)
				AvoidPlannedRow(Ui(), TextRender(), &Planned, BcLocalize("Spectate scan"));
			Planned.HSplitTop(6.0f, nullptr, &Planned);
			AvoidHint(Ui(), TextRender(), Planned,
				BcLocalize("Shared switches for these overlays live under Module and on the Tiles panel. The path renderers arrive with the agent itself."), 10.0f);
			break;
		}
		}
	}
}
