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
	// The tile size the HUD, the overlay and the telemetry work in.
	constexpr float TILE_SIZE = 32.0f;

	// In-game HUD panel geometry, expressed in the 300 units tall HUD canvas that every
	// HudLayout module lives in (see HudLayout::CANVAS_HEIGHT).
	constexpr float HUD_BASE_WIDTH = 122.0f;
	constexpr float HUD_BASE_HEIGHT = 60.0f;
	constexpr float HUD_PADDING = 3.0f;
	constexpr float HUD_HEADER_HEIGHT = 9.0f;
	constexpr float HUD_ROW_HEIGHT = 7.5f;
	constexpr float HUD_FONT_HEADER = 5.5f;
	constexpr float HUD_FONT_ROW = 5.0f;
	constexpr float HUD_BADGE_WIDTH = 30.0f;
	constexpr float HUD_LABEL_WIDTH = 40.0f;

	// Colour palette shared by the HUD and the world overlay.
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

void CAvoid::OnConsoleInit()
{
	Console()->Register("avoid_toggle", "", CFGFLAG_CLIENT, ConAvoidToggle, this, "Arm or disarm the selected Avoid agent");
	Console()->Register("avoid_status", "", CFGFLAG_CLIENT, ConAvoidStatus, this, "Print the current Avoid state to the console");
	Console()->Register("avoid_reset", "", CFGFLAG_CLIENT, ConAvoidReset, this, "Reset the Avoid telemetry counters");
}

void CAvoid::OnReset()
{
	m_Telemetry = STelemetry{};
	m_LastThreat = SThreat{};
	m_LastPlan = SInputPlan{};
	m_LastDecisionTick = -1;
	m_LastInputTick = -1;
	m_IdleTicks = 0;
}

void CAvoid::OnMapLoad()
{
	m_LastThreat = SThreat{};
	m_LastPlan = SInputPlan{};
	m_LastDecisionTick = -1;
	m_IdleTicks = 0;
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
	// Agent names follow the client language, so they are ordinary translation entries.
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
	// The client normally throttles an unchanged input down to 25 Hz. An armed agent must be able
	// to act on any tick, so ask CControls::SnapInput() to keep the full 50 Hz cadence.
	return IsArmed();
}

