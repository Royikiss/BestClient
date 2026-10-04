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

// Height the wrapped hint needs at the given width.
float AvoidHintHeight(ITextRender *pTextRender, const char *pText, float Size, float Width)
{
	if(!pText || !pText[0] || Width <= 1.0f)
		return 0.0f;
	return pTextRender->TextBoundingBox(Size, pText, -1, Width).m_H;
}

void AvoidHint(CUi *pUi, ITextRender *pTextRender, CUIRect Rect, const char *pText, float Size = 10.0f)
{
	// `DoLabel` only breaks lines when it is given the maximum width. Without it the label is
	// shrunk down to its 5 px floor instead, which turns a paragraph into an unreadable smear.
	SLabelProperties Props;
	Props.m_MaxWidth = Rect.w;
	pTextRender->TextColor(AVOID_TEXT_DIM);
	pUi->DoLabel(&Rect, pText, Size, TEXTALIGN_ML, Props);
	pTextRender->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
}

// Trailing hint of a panel: reserves exactly the height its wrapped text needs.
void AvoidHintBottom(CUi *pUi, ITextRender *pTextRender, CUIRect *pRect, const char *pText, float Size = 10.0f)
{
	const float Needed = AvoidHintHeight(pTextRender, pText, Size, pRect->w);
	CUIRect Rect;
	pRect->HSplitTop(std::min(Needed, std::max(0.0f, pRect->h)), &Rect, pRect);
	AvoidHint(pUi, pTextRender, Rect, pText, Size);
}

// A labelled checkbox row. `Id` keeps the button containers unique per row.
void AvoidCheckBoxRow(CMenus *pMenus, int Id, CUIRect Row, const char *pLabel, int *pValue)
{
	static CButtonContainer s_aBoxes[64];
	if(pMenus->DoButton_CheckBox(&s_aBoxes[Id % 64], pLabel, *pValue, &Row))
		*pValue ^= 1;
}

// Two checkboxes sharing one row.
void AvoidCheckBoxPair(CMenus *pMenus, int Id, CUIRect Row, const char *pLeft, int *pLeftValue, const char *pRight, int *pRightValue)
{
	static CButtonContainer s_aBoxes[64];
	CUIRect LeftRect, RightRect;
	Row.VSplitMid(&LeftRect, &RightRect, 6.0f);
	if(pMenus->DoButton_CheckBox(&s_aBoxes[(Id * 2) % 64], pLeft, *pLeftValue, &LeftRect))
		*pLeftValue ^= 1;
	if(pRight && pRightValue && pMenus->DoButton_CheckBox(&s_aBoxes[(Id * 2 + 1) % 64], pRight, *pRightValue, &RightRect))
		*pRightValue ^= 1;
}

// A mode picker drawn as a segmented control.
void AvoidSegmented(CMenus *pMenus, int Id, CUIRect Row, const char *const *apLabels, int Count, int *pValue)
{
	static CButtonContainer s_aButtons[64];
	CUIRect Rest = Row;
	for(int i = 0; i < Count; ++i)
	{
		CUIRect Button;
		Rest.VSplitLeft(Rest.w / (float)(Count - i), &Button, &Rest);
		Button.HMargin(1.0f, &Button);
		const int Corners = i == 0 ? IGraphics::CORNER_L : (i == Count - 1 ? IGraphics::CORNER_R : IGraphics::CORNER_NONE);
		if(pMenus->DoButton_MenuTab(&s_aButtons[(Id * 8 + i) % 64], apLabels[i], *pValue == i, &Button, Corners))
			*pValue = i;
	}
}

// ---------------------------------------------------------------------------------------------
// Panel layout of every agent. Only parameters that the agent really reads are exposed.
// ---------------------------------------------------------------------------------------------
enum EAvoidPanel
{
	PANEL_LEGIT_SETTINGS = 0,
	PANEL_LEGIT_PRIORITY,
	PANEL_LEGIT_TILES,
	PANEL_BLATANT_AVOID,
	PANEL_BLATANT_SETTINGS,
	PANEL_BLATANT_TILES,
	PANEL_BLATANT_AIMBOT,
	PANEL_FENT_CALC,
	PANEL_PILOT_MAIN,
	PANEL_PILOT_SETTINGS,
};

struct SAvoidPanelDef
{
	int m_Kind;
	const char *m_pName; // English key, resolved through BcLocalize() while drawing
};

const SAvoidPanelDef g_aLegitPanels[] = {
	{PANEL_LEGIT_SETTINGS, "Settings"},
	{PANEL_LEGIT_PRIORITY, "Priority"},
	{PANEL_LEGIT_TILES, "Tiles"},
};
const SAvoidPanelDef g_aBlatantPanels[] = {
	{PANEL_BLATANT_AVOID, "Avoid"},
	{PANEL_BLATANT_SETTINGS, "Settings"},
	{PANEL_BLATANT_TILES, "Tiles"},
	{PANEL_BLATANT_AIMBOT, "Aimbot"},
};
const SAvoidPanelDef g_aFentPanels[] = {
	{PANEL_FENT_CALC, "Calculation"},
};
const SAvoidPanelDef g_aPilotPanels[] = {
	{PANEL_PILOT_MAIN, "Main"},
	{PANEL_PILOT_SETTINGS, "Settings"},
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
	default: return 0;
	}
}

const SAvoidPanelDef &AvoidPanelDef(int Agent, int Index)
{
	switch(Agent)
	{
	case CAvoid::AGENT_LEGIT: return g_aLegitPanels[std::clamp(Index, 0, (int)std::size(g_aLegitPanels) - 1)];
	case CAvoid::AGENT_BLATANT: return g_aBlatantPanels[std::clamp(Index, 0, (int)std::size(g_aBlatantPanels) - 1)];
	case CAvoid::AGENT_FENTBOT: return g_aFentPanels[0];
	case CAvoid::AGENT_PILOT: return g_aPilotPanels[std::clamp(Index, 0, (int)std::size(g_aPilotPanels) - 1)];
	case CAvoid::AGENT_BASIC:
	default: return g_aFentPanels[0];
	}
}

// "Restore defaults" only stores the script names, so every value comes from the cvar definition
// in `config_variables_bestclient.h`. There is deliberately no second copy of the defaults that
// could drift away from the real ones.
const char *const g_aGeneralDefaultParams[] = {
	"bc_avoid_enabled",
	"bc_avoid_player_prediction",
	"bc_avoid_afk_protection",
	"bc_avoid_afk_time",
	"bc_avoid_draw_path",
	"bc_avoid_draw_track_point",
	"bc_avoid_draw_aimbot",
};

