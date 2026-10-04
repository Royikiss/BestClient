/* Copyright © 2026 BestProject Team */
#include "avoid.h"

#include <base/math.h>
#include <base/mem.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/client.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/textrender.h>

#include <game/client/components/bestclient/fast_practice.h>
#include <game/client/components/hud_layout.h>
#include <game/client/gameclient.h>
#include <game/client/prediction/entities/character.h>
#include <game/client/ui.h>
#include <game/collision.h>
#include <game/localization.h>
#include <game/mapitems.h>

#include <algorithm>
#include <cmath>

namespace
{
	constexpr float TILE_SIZE = 32.0f;

	// The HUD module is laid out on the HUD canvas, so every size is relative to that canvas.
	constexpr float HUD_BASE_WIDTH = 122.0f;
	constexpr float HUD_BASE_HEIGHT = 62.0f;
	constexpr float HUD_PADDING = 3.0f;
	constexpr float HUD_HEADER_HEIGHT = 9.0f;
	constexpr float HUD_ROW_HEIGHT = 7.5f;
	constexpr float HUD_FONT_HEADER = 5.5f;
	constexpr float HUD_FONT_ROW = 5.0f;
	constexpr float HUD_BADGE_WIDTH = 34.0f;
	constexpr float HUD_LABEL_WIDTH = 40.0f;

	ColorRGBA StateColor(int State)
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

	ColorRGBA PathColor()
	{
		return ColorRGBA(0.40f, 0.80f, 1.00f, 0.85f);
	}
} // namespace

// ---------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------

CAvoid::CAvoid() = default;

CAvoid::~CAvoid()
{
	for(int i = 0; i < NUM_AGENTS; ++i)
	{
		delete m_apAgents[i];
		m_apAgents[i] = nullptr;
	}
}

void CAvoid::OnConsoleInit()
{
	Console()->Register("avoid_toggle", "", CFGFLAG_CLIENT, ConAvoidToggle, this, "Toggle the Avoid Gores bot (same as bc_avoid_enabled)");
	Console()->Register("avoid_status", "", CFGFLAG_CLIENT, ConAvoidStatus, this, "Print the current Avoid state to the console");
	Console()->Register("avoid_reset", "", CFGFLAG_CLIENT, ConAvoidReset, this, "Reset the Avoid telemetry counters");
}

void CAvoid::OnInit()
{
	m_apAgents[AGENT_BASIC] = new Avoid::CBasicAgent(GameClient());
	m_apAgents[AGENT_LEGIT] = new Avoid::CLegitAgent(GameClient());
	m_apAgents[AGENT_BLATANT] = new Avoid::CBlatantAgent(GameClient());
	m_apAgents[AGENT_FENTBOT] = new Avoid::CFentbotAgent(GameClient());
	m_apAgents[AGENT_PILOT] = new Avoid::CPilotAgent(GameClient());
}

void CAvoid::OnReset()
{
	m_Telemetry = STelemetry{};
	m_vLastPath.clear();
	m_LastDecisionTick = -1;
	m_IdleTicks = 0;
	m_DroveTick = -1;
	m_YieldTick = -1;
	for(Avoid::BLAgent *pAgent : m_apAgents)
	{
		if(pAgent)
			pAgent->OnReset();
	}
}

void CAvoid::OnMapLoad()
{
	m_Telemetry = STelemetry{};
	m_vLastPath.clear();
	m_LastDecisionTick = -1;
	m_IdleTicks = 0;
	m_DroveTick = -1;
	m_YieldTick = -1;
	for(Avoid::BLAgent *pAgent : m_apAgents)
	{
		if(pAgent)
			pAgent->OnReset();
	}
}

// ---------------------------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------------------------

int CAvoid::Agent() const
{
	return std::clamp(g_Config.m_BcAvoidAgent, 0, (int)NUM_AGENTS - 1);
}

