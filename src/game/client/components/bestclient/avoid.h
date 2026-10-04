/* Copyright © 2026 BestProject Team */
#ifndef GAME_CLIENT_COMPONENTS_BESTCLIENT_AVOID_H
#define GAME_CLIENT_COMPONENTS_BESTCLIENT_AVOID_H

#include <base/vmath.h>

#include <engine/console.h>
#include <game/client/component.h>
#include <game/client/ui.h>

#include "avoid_engine.h"

/* ---------------------------------------------------------------------------------------------
 * CAvoid - client component managing the Avoid (Gores bot) module.
 *
 * Implements the architecture specified in docs/avoid/KRX_AVOID_REPRODUCTION_SPEC.md:
 * Dispatches to BLAgent instances (Basic, Legit, Blatant, Fentbot, Pilot) using the
 * CGameWorld forward simulation engine.
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

	enum
	{
		HAZ_NONE = Avoid::HAZ_NONE,
		HAZ_DEATH = Avoid::HAZ_DEATH,
		HAZ_FREEZE = Avoid::HAZ_FREEZE,
		HAZ_DEEP = Avoid::HAZ_DEEP,
		HAZ_LIVE = Avoid::HAZ_LIVE,
		HAZ_UNFREEZE = Avoid::HAZ_UNFREEZE,
		HAZ_TELE = Avoid::HAZ_TELE,
		HAZ_SELF = Avoid::HAZ_SELF,
		HAZ_ANY = Avoid::HAZ_ANY,
	};

	using SSettings = Avoid::SSettings;
	using SThreat = Avoid::SThreat;
	using SContext = Avoid::SContext;
	using SInputPlan = Avoid::SInputPlan;

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
		bool m_AimChanged = false;
		int m_AimTargetX = 0;
		int m_AimTargetY = -1;
		char m_aReason[64] = "";
	};

	CAvoid();
	virtual ~CAvoid();

	int Sizeof() const override { return sizeof(*this); }

	void OnConsoleInit() override;
	void OnInit() override;
	void OnReset() override;
	void OnMapLoad() override;
	void OnRender() override;

	CUIRect GetHudEditorRect() const;
	void RenderPreview();

	void ApplyInput(int *pData, int Size, bool Dummy);

	int Agent() const;
	const char *AgentName(int Agent) const;
	void SetAgent(int Agent);
	bool IsArmed() const;
	bool WantsEveryTickInput() const;
	void SetArmed(bool Armed);
	void ToggleArmed();
	void ResetCounters();
	void PrintStatus() const;

	int ClassifyTile(int Tile) const;
	int ClassifyPoint(vec2 Pos) const;
	int HazardMaskFromSettings() const;
	bool IsRelevantHazard(int Flags) const;
	SThreat ScanThreat(const CCharacterCore &Core) const;
	const SThreat &LastThreat() const { return m_LastThreat; }
	const STelemetry &Telemetry() const { return m_Telemetry; }
	bool IsHudVisible() const;

	static const char *StateName(int State);
	static const char *HazardName(int Flags);

private:
	Avoid::BLAgent *m_apAgents[NUM_AGENTS]{};
	STelemetry m_Telemetry{};
	SThreat m_LastThreat{};
	SInputPlan m_LastPlan{};
	SSettings m_Settings{};

	int m_LastDecisionTick = -1;
	int m_LastInputTick = -1;
	int m_IdleTicks = 0;

	const CCharacterCore *ActiveCore(int *pClientId, bool *pIsDummy) const;
	SSettings ReadSettings() const;
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