void CAvoid::SetArmed(bool Armed)
{
	g_Config.m_BcAvoidActive = Armed ? 1 : 0;
	m_IdleTicks = 0;
	m_LastDecisionTick = -1;
	m_LastPlan = SInputPlan{};
	// A manual arm/disarm clears a previous AFK notice. CheckAfkProtection() sets the AFK state
	// *after* calling SetArmed(false), so the automatic path keeps reporting STATE_AFK.
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

	// Configuration trap from the delivery document (appendix F.4): with `kick_in_ticks` at or
	// above `check_ticks` the agent can only ever act once the tee is already lost.
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
	// Stored in half tiles so the slider can go down to 0.5; the engine works in tiles.
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
// Sensing layer (stage 1)
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
	// The probe rules live in avoid_engine.cpp: one implementation, used by the HUD readout, the
	// decision engine and the unit tests. A second copy here is exactly the drift this module was
	// built to avoid.
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

	// The TAS local sandbox owns the physics while it is running, so sense there instead.
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
	// `m_LocalCharacterPos` is written together with the render position of the tee the player
	// controls (inside the TAS practice sandbox as well), so it is exactly where the sprite ends
	// up on screen. The distance guard only protects against a stale anchor right after a
	// respawn or a teleport, where the smoothed position has not caught up yet.
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
	// "Idle" means: no direction, no jump, no hook and no fire for a whole second.
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
		// Keep the counters; only the live readouts become meaningless without a character.
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
		Plan = EvaluateBestPlan(Ctx);
		m_LastPlan = Plan;
		m_Telemetry.m_Decisions++;
		if(Plan.m_Override)
			m_Telemetry.m_Overrides++;
		if(Plan.m_UsedFallback)
			m_Telemetry.m_NsifFallbacks++;
		if(g_Config.m_BcAvoidLog)
			LogTrace(Plan.m_aReason, Ctx, Plan.m_SafeTicks, Ctx.m_Settings.m_CheckTicks, Plan);
	}
	else if(!IsArmed())
	{
		m_LastPlan = SInputPlan{};
		Plan = m_LastPlan;
	}

	if(Plan.m_Override)
	{
		// pData points straight into CClient's input ring, so this single write feeds both
		// the network packet and the local prediction of this tick. `m_Controls.m_aInputData`
		// is deliberately left untouched: it keeps the raw key state the user is holding.
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
// Decision engine: stage 2 - Basic agent (forward simulator + direction braking)
// ---------------------------------------------------------------------------------------------

Avoid::SEnvironment CAvoid::BuildEnvironment(const SContext &Ctx, bool WithPlayers) const
{
	Avoid::SEnvironment Env;
	Env.m_pCollision = Collision();
	Env.m_LocalId = (Ctx.m_ClientId >= 0 && Ctx.m_ClientId < MAX_CLIENTS) ? Ctx.m_ClientId : -1;

	// Inside the TAS practice sandbox the active world is the sandbox, not the live one.
	if(GameClient()->m_FastPractice.Active())
	{
		Env.m_pWorld = &GameClient()->m_FastPractice.PracticeWorld().m_Core;
		Env.m_pTeams = GameClient()->m_FastPractice.PracticeWorld().Teams();
	}
	else
	{
		Env.m_pWorld = &GameClient()->m_GameWorld.m_Core;
		Env.m_pTeams = &GameClient()->m_Teams;
	}

	if(!WithPlayers || !Env.m_pTeams)
		return Env;

	// Player prediction (`bc_avoid_player_prediction`): freeze the nearest other tees into a small
	// list of snapshots that the clone world carries as moving obstacles. Only the ones close
	// enough to be reachable inside the lookahead are worth the simulation cost; the list is
	// capped by MAX_SHADOW_PLAYERS, so a crowded server degrades instead of stalling.
	const float MaxDistance = (Ctx.m_Settings.m_SensingRadius + 2.0f) * TILE_SIZE;
	bool aTaken[MAX_CLIENTS] = {};
	for(int Slot = 0; Slot < Avoid::MAX_SHADOW_PLAYERS; Slot++)
	{
		int BestId = -1;
		float BestDistance = MaxDistance;
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(aTaken[i] || i == Ctx.m_ClientId || !GameClient()->m_aClients[i].m_Active || GameClient()->m_aClients[i].m_Spec)
				continue;
			const float Distance = distance(GameClient()->m_aClients[i].m_Predicted.m_Pos, Ctx.m_Core.m_Pos);
			if(Distance <= BestDistance)
			{
				BestDistance = Distance;
				BestId = i;
			}
		}
		if(BestId < 0)
			break;
		aTaken[BestId] = true;
		Env.m_aPlayers[Env.m_NumPlayers].m_Id = BestId;
		Env.m_aPlayers[Env.m_NumPlayers].m_Core = GameClient()->m_aClients[BestId].m_Predicted;
		Env.m_NumPlayers++;
	}
	Env.m_PredictPlayers = true;
	return Env;
}

int CAvoid::FlyHammerState(const SContext &Ctx) const
{
	// Deep fly / hammer fly (TAS document 3.11.7) are two player flights in which the movement key
	// is not a walking key. The client side markers are the dummy connection and `cl_dummy_hammer`
	// (the HDF toggle, which the DF bind also sets while firing) plus the tee being airborne - on
	// the ground the pair is not flying and the agent may still brake normally.
	if(!Client()->DummyConnected() || !g_Config.m_ClDummyHammer)
		return Avoid::MOVE_NORMAL;
	if(Collision() && Collision()->IsOnGround(Ctx.m_Core.m_Pos, CCharacterCore::PhysicalSize()))
		return Avoid::MOVE_NORMAL;
	return Avoid::MOVE_FLY_HAMMER;
}