const char *const g_aBasicDefaultParams[] = {
	"bc_avoid_player_prediction",
};

const char *const g_aLegitDefaultParams[] = {
	"bc_avoid_legit_direction",
	"bc_avoid_legit_hook",
	"bc_avoid_legit_check_ticks",
	"bc_avoid_legit_iterations",
	"bc_avoid_legit_exploration",
	"bc_avoid_legit_direction_weight",
	"bc_avoid_legit_hook_weight",
	"bc_avoid_legit_lifespan_weight",
	"bc_avoid_legit_teles",
	"bc_avoid_legit_death",
	"bc_avoid_legit_unfreeze",
	"bc_avoid_legit_unfreeze_ticks",
};

const char *const g_aBlatantDefaultParams[] = {
	"bc_avoid_nsif",
	"bc_avoid_blatant_direction",
	"bc_avoid_blatant_hook",
	"bc_avoid_blatant_check_ticks",
	"bc_avoid_kick_in_ticks",
	"bc_avoid_track_point",
	"bc_avoid_safe_aim_tracking",
	"bc_avoid_auto_drag",
	"bc_avoid_blatant_teles",
	"bc_avoid_blatant_death",
	"bc_avoid_blatant_unfreeze",
	"bc_avoid_blatant_unfreeze_ticks",
	"bc_avoid_aimbot",
	"bc_avoid_aimbot_segments",
	"bc_avoid_aimbot_fov",
	"bc_avoid_auto_aim",
	"bc_avoid_aim_assist",
};

const char *const g_aFentDefaultParams[] = {
	"bc_avoid_fent_quality",
	"bc_avoid_fent_advanced",
	"bc_avoid_fent_ticks",
	"bc_avoid_fent_tweaker_actions",
	"bc_avoid_fent_tweaker_ticks",
	"bc_avoid_fent_tweaker_dosage",
	"bc_avoid_fent_light_tile",
	"bc_avoid_fent_light_tile_radius",
};

const char *const g_aPilotDefaultParams[] = {
	"bc_avoid_pilot_mode",
	"bc_avoid_pilot_population",
	"bc_avoid_pilot_depth",
	"bc_avoid_pilot_top_k",
	"bc_avoid_pilot_sequence",
};

const char *const *AvoidDefaultParams(int Agent, int *pCount)
{
	switch(Agent)
	{
	case CAvoid::AGENT_LEGIT:
		*pCount = (int)std::size(g_aLegitDefaultParams);
		return g_aLegitDefaultParams;
	case CAvoid::AGENT_BLATANT:
		*pCount = (int)std::size(g_aBlatantDefaultParams);
		return g_aBlatantDefaultParams;
	case CAvoid::AGENT_FENTBOT:
		*pCount = (int)std::size(g_aFentDefaultParams);
		return g_aFentDefaultParams;
	case CAvoid::AGENT_PILOT:
		*pCount = (int)std::size(g_aPilotDefaultParams);
		return g_aPilotDefaultParams;
	case CAvoid::AGENT_BASIC:
	default:
		*pCount = (int)std::size(g_aBasicDefaultParams);
		return g_aBasicDefaultParams;
	}
}
} // namespace

