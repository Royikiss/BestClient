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

	constexpr float HUD_BASE_WIDTH = 122.0f;
	constexpr float HUD_BASE_HEIGHT = 60.0f;
	constexpr float HUD_PADDING = 3.0f;
	constexpr float HUD_HEADER_HEIGHT = 9.0f;
	constexpr float HUD_ROW_HEIGHT = 7.5f;
	constexpr float HUD_FONT_HEADER = 5.5f;
	constexpr float HUD_FONT_ROW = 5.0f;
	constexpr float HUD_BADGE_WIDTH = 30.0f;
	constexpr float HUD_LABEL_WIDTH = 40.0f;

	ColorRGBA HazardColor(int Flags)
	{
		if(Flags & CAvoid::HAZ_DEATH)
			return ColorRGBA(1.00f, 0.22f, 0.24f, 1.0f);
		if(Flags & (CAvoid::HAZ_FREEZE | CAvoid::HAZ_DEEP | CAvoid::HAZ_LIVE))
			return ColorRGBA(0.36f, 0.68f, 1.00f, 1.0f);
		if(Flags & CAvoid::HAZ_UNFREEZE)
			return ColorRGBA(0.35f, 1.00f, 0.85f, 1.0f);
		if(Flags & CAvoid::HAZ_TELE)
			return ColorRGBA(0.80f, 0.45f, 1.00f, 1.0f);
		return ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);
	}

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

} // namespace

// ---------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------

CAvoid::CAvoid()
{
	for(int i = 0; i < NUM_AGENTS; ++i)
		m_apAgents[i] = nullptr;
}

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
	Console()->Register("avoid_toggle", "", CFGFLAG_CLIENT, ConAvoidToggle, this, "Arm or disarm the selected Avoid agent");
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
	m_LastThreat = SThreat{};
	m_LastPlan = SInputPlan{};
	m_LastDecisionTick = -1;
	m_LastInputTick = -1;
	m_IdleTicks = 0;
	for(int i = 0; i < NUM_AGENTS; ++i)
	{
		if(m_apAgents[i])
			m_apAgents[i]->OnReset();
	}
}

void CAvoid::OnMapLoad()
{
	m_LastThreat = SThreat{};
	m_LastPlan = SInputPlan{};
	m_LastDecisionTick = -1;
	m_IdleTicks = 0;
	for(int i = 0; i < NUM_AGENTS; ++i)
	{
		if(m_apAgents[i])
			m_apAgents[i]->OnReset();
	}
}

// ---------------------------------------------------------------------------------------------
// Configuration helpers
// ---------------------------------------------------------------------------------------------

int CAvoid::Agent() const
{
	return std::clamp(g_Config.m_BcAvoidAgent, 0, (int)NUM_AGENTS - 1);
}

const char *CAvoid::AgentName(int Agent) const
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
	g_Config.m_BcAvoidAgent = std::clamp(Agent, 0, (int)NUM_AGENTS - 1);
	m_LastPlan = SInputPlan{};
	m_LastDecisionTick = -1;
}

bool CAvoid::IsArmed() const
{
	return g_Config.m_BcAvoidEnabled && g_Config.m_BcAvoidActive;
}

bool CAvoid::WantsEveryTickInput() const
{
	return IsArmed();
}

void CAvoid::SetArmed(bool Armed)
{
	g_Config.m_BcAvoidActive = Armed ? 1 : 0;
	m_IdleTicks = 0;
	m_LastDecisionTick = -1;
	m_LastPlan = SInputPlan{};
	m_Telemetry.m_State = STATE_OFF;
}

void CAvoid::ToggleArmed()
{
	SetArmed(!IsArmed());
}

void CAvoid::ResetCounters()
{
	m_Telemetry.m_Decisions = 0;
	m_Telemetry.m_Overrides = 0;
	m_Telemetry.m_NsifFallbacks = 0;
	m_LastDecisionTick = -1;
	GameClient()->Echo("Avoid counters reset.");
}