const char *CAvoid::AgentName(int Agent)
{
	switch(Agent)
	{
	case AGENT_BASIC: return BcLocalize("Basic");
	case AGENT_LEGIT: return BcLocalize("Legit");
	case AGENT_BLATANT: return BcLocalize("Blatant");
	case AGENT_FENTBOT: return BcLocalize("Fentbot");
	case AGENT_PILOT: return BcLocalize("Pilot");
	default: return BcLocalize("Basic");
	}
}

void CAvoid::SetAgent(int Agent)
{
	const int Previous = this->Agent();
	g_Config.m_BcAvoidAgent = std::clamp(Agent, 0, (int)NUM_AGENTS - 1);
	m_vLastPath.clear();
	m_LastDecisionTick = -1;
	m_LastOverrideActive = false;
	// Both sides are reset: the new agent must not inherit anything, and the old one releases the
	// navigable grid it built, which is the largest allocation of the whole module.
	for(const int Index : {Previous, this->Agent()})
	{
		if(Index >= 0 && Index < NUM_AGENTS && m_apAgents[Index])
			m_apAgents[Index]->OnReset();
	}
}

bool CAvoid::IsEnabled() const
{
	return g_Config.m_BcAvoidEnabled != 0;
}

void CAvoid::SetEnabled(bool Enabled)
{
	g_Config.m_BcAvoidEnabled = Enabled ? 1 : 0;
	m_IdleTicks = 0;
	m_LastDecisionTick = -1;
	m_vLastPath.clear();
	m_LastOverrideActive = false;
	// m_DroveTick / m_YieldTick are deliberately kept: if the agent was driving, the next tick has
	// to hand the player's own input back with a packet, and the consecutive-tick test in
	// FinishInput() makes a stale value harmless otherwise.
	m_Telemetry.m_State = Enabled ? STATE_WATCHING : STATE_OFF;
}

void CAvoid::ToggleEnabled()
{
	SetEnabled(!IsEnabled());
}

void CAvoid::ResetCounters()
{
	m_Telemetry.m_Decisions = 0;
	m_Telemetry.m_Overrides = 0;
	m_Telemetry.m_NsifFallbacks = 0;
	GameClient()->Echo(BcLocalize("Avoid: counters reset"));
}

void CAvoid::PrintStatus() const
{
	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "[avoid] state %s | agent %s | enabled %s | safe %d ticks | cost %.3f ms | decisions %d | overrides %d | nsif %d",
		StateName(m_Telemetry.m_State), AgentName(m_Telemetry.m_Agent), m_Telemetry.m_Enabled ? "yes" : "no",
		m_Telemetry.m_SurvivalTicks, m_Telemetry.m_CostMs, m_Telemetry.m_Decisions,
		m_Telemetry.m_Overrides, m_Telemetry.m_NsifFallbacks);
	GameClient()->Echo(aBuf);

	str_format(aBuf, sizeof(aBuf), "[avoid] last plan: %s", m_Telemetry.m_aReason[0] ? m_Telemetry.m_aReason : "<none>");
	GameClient()->Echo(aBuf);
}

const char *CAvoid::StateName(int State)
{
	switch(State)
	{
	case STATE_OFF: return BcLocalize("OFF");
	case STATE_WATCHING: return BcLocalize("WATCH");
	case STATE_ASSISTING: return BcLocalize("ASSIST");
	case STATE_NSIF: return BcLocalize("NSIF");
	case STATE_AFK: return BcLocalize("AFK");
	default: return BcLocalize("OFF");
	}
}

