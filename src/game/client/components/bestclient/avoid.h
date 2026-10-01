/* Copyright © 2026 BestProject Team */
#ifndef GAME_CLIENT_COMPONENTS_BESTCLIENT_AVOID_H
#define GAME_CLIENT_COMPONENTS_BESTCLIENT_AVOID_H

#include "avoid_engine.h"

#include <base/vmath.h>

#include <engine/console.h>

#include <generated/protocol.h>

#include <game/client/component.h>
#include <game/client/ui_rect.h>
#include <game/gamecore.h>

class CCharacter;

/* ---------------------------------------------------------------------------------------------
 * CAvoid - the "Avoid" (Gores bot) hazard protection module that lives in the TAS& menu.
 *
 * STAGE 1 (this revision) delivers, and is expected to keep working:
 *   - the complete configuration surface (`bc_avoid_*`),
 *   - the runtime state machine plus the input interception pipeline
 *     (`CGameClient::OnSnapInput` -> CAvoid::ApplyInput),
 *   - the hazard sensing layer: DDNet-exact tile classification and a nearest-threat scan,
 *   - the in-game status HUD and the world-space threat overlay,
 *   - the "Avoid" page of the TAS& menu.
 *
 * STAGE 2 filled the decision engine with the Basic agent: a forward simulator built on a cloned
 * CCharacterCore, plus a three candidate direction search (left / keep / right). It only ever
 * rewrites `m_Input.m_Direction`; hook, jump, fire and aim stay exactly as the player sent them.
 *
 * STAGE 3 (current revision) adds the Legit agent: the candidate space grows to
 * direction x jump x hook, the search becomes a UCT search whose iteration count / exploration
 * constant / three priority weights are the `bc_avoid_*` sliders, hook release and jump become
 * real options and the clone can predict other players. All of that lives in Avoid::CPlanner
 * (avoid_engine.h/.cpp) so it stays unit testable; this component keeps the sensing layer, the
 * input pipeline, the state machine, the HUD and the Basic agent exactly as stage 2 left them.
 * ------------------------------------------------------------------------------------------- */
class CAvoid : public CComponent
{
public:
	enum EAgent
	{
		AGENT_BASIC = 0,
		AGENT_LEGIT,
		AGENT_BLATANT,
		AGENT_FENTBOT,
		AGENT_PILOT,
		NUM_AGENTS,
	};

	enum EState
	{
		STATE_OFF = 0, // module disabled, or armed state not requested
		STATE_WATCHING, // armed; your own input is currently predicted safe
		STATE_ASSISTING, // armed; the agent replaced your input
		STATE_NSIF, // armed; nothing is fully safe, safest first step reused
		STATE_AFK, // disarmed automatically by AFK protection
	};

	// Hazard classification bits, defined once in avoid_engine.h so that the sensing layer and the
	// decision engine cannot drift apart.
	enum
	{
		HAZ_NONE = Avoid::HAZ_NONE,
		HAZ_DEATH = Avoid::HAZ_DEATH, // TILE_DEATH and death switches
		HAZ_FREEZE = Avoid::HAZ_FREEZE, // TILE_FREEZE
		HAZ_DEEP = Avoid::HAZ_DEEP, // TILE_DFREEZE
		HAZ_LIVE = Avoid::HAZ_LIVE, // TILE_LFREEZE
		HAZ_UNFREEZE = Avoid::HAZ_UNFREEZE, // TILE_UNFREEZE (optional hazard, off by default)
		HAZ_TELE = Avoid::HAZ_TELE, // any teleport tile (optional hazard, off by default)
		HAZ_SELF = Avoid::HAZ_SELF, // the tee itself is frozen / deep frozen right now
		HAZ_ANY = Avoid::HAZ_ANY,
	};

	// The data contract of the decision engine. The layout lives in avoid_engine.h (it is the
	// interface between the client component and the unit tested planner); the names stay
	// `CAvoid::...` so every existing caller keeps working.
	using SSettings = Avoid::SSettings;
	using SThreat = Avoid::SThreat;
	using SContext = Avoid::SContext;
	using SInputPlan = Avoid::SInputPlan;

	// Live values the menu and the HUD display.
	struct STelemetry
	{
		int m_State = STATE_OFF;
		bool m_Armed = false;
		int m_Agent = AGENT_BASIC;
		int m_ThreatFlags = HAZ_NONE;
		float m_ThreatDistanceTiles = 0.0f;
		vec2 m_ThreatPos = vec2(0.0f, 0.0f);
		int m_SensedTiles = 0;
		int m_HazardTiles = 0;
		int m_SafeTicks = 0;
		int m_Candidates = 0;
		int m_Decisions = 0;
		int m_Overrides = 0;
		int m_NsifFallbacks = 0;
		float m_CostMs = 0.0f;
		vec2 m_PlayerPos = vec2(0.0f, 0.0f);
		vec2 m_PlayerVel = vec2(0.0f, 0.0f);
		char m_aReason[64] = "";
	};

	int Sizeof() const override { return sizeof(*this); }