int CAvoid::SimulateInput(const SContext &Ctx, const CNetObj_PlayerInput &Input, int MaxTicks,
	vec2 *pOutPos, vec2 *pOutVel)
{
	// The recipe itself lives in avoid_engine.cpp (Avoid::SimulateFixed) so that the Basic agent,
	// the Legit agent and the unit tests run the exact same physics. Basic never predicts other
	// players, so it keeps the stage 2 configuration: the live world, no deferred tick.
	const Avoid::SEnvironment Env = BuildEnvironment(Ctx, false);
	return Avoid::SimulateFixed(Ctx, Input, MaxTicks, &Env, pOutPos, pOutVel);
}

CAvoid::SInputPlan CAvoid::PlanLegit(const SContext &Ctx)
{
	const Avoid::SEnvironment Env = BuildEnvironment(Ctx, Ctx.m_Settings.m_PlayerPrediction);
	return m_Planner.Plan(Ctx, Env);
}

void CAvoid::LogTrace(const char *pTag, const SContext &Ctx, int SafeTicks, int Limit, const SInputPlan &Plan) const
{
	// `bc_avoid_log 1` prints one line per decision: the player's phase space, the hazard verdict
	// at the tee, how long the player's own input survives and what the agent answered. Walking
	// the tee along a wall of death tiles and watching the `safe` column drop is the calibration
	// procedure for the simulator (see the delivery document, 6.6 step 1).
	char aBuf[320];
	str_format(aBuf, sizeof(aBuf), "[avoid] tick %d  pos (%.1f, %.1f) tiles  vel (%.1f, %.1f)  threat %s  hazard %s  agent %s  player safe %d/%d  override %s  safe %d  plans %d  cost %.3f ms  plan d/j/h %d/%d/%d  reason %s",
		Ctx.m_Tick, Ctx.m_Core.m_Pos.x / TILE_SIZE, Ctx.m_Core.m_Pos.y / TILE_SIZE,
		Ctx.m_Core.m_Vel.x, Ctx.m_Core.m_Vel.y,
		Ctx.m_Threat.m_HasNearest ? HazardName(Ctx.m_Threat.m_Flags) : "CLEAR",
		Ctx.m_Threat.m_OnHazard ? "yes" : "no",
		AgentName(Ctx.m_Settings.m_Agent),
		SafeTicks, Limit, Plan.m_Override ? "yes" : "no", Plan.m_SafeTicks, Plan.m_Candidates,
		Plan.m_CostMs, Plan.m_Input.m_Direction, Plan.m_Input.m_Jump ? 1 : 0, Plan.m_Input.m_Hook ? 1 : 0, pTag);
	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "avoid", aBuf);
}

