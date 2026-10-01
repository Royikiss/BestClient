/* Copyright © 2026 BestProject Team */
#include "avoid.h"

#include <base/math.h>
#include <base/mem.h>
#include <base/str.h>

#include <engine/client.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/textrender.h>

#include <game/client/components/bestclient/fast_practice.h>
#include <game/client/components/hud_layout.h>
#include <game/client/gameclient.h>
#include <game/client/prediction/entities/character.h>
#include <game/client/ui.h>
#include <game/localization.h>
#include <game/mapitems.h>

#include <algorithm>
#include <cmath>

namespace
{
// Death tiles are probed at the four corners of a smaller box, exactly like
// CCharacter::HandleSkippableTiles() does. CCharacterCore::PhysicalSize() is 28px,
// so the probe offset is 28 / 3.
constexpr float HAZARD_CORNER_PROBE = CCharacterCore::PhysicalSize() / 3.0f;

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

float DistanceToTileBox(vec2 Pos, int TileX, int TileY)
{
	const float Left = TileX * TILE_SIZE;
	const float Top = TileY * TILE_SIZE;
	const vec2 Closest(
		std::clamp(Pos.x, Left, Left + TILE_SIZE),
		std::clamp(Pos.y, Top, Top + TILE_SIZE));
	return distance(Pos, Closest);
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
	S.m_SensingRadius = std::clamp(g_Config.m_BcAvoidSensingRadius, 2, 16);
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
	switch(Tile)
	{
	case TILE_DEATH: return HAZ_DEATH;
	case TILE_FREEZE: return HAZ_FREEZE;
	case TILE_DFREEZE: return HAZ_DEEP;
	case TILE_LFREEZE: return HAZ_LIVE;
	case TILE_UNFREEZE: return HAZ_UNFREEZE;
	case TILE_TELEIN:
	case TILE_TELEOUT:
	case TILE_TELECHECK:
	case TILE_TELECHECKIN:
	case TILE_TELECHECKOUT:
	case TILE_TELEINEVIL:
	case TILE_TELECHECKINEVIL:
	case TILE_TELEINWEAPON:
	case TILE_TELEINHOOK:
		return HAZ_TELE;
	default:
		return HAZ_NONE;
	}
}

int CAvoid::HazardMaskFromSettings() const
{
	int Mask = HAZ_NONE;
	if(m_Settings.m_TileDeath)
		Mask |= HAZ_DEATH;
	if(m_Settings.m_TileFreeze)
		Mask |= HAZ_FREEZE | HAZ_DEEP | HAZ_LIVE;
	if(m_Settings.m_TileUnfreeze)
		Mask |= HAZ_UNFREEZE;
	if(m_Settings.m_TileTele)
		Mask |= HAZ_TELE;
	return Mask;
}

bool CAvoid::IsRelevantHazard(int Flags) const
{
	// Being frozen right now is a state, not a tile: it always matters.
	if(Flags & HAZ_SELF)
		return true;
	return (Flags & HazardMaskFromSettings()) != 0;
}

int CAvoid::ClassifyPoint(vec2 Pos) const
{
	int Flags = HAZ_NONE;
	// The tile helpers index straight into the collision arrays, so refuse to probe while no map
	// is loaded (`GetPureMapIndex` would clamp against an empty grid, and `GetTileIndex` would
	// dereference a null tile array).
	if(!Collision() || Collision()->GetWidth() <= 0 || Collision()->GetHeight() <= 0)
		return Flags;

	// Centre probe: freeze family, unfreeze, teleport and death switches.
	// Mirrors CCharacter::HandleTiles().
	const int Index = Collision()->GetPureMapIndex(Pos);
	if(Index >= 0)
	{
		Flags |= ClassifyTile(Collision()->GetTileIndex(Index));
		Flags |= ClassifyTile(Collision()->GetFrontTileIndex(Index));
		Flags |= ClassifyTile(Collision()->GetSwitchType(Index));
	}

	// Corner probe: death tiles, mirrors CCharacter::HandleSkippableTiles().
	const float R = HAZARD_CORNER_PROBE;
	for(int Corner = 0; Corner < 4; ++Corner)
	{
		const float Px = Pos.x + ((Corner & 1) ? R : -R);
		const float Py = Pos.y + ((Corner & 2) ? R : -R);
		if(Collision()->GetCollisionAt(Px, Py) == TILE_DEATH ||
			Collision()->GetFrontCollisionAt(Px, Py) == TILE_DEATH)
		{
			Flags |= HAZ_DEATH;
			break;
		}
		const int CornerIndex = Collision()->GetPureMapIndex(vec2(Px, Py));
		if(CornerIndex >= 0 && Collision()->GetSwitchType(CornerIndex) == TILE_DEATH)
		{
			Flags |= HAZ_DEATH;
			break;
		}
	}

	return Flags;
}

CAvoid::SThreat CAvoid::ScanThreat(const CCharacterCore &Core) const
{
	SThreat Threat;
	if(!Collision() || Collision()->GetWidth() <= 0 || Collision()->GetHeight() <= 0)
		return Threat;

	const vec2 Pos = Core.m_Pos;

	if(Core.m_FreezeEnd != 0 || Core.m_DeepFrozen || Core.m_LiveFrozen)
		Threat.m_Flags |= HAZ_SELF;

	Threat.m_Flags |= ClassifyPoint(Pos);

	const int Radius = std::clamp(m_Settings.m_SensingRadius, 2, 16);
	const int CenterX = (int)std::floor(Pos.x / TILE_SIZE);
	const int CenterY = (int)std::floor(Pos.y / TILE_SIZE);

	for(int Ty = CenterY - Radius; Ty <= CenterY + Radius; ++Ty)
	{
		for(int Tx = CenterX - Radius; Tx <= CenterX + Radius; ++Tx)
		{
			Threat.m_SensedTiles++;

			const vec2 TileCenter((Tx + 0.5f) * TILE_SIZE, (Ty + 0.5f) * TILE_SIZE);
			const int Flags = ClassifyPoint(TileCenter);
			if(Flags == HAZ_NONE || !IsRelevantHazard(Flags))
				continue;

			Threat.m_HazardTiles++;

			const float Dist = DistanceToTileBox(Pos, Tx, Ty);
			if(!Threat.m_HasNearest || Dist < Threat.m_NearestDistPx)
			{
				Threat.m_HasNearest = true;
				Threat.m_NearestDistPx = Dist;
				Threat.m_NearestPos = TileCenter;
			}
		}
	}

	Threat.m_OnHazard = IsRelevantHazard(Threat.m_Flags);
	return Threat;
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
// Decision engine - STAGE 2 SLOT
// ---------------------------------------------------------------------------------------------

CAvoid::SInputPlan CAvoid::EvaluateBestPlan(const SContext &Ctx)
{
	SInputPlan Plan;
	Plan.m_Input = Ctx.m_Input;

	// ============================================================================================
	// STAGE 2 IMPLEMENTATION SLOT
	//
	// This is the only intentionally empty part of the Avoid module. The behaviour that has to be
	// reproduced here is:
	//
	//   * Simulate the player's own input for `Ctx.m_Settings.m_CheckTicks` ticks.
	//     - If it survives the whole window, do nothing (return m_Override = false): the player
	//       keeps full control, which is what makes the bot feel invisible.
	//     - If it dies earlier than `m_KickInTicks`, search for a replacement input.
	//   * The search generates candidate inputs (direction -1/0/1 x jump 0/1 x hook 0/1 x a small
	//     set of aim angles), scores them with
	//         score = m_LifeWeight  * (survived ticks)
	//               - m_DirectionWeight * (distance from the intended direction)
	//               - m_HookWeight      * (hook state changed)
	//               - m_Randomness      * exploration term,
	//     and returns the best one.
	//   * When nothing survives `CheckTicks`, NSIF (`m_Nsif`) replays the first step of the best
	//     plan found so far and sets `m_UsedFallback`.
	//
	// Everything needed is already in `Ctx`:
	//     Ctx.m_Core        - the tee's live CCharacterCore (pos/vel/hook/freeze state)
	//     Ctx.m_Input       - the input the player is about to send
	//     Ctx.m_Threat      - pre-computed hazard scan (flags, nearest hazard, on-hazard)
	//     Ctx.m_Settings    - the full configuration snapshot
	// and the physics can be advanced with a temporary CCharacterCore + CCollision, or by cloning
	// the entity inside `GameClient()->m_FastPractice.PracticeWorld()`.
	//
	// Keep the interface as it is: fill `Plan` and return it.
	// ============================================================================================

	// --------------------------------------------------------------------------------------------
	// Stage 1 input pipeline self-test (`bc_avoid_debug_override`).
	//
	// The decision engine below is empty, so this switch is the only way to prove end to end that
	// the interception really steers the tee: it takes over every tick and inverts the left/right
	// input. Hold D and the tee walks left, on your screen and on the server, because `pData` is
	// the buffer the prediction and the network packet are both built from.
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

	Plan.m_Override = false;
	Plan.m_UsedFallback = false;
	Plan.m_SafeTicks = 0;
	Plan.m_ScannedTicks = 0;
	Plan.m_Candidates = 0;
	Plan.m_Score = 0.0f;
	Plan.m_CostMs = 0.0f;
	str_copy(Plan.m_aReason, BcLocalize("engine pending (stage 2)"));
	return Plan;
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
	const int Radius = std::clamp(m_Settings.m_SensingRadius, 2, 16);
	const int CenterX = (int)std::floor(Core.m_Pos.x / TILE_SIZE);
	const int CenterY = (int)std::floor(Core.m_Pos.y / TILE_SIZE);

	// Hazard tile highlights.
	Graphics()->QuadsBegin();
	for(int Ty = CenterY - Radius; Ty <= CenterY + Radius; ++Ty)
	{
		for(int Tx = CenterX - Radius; Tx <= CenterX + Radius; ++Tx)
		{
			const vec2 TileCenter((Tx + 0.5f) * TILE_SIZE, (Ty + 0.5f) * TILE_SIZE);
			const int Flags = ClassifyPoint(TileCenter);
			if(Flags == HAZ_NONE || !IsRelevantHazard(Flags))
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
	const float RingRadius = Radius * TILE_SIZE;
	Graphics()->SetColor(0.45f, 0.65f, 0.95f, 0.35f);
	for(int i = 0; i < NumSegments; ++i)
	{
		const float A0 = 2.0f * pi * (float)i / (float)NumSegments;
		const float A1 = 2.0f * pi * (float)(i + 1) / (float)NumSegments;
		s_aRing[i] = IGraphics::CLineItem(Pos + vec2(std::cos(A0), std::sin(A0)) * RingRadius,
			Pos + vec2(std::cos(A1), std::sin(A1)) * RingRadius);
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