	void OnConsoleInit() override;
	void OnReset() override;
	void OnMapLoad() override;
	void OnRender() override;

	// --- in-game HUD module (registered as HudLayout::MODULE_AVOID) -----------------------------
	// The panel is a first class HUD module: it can be moved, scaled, tinted and toggled in the
	// built-in HUD editor, exactly like the score or the keystrokes.
	CUIRect GetHudEditorRect() const;
	void RenderPreview();

	// Called from CGameClient::OnSnapInput() for the connection the player controls.
	// `pData` holds a CNetObj_PlayerInput; `Size` is its byte size.
	void ApplyInput(int *pData, int Size, bool Dummy);

	// --- configuration helpers used by the menu -------------------------------------------------
	int Agent() const;
	const char *AgentName(int Agent) const;
	void SetAgent(int Agent);
	bool IsArmed() const;
	// True while an armed agent needs a fresh input on every tick. CControls::SnapInput() uses
	// this to bypass its "identical input" rate limit (see the delivery document, section 3.5).
	bool WantsEveryTickInput() const;
	void SetArmed(bool Armed);
	void ToggleArmed();
	void ResetCounters();
	void PrintStatus() const;

	// --- sensing layer (implemented in stage 1, used by stage 2) --------------------------------
	int ClassifyTile(int Tile) const;
	int ClassifyPoint(vec2 Pos) const;
	int HazardMaskFromSettings() const;
	bool IsRelevantHazard(int Flags) const;
	SThreat ScanThreat(const CCharacterCore &Core) const;
	const SThreat &LastThreat() const { return m_LastThreat; }
	const STelemetry &Telemetry() const { return m_Telemetry; }
	bool IsHudVisible() const;

	// --- decision engine (stage 2) --------------------------------------------------------------
	// Basic mode only rewrites `m_Direction`; the remaining candidate plans of the later agents
	// (jump / hook / aim) will slot into the same simulation loop.
	static constexpr int MAX_CANDIDATES = 8;
	static constexpr int MAX_SIM_TICKS = Avoid::MAX_SIM_TICKS;
	SInputPlan EvaluateBestPlan(const SContext &Ctx);

	static const char *StateName(int State);
	static const char *HazardName(int Flags);

private:
	STelemetry m_Telemetry{};
	SThreat m_LastThreat{};
	SInputPlan m_LastPlan{};
	SSettings m_Settings{};

	int m_LastDecisionTick = -1;
	int m_LastInputTick = -1;
	int m_IdleTicks = 0;

	// Stage 2 forward simulator. Clones the live CCharacterCore and advances it with the real
	// DDNet core physics, so it needs no world of its own. Returns the number of ticks the
	// candidate survives (== MaxTicks when the whole window is safe) and, optionally, the final
	// position and velocity for debugging.
	struct SSample
	{
		int m_Direction = 0;
		int m_SafeTicks = 0;
		float m_Score = 0.0f;
	};
	// Per candidate results of the current decision, held in a fixed member buffer so that the
	// decision path never allocates. Cleared in OnReset().
	SSample m_aSamples[MAX_CANDIDATES]{};

	int SimulateInput(const SContext &Ctx, const CNetObj_PlayerInput &Input, int MaxTicks,
		vec2 *pOutPos = nullptr, vec2 *pOutVel = nullptr);
	void LogTrace(const char *pTag, const SContext &Ctx, int SafeTicks, int Limit, const SInputPlan &Plan) const;

	// The Legit agent (stage 3). Owns the search buffers; the environment has to stay valid for the
	// duration of the call only.
	Avoid::CPlanner m_Planner;
	SInputPlan PlanLegit(const SContext &Ctx);
	// The clone world of the current decision: the live world, or the practice sandbox world while
	// the TAS sandbox owns the physics. `WithPlayers` adds the predicted player snapshots.
	Avoid::SEnvironment BuildEnvironment(const SContext &Ctx, bool WithPlayers) const;
	// Hammer fly / deep fly detection, the one movement state the engine cannot see on its own.
	int FlyHammerState(const SContext &Ctx) const;

	const CCharacterCore *ActiveCore(int *pClientId, bool *pIsDummy) const;
	SSettings ReadSettings() const;
	// Position the tee is actually DRAWN at (smoothed), used to anchor the world overlay.
	vec2 OverlayAnchor(const CCharacterCore &Core) const;
	void UpdateTelemetry(const SContext &Ctx, const SInputPlan &Plan);
	void CheckAfkProtection(const SContext &Ctx);

	CUIRect GetHudRect(bool ForcePreview) const;
	void RenderHudModule(bool ForcePreview);
	void RenderWorldOverlay(const CCharacterCore &Core, const SThreat &Threat);

	static void ConAvoidToggle(IConsole::IResult *pResult, void *pUserData);
	static void ConAvoidStatus(IConsole::IResult *pResult, void *pUserData);
	static void ConAvoidReset(IConsole::IResult *pResult, void *pUserData);
};

#endif // GAME_CLIENT_COMPONENTS_BESTCLIENT_AVOID_H