CAvoid::SInputPlan CAvoid::EvaluateBestPlan(const SContext &Ctx)
{
	SInputPlan Plan;
	Plan.m_Input = Ctx.m_Input;

	// --------------------------------------------------------------------------------------------
	// Stage 1 input pipeline self-test (`bc_avoid_debug_override`).
	//
	// It takes over every tick and inverts the left/right input. Hold D and the tee walks left, on
	// your screen and on the server, because `pData` is the buffer the prediction and the network
	// packet are both built from.
	// --------------------------------------------------------------------------------------------
	if(g_Config.m_BcAvoidDebugOverride)
	{
		Plan.m_Override = true;
		Plan.m_UsedFallback = false;
		Plan.m_Input.m_Direction = -Ctx.m_Input.m_Direction;
		Plan.m_SafeTicks = 0;
		Plan.m_ScannedTicks = 0;
		Plan.m_Candidates = 1;
		Plan.m_Score = 0.0f;
		Plan.m_CostMs = 0.0f;
		str_copy(Plan.m_aReason, BcLocalize("debug override (input pipeline self-test)"));
		return Plan;
	}

	const int64_t StartTime = time_get();

	const SSettings &Set = Ctx.m_Settings;
	const int CheckTicks = std::clamp(Set.m_CheckTicks, 2, MAX_SIM_TICKS);

	Plan.m_Override = false;
	Plan.m_UsedFallback = false;
	Plan.m_SafeTicks = 0;
	Plan.m_ScannedTicks = 0;
	Plan.m_Candidates = 0;
	Plan.m_Score = 0.0f;
	Plan.m_CostMs = 0.0f;
	Plan.m_aReason[0] = '\0';

	auto Finish = [&](const char *pReason) {
		str_copy(Plan.m_aReason, pReason);
		Plan.m_CostMs = (float)((time_get() - StartTime) * 1000.0 / (double)time_freq());
		return Plan;
	};

	// Nothing can be predicted without a map; stay out of the way and say so on the HUD instead of
	// guessing.
	if(!Collision() || Collision()->GetWidth() <= 0 || Collision()->GetHeight() <= 0)
		return Finish(BcLocalize("no map data"));

	// Being frozen, deep frozen or live frozen right now is a state, not a tile: no input can
	// change it, so the agent has nothing to contribute (see the delivery document, 6.5 point 4).
	if(Ctx.m_Threat.m_Flags & HAZ_SELF)
		return Finish(BcLocalize("frozen, agent idle"));

	// --------------------------------------------------------------------------------------------
	// 0. Sensor range gate: `bc_avoid_sensing_radius` decides how far the agent is allowed to look,
	//    and therefore how early it may react.
	//
	//    The forward simulator deliberately has no range limit of its own - it advances the tee and
	//    asks `ClassifyPoint()` about wherever it ends up. That makes the *physics* prediction
	//    exact, but it would also let the agent react to hazards far outside the sensing radius, so
	//    the radius has to gate the engine explicitly.
	//
	//    `ScanThreat()` only sets `m_HasNearest` for a tile that `IsRelevantHazard()` accepts, so
	//    this single flag already folds in `bc_avoid_sensing_radius` and every `bc_avoid_tile_*`
	//    switch: raise the radius and the tee is braked earlier, lower it and the agent holds off
	//    until the hazard is closer.
	// --------------------------------------------------------------------------------------------
	if(Set.m_SensingRadius > 0 && !Ctx.m_Threat.m_HasNearest)
		return Finish(BcLocalize("no hazard within sensing range"));

	// --------------------------------------------------------------------------------------------
	// 0b. Movement state gate (stage 3, TAS red line 3.11.7).
	//
	//     Rewriting `m_Direction` is only ever justified as "do not walk into that hazard". While
	//     the tee is on a jetpack, hanging on another tee or being launched by the dummy's hammer,
	//     the movement key is not a walking key, so the agent is hands off by default and says why.
	//     The dummy connection itself is never touched at all (the `!Dummy` branch of OnSnapInput).
	// --------------------------------------------------------------------------------------------
	const int MoveState = Avoid::ClassifyMovement(Ctx.m_Core, FlyHammerState(Ctx) != Avoid::MOVE_NORMAL);
	if(MoveState != Avoid::MOVE_NORMAL)
	{
		if(MoveState & Avoid::MOVE_JETPACK)
			return Finish(BcLocalize("jetpack, hands off"));
		if(MoveState & Avoid::MOVE_HOOKED_PLAYER)
			return Finish(BcLocalize("hooked to a player, hands off"));
		return Finish(BcLocalize("fly hammer, hands off"));
	}

	// --------------------------------------------------------------------------------------------
	// 0c. Agent dispatch.
	//
	//     Legit runs its own search: its fast path, kick-in threshold and lookahead all have to use
	//     the prediction aware simulation, so they live in the planner. Everything below this point
	//     is the untouched stage 2 Basic agent.
	// --------------------------------------------------------------------------------------------
	if(Set.m_Agent == AGENT_LEGIT)
		return PlanLegit(Ctx);
	if(Set.m_Agent != AGENT_BASIC)
		return Finish(BcLocalize("agent not implemented yet"));

	// --------------------------------------------------------------------------------------------
	// 1. Fast path: does the input the player is about to send stay safe on its own?
	//    This decides the whole feel of the module - any tick that lands here is a tick the player
	//    never notices the agent exists.
	// --------------------------------------------------------------------------------------------
	const int PlayerSafe = SimulateInput(Ctx, Ctx.m_Input, CheckTicks);
	Plan.m_SafeTicks = PlayerSafe;
	Plan.m_ScannedTicks = CheckTicks;

	if(PlayerSafe >= CheckTicks)
		return Finish(BcLocalize("player input safe"));

	// 2. Kick-in: the player's input still survives long enough, so do not touch it yet. This is
	//    what keeps the agent from hovering over every tee that walks past a hazard.
	// `kick_in_ticks = 0` means "do not wait at all"; without the guard the comparison would always
	// be true and the lowest slider setting would mean "never intervene".
	const int KickIn = std::clamp(Set.m_KickInTicks, 0, MAX_SIM_TICKS);
	if(KickIn > 0 && PlayerSafe >= KickIn)
		return Finish(BcLocalize("still time before the hazard"));

	// 3. Basic candidate set: the three direction keys. The player's own direction is always in
	//    the set, so "do nothing" is a real option and the search can never report that nothing was
	//    evaluated; `bc_avoid_direction_assist` narrows it back to that single option.
	int aDirections[3];
	int NumDirections = 0;
	aDirections[NumDirections++] = std::clamp(Ctx.m_Input.m_Direction, -1, 1);
	if(Set.m_DirectionAssist)
	{
		for(int Dir = -1; Dir <= 1; ++Dir)
		{
			if(Dir != aDirections[0])
				aDirections[NumDirections++] = Dir;
		}
	}

	// Basic never touches the hook, the jump key or the aim: every candidate is the player's own
	// input with nothing but `m_Direction` replaced.
	CNetObj_PlayerInput Candidate = Ctx.m_Input;
	// The first sample always wins the initial comparison, whatever the weights are: with
	// `life_weight = 0` and a non-zero direction weight the scores can legitimately go negative.
	bool HasBest = false;
	int BestSafe = 0;
	int BestDir = Ctx.m_Input.m_Direction;
	float BestScore = 0.0f;

	const int SampleCount = std::min(NumDirections, MAX_CANDIDATES);
	for(int i = 0; i < SampleCount; ++i)
	{
		Candidate.m_Direction = aDirections[i];
		const int Safe = SimulateInput(Ctx, Candidate, CheckTicks);

		SSample &Sample = m_aSamples[i];
		Sample.m_Direction = aDirections[i];
		Sample.m_SafeTicks = Safe;
		// Survival is the objective; the direction weight only breaks ties between plans that live
		// equally long, which is what keeps the braking as short as the situation allows.
		Sample.m_Score = Set.m_LifeWeight * Safe - Set.m_DirectionWeight * std::abs(aDirections[i] - Ctx.m_Input.m_Direction);

		Plan.m_Candidates++;
		Plan.m_ScannedTicks = std::max(Plan.m_ScannedTicks, Safe);
		if(!HasBest || Sample.m_Score > BestScore)
		{
			HasBest = true;
			BestSafe = Safe;
			BestDir = aDirections[i];
			BestScore = Sample.m_Score;
		}
	}

	if(!HasBest)
		return Finish(BcLocalize("nothing evaluated"));

	const bool SafeEnough = BestSafe >= CheckTicks;
	const bool Gained = BestSafe > PlayerSafe;

	// A full window is always taken; otherwise the player's own input stays in charge unless
	// another option genuinely survives longer. Ties therefore resolve to "do not intervene".
	if(!(SafeEnough || (Gained && BestDir != Ctx.m_Input.m_Direction)))
		return Finish(BcLocalize("no safer direction"));

	Plan.m_Override = true;
	Plan.m_SafeTicks = BestSafe;
	Plan.m_Score = BestScore;
	Plan.m_Input.m_Direction = BestDir;
	// NSIF: the usual case here is "standing next to a hazard is already unavoidable"; the best
	// first step found so far is replayed, and the HUD says so through the red badge.
	Plan.m_UsedFallback = !SafeEnough;

	if(!SafeEnough)
	{
		if(BestDir == 0)
			str_copy(Plan.m_aReason, BcLocalize("NSIF: brake before hazard"));
		else
			str_copy(Plan.m_aReason, BestDir < 0 ? BcLocalize("NSIF: steer left before hazard") : BcLocalize("NSIF: steer right before hazard"));
	}
	else if(BestDir == 0)
	{
		str_copy(Plan.m_aReason, BcLocalize("brake before hazard"));
	}
	else
	{
		str_copy(Plan.m_aReason, BestDir < 0 ? BcLocalize("steer left before hazard") : BcLocalize("steer right before hazard"));
	}

	return Finish(Plan.m_aReason);
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

	if(g_Config.m_BcAvoidShowVisuals && m_LastThreat.m_SensedTiles > 0)
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
	// Two independent switches, following the convention of the other HUD modules: the feature
	// switch in the menu (`bc_avoid_show_hud`) and the per-module switch of the HUD editor
	// (`HudLayout::IsEnabled`). Both have to be on.
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

	CUIRect Rect;
	Rect.w = HUD_BASE_WIDTH * Scale;
	Rect.h = HUD_BASE_HEIGHT * Scale;
	Rect.x = std::clamp(Layout.m_X, 0.0f, std::max(0.0f, HudWidth - Rect.w));
	Rect.y = std::clamp(Layout.m_Y, 0.0f, std::max(0.0f, HudHeight - Rect.h));
	return Rect;
}

