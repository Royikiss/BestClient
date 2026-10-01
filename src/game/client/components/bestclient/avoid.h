/* Copyright © 2026 BestProject Team */
#ifndef GAME_CLIENT_COMPONENTS_BESTCLIENT_AVOID_H
#define GAME_CLIENT_COMPONENTS_BESTCLIENT_AVOID_H

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
 * STAGE 2 (current revision) fills the decision engine with the Basic agent: a forward simulator
 * built on a cloned CCharacterCore, plus a three candidate direction search (left / keep / right).
 * It only ever rewrites `m_Input.m_Direction`; hook, jump, fire and aim stay exactly as the player
 * sent them. Legit / Blatant (hook release, MCTS, NSIF candidate search, aimbot) are still to come
 * and reuse the same SimulateInput().
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

	// Hazard classification bits. A tile can carry several of them.
	enum
	{
		HAZ_NONE = 0,
		HAZ_DEATH = 1 << 0, // TILE_DEATH and death switches
		HAZ_FREEZE = 1 << 1, // TILE_FREEZE
		HAZ_DEEP = 1 << 2, // TILE_DFREEZE
		HAZ_LIVE = 1 << 3, // TILE_LFREEZE
		HAZ_UNFREEZE = 1 << 4, // TILE_UNFREEZE (optional hazard, off by default)
		HAZ_TELE = 1 << 5, // any teleport tile (optional hazard, off by default)
		HAZ_SELF = 1 << 6, // the tee itself is frozen / deep frozen right now
		HAZ_ANY = HAZ_DEATH | HAZ_FREEZE | HAZ_DEEP | HAZ_LIVE | HAZ_UNFREEZE | HAZ_TELE,
	};

	// Snapshot of the cvars, refreshed once per decision so that the engine always
	// reads a stable configuration.
	struct SSettings
	{
		int m_Agent = AGENT_BASIC;
		bool m_DirectionAssist = true;
		bool m_HookAssist = true;
		int m_CheckTicks = 26;
		int m_KickInTicks = 20;
		int m_Quality = 24;
		int m_Randomness = 30;
		int m_DirectionWeight = 100;
		int m_HookWeight = 100;
		int m_LifeWeight = 150;
		bool m_TileDeath = true;
		bool m_TileFreeze = true;
		bool m_TileUnfreeze = false;
		int m_UnfreezeTicks = 26;
		bool m_TileTele = false;
		bool m_PlayerPrediction = true;
		bool m_Nsif = true;
		bool m_AfkProtect = true;
		int m_AfkTime = 60;
		bool m_TrackPoint = false;
		bool m_SafeAimTracking = true;
		bool m_AutoDrag = false;
		bool m_Aimbot = false;
		int m_AimbotMode = 0; // 0 = auto aim, 1 = aim assist
		int m_AimbotSegments = 24;
		int m_AimbotFov = 90;
		int m_SensingRadius = 6;
	};

	// Result of the hazard scan around the controlled tee.
	struct SThreat
	{
		int m_Flags = HAZ_NONE; // union of all hazard bits seen on the tee itself
		vec2 m_NearestPos = vec2(0.0f, 0.0f); // world position of the closest hazard tile centre
		float m_NearestDistPx = 0.0f; // distance from the tee to the closest hazard tile box
		bool m_HasNearest = false;
		int m_SensedTiles = 0; // tiles examined by the scan
		int m_HazardTiles = 0; // tiles inside the sensing radius that are hazardous
		bool m_OnHazard = false; // the tee is standing inside a hazard right now
	};

	// Everything the decision engine is allowed to look at.
	struct SContext
	{
		int m_Tick = 0; // client tick this decision belongs to
		int m_ClientId = -1;
		bool m_Dummy = false; // the controlled tee is the dummy connection
		CCharacterCore m_Core{}; // live physics state of the controlled tee
		CNetObj_PlayerInput m_Input{}; // the input the player is about to send
		bool m_PlayerInputSafe = false; // filled by the engine (stage 2)
		SThreat m_Threat{};
		SSettings m_Settings{};
	};

	// What the decision engine answers with.
	struct SInputPlan
	{
		bool m_Override = false; // true => replace the outgoing input with m_Input
		bool m_UsedFallback = false; // true => NSIF: nothing was fully safe
		CNetObj_PlayerInput m_Input{};
		int m_SafeTicks = 0; // how long the chosen plan survives
		int m_ScannedTicks = 0; // simulation depth actually reached
		int m_Candidates = 0; // plans evaluated
		float m_Score = 0.0f; // cost of the winning plan (lower is better)
		float m_CostMs = 0.0f; // wall clock spent in the engine
		char m_aReason[64] = ""; // short human readable explanation for the HUD
	};

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
	static constexpr int MAX_SIM_TICKS = 50;
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