CAvoid::SSettings CAvoid::ReadSettings() const
{
	SSettings S;
	S.m_Agent = Agent();
	S.m_AfkProtection = g_Config.m_BcAvoidAfkProtection != 0;
	S.m_AfkTime = std::clamp(g_Config.m_BcAvoidAfkTime, 5, 300);
	S.m_PlayerPrediction = g_Config.m_BcAvoidPlayerPrediction != 0;
	S.m_DrawPath = g_Config.m_BcAvoidDrawPath != 0;

	S.m_LegitDirectionWeight = std::clamp(g_Config.m_BcAvoidLegitDirectionWeight, 1, 1000);
	S.m_LegitLifespanWeight = std::clamp(g_Config.m_BcAvoidLegitLifespanWeight, 1, 1000);
	S.m_LegitHookWeight = std::clamp(g_Config.m_BcAvoidLegitHookWeight, 1, 1000);
	S.m_LegitExploration = std::clamp(g_Config.m_BcAvoidLegitExploration, 1, 1000);
	S.m_LegitIterations = std::clamp(g_Config.m_BcAvoidLegitIterations, 1, 1000);
	S.m_LegitCheckTicks = std::clamp(g_Config.m_BcAvoidLegitCheckTicks, 1, 50);
	S.m_LegitDirection = g_Config.m_BcAvoidLegitDirection != 0;
	S.m_LegitHook = g_Config.m_BcAvoidLegitHook != 0;
	S.m_LegitTeles = g_Config.m_BcAvoidLegitTeles != 0;
	S.m_LegitDeath = g_Config.m_BcAvoidLegitDeath != 0;
	S.m_LegitUnfreeze = g_Config.m_BcAvoidLegitUnfreeze != 0;
	S.m_LegitUnfreezeTicks = std::clamp(g_Config.m_BcAvoidLegitUnfreezeTicks, 1, 30);

	S.m_BlatantCheckTicks = std::clamp(g_Config.m_BcAvoidBlatantCheckTicks, 1, 50);
	S.m_KickInTicks = std::clamp(g_Config.m_BcAvoidKickInTicks, 1, 50);
	S.m_BlatantDirection = g_Config.m_BcAvoidBlatantDirection != 0;
	S.m_BlatantHook = g_Config.m_BcAvoidBlatantHook != 0;
	S.m_BlatantTeles = g_Config.m_BcAvoidBlatantTeles != 0;
	S.m_BlatantDeath = g_Config.m_BcAvoidBlatantDeath != 0;
	S.m_BlatantUnfreeze = g_Config.m_BcAvoidBlatantUnfreeze != 0;
	S.m_BlatantUnfreezeTicks = std::clamp(g_Config.m_BcAvoidBlatantUnfreezeTicks, 0, 30);
	S.m_Nsif = g_Config.m_BcAvoidNsif != 0;
	S.m_TrackPoint = g_Config.m_BcAvoidTrackPoint != 0;
	S.m_SafeAimTracking = g_Config.m_BcAvoidSafeAimTracking != 0;
	S.m_AutoDrag = g_Config.m_BcAvoidAutoDrag != 0;
	S.m_Aimbot = g_Config.m_BcAvoidAimbot != 0;
	S.m_AimbotFov = std::clamp(g_Config.m_BcAvoidAimbotFov, 10, 360);
	S.m_AimbotSegments = std::clamp(g_Config.m_BcAvoidAimbotSegments, 1, 64);
	S.m_AutoAim = g_Config.m_BcAvoidAutoAim != 0;
	S.m_AimAssist = g_Config.m_BcAvoidAimAssist != 0;

	S.m_FentQuality = std::clamp(g_Config.m_BcAvoidFentQuality, 0, 2);
	S.m_FentAdvanced = g_Config.m_BcAvoidFentAdvanced != 0;
	S.m_FentTicks = std::clamp(g_Config.m_BcAvoidFentTicks, 1000, 10000);
	S.m_FentTweakerActions = std::clamp(g_Config.m_BcAvoidFentTweakerActions, 50, 5000);
	S.m_FentTweakerTicks = std::clamp(g_Config.m_BcAvoidFentTweakerTicks, 1, 30);
	S.m_FentTweakerDosage = std::clamp(g_Config.m_BcAvoidFentTweakerDosage, 1, 500);
	S.m_FentLightTile = g_Config.m_BcAvoidFentLightTile != 0;
	S.m_FentLightTileRadius = std::clamp(g_Config.m_BcAvoidFentLightTileRadius, 0, 20);

	S.m_PilotMode = std::clamp(g_Config.m_BcAvoidPilotMode, 0, 2);
	S.m_PilotPopulation = std::clamp(g_Config.m_BcAvoidPilotPopulation, 128, 8192);
	S.m_PilotDepth = std::clamp(g_Config.m_BcAvoidPilotDepth, 5, 50);
	S.m_PilotTopK = std::clamp(g_Config.m_BcAvoidPilotTopK, 1, 100);
	S.m_PilotSequence = std::clamp(g_Config.m_BcAvoidPilotSequence, 1, 20);

	Avoid::ResolveFentPreset(S);
	return S;
}