CUIRect CAvoid::GetHudEditorRect() const
{
	// The HUD editor always wants a rect, even while the module is switched off or the agent is
	// idle, so that it can be dragged and resized.
	return GetHudRect(true);
}

void CAvoid::RenderPreview()
{
	RenderHudModule(true);
}

// The in-game panel. It carries the same readings as the "Live status" card of the menu page:
// the tee state, the closest hazard with its coordinates, how much of the sensor radius is hot,
// how long the current plan survives and what the agent decided.
void CAvoid::RenderHudModule(bool ForcePreview)
{
	const float HudHeight = HudLayout::CANVAS_HEIGHT;
	const float HudWidth = HudHeight * Graphics()->ScreenAspect();

	CUIRect Rect = GetHudRect(ForcePreview);
	if(Rect.w <= 0.0f || Rect.h <= 0.0f)
		return;

	Graphics()->MapScreenToSize(HudWidth, HudHeight);

	const HudLayout::SModuleLayout Layout = HudLayout::Get(HudLayout::MODULE_AVOID, HudWidth, HudHeight);
	const float Scale = std::clamp(Layout.m_Scale / 100.0f, 0.25f, 3.0f);
	const float ModuleAlpha = HudLayout::AlphaFactor(HudLayout::MODULE_AVOID);

	const STelemetry &T = m_Telemetry;
	const ColorRGBA Badge = StateColor(T.m_State);

	if(Layout.m_BackgroundEnabled)
	{
		const int Corners = HudLayout::BackgroundCorners(IGraphics::CORNER_ALL, Rect.x, Rect.y, Rect.w, Rect.h, HudWidth, HudHeight);
		Rect.Draw(color_cast<ColorRGBA>(ColorHSLA(Layout.m_BackgroundColor, true)).WithMultipliedAlpha(ModuleAlpha), Corners, 4.0f * Scale);
	}

	CUIRect Content;
	Rect.Margin(HUD_PADDING * Scale, &Content);

	CUIRect Header, Rows;
	Content.HSplitTop(HUD_HEADER_HEIGHT * Scale, &Header, &Rows);

	// State badge plus the selected mode.
	CUIRect BadgeRect, TitleRect;
	Header.VSplitLeft(HUD_BADGE_WIDTH * Scale, &BadgeRect, &TitleRect);
	BadgeRect.Draw(Badge.WithMultipliedAlpha(ModuleAlpha), IGraphics::CORNER_ALL, 3.0f * Scale);

	char aBuf[192];
	TextRender()->TextColor(ColorRGBA(0.05f, 0.05f, 0.05f, ModuleAlpha));
	Ui()->DoLabel(&BadgeRect, StateName(T.m_State), HUD_FONT_HEADER * Scale, TEXTALIGN_MC);
	TextRender()->TextColor(ColorRGBA(1.0f, 1.0f, 1.0f, ModuleAlpha));

	TitleRect.VSplitLeft(3.0f * Scale, nullptr, &TitleRect);
	str_format(aBuf, sizeof(aBuf), "AVOID  %s", AgentName(T.m_Agent));
	Ui()->DoLabel(&TitleRect, aBuf, HUD_FONT_HEADER * Scale, TEXTALIGN_ML);

	const float RowHeight = HUD_ROW_HEIGHT * Scale;
	const float LabelWidth = HUD_LABEL_WIDTH * Scale;

	auto Row = [&](const char *pLabel, const char *pValue, ColorRGBA ValueColor) {
		CUIRect RowRect, LabelRect, ValueRect;
		Rows.HSplitTop(RowHeight, &RowRect, &Rows);
		if(pLabel && pLabel[0])
		{
			RowRect.VSplitLeft(LabelWidth, &LabelRect, &ValueRect);
			TextRender()->TextColor(ColorRGBA(0.62f, 0.66f, 0.74f, ModuleAlpha));
			Ui()->DoLabel(&LabelRect, pLabel, HUD_FONT_ROW * Scale, TEXTALIGN_ML);
		}
		else
		{
			ValueRect = RowRect;
		}

		SLabelProperties Props;
		Props.m_MaxWidth = ValueRect.w;
		Props.m_EllipsisAtEnd = true;
		TextRender()->TextColor(ValueColor.WithMultipliedAlpha(ModuleAlpha));
		Ui()->DoLabel(&ValueRect, pValue, HUD_FONT_ROW * Scale, TEXTALIGN_ML, Props);
		TextRender()->TextColor(ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f));
	};

	const ColorRGBA ValueColor = ColorRGBA(0.82f, 0.87f, 0.95f, 1.0f);

	// Tee state and position.
	str_format(aBuf, sizeof(aBuf), "%s  (%.1f, %.1f)", HazardName(T.m_ThreatFlags), T.m_PlayerPos.x / TILE_SIZE, T.m_PlayerPos.y / TILE_SIZE);
	Row(BcLocalize("Tee"), aBuf, HazardColor(T.m_ThreatFlags));

	// Closest hazard.
	if(T.m_ThreatDistanceTiles >= 0.0f)
		str_format(aBuf, sizeof(aBuf), "%.1f %s  (%.1f, %.1f)", T.m_ThreatDistanceTiles, BcLocalize("tiles"), T.m_ThreatPos.x / TILE_SIZE, T.m_ThreatPos.y / TILE_SIZE);
	else
		str_copy(aBuf, BcLocalize("no hazard in range"));
	Row(BcLocalize("Nearest"), aBuf, HazardColor(T.m_ThreatFlags));

	// Sensor coverage and how long the current plan survives.
	str_format(aBuf, sizeof(aBuf), "%d / %d", T.m_HazardTiles, T.m_SensedTiles);
	Row(BcLocalize("Hazard / sensed"), aBuf, ValueColor);

	str_format(aBuf, sizeof(aBuf), "%d %s  (%.2f %s)", T.m_SafeTicks, BcLocalize("ticks"), T.m_CostMs, BcLocalize("ms"));
	Row(BcLocalize("Safe ahead"), aBuf, ValueColor);

	str_format(aBuf, sizeof(aBuf), "%d / %d", T.m_Overrides, T.m_Decisions);
	Row(BcLocalize("Overrides"), aBuf, ColorRGBA(0.95f, 0.75f, 0.35f, 1.0f));

	str_format(aBuf, sizeof(aBuf), "%s: %s", BcLocalize("Plan"), T.m_aReason[0] ? T.m_aReason : BcLocalize("<none>"));
	Row("", aBuf, ColorRGBA(0.75f, 0.78f, 0.85f, 1.0f));

	Ui()->MapScreen();
}