void CAvoid::PrintStatus() const
{
	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "[Avoid] state: %s | agent: %s | armed: %s | threat: %s %.2f tiles | sensed: %d tiles (%d hazards) | safe: %d ticks | decisions: %d | overrides: %d | cost: %.3f ms",
		StateName(m_Telemetry.m_State), AgentName(m_Telemetry.m_Agent), m_Telemetry.m_Armed ? "yes" : "no",
		HazardName(m_Telemetry.m_ThreatFlags), m_Telemetry.m_ThreatDistanceTiles, m_Telemetry.m_SensedTiles,
		m_Telemetry.m_HazardTiles, m_Telemetry.m_SafeTicks, m_Telemetry.m_Decisions,
		m_Telemetry.m_Overrides, m_Telemetry.m_CostMs);
	GameClient()->Echo(aBuf);

	str_format(aBuf, sizeof(aBuf), "[Avoid] last plan: %s", m_Telemetry.m_aReason[0] ? m_Telemetry.m_aReason : "<none>");
	GameClient()->Echo(aBuf);

	if(g_Config.m_BcAvoidKickInTicks >= g_Config.m_BcAvoidCheckTicks)
		GameClient()->Echo(BcLocalize("warning: kick in ticks is not below check ticks, the agent will react late"));
}

CAvoid::SSettings CAvoid::ReadSettings() const
{
	SSettings S;
	S.m_Agent = Agent();
	S.m_DirectionAssist = g_Config.m_BcAvoidDirectionAssist != 0;
	S.m_HookAssist = g_Config.m_BcAvoidHookAssist != 0;
	S.m_CheckTicks = std::clamp(g_Config.m_BcAvoidCheckTicks, 2, 50);
	S.m_KickInTicks = std::clamp(g_Config.m_BcAvoidKickInTicks, 0, 50);
	S.m_Quality = std::clamp(g_Config.m_BcAvoidQuality, 1, 200);
	S.m_Randomness = std::clamp(g_Config.m_BcAvoidRandomness, 0, 200);
	S.m_DirectionWeight = std::clamp(g_Config.m_BcAvoidDirectionWeight, 0, 200);
	S.m_HookWeight = std::clamp(g_Config.m_BcAvoidHookWeight, 0, 200);
	S.m_LifeWeight = std::clamp(g_Config.m_BcAvoidLifeWeight, 0, 200);
	S.m_TileDeath = g_Config.m_BcAvoidTileDeath != 0;
	S.m_TileFreeze = g_Config.m_BcAvoidTileFreeze != 0;
	S.m_TileUnfreeze = g_Config.m_BcAvoidTileUnfreeze != 0;
	S.m_UnfreezeTicks = std::clamp(g_Config.m_BcAvoidUnfreezeTicks, 2, 50);
	S.m_TileTele = g_Config.m_BcAvoidTileTele != 0;
	S.m_PlayerPrediction = g_Config.m_BcAvoidPlayerPrediction != 0;
	S.m_Nsif = g_Config.m_BcAvoidNsif != 0;
	S.m_AfkProtect = g_Config.m_BcAvoidAfkProtect != 0;
	S.m_AfkTime = std::clamp(g_Config.m_BcAvoidAfkTime, 5, 600);
	S.m_TrackPoint = g_Config.m_BcAvoidTrackPoint != 0;
	S.m_SafeAimTracking = g_Config.m_BcAvoidSafeAimTracking != 0;
	S.m_AutoDrag = g_Config.m_BcAvoidAutoDrag != 0;
	S.m_Aimbot = g_Config.m_BcAvoidAimbot != 0;
	S.m_AimbotMode = std::clamp(g_Config.m_BcAvoidAimbotMode, 0, 1);
	S.m_AimbotSegments = std::clamp(g_Config.m_BcAvoidAimbotSegments, 4, 128);
	S.m_AimbotFov = std::clamp(g_Config.m_BcAvoidAimbotFov, 10, 180);
	S.m_SensingGate = false;
	S.m_SensingRadius = (float)std::clamp(g_Config.m_BcAvoidSensingRadius, 1, 32) * 0.5f;
	return S;
}

const char *CAvoid::StateName(int State)
{
	switch(State)
	{
	case STATE_OFF: return "OFF";
	case STATE_WATCHING: return "WATCH";
	case STATE_ASSISTING: return "ASSIST";
	case STATE_NSIF: return "NSIF";
	case STATE_AFK: return "AFK";
	default: return "OFF";
	}
}