// ---------------------------------------------------------------------------------------------
// Input pipeline
// ---------------------------------------------------------------------------------------------

const CCharacterCore *CAvoid::ActiveCore(int *pClientId) const
{
	if(GameClient()->m_FastPractice.Active())
	{
		int LocalId = -1;
		int DummyId = -1;
		if(GameClient()->m_FastPractice.ResolvePracticeRoles(LocalId, DummyId))
		{
			if(CCharacter *pChar = GameClient()->m_FastPractice.PracticeWorld().GetCharacterById(LocalId))
			{
				if(pClientId)
					*pClientId = LocalId;
				return pChar->Core();
			}
		}
	}

	const int ClientId = GameClient()->m_Snap.m_LocalClientId;
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return nullptr;
	if(!GameClient()->m_aClients[ClientId].m_Active)
		return nullptr;

	if(pClientId)
		*pClientId = ClientId;
	return &GameClient()->m_aClients[ClientId].m_Predicted;
}

void CAvoid::CheckAfkProtection(const SSettings &Set, const Avoid::SContext &Ctx)
{
	const bool ActiveInput = Ctx.m_Input.m_Direction != 0 || Ctx.m_Input.m_Jump != 0 ||
				 Ctx.m_Input.m_Hook != 0 || (Ctx.m_Input.m_Fire & 1) != 0;
	if(ActiveInput)
		m_IdleTicks = 0;
	else if(m_IdleTicks < 50 * 600)
		m_IdleTicks++;

	if(!Set.m_AfkProtection || !IsEnabled())
		return;

	if(m_IdleTicks >= 50 * Set.m_AfkTime)
	{
		SetEnabled(false);
		m_Telemetry.m_State = STATE_AFK;
		str_copy(m_Telemetry.m_aReason, "AFK protection disabled the bot");
		GameClient()->Echo(BcLocalize("Avoid: AFK protection disabled the bot"));
	}
}

CAvoid::EInputResult CAvoid::FinishInput(bool Drives, CNetObj_PlayerInput *pInput, int Tick)
{
	if(Drives)
	{
		m_DroveTick = Tick;
		m_YieldTick = -1;
		*pInput = m_LastOverride;
		return INPUT_DRIVEN;
	}

	// The tick right after the agent stopped driving still has to put the player's own input on
	// the wire, because the sampler compared its raw state against its own raw state and therefore
	// has no idea that anything changed. Remembering the tick keeps that true for a re-send of the
	// same tick.
	if(m_DroveTick >= 0 && Tick == m_DroveTick + 1)
		m_YieldTick = Tick;
	m_DroveTick = -1;
	return m_YieldTick == Tick ? INPUT_YIELDED : INPUT_IDLE;
}