void CAvoid::RenderWorldOverlay(const CCharacterCore &Core, const SThreat &Threat)
{
	if(!Collision() || Collision()->GetWidth() <= 0 || Collision()->GetHeight() <= 0)
		return;

	const CScreenRect PreviousScreen = Graphics()->GetScreen();
	const CScreenRect World = Graphics()->MapScreenToWorld(
		GameClient()->m_Camera.m_Center.x, GameClient()->m_Camera.m_Center.y,
		100.0f, 100.0f, 100.0f, 0.0f, 0.0f, Graphics()->ScreenAspect(), GameClient()->m_Camera.m_Zoom);
	Graphics()->MapScreen(World);
	Graphics()->TextureClear();

	// Anchor everything on the position the tee is actually drawn at. Using the tick quantised
	// core position makes the ring stand still for a few frames and then jump, which reads as a
	// ghost/double outline as soon as the tee moves fast.
	const vec2 Anchor = OverlayAnchor(Core);
	const vec2 Pos = Anchor;
	// Exactly the reach the sensor uses - half tile steps and the same vertical squash - so the
	// ring and the highlights show what the agent can actually see.
	const float RadiusX = std::clamp(m_Settings.m_SensingRadius, 0.5f, 16.0f);
	const float RadiusY = std::max(0.5f, RadiusX * Avoid::SensingVerticalFactor(Core, m_Settings));
	const float ReachX = RadiusX * TILE_SIZE;
	const float ReachY = RadiusY * TILE_SIZE;
	const int ScanX = (int)std::ceil(RadiusX);
	const int ScanY = (int)std::ceil(RadiusY);
	const int CenterX = (int)std::floor(Core.m_Pos.x / TILE_SIZE);
	const int CenterY = (int)std::floor(Core.m_Pos.y / TILE_SIZE);

	// Hazard tile highlights.
	Graphics()->QuadsBegin();
	for(int Ty = CenterY - ScanY; Ty <= CenterY + ScanY; ++Ty)
	{
		for(int Tx = CenterX - ScanX; Tx <= CenterX + ScanX; ++Tx)
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

	// Sensing ring plus the threat vector towards the closest hazard.
	const int NumSegments = 64;
	static IGraphics::CLineItem s_aRing[NumSegments];
	static IGraphics::CLineItem s_aVector[2];
	const float RingRadius = ReachX;
	Graphics()->SetColor(0.45f, 0.65f, 0.95f, 0.35f);
	for(int i = 0; i < NumSegments; ++i)
	{
		const float A0 = 2.0f * pi * (float)i / (float)NumSegments;
		const float A1 = 2.0f * pi * (float)(i + 1) / (float)NumSegments;
		s_aRing[i] = IGraphics::CLineItem(Pos + vec2(std::cos(A0) * RingRadius, std::sin(A0) * ReachY),
			Pos + vec2(std::cos(A1) * RingRadius, std::sin(A1) * ReachY));
	}
	Graphics()->LinesBegin();
	Graphics()->LinesDraw(s_aRing, NumSegments);
	Graphics()->LinesEnd();

	if(Threat.m_HasNearest)
	{
		Graphics()->SetColor(HazardColor(Threat.m_Flags));
		s_aVector[0] = IGraphics::CLineItem(Pos, Threat.m_NearestPos);
		s_aVector[1] = IGraphics::CLineItem(Threat.m_NearestPos, Threat.m_NearestPos + vec2(0.0f, -6.0f));
		Graphics()->LinesBegin();
		Graphics()->LinesDraw(s_aVector, 2);
		Graphics()->LinesEnd();
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