const char *CAvoid::HazardName(int Flags)
{
	if(Flags & HAZ_DEATH)
		return "DEATH";
	if(Flags & HAZ_DEEP)
		return "DEEP FREEZE";
	if(Flags & HAZ_LIVE)
		return "LIVE FREEZE";
	if(Flags & HAZ_FREEZE)
		return "FREEZE";
	if(Flags & HAZ_UNFREEZE)
		return "UNFREEZE";
	if(Flags & HAZ_TELE)
		return "TELEPORT";
	if(Flags & HAZ_SELF)
		return "FROZEN";
	return "CLEAR";
}

// ---------------------------------------------------------------------------------------------
// Sensing layer
// ---------------------------------------------------------------------------------------------

int CAvoid::ClassifyTile(int Tile) const
{
	return Avoid::ClassifyTile(Tile);
}

int CAvoid::HazardMaskFromSettings() const
{
	return Avoid::HazardMask(m_Settings);
}

bool CAvoid::IsRelevantHazard(int Flags) const
{
	return Avoid::IsRelevantHazard(m_Settings, Flags);
}

int CAvoid::ClassifyPoint(vec2 Pos) const
{
	return Avoid::ClassifyPoint(Collision(), Pos);
}

CAvoid::SThreat CAvoid::ScanThreat(const CCharacterCore &Core) const
{
	return Avoid::ScanThreat(Collision(), m_Settings, Core);
}

// ---------------------------------------------------------------------------------------------
// Input pipeline
// ---------------------------------------------------------------------------------------------

const CCharacterCore *CAvoid::ActiveCore(int *pClientId, bool *pIsDummy) const
{
	if(pIsDummy)
		*pIsDummy = g_Config.m_ClDummy != 0;

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

	const int ClientId = GameClient()->m_aLocalIds[g_Config.m_ClDummy];
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return nullptr;
	if(!GameClient()->m_aClients[ClientId].m_Active)
		return nullptr;

	if(pClientId)
		*pClientId = ClientId;
	return &GameClient()->m_aClients[ClientId].m_Predicted;
}

vec2 CAvoid::OverlayAnchor(const CCharacterCore &Core) const
{
	const int LocalId = GameClient()->m_aLocalIds[g_Config.m_ClDummy];
	if(LocalId >= 0 && LocalId < MAX_CLIENTS && GameClient()->m_aClients[LocalId].m_Active)
	{
		const vec2 RenderPos = GameClient()->m_LocalCharacterPos;
		if(distance(RenderPos, Core.m_Pos) <= 128.0f)
			return RenderPos;
	}
	return Core.m_Pos;
}

void CAvoid::CheckAfkProtection(const SContext &Ctx)
{
	const bool ActiveInput = Ctx.m_Input.m_Direction != 0 || Ctx.m_Input.m_Jump != 0 ||
				 Ctx.m_Input.m_Hook != 0 || (Ctx.m_Input.m_Fire & 1) != 0;
	if(ActiveInput)
		m_IdleTicks = 0;
	else if(m_IdleTicks < 50 * 600)
		m_IdleTicks++;

	if(!m_Settings.m_AfkProtect || !IsArmed())
		return;

	if(m_IdleTicks >= 50 * m_Settings.m_AfkTime)
	{
		SetArmed(false);
		m_Telemetry.m_State = STATE_AFK;
		str_copy(m_Telemetry.m_aReason, "AFK protection disarmed the agent");
		if(g_Config.m_BcAvoidLog)
			Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "avoid", "AFK protection disarmed the agent.");
	}
}

