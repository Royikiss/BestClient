/* Copyright © 2026 BestProject Team */
#ifndef GAME_CLIENT_COMPONENTS_BESTCLIENT_AVOID_H
#define GAME_CLIENT_COMPONENTS_BESTCLIENT_AVOID_H

#include "avoid_engine.h"
#include "avoid_tile_editor.h"

#include <base/vmath.h>

#include <engine/console.h>

#include <game/client/component.h>
#include <game/client/ui.h>

#include <vector>

/* ---------------------------------------------------------------------------------------------
 * CAvoid - client component of the Avoid (Gores bot) module.
 *
 * Architecture follows docs/avoid/KRX_AVOID_REPRODUCTION_SPEC.md: the component samples the
 * input that is about to be sent, hands it to the selected agent (Basic, Legit, Blatant, Fentbot
 * or Pilot), and only replaces it when the agent reports that it intervened.
 * ------------------------------------------------------------------------------------------- */
class CAvoid : public CComponent
{
public:
	enum EAgent
	{
		AGENT_BASIC = Avoid::AGENT_BASIC,
		AGENT_LEGIT = Avoid::AGENT_LEGIT,
		AGENT_BLATANT = Avoid::AGENT_BLATANT,
		AGENT_FENTBOT = Avoid::AGENT_FENTBOT,
		AGENT_PILOT = Avoid::AGENT_PILOT,
		NUM_AGENTS = Avoid::NUM_AGENTS,
	};

	enum EState
	{
		STATE_OFF = 0,
		STATE_WATCHING,
		STATE_ASSISTING,
		STATE_NSIF,
		STATE_AFK,
	};

	// The five environment gates that run before any agent is allowed to think (reference spec 5).
	// PRE_OK means the pipeline continues; every other value blocks the tick and passes the input
	// through untouched.
	enum EPreActivation
	{
		PRE_OK = 0,
		PRE_GAMEMODE, // 1. blacklisted gamemode (fng / vanilla / f-ddrace / blockworlds)
		PRE_INACTIVE, // 2. spectator, paused game or no live character
		PRE_FROZEN, // 3. the tee itself is frozen, inputs steer nothing
		PRE_AFK, // 4. afk protection: the player left the keyboard
		PRE_SAFE, // 5. the 10 tick probe says the player's own input is safe
	};

	using SSettings = Avoid::SSettings;
	using SAimMarker = Avoid::SAimMarker;

	struct STelemetry
	{
		int m_State = STATE_OFF;
		bool m_Enabled = false;
		int m_Agent = AGENT_BASIC;
		int m_SurvivalTicks = 0;
		float m_CostMs = 0.0f;
		int m_Decisions = 0;
		int m_Overrides = 0;
		int m_NsifFallbacks = 0;
		bool m_NavigatorReady = false;
		SAimMarker m_TrackPoint{};
		SAimMarker m_AimTarget{};
		char m_aReason[64] = "";
	};

	CAvoid();
	~CAvoid() override;

	int Sizeof() const override { return sizeof(*this); }

	void OnConsoleInit() override;
	void OnInit() override;
	void OnReset() override;
	void OnMapLoad() override;
	void OnRender() override;

	CUIRect GetHudEditorRect() const;
	void RenderPreview();

	// Input pipeline hook. It inspects every tick, whether or not the sampler decided to send, and
	// rewrites pInput in place when the agent intervenes.
	enum EInputResult
	{
		INPUT_IDLE = 0, // nothing to do, the sampler's own send decision stands
		INPUT_DRIVEN, // the input was rewritten and has to be sent now
		INPUT_YIELDED, // the agent stopped driving, so the player's input has to be sent once
	};
	EInputResult ApplyInput(CNetObj_PlayerInput *pInput);

	int Agent() const;
	static const char *AgentName(int Agent);
	void SetAgent(int Agent);
	bool IsEnabled() const;
	void SetEnabled(bool Enabled);
	void ToggleEnabled();
	void ResetCounters();
	void PrintStatus() const;

	const STelemetry &Telemetry() const { return m_Telemetry; }
	const std::vector<vec2> &LastPath() const { return m_vLastPath; }
	bool IsHudVisible() const;

	// Goal set and movement restriction of Fentbot and Pilot (reference spec 9). Shared by both
	// agents, so the menu edits exactly the tiles the planners read.
	Avoid::CTileEditor &TileEditor() { return m_TileEditor; }
	const Avoid::CTileEditor &TileEditor() const { return m_TileEditor; }

	static const char *StateName(int State);
	static const char *PreActivationName(EPreActivation Gate);

private:
	Avoid::BLAgent *m_apAgents[NUM_AGENTS]{};
	Avoid::CTileEditor m_TileEditor{};
	STelemetry m_Telemetry{};
	std::vector<vec2> m_vLastPath{};

	int m_LastDecisionTick = -1;
	CNetObj_PlayerInput m_LastOverride{};
	bool m_LastOverrideActive = false;
	bool m_LastAimTargetValid = false;
	// Reference spec 5.4: the AFK state machine. The last sampled hardware input and the moment it
	// last changed; every gate above the probe reads the resulting IsAfk().
	int64_t m_LastActiveTime = 0;
	CNetObj_PlayerInput m_LastPlayerInput{};
	EPreActivation m_LastGate = PRE_OK;
	// Driving -> yielded handover. `m_YieldPending` is latched while the agent drives and is only
	// cleared once the player's own input has actually been sent, so the server can never be left
	// holding the agent's last input; the two ticks are telemetry.
	int m_DroveTick = -1;
	int m_YieldTick = -1;
	bool m_YieldPending = false;

	EInputResult FinishInput(bool Drives, CNetObj_PlayerInput *pInput, int Tick);

	const CCharacterCore *ActiveCore(int *pClientId) const;
	CCharacter *ActiveCharacter(int *pClientId) const;
	SSettings ReadSettings() const;
	void UpdateTelemetry(const Avoid::AvoidInput &Action, float CostMs);

	// --- Pre-activation pipeline (reference spec 5) -------------------------------------------------
	// 1. gamemode blacklist, 2. spectator/paused/dead, 3. already frozen, 4. AFK timeout.
	EPreActivation PreActivation() const;
	bool IsGamemodeBlacklisted() const;
	bool IsPlayerInactive() const;
	bool IsCharacterFrozen() const;
	bool IsAfk() const;
	void UpdateAfkTimer(const CNetObj_PlayerInput *pInput);
	// 5. the 10 tick baseline probe (reference spec 5.5). Returns the fully safe sentinel when the
	// player's own input has enough margin, so the agents stay asleep.
	int RunLightweightProbe(CGameWorld *pWorld, const CNetObj_PlayerInput *pInput, const SSettings &Set) const;

	CUIRect GetHudRect(bool ForcePreview) const;
	void RenderHudModule(bool ForcePreview);
	void RenderWorldOverlay(const CCharacterCore &Core);
	void RenderTileEditorOverlay();
	// Tile editor input and the one-shot actions behind bc_avoid_tile_editor_* (reference spec 9).
	void UpdateTileEditor();

	static void ConAvoidToggle(IConsole::IResult *pResult, void *pUserData);
	static void ConAvoidStatus(IConsole::IResult *pResult, void *pUserData);
	static void ConAvoidReset(IConsole::IResult *pResult, void *pUserData);
};

#endif // GAME_CLIENT_COMPONENTS_BESTCLIENT_AVOID_H