CAvoid::EInputResult CAvoid::ApplyInput(CNetObj_PlayerInput *pInput)
{
	if(!pInput)
		return INPUT_IDLE;

	Avoid::SContext Ctx;
	Ctx.m_Tick = Client()->PredGameTick(g_Config.m_ClDummy);
	Ctx.m_Settings = ReadSettings();
	Ctx.m_Input = *pInput;

	if(!ActiveCore(&Ctx.m_LocalClientId))
	{
		m_Telemetry.m_State = STATE_OFF;
		m_Telemetry.m_Enabled = IsEnabled();
		m_Telemetry.m_Agent = Agent();
		m_LastOverrideActive = false;
		return FinishInput(false, pInput, Ctx.m_Tick);
	}

	if(!IsEnabled())
	{
		m_Telemetry.m_State = STATE_OFF;
		m_Telemetry.m_Enabled = false;
		m_Telemetry.m_Agent = Agent();
		m_Telemetry.m_SurvivalTicks = 0;
		m_Telemetry.m_CostMs = 0.0f;
		m_vLastPath.clear();
		m_LastOverrideActive = false;
		return FinishInput(false, pInput, Ctx.m_Tick);
	}

	CheckAfkProtection(Ctx.m_Settings, Ctx);
	if(!IsEnabled())
	{
		// AFK protection just fired: it has to take effect on this very tick, otherwise the bot
		// would keep steering after it announced that it stopped.
		m_Telemetry.m_Enabled = false;
		m_Telemetry.m_Agent = Agent();
		m_Telemetry.m_SurvivalTicks = 0;
		m_Telemetry.m_CostMs = 0.0f;
		m_vLastPath.clear();
		m_LastOverrideActive = false;
		return FinishInput(false, pInput, Ctx.m_Tick);
	}

	// One decision per game tick. The client can ask for the same tick twice when it has to
	// re-send, so the decision is remembered and replayed instead of being recalculated (which
	// would advance the agents' search state twice for one tick).
	if(Ctx.m_Tick != m_LastDecisionTick)
	{
		m_LastDecisionTick = Ctx.m_Tick;

		Avoid::AvoidInput Action;
		Action.m_Input = Ctx.m_Input;
		const int64_t StartTime = time_get();

		const int AgentId = Agent();
		Avoid::BLAgent *pAgent = (AgentId >= 0 && AgentId < NUM_AGENTS) ? m_apAgents[AgentId] : nullptr;
		if(pAgent)
		{
			Action = pAgent->GetAction(Ctx, Avoid::GetActiveWorld(GameClient()));
		}
		else
		{
			str_copy(Action.m_aReason, "agent unavailable");
		}

		const float CostMs = (float)((double)(time_get() - StartTime) * 1000.0 / (double)time_freq());

		m_LastOverrideActive = Action.m_Active != 0;
		m_LastOverride = Action.m_Input;
		if(Action.m_Active)
		{
			m_Telemetry.m_Overrides++;
			if(Action.m_UsedFallback)
				m_Telemetry.m_NsifFallbacks++;
		}
		m_Telemetry.m_Decisions++;

		m_vLastPath = Action.m_vPath;
		UpdateTelemetry(Action, CostMs);
	}

	return FinishInput(m_LastOverrideActive, pInput, Ctx.m_Tick);
}

void CAvoid::UpdateTelemetry(const Avoid::AvoidInput &Action, float CostMs)
{
	m_Telemetry.m_Enabled = IsEnabled();
	m_Telemetry.m_Agent = Agent();
	m_Telemetry.m_SurvivalTicks = Action.m_SurvivalTicks;
	m_Telemetry.m_CostMs = CostMs;
	m_Telemetry.m_TrackPoint = Action.m_TrackPoint;
	m_Telemetry.m_AimTarget = Action.m_AimTarget;
	const int AgentId = Agent();
	m_Telemetry.m_NavigatorReady = (AgentId >= 0 && AgentId < NUM_AGENTS && m_apAgents[AgentId]) ?
					       m_apAgents[AgentId]->NavigatorReady() :
					       false;
	str_copy(m_Telemetry.m_aReason, Action.m_aReason);

	if(!IsEnabled())
	{
		m_Telemetry.m_State = m_Telemetry.m_State == STATE_AFK ? STATE_AFK : STATE_OFF;
		return;
	}
	if(!Action.m_Active)
	{
		m_Telemetry.m_State = STATE_WATCHING;
		return;
	}
	m_Telemetry.m_State = Action.m_UsedFallback ? STATE_NSIF : STATE_ASSISTING;
}

// ---------------------------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------------------------

void CAvoid::OnRender()
{
	if(Client()->State() != IClient::STATE_ONLINE)
		return;
	if(GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_LocalClientId < 0)
		return;

	const int AgentId = Agent();
	if(AgentId >= 0 && AgentId < NUM_AGENTS && m_apAgents[AgentId])
		m_apAgents[AgentId]->OnRender();

	if(IsEnabled() && (g_Config.m_BcAvoidDrawPath || g_Config.m_BcAvoidDrawTrackPoint || g_Config.m_BcAvoidDrawAimbot))
	{
		const CCharacterCore *pCore = ActiveCore(nullptr);
		if(pCore)
			RenderWorldOverlay(*pCore);
	}

	if(IsHudVisible())
		RenderHudModule(false);
}