void CAvoid::ApplyInput(int *pData, int Size, bool Dummy)
{
	if(!pData || Size < (int)sizeof(CNetObj_PlayerInput))
		return;

	m_Settings = ReadSettings();

	SContext Ctx;
	Ctx.m_Tick = Client()->PredGameTick(g_Config.m_ClDummy);
	Ctx.m_Dummy = Dummy;
	Ctx.m_Settings = m_Settings;
	mem_copy(&Ctx.m_Input, pData, sizeof(CNetObj_PlayerInput));

	const CCharacterCore *pCore = ActiveCore(&Ctx.m_ClientId, &Ctx.m_Dummy);
	if(!pCore)
	{
		m_Telemetry.m_State = STATE_OFF;
		m_Telemetry.m_Armed = IsArmed();
		m_Telemetry.m_Agent = Agent();
		m_Telemetry.m_ThreatFlags = HAZ_NONE;
		m_Telemetry.m_ThreatDistanceTiles = -1.0f;
		m_Telemetry.m_SensedTiles = 0;
		m_Telemetry.m_HazardTiles = 0;
		return;
	}

	Ctx.m_Core = *pCore;
	Ctx.m_Threat = ScanThreat(Ctx.m_Core);
	m_LastThreat = Ctx.m_Threat;

	if(!g_Config.m_BcAvoidEnabled)
	{
		m_LastPlan = SInputPlan{};
		UpdateTelemetry(Ctx, m_LastPlan);
		return;
	}

	CheckAfkProtection(Ctx);

	SInputPlan Plan = m_LastPlan;
	if(IsArmed() && Ctx.m_Tick != m_LastDecisionTick)
	{
		m_LastDecisionTick = Ctx.m_Tick;
		const int64_t StartTime = time_get();

		if(g_Config.m_BcAvoidDebugOverride)
		{
			Plan.m_Override = true;
			Plan.m_UsedFallback = false;
			Plan.m_Input = Ctx.m_Input;
			Plan.m_Input.m_Direction = -Ctx.m_Input.m_Direction;
			Plan.m_SafeTicks = 0;
			Plan.m_CostMs = 0.0f;
			str_copy(Plan.m_aReason, BcLocalize("debug override (input pipeline self-test)"));
		}
		else
		{
			Avoid::BLAgent *pAgent = (Agent() >= 0 && Agent() < NUM_AGENTS) ? m_apAgents[Agent()] : nullptr;
			Avoid::AvoidInput Action{};
			if(pAgent)
				Action = pAgent->GetAction(&Ctx.m_Input);

			const float CostMs = (float)((time_get() - StartTime) * 1000.0 / (double)time_freq());

			Plan.m_Override = (Action.m_Active != 0);
			Plan.m_UsedFallback = Action.m_UsedFallback;
			Plan.m_Input = Action.m_Input;
			Plan.m_SafeTicks = Action.m_SurvivalTicks;
			Plan.m_CostMs = CostMs;
			str_copy(Plan.m_aReason, Action.m_aReason);
		}

		m_LastPlan = Plan;
		m_Telemetry.m_Decisions++;
		if(Plan.m_Override)
			m_Telemetry.m_Overrides++;
		if(Plan.m_UsedFallback)
			m_Telemetry.m_NsifFallbacks++;

		if(g_Config.m_BcAvoidLog)
		{
			char aBuf[320];
			str_format(aBuf, sizeof(aBuf), "[avoid] tick %d  agent %s  override %s  safe %d  cost %.3f ms  reason %s",
				Ctx.m_Tick, AgentName(Agent()), Plan.m_Override ? "yes" : "no", Plan.m_SafeTicks, Plan.m_CostMs, Plan.m_aReason);
			Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "avoid", aBuf);
		}
	}
	else if(!IsArmed())
	{
		m_LastPlan = SInputPlan{};
		Plan = m_LastPlan;
	}

	if(Plan.m_Override)
	{
		mem_copy(pData, &Plan.m_Input, sizeof(CNetObj_PlayerInput));
	}

	UpdateTelemetry(Ctx, Plan);
}