void CMenus::RenderSettingsAvoid(CUIRect MainView)
{
	CAvoid &Avoid = GameClient()->m_Avoid;
	const CAvoid::STelemetry &T = Avoid.Telemetry();
	const int Agent = Avoid.Agent();
	const bool Enabled = Avoid.IsEnabled();

	// =======================================================================================
	// Top status bar: one glance tells whether protection is on and what the agent is doing.
	// =======================================================================================
	CUIRect StatusBar, LeftColumn, RightColumn;
	MainView.HSplitTop(52.0f, &StatusBar, &MainView);
	MainView.HSplitTop(8.0f, nullptr, &MainView);
	MainView.VSplitMid(&LeftColumn, &RightColumn, 14.0f);

	StatusBar.Draw(ColorRGBA(0.06f, 0.08f, 0.13f, 0.78f), IGraphics::CORNER_ALL, 6.0f);
	{
		CUIRect Inner;
		StatusBar.Margin(8.0f, &Inner);

		// The badge widths are clamped against the real width of the bar: VSplitLeft/Right do not
		// clamp, so on a 4:3 window or a raised UI scale the text area used to collapse to zero and
		// the label was drawn over the buttons.
		CUIRect StateBadge, AgentBadge, Info, Right;
		const float BarWidth = Inner.w;
		Inner.VSplitLeft(std::min(104.0f, BarWidth * 0.13f), &StateBadge, &Inner);
		Inner.VSplitLeft(6.0f, nullptr, &Inner);
		Inner.VSplitLeft(std::min(140.0f, BarWidth * 0.18f), &AgentBadge, &Inner);
		Inner.VSplitLeft(10.0f, nullptr, &Inner);
		Inner.VSplitRight(std::min(210.0f, BarWidth * 0.28f), &Info, &Right);

		const char *pStateText = !Enabled ? BcLocalize("OFF") : CAvoid::StateName(T.m_State);
		const ColorRGBA StateCol = Enabled ? AvoidStateColor(T.m_State) : ColorRGBA(0.42f, 0.44f, 0.50f, 1.0f);

		StateBadge.Draw(StateCol, IGraphics::CORNER_ALL, 4.0f);
		TextRender()->TextColor(0.05f, 0.05f, 0.05f, 1.0f);
		Ui()->DoLabel(&StateBadge, pStateText, 12.0f, TEXTALIGN_MC);

		AgentBadge.Draw(AvoidAgentColor(Agent), IGraphics::CORNER_ALL, 4.0f);
		char aAgent[64];
		str_format(aAgent, sizeof(aAgent), "AVOID: %s", CAvoid::AgentName(Agent));
		Ui()->DoLabel(&AgentBadge, aAgent, 11.0f, TEXTALIGN_MC);
		TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);

		CUIRect Row1, Row2;
		Info.HSplitTop(16.0f, &Row1, &Info);
		Info.HSplitTop(14.0f, &Row2, &Info);

		char aBuf[192];
		str_format(aBuf, sizeof(aBuf), "%s: %s", BcLocalize("Plan"), T.m_aReason[0] ? T.m_aReason : BcLocalize("<none>"));
		TextRender()->TextColor(0.95f, 0.85f, 0.45f, 1.0f);
		Ui()->DoLabel(&Row1, aBuf, 11.0f, TEXTALIGN_ML);

		// Every agent reports the lookahead it was asked for when its plan is safe, never the 9999
		// sentinel, so the readout is always a plain tick count.
		str_format(aBuf, sizeof(aBuf), "%s %d  |  %s %.2f %s  |  %s %d/%d",
			BcLocalize("safe"), T.m_SurvivalTicks, BcLocalize("cost"), T.m_CostMs, BcLocalize("ms"),
			BcLocalize("took over"), T.m_Overrides, T.m_Decisions);
		if(Agent == CAvoid::AGENT_FENTBOT || Agent == CAvoid::AGENT_PILOT)
		{
			char aGrid[96];
			str_format(aGrid, sizeof(aGrid), "  |  %s: %s", BcLocalize("grid"),
				T.m_NavigatorReady ? BcLocalize("ready") : BcLocalize("building"));
			str_append(aBuf, aGrid, sizeof(aBuf));
		}
		TextRender()->TextColor(0.70f, 0.75f, 0.84f, 1.0f);
		Ui()->DoLabel(&Row2, aBuf, 10.0f, TEXTALIGN_ML);
		TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);

		// Right: arm switch, the two console shortcuts and the bind hint.
		CUIRect ButtonRow, BindRow;
		Right.HSplitTop(20.0f, &ButtonRow, &Right);
		Right.HSplitTop(12.0f, &BindRow, &Right);

		CUIRect BtnToggle, BtnReset;
		ButtonRow.VSplitMid(&BtnToggle, &BtnReset, 6.0f);
		static CButtonContainer s_ToggleBtn;
		if(DoButton_Menu(&s_ToggleBtn, Enabled ? BcLocalize("Disable") : BcLocalize("Enable"), Enabled ? 1 : 0, &BtnToggle))
			Avoid.SetEnabled(!Enabled);
		static CButtonContainer s_ResetBtn;
		if(DoButton_Menu(&s_ResetBtn, BcLocalize("Reset counters"), 0, &BtnReset))
			Avoid.ResetCounters();

		TextRender()->TextColor(AVOID_TEXT_DIM);
		Ui()->DoLabel(&BindRow, "bind X toggle bc_avoid_enabled 1 0", 9.0f, TEXTALIGN_MR);
		TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
	}

	// =======================================================================================
	// Left column: pick the bot, then the settings that apply to every bot.
	// =======================================================================================
	{
		CUIRect AgentBox, GeneralBox, VisualsBox;
		// The three boxes share the column. Their interactive controls sit at the top of each box
		// and only the explanatory copy trails below, so a short window degrades by clipping text
		// instead of hiding a checkbox. The preferred heights are what the full copy needs; the
		// minimums are what the controls alone need.
		constexpr float BOX_GAP = 6.0f;
		// Preferred heights are the controls plus the wrapped copy at this width, so the hints are
		// readable on a normal window and only shrink once the window really is too short.
		const float AgentCopyHeight = AvoidHintHeight(TextRender(),
			BcLocalize(Agent == CAvoid::AGENT_LEGIT ? "Keeps your movement natural: it weighs staying on course against surviving, and can still fail on the hardest parts." :
				  Agent == CAvoid::AGENT_BLATANT ? "Safety first: also steers and uses the hook, and recovers on extreme Gores maps." :
				  Agent == CAvoid::AGENT_FENTBOT ? "Searches whole map segments with a flow field and a genetic input tweaker. CPU heavy." :
				  Agent == CAvoid::AGENT_PILOT ? "Drives the tee itself: autonomous, towards your cursor, or towards another player." :
								  "Only takes over the left and right keys, six ticks ahead. No options of its own."),
			10.0f, (LeftColumn.w - 16.0f) * 0.5f);
		const float GeneralCopyHeight = AvoidHintHeight(TextRender(),
			BcLocalize("Player prediction makes every bot simulate the other tees as well; AFK protection switches the bot off after the configured idle time."),
			10.0f, LeftColumn.w - 16.0f);
		const float aPreferred[3] = {
			std::max(120.0f, 120.0f + AgentCopyHeight),
			std::max(126.0f, 110.0f + GeneralCopyHeight),
			126.0f};
		const float aMinimum[3] = {106.0f, 106.0f, 106.0f};
		float aHeight[3] = {aPreferred[0], aPreferred[1], aPreferred[2]};
		const float PreferredTotal = aPreferred[0] + aPreferred[1] + aPreferred[2] + 2.0f * BOX_GAP;
		const float Available = LeftColumn.h;
		if(Available < PreferredTotal)
		{
			float MinimumTotal = 2.0f * BOX_GAP;
			for(const float Minimum : aMinimum)
				MinimumTotal += Minimum;
			const float Slack = std::max(0.0f, Available - MinimumTotal);
			const float Shrinkable = PreferredTotal - MinimumTotal;
			const float Factor = Shrinkable > 0.0f ? std::min(1.0f, Slack / Shrinkable) : 0.0f;
			for(int i = 0; i < 3; ++i)
				aHeight[i] = aMinimum[i] + (aPreferred[i] - aMinimum[i]) * Factor;
		}

		LeftColumn.HSplitTop(aHeight[0], &AgentBox, &LeftColumn);
		LeftColumn.HSplitTop(BOX_GAP, nullptr, &LeftColumn);
		LeftColumn.HSplitTop(aHeight[1], &GeneralBox, &LeftColumn);
		LeftColumn.HSplitTop(BOX_GAP, nullptr, &LeftColumn);
		LeftColumn.HSplitTop(std::min(aHeight[2], std::max(0.0f, LeftColumn.h)), &VisualsBox, &LeftColumn);

		// --- Gores bot ----------------------------------------------------------------------
		AvoidPanel(AgentBox);
		{
			CUIRect Content;
			AgentBox.Margin(8.0f, &Content);
			AvoidSectionTitle(Ui(), &Content, BcLocalize("Gores bot"));

			CUIRect RowAgent1, RowAgent2, EnabledRow;
			Content.HSplitTop(24.0f, &RowAgent1, &Content);
			Content.HSplitTop(4.0f, nullptr, &Content);
			Content.HSplitTop(24.0f, &RowAgent2, &Content);
			Content.HSplitTop(6.0f, nullptr, &Content);
			Content.HSplitTop(22.0f, &EnabledRow, &Content);
			Content.HSplitTop(6.0f, nullptr, &Content);

			static CButtonContainer s_aAgentButtons[CAvoid::NUM_AGENTS];

			CUIRect Row = RowAgent1;
			for(int AgentId = CAvoid::AGENT_BASIC; AgentId <= CAvoid::AGENT_BLATANT; ++AgentId)
			{
				CUIRect Button;
				Row.VSplitLeft(Row.w / (float)(CAvoid::AGENT_BLATANT - AgentId + 1), &Button, &Row);
				Button.HMargin(1.0f, &Button);
				if(DoButton_Menu(&s_aAgentButtons[AgentId], CAvoid::AgentName(AgentId), AgentId == Agent ? 1 : 0, &Button))
					Avoid.SetAgent(AgentId);
			}
			Row = RowAgent2;
			for(int AgentId = CAvoid::AGENT_FENTBOT; AgentId <= CAvoid::AGENT_PILOT; ++AgentId)
			{
				CUIRect Button;
				Row.VSplitLeft(Row.w / (float)(CAvoid::AGENT_PILOT - AgentId + 1), &Button, &Row);
				Button.HMargin(1.0f, &Button);
				if(DoButton_Menu(&s_aAgentButtons[AgentId], CAvoid::AgentName(AgentId), AgentId == Agent ? 1 : 0, &Button))
					Avoid.SetAgent(AgentId);
			}

			static CButtonContainer s_CbEnabled;
			if(DoButton_CheckBox(&s_CbEnabled, BcLocalize("Enable Gores bot"), Enabled ? 1 : 0, &EnabledRow))
				Avoid.SetEnabled(!Enabled);

			const char *pAgentDesc;
			switch(Agent)
			{
			case CAvoid::AGENT_LEGIT:
				pAgentDesc = BcLocalize("Keeps your movement natural: it weighs staying on course against surviving, and can still fail on the hardest parts.");
				break;
			case CAvoid::AGENT_BLATANT:
				pAgentDesc = BcLocalize("Safety first: also steers and uses the hook, and recovers on extreme Gores maps.");
				break;
			case CAvoid::AGENT_FENTBOT:
				pAgentDesc = BcLocalize("Searches whole map segments with a flow field and a genetic input tweaker. CPU heavy.");
				break;
			case CAvoid::AGENT_PILOT:
				pAgentDesc = BcLocalize("Drives the tee itself: autonomous, towards your cursor, or towards another player.");
				break;
			case CAvoid::AGENT_BASIC:
			default:
				pAgentDesc = BcLocalize("Only takes over the left and right keys, six ticks ahead. No options of its own.");
				break;
			}
			AvoidHintBottom(Ui(), TextRender(), &Content, pAgentDesc, 10.0f);
		}

		// --- Global settings ----------------------------------------------------------------
		AvoidPanel(GeneralBox, true);
		{
			CUIRect Content;
			GeneralBox.Margin(8.0f, &Content);
			AvoidSectionTitle(Ui(), &Content, BcLocalize("All bots"));

			CUIRect RowPredict, RowAfk, RowTime;
			Content.HSplitTop(22.0f, &RowPredict, &Content);
			Content.HSplitTop(22.0f, &RowAfk, &Content);
			Content.HSplitTop(24.0f, &RowTime, &Content);

			AvoidCheckBoxRow(this, 0, RowPredict, BcLocalize("Player prediction"), &g_Config.m_BcAvoidPlayerPrediction);
			AvoidCheckBoxRow(this, 1, RowAfk, BcLocalize("AFK protection"), &g_Config.m_BcAvoidAfkProtection);
			Ui()->DoScrollbarOption(&g_Config.m_BcAvoidAfkTime, &g_Config.m_BcAvoidAfkTime, &RowTime, BcLocalize("AFK time"), 5, 300, &CUi::ms_LinearScrollbarScale, 0u, BcLocalize("s"));

			Content.HSplitTop(8.0f, nullptr, &Content);
			AvoidHintBottom(Ui(), TextRender(), &Content,
				BcLocalize("Player prediction makes every bot simulate the other tees as well; AFK protection switches the bot off after the configured idle time."), 10.0f);
		}

		// --- Visuals ------------------------------------------------------------------------
		AvoidPanel(VisualsBox);
		{
			CUIRect Content;
			VisualsBox.Margin(8.0f, &Content);
			AvoidSectionTitle(Ui(), &Content, BcLocalize("Visuals"));

			CUIRect RowHud, RowPath, RowTrack, RowAim;
			Content.HSplitTop(22.0f, &RowHud, &Content);
			Content.HSplitTop(22.0f, &RowPath, &Content);
			Content.HSplitTop(22.0f, &RowTrack, &Content);
			Content.HSplitTop(22.0f, &RowAim, &Content);

			// The status panel is a regular HUD module. This checkbox and the HUD editor write the
			// very same switch, so there is no second copy of that state anywhere.
			static CButtonContainer s_CbHud;
			const bool HudEnabled = HudLayout::IsEnabled(HudLayout::MODULE_AVOID);
			if(DoButton_CheckBox(&s_CbHud, BcLocalize("Status HUD"), HudEnabled ? 1 : 0, &RowHud))
				HudLayout::SetEnabled(HudLayout::MODULE_AVOID, !HudEnabled);

			AvoidCheckBoxRow(this, 2, RowPath, BcLocalize("Render path"), &g_Config.m_BcAvoidDrawPath);
			AvoidCheckBoxRow(this, 3, RowTrack, BcLocalize("Render track point"), &g_Config.m_BcAvoidDrawTrackPoint);
			AvoidCheckBoxRow(this, 4, RowAim, BcLocalize("Render aimbot target"), &g_Config.m_BcAvoidDrawAimbot);
		}
	}

	// =======================================================================================
	// Right column: the parameters of the selected bot.
	// =======================================================================================
	{
		static int s_aPanelByAgent[CAvoid::NUM_AGENTS] = {0, 0, 0, 0, 0};
		const int PanelCount = AvoidPanelCount(Agent);
		int &Selected = s_aPanelByAgent[Agent];
		Selected = PanelCount > 0 ? std::clamp(Selected, 0, PanelCount - 1) : 0;

		CUIRect Panel = RightColumn;
		{
			// Header row: the tab bar for agents with several panels, the panel title for agents
			// with one, and the restore button for every agent (Basic included, whose only
			// parameter is the shared player prediction switch).
			CUIRect NavBar, DefaultsRow;
			Panel.HSplitTop(22.0f, &NavBar, &Panel);
			Panel.HSplitTop(6.0f, nullptr, &Panel);

			int NumDefaults = 0;
			const char *const *apDefaults = AvoidDefaultParams(Agent, &NumDefaults);
			NavBar.VSplitRight(92.0f, &NavBar, &DefaultsRow);
			NavBar.VSplitRight(6.0f, &NavBar, nullptr);

			if(PanelCount > 1)
			{
				static CButtonContainer s_aPanelButtons[8];
				for(int i = 0; i < PanelCount; ++i)
				{
					CUIRect Button;
					NavBar.VSplitLeft(NavBar.w / (float)(PanelCount - i), &Button, &NavBar);
					const int Corners = i == 0 ? IGraphics::CORNER_L : (i == PanelCount - 1 ? IGraphics::CORNER_R : IGraphics::CORNER_NONE);
					if(DoButton_MenuTab(&s_aPanelButtons[i], BcLocalize(AvoidPanelDef(Agent, i).m_pName), Selected == i, &Button, Corners))
						Selected = i;
				}
			}
			else
			{
				CUIRect Title;
				NavBar.HSplitTop(22.0f, &Title, &NavBar);
				Ui()->DoLabel(&Title, PanelCount == 1 ? BcLocalize(AvoidPanelDef(Agent, 0).m_pName) : CAvoid::AgentName(Agent), 12.0f, TEXTALIGN_ML);
			}

			static CButtonContainer s_DefaultsButton;
			static double s_RestoredAt = 0.0;
			const bool JustRestored = s_RestoredAt > 0.0 && Client()->LocalTime() - s_RestoredAt < 2.0;
			if(DoButton_CheckBox_Common(&s_DefaultsButton, JustRestored ? BcLocalize("Restored") : BcLocalize("Defaults"), "", &DefaultsRow, BUTTONFLAG_LEFT))
			{
				if(IConfigManager *pConfigManager = ConfigManager())
				{
					for(int i = 0; i < NumDefaults; i++)
						pConfigManager->Reset(apDefaults[i]);
					for(const char *pName : g_aGeneralDefaultParams)
						pConfigManager->Reset(pName);
				}
				s_RestoredAt = Client()->LocalTime();
				GameClient()->Echo(BcLocalize("Avoid: parameters restored to their defaults"));
			}
		}

		AvoidPanel(Panel);
		CUIRect Content;
		Panel.Margin(10.0f, &Content);

		const int PanelKind = PanelCount > 0 ? AvoidPanelDef(Agent, Selected).m_Kind : -1;
		switch(PanelKind)
		{
		// ------------------------------------------------------------------ Basic --------
		case -1:
		default:
		{
			AvoidSectionTitle(Ui(), &Content, BcLocalize("Basic"));
			AvoidHintBottom(Ui(), TextRender(), &Content,
				BcLocalize("Basic only takes over the left and right keys. It always looks six ticks ahead, drops the direction that runs into a freeze, a death tile or another tee, and leaves jump, hook and crosshair exactly as you pressed them."), 10.0f);
			Content.HSplitTop(12.0f, nullptr, &Content);
			AvoidHintBottom(Ui(), TextRender(), &Content,
				BcLocalize("It is the cheapest bot and the safe default for plain freeze rooms. Pick Legit for fine tuning, Blatant for maximum safety, Fentbot for long map segments or Pilot to let the tee drive itself."), 10.0f);
			break;
		}

		// ------------------------------------------------------ Legit settings --------
		case PANEL_LEGIT_SETTINGS:
		{
			AvoidSectionTitle(Ui(), &Content, BcLocalize("Assistance"));
			CUIRect Row1, RowCheck;
			Content.HSplitTop(22.0f, &Row1, &Content);
			AvoidCheckBoxPair(this, 10, Row1, BcLocalize("Direction assistance"), &g_Config.m_BcAvoidLegitDirection, BcLocalize("Hook assistance"), &g_Config.m_BcAvoidLegitHook);

			Content.HSplitTop(10.0f, nullptr, &Content);
			AvoidSectionTitle(Ui(), &Content, BcLocalize("Lookahead"));
			Content.HSplitTop(24.0f, &RowCheck, &Content);
			Ui()->DoScrollbarOption(&g_Config.m_BcAvoidLegitCheckTicks, &g_Config.m_BcAvoidLegitCheckTicks, &RowCheck, BcLocalize("Check ticks"), 1, 50, &CUi::ms_LinearScrollbarScale, 0u, BcLocalize("t"));

			Content.HSplitTop(8.0f, nullptr, &Content);
			AvoidHintBottom(Ui(), TextRender(), &Content,
				BcLocalize("Check ticks is how far ahead every simulated move has to stay safe. Turning an assistance off keeps that part of your input exactly as you pressed it."), 10.0f);
			break;
		}

		// ------------------------------------------------------ Legit priority --------
		case PANEL_LEGIT_PRIORITY:
		{
			AvoidSectionTitle(Ui(), &Content, BcLocalize("Search"));
			CUIRect RowQuality, RowRandom;
			Content.HSplitTop(24.0f, &RowQuality, &Content);
			Content.HSplitTop(24.0f, &RowRandom, &Content);
			Ui()->DoScrollbarOption(&g_Config.m_BcAvoidLegitIterations, &g_Config.m_BcAvoidLegitIterations, &RowQuality, BcLocalize("Quality"), 1, 1000, &CUi::ms_LinearScrollbarScale, 0u);
			Ui()->DoScrollbarOption(&g_Config.m_BcAvoidLegitExploration, &g_Config.m_BcAvoidLegitExploration, &RowRandom, BcLocalize("Randomness"), 1, 1000, &CUi::ms_LinearScrollbarScale, 0u);

			Content.HSplitTop(10.0f, nullptr, &Content);
			AvoidSectionTitle(Ui(), &Content, BcLocalize("Priority"));
			CUIRect RowDir, RowHook, RowLife;
			Content.HSplitTop(24.0f, &RowDir, &Content);
			Content.HSplitTop(24.0f, &RowHook, &Content);
			Content.HSplitTop(24.0f, &RowLife, &Content);
			Ui()->DoScrollbarOption(&g_Config.m_BcAvoidLegitDirectionWeight, &g_Config.m_BcAvoidLegitDirectionWeight, &RowDir, BcLocalize("Direction priority"), 1, 1000, &CUi::ms_LinearScrollbarScale, 0u);
			Ui()->DoScrollbarOption(&g_Config.m_BcAvoidLegitHookWeight, &g_Config.m_BcAvoidLegitHookWeight, &RowHook, BcLocalize("Hook priority"), 1, 1000, &CUi::ms_LinearScrollbarScale, 0u);
			Ui()->DoScrollbarOption(&g_Config.m_BcAvoidLegitLifespanWeight, &g_Config.m_BcAvoidLegitLifespanWeight, &RowLife, BcLocalize("Life priority"), 1, 1000, &CUi::ms_LinearScrollbarScale, 0u);

			Content.HSplitTop(8.0f, nullptr, &Content);
			AvoidHintBottom(Ui(), TextRender(), &Content,
				BcLocalize("Quality is the number of simulated moves per decision and Randomness is how much the search explores. The three priorities decide how strongly the bot sticks to your direction, your hook state and to simply surviving."), 10.0f);
			Content.HSplitTop(24.0f, nullptr, &Content);
			AvoidHintBottom(Ui(), TextRender(), &Content,
				BcLocalize("The search stops after 8 ms per tick to keep the frame rate stable, so very high Quality values stop paying off on maps with many entities."), 10.0f);
			break;
		}

		// --------------------------------------------------------- Legit tiles --------
		case PANEL_LEGIT_TILES:
		{
			AvoidSectionTitle(Ui(), &Content, BcLocalize("Hazard tiles"));
			CUIRect Row1, Row2, RowTicks;
			Content.HSplitTop(22.0f, &Row1, &Content);
			Content.HSplitTop(22.0f, &Row2, &Content);
			Content.HSplitTop(24.0f, &RowTicks, &Content);
			AvoidCheckBoxPair(this, 11, Row1, BcLocalize("Death tiles"), &g_Config.m_BcAvoidLegitDeath, BcLocalize("Teleport tiles"), &g_Config.m_BcAvoidLegitTeles);
			AvoidCheckBoxRow(this, 12, Row2, BcLocalize("Unfreeze tiles"), &g_Config.m_BcAvoidLegitUnfreeze);
			Ui()->DoScrollbarOption(&g_Config.m_BcAvoidLegitUnfreezeTicks, &g_Config.m_BcAvoidLegitUnfreezeTicks, &RowTicks, BcLocalize("Unfreeze ticks"), 1, 30, &CUi::ms_LinearScrollbarScale, 0u, BcLocalize("t"));

			Content.HSplitTop(8.0f, nullptr, &Content);
			AvoidHintBottom(Ui(), TextRender(), &Content,
				BcLocalize("Freeze tiles are always avoided. Death, teleport and unfreeze tiles are optional: switching them on also keeps you away from helpers you may actually want."), 10.0f);
			break;
		}

		// ------------------------------------------------------- Blatant avoid --------
		case PANEL_BLATANT_AVOID:
		{
			AvoidSectionTitle(Ui(), &Content, BcLocalize("Recovery"));
			CUIRect RowNsif;
			Content.HSplitTop(22.0f, &RowNsif, &Content);
			AvoidCheckBoxRow(this, 13, RowNsif, BcLocalize("NSIF (no safe input found)"), &g_Config.m_BcAvoidNsif);

			Content.HSplitTop(8.0f, nullptr, &Content);
			AvoidHintBottom(Ui(), TextRender(), &Content,
				BcLocalize("NSIF kicks in when no input survives the whole lookahead: instead of giving up, the bot replays the first step of the last input sequence that was fully safe."), 10.0f);
			break;
		}

		// ---------------------------------------------------- Blatant settings --------
		case PANEL_BLATANT_SETTINGS:
		{
			AvoidSectionTitle(Ui(), &Content, BcLocalize("Assistance"));
			CUIRect Row1, Row2, RowCheck, RowKick;
			Content.HSplitTop(22.0f, &Row1, &Content);
			Content.HSplitTop(6.0f, nullptr, &Content);
			AvoidCheckBoxPair(this, 14, Row1, BcLocalize("Direction assistance"), &g_Config.m_BcAvoidBlatantDirection, BcLocalize("Hook assistance"), &g_Config.m_BcAvoidBlatantHook);

			AvoidSectionTitle(Ui(), &Content, BcLocalize("Lookahead"));
			Content.HSplitTop(24.0f, &RowCheck, &Content);
			Content.HSplitTop(24.0f, &RowKick, &Content);
			Ui()->DoScrollbarOption(&g_Config.m_BcAvoidBlatantCheckTicks, &g_Config.m_BcAvoidBlatantCheckTicks, &RowCheck, BcLocalize("Check ticks"), 1, 50, &CUi::ms_LinearScrollbarScale, 0u, BcLocalize("t"));
			Ui()->DoScrollbarOption(&g_Config.m_BcAvoidKickInTicks, &g_Config.m_BcAvoidKickInTicks, &RowKick, BcLocalize("Kick in ticks"), 1, 50, &CUi::ms_LinearScrollbarScale, 0u, BcLocalize("t"));

			Content.HSplitTop(6.0f, nullptr, &Content);
			AvoidSectionTitle(Ui(), &Content, BcLocalize("Aim tracking"));
			Content.HSplitTop(22.0f, &Row2, &Content);
			AvoidCheckBoxPair(this, 15, Row2, BcLocalize("Track point"), &g_Config.m_BcAvoidTrackPoint, BcLocalize("Safe aim tracking"), &g_Config.m_BcAvoidSafeAimTracking);

			CUIRect Row3;
			Content.HSplitTop(22.0f, &Row3, &Content);
			AvoidCheckBoxRow(this, 16, Row3, BcLocalize("Auto drag"), &g_Config.m_BcAvoidAutoDrag);

			Content.HSplitTop(6.0f, nullptr, &Content);
			AvoidHintBottom(Ui(), TextRender(), &Content,
				BcLocalize("Check ticks is how long a simulated move must survive, kick in ticks is how long your own input must survive before the bot stays out of the way."), 10.0f);
			Content.HSplitTop(10.0f, nullptr, &Content);
			const char *pAimHint = g_Config.m_BcAvoidAimbot ?
						       BcLocalize("Track point keeps aiming where you last could hook onto a tile, and safe aim tracking only holds that aim while it stays safe.") :
						       BcLocalize("Track point, safe aim tracking and auto drag need the internal aimbot on the Aimbot tab; without it they change nothing.");
			CUIRect AimHint;
			Content.HSplitTop(std::min(AvoidHintHeight(TextRender(), pAimHint, 10.0f, Content.w), std::max(0.0f, Content.h)), &AimHint, &Content);
			SLabelProperties AimProps;
			AimProps.m_MaxWidth = AimHint.w;
			TextRender()->TextColor(g_Config.m_BcAvoidAimbot ? AVOID_TEXT_DIM : ColorRGBA(0.95f, 0.72f, 0.30f, 1.0f));
			Ui()->DoLabel(&AimHint, pAimHint, 10.0f, TEXTALIGN_ML, AimProps);
			TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
			break;
		}

		// ------------------------------------------------------- Blatant tiles --------
		case PANEL_BLATANT_TILES:
		{
			AvoidSectionTitle(Ui(), &Content, BcLocalize("Hazard tiles"));
			CUIRect Row1, Row2, RowTicks;
			Content.HSplitTop(22.0f, &Row1, &Content);
			Content.HSplitTop(22.0f, &Row2, &Content);
			Content.HSplitTop(24.0f, &RowTicks, &Content);
			AvoidCheckBoxPair(this, 17, Row1, BcLocalize("Death tiles"), &g_Config.m_BcAvoidBlatantDeath, BcLocalize("Teleport tiles"), &g_Config.m_BcAvoidBlatantTeles);
			AvoidCheckBoxRow(this, 18, Row2, BcLocalize("Unfreeze tiles"), &g_Config.m_BcAvoidBlatantUnfreeze);
			Ui()->DoScrollbarOption(&g_Config.m_BcAvoidBlatantUnfreezeTicks, &g_Config.m_BcAvoidBlatantUnfreezeTicks, &RowTicks, BcLocalize("Unfreeze ticks"), 0, 30, &CUi::ms_LinearScrollbarScale, 0u, BcLocalize("t"));

			Content.HSplitTop(8.0f, nullptr, &Content);
			AvoidHintBottom(Ui(), TextRender(), &Content,
				BcLocalize("Freeze tiles are always avoided. Turning unfreeze tiles on is only useful on maps where being unfrozen is more dangerous than staying frozen."), 10.0f);
			break;
		}

		// ------------------------------------------------------ Blatant aimbot --------
		case PANEL_BLATANT_AIMBOT:
		{
			AvoidSectionTitle(Ui(), &Content, BcLocalize("Internal aim"));
			CUIRect RowEnable, RowSeg, RowFov, RowAim, RowAssist;
			Content.HSplitTop(22.0f, &RowEnable, &Content);
			Content.HSplitTop(24.0f, &RowSeg, &Content);
			Content.HSplitTop(24.0f, &RowFov, &Content);
			Content.HSplitTop(22.0f, &RowAim, &Content);
			Content.HSplitTop(22.0f, &RowAssist, &Content);

			AvoidCheckBoxRow(this, 19, RowEnable, BcLocalize("Enable internal aimbot"), &g_Config.m_BcAvoidAimbot);
			Ui()->DoScrollbarOption(&g_Config.m_BcAvoidAimbotSegments, &g_Config.m_BcAvoidAimbotSegments, &RowSeg, BcLocalize("Segments"), 1, 64, &CUi::ms_LinearScrollbarScale, 0u);
			Ui()->DoScrollbarOption(&g_Config.m_BcAvoidAimbotFov, &g_Config.m_BcAvoidAimbotFov, &RowFov, BcLocalize("Field of view"), 10, 360, &CUi::ms_LinearScrollbarScale, 0u, BcLocalize("deg"));
			AvoidCheckBoxRow(this, 20, RowAim, BcLocalize("Auto aim (longest survival)"), &g_Config.m_BcAvoidAutoAim);
			AvoidCheckBoxRow(this, 21, RowAssist, BcLocalize("Aim assist (closest safe aim)"), &g_Config.m_BcAvoidAimAssist);

			Content.HSplitTop(6.0f, nullptr, &Content);
			AvoidHintBottom(Ui(), TextRender(), &Content,
				BcLocalize("Segments is how many directions are scanned inside the field of view. Auto aim takes the direction that survives longest, aim assist the safe direction closest to your cursor; both only apply while the internal aimbot is enabled."), 10.0f);
			break;
		}

		// -------------------------------------------------------- Fentbot calc --------
		case PANEL_FENT_CALC:
		{
			AvoidSectionTitle(Ui(), &Content, BcLocalize("Light tiles"));
			CUIRect RowLight, RowRadius;
			Content.HSplitTop(22.0f, &RowLight, &Content);
			Content.HSplitTop(24.0f, &RowRadius, &Content);
			AvoidCheckBoxRow(this, 22, RowLight, BcLocalize("Cross light freeze tiles"), &g_Config.m_BcAvoidFentLightTile);
			Ui()->DoScrollbarOption(&g_Config.m_BcAvoidFentLightTileRadius, &g_Config.m_BcAvoidFentLightTileRadius, &RowRadius, BcLocalize("Unfreeze radius"), 0, 20, &CUi::ms_LinearScrollbarScale, 0u, BcLocalize("tiles"));

			Content.HSplitTop(6.0f, nullptr, &Content);
			AvoidHintBottom(Ui(), TextRender(), &Content,
				BcLocalize("A freeze tile becomes crossable when an unfreeze tile is inside the radius, which is what lets the bot solve light freeze segments."), 10.0f);

			Content.HSplitTop(10.0f, nullptr, &Content);
			AvoidSectionTitle(Ui(), &Content, BcLocalize("Search"));
			CUIRect RowAdvanced;
			Content.HSplitTop(22.0f, &RowAdvanced, &Content);
			AvoidCheckBoxRow(this, 23, RowAdvanced, BcLocalize("Advanced settings"), &g_Config.m_BcAvoidFentAdvanced);

			if(!g_Config.m_BcAvoidFentAdvanced)
			{
				CUIRect RowPreset;
				Content.HSplitTop(6.0f, nullptr, &Content);
				AvoidSectionTitle(Ui(), &Content, BcLocalize("Quality setting"));
				Content.HSplitTop(24.0f, &RowPreset, &Content);
				const char *apPresets[] = {BcLocalize("Low"), BcLocalize("Mid"), BcLocalize("Max")};
				AvoidSegmented(this, 1, RowPreset, apPresets, 3, &g_Config.m_BcAvoidFentQuality);

				Content.HSplitTop(8.0f, nullptr, &Content);
				AvoidHintBottom(Ui(), TextRender(), &Content,
					BcLocalize("Quality setting picks the presets from the reference client: Low 88, Mid 160 and Max 1000 candidate inputs with 88, 160 and 300 optimisation generations. All of them plan 10000 ticks ahead."), 10.0f);
			}
			else
			{
				CUIRect RowTicks, RowActions, RowHold, RowDosage;
				Content.HSplitTop(6.0f, nullptr, &Content);
				AvoidSectionTitle(Ui(), &Content, BcLocalize("Advanced"));
				Content.HSplitTop(24.0f, &RowTicks, &Content);
				Content.HSplitTop(24.0f, &RowActions, &Content);
				Content.HSplitTop(24.0f, &RowHold, &Content);
				Content.HSplitTop(24.0f, &RowDosage, &Content);
				Ui()->DoScrollbarOption(&g_Config.m_BcAvoidFentTicks, &g_Config.m_BcAvoidFentTicks, &RowTicks, BcLocalize("Fent ticks"), 1000, 10000, &CUi::ms_LinearScrollbarScale, 0u, BcLocalize("t"));
				Ui()->DoScrollbarOption(&g_Config.m_BcAvoidFentTweakerActions, &g_Config.m_BcAvoidFentTweakerActions, &RowActions, BcLocalize("Tweaker inputs"), 50, 5000, &CUi::ms_LinearScrollbarScale, 0u);
				Ui()->DoScrollbarOption(&g_Config.m_BcAvoidFentTweakerTicks, &g_Config.m_BcAvoidFentTweakerTicks, &RowHold, BcLocalize("Tweaker ticks"), 1, 30, &CUi::ms_LinearScrollbarScale, 0u, BcLocalize("t"));
				Ui()->DoScrollbarOption(&g_Config.m_BcAvoidFentTweakerDosage, &g_Config.m_BcAvoidFentTweakerDosage, &RowDosage, BcLocalize("Tweaker dosage"), 1, 500, &CUi::ms_LinearScrollbarScale, 0u);

				Content.HSplitTop(8.0f, nullptr, &Content);
				AvoidHintBottom(Ui(), TextRender(), &Content,
					BcLocalize("Fent ticks is how far the search plans ahead, Tweaker inputs how many candidate input sequences each generation has, Tweaker ticks how many ticks one candidate covers and Tweaker dosage how many generations are run. Higher values solve more, but take longer to converge."), 10.0f);
			}
			break;
		}

		// --------------------------------------------------------- Pilot main --------
		case PANEL_PILOT_MAIN:
		{
			AvoidSectionTitle(Ui(), &Content, BcLocalize("Navigation mode"));
			CUIRect RowMode;
			Content.HSplitTop(24.0f, &RowMode, &Content);
			Content.HSplitTop(10.0f, nullptr, &Content);

			const char *apModes[] = {BcLocalize("Autonomous"), BcLocalize("Follow cursor"), BcLocalize("Follow player")};
			AvoidSegmented(this, 2, RowMode, apModes, 3, &g_Config.m_BcAvoidPilotMode);

			const char *pModeDesc;
			switch(g_Config.m_BcAvoidPilotMode)
			{
			case 1:
				pModeDesc = BcLocalize("The bot drives the tee towards your mouse cursor and still avoids every hazard on the way.");
				break;
			case 2:
				pModeDesc = BcLocalize("The bot drives the tee towards the closest other player, which is what you want for following a run.");
				break;
			case 0:
			default:
				pModeDesc = BcLocalize("The bot drives the tee on its own, following the flow field towards the finish or the next unfreeze tile of the map.");
				break;
			}
			AvoidHintBottom(Ui(), TextRender(), &Content, pModeDesc, 10.0f);
			break;
		}

		// ----------------------------------------------------- Pilot settings --------
		case PANEL_PILOT_SETTINGS:
		{
			AvoidSectionTitle(Ui(), &Content, BcLocalize("Search budget"));
			CUIRect RowPop, RowDepth, RowTopK, RowSeq;
			Content.HSplitTop(24.0f, &RowPop, &Content);
			Content.HSplitTop(24.0f, &RowDepth, &Content);
			Content.HSplitTop(24.0f, &RowTopK, &Content);
			Content.HSplitTop(24.0f, &RowSeq, &Content);
			Ui()->DoScrollbarOption(&g_Config.m_BcAvoidPilotPopulation, &g_Config.m_BcAvoidPilotPopulation, &RowPop, BcLocalize("Population size"), 128, 8192, &CUi::ms_LinearScrollbarScale, 0u);
			Ui()->DoScrollbarOption(&g_Config.m_BcAvoidPilotDepth, &g_Config.m_BcAvoidPilotDepth, &RowDepth, BcLocalize("Exploration depth"), 5, 50, &CUi::ms_LinearScrollbarScale, 0u, BcLocalize("t"));
			Ui()->DoScrollbarOption(&g_Config.m_BcAvoidPilotTopK, &g_Config.m_BcAvoidPilotTopK, &RowTopK, BcLocalize("Top-K candidates"), 1, 100, &CUi::ms_LinearScrollbarScale, 0u);
			Ui()->DoScrollbarOption(&g_Config.m_BcAvoidPilotSequence, &g_Config.m_BcAvoidPilotSequence, &RowSeq, BcLocalize("Sequence length"), 1, 20, &CUi::ms_LinearScrollbarScale, 0u, BcLocalize("t"));

			Content.HSplitTop(8.0f, nullptr, &Content);
			AvoidHintBottom(Ui(), TextRender(), &Content,
				BcLocalize("Population size is how many input sequences are tested, exploration depth how many ticks each of them is simulated for, Top-K how many of the best survive into the next generation and sequence length how many ticks are driven before the plan is replaced by a better one."), 10.0f);
			break;
		}
		}
	}
}