bool CAvoid::IsHudVisible() const
{
	return HudLayout::IsEnabled(HudLayout::MODULE_AVOID);
}

CUIRect CAvoid::GetHudRect(bool ForcePreview) const
{
	if(!ForcePreview && !IsHudVisible())
		return CUIRect{};

	const float HudHeight = HudLayout::CANVAS_HEIGHT;
	const float HudWidth = HudHeight * Graphics()->ScreenAspect();
	const HudLayout::SModuleLayout Layout = HudLayout::Get(HudLayout::MODULE_AVOID, HudWidth, HudHeight);
	const float Scale = std::clamp(Layout.m_Scale / 100.0f, 0.25f, 3.0f);

	const float BoxWidth = HUD_BASE_WIDTH * Scale;
	const float BoxHeight = HUD_BASE_HEIGHT * Scale;

	HudLayout::SModuleRect RawRect;
	if(!HudLayout::HasPositionOverride(HudLayout::MODULE_AVOID))
	{
		RawRect = {Layout.m_X, Layout.m_Y, BoxWidth, BoxHeight, 5.0f * Scale};
	}
	else
	{
		float X = Layout.m_X;
		float Y = Layout.m_Y;
		if(Layout.m_Mode == HudLayout::POSITION_MODE_BOTTOM_RIGHT)
		{
			X -= BoxWidth;
			Y -= BoxHeight;
		}
		RawRect = {X, Y, BoxWidth, BoxHeight, 5.0f * Scale};
	}

	const auto Clamped = HudLayout::ClampRectToScreen(RawRect, HudWidth, HudHeight);
	return CUIRect{Clamped.m_X, Clamped.m_Y, Clamped.m_W, Clamped.m_H};
}

CUIRect CAvoid::GetHudEditorRect() const
{
	return GetHudRect(true);
}

void CAvoid::RenderPreview()
{
	RenderHudModule(true);
}