void CAvoid::UpdateTelemetry(const SContext &Ctx, const SInputPlan &Plan)
{
	m_Telemetry.m_Armed = IsArmed();
	m_Telemetry.m_Agent = Agent();
	m_Telemetry.m_ThreatFlags = Ctx.m_Threat.m_Flags;
	m_Telemetry.m_ThreatDistanceTiles = Ctx.m_Threat.m_HasNearest ? Ctx.m_Threat.m_NearestDistPx / TILE_SIZE : -1.0f;
	m_Telemetry.m_ThreatPos = Ctx.m_Threat.m_NearestPos;
	m_Telemetry.m_SensedTiles = Ctx.m_Threat.m_SensedTiles;
	m_Telemetry.m_HazardTiles = Ctx.m_Threat.m_HazardTiles;
	m_Telemetry.m_PlayerPos = Ctx.m_Core.m_Pos;
	m_Telemetry.m_PlayerVel = Ctx.m_Core.m_Vel;
	m_Telemetry.m_SafeTicks = Plan.m_SafeTicks;
	m_Telemetry.m_Candidates = Plan.m_Candidates;
	m_Telemetry.m_CostMs = Plan.m_CostMs;
	m_Telemetry.m_AimChanged = Plan.m_Override &&
				   (Plan.m_Input.m_TargetX != Ctx.m_Input.m_TargetX ||
				    Plan.m_Input.m_TargetY != Ctx.m_Input.m_TargetY);
	m_Telemetry.m_AimTargetX = Plan.m_Input.m_TargetX;
	m_Telemetry.m_AimTargetY = Plan.m_Input.m_TargetY;
	str_copy(m_Telemetry.m_aReason, Plan.m_aReason);

	if(!g_Config.m_BcAvoidEnabled)
	{
		m_Telemetry.m_State = STATE_OFF;
		return;
	}
	if(!IsArmed())
	{
		m_Telemetry.m_State = (m_Telemetry.m_State == STATE_AFK) ? STATE_AFK : STATE_OFF;
		return;
	}
	if(Plan.m_Override)
		m_Telemetry.m_State = Plan.m_UsedFallback ? STATE_NSIF : STATE_ASSISTING;
	else
		m_Telemetry.m_State = STATE_WATCHING;
}

// ---------------------------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------------------------

void CAvoid::OnRender()
{
	if(Client()->State() != IClient::STATE_ONLINE)
		return;
	if(!g_Config.m_BcAvoidEnabled)
		return;
	if(GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_aLocalIds[g_Config.m_ClDummy] < 0)
		return;

	if(Agent() >= 0 && Agent() < NUM_AGENTS && m_apAgents[Agent()])
		m_apAgents[Agent()]->OnRender();

	if(g_Config.m_BcAvoidShowVisuals)
	{
		int ClientId = -1;
		bool IsDummy = false;
		if(const CCharacterCore *pCore = ActiveCore(&ClientId, &IsDummy))
			RenderWorldOverlay(*pCore, m_LastThreat);
	}

	if(IsHudVisible())
		RenderHudModule(false);
}

bool CAvoid::IsHudVisible() const
{
	return g_Config.m_BcAvoidShowHud != 0 && HudLayout::IsEnabled(HudLayout::MODULE_AVOID);
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
	Canvas.Draw(ColorRGBA(0.06f, 0.08f, 0.12f, 0.85f * Alpha), IGraphics::CORNER_ALL, 4.0f * Scale);

	CUIRect Header, Content;
	Canvas.Margin(HUD_PADDING * Scale, &Content);
	Content.HSplitTop(HUD_HEADER_HEIGHT * Scale, &Header, &Content);

	CUIRect Badge, TitleRect;
	Header.VSplitRight(HUD_BADGE_WIDTH * Scale, &TitleRect, &Badge);

	TextRender()->TextColor(1.0f, 1.0f, 1.0f, Alpha);
	char aTitle[48];
	str_format(aTitle, sizeof(aTitle), "AVOID: %s", AgentName(ForcePreview ? AGENT_BASIC : m_Telemetry.m_Agent));
	Ui()->DoLabel(&TitleRect, aTitle, HUD_FONT_HEADER * Scale, TEXTALIGN_ML);

	const int State = ForcePreview ? STATE_WATCHING : m_Telemetry.m_State;
	ColorRGBA BCol = StateColor(State);
	BCol.a *= Alpha;
	Badge.Draw(BCol, IGraphics::CORNER_ALL, 2.0f * Scale);
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

	char aThreatBuf[48];
	if(ForcePreview)
		str_copy(aThreatBuf, "CLEAR (6.0 t)");
	else if(m_Telemetry.m_ThreatDistanceTiles >= 0.0f)
		str_format(aThreatBuf, sizeof(aThreatBuf), "%s (%.1f t)", HazardName(m_Telemetry.m_ThreatFlags), m_Telemetry.m_ThreatDistanceTiles);
	else
		str_copy(aThreatBuf, "CLEAR");
	DrawRow(Content, BcLocalize("Threat:"), aThreatBuf, HazardColor(ForcePreview ? HAZ_NONE : m_Telemetry.m_ThreatFlags));

	char aSafeBuf[48];
	if(ForcePreview)
		str_copy(aSafeBuf, "26 tick (9999)");
	else
		str_format(aSafeBuf, sizeof(aSafeBuf), "%d tick (%.2f ms)", m_Telemetry.m_SafeTicks, m_Telemetry.m_CostMs);
	DrawRow(Content, BcLocalize("Safe:"), aSafeBuf, ColorRGBA(0.85f, 0.90f, 0.98f, 1.0f));

	char aTakesBuf[48];
	if(ForcePreview)
		str_copy(aTakesBuf, "0 / 0");
	else
		str_format(aTakesBuf, sizeof(aTakesBuf), "%d / %d", m_Telemetry.m_Overrides, m_Telemetry.m_Decisions);
	DrawRow(Content, BcLocalize("Override:"), aTakesBuf, ColorRGBA(0.85f, 0.90f, 0.98f, 1.0f));

	char aPlanBuf[64];
	if(ForcePreview)
		str_copy(aPlanBuf, "player input safe");
	else
		str_copy(aPlanBuf, m_Telemetry.m_aReason[0] ? m_Telemetry.m_aReason : "<none>");
	DrawRow(Content, BcLocalize("Plan:"), aPlanBuf, ColorRGBA(0.95f, 0.85f, 0.45f, 1.0f));

	TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
}

