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
#include <game/client/components/bestclient/tas.h>
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
	m_DroveTick = -1;
	m_YieldTick = -1;
	m_YieldPending = false;
	m_LastGate = PRE_OK;
	m_LastActiveTime = time_get();
	m_LastPlayerInput = CNetObj_PlayerInput{};
	m_TileEditor.ClearAll();
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
	m_DroveTick = -1;
	m_YieldTick = -1;
	m_YieldPending = false;
	m_LastGate = PRE_OK;
	m_LastActiveTime = time_get();
	m_LastPlayerInput = CNetObj_PlayerInput{};
	// Edited tiles are map coordinates, so they cannot survive a map change.
	m_TileEditor.ClearAll();
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
	m_LastAimTargetValid = false;
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
	m_LastDecisionTick = -1;
	m_vLastPath.clear();
	m_LastOverrideActive = false;
	m_LastAimTargetValid = false;
	// Switching the bot on after it sat out an idle stretch must not trip AFK protection on the
	// very first tick: the clock starts again with an empty "last input", so the next sampled
	// input counts as a change and starts the idle measurement over.
	if(Enabled)
	{
		m_LastActiveTime = time_get();
		m_LastPlayerInput = CNetObj_PlayerInput{};
	}
	// The driving -> yielded latch is deliberately kept across a toggle: if the agent was driving
	// when it was switched off, the next tick still has to hand the player's own input back with a
	// packet, and the pending flag in FinishInput() is what makes that happen.
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