void CAvoid::RenderHudModule(bool ForcePreview)
{
	const CUIRect Screen = GetHudRect(ForcePreview);
	if(Screen.w <= 0.0f || Screen.h <= 0.0f)
		return;

	const float HudHeight = HudLayout::CANVAS_HEIGHT;
	const float HudWidth = HudHeight * Graphics()->ScreenAspect();
	Graphics()->MapScreenToSize(HudWidth, HudHeight);

	const HudLayout::SModuleLayout Layout = HudLayout::Get(HudLayout::MODULE_AVOID, HudWidth, HudHeight);
	const float Scale = std::clamp(Layout.m_Scale / 100.0f, 0.25f, 3.0f);
	const float Alpha = std::clamp(Layout.m_Alpha / 100.0f, 0.05f, 1.0f);

	CUIRect Canvas = Screen;
	// The panel background is a normal HUD module setting, so it follows the HUD editor.
	if(Layout.m_BackgroundEnabled)
		Canvas.Draw(ColorRGBA(0.06f, 0.08f, 0.12f, 0.85f * Alpha), IGraphics::CORNER_ALL, 4.0f * Scale);

	CUIRect Header, Content;
	Canvas.Margin(HUD_PADDING * Scale, &Content);
	Content.HSplitTop(HUD_HEADER_HEIGHT * Scale, &Header, &Content);

	CUIRect Badge, TitleRect;
	Header.VSplitRight(HUD_BADGE_WIDTH * Scale, &TitleRect, &Badge);

	TextRender()->TextColor(1.0f, 1.0f, 1.0f, Alpha);
	char aTitle[64];
	str_format(aTitle, sizeof(aTitle), "AVOID: %s", AgentName(ForcePreview ? AGENT_BASIC : m_Telemetry.m_Agent));
	Ui()->DoLabel(&TitleRect, aTitle, HUD_FONT_HEADER * Scale, TEXTALIGN_ML);

	const int State = ForcePreview ? STATE_WATCHING : m_Telemetry.m_State;
	ColorRGBA BadgeCol = StateColor(State);
	BadgeCol.a *= Alpha;
	Badge.Draw(BadgeCol, IGraphics::CORNER_ALL, 2.0f * Scale);
	TextRender()->TextColor(0.05f, 0.05f, 0.08f, Alpha);
	Ui()->DoLabel(&Badge, StateName(State), HUD_FONT_HEADER * Scale, TEXTALIGN_MC);

	auto DrawRow = [&](CUIRect &Area, const char *pLabel, const char *pValue, ColorRGBA ValCol) {
		CUIRect Row, LabelArea, ValArea;
		Area.HSplitTop(HUD_ROW_HEIGHT * Scale, &Row, &Area);
		Row.VSplitLeft(HUD_LABEL_WIDTH * Scale, &LabelArea, &ValArea);
		TextRender()->TextColor(0.65f, 0.70f, 0.78f, 0.9f * Alpha);
		Ui()->DoLabel(&LabelArea, pLabel, HUD_FONT_ROW * Scale, TEXTALIGN_ML);
		ValCol.a *= Alpha;
		TextRender()->TextColor(ValCol);
		Ui()->DoLabel(&ValArea, pValue, HUD_FONT_ROW * Scale, TEXTALIGN_ML);
	};

	char aBuf[64];
	if(ForcePreview)
		str_copy(aBuf, BcLocalize("player input safe"));
	else
		str_copy(aBuf, m_Telemetry.m_aReason[0] ? m_Telemetry.m_aReason : "<none>");
	DrawRow(Content, BcLocalize("Plan:"), aBuf, ColorRGBA(0.95f, 0.85f, 0.45f, 1.0f));

	if(ForcePreview)
		str_copy(aBuf, "26 tick");
	else
		str_format(aBuf, sizeof(aBuf), "%d tick", m_Telemetry.m_SurvivalTicks);
	DrawRow(Content, BcLocalize("Safe:"), aBuf, ColorRGBA(0.85f, 0.90f, 0.98f, 1.0f));

	if(ForcePreview)
		str_copy(aBuf, "0.00 ms");
	else
		str_format(aBuf, sizeof(aBuf), "%.2f ms", m_Telemetry.m_CostMs);
	DrawRow(Content, BcLocalize("Cost:"), aBuf, ColorRGBA(0.85f, 0.90f, 0.98f, 1.0f));

	if(ForcePreview)
		str_copy(aBuf, "0 / 0");
	else
		str_format(aBuf, sizeof(aBuf), "%d / %d", m_Telemetry.m_Overrides, m_Telemetry.m_Decisions);
	DrawRow(Content, BcLocalize("Override:"), aBuf, ColorRGBA(0.85f, 0.90f, 0.98f, 1.0f));

	TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
}

