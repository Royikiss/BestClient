/* Copyright © 2026 BestProject Team */
#ifndef GAME_CLIENT_COMPONENTS_BESTCLIENT_AVOID_H
#define GAME_CLIENT_COMPONENTS_BESTCLIENT_AVOID_H

#include <base/vmath.h>

#include <engine/console.h>
#include <game/client/component.h>
#include <game/client/ui.h>

#include "avoid_engine.h"

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
		INPUT_DRIVEN,   // the input was rewritten and has to be sent now
		INPUT_YIELDED,  // the agent stopped driving, so the player's input has to be sent once
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

	static const char *StateName(int State);

private:
	Avoid::BLAgent *m_apAgents[NUM_AGENTS]{};
	STelemetry m_Telemetry{};
	std::vector<vec2> m_vLastPath{};

	int m_LastDecisionTick = -1;
	int m_IdleTicks = 0;
	CNetObj_PlayerInput m_LastOverride{};
	bool m_LastOverrideActive = false;
	// Ticks of the driving -> yielded handover, so the player's own input reaches the server on the
	// very tick the agent lets go instead of waiting for the sampler's next regular send.
	int m_DroveTick = -1;
	int m_YieldTick = -1;

	EInputResult FinishInput(bool Drives, CNetObj_PlayerInput *pInput, int Tick);

	const CCharacterCore *ActiveCore(int *pClientId) const;
	SSettings ReadSettings() const;
	void CheckAfkProtection(const SSettings &Set, const Avoid::SContext &Ctx);
	void UpdateTelemetry(const Avoid::AvoidInput &Action, float CostMs);

	CUIRect GetHudRect(bool ForcePreview) const;
	void RenderHudModule(bool ForcePreview);
	void RenderWorldOverlay(const CCharacterCore &Core);

	static void ConAvoidToggle(IConsole::IResult *pResult, void *pUserData);
	static void ConAvoidStatus(IConsole::IResult *pResult, void *pUserData);
	static void ConAvoidReset(IConsole::IResult *pResult, void *pUserData);
};

#endif // GAME_CLIENT_COMPONENTS_BESTCLIENT_AVOID_H