const char *CAvoid::PreActivationName(EPreActivation Gate)
{
	// These end up in the HUD "Plan:" row and in the console status, so they stay short and follow
	// the gate order of reference spec 5.
	switch(Gate)
	{
	case PRE_OK: return "ready";
	case PRE_GAMEMODE: return "gamemode blacklisted";
	case PRE_INACTIVE: return "player not active";
	case PRE_FROZEN: return "character frozen";
	case PRE_AFK: return "AFK protection";
	case PRE_SAFE: return "probe: player input safe";
	default: return "ready";
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

CCharacter *CAvoid::ActiveCharacter(int *pClientId) const
{
	int LocalId = -1;
	if(!ActiveCore(&LocalId))
		return nullptr;
	CGameWorld *pWorld = Avoid::GetActiveWorld(GameClient());
	if(!pWorld)
		return nullptr;
	if(pClientId)
		*pClientId = LocalId;
	return pWorld->GetCharacterById(LocalId);
}

// ---------------------------------------------------------------------------------------------
// Pre-activation pipeline (reference spec 5)
// ---------------------------------------------------------------------------------------------

bool CAvoid::IsGamemodeBlacklisted() const
{
	// Reference spec 5.1. The reference reads the string out of its game info snapshot; the DDNet
	// equivalent of that field is CGameClient::m_GameInfo.m_aGameType, which is the game type the
	// connected server announced.
	return Avoid::IsBlacklistedGametype(GameClient()->m_GameInfo.m_aGameType);
}

bool CAvoid::IsPlayerInactive() const
{
	// Reference spec 5.2: no valid local player, spectator, paused game or no live tee.
	int LocalClientId = -1;
	if(!ActiveCore(&LocalClientId))
		return true;

	// A Fast Practice sandbox runs on its own world with its own local tee, so the connection's
	// snapshot says nothing about whether that tee may act.
	if(!GameClient()->m_FastPractice.Active())
	{
		if(LocalClientId < 0 || LocalClientId >= MAX_CLIENTS)
			return true;
		if(GameClient()->m_Snap.m_SpecInfo.m_Active)
			return true;
		const CNetObj_PlayerInfo *pInfo = GameClient()->m_Snap.m_apPlayerInfos[LocalClientId];
		if(!pInfo || pInfo->m_Team == TEAM_SPECTATORS)
			return true;
		// Dead and waiting to respawn: there is no character to steer, and the reference blocks
		// this in the same gate.
		if(!GameClient()->m_Snap.m_aCharacters[LocalClientId].m_Active)
			return true;
	}

	const CNetObj_GameInfo *pGameInfo = GameClient()->m_Snap.m_pGameInfoObj;
	if(pGameInfo && (pGameInfo->m_GameStateFlags & GAMESTATEFLAG_PAUSED))
		return true;

	return false;
}

bool CAvoid::IsCharacterFrozen() const
{
	// Reference spec 5.3: a frozen tee cannot steer, so every tick of prediction spent on it is
	// wasted. The three flags are the ones the physics itself uses.
	CCharacter *pChar = ActiveCharacter(nullptr);
	if(!pChar)
		return true;
	return pChar->m_FreezeTime > 0 || pChar->m_FrozenLastTick || pChar->Core()->m_DeepFrozen;
}

void CAvoid::UpdateAfkTimer(const CNetObj_PlayerInput *pInput)
{
	// Reference spec 5.4: any change of the sampled hardware input counts as activity. The aim is
	// compared with a small tolerance so that a mouse that is left alone but reports a pixel of
	// jitter does not keep the bot awake forever.
	if(pInput->m_Direction != m_LastPlayerInput.m_Direction ||
		pInput->m_Jump != m_LastPlayerInput.m_Jump ||
		pInput->m_Fire != m_LastPlayerInput.m_Fire ||
		pInput->m_Hook != m_LastPlayerInput.m_Hook ||
		std::abs(pInput->m_TargetX - m_LastPlayerInput.m_TargetX) > 2 ||
		std::abs(pInput->m_TargetY - m_LastPlayerInput.m_TargetY) > 2)
	{
		m_LastActiveTime = time_get();
		m_LastPlayerInput = *pInput;
	}
}

bool CAvoid::IsAfk() const
{
	if(!g_Config.m_BcAvoidAfkProtection)
		return false;

	const int64_t Freq = time_freq();
	if(Freq <= 0)
		return false;

	return (time_get() - m_LastActiveTime) / Freq >= (int64_t)g_Config.m_BcAvoidAfkTime;
}

CAvoid::EPreActivation CAvoid::PreActivation() const
{
	// The order is the reference order (spec 5): it is also cheapest first, so a player who is
	// spectating or frozen never pays for anything below.
	if(IsGamemodeBlacklisted())
		return PRE_GAMEMODE;
	if(IsPlayerInactive())
		return PRE_INACTIVE;
	if(IsCharacterFrozen())
		return PRE_FROZEN;
	if(IsAfk())
		return PRE_AFK;
	return PRE_OK;
}

int CAvoid::RunLightweightProbe(CGameWorld *pWorld, const CNetObj_PlayerInput *pInput, const SSettings &Set) const
{
	// Reference spec 5.5, assembly 0x140312258 - 0x140312270. Simulating the player's own input for
	// ten ticks costs a fraction of one MCTS round, and it answers the only question that matters
	// when nothing is wrong: is the player about to be frozen within six ticks? If not, no agent is
	// woken at all, which is what keeps a safe player at full frame rate and zero twitching.
	return Avoid::RunLightweightProbe(GameClient(), pWorld, *pInput, Set);
}

CAvoid::EInputResult CAvoid::FinishInput(bool Drives, CNetObj_PlayerInput *pInput, int Tick)
{
	if(Drives)
	{
		m_DroveTick = Tick;
		m_YieldPending = true;
		if(!m_LastAimTargetValid)
		{
			m_LastOverride.m_TargetX = pInput->m_TargetX;
			m_LastOverride.m_TargetY = pInput->m_TargetY;
		}
		m_LastOverride.m_Jump = pInput->m_Jump;
		m_LastOverride.m_Fire = pInput->m_Fire;
		m_LastOverride.m_PlayerFlags = pInput->m_PlayerFlags;
		m_LastOverride.m_WantedWeapon = pInput->m_WantedWeapon;
		m_LastOverride.m_NextWeapon = pInput->m_NextWeapon;
		m_LastOverride.m_PrevWeapon = pInput->m_PrevWeapon;
		*pInput = m_LastOverride;
		return INPUT_DRIVEN;
	}

	// Handing the input back is not optional: the packet the agent forced on the driving tick
	// carried the agent's own input, and the sampler only asks for another one when the *sampled*
	// key state changes - it compares its own buffer against its own buffer and never notices the
	// rewrite. The handover is therefore latched until it has really been sent once. Testing
	// "this is the tick right after the intervention" instead missed the handover whenever the
	// predicted tick did not advance by exactly one between two calls (prediction time reset, a
	// lag spike, queued fast inputs), and the server then kept applying the agent's last input:
	// the tee walked on by itself.
	m_DroveTick = -1;
	if(m_YieldPending)
	{
		m_YieldPending = false;
		m_YieldTick = Tick;
		return INPUT_YIELDED;
	}
	return INPUT_IDLE;
}

CAvoid::EInputResult CAvoid::ApplyInput(CNetObj_PlayerInput *pInput)
{
	if(!pInput)
		return INPUT_IDLE;

	Avoid::SContext Ctx;
	Ctx.m_Tick = Client()->PredGameTick(g_Config.m_ClDummy);
	Ctx.m_Settings = ReadSettings();
	Ctx.m_Input = *pInput;
	Ctx.m_pTileEditor = &m_TileEditor;

	// [Gate 0] master switch and a live tee (reference spec 12.2, first check of ProcessInput).
	if(!IsEnabled() || !ActiveCore(&Ctx.m_LocalClientId))
	{
		m_Telemetry.m_State = STATE_OFF;
		m_Telemetry.m_Enabled = IsEnabled();
		m_Telemetry.m_Agent = Agent();
		m_Telemetry.m_SurvivalTicks = 0;
		m_Telemetry.m_CostMs = 0.0f;
		m_vLastPath.clear();
		m_LastOverrideActive = false;
		m_LastAimTargetValid = false;
		m_LastGate = PRE_OK;
		return FinishInput(false, pInput, Ctx.m_Tick);
	}

	// One decision per game tick. The client can ask for the same tick twice when it has to
	// re-send, so the decision is remembered and replayed instead of being recalculated (which
	// would advance the agents' search state twice for one tick).
	if(Ctx.m_Tick != m_LastDecisionTick)
	{
		m_LastDecisionTick = Ctx.m_Tick;

		// The AFK clock is fed every tick, before any gate reads it. It measures the sampled
		// hardware input, not what the previous tick happened to send.
		UpdateAfkTimer(pInput);

		Avoid::AvoidInput Action;
		Action.m_Input = Ctx.m_Input;
		const int64_t StartTime = time_get();

		// [Stage 1] The five environment gates. A blocked tick never reaches an agent, so the
		// player's input is passed through exactly as sampled.
		const EPreActivation PreviousGate = m_LastGate;
		const EPreActivation Gate = PreActivation();
		m_LastGate = Gate;
		if(Gate == PRE_AFK && PreviousGate != PRE_AFK)
		{
			// Announced once per idle stretch. The bot is not switched off: it stops steering while
			// the player is gone and takes over again with the next input.
			GameClient()->Echo(BcLocalize("Avoid: AFK protection paused the bot"));
		}
		if(Gate != PRE_OK)
		{
			str_copy(Action.m_aReason, PreActivationName(Gate));
		}
		else if(CGameWorld *pWorld = Avoid::GetActiveWorld(GameClient()))
		{
			const int AgentId = Ctx.m_Settings.m_Agent;
			Avoid::BLAgent *pAgent = (AgentId >= 0 && AgentId < NUM_AGENTS) ? m_apAgents[AgentId] : nullptr;

			// [Gate 5] The 10 tick baseline probe (assembly 0x140312258). Surviving at least
			// PROBE_SAFE_TICKS of the window means the player has margin, and the expensive
			// machinery below is skipped entirely - no MCTS, no clone storm, no micro twitching.
			//
			// The two planners are deliberately left out of this gate: Fentbot and Pilot navigate
			// by themselves and their search only advances while they are called, so probing them
			// would leave them without a plan exactly when one is needed. They keep their own
			// per-tick budget and their own plan guard, which is what stops them from steering an
			// input that is already safe. The three avoid agents are the ones this gate exists for.
			const bool ProbeGated = AgentId != AGENT_FENTBOT && AgentId != AGENT_PILOT;
			const int Probe = ProbeGated ? RunLightweightProbe(pWorld, pInput, Ctx.m_Settings) : 0;
			if(ProbeGated && Probe == Avoid::SIMULATION_SAFE_CONSTANT)
			{
				m_LastGate = PRE_SAFE;
				Action.m_SurvivalTicks = Avoid::PROBE_CHECK_TICKS;
				str_copy(Action.m_aReason, PreActivationName(PRE_SAFE));
			}
			else
			{
				// [Stage 2] Crosshair pre-processing (reference spec 5.6, 0x1403122d3). The sweep
				// runs before the agent and rewrites the aim the agent will work from; when it
				// accepts a ray, that aim is an intervention of its own.
				const int AimX = Action.m_Input.m_TargetX;
				const int AimY = Action.m_Input.m_TargetY;
				const bool Swept = Avoid::RunSectorScan(GameClient(), pWorld, &Action.m_Input,
					Ctx.m_Settings, &Action.m_AimTarget);
				const bool CrosshairLocked = Swept &&
							     (Action.m_Input.m_TargetX != AimX || Action.m_Input.m_TargetY != AimY);

				Avoid::SContext AgentCtx = Ctx;
				AgentCtx.m_Input = Action.m_Input;

				// [Stage 3] Dispatch to the selected agent.
				if(pAgent)
				{
					const Avoid::AvoidInput AgentAction = pAgent->GetAction(AgentCtx, pWorld);
					if(AgentAction.m_Active)
					{
						Action = AgentAction;
					}
					else
					{
						Action.m_SurvivalTicks = AgentAction.m_SurvivalTicks;
						Action.m_UsedFallback = AgentAction.m_UsedFallback;
						str_copy(Action.m_aReason, AgentAction.m_aReason);
						if(CrosshairLocked)
						{
							// Nothing else was worth doing, but the locked crosshair still has to
							// reach the server, so this tick counts as an intervention.
							Action.m_Active = 1;
							str_copy(Action.m_aReason, "sector scan locked the crosshair");
						}
					}
				}
				else
				{
					str_copy(Action.m_aReason, "agent unavailable");
				}
			}
		}
		else
		{
			str_copy(Action.m_aReason, "no world");
		}

		const float CostMs = (float)((double)(time_get() - StartTime) * 1000.0 / (double)time_freq());

		m_LastOverrideActive = Action.m_Active != 0;
		m_LastOverride = Action.m_Input;
		m_LastAimTargetValid = Action.m_Active != 0 && Action.m_AimTarget.m_Valid;
		if(Action.m_Active)
		{
			m_Telemetry.m_Overrides++;
			if(Action.m_UsedFallback)
				m_Telemetry.m_NsifFallbacks++;
		}
		m_Telemetry.m_Decisions++;

		m_vLastPath = Action.m_vPath;
		UpdateTelemetry(Action, CostMs);
		if(Gate == PRE_AFK)
			m_Telemetry.m_State = STATE_AFK;
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

	UpdateTileEditor();

	const int AgentId = Agent();
	if(AgentId >= 0 && AgentId < NUM_AGENTS && m_apAgents[AgentId])
		m_apAgents[AgentId]->OnRender();

	if(IsEnabled() && (g_Config.m_BcAvoidDrawPath || g_Config.m_BcAvoidDrawTrackPoint || g_Config.m_BcAvoidDrawAimbot))
	{
		const CCharacterCore *pCore = ActiveCore(nullptr);
		if(pCore)
			RenderWorldOverlay(*pCore);
	}

	// The editor overlay is independent of the master switch: tiles are usually painted before a
	// planner is ever enabled, and they are what the flow field is built from.
	if(g_Config.m_BcAvoidDrawPath)
		RenderTileEditorOverlay();

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
// Tile editor (reference spec 9)
// ---------------------------------------------------------------------------------------------

void CAvoid::UpdateTileEditor()
{
	CCollision *pCollision = Collision();
	if(!pCollision || pCollision->GetWidth() <= 0 || pCollision->GetHeight() <= 0)
		return;

	// One shot actions. They reset themselves, so a value that ended up in the config file cannot
	// replay the action on the next start.
	if(g_Config.m_BcAvoidTileEditorClear)
	{
		g_Config.m_BcAvoidTileEditorClear = 0;
		m_TileEditor.ClearAll();
		GameClient()->Echo(BcLocalize("Avoid tile editor: all edited tiles cleared"));
	}

	if(g_Config.m_BcAvoidTileEditorAutoFinish)
	{
		g_Config.m_BcAvoidTileEditorAutoFinish = 0;
		const int Found = m_TileEditor.AutoFinish(pCollision);
		char aBuf[128];
		str_format(aBuf, sizeof(aBuf), "%s: %d", BcLocalize("Avoid tile editor: finish tiles marked"), Found);
		GameClient()->Echo(aBuf);
	}

	if(g_Config.m_BcAvoidTileEditorAutoTunnel)
	{
		g_Config.m_BcAvoidTileEditorAutoTunnel = 0;
		std::vector<vec2> vTrajectory;
		const std::vector<CTas::STasTick> &vTicks = GameClient()->m_Tas.Ticks();
		vTrajectory.reserve(vTicks.size());
		for(const CTas::STasTick &Tick : vTicks)
			vTrajectory.push_back(Tick.m_Pos);

		const int Marked = m_TileEditor.AutoTunnels(vTrajectory,
			g_Config.m_BcAvoidTileEditorAutoTunnelWidth,
			pCollision->GetWidth(), pCollision->GetHeight());
		char aBuf[128];
		if(vTrajectory.empty())
			str_copy(aBuf, BcLocalize("Avoid tile editor: no TAS replay loaded"));
		else
			str_format(aBuf, sizeof(aBuf), "%s: %d", BcLocalize("Avoid tile editor: tunnel tiles marked"), Marked);
		GameClient()->Echo(aBuf);
	}

	// In world editing. The menu and the console own the cursor while they are open.
	if(!g_Config.m_BcAvoidTileEditorEnable)
		return;
	if(GameClient()->m_Menus.IsActive() || GameClient()->m_GameConsole.IsActive())
		return;

	const bool LeftClick = Input()->KeyIsPressed(KEY_MOUSE_1);
	const bool RightClick = Input()->KeyIsPressed(KEY_MOUSE_2);
	if(!LeftClick && !RightClick)
		return;

	m_TileEditor.Interact(pCollision->GetWidth(), pCollision->GetHeight(),
		GameClient()->m_Controls.m_aTargetPos[g_Config.m_ClDummy],
		LeftClick, RightClick, g_Config.m_BcAvoidTileEditorType);
}

void CAvoid::RenderTileEditorOverlay()
{
	CCollision *pCollision = Collision();
	if(!pCollision || pCollision->GetWidth() <= 0 || pCollision->GetHeight() <= 0)
		return;
	if(!m_TileEditor.HasTunnels() && !m_TileEditor.HasFinish())
		return;
	if(Graphics()->ScreenAspect() <= 0.0f || GameClient()->m_Camera.m_Zoom <= 0.0f)
		return;

	const CScreenRect PreviousScreen = Graphics()->GetScreen();
	const CScreenRect WorldScreen = Graphics()->MapScreenToWorld(
		GameClient()->m_Camera.m_Center.x,
		GameClient()->m_Camera.m_Center.y,
		100.0f, 100.0f, 100.0f, 0, 0,
		Graphics()->ScreenAspect(),
		GameClient()->m_Camera.m_Zoom);
	Graphics()->MapScreen(WorldScreen);

	// Only the tiles that can be on screen are built into the batch.
	const int MinX = std::max(0, (int)std::floor(WorldScreen.m_TopLeft.x / TILE_SIZE) - 1);
	const int MinY = std::max(0, (int)std::floor(WorldScreen.m_TopLeft.y / TILE_SIZE) - 1);
	const int MaxX = std::min(pCollision->GetWidth() - 1, (int)std::floor(WorldScreen.m_BottomRight.x / TILE_SIZE) + 1);
	const int MaxY = std::min(pCollision->GetHeight() - 1, (int)std::floor(WorldScreen.m_BottomRight.y / TILE_SIZE) + 1);

	static std::vector<IGraphics::CQuadItem> s_vTunnel;
	static std::vector<IGraphics::CQuadItem> s_vFinish;
	s_vTunnel.clear();
	s_vFinish.clear();
	for(int y = MinY; y <= MaxY; ++y)
	{
		for(int x = MinX; x <= MaxX; ++x)
		{
			const float TileX = (float)x * TILE_SIZE;
			const float TileY = (float)y * TILE_SIZE;
			if(m_TileEditor.IsFinish(x, y))
				s_vFinish.emplace_back(TileX + 2.0f, TileY + 2.0f, TILE_SIZE - 4.0f, TILE_SIZE - 4.0f);
			else if(m_TileEditor.IsTunnel(x, y))
				s_vTunnel.emplace_back(TileX + 4.0f, TileY + 4.0f, TILE_SIZE - 8.0f, TILE_SIZE - 8.0f);
		}
	}

	Graphics()->TextureClear();
	if(!s_vTunnel.empty())
	{
		Graphics()->QuadsBegin();
		Graphics()->SetColor(0.25f, 0.62f, 1.00f, 0.16f);
		Graphics()->QuadsDrawTL(s_vTunnel.data(), (int)s_vTunnel.size());
		Graphics()->QuadsEnd();
	}
	if(!s_vFinish.empty())
	{
		Graphics()->QuadsBegin();
		Graphics()->SetColor(0.35f, 0.95f, 0.45f, 0.22f);
		Graphics()->QuadsDrawTL(s_vFinish.data(), (int)s_vFinish.size());
		Graphics()->QuadsEnd();
	}

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