void CAvoid::RenderWorldOverlay(const CCharacterCore &Core)
{
	if(!Collision() || Collision()->GetWidth() <= 0 || Collision()->GetHeight() <= 0)
		return;
	if(Graphics()->ScreenAspect() <= 0.0f)
		return;

	const CScreenRect PreviousScreen = Graphics()->GetScreen();
	const CScreenRect WorldScreen = Graphics()->MapScreenToWorld(
		GameClient()->m_Camera.m_Center.x,
		GameClient()->m_Camera.m_Center.y,
		100.0f, 100.0f, 100.0f, 0, 0,
		Graphics()->ScreenAspect(),
		GameClient()->m_Camera.m_Zoom);
	Graphics()->MapScreen(WorldScreen);

	// 1. The predicted path of the current plan.
	if(g_Config.m_BcAvoidDrawPath && m_vLastPath.size() >= 2)
	{
		static std::vector<IGraphics::CLineItem> s_vLines;
		s_vLines.clear();
		s_vLines.reserve(m_vLastPath.size() - 1);
		for(size_t i = 0; i + 1 < m_vLastPath.size(); ++i)
		{
			const vec2 A = m_vLastPath[i];
			const vec2 B = m_vLastPath[i + 1];
			if(!std::isfinite(A.x) || !std::isfinite(A.y) || !std::isfinite(B.x) || !std::isfinite(B.y))
				continue;
			s_vLines.emplace_back(A, B);
		}
		if(!s_vLines.empty())
		{
			Graphics()->TextureClear();
			Graphics()->LinesBegin();
			Graphics()->SetColor(PathColor());
			Graphics()->LinesDraw(s_vLines.data(), (int)s_vLines.size());
			Graphics()->LinesEnd();
		}
	}

	// 2. The Blatant tracked aim point.
	if(g_Config.m_BcAvoidDrawTrackPoint && m_Telemetry.m_TrackPoint.m_Valid)
	{
		const vec2 Target = m_Telemetry.m_TrackPoint.m_Pos;
		const vec2 Diff = Target - Core.m_Pos;
		if(length(Diff) > 1.0f)
		{
			static IGraphics::CLineItem s_aMarker[5];
			s_aMarker[0] = IGraphics::CLineItem(Core.m_Pos, Target);
			s_aMarker[1] = IGraphics::CLineItem(Target + vec2(-6.0f, 0.0f), Target + vec2(6.0f, 0.0f));
			s_aMarker[2] = IGraphics::CLineItem(Target + vec2(0.0f, -6.0f), Target + vec2(0.0f, 6.0f));
			s_aMarker[3] = IGraphics::CLineItem(Target + vec2(-5.0f, -5.0f), Target + vec2(5.0f, 5.0f));
			s_aMarker[4] = IGraphics::CLineItem(Target + vec2(-5.0f, 5.0f), Target + vec2(5.0f, -5.0f));
			Graphics()->TextureClear();
			Graphics()->LinesBegin();
			Graphics()->SetColor(0.36f, 0.68f, 1.00f, 0.90f);
			Graphics()->LinesDraw(s_aMarker, 5);
			Graphics()->LinesEnd();
		}
	}

	// 3. The Blatant aimbot target.
	if(g_Config.m_BcAvoidDrawAimbot && m_Telemetry.m_AimTarget.m_Valid)
	{
		const vec2 Target = m_Telemetry.m_AimTarget.m_Pos;
		const vec2 Diff = Target - Core.m_Pos;
		if(length(Diff) > 1.0f)
		{
			static IGraphics::CLineItem s_aMarker[3];
			s_aMarker[0] = IGraphics::CLineItem(Core.m_Pos, Target);
			s_aMarker[1] = IGraphics::CLineItem(Target + vec2(-6.0f, 0.0f), Target + vec2(6.0f, 0.0f));
			s_aMarker[2] = IGraphics::CLineItem(Target + vec2(0.0f, -6.0f), Target + vec2(0.0f, 6.0f));
			Graphics()->TextureClear();
			Graphics()->LinesBegin();
			Graphics()->SetColor(0.98f, 0.62f, 0.16f, 0.90f);
			Graphics()->LinesDraw(s_aMarker, 3);
			Graphics()->LinesEnd();
		}
	}

	Graphics()->TextureClear();
	Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
	Graphics()->MapScreen(PreviousScreen);
}

// ---------------------------------------------------------------------------------------------
// Console commands
// ---------------------------------------------------------------------------------------------

void CAvoid::ConAvoidToggle(IConsole::IResult *pResult, void *pUserData)
{
	(void)pResult;
	CAvoid *pSelf = static_cast<CAvoid *>(pUserData);
	pSelf->ToggleEnabled();
	pSelf->GameClient()->Echo(pSelf->IsEnabled() ? BcLocalize("Avoid: enabled") : BcLocalize("Avoid: disabled"));
}

void CAvoid::ConAvoidStatus(IConsole::IResult *pResult, void *pUserData)
{
	(void)pResult;
	static_cast<CAvoid *>(pUserData)->PrintStatus();
}

void CAvoid::ConAvoidReset(IConsole::IResult *pResult, void *pUserData)
{
	(void)pResult;
	static_cast<CAvoid *>(pUserData)->ResetCounters();
}