void CAvoid::RenderWorldOverlay(const CCharacterCore &Core, const SThreat &Threat)
{
	if(!Collision() || Collision()->GetWidth() <= 0 || Collision()->GetHeight() <= 0)
		return;

	const vec2 Pos = OverlayAnchor(Core);

	const CScreenRect PreviousScreen = Graphics()->GetScreen();
	const CScreenRect WorldScreen = Graphics()->MapScreenToWorld(
		GameClient()->m_Camera.m_Center.x,
		GameClient()->m_Camera.m_Center.y,
		100.0f, 100.0f, 100.0f, 0, 0,
		Graphics()->ScreenAspect(),
		GameClient()->m_Camera.m_Zoom);
	Graphics()->MapScreen(WorldScreen);

	const float RadiusX = std::max(1.0f, m_Settings.m_SensingRadius);
	const float RadiusY = std::max(1.0f, RadiusX * Avoid::SensingVerticalFactor(Core, m_Settings));
	const float ReachX = RadiusX * TILE_SIZE;
	const float ReachY = RadiusY * TILE_SIZE;
	const int ScanX = (int)std::ceil(RadiusX);
	const int ScanY = (int)std::ceil(RadiusY);
	const int CenterX = (int)std::floor(Core.m_Pos.x / TILE_SIZE);
	const int CenterY = (int)std::floor(Core.m_Pos.y / TILE_SIZE);

	const int MinTileX = std::clamp(CenterX - ScanX, 0, Collision()->GetWidth() - 1);
	const int MaxTileX = std::clamp(CenterX + ScanX, 0, Collision()->GetWidth() - 1);
	const int MinTileY = std::clamp(CenterY - ScanY, 0, Collision()->GetHeight() - 1);
	const int MaxTileY = std::clamp(CenterY + ScanY, 0, Collision()->GetHeight() - 1);

	Graphics()->TextureClear();
	Graphics()->QuadsBegin();
	for(int Ty = MinTileY; Ty <= MaxTileY; ++Ty)
	{
		for(int Tx = MinTileX; Tx <= MaxTileX; ++Tx)
		{
			const vec2 TileCenter((Tx + 0.5f) * TILE_SIZE, (Ty + 0.5f) * TILE_SIZE);
			const int Flags = ClassifyPoint(TileCenter);
			if(Flags == HAZ_NONE || !IsRelevantHazard(Flags))
				continue;
			const vec2 Delta = Avoid::TileBoxDelta(Pos, Tx, Ty);
			if((Delta.x / ReachX) * (Delta.x / ReachX) + (Delta.y / ReachY) * (Delta.y / ReachY) > 1.0f)
				continue;

			ColorRGBA Col = HazardColor(Flags);
			Col.a = 0.20f;
			Graphics()->SetColor(Col);
			const IGraphics::CQuadItem Quad(Tx * TILE_SIZE + 2.0f, Ty * TILE_SIZE + 2.0f, TILE_SIZE - 4.0f, TILE_SIZE - 4.0f);
			Graphics()->QuadsDrawTL(&Quad, 1);
		}
	}
	Graphics()->QuadsEnd();

	const int NumSegments = 64;
	static IGraphics::CLineItem s_aRing[NumSegments];
	static IGraphics::CLineItem s_aVector[2];
	const float RingRadius = ReachX;
	for(int i = 0; i < NumSegments; ++i)
	{
		const float A0 = 2.0f * pi * (float)i / (float)NumSegments;
		const float A1 = 2.0f * pi * (float)(i + 1) / (float)NumSegments;
		s_aRing[i] = IGraphics::CLineItem(Pos + vec2(std::cos(A0) * RingRadius, std::sin(A0) * ReachY),
			Pos + vec2(std::cos(A1) * RingRadius, std::sin(A1) * ReachY));
	}
	Graphics()->TextureClear();
	Graphics()->LinesBegin();
	Graphics()->SetColor(0.45f, 0.65f, 0.95f, 0.35f);
	Graphics()->LinesDraw(s_aRing, NumSegments);
	Graphics()->LinesEnd();

	if(Threat.m_HasNearest)
	{
		s_aVector[0] = IGraphics::CLineItem(Pos, Threat.m_NearestPos);
		s_aVector[1] = IGraphics::CLineItem(Threat.m_NearestPos, Threat.m_NearestPos + vec2(0.0f, -6.0f));
		Graphics()->TextureClear();
		Graphics()->LinesBegin();
		Graphics()->SetColor(HazardColor(Threat.m_Flags));
		Graphics()->LinesDraw(s_aVector, 2);
		Graphics()->LinesEnd();
	}

	// Track point overlay for Blatant
	Avoid::CBlatantAgent *pBlatant = (Agent() == AGENT_BLATANT) ? static_cast<Avoid::CBlatantAgent *>(m_apAgents[AGENT_BLATANT]) : nullptr;
	if(pBlatant && pBlatant->TrackPointValid())
	{
		const vec2 TrackDiff = pBlatant->TrackPointPos() - Pos;
		if(length(TrackDiff) > 1.0f)
		{
			const vec2 TrackEnd = Pos + normalize(TrackDiff) * 48.0f;
			static IGraphics::CLineItem s_aAimLines[3];
			s_aAimLines[0] = IGraphics::CLineItem(Pos, TrackEnd);
			s_aAimLines[1] = IGraphics::CLineItem(TrackEnd, TrackEnd + vec2(0.0f, -5.0f));
			s_aAimLines[2] = IGraphics::CLineItem(TrackEnd, TrackEnd + vec2(-5.0f, 0.0f));
			Graphics()->TextureClear();
			Graphics()->LinesBegin();
			Graphics()->SetColor(0.36f, 0.68f, 1.00f, 0.85f);
			Graphics()->LinesDraw(s_aAimLines, 3);
			Graphics()->LinesEnd();
		}
	}

	if(m_Telemetry.m_AimChanged)
	{
		const vec2 AimRaw = vec2((float)m_Telemetry.m_AimTargetX, (float)m_Telemetry.m_AimTargetY);
		if(length(AimRaw) > 0.001f)
		{
			const vec2 AimDir = normalize(AimRaw);
			const vec2 AimEnd = Pos + AimDir * 80.0f;
			static IGraphics::CLineItem s_aAimLines[3];
			s_aAimLines[0] = IGraphics::CLineItem(Pos, AimEnd);
			s_aAimLines[1] = IGraphics::CLineItem(AimEnd, AimEnd + vec2(-6.0f, -3.0f));
			s_aAimLines[2] = IGraphics::CLineItem(AimEnd, AimEnd + vec2(6.0f, -3.0f));
			Graphics()->TextureClear();
			Graphics()->LinesBegin();
			Graphics()->SetColor(0.98f, 0.62f, 0.16f, 0.9f);
			Graphics()->LinesDraw(s_aAimLines, 3);
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
	pSelf->ToggleArmed();
	pSelf->GameClient()->Echo(pSelf->IsArmed() ? "Avoid agent armed." : "Avoid agent disarmed.");
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
